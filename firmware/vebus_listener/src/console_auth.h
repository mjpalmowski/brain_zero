#pragma once
#include <Arduino.h>

class Preferences;

namespace console_auth {
enum class Access { NONE, SESSION, API };
void begin(Preferences &store);
bool enabled();
bool has_token();
bool throttled(uint32_t now);
bool check_password(const String &password, uint32_t now);
bool set_password(const String &password);
bool reset();
bool rotate_token(char (&token)[65]);
Access access(const String &cookie, const String &authorization, uint32_t now);
String sign_in(uint32_t now);
void sign_out(const String &cookie, uint32_t now);
}
