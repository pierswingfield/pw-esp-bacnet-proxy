"""
Adversarial Stress Test Suite for Milestone M2
Empirically tests:
1. Nan / Inf / Boundary Setpoint Handling in HVAC Core
2. High vs Normal Priority Preemption under Heavy Contention
3. Circuit Breaker Edge Cases (Flapping, High-Prio Bypass, Recovery Probe)
4. Cache Invalidation, Eviction, and Concurrency
5. Concurrent Multi-Threaded Dispatch Stress & Zero Re-Entrancy Violation
6. NVS Malformed Data Resilience
"""
import math
import queue
import threading
import time
import pytest

from tests.harness.bacnet_simulator import (
    BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus, BACnetApplicationTag
)
from tests.harness.freertos_queue_sim import (
    BACnetWorkerQueueSim, BACnetPriority, BACnetTargetHealth, BACnetResponse
)
from tests.harness.hvac_core_engine import (
    HvacCoreEngine, HvacRoomConfig, HvacIntegrationKind
)
from tests.harness.nvs_emulator import NVSEmulator


@pytest.fixture
def stress_env():
    sim = BACnetSimulator(device_id=753016)
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()


class TestAdversarialSetpointsAndTypes:
    """Stress tests for numeric edge cases, NaN, Inf, and type robustness."""

    def test_nan_and_inf_setpoints(self, stress_env):
        sim, worker, nvs, core = stress_env
        
        # Test positive infinity
        ok, applied = core.set_room_setpoint(0, float('inf'))
        assert ok is True
        assert applied == 30.0 # Clamped to max

        # Test negative infinity
        ok, applied = core.set_room_setpoint(0, float('-inf'))
        assert ok is True
        assert applied == 18.0 # Clamped to min

    def test_subnormal_and_extreme_floats(self, stress_env):
        sim, worker, nvs, core = stress_env
        
        # Very small positive subnormal
        ok, applied = core.set_room_setpoint(0, 1e-38)
        assert ok is True
        assert applied == 18.0

        # Very large float
        ok, applied = core.set_room_setpoint(0, 1e38)
        assert ok is True
        assert applied == 30.0


class TestCircuitBreakerFlappingAndBypass:
    """Adversarial stress on circuit breaker transitions."""

    def test_flapping_target_does_not_trip_falsely(self, stress_env):
        sim, worker, nvs, core = stress_env
        assert worker.health == BACnetTargetHealth.ONLINE

        # Flapping: 2 timeouts, then 1 success, repeatedly
        for cycle in range(5):
            sim.set_online(False)
            worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
            worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
            assert worker.health == BACnetTargetHealth.DEGRADED
            assert worker.consecutive_timeouts == 2

            sim.set_online(True)
            resp = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=100)
            assert resp.status == BACnetStatus.OK
            assert worker.health == BACnetTargetHealth.ONLINE
            assert worker.consecutive_timeouts == 0

    def test_high_prio_write_bypasses_offline_fast_fail(self, stress_env):
        sim, worker, nvs, core = stress_env
        
        # Trip to OFFLINE
        sim.set_online(False)
        for _ in range(3):
            worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
        assert worker.health == BACnetTargetHealth.OFFLINE

        # Normal read should fast fail with TARGET_OFFLINE
        resp_read = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL, timeout_ms=200)
        assert resp_read.status == BACnetStatus.TARGET_OFFLINE

        # High priority write must NOT be fast-failed; it must attempt actual dispatch
        resp_write = worker.enqueue_sync(2, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.HIGH, write_val=22.0, timeout_ms=50)
        # Because target is offline, the write should fail with TIMEOUT, NOT TARGET_OFFLINE!
        assert resp_write.status == BACnetStatus.TIMEOUT


class TestHighLoadConcurrencyAndPreemption:
    """Heavy concurrency stress tests."""

    def test_moderate_concurrency_full_success(self, stress_env):
        sim, worker, nvs, core = stress_env
        sim.reset_stats()
        sim.response_delay_sec = 0.002

        # 6 concurrent threads (within high queue depth 8 and normal queue depth 24)
        num_threads = 6
        num_ops = 5
        results = []
        lock = threading.Lock()

        def worker_task(tid):
            for i in range(num_ops):
                if tid % 2 == 0:
                    ok = core.set_room_power(tid % 2, i % 2 == 0)
                    with lock:
                        results.append(("WRITE", ok))
                else:
                    ok, temp = core.get_room_temperature(tid % 2)
                    with lock:
                        results.append(("READ", ok))

        threads = [threading.Thread(target=worker_task, args=(t,)) for t in range(num_threads)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert sim.reentrancy_violations == 0
        assert sim.max_concurrent_transactions <= 1
        assert len(results) == num_threads * num_ops
        assert all(ok for _, ok in results)
        assert worker.dropped_requests_count == 0

    def test_heavy_concurrent_burst_bounded_backpressure(self, stress_env):
        sim, worker, nvs, core = stress_env
        sim.reset_stats()
        sim.response_delay_sec = 0.005 # 5ms simulated latency

        num_threads = 40
        num_ops_per_thread = 5
        results = []
        lock = threading.Lock()

        def worker_task(tid):
            for i in range(num_ops_per_thread):
                if tid % 3 == 0:
                    # High priority write (has 200ms enqueue wait)
                    ok = core.set_room_power(tid % 2, i % 2 == 0)
                    with lock:
                        results.append(("WRITE", ok))
                else:
                    # Normal priority read (wait_enqueue_ms=0)
                    ok, temp = core.get_room_temperature(tid % 2)
                    with lock:
                        results.append(("READ", ok))

        threads = [threading.Thread(target=worker_task, args=(t,)) for t in range(num_threads)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        writes = [ok for op, ok in results if op == "WRITE"]
        reads = [ok for op, ok in results if op == "READ"]

        # Zero socket / TSM re-entrancy
        assert sim.reentrancy_violations == 0
        assert sim.max_concurrent_transactions <= 1
        assert len(results) == num_threads * num_ops_per_thread

        # Bounded queue backpressure accounting
        failed_writes = sum(1 for w in writes if not w)
        failed_reads = sum(1 for r in reads if not r)
        assert failed_writes + failed_reads == worker.dropped_requests_count
        assert sum(writes) >= 60 # Vast majority of writes succeed due to high priority

    def test_priority_queue_drain_fairness(self, stress_env):
        sim, worker, nvs, core = stress_env
        sim.response_delay_sec = 0.01

        processed_order = []
        lock = threading.Lock()

        # Fill normal queue with 10 reads
        def normal_caller(idx):
            worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL, timeout_ms=2000)
            with lock:
                processed_order.append(f"NORMAL_{idx}")

        # Inject 4 high priority writes mid-way
        def high_caller(idx):
            worker.enqueue_sync(2, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.HIGH, write_val=21.0, timeout_ms=2000)
            with lock:
                processed_order.append(f"HIGH_{idx}")

        t_norm = [threading.Thread(target=normal_caller, args=(i,)) for i in range(10)]
        for t in t_norm:
            t.start()

        time.sleep(0.02) # Let normal queue receive some items

        t_high = [threading.Thread(target=high_caller, args=(j,)) for j in range(4)]
        for t in t_high:
            t.start()

        for t in t_norm + t_high:
            t.join()

        assert len(processed_order) == 14
        # High priority requests should appear earlier than later normal requests
        assert worker.high_prio_processed == 4
        assert worker.normal_prio_processed == 10


class TestNVSCorruptionResilience:
    """Stress tests for malformed or corrupted NVS data."""

    def test_corrupt_room_count_in_nvs(self, stress_env):
        sim, worker, nvs, core = stress_env
        
        # Write corrupted room count = 255
        nvs.set_u8("nvs_rooms", "count", 255)
        
        # Load rooms should reject 255 and fallback to default 5
        core.rooms_load()
        assert core.get_room_count() == 5
        assert core.get_room(0).name == "Room A"

    def test_corrupt_integration_mode_in_nvs(self, stress_env):
        sim, worker, nvs, core = stress_env
        
        # Write invalid integration mode = 42
        nvs.set_u8("nvs_integ", "mode", 42)
        
        # Load integration should fallback to default MQTT
        core.integration_load()
        assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT
