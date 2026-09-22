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
 * trim further, since this board only ever exposes one Thermostat endpoint.
 */
#define CHIP_IM_MAX_REPORTS_IN_FLIGHT 2
#define CHIP_IM_MAX_NUM_TIMED_HANDLER 4
