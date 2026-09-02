#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#define PHY_ADDRESS 0
#define PHY_POWER_GPIO GPIO_NUM_12
#define PHY_MDC_GPIO GPIO_NUM_23
#define PHY_MDIO_GPIO GPIO_NUM_18
#define PHY_RESET_GPIO (-1)

#define PROOF_STATIC_IP "10.0.3.99"
#define PROOF_NETMASK "255.255.0.0"
#define PROOF_GATEWAY "0.0.0.0"
#define PROOF_TARGET_IP "10.0.3.16"

#define ETH_LINK_UP_BIT BIT0

static const char *TAG = "TETH_PROOF";
static EventGroupHandle_t s_events;
static esp_eth_handle_t s_eth_handle;
static esp_netif_t *s_eth_netif;
static volatile bool s_eth_link_up;
static volatile int s_ping_success;
static volatile int s_ping_timeout;
static volatile bool s_ping_done;
static int s_best_wifi_rssi = -127;
static char s_best_wifi_ssid[33];
static bool s_chip_memory_ok;
static bool s_psram_ok;
static esp_reset_reason_t s_reset_reason;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT";
    case ESP_RST_SW:
        return "SOFTWARE";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    default:
        return "OTHER";
    }
}

static void print_memory_status(const char *phase)
{
    ESP_LOGI(
        TAG,
        "%s: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
        phase,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static bool test_psram(void)
{
    const size_t test_size = 1024 * 1024;
    uint8_t *buffer = heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "PSRAM test allocation of %u bytes failed", (unsigned)test_size);
        return false;
    }

    for (size_t i = 0; i < test_size; i++) {
        buffer[i] = (uint8_t)((i * 37U) ^ (i >> 8));
    }
    for (size_t i = 0; i < test_size; i++) {
        uint8_t expected = (uint8_t)((i * 37U) ^ (i >> 8));
        if (buffer[i] != expected) {
            ESP_LOGE(
                TAG,
                "PSRAM pattern mismatch at byte %u: got 0x%02x expected 0x%02x",
                (unsigned)i,
                buffer[i],
                expected);
            free(buffer);
            return false;
        }
    }

    free(buffer);
    ESP_LOGI(TAG, "PSRAM 1 MiB pattern test: PASS");
    return true;
}

static bool print_chip_status(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);

    esp_err_t flash_err = esp_flash_get_size(NULL, &flash_size);
    size_t psram_size = esp_psram_get_size();

    ESP_LOGI(
        TAG,
        "chip: model=%d revision=%d cores=%d features=0x%lx",
        chip_info.model,
        chip_info.revision,
        chip_info.cores,
        (unsigned long)chip_info.features);
    if (flash_err == ESP_OK) {
        ESP_LOGI(TAG, "flash: %u bytes (%u MiB)", (unsigned)flash_size, (unsigned)(flash_size / (1024 * 1024)));
    } else {
        ESP_LOGE(TAG, "flash size read failed: %s", esp_err_to_name(flash_err));
    }
    ESP_LOGI(
        TAG,
        "PSRAM: physical=%u bytes (%u MiB), heap-mapped=%u bytes",
        (unsigned)psram_size,
        (unsigned)(psram_size / (1024 * 1024)),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (flash_err == ESP_OK && flash_size != 16U * 1024U * 1024U) {
        ESP_LOGW(TAG, "EXPECTED 16 MiB FLASH; inspect the exact board variant");
    }
    if (psram_size != 8U * 1024U * 1024U) {
        ESP_LOGW(TAG, "EXPECTED 8 MiB PHYSICAL PSRAM; inspect module marking and sdkconfig");
    }

    return flash_err == ESP_OK && flash_size == 16U * 1024U * 1024U &&
           psram_size == 8U * 1024U * 1024U;
}

static void record_proof_result(bool passed)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("proof_gate", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open proof counter NVS: %s", esp_err_to_name(err));
        return;
    }

    const char *key = passed ? "pass_count" : "fail_count";
    uint32_t selected_count = 0;
    err = nvs_get_u32(handle, key, &selected_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        selected_count = 0;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read proof counter: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    ESP_ERROR_CHECK(nvs_set_u32(handle, key, selected_count + 1));
    ESP_ERROR_CHECK(nvs_commit(handle));

    uint32_t pass_count = 0;
    uint32_t fail_count = 0;
    (void)nvs_get_u32(handle, "pass_count", &pass_count);
    (void)nvs_get_u32(handle, "fail_count", &fail_count);
    nvs_close(handle);

    ESP_LOGI(
        TAG,
        "PROOF_RESULT=%s reset=%s persistent_passes=%lu persistent_failures=%lu",
        passed ? "PASS" : "FAIL",
        reset_reason_name(s_reset_reason),
        (unsigned long)pass_count,
        (unsigned long)fail_count);
}

static void eth_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;

    esp_eth_handle_t handle = *(esp_eth_handle_t *)event_data;
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = {0};
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        s_eth_link_up = true;
        xEventGroupSetBits(s_events, ETH_LINK_UP_BIT);
        ESP_LOGI(
            TAG,
            "Ethernet link UP, MAC %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        s_eth_link_up = false;
        xEventGroupClearBits(s_events, ETH_LINK_UP_BIT);
        ESP_LOGW(TAG, "Ethernet link DOWN");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet driver STARTED");
        break;
    case ETHERNET_EVENT_STOP:
        s_eth_link_up = false;
        xEventGroupClearBits(s_events, ETH_LINK_UP_BIT);
        ESP_LOGW(TAG, "Ethernet driver STOPPED");
        break;
    default:
        break;
    }
}

static esp_err_t configure_static_ethernet_ip(void)
{
    esp_err_t err = esp_netif_dhcpc_stop(s_eth_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (!ip4addr_aton(PROOF_STATIC_IP, (ip4_addr_t *)&ip_info.ip) ||
        !ip4addr_aton(PROOF_NETMASK, (ip4_addr_t *)&ip_info.netmask) ||
        !ip4addr_aton(PROOF_GATEWAY, (ip4_addr_t *)&ip_info.gw)) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_netif_set_ip_info(s_eth_netif, &ip_info);
}

static esp_err_t start_ethernet(void)
{
    gpio_config_t power_gpio = {
        .pin_bit_mask = 1ULL << PHY_POWER_GPIO,
        // Input is enabled as well so gpio_get_level() reports the actual pad
        // state instead of the disabled input path's zero value.
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power_gpio), TAG, "PHY power GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PHY_POWER_GPIO, 1), TAG, "PHY power enable failed");
    ESP_LOGI(TAG, "PHY rail enabled: GPIO12=HIGH");
    vTaskDelay(pdMS_TO_TICKS(100));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    phy_config.phy_addr = PHY_ADDRESS;
    phy_config.reset_gpio_num = PHY_RESET_GPIO;
    emac_config.smi_gpio.mdc_num = PHY_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = PHY_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_rtl8201(&phy_config);
    if (mac == NULL || phy == NULL) {
        if (mac != NULL) {
            mac->del(mac);
        }
        if (phy != NULL) {
            phy->del(phy);
        }
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        return err;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    if (s_eth_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    if (glue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, glue), TAG, "Ethernet netif attach failed");
    ESP_RETURN_ON_ERROR(configure_static_ethernet_ip(), TAG, "Static Ethernet IP configuration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL),
        TAG,
        "Ethernet event registration failed");

    ESP_LOGI(TAG, "Ethernet configured as %s/%s, target=%s", PROOF_STATIC_IP, PROOF_NETMASK, PROOF_TARGET_IP);
    return esp_eth_start(s_eth_handle);
}

static void ping_success_cb(esp_ping_handle_t handle, void *args)
{
    (void)handle;
    (void)args;
    s_ping_success++;
}

static void ping_timeout_cb(esp_ping_handle_t handle, void *args)
{
    (void)handle;
    (void)args;
    s_ping_timeout++;
}

static void ping_end_cb(esp_ping_handle_t handle, void *args)
{
    (void)args;
    s_ping_done = true;
    esp_ping_delete_session(handle);
}

static void run_target_ping(void)
{
    ip_addr_t target = {0};
    if (!ip4addr_aton(PROOF_TARGET_IP, &target.u_addr.ip4)) {
        ESP_LOGE(TAG, "Invalid ping target %s", PROOF_TARGET_IP);
        return;
    }
    target.type = IPADDR_TYPE_V4;

    s_ping_success = 0;
    s_ping_timeout = 0;
    s_ping_done = false;

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = 4;
    config.interval_ms = 500;
    config.timeout_ms = 2000;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
        .cb_args = NULL,
    };
    esp_ping_handle_t ping = NULL;
    esp_err_t err = esp_ping_new_session(&config, &callbacks, &ping);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ping session creation failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_ping_start(ping);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ping start failed: %s", esp_err_to_name(err));
        esp_ping_delete_session(ping);
        return;
    }

    while (!s_ping_done) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Target ping %s: %d success, %d timeout", PROOF_TARGET_IP, s_ping_success, s_ping_timeout);
}

static esp_err_t scan_wifi(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power-save disable failed");

    wifi_scan_config_t scan_config = {
        .show_hidden = true,
    };
    ESP_LOGI(TAG, "Starting Wi-Fi scan while Ethernet remains active");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, true), TAG, "Wi-Fi scan failed");

    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), TAG, "Wi-Fi result count failed");
    uint16_t fetch_count = count > 20 ? 20 : count;
    wifi_ap_record_t records[20] = {0};
    if (fetch_count > 0) {
        ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&fetch_count, records), TAG, "Wi-Fi result fetch failed");
    }

    ESP_LOGI(TAG, "Wi-Fi scan found %u APs; showing strongest %u", count, fetch_count);
    for (uint16_t i = 0; i < fetch_count; i++) {
        ESP_LOGI(TAG, "  AP[%u] ssid='%s' rssi=%d channel=%u", i, records[i].ssid, records[i].rssi, records[i].primary);
        if (records[i].rssi > s_best_wifi_rssi) {
            s_best_wifi_rssi = records[i].rssi;
            snprintf(s_best_wifi_ssid, sizeof(s_best_wifi_ssid), "%s", records[i].ssid);
        }
    }
    return ESP_OK;
}

static void proof_task(void *arg)
{
    (void)arg;
    EventBits_t bits = xEventGroupWaitBits(
        s_events,
        ETH_LINK_UP_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(30000));
    bool link_passed = (bits & ETH_LINK_UP_BIT) != 0;
    if (link_passed) {
        run_target_ping();
    } else {
        ESP_LOGE(
            TAG,
            "Ethernet did not link within 30 seconds; check cable, GPIO0/DTR, and software logs");
    }

    esp_err_t wifi_err = scan_wifi();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Concurrent Wi-Fi proof failed: %s", esp_err_to_name(wifi_err));
    }

    bool proof_passed = s_chip_memory_ok && s_psram_ok && link_passed &&
                        wifi_err == ESP_OK && gpio_get_level(PHY_POWER_GPIO) == 1;
    record_proof_result(proof_passed);

    for (;;) {
        print_memory_status("runtime");
        ESP_LOGI(
            TAG,
            "status: eth_link=%s phy_power_gpio=%d best_wifi='%s' rssi=%d",
            s_eth_link_up ? "UP" : "DOWN",
            gpio_get_level(PHY_POWER_GPIO),
            s_best_wifi_ssid[0] ? s_best_wifi_ssid : "none",
            s_best_wifi_rssi);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "LILYGO T-ETH-Lite ESP32 hardware proof");
    ESP_LOGI(TAG, "Expected: WROVER-E, RTL8201, 16 MB flash, 8 MB PSRAM");
    ESP_LOGI(TAG, "====================================================");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    s_reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s (%d)", reset_reason_name(s_reset_reason), s_reset_reason);
    s_chip_memory_ok = print_chip_status();
    print_memory_status("boot");
    s_psram_ok = test_psram();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "Event group allocation failed");
        abort();
    }

    esp_err_t eth_err = start_ethernet();
    if (eth_err != ESP_OK) {
        ESP_LOGE(TAG, "RTL8201 Ethernet start failed: %s", esp_err_to_name(eth_err));
        ESP_LOGE(TAG, "Check that GPIO0 is not connected to downloader DTR/IO0");
    }

    BaseType_t task_result = xTaskCreate(proof_task, "proof_task", 8192, NULL, 5, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "proof_task creation failed");
        abort();
    }

    ESP_LOGI(TAG, "Initial PSRAM result: %s", s_psram_ok ? "PASS" : "FAIL");
}
