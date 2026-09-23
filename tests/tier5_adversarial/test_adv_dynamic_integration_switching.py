"""
Tier 5 Adversarial Test: Dynamic Rapid Switching Between Integration Modes
Targets runtime mode transitions under active network I/O, NVS persistence stress,
invalid input boundaries, and memory budget invariance.
"""
import concurrent.futures
import pytest
import random
import threading
import time

from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority
from tests.harness.hvac_core_engine import HvacCoreEngine, HvacIntegrationKind
from tests.harness.nvs_emulator import NVSEmulator

@pytest.fixture
def adv_switching_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()

def test_rapid_mode_switching_under_continuous_bacnet_traffic(adv_switching_env):
    """
    Adversarial Stress: 60 rapid mode switches (NONE -> MQTT -> MATTER -> NONE)
    concurrently executing against 4 background worker threads issuing continuous
    BACnet telemetry reads and setpoint writes.
    """
    sim, worker, nvs, core = adv_switching_env
    sim.response_delay_sec = 0.005
    stop_event = threading.Event()
    traffic_errors = []

    def background_reader(thread_id):
        while not stop_event.is_set():
            try:
                ok, temp = core.get_room_temperature(0)
                if not ok:
                    traffic_errors.append(f"Reader {thread_id} temp read failed")
                ok, sp = core.get_room_setpoint(0)
                if not ok:
                    traffic_errors.append(f"Reader {thread_id} setpoint read failed")
            except Exception as e:
                traffic_errors.append(f"Reader {thread_id} exception: {e}")
            time.sleep(0.002)

    def background_writer(thread_id):
        val = 20.0
        while not stop_event.is_set():
            try:
                val = 20.0 + (val - 19.5) % 8.0
                ok, applied = core.set_room_setpoint(0, val)
                if not ok:
                    traffic_errors.append(f"Writer {thread_id} setpoint write failed")
            except Exception as e:
                traffic_errors.append(f"Writer {thread_id} exception: {e}")
            time.sleep(0.005)

    # Launch background traffic threads
    threads = [
        threading.Thread(target=background_reader, args=(1,)),
        threading.Thread(target=background_reader, args=(2,)),
        threading.Thread(target=background_writer, args=(1,)),
        threading.Thread(target=background_writer, args=(2,)),
    ]
    for t in threads:
        t.start()

    # Perform rapid mode transitions
    modes = [
        HvacIntegrationKind.NONE,
        HvacIntegrationKind.MQTT_HOME_ASSISTANT,
        HvacIntegrationKind.MATTER,
    ]
    switch_count = 60
    for i in range(switch_count):
        target_mode = modes[i % len(modes)]
        ok = core.integration_set(target_mode)
        assert ok is True, f"Failed to switch to mode {target_mode}"
        current = core.integration_get()
        assert current == target_mode, f"Expected {target_mode}, got {current}"
        time.sleep(0.002)

    stop_event.set()
    for t in threads:
        t.join(timeout=1.0)

    assert len(traffic_errors) == 0, f"Encountered traffic errors during mode switching: {traffic_errors}"
    assert worker.high_queue.qsize() == 0 or worker.high_queue.qsize() < 8

def test_nvs_persistence_under_mode_switch_flood(adv_switching_env):
    """
    Adversarial Stress: 100 consecutive rapid mode transitions ensuring NVS
    commit atomicity, no corruption, and exact state reload on cold boot.
    """
    sim, worker, nvs, core = adv_switching_env
    modes = [
        HvacIntegrationKind.NONE,
        HvacIntegrationKind.MQTT_HOME_ASSISTANT,
        HvacIntegrationKind.MATTER,
    ]

    for i in range(100):
        m = modes[i % len(modes)]
        assert core.integration_set(m) is True

    final_mode = core.integration_get()

    # Emulate cold boot reload
    new_core = HvacCoreEngine(worker, nvs)
    new_core.integration_load()
    assert new_core.integration_get() == final_mode

def test_invalid_integration_mode_rejections(adv_switching_env):
    """
    Boundary & Adversarial Validation: Out-of-range integration values
    must be rejected safely with False and preserve the existing mode.
    """
    sim, worker, nvs, core = adv_switching_env
    core.integration_set(HvacIntegrationKind.MQTT_HOME_ASSISTANT)

    # Test invalid integer values
    invalid_modes = [-1, 3, 4, 100, 255, 65535]
    for invalid in invalid_modes:
        ok = core.integration_set(invalid)
        assert ok is False, f"Expected rejection for invalid mode {invalid}"
        assert core.integration_get() == HvacIntegrationKind.MQTT_HOME_ASSISTANT

    # Verify NVS state was not corrupted
    handle = nvs.open("nvs_integ", readonly=True)
    val = nvs.get_u8(handle, "mode")
    nvs.close(handle)
    assert val == int(HvacIntegrationKind.MQTT_HOME_ASSISTANT)

def test_memory_budget_invariance_across_rapid_mode_transitions(adv_switching_env):
    """
    Dynamic Memory Invariance: Asserts that switching across modes maintains
    strict resource isolation (>28KB DRAM saved when not in MQTT mode).
    """
    sim, worker, nvs, core = adv_switching_env

    for _ in range(20):
        # 1. Matter mode: MQTT resources deferred, Matter data model allocated
        core.integration_set(HvacIntegrationKind.MATTER)
        b_matter = core.calculate_memory_budget()
        assert b_matter["mqtt_deferred_ram"] == 28800
        assert b_matter["mqtt_command_stack"] == 0
        assert b_matter["mqtt_state_stack"] == 0
        assert b_matter["mqtt_command_queue"] == 0
        assert b_matter["mqtt_client_buffers"] == 0
        assert b_matter["matter_data_model"] == 16384

        # 2. None mode: Both MQTT and Matter data model deferred
        core.integration_set(HvacIntegrationKind.NONE)
        b_none = core.calculate_memory_budget()
        assert b_none["mqtt_deferred_ram"] == 28800
        assert b_none["matter_data_model"] == 0
        assert b_none["mqtt_command_stack"] == 0

        # 3. MQTT mode: MQTT tasks active, deferred RAM is 0
        core.integration_set(HvacIntegrationKind.MQTT_HOME_ASSISTANT)
        b_mqtt = core.calculate_memory_budget()
        assert b_mqtt["mqtt_deferred_ram"] == 0
        assert b_mqtt["mqtt_command_stack"] == 4096
        assert b_mqtt["mqtt_state_stack"] == 16384
        assert b_mqtt["mqtt_command_queue"] == 1152
        assert b_mqtt["mqtt_client_buffers"] == 7168

def test_mode_switch_race_with_semantic_setpoint_write(adv_switching_env):
    """
    Concurrency Race: A semantic setpoint write executing at the exact instant
    an integration mode switch occurs must succeed and apply cleanly.
    """
    sim, worker, nvs, core = adv_switching_env

    def do_write():
        return core.set_room_setpoint(0, 23.5)

    def do_switch():
        return core.integration_set(HvacIntegrationKind.MATTER)

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        f_w = pool.submit(do_write)
        f_s = pool.submit(do_switch)

        ok_w, applied_w = f_w.result()
        ok_s = f_s.result()

    assert ok_w is True
    assert applied_w == 23.5
    assert ok_s is True
    assert core.integration_get() == HvacIntegrationKind.MATTER

    # Verify BACnet target has the new setpoint
    ok, sp = core.get_room_setpoint(0)
    assert ok is True
    assert sp == 23.5
