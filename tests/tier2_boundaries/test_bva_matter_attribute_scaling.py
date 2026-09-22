"""
Tier 2: Boundary Value Analysis - Matter Thermostat Attribute Scaling and Precision
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim, MatterSystemMode

@pytest.fixture
def matter_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    endpoint = MatterThermostatEndpointSim(core, room_idx=0)
    yield sim, worker, core, endpoint
    worker.shutdown()

def test_zero_celsius_scaling(matter_system):
    sim, worker, core, endpoint = matter_system
    sim.set_property(2, 1101, 85, 4, 0.0) # 0.0°C
    ok, val = endpoint.read_local_temperature()
    assert ok is True
    assert val == 0

def test_fractional_celsius_scaling_precision(matter_system):
    sim, worker, core, endpoint = matter_system
    sim.set_property(2, 1101, 85, 4, 21.55)
    ok, val = endpoint.read_local_temperature()
    assert ok is True
    assert val == 2155

def test_matter_write_setpoint_underflow_clamping(matter_system):
    sim, worker, core, endpoint = matter_system
    # Matter write 1500 (15.0°C) must clamp to 1800 (18.0°C)
    ok, applied_raw = endpoint.write_occupied_cooling_setpoint(1500)
    assert ok is True
    assert applied_raw == 1800

def test_matter_write_setpoint_overflow_clamping(matter_system):
    sim, worker, core, endpoint = matter_system
    # Matter write 3500 (35.0°C) must clamp to 3000 (30.0°C)
    ok, applied_raw = endpoint.write_occupied_cooling_setpoint(3500)
    assert ok is True
    assert applied_raw == 3000

def test_matter_system_mode_transitions(matter_system):
    sim, worker, core, endpoint = matter_system
    # Set AUTO -> power ON
    endpoint.write_system_mode(MatterSystemMode.AUTO)
    ok, mode = endpoint.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.HEAT

    # Set OFF -> power OFF
    endpoint.write_system_mode(MatterSystemMode.OFF)
    ok, mode = endpoint.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.OFF
