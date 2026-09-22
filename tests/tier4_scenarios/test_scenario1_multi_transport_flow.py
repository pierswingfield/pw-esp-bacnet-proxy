"""
Tier 4: Scenario 1 - Multi-Transport Room Control Flow
Exercises end-to-end setpoint adjustment, telemetry polling, boost mode,
and cross-transport state consistency across Web, MQTT, and Matter.
"""
import concurrent.futures
import pytest
import time
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim, MatterSystemMode

@pytest.fixture
def multi_transport_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    matter_ep = MatterThermostatEndpointSim(core, room_idx=0)
    yield sim, worker, core, matter_ep
    worker.shutdown()

def test_scenario1_multi_transport_flow(multi_transport_env):
    sim, worker, core, matter_ep = multi_transport_env
    sim.reset_stats()

    # Step 1: Web dashboard queries initial status of rooms
    ok_sp, initial_sp = core.get_room_setpoint(0)
    assert ok_sp is True
    assert initial_sp == 22.0
    ok_temp, initial_temp = core.get_room_temperature(0)
    assert ok_temp is True
    assert initial_temp == 21.5

    # Step 2: Background MQTT telemetry burst begins (polling 20 points)
    def mqtt_telemetry_loop():
        results = []
        for i in range(20):
            r = worker.enqueue_sync(
                0, BACnetObjectType.ANALOG_VALUE, 1101 + (i % 2) * 100,
                BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL, timeout_ms=500
            )
            results.append(r.status)
        return results

    # Step 3: Concurrently, Matter Thermostat writes new heating setpoint 24.0°C (2400 in raw units)
    def matter_setpoint_write():
        time.sleep(0.01) # Start right after telemetry burst kicks off
        return matter_ep.write_occupied_cooling_setpoint(2400)

    # Step 4: Web UI user activates boost mode (Heat = 4)
    def web_boost_activate():
        time.sleep(0.02)
        return core.set_boost_mode(4)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        f_telemetry = executor.submit(mqtt_telemetry_loop)
        f_matter = executor.submit(matter_setpoint_write)
        f_boost = executor.submit(web_boost_activate)

        telemetry_results = f_telemetry.result()
        matter_ok, matter_applied = f_matter.result()
        boost_ok = f_boost.result()

    # Step 5: Assertions across all transports
    assert matter_ok is True
    assert matter_applied == 2400
    assert boost_ok is True

    # Step 6: Verify presentation layer synchronization
    # Web dashboard reads updated setpoint from hvac_core
    ok_web, web_sp = core.get_room_setpoint(0)
    assert ok_web is True
    assert web_sp == 24.0

    # Matter thermostat reads updated local temperature and setpoint
    ok_m_temp, m_temp = matter_ep.read_local_temperature()
    assert ok_m_temp is True
    assert m_temp == 2150
    ok_m_sp, m_sp = matter_ep.read_occupied_cooling_setpoint()
    assert ok_m_sp is True
    assert m_sp == 2400

    # Step 7: Zero BACnet transaction reentrancy violations
    assert sim.reentrancy_violations == 0
    assert sim.max_concurrent_transactions <= 1
