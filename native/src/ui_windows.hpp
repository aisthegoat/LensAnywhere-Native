#pragma once

#include "settings.hpp"
#include <functional>
#include <windows.h>

struct UiCallbacks {
    std::function<AppSettings()> get_settings;
    std::function<void(const AppSettings&)> on_settings_changed;
    std::function<void()> on_window_closed;
};

namespace AppUi {
    void ShowSettingsWindow(HINSTANCE hInst, const UiCallbacks& cb);
    void ShowWelcomeWindow(HINSTANCE hInst, const UiCallbacks& cb);
    void CloseAll();
}
