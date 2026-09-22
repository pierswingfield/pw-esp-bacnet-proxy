"""
Tier 1: Feature Area 4 - Worker APDU Pump & Multi-Task Concurrency Tests
"""
import concurrent.futures
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority

@pytest.fixture
def pump_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    yield sim, worker
    worker.shutdown()

def test_single_socket_owner_pump(pump_system):
    sim, worker = pump_system
    ok, val = worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert ok is True
    assert val == 21.5
    assert worker.processed_requests_count == 1

def test_concurrent_multi_thread_callers(pump_system):
    sim, worker = pump_system
    sim.reset_stats()

    # 15 concurrent caller threads simulating HTTP handlers and MQTT tasks
    def worker_caller(idx):
        if idx % 3 == 0:
            return worker.write_real_sync(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, 20.0 + idx * 0.1)
        else:
            ok, val = worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
            return ok

    with concurrent.futures.ThreadPoolExecutor(max_workers=15) as pool:
        results = list(pool.map(worker_caller, range(30)))

    assert all(results)
    assert sim.reentrancy_violations == 0
    assert sim.max_concurrent_transactions <= 1

def test_sync_caller_event_unblock(pump_system):
    sim, worker = pump_system
    resp = worker.enqueue_sync(
        0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
        prio=BACnetPriority.NORMAL, timeout_ms=500
    )
    assert resp.status == BACnetStatus.OK
    assert resp.value == 21.5
    assert resp.rtt_ms >= 0.0

def test_caller_stack_isolation(pump_system):
    sim, worker = pump_system
    # Worker owns APDU pump; caller thread only blocks on sync event
    import threading
    caller_tid = threading.get_ident()
    worker_tid = worker.worker_thread.ident
    assert caller_tid != worker_tid

def test_response_payload_integrity(pump_system):
    sim, worker = pump_system
    resp = worker.enqueue_sync(
        0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
        prio=BACnetPriority.NORMAL
    )
    assert resp.status == BACnetStatus.OK
    assert resp.tag == 4 # REAL tag
    assert abs(resp.value - 21.5) < 0.001
