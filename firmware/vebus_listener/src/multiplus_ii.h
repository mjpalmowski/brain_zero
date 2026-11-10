#pragma once

#include <stdint.h>

namespace multiplus_ii {

struct Model {
    uint16_t product_id;  // first four decimal digits packed as BCD, as in Venus
    uint8_t nominal_voltage;
    const char *name;
};

// Identity for 230 V monitoring, independent of runtime features and write access.
// Match Victron's VE.Bus firmware archive device metadata against its product specifications:
// https://updates.victronenergy.com/feeds/VEBus/firmware-archive/
// https://www.victronenergy.com/inverters-chargers/multiplus-ii
// One prefix can cover several enclosures or firmware variants, including GX and EasySolar.
inline const Model *find(uint32_t firmware) {
    static constexpr Model models[] = {
        {0x2601, 48, "MultiPlus-II 48/4k5/55-32"},
        {0x2602, 48, "MultiPlus-II 48/6k5/100-50"},
        {0x2604, 12, "MultiPlus-II 12/5000/220-50"},
        {0x2605, 12, "MultiPlus-II 12/3000/120-32"},
        {0x2611, 24, "MultiPlus-II 24/3000/70-32"},
        {0x2615, 24, "MultiPlus-II 24/5000/120-50"},
        {0x2619, 48, "MultiPlus-II 48/15000/200-100"},
        {0x2621, 48, "MultiPlus-II 48/8000/110-100"},
        {0x2623, 48, "MultiPlus-II 48/5000/70-50"},
        {0x2625, 48, "MultiPlus-II 48/3000/35-32"},
        {0x2626, 48, "MultiPlus-II 48/5000/70-50"},
        {0x2627, 48, "MultiPlus-II 48/10000/140-100"},
        {0x2628, 48, "MultiPlus-II 48/3000/35-32"},
        {0x2629, 48, "MultiPlus-II 48/3000/35-32"},
        // The 2630 archive label says 180 A, while the current 12/4k manual says 160 A.
        {0x2630, 12, "MultiPlus-II 12/4k"},
        {0x2636, 48, "MultiPlus-II 48/4k5 (low transformer ratio)"},
        {0x2637, 48, "MultiPlus-II 48/6k5 (low transformer ratio)"},
        {0x2682, 48, "MultiPlus-II 48/8000/110-100"},
        {0x2683, 48, "MultiPlus-II 48/10000/140-100"},
        {0x2684, 48, "MultiPlus-II 48/15000/200-100"},
        {0x2696, 24, "MultiPlus-II 24/5000/120-50"},
        {0x2697, 24, "MultiPlus-II 24/3000/70-32"},
        {0x2698, 48, "MultiPlus-II 48/5000/70-50"},
        {0x2699, 48, "MultiPlus-II 48/3000/35-32"},
    };

    const uint32_t prefix = firmware / 1000;
    if (prefix < 2600 || prefix > 2699) return nullptr;
    const uint16_t product_id = 0x2600 | ((prefix / 10 % 10) << 4) | (prefix % 10);
    for (const Model &model : models)
        if (model.product_id == product_id) return &model;
    return nullptr;
}

}  // namespace multiplus_ii
