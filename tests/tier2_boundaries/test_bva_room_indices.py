"""
Tier 2: Boundary Value Analysis - Room Index Limits Tests
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine

@pytest.fixture
def hvac_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_first_room_index_boundary(hvac_system):
    sim, worker, core = hvac_system
    room = core.get_room(0)
    assert room is not None
    assert room.name == "Room A"

def test_last_active_room_index_boundary(hvac_system):
    sim, worker, core = hvac_system
    room = core.get_room(4)
    assert room is not None
    assert room.name == "Room E"

def test_out_of_bounds_positive_room_index(hvac_system):
    sim, worker, core = hvac_system
    # Room count is 5, so index 5 is out of bounds
    assert core.get_room(5) is None
    ok, _ = core.set_room_setpoint(5, 22.0)
    assert ok is False
    assert core.set_room_power(5, True) is False

def test_extreme_out_of_bounds_indices(hvac_system):
    sim, worker, core = hvac_system
    assert core.get_room(-1) is None
    assert core.get_room(8) is None
    assert core.get_room(255) is None
    assert core.get_room(65535) is None

def test_inactive_room_operations(hvac_system):
    sim, worker, core = hvac_system
    # Room C (index 2) is inactive by default
    room_c = core.get_room(2)
    assert room_c.active is False
    # Semantic operations on configured but inactive rooms can still execute safely
    ok, applied = core.set_room_setpoint(2, 20.0)
    assert ok is True
    assert applied == 20.0
