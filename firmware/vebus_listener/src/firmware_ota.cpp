#include "firmware_ota.h"

#include <Arduino.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <string.h>

// Arduino otherwise confirms pending OTA apps during initArduino, before setup().
extern "C" bool verifyRollbackLater() { return true; }

namespace firmware_ota {
namespace {
constexpr int BOOT_PIN = 0;
bool receiving = false, started = false, staged = false, trial = false;
char nonce[33] = "";
const char *message = "Choose an application file to begin.";
const esp_partition_t *running = nullptr, *target = nullptr;
esp_ota_handle_t handle;
size_t expected = 0, received = 0;
uint8_t prefix[PREFIX_BYTES];
Action queued = Action::NONE;

const char *fail(const char *why) {
    discard(why);
    return why;
}
}

void begin() {
    pinMode(BOOT_PIN, INPUT_PULLUP);
    running = esp_ota_get_running_partition();
    target = esp_ota_get_next_update_partition(nullptr);
    const size_t flash = ESP.getFlashChipSize();
    if (!running || !target || target->address == running->address ||
        target->address > flash || target->size > flash - target->address ||
        !esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr)) target = nullptr;
    esp_ota_img_states_t state;
    trial = running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    if (trial) message = "Trial firmware is running. Check that it works before keeping it.";
    if (!target) message = "This flash layout has no spare OTA application slot.";
}

Status status() {
    // Mint on the first local web request, once Wi-Fi is running. The token
    // blocks cross-site browser requests; it is not a LAN login credential.
    if ((target || trial) && !nonce[0]) {
        uint8_t random[16];
        esp_fill_random(random, sizeof random);
        for (size_t i = 0; i < sizeof random; ++i) snprintf(nonce + i * 2, 3, "%02x", unsigned(random[i]));
    }
    return {trial, staged, target ? target->size : 0, staged ? received : 0,
            running ? running->address : 0, running ? running->label : "", target ? target->label : "",
            nonce, message, queued};
}

bool authorized(const char *token) {
    return nonce[0] && token && strcmp(token, nonce) == 0;
}

const char *start(size_t bytes) {
    if (!target) return "No spare OTA slot is available.";
    if (trial) return "Keep or roll back the current trial before uploading another build.";
    if (queued != Action::NONE || receiving || staged) return "An update is already in progress or staged.";
    if (bytes < PREFIX_BYTES || bytes > target->size) return "The app file does not fit the OTA slot.";
    const auto *boot = esp_ota_get_boot_partition();
    if (!boot || boot->address != running->address) return "Another application is already selected for the next boot.";
    expected = bytes;
    received = 0;
    receiving = true;
    message = "Receiving firmware.";
    return nullptr;
}

const char *write(const uint8_t *data, size_t bytes) {
    if (!receiving) return fail("No firmware upload is in progress.");
    if (bytes > expected - received) return fail("Upload is larger than the declared file size.");
    if (!started) {
        const size_t n = min(bytes, sizeof prefix - received);
        memcpy(prefix + received, data, n);
        received += n;
        data += n;
        bytes -= n;
        if (received < sizeof prefix) return nullptr;
        if (!app_prefix(prefix)) return fail("Choose an ESP32-S3 application .bin, such as PlatformIO firmware.bin.");
        if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK)
            return fail("Could not open the inactive firmware slot.");
        started = true;
        if (esp_ota_write(handle, prefix, sizeof prefix) != ESP_OK) return fail("Writing to the inactive slot failed.");
    }
    if (bytes && esp_ota_write(handle, data, bytes) != ESP_OK) return fail("Writing to the inactive slot failed.");
    received += bytes;
    return nullptr;
}

const char *finish() {
    if (!receiving || !started || received != expected) return fail("Firmware upload is incomplete.");
    started = false; // esp_ota_end releases the handle on both success and failure
    if (esp_ota_end(handle) != ESP_OK) return fail("The firmware image failed integrity or compatibility checks.");
    const esp_partition_pos_t part = {target->address, target->size};
    esp_image_metadata_t metadata = {};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &part, &metadata) != ESP_OK || metadata.image_len != expected)
        return fail("Upload must contain exactly one application image.");
    receiving = false;
    staged = true;
    message = "Image verified and staged. Select Install and restart to boot it.";
    return nullptr;
}

void discard(const char *why) {
    if (started) esp_ota_abort(handle);
    started = receiving = staged = false;
    expected = received = 0;
    message = why;
}

const char *keep() {
    if (!trial || queued != Action::NONE) return "No unconfirmed trial is running.";
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) return "Could not confirm the trial firmware.";
    trial = false;
    message = "Firmware confirmed.";
    return nullptr;
}

const char *queue(Action action) {
    if (queued != Action::NONE) return "A firmware restart is already queued.";
    if (digitalRead(BOOT_PIN) == LOW) return "Release BOOT before restarting.";
    if (action == Action::INSTALL ? !staged : !trial) return "No firmware is ready for this action.";
    queued = action;
    message = "Restart queued.";
    return nullptr;
}

void cancel_restart(const char *why) {
    queued = Action::NONE;
    message = why;
}

bool activate() {
    if (digitalRead(BOOT_PIN) == LOW) return false;
    if (queued == Action::ROLLBACK) return trial; // a reboot of a pending image invokes bootloader rollback
    return queued == Action::INSTALL && staged && esp_ota_set_boot_partition(target) == ESP_OK;
}
}
