#include "../../esphome/components/pace_can_ble/protocol.h"
#include <cassert>
#include <cstdio>

using namespace pace_can_ble_protocol;
static uint32_t get32(const Record &r, size_t p) {
  return uint32_t(r[p]) | uint32_t(r[p + 1]) << 8 | uint32_t(r[p + 2]) << 16 | uint32_t(r[p + 3]) << 24;
}

int main() {
  Queue q;
  const uint8_t p[] = {0x82, 0x14, 0x8d, 0xff, 0x24, 0x01};
  assert(!q.receive(0x305, false, false, p, 6, 100, true));
  assert(!q.receive(0x356, true, false, p, 6, 100, true));
  assert(!q.receive(0x356, false, true, p, 6, 100, true));
  assert(!q.receive(0x356, false, false, p, 9, 100, true));
  assert(!q.receive(0x356, false, false, nullptr, 6, 100, true));
  assert(q.received == 0);
  assert(q.receive(0x356, false, false, p, 6, 100, true));
  assert(q.receive(0x351, false, false, p, 6, 101, true));
  assert(q.receive(0x356, false, false, p, 6, 102, true));
  assert(q.received == 3 && q.dropped == 1);
  Record r;
  // Pending 351(seq2) must precede newer 356(seq3), despite its slot index.
  assert(q.pop(r));
  assert(r[0] == 1 && r[1] == 6 && r[2] == 0x51 && r[3] == 3);
  assert(get32(r, 4) == 2 && get32(r, 8) == 101);
  assert(r[18] == 0 && r[19] == 0);
  assert(q.pop(r));
  assert(get32(r, 4) == 3 && get32(r, 8) == 102);
  assert(r[14] == 0x8d && r[15] == 0xff);  // Preserve signed bytes unchanged.
  assert(!q.pop(r));

  Queue wrapped(0xfffffffd);
  assert(wrapped.receive(0x356, false, false, p, 6, 0xfffffff0, true));
  assert(wrapped.receive(0x351, false, false, p, 6, 0xfffffff8, true));
  assert(wrapped.receive(0x373, false, false, p, 6, 0x00000010, true));
  assert(wrapped.pop(r) && get32(r, 4) == 0xfffffffe);
  assert(wrapped.pop(r) && get32(r, 4) == 0xffffffff);
  assert(wrapped.pop(r) && get32(r, 4) == 0);
  assert(!wrapped.pop(r));
  assert(q.receive(0x356, false, false, p, 6, 103, false));
  assert(!q.pop(r));  // Disconnected reception never creates a replay sample.
  for (auto id : IDS) assert(q.receive(id, false, false, p, 6, 200, true));
  for (size_t i = 0; i < SLOT_COUNT; ++i) assert(q.pop(r));
  assert(!q.pop(r));
  assert(q.receive(0x356, false, false, p, 6, 201, true));
  q.clear();
  assert(!q.pop(r));

  r = metadata(0x12345678, 0xfffffff0, 1234567, 999999);
  assert(r[0] == 'B' && r[1] == 'Z' && r[2] == 'C' && r[3] == 'B');
  assert(r[4] == 1 && r[5] == 1);
  assert(get32(r, 6) == 0x12345678 && get32(r, 10) == 0xfffffff0);
  assert(get32(r, 14) == 1234567 && r[18] == 255 && r[19] == 255);

  StallGuard a;
  a.begin(1000);
  assert(a.loop(1100));
  assert(a.observe(1101));
  // Receive callback comes first after the stall: it must not refresh time.
  assert(!a.observe(2000));
  assert(!a.loop(2000));
  assert(!a.observe(2200));
  assert(!a.loop(2200));
  assert(!a.loop(2400));
  assert(a.loop(2500));
  assert(a.observe(2501));
  StallGuard b;
  b.begin(1000);
  // Component loop comes first after the stall: quarantine must remain active
  // for the queued CAN callback that runs later in the same iteration.
  assert(!b.loop(2000));
  assert(!b.observe(2001));
  assert(!b.loop(2200));
  assert(!b.loop(2400));
  assert(b.loop(2500));
  StallGuard c;
  c.begin(0xfffffff0);
  assert(c.observe(0x10));
  assert(c.loop(0x10));
  assert(!c.loop(0x200));
  assert(!c.observe(0x201));
  assert(!c.loop(0x2c0));
  assert(!c.loop(0x380));
  assert(c.loop(0x400));
  std::puts("adapter protocol, queue ordering, bounds and stall quarantine checks passed");
}
