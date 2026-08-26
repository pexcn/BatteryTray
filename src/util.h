#pragma once

#include <string>

namespace bt {

// Full path of the running executable, empty on failure.
std::wstring module_file_path();

// Directory holding the running executable, without a trailing backslash.
std::wstring module_directory();

// True when HKCU\...\Themes\Personalize says the shell is on the light theme; a
// missing or malformed value counts as dark, which is what Windows ships. Read
// once at startup by design - following live theme changes would mean redrawing
// on a broadcast for a setting nobody flips while watching the tray.
bool system_uses_light_theme();

// Points user32 at the dark Menu theme for this process, so the tray icon's
// context menu matches the icon and the panel. A no-op on the light theme, and
// on anything that does not offer the switch.
void apply_menu_theme(bool light_theme);

// Seconds are worth showing only while the span is short enough to watch tick
// by; past an hour they are noise around an estimate that is already an
// extrapolation from one instantaneous sample.
std::wstring format_duration(double seconds);

// The same span with the spaces squeezed out (6分12秒). The UI sets a unit off
// from its digits, but in the log these stack up in a column that is already
// doing that job, and the spaces only make the column wider for nothing.
std::wstring format_duration_compact(double seconds);

} // namespace bt
