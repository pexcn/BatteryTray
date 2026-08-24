#include "util.h"

#include "win32.h"

namespace pct {

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

} // namespace pct
