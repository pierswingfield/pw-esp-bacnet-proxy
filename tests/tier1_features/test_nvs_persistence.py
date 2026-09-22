"""
Tier 1: Feature Area 2 - NVS Persistence Tests (nvs_rooms, nvs_integ)
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacRoomConfig, HvacIntegrationKind
from tests.harness.nvs_emulator import NVSEmulator

@pytest.fixture
def persisted_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()

def test_room_config_nvs_save_and_load(persisted_system):
    sim, worker, nvs, core = persisted_system
    # Modify Room A name and active status of Room C
    core.rooms[0].name = "Master Bedroom"
    core.rooms[2].active = True
    assert core.rooms_save() is True

    # Create fresh core instance and load from NVS
    new_core = HvacCoreEngine(worker, nvs)
    new_core.rooms_load()
    assert new_core.get_room(0).name == "Master Bedroom"
    assert new_core.get_room(2).active is True
    assert len(new_core.get_active_rooms()) == 3

def test_custom_room_count_persistence(persisted_system):
    sim, worker, nvs, core = persisted_system
    # Add 3 more rooms up to max 8
    core.room_count = 8
    core.rooms.append(HvacRoomConfig("Room F", True, 1600, 1601, 1601, 1602, 1605, 1606))
    core.rooms.append(HvacRoomConfig("Room G", True, 1700, 1701, 1701, 1702, 1705, 1706))
    core.rooms.append(HvacRoomConfig("Room H", False, 1800, 1801, 1801, 1802, 1805, 1806))
    assert core.rooms_save() is True

    new_core = HvacCoreEngine(worker, nvs)
    new_core.rooms_load()
    assert new_core.get_room_count() == 8
    assert new_core.get_room(7).name == "Room H"

def test_integration_mode_default_mqtt(persisted_system):
    sim, worker, nvs, core = persisted_system
    # Fresh boot with empty NVS must default to MQTT
    core.integration_load()
    assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT

def test_integration_mode_set_and_load(persisted_system):
    sim, worker, nvs, core = persisted_system
    # Set to Matter
    assert core.integration_set(HvacIntegrationKind.MATTER) is True
    assert core.integration_get() == HvacIntegrationKind.MATTER

    # Verify persistence after reload
    new_core = HvacCoreEngine(worker, nvs)
    new_core.integration_load()
    assert new_core.integration_get() == HvacIntegrationKind.MATTER

    # Set to None
    assert new_core.integration_set(HvacIntegrationKind.NONE) is True
    assert new_core.integration_get() == HvacIntegrationKind.NONE

def test_integration_reset(persisted_system):
    sim, worker, nvs, core = persisted_system
    core.integration_set(HvacIntegrationKind.MATTER)
    assert core.integration_get() == HvacIntegrationKind.MATTER

    core.integration_reset()
    assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT
