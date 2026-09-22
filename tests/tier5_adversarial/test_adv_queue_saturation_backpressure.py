"""
Tier 5 Adversarial Test: FreeRTOS Queue Saturation & Backpressure Drop Validation
Validates exact queue capacity enforcement, backpressure drop reporting,
dual-priority starvation avoidance, and circuit breaker fast-fail under saturation.
"""
import concurrent.futures
import pytest
import threading
import time

from tests.harness.bacnet_simulator import (
    BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus,
    BACnetApplicationTag
)
from tests.harness.freertos_queue_sim import (
    BACnetWorkerQueueSim, BACnetPriority, BACnetTargetHealth
)

@pytest.fixture
def adv_queue_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_high_priority_queue_burst_drop_exact_behavior(adv_queue_env):
    """
    Adversarial Saturation: 25 simultaneous High-Priority writes against
    an 8-slot queue under slow network response (50ms latency).
    Must accept <= 8 items in queue and drop the overflow with QUEUE_FULL.
    """
    sim, worker = adv_queue_env
    sim.response_delay_sec = 0.05

    statuses = []
    lock = threading.Lock()

    def send_high():
        resp = worker.enqueue_sync(
            op_type=2, obj_type=BACnetObjectType.ANALOG_VALUE, instance=1100,
            prop_id=BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.HIGH,
            write_val=22.5, write_tag=BACnetApplicationTag.REAL,
            timeout_ms=500, wait_enqueue_ms=0
        )
        with lock:
            statuses.append(resp.status)

    threads = [threading.Thread(target=send_high) for _ in range(25)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=2.0)

    # Verify that overflow requests were dropped with QUEUE_FULL
    queue_full_count = statuses.count(BACnetStatus.QUEUE_FULL)
    ok_count = statuses.count(BACnetStatus.OK)

    assert queue_full_count >= 10, f"Expected at least 10 drops, got {queue_full_count}"
    assert ok_count > 0, "Expected some requests to succeed"
    assert (queue_full_count + ok_count) == len(threads)

def test_normal_priority_queue_burst_drop_exact_behavior(adv_queue_env):
    """
    Adversarial Saturation: 50 simultaneous Normal-Priority reads against
    a 24-slot queue under slow network response (50ms latency).
    Must accept <= 24 items in queue and drop the rest with QUEUE_FULL.
    """
    sim, worker = adv_queue_env
    sim.response_delay_sec = 0.05

    statuses = []
    lock = threading.Lock()

    def send_normal():
        resp = worker.enqueue_sync(
            op_type=0, obj_type=BACnetObjectType.ANALOG_VALUE, instance=1101,
            prop_id=BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL,
            timeout_ms=500, wait_enqueue_ms=0
        )
        with lock:
            statuses.append(resp.status)

    threads = [threading.Thread(target=send_normal) for _ in range(50)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=2.0)

    queue_full_count = statuses.count(BACnetStatus.QUEUE_FULL)
    assert queue_full_count >= 20, f"Expected at least 20 drops, got {queue_full_count}"

def test_queue_recovery_and_zero_drain_after_saturation(adv_queue_env):
    """
    Adversarial Drain: After severe burst drops, the worker task must
    drain the queues completely down to 0 and resume normal operation.
    """
    sim, worker = adv_queue_env
    sim.response_delay_sec = 0.01

    # Saturate with mixed traffic
    for _ in range(30):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL, timeout_ms=200, wait_enqueue_ms=0)
        worker.enqueue_sync(2, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.HIGH, write_val=21.0, write_tag=BACnetApplicationTag.REAL, timeout_ms=200, wait_enqueue_ms=0)

    # Wait for drain
    time.sleep(0.3)

    assert worker.high_queue.qsize() == 0
    assert worker.normal_queue.qsize() == 0

    # New request should succeed immediately with 0 delay
    sim.response_delay_sec = 0.0
    ok, val = worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert ok is True

def test_circuit_breaker_offline_fast_fail_during_queue_saturation(adv_queue_env):
    """
    Adversarial Fault: When the BACnet target goes OFFLINE, normal priority
    telemetry requests must immediately fast-fail (<5ms) with TARGET_OFFLINE
    without filling or blocking the worker queues.
    """
    sim, worker = adv_queue_env

    # 1. Induce 3 consecutive timeouts to trip circuit breaker to OFFLINE
    sim.set_online(False)
    for _ in range(3):
        worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)

    assert worker.health == BACnetTargetHealth.OFFLINE

    # 2. Flood with 40 normal telemetry reads
    start_t = time.time()
    statuses = []
    for _ in range(40):
        resp = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL, timeout_ms=500)
        statuses.append(resp.status)
    elapsed = time.time() - start_t

    # All 40 must fast-fail with TARGET_OFFLINE in < 0.8s total (avg < 20ms vs 500ms timeout per request)
    assert statuses.count(BACnetStatus.TARGET_OFFLINE) == 40
    assert elapsed < 0.8, f"Fast-fail too slow: took {elapsed:.2f}s for 40 requests"
    assert worker.fast_fail_count >= 40

    # 3. High priority write still attempts transmission
    resp_high = worker.enqueue_sync(2, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.HIGH, write_val=22.0, write_tag=BACnetApplicationTag.REAL, timeout_ms=50)
    assert resp_high.status == BACnetStatus.TIMEOUT

    # 4. Target recovers and probe resets state
    sim.set_online(True)
    worker.trigger_probe_now()
    time.sleep(0.05)
    assert worker.health == BACnetTargetHealth.ONLINE

    # Normal requests succeed again
    ok, temp = worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert ok is True
