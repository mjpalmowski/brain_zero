#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pace_can_ble_protocol {
constexpr char SERVICE_UUID[] = "e6c00001-b5a3-4f4f-9b8d-73d619b32720";
constexpr char METADATA_UUID[] = "e6c00002-b5a3-4f4f-9b8d-73d619b32720";
constexpr char FRAMES_UUID[] = "e6c00003-b5a3-4f4f-9b8d-73d619b32720";
constexpr uint8_t VERSION = 1;
constexpr size_t RECORD_SIZE = 20;
constexpr uint16_t IDS[] = {0x351, 0x355, 0x356, 0x359, 0x35c, 0x35e, 0x373};
constexpr size_t SLOT_COUNT = sizeof IDS / sizeof *IDS;
using Record = std::array<uint8_t, RECORD_SIZE>;

inline void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}
inline int slot(uint32_t id) {
  for (size_t i = 0; i < SLOT_COUNT; ++i) if (id == IDS[i]) return int(i);
  return -1;
}

// ESPHome exposes a CAN receive callback, not the ISR receive time. If the main
// loop stalled, discard both pending samples and the old driver queue while it
// drains. Calling observe() from BOTH loop and receive avoids ordering holes.
class StallGuard {
 public:
  void begin(uint32_t now) { last_loop_ = now; quarantined_ = false; }
  bool observe(uint32_t now) {
    if (uint32_t(now - last_loop_) > 250) {
      quarantined_ = true;
      quarantine_start_ = now;
    }
    if (quarantined_ && uint32_t(now - quarantine_start_) >= 500) quarantined_ = false;
    return !quarantined_;
  }
  bool loop(uint32_t now) {
    const bool usable = observe(now);
    last_loop_ = now;
    return usable;
  }
 private:
  uint32_t last_loop_ = 0, quarantine_start_ = 0;
  bool quarantined_ = false;
};
inline Record metadata(uint32_t boot, uint32_t now_ms, uint32_t received, uint32_t dropped) {
  Record out{};
  memcpy(out.data(), "BZCB", 4);
  out[4] = VERSION;
  out[5] = 1;  // Read-only application transport. There is no CAN transmit endpoint.
  put32(&out[6], boot);
  put32(&out[10], now_ms);
  put32(&out[14], received);
  put16(&out[18], uint16_t(dropped > 65535 ? 65535 : dropped));
  return out;
}

// The application retains at most one pending original receive per known CAN ID.
// Newer receives supersede unsent older values. No timer manufactures samples.
class Queue {
 public:
  explicit Queue(uint32_t initial_sequence = 0) : sequence_(initial_sequence) {}
  bool receive(uint32_t id, bool extended, bool rtr, const uint8_t *data,
               size_t length, uint32_t received_ms, bool enqueue) {
    const int index = slot(id);
    if (index < 0 || extended || rtr || length > 8 || (!data && length)) return false;
    ++received;
    ++sequence_;
    if (!enqueue) return true;
    Slot &s = slots_[index];
    if (s.pending) ++dropped;
    s.record = {};
    s.record[0] = VERSION;
    s.record[1] = uint8_t(length);
    put16(&s.record[2], uint16_t(id));
    put32(&s.record[4], sequence_);
    put32(&s.record[8], received_ms);
    if (length) memcpy(&s.record[12], data, length);
    s.pending = true;
    s.sequence = sequence_;
    return true;
  }
  bool pop(Record &out) {
    int oldest = -1;
    for (size_t i = 0; i < SLOT_COUNT; ++i) {
      if (slots_[i].pending && (oldest < 0 || int32_t(slots_[i].sequence - slots_[oldest].sequence) < 0))
        oldest = int(i);
    }
    if (oldest < 0) return false;
    out = slots_[oldest].record;
    slots_[oldest].pending = false;
    return true;
  }
  void clear() { for (auto &s : slots_) s.pending = false; }
  uint32_t received = 0;
  uint32_t dropped = 0;

 private:
  struct Slot { Record record{}; uint32_t sequence = 0; bool pending = false; };
  Slot slots_[SLOT_COUNT]{};
  uint32_t sequence_ = 0;
};
}  // namespace pace_can_ble_protocol
