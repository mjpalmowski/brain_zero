# PACE CAN-to-BLE bridge and Venus Pi probe

The existing PACE ESP32 continues receiving its Pylontech-style CAN data. A
read-only BLE service forwards those original frames to brain_zero. Wi-Fi OTA,
ESPHome API, MQTT and the existing sensor callbacks can remain in the parent
configuration during migration. This package adds no CAN application transmit
function and no BLE configuration-write characteristic.

Use ESPHome 2026.10.2. Add an explicit `id: pace_can_bus` to the existing
`esp32_can` entry, retain its wiring/bitrate/mode, and include
`esphome/pace-can-ble.yaml` as a package. Copy
`esphome/components/pace_can_ble` into the existing device's `components`
directory. Existing `battery_link` components are retained. The package's
`pace_can_id` substitution can name another existing CAN bus ID.

Set API, MQTT and Wi-Fi `reboot_timeout: 0s` for operation without those services.
Use Wi-Fi `power_save_mode: light` so the radio retains modem sleep while sharing
RF time with BLE. The package supplies this default; a parent YAML with an
explicit `none` overrides it, so change the parent value to `light`. The guarded
installer makes that one radio-policy change when it encounters explicit NONE,
and retains existing LIGHT/HIGH choices, network details and OTA credentials.
Retain the existing OTA platform, credentials and partitions. Keep CAN normal
ACK participation when the ESP32 is the only CAN peer. Do not remove any
existing application heartbeat until its necessity has been captured and
qualified separately.

## BLE contract version 1

| Characteristic | UUID suffix in `e6c0000x-b5a3-4f4f-9b8d-73d619b32720` | Properties |
| --- | --- | --- |
| Service | `x = 1` | Advertised |
| Metadata | `x = 2` | Read |
| Received CAN frame | `x = 3` | Notify, 20 bytes |

Metadata is 20 bytes, generated at the actual GATT read:

| Offset | Length | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `BZCB` |
| 4 | 1 | Version, 1 |
| 5 | 1 | Flags, exactly 1 (read-only application transport) |
| 6 | 4 | Random nonzero boot ID, little endian |
| 10 | 4 | Current source `millis()`, little endian |
| 14 | 4 | Accepted CAN receive count, little endian |
| 18 | 2 | Dropped/superseded count, saturated at 65535 |

Each notification contains:

| Offset | Length | Value |
| --- | --- | --- |
| 0 | 1 | Version, 1 |
| 1 | 1 | CAN DLC, 0–8 |
| 2 | 2 | Standard CAN ID, little endian |
| 4 | 4 | Receive sequence, little endian, wraps modulo 2^32 |
| 8 | 4 | Original first CAN callback `millis()`, little endian |
| 12 | 8 | Unchanged CAN bytes, zero padded after DLC |

Only standard non-RTR data frames with IDs `0x351`, `0x355`, `0x356`, `0x359`,
`0x35C`, `0x35E` and `0x373` are forwarded. Each ID has one pending slot; a newer
sample supersedes an unsent older one. Notifications leave in sequence order,
at most one per 25 ms. Disconnect discards pending records. There is no timer
that refreshes or replays old battery measurements.

The timestamp is the **first ESPHome CAN software callback**, not a hardware ISR
or wire timestamp. The receiver adds a conservative 250 ms observation margin.
If either the adapter loop or receive callback detects a main-loop gap over
250 ms, pending records are discarded and input is quarantined for 500 ms.
The configuration validator requires CAN RX queue length no greater than the
maximum frames drained per loop; this drains a post-stall queue before new
timestamps can be trusted. The pinned TWAI default is five RX slots and the
ESPHome CAN default drains up to 50 frames per loop.

Clients read metadata with a measured RTT no greater than 250 ms, anchor source
time to the **start** of that read, and refresh it at least every 60 seconds.
Source time and sequence comparisons account for unsigned wrap. A changed boot
ID or BLE session invalidates previous measurements. Field validity expires
independently per CAN ID; fresh voltage or bridge heartbeats cannot refresh old
limits, permissions or alarms. Native BLE connection identifiers are configured
locally and are omitted from probe output.

## Venus Pi read-only test

Copy `pace_probe.py` to a temporary location on the Pi and run:

```sh
python3 pace_probe.py --seconds 45 --output pace-ble-report.json
```

If necessary, supply `--address` with the configured bridge's BLE address. The
address is never printed. The probe uses installed `dbus-python` and GLib;
there is no pip dependency on the Venus Pi. It scans LE, requires one matching
adapter, checks GATT properties, reads current metadata and subscribes to the
frame notifications. Cleanup stops its own scan/subscription and disconnects
its own connection. It does not pair, transmit CAN frames or change settings.

The JSON contains fresh decoded values, per-ID ages/counters, conservative
source-age validation, adapter drop count and cleanup results. It omits device
addresses, raw notifications and arbitrary advertised names.

Offline tests:

```sh
python -m unittest discover -s tools/bms_ble -p test_pace_probe.py -v
g++ -std=gnu++11 -O2 tools/bms_ble/adapter_protocol_test.cpp -o adapter-test
./adapter-test
```

The C++ test uses the actual adapter protocol/queue/stall implementation. The
Python tests exercise its independent Pi receiver, including original source
age, per-ID expiry, signed current, timestamps/sequence wrap and boot reset.

The BLE metadata and CAN notification implementation is project code. ESPHome's
runtime remains under its upstream licence and is separate from the main
brain_zero firmware.
