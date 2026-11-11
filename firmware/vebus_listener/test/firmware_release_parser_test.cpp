// Host regression for the streaming release checker used while TLS is active.
#include "firmware_release_parser.h"
#include <cassert>
#include <cstdio>
#include <string>

namespace {
using firmware_release::Build;
constexpr uint32_t running = 2026111003;
constexpr size_t capacity = 3342336;
const char digest[] = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const char base[] = "https://github.com/mjpalmowski/brain_zero/releases/download/";

struct Input {
    const std::string &data;
    size_t position;
    int end;
};

int next(void *context) {
    auto &input = *static_cast<Input *>(context);
    if (input.position == input.data.size()) return input.end;
    return static_cast<unsigned char>(input.data[input.position++]);
}

bool parse(const std::string &body, Build &build, bool site = true, int end = -1) {
    Input input{body, 0, end};
    return firmware_release::parse_release_list(next, &input, running, capacity, site, build);
}

std::string asset(const std::string &tag, const std::string &name,
                  const std::string &size = "2760400", const std::string &hash = digest,
                  const std::string &encoded_name = "") {
    return "{\"digest\":\"" + hash + "\",\"browser_download_url\":\"" + base + tag + "/" + name +
           "\",\"size\":" + size + ",\"state\":\"uploaded\",\"name\":\"" +
           (encoded_name.empty() ? name : encoded_name) + "\"}";
}

std::string release(const std::string &tag, const std::string &assets, bool preview = false, bool draft = false,
                    const std::string &notes = "") {
    return "{\"assets\":[" + assets + "],\"body\":\"" + notes + "\",\"draft\":" + (draft ? "true" : "false") +
           ",\"prerelease\":" + (preview ? "true" : "false") + ",\"tag_name\":\"" + tag + "\"}";
}
}

int main() {
    const std::string old = "firmware-20261110.3";
    const std::string candidate = "firmware-20261110.4";
    const std::string newer = "firmware-20261110.5";
    const std::string site_file = "firmware-site.bin";
    const std::string public_file = "firmware.bin";
    Build build;
    const auto valid_site = asset(candidate, site_file, "2760400", digest, R"(firmware\u002dsite.bin)");
    const auto valid_public = asset(newer, public_file);
    // A 50 KB release note previously overflowed the 24 KB buffer. Its
    // bracket and quote escapes must not interfere with the next release.
    const auto notes = std::string(50000, 'z') + "\\\"}]\\\\ \\ud83d\\udc99";
    const auto list = "[" + release(old, asset(old, site_file)) + "," +
                      release(newer, valid_public) + "," + release(candidate, valid_site, true, false, notes) + "]";
    assert(parse(list, build));
    assert(build.version == 2026111004 && build.size == 2760400 && build.preview && build.sha256[0] == 0x01);
    assert(parse(list, build, false) && build.version == 2026111005 && !build.preview);
    assert(parse("[" + release(newer, valid_public) + "]", build) && !build.tag[0]);
    assert(parse("[" + release(candidate, valid_site, false, true) + "]", build) && !build.tag[0]);
    assert(parse("[" + release(candidate, valid_site + "," + valid_site) + "]", build) && !build.tag[0]);
    assert(parse("[" + release(candidate, asset(old, site_file)) + "]", build) && !build.tag[0]);
    assert(parse("[" + release(candidate, asset(candidate, site_file, "3342337")) + "]", build) && !build.tag[0]);
    assert(parse("[" + release(candidate, asset(candidate, site_file, "2760400.1")) + "]", build) && !build.tag[0]);
    assert(parse("[" + release(candidate, asset(candidate, site_file, "2760400", "sha256:abcd")) + "]", build)
           && !build.tag[0]);
    assert(parse(R"([{"unknown":{"data":[null,true,-1.5e2,"escaped \" character"]}}])", build) && !build.tag[0]);
    assert(!parse(list.substr(0, list.size() - 1), build));
    assert(!parse(list, build, true, -2));
    assert(!parse(R"([{"unknown":[1,]}])", build));
    assert(!parse(R"([{"unknown":01}])", build));
    assert(!parse("[{},]", build));
    assert(!parse("[] false", build));
    std::puts("Streaming release parser tests passed.");
}
