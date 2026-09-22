"""
Tier 1: Feature Area 7 - T-ETH-Lite Matter Thermostat Endpoint (Cluster 0x0201) Tests
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim, MatterSystemMode

@pytest.fixture
def matter_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    endpoint = MatterThermostatEndpointSim(core, room_idx=0) # Room A
    yield sim, worker, core, endpoint
    worker.shutdown()

def test_matter_local_temperature_scaling(matter_system):
    sim, worker, core, endpoint = matter_system
    # Sim room A temp is 21.5°C
    ok, temp_val = endpoint.read_local_temperature()
    assert ok is True
    assert temp_val == 2150 # 21.50 °C in 0.01°C units

def test_matter_cooling_setpoint_read(matter_system):
    sim, worker, core, endpoint = matter_system
    # Sim room A setpoint is 22.0°C
    ok, sp_val = endpoint.read_occupied_cooling_setpoint()
    assert ok is True
    assert sp_val == 2200

def test_matter_heating_setpoint_read(matter_system):
    sim, worker, core, endpoint = matter_system
    ok, sp_val = endpoint.read_occupied_heating_setpoint()
    assert ok is True
    assert sp_val == 2200

def test_matter_setpoint_write_semantic_routing(matter_system):
    sim, worker, core, endpoint = matter_system
    # Write Matter setpoint 2350 (23.50°C)
    ok, applied_raw = endpoint.write_occupied_cooling_setpoint(2350)
    assert ok is True
    assert applied_raw == 2350

    # Check BACnet Present_Value in sim
    prop = sim.get_property(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop is not None
    assert prop[1] == 23.5

def test_matter_system_mode_control(matter_system):
    sim, worker, core, endpoint = matter_system
    # Turn off via Matter
    assert endpoint.write_system_mode(MatterSystemMode.OFF) is True
    ok, mode = endpoint.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.OFF

    # Turn on (Heat) via Matter
    assert endpoint.write_system_mode(MatterSystemMode.HEAT) is True
    ok, mode2 = endpoint.read_system_mode()
    assert ok is True
    assert mode2 == MatterSystemMode.HEAT

def test_matter_heating_setpoint_write(matter_system):
    sim, worker, core, endpoint = matter_system
    ok, applied = endpoint.write_occupied_heating_setpoint(2400)
    assert ok is True
    assert applied == 2400
    prop = sim.get_property(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE)
    assert prop[1] == 24.0

def test_matter_system_mode_cool_and_auto(matter_system):
    sim, worker, core, endpoint = matter_system
    assert endpoint.write_system_mode(MatterSystemMode.COOL) is True
    ok, mode = endpoint.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.HEAT # Room power is on

    assert endpoint.write_system_mode(MatterSystemMode.AUTO) is True
    ok, mode = endpoint.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.HEAT

def test_matter_system_mode_unsupported(matter_system):
    sim, worker, core, endpoint = matter_system
    # Unsupported mode e.g. 99
    assert endpoint.write_system_mode(99) is False
