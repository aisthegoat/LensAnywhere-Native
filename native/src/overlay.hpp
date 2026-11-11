#pragma once

#include "settings.hpp"
#include <functional>
#include <string>
#include <windows.h>

struct OverlayCallbacks {
    std::function<AppSettings()> get_settings;
    std::function<void()> on_open_settings;
    std::function<void(const std::wstring& title, const std::wstring& msg, bool is_warning)> on_notify;
    std::function<void()> on_closed;
};

namespace CaptureOverlay {
    void Show(HINSTANCE hInst, HWND owner_hwnd, const OverlayCallbacks& callbacks);
    void Close();
    bool IsOpen();
}
