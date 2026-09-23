"""
Tier 3: Pairwise Combination - Multi-Room Simultaneous Setpoint & Power Burst
"""
import concurrent.futures
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine

@pytest.fixture
def multi_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_all_rooms_simultaneous_setpoint_write(multi_system):
    sim, worker, core = multi_system
    sim.reset_stats()

    def set_room(idx, sp):
        return core.set_room_setpoint(idx, sp)

    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        futures = [
            pool.submit(set_room, i, 20.0 + i * 1.5)
            for i in range(5)
        ]
        results = [f.result() for f in concurrent.futures.as_completed(futures)]

    assert all(ok for ok, _ in results)
    assert sim.reentrancy_violations == 0
    assert sim.max_concurrent_transactions <= 1

    # Verify values for each room
    for i in range(5):
        ok, val = core.get_room_setpoint(i)
        assert ok is True
        assert abs(val - (20.0 + i * 1.5)) < 0.001

def test_all_rooms_simultaneous_power_toggle(multi_system):
    sim, worker, core = multi_system
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        futures = [
            pool.submit(core.set_room_power, i, (i % 2 == 0))
            for i in range(5)
        ]
        for f in concurrent.futures.as_completed(futures):
            assert f.result() is True

    for i in range(5):
        ok, pwr = core.get_room_power(i)
        assert ok is True
        assert pwr == (i % 2 == 0)

def test_mixed_reads_and_writes_across_rooms(multi_system):
    sim, worker, core = multi_system
    sim.reset_stats()

    def action(idx):
        if idx % 2 == 0:
            return core.set_room_setpoint(idx % 5, 22.5)[0]
        else:
            return core.get_room_temperature(idx % 5)[0]

    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
        results = list(pool.map(action, range(20)))

    assert all(results)
    assert sim.reentrancy_violations == 0
