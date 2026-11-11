#include "overlay.hpp"
#include "tunnel.hpp"

#include <dwmapi.h>
#include <gdiplus.h>
#include <objidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr wchar_t kOverlayClassName[] = L"LensAnywhereOverlayWnd";

struct OverlayState {
    HWND hwnd = nullptr;
    HWND owner_hwnd = nullptr;
    OverlayCallbacks callbacks;

    int virt_x = 0;
    int virt_y = 0;
    int width = 0;
    int height = 0;
    float dpi_scale = 1.0f;

    HDC mem_dc = nullptr;
    HBITMAP dib_bmp = nullptr;
    HGDIOBJ old_bmp = nullptr;
    uint32_t* dib_bits = nullptr;

    RECT toolbar_rect{};
    RECT btn_rect{};
    RECT btn_settings{};
    RECT btn_cancel{};
    int hover_btn = 0;  // 0=none, 1=rect, 2=settings, 3=cancel

    bool drawing = false;
    POINT start_pt{};
    POINT curr_pt{};
};

OverlayState* g_overlay = nullptr;

int ScalePx(float scale, int px) {
    return static_cast<int>(std::lround(px * scale));
}

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void ComputeToolbarLayout(OverlayState* st) {
    float s = st->dpi_scale;
    int pad = ScalePx(s, 6);
    int btn_sz = ScalePx(s, 36);
    int gap = ScalePx(s, 4);
    int sep_w = ScalePx(s, 9);  // 4 + 1 + 4

    int tb_w = pad * 2 + btn_sz * 3 + gap * 2 + sep_w;
    int tb_h = pad * 2 + btn_sz;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR hMon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    int mon_left = 0, mon_top = 0, mon_w = st->width;
    if (GetMonitorInfoW(hMon, &mi)) {
        mon_left = mi.rcMonitor.left - st->virt_x;
        mon_top = mi.rcMonitor.top - st->virt_y;
        mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
    }

    int tb_x = mon_left + (mon_w - tb_w) / 2;
    int tb_y = mon_top + ScalePx(s, 20);
    st->toolbar_rect = RECT{tb_x, tb_y, tb_x + tb_w, tb_y + tb_h};

    int x = tb_x + pad;
    int y = tb_y + pad;
    st->btn_rect = RECT{x, y, x + btn_sz, y + btn_sz};
    x += btn_sz + gap + sep_w + gap;
    st->btn_settings = RECT{x, y, x + btn_sz, y + btn_sz};
    x += btn_sz + gap;
    st->btn_cancel = RECT{x, y, x + btn_sz, y + btn_sz};
}

void DrawToolbar(Gdiplus::Graphics& g, OverlayState* st) {
    float s = st->dpi_scale;
    const RECT& tb = st->toolbar_rect;

    // Subtle shadow
    {
        Gdiplus::GraphicsPath spath;
        AddRoundedRectPath(
            spath,
            Gdiplus::RectF(static_cast<float>(tb.left), static_cast<float>(tb.top + ScalePx(s, 4)),
                           static_cast<float>(tb.right - tb.left), static_cast<float>(tb.bottom - tb.top)),
            8.0f * s
        );
        Gdiplus::SolidBrush shadow_brush(Gdiplus::Color(90, 0, 0, 0));
        g.FillPath(&shadow_brush, &spath);
    }

    // Main toolbar pill
    {
        Gdiplus::GraphicsPath path;
        Gdiplus::RectF rf(static_cast<float>(tb.left), static_cast<float>(tb.top),
                          static_cast<float>(tb.right - tb.left), static_cast<float>(tb.bottom - tb.top));
        AddRoundedRectPath(path, rf, 8.0f * s);
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, 32, 32, 32));
        Gdiplus::Pen border(Gdiplus::Color(255, 51, 51, 51), 1.0f * s);
        g.FillPath(&bg, &path);
        g.DrawPath(&border, &path);
    }

    auto fill_btn = [&](const RECT& r, Gdiplus::Color col) {
        Gdiplus::GraphicsPath p;
        AddRoundedRectPath(p, Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                                             static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top)),
                           4.0f * s);
        Gdiplus::SolidBrush b(col);
        g.FillPath(&b, &p);
    };

    // Rect tool button (always active)
    fill_btn(st->btn_rect, st->hover_btn == 1 ? Gdiplus::Color(255, 156, 114, 186) : Gdiplus::Color(255, 26, 115, 232));

    if (st->hover_btn == 2) fill_btn(st->btn_settings, Gdiplus::Color(255, 51, 51, 51));
    if (st->hover_btn == 3) fill_btn(st->btn_cancel, Gdiplus::Color(255, 51, 51, 51));

    // Separator line
    {
        int sep_x = st->btn_rect.right + ScalePx(s, 8);
        int sep_y = tb.top + ScalePx(s, 12);
        int sep_h = ScalePx(s, 24);
        Gdiplus::Pen sep_pen(Gdiplus::Color(255, 68, 68, 68), 1.0f * s);
        g.DrawLine(&sep_pen, static_cast<float>(sep_x), static_cast<float>(sep_y),
                   static_cast<float>(sep_x), static_cast<float>(sep_y + sep_h));
    }

    Gdiplus::Pen icon_pen(Gdiplus::Color(255, 255, 255, 255), 1.7f * s);
    icon_pen.SetStartCap(Gdiplus::LineCapRound);
    icon_pen.SetEndCap(Gdiplus::LineCapRound);
    icon_pen.SetLineJoin(Gdiplus::LineJoinRound);

    // 1. Rect icon (15x15 centered inside 36x36)
    {
        float cx = (st->btn_rect.left + st->btn_rect.right) * 0.5f;
        float cy = (st->btn_rect.top + st->btn_rect.bottom) * 0.5f;
        float sz = 15.0f * s;
        Gdiplus::GraphicsPath rp;
        AddRoundedRectPath(rp, Gdiplus::RectF(cx - sz * 0.5f, cy - sz * 0.5f, sz, sz), 2.0f * s);
        g.DrawPath(&icon_pen, &rp);
    }

    // 2. Settings gear icon
    {
        float cx = (st->btn_settings.left + st->btn_settings.right) * 0.5f;
        float cy = (st->btn_settings.top + st->btn_settings.bottom) * 0.5f;
        float inner_r = 2.6f * s;
        g.DrawEllipse(&icon_pen, cx - inner_r, cy - inner_r, inner_r * 2.0f, inner_r * 2.0f);
        float mid_r = 5.6f * s;
        g.DrawEllipse(&icon_pen, cx - mid_r, cy - mid_r, mid_r * 2.0f, mid_r * 2.0f);
        for (int i = 0; i < 6; ++i) {
            float ang = i * 3.14159265f / 3.0f;
            float r1 = 5.8f * s;
            float r2 = 8.2f * s;
            g.DrawLine(&icon_pen,
                       cx + std::cos(ang) * r1, cy + std::sin(ang) * r1,
                       cx + std::cos(ang) * r2, cy + std::sin(ang) * r2);
        }
    }

    // 3. Cancel X icon
    {
        float cx = (st->btn_cancel.left + st->btn_cancel.right) * 0.5f;
        float cy = (st->btn_cancel.top + st->btn_cancel.bottom) * 0.5f;
        float half = 5.2f * s;
        g.DrawLine(&icon_pen, cx - half, cy - half, cx + half, cy + half);
        g.DrawLine(&icon_pen, cx + half, cy - half, cx - half, cy + half);
    }
}

void RenderOverlay(OverlayState* st) {
    if (!st || !st->hwnd || !st->dib_bits) return;

    const int w = st->width;
    const int h = st->height;
    std::fill_n(st->dib_bits, static_cast<size_t>(w) * static_cast<size_t>(h), 0x8C000000u);

    RECT sel{};
    bool has_sel = false;
    if (st->drawing) {
        sel.left = std::clamp<int>(std::min(st->start_pt.x, st->curr_pt.x), 0, w);
        sel.right = std::clamp<int>(std::max(st->start_pt.x, st->curr_pt.x), 0, w);
        sel.top = std::clamp<int>(std::min(st->start_pt.y, st->curr_pt.y), 0, h);
        sel.bottom = std::clamp<int>(std::max(st->start_pt.y, st->curr_pt.y), 0, h);
        has_sel = (sel.right > sel.left && sel.bottom > sel.top);
        if (has_sel) {
            for (int y = sel.top; y < sel.bottom; ++y) {
                uint32_t* row = st->dib_bits + static_cast<size_t>(y) * static_cast<size_t>(w);
                std::fill(row + sel.left, row + sel.right, 0x00000000u);
            }
        }
    }

    {
        Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(st->dib_bits));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        if (has_sel) {
            Gdiplus::Pen dash_pen(Gdiplus::Color(220, 255, 255, 255), 2.0f * st->dpi_scale);
            dash_pen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawRectangle(&dash_pen,
                            static_cast<float>(sel.left),
                            static_cast<float>(sel.top),
                            static_cast<float>(sel.right - sel.left),
                            static_cast<float>(sel.bottom - sel.top));
        }

        if (!st->drawing) {
            DrawToolbar(g, st);
        }
    }

    HDC screen_dc = GetDC(nullptr);
    POINT pt_dst{st->virt_x, st->virt_y};
    SIZE sz{w, h};
    POINT pt_src{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(st->hwnd, screen_dc, &pt_dst, &sz, st->mem_dc, &pt_src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen_dc);
}

bool GetPngEncoderClsid(CLSID& out_clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(num, size, encoders) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            out_clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

bool EncodeHBitmapToPng(HBITMAP hbmp, std::vector<uint8_t>& out_png) {
    CLSID png_clsid{};
    if (!GetPngEncoderClsid(png_clsid)) return false;

    Gdiplus::Bitmap bmp(hbmp, nullptr);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return false;

    bool ok = (bmp.Save(stream, &png_clsid, nullptr) == Gdiplus::Ok);
    if (ok) {
        HGLOBAL hMem = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &hMem)) && hMem) {
            SIZE_T bytes = GlobalSize(hMem);
            void* ptr = GlobalLock(hMem);
            if (ptr && bytes > 0) {
                const uint8_t* p = static_cast<const uint8_t*>(ptr);
                out_png.assign(p, p + bytes);
                GlobalUnlock(hMem);
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    }
    stream->Release();
    return ok;
}

bool CopyHBitmapToClipboard(HWND owner, HBITMAP hbmp, int w, int h) {
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    DWORD stride = ((static_cast<DWORD>(w) * 3 + 3) & ~3u);
    DWORD img_bytes = stride * static_cast<DWORD>(h);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + img_bytes);
    if (!hMem) return false;

    void* ptr = GlobalLock(hMem);
    if (!ptr) {
        GlobalFree(hMem);
        return false;
    }
    std::memcpy(ptr, &bih, sizeof(bih));
    HDC dc = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = bih;
    int got = GetDIBits(dc, hbmp, 0, static_cast<UINT>(h),
                        static_cast<uint8_t*>(ptr) + sizeof(BITMAPINFOHEADER), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    GlobalUnlock(hMem);

    if (got == 0) {
        GlobalFree(hMem);
        return false;
    }

    if (!OpenClipboard(owner)) {
        GlobalFree(hMem);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hMem)) {
        CloseClipboard();
        GlobalFree(hMem);
        return false;
    }
    CloseClipboard();
    return true;
}

void CaptureAndSearch(OverlayState* st, RECT sel) {
    int cw = sel.right - sel.left;
    int ch = sel.bottom - sel.top;
    if (cw <= 0 || ch <= 0) return;

    int src_x = st->virt_x + sel.left;
    int src_y = st->virt_y + sel.top;
    OverlayCallbacks cb = st->callbacks;

    ShowWindow(st->hwnd, SW_HIDE);
    if (FAILED(DwmFlush())) {
        Sleep(30);
    }

    HDC screen_dc = GetDC(nullptr);
    HDC cap_dc = CreateCompatibleDC(screen_dc);
    HBITMAP cap_bmp = CreateCompatibleBitmap(screen_dc, cw, ch);
    HGDIOBJ old = SelectObject(cap_dc, cap_bmp);
    BitBlt(cap_dc, 0, 0, cw, ch, screen_dc, src_x, src_y, SRCCOPY | CAPTUREBLT);
    SelectObject(cap_dc, old);
    DeleteDC(cap_dc);
    ReleaseDC(nullptr, screen_dc);

    AppSettings settings = cb.get_settings ? cb.get_settings() : AppSettings{};
    std::wstring info;

    if (settings.save_to_clipboard) {
        HWND clip_owner = st->owner_hwnd ? st->owner_hwnd : st->hwnd;
        if (CopyHBitmapToClipboard(clip_owner, cap_bmp, cw, ch)) {
            info += L"\u2713 Copied to Clipboard\n";
        }
    }

    std::vector<uint8_t> png_bytes;
    bool encoded = EncodeHBitmapToPng(cap_bmp, png_bytes);
    DeleteObject(cap_bmp);

    if (!encoded || png_bytes.empty()) {
        DestroyWindow(st->hwnd);
        return;
    }

    bool saved_disk = false;
    if (!settings.save_location.empty()) {
        DWORD attr = GetFileAttributesW(settings.save_location.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            wchar_t fname[64];
            std::swprintf(fname, 64, L"LensAnywhere_%lld.png", static_cast<long long>(sec));
            std::wstring full = settings.save_location;
            if (full.back() != L'\\' && full.back() != L'/') full.push_back(L'\\');
            full += fname;

            HANDLE hFile = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                if (WriteFile(hFile, png_bytes.data(), static_cast<DWORD>(png_bytes.size()), &written, nullptr) &&
                    written == png_bytes.size()) {
                    saved_disk = true;
                    info += L"\u2713 Saved to " + full;
                }
                CloseHandle(hFile);
            }
        }
    }
    if (!saved_disk) {
        info += L"\u2139 Not saved to local disk";
    }

    if (cb.on_notify) {
        cb.on_notify(L"LensAnywhere", L"Opening search in Google Lens...\n" + info, false);
    }

    Tunnel::SearchLens(std::move(png_bytes));
    DestroyWindow(st->hwnd);
}

int HitTestToolbarButton(OverlayState* st, POINT pt) {
    if (PtInRect(&st->btn_rect, pt)) return 1;
    if (PtInRect(&st->btn_settings, pt)) return 2;
    if (PtInRect(&st->btn_cancel, pt)) return 3;
    return 0;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OverlayState* st = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_SETCURSOR: {
            if (st) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                bool over_tb = (!st->drawing && PtInRect(&st->toolbar_rect, pt));
                SetCursor(LoadCursorW(nullptr, over_tb ? IDC_HAND : IDC_CROSS));
                return TRUE;
            }
            break;
        }
        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_RBUTTONDOWN: {
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (!st->drawing && PtInRect(&st->toolbar_rect, pt)) {
                int hit = HitTestToolbarButton(st, pt);
                if (hit == 2) {
                    auto open_cb = st->callbacks.on_open_settings;
                    DestroyWindow(hwnd);
                    if (open_cb) open_cb();
                } else if (hit == 3) {
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            st->drawing = true;
            st->start_pt = pt;
            st->curr_pt = pt;
            SetCapture(hwnd);
            RenderOverlay(st);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (st->drawing) {
                st->curr_pt = pt;
                RenderOverlay(st);
            } else {
                int hit = PtInRect(&st->toolbar_rect, pt) ? HitTestToolbarButton(st, pt) : 0;
                if (hit != st->hover_btn) {
                    st->hover_btn = hit;
                    RenderOverlay(st);
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!st || !st->drawing) break;
            ReleaseCapture();
            st->drawing = false;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            st->curr_pt = pt;
            RECT sel{
                std::clamp<int>(std::min(st->start_pt.x, st->curr_pt.x), 0, st->width),
                std::clamp<int>(std::min(st->start_pt.y, st->curr_pt.y), 0, st->height),
                std::clamp<int>(std::max(st->start_pt.x, st->curr_pt.x), 0, st->width),
                std::clamp<int>(std::max(st->start_pt.y, st->curr_pt.y), 0, st->height)
            };
            if ((sel.right - sel.left) > 5 && (sel.bottom - sel.top) > 5) {
                CaptureAndSearch(st, sel);
            } else {
                RenderOverlay(st);
            }
            return 0;
        }
        case WM_NCDESTROY: {
            if (st) {
                if (st->mem_dc) {
                    if (st->old_bmp) SelectObject(st->mem_dc, st->old_bmp);
                    DeleteDC(st->mem_dc);
                }
                if (st->dib_bmp) DeleteObject(st->dib_bmp);
                auto closed_cb = st->callbacks.on_closed;
                delete st;
                g_overlay = nullptr;
                if (closed_cb) closed_cb();
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace CaptureOverlay {

void Show(HINSTANCE hInst, HWND owner_hwnd, const OverlayCallbacks& callbacks) {
    if (g_overlay && g_overlay->hwnd && IsWindow(g_overlay->hwnd)) {
        SetForegroundWindow(g_overlay->hwnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kOverlayClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        RegisterClassExW(&wc);
        registered = true;
    }

    auto* st = new OverlayState();
    st->owner_hwnd = owner_hwnd;
    st->callbacks = callbacks;
    st->virt_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    st->virt_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    st->width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    st->height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (st->width <= 0 || st->height <= 0) {
        st->virt_x = 0;
        st->virt_y = 0;
        st->width = GetSystemMetrics(SM_CXSCREEN);
        st->height = GetSystemMetrics(SM_CYSCREEN);
    }

    HDC screen_dc = GetDC(nullptr);
    UINT dpi = GetDpiForSystem();
    st->dpi_scale = dpi ? (static_cast<float>(dpi) / 96.0f) : 1.0f;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = st->width;
    bmi.bmiHeader.biHeight = -st->height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    st->mem_dc = CreateCompatibleDC(screen_dc);
    void* bits = nullptr;
    st->dib_bmp = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);

    if (!st->mem_dc || !st->dib_bmp || !bits) {
        if (st->mem_dc) DeleteDC(st->mem_dc);
        if (st->dib_bmp) DeleteObject(st->dib_bmp);
        delete st;
        return;
    }
    st->dib_bits = static_cast<uint32_t*>(bits);
    st->old_bmp = SelectObject(st->mem_dc, st->dib_bmp);

    ComputeToolbarLayout(st);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kOverlayClassName,
        L"",
        WS_POPUP,
        st->virt_x, st->virt_y, st->width, st->height,
        nullptr, nullptr, hInst, st
    );
    if (!hwnd) {
        SelectObject(st->mem_dc, st->old_bmp);
        DeleteDC(st->mem_dc);
        DeleteObject(st->dib_bmp);
        delete st;
        return;
    }

    st->hwnd = hwnd;
    g_overlay = st;

    RenderOverlay(st);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

void Close() {
    if (g_overlay && g_overlay->hwnd && IsWindow(g_overlay->hwnd)) {
        DestroyWindow(g_overlay->hwnd);
    }
}

bool IsOpen() {
    return g_overlay && g_overlay->hwnd && IsWindow(g_overlay->hwnd);
}

}  // namespace CaptureOverlay
