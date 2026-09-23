"""
BACnet/IP Network Simulator for Delta DAC-1180E Controller
Simulates BACnet confirmed/unconfirmed transactions, network latency,
packet loss, offline state, and APDU protocol semantics.
"""
from enum import IntEnum
import threading
import time
from typing import Dict, Tuple, Any, Optional

class BACnetObjectType(IntEnum):
    ANALOG_INPUT = 0
    ANALOG_OUTPUT = 1
    ANALOG_VALUE = 2
    BINARY_INPUT = 3
    BINARY_OUTPUT = 4
    BINARY_VALUE = 5
    DEVICE = 8
    MULTI_STATE_INPUT = 13
    MULTI_STATE_OUTPUT = 14
    MULTI_STATE_VALUE = 19

class BACnetPropertyId(IntEnum):
    PROP_OBJECT_IDENTIFIER = 75
    PROP_OBJECT_LIST = 76
    PROP_OBJECT_NAME = 77
    PROP_OBJECT_TYPE = 79
    PROP_PRESENT_VALUE = 85
    PROP_STATUS_FLAGS = 111
    PROP_UNITS = 117
    PROP_DESCRIPTION = 28
    PROP_NUMBER_OF_STATES = 74
    PROP_STATE_TEXT = 110

class BACnetApplicationTag(IntEnum):
    NULL = 0
    BOOLEAN = 1
    UNSIGNED_INT = 2
    SIGNED_INT = 3
    REAL = 4
    DOUBLE = 5
    OCTET_STRING = 6
    CHARACTER_STRING = 7
    BIT_STRING = 8
    ENUMERATED = 9
    DATE = 10
    TIME = 11
    OBJECT_ID = 12

class BACnetStatus(IntEnum):
    OK = 0
    TIMEOUT = 1
    ERROR = 2
    ABORT = 3
    REJECT = 4
    TARGET_OFFLINE = 5
    QUEUE_FULL = 6
    INVALID_ARG = 7

class BACnetSimulator:
    """
    Simulates Delta DAC-1180E BACnet/IP target device at 10.0.3.16:47808 (Device 1180).
    """
    def __init__(self, device_id: int = 1180):
        self.device_id = device_id
        self._lock = threading.RLock()
        self.online = True
        self.packet_loss_rate = 0.0
        self.response_delay_sec = 0.001
        self.in_flight_transactions = 0
        self.max_concurrent_transactions = 0
        self.total_read_requests = 0
        self.total_write_requests = 0
        self.total_timeouts = 0
        self.reentrancy_violations = 0
        
        # Object Table: (type, instance) -> Dict[property_id, (tag, value)]
        self._objects: Dict[Tuple[int, int], Dict[int, Tuple[BACnetApplicationTag, Any]]] = {}
        self._init_default_objects()

    def _init_default_objects(self):
        """Populate initial objects representing Delta DAC-1180E catalog."""
        # Device object
        self.set_property(BACnetObjectType.DEVICE, self.device_id, BACnetPropertyId.PROP_OBJECT_NAME,
                          BACnetApplicationTag.CHARACTER_STRING, "DAC-1180E HVAC Controller")
        self.set_property(BACnetObjectType.DEVICE, self.device_id, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.UNSIGNED_INT, self.device_id)

        # Master Controls
        self.set_property(BACnetObjectType.BINARY_VALUE, 13, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.ENUMERATED, 1) # System Run Command
        self.set_property(BACnetObjectType.BINARY_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.ENUMERATED, 1)  # System Run Status
        self.set_property(BACnetObjectType.MULTI_STATE_VALUE, 1, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.UNSIGNED_INT, 1) # Mode: 1=Auto, 4=Heat, 5=Cool

        # Room A (Base 1100)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1100, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 22.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 21.5)
        self.set_property(BACnetObjectType.BINARY_VALUE, 1101, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.ENUMERATED, 1)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1102, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 19.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1105, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 45.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1106, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 42.0)

        # Room B (Base 1200)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1200, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 21.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1201, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 21.0)
        self.set_property(BACnetObjectType.BINARY_VALUE, 1201, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.ENUMERATED, 1)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1202, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 18.5)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1205, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 30.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1206, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 30.0)

        # Room C (Base 1300)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1300, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 20.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1301, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 20.5)
        self.set_property(BACnetObjectType.BINARY_VALUE, 1301, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.ENUMERATED, 0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1302, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 18.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1305, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 0.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1306, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 0.0)

        # Room D (Base 1400)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1400, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 22.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1401, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 22.0)
        self.set_property(BACnetObjectType.BINARY_VALUE, 1401, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.ENUMERATED, 0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1402, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 18.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1405, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 0.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1406, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 0.0)

        # Room E (Base 1500)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1500, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 22.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1501, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 22.0)
        self.set_property(BACnetObjectType.BINARY_VALUE, 1501, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.ENUMERATED, 0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1502, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 18.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1505, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 0.0)
        self.set_property(BACnetObjectType.ANALOG_VALUE, 1506, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 0.0)

        # Diagnostics / Health
        self.set_property(BACnetObjectType.ANALOG_VALUE, 201, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 35.0) # Cooling Valve %
        self.set_property(BACnetObjectType.ANALOG_VALUE, 202, BACnetPropertyId.PROP_PRESENT_VALUE,
                          BACnetApplicationTag.REAL, 0.0)  # Heating Valve %
        for alm in range(10, 18):
            self.set_property(BACnetObjectType.BINARY_VALUE, alm, BACnetPropertyId.PROP_PRESENT_VALUE,
                              BACnetApplicationTag.ENUMERATED, 0) # No alarms active

    def set_property(self, obj_type: int, instance: int, prop_id: int,
                     tag: BACnetApplicationTag, value: Any):
        with self._lock:
            key = (int(obj_type), int(instance))
            if key not in self._objects:
                self._objects[key] = {}
            self._objects[key][int(prop_id)] = (tag, value)

    def get_property(self, obj_type: int, instance: int, prop_id: int) -> Optional[Tuple[BACnetApplicationTag, Any]]:
        with self._lock:
            key = (int(obj_type), int(instance))
            if key in self._objects and int(prop_id) in self._objects[key]:
                return self._objects[key][int(prop_id)]
            return None

    def read_property(self, obj_type: int, instance: int, prop_id: int, timeout_ms: int = 500) -> Tuple[BACnetStatus, Optional[BACnetApplicationTag], Any]:
        with self._lock:
            self.total_read_requests += 1
            self.in_flight_transactions += 1
            if self.in_flight_transactions > 1:
                self.reentrancy_violations += 1
            if self.in_flight_transactions > self.max_concurrent_transactions:
                self.max_concurrent_transactions = self.in_flight_transactions

        try:
            if not self.online:
                time.sleep(min(timeout_ms / 1000.0, 0.05))
                self.total_timeouts += 1
                return BACnetStatus.TIMEOUT, None, None

            if self.response_delay_sec > 0:
                time.sleep(self.response_delay_sec)

            with self._lock:
                prop = self.get_property(obj_type, instance, prop_id)
                if prop is None:
                    return BACnetStatus.ERROR, None, None
                tag, val = prop
                return BACnetStatus.OK, tag, val
        finally:
            with self._lock:
                self.in_flight_transactions -= 1

    def write_property(self, obj_type: int, instance: int, prop_id: int,
                       tag: BACnetApplicationTag, value: Any, priority: int = 16, timeout_ms: int = 500) -> BACnetStatus:
        with self._lock:
            self.total_write_requests += 1
            self.in_flight_transactions += 1
            if self.in_flight_transactions > 1:
                self.reentrancy_violations += 1
            if self.in_flight_transactions > self.max_concurrent_transactions:
                self.max_concurrent_transactions = self.in_flight_transactions

        try:
            if not self.online:
                time.sleep(min(timeout_ms / 1000.0, 0.05))
                self.total_timeouts += 1
                return BACnetStatus.TIMEOUT

            if self.response_delay_sec > 0:
                time.sleep(self.response_delay_sec)

            with self._lock:
                # Type validation
                if obj_type == BACnetObjectType.ANALOG_VALUE:
                    val_float = float(value)
                    self.set_property(obj_type, instance, prop_id, BACnetApplicationTag.REAL, val_float)
                elif obj_type == BACnetObjectType.BINARY_VALUE:
                    val_enum = 1 if bool(value) or value == 1 else 0
                    self.set_property(obj_type, instance, prop_id, BACnetApplicationTag.ENUMERATED, val_enum)
                elif obj_type == BACnetObjectType.MULTI_STATE_VALUE:
                    val_uint = int(value)
                    self.set_property(obj_type, instance, prop_id, BACnetApplicationTag.UNSIGNED_INT, val_uint)
                else:
                    self.set_property(obj_type, instance, prop_id, tag, value)
                return BACnetStatus.OK
        finally:
            with self._lock:
                self.in_flight_transactions -= 1

    def set_online(self, online: bool):
        with self._lock:
            self.online = online

    def reset_stats(self):
        with self._lock:
            self.total_read_requests = 0
            self.total_write_requests = 0
            self.total_timeouts = 0
            self.in_flight_transactions = 0
            self.max_concurrent_transactions = 0
            self.reentrancy_violations = 0
