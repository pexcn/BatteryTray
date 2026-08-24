#include "autostart.h"

#include "util.h"
#include "win32.h"
#include "win32_raii.h"

#include <string>

namespace bt::autostart {
namespace {

constexpr wchar_t kRunKeyPath[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"BatteryTray";

} // namespace

bool is_enabled() {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS) {
        return false;
    }
    const unique_regkey key(raw);

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key.get(), kValueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        return false;
    }
    // Anything that is not a non-empty string was not written by us.
    return (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t);
}

bool set_enabled(bool enable) {
    HKEY raw = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &raw, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const unique_regkey key(raw);

    if (!enable) {
        const LSTATUS status = RegDeleteValueW(key.get(), kValueName);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    const std::wstring path = module_file_path();
    if (path.empty()) {
        return false;
    }
    // The Run key splits an unquoted command line on spaces.
    const std::wstring command = L'"' + path + L'"';
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key.get(), kValueName, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(command.c_str()), bytes) == ERROR_SUCCESS;
}

} // namespace bt::autostart
