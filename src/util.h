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

// Seconds are worth showing only while the span is short enough to watch tick
// by; past an hour they are noise around an estimate that is already an
// extrapolation from one instantaneous sample.
std::wstring format_duration(double seconds);

} // namespace bt
