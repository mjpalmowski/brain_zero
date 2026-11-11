#include "controller_time.h"
#include "ess_policy.h"

#include <assert.h>
#include <stdio.h>

int main() {
    const auto *london = controller_time::find("Europe/London");
    const auto *new_york = controller_time::find("America/New_York");
    assert(london && new_york && !controller_time::find("GMT0BST"));

    ess::Schedule overnight;
    overnight.enabled = true;
    overnight.start_s = 23 * 3600 + 30 * 60;
    overnight.duration_s = 6 * 3600;
    auto active = [&](const controller_time::Zone &zone, time_t utc) {
        struct tm local = {};
        assert(controller_time::local(zone, utc, local));
        return ess::active_window(&overnight, 1, local) == 0;
    };

    // The saved window remains 23:30–05:30 local on both UK clock changes.
    assert(!active(*london, 1774740599));  // Mar 28 23:29:59 UTC
    assert(active(*london, 1774740600));
    assert(active(*london, 1774758599));   // Mar 29 04:29:59 UTC
    assert(!active(*london, 1774758600));
    assert(active(*london, 1792881000));   // Oct 24 22:30:00 UTC
    assert(active(*london, 1792906199));   // Oct 25 05:29:59 UTC
    assert(!active(*london, 1792906200));

    // Choosing a different zone moves the UTC window without editing its times.
    assert(!active(*new_york, 1774740600));
    assert(active(*new_york, 1774755000));  // Mar 29 03:30:00 UTC
    puts("ESS overnight window follows its selected time zone and both DST changes.");
}
