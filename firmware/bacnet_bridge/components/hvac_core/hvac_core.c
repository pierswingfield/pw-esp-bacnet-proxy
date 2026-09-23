#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "nvs.h"
#include "hvac_core.h"
#include "bacnet_worker.h"

#define NVS_ROOMS_NAMESPACE "nvs_rooms"
#define NVS_INTEGRATION_NAMESPACE "nvs_integ"
#define NVS_KEY_INTEGRATION_MODE "mode"
#define NVS_KEY_MATTER_PAIRING "matter_pair"
#define NVS_POINTS_NAMESPACE "nvs_points"
/* Stored value for "installer confirmed this controller has no such point".
 * Real bindings are BACnet object identifiers (type << 22 | instance), and
 * object type 1023 is reserved by the standard, so this can never collide. */
#define POINT_OBJID_UNMAPPED 0xFFFFFFFFu
#define POINT_MAX_INSTANCE 0x3FFFFEu /* BACNET_MAX_INSTANCE - 4194303 is the wildcard */

hvac_room_config_t HvacRooms[HVAC_CORE_MAX_ROOMS] = {
    { "Room A", true, 1100, 1101, 1101, 1102, 1105, 1106 },
    { "Room B", true, 1200, 1201, 1201, 1202, 1205, 1206 },
    { "Room C", false, 1300, 1301, 1301, 1302, 1305, 1306 },
    { "Room D", false, 1400, 1401, 1401, 1402, 1405, 1406 },
    { "Room E", false, 1500, 1501, 1501, 1502, 1505, 1506 },
};
size_t HvacRoomCount = 5;
static hvac_integration_kind_t IntegrationKind = HVAC_INTEGRATION_MQTT_HOME_ASSISTANT;
static hvac_pairing_status_t MatterPairingStatus = HVAC_PAIRING_IDLE;

static const hvac_room_config_t DefaultRooms[HVAC_CORE_MAX_ROOMS] = {
    { "Room A", true, 1100, 1101, 1101, 1102, 1105, 1106 },
    { "Room B", true, 1200, 1201, 1201, 1202, 1205, 1206 },
    { "Room C", false, 1300, 1301, 1301, 1302, 1305, 1306 },
    { "Room D", false, 1400, 1401, 1401, 1402, 1405, 1406 },
    { "Room E", false, 1500, 1501, 1501, 1502, 1505, 1506 },
};

/* ========================================================================= */
/* Whole-unit point map                                                      */
/* ========================================================================= */

/* Reference map: the Delta DAC-1180E program this bridge was built against,
 * each instance verified live or against its EPICS dump (see the notes that
 * used to sit next to the old per-point #defines in main.c, preserved in the
 * descriptions below). Patterns are anchored so per-room objects such as
 * "Room_A Run Status" or "Room_A Design Cooling Duty" never match the
 * whole-unit points, and each one matches exactly one object in the
 * reference controller's 426-object catalogue. */
static const hvac_point_def_t PointDefs[HVAC_POINT_COUNT] = {
    [HVAC_POINT_SYS_POWER_WRITE] = {
        "sys_pwr_cmd", "System power command", "control",
        "Written to switch the whole plant on/off (dashboard, MQTT, Matter system switch). Read back as the commanded state.",
        "BMS Run Signal",
        "^(bms|bas|remote|master)\\s*(run|enable|start|on\\W*off)\\s*(signal|command|cmd|request)?$",
        HVAC_POINT_KIND_BOOL, true, OBJECT_BINARY_VALUE, 13},
    [HVAC_POINT_SYS_POWER_READBACK] = {
        "sys_pwr_fb", "System running status", "control",
        "Read-only: whether the unit is actually running. Can differ from the command (e.g. no room calling).",
        "FCU Run Status",
        "^(fcu\\s*|unit\\s*|system\\s*)?run(ning)?\\s*status$",
        HVAC_POINT_KIND_BOOL, false, OBJECT_BINARY_VALUE, 1},
    [HVAC_POINT_BOOST_MODE] = {
        "boost_mode", "Boost / operating mode", "control",
        "Written for Boost Heat/Boost Cool (web, MQTT, Matter boost switches). States: 1=Auto, 4=Full heating, 5=Full cooling.",
        "FCU Operating Mode",
        "^(fcu\\s*|unit\\s*)?operat(ing|ion)\\s*mode$",
        HVAC_POINT_KIND_MULTISTATE, true, OBJECT_MULTI_STATE_VALUE, 1},
    /* Design (nameplate) max, NOT a live demand. Confirmed on real hardware:
     * reads 4.40 kW, identical to what the required cooling output reports
     * while Boost is in Full Cooling. The health objects were confirmed by
     * name/units against the reference unit's EPICS dump. */
    [HVAC_POINT_DESIGN_COOLING_DUTY] = {
        "cool_design", "Design cooling duty", "health",
        "Nameplate cooling capacity (kW); the reference the required output is pinned to under Boost Cool.",
        "FCU Design Cooling Duty (Sensible)",
        "^(fcu\\s*)?design\\s*cooling\\s*duty(\\s*\\(?sensible\\)?)?$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 25},
    [HVAC_POINT_COOLING_OUTPUT] = {
        "cool_out", "Current cooling output", "health", "Live cooling delivered (kW).",
        "Overall Current Cooling Output",
        "^(overall\\s*)?current\\s*cooling\\s*output$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 26},
    [HVAC_POINT_REQUIRED_COOLING_OUTPUT] = {
        "cool_req", "Required cooling output", "health", "Cooling the controller is asking for (kW).",
        "Overall Required Cooling Output",
        "^(overall\\s*)?required\\s*cooling\\s*output$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 27},
    [HVAC_POINT_COOLING_FLOW] = {
        "cool_flow", "Cooling valve flow", "health", "Measured chilled-water flow (L/s).",
        "Cooling Valve Current Flow Rate",
        "^cooling\\s*valve\\s*current\\s*flow(\\s*rate)?$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 42},
    [HVAC_POINT_REQUIRED_COOLING_FLOW] = {
        "cool_flow_req", "Cooling valve required flow", "health", "Chilled-water flow the controller wants (L/s).",
        "Cooling Valve Required Flow Rate",
        "^cooling\\s*valve\\s*required\\s*flow(\\s*rate)?$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 43},
    [HVAC_POINT_COOLING_FLOW_DESIGN_PCT] = {
        "cool_flow_pct", "Cooling flow % of design", "health", "Controller-computed flow as a percentage of design flow.",
        "Cooling Valve Percentage of Design Flow",
        "^cooling\\s*valve\\s*(percent(age)?|%|pct)\\s*(of\\s*)?design\\s*flow$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 45},
    [HVAC_POINT_COOLING_VALVE_SIGNAL] = {
        "cool_valve", "Cooling valve signal", "health", "Cooling valve drive signal (%).",
        "Cooling Valve Control Signal",
        "^cooling\\s*valve\\s*(control\\s*)?signal$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_OUTPUT, 8},
    [HVAC_POINT_DESIGN_HEATING_DUTY] = {
        "heat_design", "Design heating duty", "health", "Nameplate heating capacity (kW).",
        "FCU Design Heating Duty",
        "^(fcu\\s*)?design\\s*heating\\s*duty$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 20},
    [HVAC_POINT_HEATING_OUTPUT] = {
        "heat_out", "Current heating output", "health", "Live heating delivered (kW).",
        "Overall Current Heating Output",
        "^(overall\\s*)?current\\s*heating\\s*output$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 21},
    [HVAC_POINT_REQUIRED_HEATING_OUTPUT] = {
        "heat_req", "Required heating output", "health", "Heating the controller is asking for (kW).",
        "Overall Required Heating Output",
        "^(overall\\s*)?required\\s*heating\\s*output$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 22},
    [HVAC_POINT_HEATING_FLOW] = {
        "heat_flow", "Heating valve flow", "health", "Measured heating-water flow (L/s).",
        "Heating Valve Current Flow Rate",
        "^heating\\s*valve\\s*current\\s*flow(\\s*rate)?$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 32},
    [HVAC_POINT_REQUIRED_HEATING_FLOW] = {
        "heat_flow_req", "Heating valve required flow", "health", "Heating-water flow the controller wants (L/s).",
        "Heating Valve Required Flow Rate",
        "^heating\\s*valve\\s*required\\s*flow(\\s*rate)?$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 33},
    [HVAC_POINT_HEATING_FLOW_DESIGN_PCT] = {
        "heat_flow_pct", "Heating flow % of design", "health", "Controller-computed flow as a percentage of design flow.",
        "Heating Valve Percentage of Design Flow",
        "^heating\\s*valve\\s*(percent(age)?|%|pct)\\s*(of\\s*)?design\\s*flow$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 35},
    [HVAC_POINT_HEATING_VALVE_SIGNAL] = {
        "heat_valve", "Heating valve signal", "health", "Heating valve / element drive signal (%).",
        "Heating Valve / Element 1 Control Signal",
        "^heating\\s*valve(\\s*\\/\\s*element(\\s*\\d+)?)?\\s*(control\\s*)?signal$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_OUTPUT, 7},
    [HVAC_POINT_RETURN_AIR] = {
        "return_air", "Return air temperature", "health", "Air temperature coming back into the unit.",
        "Return Air Temperature Sensor",
        "^return\\s*air\\s*temp(erature)?(\\s*sensor)?$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_INPUT, 9},
    [HVAC_POINT_FAN_COUNT] = {
        "fan_count", "Number of fans", "health",
        "How many fan channels are fitted; filters out unfitted fan sensors that still read plausibly.",
        "Number of FCU Fans",
        "^(number\\s*of\\s*(fcu\\s*)?fans|(fcu\\s*)?fan\\s*count)$",
        HVAC_POINT_KIND_REAL, false, OBJECT_ANALOG_VALUE, 50},
    [HVAC_POINT_COOLING_VALVE_STATUS] = {
        "cool_vlv_state", "Cooling valve status", "health", "1=Closed, 2=Controlling, 3=Resyncing.",
        "Cooling Valve Status",
        "^cooling\\s*valve\\s*status$",
        HVAC_POINT_KIND_MULTISTATE, false, OBJECT_MULTI_STATE_VALUE, 4},
    [HVAC_POINT_HEATING_VALVE_STATUS] = {
        "heat_vlv_state", "Heating valve status", "health", "1=Closed, 2=Controlling, 3=Resyncing.",
        "Heating Valve Status",
        "^heating\\s*valve\\s*status$",
        HVAC_POINT_KIND_MULTISTATE, false, OBJECT_MULTI_STATE_VALUE, 3},
};

/* 21 x (4 + 1) bytes. Read on every status/health/MQTT/Matter poll but only
 * written from the (rare) mapping flow; PSRAM like the other
 * commissioning-owned state so internal .bss keeps its headroom. Not
 * statically initialised (EXT_RAM_BSS is zero-filled at boot, not loaded),
 * so hvac_core_points_load() must run before any point is used. */
static EXT_RAM_BSS_ATTR uint32_t PointObjIds[HVAC_POINT_COUNT];
static EXT_RAM_BSS_ATTR uint8_t PointSources[HVAC_POINT_COUNT];

static inline uint32_t point_objid(uint16_t type, uint32_t instance)
{
    return ((uint32_t)type << 22) | (instance & 0x3FFFFFu);
}

const hvac_point_def_t *hvac_core_point_def(hvac_point_id_t id)
{
    return ((unsigned)id < HVAC_POINT_COUNT) ? &PointDefs[id] : NULL;
}

bool hvac_core_point_get(hvac_point_id_t id, uint16_t *out_type, uint32_t *out_instance)
{
    if ((unsigned)id >= HVAC_POINT_COUNT) return false;
    uint32_t objid = PointObjIds[id];
    if (PointSources[id] == HVAC_POINT_SOURCE_UNMAPPED || objid == POINT_OBJID_UNMAPPED) return false;
    if (out_type) *out_type = (uint16_t)(objid >> 22);
    if (out_instance) *out_instance = objid & 0x3FFFFFu;
    return true;
}

hvac_point_source_t hvac_core_point_source(hvac_point_id_t id)
{
    return ((unsigned)id < HVAC_POINT_COUNT) ? (hvac_point_source_t)PointSources[id] : HVAC_POINT_SOURCE_UNMAPPED;
}

bool hvac_core_point_type_ok(hvac_point_id_t id, uint16_t type)
{
    if ((unsigned)id >= HVAC_POINT_COUNT) return false;
    const hvac_point_def_t *def = &PointDefs[id];
    switch (def->kind) {
    case HVAC_POINT_KIND_REAL:
        return type == OBJECT_ANALOG_INPUT || type == OBJECT_ANALOG_OUTPUT || type == OBJECT_ANALOG_VALUE;
    case HVAC_POINT_KIND_BOOL:
        if (type == OBJECT_BINARY_VALUE || type == OBJECT_BINARY_OUTPUT) return true;
        return !def->writable && type == OBJECT_BINARY_INPUT;
    case HVAC_POINT_KIND_MULTISTATE:
        if (type == OBJECT_MULTI_STATE_VALUE || type == OBJECT_MULTI_STATE_OUTPUT) return true;
        return !def->writable && type == OBJECT_MULTI_STATE_INPUT;
    }
    return false;
}

bool hvac_core_point_set(hvac_point_id_t id, bool mapped, uint16_t type, uint32_t instance)
{
    if ((unsigned)id >= HVAC_POINT_COUNT) return false;
    if (!mapped) {
        PointObjIds[id] = POINT_OBJID_UNMAPPED;
        PointSources[id] = HVAC_POINT_SOURCE_UNMAPPED;
        return true;
    }
    if (instance > POINT_MAX_INSTANCE || !hvac_core_point_type_ok(id, type)) return false;
    PointObjIds[id] = point_objid(type, instance);
    PointSources[id] = HVAC_POINT_SOURCE_CONFIRMED;
    return true;
}

void hvac_core_points_reset(void)
{
    for (size_t i = 0; i < HVAC_POINT_COUNT; i++) {
        PointObjIds[i] = point_objid(PointDefs[i].default_type, PointDefs[i].default_instance);
        PointSources[i] = HVAC_POINT_SOURCE_DEFAULT;
    }
}

void hvac_core_points_load(void)
{
    hvac_core_points_reset();
    nvs_handle_t handle;
    if (nvs_open(NVS_POINTS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    /* Per-point keys (like rooms): a point added by a later firmware simply
     * has no key yet and keeps its reference default until re-confirmed. */
    for (size_t i = 0; i < HVAC_POINT_COUNT; i++) {
        uint32_t objid = 0;
        if (nvs_get_u32(handle, PointDefs[i].key, &objid) != ESP_OK) continue;
        if (objid == POINT_OBJID_UNMAPPED) {
            hvac_core_point_set((hvac_point_id_t)i, false, 0, 0);
        } else if (!hvac_core_point_set((hvac_point_id_t)i, true, (uint16_t)(objid >> 22), objid & 0x3FFFFFu)) {
            /* Stored value no longer valid for this point - keep the default. */
            PointObjIds[i] = point_objid(PointDefs[i].default_type, PointDefs[i].default_instance);
            PointSources[i] = HVAC_POINT_SOURCE_DEFAULT;
        }
    }
    nvs_close(handle);
}

bool hvac_core_points_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_POINTS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < HVAC_POINT_COUNT && err == ESP_OK; i++) {
        uint32_t objid = PointObjIds[i];
        uint8_t source = PointSources[i];
        if (source == HVAC_POINT_SOURCE_DEFAULT) {
            err = nvs_erase_key(handle, PointDefs[i].key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        } else {
            err = nvs_set_u32(handle, PointDefs[i].key, objid);
        }
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void hvac_core_points_erase(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_POINTS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    hvac_core_points_reset();
}

bool hvac_core_point_read_real(hvac_point_id_t id, float *out_val)
{
    uint16_t type;
    uint32_t instance;
    if (!out_val || !hvac_core_point_get(id, &type, &instance)) return false;
    return bacnet_worker_read_real((BACNET_OBJECT_TYPE)type, instance, PROP_PRESENT_VALUE, out_val);
}

bool hvac_core_point_read_bool(hvac_point_id_t id, bool *out_val)
{
    uint16_t type;
    uint32_t instance;
    if (!out_val || !hvac_core_point_get(id, &type, &instance)) return false;
    return bacnet_worker_read_bool((BACNET_OBJECT_TYPE)type, instance, PROP_PRESENT_VALUE, out_val);
}

bool hvac_core_point_read_multistate(hvac_point_id_t id, unsigned *out_val)
{
    uint16_t type;
    uint32_t instance;
    if (!out_val || !hvac_core_point_get(id, &type, &instance)) return false;
    return bacnet_worker_read_msv((BACNET_OBJECT_TYPE)type, instance, PROP_PRESENT_VALUE, out_val);
}

bool hvac_core_point_write_bool(hvac_point_id_t id, bool val)
{
    uint16_t type;
    uint32_t instance;
    if ((unsigned)id >= HVAC_POINT_COUNT || !PointDefs[id].writable ||
        !hvac_core_point_get(id, &type, &instance)) return false;
    return bacnet_worker_write_bool((BACNET_OBJECT_TYPE)type, instance, PROP_PRESENT_VALUE, val);
}

bool hvac_core_point_write_multistate(hvac_point_id_t id, unsigned val)
{
    uint16_t type;
    uint32_t instance;
    if ((unsigned)id >= HVAC_POINT_COUNT || !PointDefs[id].writable ||
        !hvac_core_point_get(id, &type, &instance)) return false;
    return bacnet_worker_write_msv((BACNET_OBJECT_TYPE)type, instance, PROP_PRESENT_VALUE, val);
}

void hvac_core_rooms_load(void)
{
    memcpy(HvacRooms, DefaultRooms, sizeof(DefaultRooms));
    HvacRoomCount = 5;
    nvs_handle_t handle;
    if (nvs_open(NVS_ROOMS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;

    uint8_t count = 0;
    if (nvs_get_u8(handle, "count", &count) == ESP_OK && count > 0 && count <= HVAC_CORE_MAX_ROOMS) {
        HvacRoomCount = count;
        for (size_t i = 0; i < HvacRoomCount; i++) {
            char key[32];
            uint32_t value = 0;
            snprintf(key, sizeof(key), "r%u_name", (unsigned)i);
            size_t name_len = sizeof(HvacRooms[i].name);
            nvs_get_str(handle, key, HvacRooms[i].name, &name_len);
            snprintf(key, sizeof(key), "r%u_act", (unsigned)i);
            uint8_t active = 0;
            if (nvs_get_u8(handle, key, &active) == ESP_OK) HvacRooms[i].active = active != 0;
            snprintf(key, sizeof(key), "r%u_sp", (unsigned)i); if (nvs_get_u32(handle, key, &value) == ESP_OK && value) HvacRooms[i].setpoint_instance = value;
            snprintf(key, sizeof(key), "r%u_temp", (unsigned)i); if (nvs_get_u32(handle, key, &value) == ESP_OK && value) HvacRooms[i].temperature_instance = value;
            snprintf(key, sizeof(key), "r%u_pwr", (unsigned)i); if (nvs_get_u32(handle, key, &value) == ESP_OK && value) HvacRooms[i].power_instance = value;
            snprintf(key, sizeof(key), "r%u_sa", (unsigned)i); if (nvs_get_u32(handle, key, &value) == ESP_OK && value) HvacRooms[i].supply_air_instance = value;
            snprintf(key, sizeof(key), "r%u_req", (unsigned)i); if (nvs_get_u32(handle, key, &value) == ESP_OK && value) HvacRooms[i].required_output_instance = value;
            snprintf(key, sizeof(key), "r%u_cur", (unsigned)i); if (nvs_get_u32(handle, key, &value) == ESP_OK && value) HvacRooms[i].current_output_instance = value;
        }
    }
    nvs_close(handle);

    for (size_t i = 0; i < HvacRoomCount; i++) {
        const hvac_room_config_t *fallback = &DefaultRooms[i < 5 ? i : 0];
        if (!HvacRooms[i].setpoint_instance) HvacRooms[i].setpoint_instance = fallback->setpoint_instance;
        if (!HvacRooms[i].temperature_instance) HvacRooms[i].temperature_instance = fallback->temperature_instance;
        if (!HvacRooms[i].power_instance) HvacRooms[i].power_instance = fallback->power_instance;
        if (!HvacRooms[i].supply_air_instance) HvacRooms[i].supply_air_instance = fallback->supply_air_instance;
        if (!HvacRooms[i].required_output_instance) HvacRooms[i].required_output_instance = fallback->required_output_instance;
        if (!HvacRooms[i].current_output_instance) HvacRooms[i].current_output_instance = fallback->current_output_instance;
        if (!HvacRooms[i].name[0]) strlcpy(HvacRooms[i].name, fallback->name, sizeof(HvacRooms[i].name));
    }
}

void hvac_core_rooms_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_ROOMS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "count", (uint8_t)HvacRoomCount);
    for (size_t i = 0; i < HvacRoomCount; i++) {
        const hvac_room_config_t *room = &HvacRooms[i];
        char key[32];
        snprintf(key, sizeof(key), "r%u_name", (unsigned)i); nvs_set_str(handle, key, room->name);
        snprintf(key, sizeof(key), "r%u_act", (unsigned)i); nvs_set_u8(handle, key, room->active ? 1 : 0);
        snprintf(key, sizeof(key), "r%u_sp", (unsigned)i); nvs_set_u32(handle, key, room->setpoint_instance);
        snprintf(key, sizeof(key), "r%u_temp", (unsigned)i); nvs_set_u32(handle, key, room->temperature_instance);
        snprintf(key, sizeof(key), "r%u_pwr", (unsigned)i); nvs_set_u32(handle, key, room->power_instance);
        snprintf(key, sizeof(key), "r%u_sa", (unsigned)i); nvs_set_u32(handle, key, room->supply_air_instance);
        snprintf(key, sizeof(key), "r%u_req", (unsigned)i); nvs_set_u32(handle, key, room->required_output_instance);
        snprintf(key, sizeof(key), "r%u_cur", (unsigned)i); nvs_set_u32(handle, key, room->current_output_instance);
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void hvac_core_integration_load(void)
{
    /* Existing installations predate this key and must retain MQTT behaviour. */
    IntegrationKind = HVAC_INTEGRATION_MQTT_HOME_ASSISTANT;
    MatterPairingStatus = HVAC_PAIRING_IDLE;
    nvs_handle_t handle;
    if (nvs_open(NVS_INTEGRATION_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t stored = HVAC_INTEGRATION_MQTT_HOME_ASSISTANT;
    if (nvs_get_u8(handle, NVS_KEY_INTEGRATION_MODE, &stored) == ESP_OK &&
        stored <= HVAC_INTEGRATION_MATTER) {
        IntegrationKind = (hvac_integration_kind_t)stored;
    }
    uint8_t pairing = HVAC_PAIRING_IDLE;
    if (nvs_get_u8(handle, NVS_KEY_MATTER_PAIRING, &pairing) == ESP_OK &&
        pairing <= HVAC_PAIRING_TIMED_OUT) {
        MatterPairingStatus = (hvac_pairing_status_t)pairing;
    }
    nvs_close(handle);
}

hvac_pairing_status_t hvac_core_matter_pairing_get(void)
{
    return MatterPairingStatus;
}

bool hvac_core_matter_pairing_set(hvac_pairing_status_t status)
{
    if (status > HVAC_PAIRING_TIMED_OUT) return false;
    nvs_handle_t handle;
    if (nvs_open(NVS_INTEGRATION_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, NVS_KEY_MATTER_PAIRING, (uint8_t)status);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    MatterPairingStatus = status;
    return true;
}

hvac_integration_kind_t hvac_core_integration_get(void)
{
    return IntegrationKind;
}

bool hvac_core_integration_set(hvac_integration_kind_t integration)
{
    if (integration > HVAC_INTEGRATION_MATTER) return false;
    bool switching_to_matter = (integration == HVAC_INTEGRATION_MATTER && IntegrationKind != HVAC_INTEGRATION_MATTER);
    nvs_handle_t handle;
    if (nvs_open(NVS_INTEGRATION_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, NVS_KEY_INTEGRATION_MODE, (uint8_t)integration);
    if (err == ESP_OK && switching_to_matter) {
        /* Fresh selection of Matter (not just re-confirming it while already
         * active) starts a clean pairing attempt, not a leftover timed-out
         * status from whenever it was last tried. */
        err = nvs_set_u8(handle, NVS_KEY_MATTER_PAIRING, (uint8_t)HVAC_PAIRING_IDLE);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    IntegrationKind = integration;
    if (switching_to_matter) {
        MatterPairingStatus = HVAC_PAIRING_IDLE;
    }
    return true;
}

void hvac_core_integration_reset(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_INTEGRATION_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    IntegrationKind = HVAC_INTEGRATION_MQTT_HOME_ASSISTANT;
    MatterPairingStatus = HVAC_PAIRING_IDLE;
}

/* ========================================================================= */
/* Semantic HVAC Domain Operations                                           */
/* ========================================================================= */

hvac_room_config_t *hvac_core_get_room(size_t room_idx)
{
    if (room_idx >= HvacRoomCount) return NULL;
    return &HvacRooms[room_idx];
}

size_t hvac_core_get_room_count(void)
{
    return HvacRoomCount;
}

bool hvac_core_set_room_setpoint(size_t room_idx, float requested, float *applied)
{
    if (room_idx >= HvacRoomCount) return false;
    float value = requested < MIN_SETPOINT_C ? MIN_SETPOINT_C : (requested > MAX_SETPOINT_C ? MAX_SETPOINT_C : requested);
    if (applied) *applied = value;
    return bacnet_worker_write_real(OBJECT_ANALOG_VALUE, HvacRooms[room_idx].setpoint_instance, PROP_PRESENT_VALUE, value);
}

bool hvac_core_set_room_power(size_t room_idx, bool on)
{
    if (room_idx >= HvacRoomCount) return false;
    return bacnet_worker_write_bool(OBJECT_BINARY_VALUE, HvacRooms[room_idx].power_instance, PROP_PRESENT_VALUE, on);
}

bool hvac_core_set_system_power(bool on)
{
    /* Room power is deliberately untouched here: system power and room power
     * are functionally separate so the same room configuration survives a
     * system-off/on cycle without the user re-setting anything. */
    return hvac_core_point_write_bool(HVAC_POINT_SYS_POWER_WRITE, on);
}

bool hvac_core_get_room_setpoint(size_t room_idx, float *out_val)
{
    if (room_idx >= HvacRoomCount || !out_val) return false;
    return bacnet_worker_read_real(OBJECT_ANALOG_VALUE, HvacRooms[room_idx].setpoint_instance, PROP_PRESENT_VALUE, out_val);
}

bool hvac_core_get_room_temp(size_t room_idx, float *out_val)
{
    if (room_idx >= HvacRoomCount || !out_val) return false;
    return bacnet_worker_read_real(OBJECT_ANALOG_VALUE, HvacRooms[room_idx].temperature_instance, PROP_PRESENT_VALUE, out_val);
}

bool hvac_core_get_room_power(size_t room_idx, bool *out_val)
{
    if (room_idx >= HvacRoomCount || !out_val) return false;
    return bacnet_worker_read_bool(OBJECT_BINARY_VALUE, HvacRooms[room_idx].power_instance, PROP_PRESENT_VALUE, out_val);
}

bool hvac_core_get_system_power(bool *out_val)
{
    if (!out_val) return false;
    return hvac_core_point_read_bool(HVAC_POINT_SYS_POWER_READBACK, out_val);
}

bool hvac_core_get_system_power_commanded(bool *out_val)
{
    /* The command point is the one we write - reading it back reports exactly
     * what was last commanded, independent of whether anything is actually
     * running (the readback point) and independent of per-room state. */
    return hvac_core_point_read_bool(HVAC_POINT_SYS_POWER_WRITE, out_val);
}
