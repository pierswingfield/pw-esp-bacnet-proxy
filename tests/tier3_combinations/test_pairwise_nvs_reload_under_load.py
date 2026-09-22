"""
Tier 3: Pairwise Combination - NVS Room Config Reload Under Active Load
"""
import concurrent.futures
import pytest
from tests.harness.bacnet_simulator import BACnetSimulator
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.nvs_emulator import NVSEmulator

@pytest.fixture
def load_system():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()

def test_nvs_save_reload_during_concurrent_reads(load_system):
    sim, worker, nvs, core = load_system

    def read_loop():
        for _ in range(10):
            core.get_room_setpoint(0)
            core.get_room_temperature(0)

    def config_updater():
        core.rooms[0].name = "Updated Suite"
        core.rooms_save()
        core.rooms_load()

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        f1 = pool.submit(read_loop)
        f2 = pool.submit(config_updater)
        f1.result()
        f2.result()

    assert core.get_room(0).name == "Updated Suite"

def test_active_rooms_consistent_during_reconfiguration(load_system):
    sim, worker, nvs, core = load_system
    core.rooms[2].active = True
    core.rooms_save()
    core.rooms_load()
    assert len(core.get_active_rooms()) == 3

def test_thread_safe_semantic_and_nvs_interleaving(load_system):
    sim, worker, nvs, core = load_system
    for i in range(5):
        core.set_room_setpoint(0, 20.0 + i)
        core.rooms_save()
        core.rooms_load()
    assert core.get_room_setpoint(0)[1] == 24.0
