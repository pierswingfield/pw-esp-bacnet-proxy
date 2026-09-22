"""
Tier 3: Pairwise Combination - Web UI Boost Mode During Matter Setpoint Write
"""
import concurrent.futures
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim

@pytest.fixture
def boost_matter_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    endpoint = MatterThermostatEndpointSim(core, room_idx=0)
    yield sim, worker, core, endpoint
    worker.shutdown()

def test_concurrent_boost_and_matter_setpoint(boost_matter_system):
    sim, worker, core, endpoint = boost_matter_system

    def activate_boost():
        return core.set_boost_mode(4) # Heat boost

    def matter_write_setpoint():
        return endpoint.write_occupied_cooling_setpoint(2350) # 23.5°C

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        f_boost = pool.submit(activate_boost)
        f_matter = pool.submit(matter_write_setpoint)

        boost_ok = f_boost.result()
        matter_ok, applied_raw = f_matter.result()

    assert boost_ok is True
    assert matter_ok is True
    assert applied_raw == 2350

    # Verify BACnet objects in simulator
    prop_boost = sim.get_property(BACnetObjectType.MULTI_STATE_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop_boost[1] == 4
    prop_sp = sim.get_property(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop_sp[1] == 23.5

def test_zero_reentrancy_on_concurrent_web_and_matter_actions(boost_matter_system):
    sim, worker, core, endpoint = boost_matter_system
    sim.reset_stats()

    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        futures = []
        for i in range(10):
            if i % 2 == 0:
                futures.append(pool.submit(core.set_boost_mode, 1 if i % 4 == 0 else 4))
            else:
                futures.append(pool.submit(endpoint.write_occupied_cooling_setpoint, 2200 + i * 10))
        for f in concurrent.futures.as_completed(futures):
            f.result()

    assert sim.reentrancy_violations == 0
    assert sim.max_concurrent_transactions <= 1

def test_boost_mode_readback_accuracy(boost_matter_system):
    sim, worker, core, endpoint = boost_matter_system
    core.set_boost_mode(5) # Cool boost
    prop = sim.get_property(BACnetObjectType.MULTI_STATE_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop[1] == 5
