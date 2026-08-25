// BatteryTray - shows the battery level as a tray icon.

#include <initguid.h> // must precede win32.h so the power setting GUIDs get defined here

#include "win32.h"

#include <shellapi.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "autostart.h"
#include "battery_log.h"
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

// TEMPORARY (tray glyph tuning): "--fill=85 --percent=88" overrides the box
// budget the font is fitted into and the battery reading, so the look can be
// judged without a battery that cooperates. Passing any argument also lifts the
// single instance guard, which is the point: several copies with different
// --fill values sit in the tray next to each other for comparison, and the
// tooltip carries the value so they stay tellable apart. Revert this commit
// once the number is settled.
int g_forced_percent = -1;
int g_fill_percent = kDefaultFillPercent;

int parse_tuning_option(PCWSTR command_line, PCWSTR option, int fallback) {
    const wchar_t* const found = wcsstr(command_line, option);
    return found ? _wtoi(found + wcslen(option)) : fallback;
}

struct BatteryState {
    int percent;
    bool charging;
};

BatteryState query_battery() {
    if (g_forced_percent >= 0) {
        return {g_forced_percent, false}; // TEMPORARY: tuning override
    }

    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) {
        return {100, false};
    }
    // 255 means "unknown", which is what a machine without a battery reports;
    // like the original, treat that as full rather than showing nothing.
    const int percent = status.BatteryLifePercent == 255 ? 100 : static_cast<int>(status.BatteryLifePercent);
    return {percent, status.ACLineStatus == 1};
}

std::wstring display_text(int percent) {
    return percent > 99 ? std::wstring(L"FL") : std::to_wstring(percent);
}

// Sampled once at startup, matching the original: following live theme changes
// would mean re-rendering on broadcast messages for a setting nobody flips
// while watching the tray.
COLORREF read_theme_text_color() {
    constexpr COLORREF kDarkThemeText = RGB(255, 255, 255);

    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS) {
        return kDarkThemeText;
    }
    const unique_regkey key(raw);

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key.get(), L"SystemUsesLightTheme", nullptr, &type, reinterpret_cast<BYTE*>(&value),
                         &size) != ERROR_SUCCESS ||
        type != REG_DWORD || size != sizeof(value)) {
        return kDarkThemeText;
    }
    return value == 1 ? RGB(0, 0, 0) : kDarkThemeText;
}

class App {
public:
    explicit App(HINSTANCE instance)
        : instance_(instance), renderer_(read_theme_text_color(), g_fill_percent) {}

    bool create_window();
    int run();

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    void add_tray_icon();
    void remove_tray_icon();
    void refresh(bool force);
    void show_menu(int x, int y);

    HINSTANCE instance_;
    HWND window_ = nullptr;
    IconRenderer renderer_;
    unique_icon icon_;
    BatteryLog log_;
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
        }
        return TRUE;

    case WM_DPICHANGED:
        refresh(true); // the tray icon size changed with the DPI
        return 0;

    case kTrayCallbackMessage:
        if (LOWORD(lparam) == WM_CONTEXTMENU) {
            show_menu(GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam));
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
    const BatteryState state = query_battery();
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
    // TEMPORARY: the fill value rides along so side by side copies stay apart.
    swprintf_s(data.szTip, L"%s：%s%%（fill=%d%%）", state.charging ? L"正在充电" : L"使用电池", text.c_str(),
               g_fill_percent);
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

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR command_line, _In_ int) {
    // TEMPORARY: see the tuning knobs above.
    const bool tuning = command_line != nullptr && *command_line != L'\0';
    if (tuning) {
        g_fill_percent = parse_tuning_option(command_line, L"--fill=", kDefaultFillPercent);
        g_forced_percent = parse_tuning_option(command_line, L"--percent=", -1);
    }

    // Optional single instance guard: a second copy would just stack another
    // identical icon in the tray.
    const unique_kernel_handle singleton(CreateMutexW(nullptr, TRUE, kSingletonMutexName));
    if (!tuning && (!singleton || GetLastError() == ERROR_ALREADY_EXISTS)) {
        return 0;
    }

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    App app(instance);
    if (!app.create_window()) {
        return 1;
    }
    return app.run();
}
