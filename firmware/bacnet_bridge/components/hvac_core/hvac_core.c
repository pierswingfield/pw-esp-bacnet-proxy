#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "hvac_core.h"
#include "bacnet_worker.h"

#define NVS_ROOMS_NAMESPACE "nvs_rooms"
#define NVS_INTEGRATION_NAMESPACE "nvs_integ"
#define NVS_KEY_INTEGRATION_MODE "mode"
#define NVS_KEY_MATTER_PAIRING "matter_pair"

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
    return bacnet_worker_write_bool(OBJECT_BINARY_VALUE, SYS_POWER_WRITE_INSTANCE, PROP_PRESENT_VALUE, on);
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
    return bacnet_worker_read_bool(OBJECT_BINARY_VALUE, SYS_POWER_READBACK_INSTANCE, PROP_PRESENT_VALUE, out_val);
}

bool hvac_core_get_system_power_commanded(bool *out_val)
{
    /* BV:13 is the point we write - reading it back reports exactly what was
     * last commanded, independent of whether anything is actually running
     * (BV:1, "Running now") and independent of per-room state. */
    if (!out_val) return false;
    return bacnet_worker_read_bool(OBJECT_BINARY_VALUE, SYS_POWER_WRITE_INSTANCE, PROP_PRESENT_VALUE, out_val);
}
