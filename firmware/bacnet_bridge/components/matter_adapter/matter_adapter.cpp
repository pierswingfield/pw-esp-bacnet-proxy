/*
 * Real ESP-Matter Thermostat endpoint for the T-ETH-Lite Matter build
 * variant. Compiled only when ESP_MATTER_PATH is set at configure time
 * (see the top-level CMakeLists.txt and components/matter_adapter/CMakeLists.txt).
 * The W5500 and default T-ETH-Lite profiles compile matter_adapter_stub.c
 * instead and never link esp_matter.
 */
#include <atomic>
#include <math.h>

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <esp_coexist.h>
#include <esp_matter.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

#include "hvac_core.h"
#include "matter_adapter.h"

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "matter_adapter";

static node_t *s_node = nullptr;
static uint16_t s_thermo_endpoint_id = 0;
static bool s_matter_running = false;
static TaskHandle_t s_sync_task_handle = nullptr;

/* Room A only, matching the M4 Thermostat PoC scope in PROJECT.md. */
static constexpr size_t kRoomIdx = 0;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t /*arg*/)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        hvac_core_matter_pairing_set(HVAC_PAIRING_PAIRED);
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Matter commissioning window opened");
        hvac_core_matter_pairing_set(HVAC_PAIRING_AWAITING);
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Matter commissioning window closed");
        /* Only a timeout if we were still waiting; a window can also close
         * right after kCommissioningComplete (BLE shutting down), which is
         * not a failure and must not overwrite the PAIRED status. */
        if (hvac_core_matter_pairing_get() == HVAC_PAIRING_AWAITING) {
            hvac_core_matter_pairing_set(HVAC_PAIRING_TIMED_OUT);
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Matter: interface IP address changed");
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                        uint8_t effect_id, uint8_t effect_variant, void * /*priv_data*/)
{
    ESP_LOGI(TAG, "Identify callback: endpoint %u, effect %u/%u", endpoint_id, effect_id, effect_variant);
    return ESP_OK;
}

/* Every Matter write to the Thermostat cluster lands here before the local
 * attribute store is updated. Route setpoint/system-mode writes into the
 * canonical HVAC core (single source of truth); do not touch anything else.
 */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                          uint32_t attribute_id, esp_matter_attr_val_t *val, void * /*priv_data*/)
{
    if (type != PRE_UPDATE || cluster_id != Thermostat::Id) {
        return ESP_OK;
    }

    if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        int16_t applied = val->val.i16;
        if (!matter_thermostat_write_cooling_setpoint(kRoomIdx, val->val.i16, &applied)) {
            ESP_LOGW(TAG, "Cooling setpoint write rejected by HVAC core");
            return ESP_ERR_INVALID_STATE;
        }
        val->val.i16 = applied;
    } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
        int16_t applied = val->val.i16;
        if (!matter_thermostat_write_heating_setpoint(kRoomIdx, val->val.i16, &applied)) {
            ESP_LOGW(TAG, "Heating setpoint write rejected by HVAC core");
            return ESP_ERR_INVALID_STATE;
        }
        val->val.i16 = applied;
    } else if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
        if (!matter_thermostat_write_system_mode(kRoomIdx, (matter_system_mode_t)val->val.u8)) {
            ESP_LOGW(TAG, "System mode write rejected by HVAC core");
            return ESP_ERR_INVALID_STATE;
        }
    }

    return ESP_OK;
}

/* Guards against piling up ScheduleLambda work faster than the CHIP event
 * loop can drain it (observed: under BLE commissioning load, an unbounded
 * queue of these jobs compounds and degrades the whole device over time,
 * not just this one push). At most one push is ever in flight.
 */
static std::atomic<bool> s_push_in_flight{false};

/* Pushes the BACnet-backed room state into the Matter attribute store so
 * subscribed controllers see live readings, not just the value from the
 * last commissioning-time default. Matter attribute updates must happen on
 * the Matter (CHIP) thread, hence ScheduleLambda rather than calling
 * attribute::update() directly from this FreeRTOS task.
 */
static void push_room_state_to_matter(void)
{
    uint16_t ep_id = s_thermo_endpoint_id;
    if (ep_id == 0) {
        return;
    }

    bool expected = false;
    if (!s_push_in_flight.compare_exchange_strong(expected, true)) {
        /* Previous push hasn't been processed by the CHIP event loop yet;
         * skip this cycle rather than queue another one on top of it. */
        return;
    }

    int16_t local_temp = 0;
    bool have_temp = matter_thermostat_read_local_temperature(kRoomIdx, &local_temp);

    chip::DeviceLayer::SystemLayer().ScheduleLambda([ep_id, have_temp, local_temp]() {
        if (have_temp) {
            attribute_t *attr = attribute::get(ep_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id);
            if (attr) {
                esp_matter_attr_val_t val = esp_matter_nullable_int16(local_temp);
                attribute::update(ep_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
            }
        }
        s_push_in_flight.store(false);
    });
}

static void matter_sync_task(void * /*arg*/)
{
    while (s_matter_running) {
        push_room_state_to_matter();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    s_sync_task_handle = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t matter_adapter_init(void)
{
    if (s_node != nullptr) {
        return ESP_OK;
    }

    node::config_t node_config;
    s_node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (s_node == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    thermostat::config_t thermo_config;
    thermo_config.thermostat.local_temperature = nullable<int16_t>(2100);
    thermo_config.thermostat.system_mode = (uint8_t)MATTER_SYSTEM_MODE_OFF;
    thermo_config.thermostat.heating.occupied_heating_setpoint = 2000;
    thermo_config.thermostat.cooling.occupied_cooling_setpoint = 2200;

    endpoint_t *ep = thermostat::create(s_node, &thermo_config, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter Thermostat endpoint");
        return ESP_FAIL;
    }
    s_thermo_endpoint_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "Matter Thermostat endpoint created, endpoint_id=%u", s_thermo_endpoint_id);
    return ESP_OK;
}

esp_err_t matter_adapter_start(void)
{
    if (s_node == nullptr) {
        esp_err_t err = matter_adapter_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_matter_running) {
        return ESP_OK;
    }

    hvac_pairing_status_t prior_pairing_status = hvac_core_matter_pairing_get();

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %d", err);
        return err;
    }

    /* BLE commissioning and the HTTP dashboard/BACnet path both need this
     * board's single 2.4GHz radio. During the commissioning window (BLE
     * advertising) plain WiFi coexistence timing can stall the dashboard's
     * TCP connections. Bias toward WiFi so the bridge stays responsive;
     * this only slows/shortens BLE's own range during that same window,
     * and stops mattering once BLE shuts off after pairing completes. */
    esp_err_t coex_err = esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    if (coex_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_coex_preference_set(ESP_COEX_PREFER_WIFI) failed: %d", coex_err);
    }

    /* esp_matter::start() auto-opens a commissioning window whenever the
     * device isn't yet paired with any fabric, unconditionally, every
     * boot. If the LAST attempt already timed out without pairing, an
     * unattended reboot (power blip, crash) would otherwise silently open
     * another BLE-congested window with nobody there to pair it. Shut that
     * auto-opened window right back down; a real retry goes through
     * matter_adapter_retry_pairing() instead, triggered by the wizard.
     */
    if (prior_pairing_status == HVAC_PAIRING_TIMED_OUT) {
        /* CommissioningWindowManager methods assert if called without
         * holding the CHIP stack lock from a non-Matter task context
         * ("Chip stack locking error ... unsafe/racy") - this call happens
         * from app_main's task, not the CHIP event loop, so it must take
         * the lock explicitly. */
        chip::DeviceLayer::PlatformMgr().LockChipStack();
        chip::Server::GetInstance().GetCommissioningWindowManager().CloseCommissioningWindow();
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        hvac_core_matter_pairing_set(HVAC_PAIRING_TIMED_OUT);
        ESP_LOGI(TAG, "Last pairing attempt timed out; not auto-reopening commissioning window on this boot");
    }

    s_matter_running = true;
    if (xTaskCreate(matter_sync_task, "matter_sync", 8192, nullptr, 5, &s_sync_task_handle) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start matter_sync_task; live attribute push disabled");
    }
    ESP_LOGI(TAG, "Matter Thermostat endpoint %u started (Room A)", s_thermo_endpoint_id);
    return ESP_OK;
}

esp_err_t matter_adapter_stop(void)
{
    /* esp_matter/CHIP does not support tearing down a running Matter
     * stack cleanly; once started, the fabric/session state stays live
     * for the life of the process. Stop only the local sync loop and the
     * bookkeeping flag so /api/integration reporting is accurate; a real
     * mode switch away from Matter still requires a reboot.
     */
    s_matter_running = false;
    ESP_LOGW(TAG, "Matter stack cannot be stopped without reboot; sync loop disabled");
    return ESP_OK;
}

bool matter_adapter_retry_pairing(void)
{
    if (!s_matter_running) {
        ESP_LOGW(TAG, "Cannot retry pairing: Matter adapter is not running");
        return false;
    }
    /* Called from the HTTP server task, not the CHIP event loop - must hold
     * the CHIP stack lock, same reason as the boot-time close in
     * matter_adapter_start(). */
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto &commissioning_mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (commissioning_mgr.IsCommissioningWindowOpen()) {
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return true; /* Already open; nothing to do. */
    }
    CHIP_ERROR err = commissioning_mgr.OpenBasicCommissioningWindow();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to reopen commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
        return false;
    }
    return true;
}

bool matter_adapter_is_running(void)
{
    return s_matter_running;
}

/* ========================================================================= */
/* Matter Thermostat Attribute Accessors (Cluster 0x0201)                   */
/* Directly routed to canonical HVAC Core (Single Source of Truth)         */
/* ========================================================================= */

bool matter_thermostat_read_local_temperature(size_t room_idx, int16_t *out_val_hundredths)
{
    if (!out_val_hundredths) return false;
    float temp_c = 0.0f;
    if (!hvac_core_get_room_temp(room_idx, &temp_c)) {
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
