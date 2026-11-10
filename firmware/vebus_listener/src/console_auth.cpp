#include "console_auth.h"

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace console_auth {
namespace {
constexpr char KEY[] = "console";
constexpr char COOKIE[] = "bz_session=";
constexpr uint32_t SESSION_MS = 12u * 60 * 60 * 1000;
constexpr uint32_t PASSWORD_ROUNDS = 60000;
struct Record {
    uint32_t version = 1;
    uint8_t salt[16] = {}, password[32] = {}, api_token[32] = {};
};
struct Session {
    char token[65] = {};
    uint32_t started = 0;
};
Preferences *prefs = nullptr;
Record record;
Session sessions[4];
uint32_t last_failure = 0;
unsigned failures = 0;

bool any(const uint8_t *value, size_t len) {
    uint8_t out = 0;
    for (size_t i = 0; i < len; ++i) out |= value[i];
    return out != 0;
}
bool equal(const void *a, const void *b, size_t len) {
    const auto *left = static_cast<const uint8_t *>(a), *right = static_cast<const uint8_t *>(b);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= left[i] ^ right[i];
    return diff == 0;
}
bool save(const Record &next) {
    if (!prefs || prefs->putBytes(KEY, &next, sizeof next) != sizeof next) return false;
    record = next;
    return true;
}
void clear_sessions() {
    for (auto &s : sessions) s = Session{};
}
bool password_hash(const String &password, const uint8_t (&salt)[16], uint8_t (&hash)[32]) {
    if (password.length() < 12 || password.length() > 64 || strlen(password.c_str()) != password.length()) return false;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const auto *sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const int ok = mbedtls_md_setup(&ctx, sha, 1) == 0 &&
        mbedtls_pkcs5_pbkdf2_hmac(&ctx, reinterpret_cast<const uint8_t *>(password.c_str()),
            password.length(), salt, sizeof salt, PASSWORD_ROUNDS, sizeof hash, hash) == 0;
    mbedtls_md_free(&ctx);
    return ok;
}
void random_token(char (&out)[65]) {
    uint8_t bytes[32];
    esp_fill_random(bytes, sizeof bytes);
    for (size_t i = 0; i < sizeof bytes; ++i) snprintf(out + i * 2, 3, "%02x", unsigned(bytes[i]));
}
bool valid(const Session &s, uint32_t now) {
    return s.token[0] && now - s.started < SESSION_MS;
}
Session *find(const String &cookie, uint32_t now) {
    const char *p = cookie.c_str();
    while (*p) {
        while (*p == ' ' || *p == ';') ++p;
        const char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        if (end - p == int(sizeof COOKIE - 1 + 64) && !strncmp(p, COOKIE, sizeof COOKIE - 1)) {
            for (auto &s : sessions)
                if (valid(s, now) && equal(s.token, p + sizeof COOKIE - 1, 64)) return &s;
        }
        p = end;
    }
    return nullptr;
}
}

void begin(Preferences &store) {
    prefs = &store;
    Record loaded;
    if (store.getBytesLength(KEY) == sizeof loaded && store.getBytes(KEY, &loaded, sizeof loaded) == sizeof loaded &&
        loaded.version == record.version) record = loaded;
}
bool enabled() { return any(record.password, sizeof record.password); }
bool has_token() { return any(record.api_token, sizeof record.api_token); }
bool throttled(uint32_t now) {
    return failures && now - last_failure < (failures >= 5 ? 30000u : 1000u);
}
bool check_password(const String &password, uint32_t now) {
    if (!enabled()) return true;
    if (throttled(now)) return false;
    uint8_t hash[32];
    if (password_hash(password, record.salt, hash) && equal(hash, record.password, sizeof hash)) {
        failures = 0;
        return true;
    }
    if (now - last_failure >= 120000) failures = 0;
    last_failure = now;
    if (failures < 5) ++failures;
    return false;
}
bool set_password(const String &password) {
    Record next = record;
    esp_fill_random(next.salt, sizeof next.salt);
    if (!password_hash(password, next.salt, next.password) || !save(next)) return false;
    clear_sessions();
    failures = 0;
    return true;
}
bool reset() {
    if (!save(Record{})) return false;
    clear_sessions();
    failures = 0;
    return true;
}
bool rotate_token(char (&token)[65]) {
    Record next = record;
    random_token(token);
    return mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        reinterpret_cast<const uint8_t *>(token), 64, next.api_token) == 0 && save(next);
}
Access access(const String &cookie, const String &authorization, uint32_t now) {
    if (find(cookie, now)) return Access::SESSION;
    if (has_token() && authorization.startsWith("Bearer ") && authorization.length() == 71) {
        uint8_t hash[32];
        if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
            reinterpret_cast<const uint8_t *>(authorization.c_str() + 7), 64, hash) == 0 &&
            equal(hash, record.api_token, sizeof hash)) return Access::API;
    }
    return Access::NONE;
}
String sign_in(uint32_t now) {
    auto *slot = &sessions[0];
    for (auto &s : sessions) {
        if (!valid(s, now)) { slot = &s; break; }
        if (now - s.started > now - slot->started) slot = &s;
    }
    random_token(slot->token);
    slot->started = now;
    return String(COOKIE) + slot->token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200";
}
void sign_out(const String &cookie, uint32_t now) {
    if (auto *s = find(cookie, now)) *s = Session{};
}
}
