"""
Tier 4: Scenario 3 - NVS Cold Boot Persistence & Corruption Resilience
Simulates cold power cycle, custom room persistence, and recovery from flash corruption.
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacRoomConfig, HvacIntegrationKind
from tests.harness.nvs_emulator import NVSEmulator

@pytest.fixture
def nvs_scenario_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()

def test_scenario3_nvs_boot_persistence(nvs_scenario_env):
    sim, worker, nvs, core = nvs_scenario_env

    # Step 1: Provision custom 6-room configuration
    core.room_count = 6
    core.rooms[0].name = "Executive Suite"
    core.rooms[1].name = "Conference Room"
    core.rooms[2].name = "Open Office A"
    core.rooms[2].active = True
    core.rooms.append(HvacRoomConfig("Server Room", True, 1600, 1601, 1601, 1602, 1605, 1606))
    assert core.rooms_save() is True

    # Step 2: Set integration mode to Matter
    assert core.integration_set(HvacIntegrationKind.MATTER) is True

    # Step 3: Simulate power-cycle reboot
    reboot_core = HvacCoreEngine(worker, nvs)
    reboot_core.rooms_load()
    reboot_core.integration_load()

    # Verify state after reboot
    assert reboot_core.get_room_count() == 6
    assert reboot_core.get_room(0).name == "Executive Suite"
    assert reboot_core.get_room(1).name == "Conference Room"
    assert reboot_core.get_room(2).active is True
    assert reboot_core.get_room(5).name == "Server Room"
    assert reboot_core.integration_get() == HvacIntegrationKind.MATTER

    # Step 4: Simulate partial flash corruption
    handle = nvs.open("nvs_integ", readonly=False)
    nvs.set_u8(handle, "mode", 0xFE) # Invalid mode value
    nvs.commit(handle)
    nvs.close(handle)

    # Step 5: Reboot from corrupted NVS
    recovery_core = HvacCoreEngine(worker, nvs)
    recovery_core.integration_load()
    # Must gracefully fall back to default MQTT mode without throwing exception
    assert recovery_core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT
