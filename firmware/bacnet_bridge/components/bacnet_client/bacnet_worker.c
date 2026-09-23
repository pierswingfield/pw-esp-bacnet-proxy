#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "bacnet/bacdef.h"
#include "bacnet/bacaddr.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacenum.h"
#include "bacnet/npdu.h"
#include "bacnet/apdu.h"
#include "bacnet/rp.h"
#include "bacnet/wp.h"
#include "bacnet/bactext.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/basic/service/s_rp.h"
#include "bacnet/basic/service/s_wp.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/basic/npdu/h_npdu.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/iam.h"
#include "bacnet/whois.h"
#include "bacnet/basic/service/s_whois.h"

#include "bacnet_worker.h"

static const char *TAG_WORKER = "bacnet_worker";

/* Static Task Memory */
static StaticTask_t s_worker_tcb;
static StackType_t s_worker_stack[BACNET_WORKER_STACK_SIZE / sizeof(StackType_t)];
static TaskHandle_t s_worker_task_handle = NULL;

/* Static Queue Memory */
static QueueHandle_t s_high_prio_queue = NULL;
static QueueHandle_t s_normal_prio_queue = NULL;
static uint8_t s_high_prio_queue_storage[BACNET_HIGH_PRIO_QUEUE_DEPTH * sizeof(bacnet_request_t)];
static StaticQueue_t s_high_prio_queue_struct;
static uint8_t s_normal_prio_queue_storage[BACNET_NORMAL_PRIO_QUEUE_DEPTH * sizeof(bacnet_request_t)];
static StaticQueue_t s_normal_prio_queue_struct;

/* Configuration & Target State */
static portMUX_TYPE s_config_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_t *s_netif = NULL;
static uint32_t s_target_dev_id = 753016;
static char s_target_ip[16] = "10.0.3.3";
static uint16_t s_target_port = 47808;
static BACNET_ADDRESS s_target_address;
static volatile bool s_is_ready = false;

/* Circuit Breaker & Health State */
static volatile bacnet_health_state_t s_health_state = BACNET_HEALTH_ONLINE;
static volatile uint32_t s_consecutive_timeouts = 0;
static int64_t s_last_probe_time_us = 0;

/* Transaction Response State (strictly owned by worker task) */
static uint8_t s_current_invoke_id = 0;
static volatile bool s_reply_received = false;
static volatile bool s_reply_errored = false;
static volatile uint8_t s_reply_error_class = 0;
static volatile uint8_t s_reply_error_code = 0;
static volatile uint8_t s_reply_abort_reason = 0;
static volatile uint8_t s_reply_reject_reason = 0;
static volatile bacnet_worker_status_t s_transaction_status = BACNET_WORKER_STATUS_OK;
static BACNET_APPLICATION_DATA_VALUE s_last_read_value;
static uint8_t s_last_read_raw[MAX_APDU];
static int s_last_read_raw_len = 0;

/* Discovery Response State */
static bacnet_discovered_dev_t s_discovered_devs[MAX_DISCOVERED_DEVICES];
static size_t s_discovered_count = 0;
/* Counts every I-Am seen this discovery pass, even past MAX_DISCOVERED_DEVICES
 * - without this, a network with more responders than the fixed storage array
 * silently reported the same result as one with fewer, with no way to tell
 * discovery was incomplete. */
static size_t s_discovered_seen_count = 0;

/* Read Cache */
typedef enum {
    BC_KIND_NONE = 0,
    BC_KIND_REAL,
    BC_KIND_BOOL,
    BC_KIND_MSV
} bacnet_cache_kind_t;

typedef struct {
    uint16_t type;
    uint32_t instance;
    uint32_t property;
    uint8_t kind;
    union {
        float f;
        bool b;
        unsigned u;
    } v;
    int64_t stamp_us;
} bacnet_cache_entry_t;

static bacnet_cache_entry_t s_bacnet_cache[BACNET_CACHE_ENTRIES];
static portMUX_TYPE s_cache_mux = portMUX_INITIALIZER_UNLOCKED;

/* ========================================================================= */
/* Cache Layer Implementation                                                */
/* ========================================================================= */

static bacnet_cache_entry_t *bacnet_cache_find_locked(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID property)
{
    for (size_t i = 0; i < BACNET_CACHE_ENTRIES; i++) {
        bacnet_cache_entry_t *e = &s_bacnet_cache[i];
        if (e->kind != BC_KIND_NONE && e->type == (uint16_t)type &&
            e->instance == instance && e->property == (uint32_t)property) {
            return e;
        }
    }
    return NULL;
}

static bacnet_cache_entry_t *bacnet_cache_slot_locked(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID property)
{
    bacnet_cache_entry_t *e = bacnet_cache_find_locked(type, instance, property);
    if (e) return e;

    bacnet_cache_entry_t *oldest = &s_bacnet_cache[0];
    for (size_t i = 0; i < BACNET_CACHE_ENTRIES; i++) {
        if (s_bacnet_cache[i].kind == BC_KIND_NONE) {
            return &s_bacnet_cache[i];
        }
        if (s_bacnet_cache[i].stamp_us < oldest->stamp_us) {
            oldest = &s_bacnet_cache[i];
        }
    }
    return oldest;
}

static bool bacnet_cache_get(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID property,
    bacnet_cache_kind_t kind, void *out)
{
    bool hit = false;
    taskENTER_CRITICAL(&s_cache_mux);
    bacnet_cache_entry_t *e = bacnet_cache_find_locked(type, instance, property);
    if (e && e->kind == (uint8_t)kind &&
        (esp_timer_get_time() - e->stamp_us) / 1000 < BACNET_CACHE_TTL_MS) {
        switch (kind) {
            case BC_KIND_REAL: *(float *)out = e->v.f; break;
            case BC_KIND_BOOL: *(bool *)out = e->v.b; break;
            case BC_KIND_MSV:  *(unsigned *)out = e->v.u; break;
            default: break;
        }
        hit = true;
    }
    taskEXIT_CRITICAL(&s_cache_mux);
    return hit;
}

static void bacnet_cache_put(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID property,
    bacnet_cache_kind_t kind, const void *val)
{
    taskENTER_CRITICAL(&s_cache_mux);
    bacnet_cache_entry_t *e = bacnet_cache_slot_locked(type, instance, property);
    e->type = (uint16_t)type;
    e->instance = instance;
    e->property = (uint32_t)property;
    e->kind = (uint8_t)kind;
    switch (kind) {
        case BC_KIND_REAL: e->v.f = *(const float *)val; break;
        case BC_KIND_BOOL: e->v.b = *(const bool *)val; break;
        case BC_KIND_MSV:  e->v.u = *(const unsigned *)val; break;
        default: break;
    }
    e->stamp_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_cache_mux);
}

static void bacnet_cache_invalidate(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID property)
{
    taskENTER_CRITICAL(&s_cache_mux);
    bacnet_cache_entry_t *e = bacnet_cache_find_locked(type, instance, property);
    if (e) {
        e->kind = BC_KIND_NONE;
        e->stamp_us = 0;
    }
    taskEXIT_CRITICAL(&s_cache_mux);
}

/* ========================================================================= */
/* BACnet APDU Handler Callbacks                                             */
/* ========================================================================= */

static void worker_error_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    BACNET_ERROR_CLASS error_class,
    BACNET_ERROR_CODE error_code)
{
    if (address_match(&s_target_address, src) && (invoke_id == s_current_invoke_id)) {
        ESP_LOGE(TAG_WORKER, "BACnet Error: class=%d code=%d", error_class, error_code);
        s_reply_errored = true;
        s_reply_error_class = (uint8_t)error_class;
        s_reply_error_code = (uint8_t)error_code;
        s_transaction_status = BACNET_WORKER_STATUS_ERROR;
    }
}

static void worker_abort_handler(
    BACNET_ADDRESS *src, uint8_t invoke_id, uint8_t abort_reason, bool server)
{
    (void)server;
    if (address_match(&s_target_address, src) && (invoke_id == s_current_invoke_id)) {
        ESP_LOGE(TAG_WORKER, "BACnet Abort: reason=%d", abort_reason);
        s_reply_errored = true;
        s_reply_abort_reason = abort_reason;
        s_transaction_status = BACNET_WORKER_STATUS_ABORT;
    }
}

static void worker_reject_handler(
    BACNET_ADDRESS *src, uint8_t invoke_id, uint8_t reject_reason)
{
    if (address_match(&s_target_address, src) && (invoke_id == s_current_invoke_id)) {
        ESP_LOGE(TAG_WORKER, "BACnet Reject: reason=%d", reject_reason);
        s_reply_errored = true;
        s_reply_reject_reason = reject_reason;
        s_transaction_status = BACNET_WORKER_STATUS_REJECT;
    }
}

static void worker_read_property_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    BACNET_READ_PROPERTY_DATA data;
    if (!(address_match(&s_target_address, src) &&
          (service_data->invoke_id == s_current_invoke_id))) {
        return;
    }

    int len = rp_ack_decode_service_request(service_request, service_len, &data);
    if (len < 0) {
        ESP_LOGE(TAG_WORKER, "ReadProperty ack: decode failed");
        s_reply_errored = true;
        s_transaction_status = BACNET_WORKER_STATUS_ERROR;
        return;
    }

    s_last_read_raw_len = data.application_data_len < (int)sizeof(s_last_read_raw)
        ? data.application_data_len : (int)sizeof(s_last_read_raw);
    memcpy(s_last_read_raw, data.application_data, s_last_read_raw_len);

    if (bacapp_decode_application_data(
            data.application_data, (uint32_t)data.application_data_len,
            &s_last_read_value) < 0) {
        ESP_LOGE(TAG_WORKER, "ReadProperty ack: value decode failed");
        s_reply_errored = true;
        s_transaction_status = BACNET_WORKER_STATUS_ERROR;
        return;
    }

    s_reply_received = true;
    s_transaction_status = BACNET_WORKER_STATUS_OK;
}

static void worker_write_property_simple_ack_handler(
    BACNET_ADDRESS *src, uint8_t invoke_id)
{
    if (address_match(&s_target_address, src) && (invoke_id == s_current_invoke_id)) {
        ESP_LOGD(TAG_WORKER, "WriteProperty ack: success (Simple-ACK)");
        s_reply_received = true;
        s_transaction_status = BACNET_WORKER_STATUS_OK;
    }
}

static void worker_i_am_handler(
    uint8_t *service_request, uint16_t len, BACNET_ADDRESS *src)
{
    (void)len;
    uint32_t device_id = 0;
    unsigned max_apdu = 0;
    int segmentation = 0;
    uint16_t vendor_id = 0;

    int decoded = iam_decode_service_request(
        service_request, &device_id, &max_apdu, &segmentation, &vendor_id);
    if (decoded <= 0 || !src) {
        return;
    }

    char ip_str[16] = {0};
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
             src->mac[0], src->mac[1], src->mac[2], src->mac[3]);
    uint16_t port = 0;
    memcpy(&port, &src->mac[4], 2);
    if (port == 0) port = 47808;

    ESP_LOGI(TAG_WORKER, "I-Am received: Device %u from %s:%u (Vendor %u)",
             (unsigned)device_id, ip_str, (unsigned)port, (unsigned)vendor_id);

    address_add(device_id, max_apdu, src);

    for (size_t i = 0; i < s_discovered_count; i++) {
        if (s_discovered_devs[i].device_id == device_id &&
            strcmp(s_discovered_devs[i].ip, ip_str) == 0) {
            return;
        }
    }

    s_discovered_seen_count++;
    if (s_discovered_count < MAX_DISCOVERED_DEVICES) {
        size_t idx = s_discovered_count++;
        s_discovered_devs[idx].device_id = device_id;
        strlcpy(s_discovered_devs[idx].ip, ip_str, sizeof(s_discovered_devs[idx].ip));
        s_discovered_devs[idx].port = port;
        s_discovered_devs[idx].vendor_id = vendor_id;
        snprintf(s_discovered_devs[idx].vendor_name, sizeof(s_discovered_devs[idx].vendor_name),
                 vendor_id == 8 ? "Delta Controls" : "Vendor %u", (unsigned)vendor_id);
        strlcpy(s_discovered_devs[idx].model_name, "DAC Controller", sizeof(s_discovered_devs[idx].model_name));
        snprintf(s_discovered_devs[idx].name, sizeof(s_discovered_devs[idx].name),
                 "Device %u", (unsigned)device_id);
    }
}

static void init_worker_handlers(void)
{
    apdu_set_confirmed_ack_handler(
        SERVICE_CONFIRMED_READ_PROPERTY, worker_read_property_ack_handler);
    apdu_set_confirmed_simple_ack_handler(
        SERVICE_CONFIRMED_WRITE_PROPERTY, worker_write_property_simple_ack_handler);
    apdu_set_unconfirmed_handler(
        SERVICE_UNCONFIRMED_I_AM, worker_i_am_handler);
    apdu_set_error_handler(SERVICE_CONFIRMED_READ_PROPERTY, worker_error_handler);
    apdu_set_error_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, worker_error_handler);
    apdu_set_abort_handler(worker_abort_handler);
    apdu_set_reject_handler(worker_reject_handler);
}

static void bind_target_device_locked(void)
{
    memset(&s_target_address, 0, sizeof(s_target_address));
    uint16_t native_port = s_target_port;
    unsigned a, b, c, d;

    if (sscanf(s_target_ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        s_target_address.mac[0] = (uint8_t)a;
        s_target_address.mac[1] = (uint8_t)b;
        s_target_address.mac[2] = (uint8_t)c;
        s_target_address.mac[3] = (uint8_t)d;
        memcpy(&s_target_address.mac[4], &native_port, 2);
        s_target_address.mac_len = 6;
        s_target_address.net = 0;
        s_target_address.len = 0;

        address_add(s_target_dev_id, MAX_APDU, &s_target_address);
    }
}

/* ========================================================================= */
/* Transaction Engine & Pumping Loop                                         */
/* ========================================================================= */

static bool wait_for_worker_transaction(uint32_t timeout_ms)
{
    if (s_current_invoke_id == 0) return false;

    uint32_t max_wait = timeout_ms ? timeout_ms : BACNET_DEFAULT_TIMEOUT_MS;
    int iterations = (int)(max_wait / 20) + 1;
    uint32_t last_tick = xTaskGetTickCount();

    for (int i = 0; i < iterations; i++) {
        uint8_t pdu[BIP_MPDU_MAX] = {0};
        BACNET_ADDRESS src = {0};
        uint16_t pdu_len = bip_receive(&src, pdu, sizeof(pdu), 0);
        if (pdu_len) {
            npdu_handler(&src, pdu, pdu_len);
        }

        uint32_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = (now - last_tick) * portTICK_PERIOD_MS;
        if (elapsed_ms >= 50) {
            tsm_timer_milliseconds(elapsed_ms);
            last_tick = now;
        }

        if (s_reply_received) {
            s_consecutive_timeouts = 0;
            if (s_health_state != BACNET_HEALTH_ONLINE) {
                ESP_LOGI(TAG_WORKER, "Target recovered -> Health: ONLINE");
                s_health_state = BACNET_HEALTH_ONLINE;
            }
            return true;
        }
        if (s_reply_errored) {
            return false;
        }
        if (tsm_invoke_id_free(s_current_invoke_id)) {
            s_transaction_status = BACNET_WORKER_STATUS_TIMEOUT;
            return false;
        }
        if (tsm_invoke_id_failed(s_current_invoke_id)) {
            tsm_free_invoke_id(s_current_invoke_id);
            s_transaction_status = BACNET_WORKER_STATUS_TIMEOUT;
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_current_invoke_id != 0) {
        tsm_free_invoke_id(s_current_invoke_id);
    }
    s_transaction_status = BACNET_WORKER_STATUS_TIMEOUT;
    s_consecutive_timeouts++;
    if (s_consecutive_timeouts >= BACNET_OFFLINE_CONSECUTIVE_TIMEOUTS && s_health_state != BACNET_HEALTH_OFFLINE) {
        ESP_LOGW(TAG_WORKER, "Target unreachable (3 timeouts) -> Health: OFFLINE");
        s_health_state = BACNET_HEALTH_OFFLINE;
    } else if (s_health_state == BACNET_HEALTH_ONLINE) {
        s_health_state = BACNET_HEALTH_DEGRADED;
    }
    return false;
}

static void execute_worker_read(bacnet_request_t *req, bacnet_response_t *resp)
{
    s_reply_received = false;
    s_reply_errored = false;
    s_transaction_status = BACNET_WORKER_STATUS_TIMEOUT;

    s_current_invoke_id = Send_Read_Property_Request(
        s_target_dev_id, req->obj_type, req->obj_instance, req->property, req->array_index);

    bool ok = wait_for_worker_transaction(req->timeout_ms);
    resp->status = s_transaction_status;
    if (ok) {
        resp->app_val = s_last_read_value;
        resp->tag = s_last_read_value.tag;
        resp->raw_data_len = s_last_read_raw_len;
        if (s_last_read_raw_len > 0) {
            memcpy(resp->raw_data, s_last_read_raw, s_last_read_raw_len);
        }
    }
}

static void execute_worker_write(bacnet_request_t *req, bacnet_response_t *resp)
{
    s_reply_received = false;
    s_reply_errored = false;
    s_transaction_status = BACNET_WORKER_STATUS_TIMEOUT;

    s_current_invoke_id = Send_Write_Property_Request(
        s_target_dev_id, req->obj_type, req->obj_instance, req->property,
        &req->write_val, req->write_priority, req->array_index);

    bool ok = wait_for_worker_transaction(req->timeout_ms);
    resp->status = s_transaction_status;
    if (ok) {
        bacnet_cache_invalidate(req->obj_type, req->obj_instance, req->property);
    }
}

static void execute_worker_discovery(bacnet_request_t *req, bacnet_response_t *resp)
{
    (void)req;
    s_discovered_count = 0;
    s_discovered_seen_count = 0;

    Send_WhoIs_Global(-1, -1);

    uint16_t nport = s_target_port ? s_target_port : 47808;
    BACNET_ADDRESS bcast = {0};
    bcast.mac[0] = 10; bcast.mac[1] = 0; bcast.mac[2] = 255; bcast.mac[3] = 255;
    memcpy(&bcast.mac[4], &nport, 2);
    bcast.mac_len = 6;
    Send_WhoIs_To_Network(&bcast, -1, -1);

    bcast.mac[2] = 3;
    Send_WhoIs_To_Network(&bcast, -1, -1);

    for (unsigned oct = 1; oct <= 32; oct++) {
        BACNET_ADDRESS udest = {0};
        udest.mac[0] = 10; udest.mac[1] = 0; udest.mac[2] = 3; udest.mac[3] = (uint8_t)oct;
        memcpy(&udest.mac[4], &nport, 2);
        udest.mac_len = 6;
        Send_WhoIs_To_Network(&udest, -1, -1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    int64_t deadline = esp_timer_get_time() + 2000000;
    while (esp_timer_get_time() < deadline) {
        uint8_t pdu[BIP_MPDU_MAX] = {0};
        BACNET_ADDRESS src = {0};
        uint16_t pdu_len = bip_receive(&src, pdu, sizeof(pdu), 20);
        if (pdu_len > 0) {
            npdu_handler(&src, pdu, pdu_len);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    resp->status = BACNET_WORKER_STATUS_OK;
    resp->discovered_count = s_discovered_count;
    resp->discovered_seen_count = s_discovered_seen_count;
    memcpy(resp->discovered, s_discovered_devs, sizeof(s_discovered_devs));
}

static void execute_worker_probe(void)
{
    s_reply_received = false;
    s_reply_errored = false;
    s_transaction_status = BACNET_WORKER_STATUS_TIMEOUT;

    s_current_invoke_id = Send_Read_Property_Request(
        s_target_dev_id, OBJECT_DEVICE, s_target_dev_id, PROP_OBJECT_NAME, BACNET_ARRAY_ALL);

    wait_for_worker_transaction(800);
}

/* ========================================================================= */
/* Worker Task Main Loop                                                     */
/* ========================================================================= */

static void bacnet_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG_WORKER, "BACnet Worker Task starting on netif %p, target %s:%u dev_id %u",
             s_netif, s_target_ip, s_target_port, (unsigned)s_target_dev_id);

    bip_socket_esp_idf_set_netif(s_netif);
    if (!bip_init(s_target_port)) {
        ESP_LOGE(TAG_WORKER, "bip_init(%u) failed", s_target_port);
        vTaskDelete(NULL);
        return;
    }

    address_init();
    init_worker_handlers();

    int bind_attempt = 0;
    unsigned max_apdu = 0;
    for (;;) {
        bind_target_device_locked();
        if (address_bind_request(s_target_dev_id, &max_apdu, &s_target_address)) {
            break;
        }
        bind_attempt++;
        ESP_LOGE(TAG_WORKER, "address_bind_request failed (attempt %d) - retrying in 5s", bind_attempt);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (bind_attempt > 0) {
        ESP_LOGI(TAG_WORKER, "bacnet bind succeeded after %d retries", bind_attempt);
    }

    s_is_ready = true;
    s_health_state = BACNET_HEALTH_ONLINE;
    s_consecutive_timeouts = 0;
    s_last_probe_time_us = esp_timer_get_time();

    ESP_LOGI(TAG_WORKER, "BACnet Worker Task ready and processing queues");

    bacnet_request_t req;
    for (;;) {
        bool got_work = false;

        /* Drain high priority queue first */
        if (xQueueReceive(s_high_prio_queue, &req, 0) == pdTRUE) {
            got_work = true;
        } else if (xQueueReceive(s_normal_prio_queue, &req, pdMS_TO_TICKS(50)) == pdTRUE) {
            got_work = true;
        }

        if (got_work) {
            bacnet_response_t local_resp = {0};
            bacnet_response_t *resp_ptr = req.out_resp ? req.out_resp : &local_resp;

            /* Check circuit breaker for normal/low prio read requests */
            if (s_health_state == BACNET_HEALTH_OFFLINE && req.prio != BACNET_PRIO_HIGH && req.op == BACNET_OP_READ_PROPERTY) {
                resp_ptr->status = BACNET_WORKER_STATUS_TARGET_OFFLINE;
            } else {
                switch (req.op) {
                    case BACNET_OP_READ_PROPERTY:
                        execute_worker_read(&req, resp_ptr);
                        break;
                    case BACNET_OP_WRITE_PROPERTY:
                        execute_worker_write(&req, resp_ptr);
                        break;
                    case BACNET_OP_WHOIS_DISCOVERY:
                        execute_worker_discovery(&req, resp_ptr);
                        break;
                    case BACNET_OP_PROBE_LIVENESS:
                        execute_worker_probe();
                        resp_ptr->status = s_transaction_status;
                        break;
                    default:
                        resp_ptr->status = BACNET_WORKER_STATUS_INVALID_ARG;
                        break;
                }
            }

            if (req.sync_sem) {
                xSemaphoreGive(req.sync_sem);
            } else if (req.async_cb) {
                req.async_cb(resp_ptr, req.async_arg);
            }
        }

        /* Periodic background probe when OFFLINE / DEGRADED or idle */
        int64_t now_us = esp_timer_get_time();
        if (s_health_state == BACNET_HEALTH_OFFLINE || s_health_state == BACNET_HEALTH_DEGRADED) {
            if ((now_us - s_last_probe_time_us) / 1000 >= BACNET_PROBE_INTERVAL_MS) {
                s_last_probe_time_us = now_us;
                execute_worker_probe();
            }
        }
    }
}

/* ========================================================================= */
/* Public API Implementation                                                 */
/* ========================================================================= */

esp_err_t bacnet_worker_start(
    esp_netif_t *netif, uint32_t target_dev_id, const char *target_ip, uint16_t target_port)
{
    if (s_worker_task_handle != NULL) {
        return ESP_OK; /* Already running */
    }

    s_netif = netif;
    s_target_dev_id = target_dev_id;
    if (target_ip) strlcpy(s_target_ip, target_ip, sizeof(s_target_ip));
    s_target_port = target_port ? target_port : 47808;

    s_high_prio_queue = xQueueCreateStatic(
        BACNET_HIGH_PRIO_QUEUE_DEPTH, sizeof(bacnet_request_t),
        s_high_prio_queue_storage, &s_high_prio_queue_struct);

    s_normal_prio_queue = xQueueCreateStatic(
        BACNET_NORMAL_PRIO_QUEUE_DEPTH, sizeof(bacnet_request_t),
        s_normal_prio_queue_storage, &s_normal_prio_queue_struct);

    if (!s_high_prio_queue || !s_normal_prio_queue) {
        ESP_LOGE(TAG_WORKER, "Failed to create FreeRTOS queues");
        return ESP_ERR_NO_MEM;
    }

    s_worker_task_handle = xTaskCreateStatic(
        bacnet_worker_task, "bacnet_worker",
        sizeof(s_worker_stack) / sizeof(StackType_t),
        NULL, 5, s_worker_stack, &s_worker_tcb);

    if (!s_worker_task_handle) {
        ESP_LOGE(TAG_WORKER, "Failed to create static worker task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void bacnet_worker_set_target(uint32_t dev_id, const char *ip, uint16_t port)
{
    taskENTER_CRITICAL(&s_config_mux);
    s_target_dev_id = dev_id;
    if (ip) strlcpy(s_target_ip, ip, sizeof(s_target_ip));
    if (port) s_target_port = port;
    taskEXIT_CRITICAL(&s_config_mux);
    bind_target_device_locked();
}

void bacnet_worker_get_target(uint32_t *dev_id, char *ip_buf, size_t ip_len, uint16_t *port)
{
    taskENTER_CRITICAL(&s_config_mux);
    if (dev_id) *dev_id = s_target_dev_id;
    if (ip_buf && ip_len > 0) strlcpy(ip_buf, s_target_ip, ip_len);
    if (port) *port = s_target_port;
    taskEXIT_CRITICAL(&s_config_mux);
}

bool bacnet_worker_is_ready(void)
{
    return s_is_ready;
}

bacnet_health_state_t bacnet_worker_get_health(void)
{
    return s_health_state;
}

TaskHandle_t bacnet_worker_get_task_handle(void)
{
    return s_worker_task_handle;
}

bacnet_worker_status_t bacnet_worker_dispatch_sync(
    bacnet_request_t *req, bacnet_response_t *resp, uint32_t timeout_ms)
{
    if (!req || !resp || !s_is_ready) {
        return BACNET_WORKER_STATUS_INVALID_ARG;
    }

    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) return BACNET_WORKER_STATUS_QUEUE_FULL;

    req->sync_sem = sem;
    req->out_resp = resp;

    QueueHandle_t target_q = (req->prio == BACNET_PRIO_HIGH) ? s_high_prio_queue : s_normal_prio_queue;
    if (xQueueSend(target_q, req, pdMS_TO_TICKS(100)) != pdTRUE) {
        vSemaphoreDelete(sem);
        return BACNET_WORKER_STATUS_QUEUE_FULL;
    }

    uint32_t wait_ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : (req->timeout_ms + 200));
    if (xSemaphoreTake(sem, wait_ticks) != pdTRUE) {
        vSemaphoreDelete(sem);
        return BACNET_WORKER_STATUS_TIMEOUT;
    }

    vSemaphoreDelete(sem);
    return resp->status;
}

esp_err_t bacnet_worker_dispatch_async(
    const bacnet_request_t *req, bacnet_async_cb_t cb, void *user_arg)
{
    if (!req || !s_is_ready) return ESP_ERR_INVALID_ARG;

    bacnet_request_t async_req = *req;
    async_req.sync_sem = NULL;
    async_req.async_cb = cb;
    async_req.async_arg = user_arg;
    async_req.out_resp = NULL;

    QueueHandle_t target_q = (req->prio == BACNET_PRIO_HIGH) ? s_high_prio_queue : s_normal_prio_queue;
    if (xQueueSend(target_q, &async_req, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ========================================================================= */
/* Typed Synchronous Helpers                                                 */
/* ========================================================================= */

bool bacnet_worker_read_real(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float *out_val)
{
    if (!out_val) return false;
    if (bacnet_cache_get(type, instance, prop, BC_KIND_REAL, out_val)) {
        return true;
    }
    if (!s_is_ready) return false;

    bacnet_request_t req = {
        .op = BACNET_OP_READ_PROPERTY,
        .prio = BACNET_PRIO_NORMAL,
        .obj_type = type,
        .obj_instance = instance,
        .property = prop,
        .array_index = BACNET_ARRAY_ALL,
        .timeout_ms = BACNET_DEFAULT_TIMEOUT_MS
    };
    bacnet_response_t resp = {0};
    if (bacnet_worker_dispatch_sync(&req, &resp, 1200) == BACNET_WORKER_STATUS_OK) {
        if (resp.app_val.tag == BACNET_APPLICATION_TAG_REAL) {
            *out_val = resp.app_val.type.Real;
            bacnet_cache_put(type, instance, prop, BC_KIND_REAL, out_val);
            return true;
        }
    }
    return false;
}

bool bacnet_worker_write_real(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float val)
{
    if (!s_is_ready) return false;

    BACNET_APPLICATION_DATA_VALUE write_val = {0};
    write_val.tag = BACNET_APPLICATION_TAG_REAL;
    write_val.type.Real = val;

    bacnet_request_t req = {
        .op = BACNET_OP_WRITE_PROPERTY,
        .prio = BACNET_PRIO_HIGH,
        .obj_type = type,
        .obj_instance = instance,
        .property = prop,
        .array_index = BACNET_ARRAY_ALL,
        .write_priority = BACNET_NO_PRIORITY,
        .write_val = write_val,
        .timeout_ms = BACNET_DEFAULT_TIMEOUT_MS
    };
    bacnet_response_t resp = {0};
    return (bacnet_worker_dispatch_sync(&req, &resp, 1200) == BACNET_WORKER_STATUS_OK);
}

bool bacnet_worker_read_bool(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool *out_val)
{
    if (!out_val) return false;
    if (bacnet_cache_get(type, instance, prop, BC_KIND_BOOL, out_val)) {
        return true;
    }
    if (!s_is_ready) return false;

    bacnet_request_t req = {
        .op = BACNET_OP_READ_PROPERTY,
        .prio = BACNET_PRIO_NORMAL,
        .obj_type = type,
        .obj_instance = instance,
        .property = prop,
        .array_index = BACNET_ARRAY_ALL,
        .timeout_ms = BACNET_DEFAULT_TIMEOUT_MS
    };
    bacnet_response_t resp = {0};
    if (bacnet_worker_dispatch_sync(&req, &resp, 1200) == BACNET_WORKER_STATUS_OK) {
        if (resp.app_val.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
            *out_val = (resp.app_val.type.Enumerated != 0);
            bacnet_cache_put(type, instance, prop, BC_KIND_BOOL, out_val);
            return true;
        }
    }
    return false;
}

bool bacnet_worker_write_bool(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool val)
{
    if (!s_is_ready) return false;

    BACNET_APPLICATION_DATA_VALUE write_val = {0};
    write_val.tag = BACNET_APPLICATION_TAG_ENUMERATED;
    write_val.type.Enumerated = val ? 1 : 0;

    bacnet_request_t req = {
        .op = BACNET_OP_WRITE_PROPERTY,
        .prio = BACNET_PRIO_HIGH,
        .obj_type = type,
        .obj_instance = instance,
        .property = prop,
        .array_index = BACNET_ARRAY_ALL,
        .write_priority = BACNET_NO_PRIORITY,
        .write_val = write_val,
        .timeout_ms = BACNET_DEFAULT_TIMEOUT_MS
    };
    bacnet_response_t resp = {0};
    return (bacnet_worker_dispatch_sync(&req, &resp, 1200) == BACNET_WORKER_STATUS_OK);
}

bool bacnet_worker_read_msv(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned *out_val)
{
    if (!out_val) return false;
    if (bacnet_cache_get(type, instance, prop, BC_KIND_MSV, out_val)) {
        return true;
    }
    if (!s_is_ready) return false;

    bacnet_request_t req = {
        .op = BACNET_OP_READ_PROPERTY,
        .prio = BACNET_PRIO_NORMAL,
        .obj_type = type,
        .obj_instance = instance,
        .property = prop,
        .array_index = BACNET_ARRAY_ALL,
        .timeout_ms = BACNET_DEFAULT_TIMEOUT_MS
    };
    bacnet_response_t resp = {0};
    if (bacnet_worker_dispatch_sync(&req, &resp, 1200) == BACNET_WORKER_STATUS_OK) {
        if (resp.app_val.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
            *out_val = (unsigned)resp.app_val.type.Unsigned_Int;
            bacnet_cache_put(type, instance, prop, BC_KIND_MSV, out_val);
            return true;
        }
    }
    return false;
}

bool bacnet_worker_write_msv(
    BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned val)
{
    if (!s_is_ready) return false;

    BACNET_APPLICATION_DATA_VALUE write_val = {0};
    write_val.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
    write_val.type.Unsigned_Int = val;

    bacnet_request_t req = {
        .op = BACNET_OP_WRITE_PROPERTY,
        .prio = BACNET_PRIO_HIGH,
        .obj_type = type,
        .obj_instance = instance,
        .property = prop,
        .array_index = BACNET_ARRAY_ALL,
        .write_priority = BACNET_NO_PRIORITY,
        .write_val = write_val,
        .timeout_ms = BACNET_DEFAULT_TIMEOUT_MS
    };
    bacnet_response_t resp = {0};
    return (bacnet_worker_dispatch_sync(&req, &resp, 1200) == BACNET_WORKER_STATUS_OK);
}

/* ========================================================================= */
/* Synchronous Explorer & Discovery Helpers                                  */
/* ========================================================================= */

bacnet_worker_status_t bacnet_worker_explorer_read_sync(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, uint32_t array_index,
    bacnet_response_t *resp, uint32_t timeout_ms)
{
    if (!resp || !s_is_ready) return BACNET_WORKER_STATUS_INVALID_ARG;

    bacnet_request_t req = {
        .op = BACNET_OP_READ_PROPERTY,
        .prio = BACNET_PRIO_NORMAL,
        .obj_type = object_type,
        .obj_instance = object_instance,
        .property = property,
        .array_index = array_index,
        .timeout_ms = timeout_ms ? timeout_ms : BACNET_DEFAULT_TIMEOUT_MS
    };
    return bacnet_worker_dispatch_sync(&req, resp, timeout_ms + 400);
}

bacnet_worker_status_t bacnet_worker_explorer_write_sync(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, uint32_t array_index,
    uint8_t priority, const BACNET_APPLICATION_DATA_VALUE *val,
    uint32_t timeout_ms)
{
    if (!val || !s_is_ready) return BACNET_WORKER_STATUS_INVALID_ARG;

    bacnet_request_t req = {
        .op = BACNET_OP_WRITE_PROPERTY,
        .prio = BACNET_PRIO_HIGH,
        .obj_type = object_type,
        .obj_instance = object_instance,
        .property = property,
        .array_index = array_index,
        .write_priority = priority,
        .write_val = *val,
        .timeout_ms = timeout_ms ? timeout_ms : BACNET_DEFAULT_TIMEOUT_MS
    };
    bacnet_response_t resp = {0};
    return bacnet_worker_dispatch_sync(&req, &resp, timeout_ms + 400);
}

bacnet_worker_status_t bacnet_worker_discover_sync(
    bacnet_discovered_dev_t *out_devs, size_t max_devs,
    size_t *out_count, size_t *out_seen_count, uint32_t timeout_ms)
{
    if (!out_devs || !out_count || !s_is_ready) return BACNET_WORKER_STATUS_INVALID_ARG;

    bacnet_request_t req = {
        .op = BACNET_OP_WHOIS_DISCOVERY,
        .prio = BACNET_PRIO_HIGH,
        .timeout_ms = timeout_ms ? timeout_ms : 3000
    };
    bacnet_response_t resp = {0};
    bacnet_worker_status_t status = bacnet_worker_dispatch_sync(&req, &resp, timeout_ms + 1000);
    if (status == BACNET_WORKER_STATUS_OK) {
        size_t count = resp.discovered_count > max_devs ? max_devs : resp.discovered_count;
        memcpy(out_devs, resp.discovered, count * sizeof(bacnet_discovered_dev_t));
        *out_count = count;
        if (out_seen_count) *out_seen_count = resp.discovered_seen_count;
    }
    return status;
}
