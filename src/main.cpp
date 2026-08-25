// BatteryTray - shows the battery level as a tray icon.

#include <initguid.h> // must precede win32.h so the power setting GUIDs get defined here

#include "win32.h"

#include <shellapi.h>
#include <windowsx.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "autostart.h"
#include "battery_history.h"
#include "battery_log.h"
#include "battery_power.h"
#include "info_panel.h"
#include "tray_icon.h"
#include "util.h"
#include "win32_raii.h"

namespace {

using namespace bt;

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

constexpr int kMenuLog = 1;
constexpr int kMenuAutostart = 2;
constexpr int kMenuExit = 3;

constexpr wchar_t kWindowClassName[] = L"BatteryTray.window";
constexpr wchar_t kSingletonMutexName[] = L"Local\\BatteryTray.singleton";

// Sent by the shell after explorer restarts; every tray program has to re-add
// its icon then.
UINT g_taskbar_created = 0;

std::wstring display_text(int percent) {
    return percent > 99 ? std::wstring(L"FL") : std::to_wstring(percent);
}

// The tray tooltip is where "how long does one percent last" gets answered:
// full charge / 100 is the energy in one percent, and the driver's rate is how
// fast that energy is moving right now.
std::wstring build_tooltip(const BatteryState& state) {
    std::wstring tip = state.charging ? L"正在充电：" : L"使用电池：";
    tip += display_text(state.percent);
    tip += L'%';

    // Opened and dropped again around a single sample: unlike the info panel,
    // this runs on a hover, and holding the device between hovers would keep a
    // handle alive through hours of idling for nothing.
    BatteryDevice device;
    BatterySample sample;
    if (!device.open() || !device.read_sample(sample)) {
        return tip;
    }
    const long rate = sample.rate_mw < 0 ? -sample.rate_mw : sample.rate_mw;
    const unsigned long full_mwh = device.facts().full_charge_mwh;
    // No battery, a driver that reports neither, or a pack sitting at full on
    // AC with nothing flowing: the percentage alone is all there is to say.
    if (rate == 0 || full_mwh == 0) {
        return tip;
    }

    wchar_t line[64];
    const double one_percent_seconds = full_mwh / 100.0 / rate * 3600.0;
    swprintf_s(line, L"\r\n%s %.1f W · 每 1%% 约 %s", sample.rate_mw > 0 ? L"充电" : L"功耗", rate / 1000.0,
               format_duration(one_percent_seconds).c_str());
    tip += line;

    // Direction comes from the rate, not from ACLineStatus: a full pack on AC
    // is plugged in without charging.
    const bool charging = sample.rate_mw > 0;
    const unsigned long energy =
        charging ? (full_mwh > sample.remaining_mwh ? full_mwh - sample.remaining_mwh : 0)
                 : sample.remaining_mwh;
    if (energy != 0) {
        swprintf_s(line, L"\r\n预计%s %s", charging ? L"充满" : L"剩余",
                   format_duration(energy / static_cast<double>(rate) * 3600.0).c_str());
        tip += line;
    }
    return tip;
}

void set_tooltip(NOTIFYICONDATAW& data, const BatteryState& state) {
    const std::wstring tip = build_tooltip(state);
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
}

class App {
public:
    explicit App(HINSTANCE instance)
        : instance_(instance), light_theme_(system_uses_light_theme()),
          renderer_(light_theme_ ? RGB(0, 0, 0) : RGB(255, 255, 255)),
          panel_(instance, history_, light_theme_) {}

    bool create_window();
    int run();

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    void add_tray_icon();
    void remove_tray_icon();
    void refresh(bool force);
    void refresh_tooltip();
    void show_menu(int x, int y);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    bool light_theme_;
    IconRenderer renderer_;
    unique_icon icon_;
    BatteryLog log_;
    BatteryHistory history_;
    InfoPanel panel_;
    HPOWERNOTIFY power_notify_ = nullptr;
    int last_percent_ = -1;
    bool last_charging_ = false;
    bool has_last_ = false;
};

bool App::create_window() {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &App::window_proc;
    window_class.hInstance = instance_;
    window_class.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&window_class)) {
        return false;
    }

    // A real top-level window that is simply never shown. HWND_MESSAGE would be
    // cheaper but message-only windows receive no broadcasts, and both
    // PBT_APMPOWERSTATUSCHANGE and TaskbarCreated arrive that way.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClassName, L"BatteryTray", WS_OVERLAPPED, 0, 0, 0, 0,
                              nullptr, nullptr, instance_, this);
    return window_ != nullptr;
}

int App::run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK App::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (app) {
        return app->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT App::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_taskbar_created && g_taskbar_created != 0) {
        add_tray_icon();
        refresh(true);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        // Event driven instead of polling: the system pushes a message when the
        // percentage moves, so the process is idle in between.
        power_notify_ = RegisterPowerSettingNotification(window_, &GUID_BATTERY_PERCENTAGE_REMAINING,
                                                        DEVICE_NOTIFY_WINDOW_HANDLE);
        add_tray_icon();
        refresh(true);
        return 0;

    case WM_POWERBROADCAST:
        // PBT_POWERSETTINGCHANGE covers the percentage, PBT_APMPOWERSTATUSCHANGE
        // the charger being plugged or pulled.
        if (wparam == PBT_POWERSETTINGCHANGE || wparam == PBT_APMPOWERSTATUSCHANGE) {
            refresh(false);
        } else if (wparam == PBT_APMSUSPEND) {
            // A percent step that spans a sleep took eight hours of wall clock
            // and no use at all; the resume needs no handler of its own, because
            // the next percent change restarts the interval either way.
            history_.discard_pending();
        }
        return TRUE;

    case WM_DPICHANGED:
        refresh(true); // the tray icon size changed with the DPI
        return 0;

    case kTrayCallbackMessage:
        switch (LOWORD(lparam)) {
        case WM_CONTEXTMENU:
            show_menu(GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam));
            break;
        case WM_LBUTTONDBLCLK:
        case NIN_KEYSELECT:
            // Double click only. A single click on a tray icon is the easiest
            // thing on the taskbar to hit by accident, and NIN_KEYSELECT is the
            // keyboard's equivalent of the double click.
            panel_.toggle(window_, kTrayIconId);
            break;
        case NIN_POPUPOPEN:
            // The draw shown in the tooltip is instantaneous, so a value left
            // over from the last percent change would be minutes stale. The
            // shell sends this as the tooltip is about to appear, which is the
            // only hook there is for sampling at the moment someone looks.
            refresh_tooltip();
            break;
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        remove_tray_icon();
        if (power_notify_) {
            UnregisterPowerSettingNotification(power_notify_);
            power_notify_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void App::add_tray_icon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    Shell_NotifyIconW(NIM_ADD, &data);

    data.uVersion = NOTIFYICON_VERSION_4; // gives WM_CONTEXTMENU with screen coordinates
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void App::remove_tray_icon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void App::refresh(bool force) {
    const BatteryState state = query_battery_state();
    // Fed unconditionally: the ring itself ignores readings that did not move,
    // and it has to keep filling while the panel is closed - that is what makes
    // the elapsed steps there worth anything the moment it opens.
    history_.observe(state.percent, state.charging);
    const bool changed = !has_last_ || state.percent != last_percent_ || state.charging != last_charging_;
    // Power events repeat; rebuilding the icon for an unchanged reading would be
    // the only work this program ever does at idle.
    if (!changed && !force) {
        return;
    }

    const std::wstring text = display_text(state.percent);
    // Our hidden window sits on the primary monitor, which is where the taskbar
    // normally is; if the tray lives on a monitor with a different DPI the shell
    // rescales the icon, which is why this is not worth chasing further.
    unique_icon icon = renderer_.render(text, GetDpiForWindow(window_));

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    if (icon) {
        data.uFlags |= NIF_ICON;
        data.hIcon = icon.get();
    }
    set_tooltip(data, state);
    Shell_NotifyIconW(NIM_MODIFY, &data);

    // Only now is the previous icon safe to destroy: the shell has been handed
    // the replacement.
    if (icon) {
        icon_ = std::move(icon);
    }

    if (!changed) {
        return;
    }
    last_percent_ = state.percent;
    last_charging_ = state.charging;
    has_last_ = true;
    log_.append_status(text, state.charging);
}

void App::refresh_tooltip() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    set_tooltip(data, query_battery_state());
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::show_menu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    // Rebuilt on every click so the check marks reflect state that may have been
    // changed elsewhere, such as the Run key.
    AppendMenuW(menu, MF_STRING | (log_.enabled() ? MF_CHECKED : MF_UNCHECKED), kMenuLog, L"电量日志");
    AppendMenuW(menu, MF_STRING | (autostart::is_enabled() ? MF_CHECKED : MF_UNCHECKED), kMenuAutostart,
                L"开机启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

    // TrackPopupMenu only dismisses on an outside click while our window is in
    // the foreground; the trailing post is the documented other half of that
    // workaround.
    SetForegroundWindow(window_);
    const int command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, window_,
                                         nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);

    switch (command) {
    case kMenuLog:
        if (log_.enabled()) {
            log_.stop();
        } else {
            log_.start(); // stays unchecked when the directory is not writable
        }
        break;

    case kMenuAutostart:
        autostart::set_enabled(!autostart::is_enabled()); // a failure just leaves the state as it was
        break;

    case kMenuExit:
        DestroyWindow(window_);
        break;

    default:
        break;
    }
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    // Optional single instance guard: a second copy would just stack another
    // identical icon in the tray.
    const unique_kernel_handle singleton(CreateMutexW(nullptr, TRUE, kSingletonMutexName));
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    App app(instance);
    if (!app.create_window()) {
        return 1;
    }
    return app.run();
}
