#pragma once

#include <string>
#include <windows.h>

struct AppSettings {
    bool onboarding_completed = false;
    bool hotkey_enabled = true;
    bool ctrl = true;
    bool shift = true;
    bool win = false;
    bool alt = true;
    char key = 'S';
    bool save_to_clipboard = false;
    std::wstring save_location;
    bool launch_on_startup = true;

    std::wstring HotkeyDisplayString() const;
    bool ToWin32Hotkey(UINT& out_modifiers, UINT& out_vk) const;
};

std::wstring GetSettingsPath();
std::wstring GetLocalDataDir();
std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

AppSettings LoadSettings();
bool SaveSettings(const AppSettings& s);

namespace StartupManager {
    bool SetEnabled(bool enable);
}
