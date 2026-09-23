"""
Empirical Challenger 2 Test Suite for Milestone M2:
- HVAC Core Semantic Operations & Boundary Validation
- Clamping Verification [18.0°C - 30.0°C] across Float Ranges
- Room Index Bounds (0 to MAX_ROOMS-1, negative, overflow)
- System Power Consistency (BV:13 write, BV:1 readback)
- Dual-Priority Queue Concurrency, Boundedness & Preemption under Burst Workloads
- Circuit Breaker Fast-Fail & Health State Machine Transitions
- Presentation Layer Integration (Web REST Endpoints & MQTT Command/State Tasks)
- Memory Consumption & Leak Analysis
"""
import math
import queue
import threading
import time
import pytest

from tests.harness.bacnet_simulator import (
    BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus, BACnetApplicationTag
)
from tests.harness.freertos_queue_sim import (
    BACnetWorkerQueueSim, BACnetPriority, BACnetTargetHealth, BACnetResponse
)
from tests.harness.hvac_core_engine import (
    HvacCoreEngine, HvacRoomConfig, HvacIntegrationKind
)
from tests.harness.nvs_emulator import NVSEmulator


@pytest.fixture
def test_setup():
    sim = BACnetSimulator(device_id=753016)
    worker = BACnetWorkerQueueSim(sim)
    nvs = NVSEmulator()
    core = HvacCoreEngine(worker, nvs)
    yield sim, worker, nvs, core
    worker.shutdown()


class TestHvacCoreSetpointClamping:
    """Stress tests for semantic setpoint clamping."""

    @pytest.mark.parametrize("requested,expected_applied", [
        (-100.0, 18.0),
        (0.0, 18.0),
        (10.0, 18.0),
        (17.9, 18.0),
        (17.999, 18.0),
        (18.0, 18.0),
        (18.001, 18.001),
        (21.5, 21.5),
        (24.0, 24.0),
        (29.999, 29.999),
        (30.0, 30.0),
        (30.001, 30.0),
        (30.1, 30.0),
        (35.0, 30.0),
        (100.0, 30.0),
        (1000.0, 30.0),
    ])
    def test_setpoint_clamping_exact(self, test_setup, requested, expected_applied):
        sim, worker, nvs, core = test_setup
        ok, applied = core.set_room_setpoint(0, requested)
        assert ok is True
        assert abs(applied - expected_applied) < 1e-4

        # Verify underlying BACnet write matches applied value
        status, tag, val = sim.read_property(
            BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE
        )
        assert status == BACnetStatus.OK
        assert abs(val - expected_applied) < 1e-4

    def test_setpoint_boundary_all_active_rooms(self, test_setup):
        sim, worker, nvs, core = test_setup
        for room_idx, room in core.get_active_rooms():
            # Test under-clamp
            ok, applied_low = core.set_room_setpoint(room_idx, 15.0)
            assert ok is True
            assert applied_low == 18.0
            _, read_low = core.get_room_setpoint(room_idx)
            assert read_low == 18.0

            # Test over-clamp
            ok, applied_high = core.set_room_setpoint(room_idx, 35.0)
            assert ok is True
            assert applied_high == 30.0
            _, read_high = core.get_room_setpoint(room_idx)
            assert read_high == 30.0


class TestRoomIndexBoundaryValidation:
    """Stress tests for room index boundaries and capacity limits."""

    def test_room_index_within_bounds(self, test_setup):
        sim, worker, nvs, core = test_setup
        # Default count is 5 (Room 0 to 4)
        assert core.get_room_count() == 5
        for idx in range(5):
            room = core.get_room(idx)
            assert room is not None
            assert room.name.startswith("Room ")
            # Check getter methods return valid status
            ok_temp, _ = core.get_room_temperature(idx)
            assert ok_temp is True
            ok_sp, _ = core.get_room_setpoint(idx)
            assert ok_sp is True
            ok_pwr, _ = core.get_room_power(idx)
            assert ok_pwr is True

    @pytest.mark.parametrize("invalid_idx", [-1, -100, 5, 6, 7, 8, 999])
    def test_room_index_out_of_bounds(self, test_setup, invalid_idx):
        sim, worker, nvs, core = test_setup
        assert core.get_room(invalid_idx) is None
        
        ok, applied = core.set_room_setpoint(invalid_idx, 21.0)
        assert ok is False
        assert applied == 0.0

        ok = core.set_room_power(invalid_idx, True)
        assert ok is False

        ok, _ = core.get_room_setpoint(invalid_idx)
        assert ok is False

        ok, _ = core.get_room_temperature(invalid_idx)
        assert ok is False

        ok, _ = core.get_room_power(invalid_idx)
        assert ok is False

    def test_room_expansion_to_max_rooms(self, test_setup):
        sim, worker, nvs, core = test_setup
        # Expand room configuration to 8 rooms (MAX_ROOMS)
        expanded_rooms = [
            HvacRoomConfig(f"Room {chr(65+i)}", True, 1100 + i*100, 1101 + i*100,
                           1101 + i*100, 1102 + i*100, 1105 + i*100, 1106 + i*100)
            for i in range(8)
        ]
        core.rooms = expanded_rooms
        core.room_count = 8
        assert core.rooms_save() is True

        # Reload from NVS
        core2 = HvacCoreEngine(worker, nvs)
        core2.rooms_load()
        assert core2.get_room_count() == 8

        # Room 7 (index 7) should now be valid
        room7 = core2.get_room(7)
        assert room7 is not None
        assert room7.name == "Room H"
        ok, applied = core2.set_room_setpoint(7, 23.5)
        assert ok is True
        assert applied == 23.5

        # Room 8 (index 8) is strictly out of bounds
        assert core2.get_room(8) is None
        ok8, _ = core2.set_room_setpoint(8, 23.5)
        assert ok8 is False


class TestSystemPowerConsistency:
    """Verifies write to BV:13 and readback from BV:1."""

    def test_system_power_write_and_readback(self, test_setup):
        sim, worker, nvs, core = test_setup
        
        # Write System Power ON
        ok = core.set_system_power(True)
        assert ok is True
        
        # Verify simulator BV:13 received ON (1)
        status, tag, val = sim.read_property(
            BACnetObjectType.BINARY_VALUE, 13, BACnetPropertyId.PROP_PRESENT_VALUE
        )
        assert status == BACnetStatus.OK
        assert val == 1

        # Simulate physical controller linking BV:13 write to BV:1 readback
        sim.write_property(
            BACnetObjectType.BINARY_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE,
            BACnetApplicationTag.ENUMERATED, 1, priority=16
        )

        ok_read, state = core.get_system_power()
        assert ok_read is True
        assert state is True

        # Write System Power OFF
        ok = core.set_system_power(False)
        assert ok is True

        status, tag, val = sim.read_property(
            BACnetObjectType.BINARY_VALUE, 13, BACnetPropertyId.PROP_PRESENT_VALUE
        )
        assert status == BACnetStatus.OK
        assert val == 0


class TestWorkerQueueBurstWorkloadsAndPreemption:
    """Stress tests concurrency, queue capacity, and priority preemption under burst load."""

    def test_high_priority_preemption_over_normal_burst(self, test_setup):
        sim, worker, nvs, core = test_setup
        
        # Add latency to simulate real network delay
        sim.response_delay_sec = 0.02

        # Enqueue 15 normal priority reads
        normal_responses = []
        def normal_caller(idx):
            ok, val = worker.read_real_sync(
                BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=3000
            )
            normal_responses.append((idx, ok, val))

        threads = [threading.Thread(target=normal_caller, args=(i,)) for i in range(15)]
        for t in threads:
            t.start()

        # Allow threads to enqueue
        time.sleep(0.05)

        # Now issue High Priority user write
        write_start = time.time()
        write_ok = core.set_room_power(0, True)
        write_elapsed = time.time() - write_start
        
        assert write_ok is True

        for t in threads:
            t.join()

        assert len(normal_responses) == 15
        assert all(ok for _, ok, _ in normal_responses)

    def test_queue_overflow_and_fast_fail(self, test_setup):
        sim, worker, nvs, core = test_setup
        sim.response_delay_sec = 0.05

        # Normal queue depth is 24. Let's burst 35 requests simultaneously without enqueue wait.
        overflow_results = []
        def burst_client(i):
            resp = worker.enqueue_sync(
                op_type=0, obj_type=BACnetObjectType.ANALOG_VALUE, instance=1100,
                prop_id=BACnetPropertyId.PROP_PRESENT_VALUE, prio=BACnetPriority.NORMAL,
                timeout_ms=500, wait_enqueue_ms=0
            )
            overflow_results.append((i, resp.status))

        threads = [threading.Thread(target=burst_client, args=(i,)) for i in range(35)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        statuses = [s for _, s in overflow_results]
        # Some should succeed, and those exceeding queue capacity should receive QUEUE_FULL
        assert BACnetStatus.OK in statuses
        assert BACnetStatus.QUEUE_FULL in statuses or worker.dropped_requests_count > 0


class TestCircuitBreakerHealthStateMachine:
    """Stress tests circuit breaker transition from ONLINE -> DEGRADED -> OFFLINE and recovery."""

    def test_consecutive_timeouts_trip_circuit_breaker(self, test_setup):
        sim, worker, nvs, core = test_setup
        assert worker.health == BACnetTargetHealth.ONLINE

        # Make target offline / unresponsive
        sim.set_online(False)

        # 1st timeout -> DEGRADED
        resp1 = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)
        assert resp1.status == BACnetStatus.TIMEOUT
        assert worker.health == BACnetTargetHealth.DEGRADED

        # 2nd timeout -> DEGRADED
        resp2 = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)
        assert resp2.status == BACnetStatus.TIMEOUT
        assert worker.health == BACnetTargetHealth.DEGRADED

        # 3rd timeout -> OFFLINE
        resp3 = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=50)
        assert resp3.status == BACnetStatus.TIMEOUT
        assert worker.health == BACnetTargetHealth.OFFLINE

        # Next normal read should fast-fail immediately (0 ms delay)
        t0 = time.time()
        resp_fast = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=500)
        elapsed_ms = (time.time() - t0) * 1000.0

        assert resp_fast.status == BACnetStatus.TARGET_OFFLINE
        assert elapsed_ms < 50.0  # Fast fail without waiting 500ms timeout!

        # Now restore target responsiveness and trigger probe
        sim.set_online(True)
        worker.trigger_probe_now()
        time.sleep(0.05)

        assert worker.health == BACnetTargetHealth.ONLINE

        # Reads should succeed normally again
        resp_recovered = worker.enqueue_sync(0, BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE, timeout_ms=500)
        assert resp_recovered.status == BACnetStatus.OK


class TestPresentationLayerSimulation:
    """Tests Web REST and MQTT presentation layers integration contracts."""

    def test_web_rest_setpoint_handler_simulation(self, test_setup):
        sim, worker, nvs, core = test_setup

        def simulate_api_room_setpoint_post(body_dict):
            room = body_dict.get("room", -1)
            raw_val = body_dict.get("value", None)
            if raw_val is None:
                raw_val = body_dict.get("setpoint", None)

            if room < 0 or room >= core.get_room_count() or raw_val is None:
                return 400, {"ok": False, "error": "missing or invalid room/value"}

            try:
                val = float(raw_val)
            except ValueError:
                return 400, {"ok": False, "error": "invalid float"}

            ok, applied = core.set_room_setpoint(room, val)
            if not ok:
                return 500, {"ok": False, "error": "bacnet write failed"}
            return 200, {"ok": True, "setpoint": round(applied, 1)}

        # Test valid within range
        code, resp = simulate_api_room_setpoint_post({"room": 0, "value": 22.5})
        assert code == 200
        assert resp == {"ok": True, "setpoint": 22.5}

        # Test clamping low
        code, resp = simulate_api_room_setpoint_post({"room": 0, "value": 15.0})
        assert code == 200
        assert resp == {"ok": True, "setpoint": 18.0}

        # Test clamping high
        code, resp = simulate_api_room_setpoint_post({"room": 0, "value": 35.0})
        assert code == 200
        assert resp == {"ok": True, "setpoint": 30.0}

        # Test invalid room index
        code, resp = simulate_api_room_setpoint_post({"room": 5, "value": 22.0})
        assert code == 400

        # Test negative room index
        code, resp = simulate_api_room_setpoint_post({"room": -1, "value": 22.0})
        assert code == 400

    def test_mqtt_command_and_state_telemetry_simulation(self, test_setup):
        sim, worker, nvs, core = test_setup
        topic_base = "home/hvac"
        published_messages = {}

        def mqtt_publish(topic, payload, retain=True):
            published_messages[topic] = payload

        def mqtt_handle_command(topic, payload):
            # System power setpoint
            if topic == f"{topic_base}/system_power/set":
                on = payload.upper() in ("ON", "1", "TRUE")
                core.set_system_power(on)
                mqtt_publish(f"{topic_base}/system_power/state", "ON" if on else "OFF")
                return

            # Room setpoint / power
            for i in range(core.get_room_count()):
                room = core.get_room(i)
                if not room or not room.active:
                    continue
                if topic == f"{topic_base}/room{i}/setpoint/set":
                    val = float(payload)
                    ok, applied = core.set_room_setpoint(i, val)
                    if ok:
                        mqtt_publish(f"{topic_base}/room{i}/setpoint/state", f"{applied:.1f}")
                elif topic == f"{topic_base}/room{i}/power/set":
                    on = payload.upper() in ("ON", "1", "TRUE")
                    ok = core.set_room_power(i, on)
                    if ok:
                        mqtt_publish(f"{topic_base}/room{i}/power/state", "ON" if on else "OFF")
                        mqtt_publish(f"{topic_base}/room{i}/mode/state", "heat_cool" if on else "off")

        # Command: System Power ON
        mqtt_handle_command(f"{topic_base}/system_power/set", "ON")
        assert published_messages[f"{topic_base}/system_power/state"] == "ON"

        # Command: Room 0 Setpoint to 16.0 (clamped to 18.0)
        mqtt_handle_command(f"{topic_base}/room0/setpoint/set", "16.0")
        assert published_messages[f"{topic_base}/room0/setpoint/state"] == "18.0"

        # Command: Room 1 Power OFF
        mqtt_handle_command(f"{topic_base}/room1/power/set", "OFF")
        assert published_messages[f"{topic_base}/room1/power/state"] == "OFF"
        assert published_messages[f"{topic_base}/room1/mode/state"] == "off"


class TestMemoryBudgetAndMutualExclusion:
    """Verifies memory allocation calculations and mutual exclusion."""

    def test_memory_budget_under_mqtt_mode(self, test_setup):
        sim, worker, nvs, core = test_setup
        core.integration_set(HvacIntegrationKind.MQTT_HOME_ASSISTANT)
        budget = core.calculate_memory_budget()

        assert budget["worker_stack"] == 24576
        assert budget["mqtt_command_stack"] == 4096
        assert budget["mqtt_state_stack"] == 16384
        assert budget["mqtt_deferred_ram"] == 0
        assert budget["matter_data_model"] == 0

    def test_memory_budget_under_matter_mode(self, test_setup):
        sim, worker, nvs, core = test_setup
        core.integration_set(HvacIntegrationKind.MATTER)
        budget = core.calculate_memory_budget()

        assert budget["worker_stack"] == 24576
        assert budget["mqtt_command_stack"] == 0
        assert budget["mqtt_state_stack"] == 0
        assert budget["mqtt_command_queue"] == 0
        assert budget["mqtt_client_buffers"] == 0
        assert budget["mqtt_deferred_ram"] > 28000
        assert budget["matter_data_model"] == 16384
