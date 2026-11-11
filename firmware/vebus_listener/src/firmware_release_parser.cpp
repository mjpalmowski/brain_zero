#include "firmware_release_parser.h"
#include "firmware_ota.h"

#include <stdio.h>
#include <string.h>

namespace firmware_release {
namespace {

constexpr char RELEASE_ASSETS[] = "https://github.com/mjpalmowski/brain_zero/releases/download/";
constexpr unsigned MAX_DEPTH = 16;

bool digit(int c) { return c >= '0' && c <= '9'; }

int hex_digit(int c) {
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_digest(const char *digest, uint8_t (&out)[32]) {
    if (strlen(digest) != 71 || strncmp(digest, "sha256:", 7)) return false;
    for (size_t i = 0; i < sizeof out; ++i) {
        const int high = hex_digit(digest[7 + 2 * i]);
        const int low = hex_digit(digest[8 + 2 * i]);
        if (high < 0 || low < 0) return false;
        out[i] = uint8_t(high * 16 + low);
    }
    return true;
}

// Convert an exact nonnegative integer without floating point or overflow.
uint32_t integer_size(const char *number) {
    if (*number == '-') return 0;
    char digits[40];
    int count = 0, shift = 0;
    bool fraction = false;
    while (*number && *number != 'e' && *number != 'E') {
        if (*number == '.') {
            fraction = true;
        } else {
            if (count || *number != '0') digits[count++] = *number;
            if (fraction) --shift;
        }
        ++number;
    }
    if (!count) return 0;
    if (*number) {
        ++number;
        const bool negative = *number == '-';
        if (*number == '-' || *number == '+') ++number;
        int exponent = 0;
        while (*number) {
            exponent = exponent * 10 + *number++ - '0';
            if (exponent > 40) return 0;
        }
        shift += negative ? -exponent : exponent;
    }
    while (digits[count - 1] == '0') {
        --count;
        ++shift;
    }
    if (shift < 0 || count + shift > 10) return 0;
    uint64_t value = 0;
    for (int i = 0; i < count; ++i) value = 10 * value + (digits[i] - '0');
    while (shift-- > 0) value *= 10;
    return value <= UINT32_MAX ? uint32_t(value) : 0;
}

struct Asset {
    char url[128] = {};
    uint8_t sha256[32] = {};
    uint32_t size = 0;
    unsigned seen = 0;
    bool matched = false, uploaded = false, has_digest = false, invalid = false;
};

struct Release {
    char tag[sizeof(Build::tag)] = {};
    Asset asset;
    unsigned seen = 0;
    bool invalid = false, draft = true, preview = false;
};

// The HTTP reader buffers the stream. Keep only known fields for the current
// release/asset and validate the remaining JSON without storing it.
class Parser {
public:
    Parser(int (*next)(void *), void *context, uint32_t running, size_t capacity, bool site)
        : next_(next), context_(context), running_(running), capacity_(capacity), name_(asset_name(site)) {
        read();
    }

    bool parse(Build &out) {
        space();
        if (!array(1, true)) return false;
        space();
        if (ch_ != -1) return false;
        out = best_;
        return true;
    }

private:
    int (*next_)(void *);
    void *context_;
    uint32_t running_;
    size_t capacity_;
    const char *name_;
    int ch_;
    Build best_;

    void read() { ch_ = next_(context_); }

    bool take(int expected) {
        if (ch_ != expected) return false;
        read();
        return true;
    }

    void space() {
        while (ch_ == ' ' || ch_ == '\r' || ch_ == '\n' || ch_ == '\t') read();
    }

    bool literal(const char *expected) {
        while (*expected) if (!take(*expected++)) return false;
        return true;
    }

    bool unicode(int &unit) {
        unit = 0;
        for (int i = 0; i < 4; ++i) {
            const int hex = hex_digit(ch_);
            if (hex < 0) return false;
            unit = 16 * unit + hex;
            read();
        }
        return true;
    }

    bool escape(int &value) {
        if (ch_ < 0) return false;
        value = ch_;
        read();
        switch (value) {
            case '"': case '\\': case '/': return true;
            case 'b': value = '\b'; return true;
            case 'f': value = '\f'; return true;
            case 'n': value = '\n'; return true;
            case 'r': value = '\r'; return true;
            case 't': value = '\t'; return true;
            case 'u': {
                if (!unicode(value)) return false;
                if (value >= 0xd800 && value <= 0xdbff) {
                    int low;
                    if (!take('\\') || !take('u') || !unicode(low) || low < 0xdc00 || low > 0xdfff)
                        return false;
                    value = 0x80;
                } else if (value >= 0xdc00 && value <= 0xdfff) {
                    return false;
                }
                // All recognized keys and values are ASCII. A non-ASCII
                // escape must never alias an ASCII key or terminate a string.
                if (value == 0 || value > 0x7f) value = 0x80;
                return true;
            }
            default: return false;
        }
    }

    bool string(char *out = nullptr, size_t capacity = 0) {
        if (!take('"')) return false;
        size_t used = 0;
        bool overflow = false;
        while (ch_ != '"') {
            if (ch_ < 0x20) return false;
            int value = ch_;
            read();
            if (value == '\\' && !escape(value)) return false;
            if (out) {
                if (used + 1 < capacity) out[used++] = char(value);
                else overflow = true;
            }
        }
        if (out) out[overflow ? 0 : used] = 0;
        return take('"');
    }

    bool number(char *out = nullptr, size_t capacity = 0) {
        size_t used = 0;
        bool overflow = false;
        auto consume = [&]() {
            if (out) {
                if (used + 1 < capacity) out[used++] = char(ch_);
                else overflow = true;
            }
            read();
        };
        if (ch_ == '-') consume();
        if (!digit(ch_)) return false;
        if (ch_ == '0') consume();
        else while (digit(ch_)) consume();
        if (ch_ == '.') {
            consume();
            if (!digit(ch_)) return false;
            while (digit(ch_)) consume();
        }
        if (ch_ == 'e' || ch_ == 'E') {
            consume();
            if (ch_ == '-' || ch_ == '+') consume();
            if (!digit(ch_)) return false;
            while (digit(ch_)) consume();
        }
        if (out) out[overflow ? 0 : used] = 0;
        return true;
    }

    bool skip(unsigned depth) {
        switch (ch_) {
            case '{': return object(depth);
            case '[': return array(depth, false);
            case '"': return string();
            case 't': return literal("true");
            case 'f': return literal("false");
            case 'n': return literal("null");
            default: return number();
        }
    }

    void seen(unsigned field, unsigned &fields, bool &invalid) {
        if (fields & field) invalid = true;
        fields |= field;
    }

    // Keep the field buffers out of every recursive object frame.
    __attribute__((noinline)) bool asset_field(Asset &asset, const char *key, unsigned depth) {
        if (!strcmp(key, "name")) {
            seen(1, asset.seen, asset.invalid);
            char name[32];
            if (ch_ != '"') return skip(depth);
            if (!string(name, sizeof name)) return false;
            if (!strcmp(name, name_)) asset.matched = true;
        } else if (!strcmp(key, "state")) {
            seen(2, asset.seen, asset.invalid);
            char state[16];
            if (ch_ != '"') return skip(depth);
            if (!string(state, sizeof state)) return false;
            asset.uploaded = !strcmp(state, "uploaded");
        } else if (!strcmp(key, "browser_download_url")) {
            seen(4, asset.seen, asset.invalid);
            if (ch_ != '"') return skip(depth);
            return string(asset.url, sizeof asset.url);
        } else if (!strcmp(key, "size")) {
            seen(8, asset.seen, asset.invalid);
            char value[40];
            if (!digit(ch_) && ch_ != '-') return skip(depth);
            if (!number(value, sizeof value)) return false;
            asset.size = integer_size(value);
        } else if (!strcmp(key, "digest")) {
            seen(16, asset.seen, asset.invalid);
            char digest[72];
            if (ch_ != '"') return skip(depth);
            if (!string(digest, sizeof digest)) return false;
            asset.has_digest = parse_digest(digest, asset.sha256);
        } else {
            return skip(depth);
        }
        return true;
    }

    bool release_field(Release &release, const char *key, unsigned depth) {
        if (!strcmp(key, "tag_name")) {
            seen(1, release.seen, release.invalid);
            if (ch_ != '"') return skip(depth);
            return string(release.tag, sizeof release.tag);
        }
        if (!strcmp(key, "draft") || !strcmp(key, "prerelease")) {
            const bool draft = !strcmp(key, "draft");
            seen(draft ? 2 : 4, release.seen, release.invalid);
            if (ch_ != 't' && ch_ != 'f') {
                release.invalid = true;
                return skip(depth);
            }
            const bool value = ch_ == 't';
            if (draft) release.draft = value;
            else release.preview = value;
            return literal(value ? "true" : "false");
        }
        if (!strcmp(key, "assets")) {
            seen(8, release.seen, release.invalid);
            if (ch_ != '[') return skip(depth);
            return array(depth, false, &release);
        }
        return skip(depth);
    }

    bool object(unsigned depth, Release *release = nullptr, Asset *asset = nullptr) {
        if (depth > MAX_DEPTH || !take('{')) return false;
        space();
        if (take('}')) return true;
        for (;;) {
            char key[24];
            if (!string(release || asset ? key : nullptr, sizeof key)) return false;
            space();
            if (!take(':')) return false;
            space();
            if (release) {
                if (!release_field(*release, key, depth + 1)) return false;
            } else if (asset) {
                if (!asset_field(*asset, key, depth + 1)) return false;
            } else if (!skip(depth + 1)) {
                return false;
            }
            space();
            if (take('}')) return true;
            if (!take(',')) return false;
            space();
        }
    }

    bool release_entry(unsigned depth) {
        Release release;
        if (!object(depth, &release)) return false;
        const auto version = tag_version(release.tag);
        const auto &asset = release.asset;
        if (release.invalid || release.draft || !(release.seen & 4) || version <= running_ ||
            version <= best_.version || !asset.matched || asset.invalid || !asset.uploaded ||
            !asset.has_digest || asset.size < firmware_ota::PREFIX_BYTES || asset.size > capacity_)
            return true;
        char url[sizeof asset.url];
        snprintf(url, sizeof url, "%s%s/%s", RELEASE_ASSETS, release.tag, name_);
        if (strcmp(url, asset.url)) return true;
        memcpy(best_.tag, release.tag, sizeof best_.tag);
        best_.version = version;
        best_.size = asset.size;
        memcpy(best_.sha256, asset.sha256, sizeof best_.sha256);
        best_.preview = release.preview;
        return true;
    }

    // Only the assets array needs an Asset buffer.
    __attribute__((noinline)) bool asset_entry(unsigned depth, Release &release) {
        Asset asset;
        if (!object(depth, nullptr, &asset)) return false;
        if (asset.matched) {
            if (release.asset.matched) release.invalid = true;
            else release.asset = asset;
        }
        return true;
    }

    bool array(unsigned depth, bool root, Release *release = nullptr) {
        if (depth > MAX_DEPTH || !take('[')) return false;
        space();
        if (take(']')) return true;
        for (;;) {
            if (root) {
                if (!release_entry(depth + 1)) return false;
            } else if (release && ch_ == '{') {
                if (!asset_entry(depth + 1, *release)) return false;
            } else if (!skip(depth + 1)) {
                return false;
            }
            space();
            if (take(']')) return true;
            if (!take(',')) return false;
            space();
        }
    }
};

}  // namespace

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

bool parse_release_list(int (*next)(void *), void *context, uint32_t running, size_t capacity,
                        bool site, Build &out) {
    out = {};
    return Parser(next, context, running, capacity, site).parse(out);
}

}  // namespace firmware_release
