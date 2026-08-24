#pragma once

#include <string>
#include <string_view>

namespace pct {

// Appends battery changes to percentage.log next to the executable. Writes are
// synchronous open-append-close: changes arrive minutes apart, so a background
// thread would only buy a shutdown race in exchange for nothing.
class BatteryLog {
public:
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Silently stays disabled when the executable's directory is not writable,
    // e.g. under Program Files; nothing is ever written elsewhere.
    void start();
    void stop();

    void append_status(std::wstring_view display, bool charging);

private:
    bool enabled_ = false;
};

} // namespace pct
