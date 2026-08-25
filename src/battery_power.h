#pragma once

namespace bt {

// A reading taken straight from the battery driver. GetSystemPowerStatus only
// exposes whole percent, which cannot answer "how long does one percent last"
// without waiting for a whole step to elapse - and by then the answer is about
// the past. The driver reports stored energy in mWh and the instantaneous draw
// in mW, so a single sample answers it for the load right now.
struct BatteryPower {
    unsigned long full_charge_mwh = 0; // capacity of a full charge, 0 when unknown
    unsigned long remaining_mwh = 0;   // energy left in the pack, 0 when unknown
    long rate_mw = 0;                  // negative while discharging, 0 when unknown
};

// All zero on a machine with no battery, on a pack that reports capacity in
// relative units rather than mWh, and on any query failure - callers only need
// to check the fields they are about to divide by.
BatteryPower query_battery_power();

} // namespace bt
