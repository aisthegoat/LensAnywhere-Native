#include "settings.hpp"
#include "tunnel.hpp"
#include "overlay.hpp"
#include "ui_windows.hpp"
#include "../resources/resource.h"

#include <gdiplus.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

namespace {

constexpr wchar_t kTrayClassName[] = L"LensAnywhereTrayHostWnd";
constexpr UINT WM_TRAYICON = WM_USER + 1;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 1;
constexpr UINT_PTR kTunnelTimerId = 1;
constexpr int kHotkeyId = 1;

constexpr UINT IDM_CAPTURE = 1001;
constexpr UINT IDM_SETTINGS = 1002;
constexpr UINT IDM_EXIT = 1003;

HINSTANCE g_hInst = nullptr;
HWND g_tray_hwnd = nullptr;
AppSettings g_settings;
bool g_hotkey_registered = false;
UINT g_taskbar_created_msg = 0;
NOTIFYICONDATAW g_nid{};

void TrimMemory() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void UpdateTrayIcon(bool add_new) {
    if (!g_tray_hwnd) return;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    if (!g_nid.hIcon) {
        g_nid.hIcon = static_cast<HICON>(LoadImageW(
            g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR
        ));
    }
    const wchar_t* tip = Tunnel::IsServerReady() ? L"LensAnywhere (Ready)" : L"LensAnywhere (Connecting Tunnel...)";
    std::wcsncpy(g_nid.szTip, tip, sizeof(g_nid.szTip) / sizeof(wchar_t) - 1);
    Shell_NotifyIconW(add_new ? NIM_ADD : NIM_MODIFY, &g_nid);
}

void ShowTrayBalloon(const std::wstring& title, const std::wstring& msg, bool is_warning) {
    if (!g_tray_hwnd) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.cbSize = sizeof(nid);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = is_warning ? NIIF_WARNING : NIIF_INFO;
    std::wcsncpy(nid.szInfoTitle, title.c_str(), sizeof(nid.szInfoTitle) / sizeof(wchar_t) - 1);
    std::wcsncpy(nid.szInfo, msg.c_str(), sizeof(nid.szInfo) / sizeof(wchar_t) - 1);
    nid.szInfoTitle[63] = L'\0';
    nid.szInfo[255] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void SyncHotkeyRegistration() {
    if (!g_tray_hwnd) return;
    if (g_hotkey_registered) {
        UnregisterHotKey(g_tray_hwnd, kHotkeyId);
        g_hotkey_registered = false;
    }
    if (g_settings.hotkey_enabled && Tunnel::IsServerReady()) {
        UINT mods = 0, vk = 0;
        if (g_settings.ToWin32Hotkey(mods, vk)) {
            if (RegisterHotKey(g_tray_hwnd, kHotkeyId, mods, vk)) {
                g_hotkey_registered = true;
            }
        }
    }
}

UiCallbacks MakeUiCallbacks() {
    UiCallbacks cb;
    cb.get_settings = []() { return g_settings; };
    cb.on_settings_changed = [](const AppSettings& updated) {
        g_settings = updated;
        SyncHotkeyRegistration();
    };
    cb.on_window_closed = []() {
        TrimMemory();
    };
    return cb;
}

void OpenSettings() {
    AppUi::ShowSettingsWindow(g_hInst, MakeUiCallbacks());
}

void TriggerCapture() {
    if (!Tunnel::IsServerReady()) {
        ShowTrayBalloon(L"LensAnywhere", L"Cloudflare Tunnel initializing... Please wait a few seconds.", true);
        return;
    }
    OverlayCallbacks cb;
    cb.get_settings = []() { return g_settings; };
    cb.on_open_settings = []() { OpenSettings(); };
    cb.on_notify = [](const std::wstring& title, const std::wstring& msg, bool warn) {
        ShowTrayBalloon(title, msg, warn);
    };
    cb.on_closed = []() {
        TrimMemory();
    };
    CaptureOverlay::Show(g_hInst, g_tray_hwnd, cb);
}

void ShowTrayContextMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_CAPTURE, L"Capture Screen");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit LensAnywhere");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case IDM_CAPTURE:  TriggerCapture(); break;
        case IDM_SETTINGS: OpenSettings(); break;
        case IDM_EXIT:     DestroyWindow(hwnd); break;
        default: break;
    }
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbar_created_msg != 0 && msg == g_taskbar_created_msg) {
        UpdateTrayIcon(true);
        return 0;
    }

    switch (msg) {
        case WM_HOTKEY:
            if (wp == kHotkeyId) {
                TriggerCapture();
            }
            return 0;

        case WM_APP_SHOW_SETTINGS:
            OpenSettings();
            return 0;

        case WM_TIMER:
            if (wp == kTunnelTimerId && Tunnel::IsServerReady()) {
                KillTimer(hwnd, kTunnelTimerId);
                SyncHotkeyRegistration();
                UpdateTrayIcon(false);
                TrimMemory();
            }
            return 0;

        case WM_TRAYICON:
            switch (LOWORD(lp)) {
                case WM_LBUTTONDBLCLK:
                    TriggerCapture();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayContextMenu(hwnd);
                    break;
                default:
                    break;
            }
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wp) {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTunnelTimerId);
            if (g_hotkey_registered) {
                UnregisterHotKey(hwnd, kHotkeyId);
                g_hotkey_registered = false;
            }
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_nid.hIcon) {
                DestroyIcon(g_nid.hIcon);
                g_nid.hIcon = nullptr;
            }
            CaptureOverlay::Close();
            AppUi::CloseAll();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInst = hInstance;

    HANDLE single_mutex = CreateMutexW(nullptr, FALSE, L"Local\\LensAnywhere_SingleInstance");
    if (single_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kTrayClassName, nullptr);
        if (existing) {
            PostMessageW(existing, WM_APP_SHOW_SETTINGS, 0, 0);
        }
        CloseHandle(single_mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Gdiplus::GdiplusStartupInput gdip_input;
    ULONG_PTR gdip_token = 0;
    Gdiplus::GdiplusStartup(&gdip_token, &gdip_input, nullptr);

    g_settings = LoadSettings();
    Tunnel::StartBackend();

    g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kTrayClassName;
    RegisterClassExW(&wc);

    g_tray_hwnd = CreateWindowExW(
        0, kTrayClassName, L"LensAnywhereHost",
        WS_OVERLAPPED, 0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!g_tray_hwnd) {
        Tunnel::ShutdownBackend();
        Gdiplus::GdiplusShutdown(gdip_token);
        CoUninitialize();
        if (single_mutex) CloseHandle(single_mutex);
        return 1;
    }

    UpdateTrayIcon(true);
    SetTimer(g_tray_hwnd, kTunnelTimerId, 500, nullptr);

    if (!g_settings.onboarding_completed) {
        AppUi::ShowWelcomeWindow(hInstance, MakeUiCallbacks());
    } else {
        TrimMemory();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Tunnel::ShutdownBackend();
    Gdiplus::GdiplusShutdown(gdip_token);
    CoUninitialize();
    if (single_mutex) CloseHandle(single_mutex);
    return 0;
}
