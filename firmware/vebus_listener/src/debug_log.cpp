#include "debug_log.h"

#include <stdarg.h>
#include <stdio.h>

namespace debug_log {
uint8_t Writer::try_write(Target target, const char *data, size_t size) noexcept {
    if (!data || !size || size > MAX_WRITE_BYTES ||
        busy_.test_and_set(std::memory_order_acquire)) return 0;
    struct Unlock {
        std::atomic_flag &flag;
        ~Unlock() { flag.clear(std::memory_order_release); }
    } unlock{busy_};
    const auto *bytes = reinterpret_cast<const uint8_t *>(data);
    const uint8_t wanted = static_cast<uint8_t>(target);
    uint8_t accepted = 0;
    if (wanted & static_cast<uint8_t>(Target::UART_DEBUG)) {
        const size_t space = sink_.uart_space();
        // Arduino returns FIFO space (<=128) when its TX ring has no space.
        // Reserve room for the driver's event/chunk ring-buffer metadata too.
        if (space > 128 && space >= size && space - size >= UART_HEADROOM_BYTES) {
            sink_.uart_write(bytes, size);
            accepted |= static_cast<uint8_t>(Target::UART_DEBUG);
        }
    }
    if ((wanted & static_cast<uint8_t>(Target::USB_DEBUG)) &&
        sink_.usb_connected() && sink_.usb_space() >= size) {
        // HWCDC's timeout=0 path is not sufficient on Arduino 2.0.17 when a
        // connected host stops reading. Never enter its full-ring wait path.
        sink_.usb_write(bytes, size);
        accepted |= static_cast<uint8_t>(Target::USB_DEBUG);
    }
    return accepted;
}
} // namespace debug_log

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>

namespace debug_log {
namespace {
class HardwareSink final : public Sink {
public:
    size_t uart_space() noexcept override { return Serial0.availableForWrite(); }
    void uart_write(const uint8_t *data, size_t size) noexcept override { Serial0.write(data, size); }
    bool usb_connected() noexcept override { return bool(Serial); }
    size_t usb_space() noexcept override { return Serial.availableForWrite(); }
    void usb_write(const uint8_t *data, size_t size) noexcept override { Serial.write(data, size); }
};
HardwareSink hardware;
Writer writer(hardware);
}

uint8_t write(Target target, const char *data, size_t size) noexcept {
    return writer.try_write(target, data, size);
}

void printf(Target target, const char *format, ...) {
    if (!format) return;
    char text[512];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    // Drop an oversized line rather than emitting a misleading partial record.
    if (length > 0 && size_t(length) < sizeof text) write(target, text, size_t(length));
}
} // namespace debug_log
#endif
