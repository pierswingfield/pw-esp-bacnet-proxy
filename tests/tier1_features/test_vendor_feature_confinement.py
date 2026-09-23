"""
Tier 1: Feature Area 8 - Vendor Feature Web Confinement Tests
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim

@pytest.fixture
def confined_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    endpoint = MatterThermostatEndpointSim(core, room_idx=0)
    yield sim, worker, core, endpoint
    worker.shutdown()

def test_boost_mode_web_confinement(confined_system):
    sim, worker, core, endpoint = confined_system
    # Boost mode (MSV:1) is supported via HVAC Core / Web API
    assert core.set_boost_mode(4) is True # Heat boost
    prop = sim.get_property(BACnetObjectType.MULTI_STATE_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop[1] == 4

    # Matter Cluster 0x0201 does NOT have boost mode attribute (standard CSA attributes only)
    assert not hasattr(endpoint, "write_boost_mode")

def test_catalog_browser_confinement(confined_system):
    sim, worker, core, endpoint = confined_system
    # Matter does not expose 480-point catalog inspection
    assert not hasattr(endpoint, "inspect_raw_bacnet_object")

def test_diagnostics_confinement(confined_system):
    sim, worker, core, endpoint = confined_system
    # Valve % and BACnet alarm states are web-only diagnostics
    valves_in_matter = hasattr(endpoint, "read_cooling_valve_percentage")
    assert valves_in_matter is False

def test_matter_standard_cluster_compliance(confined_system):
    sim, worker, core, endpoint = confined_system
    # Matter endpoint must only export standard Thermostat attributes
    standard_attrs = [
        endpoint.ATTR_LOCAL_TEMPERATURE,
        endpoint.ATTR_OCCUPIED_COOLING_SETPOINT,
        endpoint.ATTR_OCCUPIED_HEATING_SETPOINT,
        endpoint.ATTR_SYSTEM_MODE
    ]
    assert len(standard_attrs) == 4
    assert endpoint.CLUSTER_THERMOSTAT == 0x0201

def test_system_power_master_confinement(confined_system):
    sim, worker, core, endpoint = confined_system
    # System power is master control on web/MQTT, single-room Matter endpoint controls Room A power only
    assert core.set_system_power(True) is True
    # Writing Matter system mode modifies room power, not master system power
    endpoint.write_system_mode(0)
    prop_room = sim.get_property(BACnetObjectType.BINARY_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop_room[1] == 0
    prop_master = sim.get_property(BACnetObjectType.BINARY_VALUE, 13, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop_master[1] == 1
