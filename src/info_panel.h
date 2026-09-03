#pragma once

#include "battery_power.h"
#include "win32.h"
#include "win32_raii.h"

#include <string>
#include <vector>

namespace bt {

class BatteryHistory;

// The flyout a double click on the tray icon raises. It carries what a tooltip
// cannot: the pack's health, the last few percent steps that actually elapsed,
// and a reading that keeps refreshing while someone watches it - which is the
// point of a window that stays put instead of vanishing with the pointer.
class InfoPanel {
public:
    InfoPanel(HINSTANCE instance, const BatteryHistory& history, bool light_theme);
    ~InfoPanel();

    InfoPanel(const InfoPanel&) = delete;
    InfoPanel& operator=(const InfoPanel&) = delete;

    // Raised against the tray icon of `owner`; a second call while it is up
    // takes it back down.
    void toggle(HWND owner, UINT icon_id);

private:
    // Rows are the whole layout: a label on the left, a value on the right.
    enum class RowStyle {
        Entry,     // label and value
        Heading,   // section title, no value
        Detail,    // dimmer than Entry, sits under a Heading - the elapsed steps
        Separator, // a rule, no text
    };

    struct Row {
        RowStyle style = RowStyle::Entry;
        std::wstring label;
        std::wstring value;
    };

    struct Palette {
        COLORREF background;
        COLORREF text;
        COLORREF secondary;
        COLORREF separator;
        COLORREF border;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    bool ensure_window();
    void ensure_fonts();
    void show();
    void hide();

    void sample();
    void reset_rate_window() noexcept;
    void build_rows();
    [[nodiscard]] int scale(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
    [[nodiscard]] int row_height(RowStyle style) const;
    [[nodiscard]] SIZE measure() const;
    [[nodiscard]] POINT anchor_origin(SIZE size) const;
    void relayout();
    void paint(HDC dc, const RECT& client) const;

    HINSTANCE instance_;
    const BatteryHistory& history_;
    Palette colors_;

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    UINT icon_id_ = 0;
    bool visible_ = false;
    unsigned long long hidden_ms_ = 0;

    UINT dpi_ = 96;
    UINT font_dpi_ = 0;
    unique_font font_;
    unique_font title_font_;
    int line_height_ = 0;
    int title_height_ = 0;

    // Held open for as long as the panel is up; see BatteryDevice.
    BatteryDevice device_;
    BatteryDate date_;
    BatteryState state_;
    BatterySample sample_;
    bool has_sample_ = false;

    // The instantaneous draw jumps around, and a number that changes every two
    // seconds is unreadable next to a setting someone is trying to compare.
    static constexpr int kRateWindow = 3;
    long rate_window_[kRateWindow]{};
    int rate_index_ = 0;
    int rate_count_ = 0;
    // Which way the samples in the window flow: +1 charging, -1 discharging, 0
    // while nothing but zeroes has been seen. Averaging across a reversal would
    // produce a wattage that never occurred, so the window starts over instead.
    int rate_direction_ = 0;
    long rate_mw_ = 0;

    std::vector<Row> rows_;
};

} // namespace bt
