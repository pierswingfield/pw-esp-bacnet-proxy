"""
Tier 5 Adversarial Test: Matter Attribute Read/Write Concurrency & Telemetry
Targets high-concurrency Matter attribute access during background BACnet telemetry,
extreme value clamping, system mode mapping, and queue priority preemption.
"""
import concurrent.futures
import pytest
import threading
import time

from tests.harness.bacnet_simulator import BACnetSimulator, BACnetObjectType, BACnetPropertyId
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim, BACnetPriority
from tests.harness.hvac_core_engine import HvacCoreEngine
from tests.harness.matter_endpoint_sim import MatterThermostatEndpointSim, MatterSystemMode

@pytest.fixture
def adv_matter_env():
    sim = BACnetSimulator()
    worker = BACnetWorkerQueueSim(sim)
    core = HvacCoreEngine(worker)
    matter = MatterThermostatEndpointSim(core, room_idx=0)
    yield sim, worker, core, matter
    worker.shutdown()

def test_matter_reads_and_writes_concurrent_with_telemetry_flooding(adv_matter_env):
    """
    Adversarial Stress: 8 concurrent worker threads (2 Matter readers, 2 Matter writers,
    2 background telemetry readers, 2 Web UI pollers) hammering the bridge simultaneously.
    """
    sim, worker, core, matter = adv_matter_env
    sim.response_delay_sec = 0.005
    stop_event = threading.Event()
    errors = []

    def matter_reader(tid):
        while not stop_event.is_set():
            ok_t, temp = matter.read_local_temperature()
            if not ok_t:
                errors.append(f"Matter reader {tid} failed temp read")
            ok_s, sp = matter.read_occupied_cooling_setpoint()
            if not ok_s:
                errors.append(f"Matter reader {tid} failed setpoint read")
            ok_m, mode = matter.read_system_mode()
            if not ok_m:
                errors.append(f"Matter reader {tid} failed mode read")
            time.sleep(0.003)

    def matter_writer(tid):
        val = 2000
        while not stop_event.is_set():
            val = 2000 + (val - 1900 + 50) % 800
            ok_w, applied = matter.write_occupied_cooling_setpoint(val)
            if not ok_w:
                errors.append(f"Matter writer {tid} failed setpoint write")
            ok_m = matter.write_system_mode(MatterSystemMode.HEAT if (val % 100 == 0) else MatterSystemMode.AUTO)
            if not ok_m:
                errors.append(f"Matter writer {tid} failed mode write")
            time.sleep(0.006)

    def telemetry_reader(tid):
        while not stop_event.is_set():
            worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1102, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=300)
            worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1105, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=300)
            time.sleep(0.004)

    def web_poller(tid):
        while not stop_event.is_set():
            core.get_system_power()
            core.get_room_temperature(0)
            time.sleep(0.005)

    threads = [
        threading.Thread(target=matter_reader, args=(1,)),
        threading.Thread(target=matter_reader, args=(2,)),
        threading.Thread(target=matter_writer, args=(1,)),
        threading.Thread(target=matter_writer, args=(2,)),
        threading.Thread(target=telemetry_reader, args=(1,)),
        threading.Thread(target=telemetry_reader, args=(2,)),
        threading.Thread(target=web_poller, args=(1,)),
        threading.Thread(target=web_poller, args=(2,)),
    ]

    for t in threads:
        t.start()

    time.sleep(0.3)
    stop_event.set()

    for t in threads:
        t.join(timeout=1.0)

    assert len(errors) == 0, f"Matter concurrency errors detected: {errors}"

def test_matter_setpoint_extreme_boundary_clamping(adv_matter_env):
    """
    Adversarial Boundary Testing: Matter setpoint writes with extreme, negative,
    and fractional values scaled to 0.01°C integers.
    """
    sim, worker, core, matter = adv_matter_env

    test_cases = [
        (-5000, 1800), # -50.00°C -> 18.00°C
        (-100, 1800),  # -1.00°C -> 18.00°C
        (0, 1800),     # 0.00°C -> 18.00°C
        (1000, 1800),  # 10.00°C -> 18.00°C
        (1799, 1800),  # 17.99°C -> 18.00°C
        (1800, 1800),  # 18.00°C -> 18.00°C (Min boundary)
        (2150, 2150),  # 21.50°C -> 21.50°C
        (2275, 2275),  # 22.75°C -> 22.75°C
        (3000, 3000),  # 30.00°C -> 30.00°C (Max boundary)
        (3001, 3000),  # 30.01°C -> 30.00°C
        (5000, 3000),  # 50.00°C -> 30.00°C
        (9999, 3000),  # 99.99°C -> 30.00°C
    ]

    for raw_in, expected_applied in test_cases:
        ok, applied = matter.write_occupied_cooling_setpoint(raw_in)
        assert ok is True
        assert applied == expected_applied, f"Input {raw_in}: expected {expected_applied}, got {applied}"

        # Verify readback matches applied
        ok_r, read_val = matter.read_occupied_cooling_setpoint()
        assert ok_r is True
        assert read_val == expected_applied

def test_matter_system_mode_all_valid_and_invalid_transitions(adv_matter_env):
    """
    Adversarial State Transitions: Valid and invalid Matter system mode mappings.
    """
    sim, worker, core, matter = adv_matter_env

    # 1. Turn OFF -> Power should be False, readback should be OFF (0)
    assert matter.write_system_mode(MatterSystemMode.OFF) is True
    ok, mode = matter.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.OFF
    ok_p, pwr = core.get_room_power(0)
    assert ok_p is True and pwr is False

    # 2. Turn HEAT -> Power should be True, readback should be HEAT (4)
    assert matter.write_system_mode(MatterSystemMode.HEAT) is True
    ok, mode = matter.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.HEAT
    ok_p, pwr = core.get_room_power(0)
    assert ok_p is True and pwr is True

    # 3. Turn COOL -> Power should be True, readback should be HEAT (4)
    assert matter.write_system_mode(MatterSystemMode.COOL) is True
    ok, mode = matter.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.HEAT

    # 4. Turn AUTO -> Power should be True, readback should be HEAT (4)
    assert matter.write_system_mode(MatterSystemMode.AUTO) is True
    ok, mode = matter.read_system_mode()
    assert ok is True
    assert mode == MatterSystemMode.HEAT

    # 5. Invalid system mode values must be rejected
    invalid_modes = [2, 5, 10, -1, 999]
    for inv in invalid_modes:
        assert matter.write_system_mode(inv) is False

def test_matter_preempts_normal_telemetry_in_queue(adv_matter_env):
    """
    Adversarial Priority Inversion Check: A Matter setpoint write must be
    placed on the High-Priority queue (depth 8) and preempt pending normal reads.
    """
    sim, worker, core, matter = adv_matter_env
    sim.response_delay_sec = 0.02

    # Spawn 8 background threads issuing normal telemetry reads
    def send_normal():
        worker.read_real_sync(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=800)

    threads = [threading.Thread(target=send_normal) for _ in range(8)]
    for t in threads:
        t.start()

    time.sleep(0.005) # Allow threads to queue up in normal_queue

    # Issue Matter setpoint write (routes to High queue)
    t0 = time.time()
    ok, applied = matter.write_occupied_cooling_setpoint(2300)
    elapsed = time.time() - t0

    assert ok is True
    assert applied == 2300
    assert elapsed < 0.3

    for t in threads:
        t.join(timeout=1.0)

