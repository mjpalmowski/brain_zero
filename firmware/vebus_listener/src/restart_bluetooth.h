#pragma once
#include <stdint.h>

namespace restart_bluetooth {
constexpr uint32_t CLOSE_TIMEOUT_MS = 15000;
enum class Result : uint8_t { WAIT, READY, FAILED };
struct Hooks {
    bool (*pause)();
    bool (*quiescent)();
    bool (*restore)();
};
// Pauses application clients without deinitializing the Bluetooth host. This
// gate must complete before an intentional restart begins Wi-Fi shutdown.
class Guard {
  public:
    Result poll(uint32_t now, bool allocated, const Hooks &);
    void cancel(const Hooks &);
    void retry_restore(const Hooks &);
    bool restoring() const { return restoring_; }
    bool paused() const { return paused_; }
  private:
    uint32_t started_at_ = 0;
    bool started_ = false, paused_ = false, restoring_ = false;
};
}
