#ifndef BACNET_WORKER_H
#define BACNET_WORKER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_netif.h"
#include "esp_err.h"

#include "bacnet/bacdef.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacenum.h"
#include "bacnet/bactext.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACNET_WORKER_STACK_SIZE     24576
#define BACNET_HIGH_PRIO_QUEUE_DEPTH 8
#define BACNET_NORMAL_PRIO_QUEUE_DEPTH 24
#define BACNET_DEFAULT_TIMEOUT_MS    800
#define BACNET_PROBE_INTERVAL_MS     5000
#define BACNET_OFFLINE_CONSECUTIVE_TIMEOUTS 3
#define BACNET_CACHE_TTL_MS          30000
#define BACNET_CACHE_ENTRIES         64
#define MAX_DISCOVERED_DEVICES       8

typedef enum {
    BACNET_WORKER_STATUS_OK = 0,
    BACNET_WORKER_STATUS_TIMEOUT,
    BACNET_WORKER_STATUS_ERROR,
    BACNET_WORKER_STATUS_ABORT,
    BACNET_WORKER_STATUS_REJECT,
    BACNET_WORKER_STATUS_TARGET_OFFLINE,
    BACNET_WORKER_STATUS_QUEUE_FULL,
    BACNET_WORKER_STATUS_INVALID_ARG
} bacnet_worker_status_t;

typedef bacnet_worker_status_t bacnet_status_t;

typedef enum {
    BACNET_HEALTH_ONLINE = 0,
    BACNET_HEALTH_DEGRADED,
    BACNET_HEALTH_OFFLINE
} bacnet_health_state_t;

typedef enum {
    BACNET_PRIO_LOW = 0,
    BACNET_PRIO_NORMAL,
    BACNET_PRIO_HIGH
} bacnet_prio_t;

typedef enum {
    BACNET_OP_READ_PROPERTY = 0,
    BACNET_OP_WRITE_PROPERTY,
    BACNET_OP_WHOIS_DISCOVERY,
    BACNET_OP_PROBE_LIVENESS
} bacnet_op_t;

typedef struct {
    uint32_t device_id;
    char ip[16];
    uint16_t port;
    uint16_t vendor_id;
    char name[48];
    char vendor_name[48];
    char model_name[48];
} bacnet_discovered_dev_t;

typedef struct {
    bacnet_worker_status_t status;
    BACNET_APPLICATION_DATA_VALUE app_val;
    uint8_t tag;
    uint8_t raw_data[MAX_APDU];
    int raw_data_len;
    bacnet_discovered_dev_t discovered[MAX_DISCOVERED_DEVICES];
    size_t discovered_count;
    /* Unique devices seen this discovery pass, even past
     * MAX_DISCOVERED_DEVICES - lets a caller tell "found everything" apart
     * from "storage capped, more devices actually responded". */
    size_t discovered_seen_count;
} bacnet_response_t;

typedef void (*bacnet_async_cb_t)(const bacnet_response_t *resp, void *user_arg);

typedef struct {
    bacnet_op_t op;
    bacnet_prio_t prio;
    BACNET_OBJECT_TYPE obj_type;
    uint32_t obj_instance;
    BACNET_PROPERTY_ID property;
    uint32_t array_index;
    uint8_t write_priority;
    BACNET_APPLICATION_DATA_VALUE write_val;
    uint32_t timeout_ms;
    SemaphoreHandle_t sync_sem;
    bacnet_response_t *out_resp;
    bacnet_async_cb_t async_cb;
    void *async_arg;
} bacnet_request_t;

/* Lifecycle and configuration */
esp_err_t bacnet_worker_start(esp_netif_t *netif, uint32_t target_dev_id, const char *target_ip, uint16_t target_port);
void bacnet_worker_set_target(uint32_t dev_id, const char *ip, uint16_t port);
void bacnet_worker_get_target(uint32_t *dev_id, char *ip_buf, size_t ip_len, uint16_t *port);
bool bacnet_worker_is_ready(void);
bacnet_health_state_t bacnet_worker_get_health(void);
TaskHandle_t bacnet_worker_get_task_handle(void);

/* Queue dispatch */
bacnet_worker_status_t bacnet_worker_dispatch_sync(bacnet_request_t *req, bacnet_response_t *resp, uint32_t timeout_ms);
esp_err_t bacnet_worker_dispatch_async(const bacnet_request_t *req, bacnet_async_cb_t cb, void *user_arg);

/* Synchronous typed helpers */
bool bacnet_worker_read_real(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float *out_val);
bool bacnet_worker_write_real(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float val);
bool bacnet_worker_read_bool(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool *out_val);
bool bacnet_worker_write_bool(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool val);
bool bacnet_worker_read_msv(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned *out_val);
bool bacnet_worker_write_msv(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned val);

/* Synchronous explorer & discovery helpers */
bacnet_worker_status_t bacnet_worker_explorer_read_sync(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, uint32_t array_index,
    bacnet_response_t *resp, uint32_t timeout_ms);

bacnet_worker_status_t bacnet_worker_explorer_write_sync(
    BACNET_OBJECT_TYPE object_type, uint32_t object_instance,
    BACNET_PROPERTY_ID property, uint32_t array_index,
    uint8_t priority, const BACNET_APPLICATION_DATA_VALUE *val,
    uint32_t timeout_ms);

/* out_seen_count (nullable) is the true number of unique devices seen this
 * pass, even past max_devs - compare it to *out_count to detect truncation. */
bacnet_worker_status_t bacnet_worker_discover_sync(
    bacnet_discovered_dev_t *out_devs, size_t max_devs,
    size_t *out_count, size_t *out_seen_count, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BACNET_WORKER_H */
