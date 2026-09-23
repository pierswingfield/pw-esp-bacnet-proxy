"""
Matter Thermostat Endpoint Simulator (Cluster 0x0201)
Simulates CSA Matter data model attribute reads and writes mapped to
the canonical HVAC room model in hvac_core.
"""
from enum import IntEnum
from typing import Tuple, Optional

from tests.harness.hvac_core_engine import HvacCoreEngine

class MatterSystemMode(IntEnum):
    OFF = 0
    AUTO = 1
    COOL = 3
    HEAT = 4

class MatterThermostatEndpointSim:
    """
    Simulates a single-room Matter Thermostat Endpoint (Endpoint 1, Cluster 0x0201).
    Mapped to canonical Room A (room_idx = 0).
    """
    CLUSTER_THERMOSTAT = 0x0201

    # Attribute IDs
    ATTR_LOCAL_TEMPERATURE = 0x0000
    ATTR_OCCUPIED_COOLING_SETPOINT = 0x0011
    ATTR_OCCUPIED_HEATING_SETPOINT = 0x0012
    ATTR_SYSTEM_MODE = 0x001C

    def __init__(self, hvac_core: HvacCoreEngine, room_idx: int = 0):
        self.hvac_core = hvac_core
        self.room_idx = room_idx

    def read_local_temperature(self) -> Tuple[bool, Optional[int]]:
        """
        Reads Room temperature, scaled to 0.01°C (int16_t).
        e.g. 21.5°C -> 2150.
        """
        ok, temp_c = self.hvac_core.get_room_temperature(self.room_idx)
        if not ok:
            return False, None
        return True, int(round(temp_c * 100))

    def read_occupied_cooling_setpoint(self) -> Tuple[bool, Optional[int]]:
        """
        Reads Room setpoint, scaled to 0.01°C (int16_t).
        e.g. 22.0°C -> 2200.
        """
        ok, sp_c = self.hvac_core.get_room_setpoint(self.room_idx)
        if not ok:
            return False, None
        return True, int(round(sp_c * 100))

    def write_occupied_cooling_setpoint(self, raw_val: int) -> Tuple[bool, int]:
        """
        Writes Room setpoint from raw 0.01°C value.
        e.g. 2350 -> 23.5°C. Clamped to [18.0°C, 30.0°C].
        Returns (success, applied_raw_val).
        """
        temp_c = float(raw_val) / 100.0
        ok, applied_c = self.hvac_core.set_room_setpoint(self.room_idx, temp_c)
        return ok, int(round(applied_c * 100))

    def read_occupied_heating_setpoint(self) -> Tuple[bool, Optional[int]]:
        return self.read_occupied_cooling_setpoint()

    def write_occupied_heating_setpoint(self, raw_val: int) -> Tuple[bool, int]:
        return self.write_occupied_cooling_setpoint(raw_val)

    def read_system_mode(self) -> Tuple[bool, MatterSystemMode]:
        """
        Reads Room power status and maps to Matter SystemMode (0=Off, 4=Heat).
        """
        ok, is_on = self.hvac_core.get_room_power(self.room_idx)
        if not ok:
            return False, MatterSystemMode.OFF
        return True, MatterSystemMode.HEAT if is_on else MatterSystemMode.OFF

    def write_system_mode(self, mode: MatterSystemMode) -> bool:
        """
        Writes SystemMode: OFF (0) turns power off; AUTO/HEAT/COOL turns power on.
        """
        if mode == MatterSystemMode.OFF:
            return self.hvac_core.set_room_power(self.room_idx, False)
        elif mode in (MatterSystemMode.AUTO, MatterSystemMode.COOL, MatterSystemMode.HEAT):
            return self.hvac_core.set_room_power(self.room_idx, True)
        return False
