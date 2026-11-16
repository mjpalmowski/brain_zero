# Bluetooth batteries

Open **Settings → Batteries** to configure direct battery monitoring. brain_zero
can read the qualified JK Bluetooth profile and the PACE CAN-to-BLE bridge at
the same time, and publish their measurements to Home Assistant over MQTT.
Neither the Pi nor Home Assistant carries the Bluetooth measurements to
brain_zero.

## Battery connections

- **JK:** a direct read-only Bluetooth connection. The initial qualified profile
  is JK-B2A24S20P, hardware 10.XG, firmware 10.07, using JK02_24S telemetry.
  Other identity/layout combinations are rejected until qualified.
- **PACE:** the existing ESP32 CAN bridge retains its CAN wiring, Wi-Fi, HA
  sensors and password-protected OTA. The [ESPHome package](../esphome/pace-can-ble.yaml)
  adds a read-only Bluetooth service for the PYLON CAN messages already received
  by that bridge. There is no Bluetooth-to-CAN command endpoint.

Set the two Bluetooth addresses locally. They are excluded from MQTT telemetry
and discovery. Release the JK connection from an existing bridge or phone before
enabling it in brain_zero; this JK accepts one active connection.

Leave **ESS state of charge source** on **Existing source** to preserve the
current controller behaviour. Selecting JK, PACE or the combined bank is an
explicit control-source change. If an explicitly selected battery stops
reporting, its SOC becomes unavailable rather than falling back to a previously
synchronized inverter value.

## Parallel packs

Combined bank monitoring is for packs connected in parallel to the same DC bus.
Voltage remains a shared-bus voltage; current is summed, with positive current
meaning charging. Combined SOC is weighted by each pack's capacity.

JK capacity can be read from the BMS. Set a nonzero override only when needed.
Enter PACE's usable pack capacity in Ah: this CAN profile does not report it.
Until both capacities and both fresh SOC values are known, combined SOC is
unavailable. Individual pack measurements remain available.

## Optional charge limits

Charge limits start **off**. To enable them, configure all three thresholds:
maximum pack SOC, maximum cell voltage and maximum cell spread. Then select the
charging power permitted when a threshold trips; zero pauses charging. Small
hysteresis prevents repeated switching at a threshold.

The guard requires fresh SOC, cell extrema, charging permission and protection
status from **both** packs. Missing required data or an active protection sets a
zero charge request. The cap applies in every ESS mode, including External
control, and tightening it does not wait for the normal setpoint ramp.

This controls the MultiPlus ESS AC charge request. Home Assistant remains
responsible for coordinating other chargers. Battery protection hardware remains
independent. Saving these options does not rewrite stored inverter settings.

## MQTT and diagnostics

Each pack and the combined bank has a state topic under the configured base:
`<base>/bms/jk`, `<base>/bms/pace`, and `<base>/bms/bank`. Invalid or expired
measurements are JSON `null`; a new bridge heartbeat cannot refresh an old CAN
measurement. Home Assistant discovery creates battery devices and their sensors,
including SOC, voltage, current, cell extrema/spread, temperatures, limits and
protection status. Individual JK cell sensors are available as optional
diagnostic entities.

`/api/bms` exposes local link state, decoder counters, per-link stack margin and
actual internal heap usage. The firmware's static RAM budget and runtime heap
usage are separate measurements. Disabling Bluetooth disconnects the packs;
the initialized radio resources are released on the next normal restart.

## Firmware updates

**System → Firmware update** uses a temporary update mode when Bluetooth is
running. The controller closes its battery links and restarts with Bluetooth
paused, freeing memory for the HTTPS check or download. Saved battery settings,
ESS settings and schedules are retained. Firmware installation still uses the
normal **Install and restart** and **Keep this build** controls.

Battery monitoring reconnects after a completed check with no newer release,
an error, cancellation or an unused five-minute update window. When a release
is available, monitoring stays paused while you choose whether to download or
install it. **Cancel** resumes monitoring. A normal boot reconnects both packs.

The BMS charge guard, when enabled, continues to treat missing required battery
data as a reason to pause charging. The existing ESS SOC source is preserved;
changing that source remains an explicit owner choice.

The firmware enforces a static RAM budget below 35%. Bluetooth and networking
also allocate runtime heap, so actual memory use is higher. Two 2 KiB diagnostic
TX buffers replace the former 16 KiB buffers; diagnostic output drops whole
messages under backpressure rather than holding up the inverter task. The update
mode avoids running the BLE host and the HTTPS updater simultaneously.

## Qualification

The qualified JK identity above passed live Venus Pi reads and decoder replay,
then live monitoring on Rev C. The PACE bridge passed live Rev C monitoring of
SOC, signed current, cell extrema, limits, permissions and protection status.
The Pi's PACE BLE test remains incomplete; a cached advertisement is not counted
as a successful discovery or GATT test.

Rev C was tested with both links active, MQTT discovery and live HA readings.
An HTTPS release check closed both links, rebooted into update mode, and
reconnected both automatically on their first attempt. A full local Wi-Fi OTA
upload was verified and discarded while both streams stayed active. ESS settings
and schedules were retained, with no stored inverter-setting writes. Charge
guard behaviour was tested offline and left disabled during live acceptance.

Build `20261116.7` uses 114,392 of 327,680 bytes of static RAM (34.9%). Live
internal heap free was approximately 46–52 KiB with both links, rising to about
125 KiB before HTTPS in update mode. Runtime memory depends on network activity.

For the gateway protocol, the read-only Venus Pi probes and offline tests, see
[the bridge development guide](../tools/bms_ble/README.md).
