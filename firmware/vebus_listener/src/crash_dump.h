#pragma once

class WebServer;

namespace crash_dump {
// Streams the existing flash core dump without modifying it. The caller must
// authenticate the request and require the firmware-page nonce first: task
// stacks in a core dump can contain credentials and other private data.
void download(WebServer &server);
}
