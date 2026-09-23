# Integration module direction

See [`MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md`](MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md)
for the ecosystem assessment, hardware decision, implementation sequence and
verification gates.

The BACnet bridge has one protocol-neutral HVAC model. It owns the persisted
room-to-BACnet mapping, active-room count and the device-facing meaning of a
room's setpoint, temperature, power and health points. External integrations
translate this model; they must not add a second copy of the mapping or write
BACnet directly.

## Current extraction

`components/hvac_core` owns the public room-model contract and NVS schema.
`main.c` uses that shared model while the existing MQTT/Home Assistant code is
left behaviourally unchanged. This is deliberately the first extraction: it
keeps saved configuration and Home Assistant entity identity stable.

## Module selection

Only one automation transport is active at a time:

- `none`: web dashboard only;
- `mqtt_home_assistant`: existing MQTT plus Home Assistant discovery; and
- `matter`: Matter-over-Wi-Fi, using standard Thermostat endpoints.

The selected mode is now persisted by `hvac_core` in its own NVS namespace.
Existing installations default to `mqtt_home_assistant`, preserving their
current startup behaviour. The wizard/API selection control is a later step;
Matter must not be selected in production firmware until its implementation
and resource gates pass.

The web dashboard, BACnet worker, OTA and diagnostics remain available in all
modes. Matter is the direct Apple Home, Google Home and Alexa path; it does
not replace the dashboard for Delta-specific diagnostics or boost controls.

## Matter implementation gates

Matter is T-ETH-Lite only. It needs ESP-Matter, BLE commissioning, a T-ETH
partition table with larger OTA slots and factory commissioning data. Do not
enable it alongside MQTT by default: the selected transport must be measured
with BACnet, Wi-Fi, HTTP and object scanning for flash usage, internal heap,
largest free block and task headroom before it is offered in the wizard.

The W5500 recovery profile remains MQTT/web only because its 4 MiB flash and
no-PSRAM budget cannot provide a supported Matter target.
