"""
Tier 5 Adversarial Test: Partition Table Boundary Validation & Profile Isolation
Validates exact flash boundaries, sector alignments, offset continuity,
and cross-profile isolation for both 16MB T-ETH-Lite and 4MB W5500.
"""
import os
import pytest
from tests.harness.build_profile_auditor import BuildProfileAuditor

@pytest.fixture
def auditor():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    return BuildProfileAuditor(repo_root)

def parse_size_str(size_str: str) -> int:
    s = size_str.strip()
    if s.endswith("K") or s.endswith("k"):
        return int(s[:-1]) * 1024
    elif s.endswith("M") or s.endswith("m"):
        return int(s[:-1]) * 1024 * 1024
    elif s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    return int(s)

def parse_offset_str(offset_str: str) -> int:
    s = offset_str.strip()
    if not s:
        return 0
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    return int(s)

def test_t_eth_lite_partition_boundary_and_layout_validation(auditor):
    """
    Adversarial Partition Audit: T-ETH-Lite 16MB table (partitions_t_eth_lite.csv)
    must satisfy exact offsets, zero overlaps, 64KB OTA alignments, and >7MB free headroom.
    """
    rows = auditor.parse_partition_csv("partitions_t_eth_lite.csv")
    assert len(rows) == 7

    # Verify partition order and identities
    expected_order = ["nvs", "otadata", "phy_init", "matter_fctry", "ota_0", "ota_1", "coredump"]
    actual_order = [r["name"] for r in rows]
    assert actual_order == expected_order

    parsed_partitions = []
    for r in rows:
        offset = parse_offset_str(r["offset"])
        size = parse_size_str(r["size"])
        parsed_partitions.append({
            "name": r["name"],
            "type": r["type"],
            "subtype": r["subtype"],
            "offset": offset,
            "size": size,
            "end": offset + size
        })

    # Exact offset checks
    assert parsed_partitions[0]["offset"] == 0x9000
    assert parsed_partitions[0]["size"] == 0x10000 # 64KB NVS
    assert parsed_partitions[1]["offset"] == 0x19000
    assert parsed_partitions[1]["size"] == 0x2000  # 8KB otadata
    assert parsed_partitions[2]["offset"] == 0x1b000
    assert parsed_partitions[2]["size"] == 0x1000  # 4KB phy_init
    assert parsed_partitions[3]["offset"] == 0x1c000
    assert parsed_partitions[3]["size"] == 0x6000  # 24KB matter_fctry
    assert parsed_partitions[3]["subtype"] == "0x99"
    assert parsed_partitions[4]["offset"] == 0x30000
    assert parsed_partitions[4]["size"] == 4096 * 1024 # 4MB ota_0
    assert parsed_partitions[5]["offset"] == 0x430000
    assert parsed_partitions[5]["size"] == 4096 * 1024 # 4MB ota_1
    assert parsed_partitions[6]["offset"] == 0x830000
    assert parsed_partitions[6]["size"] == 128 * 1024 # 128KB coredump

    # Overlap and continuity validation
    for i in range(len(parsed_partitions) - 1):
        curr_p = parsed_partitions[i]
        next_p = parsed_partitions[i + 1]
        assert curr_p["end"] <= next_p["offset"], (
            f"Partition overlap detected between {curr_p['name']} (ends at 0x{curr_p['end']:X}) "
            f"and {next_p['name']} (starts at 0x{next_p['offset']:X})"
        )

    # Sector alignment (4KB multiples)
    for p in parsed_partitions:
        assert p["offset"] % 4096 == 0, f"Partition {p['name']} offset 0x{p['offset']:X} not 4KB aligned"
        assert p["size"] % 4096 == 0, f"Partition {p['name']} size {p['size']} not 4KB aligned"

    # OTA 64KB alignment
    assert parsed_partitions[4]["offset"] % 0x10000 == 0
    assert parsed_partitions[5]["offset"] % 0x10000 == 0

    # Total 16MB flash capacity check & free headroom
    total_flash_size = 16 * 1024 * 1024
    highest_offset = parsed_partitions[-1]["end"]
    assert highest_offset <= total_flash_size
    free_headroom = total_flash_size - highest_offset
    assert free_headroom >= 7 * 1024 * 1024, f"Insufficient flash headroom: {free_headroom} bytes free"

def test_w5500_partition_boundary_and_legacy_safety(auditor):
    """
    Adversarial Partition Audit: W5500 4MB table (partitions.csv)
    must strictly fit within 4MB (0x400000), contain dual 1900K slots,
    and have ZERO Matter partitions.
    """
    rows = auditor.parse_partition_csv("partitions.csv")
    names = [r["name"] for r in rows]

    assert "nvs" in names
    assert "otadata" in names
    assert "phy_init" in names
    assert "ota_0" in names
    assert "ota_1" in names
    assert "coredump" in names

    # Must NOT have any matter_fctry or matter partitions
    assert "matter_fctry" not in names
    for r in rows:
        assert r["subtype"] != "0x99"

    ota_0 = next(r for r in rows if r["name"] == "ota_0")
    ota_1 = next(r for r in rows if r["name"] == "ota_1")
    assert parse_size_str(ota_0["size"]) == 1900 * 1024
    assert parse_size_str(ota_1["size"]) == 1900 * 1024

    total_size = sum(parse_size_str(r["size"]) for r in rows)
    # Total assigned partition sizes must fit comfortably in 4MB
    assert total_size < 4 * 1024 * 1024

def test_dual_profile_sdkconfig_isolation_and_cross_contamination(auditor):
    """
    Adversarial Profile Isolation: Ensure complete decoupling between
    T-ETH-Lite and W5500 profiles without cross-contamination.
    """
    t_eth_aud = auditor.audit_t_eth_lite_sdkconfig()
    assert t_eth_aud["has_16mb_flash"] is True
    assert t_eth_aud["has_internal_eth"] is True
    assert t_eth_aud["has_rtl8201_phy"] is True
    assert t_eth_aud["has_spiram"] is True

    w5500_aud = auditor.audit_w5500_sdkconfig()
    assert w5500_aud["has_4mb_flash"] is True
    assert w5500_aud["has_spi_eth"] is True
    assert w5500_aud["has_w5500"] is True
    assert w5500_aud["internal_eth_disabled"] is True
    assert w5500_aud["matter_disabled"] is True

    # Read raw sdkconfig files to verify partition table configuration
    with open(os.path.join(auditor.firmware_dir, "sdkconfig.defaults"), "r") as f:
        defaults_raw = f.read()
    assert 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"' in defaults_raw

    with open(os.path.join(auditor.firmware_dir, "sdkconfig.t_eth_lite.defaults"), "r") as f:
        t_eth_raw = f.read()
    assert 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_t_eth_lite.csv"' in t_eth_raw
    assert 'CONFIG_ENABLE_ESP_MATTER=y' in t_eth_raw

    with open(os.path.join(auditor.firmware_dir, "sdkconfig.w5500.defaults"), "r") as f:
        w5500_raw = f.read()
    assert 'CONFIG_ENABLE_ESP_MATTER=n' in w5500_raw
