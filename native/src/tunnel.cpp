#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "tunnel.hpp"
#include "settings.hpp"
#include "../resources/resource.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kDefaultPort = 8998;

std::atomic<bool> g_running{false};
std::atomic<bool> g_server_ready{false};
std::atomic<uint16_t> g_http_port{kDefaultPort};

std::mutex g_state_mutex;
std::vector<uint8_t> g_current_image;
std::string g_public_url;

std::atomic<SOCKET> g_listen_sock{INVALID_SOCKET};
HANDLE g_job_handle = nullptr;
HANDLE g_cf_process = nullptr;
std::thread g_http_thread;
std::thread g_cf_thread;

std::wstring EnsureCloudflaredPath() {
    wchar_t exe_dir_buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exe_dir_buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::wstring exe_dir(exe_dir_buf, len);
        size_t pos = exe_dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring local_cf = exe_dir.substr(0, pos + 1) + L"cloudflared.exe";
            if (GetFileAttributesW(local_cf.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return local_cf;
            }
        }
    }

    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(IDR_CLOUDFLARED_EXE), RT_RCDATA);
    if (!hRes) return {};
    DWORD res_size = SizeofResource(hMod, hRes);
    if (res_size == 0) return {};

    std::wstring data_dir = GetLocalDataDir();
    SHCreateDirectoryExW(nullptr, data_dir.c_str(), nullptr);
    std::wstring target = data_dir + L"\\cloudflared.exe";

    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &attr)) {
        uint64_t existing_size = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
        if (existing_size == res_size) {
            return target;
        }
    }

    HGLOBAL hLoaded = LoadResource(hMod, hRes);
    if (!hLoaded) return {};
    const void* res_ptr = LockResource(hLoaded);
    if (!res_ptr) return {};

    std::wstring tmp = target + L".tmp";
    HANDLE hFile = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    const uint8_t* bytes = static_cast<const uint8_t*>(res_ptr);
    DWORD remaining = res_size;
    DWORD offset = 0;
    bool ok = true;
    while (remaining > 0) {
        DWORD chunk = remaining > (4u * 1024u * 1024u) ? (4u * 1024u * 1024u) : remaining;
        DWORD written = 0;
        if (!WriteFile(hFile, bytes + offset, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += written;
        remaining -= written;
    }
    if (ok) FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (!ok) {
        DeleteFileW(tmp.c_str());
        return {};
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return target;
        }
        return {};
    }
    return target;
}

bool SendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int chunk = static_cast<int>((len - sent) > 65536 ? 65536 : (len - sent));
        int rc = send(s, data + sent, chunk, 0);
        if (rc <= 0) return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

void HandleHttpClient(SOCKET client) {
    DWORD tv = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    std::string req;
    req.reserve(1024);
    char buf[1024];
    while (req.size() < 8192) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, buf + n);
        if (req.find("\r\n\r\n") != std::string::npos) break;
    }

    bool is_image_get = (req.rfind("GET /image.png", 0) == 0);
    if (is_image_get) {
        std::vector<uint8_t> img_copy;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            img_copy = g_current_image;
        }
        if (!img_copy.empty()) {
            char header[256];
            int hlen = std::snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/png\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Connection: close\r\n\r\n",
                img_copy.size()
            );
            if (hlen > 0 && SendAll(client, header, static_cast<size_t>(hlen))) {
                SendAll(client, reinterpret_cast<const char*>(img_copy.data()), img_copy.size());
            }
        } else {
            const char resp[] =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 19\r\n"
                "Connection: close\r\n\r\n"
                "No image captured\r\n";
            SendAll(client, resp, sizeof(resp) - 1);
        }
    } else {
        const char resp[] =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 9\r\n"
            "Connection: close\r\n\r\n"
            "Not Found";
        SendAll(client, resp, sizeof(resp) - 1);
    }

    closesocket(client);
}

void HttpServerLoop() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(kDefaultPort);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        addr.sin_port = 0;
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(s);
            WSACleanup();
            return;
        }
    }

    int namelen = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &namelen) == 0) {
        g_http_port.store(ntohs(addr.sin_port));
    }

    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(s);
        WSACleanup();
        return;
    }

    g_listen_sock.store(s);

    while (g_running.load()) {
        SOCKET client = accept(s, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            break;
        }
        std::thread(HandleHttpClient, client).detach();
    }

    SOCKET to_close = g_listen_sock.exchange(INVALID_SOCKET);
    if (to_close != INVALID_SOCKET) {
        closesocket(to_close);
    }
    WSACleanup();
}

std::wstring BuildTunedEnvironmentBlock() {
    LPWCH env = GetEnvironmentStringsW();
    std::wstring block;
    if (env) {
        const wchar_t* p = env;
        while (*p) {
            size_t len = std::wcslen(p);
            std::wstring entry(p, len);
            if (_wcsnicmp(entry.c_str(), L"GOMEMLIMIT=", 11) != 0 &&
                _wcsnicmp(entry.c_str(), L"GOGC=", 5) != 0) {
                block.append(entry);
                block.push_back(L'\0');
            }
            p += len + 1;
        }
        FreeEnvironmentStringsW(env);
    }
    block.append(L"GOMEMLIMIT=16MiB");
    block.push_back(L'\0');
    block.append(L"GOGC=20");
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

std::string ExtractTryCloudflareUrl(const std::string& text) {
    const std::string prefix = "https://";
    const std::string suffix = ".trycloudflare.com";
    size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
        size_t host_start = pos + prefix.size();
        size_t end = host_start;
        while (end < text.size()) {
            char c = text[end];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.') {
                ++end;
            } else {
                break;
            }
        }
        std::string candidate = text.substr(pos, end - pos);
        if (candidate.size() > prefix.size() + suffix.size() &&
            candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return candidate;
        }
        pos = host_start;
    }
    return {};
}

void CloudflareLoop() {
    std::wstring cf_exe = EnsureCloudflaredPath();
    if (cf_exe.empty()) return;

    if (!g_job_handle) {
        g_job_handle = CreateJobObjectW(nullptr, nullptr);
        if (g_job_handle) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_job_handle, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }
    }

    while (g_running.load()) {
        while (g_running.load() && g_listen_sock.load() == INVALID_SOCKET) {
            Sleep(50);
        }
        if (!g_running.load()) break;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe = nullptr;
        HANDLE hWritePipe = nullptr;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
            Sleep(1000);
            continue;
        }
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;
        si.hStdInput = nullptr;

        PROCESS_INFORMATION pi{};
        wchar_t port_buf[16];
        std::swprintf(port_buf, 16, L"%u", static_cast<unsigned>(g_http_port.load()));
        std::wstring cmd = L"\"" + cf_exe + L"\" tunnel --no-autoupdate --url http://127.0.0.1:" + port_buf;
        std::wstring env_block = BuildTunedEnvironmentBlock();

        BOOL created = CreateProcessW(
            cf_exe.c_str(),
            cmd.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            env_block.data(),
            nullptr,
            &si,
            &pi
        );
        CloseHandle(hWritePipe);

        if (!created) {
            CloseHandle(hReadPipe);
            Sleep(2000);
            continue;
        }

        if (g_job_handle) {
            AssignProcessToJobObject(g_job_handle, pi.hProcess);
        }
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_cf_process = pi.hProcess;
        }
        CloseHandle(pi.hThread);

        std::string accum;
        char chunk[1024];
        DWORD nread = 0;
        bool url_found = false;
        while (g_running.load() && ReadFile(hReadPipe, chunk, sizeof(chunk), &nread, nullptr) && nread > 0) {
            if (!url_found) {
                accum.append(chunk, chunk + nread);
                std::string url = ExtractTryCloudflareUrl(accum);
                if (!url.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(g_state_mutex);
                        g_public_url = url;
                    }
                    g_server_ready.store(true);
                    url_found = true;
                    accum.clear();
                    accum.shrink_to_fit();
                } else if (accum.size() > 8192) {
                    accum.erase(0, accum.size() - 512);
                }
            }
        }
        CloseHandle(hReadPipe);

        WaitForSingleObject(pi.hProcess, INFINITE);
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_cf_process = nullptr;
            g_public_url.clear();
        }
        g_server_ready.store(false);
        CloseHandle(pi.hProcess);

        if (g_running.load()) {
            Sleep(1500);
        }
    }
}

std::string UrlEncode(const std::string& input) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

namespace Tunnel {

void StartBackend() {
    if (g_running.exchange(true)) return;
    g_http_thread = std::thread(HttpServerLoop);
    g_cf_thread = std::thread(CloudflareLoop);
}

void ShutdownBackend() {
    if (!g_running.exchange(false)) return;
    g_server_ready.store(false);

    SOCKET ls = g_listen_sock.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) {
        closesocket(ls);
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_cf_process) {
            TerminateProcess(g_cf_process, 0);
        }
    }

    if (g_job_handle) {
        CloseHandle(g_job_handle);
        g_job_handle = nullptr;
    }

    if (g_http_thread.joinable()) g_http_thread.join();
    if (g_cf_thread.joinable()) g_cf_thread.join();
}

bool IsServerReady() {
    return g_server_ready.load();
}

void SearchLens(std::vector<uint8_t> png_bytes) {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_current_image = std::move(png_bytes);
    }

    std::thread([]() {
        auto start = std::chrono::steady_clock::now();
        while (!g_server_ready.load() && g_running.load()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start
            ).count();
            if (elapsed >= 10) break;
            Sleep(100);
        }

        std::string pub_url;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            pub_url = g_public_url;
        }
        if (pub_url.empty()) return;

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        std::string img_url = pub_url + "/image.png?t=" + std::to_string(ms);
        std::string lens_url = "https://lens.google.com/uploadbyurl?url=" + UrlEncode(img_url);

        std::wstring wide_url = Utf8ToWide(lens_url);
        ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }).detach();
}

}  // namespace Tunnel
