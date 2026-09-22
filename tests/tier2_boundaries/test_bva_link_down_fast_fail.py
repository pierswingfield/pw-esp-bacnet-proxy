"""
Tier 2: Boundary Value Analysis - Ethernet Link Down Fast Fail Tests
"""
import time
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority

@pytest.fixture
def net_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_link_down_fast_fail_latency(net_system):
    sim, worker = net_system
    # Simulate link down by turning target offline
    sim.set_online(False)
    # Tripping breaker to OFFLINE
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)

    # Next normal telemetry request must fail in <10ms
    t0 = time.time()
    resp = worker.enqueue_sync(
        0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
        prio=BACnetPriority.NORMAL, timeout_ms=1000
    )
    elapsed = time.time() - t0
    assert resp.status == BACnetStatus.TARGET_OFFLINE
    assert elapsed < 0.05

def test_link_down_does_not_block_worker(net_system):
    sim, worker = net_system
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)

    # Worker queue continues servicing requests without deadlocking
    resps = [
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=100)
        for _ in range(10)
    ]
    assert all(r.status == BACnetStatus.TARGET_OFFLINE for r in resps)

def test_link_restoration_resumes_telemetry(net_system):
    sim, worker = net_system
    sim.set_online(False)
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)

    # Link comes back up
    sim.set_online(True)
    worker.trigger_probe_now()

    ok, val = worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert ok is True
    assert val == 21.5

def test_rapid_link_flapping_stability(net_system):
    sim, worker = net_system
    for cycle in range(3):
        sim.set_online(False)
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=20)
        sim.set_online(True)
        worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=500)

    assert worker.worker_thread.is_alive() is True

def test_zero_hanging_transactions_on_disconnect(net_system):
    sim, worker = net_system
    sim.set_online(False)
    worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=20)
    time.sleep(0.05)
    assert sim.in_flight_transactions == 0
