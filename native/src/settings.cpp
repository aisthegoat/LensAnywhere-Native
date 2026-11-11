#include "settings.hpp"

#include <shlobj.h>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAppName[] = L"LensAnywhere";

std::wstring GetKnownFolderOrEnv(REFKNOWNFOLDERID fid, const wchar_t* env_name) {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(fid, KF_FLAG_CREATE, nullptr, &path)) && path) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(env_name, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(buf, len);
    }
    return L".";
}

bool EnsureDirectoryExists(const std::wstring& dir) {
    if (dir.empty()) return false;
    int res = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return res == ERROR_SUCCESS || res == ERROR_ALREADY_EXISTS || res == ERROR_FILE_EXISTS;
}

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

void SkipWhitespace(const std::string& src, size_t& pos) {
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' || src[pos] == '\n')) {
        ++pos;
    }
}

void AppendCodepointUtf8(uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool ParseHex4(const std::string& src, size_t& pos, uint32_t& out) {
    if (pos + 4 > src.size()) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = src[pos + i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else return false;
    }
    pos += 4;
    out = v;
    return true;
}

bool ParseJsonString(const std::string& src, size_t& pos, std::string& out) {
    if (pos >= src.size() || src[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < src.size()) {
        char c = src[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= src.size()) return false;
            char esc = src[pos++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(src, pos, cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 2 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                        size_t save = pos;
                        pos += 2;
                        uint32_t low = 0;
                        if (ParseHex4(src, pos, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        } else {
                            pos = save;
                        }
                    }
                    AppendCodepointUtf8(cp, out);
                    break;
                }
                default:
                    return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool SkipJsonValue(const std::string& src, size_t& pos) {
    SkipWhitespace(src, pos);
    if (pos >= src.size()) return false;
    char c = src[pos];
    if (c == '"') {
        std::string dummy;
        return ParseJsonString(src, pos, dummy);
    }
    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 1;
        ++pos;
        while (pos < src.size() && depth > 0) {
            if (src[pos] == '"') {
                std::string dummy;
                if (!ParseJsonString(src, pos, dummy)) return false;
                continue;
            }
            if (src[pos] == open) ++depth;
            else if (src[pos] == close) --depth;
            ++pos;
        }
        return depth == 0;
    }
    if (src.compare(pos, 4, "true") == 0) { pos += 4; return true; }
    if (src.compare(pos, 5, "false") == 0) { pos += 5; return true; }
    if (src.compare(pos, 4, "null") == 0) { pos += 4; return true; }
    while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) ||
           src[pos] == '-' || src[pos] == '+' || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E')) {
        ++pos;
    }
    return true;
}

bool ParseSettingsJson(const std::string& src, AppSettings& out) {
    size_t pos = 0;
    if (src.size() >= 3 &&
        static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        pos = 3;
    }
    SkipWhitespace(src, pos);
    if (pos >= src.size() || src[pos] != '{') return false;
    ++pos;

    AppSettings parsed;
    while (true) {
        SkipWhitespace(src, pos);
        if (pos >= src.size()) return false;
        if (src[pos] == '}') {
            ++pos;
            out = parsed;
            return true;
        }
        std::string key;
        if (!ParseJsonString(src, pos, key)) return false;
        SkipWhitespace(src, pos);
        if (pos >= src.size() || src[pos] != ':') return false;
        ++pos;
        SkipWhitespace(src, pos);
        if (pos >= src.size()) return false;

        auto read_bool = [&](bool& target) -> bool {
            if (src.compare(pos, 4, "true") == 0) { pos += 4; target = true; return true; }
            if (src.compare(pos, 5, "false") == 0) { pos += 5; target = false; return true; }
            return SkipJsonValue(src, pos);
        };

        if (key == "onboarding_completed") {
            if (!read_bool(parsed.onboarding_completed)) return false;
        } else if (key == "hotkey_enabled") {
            if (!read_bool(parsed.hotkey_enabled)) return false;
        } else if (key == "ctrl") {
            if (!read_bool(parsed.ctrl)) return false;
        } else if (key == "shift") {
            if (!read_bool(parsed.shift)) return false;
        } else if (key == "win") {
            if (!read_bool(parsed.win)) return false;
        } else if (key == "alt") {
            if (!read_bool(parsed.alt)) return false;
        } else if (key == "save_to_clipboard") {
            if (!read_bool(parsed.save_to_clipboard)) return false;
        } else if (key == "launch_on_startup") {
            if (!read_bool(parsed.launch_on_startup)) return false;
        } else if (key == "key") {
            if (src[pos] == '"') {
                std::string val;
                if (!ParseJsonString(src, pos, val)) return false;
                if (val.size() == 1) {
                    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(val[0])));
                    if (upper >= 'A' && upper <= 'Z') parsed.key = upper;
                }
            } else if (!SkipJsonValue(src, pos)) {
                return false;
            }
        } else if (key == "save_location") {
            if (src[pos] == '"') {
                std::string val;
                if (!ParseJsonString(src, pos, val)) return false;
                parsed.save_location = Utf8ToWide(val);
            } else if (!SkipJsonValue(src, pos)) {
                return false;
            }
        } else {
            if (!SkipJsonValue(src, pos)) return false;
        }

        SkipWhitespace(src, pos);
        if (pos < src.size() && src[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < src.size() && src[pos] == '}') {
            ++pos;
            out = parsed;
            return true;
        }
        return false;
    }
}

}  // namespace

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring AppSettings::HotkeyDisplayString() const {
    std::wstring s;
    auto append = [&](const wchar_t* part) {
        if (!s.empty()) s += L"+";
        s += part;
    };
    if (ctrl) append(L"Ctrl");
    if (shift) append(L"Shift");
    if (alt) append(L"Alt");
    if (win) append(L"Win");
    if (!s.empty()) s += L"+";
    s.push_back(static_cast<wchar_t>(key));
    return s;
}

bool AppSettings::ToWin32Hotkey(UINT& out_modifiers, UINT& out_vk) const {
    if (key < 'A' || key > 'Z') return false;
    UINT mods = MOD_NOREPEAT;
    if (alt) mods |= MOD_ALT;
    if (ctrl) mods |= MOD_CONTROL;
    if (shift) mods |= MOD_SHIFT;
    if (win) mods |= MOD_WIN;
    out_modifiers = mods;
    out_vk = static_cast<UINT>(key);
    return true;
}

std::wstring GetSettingsPath() {
    std::wstring base = GetKnownFolderOrEnv(FOLDERID_RoamingAppData, L"APPDATA");
    return base + L"\\LensAnywhere\\settings.json";
}

std::wstring GetLocalDataDir() {
    std::wstring base = GetKnownFolderOrEnv(FOLDERID_LocalAppData, L"LOCALAPPDATA");
    return base + L"\\LensAnywhere";
}

AppSettings LoadSettings() {
    AppSettings s;
    std::wstring path = GetSettingsPath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(hFile, &size) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024) {
            std::string content(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read = 0;
            if (ReadFile(hFile, content.data(), static_cast<DWORD>(content.size()), &read, nullptr)) {
                content.resize(read);
                AppSettings parsed;
                if (ParseSettingsJson(content, parsed)) {
                    s = parsed;
                }
            }
        }
        CloseHandle(hFile);
    }
    StartupManager::SetEnabled(s.launch_on_startup);
    return s;
}

bool SaveSettings(const AppSettings& s) {
    std::wstring path = GetSettingsPath();
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        if (!EnsureDirectoryExists(path.substr(0, slash))) return false;
    }

    auto b = [](bool v) { return v ? "true" : "false"; };
    std::ostringstream oss;
    oss << "{\n"
        << "    \"onboarding_completed\": " << b(s.onboarding_completed) << ",\n"
        << "    \"hotkey_enabled\": " << b(s.hotkey_enabled) << ",\n"
        << "    \"ctrl\": " << b(s.ctrl) << ",\n"
        << "    \"shift\": " << b(s.shift) << ",\n"
        << "    \"win\": " << b(s.win) << ",\n"
        << "    \"alt\": " << b(s.alt) << ",\n"
        << "    \"key\": \"" << static_cast<char>(s.key) << "\",\n"
        << "    \"save_to_clipboard\": " << b(s.save_to_clipboard) << ",\n"
        << "    \"save_location\": \"" << EscapeJsonString(WideToUtf8(s.save_location)) << "\",\n"
        << "    \"launch_on_startup\": " << b(s.launch_on_startup) << "\n"
        << "}";
    std::string payload = oss.str();

    std::wstring tmp = path + L".tmp";
    HANDLE hFile = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    bool ok = WriteFile(hFile, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)
              && written == payload.size()
              && FlushFileBuffers(hFile);
    CloseHandle(hFile);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

namespace StartupManager {

bool SetEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = true;
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring cmd = std::wstring(L"\"") + exePath + L"\"";
            LONG rc = RegSetValueExW(
                hKey, kAppName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))
            );
            ok = (rc == ERROR_SUCCESS);
        } else {
            ok = false;
        }
    } else {
        LONG rc = RegDeleteValueW(hKey, kAppName);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(hKey);
    return ok;
}

}  // namespace StartupManager
