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

std::wstring format_duration(double seconds) {
    const long long total = static_cast<long long>(seconds);
    if (total >= 3600) {
        return std::to_wstring(total / 3600) + L"小时" + std::to_wstring(total % 3600 / 60) + L"分";
    }
    if (total >= 60) {
        return std::to_wstring(total / 60) + L"分" + std::to_wstring(total % 60) + L"秒";
    }
    return std::to_wstring(total) + L"秒";
}

} // namespace bt
