// The runner extracts the pinned NimBLE wait/release and production callback
// bodies verbatim. Only the small FreeRTOS notification channel is modelled.
#include "battery_ble_queue.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <functional>
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

class NimBLEAddress {};
class NimBLEClient {
public:
    bool connected = false;
    bool asynchronous = false, exchange_mtu = true;
    int error = 0;
    unsigned starts = 0, cancels = 0, disconnects = 0;
    std::function<bool()> on_start;
    std::function<void()> on_cancel, on_disconnect;
    bool isConnected() const { return connected; }
    int getLastError() const { return error; }
    bool connect(const NimBLEAddress &, bool erase, bool async, bool mtu) {
        assert(erase); ++starts; asynchronous = async; exchange_mtu = mtu;
        return on_start ? on_start() : true;
    }
    bool cancelConnect() { ++cancels; if (on_cancel) on_cancel(); return true; }
    bool disconnect() { ++disconnects; if (on_disconnect) on_disconnect(); return true; }
};
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
uint32_t fake_millis = 1000;
uint32_t millis() { return fake_millis; }
#define portENTER_CRITICAL(mutex) (mutex)->lock()
#define portEXIT_CRITICAL(mutex) (mutex)->unlock()
struct Link {
    enum class State { OFF, STREAMING, WAITING, CONNECTING, CLOSING };
    NimBLEClient *client = nullptr;
    std::atomic<bool> connected{false}, queue_problem{false};
    std::atomic<uint32_t> connections{0}, epoch{1}, notifications{0}, dropped{0}, last_notification_ms{0};
    std::atomic<uint32_t> deadline_ms{0}, configuration_revision{0};
    std::atomic<uint32_t> gap_events{0};
    std::atomic<State> state{State::OFF};
    std::atomic<int> last_error{0};
    std::atomic<TaskHandle_t> task{nullptr};
    std::mutex queue_lock;
    battery_ble::detail::NotificationQueue pending;
    uint32_t gap_event_before = 0;
    bool awaiting_gap_event = false;
    int gap_failure = 0;
    void enqueue(const uint8_t *, size_t, uint32_t);
};
#include "actual_ble_callbacks.inc"

using State = Link::State;
struct Runtime {
    Link links[2];
    std::atomic<bool> initialized{false}, starting{true};
    std::atomic<uint32_t> startup_idle_revision{0};
};
Runtime *runtime = nullptr;
bool creation_in_progress = false, gap_active = false;
struct Settings { bool enabled = false; } desired;
uint32_t desired_revision = 7;
std::mutex settings_lock;
int ble_gap_conn_active() { return gap_active; }
Settings settings_copy(uint32_t *revision) { *revision = desired_revision; return desired; }
void disconnected(Link &link) { link.connected = false; ++link.epoch; link.pending.clear(); }
std::function<void()> on_tick;
#define pdMS_TO_TICKS(value) (value)
void vTaskDelay(uint32_t ticks) { fake_millis += ticks; if (on_tick) on_tick(); }
#include "actual_runtime_helpers.inc"

struct Attempt {
    Link link;
    NimBLEClient client;
    Callbacks callback{&link};
    NimBLEAddress address;
    Attempt() {
        fake_millis = 1000; on_tick = {}; desired.enabled = true; desired_revision = 7;
        link.client = &client; link.configuration_revision = desired_revision;
        link.deadline_ms = operation_deadline(CONNECT_GUARD_MS); link.state = State::CONNECTING;
        gap_active = false;
        client.on_start = [&] { gap_active = true; return true; };
        client.on_disconnect = [&] { callback.onDisconnect(nullptr, 0x216); client.connected = false; };
    }
    ~Attempt() { on_tick = {}; }
};

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
    {
        assert(OPERATION_MS == 5500 && CONNECT_TIMEOUT_MS == 10000 && CONNECT_GUARD_MS == 12000);
        assert(CONNECT_GUARD_MS > CONNECT_TIMEOUT_MS + 1000);
        assert(operation_deadline() == 6500 && operation_deadline(CONNECT_GUARD_MS) == 13000);
        for (uint32_t duration : {OPERATION_MS, CONNECT_GUARD_MS}) {
            fake_millis = UINT32_MAX - duration + 1;
            assert(operation_deadline(duration) == 1); // Zero remains the no-operation sentinel.
            fake_millis = UINT32_MAX - 50;
            const auto deadline = operation_deadline(duration);
            assert(deadline != 0 && int32_t(deadline - fake_millis) > 0);
        }
    }
    {
        // A successful event can arrive inside connect(), before a synchronous
        // wrapper would have installed its stack-local task data.
        Attempt attempt;
        attempt.client.error = 999; // A previous SDK error is not this attempt's result.
        attempt.link.gap_events = UINT32_MAX;
        attempt.client.on_start = [&] {
            attempt.client.connected = true; gap_active = false;
            attempt.callback.onConnect(nullptr); return true;
        };
        assert(connect_jk(attempt.link, attempt.address));
        assert(attempt.client.asynchronous && !attempt.client.exchange_mtu && attempt.link.gap_events == 0);
    }
    {
        Attempt attempt;
        on_tick = [&] {
            if (fake_millis == 1080) attempt.client.connected = true;
            if (fake_millis == 1100) { gap_active = false; attempt.callback.onConnect(nullptr); }
        };
        assert(connect_jk(attempt.link, attempt.address) && fake_millis == 1100);
    }
    for (bool revision_change : {false, true}) {
        Attempt attempt;
        attempt.client.on_cancel = [&] { gap_active = false; };
        on_tick = [&] {
            if (fake_millis == 1040) {
                if (revision_change) ++desired_revision; else desired.enabled = false;
            }
            if (fake_millis == 1060) { // A late success must be closed, not admitted.
                attempt.client.connected = true; attempt.callback.onConnect(nullptr);
            }
            if (fake_millis == 1080) attempt.callback.onConnectFail(nullptr, 0x23e);
        };
        assert(!connect_jk(attempt.link, attempt.address));
        assert(fake_millis >= 1120 && attempt.client.cancels == 1 && attempt.client.disconnects == 1);
        assert(!attempt.client.connected && !attempt.link.awaiting_gap_event && !gap_active);
        assert(attempt.link.last_error == CONFIG_CHANGED_ERROR && attempt.link.state == State::WAITING);
    }
    {
        Attempt attempt;
        desired.enabled = false;
        assert(!connect_jk(attempt.link, attempt.address) && !attempt.client.starts);
    }
    {
        Attempt attempt;
        attempt.client.on_cancel = [&] {
            gap_active = false; attempt.callback.onConnectFail(nullptr, 13);
        };
        assert(!connect_jk(attempt.link, attempt.address));
        assert(fake_millis >= 13000 && fake_millis <= 13000 + CLOSE_GUARD_MS);
        assert(attempt.client.cancels == 1 && attempt.link.last_error == GAP_TIMEOUT_ERROR);
    }
    {
        Attempt attempt;
        attempt.client.on_cancel = [&] { gap_active = false; }; // No final callback yet.
        assert(!connect_jk(attempt.link, attempt.address));
        assert(attempt.link.state == State::CLOSING && attempt.link.awaiting_gap_event);
        assert(attempt.link.gap_failure == GAP_TIMEOUT_ERROR);
        attempt.callback.onConnectFail(nullptr, 13);
        assert(close_jk_attempt(attempt.link) && !attempt.link.awaiting_gap_event);
    }
    {
        Attempt attempt;
        attempt.client.error = 42;
        attempt.client.on_start = [] { return false; };
        assert(!connect_jk(attempt.link, attempt.address));
        assert(attempt.link.last_error == 42 && !attempt.client.cancels);
    }
    {
        assert(quiescent());
        desired.enabled = true; assert(quiescent()); // A deferred boot has no allocated host.
        creation_in_progress = true; assert(!quiescent());
        creation_in_progress = false;
        Runtime allocated; runtime = &allocated;
        assert(!quiescent()); // Future attempts are still enabled.
        desired.enabled = false;
        assert(!quiescent()); // Startup has not acknowledged this disable.
        allocated.startup_idle_revision = 6; assert(!quiescent());
        allocated.startup_idle_revision = 7; assert(quiescent());
        allocated.startup_idle_revision = 0; assert(!quiescent()); // Init in progress.
        allocated.initialized = true; assert(!quiescent()); // Worker setup in progress.
        allocated.starting = false;
        NimBLEClient clients[2];
        allocated.links[0].client = &clients[0]; allocated.links[1].client = &clients[1];
        assert(quiescent()); // Allocation failure left no worker that could reconnect.
        clients[0].connected = true; assert(!quiescent()); clients[0].connected = false;
        gap_active = true; assert(!quiescent()); gap_active = false;
        allocated.links[0].deadline_ms = 123; assert(!quiescent()); allocated.links[0].deadline_ms = 0;
        FakeTask worker;
        for (auto &link : allocated.links) {
            link.task = &worker; link.configuration_revision = 7;
        }
        assert(quiescent());
        allocated.links[1].state = State::STREAMING; assert(!quiescent());
        allocated.links[1].state = State::OFF;
        ++desired_revision; assert(!quiescent()); // Invalidated flags alone are not a worker acknowledgement.
        allocated.links[0].configuration_revision = desired_revision; assert(!quiescent());
        allocated.links[1].configuration_revision = desired_revision; assert(quiescent());
        runtime = nullptr;
    }
    std::puts("Pinned NimBLE notification collision reproduced; production connect/disconnect/queue callbacks cannot release GATT waits");
    std::puts("Actual runtime helpers: separate GAP/GATT budgets, millis wrap, init/GAP closure and worker-revision quiescence passed");
    std::puts("Actual JK async GAP: early/delayed/late callbacks, no-MTU, disabled/revision cancellation, deadline and callback-confirmed closure passed");
}
