#include "firmware_release.h"
#include "firmware_ota.h"

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <new>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// WiFiClientSecure also ships an esp_crt_bundle.h in Arduino 2.0.x. Use
// the SDK's attach function, which selects the certificate bundle built into IDF.
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

namespace firmware_release {
namespace {
constexpr char RELEASES[] = "https://api.github.com/repos/mjpalmowski/brain_zero/releases?per_page=10";
constexpr char RELEASE_ASSETS[] = "https://github.com/mjpalmowski/brain_zero/releases/download/";
constexpr char FIRMWARE[] = "https://brain-zero.com/firmware/";
constexpr size_t ITEM_BYTES = 24 * 1024;
constexpr size_t LIST_BYTES = 10 * (ITEM_BYTES + 16);
constexpr int NETWORK_TIMEOUT_MS = 6000;
constexpr int64_t CHECK_TIMEOUT_US = 30 * 1000000LL;
constexpr int64_t DOWNLOAD_TIMEOUT_US = 120 * 1000000LL;
constexpr time_t VALID_CLOCK = 1704067200; // 2024-01-01 UTC

const char *asset_name(bool site) { return site ? "firmware-site.bin" : "firmware.bin"; }

bool digit(char c) { return c >= '0' && c <= '9'; }

int hex_digit(char c) {
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_digest(const char *digest, uint8_t (&out)[32]) {
    if (!digest || strlen(digest) != 71 || strncmp(digest, "sha256:", 7)) return false;
    for (size_t i = 0; i < sizeof out; ++i) {
        const int high = hex_digit(digest[7 + 2 * i]);
        const int low = hex_digit(digest[8 + 2 * i]);
        if (high < 0 || low < 0) return false;
        out[i] = uint8_t(high * 16 + low);
    }
    return true;
}

const char *field(const cJSON *object, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

bool select_build(const char *json, uint32_t running, size_t capacity, bool site, Build &best) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> release(cJSON_ParseWithOpts(json, nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(release.get())) return false;
    const char *tag = field(release.get(), "tag_name");
    const auto version = tag_version(tag);
    const auto *preview = cJSON_GetObjectItemCaseSensitive(release.get(), "prerelease");
    if (!version || version <= running || version <= best.version || !cJSON_IsBool(preview) ||
        !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(release.get(), "draft"))) return true;

    const cJSON *asset = nullptr;
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(release.get(), "assets");
    if (!cJSON_IsArray(assets)) return true;
    const cJSON *item;
    cJSON_ArrayForEach(item, assets) {
        const char *name = field(item, "name");
        if (name && !strcmp(name, asset_name(site))) {
            if (asset) return true; // Reject ambiguous asset names.
            asset = item;
        }
    }
    if (!asset) return true;
    const char *state = field(asset, "state");
    const char *url = field(asset, "browser_download_url");
    char expected_url[128];
    snprintf(expected_url, sizeof expected_url, "%s%s/%s", RELEASE_ASSETS, tag, asset_name(site));
    if (!state || strcmp(state, "uploaded") || !url || strcmp(url, expected_url)) return true;
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
    if (!cJSON_IsNumber(size) || !(size->valuedouble >= firmware_ota::PREFIX_BYTES &&
                                  size->valuedouble <= capacity && size->valuedouble <= UINT32_MAX)) return true;

    Build build;
    build.size = uint32_t(size->valuedouble);
    if (build.size != size->valuedouble || !parse_digest(field(asset, "digest"), build.sha256)) return true;
    memcpy(build.tag, tag, strlen(tag) + 1);
    build.version = version;
    build.preview = cJSON_IsTrue(preview);
    best = build;
    return true;
}

// Retain one release object at a time so unrelated releases and notes don't
// require a second copy of the entire response in RAM.
struct ReleaseList {
    enum class Position { OPEN, FIRST, NEXT, SEPARATOR, END };
    Position position = Position::OPEN;
    size_t used = 0, depth = 0;
    bool quoted = false, escaped = false;

    bool append(char c, char *item, uint32_t running, size_t capacity, bool site, Build &best) {
        if (!depth) {
            if (c == ' ' || c == '\r' || c == '\n' || c == '\t') return true;
            if (position == Position::OPEN && c == '[') {
                position = Position::FIRST;
                return true;
            }
            if ((position == Position::FIRST || position == Position::SEPARATOR) && c == ']') {
                position = Position::END;
                return true;
            }
            if (position == Position::SEPARATOR && c == ',') {
                position = Position::NEXT;
                return true;
            }
            if (c != '{' || (position != Position::FIRST && position != Position::NEXT)) return false;
            used = 0;
        }

        if (used == ITEM_BYTES - 1 || !c) return false;
        item[used++] = c;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            if (++depth > 16) return false;
        } else if (c == '}' && --depth == 0) {
            item[used] = 0;
            position = Position::SEPARATOR;
            if (!select_build(item, running, capacity, site, best)) return false;
        }
        return true;
    }
};

esp_err_t receive_header(esp_http_client_event_t *event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
    if (!strcasecmp(event->header_key, "Content-Encoding") && strcasecmp(event->header_value, "identity"))
        *static_cast<bool *>(event->user_data) = true;
    return ESP_OK;
}

class Http {
public:
    ~Http() { if (client) esp_http_client_cleanup(client); }

    bool open(const char *url, bool metadata, bool &encoded) {
        esp_http_client_config_t config = {};
        config.url = url;
        config.user_agent = "brain_zero firmware updater";
        config.timeout_ms = NETWORK_TIMEOUT_MS;
        config.disable_auto_redirect = true;
        config.max_authorization_retries = -1;
        config.event_handler = receive_header;
        config.user_data = &encoded;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        client = esp_http_client_init(&config);
        if (!client ||
            esp_http_client_set_header(client, "Accept-Encoding", "identity") != ESP_OK ||
            esp_http_client_set_header(client, "Accept", metadata ? "application/vnd.github+json" :
                                                               "application/octet-stream") != ESP_OK ||
            esp_http_client_open(client, 0) != ESP_OK) return false;
        return esp_http_client_fetch_headers(client) >= 0;
    }

    esp_http_client_handle_t client = nullptr;
};

const char *read_image(esp_http_client_handle_t client, const Build &build, Sink sink, void *context,
                       int64_t deadline) {
    const int declared = esp_http_client_get_content_length(client);
    const bool chunked = esp_http_client_is_chunked_response(client);
    if ((!chunked && (declared < 0 || uint32_t(declared) != build.size)) ||
        (chunked && declared > 0 && uint32_t(declared) != build.size)) return "Firmware size does not match the release.";

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    const char *error = mbedtls_sha256_starts_ret(&sha, 0) ? "Could not verify the firmware checksum." : nullptr;
    size_t received = 0;
    uint8_t buffer[1024];
    while (!error) {
        if (esp_timer_get_time() >= deadline) {
            error = "Firmware download timed out.";
            break;
        }
        const int count = esp_http_client_read(client, reinterpret_cast<char *>(buffer), sizeof buffer);
        if (!count && esp_http_client_is_complete_data_received(client)) break;
        if (count <= 0) {
            error = "Firmware download was interrupted.";
        } else if (size_t(count) > build.size - received) {
            error = "Firmware size does not match the release.";
        } else if (mbedtls_sha256_update_ret(&sha, buffer, count)) {
            error = "Could not verify the firmware checksum.";
        } else {
            received += count;
            error = sink(buffer, count, context);
        }
    }
    uint8_t actual[32];
    if (!error && received != build.size) error = "Firmware size does not match the release.";
    if (!error && (mbedtls_sha256_finish_ret(&sha, actual) || memcmp(actual, build.sha256, sizeof actual)))
        error = "Firmware checksum does not match the release.";
    mbedtls_sha256_free(&sha);
    return error;
}
}

uint32_t tag_version(const char *tag) {
    constexpr char prefix[] = "firmware-";
    if (!tag || strncmp(tag, prefix, sizeof prefix - 1)) return 0;
    const char *date = tag + sizeof prefix - 1;
    uint32_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (!digit(date[i])) return 0;
        value = value * 10 + uint32_t(date[i] - '0');
    }
    if (date[8] != '.' || !digit(date[9]) || date[9] == '0') return 0;
    uint32_t sequence = date[9] - '0';
    if (date[10]) {
        if (!digit(date[10]) || date[11]) return 0;
        sequence = sequence * 10 + uint32_t(date[10] - '0');
    }
    const unsigned year = value / 10000, month = value / 100 % 100, day = value % 100;
    constexpr uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 ||
        day > unsigned(month_days[month - 1]) + (month == 2 && year % 4 == 0)) return 0;
    return value * 100 + sequence;
}

const char *check(uint32_t running, size_t capacity, bool site, Build &out) {
    out = Build{};
    if (!running || capacity < firmware_ota::PREFIX_BYTES) return "This build cannot check for a GitHub update.";
    if (time(nullptr) < VALID_CLOCK) return "Wait for the clock to sync before connecting to GitHub.";
    const int64_t deadline = esp_timer_get_time() + CHECK_TIMEOUT_US;
    bool encoded = false;
    Http response;
    if (!response.open(RELEASES, true, encoded)) return "Could not connect securely to GitHub.";
    const int code = esp_http_client_get_status_code(response.client);
    if (code == 403 || code == 429) return "GitHub is limiting requests. Try again later.";
    if (code != 200 || encoded) return "GitHub did not return a release list.";
    const int declared = esp_http_client_get_content_length(response.client);
    if (declared > int(LIST_BYTES)) return "The GitHub release list is too large.";

    std::unique_ptr<char[]> item(new (std::nothrow) char[ITEM_BYTES]);
    if (!item) return "Not enough memory to check GitHub Releases.";
    Build best;
    ReleaseList list;
    char buffer[1024];
    size_t received = 0;
    for (;;) {
        if (esp_timer_get_time() >= deadline) return "Checking GitHub Releases timed out.";
        const int count = esp_http_client_read(response.client, buffer, sizeof buffer);
        if (!count && esp_http_client_is_complete_data_received(response.client)) break;
        if (count <= 0) return "The GitHub release list was interrupted.";
        if (size_t(count) > LIST_BYTES - received) return "The GitHub release list is too large.";
        received += count;
        for (int i = 0; i < count; ++i) {
            if (!list.append(buffer[i], item.get(), running, capacity, site, best))
                return "The GitHub release list has an invalid or oversized entry.";
        }
    }
    if (list.position != ReleaseList::Position::END) return "The GitHub release list was incomplete.";
    out = best;
    return nullptr;
}

const char *download(const Build &build, bool site, Sink sink, void *context) {
    if (!sink || !memchr(build.tag, 0, sizeof build.tag) || !build.version ||
        tag_version(build.tag) != build.version || build.size < firmware_ota::PREFIX_BYTES)
        return "Invalid GitHub firmware selection.";
    if (time(nullptr) < VALID_CLOCK) return "Wait for the clock to sync before downloading firmware.";
    const int64_t deadline = esp_timer_get_time() + DOWNLOAD_TIMEOUT_US;
    char url[128];
    snprintf(url, sizeof url, "%s%s/%s", FIRMWARE, build.tag, asset_name(site));
    bool encoded = false;
    Http response;
    if (!response.open(url, false, encoded)) return "Could not connect securely to the brain_zero firmware server.";
    if (esp_http_client_get_status_code(response.client) != 200 || encoded)
        return "The brain_zero server did not return a firmware image.";
    return read_image(response.client, build, sink, context, deadline);
}
}
