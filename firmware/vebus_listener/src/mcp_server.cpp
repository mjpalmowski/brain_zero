#include "mcp_server.h"

#include <WebServer.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <cmath>
#include <memory>

#include "diagnostics.h"
#include "web.h"

namespace mcp {
namespace {

constexpr size_t MAX_REQUEST = 2048;
constexpr char MODERN[] = "2026-07-28";
constexpr char LEGACY[] = "2025-11-25";
constexpr char VERSIONS[] = R"(["2026-07-28","2025-11-25","2025-06-18","2025-03-26"])";
constexpr char INSTRUCTIONS[] = "Read the local brain_zero MultiPlus controller. Tools return cached data. "
    "Check data age and availability before drawing conclusions. Null means unknown or unavailable.";

struct Definition {
    Tool tool;
    const char *name;
    const char *description;
};

constexpr Definition TOOLS[] = {
    {Tool::DIAGNOSTICS, "get_diagnostics", "Read controller health, Wi-Fi signal, current warnings and the cached VE.Bus fault report. Report ages describe stored observations."},
    {Tool::ESS, "get_ess", "Read ESS mode, current decision, grid setpoint, power limits and the MultiPlus charge schedule."},
    {Tool::POWER_FLOW, "get_power_flow", "Read AC input, AC output and battery measurements. Positive AC input watts mean import. Positive battery watts or amps mean charging. AC output is net at the MultiPlus output, which can include backfeeding solar. Measurements have units and ages."},
    {Tool::WARNING_HISTORY, "get_warning_history", "Read recorded warnings and alarms, newest first, with appearance and clear timestamps. Open entries have no clear timestamp."},
};

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using Encoded = std::unique_ptr<char, decltype(&cJSON_free)>;

const cJSON *field(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

const char *string(const cJSON *value) {
    return cJSON_IsString(value) ? value->valuestring : "";
}

bool legacy(const char *version) {
    return !strcmp(version, LEGACY) || !strcmp(version, "2025-06-18") || !strcmp(version, "2025-03-26");
}

// Bound the parser's stack and node allocations before cJSON sees the request.
bool bounded_json(const String &body) {
    unsigned depth = 0, tokens = 0;
    bool quoted = false;
    for (size_t i = 0; i < body.length(); ++i) {
        char c = body[i];
        if (quoted) {
            if (c == '"') quoted = false;
            else if (c == '\\' && ++i < body.length() && !strncmp(body.c_str() + i, "u0000", 5)) return false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 12 || ++tokens > 96) return false;
        } else if (c == '}' || c == ']') {
            if (!depth) return false;
            --depth;
        } else if ((c == ',' || c == ':') && ++tokens > 96) {
            return false;
        }
    }
    return !quoted && !depth;
}

bool local_request(WebServer &server) {
    String host = server.header("Host");
    if (host.endsWith(":80")) host.remove(host.length() - 3);
    if (!web::local_host(host)) return false;
    const String origin = server.header("Origin");
    const String local = String("http://") + host;
    return origin.isEmpty() || origin.equalsIgnoreCase(local) || origin.equalsIgnoreCase(local + ":80");
}

bool name_matches(const String &header, const char *name) {
    if (!header.startsWith("=?base64?")) return header == name && !header.isEmpty();
    if (!header.endsWith("?=") || header.length() < 11) return false;
    unsigned char decoded[128];
    size_t length = 0;
    const auto *input = reinterpret_cast<const unsigned char *>(header.c_str() + 9);
    return mbedtls_base64_decode(decoded, sizeof decoded, &length, input, header.length() - 11) == 0 &&
           length == strlen(name) && !memcmp(decoded, name, length);
}

void error(WebServer &server, int status, const char *id, int code, const char *message, const char *data = nullptr) {
    String response = String("{\"jsonrpc\":\"2.0\",\"id\":") + id + ",\"error\":{\"code\":" + code +
                      ",\"message\":\"" + message + "\"";
    if (data) response += String(",\"data\":") + data;
    response += "}}";
    server.send(status, "application/json", response);
}

String server_info() {
    return String(R"({"name":"brain-zero","title":"brain_zero S3","version":")") + diagnostics::BUILD + "\"}";
}

String tools_list() {
    String list;
    if (!list.reserve(2600)) return list;
    list = "[";
    for (const auto &definition : TOOLS) {
        if (list.length() > 1) list += ',';
        list += "{\"name\":\"";
        list += definition.name;
        list += "\",\"description\":\"";
        list += definition.description;
        list += R"(","inputSchema":{"type":"object","properties":{},"additionalProperties":false},)"
                R"("annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}})";
    }
    list += ']';
    return list;
}

// Write large cached results in pieces, without a second full-size response buffer.
void result(WebServer &server, const char *id, const String &fields, bool modern,
            const char *text = nullptr, const char *snapshot = nullptr) {
    String start = String("{\"jsonrpc\":\"2.0\",\"id\":") + id + ",\"result\":{";
    if (modern) {
        start += String(R"("resultType":"complete","_meta":{"io.modelcontextprotocol/serverInfo":)") +
                 server_info() + "},";
    }
    start += fields;
    const char *content_start = R"("content":[{"type":"text","text":)";
    const char *content_end = snapshot ? R"(}],"structuredContent":)" : "}]";
    size_t length = start.length() + 2;
    if (text) length += strlen(content_start) + strlen(text) + strlen(content_end);
    if (snapshot) length += strlen(snapshot);
    server.setContentLength(length);
    server.send(200, "application/json", "");
    server.sendContent(start);
    if (text) {
        server.sendContent(content_start);
        server.sendContent(text, strlen(text));
        server.sendContent(content_end);
    }
    if (snapshot) server.sendContent(snapshot, strlen(snapshot));
    server.sendContent("}}", 2);
}

void tool_error(WebServer &server, const char *id, bool modern, const char *text) {
    result(server, id, "\"isError\":true,", modern, text);
}

void handle(WebServer &server, ReadSnapshot read, char *snapshot, size_t snapshot_size) {
    server.sendHeader("Cache-Control", "no-store");
    if (!local_request(server)) {
        server.send(403, "text/plain", "Use the controller's local address.");
        return;
    }
    if (server.method() != HTTP_POST) {
        server.sendHeader("Allow", "POST");
        server.send(405, "text/plain", "Use POST for MCP requests.");
        return;
    }
    String media = server.header("Content-Type");
    const int param = media.indexOf(';');
    if (param >= 0) media.remove(param);
    media.trim();
    if (!media.equalsIgnoreCase("application/json")) {
        server.send(415, "text/plain", "Use application/json for MCP requests.");
        return;
    }
    if (server.clientContentLength() > int(MAX_REQUEST)) {
        server.send(413, "text/plain", "MCP requests are limited to 2048 bytes.");
        return;
    }
    const String body = server.arg("plain");
    if (server.args() != 1 || server.clientContentLength() <= 0 ||
        size_t(server.clientContentLength()) != body.length() ||
        !bounded_json(body)) {
        error(server, 400, "null", -32700, "Invalid JSON body");
        return;
    }
    const Json request(cJSON_ParseWithLengthOpts(body.c_str(), body.length() + 1, nullptr, true), cJSON_Delete);
    if (!request) { error(server, 400, "null", -32700, "Parse error"); return; }
    const auto *request_id = field(request.get(), "id");
    const auto *params = field(request.get(), "params");
    const char *method = string(field(request.get(), "method"));
    const bool valid_id = !request_id || (cJSON_IsString(request_id) && strlen(request_id->valuestring) <= 128) ||
        (cJSON_IsNumber(request_id) && std::fabs(request_id->valuedouble) <= 9007199254740991.0 &&
         std::floor(request_id->valuedouble) == request_id->valuedouble);
    if (!cJSON_IsObject(request.get()) || strcmp(string(field(request.get(), "jsonrpc")), "2.0") ||
        !*method || !valid_id) {
        error(server, 400, "null", -32600, "Invalid request");
        return;
    }
    const Encoded encoded_id(request_id ? cJSON_PrintUnformatted(request_id) : nullptr, cJSON_free);
    const char *id = encoded_id ? encoded_id.get() : "null";
    if (params && !cJSON_IsObject(params)) { error(server, 400, id, -32602, "Params must be an object"); return; }

    const String version = server.header("MCP-Protocol-Version");
    const auto *meta = field(params, "_meta");
    const char *body_version = string(field(meta, "io.modelcontextprotocol/protocolVersion"));
    const bool modern = *body_version || (!version.isEmpty() && !legacy(version.c_str())) ||
                        !strcmp(method, "server/discover");
    if (modern) {
        if (!*body_version || !cJSON_IsObject(field(meta, "io.modelcontextprotocol/clientCapabilities"))) {
            error(server, 400, id, -32602, "Missing per-request protocol metadata");
            return;
        }
        if (version != body_version || server.header("Mcp-Method") != method) {
            error(server, 400, id, -32020, "MCP header mismatch");
            return;
        }
        if (version != MODERN) {
            const Json requested(cJSON_CreateString(body_version), cJSON_Delete);
            const Encoded value(cJSON_PrintUnformatted(requested.get()), cJSON_free);
            const String data = String("{\"supported\":") + VERSIONS + ",\"requested\":" +
                                (value ? value.get() : "null") + "}";
            error(server, 400, id, -32022, "Unsupported protocol version", data.c_str());
            return;
        }
    }

    if (!request_id) {
        const bool notification = !modern && (!strcmp(method, "notifications/initialized") ||
                                             !strcmp(method, "notifications/cancelled"));
        server.send(notification ? 202 : 400, "application/json", "");
        return;
    }
    if (!modern && !strcmp(method, "initialize")) {
        const char *requested = string(field(params, "protocolVersion"));
        if (!*requested) { error(server, 400, id, -32602, "Missing protocol version"); return; }
        result(server, id, String("\"protocolVersion\":\"") + (legacy(requested) ? requested : LEGACY) +
               "\",\"capabilities\":{\"tools\":{}},\"serverInfo\":" + server_info() +
               ",\"instructions\":\"" + INSTRUCTIONS + "\"", false);
    } else if (modern && !strcmp(method, "server/discover")) {
        result(server, id, String("\"supportedVersions\":") + VERSIONS +
               ",\"capabilities\":{\"tools\":{}},\"instructions\":\"" + INSTRUCTIONS + "\"", true);
    } else if (!modern && !strcmp(method, "ping")) {
        result(server, id, "", false);
    } else if (!strcmp(method, "tools/list")) {
        const String list = tools_list();
        if (list.isEmpty()) { error(server, 503, id, -32603, "Tool catalog unavailable"); return; }
        result(server, id, String("\"tools\":") + list, modern);
    } else if (!strcmp(method, "tools/call")) {
        const char *name = string(field(params, "name"));
        if (modern && !name_matches(server.header("Mcp-Name"), name)) {
            error(server, 400, id, -32020, "Mcp-Name does not match the tool name");
            return;
        }
        const auto *args = field(params, "arguments");
        if (args && (!cJSON_IsObject(args) || args->child)) {
            tool_error(server, id, modern, "\"This tool takes no arguments.\"");
            return;
        }
        for (const auto &definition : TOOLS) {
            if (strcmp(definition.name, name)) continue;
            if (!read(definition.tool, snapshot, snapshot_size)) {
                tool_error(server, id, modern, "\"The cached snapshot is unavailable.\"");
                return;
            }
            const Json value(cJSON_CreateStringReference(snapshot), cJSON_Delete);
            const Encoded quoted(cJSON_PrintUnformatted(value.get()), cJSON_free);
            if (!quoted) { error(server, 503, id, -32603, "Snapshot unavailable"); return; }
            result(server, id, "", modern, quoted.get(), snapshot);
            return;
        }
        error(server, modern ? 400 : 200, id, -32602, "Unknown tool");
    } else {
        error(server, modern ? 404 : 200, id, -32601, "Method not found");
    }
}

} // namespace

void register_route(WebServer &server, ReadSnapshot read, char *snapshot, size_t snapshot_size, AllowRequest allow) {
    server.on("/mcp", HTTP_ANY, [&server, read, snapshot, snapshot_size, allow] {
        if (!allow || allow()) handle(server, read, snapshot, snapshot_size);
    });
}

} // namespace mcp
