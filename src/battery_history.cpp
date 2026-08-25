#include "battery_history.h"

#include "win32.h"

namespace bt {

void BatteryHistory::push(const BatteryStep& step) noexcept {
    // The ring grows backwards so that index 0 is always the newest.
    newest_ = (newest_ + kCapacity - 1) % kCapacity;
    steps_[newest_] = step;
    if (count_ < kCapacity) {
        ++count_;
    }
}

void BatteryHistory::observe(int percent, ChargeState charge) {
    // GetTickCount64 rather than QueryUnbiasedInterruptTime: this measures how
    // long a percent lasted in wall clock terms, and the suspend that would
    // corrupt that is dropped by discard_pending instead.
    const unsigned long long now = GetTickCount64();

    if (last_percent_ < 0 || charge != last_charge_) {
        // Any move between charge states splits the list: the step in progress
        // straddles it, and charge steps do not belong beside discharge steps.
        clear();
    } else if (percent != last_percent_) {
        const bool rising = percent > last_percent_;
        // The direction can also flip with the charger untouched - a full pack
        // on AC drifts down a percent and tops itself back up.
        if (count_ != 0 && rising != ((*this)[0].to_percent > (*this)[0].from_percent)) {
            clear();
        }
        if (pending_) {
            push({last_percent_, percent, (now - marked_ms_) / 1000.0});
        }
    } else {
        return; // nothing moved, so the interval in progress keeps running
    }

    marked_ms_ = now;
    pending_ = true;
    last_percent_ = percent;
    last_charge_ = charge;
}

} // namespace bt
