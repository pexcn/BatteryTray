#pragma once

#include "win32.h"
#include "win32_raii.h"

namespace bt {

// A plugged in pack is in one of three situations and the shell names all
// three; collapsing them into "the charger is in" is what made a full battery
// report as charging.
enum class ChargeState {
    Discharging,
    Charging,
    Full,
    PluggedIn, // on AC below full, held there by a vendor charge threshold
};

// One wording for the four states, shared by the tooltip, the panel and the
// log so that the same situation never gets two names.
const wchar_t* charge_state_text(ChargeState charge);

// What GetSystemPowerStatus can answer: whole percent, and what the charger is
// doing. Everything below it comes from the battery driver instead.
struct BatteryState {
    int percent = 100; // 255 ("unknown", what a machine with no battery reports) becomes 100
    ChargeState charge = ChargeState::Discharging;
    // False when the call itself failed. The other two fields then hold defaults
    // that read exactly like "full, on battery", which is a reading nobody took;
    // callers skip the tick instead of believing it.
    bool valid = true;
};

BatteryState query_battery_state();

// Fields the driver reports once per pack. Read while the device is opened,
// because the capability check has to query BatteryInformation anyway.
struct BatteryFacts {
    unsigned long design_mwh = 0;      // 0 when unknown
    unsigned long full_charge_mwh = 0; // 0 when unknown
    // 0 on the many machines whose ACPI _BIF/_BIX simply leaves it empty; there
    // is no second source for it, powercfg /batteryreport reads the same field.
    unsigned long cycle_count = 0;
};

// The values that move. GetSystemPowerStatus only exposes whole percent, which
// cannot answer "how long does one percent last" without waiting for a whole
// step to elapse - and by then the answer is about the past. The driver reports
// stored energy in mWh and the instantaneous draw in mW, so a single sample
// answers it for the load right now.
struct BatterySample {
    unsigned long remaining_mwh = 0; // 0 when unknown
    long rate_mw = 0;                // negative while discharging, 0 when unknown
    double celsius = 0.0;
    bool has_temperature = false; // most consumer laptops do not report it at all
};

struct BatteryDate {
    unsigned short year = 0; // 0 when the driver does not answer
    unsigned char month = 0;
};

// An open handle on the system battery, kept across samples by the info panel:
// while it is up a sample is taken every couple of seconds, and reopening the
// device each time would repeat the interface enumeration for nothing. The
// price is that the tag goes stale when the pack is hot swapped, so a failing
// IOCTL means drop the handle and open again rather than retry.
//
// A pack that reports BATTERY_CAPACITY_RELATIVE is skipped: its capacity units
// are whatever the driver decided, so mWh arithmetic on them is meaningless.
// That also skips UPS units, which are not the system battery.
class BatteryDevice {
public:
    [[nodiscard]] bool open();
    void close() noexcept;

    explicit operator bool() const noexcept { return static_cast<bool>(device_); }

    // All zero until open() succeeds, so callers only need to check the fields
    // they are about to divide by.
    [[nodiscard]] const BatteryFacts& facts() const noexcept { return facts_; }

    [[nodiscard]] bool read_sample(BatterySample& sample) const;

    // Its own call rather than part of open(): the tooltip path opens a device
    // on every hover and has no use for the date.
    [[nodiscard]] BatteryDate read_manufacture_date() const;

private:
    bool bind(const wchar_t* path);

    unique_file device_;
    ULONG tag_ = 0;
    BatteryFacts facts_;
};

} // namespace bt
