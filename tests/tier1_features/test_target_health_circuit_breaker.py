"""
Tier 1: Feature Area 5 - Target Health & Circuit Breaker Tests
"""
import time
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority, BACnetTargetHealth

@pytest.fixture
def cb_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_initial_online_state(cb_system):
    sim, worker = cb_system
    assert worker.health == BACnetTargetHealth.ONLINE
    assert worker.consecutive_timeouts == 0

def test_degraded_state_transition(cb_system):
    sim, worker = cb_system
    sim.set_online(False)

    # 1 timeout -> DEGRADED
    worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)
    assert worker.health == BACnetTargetHealth.DEGRADED
    assert worker.consecutive_timeouts == 1

    # 2 timeouts -> still DEGRADED
    worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)
    assert worker.health == BACnetTargetHealth.DEGRADED
    assert worker.consecutive_timeouts == 2

def test_offline_state_transition(cb_system):
    sim, worker = cb_system
    sim.set_online(False)

    # 3 consecutive timeouts -> OFFLINE
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)

    assert worker.health == BACnetTargetHealth.OFFLINE
    assert worker.consecutive_timeouts >= 3

def test_fast_fail_in_offline_state(cb_system):
    sim, worker = cb_system
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)
    assert worker.health == BACnetTargetHealth.OFFLINE

    # Normal read in OFFLINE state should return immediately (<10ms) with TARGET_OFFLINE
    t0 = time.time()
    resp = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
                               prio=BACnetPriority.NORMAL, timeout_ms=500)
    elapsed = time.time() - t0
    assert resp.status == BACnetStatus.TARGET_OFFLINE
    assert elapsed < 0.05
    assert worker.fast_fail_count >= 1

def test_heartbeat_recovery_probe(cb_system):
    sim, worker = cb_system
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)
    assert worker.health == BACnetTargetHealth.OFFLINE

    # Target comes back online
    sim.set_online(True)
    # Trigger probe
    worker.trigger_probe_now()

    assert worker.health == BACnetTargetHealth.ONLINE
    assert worker.consecutive_timeouts == 0
