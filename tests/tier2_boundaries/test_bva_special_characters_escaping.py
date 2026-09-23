"""
Tier 2: Boundary Value Analysis - Room Name Encoding, Escaping & Special Characters
"""
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.nvs_emulator import NVSEmulator

@pytest.fixture
def hvac_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()

def test_utf8_room_name_persistence(hvac_system):
    sim, worker, nvs, core = hvac_system
    core.rooms[0].name = "Chambre à coucher"
    core.rooms[1].name = "Büro 1 (Süd)"
    assert core.rooms_save() is True

    new_core = HvacCoreEngine(worker, nvs)
    new_core.rooms_load()
    assert new_core.get_room(0).name == "Chambre à coucher"
    assert new_core.get_room(1).name == "Büro 1 (Süd)"

def test_quotes_and_brackets_escaping(hvac_system):
    sim, worker, nvs, core = hvac_system
    core.rooms[0].name = 'Living "Room" [A]'
    assert core.rooms_save() is True

    new_core = HvacCoreEngine(worker, nvs)
    new_core.rooms_load()
    assert new_core.get_room(0).name == 'Living "Room" [A]'

def test_symbols_and_numbers_room_name(hvac_system):
    sim, worker, nvs, core = hvac_system
    core.rooms[0].name = "Suite #101 & Lounge <VIP>"
    assert core.rooms_save() is True

    new_core = HvacCoreEngine(worker, nvs)
    new_core.rooms_load()
    assert new_core.get_room(0).name == "Suite #101 & Lounge <VIP>"

def test_exact_31_char_boundary_room_name(hvac_system):
    sim, worker, nvs, core = hvac_system
    name_31 = "A" * 31
    core.rooms[0].name = name_31
    assert core.rooms_save() is True

    new_core = HvacCoreEngine(worker, nvs)
    new_core.rooms_load()
    assert new_core.get_room(0).name == name_31

def test_empty_room_name_handling(hvac_system):
    sim, worker, nvs, core = hvac_system
    core.rooms[0].name = ""
    assert core.rooms_save() is True

    new_core = HvacCoreEngine(worker, nvs)
    new_core.rooms_load()
    # Empty name loads default name "Room A"
    assert new_core.get_room(0).name in ("", "Room A")
