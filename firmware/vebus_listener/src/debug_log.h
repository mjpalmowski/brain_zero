#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

// Diagnostics must yield to inverter traffic. Every application writer uses
// the same try-lock and drops a complete message when its output lacks space.
namespace debug_log {
enum class Target : uint8_t { UART_DEBUG = 1, USB_DEBUG = 2, BOTH = 3 };
constexpr size_t MAX_WRITE_BYTES = 1023;
constexpr size_t UART_HEADROOM_BYTES = 256;

class Sink {
public:
    virtual ~Sink() = default;
    virtual size_t uart_space() noexcept = 0;
    virtual void uart_write(const uint8_t *data, size_t size) noexcept = 0;
    virtual bool usb_connected() noexcept = 0;
    virtual size_t usb_space() noexcept = 0;
    virtual void usb_write(const uint8_t *data, size_t size) noexcept = 0;
};

class Writer {
public:
    explicit Writer(Sink &sink) : sink_(sink) {}
    // Returns the bitmask of outputs that accepted the whole message. No wait
    // for another application writer, ring space, or a USB host is introduced.
    uint8_t try_write(Target target, const char *data, size_t size) noexcept;
private:
    Sink &sink_;
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
};

uint8_t write(Target target, const char *data, size_t size) noexcept;
void printf(Target target, const char *format, ...)
    __attribute__((format(printf, 2, 3), noinline));
} // namespace debug_log
