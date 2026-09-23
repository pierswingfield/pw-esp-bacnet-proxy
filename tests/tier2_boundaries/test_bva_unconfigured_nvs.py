"""
Tier 2: Boundary Value Analysis - Unconfigured / Corrupted NVS Resilience
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacIntegrationKind
from tests.harness.nvs_emulator import NVSEmulator

@pytest.fixture
def empty_nvs_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator() # Brand new, empty NVS flash
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()

def test_clean_flash_cold_boot_rooms(empty_nvs_system):
    sim, worker, nvs, core = empty_nvs_system
    core.rooms_load()
    # Must initialize 5 canonical rooms from hardcoded default constants
    assert core.get_room_count() == 5
    assert core.get_room(0).name == "Room A"
    assert core.get_room(0).active is True
    assert core.get_room(1).active is True
    assert core.get_room(2).active is False

def test_clean_flash_cold_boot_integration_mode(empty_nvs_system):
    sim, worker, nvs, core = empty_nvs_system
    core.integration_load()
    # Must initialize to MQTT for backward-compatibility
    assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT

def test_corrupted_integration_mode_fallback(empty_nvs_system):
    sim, worker, nvs, core = empty_nvs_system
    # Write invalid mode 99 to NVS
    handle = nvs.open("nvs_integ", readonly=False)
    nvs.set_u8(handle, "mode", 99)
    nvs.commit(handle)
    nvs.close(handle)

    core.integration_load()
    # Invalid mode falls back to MQTT
    assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT

def test_partial_room_nvs_record_fallback(empty_nvs_system):
    sim, worker, nvs, core = empty_nvs_system
    # Write count = 2, but omit object instance IDs for room 0 and 1
    handle = nvs.open("nvs_rooms", readonly=False)
    nvs.set_u8(handle, "count", 2)
    nvs.set_str(handle, "r0_name", "Partial Room A")
    nvs.commit(handle)
    nvs.close(handle)

    core.rooms_load()
    assert core.get_room_count() == 2
    r0 = core.get_room(0)
    assert r0.name == "Partial Room A"
    # Omitted instance fields must fall back to default DAC-1180E instances
    assert r0.setpoint_instance == 1100
    assert r0.temperature_instance == 1101

def test_flash_wipe_and_reinitialization(empty_nvs_system):
    sim, worker, nvs, core = empty_nvs_system
    core.integration_set(HvacIntegrationKind.MATTER)
    assert core.integration_get() == HvacIntegrationKind.MATTER

    nvs.wipe_flash()
    core.integration_load()
    assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT
