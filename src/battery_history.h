#pragma once

#include <cstddef>

namespace bt {

// One percent step that actually elapsed. Far more trustworthy than
// extrapolating the instantaneous rate: a compile or a video swings the draw
// wildly, while this is the average over the whole step.
struct BatteryStep {
    int from_percent = 0;
    int to_percent = 0;
    double seconds = 0.0;
};

// A fixed ring of the most recent steps, newest first. Memory only - it is an
// observation window over the current session, and nothing this program keeps
// may outlive the process (see docs/specification.md 1.5).
class BatteryHistory {
public:
    // Fed every reading the tray takes; only a moved percentage records a step.
    void observe(int percent, bool charging);

    // The step in progress spans a suspend, so its duration is wall clock and
    // not use - eight hours for one percent says nothing. Drop it.
    void discard_pending() noexcept { pending_ = false; }

    [[nodiscard]] size_t size() const noexcept { return count_; }

    // 0 is the most recent step.
    [[nodiscard]] const BatteryStep& operator[](size_t index) const noexcept {
        return steps_[(newest_ + index) % kCapacity];
    }

private:
    void clear() noexcept { count_ = 0; }
    void push(const BatteryStep& step) noexcept;

    static constexpr size_t kCapacity = 12; // the panel shows a few; the rest is headroom

    BatteryStep steps_[kCapacity]{};
    size_t count_ = 0;
    size_t newest_ = 0;
    unsigned long long marked_ms_ = 0;
    int last_percent_ = -1;
    bool last_charging_ = false;
    bool pending_ = false; // is the interval since marked_ms_ worth recording
};

} // namespace bt
