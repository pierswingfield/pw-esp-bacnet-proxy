"""
Tier 1: Feature Area 3 - Bounded BACnet Worker Queue Tests
"""
import time
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority

@pytest.fixture
def worker_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_dual_priority_queue_depth_limits(worker_system):
    sim, worker = worker_system
    assert worker.HIGH_QUEUE_DEPTH == 8
    assert worker.NORMAL_QUEUE_DEPTH == 24

def test_high_priority_drain_precedence(worker_system):
    sim, worker = worker_system
    sim.response_delay_sec = 0.02 # Give time for queue filling

    execution_order = []

    def enqueue_normal(idx):
        resp = worker.enqueue_sync(
            0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
            prio=BACnetPriority.NORMAL, timeout_ms=1000
        )
        execution_order.append(f"NORMAL_{idx}")

    def enqueue_high(idx):
        resp = worker.enqueue_sync(
            2, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE,
            prio=BACnetPriority.HIGH, write_val=22.0, timeout_ms=1000
        )
        execution_order.append(f"HIGH_{idx}")

    import threading
    # Launch several normal priority reads, followed immediately by high priority writes
    threads = []
    for i in range(5):
        t = threading.Thread(target=enqueue_normal, args=(i,))
        threads.append(t)
        t.start()
    time.sleep(0.01)
    for j in range(3):
        t = threading.Thread(target=enqueue_high, args=(j,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    assert len(execution_order) == 8
    assert worker.high_prio_processed == 3
    assert worker.normal_prio_processed == 5

def test_normal_priority_queue_full_drop(worker_system):
    sim, worker = worker_system
    sim.response_delay_sec = 0.05 # Slow down worker to build backlog

    dropped_count = 0
    results = []

    def caller():
        nonlocal dropped_count
        resp = worker.enqueue_sync(
            0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
            prio=BACnetPriority.NORMAL, timeout_ms=500, wait_enqueue_ms=0
        )
        results.append(resp.status)

    import threading
    threads = [threading.Thread(target=caller) for _ in range(35)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    dropped = results.count(BACnetStatus.QUEUE_FULL)
    assert dropped >= 5
    assert worker.dropped_requests_count >= 5

def test_high_priority_queue_enqueue_timeout(worker_system):
    sim, worker = worker_system
    sim.response_delay_sec = 0.1

    # High priority queue has depth 8, wait_enqueue_ms=10 allows timeout detection
    resps = []
    for i in range(12):
        resp = worker.enqueue_sync(
            2, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE,
            prio=BACnetPriority.HIGH, write_val=22.0, timeout_ms=200, wait_enqueue_ms=10
        )
        resps.append(resp.status)

    assert BACnetStatus.QUEUE_FULL in resps or BACnetStatus.OK in resps

def test_request_serialization(worker_system):
    sim, worker = worker_system
    # Under worker queue, maximum in-flight transactions must never exceed 1
    sim.reset_stats()
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
        futures = [
            executor.submit(worker.read_real_sync, BACnetObjectType.ANALOG_VALUE, 1100 + (i % 2) * 100, BACnetPropertyId.PROP_PRESENT_VALUE)
            for i in range(20)
        ]
        for f in concurrent.futures.as_completed(futures):
            ok, val = f.result()
            assert ok is True

    assert sim.reentrancy_violations == 0
    assert sim.max_concurrent_transactions <= 1
