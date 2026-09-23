"""
Tier 2: Boundary Value Analysis - Circuit Breaker State Transition Thresholds
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetTargetHealth

@pytest.fixture
def cb_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_zero_timeout_threshold_online(cb_system):
    sim, worker = cb_system
    assert worker.health == BACnetTargetHealth.ONLINE
    assert worker.consecutive_timeouts == 0

def test_one_timeout_threshold_degraded(cb_system):
    sim, worker = cb_system
    sim.set_online(False)
    worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
    assert worker.health == BACnetTargetHealth.DEGRADED
    assert worker.consecutive_timeouts == 1

def test_two_timeouts_threshold_degraded(cb_system):
    sim, worker = cb_system
    sim.set_online(False)
    worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
    worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
    assert worker.health == BACnetTargetHealth.DEGRADED
    assert worker.consecutive_timeouts == 2

def test_three_timeouts_threshold_offline(cb_system):
    sim, worker = cb_system
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
    assert worker.health == BACnetTargetHealth.OFFLINE
    assert worker.consecutive_timeouts == 3

def test_offline_sustained_on_further_failures(cb_system):
    sim, worker = cb_system
    sim.set_online(False)
    for _ in range(5):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
    assert worker.health == BACnetTargetHealth.OFFLINE

def test_single_probe_restores_online_immediately(cb_system):
    sim, worker = cb_system
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
    assert worker.health == BACnetTargetHealth.OFFLINE

    sim.set_online(True)
    worker.trigger_probe_now()
    assert worker.health == BACnetTargetHealth.ONLINE
    assert worker.consecutive_timeouts == 0
