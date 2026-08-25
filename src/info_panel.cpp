#include "info_panel.h"

#include "battery_history.h"
#include "util.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>

namespace bt {
namespace {

constexpr wchar_t kPanelClassName[] = L"BatteryTray.panel";

constexpr UINT kTimerId = 1;
// The source under IOCTL_BATTERY_QUERY_STATUS is ACPI _BST: an AML interpreter
// run and a round trip to the embedded controller, whose own sampling period is
// typically 1-5 seconds. Polling every second would mostly re-read one value
// four times, and the read is not free.
constexpr UINT kTimerIntervalMs = 2000;

// Device independent pixels; everything on screen goes through scale().
constexpr int kWidthDip = 268; // fixed, sized for the longest label plus its value
constexpr int kPaddingDip = 14;
constexpr int kRowGapDip = 8;
constexpr int kSeparatorDip = 13;
// Small on purpose. A window region is a one-bit clip, so the arc comes out as
// a staircase, and the wider the radius the more steps there are to notice. At
// this radius the corner is barely more than a chamfer - it takes the hard
// point off the square without ever showing how it was cut.
constexpr int kCornerDip = 2;
constexpr int kEdgeGapDip = 8; // breathing room against the taskbar

} // namespace

InfoPanel::InfoPanel(HINSTANCE instance, const BatteryHistory& history, bool light_theme)
    : instance_(instance), history_(history) {
    // Two flat palettes rather than anything theme aware at runtime, matching
    // the tray icon's read-once colour (see util::system_uses_light_theme).
    colors_ = light_theme ? Palette{RGB(249, 249, 249), RGB(0, 0, 0), RGB(96, 96, 96), RGB(224, 224, 224),
                                    RGB(190, 190, 190)}
                          : Palette{RGB(32, 32, 32), RGB(255, 255, 255), RGB(160, 160, 160),
                                    RGB(62, 62, 62), RGB(80, 80, 80)};
}

InfoPanel::~InfoPanel() {
    if (window_) {
        DestroyWindow(window_);
    }
}

void InfoPanel::toggle(HWND owner, UINT icon_id) {
    // Clicking the tray icon takes the foreground away from the panel first, so
    // by the time the click reaches us the panel has already hidden itself on
    // WM_ACTIVATE. Without this window a click on an open panel would close and
    // immediately reopen it, which reads as "nothing happened".
    if (visible_ || GetTickCount64() - hidden_ms_ < 500) {
        hide();
        return;
    }
    owner_ = owner;
    icon_id_ = icon_id;
    show();
}

bool InfoPanel::ensure_window() {
    if (window_) {
        return true;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &InfoPanel::window_proc;
    window_class.hInstance = instance_;
    window_class.lpszClassName = kPanelClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class)) {
        return false;
    }

    // WS_POPUP with no caption, topmost so it sits over the taskbar it hugs, and
    // WS_EX_TOOLWINDOW to stay out of the taskbar and Alt+Tab - the same shape
    // the shell's own volume and battery flyouts have.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClassName, L"电量托盘", WS_POPUP, 0, 0,
                              0, 0, nullptr, nullptr, instance_, this);
    return window_ != nullptr;
}

void InfoPanel::ensure_fonts() {
    if (font_ && font_dpi_ == dpi_) {
        return;
    }

    // lfMessageFont, not the tray icon's hardcoded Segoe UI: the reason for that
    // one is embedded bitmap strikes at tray ppem (specification 2.3), which do
    // not apply at body size, and these labels are Chinese - the system UI font
    // is the right face for them.
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        return;
    }

    LOGFONTW title = metrics.lfMessageFont;
    title.lfHeight = MulDiv(title.lfHeight, 9, 5); // the percentage leads the panel
    font_.reset(CreateFontIndirectW(&metrics.lfMessageFont));
    title_font_.reset(CreateFontIndirectW(&title));
    font_dpi_ = dpi_;

    const unique_dc dc(CreateCompatibleDC(nullptr));
    if (!dc) {
        return;
    }
    TEXTMETRICW text_metrics{};
    HGDIOBJ previous = SelectObject(dc.get(), font_.get());
    GetTextMetricsW(dc.get(), &text_metrics);
    line_height_ = text_metrics.tmHeight;
    SelectObject(dc.get(), title_font_.get());
    GetTextMetricsW(dc.get(), &text_metrics);
    title_height_ = text_metrics.tmHeight;
    SelectObject(dc.get(), previous);
}

void InfoPanel::show() {
    if (!ensure_window()) {
        return;
    }
    dpi_ = GetDpiForWindow(window_);
    ensure_fonts();

    rate_count_ = 0; // the window from the last time the panel was up is stale
    rate_index_ = 0;
    sample();
    relayout();

    // State first, window second: showing it can already deliver WM_ACTIVATE,
    // and a hide arriving in the middle has to find the panel consistent.
    visible_ = true;
    SetTimer(window_, kTimerId, kTimerIntervalMs, nullptr);
    ShowWindow(window_, SW_SHOW);
    // Focus is what makes WM_ACTIVATE arrive, and losing it is how the panel
    // closes; a tray click leaves us allowed to take the foreground.
    SetForegroundWindow(window_);
}

void InfoPanel::hide() {
    if (!window_) {
        return;
    }
    // Every hiding path lands here so that the timer cannot outlive the window
    // being visible - a timer left running is exactly the idle polling the rest
    // of the program is built to avoid.
    KillTimer(window_, kTimerId);
    ShowWindow(window_, SW_HIDE);
    visible_ = false;
    hidden_ms_ = GetTickCount64();
    device_.close();
    has_sample_ = false;
}

void InfoPanel::sample() {
    state_ = query_battery_state();

    if (!device_ && device_.open()) {
        // Static fields are read once per opening, never in the tick.
        date_ = device_.read_manufacture_date();
    }

    BatterySample fresh;
    if (!device_.read_sample(fresh)) {
        // The tag dies with a hot swap, and a stale handle never recovers.
        device_.close();
        has_sample_ = false;
        rate_count_ = 0;
        rate_index_ = 0;
        return;
    }

    sample_ = fresh;
    has_sample_ = true;
    rate_window_[rate_index_] = fresh.rate_mw;
    rate_index_ = (rate_index_ + 1) % kRateWindow;
    if (rate_count_ < kRateWindow) {
        ++rate_count_;
    }
    long sum = 0;
    for (int i = 0; i < rate_count_; ++i) {
        sum += rate_window_[i];
    }
    rate_mw_ = sum / rate_count_;
}

void InfoPanel::build_rows() {
    rows_.clear();
    wchar_t buffer[64];

    // Unavailable fields are dropped whole rather than shown as a placeholder,
    // and a section with nothing left in it takes its separator with it: a
    // stranded rule would only advertise what the firmware refuses to answer.
    size_t mark = rows_.size();
    rows_.push_back({RowStyle::Separator, {}, {}});
    const long magnitude = rate_mw_ < 0 ? -rate_mw_ : rate_mw_;
    const unsigned long full_mwh = device_.facts().full_charge_mwh;
    if (has_sample_ && magnitude != 0) {
        // Direction comes from the current, not from the charger: a full pack on
        // AC is plugged in with nothing flowing.
        const bool charging = rate_mw_ > 0;
        swprintf_s(buffer, L"%.1f W", magnitude / 1000.0);
        rows_.push_back({RowStyle::Entry, charging ? L"充电功率" : L"实时功耗", buffer});

        if (full_mwh != 0) {
            rows_.push_back(
                {RowStyle::Entry, L"每变化 1%", format_duration(full_mwh / 100.0 / magnitude * 3600.0)});
            const unsigned long energy =
                charging ? (full_mwh > sample_.remaining_mwh ? full_mwh - sample_.remaining_mwh : 0)
                         : sample_.remaining_mwh;
            if (energy != 0) {
                rows_.push_back({RowStyle::Entry, charging ? L"预计充满" : L"预计剩余",
                                 format_duration(energy / static_cast<double>(magnitude) * 3600.0)});
            }
        }
    }
    if (rows_.size() == mark + 1) {
        rows_.resize(mark);
    }

    // The measured steps always get their section: an empty list is a fact about
    // this session, not about the machine, and it fills in as it runs.
    rows_.push_back({RowStyle::Separator, {}, {}});
    rows_.push_back({RowStyle::Heading, L"最近实测", {}});
    const size_t steps = std::min<size_t>(history_.size(), 6);
    for (size_t i = 0; i < steps; ++i) {
        const BatteryStep& step = history_[i];
        swprintf_s(buffer, L"%d → %d%%", step.from_percent, step.to_percent);
        rows_.push_back({RowStyle::Detail, buffer, format_duration(step.seconds)});
    }
    if (steps == 0) {
        rows_.push_back({RowStyle::Detail, L"还没有数据", {}});
    }

    mark = rows_.size();
    rows_.push_back({RowStyle::Separator, {}, {}});
    const BatteryFacts& facts = device_.facts();
    if (facts.cycle_count != 0) {
        rows_.push_back({RowStyle::Entry, L"循环次数", std::to_wstring(facts.cycle_count) + L" 次"});
    }
    if (facts.full_charge_mwh != 0 && facts.design_mwh != 0) {
        swprintf_s(buffer, L"%.1f / %.1f Wh (%lu%%)", facts.full_charge_mwh / 1000.0,
                   facts.design_mwh / 1000.0,
                   (facts.full_charge_mwh * 100 + facts.design_mwh / 2) / facts.design_mwh);
        rows_.push_back({RowStyle::Entry, L"健康度", buffer});
    }
    if (has_sample_ && sample_.has_temperature) {
        swprintf_s(buffer, L"%.1f °C", sample_.celsius);
        rows_.push_back({RowStyle::Entry, L"温度", buffer});
    }
    if (date_.year != 0) {
        swprintf_s(buffer, L"%u-%02u", static_cast<unsigned>(date_.year), static_cast<unsigned>(date_.month));
        rows_.push_back({RowStyle::Entry, L"出厂日期", buffer});
    }
    if (rows_.size() == mark + 1) {
        rows_.resize(mark);
    }
}

int InfoPanel::row_height(RowStyle style) const {
    return style == RowStyle::Separator ? scale(kSeparatorDip) : line_height_ + scale(kRowGapDip);
}

SIZE InfoPanel::measure() const {
    // Height has to come out of the rows that survived the rules above, which is
    // why nothing about the window size can be decided at creation time.
    int height = scale(kPaddingDip) * 2 + title_height_;
    for (const Row& row : rows_) {
        height += row_height(row.style);
    }
    return {scale(kWidthDip), height};
}

POINT InfoPanel::anchor_origin(SIZE size) const {
    RECT anchor{};
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = owner_;
    identifier.uID = icon_id_;
    // S_FALSE means the icon is folded into the overflow flyout and the rect
    // belongs to the chevron button, not the icon - both are successes, so a
    // SUCCEEDED test alone would quietly pass the button off as the icon. The
    // placement is the same either way: hug whatever the user clicked.
    const HRESULT anchored = Shell_NotifyIconGetRect(&identifier, &anchor);
    const bool has_anchor = anchored == S_OK || anchored == S_FALSE;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const HMONITOR handle = has_anchor ? MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST)
                                       : MonitorFromWindow(owner_, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(handle, &monitor)) {
        return {0, 0};
    }
    const RECT& work = monitor.rcWork;
    const int gap = scale(kEdgeGapDip);

    // Bottom right of the work area is the fallback for the rare case where the
    // shell will not say where the icon is; it is at least on screen.
    POINT origin{work.right - gap - size.cx, work.bottom - gap - size.cy};
    if (has_anchor) {
        // Which edge the taskbar occupies is read off rcWork rather than
        // guessed: it already accounts for all four positions and for auto-hide.
        // The nearest edge to the icon is the one the taskbar is on.
        const LONG center_x = (anchor.left + anchor.right) / 2;
        const LONG center_y = (anchor.top + anchor.bottom) / 2;
        const LONG distance[] = {center_y - work.top, work.bottom - center_y, center_x - work.left,
                                 work.right - center_x};
        const int side = static_cast<int>(std::min_element(distance, distance + 4) - distance);

        if (side < 2) {
            // Taskbar across the top or bottom: hug that edge of the work area,
            // and centre the panel on the icon along the other axis.
            origin.y = side == 0 ? work.top + gap : work.bottom - gap - size.cy;
            origin.x = center_x - size.cx / 2;
        } else {
            origin.x = side == 2 ? work.left + gap : work.right - gap - size.cx;
            origin.y = center_y - size.cy / 2;
        }
    }

    // Nothing may end up outside the work area, so an icon close to a corner
    // pushes the whole panel back in.
    origin.x = std::max(work.left + gap, std::min(origin.x, work.right - gap - size.cx));
    origin.y = std::max(work.top + gap, std::min(origin.y, work.bottom - gap - size.cy));
    return origin;
}

void InfoPanel::relayout() {
    build_rows();
    const SIZE size = measure();

    // Rounded corners are cut out of the window shape, which is the only way a
    // plain GDI popup gets them; the region is owned by the system afterwards.
    const int corner = scale(kCornerDip) * 2;
    SetWindowRgn(window_, CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, corner, corner), FALSE);

    const POINT origin = anchor_origin(size);
    SetWindowPos(window_, HWND_TOPMOST, origin.x, origin.y, size.cx, size.cy, SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}

void InfoPanel::paint(HDC dc, const RECT& client) const {
    const unique_brush background(CreateSolidBrush(colors_.background));
    const unique_pen border(CreatePen(PS_SOLID, 1, colors_.border));
    if (!background || !border) {
        return;
    }
    HGDIOBJ previous_brush = SelectObject(dc, background.get());
    HGDIOBJ previous_pen = SelectObject(dc, border.get());
    const int corner = scale(kCornerDip) * 2;
    RoundRect(dc, client.left, client.top, client.right, client.bottom, corner, corner);

    SetBkMode(dc, TRANSPARENT);
    const int left = scale(kPaddingDip);
    const int right = client.right - scale(kPaddingDip);
    int y = scale(kPaddingDip);

    RECT line{left, y, right, y + title_height_};
    SelectObject(dc, title_font_.get());
    SetTextColor(dc, colors_.text);
    const std::wstring percent = std::to_wstring(state_.percent) + L"%";
    DrawTextW(dc, percent.c_str(), -1, &line, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, font_.get());
    SetTextColor(dc, colors_.secondary);
    DrawTextW(dc, charge_state_text(state_.charge), -1, &line,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    y += title_height_;

    const unique_pen rule(CreatePen(PS_SOLID, 1, colors_.separator));
    for (const Row& row : rows_) {
        const int height = row_height(row.style);
        if (row.style == RowStyle::Separator) {
            if (rule) {
                SelectObject(dc, rule.get());
                MoveToEx(dc, left, y + height / 2, nullptr);
                LineTo(dc, right, y + height / 2);
            }
        } else {
            line = {left, y, right, y + height};
            SetTextColor(dc, colors_.secondary);
            DrawTextW(dc, row.label.c_str(), -1, &line, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            if (!row.value.empty()) {
                SetTextColor(dc, row.style == RowStyle::Entry ? colors_.text : colors_.secondary);
                DrawTextW(dc, row.value.c_str(), -1, &line, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
        }
        y += height;
    }

    SelectObject(dc, previous_pen);
    SelectObject(dc, previous_brush);
}

LRESULT CALLBACK InfoPanel::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    InfoPanel* panel = nullptr;
    if (message == WM_NCCREATE) {
        panel = static_cast<InfoPanel*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        panel->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    } else {
        panel = reinterpret_cast<InfoPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (panel) {
        return panel->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT InfoPanel::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_TIMER:
        if (wparam == kTimerId) {
            sample();
            relayout(); // a row can appear or vanish, and then the height moves
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; // WM_PAINT covers every pixel

    case WM_PAINT: {
        PAINTSTRUCT paint_struct{};
        const HDC dc = BeginPaint(window_, &paint_struct);
        RECT client{};
        GetClientRect(window_, &client);
        // Through a memory bitmap: the whole panel is repainted on every tick,
        // and filling the background under the text on screen flickers.
        const unique_dc memory(CreateCompatibleDC(dc));
        const unique_bitmap bitmap(CreateCompatibleBitmap(dc, client.right, client.bottom));
        if (memory && bitmap) {
            HGDIOBJ previous = SelectObject(memory.get(), bitmap.get());
            paint(memory.get(), client);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory.get(), 0, 0, SRCCOPY);
            SelectObject(memory.get(), previous);
        } else {
            paint(dc, client);
        }
        EndPaint(window_, &paint_struct);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            hide(); // same as the shell's own flyouts: click away and it is gone
        }
        return 0;

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            hide();
        }
        return 0;

    case WM_CLOSE:
        hide(); // hidden, never destroyed: reopening should not rebuild the window
        return 0;

    case WM_DPICHANGED:
        // The suggested rect is ignored on purpose - the panel is anchored to
        // the tray icon, so its size and position are recomputed from scratch.
        dpi_ = LOWORD(wparam);
        ensure_fonts();
        if (visible_) {
            relayout();
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

} // namespace bt
