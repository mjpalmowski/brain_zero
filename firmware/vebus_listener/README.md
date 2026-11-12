# brain_zero firmware

This is the ESP32-S3 firmware for the brain_zero Rev C VE.Bus board. See the [project compatibility table](../../README.md#multiplus-ii-compatibility) for tested inverter models and firmware.

## Build

Release downloads contain one OTA application image, `firmware.bin`, for brain_zero hardware. For another board, build from this repository and configure its board, UART pins and flash layout in `platformio.ini` and `src/main.cpp` before flashing.

Install PlatformIO and run this command from the repository root.

```sh
pio run -d firmware/vebus_listener -e listener
```

The application image is `firmware/vebus_listener/.pio/build/listener/firmware.bin`. The build uses [PlatformIO Espressif 32 v6.8.1](https://github.com/platformio/platform-espressif32/tree/v6.8.1) and the [Arduino-ESP32 framework source](https://github.com/espressif/arduino-esp32/tree/dcc1105b0cf1322a437b354c336f2abf72b7e512).

Settings downloads support the verified legacy `2628459` identity using the
older schema 2 / unit 2 RVSC format. Grid metadata is omitted by that format;
timeouts on other identities still fail. The verified eight-byte identity also
supports guarded settings uploads, ESS after live Assistant discovery, and
Assistant installation/replacement and removal with the verified 4 KiB memory
area. Legacy removal follows Venus: it disables the presence flag and retains
all stored program bytes. A fresh complete readback verifies both conditions;
an already-disabled bank needs no Assistant write. Ten-byte legacy exports
remain read-only.
See the [implementation and isolated tests](../../docs/vebus_settings_downloads_2026-11-11.md#assistant-removal-for-2628459-12-november-2026).

Portable regressions in `test/legacy_backup_test.cpp` and `test/rvsc_test.cpp`
use synthetic traffic and configurations, with no physical device transport.
They can be compiled with a C++11 compiler, including `src/requester.cpp`,
`src/rvsc.cpp`, `src/charger_settings.cpp`, `src/inverter_controls.cpp` and
`src/vebus_state.cpp`, and `src` on the include path.

For a custom first-boot image, copy `include/wifi_secrets.example.h` to `include/wifi_secrets.h` in this folder and set the Wi-Fi credentials locally before building. Keep that file and images built with those credentials private.

## Settings downloads for unqualified inverters

**System → Inverter settings → Download settings** performs a full read-only
backup. It does not require an ESS or settings-write support profile. A model or
firmware that is not qualified for control can still provide its `.rvsc` backup
when it answers the supported VE.Bus settings-read protocol.

The complete firmware/unit reply, settings, access descriptors, grid records and
Assistant records must fit the controller's buffers and finish successfully.
Incomplete reads are not offered as downloadable backups. Reading a backup does
not enable settings uploads, ESS or other controls; their qualification checks
remain separate.

The sanitized compatibility JSON exporter additionally requires a recognized
230 V MultiPlus-II family. That is separate from downloading the original
`.rvsc` backup. Simulation checks cover the read/export path; untested physical
models still need hardware verification.

The [35-prefix Venus simulator investigation](../../docs/vebus_settings_downloads_2026-11-11.md)
records native downloads and byte-identical production reader comparisons.
Supported long descriptors must include their access byte; truncated replies
block the backup even when the inverter is not qualified for writes.

## Update over Wi-Fi

1. Open the controller’s local console and select **System → Firmware update**.
2. Select **Check for updates**. If a newer Rev C build is available, select **Download and verify**.
3. Once the image is staged, select **Install and restart**. After the restart, check the dashboard and select **Keep this build** when you are satisfied.

The controller checks GitHub Releases for the version and checksum, then downloads the matching image over HTTPS from brain-zero.com. It verifies the SHA-256 before staging the image in the inactive slot. The update preserves saved settings and requires internet access, a synced clock, and a compatible ESP32-S3 partition layout.

Release downloads defer the automatic read-only charger refresh until staging
finishes or fails. This prevents that refresh from being mistaken for a busy
inverter during download. Readiness is still checked every 100 ms, before
finishing staging and again before installation. A cancellation records its
first readiness condition as `blocked_by` in `/api/firmware/release`.

Delayed receive timestamps cannot turn a recent, exact copy of an actual
transmission into a second controller. Ownership attribution is single-use and
bounded by the existing 200 ms reply window; it does not acknowledge requests
or extend their deadlines. Other frames retain the normal foreign-master
checks. Diagnostics exposes the count as `ctl.late_own_echoes`.

If the page has only **Upload a build**, first download the Rev C application image and select **Upload and verify** once. This also works for locally built images. [GitHub Releases](https://github.com/mjpalmowski/brain_zero/releases)

For a new public OTA release, set `RELEASE_VERSION` to the build date times 100 plus its sequence number. Use the matching `firmware-YYYYMMDD.N` GitHub tag. Publish the same `firmware.bin` under `/firmware/<tag>/firmware.bin` on brain-zero.com before publishing the GitHub release. Attach only that OTA application image to the release; include its checksum and links to build instructions, source and notices in the release description. Other boards use builds compiled from the repository.

Publish from a clean public checkout using the release helper, replacing the example tag with the matching version:

```sh
python tools/publish_firmware.py firmware-YYYYMMDD.N --dry-run
python tools/publish_firmware.py firmware-YYYYMMDD.N --notes release-notes.md --preview
```

The helper accepts only the public `listener/firmware.bin`, rejects private site builds and secret headers, and checks the OTA image and hosted copy. It creates a draft with one asset, verifies GitHub's size and SHA-256, then publishes. Omit `--preview` for a stable release. Source archives and notices are linked in the description rather than attached. GitHub also generates its own source-code archive links.

## Third-party notices

The translation catalogue is based on Victron Energy code and remains subject to the [Victron Energy OS license](../../assets/brand/VENUS-TRANSLATIONS-LICENSE.txt). The VEConfigure field labels and file-format references include material from mk2vsc under its [MIT license](third_party/mk2vsc_LICENSE.txt).

The application links Arduino-ESP32 under the [GNU LGPL 2.1 or later](third_party/Arduino-ESP32-LICENSE.md). The corresponding [Arduino source archive](https://brain-zero.com/sources/arduino-esp32-dcc1105b-source.tar.gz) and [third-party notices bundle](https://brain-zero.com/sources/third-party-notices.zip) are available separately, with the notices also kept in this repository. It uses PlatformIO framework package `3.20017.241212+sha.dcc1105b` and [ESP-IDF 4.4.7 at revision `38eeba213a`](https://github.com/espressif/esp-idf/tree/38eeba213aa695aabfd6d89aa9f5078dbe5a94c3). See the [ESP-IDF license](third_party/ESP-IDF-LICENSE.txt) and [component notices](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/COPYRIGHT.html), including the [Wi-Fi](third_party/ESP32-WIFI-LICENSE.txt) and [PHY](third_party/ESP32-PHY-LICENSE.txt) library licenses.

To rebuild against a modified Arduino framework, set PlatformIO's [`platform_packages`](https://docs.platformio.org/en/latest/projectconf/sections/env/options/platform/platform_packages.html) option to a local framework package with the matching ESP32-S3 SDK, then run the same build command. Espressif publishes the [SDK build recipe](https://github.com/espressif/esp32-arduino-lib-builder/tree/release/v4.4).
