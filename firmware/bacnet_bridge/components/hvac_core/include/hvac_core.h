#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol-neutral room mapping. Integrations own neither this data nor its NVS schema. */
#define HVAC_CORE_MAX_ROOMS 8

#define SYS_POWER_WRITE_INSTANCE 13
#define SYS_POWER_READBACK_INSTANCE 1
#define MIN_SETPOINT_C 18.0f
#define MAX_SETPOINT_C 30.0f

typedef struct {
    char name[32];
    bool active;
    uint32_t setpoint_instance;
    uint32_t temperature_instance;
    uint32_t power_instance;
    uint32_t supply_air_instance;
    uint32_t required_output_instance;
    uint32_t current_output_instance;
} hvac_room_config_t;

typedef enum {
    HVAC_INTEGRATION_NONE = 0,
    HVAC_INTEGRATION_MQTT_HOME_ASSISTANT,
    HVAC_INTEGRATION_MATTER,
} hvac_integration_kind_t;

/* Modules with a setup handshake (currently just Matter's BLE commissioning)
 * persist how their last attempt went, so the wizard can describe the
 * current situation instead of the dashboard just silently reappearing.
 * MQTT has no equivalent handshake - its connect/disconnect state is
 * already live-visible via /api/integration's mqtt_active, not persisted.
 */
typedef enum {
    HVAC_PAIRING_IDLE = 0,       /* module not selected, or long since settled */
    HVAC_PAIRING_AWAITING = 1,   /* handshake window currently open */
    HVAC_PAIRING_PAIRED = 2,     /* handshake completed successfully */
    HVAC_PAIRING_TIMED_OUT = 3,  /* window closed before handshake completed */
} hvac_pairing_status_t;

extern hvac_room_config_t HvacRooms[HVAC_CORE_MAX_ROOMS];
extern size_t HvacRoomCount;

void hvac_core_rooms_load(void);
void hvac_core_rooms_save(void);

/* Default is MQTT for backwards-compatible upgrades from existing firmware. */
void hvac_core_integration_load(void);
hvac_integration_kind_t hvac_core_integration_get(void);
bool hvac_core_integration_set(hvac_integration_kind_t integration);
void hvac_core_integration_reset(void);

/* Persisted so a reboot mid-timeout (or shortly after one) doesn't lose the
 * "you need to retry pairing" signal. Loaded once alongside the mode. */
hvac_pairing_status_t hvac_core_matter_pairing_get(void);
bool hvac_core_matter_pairing_set(hvac_pairing_status_t status);

/* Protocol-neutral semantic HVAC operations (Single Source of Truth) */
hvac_room_config_t *hvac_core_get_room(size_t room_idx);
size_t hvac_core_get_room_count(void);

bool hvac_core_set_room_setpoint(size_t room_idx, float requested, float *applied);
bool hvac_core_set_room_power(size_t room_idx, bool on);
bool hvac_core_set_system_power(bool on);

bool hvac_core_get_room_setpoint(size_t room_idx, float *out_val);
bool hvac_core_get_room_temp(size_t room_idx, float *out_val);
bool hvac_core_get_room_power(size_t room_idx, bool *out_val);
bool hvac_core_get_system_power(bool *out_val);
bool hvac_core_get_system_power_commanded(bool *out_val);

#ifdef __cplusplus
}
#endif
