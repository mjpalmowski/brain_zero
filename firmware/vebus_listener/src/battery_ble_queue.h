#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace battery_ble { namespace detail {

constexpr size_t NOTIFICATION_BYTES = 61;  // Negotiated MTU64 minus ATT header.
struct Notification {
    int64_t at_us;
    uint32_t epoch;
    uint16_t length;
    uint8_t data[NOTIFICATION_BYTES];
};

inline void secure_zero(void *data, size_t length) {
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
    while (length--) *bytes++ = 0;
}

// The caller supplies bounded storage and serializes every method. No allocation
// occurs here. Pop clears the consumed slot, so identity fragments cannot linger
// in a FreeRTOS queue after the decoder has consumed and wiped its own copy.
class NotificationQueue {
public:
    void bind(Notification *storage, uint8_t capacity) {
        storage_ = storage; capacity_ = capacity; clear();
    }
    void clear() {
        if (storage_) secure_zero(storage_, size_t(capacity_) * sizeof(Notification));
        head_ = count_ = 0;
    }
    bool push(const uint8_t *data, size_t length, int64_t at_us, uint32_t epoch) {
        if (!storage_ || !capacity_ || count_ == capacity_ || !data || !length ||
            length > NOTIFICATION_BYTES) return false;
        Notification &out = storage_[(head_ + count_) % capacity_];
        out.at_us = at_us; out.epoch = epoch; out.length = uint16_t(length);
        memcpy(out.data, data, length); ++count_;
        return true;
    }
    bool pop(Notification &out) {
        if (!count_) return false;
        out = storage_[head_];
        secure_zero(&storage_[head_], sizeof(Notification));
        head_ = uint8_t((head_ + 1) % capacity_); --count_;
        return true;
    }
    uint8_t count() const { return count_; }
private:
    Notification *storage_ = nullptr;
    uint8_t capacity_ = 0, head_ = 0, count_ = 0;
};

}}  // namespace battery_ble::detail
