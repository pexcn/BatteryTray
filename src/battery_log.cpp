#include "battery_log.h"

#include "util.h"
#include "win32.h"
#include "win32_raii.h"

#include <cstdio>
#include <cwchar>
#include <string>

namespace bt {
namespace {

// Rolls to a single .old copy, so the pair never exceeds about 1 MB.
constexpr unsigned long long kMaxLogBytes = 512 * 1024;

std::wstring log_path() {
    const std::wstring directory = module_directory();
    if (directory.empty()) {
        return {};
    }
    return directory + L"\\BatteryTray.log";
}

std::wstring timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    // Fixed format instead of the locale default: sortable and identical on
    // every machine the log is read on.
    wchar_t buffer[32];
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond));
    return buffer;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

bool append_bytes(const std::string& bytes) {
    const std::wstring path = log_path();
    if (path.empty() || bytes.empty()) {
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        const unsigned long long size =
            (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        if (size + bytes.size() > kMaxLogBytes) {
            MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    const HANDLE raw = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD open_error = GetLastError(); // OPEN_ALWAYS reports ERROR_ALREADY_EXISTS on reuse
    const unique_file file(raw);
    if (!file) {
        return false;
    }

    std::string payload;
    if (open_error != ERROR_ALREADY_EXISTS) {
        payload += "\xEF\xBB\xBF"; // without a BOM Notepad still reads a fresh log as ANSI
    }
    payload += bytes;

    DWORD written = 0;
    return WriteFile(file.get(), payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
           written == payload.size();
}

bool write_line(const std::wstring& text) {
    return append_bytes(to_utf8(L"[" + timestamp() + L"]: " + text + L"\r\n"));
}

} // namespace

void BatteryLog::start() {
    enabled_ = write_line(L"开启电量日志");
}

void BatteryLog::stop() {
    if (!enabled_) {
        return;
    }
    write_line(L"关闭电量日志");
    enabled_ = false;
}

void BatteryLog::append_status(std::wstring_view display, bool charging) {
    if (!enabled_) {
        return;
    }
    std::wstring line = charging ? L"正在充电" : L"使用电池";
    line += L" -> ";
    line += display;
    line += L'%';
    // A directory that turned read-only mid-run disables logging instead of
    // retrying on every battery change.
    enabled_ = write_line(line);
}

} // namespace bt
