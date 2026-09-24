#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"

#include "hvac_core.h"
#include "matter_adapter.h"

static const char *TAG = "MATTER_ADAPTER";

static bool s_matter_running = false;

esp_err_t matter_adapter_init(void)
{
#if defined(CONFIG_ENABLE_ESP_MATTER) && CONFIG_ENABLE_ESP_MATTER
    ESP_LOGI(TAG, "Initializing Matter Thermostat Adapter (Cluster 0x%04X)...", MATTER_CLUSTER_THERMOSTAT);
    s_matter_running = false;
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Matter integration is not supported on this hardware profile (W5500 4MB) - remaining Web-only");
    s_matter_running = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void matter_adapter_set_system_power_handlers(matter_system_power_write_cb_t write_cb,
                                              matter_system_power_read_cb_t read_cb)
{
    (void)write_cb;
    (void)read_cb;
}

void matter_adapter_set_boost_handlers(matter_boost_write_cb_t write_cb,
                                       matter_boost_read_cb_t read_cb)
{
    (void)write_cb;
    (void)read_cb;
}

esp_err_t matter_adapter_start(void)
{
#if defined(CONFIG_ENABLE_ESP_MATTER) && CONFIG_ENABLE_ESP_MATTER
    if (s_matter_running) {
        ESP_LOGI(TAG, "Matter adapter is already active");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Starting Matter Thermostat Endpoint 1 (Room A, Cluster 0x%04X)", MATTER_CLUSTER_THERMOSTAT);
    s_matter_running = true;
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Matter integration disabled in build configuration - start request ignored");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t matter_adapter_stop(void)
{
#if defined(CONFIG_ENABLE_ESP_MATTER) && CONFIG_ENABLE_ESP_MATTER
    if (!s_matter_running) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Stopping Matter adapter and releasing resources");
    s_matter_running = false;
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

bool matter_adapter_is_running(void)
{
    return s_matter_running;
}

bool matter_adapter_retry_pairing(void)
{
    return false;
}

bool matter_adapter_close_pairing(void)
{
    return false;
}

bool matter_adapter_get_onboarding_info(matter_onboarding_info_t *out)
{
    if (out) {
        *out = (matter_onboarding_info_t){0};
        /* Mirror matter_adapter_is_running() - this stub build has no real
         * CHIP stack, so window/paired/QR fields stay zeroed, but `running`
         * must still agree with /api/integration's matter_active, or the
         * Smart Home page (which reads this endpoint) disagrees with the
         * status the rest of the UI shows. */
        out->running = s_matter_running;
    }
    return true;
}

/* ========================================================================= */
/* Matter Thermostat Attribute Accessors (Cluster 0x0201)                   */
/* ========================================================================= */

bool matter_thermostat_read_local_temperature(size_t room_idx, int16_t *out_val_hundredths)
{
    if (!out_val_hundredths) return false;
    float temp_c = 0.0f;
    if (!hvac_core_get_room_temp(room_idx, &temp_c)) {
        ESP_LOGD(TAG, "Failed to read temperature from HVAC core for room %u", (unsigned)room_idx);
        return false;
    }
    *out_val_hundredths = (int16_t)roundf(temp_c * 100.0f);
    return true;
}

bool matter_thermostat_read_cooling_setpoint(size_t room_idx, int16_t *out_val_hundredths)
{
    if (!out_val_hundredths) return false;
    float sp_c = 0.0f;
    if (!hvac_core_get_room_setpoint(room_idx, &sp_c)) {
        ESP_LOGD(TAG, "Failed to read setpoint from HVAC core for room %u", (unsigned)room_idx);
        return false;
    }
    *out_val_hundredths = (int16_t)roundf(sp_c * 100.0f);
    return true;
}

bool matter_thermostat_write_cooling_setpoint(size_t room_idx, int16_t raw_val_hundredths, int16_t *out_applied_hundredths)
{
    float requested_c = (float)raw_val_hundredths / 100.0f;
    float applied_c = 0.0f;
    if (!hvac_core_set_room_setpoint(room_idx, requested_c, &applied_c)) {
        ESP_LOGE(TAG, "Failed to write setpoint %.2f°C to HVAC core for room %u", requested_c, (unsigned)room_idx);
        return false;
    }
    if (out_applied_hundredths) {
        *out_applied_hundredths = (int16_t)roundf(applied_c * 100.0f);
    }
    return true;
}

bool matter_thermostat_read_heating_setpoint(size_t room_idx, int16_t *out_val_hundredths)
{
    return matter_thermostat_read_cooling_setpoint(room_idx, out_val_hundredths);
}

bool matter_thermostat_write_heating_setpoint(size_t room_idx, int16_t raw_val_hundredths, int16_t *out_applied_hundredths)
{
    return matter_thermostat_write_cooling_setpoint(room_idx, raw_val_hundredths, out_applied_hundredths);
}

bool matter_thermostat_read_system_mode(size_t room_idx, matter_system_mode_t *out_mode)
{
    if (!out_mode) return false;
    bool is_on = false;
    if (!hvac_core_get_room_power(room_idx, &is_on)) {
        *out_mode = MATTER_SYSTEM_MODE_OFF;
        return false;
    }
    *out_mode = is_on ? MATTER_SYSTEM_MODE_HEAT : MATTER_SYSTEM_MODE_OFF;
    return true;
}

bool matter_thermostat_write_system_mode(size_t room_idx, matter_system_mode_t mode)
{
    if (mode == MATTER_SYSTEM_MODE_OFF) {
        return hvac_core_set_room_power(room_idx, false);
    } else if (mode == MATTER_SYSTEM_MODE_AUTO || mode == MATTER_SYSTEM_MODE_COOL || mode == MATTER_SYSTEM_MODE_HEAT) {
        return hvac_core_set_room_power(room_idx, true);
    }
    ESP_LOGW(TAG, "Unsupported Matter system mode: %d", (int)mode);
    return false;
}
