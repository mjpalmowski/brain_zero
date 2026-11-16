#include "debug_log.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using debug_log::Target;
namespace {
class Capture final : public debug_log::Sink {
public:
    size_t uart_free = 4096, usb_free = 2048;
    bool connected = true;
    unsigned usb_space_reads = 0;
    std::vector<std::string> uart, usb;
    debug_log::Writer *reenter = nullptr;
    bool reentered = false;
    size_t uart_space() noexcept override { return uart_free; }
    void uart_write(const uint8_t *data, size_t size) noexcept override {
        uart.emplace_back(reinterpret_cast<const char *>(data), size);
        if (reenter) {
            auto *writer = reenter; reenter = nullptr;
            assert(writer->try_write(Target::BOTH, "nested\n", 7) == 0);
            reentered = true;
        }
    }
    bool usb_connected() noexcept override { return connected; }
    size_t usb_space() noexcept override { ++usb_space_reads; return usb_free; }
    void usb_write(const uint8_t *data, size_t size) noexcept override {
        usb.emplace_back(reinterpret_cast<const char *>(data), size);
    }
};

class Blocked final : public debug_log::Sink {
public:
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false, released = false;
    size_t uart_space() noexcept override { return 4096; }
    void uart_write(const uint8_t *, size_t) noexcept override {
        std::unique_lock<std::mutex> guard(lock);
        entered = true; changed.notify_all();
        changed.wait(guard, [&] { return released; });
    }
    bool usb_connected() noexcept override { return false; }
    size_t usb_space() noexcept override { assert(false); return 0; }
    void usb_write(const uint8_t *, size_t) noexcept override { assert(false); }
};

class Concurrent final : public debug_log::Sink {
public:
    std::atomic<unsigned> in_output{0}, collisions{0}, outputs{0};
    size_t uart_space() noexcept override { return 4096; }
    bool usb_connected() noexcept override { return true; }
    size_t usb_space() noexcept override { return 2048; }
    void record(const uint8_t *data, size_t size) noexcept {
        if (in_output.fetch_add(1)) ++collisions;
        assert(size == 5 && !std::memcmp(data, "line\n", 5));
        std::this_thread::yield();
        ++outputs; --in_output;
    }
    void uart_write(const uint8_t *data, size_t size) noexcept override { record(data, size); }
    void usb_write(const uint8_t *data, size_t size) noexcept override { record(data, size); }
};
}

int main() {
    {
        Capture sink; debug_log::Writer writer(sink);
        const char message[] = "# unchanged text\r\n";
        const size_t size = sizeof message - 1;
        assert(writer.try_write(Target::BOTH, message, size) == 3);
        assert(sink.uart.back() == message && sink.usb.back() == message);
        sink.uart_free = size + 255;
        assert(writer.try_write(Target::BOTH, message, size) == 2);
        assert(sink.uart.size() == 1 && sink.usb.size() == 2);
        sink.uart_free = size + 256;
        assert(writer.try_write(Target::UART_DEBUG, message, size) == 1);
        sink.uart_free = 128;  // Arduino's full-ring FIFO fallback is not usable ring space.
        assert(writer.try_write(Target::UART_DEBUG, "x", 1) == 0);
        sink.uart_free = 4096; sink.connected = false;
        const auto reads = sink.usb_space_reads;
        assert(writer.try_write(Target::BOTH, message, size) == 1);
        assert(sink.usb_space_reads == reads);
        sink.connected = true; sink.usb_free = size - 1;
        assert(writer.try_write(Target::BOTH, message, size) == 1);
        sink.usb_free = size;
        assert(writer.try_write(Target::USB_DEBUG, message, size) == 2);
        const std::string maximum(debug_log::MAX_WRITE_BYTES, 'm');
        sink.uart_free = 4096; sink.usb_free = maximum.size();
        assert(writer.try_write(Target::BOTH, maximum.data(), maximum.size()) == 3);
        const auto uart_count = sink.uart.size(), usb_count = sink.usb.size();
        assert(!writer.try_write(Target::BOTH, maximum.data(), maximum.size() + 1));
        assert(!writer.try_write(Target::BOTH, maximum.data(), SIZE_MAX));
        assert(!writer.try_write(Target::BOTH, nullptr, 1));
        assert(!writer.try_write(Target::BOTH, "", 0));
        assert(sink.uart.size() == uart_count && sink.usb.size() == usb_count);
        sink.reenter = &writer;
        assert(writer.try_write(Target::BOTH, message, size) == 3 && sink.reentered);
    }
    {
        // A slow/backpressured diagnostic sink must not make a control-task
        // contender wait. It loses the whole diagnostic instead.
        Blocked sink; debug_log::Writer writer(sink);
        auto first = std::async(std::launch::async, [&] { return writer.try_write(Target::UART_DEBUG, "a", 1); });
        {
            std::unique_lock<std::mutex> lock(sink.lock);
            assert(sink.changed.wait_for(lock, std::chrono::seconds(2), [&] { return sink.entered; }));
        }
        auto second = std::async(std::launch::async, [&] { return writer.try_write(Target::BOTH, "b", 1); });
        const bool bounded = second.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
        {
            std::lock_guard<std::mutex> lock(sink.lock); sink.released = true;
        }
        sink.changed.notify_all();
        assert(first.get() == 1);
        assert(bounded && second.get() == 0);
    }
    {
        Concurrent sink; debug_log::Writer writer(sink);
        std::atomic<unsigned> accepted{0};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 8; ++t) threads.emplace_back([&] {
            for (unsigned n = 0; n < 10000; ++n) {
                const auto result = writer.try_write(Target::BOTH, "line\n", 5);
                assert(result == 0 || result == 3);
                if (result) ++accepted;
            }
        });
        for (auto &thread : threads) thread.join();
        assert(accepted > 0 && sink.collisions == 0 && sink.outputs == accepted * 2);
    }
    std::puts("Diagnostic writes: exact text, FIFO/ring headroom, disconnected/full USB, invalid sizes, reentry and eight-thread contention passed");
}
