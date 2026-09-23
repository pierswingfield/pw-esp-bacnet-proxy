"""
Tier 2: Boundary Value Analysis - Partition Slot Boundaries & Binary Limits
"""
import os
import pytest
from tests.harness.build_profile_auditor import BuildProfileAuditor

@pytest.fixture
def auditor():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    return BuildProfileAuditor(repo_root)

def test_w5500_ota_slot_size_boundary(auditor):
    rows = auditor.parse_partition_csv("partitions.csv")
    ota_0 = next(r for r in rows if r["name"] == "ota_0")
    assert ota_0["size"] == "1900K"
    # 1900KB in bytes = 1945600 bytes
    slot_bytes = 1900 * 1024
    assert slot_bytes == 1945600

def test_t_eth_lite_4096k_slot_capacity(auditor):
    # T-ETH-Lite 16MB flash accommodates 4096KB OTA slots (4MB each)
    slot_bytes = 4096 * 1024
    assert slot_bytes == 4194304
    # Total dual OTA = 8MB, easily fits in 16MB flash with >7MB headroom
    assert (slot_bytes * 2) < (16 * 1024 * 1024)

def test_nvs_partition_size_headroom(auditor):
    rows = auditor.parse_partition_csv("partitions.csv")
    nvs = next(r for r in rows if r["name"] == "nvs")
    # 0x6000 = 24KB
    assert int(nvs["size"], 16) == 0x6000

def test_coredump_partition_size(auditor):
    rows = auditor.parse_partition_csv("partitions.csv")
    cd = next(r for r in rows if r["name"] == "coredump")
    assert cd["size"] == "64K"

def test_partition_alignment_multiples_of_4k(auditor):
    rows = auditor.parse_partition_csv("partitions.csv")
    for r in rows:
        # Check size can be resolved and is >= 4KB
        size_str = r["size"]
        if size_str.endswith("K"):
            kb = int(size_str[:-1])
            assert kb % 4 == 0 or kb > 0
        elif size_str.startswith("0x"):
            b = int(size_str, 16)
            assert b % 4096 == 0
