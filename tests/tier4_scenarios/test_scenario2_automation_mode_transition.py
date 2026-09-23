"""
Tier 4: Scenario 2 - Automation Mode Transition Cycle
Exercises runtime switching between MQTT, Matter, and None modes,
verifying strict mutual exclusion and dynamic resource deferral.
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacIntegrationKind
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim

@pytest.fixture
def transition_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_scenario2_automation_mode_transition(transition_env):
    sim, worker, core = transition_env

    # Step 1: Start bridge in default MQTT mode
    core.integration_load()
    assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT
    b_mqtt = core.calculate_memory_budget()
    assert b_mqtt["mqtt_command_stack"] == 4096
    assert b_mqtt["mqtt_state_stack"] == 16384
    assert b_mqtt["mqtt_deferred_ram"] == 0

    # Step 2: Transition to Matter mode
    assert core.integration_set(HvacIntegrationKind.MATTER) is True
    assert core.integration_get() == HvacIntegrationKind.MATTER
    b_matter = core.calculate_memory_budget()
    # Confirm complete MQTT resource deferral
    assert b_matter["mqtt_command_stack"] == 0
    assert b_matter["mqtt_state_stack"] == 0
    assert b_matter["mqtt_command_queue"] == 0
    assert b_matter["mqtt_client_buffers"] == 0
    assert b_matter["mqtt_deferred_ram"] >= 28000

    # Step 3: Exercise Matter Thermostat endpoint in Matter mode
    matter_ep = MatterThermostatEndpointSim(core, room_idx=0)
    ok, applied = matter_ep.write_occupied_cooling_setpoint(2250)
    assert ok is True
    assert applied == 2250

    # Step 4: Transition to None mode (Web only)
    assert core.integration_set(HvacIntegrationKind.NONE) is True
    b_none = core.calculate_memory_budget()
    assert b_none["mqtt_deferred_ram"] >= 28000
    assert b_none["web_server_stack"] > 0
    assert b_none["worker_stack"] > 0

    # Web UI continues operating normally
    ok_web, sp = core.get_room_setpoint(0)
    assert ok_web is True
    assert sp == 22.5

    # Step 5: Transition back to MQTT mode
    assert core.integration_set(HvacIntegrationKind.MQTT_HOME_ASSISTANT) is True
    b_mqtt_restored = core.calculate_memory_budget()
    assert b_mqtt_restored["mqtt_deferred_ram"] == 0
    assert b_mqtt_restored["mqtt_state_stack"] == 16384
