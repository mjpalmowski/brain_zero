#pragma once
#include "battery_ble.h"
#include "battery_control.h"
struct BatterySettings {
    battery_ble::Settings ble;
    battery_control::Settings control;
};
