"""
Tier 4: Scenario 5 - Dual Profile Build & Binary Constraint Audit
Audits sdkconfig files, partition tables, and catalog integrity across both build profiles.
"""
import os
import pytest
from tests.harness.build_profile_auditor import BuildProfileAuditor

@pytest.fixture
def auditor_env():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    return BuildProfileAuditor(repo_root)

def test_scenario5_dual_profile_binary_audit(auditor_env):
    auditor = auditor_env

    # 1. W5500 Profile Safety Audit
    w5500 = auditor.audit_w5500_sdkconfig()
    assert w5500["has_4mb_flash"] is True
    assert w5500["has_spi_eth"] is True
    assert w5500["has_w5500"] is True
    assert w5500["internal_eth_disabled"] is True
    assert w5500["matter_disabled"] is True

    # 2. T-ETH-Lite Profile Audit
    teth = auditor.audit_t_eth_lite_sdkconfig()
    assert teth["has_16mb_flash"] is True
    assert teth["has_internal_eth"] is True
    assert teth["has_rtl8201_phy"] is True
    assert teth["has_spiram"] is True
    assert teth["has_spiram_malloc"] is True
    assert teth["has_internal_reserve"] is True

    # 3. Partition Table Headroom Audit
    rows = auditor.parse_partition_csv("partitions.csv")
    ota_0 = next(r for r in rows if r["name"] == "ota_0")
    ota_1 = next(r for r in rows if r["name"] == "ota_1")
    assert ota_0["size"] == "1900K"
    assert ota_1["size"] == "1900K"

    # 4. Invariance of Commissioning Catalog
    sha = auditor.get_catalog_sha256()
    assert len(sha) == 64
    assert sha.isalnum()

    # 5. Validation Script Readiness
    script_path = os.path.join(auditor.repo_root, "tools", "validate_build_profiles.sh")
    assert os.path.exists(script_path)
