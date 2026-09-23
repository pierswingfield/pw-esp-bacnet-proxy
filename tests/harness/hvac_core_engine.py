"""
HVAC Core Domain Model Engine & Presentation Layer Gate
Implements the canonical room model, semantic operations, setpoint clamping,
NVS serialization/deserialization, and integration mutual exclusion.
"""
from dataclasses import dataclass
from enum import IntEnum
import threading
from typing import List, Optional, Tuple, Dict

from tests.harness.bacnet_simulator import (
    BACnetObjectType, BACnetPropertyId, BACnetStatus, BACnetApplicationTag
)
from tests.harness.freertos_queue_sim import BACnetWorkerQueueSim
from tests.harness.nvs_emulator import NVSEmulator

class HvacIntegrationKind(IntEnum):
    NONE = 0
    MQTT_HOME_ASSISTANT = 1
    MATTER = 2

@dataclass
class HvacRoomConfig:
    name: str
    active: bool
    setpoint_instance: int
    temperature_instance: int
    power_instance: int
    supply_air_instance: int
    required_output_instance: int
    current_output_instance: int

class HvacCoreEngine:
    """
    Protocol-Agnostic HVAC Core Single Source of Truth.
    """
    MAX_ROOMS = 8
    MIN_SETPOINT_C = 18.0
    MAX_SETPOINT_C = 30.0

    DEFAULT_ROOMS = [
        HvacRoomConfig("Room A", True, 1100, 1101, 1101, 1102, 1105, 1106),
        HvacRoomConfig("Room B", True, 1200, 1201, 1201, 1202, 1205, 1206),
        HvacRoomConfig("Room C", False, 1300, 1301, 1301, 1302, 1305, 1306),
        HvacRoomConfig("Room D", False, 1400, 1401, 1401, 1402, 1405, 1406),
        HvacRoomConfig("Room E", False, 1500, 1501, 1501, 1502, 1505, 1506),
    ]

    def __init__(self, worker: BACnetWorkerQueueSim, nvs: Optional[NVSEmulator] = None):
        self.worker = worker
        self.nvs = nvs if nvs is not None else NVSEmulator()
        self._lock = threading.RLock()
        self.rooms: List[HvacRoomConfig] = [
            HvacRoomConfig(**r.__dict__) for r in self.DEFAULT_ROOMS
        ]
        self.room_count = len(self.DEFAULT_ROOMS)
        self.integration_mode = HvacIntegrationKind.MQTT_HOME_ASSISTANT

    def get_room_count(self) -> int:
        with self._lock:
            return self.room_count

    def get_room(self, room_idx: int) -> Optional[HvacRoomConfig]:
        with self._lock:
            if 0 <= room_idx < self.room_count:
                return self.rooms[room_idx]
            return None

    def get_active_rooms(self) -> List[Tuple[int, HvacRoomConfig]]:
        with self._lock:
            return [(i, r) for i, r in enumerate(self.rooms) if r.active]

    def set_room_setpoint(self, room_idx: int, requested: float) -> Tuple[bool, float]:
        with self._lock:
            room = self.get_room(room_idx)
            if room is None:
                return False, 0.0
            
            # Enforce clamping
            applied = max(self.MIN_SETPOINT_C, min(self.MAX_SETPOINT_C, float(requested)))
            instance = room.setpoint_instance

        # Route via worker queue
        ok = self.worker.write_real_sync(
            BACnetObjectType.ANALOG_VALUE, instance,
            BACnetPropertyId.PROP_PRESENT_VALUE, applied
        )
        return ok, applied

    def get_room_setpoint(self, room_idx: int) -> Tuple[bool, float]:
        with self._lock:
            room = self.get_room(room_idx)
            if room is None:
                return False, 0.0
            instance = room.setpoint_instance
        return self.worker.read_real_sync(
            BACnetObjectType.ANALOG_VALUE, instance, BACnetPropertyId.PROP_PRESENT_VALUE
        )

    def get_room_temperature(self, room_idx: int) -> Tuple[bool, float]:
        with self._lock:
            room = self.get_room(room_idx)
            if room is None:
                return False, 0.0
            instance = room.temperature_instance
        return self.worker.read_real_sync(
            BACnetObjectType.ANALOG_VALUE, instance, BACnetPropertyId.PROP_PRESENT_VALUE
        )

    def set_room_power(self, room_idx: int, on: bool) -> bool:
        with self._lock:
            room = self.get_room(room_idx)
            if room is None:
                return False
            instance = room.power_instance
        return self.worker.write_bool_sync(
            BACnetObjectType.BINARY_VALUE, instance,
            BACnetPropertyId.PROP_PRESENT_VALUE, on
        )

    def get_room_power(self, room_idx: int) -> Tuple[bool, bool]:
        with self._lock:
            room = self.get_room(room_idx)
            if room is None:
                return False, False
            instance = room.power_instance
        return self.worker.read_bool_sync(
            BACnetObjectType.BINARY_VALUE, instance, BACnetPropertyId.PROP_PRESENT_VALUE
        )

    def set_system_power(self, on: bool) -> bool:
        # Master system power write is BV:13
        return self.worker.write_bool_sync(
            BACnetObjectType.BINARY_VALUE, 13,
            BACnetPropertyId.PROP_PRESENT_VALUE, on
        )

    def get_system_power(self) -> Tuple[bool, bool]:
        # Master system power read is BV:1
        return self.worker.read_bool_sync(
            BACnetObjectType.BINARY_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE
        )

    def set_boost_mode(self, mode_val: int) -> bool:
        # FCU Operating Mode MSV:1 (1=Auto, 4=Heat, 5=Cool)
        if mode_val not in (1, 4, 5):
            return False
        return self.worker.write_msv_sync(
            BACnetObjectType.MULTI_STATE_VALUE, 1,
            BACnetPropertyId.PROP_PRESENT_VALUE, mode_val
        )

    # NVS Persistence Operations
    def rooms_save(self) -> bool:
        with self._lock:
            handle = self.nvs.open("nvs_rooms", readonly=False)
            self.nvs.set_u8(handle, "count", self.room_count)
            for i in range(self.room_count):
                r = self.rooms[i]
                self.nvs.set_str(handle, f"r{i}_name", r.name)
                self.nvs.set_u8(handle, f"r{i}_act", 1 if r.active else 0)
                self.nvs.set_u32(handle, f"r{i}_sp", r.setpoint_instance)
                self.nvs.set_u32(handle, f"r{i}_temp", r.temperature_instance)
                self.nvs.set_u32(handle, f"r{i}_pwr", r.power_instance)
                self.nvs.set_u32(handle, f"r{i}_sa", r.supply_air_instance)
                self.nvs.set_u32(handle, f"r{i}_req", r.required_output_instance)
                self.nvs.set_u32(handle, f"r{i}_cur", r.current_output_instance)
            ok = self.nvs.commit(handle)
            self.nvs.close(handle)
            return ok

    def rooms_load(self):
        with self._lock:
            # Fallback to default
            self.rooms = [HvacRoomConfig(**r.__dict__) for r in self.DEFAULT_ROOMS]
            self.room_count = len(self.DEFAULT_ROOMS)
            handle = self.nvs.open("nvs_rooms", readonly=True)
            stored_count = self.nvs.get_u8(handle, "count")
            if stored_count is not None and 0 < stored_count <= self.MAX_ROOMS:
                self.room_count = stored_count
                loaded_rooms = []
                for i in range(self.room_count):
                    name = self.nvs.get_str(handle, f"r{i}_name") or f"Room {chr(65+i)}"
                    act_val = self.nvs.get_u8(handle, f"r{i}_act")
                    active = (act_val == 1) if act_val is not None else False
                    sp = self.nvs.get_u32(handle, f"r{i}_sp") or (1100 + i * 100)
                    temp = self.nvs.get_u32(handle, f"r{i}_temp") or (1101 + i * 100)
                    pwr = self.nvs.get_u32(handle, f"r{i}_pwr") or (1101 + i * 100)
                    sa = self.nvs.get_u32(handle, f"r{i}_sa") or (1102 + i * 100)
                    req = self.nvs.get_u32(handle, f"r{i}_req") or (1105 + i * 100)
                    cur = self.nvs.get_u32(handle, f"r{i}_cur") or (1106 + i * 100)
                    loaded_rooms.append(HvacRoomConfig(name, active, sp, temp, pwr, sa, req, cur))
                self.rooms = loaded_rooms
            self.nvs.close(handle)

    def integration_load(self):
        with self._lock:
            self.integration_mode = HvacIntegrationKind.MQTT_HOME_ASSISTANT
            handle = self.nvs.open("nvs_integ", readonly=True)
            stored = self.nvs.get_u8(handle, "mode")
            if stored is not None and stored in (0, 1, 2):
                self.integration_mode = HvacIntegrationKind(stored)
            self.nvs.close(handle)

    def integration_get(self) -> HvacIntegrationKind:
        with self._lock:
            return self.integration_mode

    def integration_set(self, mode: HvacIntegrationKind) -> bool:
        with self._lock:
            if mode not in (0, 1, 2):
                return False
            handle = self.nvs.open("nvs_integ", readonly=False)
            self.nvs.set_u8(handle, "mode", int(mode))
            ok = self.nvs.commit(handle)
            self.nvs.close(handle)
            if ok:
                self.integration_mode = mode
            return ok

    def integration_reset(self):
        with self._lock:
            handle = self.nvs.open("nvs_integ", readonly=False)
            self.nvs.erase_all(handle)
            self.nvs.commit(handle)
            self.nvs.close(handle)
            self.integration_mode = HvacIntegrationKind.MQTT_HOME_ASSISTANT

    # Resource Allocation / Memory Budget Analyzer
    def calculate_memory_budget(self) -> Dict[str, int]:
        """
        Calculates RAM committed per subsystem under current integration mode.
        Returns bytes allocated.
        """
        with self._lock:
            mode = self.integration_mode
            allocations = {
                "worker_stack": 24576,
                "worker_queues": (8 + 24) * 128, # ~4KB
                "web_server_stack": 4096,
                "mqtt_command_stack": 0,
                "mqtt_state_stack": 0,
                "mqtt_command_queue": 0,
                "mqtt_client_buffers": 0,
                "matter_data_model": 0
            }

            if mode == HvacIntegrationKind.MQTT_HOME_ASSISTANT:
                allocations["mqtt_command_stack"] = 4096
                allocations["mqtt_state_stack"] = 16384
                allocations["mqtt_command_queue"] = 1152
                allocations["mqtt_client_buffers"] = 7168
            elif mode == HvacIntegrationKind.MATTER:
                allocations["matter_data_model"] = 16384

            allocations["total_dynamic_ram"] = sum(allocations.values())
            allocations["mqtt_deferred_ram"] = 0 if mode == HvacIntegrationKind.MQTT_HOME_ASSISTANT else (4096 + 16384 + 1152 + 7168)
            return allocations
