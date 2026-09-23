"""
Tier 1: whole-unit point mapping (system power, boost, health diagnostics).

Runs the real browser matcher (firmware/bacnet_bridge/main/points.js) under
node against the point definitions parsed straight out of hvac_core.c, so the
patterns tested are exactly the ones the firmware serves from /api/points.
"""
import json
import pathlib
import re
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HVAC_CORE_C = ROOT / "firmware/bacnet_bridge/components/hvac_core/hvac_core.c"
POINTS_JS = ROOT / "firmware/bacnet_bridge/main/points.js"
CATALOG = ROOT / "bacnet-object-catalog.json"

NODE = shutil.which("node")
pytestmark = pytest.mark.skipif(NODE is None, reason="node is required to run points.js")

TYPE_NAMES = {
    "OBJECT_ANALOG_INPUT": "analog-input", "OBJECT_ANALOG_OUTPUT": "analog-output",
    "OBJECT_ANALOG_VALUE": "analog-value", "OBJECT_BINARY_INPUT": "binary-input",
    "OBJECT_BINARY_OUTPUT": "binary-output", "OBJECT_BINARY_VALUE": "binary-value",
    "OBJECT_MULTI_STATE_INPUT": "multi-state-input", "OBJECT_MULTI_STATE_OUTPUT": "multi-state-output",
    "OBJECT_MULTI_STATE_VALUE": "multi-state-value",
}


def compatible_types(kind, writable):
    """Mirror of hvac_core_point_type_ok()."""
    if kind == "HVAC_POINT_KIND_REAL":
        return ["analog-input", "analog-output", "analog-value"]
    if kind == "HVAC_POINT_KIND_BOOL":
        return ["binary-output", "binary-value"] + ([] if writable else ["binary-input"])
    return ["multi-state-output", "multi-state-value"] + ([] if writable else ["multi-state-input"])


def load_point_defs():
    src = HVAC_CORE_C.read_text()
    table = src[src.index("static const hvac_point_def_t PointDefs"):]
    table = table[: table.index("\n};")] + "\n"
    defs = []
    for m in re.finditer(r"\[(HVAC_POINT_\w+)\]\s*=\s*\{(.*?)\},\n", table, re.S):
        body = m.group(2)
        strings = [bytes(s, "utf-8").decode("unicode_escape")
                   for s in re.findall(r'"((?:[^"\\]|\\.)*)"', body)]
        tail = re.sub(r'"((?:[^"\\]|\\.)*)"', "", body)
        kind, writable, otype, inst = [t.strip() for t in tail.split(",") if t.strip()][-4:]
        key, label, group, description, reference_name, pattern = strings
        defs.append({
            "id": key, "label": label, "group": group, "description": description,
            "reference_name": reference_name, "pattern": pattern,
            "writable": writable == "true",
            "types": compatible_types(kind, writable == "true"),
            "default": {"type": TYPE_NAMES[otype], "instance": int(inst)},
        })
    return defs


def run_matcher(defs, objects):
    script = (
        "global.window = global;"
        + POINTS_JS.read_text()
        + "\nconst input = JSON.parse(require('fs').readFileSync(0, 'utf8'));"
        + "\nconst out = {};"
        + "\nfor (const p of input.defs) {"
        + "\n  const m = PointMapper.matchPoint(p, input.objects);"
        + "\n  out[p.id] = {status: m.status, ambiguous: m.ambiguous,"
        + "\n    best: m.best ? {type: m.best.obj.type, instance: m.best.obj.instance, score: m.best.score} : null};"
        + "\n}"
        + "\nprocess.stdout.write(JSON.stringify(out));"
    )
    proc = subprocess.run([NODE, "-e", script], input=json.dumps({"defs": defs, "objects": objects}),
                          capture_output=True, text=True, check=True)
    return json.loads(proc.stdout)


@pytest.fixture(scope="module")
def defs():
    d = load_point_defs()
    assert len(d) == 21
    return d


def reference_objects(defs):
    objs = [{"type": p["default"]["type"], "instance": p["default"]["instance"], "name": p["reference_name"]}
            for p in defs]
    # Per-room look-alikes that must never be proposed for whole-unit points.
    objs += [
        {"type": "binary-value", "instance": 1101, "name": "Room_A Run Status"},
        {"type": "analog-value", "instance": 1104, "name": "Room_A Design Cooling Duty"},
        {"type": "analog-value", "instance": 1103, "name": "Room_A Design Heating Duty"},
        {"type": "analog-input", "instance": 8, "name": "Cooling Valve Flow Meter"},
        {"type": "multi-state-value", "instance": 1101, "name": "Room_A Run Status Control"},
    ]
    return objs


def test_definitions_have_unique_nvs_safe_keys(defs):
    keys = [p["id"] for p in defs]
    assert len(set(keys)) == len(keys)
    assert all(len(k) <= 15 for k in keys), "NVS keys are limited to 15 characters"
    assert {p["group"] for p in defs} == {"control", "health"}
    assert [p["id"] for p in defs if p["group"] == "control"] == ["sys_pwr_cmd", "sys_pwr_fb", "boost_mode"]


def test_reference_program_matches_every_point_to_its_verified_instance(defs):
    result = run_matcher(defs, reference_objects(defs))
    for p in defs:
        r = result[p["id"]]
        assert r["status"] == "matched", p["id"]
        assert (r["best"]["type"], r["best"]["instance"]) == (p["default"]["type"], p["default"]["instance"]), p["id"]
        assert r["best"]["score"] == 100


@pytest.mark.skipif(not CATALOG.exists(), reason="local commissioning catalogue not present")
def test_real_controller_catalogue_matches_all_points(defs):
    objects = json.loads(CATALOG.read_text())["objects"]
    result = run_matcher(defs, objects)
    for p in defs:
        r = result[p["id"]]
        assert r["status"] == "matched", p["id"]
        assert (r["best"]["type"], r["best"]["instance"]) == (p["default"]["type"], p["default"]["instance"]), p["id"]


def test_missing_point_is_reported_not_guessed(defs):
    objs = [o for o in reference_objects(defs) if o["name"] != "FCU Run Status"]
    result = run_matcher(defs, objs)
    # "Room_A Run Status" is similar but must not be applied.
    assert result["sys_pwr_fb"]["status"] == "missing"
    assert result["sys_pwr_fb"]["best"]["score"] < 90


def test_pattern_variant_matches_with_lower_score(defs):
    objs = [o for o in reference_objects(defs) if o["name"] != "FCU Operating Mode"]
    objs.append({"type": "multi-state-value", "instance": 7, "name": "fcu_operation_mode"})
    r = run_matcher(defs, objs)["boost_mode"]
    assert r["status"] == "matched"
    assert (r["best"]["instance"], r["best"]["score"]) == (7, 90)


def test_incompatible_type_is_never_a_candidate(defs):
    objs = [o for o in reference_objects(defs) if o["name"] != "BMS Run Signal"]
    objs.append({"type": "binary-input", "instance": 13, "name": "BMS Run Signal"})
    r = run_matcher(defs, objs)["sys_pwr_cmd"]
    # A binary-input cannot be written, so the system power command stays unmapped.
    assert r["status"] == "missing"


def test_two_identical_names_are_ambiguous(defs):
    objs = reference_objects(defs) + [{"type": "binary-value", "instance": 99, "name": "BMS Run Signal"}]
    r = run_matcher(defs, objs)["sys_pwr_cmd"]
    assert r["status"] == "missing" and r["ambiguous"] is True
