"""
Tier 1: Feature Area 9 - Dual Profile Build Configuration Tests
"""
import os
import pytest
from tests.harness.build_profile_auditor import BuildProfileAuditor

@pytest.fixture
def auditor():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    return BuildProfileAuditor(repo_root)

def test_w5500_sdkconfig_safety_rules(auditor):
    res = auditor.audit_w5500_sdkconfig()
    assert res["has_4mb_flash"] is True
    assert res["has_spi_eth"] is True
    assert res["has_w5500"] is True
    assert res["internal_eth_disabled"] is True
    assert res["matter_disabled"] is True

def test_t_eth_lite_sdkconfig_rules(auditor):
    res = auditor.audit_t_eth_lite_sdkconfig()
    assert res["has_16mb_flash"] is True
    assert res["has_internal_eth"] is True
    assert res["has_rtl8201_phy"] is True
    assert res["has_spiram"] is True
    assert res["has_spiram_malloc"] is True
    assert res["has_internal_reserve"] is True
    assert res["has_wifi_lwip_spiram"] is True

def test_w5500_partition_table_fit(auditor):
    rows = auditor.parse_partition_csv("partitions.csv")
    names = [r["name"] for r in rows]
    assert "nvs" in names
    assert "otadata" in names
    assert "ota_0" in names
    assert "ota_1" in names

    ota_0 = next(r for r in rows if r["name"] == "ota_0")
    ota_1 = next(r for r in rows if r["name"] == "ota_1")
    assert ota_0["size"] == "1900K"
    assert ota_1["size"] == "1900K"

def test_validate_build_profiles_script_exists(auditor):
    script_path = os.path.join(auditor.repo_root, "tools", "validate_build_profiles.sh")
    assert os.path.exists(script_path)
    assert os.access(script_path, os.X_OK)

def test_partition_ota_symmetry(auditor):
    rows = auditor.parse_partition_csv("partitions.csv")
    ota_0 = next(r for r in rows if r["name"] == "ota_0")
    ota_1 = next(r for r in rows if r["name"] == "ota_1")
    assert ota_0["size"] == ota_1["size"]

def test_t_eth_lite_partition_table_layout(auditor):
    rows = auditor.parse_partition_csv("partitions_t_eth_lite.csv")
    names = [r["name"] for r in rows]
    assert "nvs" in names
    assert "otadata" in names
    assert "phy_init" in names
    assert "matter_fctry" in names
    assert "ota_0" in names
    assert "ota_1" in names
    assert "coredump" in names

    nvs = next(r for r in rows if r["name"] == "nvs")
    assert nvs["size"] == "0x10000"
    assert nvs["offset"] == "0x9000"

    mf = next(r for r in rows if r["name"] == "matter_fctry")
    assert mf["size"] == "0x6000"
    assert mf["subtype"] == "0x99"

    ota_0 = next(r for r in rows if r["name"] == "ota_0")
    ota_1 = next(r for r in rows if r["name"] == "ota_1")
    assert ota_0["size"] == "4096K"
    assert ota_1["size"] == "4096K"
    assert ota_0["offset"] == "0x30000"
    assert ota_1["offset"] == "0x430000"

    cd = next(r for r in rows if r["name"] == "coredump")
    assert cd["size"] == "128K"
    assert cd["offset"] == "0x830000"

def test_t_eth_lite_sdkconfig_custom_partition_and_matter(auditor):
    path = os.path.join(auditor.firmware_dir, "sdkconfig.t_eth_lite.defaults")
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    assert 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_t_eth_lite.csv"' in content
    assert "CONFIG_ENABLE_ESP_MATTER=y" in content
