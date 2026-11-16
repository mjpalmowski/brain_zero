#include "../src/battery_ble_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using battery_ble::detail::Notification;
using battery_ble::detail::NotificationQueue;

static bool zero(const void *data, size_t length) {
    auto bytes = static_cast<const uint8_t *>(data);
    while (length--) if (*bytes++) return false;
    return true;
}

int main() {
    struct Guarded { uint32_t first; Notification slots[3]; uint32_t last; } memory;
    memset(&memory, 0xCC, sizeof memory);
    const uint32_t guard = 0xCCCCCCCC;
    NotificationQueue q;
    q.bind(memory.slots, 3);
    assert(zero(memory.slots, sizeof memory.slots));
    uint8_t secret[61]; memset(secret, 0xAB, sizeof secret);
    assert(q.push(secret, sizeof secret, 100, 7));
    assert(q.push(secret, 20, 200, 8));
    assert(q.push(secret, 1, 300, 9));
    assert(!q.push(secret, 20, 400, 10));
    assert(q.count() == 3);
    Notification out{};
    assert(q.pop(out));
    assert(out.length == 61 && out.at_us == 100 && out.epoch == 7);
    assert(!memcmp(out.data, secret, 61));
    assert(zero(&memory.slots[0], sizeof(Notification)));

    // Wrap reuses only the wiped slot. Shorter fragments retain no tail from
    // the previous identity fragment stored at that location.
    assert(q.push(secret, 4, 500, 11));
    assert(zero(memory.slots[0].data + 4, sizeof memory.slots[0].data - 4));
    assert(q.pop(out) && out.at_us == 200 && out.epoch == 8);
    assert(q.pop(out) && out.at_us == 300 && out.epoch == 9);
    assert(q.pop(out) && out.at_us == 500 && out.epoch == 11);
    assert(!q.pop(out));
    assert(zero(memory.slots, sizeof memory.slots));
    assert(!q.push(nullptr, 1, 0, 0));
    assert(!q.push(secret, 0, 0, 0));
    assert(!q.push(secret, 62, 0, 0));
    assert(q.count() == 0);
    assert(q.push(secret, 61, 600, 12));
    q.clear();
    assert(q.count() == 0 && zero(memory.slots, sizeof memory.slots));
    assert(memory.first == guard && memory.last == guard);
    puts("battery BLE notification queue bounds and privacy passed");
}
