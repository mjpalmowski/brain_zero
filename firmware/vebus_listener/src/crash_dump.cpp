#include "crash_dump.h"

#include <Arduino.h>
#include <WebServer.h>
#include <esp_core_dump.h>
#include <esp_partition.h>

namespace crash_dump {
void download(WebServer &server) {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Content-Type-Options", "nosniff");
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                    ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!partition) {
        server.send(404, "application/json", "{\"error\":\"no_coredump_partition\"}");
        return;
    }
    uint32_t stored_length = 0;
    if (partition->size < sizeof stored_length ||
        esp_partition_read(partition, 0, &stored_length, sizeof stored_length) != ESP_OK) {
        server.send(500, "application/json", "{\"error\":\"coredump_read_failed\"}");
        return;
    }
    // IDF 4.4 reports a blank partition as INVALID_SIZE rather than NOT_FOUND.
    if (stored_length == UINT32_MAX) {
        server.send(404, "application/json", "{\"error\":\"no_stored_coredump\"}");
        return;
    }
    size_t address = 0, length = 0;
    const esp_err_t found = esp_core_dump_image_get(&address, &length);
    if (found == ESP_ERR_NOT_FOUND) {
        server.send(404, "application/json", "{\"error\":\"no_stored_coredump\"}");
        return;
    }
    if (found != ESP_OK) {
        server.send(500, "application/json", "{\"error\":\"coredump_read_failed\"}");
        return;
    }
    // Never export bytes outside the dedicated partition, even if a damaged
    // crash header advertises a nonsensical size. Include the checksum and flash
    // header unchanged so the host's ESP-IDF decoder can validate the evidence.
    if (address != partition->address || length < 20 || length > partition->size) {
        server.send(409, "application/json", "{\"error\":\"invalid_coredump_bounds\"}");
        return;
    }
    uint8_t block[512];
    size_t offset = 0;
    const uint32_t started = millis();
    server.sendHeader("Content-Disposition", "attachment; filename=\"brain-zero-coredump.bin\"");
    server.setContentLength(length);
    server.send(200, "application/octet-stream", "");
    auto client = server.client();
    while (offset < length) {
        const size_t count = length - offset < sizeof block ? length - offset : sizeof block;
        if (!client.connected() || uint32_t(millis() - started) >= 15000 ||
            esp_partition_read(partition, offset, block, count) != ESP_OK ||
            client.write(block, count) != count) {
            // A truncated response is intentionally detectable by Content-Length.
            // Leave the dump untouched so an authenticated client can retry.
            client.stop();
            return;
        }
        offset += count;
        delay(0);
    }
}
}  // namespace crash_dump
