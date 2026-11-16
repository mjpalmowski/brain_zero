// The runner extracts the pinned NimBLE wait/release and production callback
// bodies verbatim. Only the small FreeRTOS notification channel is modelled.
#include "battery_ble_queue.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>

#define INC_FREERTOS_H 1
using ble_npl_time_t = uint32_t;
constexpr uint32_t BLE_NPL_TIME_FOREVER = UINT32_MAX, TASK_BLOCK_BIT = uint32_t(1) << 31;
constexpr int pdTRUE = 1, pdFALSE = 0;
struct FakeTask {
    std::mutex mutex;
    std::condition_variable changed;
    uint32_t value = 0;
    bool pending = false, waiting = false;
};
using TaskHandle_t = FakeTask *;
thread_local FakeTask *current_task = nullptr;
void ble_npl_time_ms_to_ticks(uint32_t value, ble_npl_time_t *out) { *out = value; }
int xTaskNotifyWait(uint32_t clear_entry, uint32_t clear_exit, uint32_t *out, uint32_t ticks) {
    auto &task = *current_task;
    std::unique_lock<std::mutex> lock(task.mutex);
    task.value &= ~clear_entry;
    if (!task.pending && ticks) {
        task.waiting = true; task.changed.notify_all();
        const uint32_t bounded = ticks == BLE_NPL_TIME_FOREVER ? 2000 : ticks;
        task.changed.wait_for(lock, std::chrono::milliseconds(bounded), [&] { return task.pending; });
        task.waiting = false;
    }
    if (!task.pending) { if (out) *out = 0; return pdFALSE; }
    if (out) *out = task.value;
    task.value &= ~clear_exit; task.pending = false;
    return pdTRUE;
}
enum NotifyAction { eSetBits };
void xTaskNotify(TaskHandle_t task, uint32_t value, NotifyAction) {
    std::lock_guard<std::mutex> lock(task->mutex);
    task->value |= value; task->pending = true; task->changed.notify_all();
}
void xTaskNotifyGive(TaskHandle_t task) {
    std::lock_guard<std::mutex> lock(task->mutex);
    ++task->value; task->pending = true; task->changed.notify_all();
}
struct NimBLETaskData {
    explicit NimBLETaskData(FakeTask &task) : m_pHandle(&task) {}
    void *m_pHandle;
    mutable int m_flags = 0;
};
class NimBLEUtils {
public:
    static bool taskWait(const NimBLETaskData &, uint32_t);
    static void taskRelease(const NimBLETaskData &, int);
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "actual_nimble_wait.inc"
#pragma GCC diagnostic pop

class NimBLEClient {};
struct Link;
struct Callbacks {
    explicit Callbacks(Link *value) : link(value) {}
    Link *link;
    void onConnect(NimBLEClient *);
    void onConnectFail(NimBLEClient *, int);
    void onDisconnect(NimBLEClient *, int);
};
constexpr size_t MAX_NOTIFICATION = battery_ble::detail::NOTIFICATION_BYTES;
int64_t esp_timer_get_time() { return 1000000; }
uint32_t millis() { return 1000; }
#define portENTER_CRITICAL(mutex) (mutex)->lock()
#define portEXIT_CRITICAL(mutex) (mutex)->unlock()
struct Link {
    std::atomic<bool> connected{false}, queue_problem{false};
    std::atomic<uint32_t> connections{0}, epoch{1}, notifications{0}, dropped{0}, last_notification_ms{0};
    std::atomic<int> last_error{0};
    std::atomic<TaskHandle_t> task{nullptr};
    std::mutex queue_lock;
    battery_ble::detail::NotificationQueue pending;
    void enqueue(const uint8_t *, size_t, uint32_t);
};
#include "actual_ble_callbacks.inc"

void wait_until_waiting(FakeTask &task) {
    std::unique_lock<std::mutex> lock(task.mutex);
    assert(task.changed.wait_for(lock, std::chrono::seconds(1), [&] { return task.waiting; }));
}
std::future<bool> wait_async(FakeTask &task, const NimBLETaskData &data) {
    return std::async(std::launch::async, [&] {
        current_task = &task;
        return NimBLEUtils::taskWait(data, BLE_NPL_TIME_FOREVER);
    });
}

int main() {
    {
        // Reproduce the fault mechanism with the actual pinned NimBLE function:
        // a plain application notification completes GATT with no GATT result.
        FakeTask task; NimBLETaskData data(task);
        auto result = wait_async(task, data); wait_until_waiting(task);
        xTaskNotifyGive(&task);
        assert(result.get() && data.m_flags == 0);
    }
    {
        FakeTask task; NimBLETaskData data(task);
        Link link; link.task = &task; Callbacks callbacks(&link);
        battery_ble::detail::Notification slots[2]{};
        link.pending.bind(slots, 2);
        auto result = wait_async(task, data); wait_until_waiting(task);
        callbacks.onConnect(nullptr);
        uint8_t packet[20] = {1};
        link.enqueue(packet, sizeof packet, link.epoch);
        callbacks.onConnectFail(nullptr, 7);
        callbacks.onDisconnect(nullptr, 8);
        link.enqueue(packet, sizeof packet, 1);  // Stale-session callback is refused.
        assert(link.notifications == 1 && link.pending.count() == 1);
        const bool still_waiting = result.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready;
        NimBLEUtils::taskRelease(data, 123);
        assert(result.get() && still_waiting && data.m_flags == 123);
        assert(link.last_error == 8 && link.epoch == 2);
        battery_ble::detail::Notification copied{};
        assert(link.pending.pop(copied) && copied.length == 20 && copied.data[0] == 1);
    }
    std::puts("Pinned NimBLE notification collision reproduced; production connect/disconnect/queue callbacks cannot release GATT waits");
}
