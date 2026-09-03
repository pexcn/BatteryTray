#include "util.h"

#include "win32.h"
#include "win32_raii.h"

namespace bt {

std::wstring module_file_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        // A full fit is reported as "length == buffer size" without an error on
        // some Windows versions, so treat it as truncation and grow.
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= 32768) {
            return {};
        }
        path.resize(path.size() * 2);
    }
}

std::wstring module_directory() {
    std::wstring path = module_file_path();
    const size_t separator = path.find_last_of(L'\\');
    if (separator == std::wstring::npos) {
        return {};
    }
    path.resize(separator);
    return path;
}

bool system_uses_light_theme() {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS) {
        return false;
    }
    const unique_regkey key(raw);

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key.get(), L"SystemUsesLightTheme", nullptr, &type, reinterpret_cast<BYTE*>(&value),
                         &size) != ERROR_SUCCESS ||
        type != REG_DWORD || size != sizeof(value)) {
        return false;
    }
    return value == 1;
}

void apply_menu_theme(bool light_theme) {
    if (light_theme) {
        return; // what uxtheme does by default; not worth loading a DLL to ask for
    }

    // Popup menus are drawn by user32 out of the system's Menu theme, and Win32
    // has never exposed which of the two it picks. The switch the shell uses for
    // itself lives in uxtheme, exported by ordinal only: 135 is
    // SetPreferredAppMode and 136 is FlushMenuThemes, which makes user32 drop
    // the menu theme it has already cached. Ordinal 135 was AllowDarkModeForApp
    // with a different signature before build 18362, but the target floor here
    // is 19044 (Windows 10 LTSC 2021), so there is only one meaning to hit.
    enum PreferredAppMode { Default, AllowDark, ForceDark, ForceLight };
    using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
    using FlushMenuThemesFn = void(WINAPI*)();

    // System32 only: an ordinal taken from whatever uxtheme.dll happens to sit
    // earlier on the search path is a DLL plant with extra steps. Deliberately
    // never freed - the mode is uxtheme's own process state, and user32 holds
    // the library for theming regardless.
    const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) {
        return;
    }
    const auto set_preferred_app_mode =
        reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    const auto flush_menu_themes =
        reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (!set_preferred_app_mode || !flush_menu_themes) {
        return; // the menu stays light, which is where it was
    }

    // Forced, not AllowDark: left to itself uxtheme follows AppsUseLightTheme,
    // while the icon and the panel follow SystemUsesLightTheme, and the user can
    // set those two the opposite way round - one program showing both themes at
    // once is worse than a menu on the wrong one.
    set_preferred_app_mode(ForceDark);
    flush_menu_themes();
}

namespace {

std::wstring compose_duration(double seconds, const wchar_t* gap) {
    const long long total = static_cast<long long>(seconds);
    if (total >= 3600) {
        return std::to_wstring(total / 3600) + gap + L"小时" + gap + std::to_wstring(total % 3600 / 60) +
               gap + L"分";
    }
    if (total >= 60) {
        return std::to_wstring(total / 60) + gap + L"分" + gap + std::to_wstring(total % 60) + gap + L"秒";
    }
    return std::to_wstring(total) + gap + L"秒";
}

} // namespace

// A unit is set off from its digits the way every other number in the UI sets
// one off, Latin or Chinese alike: 6.3 W, 每变化 1%, 3 分 58 秒.
std::wstring format_duration(double seconds) {
    return compose_duration(seconds, L" ");
}

std::wstring format_duration_compact(double seconds) {
    return compose_duration(seconds, L"");
}

} // namespace bt
