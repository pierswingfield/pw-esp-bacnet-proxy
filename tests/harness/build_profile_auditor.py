"""
Build Profile & Partition Table Static Auditor
Validates sdkconfig defaults, partition configurations, flash headroom,
and verifies catalog invariance against tampering.
"""
import hashlib
import os
import re
from typing import Dict, Any, List

class BuildProfileAuditor:
    """
    Static analysis and audit validator for ESP-BACnet build artifacts.
    """
    def __init__(self, repo_root: str):
        self.repo_root = repo_root
        self.firmware_dir = os.path.join(repo_root, "firmware", "bacnet_bridge")
        self.catalog_path = os.path.join(repo_root, "bacnet-object-catalog.json")

    def get_catalog_sha256(self) -> str:
        if not os.path.exists(self.catalog_path):
            raise FileNotFoundError(f"Catalog not found at {self.catalog_path}")
        with open(self.catalog_path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()

    def audit_w5500_sdkconfig(self) -> Dict[str, Any]:
        """Audits sdkconfig.w5500.defaults for legacy 4MB safety rules."""
        path = os.path.join(self.firmware_dir, "sdkconfig.w5500.defaults")
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()

        results = {
            "has_4mb_flash": bool(re.search(r"CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y", content)),
            "has_spi_eth": bool(re.search(r"CONFIG_EXAMPLE_USE_SPI_ETHERNET=y", content)),
            "has_w5500": bool(re.search(r"CONFIG_EXAMPLE_USE_W5500=y", content)),
            "internal_eth_disabled": bool(re.search(r"CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET=n", content)),
            "matter_disabled": not bool(re.search(r"CONFIG_ENABLE_ESP_MATTER=y", content))
        }
        return results

    def audit_t_eth_lite_sdkconfig(self) -> Dict[str, Any]:
        """Audits sdkconfig.t_eth_lite.defaults for 16MB PSRAM profile."""
        path = os.path.join(self.firmware_dir, "sdkconfig.t_eth_lite.defaults")
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()

        results = {
            "has_16mb_flash": bool(re.search(r"CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y", content)),
            "has_internal_eth": bool(re.search(r"CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET=y", content)),
            "has_rtl8201_phy": bool(re.search(r"CONFIG_EXAMPLE_ETH_PHY_RTL8201=y", content)),
            "has_spiram": bool(re.search(r"CONFIG_SPIRAM=y", content)),
            "has_spiram_malloc": bool(re.search(r"CONFIG_SPIRAM_USE_MALLOC=y", content)),
            "has_internal_reserve": bool(re.search(r"CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768", content)),
            "has_wifi_lwip_spiram": bool(re.search(r"CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y", content))
        }
        return results

    def parse_partition_csv(self, csv_relpath: str) -> List[Dict[str, Any]]:
        """Parses an ESP-IDF partition table CSV into structured rows."""
        path = os.path.join(self.firmware_dir, csv_relpath)
        if not os.path.exists(path):
            raise FileNotFoundError(f"Partition CSV not found at {path}")

        rows = []
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = [p.strip() for p in line.split(",")]
                if len(parts) >= 5:
                    name = parts[0]
                    p_type = parts[1]
                    subtype = parts[2]
                    offset = parts[3]
                    size = parts[4]
                    rows.append({
                        "name": name,
                        "type": p_type,
                        "subtype": subtype,
                        "offset": offset,
                        "size": size
                    })
        return rows
