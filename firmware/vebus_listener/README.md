# brain_zero firmware

This is the ESP32-S3 firmware for the brain_zero Rev C VE.Bus board. See the [project compatibility table](../../README.md#multiplus-ii-compatibility) for tested inverter models and firmware.

## Build

Install PlatformIO and run this command from the repository root.

```sh
pio run -d firmware/vebus_listener -e listener
```

The application image is `firmware/vebus_listener/.pio/build/listener/firmware.bin`. The build uses [PlatformIO Espressif 32 v6.8.1](https://github.com/platformio/platform-espressif32/tree/v6.8.1) and the [Arduino-ESP32 framework source](https://github.com/espressif/arduino-esp32/tree/dcc1105b0cf1322a437b354c336f2abf72b7e512).

For a custom first-boot image, copy `include/wifi_secrets.example.h` to `include/wifi_secrets.h` in this folder and set the Wi-Fi credentials locally before building. Keep that file and images built with those credentials private.

## Update over Wi-Fi

1. Open the controller’s local console and select **System → Firmware update**.
2. Select **Check for updates**. If a newer Rev C build is available, select **Download and verify**.
3. Once the image is staged, select **Install and restart**. After the restart, check the dashboard and select **Keep this build** when you are satisfied.

The controller checks GitHub Releases for the version and checksum, then downloads the matching image over HTTPS from brain-zero.com. It verifies the SHA-256 before staging the image in the inactive slot. The update preserves saved settings and requires internet access, a synced clock, and a compatible ESP32-S3 partition layout.

If the page has only **Upload a build**, first download the Rev C application image and select **Upload and verify** once. This also works for locally built images. [GitHub Releases](https://github.com/mjpalmowski/brain_zero/releases)

For a new public OTA release, set `RELEASE_VERSION` to the build date times 100 plus its sequence number. Use the matching `firmware-YYYYMMDD.N` GitHub tag. Publish the same `firmware.bin` under `/firmware/<tag>/firmware.bin` on brain-zero.com before publishing the GitHub release.

## Third-party notices

The translation catalogue is based on Victron Energy code and remains subject to the [Victron Energy OS license](../../assets/brand/VENUS-TRANSLATIONS-LICENSE.txt). The VEConfigure field labels and file-format references include material from mk2vsc under its [MIT license](third_party/mk2vsc_LICENSE.txt).

The application links Arduino-ESP32 under the [GNU LGPL 2.1 or later](third_party/Arduino-ESP32-LICENSE.md). The firmware release provides the corresponding Arduino source archive. It uses PlatformIO framework package `3.20017.241212+sha.dcc1105b` and [ESP-IDF 4.4.7 at revision `38eeba213a`](https://github.com/espressif/esp-idf/tree/38eeba213aa695aabfd6d89aa9f5078dbe5a94c3). See the [ESP-IDF license](third_party/ESP-IDF-LICENSE.txt) and [component notices](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/COPYRIGHT.html), including the [Wi-Fi](third_party/ESP32-WIFI-LICENSE.txt) and [PHY](third_party/ESP32-PHY-LICENSE.txt) library licenses.

To rebuild against a modified Arduino framework, set PlatformIO's [`platform_packages`](https://docs.platformio.org/en/latest/projectconf/sections/env/options/platform/platform_packages.html) option to a local framework package with the matching ESP32-S3 SDK, then run the same build command. Espressif publishes the [SDK build recipe](https://github.com/espressif/esp32-arduino-lib-builder/tree/release/v4.4).
