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

// Width of "100% -> 99%", the widest reading there is.
constexpr size_t kReadingColumns = 11;

// Computed once: the executable cannot move under a running process, and every
// line written asks for this, which otherwise means walking the module path and
// a handful of allocations per write.
const std::wstring& log_path() {
    static const std::wstring path = [] {
        const std::wstring directory = module_directory();
        return directory.empty() ? std::wstring() : directory + L"\\BatteryTray.log";
    }();
    return path;
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
    const std::wstring& path = log_path();
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return 0; // a missing file is an empty one as far as every caller cares
    }
    return (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
}

bool append_bytes(const std::string& bytes) {
    const std::wstring& path = log_path();
    if (path.empty() || bytes.empty()) {
        return false;
    }

    if (log_size() + bytes.size() > kMaxLogBytes) {
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    const HANDLE raw = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const unique_file file(raw);
    if (!file) {
        return false;
    }

    DWORD written = 0;
    return WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
           written == bytes.size();
}

std::wstring percent_text(int percent) {
    return std::to_wstring(percent) + L"%";
}

// The shape the panel gives a measured step, so the same thing is not called two
// things in two places. ASCII arrow rather than the panel's →: that character's
// width is ambiguous in East Asian fonts, and this one sits in a padded column.
std::wstring step_text(int from, int to) {
    return percent_text(from) + L" -> " + percent_text(to);
}

// Timestamp, reading, and what happened, two spaces apart. The reading is a
// column of its own because every line has one, and it is right aligned so that
// the current percentage of every line lands on the same vertical rule, a step
// growing leftward out of it. Being all ASCII is what keeps that padding honest:
// the column after it is Chinese on some lines and a duration on others, and
// squaring that one up would mean guessing how wide a character is in whatever
// font the file gets opened in. Nothing lines up after the last column, so it is
// free form - and an empty one leaves a step whose duration is not known.
bool write_line(const std::wstring& reading, std::wstring_view event) {
    std::wstring line = timestamp();
    line += L"  ";
    line.append(reading.size() < kReadingColumns ? kReadingColumns - reading.size() : 0, L' ');
    line += reading;
    if (!event.empty()) {
        line += L"  ";
        line += event;
    }
    line += L"\r\n";
    return append_bytes(to_utf8(line));
}

} // namespace

void BatteryLog::start(const BatteryState& state) {
    // Runs read as separate blocks, but a fresh file should not open on a blank
    // line, so the separator goes in only when there is something above it.
    if (log_size() != 0) {
        append_bytes("\r\n");
    }
    enabled_ = write_line(percent_text(state.percent), L"开始记录");
    if (!enabled_) {
        return;
    }
    // Nothing has been observed yet, so this writes the opening state line and
    // leaves the first step unmeasured: logging started in the middle of one.
    observe(state);
}

void BatteryLog::disable() noexcept {
    enabled_ = false;
    last_percent_ = -1;
    marked_ms_ = 0;
    pending_ = false;
    asleep_ = false;
}

void BatteryLog::stop() {
    if (!enabled_) {
        return;
    }
    // The last reading observed: nobody hands one in on the way out, and the
    // battery cannot have moved since without the log hearing about it.
    write_line(percent_text(last_percent_), L"结束记录");
    disable();
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
    // reaching 100% is why the pack is now full. It also leaves the state line as
    // the last one written, so the newest one in the file always describes where
    // things stand.
    if (percent_moved) {
        // The step is written either way - the reading did move - but the
        // duration is held back, since one that straddles a state change was
        // spent partly going the other way and one that follows a start or a
        // suspend began before there was anything to measure it from.
        const bool measured = pending_ && !state_moved;
        const std::wstring elapsed =
            measured ? format_duration_compact((now - marked_ms_) / 1000.0) : std::wstring();
        // A directory that turned read-only mid-run disables logging instead of
        // retrying on every battery change, and takes the step in progress with
        // it: what elapsed while the log was off is not a step anyone measured.
        if (!write_line(step_text(last_percent_, state.percent), elapsed)) {
            disable();
            return;
        }
        marked_ms_ = now;
        pending_ = true; // a percent transition is a legitimate starting point
    } else {
        pending_ = false; // whatever was in flight straddles the state change
    }
    if (state_moved && !write_line(percent_text(state.percent), charge_state_text(state.charge))) {
        disable();
        return;
    }

    last_percent_ = state.percent;
    last_charge_ = state.charge;
}

void BatteryLog::note_suspend(const BatteryState& state) {
    if (!enabled_) {
        return;
    }
    if (!write_line(percent_text(state.percent), L"进入睡眠")) {
        disable();
        return;
    }
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
    std::wstring event =
        L"唤醒，睡眠 " + format_duration_compact((GetTickCount64() - suspend_ms_) / 1000.0) + L"，";
    const int delta = state.percent - suspend_percent_;
    if (delta < 0) {
        event += L"掉电 " + std::to_wstring(-delta) + L"%";
    } else if (delta > 0) {
        event += L"充电 " + std::to_wstring(delta) + L"%"; // asleep on the charger
    } else {
        event += L"电量未变";
    }
    if (!write_line(percent_text(state.percent), event)) {
        disable();
        return;
    }

    // Resync instead of letting observe() see the jump: what the sleep cost is
    // on the line above, and a "100 -> 94%" under it would claim a step that
    // never happened. A charge state that changed while the machine was down is
    // deliberately left for observe() to report.
    last_percent_ = state.percent;
}

} // namespace bt
