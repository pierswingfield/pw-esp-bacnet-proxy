"""
Tier 1: Feature Area 1 - Canonical HVAC Core Domain Model Tests
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine

@pytest.fixture
def hvac_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_default_room_configuration(hvac_system):
    sim, worker, core = hvac_system
    assert core.get_room_count() == 5
    room_a = core.get_room(0)
    assert room_a is not None
    assert room_a.name == "Room A"
    assert room_a.active is True
    assert room_a.setpoint_instance == 1100
    assert room_a.temperature_instance == 1101
    assert room_a.power_instance == 1101

    room_b = core.get_room(1)
    assert room_b.name == "Room B"
    assert room_b.active is True

    room_c = core.get_room(2)
    assert room_c.name == "Room C"
    assert room_c.active is False

def test_active_room_count(hvac_system):
    sim, worker, core = hvac_system
    active_rooms = core.get_active_rooms()
    assert len(active_rooms) == 2
    assert active_rooms[0][0] == 0 # Room A index
    assert active_rooms[1][0] == 1 # Room B index

def test_room_index_access_contract(hvac_system):
    sim, worker, core = hvac_system
    assert core.get_room(0) is not None
    assert core.get_room(4) is not None
    assert core.get_room(5) is None
    assert core.get_room(-1) is None
    assert core.get_room(99) is None

def test_semantic_setpoint_command(hvac_system):
    sim, worker, core = hvac_system
    # Write setpoint for Room A (22.5°C)
    ok, applied = core.set_room_setpoint(0, 22.5)
    assert ok is True
    assert applied == 22.5

    # Verify BACnet Present_Value was updated
    prop = sim.get_property(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop is not None
    assert prop[1] == 22.5

    # Read back through core
    read_ok, val = core.get_room_setpoint(0)
    assert read_ok is True
    assert val == 22.5

def test_semantic_room_power_command(hvac_system):
    sim, worker, core = hvac_system
    # Turn Room B power off
    ok = core.set_room_power(1, False)
    assert ok is True

    prop = sim.get_property(BACnetObjectType.BINARY_VALUE, 1201, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop is not None
    assert prop[1] == 0

    read_ok, is_on = core.get_room_power(1)
    assert read_ok is True
    assert is_on is False

def test_semantic_system_power_command(hvac_system):
    sim, worker, core = hvac_system
    # Master system power write is BV:13
    ok = core.set_system_power(False)
    assert ok is True
    prop13 = sim.get_property(BACnetObjectType.BINARY_VALUE, 13, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop13[1] == 0

    # System status readback from BV:1
    sim.set_property(BACnetObjectType.BINARY_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE, 9, 0)
    read_ok, sys_status = core.get_system_power()
    assert read_ok is True
    assert sys_status is False
