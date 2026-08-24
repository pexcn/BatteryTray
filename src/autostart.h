#pragma once

namespace pct::autostart {

// Per-user Run key: no elevation needed, and it is the only state this program
// persists anywhere.
[[nodiscard]] bool is_enabled();

bool set_enabled(bool enable);

} // namespace pct::autostart
