#pragma once

#include "battery_power.h"

namespace bt {

// Appends battery changes to BatteryTray.log next to the executable. Writes are
// synchronous open-append-close: changes arrive minutes apart, so a background
// thread would only buy a shutdown race in exchange for nothing.
//
// Two shapes of line: a state line when the charge state moves, and a step line
// for every percent that elapses, carrying how long it took. The state used to
// sit on every line instead, which turned a screenful of readings into a
// screenful of the same four characters with the one number that was actually
// moving buried among them.
class BatteryLog {
public:
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Silently stays disabled when the executable's directory is not writable,
    // e.g. under Program Files; nothing is ever written elsewhere.
    void start(const BatteryState& state);
    void stop();

    // Fed every reading the tray takes, the way BatteryHistory::observe is; what
    // comes out is a state line, a step line, both, or nothing at all.
    void observe(const BatteryState& state);

    // Without these a gap in the file reads as a battery that sat still for eight
    // hours. They name it instead, and the resume line carries what the sleep
    // cost - the one thing the panel's steps cannot keep, since the interval that
    // spans a suspend is dropped there.
    void note_suspend(const BatteryState& state);
    void note_resume(const BatteryState& state);

private:
    // The one way logging stops, whether the user unchecked it or a write
    // failed: the step in progress and the sleep in progress belong to a session
    // that has ended, and leaving them behind would let the next start() measure
    // a step across the whole disabled stretch.
    void disable() noexcept;

    bool enabled_ = false;

    int last_percent_ = -1; // negative until the first reading is observed
    ChargeState last_charge_ = ChargeState::Discharging;
    // Only a percent transition is a legitimate starting point for the next
    // step's duration. A start, a state change or a suspend leaves the step in
    // progress unmeasurable, and it gets written without one rather than with a
    // number that counts from the wrong moment.
    unsigned long long marked_ms_ = 0;
    bool pending_ = false;

    unsigned long long suspend_ms_ = 0;
    int suspend_percent_ = 0;
    bool asleep_ = false;
};

} // namespace bt
