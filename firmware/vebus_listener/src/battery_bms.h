#pragma once

#include <stddef.h>
#include <stdint.h>

// Read-only battery protocol decoders. There is no inverter, MOSFET or battery
// settings-write API here. All times supplied by callers are monotonic microseconds.
namespace battery_bms {

constexpr int64_t FRESH_US = 10000000;
constexpr int64_t ANCHOR_FRESH_US = 60000000;
constexpr int64_t SOURCE_TIMESTAMP_UNCERTAINTY_US = 250000;
constexpr size_t MAX_CELLS = 24;
constexpr size_t CAN_BLE_BYTES = 20;

enum Field : uint32_t {
    SOC = 1u << 0, VOLTAGE = 1u << 1, CURRENT = 1u << 2,
    CELL_EXTREMA = 1u << 3, CELL_VALUES = 1u << 4,
    CAPACITY = 1u << 5, REMAINING = 1u << 6, SOH = 1u << 7,
    TEMPERATURES = 1u << 8, MOS_TEMPERATURE = 1u << 9,
    LIMITS = 1u << 10, PERMISSIONS = 1u << 11, ALARMS = 1u << 12,
    BALANCING = 1u << 13, CYCLES = 1u << 14, IDENTITY = 1u << 15,
    PACK_TEMPERATURE = 1u << 16,
};

struct Snapshot {
    uint32_t valid = 0;
    bool connected = false, complete = false;
    float soc = 0, voltage = 0, current = 0; // positive current charges the pack
    float capacity_ah = 0, remaining_ah = 0, soh = 0;
    float min_cell_v = 0, max_cell_v = 0, delta_cell_v = 0;
    float cell_v[MAX_CELLS] = {};
    uint8_t cell_count = 0, min_cell_index = 0, max_cell_index = 0;
    float temp_min_c = 0, temp_max_c = 0, temp_mos_c = 0, temperature_c = 0;
    float charge_voltage_limit_v = 0, charge_current_limit_a = 0, discharge_current_limit_a = 0;
    float balancing_current_a = 0;
    bool charge_allowed = false, discharge_allowed = false, balancing = false;
    bool force_charge = false, full_charge_requested = false;
    uint32_t alarms = 0, warnings = 0, cycles = 0;
    uint8_t module_count = 0;
    char model[17] = {}, hardware[9] = {}, firmware[9] = {}, protocol[9] = {};
    float sample_age_s = -1, soc_age_s = -1, cell_age_s = -1;
    float limits_age_s = -1, permissions_age_s = -1, alarms_age_s = -1;
    bool has(uint32_t fields) const { return (valid & fields) == fields; }
};

struct Stats {
    uint32_t accepted = 0, refused = 0, checksum_errors = 0, duplicates = 0;
};

class JkFeed {
public:
    void connected(bool value);
    void reset();
    // A notification may be one fragment, one frame, or a frame with a trailer.
    // Returns true only if at least one supported, checksum-valid record was accepted.
    bool accept(const uint8_t *data, size_t length, int64_t now_us);
    Snapshot view(int64_t now_us) const;
    bool profile_ready() const { return connected_ && profile_ready_; }
    const Stats &stats() const { return stats_; }
    // Only the two zero-value read requests are encodable. All other opcodes
    // fail and wipe the output. This is not a generic register-write helper.
    static bool request(uint8_t opcode, uint8_t out[20]);
private:
    void clear_samples();
    void clear_assembly();
    bool decode(int64_t now_us);
    bool decode_identity();
    bool decode_status(int64_t now_us);
    Snapshot sample_;
    Stats stats_;
    int64_t status_at_ = -1, fragment_at_ = -1;
    uint8_t frame_[300] = {};
    uint16_t used_ = 0;
    uint8_t last_counter_ = 0;
    bool connected_ = false, profile_ready_ = false, counter_seen_ = false;
};

class PaceFeed {
public:
    void connected(bool value);
    void reset();
    // Read metadata once per connection and at least once per minute. The read
    // must be timed by the caller; RTT >250ms cannot establish a fresh anchor.
    // Metadata: BZCB, version, flags=1, boot-id LE32, source millis LE32,
    // rx-count LE32, drops LE16. It never refreshes battery field timestamps.
    bool anchor(const uint8_t *data, size_t length, int64_t local_begin_us, int64_t local_end_us);
    // Frame: version, DLC, standard ID LE16, sequence LE32, original CAN receive
    // millis LE32, eight payload bytes (zero padding). Exactly20bytes.
    bool accept(const uint8_t *data, size_t length, int64_t now_us);
    Snapshot view(int64_t now_us) const;
    bool anchored() const { return connected_ && have_anchor_; }
    uint32_t boot_id() const { return boot_id_; }
    const Stats &stats() const { return stats_; }
private:
    void clear_samples();
    bool decode_can(uint16_t id, const uint8_t *data, uint8_t length, int64_t at_us);
    Snapshot sample_;
    Stats stats_;
    int64_t at_[7] = {-1, -1, -1, -1, -1, -1, -1};
    int64_t anchor_begin_ = -1, anchor_end_ = -1;
    uint32_t anchor_ms_ = 0, boot_id_ = 0, last_sequence_ = 0;
    bool connected_ = false, have_anchor_ = false, sequence_seen_ = false;
};

// Both arguments must be views taken at the same local time. PACE CAN may not
// supply capacity; an explicitly configured capacity can supply that weight.
// Fields never become valid from only one of the two parallel packs.
Snapshot aggregate(const Snapshot &jk, const Snapshot &pace, float configured_pace_capacity_ah = 0);

} // namespace battery_bms
