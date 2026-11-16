#include "../src/battery_ble.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <initializer_list>

using battery_ble::Settings;

static Settings valid() {
    Settings s;
    s.enabled = true;
    strcpy(s.jk_address, "02:00:00:00:00:01");
    strcpy(s.pace_address, "02:00:00:00:00:02");
    s.pace_capacity_ah = 280;
    return s;
}

int main() {
    const char *why = reinterpret_cast<const char *>(1);
    Settings disabled;
    assert(battery_ble::normalize(disabled, &why) && !why);
    Settings s = valid();
    assert(battery_ble::normalize(s));
    s.enabled = true; s.jk_address[0] = s.pace_address[0] = 0;
    assert(!battery_ble::normalize(s, &why) && why);

    // Device bindings are distinct after canonicalisation, not just as typed.
    s = valid();
    strcpy(s.jk_address, "aa:bb:cc:dd:ee:01");
    strcpy(s.pace_address, "AA:BB:CC:DD:EE:01");
    assert(!battery_ble::normalize(s));
    s.pace_address[0] = 0;
    assert(battery_ble::normalize(s));
    assert(!strcmp(s.jk_address, "AA:BB:CC:DD:EE:01"));

    const char *bad[] = {"00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "02-00-00-00-00-01",
                         "02:00:00:00:00:0G", "02:00:00:00:00:1", " 2:00:00:00:00:01"};
    for (const char *a : bad) {
        s = valid(); strcpy(s.jk_address, a);
        assert(!battery_ble::normalize(s));
    }
    s = valid(); memset(s.jk_address, 'A', sizeof s.jk_address);
    assert(!battery_ble::normalize(s));

    const float bad_capacity[] = {-1.f, float(INFINITY), float(NAN), 10001.f};
    for (float capacity : bad_capacity) {
        s = valid(); s.pace_capacity_ah = capacity;
        assert(!battery_ble::normalize(s));
        s = valid(); s.jk_capacity_ah = capacity;
        assert(!battery_ble::normalize(s));
    }
    s = valid(); s.version = 0;
    assert(!battery_ble::normalize(s));
    s.version = 2;
    assert(!battery_ble::normalize(s));
    printf("battery BLE settings validation passed; Settings=%u Snapshot=%u View=%u bytes\n",
           unsigned(sizeof(Settings)), unsigned(sizeof(battery_bms::Snapshot)), unsigned(sizeof(battery_ble::View)));
}
