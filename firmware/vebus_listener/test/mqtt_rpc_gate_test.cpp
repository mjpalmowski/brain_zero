// Home Assistant's MQTT requests wait for a busy outbox instead of going unanswered.
#include "mqtt_rpc_gate.h"
#include <cassert>
#include <cstdio>

using namespace mqtt_rpc;

int main() {
    constexpr int LIMIT = 3072;
    // A fresh request on an idle link is answered.
    assert(gate(false, true, 5000, 1000, 0, LIMIT) == Gate::ANSWER);
    assert(gate(false, true, MIN_GAP_MS, MAX_AGE_US, LIMIT - 1, LIMIT) == Gate::ANSWER);
    // A busy outbox, a reply sent moments ago or a dropped connection make it wait, not vanish.
    assert(gate(false, true, 5000, 1000, LIMIT, LIMIT) == Gate::WAIT);
    assert(gate(false, true, 5000, 1000, 7306, LIMIT) == Gate::WAIT);
    assert(gate(false, true, MIN_GAP_MS - 1, 1000, 0, LIMIT) == Gate::WAIT);
    assert(gate(false, false, 5000, 1000, 0, LIMIT) == Gate::WAIT);
    // Too old for Home Assistant's 5 s wait: dropped, however idle the link.
    assert(gate(false, true, 5000, MAX_AGE_US + 1, 0, LIMIT) == Gate::DROP_STALE);
    assert(gate(false, true, 5000, MAX_AGE_US + 1, LIMIT, LIMIT) == Gate::DROP_STALE);
    assert(gate(false, true, 5000, -1, 0, LIMIT) == Gate::DROP_STALE);  // a clock step backwards
    // A retained request never runs.
    assert(gate(true, true, 5000, 1000, 0, LIMIT) == Gate::IGNORE);
    // A waiting request is answered once the outbox drains within its 3 s.
    int outbox = 4743;
    Gate g = Gate::WAIT;
    for (int64_t age = 0; age <= MAX_AGE_US && g == Gate::WAIT; age += 50000) {
        g = gate(false, true, 5000, age, outbox, LIMIT);
        outbox -= 300;  // acknowledgements arriving
    }
    assert(g == Gate::ANSWER);
    std::puts("MQTT requests wait for a busy outbox and are answered within their 3 s");
    return 0;
}
