#include "firmware_release.h"
#include "firmware_release_parser.h"
#include "firmware_ota.h"

#include <esp_http_client.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
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
constexpr char FIRMWARE[] = "https://brain-zero.com/firmware/";
constexpr size_t LIST_BYTES = 10 * (24 * 1024 + 16);
constexpr int NETWORK_TIMEOUT_MS = 6000;
constexpr int64_t CHECK_TIMEOUT_US = 30 * 1000000LL;
constexpr int64_t DOWNLOAD_TIMEOUT_US = 120 * 1000000LL;
constexpr time_t VALID_CLOCK = 1704067200; // 2024-01-01 UTC

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

struct ReleaseReader {
    ReleaseReader(esp_http_client_handle_t client, int64_t deadline) : client(client), deadline(deadline) {}

    esp_http_client_handle_t client;
    int64_t deadline;
    char buffer[1024];
    size_t read = 0, end = 0, received = 0;
    const char *error = nullptr;

    static int next(void *context) {
        return static_cast<ReleaseReader *>(context)->next_byte();
    }

    int next_byte() {
        if (read == end) {
            if (esp_timer_get_time() >= deadline) {
                error = "Checking GitHub Releases timed out.";
                return -2;
            }
            const int count = esp_http_client_read(client, buffer, sizeof buffer);
            if (!count && esp_http_client_is_complete_data_received(client)) return -1;
            if (count <= 0) {
                error = "The GitHub release list was interrupted.";
                return -2;
            }
            if (size_t(count) > LIST_BYTES - received) {
                error = "The GitHub release list is too large.";
                return -2;
            }
            received += count;
            read = 0;
            end = count;
        }
        return static_cast<unsigned char>(buffer[read++]);
    }
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

const char *check(uint32_t running, size_t capacity, Build &out) {
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

    ReleaseReader reader(response.client, deadline);
    if (!parse_release_list(ReleaseReader::next, &reader, running, capacity, out))
        return reader.error ? reader.error : "The GitHub release list was invalid or incomplete.";
    return nullptr;
}

const char *download(const Build &build, Sink sink, void *context) {
    if (!sink || !memchr(build.tag, 0, sizeof build.tag) || !build.version ||
        tag_version(build.tag) != build.version || build.size < firmware_ota::PREFIX_BYTES)
        return "Invalid GitHub firmware selection.";
    if (time(nullptr) < VALID_CLOCK) return "Wait for the clock to sync before downloading firmware.";
    const int64_t deadline = esp_timer_get_time() + DOWNLOAD_TIMEOUT_US;
    char url[128];
    snprintf(url, sizeof url, "%s%s/%s", FIRMWARE, build.tag, asset_name());
    bool encoded = false;
    Http response;
    if (!response.open(url, false, encoded)) return "Could not connect securely to the brain_zero firmware server.";
    if (esp_http_client_get_status_code(response.client) != 200 || encoded)
        return "The brain_zero server did not return a firmware image.";
    return read_image(response.client, build, sink, context, deadline);
}
}
