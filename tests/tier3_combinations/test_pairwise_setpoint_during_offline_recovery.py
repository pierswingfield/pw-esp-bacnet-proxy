"""
Tier 3: Pairwise Combination - Setpoint Write During Target Offline Recovery
"""
import pytest
import time
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetTargetHealth
from tests.harness.hvac_core_engine import HvacCoreEngine

@pytest.fixture
def offline_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_setpoint_write_recovers_when_target_comes_online(offline_system):
    sim, worker, core = offline_system
    # Put target offline
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=20)
    assert worker.health == BACnetTargetHealth.OFFLINE

    # Bring target back online
    sim.set_online(True)
    worker.trigger_probe_now()
    assert worker.health == BACnetTargetHealth.ONLINE

    # User write setpoint
    ok, applied = core.set_room_setpoint(0, 23.5)
    assert ok is True
    assert applied == 23.5

def test_offline_probe_does_not_corrupt_subsequent_writes(offline_system):
    sim, worker, core = offline_system
    sim.set_online(False)
    worker.trigger_probe_now()
    sim.set_online(True)
    worker.trigger_probe_now()

    ok = core.set_room_power(0, True)
    assert ok is True

def test_circuit_breaker_resets_error_count_on_recovery(offline_system):
    sim, worker, core = offline_system
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=20)
    assert worker.consecutive_timeouts >= 3

    sim.set_online(True)
    worker.trigger_probe_now()
    assert worker.consecutive_timeouts == 0
