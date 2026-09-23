"""
Tier 1: Feature Area 10 - Catalog & Safety Invariance Tests
"""
import json
import os
import pytest
from tests.harness.build_profile_auditor import BuildProfileAuditor

@pytest.fixture
def auditor():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    return BuildProfileAuditor(repo_root)

def test_catalog_file_presence(auditor):
    assert os.path.exists(auditor.catalog_path)
    assert os.path.getsize(auditor.catalog_path) > 10000

def test_catalog_json_validity(auditor):
    with open(auditor.catalog_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    assert isinstance(data, (dict, list))

def test_catalog_checksum_stability(auditor):
    # Compute SHA256 checksum
    sha = auditor.get_catalog_sha256()
    assert len(sha) == 64
    assert sha.isalnum()

def test_no_hardware_flash_command_in_scripts(auditor):
    # Verify tools and scripts do not trigger uncoordinated esptool write_flash
    for dir_name in ("tools", "scripts"):
        target_dir = os.path.join(auditor.repo_root, dir_name)
        if os.path.exists(target_dir):
            for root, _, files in os.walk(target_dir):
                for file in files:
                    if file.endswith((".py", ".sh")):
                        with open(os.path.join(root, file), "r", encoding="utf-8") as f:
                            content = f.read()
                        assert "esptool.py write_flash" not in content
                        assert "idf.py flash" not in content

def test_catalog_object_instance_integrity(auditor):
    with open(auditor.catalog_path, "r", encoding="utf-8") as f:
        raw = f.read()
    # Check key object instances exist in catalog
    assert "1100" in raw # Room A setpoint
    assert "1101" in raw # Room A temp / power
    assert "1200" in raw # Room B setpoint
    assert "1201" in raw # Room B temp
