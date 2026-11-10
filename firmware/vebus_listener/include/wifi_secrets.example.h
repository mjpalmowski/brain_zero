// Copy to wifi_secrets.h (git-ignored) and put in the password of the network the listener should
// join. With an empty password the listener leaves Wi-Fi off and only logs.
#pragma once

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Optional transmit power cap in quarter-dBm units. The SDK default is used otherwise.
// #define WIFI_TX_POWER_QDBM 34  // 8.5 dBm
// #define WIFI_CHANNEL_WIDTH_20  // Limit the 2.4 GHz channel to 20 MHz.
// #define WIFI_LEGACY_BG  // Try 802.11b/g if an AP repeatedly drops the S3 in 802.11n mode.
