"""
Tier 1: Feature Area 6 - Single Active Integration & Mutual Exclusion Tests
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacIntegrationKind

@pytest.fixture
def integ_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    yield sim, worker, core
    worker.shutdown()

def test_mqtt_mode_resource_allocation(integ_system):
    sim, worker, core = integ_system
    core.integration_set(HvacIntegrationKind.MQTT_HOME_ASSISTANT)
    budget = core.calculate_memory_budget()
    assert budget["mqtt_command_stack"] == 4096
    assert budget["mqtt_state_stack"] == 16384
    assert budget["mqtt_command_queue"] == 1152
    assert budget["mqtt_client_buffers"] > 0
    assert budget["matter_data_model"] == 0
    assert budget["mqtt_deferred_ram"] == 0

def test_matter_mode_mqtt_deferral(integ_system):
    sim, worker, core = integ_system
    core.integration_set(HvacIntegrationKind.MATTER)
    budget = core.calculate_memory_budget()
    # In Matter mode, all MQTT tasks, queues and buffers are strictly unallocated
    assert budget["mqtt_command_stack"] == 0
    assert budget["mqtt_state_stack"] == 0
    assert budget["mqtt_command_queue"] == 0
    assert budget["mqtt_client_buffers"] == 0
    assert budget["matter_data_model"] > 0
    assert budget["mqtt_deferred_ram"] >= 28000 # Over 28KB DRAM saved

def test_none_mode_mqtt_deferral(integ_system):
    sim, worker, core = integ_system
    core.integration_set(HvacIntegrationKind.NONE)
    budget = core.calculate_memory_budget()
    assert budget["mqtt_command_stack"] == 0
    assert budget["mqtt_state_stack"] == 0
    assert budget["mqtt_command_queue"] == 0
    assert budget["matter_data_model"] == 0
    assert budget["mqtt_deferred_ram"] >= 28000

def test_web_server_universal_availability(integ_system):
    sim, worker, core = integ_system
    for mode in (HvacIntegrationKind.NONE, HvacIntegrationKind.MQTT_HOME_ASSISTANT, HvacIntegrationKind.MATTER):
        core.integration_set(mode)
        budget = core.calculate_memory_budget()
        assert budget["web_server_stack"] > 0
        assert budget["worker_stack"] > 0

def test_dynamic_mode_switching_memory_reclaim(integ_system):
    sim, worker, core = integ_system
    # MQTT -> Matter -> None -> MQTT
    core.integration_set(HvacIntegrationKind.MQTT_HOME_ASSISTANT)
    b1 = core.calculate_memory_budget()
    assert b1["mqtt_deferred_ram"] == 0

    core.integration_set(HvacIntegrationKind.MATTER)
    b2 = core.calculate_memory_budget()
    assert b2["mqtt_deferred_ram"] >= 28000

    core.integration_set(HvacIntegrationKind.NONE)
    b3 = core.calculate_memory_budget()
    assert b3["mqtt_deferred_ram"] >= 28000

    core.integration_set(HvacIntegrationKind.MQTT_HOME_ASSISTANT)
    b4 = core.calculate_memory_budget()
    assert b4["mqtt_deferred_ram"] == 0
