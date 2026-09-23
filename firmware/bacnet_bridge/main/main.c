/* Merged firmware: dual-interface bring-up per requirements.md's network
 * model (S1) - Ethernet/W5500 for BACnet/IP only, WiFi for everything else
 * (config portal, and eventually MQTT/HA). Combines two previously separate,
 * independently-tested milestones:
 *   - firmware/bacnet_bridge (this project, pre-merge): BACnet/IP client
 *     proven against the real Delta panel - ReadProperty and WriteProperty
 *     both confirmed working.
 *   - firmware/wifi_provisioning: SoftAP + captive portal WiFi setup, NVS
 *     credential storage, station-mode retry, network scan - proven against
 *     a real home WiFi network.
 *
 * Both interfaces come up concurrently: Ethernet bring-up + the BACnet test
 * sequence run in their own task, while app_main() handles WiFi (bounded
 * duration either way - connects within ~25s or falls back to SoftAP
 * immediately) without waiting on Ethernet first.
 *
 * Once WiFi connects (or reconnects on a saved-credentials boot), the HTTP
 * server serves four post-connect pages instead of nothing: manage.html
 * (system power/Boost + per-room setpoint/power control), status.html
 * (WiFi/Ethernet/BACnet connection status, read-only, no BACnet transaction
 * needed), health.html (FCU performance/diagnostics - see below), and
 * reset.html (the "Reconfigure WiFi" action: clears NVS, reboots into the
 * SoftAP portal). Confirmed on real hardware: boots straight through
 * Ethernet+BACnet bring-up and WiFi STA reconnect (retried past the usual
 * transient WPA timeouts) to "Starting connected-mode dashboard server" with
 * no crash. A BacnetMutex serializes BACnet/IP transactions between the
 * boot-time milestone test and on-demand reads triggered by any of the
 * API handlers below, all of which run directly on the httpd task (given
 * a 24KB stack for exactly this reason - see start_connected_webserver()).
 *
 * health.html's point map (System-level cooling output/flow/valve-signal/
 * air-ΔT, per-room supply-air/required-vs-current thermal output, and the
 * alarm/valve-status pills) was fact-checked object-by-object against the
 * real 430-object EPICS dump (discovery/device_753016_epics.txt) before
 * being wired in - every object referenced is confirmed to exist with
 * matching name/units; none of it has been individually live-tested via
 * ReadProperty yet, and the alarm polarity (0=healthy) is inferred from the
 * dump's own active-text/inactive-text fields, not yet confirmed against a
 * real triggered fault.
 *
 * Still open (per requirements.md S4C / architecture-plan.md Phase 2 item
 * 6): BACnet target selection/discovery and a room-mapping editor -
 * deliberately deferred since rooms/points are still fixed in firmware
 * (Rooms[] in this file), not user-editable from the UI.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_netif.h"
#include "esp_attr.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "ethernet_init.h"
#include "hvac_core.h"
#include "matter_adapter.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "dns_server.h"
#include "mqtt_client.h"
#include "mdns.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "mbedtls/base64.h"

#if CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET
#define ETHERNET_HARDWARE_LABEL "RTL8201"
#define ETHERNET_RECOVERY_HINT "check GPIO0 clock, GPIO12 PHY power, and RJ45 link"
#define BUILD_BOARD_ID "t-eth-lite"
/* Live T-ETH-Lite BACnet read/write testing used 23,468 bytes of the HTTP
   task's 24,576-byte stack. Its PSRAM profile reserves internal memory for
   task stacks, so use that headroom rather than accepting a 1,108-byte margin. */
#define CONNECTED_HTTP_STACK_SIZE 32768
#else
#define ETHERNET_HARDWARE_LABEL "W5500"
#define ETHERNET_RECOVERY_HINT "check W5500 wiring and SPI"
#define BUILD_BOARD_ID "w5500"
#define CONNECTED_HTTP_STACK_SIZE 24576
#endif

#include "bacnet/bacdef.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacenum.h"
#include "bacnet/bactext.h"
#include "bacnet_worker.h"
#include "hvac_core.h"

/* Forward declared here (before every call site) so it's usable from
   anywhere in the file regardless of definition order. Ships a line to the
   serial console; see the "Diagnostics" block near app_main for the
   definition and history. */
static void diag_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Pause/resume the MQTT client across a WiFi outage. Forward declared here
   because sta_event_handler() runs long before MqttClient is defined. See the
   definitions next to mqtt_app_restart() for why this is disconnect()/
   reconnect() rather than stop()/start(). */
static void mqtt_pause_for_link_loss(void);
static void mqtt_resume_after_link_up(void);

/* mDNS .local advertisement. Declared here because /api/network reports the
   state and /api/device/mdns sets it, both of which appear well before the
   mDNS helpers themselves. Off by default - see NVS_KEY_MDNS_ENABLED. */
#define MDNS_HOSTNAME "esp-bacnet-bridge"
/* The actually-active hostname: MDNS_HOSTNAME until mdns_start_service()
 * finds that name already taken on the LAN, at which point it becomes
 * MDNS_HOSTNAME-1, -2, etc. Two bridges on one network should still both
 * get a working .local name instead of colliding on the fixed default. */
static EXT_RAM_BSS_ATTR char MdnsHostname[40];
static bool MdnsEnabled = false;
static bool MdnsRunning = false;
static void mdns_apply_setting(void);

/* ===================== Task registry & checked spawn =====================
   Every xTaskCreate() in this file used to ignore its return value. That is
   the single hardest class of bug this firmware has produced: a task stack
   needs CONTIGUOUS internal DRAM, so under fragmentation the create fails
   while total free heap still looks healthy, and the subsystem simply never
   starts. It cost weeks - BACnet appearing broken after a flash was diagnosed
   for a long time as a mysterious "flash quirk" when it was actually
   bacnet_client_task failing to spawn against a heap MQTT had just claimed.

   spawn_task() checks the result and logs the two numbers that explain it:
   free heap AND largest free block. It also records the task so
   /api/debug/stacks can report real headroom over HTTP - this board runs
   headless in a wall enclosure, so serial-only instrumentation is no
   instrumentation at all. */
#define TASK_REGISTRY_MAX 10
typedef struct {
    const char  *name;
    TaskHandle_t handle;
    uint32_t     requested;
} task_reg_entry_t;
static task_reg_entry_t TaskRegistry[TASK_REGISTRY_MAX];
static size_t TaskRegistryCount = 0;
static portMUX_TYPE TaskRegistryMux = portMUX_INITIALIZER_UNLOCKED;

static void task_registry_add(const char *name, TaskHandle_t h, uint32_t requested)
{
    taskENTER_CRITICAL(&TaskRegistryMux);
    if (TaskRegistryCount < TASK_REGISTRY_MAX) {
        TaskRegistry[TaskRegistryCount].name = name;
        TaskRegistry[TaskRegistryCount].handle = h;
        TaskRegistry[TaskRegistryCount].requested = requested;
        TaskRegistryCount++;
    }
    taskEXIT_CRITICAL(&TaskRegistryMux);
}

/* Any task that deletes itself MUST call this first. Querying a handle whose
   TCB has been freed is undefined behaviour, so a self-deleting task has to
   leave the registry before it goes. */
static void task_registry_remove_self(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&TaskRegistryMux);
    for (size_t i = 0; i < TaskRegistryCount; i++) {
        if (TaskRegistry[i].handle == me) {
            TaskRegistry[i] = TaskRegistry[TaskRegistryCount - 1];
            TaskRegistryCount--;
            break;
        }
    }
    taskEXIT_CRITICAL(&TaskRegistryMux);
}

static bool spawn_task(
    TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
    UBaseType_t prio, TaskHandle_t *out_handle)
{
    TaskHandle_t h = NULL;
    if (xTaskCreate(fn, name, stack_bytes, arg, prio, &h) == pdPASS) {
        task_registry_add(name, h, stack_bytes);
        if (out_handle) { *out_handle = h; }
        return true;
    }
    ESP_LOGE("MAIN",
             "xTaskCreate(\"%s\", %u bytes) FAILED - free heap %u B, largest block %u B",
             name, (unsigned)stack_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    diag_log("task create FAILED: %s (%u bytes)", name, (unsigned)stack_bytes);
    if (out_handle) { *out_handle = NULL; }
    return false;
}

/* Connected-mode HTTPD deliberately uses a PSRAM stack for its large BACnet
 * handlers. Flash cache is disabled during NVS and OTA writes, so those writes
 * must never run on that task. Submit short persistence operations here; they
 * execute synchronously on a temporary internal-RAM stack and propagate their
 * result back to the caller. */
typedef bool (*internal_flash_op_t)(void *arg);
typedef struct {
    internal_flash_op_t op;
    void *arg;
    SemaphoreHandle_t finished;
    bool ok;
} internal_flash_request_t;

static void internal_flash_worker(void *arg)
{
    internal_flash_request_t *request = (internal_flash_request_t *)arg;
    request->ok = request->op(request->arg);
    xSemaphoreGive(request->finished);
    vTaskDelete(NULL);
}

static bool run_internal_flash_op(internal_flash_op_t op, void *arg)
{
    internal_flash_request_t *request = heap_caps_calloc(
        1, sizeof(*request), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!request) return false;
    request->finished = xSemaphoreCreateBinary();
    if (!request->finished) {
        heap_caps_free(request);
        return false;
    }
    request->op = op;
    request->arg = arg;
    if (xTaskCreatePinnedToCore(internal_flash_worker, "flash_store", 4096,
                                request, 6, NULL, 0) != pdPASS) {
        vSemaphoreDelete(request->finished);
        heap_caps_free(request);
        ESP_LOGE("MAIN", "internal flash worker could not start");
        return false;
    }
    xSemaphoreTake(request->finished, portMAX_DELAY);
    bool ok = request->ok;
    vSemaphoreDelete(request->finished);
    heap_caps_free(request);
    return ok;
}

/* ===================== In-Memory Web Log Buffer ===================== */
/* Halved from 16384 on 2026-08-25. This is a plain static ring buffer for
   /api/logs; 8K still holds a useful scrollback and returns 8K of DRAM to a
   device measured at 3-12K free heap once BACnet, WiFi, MQTT and the HTTP
   server are all running. */
#define LOG_BUFFER_SIZE 8192
static char LogBuffer[LOG_BUFFER_SIZE];
static size_t LogHead = 0;
static size_t LogCount = 0;
static portMUX_TYPE LogMux = portMUX_INITIALIZER_UNLOCKED;

static int web_log_vprintf(const char *fmt, va_list args)
{
    char temp[256];
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(temp, sizeof(temp), fmt, copy);
    va_end(copy);

    if (len > 0) {
        taskENTER_CRITICAL(&LogMux);
        for (int i = 0; i < len; i++) {
            LogBuffer[LogHead] = temp[i];
            LogHead = (LogHead + 1) % LOG_BUFFER_SIZE;
            if (LogCount < LOG_BUFFER_SIZE) {
                LogCount++;
            }
        }
        taskEXIT_CRITICAL(&LogMux);
    }

    return vprintf(fmt, args);
}

/* ===================== BACnet/Ethernet side ===================== */

#define DEFAULT_TARGET_DEVICE_INSTANCE 753016
#define DEFAULT_TARGET_IP "10.0.3.16"
#define DEFAULT_TARGET_PORT 47808

static uint32_t TargetDeviceInstance = DEFAULT_TARGET_DEVICE_INSTANCE;
static char TargetIp[16] = DEFAULT_TARGET_IP;
static uint16_t TargetPort = DEFAULT_TARGET_PORT;

/* This device's static IP on the isolated BACnet segment (Phase 1). */
#define LOCAL_STATIC_IP "10.0.3.99"
#define LOCAL_NETMASK "255.255.0.0"
#define LOCAL_GATEWAY "0.0.0.0"

#define SYS_POWER_WRITE_INSTANCE 13 /* `BMS Run Signal` (binary-value) - confirmed write target */
#define SYS_POWER_READBACK_INSTANCE 1 /* `FCU Run Status` (binary-value) - read-only, can diverge from BMS Run Signal */
#define BOOST_INSTANCE 1 /* `FCU Operating Mode` (multi-state-value) - confirmed via Phase 0.5 diff */
#define BOOST_MODE_AUTO 1
#define BOOST_MODE_FULL_HEATING 4
#define BOOST_MODE_FULL_COOLING 5

#define BOOST_TIMEOUT_DEFAULT_MIN 60
#define BOOST_TIMEOUT_MAX_MIN 240

static uint16_t BoostTimeoutMinutes = BOOST_TIMEOUT_DEFAULT_MIN;
static bool BoostRevertExternal = false;
static bool BoostSelfInitiated = false;
static int64_t BoostDeadlineUs = 0;

static bool boost_apply(unsigned mode, bool self_initiated);
static bool app_config_save(void);

static bool matter_system_power_write(bool on);
static bool matter_system_power_read(bool *out_on);
static bool matter_boost_write(matter_boost_target_t target, bool on);
static bool matter_boost_read(matter_system_mode_t *out_mode);

static int boost_remaining_minutes(void)
{
    if (BoostDeadlineUs == 0) {
        return 0;
    }
    int64_t remaining_us = BoostDeadlineUs - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    return (int)((remaining_us + 59999999) / 60000000);
}

#define MIN_SETPOINT_C 18.0f

typedef hvac_room_config_t room_config_t;
#define MAX_ROOMS HVAC_CORE_MAX_ROOMS
#define Rooms HvacRooms
#define RoomCount HvacRoomCount

#define NVS_TARGET_NAMESPACE "nvs_target"
#define NVS_ROOMS_NAMESPACE "nvs_rooms"

static char TargetDeviceName[MAX_CHARACTER_STRING_BYTES + 1] = {0};
static volatile bool TargetDeviceNameValid = false;

static void target_config_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_TARGET_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t ip_len = sizeof(TargetIp);
    nvs_get_str(handle, "ip", TargetIp, &ip_len);
    nvs_get_u16(handle, "port", &TargetPort);
    nvs_get_u32(handle, "dev_id", &TargetDeviceInstance);
    /* Discovery confirms the target's self-reported name once, interactively
     * (see api_bacnet_discover_handler); without persisting it, the status
     * page reverted to "Not confirmed yet" on every reboot even though the
     * target itself (ip/port/dev_id above) was correctly configured and
     * responding the whole time. */
    size_t name_len = sizeof(TargetDeviceName);
    if (nvs_get_str(handle, "name", TargetDeviceName, &name_len) == ESP_OK && TargetDeviceName[0] != '\0') {
        TargetDeviceNameValid = true;
    }
    nvs_close(handle);
    bacnet_worker_set_target(TargetDeviceInstance, TargetIp, TargetPort);
}

static bool target_config_save_direct(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open(NVS_TARGET_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(handle, "ip", TargetIp);
    if (err == ESP_OK) err = nvs_set_u16(handle, "port", TargetPort);
    if (err == ESP_OK) err = nvs_set_u32(handle, "dev_id", TargetDeviceInstance);
    if (err == ESP_OK && TargetDeviceNameValid) {
        err = nvs_set_str(handle, "name", TargetDeviceName);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    bacnet_worker_set_target(TargetDeviceInstance, TargetIp, TargetPort);
    return true;
}

static bool target_config_save(void)
{
    return run_internal_flash_op(target_config_save_direct, NULL);
}

static void rooms_config_load(void)
{
    hvac_core_rooms_load();
}

static bool rooms_config_save_direct(void *arg)
{
    (void)arg;
    hvac_core_rooms_save();
    return true;
}

static bool rooms_config_save(void)
{
    return run_internal_flash_op(rooms_config_save_direct, NULL);
}

typedef struct {
    hvac_integration_kind_t kind;
    bool result;
} integration_store_request_t;

static bool integration_store_direct(void *arg)
{
    integration_store_request_t *request = (integration_store_request_t *)arg;
    request->result = hvac_core_integration_set(request->kind);
    return request->result;
}

static bool integration_store(hvac_integration_kind_t kind)
{
    integration_store_request_t request = {.kind = kind, .result = false};
    return run_internal_flash_op(integration_store_direct, &request) && request.result;
}

typedef struct {
    hvac_pairing_status_t status;
    bool result;
} pairing_store_request_t;

static bool pairing_store_direct(void *arg)
{
    pairing_store_request_t *request = (pairing_store_request_t *)arg;
    request->result = hvac_core_matter_pairing_set(request->status);
    return request->result;
}

static bool pairing_store(hvac_pairing_status_t status)
{
    pairing_store_request_t request = {.status = status, .result = false};
    return run_internal_flash_op(pairing_store_direct, &request) && request.result;
}


/* Whole-unit health/performance objects, confirmed to exist with matching
   name/units against discovery/device_753016_epics.txt (not yet individually
   live-tested via ReadProperty - see architecture-plan.md). Naming keeps the
   "Cooling Valve"/"Fan N" qualifiers from the real object names rather than
   the generic "Flow"/"Supply Air" labels an earlier draft of this page used,
   since this unit also has separate heating-valve and per-fan objects that
   those generic names would collide with. */
/* Design (nameplate) max, NOT a live demand - useful as the reference the
   "required" figure gets pinned to when the unit is forced to full output.
   Confirmed on real hardware: reads 4.40 kW, identical to what
   `Overall Required Cooling Output` reports while Boost is in Full Cooling. */
#define HEALTH_DESIGN_COOLING_DUTY_INSTANCE 25 /* AV: "FCU Design Cooling Duty (Sensible)" (kW) */
#define HEALTH_COOLING_OUTPUT_INSTANCE 26   /* AV: "Overall Current Cooling Output" (kW) */
#define HEALTH_REQUIRED_OUTPUT_INSTANCE 27  /* AV: "Overall Required Cooling Output" (kW) */
#define HEALTH_COOLING_FLOW_INSTANCE 42     /* AV: "Cooling Valve Current Flow Rate" (L/s) */
#define HEALTH_REQUIRED_FLOW_INSTANCE 43    /* AV: "Cooling Valve Required Flow Rate" (L/s) */
#define HEALTH_FLOW_DESIGN_PCT_INSTANCE 45  /* AV: "Cooling Valve Percentage of Design Flow" (%), controller-computed */
#define HEALTH_VALVE_SIGNAL_INSTANCE 8      /* AO: "Cooling Valve Control Signal" (%) */

#define HEALTH_DESIGN_HEATING_DUTY_INSTANCE 20 /* AV: "FCU Design Heating Duty" (kW) */
#define HEALTH_HEATING_OUTPUT_INSTANCE 21      /* AV: "Overall Current Heating Output" (kW) */
#define HEALTH_REQUIRED_HEATING_OUTPUT_INSTANCE 22 /* AV: "Overall Required Heating Output" (kW) */
#define HEALTH_HEATING_FLOW_INSTANCE 32        /* AV: "Heating Valve Current Flow Rate" (L/s) */
#define HEALTH_REQUIRED_HEATING_FLOW_INSTANCE 33 /* AV: "Heating Valve Required Flow Rate" (L/s) */
#define HEALTH_HEATING_FLOW_DESIGN_PCT_INSTANCE 35 /* AV: "Heating Valve Percentage of Design Flow" (%) */
#define HEALTH_HEATING_VALVE_SIGNAL_INSTANCE 7 /* AO: "Heating Valve / Element 1 Control Signal" (%) */

#define HEALTH_RETURN_AIR_INSTANCE 9        /* AI: "Return Air Temperature Sensor" */
/* AI:1-5 are "Fan N Supply Air Temperature Sensor" - not every unit has all
   5 fans wired/enabled, so the health handler reads all 5 and averages
   whichever ones succeed rather than assuming a fixed count. */
#define HEALTH_FAN_SUPPLY_AIR_COUNT 5
static const uint32_t HealthFanSupplyAirInstances[HEALTH_FAN_SUPPLY_AIR_COUNT] = {1, 2, 3, 4, 5};

#define HEALTH_FAN_SPEED_COUNT 4
static const uint32_t HealthFanSpeedInstances[HEALTH_FAN_SPEED_COUNT] = {1, 2, 3, 4}; /* AO: 1..4 "Fan N Speed Signal" */

/* How many of the 5 fan channels are actually fitted. The controller knows:
   `Number of FCU Fans` reads 2.0 on this unit (one fan per room), and its
   own description is "Selects number of fan outputs to use...". Always read
   it rather than assuming 5 - fan count is exactly the kind of thing that
   differs between installations.

   This matters because a BACnet read SUCCEEDING says nothing about whether
   the sensor is physically fitted: this unit reports all 5 Fan Supply Air
   Temperature Sensors as readable with out-of-service FALSE and no
   reliability fault. Channels 4 and 5 sit at -40C (open-circuit
   thermistor), but channel 3 reads a perfectly plausible ~20.5C despite
   there being no third fan - so a plausibility range alone would NOT have
   caught it. The fan count is the authoritative filter; the range check
   below is only a secondary signal for a fitted-but-faulty sensor. */
#define HEALTH_FAN_COUNT_INSTANCE 50 /* AV: "Number of FCU Fans" */
#define PLAUSIBLE_TEMP_MIN_C (-10.0f)
#define PLAUSIBLE_TEMP_MAX_C 60.0f
static inline bool temp_is_plausible(float c)
{
    return c >= PLAUSIBLE_TEMP_MIN_C && c <= PLAUSIBLE_TEMP_MAX_C;
}

#define HEALTH_COOLING_VALVE_STATUS_INSTANCE 4 /* MSV: "Cooling Valve Status" - 1=CLOSED 2=CONTROLLING 3=RESYNCING */
#define HEALTH_HEATING_VALVE_STATUS_INSTANCE 3 /* MSV: "Heating Valve Status" - same state text */
static const char *HealthValveStatusNames[] = {"Unknown", "Closed", "Controlling", "Resyncing"};

/* Alarm binary-values: active-text/inactive-text confirmed "ON"/"OFF" in the
   EPICS dump, i.e. ACTIVE(1)=fault, INACTIVE(0)=healthy - not live-tripped
   yet (see architecture-plan.md), so treat with normal caution until a real
   fault has been observed and confirmed to actually set one of these. */
typedef struct {
    const char *label;
    uint32_t instance;
} health_alarm_t;
static const health_alarm_t HealthAlarms[] = {
    {"Master Alarm", 20},
    {"Filter Alarm", 22},
    {"Replace Filter Alarm", 30},
    {"Condensate Alarm", 23},
    {"Fan Failure Alarm", 24},
    {"Sensor Failure Alarm", 25},
    {"Element Cutout Alarm", 28},
};
#define HEALTH_ALARM_COUNT (sizeof(HealthAlarms) / sizeof(HealthAlarms[0]))

static const char *TAG_BAC = "bacnet_client";

static volatile bool EthConnected = false;
static esp_netif_t *EthNetif = NULL;

#define BacnetReady (bacnet_worker_is_ready())

static bacnet_discovered_dev_t DiscoveredDevices[MAX_DISCOVERED_DEVICES];
static size_t DiscoveredDeviceCount = 0;
static size_t DiscoveredDeviceSeenCount = 0;

static void eth_event_handler(
    void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG_BAC, "Ethernet Link Up");
            EthConnected = true;
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG_BAC, "Ethernet Link Down");
            EthConnected = false;
            break;
        default:
            break;
    }
}

/* Set once by eth_ip_event_handler if DHCP actually assigns an address;
 * read by the Ethernet bring-up sequence to decide whether to fall back to
 * the static config below. Not reset afterward - a mid-session DHCP lease
 * change/renewal is not this bridge's concern, only whether initial
 * bring-up got an address. */
static volatile bool EthGotDhcpIp = false;

static void eth_ip_event_handler(
    void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_BAC, "Ethernet DHCP assigned " IPSTR, IP2STR(&event->ip_info.ip));
        EthGotDhcpIp = true;
    }
}

static inline bool read_real_property(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, float *out_value)
{
    return bacnet_worker_read_real(object_type, object_instance, property, out_value);
}

static inline bool write_real_property(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, float value)
{
    return bacnet_worker_write_real(object_type, object_instance, property, value);
}

static inline bool read_bool_property(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, bool *out_value)
{
    return bacnet_worker_read_bool(object_type, object_instance, property, out_value);
}

static inline bool write_bool_property(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, bool value)
{
    return bacnet_worker_write_bool(object_type, object_instance, property, value);
}

static inline bool read_msv_property(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, unsigned *out_value)
{
    return bacnet_worker_read_msv(object_type, object_instance, property, out_value);
}

static inline bool write_msv_property(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, unsigned value)
{
    return bacnet_worker_write_msv(object_type, object_instance, property, value);
}

/* Matter's system endpoint is a plain OnOff switch: there is no home-level
 * temperature control or mode, only whether the plant is allowed to run.
 * Boost Heat/Boost Cool are BACnet-level plant commands (see boost_apply()
 * above) and are intentionally not reachable from Matter - only from the
 * web UI/API - so this never touches boost state. */
static bool matter_system_power_write(bool on)
{
    return hvac_core_set_system_power(on);
}

static bool matter_system_power_read(bool *out_on)
{
    if (!out_on) return false;
    return hvac_core_get_system_power_commanded(out_on);
}

/* Two OnOff switches ("Boost Heat"/"Boost Cool") over one tri-state BACnet
 * boost mode. Mutual exclusion lives here, not in matter_adapter: turning a
 * switch on selects that boost; turning the currently-active one off
 * returns to Auto (no boost). Turning off the inactive switch is a no-op -
 * matter_adapter never reports it as on in the first place, so a genuine
 * write there would mean a stale controller cache, not a real request. */
static bool matter_boost_write(matter_boost_target_t target, bool on)
{
    unsigned mode = on
        ? (target == MATTER_BOOST_TARGET_HEAT ? BOOST_MODE_FULL_HEATING : BOOST_MODE_FULL_COOLING)
        : BOOST_MODE_AUTO;
    return boost_apply(mode, true);
}

static bool matter_boost_read(matter_system_mode_t *out_mode)
{
    if (!out_mode) return false;
    unsigned boost_mode = BOOST_MODE_AUTO;
    if (BacnetReady && read_msv_property(OBJECT_MULTI_STATE_VALUE, BOOST_INSTANCE,
                                         PROP_PRESENT_VALUE, &boost_mode)) {
        if (boost_mode == BOOST_MODE_FULL_HEATING) {
            *out_mode = MATTER_SYSTEM_MODE_HEAT;
            return true;
        }
        if (boost_mode == BOOST_MODE_FULL_COOLING) {
            *out_mode = MATTER_SYSTEM_MODE_COOL;
            return true;
        }
    }
    *out_mode = MATTER_SYSTEM_MODE_AUTO;
    return true;
}

/* ===================== Protocol-neutral HVAC commands ===================== */
static inline bool hvac_command_room_setpoint(size_t room_idx, float requested, float *applied)
{
    return hvac_core_set_room_setpoint(room_idx, requested, applied);
}

static inline bool hvac_command_room_power(size_t room_idx, bool on)
{
    return hvac_core_set_room_power(room_idx, on);
}

static inline bool hvac_command_system_power(bool on)
{
    return hvac_core_set_system_power(on);
}


/* ---- Generic BACnet explorer (read/write any object/property) ----
   Backs /api/bacnet/read and /api/bacnet/write. Unlike the typed helpers
   above (one object/property pair each, wired into MQTT/HA control), this
   takes object type/instance/property as request parameters so ad hoc
   discovery/diagnosis can be done straight against this device over HTTP,
   replacing the old workflow of SSHing into a separate box to run a desktop
   BACnet CLI. Accepts both bactext names ("binary-value",
   "present-value", ...) and raw numeric IDs for type/property, matching
   what the bacnet-stack CLI tools already accept. */

static void json_escape(char *dst, const char *src, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t di = 0;
    while (*src && di + 2 < dst_len) {
        if (*src == '"' || *src == '\\') {
            dst[di++] = '\\';
        }
        if ((unsigned char)*src < 0x20) {
            src++;
            continue;
        }
        dst[di++] = *src++;
    }
    dst[di] = '\0';
}

static inline void copy_character_string(char *dst, size_t dst_size, const BACNET_CHARACTER_STRING *src)
{
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t len = characterstring_length(src);
    if (len >= dst_size) len = dst_size - 1;
    const char *s = characterstring_value_const(src);
    if (s && len > 0) {
        memcpy(dst, s, len);
    }
    dst[len] = '\0';
}

static size_t append_value_json(char *buf, size_t buf_len, const BACNET_APPLICATION_DATA_VALUE *v)
{
    if (!buf || buf_len == 0 || !v) return 0;
    switch (v->tag) {
        case BACNET_APPLICATION_TAG_NULL:
            return (size_t)snprintf(buf, buf_len, "null");
        case BACNET_APPLICATION_TAG_BOOLEAN:
            return (size_t)snprintf(buf, buf_len, v->type.Boolean ? "true" : "false");
        case BACNET_APPLICATION_TAG_UNSIGNED_INT:
            return (size_t)snprintf(buf, buf_len, "%lu", (unsigned long)v->type.Unsigned_Int);
        case BACNET_APPLICATION_TAG_SIGNED_INT:
            return (size_t)snprintf(buf, buf_len, "%ld", (long)v->type.Signed_Int);
        case BACNET_APPLICATION_TAG_REAL:
            return (size_t)snprintf(buf, buf_len, "%.3f", (double)v->type.Real);
        case BACNET_APPLICATION_TAG_DOUBLE:
            return (size_t)snprintf(buf, buf_len, "%.3f", v->type.Double);
        case BACNET_APPLICATION_TAG_ENUMERATED:
            return (size_t)snprintf(buf, buf_len, "%lu", (unsigned long)v->type.Enumerated);
        case BACNET_APPLICATION_TAG_OBJECT_ID:
            return (size_t)snprintf(
                buf, buf_len, "\"%s:%u\"",
                bactext_object_type_name_default(v->type.Object_Id.type, "?"),
                (unsigned)v->type.Object_Id.instance);
        case BACNET_APPLICATION_TAG_CHARACTER_STRING: {
            char tmp[64] = {0};
            copy_character_string(tmp, sizeof(tmp), (const BACNET_CHARACTER_STRING *)&v->type.Character_String);
            char escaped[80] = {0};
            json_escape(escaped, tmp, sizeof(escaped));
            return (size_t)snprintf(buf, buf_len, "\"%s\"", escaped);
        }
        default:
            return (size_t)snprintf(buf, buf_len, "null");
    }
}

/* Fills out/out_len with a JSON response either way (success or a
   {"ok":false,"error":...} body) - callers just send it back verbatim. */
static void explorer_read(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, uint32_t array_index, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!BacnetReady) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"bacnet not ready\"}");
        return;
    }

    bacnet_response_t resp = {0};
    bacnet_status_t status = bacnet_worker_explorer_read_sync(
        object_type, object_instance, property, array_index, &resp, 1000);

    if (status != BACNET_WORKER_STATUS_OK) {
        if (status == BACNET_WORKER_STATUS_TARGET_OFFLINE) {
            snprintf(out, out_len, "{\"ok\":false,\"error\":\"target offline\"}");
        } else if (status == BACNET_WORKER_STATUS_TIMEOUT) {
            snprintf(out, out_len, "{\"ok\":false,\"error\":\"read timeout\"}");
        } else {
            snprintf(out, out_len, "{\"ok\":false,\"error\":\"read failed (timeout or BACnet error - see device log)\"}");
        }
        return;
    }

    if (property == PROP_PRIORITY_ARRAY && array_index == BACNET_ARRAY_ALL) {
        size_t pos = (size_t)snprintf(out, out_len, "{\"ok\":true,\"tag\":\"priority-array\",\"values\":[");
        int offset = 0;
        int slot = 0;
        while (offset < resp.raw_data_len && slot < 16 && pos < out_len - 32) {
            BACNET_APPLICATION_DATA_VALUE elem = {0};
            int consumed = bacapp_decode_application_data(
                resp.raw_data + offset, (uint32_t)(resp.raw_data_len - offset), &elem);
            if (consumed <= 0) {
                break;
            }
            offset += consumed;
            if (slot > 0 && pos < out_len - 32) {
                pos += (size_t)snprintf(out + pos, out_len - pos, ",");
            }
            if (pos < out_len - 32) {
                pos += append_value_json(out + pos, out_len - pos, &elem);
            }
            slot++;
        }
        if (pos < out_len - 4) {
            snprintf(out + pos, out_len - pos, "]}");
        }
        return;
    }

    size_t pos = (size_t)snprintf(
        out, out_len, "{\"ok\":true,\"tag\":\"%s\",\"value\":",
        bactext_application_tag_name_default(resp.tag, "unknown"));
    if (pos < out_len - 32) {
        pos += append_value_json(out + pos, out_len - pos, &resp.app_val);
    }
    if (pos < out_len - 2) {
        snprintf(out + pos, out_len - pos, "}");
    }
}

/* value_tag selects how value_str is parsed/encoded: "real", "enum" (or
   "bool" - same tag, BACnet's ENUMERATED), "uint", or "null" (writes an
   actual BACNET_APPLICATION_TAG_NULL, e.g. to relinquish a priority-array
   slot by array index rather than change Present_Value). */
static void explorer_write(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, uint32_t array_index, uint8_t priority,
    const char *value_tag, const char *value_str, char *out, size_t out_len)
{
    if (!BacnetReady) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"bacnet not ready\"}");
        return;
    }
    BACNET_APPLICATION_DATA_VALUE v = {0};
    if (strcmp(value_tag, "real") == 0) {
        v.tag = BACNET_APPLICATION_TAG_REAL;
        v.type.Real = strtof(value_str, NULL);
    } else if (strcmp(value_tag, "enum") == 0 || strcmp(value_tag, "bool") == 0) {
        v.tag = BACNET_APPLICATION_TAG_ENUMERATED;
        v.type.Enumerated = (uint32_t)strtoul(value_str, NULL, 10);
    } else if (strcmp(value_tag, "uint") == 0) {
        v.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
        v.type.Unsigned_Int = (uint32_t)strtoul(value_str, NULL, 10);
    } else if (strcmp(value_tag, "null") == 0) {
        v.tag = BACNET_APPLICATION_TAG_NULL;
    } else {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"valuetag must be real|enum|bool|uint|null\"}");
        return;
    }

    bacnet_status_t status = bacnet_worker_explorer_write_sync(
        object_type, object_instance, property, array_index, priority, &v, 1000);

    if (status != BACNET_WORKER_STATUS_OK) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"write failed (timeout or BACnet error - see device log)\"}");
        return;
    }
    snprintf(out, out_len, "{\"ok\":true}");
}

static bool parse_object_type(const char *s, BACNET_OBJECT_TYPE *out)
{
    uint32_t idx;
    if (bactext_object_type_strtol(s, &idx)) {
        *out = (BACNET_OBJECT_TYPE)idx;
        return true;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (BACNET_OBJECT_TYPE)v;
    return true;
}

static bool parse_property_id(const char *s, BACNET_PROPERTY_ID *out)
{
    uint32_t idx;
    if (bactext_property_strtol(s, &idx)) {
        *out = (BACNET_PROPERTY_ID)idx;
        return true;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (BACNET_PROPERTY_ID)v;
    return true;
}



static volatile int PingSuccessCount = 0;
static volatile int PingTimeoutCount = 0;
static volatile bool PingDone = false;

static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    uint32_t elapsed_time;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    ESP_LOGI(TAG_BAC, "PING reply from %s: time=%ums", TargetIp, (unsigned)elapsed_time);
    PingSuccessCount++;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    ESP_LOGW(TAG_BAC, "PING timeout");
    PingTimeoutCount++;
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    PingDone = true;
    esp_ping_delete_session(hdl);
}

static void ping_test(void)
{
    ip_addr_t target;
    ip4addr_aton(TargetIp, (ip4_addr_t *)&target.u_addr.ip4);
    target.type = IPADDR_TYPE_V4;

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = 4;
    config.interval_ms = 500;
    config.timeout_ms = 2000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
        .cb_args = NULL,
    };

    esp_ping_handle_t ping;
    ESP_ERROR_CHECK(esp_ping_new_session(&config, &cbs, &ping));
    ESP_ERROR_CHECK(esp_ping_start(ping));

    ESP_LOGI(TAG_BAC, "Pinging %s to check IP-layer connectivity...", TargetIp);
    while (!PingDone) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG_BAC, "Ping result: %d success, %d timeout", PingSuccessCount, PingTimeoutCount);
}

/* Ethernet bring-up + BACnet test, in its own task so it doesn't block
   app_main() from getting WiFi up concurrently (see file header). */
static void eth_bringup_task(void *arg)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;

    /* Do NOT ESP_ERROR_CHECK this. An Ethernet controller that fails init must
       to abort() here, which boot-looped the entire bridge - WiFi, dashboard,
       MQTT and OTA all died because the Ethernet chip was unreachable. With the
       board in a wall enclosure that is unrecoverable without pulling it out.
       This was hit on W5500 hardware on 2026-08-24 after physical disturbance:
       "emac_w5500_init(826): verify chip ID failed" -> ~65 panics in 40s.
       Ethernet is not required for the device to be reachable or updatable, so
       either board profile degrades to WiFi-only instead of bricking the unit.
       BACnet simply never starts, BacnetReady stays false, and /api/status
       reports its fields invalid - which is the correct visible symptom. */
    esp_err_t eth_err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        eth_err = example_eth_init(&eth_handles, &eth_port_cnt);
        if (eth_err == ESP_OK && eth_port_cnt > 0) {
            break;
        }
        ESP_LOGE(TAG_BAC, "Ethernet/%s init failed (attempt %d/3): %s",
                 ETHERNET_HARDWARE_LABEL, attempt, esp_err_to_name(eth_err));
        diag_log("eth init failed attempt %d/3 - %s", attempt, ETHERNET_RECOVERY_HINT);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (eth_err != ESP_OK || eth_port_cnt == 0) {
        ESP_LOGE(TAG_BAC,
                 "Ethernet/%s unavailable - continuing WiFi-only, no BACnet; %s.",
                 ETHERNET_HARDWARE_LABEL, ETHERNET_RECOVERY_HINT);
        diag_log("eth unavailable - running WiFi-only, no BACnet");
        task_registry_remove_self();
    vTaskDelete(NULL);
        return;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    EthNetif = esp_netif_new(&cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handles[0]);
    ESP_ERROR_CHECK(esp_netif_attach(EthNetif, glue));

    /* Try DHCP first rather than assuming this specific static IP is right
     * for whatever segment this bridge actually ends up on - falls back to
     * the historical static config below only if nothing answers within a
     * few seconds. The isolated BACnet segment this bridge targets
     * typically runs no DHCP server (part of why it's isolated), so this
     * usually times out and costs a few seconds of boot; a differently
     * set up segment that does run DHCP now gets a correct address
     * instead of silently colliding with or missing this hardcoded one. */
    ESP_ERROR_CHECK(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));

    ESP_LOGI(TAG_BAC, "Waiting for Ethernet link...");
    while (!EthConnected) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG_BAC, "Link up - waiting up to 5s for DHCP...");
    for (int waited_ms = 0; waited_ms < 5000 && !EthGotDhcpIp; waited_ms += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (EthGotDhcpIp) {
        esp_netif_ip_info_t dhcp_info = {0};
        esp_netif_get_ip_info(EthNetif, &dhcp_info);
        char dhcp_ip_str[16];
        esp_ip4addr_ntoa(&dhcp_info.ip, dhcp_ip_str, sizeof(dhcp_ip_str));
        ESP_LOGI(TAG_BAC, "DHCP IP configured: %s", dhcp_ip_str);
        diag_log("eth DHCP assigned %s", dhcp_ip_str);
    } else {
        ESP_LOGI(TAG_BAC, "No DHCP response - falling back to static IP");
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(EthNetif));
        esp_netif_ip_info_t ip_info = {0};
        ip4addr_aton(LOCAL_STATIC_IP, (ip4_addr_t *)&ip_info.ip);
        ip4addr_aton(LOCAL_NETMASK, (ip4_addr_t *)&ip_info.netmask);
        ip4addr_aton(LOCAL_GATEWAY, (ip4_addr_t *)&ip_info.gw);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(EthNetif, &ip_info));
        ESP_LOGI(TAG_BAC, "Static IP configured: %s", LOCAL_STATIC_IP);
    }

    ping_test();
    /* 24KB stack: nested MAX_APDU=1476-sized buffers across
       bip_receive/rp_ack_decode/bacapp_decode overflowed an 8KB stack
       (see bacnet_client's README for the crash symptoms this fixed).

       Retry the create, and check its return. 24576 bytes of stack comes out
       of a heap the WiFi stack and MQTT client have usually already claimed:
       free heap was 19K at this point on 2026-08-24 and the create failed
       silently, leaving BACnet dead while the controller was fully reachable
       (ping 4/4, 2ms). This is the real cause of the long-standing "first boot
       after a flash shows broken BACnet reads for 60-200s, a second plain
       reset clears it" quirk documented in docs/LOGGING_TOOLS.md - it is a
       heap race against MQTT connecting, nothing to do with flashing, and the
       second reset merely reshuffles the timing. */    
    /* Start dedicated BACnet worker queue */
    esp_err_t worker_err = bacnet_worker_start(EthNetif, TargetDeviceInstance, TargetIp, TargetPort);
    if (worker_err != ESP_OK) {
        ESP_LOGE(TAG_BAC, "bacnet_worker_start failed - no BACnet this boot");
        diag_log("bacnet worker start failed - no BACnet this boot");
    } else {
        TaskHandle_t bacnet_handle = bacnet_worker_get_task_handle();
        if (bacnet_handle) {
            task_registry_add("bacnet_worker", bacnet_handle, BACNET_WORKER_STACK_SIZE);
        }
    }
    task_registry_remove_self();
    vTaskDelete(NULL);
}

/* ===================== WiFi provisioning side ===================== */

#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_MAX_STA_CONN CONFIG_ESP_MAX_STA_CONN

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define STA_CONNECT_TIMEOUT_MS 25000
#define STA_MAX_RETRIES 5

extern const char root_start[] asm("_binary_root_html_start");
extern const char root_end[] asm("_binary_root_html_end");
extern const char manage_start[] asm("_binary_manage_html_start");
extern const char manage_end[] asm("_binary_manage_html_end");
extern const char status_page_start[] asm("_binary_status_html_start");
extern const char status_page_end[] asm("_binary_status_html_end");
extern const char health_page_start[] asm("_binary_health_html_start");
extern const char health_page_end[] asm("_binary_health_html_end");
extern const char reset_page_start[] asm("_binary_reset_html_start");
extern const char reset_page_end[] asm("_binary_reset_html_end");
extern const char update_page_start[] asm("_binary_update_html_start");
extern const char update_page_end[] asm("_binary_update_html_end");
extern const char objects_page_start[] asm("_binary_objects_html_start");
extern const char objects_page_end[] asm("_binary_objects_html_end");
extern const char wizard_page_start[] asm("_binary_wizard_html_start");
extern const char wizard_page_end[] asm("_binary_wizard_html_end");
extern const char smart_home_page_start[] asm("_binary_smart_home_html_start");
extern const char smart_home_page_end[] asm("_binary_smart_home_html_end");

static const char *TAG_WIFI = "wifi_prov";
static EventGroupHandle_t WifiEventGroup;
#define STA_CONNECTED_BIT BIT0
#define STA_FAILED_BIT BIT1
static int StaRetryCount = 0;
static esp_netif_t *WifiNetif = NULL;
static esp_timer_handle_t WifiReconnectTimer = NULL;

/* Reconnect backoff step, separate from StaRetryCount (which pins at
   STA_MAX_RETRIES after the first storm and stops being informative). Reset on
   IP_EVENT_STA_GOT_IP. */
static int StaBackoffStep = 0;

/* Set on IP_EVENT_STA_GOT_IP, cleared on disconnect. StaEverHadIp gates the
   reboot backstop so a device that has never been provisioned cannot reboot
   itself out of the setup AP. */
static volatile bool StaHasIp = false;
static volatile bool StaEverHadIp = false;
static volatile int64_t StaLostIpSinceUs = 0;

/* Reboot if the station cannot regain an IP for this long. Last resort for a
   wedged WiFi driver - see wifi_reconnect_timer_cb(). */
#define STA_STALL_REBOOT_MS (10 * 60 * 1000)

static uint32_t sta_backoff_ms(void);

/* Exponential backoff with jitter: 1s, 2s, 4s, 8s, 15s, 15s...
   The flat 1s/2s retry this replaces produced roughly 200 association attempts
   during a single 6-minute outage (measured 2026-08-24). The AP answered that
   with repeated `timeout (27)` and 4-way-handshake failures - auth would
   succeed, association would not. Backing off gives each attempt room to
   complete. The jitter avoids re-synchronising with any periodic AP-side
   behaviour. 15s cap keeps recovery from a genuine one-second blip quick. */
static uint32_t sta_backoff_ms(void)
{
    uint32_t base = 1000u << (StaBackoffStep > 4 ? 4 : StaBackoffStep);
    if (base > 15000u) {
        base = 15000u;
    }
    if (StaBackoffStep < 4) {
        StaBackoffStep++;
    }
    return base + (esp_random() % 500u);
}

/* Self-sustaining reconnect. This MUST re-arm itself rather than relying on
   the disconnect event handler to do it.
   Failure this fixes (2026-08-25 00:42): esp_wifi_connect() was called once
   from here, produced no disconnect event, and because the timer was only ever
   armed from inside sta_event_handler() nothing ever rescheduled. The driver
   then went completely silent for 9h43m while the device stayed up, healthy
   and heap-clean, retrying MQTT into the void 3865 times. Before the
   2026-08-24 change, esp_wifi_disconnect() in the disconnect handler generated
   phantom events that happened to keep this loop alive - removing it took the
   accidental safety net with it. Now the loop stands on its own. */
static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (StaHasIp) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        diag_log("wifi connect call failed: %s", esp_err_to_name(err));
    }

    /* Last resort: if we have previously held an IP and have now been without
       one for STA_STALL_REBOOT_MS, the driver is wedged beyond what
       esp_wifi_connect() will fix. A reboot is recoverable; a silent 10-hour
       stall on a board sealed in a wall enclosure is not. */
    if (StaEverHadIp && StaLostIpSinceUs != 0) {
        int64_t down_ms = (esp_timer_get_time() - StaLostIpSinceUs) / 1000;
        if (down_ms > STA_STALL_REBOOT_MS) {
            diag_log("wifi stalled %llds with no IP - rebooting", (long long)(down_ms / 1000));
            esp_restart();
        }
    }

    if (WifiReconnectTimer) {
        esp_timer_start_once(WifiReconnectTimer, (uint64_t)sta_backoff_ms() * 1000);
    }
}

/* Gateway IP (set on STA_GOT_IP) and last time it answered a ping - fed by
   link_watchdog_task(), defined near app_main() once MqttBrokerHost exists.
   diag_log() is forward-declared near the top of the file (before every call
   site, including bacnet_client_task, which runs before this point). */
static char GwIp[16] = {0};
static volatile int64_t GwLastPingOkUs = 0;
static volatile bool LinkWatchdogReconnectPending = false;

/* Set once STA connects; read by the post-connect dashboard's /api/status. */
static char StaSsid[33] = {0};
static char StaIp[16] = {0};

/* Whether /api/wizard/finish has ever been called. Until it has, "/" redirects
 * to "/wizard" instead of the normal dashboard, so a freshly-provisioned
 * device lands on setup rather than looking like nothing happened. */
#define NVS_KEY_WIZARD_DONE "wizard_done"
static bool WizardCompleted = false;

static void wizard_completed_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t done = 0;
    if (nvs_get_u8(handle, NVS_KEY_WIZARD_DONE, &done) == ESP_OK) {
        WizardCompleted = done != 0;
    }
    nvs_close(handle);
}

static bool wizard_completed_save_direct(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u8(handle, NVS_KEY_WIZARD_DONE, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    WizardCompleted = true;
    return true;
}

static bool wizard_completed_save(void)
{
    return run_internal_flash_op(wizard_completed_save_direct, NULL);
}

static bool load_wifi_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err_ssid = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    esp_err_t err_pass = nvs_get_str(handle, NVS_KEY_PASS, pass, &pass_len);
    nvs_close(handle);

    if (err_ssid != ESP_OK) {
        return false;
    }
    if (err_pass == ESP_ERR_NVS_NOT_FOUND) {
        pass[0] = '\0';
    } else if (err_pass != ESP_OK) {
        return false;
    }
    return strlen(ssid) > 0;
}

static esp_err_t save_wifi_credentials_direct(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

typedef struct {
    const char *ssid;
    const char *pass;
    esp_err_t result;
} wifi_store_request_t;

static bool wifi_store_direct(void *arg)
{
    wifi_store_request_t *request = (wifi_store_request_t *)arg;
    request->result = save_wifi_credentials_direct(request->ssid, request->pass);
    return request->result == ESP_OK;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *pass)
{
    wifi_store_request_t request = {.ssid = ssid, .pass = pass, .result = ESP_FAIL};
    return run_internal_flash_op(wifi_store_direct, &request) ? request.result : ESP_FAIL;
}

static void sta_event_handler(
    void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        /* One log line, not two. This runs on sys_evt; the previous ESP_LOGW +
           diag_log() pair overflowed its stack on 2026-08-23 (the log line is
           truncated mid-print at "DIA"). diag_log already carries the reason,
           the retry count, uptime and free heap. */
        diag_log("wifi disconnected reason=%d retry=%d", event->reason, StaRetryCount + 1);

        /* Pause MQTT for the duration of the outage. Leaving the client
           retrying through a storm pinned ~16K of heap in stuck esp-tls
           sockets: free heap measured 3-4K during long storms, snapping back
           to 19K the instant the netif tore down. */
        if (StaHasIp) {
            StaHasIp = false;
            StaLostIpSinceUs = esp_timer_get_time();
        }
        mqtt_pause_for_link_loss();

        /* esp_wifi_disconnect() used to be called here. It generated a second,
           phantom disconnect event for every real one - 62x reason=205 against
           4x reason=4 in one 32-minute window (2026-08-24) - doubling event
           churn and making the retry counter meaningless. Removed. */
        if (StaRetryCount < STA_MAX_RETRIES) {
            StaRetryCount++;
        } else if (WifiEventGroup) {
            /* Initial boot provisioning check: give up and fall back to the AP */
            xEventGroupSetBits(WifiEventGroup, STA_FAILED_BIT);
            return;
        }
        /* Normal operation: retry indefinitely, backing off. */
        if (WifiReconnectTimer) {
            esp_timer_stop(WifiReconnectTimer);
            esp_timer_start_once(WifiReconnectTimer, (uint64_t)sta_backoff_ms() * 1000);
        } else {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_WIFI, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(StaIp, sizeof(StaIp), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(GwIp, sizeof(GwIp), IPSTR, IP2STR(&event->ip_info.gw));
        GwLastPingOkUs = esp_timer_get_time();
        LinkWatchdogReconnectPending = false;
        diag_log("wifi up ip=%s gw=%s", StaIp, GwIp);
        StaRetryCount = 0;
        StaBackoffStep = 0;
        StaHasIp = true;
        StaEverHadIp = true;
        StaLostIpSinceUs = 0;
        mqtt_resume_after_link_up();
        if (WifiNetif) {
            esp_netif_set_default_netif(WifiNetif);
        }
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (WifiReconnectTimer) {
            esp_timer_stop(WifiReconnectTimer);
        }
        if (WifiEventGroup) {
            xEventGroupSetBits(WifiEventGroup, STA_CONNECTED_BIT);
        }
    }
}

static bool try_connect_sta(const char *ssid, const char *pass)
{
    StaRetryCount = 0;
    strlcpy(StaSsid, ssid, sizeof(StaSsid));
    WifiEventGroup = xEventGroupCreate();
    if (!WifiNetif) {
        WifiNetif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    if (strlen(pass) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    if (!WifiReconnectTimer) {
        const esp_timer_create_args_t timer_args = {
            .callback = &wifi_reconnect_timer_cb,
            .name = "wifi_reconnect"
        };
        esp_timer_create(&timer_args, &WifiReconnectTimer);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Disable modem-sleep power save: the default (WIFI_PS_MIN_MODEM) drops unicast
       packets (pings, TCP SYN for web UI, ARP) on 802.11ax/DTIM APs. Device is mains powered. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* Reverted 2026-08-23: an 18dBm cap here (from an archived, untested comment about
       power-rail dips during handshake) correlated with the router's own log showing
       this client's RSSI decaying session over session (-62 -> -71 -> -93dBm) into
       "disassociated due to inactivity" - i.e. a link that's too weak to sustain, the
       opposite problem from what the cap was meant to fix. Left at the chip default
       (no explicit cap) until there's real evidence the default causes brownouts on
       this specific board. */

    ESP_LOGI(TAG_WIFI, "Connecting to saved SSID '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        WifiEventGroup, STA_CONNECTED_BIT | STA_FAILED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));

    if (bits & STA_CONNECTED_BIT) {
        vEventGroupDelete(WifiEventGroup);
        WifiEventGroup = NULL;
        return true;
    }

    ESP_LOGW(TAG_WIFI, "STA connect failed/timed out - falling back to provisioning AP");
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler);
    vEventGroupDelete(WifiEventGroup);
    WifiEventGroup = NULL;
    return false;
}

static void ap_event_handler(
    void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG_WIFI, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(
            TAG_WIFI, "station " MACSTR " leave, AID=%d, reason=%d", MAC2STR(event->mac),
            event->aid, event->reason);
    }
}

static void wifi_init_softap(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_event_handler, NULL));
    esp_netif_create_default_wifi_sta(); /* APSTA - lets the setup page scan for real networks */

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG_WIFI, "Set up softAP with IP: %s", ip_addr);
    ESP_LOGI(
        TAG_WIFI, "wifi_init_softap finished. SSID:'%s' password:'%s'", EXAMPLE_ESP_WIFI_SSID,
        EXAMPLE_ESP_WIFI_PASS);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const uint32_t root_len = root_end - root_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, root_start, root_len);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    uint16_t max_aps = 30;
    wifi_ap_record_t ap_records[30];

    wifi_scan_config_t scan_config = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&max_aps, ap_records);

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t off = 0;
    off += snprintf(buf + off, 4096 - off, "[");

    char seen_ssid[30][33];
    int seen_count = 0;
    bool first = true;
    for (int i = 0; i < max_aps && off < 4000; i++) {
        if (ap_records[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp((char *)ap_records[i].ssid, seen_ssid[j]) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (seen_count < 30) {
            strlcpy(seen_ssid[seen_count], (char *)ap_records[i].ssid, sizeof(seen_ssid[0]));
            seen_count++;
        }

        char escaped[96];
        json_escape(escaped, (char *)ap_records[i].ssid, sizeof(escaped));
        bool secure = ap_records[i].authmode != WIFI_AUTH_OPEN;
        off += snprintf(
            buf + off, 4096 - off, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
            first ? "" : ",", escaped, ap_records[i].rssi, secure ? "true" : "false");
        first = false;
    }
    off += snprintf(buf + off, 4096 - off, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    return ESP_OK;
}

static const httpd_uri_t scan_uri = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler};

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    while (*src && di + 1 < dst_len) {
        if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static bool form_extract(const char *body, const char *key, char *out, size_t out_len)
{
    char needle[16];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *start = strstr(body, needle);
    if (!start) {
        return false;
    }
    start += strlen(needle);
    const char *end = strchr(start, '&');
    size_t raw_len = end ? (size_t)(end - start) : strlen(start);
    if (raw_len >= out_len * 3) {
        raw_len = out_len * 3 - 1;
    }
    /* Percent-encoded input can be up to 3x the decoded output length. This
     * buffer must cover the largest caller's out_len (api_objects_batch_handler's
     * points_str[1024]) or the copy below silently truncates before url_decode
     * ever runs - confirmed live: the default 17-point pin set encodes past the
     * old 196-byte cap and only the first 10 points were ever read. */
    char raw[3200];
    size_t copy_len = raw_len < sizeof(raw) - 1 ? raw_len : sizeof(raw) - 1;
    memcpy(raw, start, copy_len);
    raw[copy_len] = '\0';
    url_decode(out, raw, out_len);
    return true;
}

static const char *confirm_page =
    "<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:420px;margin:2em auto;'>"
    "<h1>Saved</h1>"
    "<p>Restarting and connecting to your network. This access point (ESP-BACnet-Setup) will "
    "disappear once it connects; if it can't connect within about 25 seconds, the access point "
    "will reappear so you can try again.</p>"
    "<p><strong>Once connected</strong>, on the same WiFi network open:</p>"
    "<p style='font-size:1.1em'><strong>http://esp-bacnet-bridge.local</strong></p>"
    "<p>in a browser. It will take you straight to the setup wizard to finish configuring "
    "BACnet and MQTT. If that address doesn't resolve (some routers/phones don't support "
    ".local names), check your router's connected-devices list for an IP address instead.</p>"
    "</body></html>";

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!form_extract(body, "ssid", ssid, sizeof(ssid)) || strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Missing SSID", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    form_extract(body, "password", pass, sizeof(pass));

    esp_err_t err = save_wifi_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Failed to save credentials: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_WIFI, "Saved credentials for SSID '%s', restarting in 3s...", ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, confirm_page, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

static const httpd_uri_t connect_uri = {
    .uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler};

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ===================== Post-connect pages ===================== */

static esp_err_t manage_get_handler(httpd_req_t *req)
{
    if (!WizardCompleted) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wizard");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    const uint32_t len = manage_end - manage_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, manage_start, len);
    return ESP_OK;
}

static const httpd_uri_t manage_uri = {
    .uri = "/", .method = HTTP_GET, .handler = manage_get_handler};

static esp_err_t status_page_get_handler(httpd_req_t *req)
{
    const uint32_t len = status_page_end - status_page_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, status_page_start, len);
    return ESP_OK;
}

static const httpd_uri_t status_page_uri = {
    .uri = "/status", .method = HTTP_GET, .handler = status_page_get_handler};

static esp_err_t health_page_get_handler(httpd_req_t *req)
{
    const uint32_t len = health_page_end - health_page_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, health_page_start, len);
    return ESP_OK;
}

static const httpd_uri_t health_page_uri = {
    .uri = "/health", .method = HTTP_GET, .handler = health_page_get_handler};

static esp_err_t reset_page_get_handler(httpd_req_t *req)
{
    const uint32_t len = reset_page_end - reset_page_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, reset_page_start, len);
    return ESP_OK;
}

static const httpd_uri_t reset_page_uri = {
    .uri = "/reset", .method = HTTP_GET, .handler = reset_page_get_handler};

/* Original ESP32 (unlike S2/S3/C3) has no ADC channel wired to its own
   supply rail, so there's no real voltage reading available without adding
   an external divider into a spare GPIO - a hardware change, not a
   firmware one. The brownout detector is the one power-health signal
   available for free: it resets the chip below ~2.43V, and the reason for
   the MOST RECENT boot (not a live reading) is queryable via
   esp_reset_reason(). A device that keeps coming back with "Brownout" or
   "Power glitch" here has a supply problem worth chasing down. */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:    return "Power-on";
        case ESP_RST_SW:         return "Software restart";
        case ESP_RST_PANIC:      return "Crash (panic)";
        case ESP_RST_INT_WDT:    return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:   return "Task watchdog";
        case ESP_RST_WDT:        return "Other watchdog";
        case ESP_RST_DEEPSLEEP:  return "Woke from deep sleep";
        case ESP_RST_BROWNOUT:   return "Brownout (supply sagged)";
        case ESP_RST_SDIO:       return "SDIO";
        case ESP_RST_USB:        return "USB";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "eFuse error";
        case ESP_RST_PWR_GLITCH: return "Power glitch";
        case ESP_RST_CPU_LOCKUP: return "CPU lockup";
        default:                 return "Unknown";
    }
}

/* Forward-declared: defined alongside the rest of the OTA-password logic
   further down, but api_network_get_handler (used by the setup wizard's
   Step 1 to show whether a password still needs to be chosen) runs first
   in the file. */
static bool ota_password_is_set(void);

/* Fast/cheap - no BACnet transaction, just cached connection state, so this
   stays responsive even if the BACnet link/target is unreachable. */
static esp_err_t api_network_get_handler(httpd_req_t *req)
{
    char target_name[MAX_CHARACTER_STRING_BYTES + 1] = {0};
    bool target_name_valid = TargetDeviceNameValid;
    if (target_name_valid) {
        strlcpy(target_name, TargetDeviceName, sizeof(target_name));
    }
    char name_escaped[96] = {0};
    char name_json[100] = "null";
    if (target_name_valid) {
        json_escape(name_escaped, target_name, sizeof(name_escaped));
        snprintf(name_json, sizeof(name_json), "\"%s\"", name_escaped);
    }

    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    const char *reset_reason = reset_reason_name(esp_reset_reason());
    bool reset_is_power_issue =
        esp_reset_reason() == ESP_RST_BROWNOUT || esp_reset_reason() == ESP_RST_PWR_GLITCH;

    char rssi_json[16] = "null";
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(rssi_json, sizeof(rssi_json), "%d", ap_info.rssi);
    }

    char buf[512];
    int off = snprintf(
        buf, sizeof(buf),
        "{\"wifi_ssid\":\"%s\",\"wifi_ip\":\"%s\",\"wifi_rssi\":%s,\"eth_connected\":%s,"
        "\"bacnet_target_name\":%s,\"bacnet_target_ip\":\"%s\","
        "\"uptime_seconds\":%lld,\"last_reset_reason\":\"%s\",\"last_reset_is_power_issue\":%s,"
        "\"ota_password_set\":%s,\"mdns_enabled\":%s,\"mdns_hostname\":\"%s.local\"}",
        StaSsid, StaIp, rssi_json, EthConnected ? "true" : "false", name_json, TargetIp,
        (long long)uptime_s, reset_reason, reset_is_power_issue ? "true" : "false",
        ota_password_is_set() ? "true" : "false",
        MdnsEnabled ? "true" : "false", MdnsHostname);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    return ESP_OK;
}

static const httpd_uri_t api_network_uri = {
    .uri = "/api/network", .method = HTTP_GET, .handler = api_network_get_handler};

static esp_err_t api_version_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char buf[160];
    int len = snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"project\":\"%s\",\"board\":\"%s\"}",
        app->version, app->project_name, BUILD_BOARD_ID);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}
static const httpd_uri_t api_version_uri = {
    .uri = "/api/version", .method = HTTP_GET, .handler = api_version_get_handler};

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WIFI, "HTTP dispatch GET /api/status; stack HWM=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    int64_t __req_start_us = esp_timer_get_time();
    diag_log("http GET /api/status start");
    if (!EthConnected) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"bacnet_available\":false,\"bacnet_reason\":\"Ethernet link down\",\"rooms\":[]}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    bool sys_power = false;
    bool sys_power_valid = BacnetReady && read_bool_property(
        OBJECT_BINARY_VALUE, SYS_POWER_READBACK_INSTANCE, PROP_PRESENT_VALUE, &sys_power);
    bool sys_power_commanded = false;
    bool sys_power_commanded_valid = BacnetReady &&
        hvac_core_get_system_power_commanded(&sys_power_commanded);

    unsigned boost_mode = 0;
    bool boost_valid = BacnetReady &&
        read_msv_property(OBJECT_MULTI_STATE_VALUE, BOOST_INSTANCE, PROP_PRESENT_VALUE, &boost_mode);

    char buf[1536];
    size_t off = 0;
    off += snprintf(
        buf + off, sizeof(buf) - off,
        "{\"sys_power_valid\":%s,\"sys_power\":%s,"
        "\"sys_power_commanded_valid\":%s,\"sys_power_commanded\":%s,"
        "\"boost_valid\":%s,\"boost_mode\":%u,"
        "\"boost_timeout_minutes\":%u,\"boost_remaining_minutes\":%d,"
        "\"integration_mode\":%d,"
        "\"rooms\":[",
        sys_power_valid ? "true" : "false", sys_power ? "true" : "false",
        sys_power_commanded_valid ? "true" : "false", sys_power_commanded ? "true" : "false",
        boost_valid ? "true" : "false", boost_mode,
        (unsigned)BoostTimeoutMinutes, boost_remaining_minutes(),
        (int)hvac_core_integration_get());

    bool first_room = true;
    for (size_t i = 0; i < RoomCount && off < sizeof(buf); i++) {
        const room_config_t *room = &Rooms[i];
        if (!room->active) {
            continue;
        }
        float setpoint = 0.0f;
        float temperature = 0.0f;
        bool power = false;
        bool setpoint_valid = BacnetReady && read_real_property(
            OBJECT_ANALOG_VALUE, room->setpoint_instance, PROP_PRESENT_VALUE, &setpoint);
        bool temperature_valid = BacnetReady && read_real_property(
            OBJECT_ANALOG_VALUE, room->temperature_instance, PROP_PRESENT_VALUE, &temperature);
        bool power_valid = BacnetReady && read_bool_property(
            OBJECT_BINARY_VALUE, room->power_instance, PROP_PRESENT_VALUE, &power);

        off += snprintf(
            buf + off, sizeof(buf) - off,
            "%s{\"id\":%u,\"name\":\"%s\",\"setpoint_valid\":%s,\"setpoint\":%.1f,"
            "\"temperature_valid\":%s,\"temperature\":%.1f,"
            "\"power_valid\":%s,\"power\":%s}",
            first_room ? "" : ",", (unsigned)i, room->name, setpoint_valid ? "true" : "false",
            setpoint_valid ? setpoint : 0.0f, temperature_valid ? "true" : "false",
            temperature_valid ? temperature : 0.0f, power_valid ? "true" : "false",
            power ? "true" : "false");
        first_room = false;
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    diag_log("http GET /api/status done %lldms stack_free=%uB",
        (long long)((esp_timer_get_time() - __req_start_us) / 1000),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ESP_OK;
}

static const httpd_uri_t api_status_uri = {
    .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler};

static esp_err_t api_health_get_handler(httpd_req_t *req)
{
    int64_t __req_start_us = esp_timer_get_time();
    diag_log("http GET /api/health start");
    /* BacnetReady says the datalink was initialised, not that the cable is
       presently connected. Avoid six-second read timeouts per property. */
    if (!EthConnected) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"bacnet_available\":false,\"bacnet_reason\":\"Ethernet link down\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    /* Cooling metrics */
    float cooling_output = 0.0f, required_cooling_output = 0.0f;
    bool cooling_output_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_COOLING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &cooling_output);
    bool required_cooling_output_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &required_cooling_output);

    float design_cooling_duty = 0.0f;
    bool design_cooling_duty_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_DESIGN_COOLING_DUTY_INSTANCE, PROP_PRESENT_VALUE,
        &design_cooling_duty);

    float cooling_flow = 0.0f, required_cooling_flow = 0.0f, cooling_flow_design_pct = 0.0f;
    bool cooling_flow_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_COOLING_FLOW_INSTANCE, PROP_PRESENT_VALUE, &cooling_flow);
    bool required_cooling_flow_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_FLOW_INSTANCE, PROP_PRESENT_VALUE, &required_cooling_flow);
    bool cooling_flow_design_pct_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_FLOW_DESIGN_PCT_INSTANCE, PROP_PRESENT_VALUE, &cooling_flow_design_pct);

    float cooling_valve_signal = 0.0f;
    bool cooling_valve_signal_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_OUTPUT, HEALTH_VALVE_SIGNAL_INSTANCE, PROP_PRESENT_VALUE, &cooling_valve_signal);

    /* Heating metrics */
    float heating_output = 0.0f, required_heating_output = 0.0f;
    bool heating_output_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_HEATING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &heating_output);
    bool required_heating_output_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_HEATING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &required_heating_output);

    float design_heating_duty = 0.0f;
    bool design_heating_duty_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_DESIGN_HEATING_DUTY_INSTANCE, PROP_PRESENT_VALUE,
        &design_heating_duty);

    float heating_flow = 0.0f, required_heating_flow = 0.0f, heating_flow_design_pct = 0.0f;
    bool heating_flow_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_HEATING_FLOW_INSTANCE, PROP_PRESENT_VALUE, &heating_flow);
    bool required_heating_flow_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_HEATING_FLOW_INSTANCE, PROP_PRESENT_VALUE, &required_heating_flow);
    bool heating_flow_design_pct_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_HEATING_FLOW_DESIGN_PCT_INSTANCE, PROP_PRESENT_VALUE, &heating_flow_design_pct);

    float heating_valve_signal = 0.0f;
    bool heating_valve_signal_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_OUTPUT, HEALTH_HEATING_VALVE_SIGNAL_INSTANCE, PROP_PRESENT_VALUE, &heating_valve_signal);

    float return_air = 0.0f;
    bool return_air_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_INPUT, HEALTH_RETURN_AIR_INSTANCE, PROP_PRESENT_VALUE, &return_air);

    float fan_count_raw = 0.0f;
    bool fan_count_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_FAN_COUNT_INSTANCE, PROP_PRESENT_VALUE, &fan_count_raw);
    int fan_count = 0;
    if (fan_count_valid) {
        fan_count = (int)(fan_count_raw + 0.5f);
        if (fan_count < 0) {
            fan_count = 0;
        }
        if (fan_count > HEALTH_FAN_SUPPLY_AIR_COUNT) {
            fan_count = HEALTH_FAN_SUPPLY_AIR_COUNT;
        }
    }

    float fan_supply[HEALTH_FAN_SUPPLY_AIR_COUNT];
    bool fan_supply_valid[HEALTH_FAN_SUPPLY_AIR_COUNT];
    for (int i = 0; i < HEALTH_FAN_SUPPLY_AIR_COUNT; i++) {
        fan_supply_valid[i] = BacnetReady && read_real_property(
            OBJECT_ANALOG_INPUT, HealthFanSupplyAirInstances[i], PROP_PRESENT_VALUE, &fan_supply[i]);
    }

    float fan_speeds[HEALTH_FAN_SPEED_COUNT];
    bool fan_speeds_valid[HEALTH_FAN_SPEED_COUNT];
    for (int i = 0; i < HEALTH_FAN_SPEED_COUNT; i++) {
        fan_speeds_valid[i] = BacnetReady && read_real_property(
            OBJECT_ANALOG_OUTPUT, HealthFanSpeedInstances[i], PROP_PRESENT_VALUE, &fan_speeds[i]);
    }

    float room_supply_air[MAX_ROOMS];
    bool room_supply_air_valid[MAX_ROOMS];
    float supply_air_sum = 0.0f;
    int supply_air_count = 0;
    for (size_t i = 0; i < RoomCount; i++) {
        room_supply_air_valid[i] = BacnetReady && read_real_property(
            OBJECT_ANALOG_VALUE, Rooms[i].supply_air_instance, PROP_PRESENT_VALUE, &room_supply_air[i]);
        if (Rooms[i].active && room_supply_air_valid[i] && temp_is_plausible(room_supply_air[i])) {
            supply_air_sum += room_supply_air[i];
            supply_air_count++;
        }
    }

    /* Determine active mode */
    const char *active_mode = "idle";
    float cool_dem = (required_cooling_output_valid && required_cooling_output > 0.05f) ? required_cooling_output : (cooling_output_valid ? cooling_output : 0.0f);
    float heat_dem = (required_heating_output_valid && required_heating_output > 0.05f) ? required_heating_output : (heating_output_valid ? heating_output : 0.0f);
    if (heat_dem > cool_dem && heat_dem > 0.05f) {
        active_mode = "heating";
    } else if (cool_dem > heat_dem && cool_dem > 0.05f) {
        active_mode = "cooling";
    } else if (heating_valve_signal_valid && heating_valve_signal > 5.0f && (!cooling_valve_signal_valid || heating_valve_signal > cooling_valve_signal)) {
        active_mode = "heating";
    } else if (cooling_valve_signal_valid && cooling_valve_signal > 5.0f) {
        active_mode = "cooling";
    }

    bool delta_t_valid =
        return_air_valid && temp_is_plausible(return_air) && supply_air_count > 0;
    float delta_t = 0.0f;
    if (delta_t_valid) {
        /* Signed convention (2026-08-23): negative = cooling, positive = warming,
           regardless of active_mode. Previously magnitude-only (always positive in
           whichever mode was active), which required knowing the mode to interpret
           the sign - see docs/SCOPE_2026-08-16_baseline-revert.md "Delta sign
           convention". HA's temperature_delta device class handles negative values. */
        float avg_sa = supply_air_sum / supply_air_count;
        delta_t = avg_sa - return_air;
    }

    unsigned cooling_valve_status = 0, heating_valve_status = 0;
    bool cooling_valve_status_valid = BacnetReady && read_msv_property(
        OBJECT_MULTI_STATE_VALUE, HEALTH_COOLING_VALVE_STATUS_INSTANCE, PROP_PRESENT_VALUE,
        &cooling_valve_status);
    bool heating_valve_status_valid = BacnetReady && read_msv_property(
        OBJECT_MULTI_STATE_VALUE, HEALTH_HEATING_VALVE_STATUS_INSTANCE, PROP_PRESENT_VALUE,
        &heating_valve_status);

    char buf[4096];
    size_t off = 0;
    off += snprintf(
        buf + off, sizeof(buf) - off,
        "{\"active_mode\":\"%s\","
        "\"cooling_output_valid\":%s,\"cooling_output\":%.2f,"
        "\"required_output_valid\":%s,\"required_output\":%.2f,"
        "\"cooling_flow_valid\":%s,\"cooling_flow\":%.4f,"
        "\"required_flow_valid\":%s,\"required_flow\":%.4f,"
        "\"flow_design_pct_valid\":%s,\"flow_design_pct\":%.1f,"
        "\"valve_signal_valid\":%s,\"valve_signal\":%.1f,"
        "\"design_cooling_duty_valid\":%s,\"design_cooling_duty\":%.2f,"
        "\"heating_output_valid\":%s,\"heating_output\":%.2f,"
        "\"required_heating_output_valid\":%s,\"required_heating_output\":%.2f,"
        "\"heating_flow_valid\":%s,\"heating_flow\":%.4f,"
        "\"required_heating_flow_valid\":%s,\"required_heating_flow\":%.4f,"
        "\"heating_flow_design_pct_valid\":%s,\"heating_flow_design_pct\":%.1f,"
        "\"heating_valve_signal_valid\":%s,\"heating_valve_signal\":%.1f,"
        "\"design_heating_duty_valid\":%s,\"design_heating_duty\":%.2f,"
        "\"delta_t_valid\":%s,\"delta_t\":%.2f,"
        "\"return_air_valid\":%s,\"return_air\":%.1f,"
        "\"cooling_valve_status\":\"%s\",\"heating_valve_status\":\"%s\",",
        active_mode,
        cooling_output_valid ? "true" : "false", cooling_output_valid ? cooling_output : 0.0f,
        required_cooling_output_valid ? "true" : "false", required_cooling_output_valid ? required_cooling_output : 0.0f,
        cooling_flow_valid ? "true" : "false", cooling_flow_valid ? cooling_flow : 0.0f,
        required_cooling_flow_valid ? "true" : "false", required_cooling_flow_valid ? required_cooling_flow : 0.0f,
        cooling_flow_design_pct_valid ? "true" : "false", cooling_flow_design_pct_valid ? cooling_flow_design_pct : 0.0f,
        cooling_valve_signal_valid ? "true" : "false", cooling_valve_signal_valid ? cooling_valve_signal : 0.0f,
        design_cooling_duty_valid ? "true" : "false", design_cooling_duty_valid ? design_cooling_duty : 0.0f,
        heating_output_valid ? "true" : "false", heating_output_valid ? heating_output : 0.0f,
        required_heating_output_valid ? "true" : "false", required_heating_output_valid ? required_heating_output : 0.0f,
        heating_flow_valid ? "true" : "false", heating_flow_valid ? heating_flow : 0.0f,
        required_heating_flow_valid ? "true" : "false", required_heating_flow_valid ? required_heating_flow : 0.0f,
        heating_flow_design_pct_valid ? "true" : "false", heating_flow_design_pct_valid ? heating_flow_design_pct : 0.0f,
        heating_valve_signal_valid ? "true" : "false", heating_valve_signal_valid ? heating_valve_signal : 0.0f,
        design_heating_duty_valid ? "true" : "false", design_heating_duty_valid ? design_heating_duty : 0.0f,
        delta_t_valid ? "true" : "false", delta_t_valid ? delta_t : 0.0f,
        return_air_valid ? "true" : "false", return_air_valid ? return_air : 0.0f,
        (cooling_valve_status_valid && cooling_valve_status < 4) ?
            HealthValveStatusNames[cooling_valve_status] : "Unavailable",
        (heating_valve_status_valid && heating_valve_status < 4) ?
            HealthValveStatusNames[heating_valve_status] : "Unavailable");

    off += snprintf(
        buf + off, sizeof(buf) - off,
        "\"fan_count_valid\":%s,\"fan_count\":%d,\"fan_channels_total\":%d,\"fan_supply_air\":[",
        fan_count_valid ? "true" : "false", fan_count, HEALTH_FAN_SUPPLY_AIR_COUNT);
    for (int i = 0; i < HEALTH_FAN_SUPPLY_AIR_COUNT && off < sizeof(buf); i++) {
        bool spd_valid = (i < HEALTH_FAN_SPEED_COUNT) && fan_speeds_valid[i];
        float spd_val = spd_valid ? fan_speeds[i] : 0.0f;
        off += snprintf(
            buf + off, sizeof(buf) - off,
            "%s{\"fan\":%u,\"valid\":%s,\"value\":%.1f,\"speed_valid\":%s,\"speed\":%.1f,\"plausible\":%s,\"configured\":%s}",
            i == 0 ? "" : ",", (unsigned)HealthFanSupplyAirInstances[i],
            fan_supply_valid[i] ? "true" : "false", fan_supply_valid[i] ? fan_supply[i] : 0.0f,
            spd_valid ? "true" : "false", spd_val,
            (fan_supply_valid[i] && temp_is_plausible(fan_supply[i])) ? "true" : "false",
            (fan_count_valid && i < fan_count) ? "true" : "false");
    }
    off += snprintf(buf + off, sizeof(buf) - off, "],\"rooms\":[");

    bool first_h_room = true;
    for (size_t i = 0; i < RoomCount && off < sizeof(buf); i++) {
        const room_config_t *room = &Rooms[i];
        if (!room->active) {
            continue;
        }
        float required = 0.0f, current = 0.0f;
        bool required_valid = BacnetReady && read_real_property(
            OBJECT_ANALOG_VALUE, room->required_output_instance, PROP_PRESENT_VALUE, &required);
        bool current_valid = BacnetReady && read_real_property(
            OBJECT_ANALOG_VALUE, room->current_output_instance, PROP_PRESENT_VALUE, &current);

        off += snprintf(
            buf + off, sizeof(buf) - off,
            "%s{\"name\":\"%s\",\"supply_air_valid\":%s,\"supply_air\":%.1f,"
            "\"required_output_valid\":%s,\"required_output\":%.2f,"
            "\"current_output_valid\":%s,\"current_output\":%.2f}",
            first_h_room ? "" : ",", room->name, room_supply_air_valid[i] ? "true" : "false",
            room_supply_air_valid[i] ? room_supply_air[i] : 0.0f, required_valid ? "true" : "false",
            required_valid ? required : 0.0f, current_valid ? "true" : "false",
            current_valid ? current : 0.0f);
        first_h_room = false;
    }
    off += snprintf(buf + off, sizeof(buf) - off, "],\"alarms\":[");

    for (size_t i = 0; i < HEALTH_ALARM_COUNT && off < sizeof(buf); i++) {
        bool active = false;
        bool valid = BacnetReady && read_bool_property(
            OBJECT_BINARY_VALUE, HealthAlarms[i].instance, PROP_PRESENT_VALUE, &active);
        off += snprintf(
            buf + off, sizeof(buf) - off, "%s{\"label\":\"%s\",\"valid\":%s,\"active\":%s}",
            i == 0 ? "" : ",", HealthAlarms[i].label, valid ? "true" : "false",
            active ? "true" : "false");
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    diag_log("http GET /api/health done %lldms stack_free=%uB",
        (long long)((esp_timer_get_time() - __req_start_us) / 1000),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ESP_OK;
}

static const httpd_uri_t api_health_uri = {
    .uri = "/api/health", .method = HTTP_GET, .handler = api_health_get_handler};

static bool recv_body(httpd_req_t *req, char *body, size_t body_len)
{
    int total = req->content_len < (int)body_len - 1 ? req->content_len : (int)body_len - 1;
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        return false;
    }
    body[received] = '\0';
    return true;
}

static void send_write_result(httpd_req_t *req, bool ok, const char *extra_json)
{
    httpd_resp_set_type(req, "application/json");
    if (!ok) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"write failed\"}", HTTPD_RESP_USE_STRLEN);
        return;
    }
    if (extra_json) {
        httpd_resp_send(req, extra_json, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
}

static void send_bad_request(httpd_req_t *req, const char *error_json)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_bacnet_read_get_handler(httpd_req_t *req)
{
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"need type, instance, property query params\"}");
        return ESP_OK;
    }
    char type_str[24] = {0}, instance_str[12] = {0}, prop_str[32] = {0}, index_str[12] = {0};
    if (!form_extract(query, "type", type_str, sizeof(type_str)) ||
        !form_extract(query, "instance", instance_str, sizeof(instance_str)) ||
        !form_extract(query, "property", prop_str, sizeof(prop_str))) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"need type, instance, property query params\"}");
        return ESP_OK;
    }
    BACNET_OBJECT_TYPE object_type;
    BACNET_PROPERTY_ID property;
    if (!parse_object_type(type_str, &object_type) || !parse_property_id(prop_str, &property)) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"unrecognized type or property name\"}");
        return ESP_OK;
    }
    uint32_t instance = (uint32_t)strtoul(instance_str, NULL, 10);
    uint32_t array_index = BACNET_ARRAY_ALL;
    if (form_extract(query, "index", index_str, sizeof(index_str)) && strlen(index_str) > 0) {
        array_index = (uint32_t)strtoul(index_str, NULL, 10);
    }

    char resp[1024];
    explorer_read(object_type, instance, property, array_index, resp, sizeof(resp));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t api_bacnet_read_uri = {
    .uri = "/api/bacnet/read", .method = HTTP_GET, .handler = api_bacnet_read_get_handler};

static esp_err_t api_bacnet_write_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char type_str[24] = {0}, instance_str[12] = {0}, prop_str[32] = {0};
    char index_str[12] = {0}, priority_str[8] = {0};
    char valuetag_str[8] = {0}, value_str[32] = {0};
    if (!form_extract(body, "type", type_str, sizeof(type_str)) ||
        !form_extract(body, "instance", instance_str, sizeof(instance_str)) ||
        !form_extract(body, "property", prop_str, sizeof(prop_str)) ||
        !form_extract(body, "valuetag", valuetag_str, sizeof(valuetag_str)) ||
        !form_extract(body, "value", value_str, sizeof(value_str))) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"need type, instance, property, valuetag, value\"}");
        return ESP_OK;
    }
    BACNET_OBJECT_TYPE object_type;
    BACNET_PROPERTY_ID property;
    if (!parse_object_type(type_str, &object_type) || !parse_property_id(prop_str, &property)) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"unrecognized type or property name\"}");
        return ESP_OK;
    }
    uint32_t instance = (uint32_t)strtoul(instance_str, NULL, 10);
    uint32_t array_index = BACNET_ARRAY_ALL;
    if (form_extract(body, "index", index_str, sizeof(index_str)) && strlen(index_str) > 0) {
        array_index = (uint32_t)strtoul(index_str, NULL, 10);
    }
    uint8_t priority = BACNET_NO_PRIORITY;
    if (form_extract(body, "priority", priority_str, sizeof(priority_str)) && strlen(priority_str) > 0) {
        unsigned long p = strtoul(priority_str, NULL, 10);
        if (p < BACNET_MIN_PRIORITY || p > BACNET_MAX_PRIORITY) {
            send_bad_request(req, "{\"ok\":false,\"error\":\"priority must be 1-16\"}");
            return ESP_OK;
        }
        priority = (uint8_t)p;
    }

    char resp[128];
    explorer_write(
        object_type, instance, property, array_index, priority, valuetag_str, value_str, resp,
        sizeof(resp));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t api_bacnet_write_uri = {
    .uri = "/api/bacnet/write", .method = HTTP_POST, .handler = api_bacnet_write_post_handler};

/* ===================== JSON & Helper Primitives ===================== */

static bool json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) return false;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p == '\"') {
        p++;
        size_t idx = 0;
        while (*p && *p != '\"' && idx + 1 < out_len) {
            if (*p == '\\' && *(p + 1)) {
                p++;
            }
            out[idx++] = *p++;
        }
        out[idx] = '\0';
        return true;
    }
    return false;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return false;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    char *end = NULL;
    long val = strtol(p, &end, 10);
    if (end != p) {
        *out = (int)val;
        return true;
    }
    return false;
}

static bool json_get_float(const char *json, const char *key, float *out)
{
    if (!json || !key || !out) return false;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p == '\"') p++;
    char *end = NULL;
    float val = strtof(p, &end);
    if (end != p) {
        *out = val;
        return true;
    }
    return false;
}

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) return false;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    } else if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_extract_obj(const char *json, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) return false;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p == '{') {
        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len >= out_len) len = out_len - 1;
        memcpy(out, start, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

static bool recv_full_body(httpd_req_t *req, char **out_body, size_t *out_len)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 16384) {
        return false;
    }
    char *buf = malloc(total_len + 1);
    if (!buf) {
        return false;
    }
    int cur = 0;
    while (cur < total_len) {
        int received = httpd_req_recv(req, buf + cur, total_len - cur);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return false;
        }
        cur += received;
    }
    buf[cur] = '\0';
    *out_body = buf;
    if (out_len) *out_len = (size_t)cur;
    return true;
}

/* Parses room index and value from either JSON body ({"room":0, "value":"..."})
   or form body ("room=0&value=..."). Validates against RoomCount. */
static bool parse_room_and_value(
    httpd_req_t *req, const char *body, int *out_room_idx, char *value_out, size_t value_out_len)
{
    int room_idx = -1;
    if (body && body[0] == '{') {
        int r = -1;
        if (json_get_int(body, "room", &r) || json_get_int(body, "id", &r)) {
            room_idx = r;
        }
        if (!json_get_str(body, "value", value_out, value_out_len)) {
            float fval = 0.0f;
            if (json_get_float(body, "value", &fval) || json_get_float(body, "setpoint", &fval)) {
                snprintf(value_out, value_out_len, "%.1f", fval);
            }
        }
    } else if (body) {
        char room_str[16] = {0};
        if (form_extract(body, "room", room_str, sizeof(room_str)) ||
            form_extract(body, "id", room_str, sizeof(room_str))) {
            room_idx = atoi(room_str);
        }
        form_extract(body, "value", value_out, value_out_len);
    }
    if (room_idx < 0 || room_idx >= (int)RoomCount || strlen(value_out) == 0) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"missing or invalid room/value\"}");
        return false;
    }
    *out_room_idx = room_idx;
    return true;
}

static esp_err_t api_room_setpoint_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int room_idx;
    char value_str[16] = {0};
    if (!parse_room_and_value(req, body, &room_idx, value_str, sizeof(value_str))) {
        return ESP_OK;
    }

    float value = strtof(value_str, NULL);
    bool ok = hvac_command_room_setpoint((size_t)room_idx, value, &value);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"setpoint\":%.1f}", value);
    send_write_result(req, ok, resp);
    return ESP_OK;
}

static const httpd_uri_t api_room_setpoint_uri = {
    .uri = "/api/room-setpoint", .method = HTTP_POST, .handler = api_room_setpoint_post_handler};

static esp_err_t api_room_power_post_handler(httpd_req_t *req)
{
    char body[64] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int room_idx;
    char value_str[8] = {0};
    if (!parse_room_and_value(req, body, &room_idx, value_str, sizeof(value_str))) {
        return ESP_OK;
    }
    if (strcmp(value_str, "on") != 0 && strcmp(value_str, "off") != 0) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"value must be on/off\"}");
        return ESP_OK;
    }

    bool on = strcmp(value_str, "on") == 0;
    bool ok = hvac_command_room_power((size_t)room_idx, on);
    diag_log("room-power room=%d value=%s ok=%d", room_idx, value_str, ok);
    send_write_result(req, ok, NULL);
    return ESP_OK;
}

static const httpd_uri_t api_room_power_uri = {
    .uri = "/api/room-power", .method = HTTP_POST, .handler = api_room_power_post_handler};

static esp_err_t api_system_power_post_handler(httpd_req_t *req)
{
    char body[32] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char value_str[8] = {0};
    form_extract(body, "value", value_str, sizeof(value_str));
    if (strcmp(value_str, "on") != 0 && strcmp(value_str, "off") != 0) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"value must be on/off\"}");
        return ESP_OK;
    }

    bool on = strcmp(value_str, "on") == 0;
    bool ok = hvac_command_system_power(on);
    diag_log("system-power value=%s ok=%d", value_str, ok);
    send_write_result(req, ok, NULL);
    return ESP_OK;
}

static const httpd_uri_t api_system_power_uri = {
    .uri = "/api/system-power", .method = HTTP_POST, .handler = api_system_power_post_handler};

static esp_err_t api_boost_post_handler(httpd_req_t *req)
{
    char body[32] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char value_str[8] = {0};
    form_extract(body, "value", value_str, sizeof(value_str));

    unsigned mode;
    if (strcmp(value_str, "auto") == 0) {
        mode = BOOST_MODE_AUTO;
    } else if (strcmp(value_str, "heat") == 0) {
        mode = BOOST_MODE_FULL_HEATING;
    } else if (strcmp(value_str, "cool") == 0) {
        mode = BOOST_MODE_FULL_COOLING;
    } else {
        send_bad_request(req, "{\"ok\":false,\"error\":\"value must be auto/heat/cool\"}");
        return ESP_OK;
    }

    bool ok = boost_apply(mode, true);
    send_write_result(req, ok, NULL);
    return ESP_OK;
}

static const httpd_uri_t api_boost_uri = {
    .uri = "/api/boost", .method = HTTP_POST, .handler = api_boost_post_handler};

static esp_err_t api_boost_timeout_post_handler(httpd_req_t *req)
{
    char body[32] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char value_str[8] = {0};
    form_extract(body, "value", value_str, sizeof(value_str));

    char *endptr = NULL;
    long minutes = strtol(value_str, &endptr, 10);
    if (endptr == value_str || minutes < 0 || minutes > BOOST_TIMEOUT_MAX_MIN) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"value must be 0-240\"}");
        return ESP_OK;
    }

    BoostTimeoutMinutes = (uint16_t)minutes;
    app_config_save();
    if (BoostSelfInitiated && BoostDeadlineUs != 0) {
        BoostDeadlineUs = BoostTimeoutMinutes > 0
            ? esp_timer_get_time() + (int64_t)BoostTimeoutMinutes * 60000000LL
            : 0;
    }
    send_write_result(req, true, NULL);
    return ESP_OK;
}

static const httpd_uri_t api_boost_timeout_uri = {
    .uri = "/api/boost-timeout", .method = HTTP_POST, .handler = api_boost_timeout_post_handler};

/* Toggle the mDNS .local advertisement. Off by default; exposed in the setup
   wizard and on the Update page. Applied immediately - no reboot needed. */
static esp_err_t api_device_mdns_post_handler(httpd_req_t *req)
{
    char body[48] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char value_str[12] = {0};
    form_extract(body, "value", value_str, sizeof(value_str));

    bool enable;
    if (strcasecmp(value_str, "on") == 0 || strcasecmp(value_str, "true") == 0 ||
        strcmp(value_str, "1") == 0) {
        enable = true;
    } else if (strcasecmp(value_str, "off") == 0 || strcasecmp(value_str, "false") == 0 ||
               strcmp(value_str, "0") == 0) {
        enable = false;
    } else {
        send_bad_request(req, "{\"ok\":false,\"error\":\"value must be on or off\"}");
        return ESP_OK;
    }

    MdnsEnabled = enable;
    app_config_save();
    mdns_apply_setting();

    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"mdns_enabled\":%s,\"hostname\":\"%s.local\"}",
             MdnsEnabled ? "true" : "false", MdnsHostname);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_device_mdns_uri = {
    .uri = "/api/device/mdns", .method = HTTP_POST, .handler = api_device_mdns_post_handler};

/* Stack headroom and heap, over HTTP.
   headroom_bytes = requested_stack - worst-ever-used, from
   uxTaskGetStackHighWaterMark(). A small number means that task came close to
   overflowing; the httpd stack once ran with 1,772 bytes LESS than it needed
   and crashed only under heavy requests, which is the failure this exists to
   make visible before it happens.
   Only currently-running tasks appear - self-deleting tasks (eth_bringup,
   obj_scan) leave the registry before they exit, because querying a freed TCB
   is undefined behaviour. "httpd" is measured directly: this handler runs on
   that task, so NULL means "me".
   largest_free_block is the number that predicts task-create failures, not
   free_heap - stacks need contiguous memory. */
static esp_err_t api_debug_stacks_get_handler(httpd_req_t *req)
{
    char buf[768];
    int off = snprintf(
        buf, sizeof(buf),
        "{\"free_heap\":%u,\"min_free_heap\":%u,\"largest_free_block\":%u,\"tasks\":[",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

    /* Snapshot the registry so the critical section stays short. */
    task_reg_entry_t snap[TASK_REGISTRY_MAX];
    size_t count;
    taskENTER_CRITICAL(&TaskRegistryMux);
    count = TaskRegistryCount;
    for (size_t i = 0; i < count; i++) {
        snap[i] = TaskRegistry[i];
    }
    taskEXIT_CRITICAL(&TaskRegistryMux);

    bool first = true;
    for (size_t i = 0; i < count && off < (int)sizeof(buf) - 96; i++) {
        unsigned head = (unsigned)uxTaskGetStackHighWaterMark(snap[i].handle);
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"name\":\"%s\",\"requested\":%u,\"headroom_bytes\":%u}",
                        first ? "" : ",", snap[i].name, (unsigned)snap[i].requested, head);
        first = false;
    }
    if (off < (int)sizeof(buf) - 96) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"name\":\"httpd\",\"requested\":%u,\"headroom_bytes\":%u}",
                        first ? "" : ",", (unsigned)CONNECTED_HTTP_STACK_SIZE,
                        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    return ESP_OK;
}
static const httpd_uri_t api_debug_stacks_uri = {
    .uri = "/api/debug/stacks", .method = HTTP_GET, .handler = api_debug_stacks_get_handler};

static bool wifi_credentials_erase_direct(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        esp_err_t err = nvs_erase_key(handle, NVS_KEY_SSID);
        if (err == ESP_OK) err = nvs_erase_key(handle, NVS_KEY_PASS);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        return err == ESP_OK;
    }
    return false;
}

static esp_err_t api_wifi_reset_post_handler(httpd_req_t *req)
{
    if (!run_internal_flash_op(wifi_credentials_erase_direct, NULL)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_WIFI, "WiFi credentials cleared via dashboard, restarting in 2s...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static const httpd_uri_t api_wifi_reset_uri = {
    .uri = "/api/wifi-reset", .method = HTTP_POST, .handler = api_wifi_reset_post_handler};

/* ===================== LAN firmware update (OTA) ===================== */
/* User-chosen admin password (not auto-generated - a device you can't ever
   ask "what did I set this to" over USB once it's in the wall is worse
   than one you set on purpose and are responsible for remembering).
   Stashed in NVS, never in source/git. Set from the setup wizard's Step 1
   (api_setup_password_post_handler below) - see the "before then, no auth"
   logic in check_ota_auth(): with no password chosen yet, /api/ota is wide
   open, exactly like a factory-reset router before you set its admin
   password. Losing/forgetting it isn't a USB-recovery problem anymore
   either: a factory reset (already wireless-reachable) wipes
   OTA_NVS_NAMESPACE, which drops this straight back to the open/unset
   state, ready for Step 1 to set a new one - no secret ever needs to
   round-trip back out over the network to recover from that. */
#define OTA_NVS_NAMESPACE "ota_cfg"
#define OTA_NVS_KEY_PASSWORD "password"
#define OTA_PASSWORD_MAX_LEN 64
#define OTA_PASSWORD_MIN_LEN 8
static char OtaPassword[OTA_PASSWORD_MAX_LEN + 1] = {0};

static bool ota_password_is_set(void)
{
    return OtaPassword[0] != '\0';
}

static void ota_password_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(OtaPassword);
        nvs_get_str(handle, OTA_NVS_KEY_PASSWORD, OtaPassword, &len);
        nvs_close(handle);
    }
    if (ota_password_is_set()) {
        ESP_LOGI(TAG_WIFI, "OTA update (/update) password: set (user: admin)");
    } else {
        ESP_LOGW(
            TAG_WIFI,
            "OTA update (/update) password: NOT SET - /api/ota is OPEN to anyone on the LAN "
            "until one is chosen in the setup wizard's Step 1");
    }
}

typedef struct {
    char password[OTA_PASSWORD_MAX_LEN + 1];
    SemaphoreHandle_t finished;
    bool ok;
} ota_password_store_request_t;

/* NVS commits write flash and therefore cannot execute on the PSRAM-backed
 * HTTPD stack. Keep this tiny persistence operation on an internal-RAM stack,
 * just as OTA image writes are handled below. */
static void ota_password_store_worker(void *arg)
{
    ota_password_store_request_t *request = (ota_password_store_request_t *)arg;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, OTA_NVS_KEY_PASSWORD, request->password);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    request->ok = err == ESP_OK;
    if (request->ok) {
        strlcpy(OtaPassword, request->password, sizeof(OtaPassword));
    } else {
        ESP_LOGE(TAG_WIFI, "OTA password NVS save failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(request->finished);
    vTaskDelete(NULL);
}

static bool ota_password_store(const char *pw)
{
    ota_password_store_request_t *request = heap_caps_calloc(
        1, sizeof(*request), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!request) {
        return false;
    }
    request->finished = xSemaphoreCreateBinary();
    if (!request->finished) {
        heap_caps_free(request);
        return false;
    }
    strlcpy(request->password, pw, sizeof(request->password));
    if (xTaskCreatePinnedToCore(ota_password_store_worker, "ota_pw_save", 4096,
                                request, 6, NULL, 0) != pdPASS) {
        vSemaphoreDelete(request->finished);
        heap_caps_free(request);
        ESP_LOGE(TAG_WIFI, "OTA password worker could not start");
        return false;
    }
    xSemaphoreTake(request->finished, portMAX_DELAY);
    bool ok = request->ok;
    vSemaphoreDelete(request->finished);
    heap_caps_free(request);
    return ok;
}

/* Constant-time-ish compare - avoids an early-exit strcmp() timing signal on
   an endpoint that flashes arbitrary firmware. Not that this device faces
   anything more sophisticated than curious housemates, but it's free. */
static bool secure_streq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"OTA Update\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
}

/* Expects "Authorization: Basic base64(admin:<OtaPassword>)". Returns false
   (and has already sent a 401) on any missing/malformed/wrong credential.
   No password chosen yet (fresh device, or just after a factory reset) ->
   open, so Step 1 of the setup wizard can set the first one without a
   chicken-and-egg "prove you know the password you're about to set". */
static bool check_ota_auth(httpd_req_t *req)
{
    if (!ota_password_is_set()) {
        return true;
    }

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0) {
        send_401(req);
        return false;
    }
    char *hdr = malloc(hdr_len + 1);
    if (!hdr) {
        httpd_resp_send_500(req);
        return false;
    }
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", hdr, hdr_len + 1);
    if (err != ESP_OK || strncmp(hdr, "Basic ", 6) != 0) {
        free(hdr);
        send_401(req);
        return false;
    }

    unsigned char decoded[96] = {0};
    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(
        decoded, sizeof(decoded) - 1, &decoded_len, (unsigned char *)hdr + 6, strlen(hdr + 6));
    free(hdr);
    if (rc != 0) {
        send_401(req);
        return false;
    }
    decoded[decoded_len] = '\0';

    char expected[16 + OTA_PASSWORD_MAX_LEN + 2];
    snprintf(expected, sizeof(expected), "admin:%s", OtaPassword);
    if (!secure_streq((char *)decoded, expected)) {
        send_401(req);
        return false;
    }
    return true;
}

#define OTA_CHUNK_BYTES 512
typedef struct {
    size_t len;
    bool final;
    bool abort;
    uint8_t data[OTA_CHUNK_BYTES];
} ota_chunk_t;

typedef struct {
    QueueHandle_t chunks;
    SemaphoreHandle_t finished;
    size_t image_len;
    esp_err_t result;
} ota_session_t;

/* The connected-mode HTTP task runs from PSRAM so its 32 KiB BACnet handler
 * stack fits. ESP-IDF deliberately forbids flash writes on a PSRAM stack,
 * because cache is disabled during the write. This worker therefore owns all
 * esp_ota_* calls and has a small, temporary internal-RAM stack; HTTPD only
 * receives and queues network data. */
static void ota_flash_worker(void *arg)
{
    ota_session_t *session = (ota_session_t *)arg;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t handle = 0;
    bool begun = false;
    bool failed = partition == NULL;
    esp_err_t err = partition ? esp_ota_begin(partition, session->image_len, &handle) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        begun = true;
        ESP_LOGI(TAG_WIFI, "OTA: writing %u bytes to partition '%s' at 0x%lx from internal worker",
                 (unsigned)session->image_len, partition->label, (unsigned long)partition->address);
    } else {
        failed = true;
        ESP_LOGE(TAG_WIFI, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
    }

    for (;;) {
        ota_chunk_t chunk;
        if (xQueueReceive(session->chunks, &chunk, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (chunk.abort) {
            failed = true;
        } else if (!chunk.final && !failed && esp_ota_write(handle, chunk.data, chunk.len) != ESP_OK) {
            failed = true;
            ESP_LOGE(TAG_WIFI, "OTA: esp_ota_write failed");
        }
        if (chunk.final) {
            break;
        }
    }

    if (begun && failed) {
        esp_ota_abort(handle);
    } else if (begun) {
        err = esp_ota_end(handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(partition);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG_WIFI, "OTA: image finalization failed: %s", esp_err_to_name(err));
        }
    }
    session->result = failed ? ESP_FAIL : err;
    xSemaphoreGive(session->finished);
    vTaskDelete(NULL);
}

static esp_err_t api_ota_post_handler(httpd_req_t *req)
{
    if (!check_ota_auth(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"empty body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ota_session_t *session = heap_caps_calloc(1, sizeof(*session), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!session) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    session->chunks = xQueueCreate(2, sizeof(ota_chunk_t));
    session->finished = xSemaphoreCreateBinary();
    session->image_len = req->content_len;
    if (!session->chunks || !session->finished ||
        xTaskCreatePinnedToCore(ota_flash_worker, "ota_flash", 4096, session, 6, NULL, 0) != pdPASS) {
        if (session->chunks) vQueueDelete(session->chunks);
        if (session->finished) vSemaphoreDelete(session->finished);
        heap_caps_free(session);
        ESP_LOGE(TAG_WIFI, "OTA: could not start internal flash worker");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t remaining = req->content_len;
    bool receive_failed = false;
    while (remaining > 0) {
        ota_chunk_t chunk = {0};
        int recv_len = httpd_req_recv(req, (char *)chunk.data,
                                      remaining < OTA_CHUNK_BYTES ? (int)remaining : OTA_CHUNK_BYTES);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            receive_failed = true;
            break;
        }
        chunk.len = (size_t)recv_len;
        if (xQueueSend(session->chunks, &chunk, pdMS_TO_TICKS(10000)) != pdTRUE) {
            receive_failed = true;
            break;
        }
        remaining -= chunk.len;
    }
    ota_chunk_t final_chunk = {.final = true, .abort = receive_failed};
    xQueueSend(session->chunks, &final_chunk, portMAX_DELAY);
    xSemaphoreTake(session->finished, portMAX_DELAY);

    esp_err_t result = session->result;
    vQueueDelete(session->chunks);
    vSemaphoreDelete(session->finished);
    heap_caps_free(session);
    if (receive_failed || result != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "OTA: transfer failed with %u bytes remaining", (unsigned)remaining);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"transfer failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGW(TAG_WIFI, "OTA: image written and validated, rebooting into it in 2s...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static const httpd_uri_t api_ota_uri = {
    .uri = "/api/ota", .method = HTTP_POST, .handler = api_ota_post_handler};

static esp_err_t update_page_get_handler(httpd_req_t *req)
{
    const uint32_t len = update_page_end - update_page_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, update_page_start, len);
    return ESP_OK;
}
static const httpd_uri_t update_page_uri = {
    .uri = "/update", .method = HTTP_GET, .handler = update_page_get_handler};

static esp_err_t objects_page_get_handler(httpd_req_t *req)
{
    const uint32_t len = objects_page_end - objects_page_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, objects_page_start, len);
    return ESP_OK;
}
static const httpd_uri_t objects_page_uri = {
    .uri = "/objects", .method = HTTP_GET, .handler = objects_page_get_handler};

static esp_err_t wizard_page_get_handler(httpd_req_t *req)
{
    const uint32_t len = wizard_page_end - wizard_page_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, wizard_page_start, len);
    return ESP_OK;
}
static const httpd_uri_t wizard_page_uri = {
    .uri = "/wizard", .method = HTTP_GET, .handler = wizard_page_get_handler};

static esp_err_t smart_home_page_get_handler(httpd_req_t *req)
{
    const uint32_t len = smart_home_page_end - smart_home_page_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, smart_home_page_start, len);
    return ESP_OK;
}
static const httpd_uri_t smart_home_page_uri = {
    .uri = "/smart-home", .method = HTTP_GET, .handler = smart_home_page_get_handler};



/* Step 1 of the setup wizard. Free-to-set while no password exists yet
   (check_ota_auth's open-until-set rule); once one's chosen, changing it
   needs the current one, same as /api/ota itself. Registered on both
   webservers since the wizard runs in both provisioning-AP and
   already-connected mode (re-running it to rotate the password later). */
static esp_err_t api_setup_password_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char pw[OTA_PASSWORD_MAX_LEN + 1] = {0};
    if (!json_get_str(body, "password", pw, sizeof(pw)) || strlen(pw) < OTA_PASSWORD_MIN_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(
            req, "{\"ok\":false,\"error\":\"password must be at least 8 characters\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (ota_password_is_set() && !check_ota_auth(req)) {
        return ESP_OK; /* check_ota_auth already sent the 401 */
    }

    if (!ota_password_store(pw)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG_WIFI, "OTA update password set via setup wizard Step 1");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t api_setup_password_uri = {
    .uri = "/api/setup-password", .method = HTTP_POST, .handler = api_setup_password_post_handler};

/* Forward declarations */
static void mqtt_publish_discovery(void);
static void mqtt_unpublish_discovery(void);
static void mqtt_app_restart(void);
static void mqtt_app_start(void);
static void mqtt_app_stop(void);
static void mqtt_state_task(void *arg);

static bool mqtt_integration_selected(void)
{
    return hvac_core_integration_get() == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT;
}

/* ===================== Object Scanner & Browser APIs ===================== */

typedef struct {
    uint16_t type;
    uint32_t instance;
    char name[64]; /* was 32 - confirmed live truncating "Fan 1 Supply Air Temperature Se..." */
} scanned_object_t;

#define MAX_SCANNED_OBJECTS 480

/* Allocated on demand, not reserved. As a static array this was 480 * 72 =
   34,560 bytes - the single largest allocation on the device, larger than any
   task stack - held permanently for a commissioning tool. It is never
   persisted to NVS, so it is already empty after every reboot; it exists only
   to back the /objects browser between a scan and your selections. The durable
   output of a scan is CustomMqttPoints (2,688 B, in NVS).
   Allocated by /api/objects/scan-start, released after
   SCAN_CATALOG_IDLE_MS with no reads. See scan_catalog_release_if_idle(). */
#define SCAN_PAGE_OBJECTS 60
#define SCAN_PAGE_COUNT (MAX_SCANNED_OBJECTS / SCAN_PAGE_OBJECTS)   /* 8 pages */
static scanned_object_t *ScanPages[SCAN_PAGE_COUNT] = {0};

/* Indexed accessor. The catalog is paged rather than one 34,560-byte block
   because that single allocation had to compete with obj_scan's 24,576-byte
   task stack for the same largest-free-block: taking 34.5K out of a 38K block
   left 14K, and xTaskCreate() then failed silently, hanging the scan at
   state=scanning forever (2026-08-25). Eight 4,320-byte pages fit a fragmented
   heap comfortably and never contend with a task stack. */
static inline scanned_object_t *scan_obj(size_t i)
{
    if (i >= MAX_SCANNED_OBJECTS) return NULL;
    scanned_object_t *page = ScanPages[i / SCAN_PAGE_OBJECTS];
    return page ? &page[i % SCAN_PAGE_OBJECTS] : NULL;
}
static inline bool scan_catalog_present(void) { return ScanPages[0] != NULL; }
static size_t ScannedObjectCount = 0;
static volatile int64_t ScanCatalogLastUseUs = 0;

/* Free the catalog this long after the last read. Long enough to browse and
   make selections without it vanishing mid-session; short enough that the RAM
   is back well before it is needed elsewhere. */
#define SCAN_CATALOG_IDLE_MS (10 * 60 * 1000)


typedef enum {
    SCAN_STATE_IDLE = 0,
    SCAN_STATE_RUNNING,
    SCAN_STATE_COMPLETE,
    SCAN_STATE_ERROR
} scan_state_t;

static scan_state_t ScanState = SCAN_STATE_IDLE;
static uint32_t ScanCurrent = 0;
static uint32_t ScanTotal = 0;
static uint8_t ScanPercent = 0;
static char ScanErrorMsg[64] = {0};
/* MAX_SCANNED_OBJECTS is a fixed catalog size (PSRAM budget, not an
 * arbitrary guess), but a real controller can have more objects than that -
 * without this flag, a scan that stopped early because it hit the cap
 * reported the same "complete" state as one that saw every object, with no
 * way to tell the two apart. */
static volatile bool ScanTruncated = false;
static TaskHandle_t ScanTaskHandle = NULL;
static volatile bool ScanTaskFinished = false;
/* Must be heap_caps_calloc'd with MALLOC_CAP_INTERNAL, not a plain static.
 * A FreeRTOS static task's TCB must always live in internal RAM even though
 * its stack can be in PSRAM; with CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
 * enabled for Matter, internal .bss got tight enough that the linker's
 * automatic .bss-overflow-to-PSRAM fallback swept this plain static struct
 * into PSRAM, and xTaskCreateStaticPinnedToCore's xPortCheckValidTCBMem()
 * check aborted with "assert failed" the moment a scan started. Same fix
 * matter_adapter.cpp already uses for its own sync task's TCB. */
static StaticTask_t *ScanTaskTcb = NULL;
static StackType_t *ScanTaskStack = NULL;

/* The commissioning scan is optional and transient. Keep its sizeable stack
   in PSRAM so Wi-Fi, lwIP, HTTP and BACnet retain internal DRAM. */
static void scan_task_cleanup(void)
{
    if (ScanTaskHandle != NULL) {
        vTaskDelete(ScanTaskHandle);
        ScanTaskHandle = NULL;
    }
    if (ScanTaskStack != NULL) {
        heap_caps_free(ScanTaskStack);
        ScanTaskStack = NULL;
    }
    if (ScanTaskTcb != NULL) {
        heap_caps_free(ScanTaskTcb);
        ScanTaskTcb = NULL;
    }
    ScanTaskFinished = false;
}

static bool scan_catalog_acquire(void)
{
    ScanCatalogLastUseUs = esp_timer_get_time();
    if (scan_catalog_present()) {
        return true;
    }
    for (size_t p = 0; p < SCAN_PAGE_COUNT; p++) {
        ScanPages[p] = heap_caps_calloc(
            SCAN_PAGE_OBJECTS, sizeof(scanned_object_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ScanPages[p]) {
            for (size_t q = 0; q < p; q++) { free(ScanPages[q]); ScanPages[q] = NULL; }
            ScannedObjectCount = 0;
            return false;
        }
    }
    return true;
}

/* Called from mqtt_state_task's existing 45s loop - no new task. Never runs
   while a scan is in progress. */
static void scan_catalog_release_if_idle(void)
{
    /* mqtt_state_task calls this every 45 seconds. Reap a completed static
       worker promptly even if the operator never starts another scan. */
    if (ScanTaskFinished) {
        scan_task_cleanup();
    }
    if (!scan_catalog_present() || ScanState == SCAN_STATE_RUNNING) {
        return;
    }
    if (ScanCatalogLastUseUs == 0) {
        return;
    }
    int64_t idle_ms = (esp_timer_get_time() - ScanCatalogLastUseUs) / 1000;
    if (idle_ms < SCAN_CATALOG_IDLE_MS) {
        return;
    }
    for (size_t p = 0; p < SCAN_PAGE_COUNT; p++) { free(ScanPages[p]); ScanPages[p] = NULL; }
    ScannedObjectCount = 0;
    ScanCatalogLastUseUs = 0;
    if (ScanState == SCAN_STATE_COMPLETE) {
        ScanState = SCAN_STATE_IDLE;
    }
    diag_log("object catalog released after idle - %uB back", (unsigned)(MAX_SCANNED_OBJECTS * sizeof(scanned_object_t)));
}

static void object_scan_task(void *arg)
{
    (void)arg;
    ScanState = SCAN_STATE_RUNNING;
    ScanCurrent = 0;
    ScanPercent = 0;
    ScanTotal = 0;
    ScanErrorMsg[0] = '\0';
    ScanTruncated = false;

    if (!BacnetReady || !EthConnected) {
        ScanState = SCAN_STATE_ERROR;
        snprintf(ScanErrorMsg, sizeof(ScanErrorMsg), "BACnet unavailable (check Ethernet link)");
        ScanTaskFinished = true;
        task_registry_remove_self();
        vTaskSuspend(NULL);
        return;
    }

    uint32_t total = 0;
    bacnet_response_t resp = {0};
    if (bacnet_worker_explorer_read_sync(OBJECT_DEVICE, TargetDeviceInstance, PROP_OBJECT_LIST, 0, &resp, 1000) == BACNET_WORKER_STATUS_OK) {
        if (resp.app_val.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
            total = resp.app_val.type.Unsigned_Int;
        }
    }

    if (total == 0 || total > 2000) {
        total = 430;
    }
    ScanTotal = total;

    size_t found = 0;
    for (uint32_t idx = 1; idx <= total && found < MAX_SCANNED_OBJECTS; idx++) {
        if (!scan_catalog_present()) { /* released underneath us - stop cleanly */
            break;
        }
        uint16_t obj_type = 0;
        uint32_t obj_inst = 0;
        bool got_id = false;

        memset(&resp, 0, sizeof(resp));
        if (bacnet_worker_explorer_read_sync(OBJECT_DEVICE, TargetDeviceInstance, PROP_OBJECT_LIST, idx, &resp, 800) == BACNET_WORKER_STATUS_OK) {
            if (resp.app_val.tag == BACNET_APPLICATION_TAG_OBJECT_ID) {
                obj_type = (uint16_t)resp.app_val.type.Object_Id.type;
                obj_inst = resp.app_val.type.Object_Id.instance;
                got_id = true;
            }
        }

        if (got_id) {
            char name_buf[64] = {0};
            memset(&resp, 0, sizeof(resp));
            if (bacnet_worker_explorer_read_sync((BACNET_OBJECT_TYPE)obj_type, obj_inst, PROP_OBJECT_NAME, BACNET_ARRAY_ALL, &resp, 800) == BACNET_WORKER_STATUS_OK) {
                if (resp.app_val.tag == BACNET_APPLICATION_TAG_CHARACTER_STRING) {
                    copy_character_string(name_buf, sizeof(name_buf), (const BACNET_CHARACTER_STRING *)&resp.app_val.type.Character_String);
                }
            }

            scanned_object_t *slot = scan_obj(found);
            if (!slot) {
                break;
            }
            slot->type = obj_type;
            slot->instance = obj_inst;
            if (strlen(name_buf) > 0) {
                strlcpy(slot->name, name_buf, sizeof(slot->name));
            } else {
                snprintf(slot->name, sizeof(slot->name), "%s #%u",
                         bactext_object_type_name_default(obj_type, "obj"), (unsigned)obj_inst);
            }
            found++;
            ScannedObjectCount = found;
        }

        ScanCurrent = idx;
        ScanPercent = total ? (uint8_t)((idx * 100) / total) : 0;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (found >= MAX_SCANNED_OBJECTS && ScanCurrent < total) {
        ScanTruncated = true;
        diag_log("object scan hit the %u-object catalog cap with %u of %u objects still unread",
                 (unsigned)MAX_SCANNED_OBJECTS, (unsigned)(total - ScanCurrent), (unsigned)total);
    }
    ScanState = SCAN_STATE_COMPLETE;
    ScanTaskFinished = true;
    task_registry_remove_self();
    /* The next scan request reaps this suspended static task and frees the
       PSRAM stack. A task must not free the stack it is currently using. */
    vTaskSuspend(NULL);
}

static esp_err_t api_objects_scan_start_handler(httpd_req_t *req)
{
    if (ScanState == SCAN_STATE_RUNNING) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"status\":\"already_running\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!EthConnected) {
        ScanState = SCAN_STATE_ERROR;
        snprintf(ScanErrorMsg, sizeof(ScanErrorMsg), "Ethernet link down");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Ethernet link down\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (ScanTaskFinished) {
        scan_task_cleanup();
    }
    if (!scan_catalog_acquire()) {
        /* 34.5KB contiguous is a big ask on this board. Fail the one endpoint
           loudly rather than starving the device. */
        diag_log("object catalog alloc failed - %uB contiguous unavailable",
                 (unsigned)(MAX_SCANNED_OBJECTS * sizeof(scanned_object_t)));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"insufficient memory for the object catalog - "
            "reboot the bridge and retry the scan before opening other pages\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ScanState = SCAN_STATE_RUNNING;
    ScanCurrent = 0;
    ScanPercent = 0;
    ScannedObjectCount = 0;
    /* 24576, was 8192. This task walks the controller's object-list issuing a
       ReadProperty per object, which is the same nested MAX_APDU=1476 path
       that bacnet_client_task needs 24576 for and that measures at ~18KB peak
       in the HTTP handlers. At 8192 it could never complete a scan: triggering
       one on 2026-08-25 gave "***ERROR*** A stack overflow in task obj_scan"
       followed by an IntegerDivideByZero panic on the corrupted stack. The
       task is transient - created per scan, deleted at the end - so this is
       borrowed for the duration of a scan, not held. */
    ScanTaskStack = heap_caps_malloc(24576, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ScanTaskTcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ScanTaskStack || !ScanTaskTcb) {
        ScanState = SCAN_STATE_ERROR;
        snprintf(ScanErrorMsg, sizeof(ScanErrorMsg), "no memory for scan task");
        heap_caps_free(ScanTaskStack);
        ScanTaskStack = NULL;
        heap_caps_free(ScanTaskTcb);
        ScanTaskTcb = NULL;
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"insufficient memory to start the scan\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ScanTaskHandle = xTaskCreateStaticPinnedToCore(
        object_scan_task, "obj_scan", 24576, NULL, 5, ScanTaskStack, ScanTaskTcb, tskNO_AFFINITY);
    if (!ScanTaskHandle) {
        ScanState = SCAN_STATE_ERROR;
        snprintf(ScanErrorMsg, sizeof(ScanErrorMsg), "could not create scan task");
        heap_caps_free(ScanTaskStack);
        ScanTaskStack = NULL;
        heap_caps_free(ScanTaskTcb);
        ScanTaskTcb = NULL;
        for (size_t p = 0; p < SCAN_PAGE_COUNT; p++) { heap_caps_free(ScanPages[p]); ScanPages[p] = NULL; }
        ScannedObjectCount = 0;
        diag_log("obj_scan static task create failed - scan aborted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"could not start scan task\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    task_registry_add("obj_scan", ScanTaskHandle, 24576);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_objects_scan_start_uri = {
    .uri = "/api/objects/scan-start", .method = HTTP_POST, .handler = api_objects_scan_start_handler};

static esp_err_t api_objects_scan_status_handler(httpd_req_t *req)
{
    const char *st_str = "idle";
    if (ScanState == SCAN_STATE_RUNNING) st_str = "scanning";
    else if (ScanState == SCAN_STATE_COMPLETE) st_str = "complete";
    else if (ScanState == SCAN_STATE_ERROR) st_str = "error";

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"state\":\"%s\",\"current\":%u,\"total\":%u,\"percent\":%u,\"count\":%u,"
             "\"truncated\":%s,\"error\":\"%s\"}",
             st_str, (unsigned)ScanCurrent, (unsigned)ScanTotal, (unsigned)ScanPercent,
             (unsigned)ScannedObjectCount, ScanTruncated ? "true" : "false", ScanErrorMsg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_objects_scan_status_uri = {
    .uri = "/api/objects/scan-status", .method = HTTP_GET, .handler = api_objects_scan_status_handler};

static esp_err_t api_objects_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (!scan_catalog_present()) {
        /* Released after idle, or never scanned this boot. Either way the
           catalog has to be rebuilt - it is not persisted. */
        httpd_resp_send(req,
            "{\"ok\":true,\"count\":0,\"released\":true,\"objects\":[]}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ScanCatalogLastUseUs = esp_timer_get_time(); /* keep it alive while browsing */
    char chunk[512];
    int len = snprintf(chunk, sizeof(chunk), "{\"ok\":true,\"count\":%u,\"objects\":[", (unsigned)ScannedObjectCount);
    httpd_resp_send_chunk(req, chunk, len);

    /* 64-char name * worst-case 2x escaping (every char is '"' or '\\') + slack */
    char name_escaped[129];
    for (size_t i = 0; i < ScannedObjectCount; i++) {
        const scanned_object_t *o = scan_obj(i);
        if (!o) break;
        json_escape(name_escaped, o->name, sizeof(name_escaped));
        const char *tname = bactext_object_type_name_default(o->type, "unknown");
        bool writable = (o->type == OBJECT_ANALOG_VALUE ||
                          o->type == OBJECT_BINARY_VALUE ||
                          o->type == OBJECT_MULTI_STATE_VALUE ||
                          o->type == OBJECT_ANALOG_OUTPUT ||
                          o->type == OBJECT_BINARY_OUTPUT ||
                          o->type == OBJECT_MULTI_STATE_OUTPUT);
        len = snprintf(chunk, sizeof(chunk), "%s{\"type\":\"%s\",\"instance\":%u,\"name\":\"%s\",\"writable\":%s}",
                       i == 0 ? "" : ",", tname, (unsigned)o->instance, name_escaped,
                       writable ? "true" : "false");
        httpd_resp_send_chunk(req, chunk, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
static const httpd_uri_t api_objects_uri = {
    .uri = "/api/objects", .method = HTTP_GET, .handler = api_objects_get_handler};

static inline bool is_commandable_type(BACNET_OBJECT_TYPE type)
{
    /* On Delta DAC controllers, only Analog Output, Binary Output, and Multi-State Output support Priority_Array */
    return (type == OBJECT_ANALOG_OUTPUT ||
            type == OBJECT_BINARY_OUTPUT ||
            type == OBJECT_MULTI_STATE_OUTPUT);
}

static esp_err_t api_objects_inspect_handler(httpd_req_t *req)
{
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"Missing query\"}");
        return ESP_OK;
    }
    char type_str[32] = {0}, inst_str[16] = {0};
    httpd_query_key_value(query, "type", type_str, sizeof(type_str));
    httpd_query_key_value(query, "instance", inst_str, sizeof(inst_str));

    BACNET_OBJECT_TYPE otype;
    if (!parse_object_type(type_str, &otype)) {
        send_bad_request(req, "{\"ok\":false,\"error\":\"Invalid type\"}");
        return ESP_OK;
    }
    uint32_t oinst = (uint32_t)strtoul(inst_str, NULL, 10);

    char pv_json[64] = "null";
    char desc_json[128] = "null";
    char units_json[64] = "null";
    char flags_json[64] = "null";
    char pri_json[512] = "null";

    if (!BacnetReady) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"BACnet not ready\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* 1. Present_Value */
    bacnet_response_t resp = {0};
    if (bacnet_worker_explorer_read_sync(otype, oinst, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, &resp, 800) == BACNET_WORKER_STATUS_OK) {
        append_value_json(pv_json, sizeof(pv_json), &resp.app_val);
    }

    /* 2. Units - only for Analog objects */
    if (otype == OBJECT_ANALOG_INPUT || otype == OBJECT_ANALOG_OUTPUT || otype == OBJECT_ANALOG_VALUE) {
        vTaskDelay(pdMS_TO_TICKS(10));
        memset(&resp, 0, sizeof(resp));
        if (bacnet_worker_explorer_read_sync(otype, oinst, PROP_UNITS, BACNET_ARRAY_ALL, &resp, 800) == BACNET_WORKER_STATUS_OK &&
            resp.app_val.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
            const char *uname = bactext_engineering_unit_name_default(resp.app_val.type.Enumerated, "");
            snprintf(units_json, sizeof(units_json), "\"%s\"", uname);
        }
    }

    /* 3. Priority Array - only for actual commandable outputs */
    if (is_commandable_type(otype)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        memset(&resp, 0, sizeof(resp));
        if (bacnet_worker_explorer_read_sync(otype, oinst, PROP_PRIORITY_ARRAY, BACNET_ARRAY_ALL, &resp, 800) == BACNET_WORKER_STATUS_OK) {
            int raw_len = resp.raw_data_len;
            if (raw_len > 0) {
                pri_json[0] = '[';
                pri_json[1] = '\0';
                size_t pos = 1;
                int offset = 0;
                int slot = 0;
                while (offset < raw_len && slot < 16 && pos < sizeof(pri_json) - 32) {
                    BACNET_APPLICATION_DATA_VALUE elem = {0};
                    int consumed = bacapp_decode_application_data(
                        resp.raw_data + offset, (uint32_t)(raw_len - offset), &elem);
                    if (consumed <= 0) break;
                    offset += consumed;
                    if (slot > 0 && pos < sizeof(pri_json) - 32) {
                        pri_json[pos++] = ',';
                        pri_json[pos] = '\0';
                    }
                    if (pos < sizeof(pri_json) - 32) {
                        pos += append_value_json(pri_json + pos, sizeof(pri_json) - pos, &elem);
                    }
                    slot++;
                }
                if (pos < sizeof(pri_json) - 2) {
                    pri_json[pos++] = ']';
                    pri_json[pos] = '\0';
                }
            }
        }
    }

    char buf[1536];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"type\":\"%s\",\"instance\":%u,\"present_value\":%s,\"description\":%s,\"units\":%s,\"status_flags\":%s,\"priority_array\":%s}",
             type_str, (unsigned)oinst, pv_json, desc_json, units_json, flags_json, pri_json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_objects_inspect_uri = {
    .uri = "/api/objects/inspect", .method = HTTP_GET, .handler = api_objects_inspect_handler};

/* Sized to comfortably batch-read a whole object-browser section (~50 points)
 * in one request rather than the old 32-point cap. points_str must be large
 * enough to hold the decoded points list before it's tokenized - the encoded
 * form body passes through form_extract()'s 3200-byte staging buffer, which
 * bounds how large this can safely go. */
#define BATCH_MAX_POINTS 80
#define BATCH_POINTS_BUF 2048
#define BATCH_RESP_BUF 8192

static esp_err_t api_objects_batch_handler(httpd_req_t *req)
{
    char body[BATCH_POINTS_BUF] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (!BacnetReady) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"BACnet not ready\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *buf = malloc(BATCH_RESP_BUF);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t pos = snprintf(buf, BATCH_RESP_BUF, "{\"ok\":true,\"values\":[");
    const size_t safety_margin = 200; /* room for one more entry + "],\"truncated\":true}" */

    char points_str[BATCH_POINTS_BUF] = {0};
    if (!json_get_str(body, "points", points_str, sizeof(points_str))) {
        form_extract(body, "points", points_str, sizeof(points_str));
    }

    char *saveptr = NULL;
    char *token = strtok_r(points_str, ",", &saveptr);
    int count = 0;
    bool truncated = false;

    while (token) {
        if (count >= BATCH_MAX_POINTS || pos >= BATCH_RESP_BUF - safety_margin) {
            truncated = true;
            break;
        }
        while (*token == ' ' || *token == '\"' || *token == '[' || *token == ']') token++;
        char *colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            char *tstr = token;
            char *istr = colon + 1;
            char *endquote = strpbrk(istr, " \"]");
            if (endquote) *endquote = '\0';

            BACNET_OBJECT_TYPE otype;
            if (parse_object_type(tstr, &otype)) {
                uint32_t oinst = (uint32_t)strtoul(istr, NULL, 10);
                bacnet_response_t batch_resp = {0};
                bool ok = (bacnet_worker_explorer_read_sync(otype, oinst, PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, &batch_resp, 600) == BACNET_WORKER_STATUS_OK);

                char val_json[48] = "null";
                if (ok) {
                    append_value_json(val_json, sizeof(val_json), &batch_resp.app_val);
                }

                if (count > 0) {
                    buf[pos++] = ',';
                    buf[pos] = '\0';
                }
                pos += snprintf(buf + pos, BATCH_RESP_BUF - pos,
                                "{\"type\":\"%s\",\"instance\":%u,\"valid\":%s,\"value\":%s}",
                                tstr, (unsigned)oinst, ok ? "true" : "false", val_json);
                count++;
            }
        }
        token = strtok_r(NULL, ",", &saveptr);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (pos < BATCH_RESP_BUF - 40) {
        pos += snprintf(buf + pos, BATCH_RESP_BUF - pos, "],\"truncated\":%s}", truncated ? "true" : "false");
    } else {
        snprintf(buf + pos, BATCH_RESP_BUF - pos, "]}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}
static const httpd_uri_t api_objects_batch_uri = {
    .uri = "/api/objects/batch", .method = HTTP_POST, .handler = api_objects_batch_handler};

/* ===================== Custom MQTT Configuration ===================== */

#define NVS_MQTT_NAMESPACE "nvs_mqtt"
#define NVS_CUSTOM_MQTT_NAMESPACE "nvs_custmq"
#define NVS_APP_NAMESPACE "app_cfg"
#define MAX_CUSTOM_MQTT 32

static esp_mqtt_client_handle_t MqttClient = NULL;
static volatile bool MqttConnected = false;
/* Link-liveness signal for mqtt_state_task's watchdog check below - set when
   MQTT_EVENT_DISCONNECTED fires, cleared on MQTT_EVENT_CONNECTED. 0 = not
   currently down. Deliberately NOT a dedicated esp_ping-based check: that
   approach (tried 2026-08-23, reverted same day) spawned a task per check and
   pushed this no-PSRAM board's heap from 15K into a 2K reboot loop. Piggybacking
   on MQTT's own connect state costs one int64 and a few lines inside an
   already-running task - no new task, no new heap allocation. */
static volatile int64_t MqttDisconnectedSinceUs = 0;
static volatile int64_t MqttWatchdogLastKickUs = 0;
static volatile int64_t MqttRecycleLastUs = 0;
/* Release the MQTT client's own heap after this long unconnected, well before
   the 3-minute link watchdog gives up on WiFi entirely. */
#define MQTT_RECYCLE_AFTER_MS (45 * 1000)
#define MQTT_WATCHDOG_RECONNECT_MS (3 * 60 * 1000)

static char MqttBrokerHost[64] = "";
static uint16_t MqttBrokerPort = 1883;
static char MqttBrokerUser[32] = "";
static char MqttBrokerPass[64] = "";
static char HaDeviceId[32] = "";
static char HaDeviceName[48] = "";
static char MqttTopicBase[64] = "";
static bool HaDiscoveryEnabled = true;
static bool HaHealthDiscoveryEnabled = true;
/* mqtt_publish_discovery() runs at MQTT_EVENT_CONNECTED, which typically
   fires before the BACnet task finishes binding - at that point fan-count
   discovery falls back to publishing every possible fan channel. Once
   BacnetReady flips true, mqtt_state_task republishes discovery so the fan
   list gets trimmed to the real channel count. */
static bool DiscoveryNeedsBacnetRefresh = true;

typedef struct {
    char type_str[24];
    uint16_t obj_type;
    uint32_t instance;
    char name[32];
    char component[16];
    bool enabled;
} custom_mqtt_point_t;

static custom_mqtt_point_t CustomMqttPoints[MAX_CUSTOM_MQTT];
static size_t CustomMqttCount = 0;

static void mqtt_config_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_MQTT_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t host_len = sizeof(MqttBrokerHost);
        nvs_get_str(handle, "host", MqttBrokerHost, &host_len);
        nvs_get_u16(handle, "port", &MqttBrokerPort);
        size_t user_len = sizeof(MqttBrokerUser);
        nvs_get_str(handle, "user", MqttBrokerUser, &user_len);
        size_t pass_len = sizeof(MqttBrokerPass);
        nvs_get_str(handle, "pass", MqttBrokerPass, &pass_len);
        size_t base_len = sizeof(MqttTopicBase);
        nvs_get_str(handle, "base_top", MqttTopicBase, &base_len);
        size_t devn_len = sizeof(HaDeviceName);
        nvs_get_str(handle, "dev_name", HaDeviceName, &devn_len);
        size_t devid_len = sizeof(HaDeviceId);
        nvs_get_str(handle, "dev_id", HaDeviceId, &devid_len);
        uint8_t disc = 1;
        if (nvs_get_u8(handle, "disc_en", &disc) == ESP_OK) {
            HaDiscoveryEnabled = (disc != 0);
        }
        uint8_t hdisc = 1;
        if (nvs_get_u8(handle, "hlth_en", &hdisc) == ESP_OK) {
            HaHealthDiscoveryEnabled = (hdisc != 0);
        }
        nvs_close(handle);
    }

    if (HaDeviceName[0] == '\0') {
        if (TargetDeviceNameValid && strlen(TargetDeviceName) > 0) {
            snprintf(HaDeviceName, sizeof(HaDeviceName), "BACnet Bridge (%.31s)", TargetDeviceName);
        } else {
            strlcpy(HaDeviceName, "BACnet Bridge", sizeof(HaDeviceName));
        }
    }
    if (HaDeviceId[0] == '\0') {
        if (TargetDeviceInstance > 0) {
            snprintf(HaDeviceId, sizeof(HaDeviceId), "bacnet_%u", (unsigned)TargetDeviceInstance);
        } else {
            strlcpy(HaDeviceId, "bacnet_bridge", sizeof(HaDeviceId));
        }
    }
    if (MqttTopicBase[0] == '\0') {
        if (TargetDeviceInstance > 0) {
            snprintf(MqttTopicBase, sizeof(MqttTopicBase), "esp32bacnet/%u", (unsigned)TargetDeviceInstance);
        } else {
            strlcpy(MqttTopicBase, "esp32bacnet", sizeof(MqttTopicBase));
        }
    }
}

static bool mqtt_config_save_direct(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open(NVS_MQTT_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    nvs_set_str(handle, "host", MqttBrokerHost);
    nvs_set_u16(handle, "port", MqttBrokerPort);
    nvs_set_str(handle, "user", MqttBrokerUser);
    nvs_set_str(handle, "pass", MqttBrokerPass);
    nvs_set_str(handle, "base_top", MqttTopicBase);
    nvs_set_str(handle, "dev_name", HaDeviceName);
    nvs_set_str(handle, "dev_id", HaDeviceId);
    nvs_set_u8(handle, "disc_en", HaDiscoveryEnabled ? 1 : 0);
    nvs_set_u8(handle, "hlth_en", HaHealthDiscoveryEnabled ? 1 : 0);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool mqtt_config_save(void)
{
    return run_internal_flash_op(mqtt_config_save_direct, NULL);
}

static void custom_mqtt_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_CUSTOM_MQTT_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t count = 0;
    if (nvs_get_u8(handle, "count", &count) == ESP_OK && count <= MAX_CUSTOM_MQTT) {
        CustomMqttCount = count;
        for (size_t i = 0; i < CustomMqttCount; i++) {
            char key[32];
            snprintf(key, sizeof(key), "p%u_tstr", (unsigned)i);
            size_t slen = sizeof(CustomMqttPoints[i].type_str);
            nvs_get_str(handle, key, CustomMqttPoints[i].type_str, &slen);

            snprintf(key, sizeof(key), "p%u_type", (unsigned)i);
            nvs_get_u16(handle, key, &CustomMqttPoints[i].obj_type);

            snprintf(key, sizeof(key), "p%u_inst", (unsigned)i);
            nvs_get_u32(handle, key, &CustomMqttPoints[i].instance);

            snprintf(key, sizeof(key), "p%u_name", (unsigned)i);
            size_t nlen = sizeof(CustomMqttPoints[i].name);
            nvs_get_str(handle, key, CustomMqttPoints[i].name, &nlen);

            snprintf(key, sizeof(key), "p%u_comp", (unsigned)i);
            size_t clen = sizeof(CustomMqttPoints[i].component);
            nvs_get_str(handle, key, CustomMqttPoints[i].component, &clen);

            snprintf(key, sizeof(key), "p%u_en", (unsigned)i);
            uint8_t en = 0;
            nvs_get_u8(handle, key, &en);
            CustomMqttPoints[i].enabled = (en != 0);
        }
    }
    nvs_close(handle);
}

static bool custom_mqtt_save_direct(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open(NVS_CUSTOM_MQTT_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    nvs_set_u8(handle, "count", (uint8_t)CustomMqttCount);
    for (size_t i = 0; i < CustomMqttCount; i++) {
        char key[32];
        snprintf(key, sizeof(key), "p%u_tstr", (unsigned)i);
        nvs_set_str(handle, key, CustomMqttPoints[i].type_str);

        snprintf(key, sizeof(key), "p%u_type", (unsigned)i);
        nvs_set_u16(handle, key, CustomMqttPoints[i].obj_type);

        snprintf(key, sizeof(key), "p%u_inst", (unsigned)i);
        nvs_set_u32(handle, key, CustomMqttPoints[i].instance);

        snprintf(key, sizeof(key), "p%u_name", (unsigned)i);
        nvs_set_str(handle, key, CustomMqttPoints[i].name);

        snprintf(key, sizeof(key), "p%u_comp", (unsigned)i);
        nvs_set_str(handle, key, CustomMqttPoints[i].component);

        snprintf(key, sizeof(key), "p%u_en", (unsigned)i);
        nvs_set_u8(handle, key, CustomMqttPoints[i].enabled ? 1 : 0);
    }
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool custom_mqtt_save(void)
{
    return run_internal_flash_op(custom_mqtt_save_direct, NULL);
}

static void parse_custom_mqtt_from_json(const char *body)
{
    const char *p = strstr(body, "\"custom_mqtt\"");
    if (!p) p = strstr(body, "\"points\"");
    if (p) {
        const char *arr = strchr(p, '[');
        if (arr) {
            const char *obj = arr;
            size_t idx = 0;
            while ((obj = strchr(obj, '{')) != NULL && idx < MAX_CUSTOM_MQTT) {
                const char *end_obj = strchr(obj, '}');
                if (!end_obj) break;
                char item[256];
                size_t len = (size_t)(end_obj - obj + 1);
                if (len >= sizeof(item)) len = sizeof(item) - 1;
                memcpy(item, obj, len);
                item[len] = '\0';

                char tstr[24] = {0}, name[32] = {0}, comp[16] = {0};
                int inst = -1;
                bool en = true;

                json_get_str(item, "type", tstr, sizeof(tstr));
                json_get_int(item, "instance", &inst);
                json_get_str(item, "name", name, sizeof(name));
                json_get_str(item, "component", comp, sizeof(comp));
                json_get_bool(item, "enabled", &en);

                if (strlen(tstr) > 0 && inst >= 0 && strlen(name) > 0) {
                    strlcpy(CustomMqttPoints[idx].type_str, tstr, sizeof(CustomMqttPoints[idx].type_str));
                    BACNET_OBJECT_TYPE obj_type;
                    if (parse_object_type(tstr, &obj_type)) {
                        CustomMqttPoints[idx].obj_type = (uint16_t)obj_type;
                    }
                    CustomMqttPoints[idx].instance = (uint32_t)inst;
                    strlcpy(CustomMqttPoints[idx].name, name, sizeof(CustomMqttPoints[idx].name));
                    strlcpy(CustomMqttPoints[idx].component, comp[0] ? comp : "sensor", sizeof(CustomMqttPoints[idx].component));
                    CustomMqttPoints[idx].enabled = en;
                    idx++;
                }
                obj = end_obj + 1;
            }
            CustomMqttCount = idx;
        }
    }
}

#define NVS_KEY_BOOST_TIMEOUT "boost_min"
#define NVS_KEY_BOOST_EXTERNAL "boost_ext"
/* mDNS advertisement. Default OFF and deliberately so: an always-on responder
   cost ~3-5K of heap on this no-PSRAM board, which was the reason it was cut
   in the first place. Existing installations have no such key in NVS, so they
   read as disabled and stay disabled. Note this governs only the *service
   advertisement* (.local hostname); the transient mdns_query_ptr() used to
   discover MQTT brokers in the wizard is unaffected and always available. */
#define NVS_KEY_MDNS_ENABLED "mdns_en"

static void app_config_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_APP_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint16_t minutes;
    if (nvs_get_u16(handle, NVS_KEY_BOOST_TIMEOUT, &minutes) == ESP_OK &&
        minutes <= BOOST_TIMEOUT_MAX_MIN) {
        BoostTimeoutMinutes = minutes;
    }
    uint8_t external;
    if (nvs_get_u8(handle, NVS_KEY_BOOST_EXTERNAL, &external) == ESP_OK) {
        BoostRevertExternal = external != 0;
    }
    /* Initialised, and only an explicit 1 enables it. On the first boot of the
       firmware that introduced this key the device came up advertising despite
       no prior firmware ever having written it - cause not established, so
       this path is deliberately defensive: an uninitialised read cannot leak
       through, and any value that is not exactly 1 leaves mDNS off. */
    uint8_t mdns_en = 0;
    if (nvs_get_u8(handle, NVS_KEY_MDNS_ENABLED, &mdns_en) == ESP_OK) {
        MdnsEnabled = (mdns_en == 1);
    } else {
        MdnsEnabled = false;
    }
    nvs_close(handle);
}

static bool app_config_save_direct(void *arg)
{
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open(NVS_APP_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    nvs_set_u16(handle, NVS_KEY_BOOST_TIMEOUT, BoostTimeoutMinutes);
    nvs_set_u8(handle, NVS_KEY_BOOST_EXTERNAL, BoostRevertExternal ? 1 : 0);
    nvs_set_u8(handle, NVS_KEY_MDNS_ENABLED, MdnsEnabled ? 1 : 0);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool app_config_save(void)
{
    return run_internal_flash_op(app_config_save_direct, NULL);
}

static esp_err_t api_objects_custom_mqtt_get_handler(httpd_req_t *req)
{
    char buf[2048];
    size_t off = snprintf(buf, sizeof(buf), "{\"ok\":true,\"points\":[");
    for (size_t i = 0; i < CustomMqttCount && off < sizeof(buf) - 64; i++) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"type\":\"%s\",\"instance\":%u,\"name\":\"%s\",\"component\":\"%s\",\"enabled\":%s}",
                        i == 0 ? "" : ",", CustomMqttPoints[i].type_str,
                        (unsigned)CustomMqttPoints[i].instance, CustomMqttPoints[i].name,
                        CustomMqttPoints[i].component, CustomMqttPoints[i].enabled ? "true" : "false");
    }
    snprintf(buf + off, sizeof(buf) - off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_objects_custom_mqtt_get_uri = {
    .uri = "/api/objects/custom-mqtt", .method = HTTP_GET, .handler = api_objects_custom_mqtt_get_handler};

static esp_err_t api_objects_custom_mqtt_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char action[16] = {0}, tstr[24] = {0}, name[32] = {0}, comp[16] = {0};
    int inst = 0;
    bool enabled = true;

    if (body[0] == '{') {
        json_get_str(body, "action", action, sizeof(action));
        json_get_str(body, "type", tstr, sizeof(tstr));
        json_get_int(body, "instance", &inst);
        json_get_str(body, "name", name, sizeof(name));
        json_get_str(body, "component", comp, sizeof(comp));
        json_get_bool(body, "enabled", &enabled);
    } else {
        httpd_query_key_value(body, "action", action, sizeof(action));
        httpd_query_key_value(body, "type", tstr, sizeof(tstr));
        char inst_str[16] = {0};
        httpd_query_key_value(body, "instance", inst_str, sizeof(inst_str));
        if (inst_str[0]) inst = (int)strtol(inst_str, NULL, 10);
        httpd_query_key_value(body, "name", name, sizeof(name));
        httpd_query_key_value(body, "component", comp, sizeof(comp));
    }

    if (comp[0] == '\0') {
        strlcpy(comp, "sensor", sizeof(comp));
    }

    BACNET_OBJECT_TYPE otype;
    parse_object_type(tstr, &otype);

    if (strcmp(action, "remove") == 0 || !enabled) {
        for (size_t i = 0; i < CustomMqttCount; i++) {
            if (CustomMqttPoints[i].obj_type == (uint16_t)otype && CustomMqttPoints[i].instance == (uint32_t)inst) {
                if (MqttClient && MqttConnected) {
                    char unique_id[64];
                    snprintf(unique_id, sizeof(unique_id), "%s_cust_%u_%u",
                             HaDeviceId, CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance);
                    char disc_top[128];
                    snprintf(disc_top, sizeof(disc_top), "homeassistant/%s/%s/config",
                             CustomMqttPoints[i].component, unique_id);
                    esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
                }
                for (size_t j = i; j + 1 < CustomMqttCount; j++) {
                    CustomMqttPoints[j] = CustomMqttPoints[j + 1];
                }
                CustomMqttCount--;
                break;
            }
        }
    } else {
        bool found = false;
        for (size_t i = 0; i < CustomMqttCount; i++) {
            if (CustomMqttPoints[i].obj_type == (uint16_t)otype && CustomMqttPoints[i].instance == (uint32_t)inst) {
                if (strlen(name) > 0) strlcpy(CustomMqttPoints[i].name, name, sizeof(CustomMqttPoints[i].name));
                strlcpy(CustomMqttPoints[i].component, comp, sizeof(CustomMqttPoints[i].component));
                CustomMqttPoints[i].enabled = true;
                found = true;
                break;
            }
        }
        /* CustomMqttCount == MAX_CUSTOM_MQTT used to silently no-op here
         * while still returning {"ok":true} below - the UI reported success
         * for a point that was never actually added. */
        if (!found && CustomMqttCount >= MAX_CUSTOM_MQTT) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"custom point limit reached (32)\"}",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        if (!found && CustomMqttCount < MAX_CUSTOM_MQTT) {
            strlcpy(CustomMqttPoints[CustomMqttCount].type_str, tstr, sizeof(CustomMqttPoints[CustomMqttCount].type_str));
            CustomMqttPoints[CustomMqttCount].obj_type = (uint16_t)otype;
            CustomMqttPoints[CustomMqttCount].instance = (uint32_t)inst;
            if (strlen(name) > 0) {
                strlcpy(CustomMqttPoints[CustomMqttCount].name, name, sizeof(CustomMqttPoints[CustomMqttCount].name));
            } else {
                snprintf(CustomMqttPoints[CustomMqttCount].name, sizeof(CustomMqttPoints[CustomMqttCount].name),
                         "%s #%u", tstr, (unsigned)inst);
            }
            strlcpy(CustomMqttPoints[CustomMqttCount].component, comp, sizeof(CustomMqttPoints[CustomMqttCount].component));
            CustomMqttPoints[CustomMqttCount].enabled = true;
            CustomMqttCount++;
        }
    }

    custom_mqtt_save();
    mqtt_publish_discovery();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_objects_custom_mqtt_post_uri = {
    .uri = "/api/objects/custom-mqtt", .method = HTTP_POST, .handler = api_objects_custom_mqtt_post_handler};

/* ===================== BACnet Discovery & Strategy Analysis APIs ===================== */

static esp_err_t api_bacnet_discover_handler(httpd_req_t *req)
{
    char body[256] = {0};
    recv_body(req, body, sizeof(body));

    char req_ip[16] = {0};
    int req_port = 0;
    int req_dev_id = 0;
    json_get_str(body, "ip", req_ip, sizeof(req_ip));
    json_get_int(body, "port", &req_port);
    json_get_int(body, "device_id", &req_dev_id);

    if (EthConnected) {
        bacnet_worker_discover_sync(DiscoveredDevices, MAX_DISCOVERED_DEVICES, &DiscoveredDeviceCount,
                                    &DiscoveredDeviceSeenCount, 4000);
        if (DiscoveredDeviceCount > 0) {
            TargetDeviceInstance = DiscoveredDevices[0].device_id;
            strlcpy(TargetIp, DiscoveredDevices[0].ip, sizeof(TargetIp));
            TargetPort = DiscoveredDevices[0].port;
            strlcpy(TargetDeviceName, DiscoveredDevices[0].name, sizeof(TargetDeviceName));
            TargetDeviceNameValid = true;
            target_config_save();
        }
    }

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t off = snprintf(buf, 4096, "{\"ok\":true,\"count\":%u,\"truncated\":%s,\"devices\":[",
                          (unsigned)DiscoveredDeviceCount,
                          DiscoveredDeviceSeenCount > DiscoveredDeviceCount ? "true" : "false");
    for (size_t i = 0; i < DiscoveredDeviceCount; i++) {
        char name_esc[64] = "", vend_esc[64] = "", mod_esc[64] = "";
        json_escape(name_esc, DiscoveredDevices[i].name, sizeof(name_esc));
        json_escape(vend_esc, DiscoveredDevices[i].vendor_name, sizeof(vend_esc));
        json_escape(mod_esc, DiscoveredDevices[i].model_name, sizeof(mod_esc));

        off += snprintf(buf + off, 4096 - off,
                        "%s{\"device_id\":%u,\"instance\":%u,\"name\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"vendor\":\"%s\",\"model\":\"%s\"}",
                        i == 0 ? "" : ",",
                        (unsigned)DiscoveredDevices[i].device_id,
                        (unsigned)DiscoveredDevices[i].device_id,
                        name_esc, DiscoveredDevices[i].ip,
                        (unsigned)DiscoveredDevices[i].port,
                        vend_esc, mod_esc);
    }
    snprintf(buf + off, 4096 - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}
static const httpd_uri_t api_bacnet_discover_uri = {
    .uri = "/api/bacnet/discover", .method = HTTP_POST, .handler = api_bacnet_discover_handler};

static esp_err_t api_strategy_inspect_handler(httpd_req_t *req)
{
    float fan_count_raw = 0.0f;
    bool fan_count_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_FAN_COUNT_INSTANCE, PROP_PRESENT_VALUE, &fan_count_raw);
    int fan_count = fan_count_valid ? (int)(fan_count_raw + 0.5f) : 0;

    int plausible_fans = 0;
    for (int i = 0; i < HEALTH_FAN_SUPPLY_AIR_COUNT; i++) {
        float fval = 0.0f;
        if (BacnetReady && read_real_property(OBJECT_ANALOG_INPUT, HealthFanSupplyAirInstances[i], PROP_PRESENT_VALUE, &fval)) {
            if (temp_is_plausible(fval)) {
                plausible_fans++;
            }
        }
    }
    if (!fan_count_valid) {
        fan_count = plausible_fans > 0 ? plausible_fans : 1;
    }

    float design_duty = 0.0f;
    bool design_duty_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_DESIGN_COOLING_DUTY_INSTANCE, PROP_PRESENT_VALUE, &design_duty);

    char buf[3072];
    size_t off = snprintf(
        buf, sizeof(buf),
        "{\"ok\":true,\"device_name\":\"%s\",\"device_id\":%u,\"vendor\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\","
        "\"fan_count\":%d,\"fan_count_valid\":%s,\"plausible_fans\":%d,\"design_duty\":%.2f,\"design_duty_valid\":%s,\"fans\":[",
        TargetDeviceNameValid ? TargetDeviceName : "Delta Controller",
        (unsigned)TargetDeviceInstance,
        fan_count, fan_count_valid ? "true" : "false", plausible_fans,
        design_duty_valid ? design_duty : 0.0f,
        design_duty_valid ? "true" : "false");

    for (int i = 0; i < 5; i++) {
        float fval = 0.0f;
        bool fvalid = BacnetReady && read_real_property(
            OBJECT_ANALOG_INPUT, HealthFanSupplyAirInstances[i], PROP_PRESENT_VALUE, &fval);
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"index\":%d,\"sensor\":%u,\"valid\":%s,\"temp\":%.1f,\"plausible\":%s,\"active\":%s}",
                        i == 0 ? "" : ",", i + 1, (unsigned)HealthFanSupplyAirInstances[i],
                        fvalid ? "true" : "false", fvalid ? fval : 0.0f,
                        (fvalid && temp_is_plausible(fval)) ? "true" : "false",
                        (i < fan_count) ? "true" : "false");
    }

    off += snprintf(buf + off, sizeof(buf) - off, "],\"rooms\":[");
    for (size_t i = 0; i < RoomCount; i++) {
        float sp = 0.0f, temp = 0.0f, sa = 0.0f;
        bool pwr = false;
        bool sp_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, Rooms[i].setpoint_instance, PROP_PRESENT_VALUE, &sp);
        bool temp_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, Rooms[i].temperature_instance, PROP_PRESENT_VALUE, &temp);
        bool pwr_ok = BacnetReady && read_bool_property(OBJECT_BINARY_VALUE, Rooms[i].power_instance, PROP_PRESENT_VALUE, &pwr);
        bool sa_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, Rooms[i].supply_air_instance, PROP_PRESENT_VALUE, &sa);

        char prefix[16];
        snprintf(prefix, sizeof(prefix), "Room_%c", (char)('A' + i));
        char def_name[16];
        snprintf(def_name, sizeof(def_name), "Room %c", (char)('A' + i));

        bool temp_plausible = temp_ok && temp_is_plausible(temp);
        /* Room is detected as active if it's within configured fan count OR has plausible active temperature */
        bool detected_active = (i < (size_t)fan_count) || (temp_plausible && temp > 16.0f && temp < 35.0f && (sp_ok || sa_ok));

        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"id\":%u,\"name\":\"%s\",\"default_name\":\"%s\",\"prefix\":\"%s\","
                        "\"active\":%s,\"detected_active\":%s,\"instance_base\":%u,"
                        "\"setpoint\":%.1f,\"setpoint_valid\":%s,"
                        "\"temp\":%.1f,\"temp_valid\":%s,\"temp_plausible\":%s,"
                        "\"power\":%s,\"power_valid\":%s,"
                        "\"supply_air\":%.1f,\"supply_air_valid\":%s}",
                        i == 0 ? "" : ",", (unsigned)i, Rooms[i].name, def_name, prefix,
                        Rooms[i].active ? "true" : "false", detected_active ? "true" : "false",
                        (unsigned)(1100 + i * 100),
                        sp_ok ? sp : 0.0f, sp_ok ? "true" : "false",
                        temp_ok ? temp : 0.0f, temp_ok ? "true" : "false", temp_plausible ? "true" : "false",
                        pwr_ok && pwr ? "true" : "false", pwr_ok ? "true" : "false",
                        sa_ok ? sa : 0.0f, sa_ok ? "true" : "false");
    }
    snprintf(buf + off, sizeof(buf) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_strategy_inspect_uri = {
    .uri = "/api/strategy/inspect", .method = HTTP_GET, .handler = api_strategy_inspect_handler};

/* ===================== Rooms & MQTT Management APIs ===================== */

static void parse_rooms_from_json(const char *body)
{
    const char *p = strstr(body, "\"rooms\"");
    if (p) {
        const char *arr = strchr(p, '[');
        if (arr) {
            const char *obj = arr;
            size_t idx = 0;
            while ((obj = strchr(obj, '{')) != NULL && idx < MAX_ROOMS) {
                const char *end_obj = strchr(obj, '}');
                if (!end_obj) break;
                char item[256];
                size_t len = (size_t)(end_obj - obj + 1);
                if (len >= sizeof(item)) len = sizeof(item) - 1;
                memcpy(item, obj, len);
                item[len] = '\0';

                char rname[32] = {0};
                if (json_get_str(item, "name", rname, sizeof(rname)) && strlen(rname) > 0) {
                    strlcpy(Rooms[idx].name, rname, sizeof(Rooms[idx].name));
                }
                bool act = false;
                if (json_get_bool(item, "active", &act)) {
                    Rooms[idx].active = act;
                }
                int sp = 0, temp = 0, pwr = 0, sa = 0, req = 0, cur = 0;
                if (json_get_int(item, "setpoint_instance", &sp) && sp > 0) Rooms[idx].setpoint_instance = (uint32_t)sp;
                if (json_get_int(item, "temperature_instance", &temp) && temp > 0) Rooms[idx].temperature_instance = (uint32_t)temp;
                if (json_get_int(item, "power_instance", &pwr) && pwr > 0) Rooms[idx].power_instance = (uint32_t)pwr;
                if (json_get_int(item, "supply_air_instance", &sa) && sa > 0) Rooms[idx].supply_air_instance = (uint32_t)sa;
                if (json_get_int(item, "required_output_instance", &req) && req > 0) Rooms[idx].required_output_instance = (uint32_t)req;
                if (json_get_int(item, "current_output_instance", &cur) && cur > 0) Rooms[idx].current_output_instance = (uint32_t)cur;

                idx++;
                obj = end_obj + 1;
            }
            if (idx > 0) {
                RoomCount = idx;
            }
        }
    }
    for (size_t i = 0; i < MAX_ROOMS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "r%u_name", (unsigned)i);
        char rname[32] = {0};
        if (json_get_str(body, key, rname, sizeof(rname)) && strlen(rname) > 0) {
            strlcpy(Rooms[i].name, rname, sizeof(Rooms[i].name));
        }
        snprintf(key, sizeof(key), "r%u_active", (unsigned)i);
        bool act = false;
        if (json_get_bool(body, key, &act)) {
            Rooms[i].active = act;
        }
    }
}

static esp_err_t api_rooms_get_handler(httpd_req_t *req)
{
    char buf[1536];
    size_t off = snprintf(buf, sizeof(buf), "{\"ok\":true,\"rooms\":[");
    for (size_t i = 0; i < RoomCount && off < sizeof(buf) - 64; i++) {
        off += snprintf(
            buf + off, sizeof(buf) - off,
            "%s{\"id\":%u,\"name\":\"%s\",\"active\":%s,\"setpoint_instance\":%u,\"temperature_instance\":%u,"
            "\"power_instance\":%u,\"supply_air_instance\":%u,\"required_output_instance\":%u,\"current_output_instance\":%u}",
            i == 0 ? "" : ",", (unsigned)i, Rooms[i].name, Rooms[i].active ? "true" : "false",
            (unsigned)Rooms[i].setpoint_instance, (unsigned)Rooms[i].temperature_instance,
            (unsigned)Rooms[i].power_instance, (unsigned)Rooms[i].supply_air_instance,
            (unsigned)Rooms[i].required_output_instance, (unsigned)Rooms[i].current_output_instance);
    }
    snprintf(buf + off, sizeof(buf) - off, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_rooms_get_uri = {
    .uri = "/api/rooms", .method = HTTP_GET, .handler = api_rooms_get_handler};

static esp_err_t api_rooms_post_handler(httpd_req_t *req)
{
    /* A fixed body[1024] silently truncated any payload larger than that
     * (recv_body has no overflow check) - confirmed live: saving
     * HVAC_CORE_MAX_ROOMS rooms with realistic name lengths produces a
     * payload past 1KB, and the tail (the last room) was silently dropped
     * on write, not rejected. recv_full_body allocates exactly what the
     * request declares, so this scales with room count/name length instead
     * of needing a bigger guessed constant that could still be wrong. */
    char *body = NULL;
    size_t body_len = 0;
    if (!recv_full_body(req, &body, &body_len) || !body) {
        if (body) free(body);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    parse_rooms_from_json(body);
    free(body);
    rooms_config_save();
    mqtt_publish_discovery();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_rooms_post_uri = {
    .uri = "/api/rooms", .method = HTTP_POST, .handler = api_rooms_post_handler};

static esp_err_t api_integration_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_WIFI, "HTTP dispatch GET /api/integration; stack HWM=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    hvac_integration_kind_t kind = hvac_core_integration_get();
    const char *mode_str = "none";
    if (kind == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT) {
        mode_str = "mqtt_home_assistant";
    } else if (kind == HVAC_INTEGRATION_MATTER) {
        mode_str = "matter";
    }

    const char *pairing_str = "idle";
    switch (hvac_core_matter_pairing_get()) {
    case HVAC_PAIRING_AWAITING: pairing_str = "awaiting_pairing"; break;
    case HVAC_PAIRING_PAIRED: pairing_str = "paired"; break;
    case HVAC_PAIRING_TIMED_OUT: pairing_str = "timed_out"; break;
    default: break;
    }

    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"mode\":%d,\"mode_name\":\"%s\",\"matter_supported\":%s,\"mqtt_active\":%s,\"matter_active\":%s,"
             "\"matter_pairing_status\":\"%s\"}",
             (int)kind, mode_str,
#if defined(CONFIG_ENABLE_ESP_MATTER) && CONFIG_ENABLE_ESP_MATTER
             "true",
#else
             "false",
#endif
             (kind == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT && MqttConnected) ? "true" : "false",
             (kind == HVAC_INTEGRATION_MATTER && matter_adapter_is_running()) ? "true" : "false",
             pairing_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_integration_get_uri = {
    .uri = "/api/integration", .method = HTTP_GET, .handler = api_integration_get_handler};

/* The integration summary is intentionally cheap. The Smart Home page uses
 * this richer endpoint to ask CHIP for the real, current window state and
 * the exact onboarding payload being advertised over BLE. */
static esp_err_t api_matter_setup_get_handler(httpd_req_t *req)
{
    matter_onboarding_info_t info;
    bool ok = matter_adapter_get_onboarding_info(&info);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"running\":%s,\"window_open\":%s,\"paired\":%s,"
             "\"discriminator\":%u,\"vendor_id\":%u,\"product_id\":%u,"
             "\"qr_code\":\"%s\",\"manual_code\":\"%s\"}",
             ok ? "true" : "false", info.running ? "true" : "false",
             info.window_open ? "true" : "false", info.paired ? "true" : "false", (unsigned)info.discriminator,
             (unsigned)info.vendor_id, (unsigned)info.product_id,
             info.qr_code, info.manual_code);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_matter_setup_get_uri = {
    .uri = "/api/matter/setup", .method = HTTP_GET, .handler = api_matter_setup_get_handler};

static esp_err_t api_matter_retry_pairing_post_handler(httpd_req_t *req)
{
    if (hvac_core_integration_get() != HVAC_INTEGRATION_MATTER) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Matter is not the active integration mode\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    bool started = matter_adapter_retry_pairing();
    if (started) {
        if (!pairing_store(HVAC_PAIRING_AWAITING)) {
            started = false;
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":%s}", started ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_matter_retry_pairing_uri = {
    .uri = "/api/matter/retry-pairing", .method = HTTP_POST, .handler = api_matter_retry_pairing_post_handler};

static esp_err_t api_matter_close_pairing_post_handler(httpd_req_t *req)
{
    if (hvac_core_integration_get() != HVAC_INTEGRATION_MATTER) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Matter is not the active integration mode\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    bool closed = matter_adapter_close_pairing();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, closed ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_matter_close_pairing_uri = {
    .uri = "/api/matter/close-pairing", .method = HTTP_POST, .handler = api_matter_close_pairing_post_handler};

static esp_err_t api_integration_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int mode_val = -1;
    char mode_str[32] = {0};
    if (json_get_int(body, "mode", &mode_val)) {
        /* Int mode parsed */
    } else if (json_get_str(body, "mode", mode_str, sizeof(mode_str))) {
        if (strcmp(mode_str, "none") == 0) mode_val = 0;
        else if (strcmp(mode_str, "mqtt") == 0 || strcmp(mode_str, "mqtt_home_assistant") == 0) mode_val = 1;
        else if (strcmp(mode_str, "matter") == 0) mode_val = 2;
    }

    if (mode_val < 0 || mode_val > (int)HVAC_INTEGRATION_MATTER) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Invalid integration mode\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    hvac_integration_kind_t old_mode = hvac_core_integration_get();
    hvac_integration_kind_t new_mode = (hvac_integration_kind_t)mode_val;

    if (!integration_store(new_mode)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to save integration mode\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (new_mode != old_mode) {
        if (old_mode == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT) {
            mqtt_app_stop();
        } else if (old_mode == HVAC_INTEGRATION_MATTER) {
            matter_adapter_stop();
        }

        if (new_mode == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT) {
            mqtt_app_start();
            spawn_task(mqtt_state_task, "mqtt_state", 4096, NULL, 5, NULL);
        } else if (new_mode == HVAC_INTEGRATION_MATTER) {
            matter_adapter_init();
            matter_adapter_start();
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_integration_post_uri = {
    .uri = "/api/integration", .method = HTTP_POST, .handler = api_integration_post_handler};

static esp_err_t api_mqtt_config_get_handler(httpd_req_t *req)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"pass_set\":%s,"
             "\"base_topic\":\"%s\",\"ha_device_name\":\"%s\",\"ha_device_id\":\"%s\","
             "\"ha_discovery\":%s,\"ha_health_discovery\":%s,\"connected\":%s}",
             MqttBrokerHost, (unsigned)MqttBrokerPort, MqttBrokerUser,
             strlen(MqttBrokerPass) > 0 ? "true" : "false", MqttTopicBase,
             HaDeviceName, HaDeviceId, HaDiscoveryEnabled ? "true" : "false",
             HaHealthDiscoveryEnabled ? "true" : "false",
             MqttConnected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_mqtt_config_get_uri = {
    .uri = "/api/mqtt/config", .method = HTTP_GET, .handler = api_mqtt_config_get_handler};

static esp_err_t api_mqtt_config_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    if (!recv_body(req, body, sizeof(body))) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char host[64] = {0}, user[32] = {0}, pass[64] = {0};
    char base_top[64] = {0}, dev_name[48] = {0}, dev_id[32] = {0};
    int port = 0;
    bool disc = true, hdisc = true;

    if (json_get_str(body, "host", host, sizeof(host)) && strlen(host) > 0) {
        strlcpy(MqttBrokerHost, host, sizeof(MqttBrokerHost));
    }
    if (json_get_int(body, "port", &port) && port > 0) {
        MqttBrokerPort = (uint16_t)port;
    }
    if (json_get_str(body, "user", user, sizeof(user))) {
        strlcpy(MqttBrokerUser, user, sizeof(MqttBrokerUser));
    }
    if (json_get_str(body, "pass", pass, sizeof(pass))) {
        strlcpy(MqttBrokerPass, pass, sizeof(MqttBrokerPass));
    }
    if (json_get_str(body, "base_topic", base_top, sizeof(base_top)) && strlen(base_top) > 0) {
        strlcpy(MqttTopicBase, base_top, sizeof(MqttTopicBase));
    }
    if (json_get_str(body, "ha_device_name", dev_name, sizeof(dev_name)) && strlen(dev_name) > 0) {
        strlcpy(HaDeviceName, dev_name, sizeof(HaDeviceName));
    }
    if (json_get_str(body, "ha_device_id", dev_id, sizeof(dev_id)) && strlen(dev_id) > 0) {
        strlcpy(HaDeviceId, dev_id, sizeof(HaDeviceId));
    }
    if (json_get_bool(body, "ha_discovery", &disc)) {
        HaDiscoveryEnabled = disc;
    }
    if (json_get_bool(body, "ha_health_discovery", &hdisc)) {
        HaHealthDiscoveryEnabled = hdisc;
    }

    mqtt_config_save();
    mqtt_app_start();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_mqtt_config_post_uri = {
    .uri = "/api/mqtt/config", .method = HTTP_POST, .handler = api_mqtt_config_post_handler};

static esp_err_t api_mqtt_entities_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char chunk[512];
    int len = snprintf(chunk, sizeof(chunk),
                       "{\"ok\":true,\"ha_discovery\":%s,\"ha_health_discovery\":%s,\"connected\":%s,\"base_topic\":\"%s\",\"entities\":[",
                       HaDiscoveryEnabled ? "true" : "false",
                       HaHealthDiscoveryEnabled ? "true" : "false",
                       MqttConnected ? "true" : "false",
                       MqttTopicBase);
    httpd_resp_send_chunk(req, chunk, len);

    bool first = true;

    /* Core 1: System Power */
    bool sys_pwr = false;
    bool sys_pwr_ok = BacnetReady && hvac_core_get_system_power_commanded(&sys_pwr);
    len = snprintf(chunk, sizeof(chunk),
                   "%s{\"name\":\"System Power\",\"group\":\"core\",\"type\":\"switch\","
                   "\"unique_id\":\"%s_system_power\",\"topic\":\"%s/system_power/state\","
                   "\"value\":\"%s\",\"removable\":false,\"enabled\":true}",
                   first ? "" : ",", HaDeviceId, MqttTopicBase,
                   sys_pwr_ok ? (sys_pwr ? "ON" : "OFF") : "Unavailable");
    httpd_resp_send_chunk(req, chunk, len);
    first = false;

    /* Core 2: Boost Mode */
    unsigned bmode = 0;
    bool bmode_ok = BacnetReady && read_msv_property(OBJECT_MULTI_STATE_VALUE, BOOST_INSTANCE, PROP_PRESENT_VALUE, &bmode);
    const char *bmode_str = "Unavailable";
    if (bmode_ok) {
        if (bmode == BOOST_MODE_AUTO) bmode_str = "auto";
        else if (bmode == BOOST_MODE_FULL_HEATING) bmode_str = "heat";
        else if (bmode == BOOST_MODE_FULL_COOLING) bmode_str = "cool";
        else bmode_str = "other";
    }
    len = snprintf(chunk, sizeof(chunk),
                   ",{\"name\":\"Boost Mode\",\"group\":\"core\",\"type\":\"select\","
                   "\"unique_id\":\"%s_boost\",\"topic\":\"%s/boost/state\","
                   "\"value\":\"%s\",\"removable\":false,\"enabled\":true}",
                   HaDeviceId, MqttTopicBase, bmode_str);
    httpd_resp_send_chunk(req, chunk, len);

    /* Core 3: Boost Timeout */
    char btout_str[32];
    snprintf(btout_str, sizeof(btout_str), "%u min", (unsigned)BoostTimeoutMinutes);
    len = snprintf(chunk, sizeof(chunk),
                   ",{\"name\":\"Boost Timeout\",\"group\":\"core\",\"type\":\"number\","
                   "\"unique_id\":\"%s_boost_timeout\",\"topic\":\"%s/boost_timeout/state\","
                   "\"value\":\"%s\",\"removable\":false,\"enabled\":true}",
                   HaDeviceId, MqttTopicBase, btout_str);
    httpd_resp_send_chunk(req, chunk, len);

    /* Core 4: Boost Remaining */
    char brem_str[32];
    snprintf(brem_str, sizeof(brem_str), "%d min", boost_remaining_minutes());
    len = snprintf(chunk, sizeof(chunk),
                   ",{\"name\":\"Boost Remaining\",\"group\":\"core\",\"type\":\"sensor\","
                   "\"unique_id\":\"%s_boost_remaining\",\"topic\":\"%s/boost_remaining/state\","
                   "\"value\":\"%s\",\"removable\":false,\"enabled\":true}",
                   HaDeviceId, MqttTopicBase, brem_str);
    httpd_resp_send_chunk(req, chunk, len);

    /* Core 5: Rooms */
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;
        float temp = 0.0f, sp = 0.0f;
        bool pwr = false;
        bool t_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, Rooms[i].temperature_instance, PROP_PRESENT_VALUE, &temp);
        bool sp_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, Rooms[i].setpoint_instance, PROP_PRESENT_VALUE, &sp);
        bool p_ok = BacnetReady && read_bool_property(OBJECT_BINARY_VALUE, Rooms[i].power_instance, PROP_PRESENT_VALUE, &pwr);

        char rval[64];
        if (t_ok && sp_ok) {
            snprintf(rval, sizeof(rval), "%.1f°C (Set: %.1f°C)%s", temp, sp, p_ok ? (pwr ? " [On]" : " [Off]") : "");
        } else {
            snprintf(rval, sizeof(rval), "%s%s",
                     t_ok ? "Temp OK" : "Temp unavail",
                     p_ok ? (pwr ? " [On]" : " [Off]") : "");
        }
        char name_esc[64];
        json_escape(name_esc, Rooms[i].name, sizeof(name_esc));
        len = snprintf(chunk, sizeof(chunk),
                       ",{\"name\":\"%s (Climate)\",\"group\":\"core\",\"type\":\"climate\","
                       "\"unique_id\":\"%s_room%u\",\"topic\":\"%s/room%u/mode/state\","
                       "\"value\":\"%s\",\"removable\":false,\"enabled\":true}",
                       name_esc, HaDeviceId, (unsigned)i, MqttTopicBase, (unsigned)i, rval);
        httpd_resp_send_chunk(req, chunk, len);
    }

    /* Health Group */
    float h_out = 0.0f, h_req = 0.0f, h_flow_pct = 0.0f, h_ret = 0.0f;
    bool h_out_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, HEALTH_COOLING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_out);
    bool h_req_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_req);
    bool h_fp_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, HEALTH_FLOW_DESIGN_PCT_INSTANCE, PROP_PRESENT_VALUE, &h_flow_pct);
    bool h_ret_ok = BacnetReady && read_real_property(OBJECT_ANALOG_INPUT, HEALTH_RETURN_AIR_INSTANCE, PROP_PRESENT_VALUE, &h_ret);

    float h_heat_out = 0.0f, h_heat_req = 0.0f;
    bool h_hout_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, HEALTH_HEATING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_heat_out);
    bool h_hreq_ok = BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_HEATING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_heat_req);

    const char *hmode = "cooling";
    if (h_hout_ok && h_hreq_ok && (h_heat_req > h_req || h_heat_out > h_out) && h_heat_req > 0.05f) {
        hmode = "heating";
    }

    char v_out[32], v_req[32], v_pct[32], v_flow[32], v_ret[32], v_delta[32];
    if (strcmp(hmode, "heating") == 0) {
        snprintf(v_out, sizeof(v_out), "%s", h_hout_ok ? "" : "Unavailable");
        if (h_hout_ok) snprintf(v_out, sizeof(v_out), "%.2f kW", h_heat_out);
        snprintf(v_req, sizeof(v_req), "%s", h_hreq_ok ? "" : "Unavailable");
        if (h_hreq_ok) snprintf(v_req, sizeof(v_req), "%.2f kW", h_heat_req);
        if (h_hout_ok && h_hreq_ok && h_heat_req > 0.1f) snprintf(v_pct, sizeof(v_pct), "%.0f%%", (h_heat_out / h_heat_req) * 100.0f);
        else snprintf(v_pct, sizeof(v_pct), "Unavailable");
    } else {
        snprintf(v_out, sizeof(v_out), "%s", h_out_ok ? "" : "Unavailable");
        if (h_out_ok) snprintf(v_out, sizeof(v_out), "%.2f kW", h_out);
        snprintf(v_req, sizeof(v_req), "%s", h_req_ok ? "" : "Unavailable");
        if (h_req_ok) snprintf(v_req, sizeof(v_req), "%.2f kW", h_req);
        if (h_out_ok && h_req_ok && h_req > 0.1f) snprintf(v_pct, sizeof(v_pct), "%.0f%%", (h_out / h_req) * 100.0f);
        else snprintf(v_pct, sizeof(v_pct), "Unavailable");
    }

    snprintf(v_flow, sizeof(v_flow), "%s", h_fp_ok ? "" : "Unavailable");
    if (h_fp_ok) snprintf(v_flow, sizeof(v_flow), "%.1f%%", h_flow_pct);

    snprintf(v_ret, sizeof(v_ret), "%s", h_ret_ok ? "" : "Unavailable");
    if (h_ret_ok) snprintf(v_ret, sizeof(v_ret), "%.1f°C", h_ret);

    /* Delta T calculation */
    float sa_sum = 0.0f;
    int sa_cnt = 0;
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;
        float r_sa = 0.0f;
        if (BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, Rooms[i].supply_air_instance, PROP_PRESENT_VALUE, &r_sa) && temp_is_plausible(r_sa)) {
            sa_sum += r_sa;
            sa_cnt++;
        }
    }
    if (h_ret_ok && temp_is_plausible(h_ret) && sa_cnt > 0) {
        float avg_sa = sa_sum / sa_cnt;
        float dt = avg_sa - h_ret; /* signed: negative = cooling, positive = warming */
        snprintf(v_delta, sizeof(v_delta), "%.1f°C", dt);
    } else {
        snprintf(v_delta, sizeof(v_delta), "Unavailable");
    }

    /* Output Health Entities */
    struct { const char *name; const char *subtop; const char *val; const char *unit; } h_items[] = {
        {"Delivered Thermal Output", "health/output/state", v_out, "kW"},
        {"Thermal Output Demand", "health/required_output/state", v_req, "kW"},
        {"Delivery Percentage", "health/output_pct/state", v_pct, "%"},
        {"Coil Flow % of Design", "health/flow_pct/state", v_flow, "%"},
        {"Return Air Temperature", "health/return_air/state", v_ret, "°C"},
        {"Air Delta Across Coil", "health/air_delta/state", v_delta, "°C"},
    };
    for (size_t i = 0; i < sizeof(h_items)/sizeof(h_items[0]); i++) {
        len = snprintf(chunk, sizeof(chunk),
                       ",{\"name\":\"%s\",\"group\":\"health\",\"type\":\"sensor (%s)\","
                       "\"unique_id\":\"%s_%s\",\"topic\":\"%s/%s\","
                       "\"value\":\"%s\",\"removable\":false,\"enabled\":%s}",
                       h_items[i].name, h_items[i].unit,
                       HaDeviceId, h_items[i].subtop,
                       MqttTopicBase, h_items[i].subtop,
                       h_items[i].val,
                       HaHealthDiscoveryEnabled ? "true" : "false");
        httpd_resp_send_chunk(req, chunk, len);
    }

    /* Fan Health Entities */
    float fcnt_val = 0.0f;
    int fcnt = 2;
    if (BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, HEALTH_FAN_COUNT_INSTANCE, PROP_PRESENT_VALUE, &fcnt_val)) {
        fcnt = (int)(fcnt_val + 0.5f);
        if (fcnt < 1) fcnt = 1;
        if (fcnt > HEALTH_FAN_SUPPLY_AIR_COUNT) fcnt = HEALTH_FAN_SUPPLY_AIR_COUNT;
    }
    for (int f = 1; f <= fcnt; f++) {
        float ftemp = 0.0f, fspd = 0.0f;
        bool ft_ok = BacnetReady && read_real_property(OBJECT_ANALOG_INPUT, f, PROP_PRESENT_VALUE, &ftemp);
        bool fs_ok = (f <= HEALTH_FAN_SPEED_COUNT) && BacnetReady && read_real_property(OBJECT_ANALOG_OUTPUT, f, PROP_PRESENT_VALUE, &fspd);

        char ft_val[32], fs_val[32];
        if (ft_ok && temp_is_plausible(ftemp)) snprintf(ft_val, sizeof(ft_val), "%.1f°C", ftemp);
        else snprintf(ft_val, sizeof(ft_val), "Unavailable");

        if (fs_ok) snprintf(fs_val, sizeof(fs_val), "%.1f%%", fspd);
        else snprintf(fs_val, sizeof(fs_val), "Unavailable");

        char fname_t[48], fname_s[48], fsub_t[48], fsub_s[48];
        snprintf(fname_t, sizeof(fname_t), "Fan %d Supply Air Temp", f);
        snprintf(fname_s, sizeof(fname_s), "Fan %d Speed", f);
        snprintf(fsub_t, sizeof(fsub_t), "health/fan%d/supply_air/state", f);
        snprintf(fsub_s, sizeof(fsub_s), "health/fan%d/speed/state", f);

        len = snprintf(chunk, sizeof(chunk),
                       ",{\"name\":\"%s\",\"group\":\"health\",\"type\":\"sensor (°C)\","
                       "\"unique_id\":\"%s_health_fan%d_sa\",\"topic\":\"%s/%s\","
                       "\"value\":\"%s\",\"removable\":false,\"enabled\":%s}",
                       fname_t, HaDeviceId, f, MqttTopicBase, fsub_t, ft_val,
                       HaHealthDiscoveryEnabled ? "true" : "false");
        httpd_resp_send_chunk(req, chunk, len);

        len = snprintf(chunk, sizeof(chunk),
                       ",{\"name\":\"%s\",\"group\":\"health\",\"type\":\"sensor (%%)\","
                       "\"unique_id\":\"%s_health_fan%d_spd\",\"topic\":\"%s/%s\","
                       "\"value\":\"%s\",\"removable\":false,\"enabled\":%s}",
                       fname_s, HaDeviceId, f, MqttTopicBase, fsub_s, fs_val,
                       HaHealthDiscoveryEnabled ? "true" : "false");
        httpd_resp_send_chunk(req, chunk, len);
    }

    /* Custom Group */
    for (size_t i = 0; i < CustomMqttCount; i++) {
        char val_str[32] = "Unavailable";
        if (CustomMqttPoints[i].obj_type == OBJECT_ANALOG_VALUE ||
            CustomMqttPoints[i].obj_type == OBJECT_ANALOG_INPUT ||
            CustomMqttPoints[i].obj_type == OBJECT_ANALOG_OUTPUT) {
            float val = 0.0f;
            if (BacnetReady && read_real_property((BACNET_OBJECT_TYPE)CustomMqttPoints[i].obj_type, CustomMqttPoints[i].instance, PROP_PRESENT_VALUE, &val)) {
                snprintf(val_str, sizeof(val_str), "%.2f", val);
            }
        } else if (CustomMqttPoints[i].obj_type == OBJECT_BINARY_VALUE ||
                   CustomMqttPoints[i].obj_type == OBJECT_BINARY_INPUT ||
                   CustomMqttPoints[i].obj_type == OBJECT_BINARY_OUTPUT) {
            bool bval = false;
            if (BacnetReady && read_bool_property((BACNET_OBJECT_TYPE)CustomMqttPoints[i].obj_type, CustomMqttPoints[i].instance, PROP_PRESENT_VALUE, &bval)) {
                snprintf(val_str, sizeof(val_str), "%s", bval ? "ON" : "OFF");
            }
        }
        char name_esc[64];
        json_escape(name_esc, CustomMqttPoints[i].name, sizeof(name_esc));
        char subtop[64];
        snprintf(subtop, sizeof(subtop), "custom/%u_%u/state", CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance);
        len = snprintf(chunk, sizeof(chunk),
                       ",{\"name\":\"%s\",\"group\":\"custom\",\"type\":\"%s (%s #%u)\","
                       "\"unique_id\":\"%s_cust_%u_%u\",\"topic\":\"%s/%s\","
                       "\"value\":\"%s\",\"removable\":true,\"enabled\":%s,\"obj_type\":%u,\"instance\":%u,\"component\":\"%s\"}",
                       name_esc, CustomMqttPoints[i].component, CustomMqttPoints[i].type_str,
                       (unsigned)CustomMqttPoints[i].instance,
                       HaDeviceId, CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance,
                       MqttTopicBase, subtop, val_str,
                       CustomMqttPoints[i].enabled ? "true" : "false",
                       (unsigned)CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance,
                       CustomMqttPoints[i].component);
        httpd_resp_send_chunk(req, chunk, len);
    }

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
static const httpd_uri_t api_mqtt_entities_uri = {
    .uri = "/api/mqtt/entities", .method = HTTP_GET, .handler = api_mqtt_entities_get_handler};

static bool test_tcp_connection(const char *host, uint16_t port, uint32_t timeout_ms)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return false;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        freeaddrinfo(res);
        return false;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    close(sock);
    freeaddrinfo(res);
    return ret == 0;
}

static esp_err_t api_mqtt_test_handler(httpd_req_t *req)
{
    char body[256] = {0};
    recv_body(req, body, sizeof(body));
    char host[64] = {0};
    int port = 0;
    if (!json_get_str(body, "host", host, sizeof(host)) || strlen(host) == 0) {
        strlcpy(host, MqttBrokerHost, sizeof(host));
    }
    if (!json_get_int(body, "port", &port) || port <= 0) {
        port = MqttBrokerPort;
    }

    bool ok = test_tcp_connection(host, (uint16_t)port, 3000);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"host\":\"%s\",\"port\":%u}", ok ? "true" : "false", host, (unsigned)port);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_mqtt_test_uri = {
    .uri = "/api/mqtt/test", .method = HTTP_POST, .handler = api_mqtt_test_handler};

/* Best-effort broker discovery via mDNS service browsing (_mqtt._tcp) -
   only finds brokers that actively announce themselves (e.g. Home
   Assistant's Mosquitto add-on does this by default; a bare `mosquitto`
   install, including the one this firmware talks to on the Pi, does NOT
   unless an Avahi service file is added for it). Absence of results here
   doesn't mean no broker exists on the LAN, just that none advertised -
   the MQTT step's manual host field is still the fallback either way.
   Connected-mode only (mdns_start_service() never runs in provisioning-AP
   mode - see app_main), so this is only registered on that server. */
static esp_err_t api_mqtt_scan_handler(httpd_req_t *req)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_mqtt", "_tcp", 3000, 10, &results);

    char *buf = malloc(1024);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t off = (size_t)snprintf(buf, 1024, "{\"ok\":%s,\"brokers\":[", err == ESP_OK ? "true" : "false");

    bool first = true;
    for (mdns_result_t *r = results; r && off < 900; r = r->next) {
        char ip[48] = "";
        if (r->addr && r->addr->addr.type == ESP_IPADDR_TYPE_V4) {
            esp_ip4addr_ntoa(&r->addr->addr.u_addr.ip4, ip, sizeof(ip));
        } else if (r->hostname) {
            snprintf(ip, sizeof(ip), "%s.local", r->hostname);
        }
        if (ip[0] == '\0') {
            continue;
        }
        char name_escaped[64] = "";
        if (r->instance_name) {
            json_escape(name_escaped, r->instance_name, sizeof(name_escaped));
        }
        off += (size_t)snprintf(
            buf + off, 1024 - off, "%s{\"host\":\"%s\",\"port\":%u,\"name\":\"%s\"}",
            first ? "" : ",", ip, (unsigned)r->port, name_escaped);
        first = false;
    }
    off += (size_t)snprintf(buf + off, 1024 - off, "]}");

    if (results) {
        mdns_query_results_free(results);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    return ESP_OK;
}
static const httpd_uri_t api_mqtt_scan_uri = {
    .uri = "/api/mqtt/scan", .method = HTTP_GET, .handler = api_mqtt_scan_handler};

static esp_err_t api_mqtt_republish_handler(httpd_req_t *req)
{
    mqtt_publish_discovery();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_mqtt_republish_uri = {
    .uri = "/api/mqtt/republish", .method = HTTP_POST, .handler = api_mqtt_republish_handler};

static esp_err_t api_wizard_finish_handler(httpd_req_t *req)
{
    /* Carries target + rooms + MQTT config in one payload; a fixed
     * body[1536] would silently truncate (recv_body has no overflow check,
     * confirmed live on the same pattern in api_rooms_post_handler above)
     * once room count/name length push it past that. recv_full_body scales
     * with the actual request instead of a guessed constant. */
    char *body = NULL;
    size_t body_len = 0;
    if (!recv_full_body(req, &body, &body_len) || !body) {
        if (body) free(body);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char tip[16] = {0};
    int tport = 0, tdev = 0;
    if (json_get_str(body, "target_ip", tip, sizeof(tip)) && strlen(tip) > 0) {
        strlcpy(TargetIp, tip, sizeof(TargetIp));
        /* A manually (re)entered target is unconfirmed until Discover runs
         * again - carrying over a previous target's confirmed name here
         * would attach the wrong device's name to this one. */
        TargetDeviceNameValid = false;
        TargetDeviceName[0] = '\0';
    }
    if (json_get_int(body, "target_port", &tport) && tport > 0) {
        TargetPort = (uint16_t)tport;
    }
    if (json_get_int(body, "target_device_id", &tdev) && tdev > 0) {
        TargetDeviceInstance = (uint32_t)tdev;
    }
    target_config_save();

    parse_rooms_from_json(body);
    rooms_config_save();

    char mhost[64] = {0}, muser[32] = {0}, mpass[64] = {0};
    char mtop[64] = {0}, mname[48] = {0}, mid[32] = {0};
    int mport = 0;
    bool mdisc = true;

    if (json_get_str(body, "mqtt_host", mhost, sizeof(mhost)) && strlen(mhost) > 0) {
        strlcpy(MqttBrokerHost, mhost, sizeof(MqttBrokerHost));
    }
    if (json_get_int(body, "mqtt_port", &mport) && mport > 0) {
        MqttBrokerPort = (uint16_t)mport;
    }
    if (json_get_str(body, "mqtt_user", muser, sizeof(muser))) {
        strlcpy(MqttBrokerUser, muser, sizeof(MqttBrokerUser));
    }
    if (json_get_str(body, "mqtt_pass", mpass, sizeof(mpass))) {
        strlcpy(MqttBrokerPass, mpass, sizeof(MqttBrokerPass));
    }
    if (json_get_str(body, "mqtt_base_topic", mtop, sizeof(mtop)) && strlen(mtop) > 0) {
        strlcpy(MqttTopicBase, mtop, sizeof(MqttTopicBase));
    }
    if (json_get_str(body, "ha_device_name", mname, sizeof(mname)) && strlen(mname) > 0) {
        strlcpy(HaDeviceName, mname, sizeof(HaDeviceName));
    }
    if (json_get_str(body, "ha_device_id", mid, sizeof(mid)) && strlen(mid) > 0) {
        strlcpy(HaDeviceId, mid, sizeof(HaDeviceId));
    }
    if (json_get_bool(body, "ha_discovery", &mdisc)) {
        HaDiscoveryEnabled = mdisc;
    }
    mqtt_config_save();

    char integ_mode[32] = {0};
    hvac_integration_kind_t chosen_mode = HVAC_INTEGRATION_MQTT_HOME_ASSISTANT;
    if (json_get_str(body, "integration_mode", integ_mode, sizeof(integ_mode))) {
        if (strcmp(integ_mode, "none") == 0) chosen_mode = HVAC_INTEGRATION_NONE;
        else if (strcmp(integ_mode, "matter") == 0) chosen_mode = HVAC_INTEGRATION_MATTER;
    }
    if (!integration_store(chosen_mode)) {
        free(body);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Only start the transport the user actually selected - each module is
     * self-contained and must not assume it is the only one configured. */
    if (chosen_mode == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT) {
        mqtt_app_start();
    } else if (chosen_mode == HVAC_INTEGRATION_MATTER) {
        matter_adapter_init();
        matter_adapter_start();
    }
    wizard_completed_save();
    free(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_wizard_finish_uri = {
    .uri = "/api/wizard/finish", .method = HTTP_POST, .handler = api_wizard_finish_handler};

static void delayed_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    vTaskDelete(NULL);
}

static esp_err_t api_config_export_handler(httpd_req_t *req)
{
    char wifi_ssid[33] = "";
    char wifi_pass[65] = "";
    nvs_handle_t whandle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &whandle) == ESP_OK) {
        size_t len = sizeof(wifi_ssid);
        nvs_get_str(whandle, NVS_KEY_SSID, wifi_ssid, &len);
        len = sizeof(wifi_pass);
        nvs_get_str(whandle, NVS_KEY_PASS, wifi_pass, &len);
        nvs_close(whandle);
    }

    char *buf = malloc(6144);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char wifi_ssid_esc[64] = "", wifi_pass_esc[128] = "";
    char ota_pw_esc[128] = "";
    char tip_esc[32] = "", mhost_esc[128] = "", muser_esc[64] = "", mpass_esc[128] = "";
    char mtop_esc[128] = "", mdev_esc[96] = "", mid_esc[64] = "";

    json_escape(wifi_ssid_esc, wifi_ssid, sizeof(wifi_ssid_esc));
    json_escape(wifi_pass_esc, wifi_pass, sizeof(wifi_pass_esc));
    json_escape(ota_pw_esc, OtaPassword, sizeof(ota_pw_esc));
    json_escape(tip_esc, TargetIp, sizeof(tip_esc));
    json_escape(mhost_esc, MqttBrokerHost, sizeof(mhost_esc));
    json_escape(muser_esc, MqttBrokerUser, sizeof(muser_esc));
    json_escape(mpass_esc, MqttBrokerPass, sizeof(mpass_esc));
    json_escape(mtop_esc, MqttTopicBase, sizeof(mtop_esc));
    json_escape(mdev_esc, HaDeviceName, sizeof(mdev_esc));
    json_escape(mid_esc, HaDeviceId, sizeof(mid_esc));

    size_t off = snprintf(
        buf, 6144,
        "{\n"
        "  \"version\": 1,\n"
        "  \"wizard_done\": %s,\n"
        "  \"target\": {\n"
        "    \"ip\": \"%s\",\n"
        "    \"port\": %u,\n"
        "    \"dev_id\": %u\n"
        "  },\n"
        "  \"wifi\": {\n"
        "    \"ssid\": \"%s\",\n"
        "    \"pass\": \"%s\"\n"
        "  },\n"
        "  \"ota\": {\n"
        "    \"password\": \"%s\"\n"
        "  },\n"
        "  \"rooms\": [\n",
        WizardCompleted ? "true" : "false",
        tip_esc, (unsigned)TargetPort, (unsigned)TargetDeviceInstance,
        wifi_ssid_esc, wifi_pass_esc,
        ota_pw_esc);

    for (size_t i = 0; i < RoomCount; i++) {
        char rname_esc[64] = "";
        json_escape(rname_esc, Rooms[i].name, sizeof(rname_esc));
        off += snprintf(
            buf + off, 6144 - off,
            "    {\"id\": %u, \"name\": \"%s\", \"active\": %s, "
            "\"setpoint_instance\": %u, \"temperature_instance\": %u, \"power_instance\": %u, "
            "\"supply_air_instance\": %u, \"required_output_instance\": %u, \"current_output_instance\": %u}%s\n",
            (unsigned)i, rname_esc, Rooms[i].active ? "true" : "false",
            (unsigned)Rooms[i].setpoint_instance, (unsigned)Rooms[i].temperature_instance,
            (unsigned)Rooms[i].power_instance, (unsigned)Rooms[i].supply_air_instance,
            (unsigned)Rooms[i].required_output_instance, (unsigned)Rooms[i].current_output_instance,
            (i + 1 < RoomCount) ? "," : "");
    }

    off += snprintf(
        buf + off, 6144 - off,
        "  ],\n"
        "  \"mqtt\": {\n"
        "    \"host\": \"%s\",\n"
        "    \"port\": %u,\n"
        "    \"user\": \"%s\",\n"
        "    \"pass\": \"%s\",\n"
        "    \"base_topic\": \"%s\",\n"
        "    \"ha_device_name\": \"%s\",\n"
        "    \"ha_device_id\": \"%s\",\n"
        "    \"ha_discovery\": %s,\n"
        "    \"ha_health_discovery\": %s\n"
        "  },\n"
        "  \"custom_mqtt\": [\n",
        mhost_esc, (unsigned)MqttBrokerPort, muser_esc, mpass_esc,
        mtop_esc, mdev_esc, mid_esc,
        HaDiscoveryEnabled ? "true" : "false",
        HaHealthDiscoveryEnabled ? "true" : "false");

    for (size_t i = 0; i < CustomMqttCount; i++) {
        char pname_esc[64] = "", pcomp_esc[32] = "", ptstr_esc[32] = "";
        json_escape(pname_esc, CustomMqttPoints[i].name, sizeof(pname_esc));
        json_escape(pcomp_esc, CustomMqttPoints[i].component, sizeof(pcomp_esc));
        json_escape(ptstr_esc, CustomMqttPoints[i].type_str, sizeof(ptstr_esc));
        off += snprintf(
            buf + off, 6144 - off,
            "    {\"type\": \"%s\", \"instance\": %u, \"name\": \"%s\", \"component\": \"%s\", \"enabled\": %s}%s\n",
            ptstr_esc, (unsigned)CustomMqttPoints[i].instance, pname_esc, pcomp_esc,
            CustomMqttPoints[i].enabled ? "true" : "false",
            (i + 1 < CustomMqttCount) ? "," : "");
    }

    off += snprintf(
        buf + off, 6144 - off,
        "  ],\n"
        "  \"app\": {\n"
        "    \"boost_timeout_min\": %u,\n"
        "    \"boost_revert_external\": %s\n"
        "  }\n"
        "}\n",
        (unsigned)BoostTimeoutMinutes,
        BoostRevertExternal ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"esp32_bacnet_config.json\"");
    httpd_resp_send(req, buf, off);
    free(buf);
    return ESP_OK;
}
static const httpd_uri_t api_config_export_uri = {
    .uri = "/api/config/export", .method = HTTP_GET, .handler = api_config_export_handler};

static esp_err_t api_config_import_handler(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    if (!recv_full_body(req, &body, &body_len) || !body || body_len == 0) {
        if (body) free(body);
        send_bad_request(req, "{\"ok\":false,\"error\":\"empty or invalid payload\"}");
        return ESP_OK;
    }

    bool has_wifi_update = false;
    bool reboot_required = false;

    /* 1. Target settings */
    char tgt_obj[256] = {0};
    char tip[32] = {0};
    int tport = 0, tdev = 0;
    if (json_extract_obj(body, "target", tgt_obj, sizeof(tgt_obj))) {
        json_get_str(tgt_obj, "ip", tip, sizeof(tip));
        json_get_int(tgt_obj, "port", &tport);
        json_get_int(tgt_obj, "dev_id", &tdev);
    }
    if (tip[0] == '\0') json_get_str(body, "target_ip", tip, sizeof(tip));
    if (tport <= 0) json_get_int(body, "target_port", &tport);
    if (tdev <= 0) json_get_int(body, "target_device_id", &tdev);

    if (strlen(tip) > 0) {
        strlcpy(TargetIp, tip, sizeof(TargetIp));
        /* Same reasoning as the wizard finish handler: a restored/edited
         * target is unconfirmed until Discover runs again. */
        TargetDeviceNameValid = false;
        TargetDeviceName[0] = '\0';
    }
    if (tport > 0) TargetPort = (uint16_t)tport;
    if (tdev > 0) TargetDeviceInstance = (uint32_t)tdev;
    target_config_save();

    /* 2. WiFi settings */
    char wifi_obj[256] = {0};
    char w_ssid[33] = {0}, w_pass[65] = {0};
    if (json_extract_obj(body, "wifi", wifi_obj, sizeof(wifi_obj))) {
        json_get_str(wifi_obj, "ssid", w_ssid, sizeof(w_ssid));
        json_get_str(wifi_obj, "pass", w_pass, sizeof(w_pass));
    }
    if (w_ssid[0] == '\0') {
        json_get_str(body, "ssid", w_ssid, sizeof(w_ssid));
        json_get_str(body, "password", w_pass, sizeof(w_pass));
    }
    if (strlen(w_ssid) > 0) {
        if (save_wifi_credentials(w_ssid, w_pass) == ESP_OK && wizard_completed_save()) {
            has_wifi_update = true;
        } else {
            free(body);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    /* 3. OTA password */
    char ota_obj[128] = {0};
    char ota_pw[OTA_PASSWORD_MAX_LEN + 1] = {0};
    if (json_extract_obj(body, "ota", ota_obj, sizeof(ota_obj))) {
        json_get_str(ota_obj, "password", ota_pw, sizeof(ota_pw));
    }
    if (strlen(ota_pw) >= OTA_PASSWORD_MIN_LEN) {
        ota_password_store(ota_pw);
    }

    /* 4. Rooms */
    parse_rooms_from_json(body);
    rooms_config_save();

    /* 5. MQTT settings */
    char mqtt_obj[512] = {0};
    char mhost[64] = {0}, muser[32] = {0}, mpass[64] = {0};
    char mtop[64] = {0}, mname[48] = {0}, mid[32] = {0};
    int mport = 0;
    bool mdisc = true, mhdisc = true;
    const char *mq_src = json_extract_obj(body, "mqtt", mqtt_obj, sizeof(mqtt_obj)) ? mqtt_obj : body;

    if (json_get_str(mq_src, "host", mhost, sizeof(mhost)) || json_get_str(body, "mqtt_host", mhost, sizeof(mhost))) {
        strlcpy(MqttBrokerHost, mhost, sizeof(MqttBrokerHost));
    }
    if (json_get_int(mq_src, "port", &mport) || json_get_int(body, "mqtt_port", &mport)) {
        if (mport > 0) MqttBrokerPort = (uint16_t)mport;
    }
    if (json_get_str(mq_src, "user", muser, sizeof(muser)) || json_get_str(body, "mqtt_user", muser, sizeof(muser))) {
        strlcpy(MqttBrokerUser, muser, sizeof(MqttBrokerUser));
    }
    if (json_get_str(mq_src, "pass", mpass, sizeof(mpass)) || json_get_str(body, "mqtt_pass", mpass, sizeof(mpass))) {
        strlcpy(MqttBrokerPass, mpass, sizeof(MqttBrokerPass));
    }
    if (json_get_str(mq_src, "base_topic", mtop, sizeof(mtop)) || json_get_str(body, "mqtt_base_topic", mtop, sizeof(mtop)) || json_get_str(mq_src, "base_top", mtop, sizeof(mtop))) {
        strlcpy(MqttTopicBase, mtop, sizeof(MqttTopicBase));
    }
    if (json_get_str(mq_src, "ha_device_name", mname, sizeof(mname)) || json_get_str(body, "ha_device_name", mname, sizeof(mname))) {
        strlcpy(HaDeviceName, mname, sizeof(HaDeviceName));
    }
    if (json_get_str(mq_src, "ha_device_id", mid, sizeof(mid)) || json_get_str(body, "ha_device_id", mid, sizeof(mid))) {
        strlcpy(HaDeviceId, mid, sizeof(HaDeviceId));
    }
    if (json_get_bool(mq_src, "ha_discovery", &mdisc) || json_get_bool(body, "ha_discovery", &mdisc)) {
        HaDiscoveryEnabled = mdisc;
    }
    if (json_get_bool(mq_src, "ha_health_discovery", &mhdisc)) {
        HaHealthDiscoveryEnabled = mhdisc;
    }
    mqtt_config_save();
    mqtt_app_start();
    mqtt_publish_discovery();

    /* 6. Custom MQTT points */
    parse_custom_mqtt_from_json(body);
    custom_mqtt_save();

    /* 7. App / Boost settings */
    char app_obj[128] = {0};
    int b_min = 0;
    bool b_ext = false;
    if (json_extract_obj(body, "app", app_obj, sizeof(app_obj))) {
        if (json_get_int(app_obj, "boost_timeout_min", &b_min)) {
            BoostTimeoutMinutes = (uint32_t)b_min;
        }
        if (json_get_bool(app_obj, "boost_revert_external", &b_ext)) {
            BoostRevertExternal = b_ext;
        }
        app_config_save();
    }

    /* 8. Wizard completion */
    bool w_done = false;
    if (json_get_bool(body, "wizard_done", &w_done) && w_done) {
        wizard_completed_save();
    } else {
        wizard_completed_save();
    }

    free(body);

    /* If we were in SoftAP mode or WiFi credentials changed, a reboot joins the restored network */
    if (has_wifi_update || StaIp[0] == '\0') {
        reboot_required = true;
        spawn_task(delayed_reboot_task, "delay_reboot", 2048, NULL, 5, NULL);
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"reboot_required\":%s}", reboot_required ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static const httpd_uri_t api_config_import_uri = {
    .uri = "/api/config/import", .method = HTTP_POST, .handler = api_config_import_handler};

static void erase_namespace(const char *ns)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static bool factory_reset_direct(void *arg)
{
    (void)arg;
    erase_namespace(NVS_NAMESPACE);
    erase_namespace(NVS_TARGET_NAMESPACE);
    erase_namespace(NVS_ROOMS_NAMESPACE);
    erase_namespace(NVS_MQTT_NAMESPACE);
    erase_namespace(NVS_CUSTOM_MQTT_NAMESPACE);
    erase_namespace(NVS_APP_NAMESPACE);
    erase_namespace(OTA_NVS_NAMESPACE);
    hvac_core_integration_reset();
    return true;
}

static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    mqtt_unpublish_discovery();
    if (!run_internal_flash_op(factory_reset_direct, NULL)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
static const httpd_uri_t api_factory_reset_uri = {
    .uri = "/api/factory-reset", .method = HTTP_POST, .handler = api_factory_reset_handler};

/* ===================== Web Server Life Cycle ===================== */

/* These callbacks are deliberately allocation-free: the Matter profile can
 * accept TCP connections while its dashboard stops dispatching requests. They
 * establish whether that failure is before or after HTTPD's session layer. */
static esp_err_t http_session_open_trace(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ESP_LOGI(TAG_WIFI, "HTTP session opened fd=%d; stack HWM=%u bytes", sockfd,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ESP_OK;
}

static void http_session_close_trace(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ESP_LOGI(TAG_WIFI, "HTTP session closed fd=%d", sockfd);
}

static esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char chunk[512];
    taskENTER_CRITICAL(&LogMux);
    size_t count = LogCount;
    size_t start = (LogHead + LOG_BUFFER_SIZE - count) % LOG_BUFFER_SIZE;
    taskEXIT_CRITICAL(&LogMux);

    size_t sent = 0;
    while (sent < count) {
        size_t chunk_len = count - sent;
        if (chunk_len > sizeof(chunk) - 1) chunk_len = sizeof(chunk) - 1;
        taskENTER_CRITICAL(&LogMux);
        for (size_t i = 0; i < chunk_len; i++) {
            chunk[i] = LogBuffer[(start + sent + i) % LOG_BUFFER_SIZE];
        }
        taskEXIT_CRITICAL(&LogMux);
        chunk[chunk_len] = '\0';
        if (httpd_resp_send_chunk(req, chunk, chunk_len) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        sent += chunk_len;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
static const httpd_uri_t api_logs_uri = {
    .uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_get_handler};

static httpd_handle_t start_connected_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* 13 -> 8. Each open socket costs lwIP control blocks and an httpd table
       slot. The dashboard opens a handful at a time; 13 was sized for headroom
       this board does not have (measured 3-12K free heap with everything up). */
    config.max_open_sockets = 8;
    config.lru_purge_enable = true;
    /* Restored to 24576 on 2026-08-24. The 2026-08-23 trim to 16384 (to save
       8KB of fixed heap on this no-PSRAM board) was too aggressive: loading
       /api/status followed by /api/bacnet/read reliably panicked with
       "***ERROR*** A stack overflow in task httpd has been detected."
       The comment then said to "watch for stack-overflow panics ... and raise
       it back if so" - this is that. The BACnet read path nests MAX_APDU=1476
       buffers across bip_receive/rp_ack_decode/bacapp_decode, which is exactly
       why bacnet_client_task also gets 24576; a handler doing ~20 of those
       sequentially needs comparable headroom. The `done` diag_log lines now
       report this task's stack high-water mark so the real margin is
       measurable instead of guessed - tune from that, not from intuition. */
    config.stack_size = CONNECTED_HTTP_STACK_SIZE;
    /* HTTPD needs 32 KiB on this board for the BACnet decode handlers, but at
     * connected-mode startup the largest internal block is only 14 KiB.  The
     * task itself has no ISR use; place its stack in PSRAM (enabled by
     * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) and retain internal memory
     * for Wi-Fi/lwIP and Matter control structures. */
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    /* ESP-IDF HTTPD defaults to priority 5, the same priority used by the
     * Matter and BACnet workers. On the live Matter build TCP completed its
     * handshake but HTTPD never reached its session callback. Keep it one
     * level above those background workers so dashboard requests are drained
     * promptly, while Wi-Fi/lwIP still run at their much higher priorities. */
    config.task_priority = 6;
    /* Keep the dashboard worker off CPU0, where Wi-Fi/lwIP and the Matter
     * platform event path run.  On the live Matter image, TCP handshakes
     * complete but HTTPD's open callback never runs; pinning this independent
     * select()/handler loop to CPU1 tests scheduler starvation directly. */
    config.core_id = 1;
    config.max_uri_handlers = 60;
    config.open_fn = http_session_open_trace;
    config.close_fn = http_session_close_trace;

    ESP_LOGI(TAG_WIFI, "Starting connected-mode dashboard server on port: '%d'", config.server_port);
    esp_err_t httpd_err = httpd_start(&server, &config);
    if (httpd_err == ESP_OK) {
        ESP_LOGI(TAG_WIFI, "Connected-mode HTTPD started (core=%d priority=%u stack=%uB; heap=%uB largest=%uB)",
                 (int)config.core_id, (unsigned)config.task_priority, (unsigned)config.stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        httpd_register_uri_handler(server, &manage_uri);
        httpd_register_uri_handler(server, &status_page_uri);
        httpd_register_uri_handler(server, &health_page_uri);
        httpd_register_uri_handler(server, &reset_page_uri);
        httpd_register_uri_handler(server, &update_page_uri);
        httpd_register_uri_handler(server, &objects_page_uri);
        httpd_register_uri_handler(server, &wizard_page_uri);
        httpd_register_uri_handler(server, &smart_home_page_uri);
        httpd_register_uri_handler(server, &api_network_uri);
        httpd_register_uri_handler(server, &api_version_uri);
        httpd_register_uri_handler(server, &api_status_uri);
        httpd_register_uri_handler(server, &api_health_uri);
        httpd_register_uri_handler(server, &api_room_setpoint_uri);
        httpd_register_uri_handler(server, &api_room_power_uri);
        httpd_register_uri_handler(server, &api_system_power_uri);
        httpd_register_uri_handler(server, &api_boost_uri);
        httpd_register_uri_handler(server, &api_boost_timeout_uri);
        httpd_register_uri_handler(server, &api_device_mdns_uri);
        httpd_register_uri_handler(server, &api_debug_stacks_uri);
        httpd_register_uri_handler(server, &api_bacnet_read_uri);
        httpd_register_uri_handler(server, &api_bacnet_write_uri);
        httpd_register_uri_handler(server, &api_wifi_reset_uri);
        httpd_register_uri_handler(server, &api_ota_uri);
        httpd_register_uri_handler(server, &api_setup_password_uri);
        httpd_register_uri_handler(server, &api_objects_scan_start_uri);
        httpd_register_uri_handler(server, &api_objects_scan_status_uri);
        httpd_register_uri_handler(server, &api_objects_uri);
        httpd_register_uri_handler(server, &api_objects_inspect_uri);
        httpd_register_uri_handler(server, &api_objects_batch_uri);
        httpd_register_uri_handler(server, &api_objects_custom_mqtt_get_uri);
        httpd_register_uri_handler(server, &api_objects_custom_mqtt_post_uri);
        httpd_register_uri_handler(server, &api_bacnet_discover_uri);
        httpd_register_uri_handler(server, &api_strategy_inspect_uri);
        httpd_register_uri_handler(server, &api_rooms_get_uri);
        httpd_register_uri_handler(server, &api_rooms_post_uri);
        httpd_register_uri_handler(server, &api_integration_get_uri);
        httpd_register_uri_handler(server, &api_integration_post_uri);
        httpd_register_uri_handler(server, &api_matter_setup_get_uri);
        httpd_register_uri_handler(server, &api_matter_retry_pairing_uri);
        httpd_register_uri_handler(server, &api_matter_close_pairing_uri);
        httpd_register_uri_handler(server, &api_mqtt_config_get_uri);
        httpd_register_uri_handler(server, &api_mqtt_config_post_uri);
        httpd_register_uri_handler(server, &api_mqtt_entities_uri);
        httpd_register_uri_handler(server, &api_mqtt_test_uri);
        httpd_register_uri_handler(server, &api_mqtt_scan_uri);
        httpd_register_uri_handler(server, &api_mqtt_republish_uri);
        httpd_register_uri_handler(server, &api_wizard_finish_uri);
        httpd_register_uri_handler(server, &api_factory_reset_uri);
        httpd_register_uri_handler(server, &api_config_export_uri);
        httpd_register_uri_handler(server, &api_config_import_uri);
        httpd_register_uri_handler(server, &api_logs_uri);
    } else {
        ESP_LOGE(TAG_WIFI, "Connected-mode HTTPD failed to start: %s (core=%d priority=%u stack=%uB; heap=%uB largest=%uB)",
                 esp_err_to_name(httpd_err), (int)config.core_id, (unsigned)config.task_priority,
                 (unsigned)config.stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    }
    return server;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* 13 -> 8. Each open socket costs lwIP control blocks and an httpd table
       slot. The dashboard opens a handful at a time; 13 was sized for headroom
       this board does not have (measured 3-12K free heap with everything up). */
    config.max_open_sockets = 8;
    config.lru_purge_enable = true;
    config.stack_size = 24576;
    config.max_uri_handlers = 30;

    ESP_LOGI(TAG_WIFI, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &scan_uri);
        httpd_register_uri_handler(server, &connect_uri);
        httpd_register_uri_handler(server, &wizard_page_uri);
        httpd_register_uri_handler(server, &api_network_uri);
        /* Setup flow exposes the mDNS toggle on the WiFi page (root.html). */
        httpd_register_uri_handler(server, &api_device_mdns_uri);
        httpd_register_uri_handler(server, &api_bacnet_discover_uri);
        httpd_register_uri_handler(server, &api_strategy_inspect_uri);
        httpd_register_uri_handler(server, &api_rooms_get_uri);
        httpd_register_uri_handler(server, &api_rooms_post_uri);
        httpd_register_uri_handler(server, &api_integration_get_uri);
        httpd_register_uri_handler(server, &api_integration_post_uri);
        httpd_register_uri_handler(server, &api_matter_setup_get_uri);
        httpd_register_uri_handler(server, &api_matter_retry_pairing_uri);
        httpd_register_uri_handler(server, &api_matter_close_pairing_uri);
        httpd_register_uri_handler(server, &api_mqtt_config_get_uri);
        httpd_register_uri_handler(server, &api_mqtt_config_post_uri);
        httpd_register_uri_handler(server, &api_mqtt_test_uri);
        httpd_register_uri_handler(server, &api_wizard_finish_uri);
        httpd_register_uri_handler(server, &api_setup_password_uri);
        httpd_register_uri_handler(server, &api_config_export_uri);
        httpd_register_uri_handler(server, &api_config_import_uri);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
    }
    return server;
}

static void start_provisioning_ap(void)
{
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    esp_netif_create_default_wifi_ap();
    wifi_init_softap();
    start_webserver();

    dns_server_config_t config =
        DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    start_dns_server(&config);

    ESP_LOGI(
        TAG_WIFI, "Provisioning AP ready - connect to '%s' and open any website",
        EXAMPLE_ESP_WIFI_SSID);
}

/* ===================== MQTT / Home Assistant ===================== */

typedef struct {
    char topic[128];
    char data[64];
} mqtt_command_t;
static QueueHandle_t MqttCommandQueue;

static void mqtt_publish_discovery(void)
{
    if (!MqttClient || !MqttConnected || !HaDiscoveryEnabled) {
        return;
    }
    char *buf = malloc(2048);
    if (!buf) {
        return;
    }
    char avail_topic[96];
    snprintf(avail_topic, sizeof(avail_topic), "%s/status", MqttTopicBase);

    /* System Power switch */
    snprintf(
        buf, 2048,
        "{\"name\":\"System Power\",\"unique_id\":\"%s_system_power\","
        "\"availability_topic\":\"%s\",\"state_topic\":\"%s/system_power/state\","
        "\"command_topic\":\"%s/system_power/set\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device_class\":\"switch\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
        "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
        HaDeviceId, avail_topic, MqttTopicBase, MqttTopicBase,
        HaDeviceId, HaDeviceName);
    char disc_top[160];
    snprintf(disc_top, sizeof(disc_top), "homeassistant/switch/%s_system_power/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);

    /* Boost mode select */
    snprintf(
        buf, 2048,
        "{\"name\":\"Boost Mode\",\"unique_id\":\"%s_boost\","
        "\"availability_topic\":\"%s\",\"state_topic\":\"%s/boost/state\","
        "\"command_topic\":\"%s/boost/set\","
        "\"options\":[\"auto\",\"heat\",\"cool\"],"
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
        "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
        HaDeviceId, avail_topic, MqttTopicBase, MqttTopicBase,
        HaDeviceId, HaDeviceName);
    snprintf(disc_top, sizeof(disc_top), "homeassistant/select/%s_boost/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);

    /* Boost auto-revert timeout */
    snprintf(
        buf, 2048,
        "{\"name\":\"Boost Timeout\",\"unique_id\":\"%s_boost_timeout\","
        "\"availability_topic\":\"%s\",\"state_topic\":\"%s/boost_timeout/state\","
        "\"command_topic\":\"%s/boost_timeout/set\","
        "\"min\":0,\"max\":%d,\"step\":5,\"unit_of_measurement\":\"min\","
        "\"entity_category\":\"config\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
        "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
        HaDeviceId, avail_topic, MqttTopicBase, MqttTopicBase,
        BOOST_TIMEOUT_MAX_MIN, HaDeviceId, HaDeviceName);
    snprintf(disc_top, sizeof(disc_top), "homeassistant/number/%s_boost_timeout/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);

    snprintf(
        buf, 2048,
        "{\"name\":\"Boost Remaining\",\"unique_id\":\"%s_boost_remaining\","
        "\"availability_topic\":\"%s\",\"state_topic\":\"%s/boost_remaining/state\","
        "\"unit_of_measurement\":\"min\",\"entity_category\":\"diagnostic\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
        "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
        HaDeviceId, avail_topic, MqttTopicBase, HaDeviceId, HaDeviceName);
    snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_boost_remaining/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);

    /* Per-room native climate entities */
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;

        /* Clean up any old loose entity discovery topics from broker */
        snprintf(disc_top, sizeof(disc_top), "homeassistant/switch/%s_room%u_power/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/number/%s_room%u_setpoint/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_room%u_temp/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_room%u_sa/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_room%u_out/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_room%u_req/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);

        /* Publish the single native Climate entity */
        snprintf(
            buf, 2048,
            "{\"name\":\"%s\",\"unique_id\":\"%s_room%u\","
            "\"availability_topic\":\"%s\","
            "\"current_temperature_topic\":\"%s/room%u/temperature/state\","
            "\"temperature_state_topic\":\"%s/room%u/setpoint/state\","
            "\"temperature_command_topic\":\"%s/room%u/setpoint/set\","
            "\"mode_state_topic\":\"%s/room%u/mode/state\","
            "\"mode_command_topic\":\"%s/room%u/mode/set\","
            "\"action_topic\":\"%s/room%u/action/state\","
            "\"modes\":[\"off\",\"heat_cool\"],"
            "\"temperature_unit\":\"C\",\"min_temp\":18,\"max_temp\":30,\"temp_step\":0.5,\"precision\":0.1,"
            "\"preset_mode_state_topic\":\"%s/room%u/preset/state\","
            "\"preset_mode_command_topic\":\"%s/room%u/preset/set\","
            "\"preset_modes\":[\"boost_heat\",\"boost_cool\"],"
            "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
            "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
            Rooms[i].name, HaDeviceId, (unsigned)i,
            avail_topic,
            MqttTopicBase, (unsigned)i,
            MqttTopicBase, (unsigned)i,
            MqttTopicBase, (unsigned)i,
            MqttTopicBase, (unsigned)i,
            MqttTopicBase, (unsigned)i,
            MqttTopicBase, (unsigned)i,
            MqttTopicBase, (unsigned)i,
            MqttTopicBase, (unsigned)i,
            HaDeviceId, HaDeviceName);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/climate/%s_room%u/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);
    }

    /* Custom MQTT Points */
    for (size_t i = 0; i < CustomMqttCount; i++) {
        if (!CustomMqttPoints[i].enabled) continue;
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_cust_%u_%u",
                 HaDeviceId, CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance);
        char state_top[96];
        snprintf(state_top, sizeof(state_top), "%s/custom/%u_%u/state",
                 MqttTopicBase, CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance);

        snprintf(disc_top, sizeof(disc_top), "homeassistant/%s/%s/config",
                 CustomMqttPoints[i].component, unique_id);

        snprintf(
            buf, 2048,
            "{\"name\":\"%s\",\"unique_id\":\"%s\","
            "\"availability_topic\":\"%s\",\"state_topic\":\"%s\","
            "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
            "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
            CustomMqttPoints[i].name, unique_id, avail_topic, state_top,
            HaDeviceId, HaDeviceName);
        esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);
    }

    /* Health Group Discovery */
    float disc_fan_count_raw = 0.0f;
    bool disc_fan_count_valid = BacnetReady && read_real_property(
        OBJECT_ANALOG_VALUE, HEALTH_FAN_COUNT_INSTANCE, PROP_PRESENT_VALUE, &disc_fan_count_raw);
    int disc_fan_count = disc_fan_count_valid ? (int)(disc_fan_count_raw + 0.5f) : HEALTH_FAN_SPEED_COUNT;
    if (disc_fan_count < 0) disc_fan_count = 0;
    if (disc_fan_count > HEALTH_FAN_SPEED_COUNT) disc_fan_count = HEALTH_FAN_SPEED_COUNT;

    if (HaHealthDiscoveryEnabled) {
        struct { const char *name; const char *key; const char *subtop; const char *unit; const char *dev_cla; } h_discs[] = {
            {"Delivered Thermal Output", "output", "health/output/state", "kW", "power"},
            {"Thermal Output Demand", "required_output", "health/required_output/state", "kW", "power"},
            {"Thermal Output Delivery %", "output_pct", "health/output_pct/state", "%", ""},
            {"Coil Flow % of Design", "flow_pct", "health/flow_pct/state", "%", ""},
            {"Return Air Temperature", "return_air", "health/return_air/state", "°C", "temperature"},
            {"Air Delta Across Coil", "air_delta", "health/air_delta/state", "°C", "temperature_delta"},
        };
        for (size_t i = 0; i < sizeof(h_discs)/sizeof(h_discs[0]); i++) {
            snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_%s/config", HaDeviceId, h_discs[i].key);
            char dev_cla_json[48] = "";
            if (strlen(h_discs[i].dev_cla) > 0) {
                snprintf(dev_cla_json, sizeof(dev_cla_json), ",\"device_class\":\"%s\"", h_discs[i].dev_cla);
            }
            snprintf(
                buf, 2048,
                "{\"name\":\"%s\",\"unique_id\":\"%s_health_%s\","
                "\"availability_topic\":\"%s\",\"state_topic\":\"%s/%s\","
                "\"unit_of_measurement\":\"%s\"%s,"
                "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
                "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
                h_discs[i].name, HaDeviceId, h_discs[i].key,
                avail_topic, MqttTopicBase, h_discs[i].subtop,
                h_discs[i].unit, dev_cla_json,
                HaDeviceId, HaDeviceName);
            esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);
        }

        for (int f = 1; f <= HEALTH_FAN_SPEED_COUNT; f++) {
            snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_fan%d_sa/config", HaDeviceId, f);
            if (f > disc_fan_count) {
                esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
            } else {
                snprintf(
                    buf, 2048,
                    "{\"name\":\"Fan %d Supply Air Temp\",\"unique_id\":\"%s_health_fan%d_sa\","
                    "\"availability_topic\":\"%s\",\"state_topic\":\"%s/health/fan%d/supply_air/state\","
                    "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\","
                    "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
                    "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
                    f, HaDeviceId, f,
                    avail_topic, MqttTopicBase, f,
                    HaDeviceId, HaDeviceName);
                esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);
            }

            snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_fan%d_spd/config", HaDeviceId, f);
            if (f > disc_fan_count) {
                esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
            } else {
                snprintf(
                    buf, 2048,
                    "{\"name\":\"Fan %d Speed\",\"unique_id\":\"%s_health_fan%d_spd\","
                    "\"availability_topic\":\"%s\",\"state_topic\":\"%s/health/fan%d/speed/state\","
                    "\"unit_of_measurement\":\"%%\","
                    "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
                    "\"manufacturer\":\"Delta Controls\",\"model\":\"DAC_1180E-MB\"}}",
                    f, HaDeviceId, f,
                    avail_topic, MqttTopicBase, f,
                    HaDeviceId, HaDeviceName);
                esp_mqtt_client_publish(MqttClient, disc_top, buf, 0, 1, true);
            }
        }
    } else {
        const char *hlth_keys[] = {"output", "required_output", "output_pct", "flow_pct", "return_air", "air_delta"};
        for (size_t i = 0; i < sizeof(hlth_keys)/sizeof(hlth_keys[0]); i++) {
            snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_%s/config", HaDeviceId, hlth_keys[i]);
            esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        }
        for (int f = 1; f <= HEALTH_FAN_SUPPLY_AIR_COUNT; f++) {
            snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_fan%d_sa/config", HaDeviceId, f);
            esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
            snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_fan%d_spd/config", HaDeviceId, f);
            esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        }
    }

    free(buf);
}

static void mqtt_unpublish_discovery(void)
{
    if (!MqttClient || !MqttConnected) {
        return;
    }
    char disc_top[160];

    snprintf(disc_top, sizeof(disc_top), "homeassistant/switch/%s_system_power/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
    snprintf(disc_top, sizeof(disc_top), "homeassistant/select/%s_boost/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
    snprintf(disc_top, sizeof(disc_top), "homeassistant/number/%s_boost_timeout/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
    snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_boost_remaining/config", HaDeviceId);
    esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);

    for (size_t i = 0; i < MAX_ROOMS; i++) {
        snprintf(disc_top, sizeof(disc_top), "homeassistant/climate/%s_room%u/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/switch/%s_room%u_power/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/number/%s_room%u_setpoint/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_room%u_temp/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_room%u_sa/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_room%u_out/config", HaDeviceId, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
    }

    for (size_t i = 0; i < CustomMqttCount; i++) {
        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "%s_cust_%u_%u",
                 HaDeviceId, CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/%s/%s/config",
                 CustomMqttPoints[i].component, unique_id);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
    }

    const char *hlth_keys[] = {"output", "required_output", "output_pct", "flow_pct", "return_air", "air_delta"};
    for (size_t i = 0; i < sizeof(hlth_keys)/sizeof(hlth_keys[0]); i++) {
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_%s/config", HaDeviceId, hlth_keys[i]);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
    }
    for (int f = 1; f <= HEALTH_FAN_SUPPLY_AIR_COUNT; f++) {
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_fan%d_sa/config", HaDeviceId, f);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
        snprintf(disc_top, sizeof(disc_top), "homeassistant/sensor/%s_health_fan%d_spd/config", HaDeviceId, f);
        esp_mqtt_client_publish(MqttClient, disc_top, "", 0, 1, true);
    }

    /* Give the client a moment to flush the publishes over the wire before
     * we tear down MQTT config and reboot. */
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void mqtt_subscribe_commands(void)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/system_power/set", MqttTopicBase);
    esp_mqtt_client_subscribe(MqttClient, topic, 1);
    snprintf(topic, sizeof(topic), "%s/boost/set", MqttTopicBase);
    esp_mqtt_client_subscribe(MqttClient, topic, 1);
    snprintf(topic, sizeof(topic), "%s/boost_timeout/set", MqttTopicBase);
    esp_mqtt_client_subscribe(MqttClient, topic, 1);
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;
        snprintf(topic, sizeof(topic), "%s/room%u/setpoint/set", MqttTopicBase, (unsigned)i);
        esp_mqtt_client_subscribe(MqttClient, topic, 1);
        snprintf(topic, sizeof(topic), "%s/room%u/mode/set", MqttTopicBase, (unsigned)i);
        esp_mqtt_client_subscribe(MqttClient, topic, 1);
        snprintf(topic, sizeof(topic), "%s/room%u/preset/set", MqttTopicBase, (unsigned)i);
        esp_mqtt_client_subscribe(MqttClient, topic, 1);
        snprintf(topic, sizeof(topic), "%s/room%u/power/set", MqttTopicBase, (unsigned)i);
        esp_mqtt_client_subscribe(MqttClient, topic, 1);
    }
}

static const char *boost_preset_name(unsigned mode)
{
    if (mode == BOOST_MODE_FULL_HEATING) {
        return "boost_heat";
    }
    if (mode == BOOST_MODE_FULL_COOLING) {
        return "boost_cool";
    }
    return "none";
}

static const char *boost_select_name(unsigned mode)
{
    if (mode == BOOST_MODE_FULL_HEATING) {
        return "heat";
    }
    if (mode == BOOST_MODE_FULL_COOLING) {
        return "cool";
    }
    return "auto";
}

static void mqtt_publish_boost_state(unsigned mode)
{
    if (!MqttClient || !MqttConnected) return;
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/boost/state", MqttTopicBase);
    esp_mqtt_client_publish(MqttClient, topic, boost_select_name(mode), 0, 1, true);

    const char *preset = boost_preset_name(mode);
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;
        snprintf(topic, sizeof(topic), "%s/room%u/preset/state", MqttTopicBase, (unsigned)i);
        esp_mqtt_client_publish(MqttClient, topic, preset, 0, 1, true);
    }
    char remaining[8];
    snprintf(remaining, sizeof(remaining), "%d", boost_remaining_minutes());
    snprintf(topic, sizeof(topic), "%s/boost_remaining/state", MqttTopicBase);
    esp_mqtt_client_publish(MqttClient, topic, remaining, 0, 1, true);

    char timeout[8];
    snprintf(timeout, sizeof(timeout), "%u", (unsigned)BoostTimeoutMinutes);
    snprintf(topic, sizeof(topic), "%s/boost_timeout/state", MqttTopicBase);
    esp_mqtt_client_publish(MqttClient, topic, timeout, 0, 1, true);
}

static bool boost_apply(unsigned mode, bool self_initiated)
{
    if (!BacnetReady) {
        return false;
    }
    bool ok = write_msv_property(OBJECT_MULTI_STATE_VALUE, BOOST_INSTANCE, PROP_PRESENT_VALUE, mode);

    unsigned actual = mode;
    if (!read_msv_property(OBJECT_MULTI_STATE_VALUE, BOOST_INSTANCE, PROP_PRESENT_VALUE, &actual)) {
        actual = mode;
    }
    if (actual == BOOST_MODE_AUTO) {
        BoostSelfInitiated = false;
        BoostDeadlineUs = 0;
    } else if (self_initiated) {
        BoostSelfInitiated = true;
        BoostDeadlineUs = BoostTimeoutMinutes > 0
            ? esp_timer_get_time() + (int64_t)BoostTimeoutMinutes * 60000000LL
            : 0;
    }
    mqtt_publish_boost_state(actual);
    return ok;
}

static void mqtt_republish_bool(uint32_t instance, const char *state_topic, const char *on_val, const char *off_val)
{
    bool value;
    if (BacnetReady && read_bool_property(OBJECT_BINARY_VALUE, instance, PROP_PRESENT_VALUE, &value)) {
        if (MqttClient && MqttConnected) {
            esp_mqtt_client_publish(MqttClient, state_topic, value ? on_val : off_val, 0, 1, true);
        }
    }
}

#define THERMAL_OUTPUT_DEADBAND_KW 0.05f

static const char *hvac_action_for(float current_output)
{
    if (current_output < -THERMAL_OUTPUT_DEADBAND_KW) {
        return "cooling";
    }
    if (current_output > THERMAL_OUTPUT_DEADBAND_KW) {
        return "heating";
    }
    return "idle";
}

static void mqtt_publish_room_action(size_t room_idx)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/room%u/action/state", MqttTopicBase, (unsigned)room_idx);

    bool power;
    if (!BacnetReady ||
        !read_bool_property(
            OBJECT_BINARY_VALUE, Rooms[room_idx].power_instance, PROP_PRESENT_VALUE, &power)) {
        return;
    }
    if (!power) {
        if (MqttClient && MqttConnected) {
            esp_mqtt_client_publish(MqttClient, topic, "off", 0, 1, true);
        }
        return;
    }
    /* Room power stays whatever the user configured even while System Power
     * is off - only the reported action goes idle, so the room's own on/off
     * setting survives a system-off/on cycle untouched. */
    bool sys_power_commanded = true;
    hvac_core_get_system_power_commanded(&sys_power_commanded);
    if (!sys_power_commanded) {
        if (MqttClient && MqttConnected) {
            esp_mqtt_client_publish(MqttClient, topic, "idle", 0, 1, true);
        }
        return;
    }
    float current_output;
    if (read_real_property(
            OBJECT_ANALOG_VALUE, Rooms[room_idx].current_output_instance, PROP_PRESENT_VALUE,
            &current_output)) {
        if (MqttClient && MqttConnected) {
            esp_mqtt_client_publish(MqttClient, topic, hvac_action_for(current_output), 0, 1, true);
        }
    }
}

static void mqtt_republish_real(uint32_t instance, const char *state_topic)
{
    float value;
    if (BacnetReady && read_real_property(OBJECT_ANALOG_VALUE, instance, PROP_PRESENT_VALUE, &value)) {
        if (MqttClient && MqttConnected) {
            char payload[16];
            snprintf(payload, sizeof(payload), "%.1f", value);
            esp_mqtt_client_publish(MqttClient, state_topic, payload, 0, 1, true);
        }
    }
}

static void mqtt_handle_command(const char *topic, const char *data)
{
    char match_topic[96];
    snprintf(match_topic, sizeof(match_topic), "%s/system_power/set", MqttTopicBase);
    if (strcmp(topic, match_topic) == 0) {
        bool on = strcasecmp(data, "ON") == 0 || strcasecmp(data, "1") == 0 || strcasecmp(data, "true") == 0;
        hvac_command_system_power(on);
        snprintf(match_topic, sizeof(match_topic), "%s/system_power/state", MqttTopicBase);
        if (MqttClient && MqttConnected) {
            bool commanded = false;
            hvac_core_get_system_power_commanded(&commanded);
            esp_mqtt_client_publish(MqttClient, match_topic, commanded ? "ON" : "OFF", 0, 1, true);
        }
        return;
    }
    snprintf(match_topic, sizeof(match_topic), "%s/boost/set", MqttTopicBase);
    if (strcmp(topic, match_topic) == 0) {
        unsigned mode;
        if (strcmp(data, "auto") == 0) {
            mode = BOOST_MODE_AUTO;
        } else if (strcmp(data, "heat") == 0) {
            mode = BOOST_MODE_FULL_HEATING;
        } else if (strcmp(data, "cool") == 0) {
            mode = BOOST_MODE_FULL_COOLING;
        } else {
            return;
        }
        boost_apply(mode, true);
        return;
    }
    snprintf(match_topic, sizeof(match_topic), "%s/boost_timeout/set", MqttTopicBase);
    if (strcmp(topic, match_topic) == 0) {
        int minutes = atoi(data);
        if (minutes < 0 || minutes > BOOST_TIMEOUT_MAX_MIN) {
            return;
        }
        BoostTimeoutMinutes = (uint16_t)minutes;
        app_config_save();
        if (BoostSelfInitiated && BoostDeadlineUs != 0) {
            BoostDeadlineUs = BoostTimeoutMinutes > 0
                ? esp_timer_get_time() + (int64_t)BoostTimeoutMinutes * 60000000LL
                : 0;
        }
        unsigned actual;
        if (BacnetReady &&
            read_msv_property(OBJECT_MULTI_STATE_VALUE, BOOST_INSTANCE, PROP_PRESENT_VALUE, &actual)) {
            mqtt_publish_boost_state(actual);
        }
        return;
    }
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;
        char setpoint_topic[96];
        char mode_topic[96];
        char power_topic[96];
        char preset_topic[96];
        char setpoint_state_topic[96];
        char mode_state_topic[96];
        char power_state_topic[96];

        snprintf(setpoint_topic, sizeof(setpoint_topic), "%s/room%u/setpoint/set", MqttTopicBase, (unsigned)i);
        snprintf(mode_topic, sizeof(mode_topic), "%s/room%u/mode/set", MqttTopicBase, (unsigned)i);
        snprintf(power_topic, sizeof(power_topic), "%s/room%u/power/set", MqttTopicBase, (unsigned)i);
        snprintf(preset_topic, sizeof(preset_topic), "%s/room%u/preset/set", MqttTopicBase, (unsigned)i);

        if (strcmp(topic, preset_topic) == 0) {
            unsigned mode;
            if (strcmp(data, "boost_heat") == 0) {
                mode = BOOST_MODE_FULL_HEATING;
            } else if (strcmp(data, "boost_cool") == 0) {
                mode = BOOST_MODE_FULL_COOLING;
            } else {
                mode = BOOST_MODE_AUTO;
            }
            boost_apply(mode, true);
            return;
        }
        snprintf(setpoint_state_topic, sizeof(setpoint_state_topic), "%s/room%u/setpoint/state", MqttTopicBase, (unsigned)i);
        snprintf(mode_state_topic, sizeof(mode_state_topic), "%s/room%u/mode/state", MqttTopicBase, (unsigned)i);
        snprintf(power_state_topic, sizeof(power_state_topic), "%s/room%u/power/state", MqttTopicBase, (unsigned)i);

        if (strcmp(topic, setpoint_topic) == 0) {
            float value = strtof(data, NULL);
            if (value < MIN_SETPOINT_C) {
                value = MIN_SETPOINT_C;
            }
            if (value > 30.0f) {
                value = 30.0f;
            }
            hvac_command_room_setpoint(i, value, NULL);
            mqtt_republish_real(Rooms[i].setpoint_instance, setpoint_state_topic);
            return;
        }
        if (strcmp(topic, mode_topic) == 0) {
            bool on = (strcasecmp(data, "off") != 0 && strcasecmp(data, "0") != 0 && strcasecmp(data, "false") != 0);
            hvac_command_room_power(i, on);
            mqtt_republish_bool(Rooms[i].power_instance, mode_state_topic, "heat_cool", "off");
            mqtt_republish_bool(Rooms[i].power_instance, power_state_topic, "ON", "OFF");
            mqtt_publish_room_action(i);
            return;
        }
        if (strcmp(topic, power_topic) == 0) {
            bool on = (strcasecmp(data, "ON") == 0 || strcasecmp(data, "1") == 0 || strcasecmp(data, "true") == 0 || strcasecmp(data, "on") == 0);
            hvac_command_room_power(i, on);
            mqtt_republish_bool(Rooms[i].power_instance, power_state_topic, "ON", "OFF");
            mqtt_republish_bool(Rooms[i].power_instance, mode_state_topic, "heat_cool", "off");
            mqtt_publish_room_action(i);
            return;
        }
    }
}

static void mqtt_event_handler(
    void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_WIFI, "MQTT connected to broker");
            diag_log("mqtt connected");
            MqttConnected = true;
            MqttDisconnectedSinceUs = 0;
            MqttWatchdogLastKickUs = 0;
            {
                char st_top[96];
                snprintf(st_top, sizeof(st_top), "%s/status", MqttTopicBase);
                esp_mqtt_client_publish(MqttClient, st_top, "online", 0, 1, true);
            }
            DiscoveryNeedsBacnetRefresh = true;
            mqtt_subscribe_commands();
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG_WIFI, "MQTT disconnected");
            diag_log("mqtt disconnected");
            MqttConnected = false;
            if (MqttDisconnectedSinceUs == 0) {
                MqttDisconnectedSinceUs = esp_timer_get_time();
            }
            break;
        case MQTT_EVENT_DATA: {
            mqtt_command_t cmd = {0};
            size_t topic_len = (size_t)event->topic_len < sizeof(cmd.topic) - 1
                ? (size_t)event->topic_len : sizeof(cmd.topic) - 1;
            memcpy(cmd.topic, event->topic, topic_len);
            size_t data_len = (size_t)event->data_len < sizeof(cmd.data) - 1
                ? (size_t)event->data_len : sizeof(cmd.data) - 1;
            memcpy(cmd.data, event->data, data_len);
            xQueueSend(MqttCommandQueue, &cmd, 0);
            break;
        }
        default:
            break;
    }
}

static void mqtt_command_task(void *arg)
{
    (void)arg;
    mqtt_command_t cmd;
    for (;;) {
        if (xQueueReceive(MqttCommandQueue, &cmd, portMAX_DELAY)) {
            mqtt_handle_command(cmd.topic, cmd.data);
        }
    }
}

/* Called from sta_event_handler() on the sys_evt task, so this must not
   block. esp_mqtt_client_disconnect()/_reconnect() only flag the client's own
   task; esp_mqtt_client_stop() would block waiting for it to wind down, which
   is not safe from an event handler. */
static void mqtt_pause_for_link_loss(void)
{
    if (MqttClient) {
        esp_mqtt_client_disconnect(MqttClient);
    }
}

static void mqtt_resume_after_link_up(void)
{
    if (MqttClient) {
        esp_mqtt_client_reconnect(MqttClient);
    }
}

static void mqtt_app_restart(void)
{
    if (MqttClient) {
        esp_mqtt_client_stop(MqttClient);
        esp_mqtt_client_destroy(MqttClient);
        MqttClient = NULL;
        MqttConnected = false;
    }
    if (strlen(MqttBrokerHost) > 0) {
        char uri[128];
        snprintf(uri, sizeof(uri), "mqtt://%s:%u", MqttBrokerHost, (unsigned)MqttBrokerPort);
        char will_topic[96];
        snprintf(will_topic, sizeof(will_topic), "%s/status", MqttTopicBase);

        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = uri,
            .session.last_will.topic = will_topic,
            .session.last_will.msg = "offline",
            .session.last_will.msg_len = 0,
            .session.last_will.qos = 1,
            .session.last_will.retain = true,
            /* Bound the outbox. Unset it defaults to unlimited, so every
               publish attempted while the broker is unreachable queues in
               heap. Observed 2026-08-24: "outbox_enqueue(53): Memory
               exhausted" with free heap at 3-5K. 4KB is ample for this
               device's publish volume between reconnects. */
            .outbox.limit = 4096,
            /* 8192, was 24576. This is esp-mqtt's own protocol task - it runs
               the MQTT state machine and our mqtt_event_handler, and never
               touches the BACnet read path (that is mqtt_state_task and
               mqtt_command_task, which do legitimately need 24K for nested
               MAX_APDU buffers). esp-mqtt's default is 6144. At 24576 this
               device had four 24K stacks committed out of ~185K total heap,
               which left only 19.5K free - not enough for bacnet_client_task's
               own 24K, so BACnet silently never started. 8192 keeps a
               comfortable margin over the default while freeing 16K. */
            .task.stack_size = 8192,
        };
        if (strlen(MqttBrokerUser) > 0) {
            mqtt_cfg.credentials.username = MqttBrokerUser;
            mqtt_cfg.credentials.authentication.password = MqttBrokerPass;
        }
        MqttClient = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(MqttClient, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(MqttClient);
    }
}

static void mqtt_app_stop(void)
{
    if (MqttClient) {
        esp_mqtt_client_stop(MqttClient);
        esp_mqtt_client_destroy(MqttClient);
        MqttClient = NULL;
        MqttConnected = false;
    }
    if (MqttCommandQueue) {
        vQueueDelete(MqttCommandQueue);
        MqttCommandQueue = NULL;
    }
    ESP_LOGI(TAG_WIFI, "MQTT integration stopped and resources released");
}

static void mqtt_app_start(void)
{
    if (!mqtt_integration_selected()) {
        ESP_LOGI(TAG_WIFI, "MQTT integration not selected - resources deferred");
        return;
    }
    /* A blank broker means MQTT is not configured. Do not permanently consume
       a 24 KB internal-RAM task stack and queue for an inactive feature. The
       configuration handlers call this function again when a broker is saved,
       so MQTT still becomes live without a reboot. */
    if (strlen(MqttBrokerHost) == 0) {
        mqtt_app_restart();
        ESP_LOGI(TAG_WIFI, "MQTT broker not configured - command resources deferred");
        return;
    }

    if (!MqttCommandQueue) {
        MqttCommandQueue = xQueueCreate(8, sizeof(mqtt_command_t));
        if (!MqttCommandQueue) {
            ESP_LOGE(TAG_WIFI, "MQTT command queue allocation failed");
            return;
        }
        if (!spawn_task(mqtt_command_task, "mqtt_command", 4096, NULL, 5, NULL)) {
            vQueueDelete(MqttCommandQueue);
            MqttCommandQueue = NULL;
            return;
        }
    }
    mqtt_app_restart();
}

static void mqtt_state_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Link-liveness watchdog: catches the AP silently dropping the client
           without a deauth (see the diagnostics comment near app_main) - MQTT
           unable to even open a TCP connection for minutes straight, despite
           the WiFi driver still reporting "connected", is the same dead-link
           condition as an explicit disconnect. Force one the same way a real
           disconnect event would. No new task, no esp_ping - see the comment
           on MqttDisconnectedSinceUs for why that matters on this board. */
        if (!MqttConnected && MqttDisconnectedSinceUs != 0) {
            int64_t now = esp_timer_get_time();
            int64_t down_ms = (now - MqttDisconnectedSinceUs) / 1000;
            int64_t since_last_kick_ms = (now - MqttWatchdogLastKickUs) / 1000;
            /* Stage 1: recycle the MQTT client itself. A broker unreachable
               while WiFi still reports "connected" leaves esp-mqtt retrying
               indefinitely, and the in-flight esp-tls contexts plus the outbox
               pin heap - measured at 3-5K free on 2026-08-24, at which point
               the HTTP server could no longer allocate a response and the
               device was unreachable despite being associated (AP-side
               Assoc rssi:-73, no deauth in that window). The WiFi-disconnect
               path frees this, but only fires on an actual disconnect event,
               which never came. Stop/destroy/re-init releases all of it.
               Stage 2 below still forces the WiFi reconnect if that is not
               enough. */
            if (down_ms > MQTT_RECYCLE_AFTER_MS &&
                (MqttRecycleLastUs == 0 || (now - MqttRecycleLastUs) / 1000 > MQTT_RECYCLE_AFTER_MS)) {
                MqttRecycleLastUs = now;
                diag_log("mqtt down %llds - recycling client to release heap",
                    (long long)(down_ms / 1000));
                mqtt_app_restart();
            }
            if (down_ms > MQTT_WATCHDOG_RECONNECT_MS &&
                (MqttWatchdogLastKickUs == 0 || since_last_kick_ms > MQTT_WATCHDOG_RECONNECT_MS)) {
                MqttWatchdogLastKickUs = now;
                diag_log("mqtt unreachable %llds despite wifi appearing connected - forcing reconnect",
                    (long long)(down_ms / 1000));
                esp_wifi_disconnect(); /* fires WIFI_EVENT_STA_DISCONNECTED -> normal reconnect path */
            }
        }
        scan_catalog_release_if_idle();

        if (MqttConnected) {
            if (DiscoveryNeedsBacnetRefresh) {
                mqtt_publish_discovery();
                if (BacnetReady) {
                    DiscoveryNeedsBacnetRefresh = false;
                }
            }
        }
        if (MqttConnected && BacnetReady) {
            /* Timing bracket added 2026-08-23 to test whether WiFi RSSI drops
               correlate with this loop's ~20-property BACnet/SPI read burst
               (the W5500 shares the board with the WiFi antenna over an
               unshielded 3-4" run - a plausible RFI source). Same pattern as
               the httpd handler timing added the same night. */
            int64_t __bacnet_burst_start_us = esp_timer_get_time();
            diag_log("bacnet poll burst start");
            bool sys_power;
            if (hvac_core_get_system_power_commanded(&sys_power)) {
                char st_top[96];
                snprintf(st_top, sizeof(st_top), "%s/system_power/state", MqttTopicBase);
                esp_mqtt_client_publish(
                    MqttClient, st_top, sys_power ? "ON" : "OFF", 0, 1,
                    true);
            }

            unsigned boost_mode;
            if (read_msv_property(
                    OBJECT_MULTI_STATE_VALUE, BOOST_INSTANCE, PROP_PRESENT_VALUE, &boost_mode)) {
                if (boost_mode == BOOST_MODE_AUTO) {
                    BoostSelfInitiated = false;
                    BoostDeadlineUs = 0;
                } else if (BoostTimeoutMinutes > 0 &&
                           (BoostSelfInitiated || BoostRevertExternal)) {
                    if (BoostDeadlineUs == 0) {
                        BoostDeadlineUs =
                            esp_timer_get_time() + (int64_t)BoostTimeoutMinutes * 60000000LL;
                    } else if (esp_timer_get_time() >= BoostDeadlineUs) {
                        ESP_LOGI(
                            TAG_WIFI, "Boost timeout (%u min) elapsed - reverting to Auto",
                            (unsigned)BoostTimeoutMinutes);
                        boost_apply(BOOST_MODE_AUTO, false);
                        boost_mode = BOOST_MODE_AUTO;
                    }
                }
                mqtt_publish_boost_state(boost_mode);
            }

            for (size_t i = 0; i < RoomCount; i++) {
                if (!Rooms[i].active) continue;
                char topic[96];
                float temperature;
                if (read_real_property(
                        OBJECT_ANALOG_VALUE, Rooms[i].temperature_instance, PROP_PRESENT_VALUE,
                        &temperature)) {
                    char payload[16];
                    snprintf(payload, sizeof(payload), "%.1f", temperature);
                    snprintf(topic, sizeof(topic), "%s/room%u/temperature/state", MqttTopicBase, (unsigned)i);
                    esp_mqtt_client_publish(MqttClient, topic, payload, 0, 1, true);
                }
                float setpoint;
                if (read_real_property(
                        OBJECT_ANALOG_VALUE, Rooms[i].setpoint_instance, PROP_PRESENT_VALUE, &setpoint)) {
                    char payload[16];
                    snprintf(payload, sizeof(payload), "%.1f", setpoint);
                    snprintf(topic, sizeof(topic), "%s/room%u/setpoint/state", MqttTopicBase, (unsigned)i);
                    esp_mqtt_client_publish(MqttClient, topic, payload, 0, 1, true);
                }
                bool power;
                if (read_bool_property(
                        OBJECT_BINARY_VALUE, Rooms[i].power_instance, PROP_PRESENT_VALUE, &power)) {
                    snprintf(topic, sizeof(topic), "%s/room%u/mode/state", MqttTopicBase, (unsigned)i);
                    esp_mqtt_client_publish(
                        MqttClient, topic, power ? "heat_cool" : "off", 0, 1, true);
                    snprintf(topic, sizeof(topic), "%s/room%u/power/state", MqttTopicBase, (unsigned)i);
                    esp_mqtt_client_publish(
                        MqttClient, topic, power ? "ON" : "OFF", 0, 1, true);
                }
                float sa;
                if (read_real_property(
                        OBJECT_ANALOG_VALUE, Rooms[i].supply_air_instance, PROP_PRESENT_VALUE, &sa)) {
                    char payload[16];
                    snprintf(payload, sizeof(payload), "%.1f", sa);
                    snprintf(topic, sizeof(topic), "%s/room%u/supply_air/state", MqttTopicBase, (unsigned)i);
                    esp_mqtt_client_publish(MqttClient, topic, payload, 0, 1, true);
                }
                float cur_out;
                if (read_real_property(
                        OBJECT_ANALOG_VALUE, Rooms[i].current_output_instance, PROP_PRESENT_VALUE, &cur_out)) {
                    char payload[16];
                    snprintf(payload, sizeof(payload), "%.2f", cur_out);
                    snprintf(topic, sizeof(topic), "%s/room%u/current_output/state", MqttTopicBase, (unsigned)i);
                    esp_mqtt_client_publish(MqttClient, topic, payload, 0, 1, true);
                }
                float req_out;
                if (read_real_property(
                        OBJECT_ANALOG_VALUE, Rooms[i].required_output_instance, PROP_PRESENT_VALUE, &req_out)) {
                    char payload[16];
                    snprintf(payload, sizeof(payload), "%.2f", req_out);
                    snprintf(topic, sizeof(topic), "%s/room%u/required_output/state", MqttTopicBase, (unsigned)i);
                    esp_mqtt_client_publish(MqttClient, topic, payload, 0, 1, true);
                }
                mqtt_publish_room_action(i);
            }

            for (size_t i = 0; i < CustomMqttCount; i++) {
                if (!CustomMqttPoints[i].enabled) continue;
                char state_top[96];
                snprintf(state_top, sizeof(state_top), "%s/custom/%u_%u/state",
                         MqttTopicBase, CustomMqttPoints[i].obj_type, (unsigned)CustomMqttPoints[i].instance);
                if (CustomMqttPoints[i].obj_type == OBJECT_ANALOG_VALUE ||
                    CustomMqttPoints[i].obj_type == OBJECT_ANALOG_INPUT ||
                    CustomMqttPoints[i].obj_type == OBJECT_ANALOG_OUTPUT) {
                    float val = 0.0f;
                    if (read_real_property((BACNET_OBJECT_TYPE)CustomMqttPoints[i].obj_type,
                                           CustomMqttPoints[i].instance, PROP_PRESENT_VALUE, &val)) {
                        char payload[16];
                        snprintf(payload, sizeof(payload), "%.2f", val);
                        esp_mqtt_client_publish(MqttClient, state_top, payload, 0, 1, true);
                    }
                } else if (CustomMqttPoints[i].obj_type == OBJECT_BINARY_VALUE ||
                           CustomMqttPoints[i].obj_type == OBJECT_BINARY_INPUT ||
                           CustomMqttPoints[i].obj_type == OBJECT_BINARY_OUTPUT) {
                    bool val = false;
                    if (read_bool_property((BACNET_OBJECT_TYPE)CustomMqttPoints[i].obj_type,
                                           CustomMqttPoints[i].instance, PROP_PRESENT_VALUE, &val)) {
                        esp_mqtt_client_publish(MqttClient, state_top, val ? "ON" : "OFF", 0, 1, true);
                    }
                }
            }

            if (HaHealthDiscoveryEnabled) {
                float h_out = 0.0f, h_req = 0.0f, h_fp = 0.0f, h_ret = 0.0f;
                bool h_out_ok = read_real_property(OBJECT_ANALOG_VALUE, HEALTH_COOLING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_out);
                bool h_req_ok = read_real_property(OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_req);
                bool h_fp_ok = read_real_property(OBJECT_ANALOG_VALUE, HEALTH_FLOW_DESIGN_PCT_INSTANCE, PROP_PRESENT_VALUE, &h_fp);
                bool h_ret_ok = read_real_property(OBJECT_ANALOG_INPUT, HEALTH_RETURN_AIR_INSTANCE, PROP_PRESENT_VALUE, &h_ret);

                float h_hout = 0.0f, h_hreq = 0.0f;
                bool h_hout_ok = read_real_property(OBJECT_ANALOG_VALUE, HEALTH_HEATING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_hout);
                bool h_hreq_ok = read_real_property(OBJECT_ANALOG_VALUE, HEALTH_REQUIRED_HEATING_OUTPUT_INSTANCE, PROP_PRESENT_VALUE, &h_hreq);

                bool is_heating = (h_hout_ok && h_hreq_ok && (h_hreq > h_req || h_hout > h_out) && h_hreq > 0.05f);
                float cur_out = is_heating ? (h_hout_ok ? h_hout : 0.0f) : (h_out_ok ? h_out : 0.0f);
                float cur_req = is_heating ? (h_hreq_ok ? h_hreq : 0.0f) : (h_req_ok ? h_req : 0.0f);

                char htop[128], hpayload[24];
                if ((is_heating && h_hout_ok) || (!is_heating && h_out_ok)) {
                    snprintf(hpayload, sizeof(hpayload), "%.2f", cur_out);
                    snprintf(htop, sizeof(htop), "%s/health/output/state", MqttTopicBase);
                    esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                }
                if ((is_heating && h_hreq_ok) || (!is_heating && h_req_ok)) {
                    snprintf(hpayload, sizeof(hpayload), "%.2f", cur_req);
                    snprintf(htop, sizeof(htop), "%s/health/required_output/state", MqttTopicBase);
                    esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                }
                if (cur_req > 0.1f) {
                    snprintf(hpayload, sizeof(hpayload), "%.1f", (cur_out / cur_req) * 100.0f);
                    snprintf(htop, sizeof(htop), "%s/health/output_pct/state", MqttTopicBase);
                    esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                }
                if (h_fp_ok) {
                    snprintf(hpayload, sizeof(hpayload), "%.1f", h_fp);
                    snprintf(htop, sizeof(htop), "%s/health/flow_pct/state", MqttTopicBase);
                    esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                }
                if (h_ret_ok && temp_is_plausible(h_ret)) {
                    snprintf(hpayload, sizeof(hpayload), "%.1f", h_ret);
                    snprintf(htop, sizeof(htop), "%s/health/return_air/state", MqttTopicBase);
                    esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                }

                /* Delta T */
                float sa_sum = 0.0f;
                int sa_cnt = 0;
                for (size_t i = 0; i < RoomCount; i++) {
                    if (!Rooms[i].active) continue;
                    float r_sa = 0.0f;
                    if (read_real_property(OBJECT_ANALOG_VALUE, Rooms[i].supply_air_instance, PROP_PRESENT_VALUE, &r_sa) && temp_is_plausible(r_sa)) {
                        sa_sum += r_sa;
                        sa_cnt++;
                    }
                }
                if (h_ret_ok && temp_is_plausible(h_ret) && sa_cnt > 0) {
                    float avg_sa = sa_sum / sa_cnt;
                    float dt = avg_sa - h_ret; /* signed: negative = cooling, positive = warming */
                    snprintf(hpayload, sizeof(hpayload), "%.1f", dt);
                    snprintf(htop, sizeof(htop), "%s/health/air_delta/state", MqttTopicBase);
                    esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                }

                /* Fan health states */
                float fcnt_val = 0.0f;
                int fcnt = 2;
                if (read_real_property(OBJECT_ANALOG_VALUE, HEALTH_FAN_COUNT_INSTANCE, PROP_PRESENT_VALUE, &fcnt_val)) {
                    fcnt = (int)(fcnt_val + 0.5f);
                    if (fcnt < 1) fcnt = 1;
                    if (fcnt > HEALTH_FAN_SUPPLY_AIR_COUNT) fcnt = HEALTH_FAN_SUPPLY_AIR_COUNT;
                }
                for (int f = 1; f <= fcnt; f++) {
                    float ftemp = 0.0f, fspd = 0.0f;
                    if (read_real_property(OBJECT_ANALOG_INPUT, f, PROP_PRESENT_VALUE, &ftemp) && temp_is_plausible(ftemp)) {
                        snprintf(hpayload, sizeof(hpayload), "%.1f", ftemp);
                        snprintf(htop, sizeof(htop), "%s/health/fan%d/supply_air/state", MqttTopicBase, f);
                        esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                    }
                    if (f <= HEALTH_FAN_SPEED_COUNT && read_real_property(OBJECT_ANALOG_OUTPUT, f, PROP_PRESENT_VALUE, &fspd)) {
                        snprintf(hpayload, sizeof(hpayload), "%.1f", fspd);
                        snprintf(htop, sizeof(htop), "%s/health/fan%d/speed/state", MqttTopicBase, f);
                        esp_mqtt_client_publish(MqttClient, htop, hpayload, 0, 1, true);
                    }
                }
            }
            diag_log("bacnet poll burst done %lldms",
                (long long)((esp_timer_get_time() - __bacnet_burst_start_us) / 1000));
        }
        /* 45s (was 20s) - 2026-08-23, less frequent BACnet polling to cut heap
           churn per Piers's request. This loop does ~20 sequential BACnet
           reads + MQTT publishes every cycle; halving the frequency roughly
           halves its share of steady-state CPU/heap pressure. */
        vTaskDelay(pdMS_TO_TICKS(45000));
    }
}

/* ===================== Diagnostics: serial debug log =====================
   Added 2026-08-23 after a device disappeared from HA/MQTT and stopped answering ping.
   Originally also shipped every line over UDP to a Pi listener so this was diagnosable
   without a USB cable attached - removed same day at Piers's request, to take wireless
   activity (a socket, sendto() calls, DNS/getaddrinfo on the broker host) off the table
   as a variable while diagnosing a WiFi RSSI/RF issue. diag_log() now only writes to the
   serial console (already true of every ESP_LOGx call); a laptop with a USB-serial
   adapter tailing the port is the only way to read it. See serial_logger.py in the repo's
   working notes / docs for a simple local capture-to-file script if wanted. */
static void diag_log(const char *fmt, ...)
{
    char msg[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000LL);
    /* Total free AND largest contiguous block. Total alone is misleading: on
       2026-08-25 a 24576-byte task stack failed to allocate with 33K free,
       because fragmentation had left no single block big enough. The `big=`
       figure is the one that actually predicts allocation failures. */
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t bigb = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGW("DIAG", "esp-bacnet up=%us heap=%uK big=%uK %s",
             (unsigned)up, (unsigned)(freeb / 1024), (unsigned)(bigb / 1024), msg);
}

/* Gateway-liveness watchdog (esp_ping-based, spawned a task per check + its own
   4KB task stack) was cut same-day: on this no-PSRAM, ~185K-heap board it pushed
   free heap from ~15K to a 2K reboot-loop within 90s of boot - the exact crisis
   documented in the heap-budget incident history. diag_log() below is cheap
   (no new task, one UDP socket) and stays. A lighter liveness check - piggybacked
   on an existing task, no esp_ping - is future work if the WiFi/PS fix above
   doesn't fully resolve the silent-association-drop failure mode on its own. */

/* ===================== app_main ===================== */


static void mdns_start_service(void)
{
    if (MdnsRunning) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG_WIFI, "mDNS init failed - continuing without .local");
        return;
    }

    /* Probe before claiming the default name: two bridges on the same LAN
     * would otherwise both advertise "esp-bacnet-bridge.local" and collide.
     * One query per candidate name, ESP_ERR_NOT_FOUND means nobody answered
     * for it - that's the one we take. Capped at 20 suffixes; if even that's
     * taken, just use the plain default rather than looping forever. */
    strlcpy(MdnsHostname, MDNS_HOSTNAME, sizeof(MdnsHostname));
    for (int suffix = 1; suffix <= 20; suffix++) {
        esp_ip4_addr_t probe_addr;
        if (mdns_query_a(MdnsHostname, 1000, &probe_addr) == ESP_ERR_NOT_FOUND) {
            break;
        }
        snprintf(MdnsHostname, sizeof(MdnsHostname), "%s-%d", MDNS_HOSTNAME, suffix);
    }

    mdns_hostname_set(MdnsHostname);
    mdns_instance_name_set("ESP32 BACnet Bridge");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    MdnsRunning = true;
    diag_log("mdns advertisement started (%s.local)", MdnsHostname);
    ESP_LOGI(TAG_WIFI, "mDNS ready - reachable at http://%s.local", MdnsHostname);
}

static void mdns_stop_service(void)
{
    if (!MdnsRunning) {
        return;
    }
    mdns_free();
    MdnsRunning = false;
    diag_log("mdns advertisement stopped");
    ESP_LOGI(TAG_WIFI, "mDNS stopped");
}

/* Bring the responder in line with MdnsEnabled. Safe to call repeatedly. */
static void mdns_apply_setting(void)
{
    if (MdnsEnabled) {
        mdns_start_service();
    } else {
        mdns_stop_service();
    }
}

void app_main(void)
{
    esp_log_set_vprintf(web_log_vprintf);

    /* MdnsHostname lives in PSRAM (EXT_RAM_BSS_ATTR) so it starts zeroed;
     * give it the sensible default up front so /api/network reports a real
     * hostname even before/without mdns_start_service() ever running (mDNS
     * is off by default). */
    strlcpy(MdnsHostname, MDNS_HOSTNAME, sizeof(MdnsHostname));

    ESP_LOGI("MAIN", "=======================================================");
    ESP_LOGI("MAIN", "  ESP32 BACnet Bridge for Delta DAC-1180E Controllers");
    ESP_LOGI("MAIN", "  Created by Piers Wingfield <piers@wingfield.tech>");
    ESP_LOGI("MAIN", "  Open Source under MIT License · Free for Community Use");
    ESP_LOGI("MAIN", "=======================================================");

    ESP_ERROR_CHECK(nvs_flash_init());

    esp_ota_mark_app_valid_cancel_rollback();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    target_config_load();
    rooms_config_load();
    hvac_core_integration_load();
    mqtt_config_load();
    custom_mqtt_load();
    app_config_load();
    wizard_completed_load();
    ota_password_load();
    matter_adapter_set_system_power_handlers(matter_system_power_write, matter_system_power_read);
    matter_adapter_set_boost_handlers(matter_boost_write, matter_boost_read);

    spawn_task(eth_bringup_task, "eth_bringup", 4096, NULL, 5, NULL);

    char ssid[33] = {0};
    char pass[65] = {0};
    if (load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        if (try_connect_sta(ssid, pass)) {
            ESP_LOGI(TAG_WIFI, "Connected to '%s' - provisioning not needed this boot", ssid);
            esp_wifi_set_ps(WIFI_PS_NONE);
            /* mDNS costs ~3-5K heap at boot - on this no-PSRAM, ~185K-heap device that's
               the difference between stable and a reboot loop (see esp32-heap-budget-no-psram
               memory / docs/SCOPE_2026-08-16_baseline-revert.md). Baseline never got the
               runtime toggle that later work added; cutting it outright is the simplest
               isolated fix. Device is DHCP-reserved (192.168.1.216) so .local access isn't
               needed. Re-add mdns_start_service() deliberately, with the NVS-backed toggle,
               if .local access is ever wanted again - don't re-enable unconditionally. */
            start_connected_webserver();
            /* Advertisement is opt-in and off by default - see NVS_KEY_MDNS_ENABLED.
               Toggleable from the setup wizard and the Update page. */
            mdns_apply_setting();
            if (hvac_core_integration_get() == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT) {
                mqtt_app_start();
                if (mqtt_integration_selected()) {
                    spawn_task(mqtt_state_task, "mqtt_state", 4096, NULL, 5, NULL);
                }
            } else if (hvac_core_integration_get() == HVAC_INTEGRATION_MATTER) {
                matter_adapter_init();
                matter_adapter_start();
            } else {
                ESP_LOGI(TAG_WIFI, "Integration mode set to NONE (Web Dashboard only)");
            }
            return;
        }
    } else {
        ESP_LOGI(TAG_WIFI, "No saved WiFi credentials found");
    }

    start_provisioning_ap();
}
