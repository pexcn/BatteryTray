#include "info_panel.h"

#include "battery_history.h"
#include "util.h"

#include <dwmapi.h>
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

// Device independent pixels; everything on screen goes through scale(). The
// numbers are the shell's own flyout metrics: 16 of padding, rows a good deal
// taller than the text in them, and a panel wide enough that the longest value
// still sits well clear of its label.
constexpr int kWidthDip = 320; // fixed, sized for the longest label plus its value
constexpr int kPaddingDip = 16;
constexpr int kRowGapDip = 10;
constexpr int kSeparatorDip = 17;
constexpr int kIndentDip = 14;
constexpr int kCornerDip = 8;
constexpr int kEdgeGapDip = 12; // breathing room against the taskbar

// lfMessageFont is 9pt, which is the size Win32 dialogs use; the shell's own
// flyouts set body text a size larger and the headline value at twice that.
constexpr int kBodyScaleNumerator = 7;
constexpr int kBodyScaleDenominator = 6;

// DWM attributes that only exist on Windows 11, spelled out rather than taken
// from dwmapi.h so an older SDK still compiles. An unsupported attribute fails
// the call, which is precisely how the Windows 10 fallback below is chosen.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCornerRound = 2; // DWMWCP_ROUND

// Fade in the way the shell's flyouts do, but only when the user has left
// animations on - SPI_GETCLIENTAREAANIMATION is the setting that turns them off.
constexpr DWORD kFadeMs = 130;

} // namespace

InfoPanel::InfoPanel(HINSTANCE instance, const BatteryHistory& history, bool light_theme)
    : instance_(instance), history_(history), light_theme_(light_theme) {
    // The shell's own menu and flyout surfaces, read off the Windows 11 palette:
    // #2C2C2C / #F9F9F9 for the background, secondary text at roughly 60% of the
    // way to it, and a divider barely a shade off the surface. Two flat sets
    // rather than anything theme aware at runtime, matching the tray icon's
    // read-once colour (see util::system_uses_light_theme).
    colors_ = light_theme ? Palette{RGB(249, 249, 249), RGB(26, 26, 26), RGB(94, 94, 94), RGB(229, 229, 229),
                                    RGB(217, 217, 217)}
                          : Palette{RGB(44, 44, 44), RGB(255, 255, 255), RGB(205, 205, 205),
                                    RGB(62, 62, 62), RGB(66, 66, 66)};
}

InfoPanel::~InfoPanel() {
    if (window_) {
        DestroyWindow(window_);
    }
}

void InfoPanel::toggle(HWND owner, UINT icon_id) {
    // Clicking the tray icon takes the foreground away from the panel first, so
    // by the time the second click's message arrives it has already hidden
    // itself on WM_ACTIVATE. Without this window a double click on an open panel
    // would close and immediately reopen it, which reads as "nothing happened".
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
    // CS_DROPSHADOW is what lifts a popup off the wallpaper the way every menu
    // and flyout in the shell is lifted; without it the panel reads as a
    // rectangle pasted onto the screen.
    window_class.style = CS_DROPSHADOW;
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
    if (!window_) {
        return false;
    }

    // Let DWM round and outline the window instead of doing either by hand. It
    // is the same rounding the shell gives its own flyouts, it is antialiased
    // against whatever is behind the panel, and the hairline border comes with
    // it. Windows 10 has none of these attributes and fails the call, which is
    // what picks the hand cut region and the drawn border further down.
    const DWORD corner = kDwmCornerRound;
    dwm_rounded_ =
        SUCCEEDED(DwmSetWindowAttribute(window_, kDwmWindowCornerPreference, &corner, sizeof(corner)));
    if (dwm_rounded_) {
        const BOOL dark = light_theme_ ? FALSE : TRUE;
        DwmSetWindowAttribute(window_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
        const COLORREF border = colors_.border;
        DwmSetWindowAttribute(window_, kDwmBorderColor, &border, sizeof(border));
    }
    return true;
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

    LOGFONTW body = metrics.lfMessageFont;
    body.lfHeight = MulDiv(body.lfHeight, kBodyScaleNumerator, kBodyScaleDenominator);
    LOGFONTW title = body;
    title.lfHeight *= 2; // the percentage leads the panel
    font_.reset(CreateFontIndirectW(&body));
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
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    if (!animate || !AnimateWindow(window_, kFadeMs, AW_BLEND)) {
        ShowWindow(window_, SW_SHOW);
    }
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
        rows_.push_back({RowStyle::Entry, charging ? L"充电功率" : L"当前功耗", buffer});

        if (full_mwh != 0) {
            rows_.push_back(
                {RowStyle::Entry, L"每 1% 约", format_duration(full_mwh / 100.0 / magnitude * 3600.0)});
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
    const size_t steps = std::min<size_t>(history_.size(), 3);
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
            // and line the panel up with the icon along the other axis.
            origin.y = side == 0 ? work.top + gap : work.bottom - gap - size.cy;
            origin.x = anchor.right - size.cx;
        } else {
            origin.x = side == 2 ? work.left + gap : work.right - gap - size.cx;
            origin.y = anchor.bottom - size.cy;
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

    // Only where DWM would not round the window itself: a region is a hard
    // one-bit clip, so its corners are visibly stepped next to the composited
    // ones. On Windows 11 the corners are DWM's and the region stays unset.
    if (!dwm_rounded_) {
        const int corner = scale(kCornerDip) * 2;
        SetWindowRgn(window_, CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, corner, corner), FALSE);
    }

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
    if (dwm_rounded_) {
        // The corners and the hairline are DWM's; drawing a border here would
        // put a second outline just inside the system's own.
        FillRect(dc, &client, background.get());
    } else {
        const int corner = scale(kCornerDip) * 2;
        RoundRect(dc, client.left, client.top, client.right, client.bottom, corner, corner);
    }

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
    DrawTextW(dc, state_.charging ? L"正在充电" : L"使用电池", -1, &line,
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
            line = {left + (row.style == RowStyle::Detail ? scale(kIndentDip) : 0), y, right, y + height};
            // Label in the primary colour, value in the secondary one, the way
            // the shell's own settings rows read: the label is what you scan
            // for, the number is what you land on.
            SetTextColor(dc, row.style == RowStyle::Entry ? colors_.text : colors_.secondary);
            DrawTextW(dc, row.label.c_str(), -1, &line, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            if (!row.value.empty()) {
                SetTextColor(dc, colors_.secondary);
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

    case WM_PRINTCLIENT: {
        // AnimateWindow may ask for the panel's image rather than let it paint
        // itself, and WM_ERASEBKGND alone would hand it an empty rectangle.
        RECT client{};
        GetClientRect(window_, &client);
        paint(reinterpret_cast<HDC>(wparam), client);
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
