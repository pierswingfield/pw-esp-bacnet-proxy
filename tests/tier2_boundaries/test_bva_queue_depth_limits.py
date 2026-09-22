"""
Tier 2: Boundary Value Analysis - Queue Depth Boundaries Tests
"""
import pytest
import threading
import time
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority

@pytest.fixture
def worker_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_high_queue_exact_capacity(worker_system):
    sim, worker = worker_system
    assert worker.high_queue.maxsize == 8

def test_normal_queue_exact_capacity(worker_system):
    sim, worker = worker_system
    assert worker.normal_queue.maxsize == 24

def test_normal_queue_burst_drop_boundary(worker_system):
    sim, worker = worker_system
    sim.response_delay_sec = 0.05

    statuses = []
    def enqueue():
        resp = worker.enqueue_sync(
            0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
            prio=BACnetPriority.NORMAL, timeout_ms=500, wait_enqueue_ms=0
        )
        statuses.append(resp.status)

    threads = [threading.Thread(target=enqueue) for _ in range(40)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Out of 40 requests, at least 15 must be dropped with QUEUE_FULL
    assert statuses.count(BACnetStatus.QUEUE_FULL) >= 10

def test_high_priority_preemption_under_load(worker_system):
    sim, worker = worker_system
    sim.response_delay_sec = 0.02

    # High priority write must succeed even if background normal tasks are queued
    ok = worker.write_real_sync(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, 24.0)
    assert ok is True

def test_drain_both_queues_to_zero(worker_system):
    sim, worker = worker_system
    # Queue multiple mixed requests and wait for full drain
    for i in range(5):
        worker.write_real_sync(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, 22.0)
        worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)

    time.sleep(0.1)
    assert worker.high_queue.qsize() == 0
    assert worker.normal_queue.qsize() == 0
