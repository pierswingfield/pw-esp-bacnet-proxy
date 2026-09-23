/*
 * CHIP/Matter project config override for the T-ETH-Lite Matter build
 * variant. CHIP_CONFIG_MAX_FABRICS cascades into a large number of
 * statically-sized pools (secure sessions, subscriptions, session resume
 * cache, thread network directory) that do not fit this board's internal
 * DRAM budget at the SDK default of 16. A single local Thermostat realistically
 * needs a handful of ecosystem fabrics (Apple Home, Google Home, Alexa),
 * not sixteen simultaneous admins.
 */
#pragma once

#define CHIP_CONFIG_MAX_FABRICS 4

/* These already derive from CHIP_CONFIG_MAX_FABRICS in CHIPConfig.h, so
 * reducing MAX_FABRICS alone shrinks them; the explicit overrides below
 * trim further, since this bridge has one system and a small number of room
 * Thermostat endpoints rather than a large accessory fleet.
 */
#define CHIP_IM_MAX_REPORTS_IN_FLIGHT 2
#define CHIP_IM_MAX_NUM_TIMED_HANDLER 4

/* These are the device's Matter Basic Information values. They replace the
 * SDK's TEST_VENDOR / TEST_PRODUCT strings, but do not claim a certified
 * vendor identity: commissioning still uses the development VID/PID until
 * the product is formally certified and provisioned with production DACs.
 * They are read only from the Basic Information cluster's VendorName/
 * ProductName attributes, which a controller can only fetch after
 * commissioning (see GenericDeviceInstanceInfoProvider::GetVendorName/
 * GetProductName in connectedhomeip) - they do NOT reach the DNS-SD
 * commissionable-node advertisement a controller's app reads while
 * *scanning*, before pairing. That's a completely separate mechanism,
 * enabled below. */
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "Wingfield.tech"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "Home Climate Bridge"

/* The DNS-SD commissionable-node advertisement has its own "DN" (device
 * name) TXT key, populated by DnssdServer::Advertise() only when this is
 * enabled (connectedhomeip src/app/server/Dnssd.cpp) - off by default, and
 * we were never setting it, which is the likely reason Google Home showed
 * a generic "Matter-enabled accessory" label while scanning: there was no
 * device name on the wire yet for it to show, not that it deliberately
 * discarded ours. CHIP_DEVICE_CONFIG_DEVICE_NAME is a separate macro from
 * VENDOR_NAME/PRODUCT_NAME above (defaults to the SDK's "Test Kitchen").
 * Max 32 characters per the DN TXT key's spec limit. */
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME 1
#define CHIP_DEVICE_CONFIG_DEVICE_NAME "Home Climate Bridge"
