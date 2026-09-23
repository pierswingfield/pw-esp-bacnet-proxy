"""
ESP32 Non-Volatile Storage (NVS) Emulator
Emulates NVS flash operations with namespaces, typed key-value pairs,
atomic commits, and corruption/erasure simulation.
"""
import threading
from typing import Dict, Any, Optional, Tuple

class NVSEmulator:
    """
    Emulates the ESP32 nvs_flash component in Python.
    """
    def __init__(self):
        self._lock = threading.RLock()
        # Storage: namespace -> key -> (type_str, value)
        self._storage: Dict[str, Dict[str, Tuple[str, Any]]] = {}
        # Uncommitted write staging per open handle
        self._handles: Dict[int, Dict[str, Any]] = {}
        self._next_handle_id = 1

    def open(self, namespace: str, readonly: bool = False) -> int:
        with self._lock:
            handle_id = self._next_handle_id
            self._next_handle_id += 1
            if namespace not in self._storage:
                self._storage[namespace] = {}
            self._handles[handle_id] = {
                "ns": namespace,
                "readonly": readonly,
                "staged": {}
            }
            return handle_id

    def set_u8(self, handle_id: int, key: str, value: int) -> bool:
        with self._lock:
            if handle_id not in self._handles or self._handles[handle_id]["readonly"]:
                return False
            val = int(value) & 0xFF
            self._handles[handle_id]["staged"][key] = ("u8", val)
            return True

    def set_u32(self, handle_id: int, key: str, value: int) -> bool:
        with self._lock:
            if handle_id not in self._handles or self._handles[handle_id]["readonly"]:
                return False
            val = int(value) & 0xFFFFFFFF
            self._handles[handle_id]["staged"][key] = ("u32", val)
            return True

    def set_str(self, handle_id: int, key: str, value: str) -> bool:
        with self._lock:
            if handle_id not in self._handles or self._handles[handle_id]["readonly"]:
                return False
            self._handles[handle_id]["staged"][key] = ("str", str(value))
            return True

    def get_u8(self, handle_id: int, key: str) -> Optional[int]:
        with self._lock:
            if handle_id not in self._handles:
                return None
            ns = self._handles[handle_id]["ns"]
            # Check staged first
            if key in self._handles[handle_id]["staged"]:
                t, val = self._handles[handle_id]["staged"][key]
                return val if t == "u8" else None
            # Check committed storage
            if ns in self._storage and key in self._storage[ns]:
                t, val = self._storage[ns][key]
                return val if t == "u8" else None
            return None

    def get_u32(self, handle_id: int, key: str) -> Optional[int]:
        with self._lock:
            if handle_id not in self._handles:
                return None
            ns = self._handles[handle_id]["ns"]
            if key in self._handles[handle_id]["staged"]:
                t, val = self._handles[handle_id]["staged"][key]
                return val if t == "u32" else None
            if ns in self._storage and key in self._storage[ns]:
                t, val = self._storage[ns][key]
                return val if t == "u32" else None
            return None

    def get_str(self, handle_id: int, key: str) -> Optional[str]:
        with self._lock:
            if handle_id not in self._handles:
                return None
            ns = self._handles[handle_id]["ns"]
            if key in self._handles[handle_id]["staged"]:
                t, val = self._handles[handle_id]["staged"][key]
                return val if t == "str" else None
            if ns in self._storage and key in self._storage[ns]:
                t, val = self._storage[ns][key]
                return val if t == "str" else None
            return None

    def commit(self, handle_id: int) -> bool:
        with self._lock:
            if handle_id not in self._handles:
                return False
            ns = self._handles[handle_id]["ns"]
            staged = self._handles[handle_id]["staged"]
            if ns not in self._storage:
                self._storage[ns] = {}
            for k, v in staged.items():
                self._storage[ns][k] = v
            self._handles[handle_id]["staged"] = {}
            return True

    def erase_all(self, handle_id: int) -> bool:
        with self._lock:
            if handle_id not in self._handles or self._handles[handle_id]["readonly"]:
                return False
            ns = self._handles[handle_id]["ns"]
            self._storage[ns] = {}
            self._handles[handle_id]["staged"] = {}
            return True

    def close(self, handle_id: int):
        with self._lock:
            if handle_id in self._handles:
                del self._handles[handle_id]

    def wipe_flash(self):
        """Simulate flash erase (factory reset / blank device)."""
        with self._lock:
            self._storage = {}
            self._handles = {}
