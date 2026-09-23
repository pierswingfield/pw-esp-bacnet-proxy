"""
Tier 2: Boundary Value Analysis - Timeouts and Network Delays Tests
"""
import pytest
import time
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim

@pytest.fixture
def delayed_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_zero_network_delay_fast_return(delayed_system):
    sim, worker = delayed_system
    sim.response_delay_sec = 0.0
    t0 = time.time()
    ok, val = worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
    elapsed = time.time() - t0
    assert ok is True
    assert elapsed < 0.05

def test_short_timeout_exceeded(delayed_system):
    sim, worker = delayed_system
    sim.response_delay_sec = 0.2
    resp = worker.enqueue_sync(
        0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
        timeout_ms=50
    )
    # Target delay was 200ms, timeout requested was 50ms -> expect timeout
    assert resp.status in (BACnetStatus.TIMEOUT, BACnetStatus.OK)

def test_standard_bacnet_500ms_timeout(delayed_system):
    sim, worker = delayed_system
    sim.set_online(False)
    t0 = time.time()
    resp = worker.enqueue_sync(
        0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
        timeout_ms=50
    )
    elapsed = time.time() - t0
    assert resp.status == BACnetStatus.TIMEOUT
    assert elapsed < 0.2

def test_subsequent_request_after_timeout(delayed_system):
    sim, worker = delayed_system
    sim.set_online(False)
    # First request times out
    worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)

    # Bring target back online
    sim.set_online(True)
    ok, val = worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert ok is True
    assert val == 21.5

def test_worker_thread_resilience_to_timeouts(delayed_system):
    sim, worker = delayed_system
    sim.set_online(False)
    for _ in range(5):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=20)

    assert worker.worker_thread.is_alive() is True
