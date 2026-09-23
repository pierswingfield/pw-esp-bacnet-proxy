/*
 * Real ESP-Matter Thermostat endpoint for the T-ETH-Lite Matter build
 * variant. Compiled only when ESP_MATTER_PATH is set at configure time
 * (see the top-level CMakeLists.txt and components/matter_adapter/CMakeLists.txt).
 * The W5500 and default T-ETH-Lite profiles compile matter_adapter_stub.c
 * instead and never link esp_matter.
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <math.h>

#include <esp_attr.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <esp_coexist.h>
#include <esp_matter.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>

#include "hvac_core.h"
#include "matter_adapter.h"

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "matter_adapter";

static node_t *s_node = nullptr;
static uint16_t s_system_endpoint_id = 0;
static uint16_t s_boost_heat_endpoint_id = 0;
static uint16_t s_boost_cool_endpoint_id = 0;
static uint16_t s_room_endpoint_ids[HVAC_CORE_MAX_ROOMS] = {};
static bool s_matter_running = false;
static TaskHandle_t s_sync_task_handle = nullptr;
static StaticTask_t *s_sync_task_tcb = nullptr;
static StackType_t *s_sync_task_stack = nullptr;
static constexpr uint32_t kMatterSyncStackBytes = 8192;
static constexpr uint32_t kMatterSyncStackWords = kMatterSyncStackBytes / sizeof(StackType_t);
static_assert(kMatterSyncStackBytes % sizeof(StackType_t) == 0,
              "Matter sync stack must have an integral FreeRTOS word count");
static matter_system_power_write_cb_t s_system_power_write_cb = nullptr;
static matter_system_power_read_cb_t s_system_power_read_cb = nullptr;

void matter_adapter_set_system_power_handlers(matter_system_power_write_cb_t write_cb,
                                              matter_system_power_read_cb_t read_cb)
{
    s_system_power_write_cb = write_cb;
    s_system_power_read_cb = read_cb;
}

static matter_boost_write_cb_t s_boost_write_cb = nullptr;
static matter_boost_read_cb_t s_boost_read_cb = nullptr;

void matter_adapter_set_boost_handlers(matter_boost_write_cb_t write_cb,
                                       matter_boost_read_cb_t read_cb)
{
    s_boost_write_cb = write_cb;
    s_boost_read_cb = read_cb;
}

static int room_index_for_endpoint(uint16_t endpoint_id)
{
    for (size_t i = 0; i < HVAC_CORE_MAX_ROOMS; ++i) {
        if (s_room_endpoint_ids[i] == endpoint_id) return (int)i;
    }
    return -1;
}

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

/* Every Matter write to the Thermostat cluster (rooms) or the OnOff cluster
 * (the system switch) lands here before the local attribute store is
 * updated. Route setpoint/system-mode/power writes into the canonical
 * HVAC core (single source of truth); do not touch anything else.
 */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                          uint32_t attribute_id, esp_matter_attr_val_t *val, void * /*priv_data*/)
{
    if (type != PRE_UPDATE) {
        return ESP_OK;
    }

    if (endpoint_id == s_system_endpoint_id) {
        if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id || !s_system_power_write_cb ||
            !s_system_power_write_cb(val->val.b)) {
            ESP_LOGW(TAG, "System endpoint write rejected");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    if (endpoint_id == s_boost_heat_endpoint_id || endpoint_id == s_boost_cool_endpoint_id) {
        matter_boost_target_t target = (endpoint_id == s_boost_heat_endpoint_id)
            ? MATTER_BOOST_TARGET_HEAT : MATTER_BOOST_TARGET_COOL;
        if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id || !s_boost_write_cb ||
            !s_boost_write_cb(target, val->val.b)) {
            ESP_LOGW(TAG, "Boost endpoint write rejected");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    if (cluster_id != Thermostat::Id) {
        return ESP_OK;
    }

    int room_idx = room_index_for_endpoint(endpoint_id);
    if (room_idx < 0) {
        ESP_LOGW(TAG, "Write for unknown Matter endpoint %u", endpoint_id);
        return ESP_ERR_NOT_FOUND;
    }

    if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        int16_t applied = val->val.i16;
        if (!matter_thermostat_write_cooling_setpoint((size_t)room_idx, val->val.i16, &applied)) {
            ESP_LOGW(TAG, "Cooling setpoint write rejected by HVAC core");
            return ESP_ERR_INVALID_STATE;
        }
        val->val.i16 = applied;
    } else if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
        if (!matter_thermostat_write_system_mode((size_t)room_idx, (matter_system_mode_t)val->val.u8)) {
            ESP_LOGW(TAG, "System mode write rejected by HVAC core");
            return ESP_ERR_INVALID_STATE;
        }
        /* This controller has automatic heat/cool selection with one
         * setpoint; the room endpoint only advertises the Cool feature (see
         * create_room_thermostat_endpoint), so Cool/Off are the only modes
         * that are actually valid to report back. */
        if (val->val.u8 != MATTER_SYSTEM_MODE_OFF) val->val.u8 = MATTER_SYSTEM_MODE_COOL;
    }

    return ESP_OK;
}

/* Guards against piling up ScheduleLambda work faster than the CHIP event
 * loop can drain it (observed: under BLE commissioning load, an unbounded
 * queue of these jobs compounds and degrades the whole device over time,
 * not just this one push). No new push cycle starts while any job from the
 * previous one is still outstanding.
 */
static std::atomic<int> s_pending_pushes{0};

/* Each of these does exactly one attribute::update() call. Earlier this
 * batched every room's fields into a single ScheduleLambda, which chained
 * up to 9 attribute::update() calls (system mode + 4 fields x N rooms) deep
 * on the CHIP task's own stack and overflowed it in hardware testing - no
 * stack size both fit this board's ~14KB largest contiguous internal block
 * and covered that call depth. One update per lambda keeps the CHIP task's
 * stack at the same depth a single-endpoint push always used, matching what
 * was already proven stable. Values are captured by copy (well under the
 * SDK's 24-byte inline lambda-storage cap), so no heap/PSRAM snapshot is
 * needed either. */
static void schedule_uint8_update(uint16_t endpoint_id, uint32_t attribute_id, uint8_t value)
{
    s_pending_pushes.fetch_add(1, std::memory_order_relaxed);
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, attribute_id, value]() {
        esp_matter_attr_val_t val = esp_matter_uint8(value);
        attribute::update(endpoint_id, Thermostat::Id, attribute_id, &val);
        s_pending_pushes.fetch_sub(1, std::memory_order_relaxed);
    });
}

static void schedule_int16_update(uint16_t endpoint_id, uint32_t attribute_id, int16_t value, bool nullable)
{
    s_pending_pushes.fetch_add(1, std::memory_order_relaxed);
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, attribute_id, value, nullable]() {
        esp_matter_attr_val_t val = nullable ? esp_matter_nullable_int16(value) : esp_matter_int16(value);
        attribute::update(endpoint_id, Thermostat::Id, attribute_id, &val);
        s_pending_pushes.fetch_sub(1, std::memory_order_relaxed);
    });
}

static void schedule_onoff_update(uint16_t endpoint_id, bool value)
{
    s_pending_pushes.fetch_add(1, std::memory_order_relaxed);
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, value]() {
        esp_matter_attr_val_t val = esp_matter_bool(value);
        attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
        s_pending_pushes.fetch_sub(1, std::memory_order_relaxed);
    });
}

/* Pushes the BACnet-backed room state into the Matter attribute store so
 * subscribed controllers see live readings, not just the value from the
 * last commissioning-time default. Matter attribute updates must happen on
 * the Matter (CHIP) thread, hence ScheduleLambda rather than calling
 * attribute::update() directly from this FreeRTOS task.
 */
static void push_room_state_to_matter(void)
{
    if (s_system_endpoint_id == 0) {
        return;
    }
    if (s_pending_pushes.load(std::memory_order_relaxed) != 0) {
        /* Previous push hasn't fully drained through the CHIP event loop
         * yet; skip this cycle rather than queue more work on top of it. */
        return;
    }

    bool system_on;
    if (s_system_power_read_cb && s_system_power_read_cb(&system_on)) {
        schedule_onoff_update(s_system_endpoint_id, system_on);
    }

    matter_system_mode_t boost_mode;
    if (s_boost_read_cb && s_boost_read_cb(&boost_mode)) {
        schedule_onoff_update(s_boost_heat_endpoint_id, boost_mode == MATTER_SYSTEM_MODE_HEAT);
        schedule_onoff_update(s_boost_cool_endpoint_id, boost_mode == MATTER_SYSTEM_MODE_COOL);
    }

    for (size_t i = 0; i < hvac_core_get_room_count() && i < HVAC_CORE_MAX_ROOMS; ++i) {
        hvac_room_config_t *room = hvac_core_get_room(i);
        if (!room || !room->active || s_room_endpoint_ids[i] == 0) continue;
        uint16_t endpoint_id = s_room_endpoint_ids[i];

        int16_t temp;
        if (matter_thermostat_read_local_temperature(i, &temp)) {
            schedule_int16_update(endpoint_id, Thermostat::Attributes::LocalTemperature::Id, temp, true);
        }
        int16_t setpoint;
        if (matter_thermostat_read_cooling_setpoint(i, &setpoint)) {
            schedule_int16_update(endpoint_id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, setpoint, false);
        }
        matter_system_mode_t mode;
        if (matter_thermostat_read_system_mode(i, &mode)) {
            schedule_uint8_update(endpoint_id, Thermostat::Attributes::SystemMode::Id, (uint8_t)mode);
        }
    }
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

/* esp_matter's own endpoint::thermostat::create() hardcodes both the Heat
 * and Cool features together (see esp_matter_endpoint.cpp), which is what
 * gives a Matter "Auto" thermostat its native two-setpoint UI in Google/
 * Apple Home - there is no single-setpoint auto-changeover concept in the
 * Matter Thermostat cluster spec. This FCU only ever has one setpoint, so
 * a room endpoint is built by hand here with only the Cool feature (the
 * same identify/groups/descriptor/device-type wiring esp_matter's helper
 * does internally, just with a smaller feature set) instead of going
 * through that helper. Reported SystemMode is Cool/Off, not Auto/Off, to
 * match: Auto is not a valid mode without the Auto feature bit. */
/* Gives an endpoint a friendly name distinct from its generic device-type
 * label (e.g. "Bedroom Climate" instead of a bare "Thermostat" card) via
 * the Bridged Device Basic Information cluster's NodeLabel attribute - a
 * plain CHAR_STRING, unlike FixedLabel/Descriptor TagList which are
 * List<Struct> attributes esp_matter has no documented raw-byte encoding
 * for in this SDK version (see docs/BACKLOG.md history). Google/Apple Home
 * are observed reading this even without a literal Aggregator/bridge
 * parent, since both already split this node's endpoints into separate
 * device cards on their own. `label` must outlive the endpoint (matter
 * runs until reboot), so callers must pass storage with static duration. */
static void set_bridged_node_label(endpoint_t *ep, const char *label)
{
    cluster::bridged_device_basic_information::config_t bdbi_config;
    cluster_t *bdbi_cluster = cluster::bridged_device_basic_information::create(ep, &bdbi_config, CLUSTER_FLAG_SERVER);
    if (bdbi_cluster == nullptr) {
        ESP_LOGW(TAG, "Failed to create Bridged Device Basic Information cluster for label '%s'", label);
        return;
    }
    /* Matter's NodeLabel is capped at 32 bytes; truncate rather than let a
     * long room name silently fail to set a label at all. */
    constexpr size_t kMaxNodeLabelLength = 32;
    size_t label_len = strlen(label);
    if (label_len > kMaxNodeLabelLength) {
        label_len = kMaxNodeLabelLength;
    }
    cluster::bridged_device_basic_information::attribute::create_node_label(
        bdbi_cluster, const_cast<char *>(label), (uint16_t)label_len);
}

static endpoint_t *create_room_thermostat_endpoint(node_t *node, cluster::thermostat::config_t *thermostat_config,
                                                   const char *node_label)
{
    endpoint_t *ep = endpoint::create(node, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }

    cluster::descriptor::config_t descriptor_config;
    if (cluster::descriptor::create(ep, &descriptor_config, CLUSTER_FLAG_SERVER) == nullptr) {
        return nullptr;
    }
    if (add_device_type(ep, thermostat::get_device_type_id(), thermostat::get_device_type_version()) != ESP_OK) {
        return nullptr;
    }

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(chip::app::Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator);
    cluster::identify::create(ep, &identify_config, CLUSTER_FLAG_SERVER);

    cluster::groups::config_t groups_config;
    cluster::groups::create(ep, &groups_config, CLUSTER_FLAG_SERVER);

    if (cluster::thermostat::create(ep, thermostat_config, CLUSTER_FLAG_SERVER,
                                    cluster::thermostat::feature::cooling::get_id()) == nullptr) {
        return nullptr;
    }
    set_bridged_node_label(ep, node_label);
    return ep;
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

    /* EXPERIMENTAL: nest every endpoint under an Aggregator (device type
     * 0x000E), following the pattern Espressif's own zigbee_bridge example
     * uses (examples/bridge_apps/zigbee_bridge). Google Home already splits
     * our flat sibling endpoints into separate cards without this; this is
     * specifically to test whether HomeKit needs the literal Aggregator +
     * PartsList structure to do the same (see docs/BACKLOG.md). The
     * Descriptor cluster's PartsList is computed dynamically by walking
     * each endpoint's parent (connectedhomeip src/app/clusters/descriptor/
     * descriptor.cpp, emberAfParentEndpointFromIndex) - set_parent_endpoint()
     * below is the only wiring needed, nothing to populate by hand. */
    aggregator::config_t aggregator_config;
    endpoint_t *aggregator_ep = aggregator::create(s_node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    if (aggregator_ep == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter aggregator endpoint");
        return ESP_FAIL;
    }

    /* The system endpoint is a plain OnOff switch, not a thermostat: there
     * is no home-level temperature control or mode, only whether the plant
     * is allowed to run. It is deliberately separate from rooms - a room's
     * own power state must never be conflated with the plant switch. */
    on_off_plugin_unit::config_t system_config;
    system_config.on_off.on_off = false;
    endpoint_t *system_ep = on_off_plugin_unit::create(s_node, &system_config, ENDPOINT_FLAG_NONE, nullptr);
    if (system_ep == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter system OnOff endpoint");
        return ESP_FAIL;
    }
    s_system_endpoint_id = endpoint::get_id(system_ep);
    set_bridged_node_label(system_ep, "Home Cooling System");
    set_parent_endpoint(system_ep, aggregator_ep);

    /* Boost Heat/Boost Cool: two more plain OnOff switches, same shape as
     * the system switch above. Their config/create calls are on the stack
     * (small, transient); their labels are string literals (.rodata, no RAM
     * cost); esp_matter's own cluster/attribute allocations are already
     * routed to PSRAM (CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL) - nothing
     * new here adds to the internal .bss budget that overflowed earlier. */
    on_off_plugin_unit::config_t boost_heat_config;
    boost_heat_config.on_off.on_off = false;
    endpoint_t *boost_heat_ep = on_off_plugin_unit::create(s_node, &boost_heat_config, ENDPOINT_FLAG_NONE, nullptr);
    if (boost_heat_ep == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter Boost Heat endpoint");
        return ESP_FAIL;
    }
    s_boost_heat_endpoint_id = endpoint::get_id(boost_heat_ep);
    set_bridged_node_label(boost_heat_ep, "Boost Heat");
    set_parent_endpoint(boost_heat_ep, aggregator_ep);

    on_off_plugin_unit::config_t boost_cool_config;
    boost_cool_config.on_off.on_off = false;
    endpoint_t *boost_cool_ep = on_off_plugin_unit::create(s_node, &boost_cool_config, ENDPOINT_FLAG_NONE, nullptr);
    if (boost_cool_ep == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter Boost Cool endpoint");
        return ESP_FAIL;
    }
    s_boost_cool_endpoint_id = endpoint::get_id(boost_cool_ep);
    set_bridged_node_label(boost_cool_ep, "Boost Cool");
    set_parent_endpoint(boost_cool_ep, aggregator_ep);

    /* 8*40B of internal .bss overflowed this board's tight internal DRAM
     * budget by 280B at link time; this buffer only needs to outlive the
     * endpoint (matter runs until reboot), so PSRAM is fine for it. */
    static EXT_RAM_BSS_ATTR char s_room_node_labels[HVAC_CORE_MAX_ROOMS][40];
    size_t created_rooms = 0;
    for (size_t i = 0; i < hvac_core_get_room_count() && i < HVAC_CORE_MAX_ROOMS; ++i) {
        hvac_room_config_t *room = hvac_core_get_room(i);
        if (!room || !room->active) continue;

        cluster::thermostat::config_t thermostat_config;
        int16_t temperature = 2100;
        int16_t setpoint = 2200;
        matter_thermostat_read_local_temperature(i, &temperature);
        matter_thermostat_read_cooling_setpoint(i, &setpoint);
        matter_system_mode_t mode = MATTER_SYSTEM_MODE_OFF;
        matter_thermostat_read_system_mode(i, &mode);
        thermostat_config.local_temperature = nullable<int16_t>(temperature);
        thermostat_config.system_mode = (uint8_t)mode;
        thermostat_config.cooling.occupied_cooling_setpoint = setpoint;
        snprintf(s_room_node_labels[i], sizeof(s_room_node_labels[i]), "%s Climate", room->name);
        endpoint_t *room_ep = create_room_thermostat_endpoint(s_node, &thermostat_config, s_room_node_labels[i]);
        if (room_ep == nullptr) {
            ESP_LOGE(TAG, "Failed to create Matter endpoint for room %u", (unsigned)i);
            return ESP_FAIL;
        }
        s_room_endpoint_ids[i] = endpoint::get_id(room_ep);
        set_parent_endpoint(room_ep, aggregator_ep);
        ++created_rooms;
        ESP_LOGI(TAG, "Matter room endpoint %u created for %s", s_room_endpoint_ids[i], room->name);
    }
    ESP_LOGI(TAG, "Matter system endpoint %u plus %u active room endpoints created",
             s_system_endpoint_id, (unsigned)created_rooms);
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
    ESP_LOGI(TAG, "Matter post-start internal heap=%u B largest=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    /* Attribute push can transit deep CHIP scheduling paths: hardware proved
     * that 4 KiB overflows. Keep the 8 KiB stack, but put it in PSRAM so the
     * HTTP server retains an internal contiguous block for new sessions. */
    s_sync_task_tcb = static_cast<StaticTask_t *>(heap_caps_calloc(
        1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_sync_task_stack = static_cast<StackType_t *>(heap_caps_calloc(
        1, kMatterSyncStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_sync_task_tcb == nullptr || s_sync_task_stack == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate PSRAM stack for matter_sync_task");
        heap_caps_free(s_sync_task_stack);
        heap_caps_free(s_sync_task_tcb);
        s_sync_task_stack = nullptr;
        s_sync_task_tcb = nullptr;
    } else {
        s_sync_task_handle = xTaskCreateStaticPinnedToCore(
            matter_sync_task, "matter_sync", kMatterSyncStackWords, nullptr, 5,
            s_sync_task_stack, s_sync_task_tcb, tskNO_AFFINITY);
    }
    if (s_sync_task_handle == nullptr) {
        ESP_LOGW(TAG, "Failed to start matter_sync_task; live attribute push disabled");
    }
    ESP_LOGI(TAG, "Matter sync launched internal heap=%u B largest=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Matter system endpoint %u started; room endpoints are live", s_system_endpoint_id);
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

bool matter_adapter_close_pairing(void)
{
    if (!s_matter_running) {
        return false;
    }
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto &commissioning_mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    bool was_open = commissioning_mgr.IsCommissioningWindowOpen();
    if (was_open) {
        commissioning_mgr.CloseCommissioningWindow();
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return was_open;
}

bool matter_adapter_get_onboarding_info(matter_onboarding_info_t *out)
{
    if (out == nullptr) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->running = s_matter_running;
    out->paired = hvac_core_matter_pairing_get() == HVAC_PAIRING_PAIRED;
    if (!s_matter_running) {
        return true;
    }

    /* Both the commissioning manager and the SDK onboarding provider are
     * CHIP-stack state. The web server runs outside that event loop. */
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    out->window_open = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();

    chip::PayloadContents payload;
    CHIP_ERROR err = GetPayloadContents(payload,
        chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    if (err == CHIP_NO_ERROR) {
        out->discriminator = payload.discriminator.GetLongValue();
        out->vendor_id = payload.vendorID;
        out->product_id = payload.productID;

        chip::MutableCharSpan qr(out->qr_code, sizeof(out->qr_code) - 1);
        err = GetQRCode(qr, payload);
        if (err == CHIP_NO_ERROR) {
            out->qr_code[qr.size()] = '\0';
        }
        chip::MutableCharSpan manual(out->manual_code, sizeof(out->manual_code) - 1);
        if (err == CHIP_NO_ERROR) {
            err = GetManualPairingCode(manual, payload);
            if (err == CHIP_NO_ERROR) {
                out->manual_code[manual.size()] = '\0';
            }
        }
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Could not generate Matter onboarding payload: %" CHIP_ERROR_FORMAT, err.Format());
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
    /* Room endpoints only advertise the Cool feature (see
     * create_room_thermostat_endpoint): Auto is not a valid SystemMode
     * without the Auto feature bit, so report Cool for "on" instead. */
    *out_mode = is_on ? MATTER_SYSTEM_MODE_COOL : MATTER_SYSTEM_MODE_OFF;
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
