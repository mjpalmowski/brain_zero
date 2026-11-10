#pragma once

#include <stddef.h>

class WebServer;

namespace mcp {

enum class Tool { POWER_FLOW, ESS, DIAGNOSTICS, WARNING_HISTORY };
using ReadSnapshot = bool (*)(Tool, char *, size_t);
using AllowRequest = bool (*)();

void register_route(WebServer &server, ReadSnapshot read, char *snapshot, size_t snapshot_size, AllowRequest allow = nullptr);

} // namespace mcp
