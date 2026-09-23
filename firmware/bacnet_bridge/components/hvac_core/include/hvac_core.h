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

/* Whole-unit controller points (system power, boost, health diagnostics).
 * hvac_core is the single owner of these object references: nothing else
 * may hardcode their instance numbers. Each install maps them once (setup
 * wizard or Objects page: scan -> name match -> confirm) and the result is
 * persisted to NVS. With no stored mapping the reference Delta DAC-1180E
 * program's verified point map applies, so existing installs keep working
 * unchanged across the upgrade. */
typedef enum {
    HVAC_POINT_SYS_POWER_WRITE = 0,
    HVAC_POINT_SYS_POWER_READBACK,
    HVAC_POINT_BOOST_MODE,
    HVAC_POINT_DESIGN_COOLING_DUTY,
    HVAC_POINT_COOLING_OUTPUT,
    HVAC_POINT_REQUIRED_COOLING_OUTPUT,
    HVAC_POINT_COOLING_FLOW,
    HVAC_POINT_REQUIRED_COOLING_FLOW,
    HVAC_POINT_COOLING_FLOW_DESIGN_PCT,
    HVAC_POINT_COOLING_VALVE_SIGNAL,
    HVAC_POINT_DESIGN_HEATING_DUTY,
    HVAC_POINT_HEATING_OUTPUT,
    HVAC_POINT_REQUIRED_HEATING_OUTPUT,
    HVAC_POINT_HEATING_FLOW,
    HVAC_POINT_REQUIRED_HEATING_FLOW,
    HVAC_POINT_HEATING_FLOW_DESIGN_PCT,
    HVAC_POINT_HEATING_VALVE_SIGNAL,
    HVAC_POINT_RETURN_AIR,
    HVAC_POINT_FAN_COUNT,
    HVAC_POINT_COOLING_VALVE_STATUS,
    HVAC_POINT_HEATING_VALVE_STATUS,
    HVAC_POINT_COUNT
} hvac_point_id_t;

/* What the firmware does with the value - decides which object types are
 * compatible with the point (a real can come from AI/AO/AV, and so on). */
typedef enum {
    HVAC_POINT_KIND_REAL = 0,
    HVAC_POINT_KIND_BOOL,
    HVAC_POINT_KIND_MULTISTATE,
} hvac_point_kind_t;

typedef enum {
    HVAC_POINT_SOURCE_DEFAULT = 0, /* reference map, never confirmed on this install */
    HVAC_POINT_SOURCE_CONFIRMED,   /* chosen and saved by the installer */
    HVAC_POINT_SOURCE_UNMAPPED,    /* installer confirmed this controller has no such point */
} hvac_point_source_t;

/* Static description of one point. Lives in flash (.rodata); the patterns
 * are case-insensitive regular expressions evaluated by the browser against
 * the scanned object names, so the device never runs a regex engine. */
typedef struct {
    const char *key;            /* NVS key and API id (<= 15 chars) */
    const char *label;          /* installer-facing name */
    const char *group;          /* "control" or "health" */
    const char *description;    /* what it is used for */
    const char *reference_name; /* object name on the reference controller */
    const char *pattern;        /* name-match regex */
    hvac_point_kind_t kind;
    bool writable;              /* firmware writes to it */
    uint16_t default_type;      /* BACNET_OBJECT_TYPE on the reference controller */
    uint32_t default_instance;
} hvac_point_def_t;

const hvac_point_def_t *hvac_core_point_def(hvac_point_id_t id);
/* Binding currently in force; false when the point is unmapped. */
bool hvac_core_point_get(hvac_point_id_t id, uint16_t *out_type, uint32_t *out_instance);
hvac_point_source_t hvac_core_point_source(hvac_point_id_t id);
/* Whether an object type can carry this point's value (and be written, if
 * the point is written). */
bool hvac_core_point_type_ok(hvac_point_id_t id, uint16_t type);
/* Stage a binding in RAM. mapped=false records "not present on this
 * controller". Returns false on an incompatible type or bad instance. */
bool hvac_core_point_set(hvac_point_id_t id, bool mapped, uint16_t type, uint32_t instance);
/* Reference map, discarding any confirmed bindings (RAM only). */
void hvac_core_points_reset(void);
void hvac_core_points_load(void);
/* Persists the whole map; must run on an internal-RAM stack (flash write). */
bool hvac_core_points_save(void);
/* NVS erase of the stored map; same stack rule as save. */
void hvac_core_points_erase(void);

/* Typed access through the map. Every one returns false for an unmapped
 * point without touching the network, so callers' existing invalid-value
 * paths cover "this controller has no such point". */
bool hvac_core_point_read_real(hvac_point_id_t id, float *out_val);
bool hvac_core_point_read_bool(hvac_point_id_t id, bool *out_val);
bool hvac_core_point_read_multistate(hvac_point_id_t id, unsigned *out_val);
bool hvac_core_point_write_bool(hvac_point_id_t id, bool val);
bool hvac_core_point_write_multistate(hvac_point_id_t id, unsigned val);

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
