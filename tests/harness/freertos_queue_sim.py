"""
FreeRTOS Dual-Priority Queue & BACnet Worker Task Simulator
Implements bounded dual-priority queue serialization, priority preemption,
circuit breaker target health state machine, and sync/async caller interfaces.
"""
from dataclasses import dataclass, field
from enum import IntEnum
import queue
import threading
import time
from typing import Optional, Callable, Any, Tuple

from tests.harness.bacnet_simulator import (
    BACnetSimulator, BACnetObjectType, BACnetPropertyId, BACnetStatus,
    BACnetApplicationTag
)

class BACnetPriority(IntEnum):
    LOW = 0
    NORMAL = 1
    HIGH = 2

class BACnetTargetHealth(IntEnum):
    ONLINE = 0
    DEGRADED = 1
    OFFLINE = 2

@dataclass
class BACnetResponse:
    request_id: int = 0
    status: BACnetStatus = BACnetStatus.OK
    tag: Optional[BACnetApplicationTag] = None
    value: Any = None
    rtt_ms: float = 0.0
    error_class: int = 0
    error_code: int = 0

@dataclass
class BACnetRequest:
    request_id: int
    op_type: int # 0=Read, 2=Write, 3=WhoIs, 4=Probe
    priority: BACnetPriority
    device_id: int
    object_type: int
    object_instance: int
    property_id: int
    array_index: int = 0xFFFFFFFF
    write_value: Any = None
    write_tag: BACnetApplicationTag = BACnetApplicationTag.NULL
    write_priority: int = 16
    is_sync: bool = True
    sync_event: Optional[threading.Event] = None
    response: Optional[BACnetResponse] = None
    async_cb: Optional[Callable[[BACnetResponse], None]] = None
    timeout_ms: int = 500
    enqueued_time: float = field(default_factory=time.time)

class BACnetWorkerQueueSim:
    """
    Simulates the dedicated BACnet Worker Task and dual-priority FreeRTOS queues.
    """
    HIGH_QUEUE_DEPTH = 8
    NORMAL_QUEUE_DEPTH = 24

    def __init__(self, simulator: BACnetSimulator):
        self.simulator = simulator
        self.high_queue = queue.Queue(maxsize=self.HIGH_QUEUE_DEPTH)
        self.normal_queue = queue.Queue(maxsize=self.NORMAL_QUEUE_DEPTH)
        self._lock = threading.RLock()
        self._running = True
        self._request_counter = 0

        # Target Health / Circuit Breaker State
        self.health = BACnetTargetHealth.ONLINE
        self.consecutive_timeouts = 0
        self.fast_fail_count = 0
        self.last_probe_time = 0.0
        self.probe_interval_sec = 5.0
        self.processed_requests_count = 0
        self.high_prio_processed = 0
        self.normal_prio_processed = 0
        self.dropped_requests_count = 0

        # Dedicated worker thread
        self.worker_thread = threading.Thread(target=self._worker_loop, daemon=True)
        self.worker_thread.start()

    def _next_request_id(self) -> int:
        with self._lock:
            self._request_counter += 1
            return self._request_counter

    def _worker_loop(self):
        while self._running:
            req: Optional[BACnetRequest] = None
            # 1. Drain High Priority queue first
            try:
                req = self.high_queue.get_nowait()
            except queue.Empty:
                # 2. If High is empty, drain Normal Priority queue
                try:
                    req = self.normal_queue.get_nowait()
                except queue.Empty:
                    req = None

            if req is not None:
                self._process_request(req)
            else:
                self._housekeeping()
                time.sleep(0.005)

    def _housekeeping(self):
        """Idle housekeeping: background recovery probe if offline."""
        with self._lock:
            now = time.time()
            if self.health == BACnetTargetHealth.OFFLINE:
                if (now - self.last_probe_time) >= self.probe_interval_sec:
                    self.last_probe_time = now
                    # Probe target with device name read
                    status, tag, val = self.simulator.read_property(
                        BACnetObjectType.DEVICE, self.simulator.device_id,
                        BACnetPropertyId.PROP_OBJECT_NAME, timeout_ms=300
                    )
                    if status == BACnetStatus.OK:
                        self.health = BACnetTargetHealth.ONLINE
                        self.consecutive_timeouts = 0

    def _process_request(self, req: BACnetRequest):
        start_time = time.time()
        resp = BACnetResponse(request_id=req.request_id)
        
        # Check Circuit Breaker fast-fail
        with self._lock:
            is_offline = (self.health == BACnetTargetHealth.OFFLINE)
            if is_offline and req.priority != BACnetPriority.HIGH and req.op_type != 4:
                self.fast_fail_count += 1
                resp.status = BACnetStatus.TARGET_OFFLINE
                resp.rtt_ms = (time.time() - start_time) * 1000.0
                req.response = resp
                if req.is_sync and req.sync_event:
                    req.sync_event.set()
                if req.async_cb:
                    req.async_cb(resp)
                return

        # Execute BACnet operation
        if req.op_type in (0, 1, 4): # Read Property / Probe
            status, tag, val = self.simulator.read_property(
                req.object_type, req.object_instance, req.property_id, req.timeout_ms
            )
            resp.status = status
            resp.tag = tag
            resp.value = val
        elif req.op_type == 2: # Write Property
            status = self.simulator.write_property(
                req.object_type, req.object_instance, req.property_id,
                req.write_tag, req.write_value, req.write_priority, req.timeout_ms
            )
            resp.status = status
            resp.value = req.write_value

        resp.rtt_ms = (time.time() - start_time) * 1000.0

        # Update Circuit Breaker metrics
        with self._lock:
            self.processed_requests_count += 1
            if req.priority == BACnetPriority.HIGH:
                self.high_prio_processed += 1
            else:
                self.normal_prio_processed += 1

            if resp.status == BACnetStatus.TIMEOUT:
                self.consecutive_timeouts += 1
                if self.consecutive_timeouts >= 3:
                    self.health = BACnetTargetHealth.OFFLINE
                    self.last_probe_time = time.time()
                elif self.consecutive_timeouts >= 1:
                    self.health = BACnetTargetHealth.DEGRADED
            elif resp.status == BACnetStatus.OK:
                self.consecutive_timeouts = 0
                self.health = BACnetTargetHealth.ONLINE

        req.response = resp
        if req.is_sync and req.sync_event:
            req.sync_event.set()
        if req.async_cb:
            req.async_cb(resp)

    def enqueue_sync(self, op_type: int, obj_type: int, instance: int, prop_id: int,
                     prio: BACnetPriority = BACnetPriority.NORMAL,
                     write_val: Any = None, write_tag: BACnetApplicationTag = BACnetApplicationTag.NULL,
                     timeout_ms: int = 500, wait_enqueue_ms: int = 0) -> BACnetResponse:
        req_id = self._next_request_id()
        sync_event = threading.Event()
        req = BACnetRequest(
            request_id=req_id,
            op_type=op_type,
            priority=prio,
            device_id=self.simulator.device_id,
            object_type=obj_type,
            object_instance=instance,
            property_id=prop_id,
            write_value=write_val,
            write_tag=write_tag,
            is_sync=True,
            sync_event=sync_event,
            timeout_ms=timeout_ms
        )

        target_q = self.high_queue if prio == BACnetPriority.HIGH else self.normal_queue
        wait_time = (wait_enqueue_ms / 1000.0) if wait_enqueue_ms > 0 else 0.0

        try:
            if wait_time > 0:
                target_q.put(req, timeout=wait_time)
            else:
                target_q.put_nowait(req)
        except queue.Full:
            with self._lock:
                self.dropped_requests_count += 1
            return BACnetResponse(request_id=req_id, status=BACnetStatus.QUEUE_FULL)

        # Wait for worker completion
        completed = sync_event.wait(timeout=(timeout_ms / 1000.0) + 0.5)
        if not completed or req.response is None:
            return BACnetResponse(request_id=req_id, status=BACnetStatus.TIMEOUT)
        return req.response

    def read_real_sync(self, obj_type: int, instance: int, prop_id: int, timeout_ms: int = 500) -> Tuple[bool, float]:
        resp = self.enqueue_sync(0, obj_type, instance, prop_id, BACnetPriority.NORMAL, timeout_ms=timeout_ms)
        if resp.status == BACnetStatus.OK and resp.value is not None:
            return True, float(resp.value)
        return False, 0.0

    def read_bool_sync(self, obj_type: int, instance: int, prop_id: int, timeout_ms: int = 500) -> Tuple[bool, bool]:
        resp = self.enqueue_sync(0, obj_type, instance, prop_id, BACnetPriority.NORMAL, timeout_ms=timeout_ms)
        if resp.status == BACnetStatus.OK and resp.value is not None:
            return True, bool(resp.value)
        return False, False

    def read_msv_sync(self, obj_type: int, instance: int, prop_id: int, timeout_ms: int = 500) -> Tuple[bool, int]:
        resp = self.enqueue_sync(0, obj_type, instance, prop_id, BACnetPriority.NORMAL, timeout_ms=timeout_ms)
        if resp.status == BACnetStatus.OK and resp.value is not None:
            return True, int(resp.value)
        return False, 0

    def write_real_sync(self, obj_type: int, instance: int, prop_id: int, val: float, timeout_ms: int = 500) -> bool:
        resp = self.enqueue_sync(2, obj_type, instance, prop_id, BACnetPriority.HIGH,
                                 write_val=val, write_tag=BACnetApplicationTag.REAL,
                                 timeout_ms=timeout_ms, wait_enqueue_ms=200)
        return resp.status == BACnetStatus.OK

    def write_bool_sync(self, obj_type: int, instance: int, prop_id: int, val: bool, timeout_ms: int = 500) -> bool:
        resp = self.enqueue_sync(2, obj_type, instance, prop_id, BACnetPriority.HIGH,
                                 write_val=1 if val else 0, write_tag=BACnetApplicationTag.ENUMERATED,
                                 timeout_ms=timeout_ms, wait_enqueue_ms=200)
        return resp.status == BACnetStatus.OK

    def write_msv_sync(self, obj_type: int, instance: int, prop_id: int, val: int, timeout_ms: int = 500) -> bool:
        resp = self.enqueue_sync(2, obj_type, instance, prop_id, BACnetPriority.HIGH,
                                 write_val=val, write_tag=BACnetApplicationTag.UNSIGNED_INT,
                                 timeout_ms=timeout_ms, wait_enqueue_ms=200)
        return resp.status == BACnetStatus.OK

    def trigger_probe_now(self):
        """Force a background recovery probe immediately."""
        with self._lock:
            self.last_probe_time = 0.0
        self._housekeeping()

    def shutdown(self):
        self._running = False
        if self.worker_thread.is_alive():
            self.worker_thread.join(timeout=0.5)
