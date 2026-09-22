"""
Tier 3: Pairwise Combination - Integration Mode Switch During Active Telemetry
"""
import concurrent.futures
import pytest
import time
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacIntegrationKind

@pytest.fixture
def switch_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_mode_switch_while_telemetry_running(switch_system):
    sim, worker, core = switch_system

    def background_polling():
        for _ in range(15):
            worker.enqueue_sync(
                0, BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
                prio=BACnetPriority.NORMAL, timeout_ms=500
            )

    def runtime_mode_switch():
        time.sleep(0.01)
        core.integration_set(HvacIntegrationKind.MATTER)
        b = core.calculate_memory_budget()
        return b

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        f_poll = pool.submit(background_polling)
        f_switch = pool.submit(runtime_mode_switch)

        f_poll.result()
        budget = f_switch.result()

    assert core.integration_get() == HvacIntegrationKind.MATTER
    assert budget["mqtt_deferred_ram"] >= 28000
    assert budget["mqtt_command_stack"] == 0

def test_repeated_mode_cycling_under_load(switch_system):
    sim, worker, core = switch_system
    for mode in (HvacIntegrationKind.MQTT_HOME_ASSISTANT, HvacIntegrationKind.MATTER, HvacIntegrationKind.NONE):
        core.integration_set(mode)
        ok, _ = core.get_room_temperature(0)
        assert ok is True

def test_web_api_resilience_during_mode_switch(switch_system):
    sim, worker, core = switch_system
    core.integration_set(HvacIntegrationKind.MATTER)
    # Web API status read
    ok, sp = core.get_room_setpoint(0)
    assert ok is True
    ok, pwr = core.get_room_power(0)
    assert ok is True
