"""
Tier 2: Boundary Value Analysis - Setpoint Limits and Clamping Tests
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

def test_setpoint_lower_bound_exact(hvac_system):
    sim, worker, core = hvac_system
    ok, applied = core.set_room_setpoint(0, 18.0)
    assert ok is True
    assert applied == 18.0

def test_setpoint_below_lower_bound_clamped(hvac_system):
    sim, worker, core = hvac_system
    # 17.9°C must be clamped to 18.0°C
    ok, applied = core.set_room_setpoint(0, 17.9)
    assert ok is True
    assert applied == 18.0

    # 0.0°C must be clamped to 18.0°C
    ok, applied = core.set_room_setpoint(0, 0.0)
    assert ok is True
    assert applied == 18.0

    # Negative temperature (-15.0°C) clamped to 18.0°C
    ok, applied = core.set_room_setpoint(0, -15.0)
    assert ok is True
    assert applied == 18.0

def test_setpoint_upper_bound_exact(hvac_system):
    sim, worker, core = hvac_system
    ok, applied = core.set_room_setpoint(0, 30.0)
    assert ok is True
    assert applied == 30.0

def test_setpoint_above_upper_bound_clamped(hvac_system):
    sim, worker, core = hvac_system
    # 30.1°C clamped to 30.0°C
    ok, applied = core.set_room_setpoint(0, 30.1)
    assert ok is True
    assert applied == 30.0

    # 99.9°C clamped to 30.0°C
    ok, applied = core.set_room_setpoint(0, 99.9)
    assert ok is True
    assert applied == 30.0

def test_setpoint_decimal_precision(hvac_system):
    sim, worker, core = hvac_system
    ok, applied = core.set_room_setpoint(0, 21.5)
    assert ok is True
    assert applied == 21.5

    ok, applied = core.set_room_setpoint(0, 23.25)
    assert ok is True
    assert abs(applied - 23.25) < 0.001
