#include "battery_log.h"

#include "util.h"
#include "win32.h"
#include "win32_raii.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>

namespace bt {
namespace {

// Rolls to a single .old copy, so the pair never exceeds about 1 MB.
constexpr unsigned long long kMaxLogBytes = 512 * 1024;

// Display columns reserved for the event column, so the one after it starts in
// the same place on every line. The two kinds of token that go there happen to
// come out the same width: 电池已充满 is five double width characters, and the
// widest step, "100 -> 99%", is ten single width ones.
constexpr int kEventColumns = 10;
constexpr int kValueColumns = 4; // "100%"

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

unsigned long long log_size() {
    const std::wstring path = log_path();
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return 0; // a missing file is an empty one as far as every caller cares
    }
    return (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
}

bool append_bytes(const std::string& bytes) {
    const std::wstring path = log_path();
    if (path.empty() || bytes.empty()) {
        return false;
    }

    if (log_size() + bytes.size() > kMaxLogBytes) {
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
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

// Only two kinds of token ever reach a padded column: ASCII for the steps and
// the percentages, Chinese for the event names. Everything outside ASCII here is
// a full width character, so none of the real width tables are needed.
int display_columns(std::wstring_view text) {
    int columns = 0;
    for (const wchar_t character : text) {
        columns += character < 0x80 ? 1 : 2;
    }
    return columns;
}

// Two spaces between columns, plus whatever squares the field up. The file is
// read in a text editor, and the default face in both (Consolas on Windows 10,
// Cascadia Mono on 11) is monospace with a double width CJK fallback, so the
// columns land. A proportional face lines nothing up, but no scheme survives one
// -- the tooltip gave up on alignment for that very reason (specification 2.6).
void append_column(std::wstring& line, std::wstring_view field, int columns) {
    line += field;
    const int padding = columns - display_columns(field);
    line.append(static_cast<size_t>(padding > 0 ? padding : 0) + 2, L' ');
}

bool write_line(std::wstring_view event, std::wstring_view value = {}, std::wstring_view note = {}) {
    std::wstring line = timestamp();
    line += L"  ";
    if (value.empty()) {
        line += event; // a session marker stands alone in its column
    } else {
        append_column(line, event, kEventColumns);
        if (note.empty()) {
            line += value;
        } else {
            append_column(line, value, kValueColumns);
            line += note;
        }
    }
    line += L"\r\n";
    return append_bytes(to_utf8(line));
}

std::wstring percent_text(int percent) {
    return std::to_wstring(percent) + L"%";
}

// The shape the panel gives a measured step, so the same number is not called
// two things in two places. ASCII arrow rather than the panel's →: that
// character's width is ambiguous in East Asian fonts, and a column that is one
// cell wide in some faces and two in others defeats the padding above.
std::wstring step_text(int from, int to) {
    return std::to_wstring(from) + L" -> " + percent_text(to);
}

} // namespace

void BatteryLog::start(const BatteryState& state) {
    // Runs read as separate blocks, but a fresh file should not open on a blank
    // line, so the separator goes in only when there is something above it.
    if (log_size() != 0) {
        append_bytes("\r\n");
    }
    enabled_ = write_line(L"开始记录");
    if (!enabled_) {
        return;
    }
    // Nothing has been observed yet, so this writes the opening state line and
    // leaves the first step unmeasured: logging started in the middle of one.
    observe(state);
}

void BatteryLog::stop() {
    if (!enabled_) {
        return;
    }
    write_line(L"结束记录");
    enabled_ = false;
    last_percent_ = -1;
    pending_ = false;
    asleep_ = false;
}

void BatteryLog::observe(const BatteryState& state) {
    if (!enabled_) {
        return;
    }
    const bool state_moved = last_percent_ < 0 || state.charge != last_charge_;
    const bool percent_moved = last_percent_ >= 0 && state.percent != last_percent_;
    if (!state_moved && !percent_moved) {
        return; // nothing to say, and the interval in progress keeps running
    }

    // Wall clock, the way the panel measures a step (battery_history.cpp): what
    // a percent cost includes the time the machine spent idle inside it.
    const unsigned long long now = GetTickCount64();

    // The step goes first when both moved, because it is what caused the other:
    // 99 -> 100% is why the pack is now full. It also leaves the state line as
    // the last one written, so the newest one in the file always describes where
    // things stand.
    if (percent_moved) {
        // The step is written either way; only the duration is held back, since
        // one that straddles a state change was spent partly going the other way
        // and one that follows a start or a suspend began before there was
        // anything to measure it from.
        const bool measured = pending_ && !state_moved;
        // A directory that turned read-only mid-run disables logging instead of
        // retrying on every battery change.
        enabled_ = write_line(step_text(last_percent_, state.percent),
                              measured ? format_duration((now - marked_ms_) / 1000.0) : std::wstring());
        marked_ms_ = now;
        pending_ = true; // a percent transition is a legitimate starting point
    } else {
        pending_ = false; // whatever was in flight straddles the state change
    }
    if (state_moved && enabled_) {
        enabled_ = write_line(charge_state_text(state.charge), percent_text(state.percent));
    }

    last_percent_ = state.percent;
    last_charge_ = state.charge;
}

void BatteryLog::note_suspend(const BatteryState& state) {
    if (!enabled_) {
        return;
    }
    enabled_ = write_line(L"进入睡眠", percent_text(state.percent));
    suspend_ms_ = GetTickCount64();
    suspend_percent_ = state.percent;
    pending_ = false; // the interval in progress is about to span the sleep
    asleep_ = true;
}

void BatteryLog::note_resume(const BatteryState& state) {
    if (!enabled_ || !asleep_) {
        return;
    }
    asleep_ = false;

    // GetTickCount64 keeps counting through a suspend where the unbiased clock
    // does not, and wall clock is the whole point of this line.
    std::wstring note = L"睡眠 " + format_duration((GetTickCount64() - suspend_ms_) / 1000.0) + L"，";
    const int delta = state.percent - suspend_percent_;
    if (delta < 0) {
        note += L"掉电 " + std::to_wstring(-delta) + L"%";
    } else if (delta > 0) {
        note += L"充电 " + std::to_wstring(delta) + L"%"; // asleep on the charger
    } else {
        note += L"电量未变";
    }
    enabled_ = write_line(L"唤醒", percent_text(state.percent), note);

    // Resync instead of letting observe() see the jump: what the sleep cost is
    // on the line above, and a "100 -> 94%" under it would claim a step that
    // never happened. A charge state that changed while the machine was down is
    // deliberately left for observe() to report.
    last_percent_ = state.percent;
}

} // namespace bt
