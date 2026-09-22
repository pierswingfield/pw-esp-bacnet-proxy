# Handoff Report: BACnet Architecture Survey & Bounded Worker Queue Design

**Author**: BACnet & Worker Queue Explorer (`explorer_survey_2`)  
**Target Recipient**: Parent Orchestrator / Implementer Agent  
**Date**: 2026-09-02  
**Branch Context**: `codex/protocol-agnostic-core`  

---

## 1. Observation

### 1.1 BACnet Client Stack Architecture & Constraints
Direct examination of `firmware/bacnet_bridge/components/bacnet_client/` and `firmware/bacnet_bridge/main/main.c` reveals the following concrete characteristics:

1. **Strict Single Transaction Limit (`MAX_TSM_TRANSACTIONS=1`)**:
   - In `components/bacnet_client/CMakeLists.txt` lines 59–69:
     ```cmake
     target_compile_definitions(${COMPONENT_LIB} PRIVATE
         BACDL_BIP
         BACNET_IP_PORT=47808
         MAX_APDU=1476
         MAX_TSM_TRANSACTIONS=1
         PRINT_ENABLED=0
         BACNET_BIG_ENDIAN=0
         BACAPP_MINIMAL
         BACNET_STACK_DEPRECATED_DISABLE
         BACNET_PROTOCOL_REVISION=16
     )
     ```
   - In `components/bacnet_client/bacnet/basic/tsm/tsm.c` lines 26–42:
     ```c
     uint8_t Handler_Transmit_Buffer[MAX_PDU];
     #if (MAX_TSM_TRANSACTIONS)
     static BACNET_TSM_DATA TSM_List[MAX_TSM_TRANSACTIONS];
     static uint8_t Current_Invoke_ID = 1;
     ```
   - **Finding**: The Transaction State Machine (TSM) has exactly 1 slot (`TSM_List[1]`). The transmit buffer (`Handler_Transmit_Buffer`) is a single global static buffer shared across all BACnet services (`s_rp.c`, `s_wp.c`, `s_whois.c`, `s_iam.c`). Any concurrent transmission or invocation immediately corrupts the active transaction state and packet buffers.

2. **Datalink & UDP Socket Binding (`bip_socket_esp_idf.c`)**:
   - File: `firmware/bacnet_bridge/components/bacnet_client/bacnet/datalink/bip_socket_esp_idf.c`
   - Line 21: `static int BipSockFd = -1;`
   - Line 56: `fcntl(BipSockFd, F_SETFL, flags | O_NONBLOCK);`
   - Line 61: `setsockopt(BipSockFd, SOL_SOCKET, SO_BINDTODEVICE, if_name, strlen(if_name))` explicitly binds the single non-blocking UDP socket on port 47808 to `EthNetif` (W5500 SPI or RTL8201 RMII).

3. **Current In-Caller Polling Loop & State Machine**:
   - In `main.c` lines 534–554, shared transaction variables are declared:
     ```c
     static BACNET_ADDRESS Target_Address;
     static uint8_t Request_Invoke_ID = 0;
     static volatile bool Reply_Received = false;
     static volatile bool Reply_Errored = false;
     static BACNET_APPLICATION_DATA_VALUE Last_Read_Value;
     static uint8_t Last_Read_Raw[MAX_APDU];
     static int Last_Read_Raw_Len = 0;
     static SemaphoreHandle_t BacnetMutex;
     static volatile bool BacnetReady = false;
     ```
   - In `main.c` lines 753–795 (`wait_for_transaction()`):
     ```c
     static bool wait_for_transaction(void)
     {
         if (Request_Invoke_ID == 0) return false;
         uint32_t last_tick = xTaskGetTickCount();
         for (int i = 0; i < 20; i++) {
             uint8_t pdu[BIP_MPDU_MAX] = {0};
             BACNET_ADDRESS src = {0};
             uint16_t pdu_len = bip_receive(&src, pdu, sizeof(pdu), 0);
             if (pdu_len) {
                 npdu_handler(&src, pdu, pdu_len);
             }
             uint32_t now = xTaskGetTickCount();
             uint32_t elapsed_ms = (now - last_tick) * portTICK_PERIOD_MS;
             if (elapsed_ms >= 100) {
                 tsm_timer_milliseconds(elapsed_ms);
                 last_tick = now;
             }
             if (Reply_Received || Reply_Errored) return Reply_Received;
             if (tsm_invoke_id_free(Request_Invoke_ID)) return false;
             if (tsm_invoke_id_failed(Request_Invoke_ID)) {
                 tsm_free_invoke_id(Request_Invoke_ID);
                 return false;
             }
             vTaskDelay(pdMS_TO_TICKS(25));
         }
         if (Request_Invoke_ID != 0) tsm_free_invoke_id(Request_Invoke_ID);
         return false;
     }
     ```
   - In `main.c` lines 801–846, `bacnet_read_locked_idx()` and `bacnet_write_locked_idx()` clear flags, issue `Send_Read_Property_Request` / `Send_Write_Property_Request`, and execute `wait_for_transaction()` inside the calling task.

4. **Task Lifecycle of `bacnet_client_task`**:
   - In `main.c` lines 1374–1457: `bacnet_client_task` initializes the datalink, binds the target device, performs a single read test on startup, sets `BacnetReady = true`, and then deletes itself (`vTaskDelete(NULL)`).
   - **Finding**: There is currently **no permanent BACnet background worker or receiver task running**. When BACnet transactions happen later, they are driven entirely inside the calling threads (e.g. `httpd`, `mqtt_command_task`, `mqtt_state_task`, `object_scan_task`).

---

### 1.2 Catalog of All BACnet Request Entry Points
Code investigation across `firmware/bacnet_bridge/main/main.c` identified 4 distinct caller categories initiating BACnet read/write transactions:

| Category | Source File / Task | Specific Function / Endpoint | BACnet Operations Triggered | Concurrency & Mutex Usage |
|---|---|---|---|---|
| **Web REST API Handlers** | `main.c` (running on `httpd` worker threads) | `GET /api/status` (`api_status_handler`, line 2327) | Reads ~15–30 points on cache miss (sys power, boost, room setpoints/temps/power, valve statuses, flow, fan speeds, alarms). | Calls `read_real_property`, `read_bool_property`, `read_msv_property` (acquires `BacnetMutex` per uncached point, 6s timeout). |
| | | `GET /api/health` (`api_health_handler`, line 2648) | Reads cooling/heating valve status, 7 alarm binary values, room thermal outputs. | Calls `read_real_property`, `read_bool_property`, `read_msv_property`. |
| | | `GET /api/bacnet/read` (`api_bacnet_read_handler`, line 2716) | Ad-hoc read of arbitrary object type/instance/property. | Calls `explorer_read()` -> `bacnet_read_locked_idx()`. |
| | | `POST /api/bacnet/write` (`api_bacnet_write_handler`, line 2767) | Ad-hoc write of arbitrary object type/instance/property/value. | Calls `explorer_write()` -> `bacnet_write_locked_idx()`. |
| | | `POST /api/room/{idx}/setpoint` (`api_room_setpoint_handler`, line 2966) | User room setpoint change. | Calls `hvac_command_room_setpoint()` -> `write_real_property()`. |
| | | `POST /api/room/{idx}/power` (`api_room_power_handler`, line 2993) | User room power change. | Calls `hvac_command_room_power()` -> `write_bool_property()`. |
| | | `POST /api/system/power` (`api_system_power_handler`, line 3017) | User system power change. | Calls `hvac_command_system_power()` -> `write_bool_property()`. |
| | | `POST /api/boost` (`api_boost_handler`, line 3047) | Boost mode activation. | Calls `boost_apply()` -> `write_msv_property()`. |
| | | `GET /api/objects/inspect` (`api_objects_inspect_handler`, line 3973) | Reads object name, present value, units, description. | Calls `bacnet_read_locked()` across multiple properties. |
| | | `POST /api/objects/batch` (`api_objects_batch_handler`, line 4071) | Batch present-value read for up to 32 objects. | Calls `bacnet_read_locked()` in a loop. |
| | | `POST /api/bacnet/discover` (`api_bacnet_discover_handler`, line 4654) | Who-Is broadcast & 32-address unicast sweep + device name/vendor/model reads. | Acquires `BacnetMutex` for up to 8s; pumps `bip_receive` for 2.0s. |
| **MQTT Command Handler** | `main.c` (`mqtt_command_task`, line 6433) | Subscribed MQTT command topics (`.../set`) | Writes setpoint, room mode/power, system power, boost mode, then issues readback (`mqtt_republish_*`). | Calls `hvac_command_*`, `write_*_property`, `read_*_property` under `BacnetMutex`. |
| **MQTT Periodic Telemetry** | `main.c` (`mqtt_state_task`, line 6541) | Periodic loop (every 45 seconds) | Sequential read burst of ~30–50 points: system power, boost mode, 6 properties per active room (temps, setpoints, power, supply air, req/cur output), custom MQTT points, health valve/flow/alarms. | Loops through `read_real_property`, `read_bool_property`, `read_msv_property`, taking and releasing `BacnetMutex` for every single uncached property. |
| **Object Catalog Scanner** | `main.c` (`object_scan_task`, line 3633) | `POST /api/objects/scan/start` | Scans up to 430+ objects: reads `PROP_OBJECT_LIST` index 0..N, then reads `PROP_OBJECT_NAME` for each object. | Loops taking `BacnetMutex` for 50–100ms per object over 20–60 seconds. |
| **Future Matter Transport** | `components/matter_bridge` (future integration) | Matter Thermostat cluster attribute write/read callbacks | Write Occupied Cooling/Heating Setpoint, write System Mode, read Local Temperature. | Must route through `hvac_core` and worker queue without direct BACnet calls. |

---

### 1.3 Memory Footprint & Stack Allocation Observations
In `main.c`, stack sizes for BACnet-calling tasks were repeatedly increased due to `wait_for_transaction()` executing `bip_receive()` (with `BIP_MPDU_MAX = 1476`), `rp_ack_decode()`, and `bacapp_decode()` on the caller's stack:
- `mqtt_command_task` stack: **24,576 bytes** (line 6492–6498)
- `mqtt_state_task` stack: **16,384 bytes** (line 6492, line 6942)
- `object_scan_task` stack: **24,576 bytes** in PSRAM (line 3769)
- `bacnet_client_task` stack: **24,576 bytes** static DRAM (line 1598)
- `httpd` worker task stack: **32,768 bytes** (sdkconfig defaults / line 6494)
- **Total DRAM committed to task stacks under the old model**: **>110 KB DRAM**, leaving as little as 3–12 KB free internal DRAM on 4MB W5500 boards and causing task spawn failures (`xTaskCreate` failures).

---

## 2. Logic Chain

### 2.1 Concurrency Bottlenecks & Race Conditions
1. **Observation 1.1 (Single TSM & Shared Tx Buffer)** + **Observation 1.2 (Multi-Task Entry Points)**:
   - Because `MAX_TSM_TRANSACTIONS = 1` and `Handler_Transmit_Buffer` is a non-thread-safe global buffer, no two BACnet confirmed transactions can execute simultaneously under any circumstances.
2. **Observation 1.1 (Caller-Context Polling)** + **Observation 1.3 (Stack Inflation)**:
   - When each calling task executes `wait_for_transaction()`, every task must reserve massive stack headroom (16–24 KB) to survive deep APDU decoding frames. Moving the execution of `wait_for_transaction()`, `bip_receive()`, and APDU decoding into a **single dedicated worker task** allows all calling task stacks (`mqtt_command`, `mqtt_state`, `httpd`, `matter`) to shrink down to standard 4 KB stacks, recovering **>40 KB of precious internal SRAM**.
3. **Observation 1.2 (Lack of Prioritization)**:
   - Currently, `mqtt_state_task` issues 30–50 sequential BACnet read requests every 45 seconds, and `object_scan_task` issues over 800 read requests during a scan.
   - When a user changes a temperature setpoint or powers on a room via Web UI, MQTT, or Matter, the write request gets stuck waiting behind the background telemetry reads for seconds or fails due to a 6-second mutex timeout.
   - **Conclusion**: A dual-priority queue (High Priority for user writes; Normal Priority for background telemetry; Low for catalog scan) is required so write commands jump ahead of bulk reads without delay.
4. **Observation 1.1 (Socket Contention & Stale Packet Contamination)**:
   - When transactions are driven by random caller tasks, UDP packets arriving while no task is inside `wait_for_transaction()` linger in lwIP's socket buffer.
   - When a subsequent task initiates a transaction, its `bip_receive()` poll consumes the stale reply from the previous transaction. While the ack handler checks invoke ID, processing stale packets wastes loop iterations and can cause legitimate replies to be dropped or delayed.
   - **Conclusion**: A single worker task that owns the socket and maintains a continuous pump loop completely eliminates socket contention and stale packet accumulation.
5. **Observation 1.2 (Target Offline Freeze Cascades)**:
   - When the Delta controller is offline or Ethernet is disconnected, `wait_for_transaction()` waits 500 ms per transaction.
   - For `mqtt_state_task`, 50 points * 500 ms = **25 seconds of blocking per poll**.
   - During this 25-second window, `BacnetMutex` is held or contested continuously, starving HTTP requests and freezing the web dashboard.
   - **Conclusion**: A Circuit Breaker / Target Health Tracker state machine (`ONLINE`, `DEGRADED`, `OFFLINE`) is required in the worker to fast-fail telemetry requests (0 ms) when the target is unreachable and probe periodically with a single lightweight heartbeat.

---

## 3. Architecture Specification: Bounded Single BACnet Worker Queue

```
                       ┌────────────────────────────────────────────────────────┐
                       │                   PRESENTATION LAYERS                  │
                       │   Web REST Handlers  │  MQTT Client  │  Matter Node    │
                       └───────────┬───────────────────┬───────────────┬────────┘
                                   │                   │               │
                                   ▼                   ▼               ▼
                       ┌────────────────────────────────────────────────────────┐
                       │          HVAC CORE (Canonical Domain Model)            │
                       │   Room Mappings, Semantic Setpoint / Power Operations  │
                       └───────────────────────────┬────────────────────────────┘
                                                   │
                                                   ▼
                       ┌────────────────────────────────────────────────────────┐
                       │        BACNET WORKER CLIENT API (bacnet_worker.h)      │
                       │   - bacnet_worker_read_property_sync()                 │
                       │   - bacnet_worker_write_property_sync()                │
                       │   - bacnet_worker_enqueue_async()                      │
                       │   - Cache-first check (0ms return on cache hit)        │
                       └───────────────┬────────────────────────┬───────────────┘
                                       │                        │
                     High Priority (Writes)           Normal / Low (Polls / Scan)
                                       │                        │
                                       ▼                        ▼
                       ┌───────────────────────┐ ┌──────────────────────────────┐
                       │  High-Priority Queue  │ │ Normal / Low Priority Queue  │
                       │   (Depth: 8 items)    │ │      (Depth: 24 items)       │
                       └───────────────┬───────┘ └──────────────┬───────────────┘
                                       │                        │
                                       └───────────┬────────────┘
                                                   │
                                                   ▼
                       ┌────────────────────────────────────────────────────────┐
                       │          DEDICATED BACNET WORKER TASK                  │
                       │              (bacnet_worker_task)                      │
                       │  - Single Owner of UDP Socket & bip_receive()          │
                       │  - Single Owner of TSM & Handler_Transmit_Buffer       │
                       │  - Circuit Breaker / Health Tracker (Online/Offline)   │
                       │  - Updates BacnetCache on Successful Read/Write        │
                       │  - Completes Synced Callers via FreeRTOS Semaphore     │
                       │  - Executes Async Completion Callbacks                 │
                       └───────────────────────────┬────────────────────────────┘
                                                   │
                                                   ▼
                       ┌────────────────────────────────────────────────────────┐
                       │            BACNET/IP DATALINK (W5500 / RTL8201)        │
                       │         Delta DAC-1180E Controller (10.0.3.16)         │
                       └────────────────────────────────────────────────────────┘
```

---

### 3.1 Data Structure Definitions (`bacnet_worker.h`)

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "bacnet/bacenum.h"
#include "bacnet/bacapp.h"

#define BACNET_WORKER_HIGH_QUEUE_DEPTH   8
#define BACNET_WORKER_NORMAL_QUEUE_DEPTH 24
#define BACNET_WORKER_STACK_SIZE         24576
#define BACNET_WORKER_TASK_PRIO          5

typedef enum {
    BACNET_OP_READ_PROPERTY = 0,
    BACNET_OP_READ_PROPERTY_INDEX,
    BACNET_OP_WRITE_PROPERTY,
    BACNET_OP_WHOIS_DISCOVERY,
    BACNET_OP_PROBE_LIVENESS
} bacnet_op_type_t;

typedef enum {
    BACNET_PRIO_LOW = 0,     /* Object scanner catalog crawl */
    BACNET_PRIO_NORMAL = 1,  /* Periodic MQTT / Web status telemetry */
    BACNET_PRIO_HIGH = 2     /* User write commands (Web / MQTT / Matter) */
} bacnet_prio_t;

typedef enum {
    BACNET_STATUS_OK = 0,
    BACNET_STATUS_TIMEOUT,
    BACNET_STATUS_ERROR,
    BACNET_STATUS_ABORT,
    BACNET_STATUS_REJECT,
    BACNET_STATUS_TARGET_OFFLINE,
    BACNET_STATUS_QUEUE_FULL,
    BACNET_STATUS_INVALID_ARG
} bacnet_status_t;

typedef enum {
    BACNET_HEALTH_ONLINE = 0,
    BACNET_HEALTH_DEGRADED,
    BACNET_HEALTH_OFFLINE
} bacnet_target_health_t;

/* Response payload passed back to caller */
typedef struct {
    uint32_t                request_id;
    bacnet_status_t         status;
    BACNET_APPLICATION_TAG  tag;
    union {
        float               real_val;
        bool                bool_val;
        uint32_t            uint_val;
        int32_t             int_val;
        char                str_val[64];
    } val;
    uint8_t                 raw_data[64];
    uint16_t                raw_data_len;
    uint16_t                error_class;
    uint16_t                error_code;
    uint32_t                rtt_ms;
} bacnet_response_t;

/* Asynchronous completion callback signature */
typedef void (*bacnet_async_cb_t)(const bacnet_response_t *resp, void *user_ctx);

/* Request packet enqueued to the worker */
typedef struct {
    uint32_t                request_id;
    bacnet_op_type_t        op_type;
    bacnet_prio_t           priority;
    uint32_t                device_id;
    BACNET_OBJECT_TYPE      object_type;
    uint32_t                object_instance;
    BACNET_PROPERTY_ID      property_id;
    uint32_t                array_index;
    
    /* Write value (if op_type == BACNET_OP_WRITE_PROPERTY) */
    BACNET_APPLICATION_DATA_VALUE write_value;
    uint8_t                 write_priority; /* 1..16 or BACNET_NO_PRIORITY */

    /* Synchronization mechanism */
    bool                    is_sync;
    SemaphoreHandle_t       sync_sem;      /* Caller blocks on this if is_sync == true */
    bacnet_response_t      *sync_resp;     /* Filled by worker before giving sync_sem */
    
    /* Async callback mechanism */
    bacnet_async_cb_t       async_cb;      /* Invoked if is_sync == false and != NULL */
    void                   *user_ctx;

    /* Execution parameters */
    uint32_t                timeout_ms;
    uint32_t                enqueued_tick;
} bacnet_request_t;
```

---

### 3.2 Dual-Queue Prioritization & Scheduling Policy

1. **Queue Architecture**:
   - `s_high_prio_queue`: Bounded queue of 8 `bacnet_request_t` items.
   - `s_normal_prio_queue`: Bounded queue of 24 `bacnet_request_t` items.
   - `s_queue_set`: FreeRTOS Queue Set containing both queues.

2. **Worker Dequeue Loop**:
   - The worker executes:
     ```c
     for (;;) {
         bacnet_request_t req;
         bool got_item = false;
         
         /* 1. Always drain High Priority queue first */
         if (xQueueReceive(s_high_prio_queue, &req, 0) == pdTRUE) {
             got_item = true;
         } 
         /* 2. Otherwise receive from Normal Priority queue */
         else if (xQueueReceive(s_normal_prio_queue, &req, 0) == pdTRUE) {
             got_item = true;
         }
         /* 3. If both empty, block on Queue Set for up to 50ms */
         else {
             QueueSetMemberHandle_t active = xQueueSelectFromSet(s_queue_set, pdMS_TO_TICKS(50));
             if (active == s_high_prio_queue) {
                 xQueueReceive(s_high_prio_queue, &req, 0);
                 got_item = true;
             } else if (active == s_normal_prio_queue) {
                 xQueueReceive(s_normal_prio_queue, &req, 0);
                 got_item = true;
             }
         }

         if (got_item) {
             process_bacnet_request(&req);
         } else {
             /* Idle housekeeping: pump socket for unsolicited I-Am, tick TSM timer, check offline probe */
             bacnet_worker_housekeeping();
         }
     }
     ```

3. **Queue Full Handling**:
   - **High Priority (User Writes)**: Callers wait up to `pdMS_TO_TICKS(200)` to enqueue. If queue is full, returns `BACNET_STATUS_QUEUE_FULL`.
   - **Normal Priority (Telemetry Reads)**: Callers attempt enqueue with `timeout = 0`. If full, the poll is dropped, returning `BACNET_STATUS_QUEUE_FULL` immediately without blocking caller tasks or leaking memory.

---

### 3.3 Target Offline Handling & Circuit Breaker State Machine

To prevent the 25-second freeze cascade when the Delta controller is unreachable:

1. **Health States**:
   - `BACNET_HEALTH_ONLINE`: Consecutive timeouts = 0. All requests proceed normally.
   - `BACNET_HEALTH_DEGRADED`: 1 or 2 consecutive timeouts. Requests proceed with standard 500 ms timeout.
   - `BACNET_HEALTH_OFFLINE`: 3 consecutive timeouts.
2. **Fast-Fail Behavior in `BACNET_HEALTH_OFFLINE`**:
   - When a normal/low priority read request is received, the worker **immediately fast-fails** the request with `BACNET_STATUS_TARGET_OFFLINE` (0 ms elapsed).
   - Read requests serve stale cached values with `valid = false`.
3. **Heartbeat Recovery Probe**:
   - While in `BACNET_HEALTH_OFFLINE`, the worker suppresses bulk polling and schedules a lightweight single-property probe (`Read_Property(OBJECT_DEVICE, TargetDeviceInstance, PROP_OBJECT_NAME)`) every 5000 ms.
   - Once the probe succeeds (receives ACK), health state transitions back to `BACNET_HEALTH_ONLINE`, `consecutive_timeouts` resets to 0, and normal queue operations resume.

---

### 3.4 Decoupled Caller APIs

#### Synchronous Typed Helpers (Drop-in Replacement for Existing Callers)

```c
/* Synchronously read a float REAL property (checks cache first; falls back to worker queue) */
bool bacnet_worker_read_real(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float *out_val);

/* Synchronously read a bool ENUMERATED property */
bool bacnet_worker_read_bool(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool *out_val);

/* Synchronously read an unsigned int / MSV property */
bool bacnet_worker_read_msv(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned *out_val);

/* Synchronously write a REAL property (High Priority; invalidates cache entry) */
bool bacnet_worker_write_real(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float val);

/* Synchronously write a bool ENUMERATED property (High Priority; invalidates cache entry) */
bool bacnet_worker_write_bool(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool val);

/* Synchronously write an MSV property (High Priority; invalidates cache entry) */
bool bacnet_worker_write_msv(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned val);

/* Generic synchronous explorer read / write for /api/bacnet/read and /api/bacnet/write */
bacnet_status_t bacnet_worker_explorer_read_sync(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, uint32_t index, bacnet_response_t *resp, uint32_t timeout_ms);
bacnet_status_t bacnet_worker_explorer_write_sync(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, uint32_t index, uint8_t priority, const BACNET_APPLICATION_DATA_VALUE *val, uint32_t timeout_ms);
```

#### Synchronous Caller Execution Pattern
Inside `bacnet_worker_read_real()`:
```c
bool bacnet_worker_read_real(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float *out_val)
{
    /* 1. Cache hit check */
    if (bacnet_cache_get(type, instance, prop, BC_KIND_REAL, out_val)) {
        return true;
    }
    /* 2. Setup sync request */
    bacnet_response_t resp = {0};
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) return false;

    bacnet_request_t req = {
        .op_type = BACNET_OP_READ_PROPERTY,
        .priority = BACNET_PRIO_NORMAL,
        .device_id = TargetDeviceInstance,
        .object_type = type,
        .object_instance = instance,
        .property_id = prop,
        .array_index = BACNET_ARRAY_ALL,
        .is_sync = true,
        .sync_sem = done_sem,
        .sync_resp = &resp,
        .timeout_ms = 600
    };

    if (xQueueSend(s_normal_prio_queue, &req, 0) != pdTRUE) {
        vSemaphoreDelete(done_sem);
        return false;
    }

    /* Wait for worker to finish transaction */
    bool ok = (xSemaphoreTake(done_sem, pdMS_TO_TICKS(1000)) == pdTRUE) && (resp.status == BACNET_STATUS_OK);
    vSemaphoreDelete(done_sem);

    if (ok && resp.tag == BACNET_APPLICATION_TAG_REAL) {
        *out_val = resp.val.real_val;
        bacnet_cache_put(type, instance, prop, BC_KIND_REAL, out_val);
        return true;
    }
    return false;
}
```

---

## 4. Caveats

1. **Physical Flashing Restriction**:
   - Per project instructions, no physical flashing to live microcontrollers was executed during this session.
2. **Object Catalog JSON Invariance**:
   - `bacnet-object-catalog.json` was kept strictly untouched.
3. **Legacy W5500 Memory Constraint**:
   - On the legacy W5500 profile (ESP32-WROOM, 4MB flash, no PSRAM), `s_high_prio_queue` (8 items) and `s_normal_prio_queue` (24 items) consume only ~2.2 KB of RAM in static `.bss`. The worker stack (24 KB) replaces `bacnet_client_task`'s old 24 KB stack, while saving 32 KB across `mqtt_command_task` and `mqtt_state_task`. Thus, total SRAM consumption decreases significantly.
4. **Who-Is Discovery Duration**:
   - `/api/bacnet/discover` sweeps 32 IP addresses with a 2-second receive window. In the worker architecture, discovery is handled as a single composite job inside the worker task, ensuring no other transactions interleave during the sweep.

---

## 5. Conclusion & Actionable Roadmap

1. **Architecture Assessment**:
   - The current codebase suffers from distributed, multi-threaded BACnet socket polling and lack of serialization across 15 HTTP handlers, MQTT commands, a 50-point telemetry polling loop, and an object scanner.
   - The Bounded Single BACnet Worker Queue architecture fully resolves:
     - **Stack bloat**: Reduces caller task stacks by >40 KB.
     - **Socket contention**: Single socket owner in `bacnet_worker_task`.
     - **Priority inversion**: High-priority user writes (Web/MQTT/Matter) jump ahead of periodic polling.
     - **Unreachable target freeze**: Circuit breaker fast-fails dead-link telemetry within 0 ms.
2. **Component Separation**:
   - Implement `firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.c` and `bacnet_worker.h` (or within `hvac_core` / `main`).
   - Wire `hvac_core` semantic commands (`hvac_command_room_setpoint`, `hvac_command_room_power`, `hvac_command_system_power`, `boost_apply`) to use `bacnet_worker_*`.
   - Update `main.c` web handlers, `mqtt_command_task`, and `mqtt_state_task` to call the worker API.

---

## 6. Verification Method

### 6.1 Profile Build Verification
Run the standard dual-profile validation script to verify compile integrity:
```bash
source /Users/pierswingfield/esp/esp-idf/export.sh
./tools/validate_build_profiles.sh
```

### 6.2 Inspection Points for Implementer
- Verify that `MAX_TSM_TRANSACTIONS=1` remains respected by routing 100% of BACnet read/write calls through `s_high_prio_queue` / `s_normal_prio_queue`.
- Verify that no raw `Send_Read_Property_Request`, `Send_Write_Property_Request`, or `bip_receive` calls remain in `httpd` handlers, `mqtt_command_task`, or `mqtt_state_task`.
- Check task stack reductions in `main.c`: `mqtt_command` (24K -> 4K), `mqtt_state` (16K -> 4K).
- Validate that `git status` confirms `bacnet-object-catalog.json` remains completely clean and untouched.
