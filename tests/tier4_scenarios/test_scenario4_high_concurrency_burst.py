"""
Tier 4: Scenario 4 - High-Concurrency BACnet Burst & Target Recovery
Simulates heavy telemetry polling under concurrent writes, network disconnection,
fast-fail circuit breaker activation, and seamless target reconnection.
"""
import concurrent.futures
import pytest
import time
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority, BACnetTargetHealth
from tests.harness.hvac_core_engine import HvacCoreEngine

@pytest.fixture
def burst_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_scenario4_high_concurrency_burst(burst_env):
    sim, worker, core = burst_env
    sim.reset_stats()
    sim.response_delay_sec = 0.005

    # Step 1: Launch 30 telemetry reads across multiple threads
    def telemetry_poll():
        for i in range(10):
            worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)

    # Step 2: Inject user setpoint write commands concurrently
    def user_writes():
        results = []
        for i in range(5):
            ok, applied = core.set_room_setpoint(i % 2, 21.0 + i * 0.5)
            results.append(ok)
        return results

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        f_poll1 = pool.submit(telemetry_poll)
        f_poll2 = pool.submit(telemetry_poll)
        f_poll3 = pool.submit(telemetry_poll)
        f_writes = pool.submit(user_writes)

        f_poll1.result()
        f_poll2.result()
        f_poll3.result()
        write_results = f_writes.result()

    assert all(write_results)
    assert sim.reentrancy_violations == 0

    # Step 3: Simulate Target Controller Link Failure (Delta DAC-1180E drops offline)
    sim.set_online(False)
    # Trip circuit breaker
    for _ in range(3):
        worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=30)
    assert worker.health == BACnetTargetHealth.OFFLINE

    # Step 4: Verify Fast-Fail behavior (<10ms return)
    t0 = time.time()
    resp = worker.enqueue_sync(
        0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
        prio=BACnetPriority.NORMAL, timeout_ms=1000
    )
    elapsed = time.time() - t0
    assert resp.status == BACnetStatus.TARGET_OFFLINE
    assert elapsed < 0.05
    assert worker.fast_fail_count >= 1

    # Step 5: Target link restored
    sim.set_online(True)
    worker.trigger_probe_now()
    assert worker.health == BACnetTargetHealth.ONLINE

    # Step 6: Verify normal operation resumes immediately
    ok, val = core.get_room_temperature(0)
    assert ok is True
    assert val == 21.5
