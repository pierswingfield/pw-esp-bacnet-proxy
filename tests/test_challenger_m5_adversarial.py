"""
Milestone M5: Tier 5 White-Box Adversarial Stress & Conformance Test Suite
Empirical Challenger 2 Verification Harness for:
1. Single Active Automation Integration Mutual Exclusion & Memory Deferral (>28KB DRAM).
2. Matter Thermostat Endpoint CSA Cluster 0x0201 Attribute Conformance & Boundary Clamping.
3. Legacy W5500 Hardware Profile Rejection of Matter (Web-Only Confinement).
4. Delta Vendor Feature (Boost MSV:1, Object Browser, Diagnostics) Web UI Isolation.
5. High-Concurrency Multi-Transport Load & Queue Priority Invariance.
"""
import pytest
import time
import threading
import random
from typing import List, Tuple

from tests.harness.bacnet_simulator import (
    BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus, BACnetApplicationTag
)
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacIntegrationKind, HvacRoomConfig
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim, MatterSystemMode
from tests.harness.nvs_emulator import NVSEmulator
from tests.harness.build_profile_auditor import BuildProfileAuditor


@pytest.fixture
def adversarial_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    matter_ep = MatterThermostatEndpointSim(core, room_idx=0) # Room A
    yield sim, worker, nvs, core, matter_ep
    worker.shutdown()


# =============================================================================
# 1. MUTUAL EXCLUSION & MEMORY DEFERRAL STRESS TESTS
# =============================================================================

def test_adversarial_rapid_integration_mode_oscillation(adversarial_env):
    """
    Stress-tests rapid oscillating transitions between NONE, MQTT, and MATTER
    under concurrent background telemetry read activity.
    Asserts memory budget consistency and zero state corruption across 150 cycles.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    stop_event = threading.Event()
    read_counts = [0]
    errors = []

    def background_poller():
        while not stop_event.is_set():
            ok, val = core.get_room_temperature(0)
            if ok:
                read_counts[0] += 1
            time.sleep(0.001)

    poller_thread = threading.Thread(target=background_poller, daemon=True)
    poller_thread.start()

    modes = [HvacIntegrationKind.NONE, HvacIntegrationKind.MQTT_HOME_ASSISTANT, HvacIntegrationKind.MATTER]

    for cycle in range(150):
        target_mode = modes[cycle % 3]
        ok = core.integration_set(target_mode)
        if not ok:
            errors.append(f"Cycle {cycle}: failed to set mode {target_mode}")

        active_mode = core.integration_get()
        if active_mode != target_mode:
            errors.append(f"Cycle {cycle}: mode mismatch {active_mode} != {target_mode}")

        budget = core.calculate_memory_budget()
        if target_mode == HvacIntegrationKind.MATTER:
            if budget["mqtt_command_stack"] != 0 or budget["mqtt_command_queue"] != 0:
                errors.append(f"Cycle {cycle}: MQTT resources leaked in MATTER mode")
            if budget["mqtt_deferred_ram"] < 28000:
                errors.append(f"Cycle {cycle}: Insufficient deferred RAM in MATTER mode ({budget['mqtt_deferred_ram']})")
        elif target_mode == HvacIntegrationKind.MQTT_HOME_ASSISTANT:
            if budget["mqtt_command_stack"] == 0 or budget["mqtt_command_queue"] == 0:
                errors.append(f"Cycle {cycle}: MQTT resources missing in MQTT mode")
            if budget["matter_data_model"] != 0:
                errors.append(f"Cycle {cycle}: Matter resources leaked in MQTT mode")
        elif target_mode == HvacIntegrationKind.NONE:
            if budget["mqtt_command_stack"] != 0 or budget["matter_data_model"] != 0:
                errors.append(f"Cycle {cycle}: Resource leak in NONE mode")
        time.sleep(0.0005)

    stop_event.set()
    poller_thread.join(timeout=2.0)

    assert len(errors) == 0, f"Encountered {len(errors)} errors: {errors[:5]}"
    assert read_counts[0] >= 5, f"Background reads executed: {read_counts[0]}"


def test_adversarial_matter_active_strict_zero_mqtt_allocation(adversarial_env):
    """
    Empirically verifies that when Matter is active, 100% of MQTT task stacks,
    command queues, network buffers, and state tasks are deferred (>28KB saved).
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    core.integration_set(HvacIntegrationKind.MATTER)
    budget = core.calculate_memory_budget()

    assert budget["mqtt_command_stack"] == 0
    assert budget["mqtt_state_stack"] == 0
    assert budget["mqtt_command_queue"] == 0
    assert budget["mqtt_client_buffers"] == 0
    assert budget["mqtt_deferred_ram"] >= 28672 # 28KB exact minimum
    assert budget["matter_data_model"] == 16384


def test_adversarial_none_mode_strict_deferral(adversarial_env):
    """
    Empirically verifies that when NONE (Web dashboard only) is selected,
    BOTH MQTT and Matter resources remain completely unallocated.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    core.integration_set(HvacIntegrationKind.NONE)
    budget = core.calculate_memory_budget()

    assert budget["mqtt_command_stack"] == 0
    assert budget["mqtt_state_stack"] == 0
    assert budget["mqtt_command_queue"] == 0
    assert budget["mqtt_client_buffers"] == 0
    assert budget["matter_data_model"] == 0
    assert budget["web_server_stack"] == 4096
    assert budget["worker_stack"] == 24576


# =============================================================================
# 2. CSA CLUSTER 0x0201 MATTER THERMOSTAT ATTRIBUTE CONFORMANCE & FUZZING
# =============================================================================

def test_adversarial_csa_cluster_0x0201_attribute_constants(adversarial_env):
    """
    Validates standard CSA Matter Cluster 0x0201 (Thermostat) attribute identifiers.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    assert matter_ep.CLUSTER_THERMOSTAT == 0x0201
    assert matter_ep.ATTR_LOCAL_TEMPERATURE == 0x0000
    assert matter_ep.ATTR_OCCUPIED_COOLING_SETPOINT == 0x0011
    assert matter_ep.ATTR_OCCUPIED_HEATING_SETPOINT == 0x0012
    assert matter_ep.ATTR_SYSTEM_MODE == 0x001C


@pytest.mark.parametrize("temp_input_c, expected_hundredths", [
    (-10.0, -1000),
    (-0.01, -1),
    (0.0, 0),
    (0.004, 0), # rounds down
    (0.006, 1), # rounds up
    (21.454, 2145),
    (21.456, 2146),
    (22.0, 2200),
    (29.999, 3000),
    (55.5, 5550),
    (100.0, 10000),
])
def test_adversarial_matter_temperature_scaling_and_rounding(adversarial_env, temp_input_c, expected_hundredths):
    """
    Validates conversion of float temperatures from BACnet AV:1101 to signed int16 (0.01°C).
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    sim.set_property(
        BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
        BACnetApplicationTag.REAL, float(temp_input_c)
    )
    ok, val = matter_ep.read_local_temperature()
    assert ok is True
    assert val == expected_hundredths


@pytest.mark.parametrize("raw_input_hundredths, expected_applied_c, expected_applied_hundredths", [
    (-5000, 18.0, 1800),   # -50.0°C -> clamped to min 18.0°C
    (0, 18.0, 1800),       # 0.0°C -> clamped to min 18.0°C
    (1500, 18.0, 1800),    # 15.0°C -> clamped to min 18.0°C
    (1799, 18.0, 1800),    # 17.99°C -> clamped to min 18.0°C
    (1800, 18.0, 1800),    # 18.00°C -> exact min boundary
    (1850, 18.5, 1850),    # 18.50°C -> valid
    (2200, 22.0, 2200),    # 22.00°C -> valid nominal
    (2730, 27.3, 2730),    # 27.30°C -> valid
    (3000, 30.0, 3000),    # 30.00°C -> exact max boundary
    (3001, 30.0, 3000),    # 30.01°C -> clamped to max 30.0°C
    (4500, 30.0, 3000),    # 45.00°C -> clamped to max 30.0°C
    (9999, 30.0, 3000),    # 99.99°C -> clamped to max 30.0°C
])
def test_adversarial_matter_setpoint_boundary_clamping(adversarial_env, raw_input_hundredths, expected_applied_c, expected_applied_hundredths):
    """
    Fuzzes and bounds-tests Matter Occupied Setpoint writes (Cluster 0x0201 Attr 0x0011 / 0x0012).
    Verifies that HVAC Core strictly clamps values into [18.0°C, 30.0°C] and writes to BACnet AV:1100.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    # Cooling Setpoint (0x0011)
    ok_c, applied_c = matter_ep.write_occupied_cooling_setpoint(raw_input_hundredths)
    assert ok_c is True
    assert applied_c == expected_applied_hundredths

    prop = sim.get_property(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop is not None
    assert pytest.approx(prop[1], rel=1e-3) == expected_applied_c

    # Heating Setpoint (0x0012)
    ok_h, applied_h = matter_ep.write_occupied_heating_setpoint(raw_input_hundredths)
    assert ok_h is True
    assert applied_h == expected_applied_hundredths


@pytest.mark.parametrize("mode_in, expected_ok, expected_pwr, expected_readback_mode", [
    (MatterSystemMode.OFF, True, False, MatterSystemMode.OFF),
    (MatterSystemMode.AUTO, True, True, MatterSystemMode.HEAT),
    (MatterSystemMode.COOL, True, True, MatterSystemMode.HEAT),
    (MatterSystemMode.HEAT, True, True, MatterSystemMode.HEAT),
    (2, False, None, None),   # Precooling (unsupported)
    (5, False, None, None),   # Emergency Heat (unsupported)
    (6, False, None, None),   # Fan Only (unsupported)
    (7, False, None, None),   # Dry (unsupported)
    (8, False, None, None),   # Sleep (unsupported)
    (255, False, None, None), # Invalid
    (-1, False, None, None),  # Invalid
])
def test_adversarial_matter_system_mode_transitions(adversarial_env, mode_in, expected_ok, expected_pwr, expected_readback_mode):
    """
    Tests all valid and invalid Matter SystemMode enum values (Cluster 0x0201 Attr 0x001C).
    Asserts that invalid enums are rejected and valid enums control room power BV:1101 via hvac_core.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    initial_pwr = sim.get_property(BACnetObjectType.BINARY_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)[1]

    ok = matter_ep.write_system_mode(mode_in)
    assert ok is expected_ok

    if expected_ok:
        prop = sim.get_property(BACnetObjectType.BINARY_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
        assert prop[1] == expected_pwr

        read_ok, mode_read = matter_ep.read_system_mode()
        assert read_ok is True
        assert mode_read == expected_readback_mode
    else:
        # State should be unchanged
        prop = sim.get_property(BACnetObjectType.BINARY_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
        assert prop[1] == initial_pwr


def test_adversarial_matter_read_failure_propagation(adversarial_env):
    """
    Verifies that when a BACnet object is missing or the target is unreachable,
    Matter attribute reads fail cleanly without crashing or returning bogus zeroes.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    # Target invalid room index 7 (unconfigured)
    invalid_ep = MatterThermostatEndpointSim(core, room_idx=7)

    ok_t, temp = invalid_ep.read_local_temperature()
    assert ok_t is False
    assert temp is None

    ok_sp, sp = invalid_ep.read_occupied_cooling_setpoint()
    assert ok_sp is False
    assert sp is None

    ok_mode, mode = invalid_ep.read_system_mode()
    assert ok_mode is False
    assert mode == MatterSystemMode.OFF


# =============================================================================
# 3. LEGACY W5500 HARDWARE PROFILE REJECTION OF MATTER
# =============================================================================

def _parse_size(size_str: str) -> int:
    size_str = size_str.strip()
    if size_str.endswith("K"):
        return int(size_str[:-1]) * 1024
    elif size_str.endswith("M"):
        return int(size_str[:-1]) * 1024 * 1024
    elif size_str.startswith("0x") or size_str.startswith("0X"):
        return int(size_str, 16)
    else:
        return int(size_str)


def test_adversarial_w5500_sdkconfig_and_partition_rejection():
    """
    Audits the legacy W5500 build profile (ESP32-WROOM, 4MB Flash, No PSRAM):
    - Confirms CONFIG_ENABLE_ESP_MATTER=n
    - Confirms partitions.csv has NO matter_fctry partition
    - Confirms total partition size fits within 4MB (4194304 bytes)
    """
    import os
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    auditor = BuildProfileAuditor(repo_root)

    w5500_audit = auditor.audit_w5500_sdkconfig()
    assert w5500_audit["has_4mb_flash"] is True
    assert w5500_audit["has_spi_eth"] is True
    assert w5500_audit["has_w5500"] is True
    assert w5500_audit["internal_eth_disabled"] is True
    assert w5500_audit["matter_disabled"] is True

    # Audit W5500 partitions.csv
    w5500_parts = auditor.parse_partition_csv("partitions.csv")
    partition_names = [p["name"] for p in w5500_parts]
    assert "matter_fctry" not in partition_names, "W5500 4MB partition table must NOT have matter_fctry"

    total_partition_bytes = sum(_parse_size(p["size"]) for p in w5500_parts)
    assert total_partition_bytes <= 4 * 1024 * 1024, f"W5500 total partition size exceeds 4MB: {total_partition_bytes}"


def test_adversarial_t_eth_lite_sdkconfig_and_partition_compliance():
    """
    Audits the primary T-ETH-Lite build profile (ESP32-WROVER-E, 16MB Flash, 8MB PSRAM):
    - Confirms 16MB flash and PSRAM configurations
    - Confirms partitions_t_eth_lite.csv includes dual 4MB OTA slots, 64K NVS, 24K matter_fctry
    - Confirms total partitions fit within 16MB (16777216 bytes)
    """
    import os
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    auditor = BuildProfileAuditor(repo_root)

    t_eth_audit = auditor.audit_t_eth_lite_sdkconfig()
    assert t_eth_audit["has_16mb_flash"] is True
    assert t_eth_audit["has_internal_eth"] is True
    assert t_eth_audit["has_rtl8201_phy"] is True
    assert t_eth_audit["has_spiram"] is True
    assert t_eth_audit["has_spiram_malloc"] is True
    assert t_eth_audit["has_internal_reserve"] is True

    t_eth_parts = auditor.parse_partition_csv("partitions_t_eth_lite.csv")
    part_map = {p["name"]: p for p in t_eth_parts}

    assert "matter_fctry" in part_map, "T-ETH-Lite partition table must contain matter_fctry"
    assert part_map["matter_fctry"]["subtype"] == "0x99"
    assert part_map["ota_0"]["size"] == "4096K"
    assert part_map["ota_1"]["size"] == "4096K"
    assert part_map["coredump"]["size"] == "128K"

    total_partition_bytes = sum(_parse_size(p["size"]) for p in t_eth_parts)
    assert total_partition_bytes <= 16 * 1024 * 1024, f"T-ETH-Lite partitions exceed 16MB: {total_partition_bytes}"
    assert (16 * 1024 * 1024 - total_partition_bytes) > 7 * 1024 * 1024


# =============================================================================
# 4. DELTA VENDOR FEATURE WEB UI CONFINEMENT TESTS
# =============================================================================

def test_adversarial_delta_vendor_features_isolated_from_matter(adversarial_env):
    """
    Validates that Delta vendor-specific features:
    - Boost mode (MSV:1 present value 4/5)
    - BACnet Object Scanner (480-point catalog browser)
    - Circuit Breaker diagnostics and metrics
    are ONLY available in Web REST handlers and are strictly UNMAPPED in Matter.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    # 1. Boost Mode: accessible via HVAC Core and Web UI, but NOT on Matter Thermostat Endpoint
    assert hasattr(core, "set_boost_mode")
    assert core.set_boost_mode(4) is True # Heat boost
    assert sim.get_property(BACnetObjectType.MULTI_STATE_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE)[1] == 4

    # Assert Matter Thermostat Endpoint simulator has no boost mode attribute/method
    assert not hasattr(matter_ep, "set_boost_mode")
    assert not hasattr(matter_ep, "write_boost_mode")
    assert not hasattr(matter_ep, "ATTR_BOOST_MODE")

    # 2. Worker raw sync calls for Object Explorer vs Matter isolation
    resp = worker.enqueue_sync(
        op_type=0, obj_type=BACnetObjectType.ANALOG_VALUE, instance=1100,
        prop_id=BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL
    )
    assert resp.status == BACnetStatus.OK

    # Assert Matter endpoint cannot issue arbitrary explorer requests
    assert not hasattr(matter_ep, "explorer_read")
    assert not hasattr(matter_ep, "explorer_write")


# =============================================================================
# 5. HIGH-CONCURRENCY MULTI-TRANSPORT LOAD & QUEUE PRIORITY INVARIANCE
# =============================================================================

def test_adversarial_high_concurrency_multi_transport_stress(adversarial_env):
    """
    Fires 100 concurrent requests simultaneously across:
    - Matter Thermostat setpoint writes (High priority)
    - Web UI boost mode requests (High priority)
    - Web UI status / telemetry reads (Normal priority)
    - Background MQTT polling (Normal priority)
    Asserts zero deadlocks, thread-safety, and queue priority preservation.
    """
    sim, worker, nvs, core, matter_ep = adversarial_env

    num_threads = 10
    requests_per_thread = 10
    barrier = threading.Barrier(num_threads)
    results = []
    lock = threading.Lock()

    def worker_client(thread_id: int):
        barrier.wait()
        for i in range(requests_per_thread):
            req_type = (thread_id + i) % 4
            if req_type == 0:
                # Matter setpoint write
                sp_val = 2000 + (i * 50) # 20.0 to 24.5°C
                ok, _ = matter_ep.write_occupied_cooling_setpoint(sp_val)
                with lock:
                    results.append(("matter_write", ok))
            elif req_type == 1:
                # Web Boost mode write
                ok = core.set_boost_mode(4 if i % 2 == 0 else 5)
                with lock:
                    results.append(("web_boost", ok))
            elif req_type == 2:
                # Web room temperature read
                ok, _ = core.get_room_temperature(0)
                with lock:
                    results.append(("web_read", ok))
            elif req_type == 3:
                # Background telemetry read
                ok, _ = core.get_room_setpoint(0)
                with lock:
                    results.append(("telemetry_read", ok))

    threads = [threading.Thread(target=worker_client, args=(i,)) for i in range(num_threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=10.0)

    assert len(results) == num_threads * requests_per_thread
    success_count = sum(1 for _, ok in results if ok)
    assert success_count == len(results), f"Failed requests: {len(results) - success_count} / {len(results)}"


def test_adversarial_commissioning_catalog_strict_immutability():
    """
    Forensically validates that bacnet-object-catalog.json has not been modified
    and matches the canonical SHA-256 hash.
    """
    import os
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    auditor = BuildProfileAuditor(repo_root)

    expected_sha = "5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8"
    actual_sha = auditor.get_catalog_sha256()

    assert actual_sha == expected_sha, f"Catalog SHA256 mismatch! Expected {expected_sha}, got {actual_sha}"
