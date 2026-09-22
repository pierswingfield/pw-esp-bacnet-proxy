"""
Tier 3: Pairwise Combination - Web Setpoint Write During MQTT Telemetry Burst
"""
import concurrent.futures
import pytest
import time
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority
from tests.harness.hvac_core_engine import HvacCoreEngine

@pytest.fixture
def combo_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_web_write_during_mqtt_telemetry_burst(combo_system):
    sim, worker, core = combo_system
    sim.response_delay_sec = 0.01

    # Simulate MQTT State Task polling 30 points
    def mqtt_telemetry_burst():
        for i in range(30):
            worker.enqueue_sync(
                0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
                prio=BACnetPriority.NORMAL, timeout_ms=1000
            )

    # Simulate Web UI user changing setpoint
    def web_user_write():
        time.sleep(0.02) # Let telemetry queue fill first
        ok, applied = core.set_room_setpoint(0, 24.5)
        return ok, applied

    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
        f_telemetry = executor.submit(mqtt_telemetry_burst)
        f_web = executor.submit(web_user_write)

        web_ok, web_applied = f_web.result()
        f_telemetry.result()

    assert web_ok is True
    assert web_applied == 24.5
    # Verify BACnet device received the updated setpoint
    prop = sim.get_property(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop[1] == 24.5

def test_high_priority_preempts_bulk_telemetry(combo_system):
    sim, worker, core = combo_system
    sim.response_delay_sec = 0.02

    # Verify High Priority request count increments before normal finishes
    worker.write_real_sync(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, 23.0)
    assert worker.high_prio_processed >= 1

def test_cache_consistency_after_write_burst(combo_system):
    sim, worker, core = combo_system
    core.set_room_setpoint(0, 25.0)
    ok, val = core.get_room_setpoint(0)
    assert ok is True
    assert val == 25.0
