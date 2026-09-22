#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CSA Matter Cluster 0x0201: Thermostat */
#define MATTER_CLUSTER_THERMOSTAT 0x0201

/* Matter Thermostat Standard Attribute IDs */
#define MATTER_ATTR_LOCAL_TEMPERATURE          0x0000
#define MATTER_ATTR_OCCUPIED_COOLING_SETPOINT  0x0011
#define MATTER_ATTR_OCCUPIED_HEATING_SETPOINT  0x0012
#define MATTER_ATTR_SYSTEM_MODE                0x001C

typedef enum {
    MATTER_SYSTEM_MODE_OFF = 0,
    MATTER_SYSTEM_MODE_AUTO = 1,
    MATTER_SYSTEM_MODE_COOL = 3,
    MATTER_SYSTEM_MODE_HEAT = 4,
} matter_system_mode_t;

/**
 * @brief Initializes the Matter adapter subsystem.
 * On hardware profiles without Matter support (e.g. W5500 4MB),
 * this logs a warning and remains safely disabled.
 */
esp_err_t matter_adapter_init(void);

/**
 * @brief Starts the Matter adapter and thermostat endpoint tasks.
 */
esp_err_t matter_adapter_start(void);

/**
 * @brief Stops the Matter adapter and releases dynamic resources.
 */
esp_err_t matter_adapter_stop(void);

/**
 * @brief Returns true if the Matter adapter is currently running.
 */
bool matter_adapter_is_running(void);

/**
 * @brief Re-opens the BLE commissioning window on user request (e.g. from
 * the wizard, after a previous attempt timed out without pairing). No-op
 * if the device is already paired or a window is already open.
 * @return true if a window was (re)opened, false otherwise.
 */
bool matter_adapter_retry_pairing(void);

/* ========================================================================= */
/* Matter Thermostat Endpoint (Cluster 0x0201) Attribute Handlers             */
/* Directly routed to canonical HVAC Core (Single Source of Truth)           */
/* ========================================================================= */

/**
 * @brief Reads Local Temperature (0x0000) for a configured room.
 * @param room_idx Room index (0 for Room A).
 * @param[out] out_val_hundredths Temperature in 0.01 °C units (e.g., 2150 for 21.50 °C).
 * @return true on success, false if BACnet/HVAC read fails.
 */
bool matter_thermostat_read_local_temperature(size_t room_idx, int16_t *out_val_hundredths);

/**
 * @brief Reads Occupied Cooling Setpoint (0x0011) for a configured room.
 * @param room_idx Room index (0 for Room A).
 * @param[out] out_val_hundredths Setpoint in 0.01 °C units (e.g., 2200 for 22.00 °C).
 * @return true on success, false on failure.
 */
bool matter_thermostat_read_cooling_setpoint(size_t room_idx, int16_t *out_val_hundredths);

/**
 * @brief Writes Occupied Cooling Setpoint (0x0011) for a configured room.
 * Value is clamped to [18.0°C, 30.0°C] via HVAC Core.
 * @param room_idx Room index (0 for Room A).
 * @param raw_val_hundredths Requested setpoint in 0.01 °C units (e.g., 2350 for 23.50 °C).
 * @param[out] out_applied_hundredths Optional pointer to store the clamped/applied setpoint.
 * @return true on success, false on failure.
 */
bool matter_thermostat_write_cooling_setpoint(size_t room_idx, int16_t raw_val_hundredths, int16_t *out_applied_hundredths);

/**
 * @brief Reads Occupied Heating Setpoint (0x0012) for a configured room.
 */
bool matter_thermostat_read_heating_setpoint(size_t room_idx, int16_t *out_val_hundredths);

/**
 * @brief Writes Occupied Heating Setpoint (0x0012) for a configured room.
 */
bool matter_thermostat_write_heating_setpoint(size_t room_idx, int16_t raw_val_hundredths, int16_t *out_applied_hundredths);

/**
 * @brief Reads System Mode (0x001C) for a configured room.
 * Mapped to HVAC Core room power (OFF -> 0, ON -> HEAT = 4).
 * @param room_idx Room index (0 for Room A).
 * @param[out] out_mode Matter system mode output.
 * @return true on success, false on failure.
 */
bool matter_thermostat_read_system_mode(size_t room_idx, matter_system_mode_t *out_mode);

/**
 * @brief Writes System Mode (0x001C) for a configured room.
 * Mode OFF (0) turns power off; AUTO (1), COOL (3), HEAT (4) turn power on.
 * @param room_idx Room index (0 for Room A).
 * @param mode Target Matter system mode.
 * @return true on success, false on failure or invalid mode.
 */
bool matter_thermostat_write_system_mode(size_t room_idx, matter_system_mode_t mode);

#ifdef __cplusplus
}
#endif
