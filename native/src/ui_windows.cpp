#include "ui_windows.hpp"
#include "../resources/resource.h"

#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kSettingsClass[] = L"LensAnywhereSettingsWnd";
constexpr wchar_t kKeyPopupClass[] = L"LensAnywhereKeyPopupWnd";
constexpr wchar_t kWelcomeClass[] = L"LensAnywhereWelcomeWnd";
constexpr UINT WM_APP_AVATAR_READY = WM_APP + 10;

void EnableDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmUseImmersiveDarkModeOld = 19;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkModeOld, &dark, sizeof(dark));
    }
}

float GetWindowScale(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    return dpi ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
}

int S(float scale, int px) {
    return static_cast<int>(std::lround(px * scale));
}

float SF(float scale, float px) {
    return px * scale;
}

void AddRoundRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    float d = radius * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

std::unique_ptr<Gdiplus::Bitmap> LoadPngFromMemory(const void* data, size_t size) {
    if (!data || size == 0) return nullptr;
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data), static_cast<UINT>(size));
    if (!stream) return nullptr;
    auto bmp = std::unique_ptr<Gdiplus::Bitmap>(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) return nullptr;
    return bmp;
}

std::unique_ptr<Gdiplus::Bitmap> LoadEmbeddedLogo() {
    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(IDR_LOGO_PNG), RT_RCDATA);
    if (!hRes) return nullptr;
    DWORD sz = SizeofResource(hMod, hRes);
    HGLOBAL hMem = LoadResource(hMod, hRes);
    if (!hMem) return nullptr;
    return LoadPngFromMemory(LockResource(hMem), sz);
}

void DrawToggleSwitch(Gdiplus::Graphics& g, Gdiplus::RectF r, float anim_pos) {
    float t = std::clamp(anim_pos, 0.0f, 1.0f);
    auto lerp_c = [&](Gdiplus::Color a, Gdiplus::Color b) -> Gdiplus::Color {
        auto ch = [&](BYTE c1, BYTE c2) -> BYTE {
            return static_cast<BYTE>(std::lround(c1 + (c2 - c1) * t));
        };
        return Gdiplus::Color(ch(a.GetA(), b.GetA()), ch(a.GetR(), b.GetR()), ch(a.GetG(), b.GetG()), ch(a.GetB(), b.GetB()));
    };

    Gdiplus::Color track_c = lerp_c(Gdiplus::Color(255, 211, 211, 211), Gdiplus::Color(255, 179, 212, 255));
    Gdiplus::Color thumb_c = lerp_c(Gdiplus::Color(255, 255, 255, 255), Gdiplus::Color(255, 26, 115, 232));

    Gdiplus::GraphicsPath track_path;
    AddRoundRect(track_path, r, r.Height * 0.5f);
    Gdiplus::SolidBrush track_br(track_c);
    g.FillPath(&track_br, &track_path);

    float margin = r.Height * (4.0f / 28.0f);
    float thumb_d = r.Height - margin * 2.0f;
    float travel = r.Width - thumb_d - margin * 2.0f;
    float tx = r.X + margin + travel * t;
    float ty = r.Y + margin;

    Gdiplus::SolidBrush shadow_br(Gdiplus::Color(40, 0, 0, 0));
    g.FillEllipse(&shadow_br, tx, ty + 1.0f, thumb_d, thumb_d);

    Gdiplus::SolidBrush thumb_br(thumb_c);
    g.FillEllipse(&thumb_br, tx, ty, thumb_d, thumb_d);
}

std::wstring BrowseFolderDialog(HWND owner) {
    std::wstring selected;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        dlg->SetTitle(L"Select Save Directory");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    selected = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return selected;
}

// ==================== KEY PICKER POPUP (A..Z) ====================

struct KeyPopupState {
    HWND hwnd = nullptr;
    HWND parent_hwnd = nullptr;
    char current_key = 'S';
    int hover_idx = -1;
    float scale = 1.0f;
    std::function<void(char)> on_pick;
};

KeyPopupState* g_key_popup = nullptr;

LRESULT CALLBACK KeyPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<KeyPopupState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_ACTIVATE: {
            if (LOWORD(wp) == WA_INACTIVE) {
                DestroyWindow(hwnd);
                return 0;
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
        case WM_MOUSEMOVE: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            int pad = S(st->scale, 6);
            int cell = S(st->scale, 34);
            int gap = S(st->scale, 4);
            int col = (pt.x - pad) / (cell + gap);
            int row = (pt.y - pad) / (cell + gap);
            int idx = -1;
            if (col >= 0 && col < 6 && row >= 0 && row < 5) {
                int cand = row * 6 + col;
                int rx = pad + col * (cell + gap);
                int ry = pad + row * (cell + gap);
                if (cand < 26 && pt.x >= rx && pt.x < rx + cell && pt.y >= ry && pt.y < ry + cell) {
                    idx = cand;
                }
            }
            if (idx != st->hover_idx) {
                st->hover_idx = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            int pad = S(st->scale, 6);
            int cell = S(st->scale, 34);
            int gap = S(st->scale, 4);
            int col = (pt.x - pad) / (cell + gap);
            int row = (pt.y - pad) / (cell + gap);
            if (col >= 0 && col < 6 && row >= 0 && row < 5) {
                int cand = row * 6 + col;
                if (cand >= 0 && cand < 26) {
                    char picked = static_cast<char>('A' + cand);
                    auto cb = st->on_pick;
                    DestroyWindow(hwnd);
                    if (cb) cb(picked);
                    return 0;
                }
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_PAINT: {
            if (!st) break;
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem_dc, bmp);

            {
                Gdiplus::Graphics g(mem_dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                g.Clear(Gdiplus::Color(255, 43, 43, 43));

                Gdiplus::Pen border(Gdiplus::Color(255, 68, 68, 68), 1.0f);
                g.DrawRectangle(&border, 0.5f, 0.5f, rc.right - 1.0f, rc.bottom - 1.0f);

                Gdiplus::Font font(L"Segoe UI", 10.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::SolidBrush text_br(Gdiplus::Color(255, 255, 255, 255));

                int pad = S(st->scale, 6);
                int cell = S(st->scale, 34);
                int gap = S(st->scale, 4);

                for (int i = 0; i < 26; ++i) {
                    int row = i / 6;
                    int col = i % 6;
                    Gdiplus::RectF cr(
                        static_cast<float>(pad + col * (cell + gap)),
                        static_cast<float>(pad + row * (cell + gap)),
                        static_cast<float>(cell),
                        static_cast<float>(cell)
                    );
                    char ch = static_cast<char>('A' + i);
                    Gdiplus::Color bg = (ch == st->current_key)
                        ? Gdiplus::Color(255, 26, 115, 232)
                        : (i == st->hover_idx ? Gdiplus::Color(255, 64, 64, 64) : Gdiplus::Color(255, 51, 51, 51));
                    Gdiplus::GraphicsPath cp;
                    AddRoundRect(cp, cr, SF(st->scale, 5.0f));
                    Gdiplus::SolidBrush cb(bg);
                    g.FillPath(&cb, &cp);

                    wchar_t txt[2] = {static_cast<wchar_t>(ch), L'\0'};
                    g.DrawString(txt, 1, &font, cr, &sf, &text_br);
                }
            }

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old);
            DeleteObject(bmp);
            DeleteDC(mem_dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY: {
            if (st) {
                delete st;
                g_key_popup = nullptr;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowKeyPopup(HINSTANCE hInst, HWND parent, RECT anchor_client, char current_key, float scale, std::function<void(char)> on_pick) {
    if (g_key_popup && g_key_popup->hwnd && IsWindow(g_key_popup->hwnd)) {
        DestroyWindow(g_key_popup->hwnd);
    }

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = KeyPopupProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kKeyPopupClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        RegisterClassExW(&wc);
        reg = true;
    }

    int pad = S(scale, 6);
    int cell = S(scale, 34);
    int gap = S(scale, 4);
    int w = pad * 2 + 6 * cell + 5 * gap;
    int h = pad * 2 + 5 * cell + 4 * gap;

    POINT pt{anchor_client.right - w, anchor_client.bottom + S(scale, 4)};
    ClientToScreen(parent, &pt);

    auto* st = new KeyPopupState();
    st->parent_hwnd = parent;
    st->current_key = current_key;
    st->scale = scale;
    st->on_pick = std::move(on_pick);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kKeyPopupClass, L"",
        WS_POPUP,
        pt.x, pt.y, w, h,
        parent, nullptr, hInst, st
    );
    if (!hwnd) {
        delete st;
        return;
    }
    st->hwnd = hwnd;
    g_key_popup = st;
    ShowWindow(hwnd, SW_SHOW);
}

// ==================== SETTINGS WINDOW ====================

struct SettingsWinState {
    HWND hwnd = nullptr;
    HINSTANCE hInst = nullptr;
    UiCallbacks callbacks;
    AppSettings settings;
    std::unique_ptr<Gdiplus::Bitmap> logo;
    float scale = 1.0f;

    float anim_hotkey = 1.0f;
    float anim_clip = 0.0f;
    float anim_startup = 1.0f;
    UINT_PTR anim_timer = 0;

    RECT rc_toggle_hotkey{};
    RECT rc_btn_ctrl{}, rc_btn_shift{}, rc_btn_win{}, rc_btn_alt{}, rc_btn_key{};
    RECT rc_toggle_clip{};
    RECT rc_path_box{}, rc_btn_browse{};
    RECT rc_toggle_startup{};
    RECT rc_footer_link{};
    int hover_id = 0;
};

SettingsWinState* g_settings_win = nullptr;

enum SettingsHitId {
    HIT_NONE = 0,
    HIT_TOGGLE_HOTKEY = 1,
    HIT_CTRL = 2,
    HIT_SHIFT = 3,
    HIT_WIN = 4,
    HIT_ALT = 5,
    HIT_KEY = 6,
    HIT_TOGGLE_CLIP = 7,
    HIT_BROWSE = 8,
    HIT_TOGGLE_STARTUP = 9,
    HIT_FOOTER_LINK = 10,
};

void LayoutSettings(SettingsWinState* st, int cw) {
    float s = st->scale;
    int m = S(s, 30);
    int card_w = cw - m * 2;
    int inner_x = m + S(s, 20);
    int inner_r = m + card_w - S(s, 20);
    int sw_w = S(s, 50);
    int sw_h = S(s, 28);

    int y = S(s, 118);  // Top of Card 1 (Hotkey)
    st->rc_toggle_hotkey = RECT{inner_r - sw_w, y + S(s, 24), inner_r, y + S(s, 24) + sw_h};

    int ky = y + S(s, 78);
    int kw = S(s, 60);
    int kh = S(s, 32);
    int kg = S(s, 8);
    st->rc_btn_ctrl  = RECT{inner_x, ky, inner_x + kw, ky + kh};
    st->rc_btn_shift = RECT{inner_x + (kw + kg), ky, inner_x + (kw + kg) + kw, ky + kh};
    st->rc_btn_win   = RECT{inner_x + 2 * (kw + kg), ky, inner_x + 2 * (kw + kg) + kw, ky + kh};
    st->rc_btn_alt   = RECT{inner_x + 3 * (kw + kg), ky, inner_x + 3 * (kw + kg) + kw, ky + kh};
    int drop_w = S(s, 64);
    st->rc_btn_key   = RECT{inner_r - drop_w, ky, inner_r, ky + kh};

    int card2_y = y + S(s, 130) + S(s, 18);  // Top of Card 2 (General)
    st->rc_toggle_clip = RECT{inner_r - sw_w, card2_y + S(s, 24), inner_r, card2_y + S(s, 24) + sw_h};

    int loc_y = card2_y + S(s, 106);
    int browse_w = S(s, 88);
    int path_h = S(s, 34);
    st->rc_btn_browse = RECT{inner_r - browse_w, loc_y, inner_r, loc_y + path_h};
    st->rc_path_box   = RECT{inner_x, loc_y, inner_r - browse_w - S(s, 10), loc_y + path_h};

    int st_y = card2_y + S(s, 174);
    st->rc_toggle_startup = RECT{inner_r - sw_w, st_y + S(s, 4), inner_r, st_y + S(s, 4) + sw_h};

    int foot_y = card2_y + S(s, 232) + S(s, 16);
    st->rc_footer_link = RECT{m, foot_y, cw - m, foot_y + S(s, 24)};
}

int HitTestSettings(const SettingsWinState* st, POINT pt) {
    if (PtInRect(&st->rc_toggle_hotkey, pt)) return HIT_TOGGLE_HOTKEY;
    if (PtInRect(&st->rc_btn_ctrl, pt)) return HIT_CTRL;
    if (PtInRect(&st->rc_btn_shift, pt)) return HIT_SHIFT;
    if (PtInRect(&st->rc_btn_win, pt)) return HIT_WIN;
    if (PtInRect(&st->rc_btn_alt, pt)) return HIT_ALT;
    if (PtInRect(&st->rc_btn_key, pt)) return HIT_KEY;
    if (PtInRect(&st->rc_toggle_clip, pt)) return HIT_TOGGLE_CLIP;
    if (PtInRect(&st->rc_btn_browse, pt)) return HIT_BROWSE;
    if (PtInRect(&st->rc_toggle_startup, pt)) return HIT_TOGGLE_STARTUP;
    if (PtInRect(&st->rc_footer_link, pt)) return HIT_FOOTER_LINK;
    return HIT_NONE;
}

void CommitSettingsChange(SettingsWinState* st) {
    SaveSettings(st->settings);
    if (st->callbacks.on_settings_changed) {
        st->callbacks.on_settings_changed(st->settings);
    }
}

void PaintSettingsWindow(SettingsWinState* st, HDC hdc, const RECT& rc) {
    float s = st->scale;
    int cw = rc.right - rc.left;
    LayoutSettings(st, cw);

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(Gdiplus::Color(255, 26, 26, 26));

    int m = S(s, 30);
    int card_w = cw - m * 2;
    int inner_x = m + S(s, 20);

    Gdiplus::Font f_app(L"Segoe UI", 22.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_page(L"Segoe UI", 13.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_title(L"Segoe UI", 11.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_desc(L"Segoe UI", 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font f_btn(L"Segoe UI", 9.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

    Gdiplus::SolidBrush white_br(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::SolidBrush sub_br(Gdiplus::Color(255, 160, 160, 160));
    Gdiplus::SolidBrush desc_br(Gdiplus::Color(255, 136, 136, 136));
    Gdiplus::SolidBrush card_br(Gdiplus::Color(255, 37, 37, 37));
    Gdiplus::Pen card_border(Gdiplus::Color(255, 51, 51, 51), 1.0f);

    // Header
    int logo_sz = S(s, 36);
    if (st->logo) {
        g.DrawImage(st->logo.get(), m, S(s, 28), logo_sz, logo_sz);
    }
    Gdiplus::StringFormat sf_left;
    sf_left.SetAlignment(Gdiplus::StringAlignmentNear);
    sf_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    g.DrawString(L"LensAnywhere", -1, &f_app,
                 Gdiplus::RectF(static_cast<float>(m + logo_sz + S(s, 12)), SF(s, 22.0f), SF(s, 320.0f), SF(s, 44.0f)),
                 &sf_left, &white_br);
    g.DrawString(L"Settings", -1, &f_page,
                 Gdiplus::RectF(static_cast<float>(m), SF(s, 72.0f), SF(s, 200.0f), SF(s, 28.0f)),
                 &sf_left, &sub_br);

    auto draw_card = [&](int top_y, int h) {
        Gdiplus::GraphicsPath cp;
        AddRoundRect(cp, Gdiplus::RectF(static_cast<float>(m), static_cast<float>(top_y),
                                        static_cast<float>(card_w), static_cast<float>(h)), SF(s, 12.0f));
        g.FillPath(&card_br, &cp);
        g.DrawPath(&card_border, &cp);
    };

    // Card 1: Hotkey
    int card1_y = S(s, 118);
    draw_card(card1_y, S(s, 130));
    g.DrawString(L"Global Hotkey", -1, &f_title,
                 Gdiplus::PointF(static_cast<float>(inner_x), static_cast<float>(card1_y + S(s, 18))), &white_br);
    g.DrawString(L"Enable or disable the quick-launch shortcut.", -1, &f_desc,
                 Gdiplus::PointF(static_cast<float>(inner_x), static_cast<float>(card1_y + S(s, 42))), &desc_br);
    DrawToggleSwitch(g, Gdiplus::RectF(
        static_cast<float>(st->rc_toggle_hotkey.left), static_cast<float>(st->rc_toggle_hotkey.top),
        static_cast<float>(st->rc_toggle_hotkey.right - st->rc_toggle_hotkey.left),
        static_cast<float>(st->rc_toggle_hotkey.bottom - st->rc_toggle_hotkey.top)), st->anim_hotkey);

    Gdiplus::StringFormat sf_center;
    sf_center.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    auto draw_key_btn = [&](const RECT& r, const wchar_t* label, bool checked, int hit_id) {
        Gdiplus::RectF rf(static_cast<float>(r.left), static_cast<float>(r.top),
                          static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
        Gdiplus::GraphicsPath p;
        AddRoundRect(p, rf, SF(s, 6.0f));
        Gdiplus::Color fill = checked ? Gdiplus::Color(255, 26, 115, 232)
                                      : (st->hover_id == hit_id ? Gdiplus::Color(255, 64, 64, 64) : Gdiplus::Color(255, 51, 51, 51));
        Gdiplus::Color bdr = checked ? Gdiplus::Color(255, 0, 92, 230) : Gdiplus::Color(255, 68, 68, 68);
        Gdiplus::SolidBrush fb(fill);
        Gdiplus::Pen bp(bdr, 1.0f);
        g.FillPath(&fb, &p);
        g.DrawPath(&bp, &p);
        g.DrawString(label, -1, &f_btn, rf, &sf_center, &white_br);
    };

    draw_key_btn(st->rc_btn_ctrl,  L"Ctrl",  st->settings.ctrl,  HIT_CTRL);
    draw_key_btn(st->rc_btn_shift, L"Shift", st->settings.shift, HIT_SHIFT);
    draw_key_btn(st->rc_btn_win,   L"Win",   st->settings.win,   HIT_WIN);
    draw_key_btn(st->rc_btn_alt,   L"Alt",   st->settings.alt,   HIT_ALT);

    {
        const RECT& r = st->rc_btn_key;
        Gdiplus::RectF rf(static_cast<float>(r.left), static_cast<float>(r.top),
                          static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
        Gdiplus::GraphicsPath p;
        AddRoundRect(p, rf, SF(s, 6.0f));
        Gdiplus::SolidBrush fb(st->hover_id == HIT_KEY ? Gdiplus::Color(255, 64, 64, 64) : Gdiplus::Color(255, 51, 51, 51));
        g.FillPath(&fb, &p);
        wchar_t kbuf[8];
        std::swprintf(kbuf, 8, L"%c  \u25BE", static_cast<wchar_t>(st->settings.key));
        g.DrawString(kbuf, -1, &f_btn, rf, &sf_center, &white_br);
    }

    // Card 2: General
    int card2_y = card1_y + S(s, 130) + S(s, 18);
    draw_card(card2_y, S(s, 232));

    g.DrawString(L"Save to Clipboard", -1, &f_title,
                 Gdiplus::PointF(static_cast<float>(inner_x), static_cast<float>(card2_y + S(s, 18))), &white_br);
    g.DrawString(L"Automatically copy the result to your clipboard.", -1, &f_desc,
                 Gdiplus::PointF(static_cast<float>(inner_x), static_cast<float>(card2_y + S(s, 42))), &desc_br);
    DrawToggleSwitch(g, Gdiplus::RectF(
        static_cast<float>(st->rc_toggle_clip.left), static_cast<float>(st->rc_toggle_clip.top),
        static_cast<float>(st->rc_toggle_clip.right - st->rc_toggle_clip.left),
        static_cast<float>(st->rc_toggle_clip.bottom - st->rc_toggle_clip.top)), st->anim_clip);

    g.DrawString(L"Auto-Save Captures To:", -1, &f_title,
                 Gdiplus::PointF(static_cast<float>(inner_x), static_cast<float>(card2_y + S(s, 78))), &white_br);

    {
        const RECT& pr = st->rc_path_box;
        Gdiplus::RectF pf(static_cast<float>(pr.left), static_cast<float>(pr.top),
                          static_cast<float>(pr.right - pr.left), static_cast<float>(pr.bottom - pr.top));
        Gdiplus::GraphicsPath pp;
        AddRoundRect(pp, pf, SF(s, 6.0f));
        Gdiplus::SolidBrush pbg(Gdiplus::Color(255, 51, 51, 51));
        Gdiplus::Pen pbdr(Gdiplus::Color(255, 68, 68, 68), 1.0f);
        g.FillPath(&pbg, &pp);
        g.DrawPath(&pbdr, &pp);

        Gdiplus::RectF text_rf(pf.X + SF(s, 10.0f), pf.Y, pf.Width - SF(s, 20.0f), pf.Height);
        Gdiplus::StringFormat sf_path;
        sf_path.SetAlignment(Gdiplus::StringAlignmentNear);
        sf_path.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sf_path.SetTrimming(Gdiplus::StringTrimmingEllipsisPath);
        sf_path.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        if (st->settings.save_location.empty()) {
            g.DrawString(L"Select save location (Optional)...", -1, &f_desc, text_rf, &sf_path, &desc_br);
        } else {
            g.DrawString(st->settings.save_location.c_str(), -1, &f_desc, text_rf, &sf_path, &white_br);
        }
    }

    {
        const RECT& br = st->rc_btn_browse;
        Gdiplus::RectF bf(static_cast<float>(br.left), static_cast<float>(br.top),
                          static_cast<float>(br.right - br.left), static_cast<float>(br.bottom - br.top));
        Gdiplus::GraphicsPath bp;
        AddRoundRect(bp, bf, SF(s, 6.0f));
        Gdiplus::SolidBrush bfill(st->hover_id == HIT_BROWSE ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 224, 224, 224));
        Gdiplus::SolidBrush btxt(Gdiplus::Color(255, 17, 17, 17));
        g.FillPath(&bfill, &bp);
        g.DrawString(L"Browse", -1, &f_btn, bf, &sf_center, &btxt);
    }

    // Separator
    {
        int sep_y = card2_y + S(s, 156);
        Gdiplus::Pen sep_p(Gdiplus::Color(255, 51, 51, 51), 1.0f);
        g.DrawLine(&sep_p, static_cast<float>(inner_x), static_cast<float>(sep_y),
                   static_cast<float>(m + card_w - S(s, 20)), static_cast<float>(sep_y));
    }

    int st_y = card2_y + S(s, 170);
    g.DrawString(L"Launch on Startup", -1, &f_title,
                 Gdiplus::PointF(static_cast<float>(inner_x), static_cast<float>(st_y)), &white_br);
    g.DrawString(L"Start LensAnywhere silently in the background on boot.", -1, &f_desc,
                 Gdiplus::PointF(static_cast<float>(inner_x), static_cast<float>(st_y + S(s, 24))), &desc_br);
    DrawToggleSwitch(g, Gdiplus::RectF(
        static_cast<float>(st->rc_toggle_startup.left), static_cast<float>(st->rc_toggle_startup.top),
        static_cast<float>(st->rc_toggle_startup.right - st->rc_toggle_startup.left),
        static_cast<float>(st->rc_toggle_startup.bottom - st->rc_toggle_startup.top)), st->anim_startup);

    // Footer
    {
        Gdiplus::RectF ff(static_cast<float>(st->rc_footer_link.left), static_cast<float>(st->rc_footer_link.top),
                          static_cast<float>(st->rc_footer_link.right - st->rc_footer_link.left),
                          static_cast<float>(st->rc_footer_link.bottom - st->rc_footer_link.top));
        Gdiplus::SolidBrush link_br(st->hover_id == HIT_FOOTER_LINK ? Gdiplus::Color(255, 96, 165, 250) : Gdiplus::Color(255, 136, 136, 136));
        g.DrawString(L"LensAnywhere v1.0 \u2022 Utkarsh Kulshrestha (GitHub)", -1, &f_desc, ff, &sf_center, &link_br);
    }
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SettingsWinState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_TIMER: {
            if (st && wp == 1) {
                auto step = [](float& val, bool target) -> bool {
                    float dst = target ? 1.0f : 0.0f;
                    float diff = dst - val;
                    if (std::fabs(diff) <= 0.14f) {
                        val = dst;
                        return false;
                    }
                    val += (diff > 0 ? 0.14f : -0.14f);
                    return true;
                };
                bool a = step(st->anim_hotkey, st->settings.hotkey_enabled);
                bool b = step(st->anim_clip, st->settings.save_to_clipboard);
                bool c = step(st->anim_startup, st->settings.launch_on_startup);
                InvalidateRect(hwnd, nullptr, FALSE);
                if (!a && !b && !c && st->anim_timer) {
                    KillTimer(hwnd, 1);
                    st->anim_timer = 0;
                }
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (st && LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, st->hover_id != HIT_NONE ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            int hit = HitTestSettings(st, pt);
            if (hit != st->hover_id) {
                st->hover_id = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            int hit = HitTestSettings(st, pt);
            auto start_anim = [&]() {
                if (!st->anim_timer) st->anim_timer = SetTimer(hwnd, 1, 16, nullptr);
            };
            switch (hit) {
                case HIT_TOGGLE_HOTKEY:
                    st->settings.hotkey_enabled = !st->settings.hotkey_enabled;
                    start_anim();
                    CommitSettingsChange(st);
                    break;
                case HIT_CTRL:
                    st->settings.ctrl = !st->settings.ctrl;
                    CommitSettingsChange(st);
                    break;
                case HIT_SHIFT:
                    st->settings.shift = !st->settings.shift;
                    CommitSettingsChange(st);
                    break;
                case HIT_WIN:
                    st->settings.win = !st->settings.win;
                    CommitSettingsChange(st);
                    break;
                case HIT_ALT:
                    st->settings.alt = !st->settings.alt;
                    CommitSettingsChange(st);
                    break;
                case HIT_KEY: {
                    ShowKeyPopup(st->hInst, hwnd, st->rc_btn_key, st->settings.key, st->scale,
                                 [hwnd](char picked) {
                                     if (g_settings_win && g_settings_win->hwnd == hwnd) {
                                         g_settings_win->settings.key = picked;
                                         CommitSettingsChange(g_settings_win);
                                         InvalidateRect(hwnd, nullptr, FALSE);
                                     }
                                 });
                    break;
                }
                case HIT_TOGGLE_CLIP: {
                    st->settings.save_to_clipboard = !st->settings.save_to_clipboard;
                    start_anim();
                    if (st->settings.save_to_clipboard && st->settings.save_location.empty()) {
                        std::wstring dir = BrowseFolderDialog(hwnd);
                        if (!dir.empty()) st->settings.save_location = dir;
                    }
                    CommitSettingsChange(st);
                    break;
                }
                case HIT_BROWSE: {
                    std::wstring dir = BrowseFolderDialog(hwnd);
                    if (!dir.empty()) {
                        st->settings.save_location = dir;
                        CommitSettingsChange(st);
                    }
                    break;
                }
                case HIT_TOGGLE_STARTUP:
                    st->settings.launch_on_startup = !st->settings.launch_on_startup;
                    start_anim();
                    StartupManager::SetEnabled(st->settings.launch_on_startup);
                    CommitSettingsChange(st);
                    break;
                case HIT_FOOTER_LINK:
                    ShellExecuteW(nullptr, L"open", L"https://github.com/utkarsh05kul", nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                default:
                    break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            if (!st) break;
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem_dc, bmp);
            PaintSettingsWindow(st, mem_dc, rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old);
            DeleteObject(bmp);
            DeleteDC(mem_dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY: {
            if (st) {
                if (st->anim_timer) KillTimer(hwnd, 1);
                auto closed_cb = st->callbacks.on_window_closed;
                delete st;
                g_settings_win = nullptr;
                if (closed_cb) closed_cb();
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== WELCOME WINDOW ====================

struct WelcomeWinState {
    HWND hwnd = nullptr;
    HINSTANCE hInst = nullptr;
    UiCallbacks callbacks;
    std::unique_ptr<Gdiplus::Bitmap> logo;
    std::unique_ptr<Gdiplus::Bitmap> avatar;
    float scale = 1.0f;

    int scroll_y = 0;
    int max_scroll = 0;
    bool unlocked = false;
    bool hover_continue = false;
    bool hover_github = false;
    bool dragging_sb = false;
    int drag_start_y = 0;
    int drag_start_scroll = 0;

    RECT rc_viewport{};
    RECT rc_sb_thumb{};
    RECT rc_btn_github{};
    RECT rc_btn_next{};
};

WelcomeWinState* g_welcome_win = nullptr;

std::vector<uint8_t> DownloadGitHubAvatarBytes() {
    std::vector<uint8_t> out;
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return out;

    WinHttpSetTimeouts(hSession, 4000, 4000, 4000, 5000);
    HINTERNET hConn = WinHttpConnect(hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConn) {
        HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", L"/utkarsh05kul.png", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hReq) {
            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, nullptr)) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0 && out.size() < 2 * 1024 * 1024) {
                    size_t cur = out.size();
                    out.resize(cur + avail);
                    DWORD got = 0;
                    if (WinHttpReadData(hReq, out.data() + cur, avail, &got)) {
                        out.resize(cur + got);
                    } else {
                        out.resize(cur);
                        break;
                    }
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConn);
    }
    WinHttpCloseHandle(hSession);
    return out;
}

void UpdateWelcomeScrollBounds(WelcomeWinState* st, int ch) {
    float s = st->scale;
    int header_h = S(s, 84);
    int footer_h = S(s, 72);
    int vp_h = std::max(100, ch - header_h - footer_h);
    int content_h = S(s, 610);
    st->max_scroll = std::max(0, content_h - vp_h);
    st->scroll_y = std::clamp(st->scroll_y, 0, st->max_scroll);
    if (!st->unlocked && (st->max_scroll == 0 || st->scroll_y >= st->max_scroll - S(s, 20))) {
        st->unlocked = true;
    }
}

void PaintWelcomeWindow(WelcomeWinState* st, HDC hdc, const RECT& rc) {
    float s = st->scale;
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    UpdateWelcomeScrollBounds(st, ch);

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(Gdiplus::Color(255, 18, 18, 20));

    int m = S(s, 20);
    int header_h = S(s, 84);
    int footer_h = S(s, 72);
    int vp_top = header_h;
    int vp_h = std::max(100, ch - header_h - footer_h);
    st->rc_viewport = RECT{m, vp_top, cw - m, vp_top + vp_h};

    Gdiplus::Font f_title(L"Segoe UI", 16.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_sub(L"Segoe UI", 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_sec(L"Segoe UI", 10.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_body(L"Segoe UI", 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font f_ftitle(L"Segoe UI", 9.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_fdesc(L"Segoe UI", 8.8f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font f_btn(L"Segoe UI", 10.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

    Gdiplus::SolidBrush white_br(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::SolidBrush cyan_br(Gdiplus::Color(255, 56, 189, 248));
    Gdiplus::SolidBrush body_br(Gdiplus::Color(255, 161, 161, 170));
    Gdiplus::SolidBrush fdesc_br(Gdiplus::Color(255, 154, 154, 158));
    Gdiplus::SolidBrush card_br(Gdiplus::Color(255, 24, 24, 27));
    Gdiplus::Pen card_bdr(Gdiplus::Color(255, 39, 39, 42), 1.0f);

    // Header
    int logo_sz = S(s, 44);
    if (st->logo) {
        g.DrawImage(st->logo.get(), m, S(s, 20), logo_sz, logo_sz);
    }
    g.DrawString(L"LensAnywhere", -1, &f_title,
                 Gdiplus::PointF(static_cast<float>(m + logo_sz + S(s, 14)), SF(s, 18.0f)), &white_br);
    g.DrawString(L"Seamless visual search on Windows", -1, &f_sub,
                 Gdiplus::PointF(static_cast<float>(m + logo_sz + S(s, 14)), SF(s, 45.0f)), &cyan_br);

    // Scrollable region
    g.SetClip(Gdiplus::Rect(m, vp_top, cw - m * 2, vp_h));
    int card_w = cw - m * 2 - S(s, 12);
    int cx = m;
    int y = vp_top - st->scroll_y;

    auto draw_card = [&](int cy, int h) {
        Gdiplus::GraphicsPath cp;
        AddRoundRect(cp, Gdiplus::RectF(static_cast<float>(cx), static_cast<float>(cy),
                                        static_cast<float>(card_w), static_cast<float>(h)), SF(s, 12.0f));
        g.FillPath(&card_br, &cp);
        g.DrawPath(&card_bdr, &cp);
    };

    // 1. About Card
    int h1 = S(s, 108);
    draw_card(y, h1);
    g.DrawString(L"About LensAnywhere", -1, &f_sec,
                 Gdiplus::PointF(static_cast<float>(cx + S(s, 20)), static_cast<float>(y + S(s, 16))), &white_br);
    g.DrawString(L"LensAnywhere allows you to crop any region on your screen using a custom global hotkey "
                 L"and instantly search it on Google Lens without third-party uploads or browser extensions.",
                 -1, &f_body,
                 Gdiplus::RectF(static_cast<float>(cx + S(s, 20)), static_cast<float>(y + S(s, 42)),
                                static_cast<float>(card_w - S(s, 40)), SF(s, 60.0f)),
                 nullptr, &body_br);
    y += h1 + S(s, 14);

    // 2. Features Card
    int h2 = S(s, 178);
    draw_card(y, h2);
    g.DrawString(L"Program Features", -1, &f_sec,
                 Gdiplus::PointF(static_cast<float>(cx + S(s, 20)), static_cast<float>(y + S(s, 16))), &white_br);
    struct Feat { const wchar_t* tag; const wchar_t* title; const wchar_t* desc; };
    const Feat feats[4] = {
        {L"\u2702", L"Instant Region Cropping", L"Capture any screen region using a custom global hotkey."},
        {L"\U0001F310", L"Direct Lens Search", L"Opens search directly in your browser."},
        {L"\U0001F4CB", L"Clipboard & Auto-Save", L"Optionally copy captures or save directly to a folder."},
        {L"\u26A1", L"Background Tray Utility", L"Runs quietly in system tray with minimal RAM footprint."}
    };
    int col_w = (card_w - S(s, 58)) / 2;
    Gdiplus::StringFormat sf_c;
    sf_c.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf_c.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    for (int i = 0; i < 4; ++i) {
        int col = i % 2;
        int row = i / 2;
        int fx = cx + S(s, 20) + col * (col_w + S(s, 18));
        int fy = y + S(s, 48) + row * S(s, 62);
        Gdiplus::RectF ico_rf(static_cast<float>(fx), static_cast<float>(fy), SF(s, 36.0f), SF(s, 36.0f));
        Gdiplus::GraphicsPath ip;
        AddRoundRect(ip, ico_rf, SF(s, 8.0f));
        Gdiplus::SolidBrush ib(Gdiplus::Color(255, 39, 39, 42));
        Gdiplus::Pen ibp(Gdiplus::Color(255, 63, 63, 70), 1.0f);
        g.FillPath(&ib, &ip);
        g.DrawPath(&ibp, &ip);
        g.DrawString(feats[i].tag, -1, &f_sec, ico_rf, &sf_c, &white_br);

        int tx = fx + S(s, 46);
        int tw = col_w - S(s, 46);
        g.DrawString(feats[i].title, -1, &f_ftitle,
                     Gdiplus::PointF(static_cast<float>(tx), static_cast<float>(fy - S(s, 2))), &white_br);
        g.DrawString(feats[i].desc, -1, &f_fdesc,
                     Gdiplus::RectF(static_cast<float>(tx), static_cast<float>(fy + S(s, 18)),
                                    static_cast<float>(tw), SF(s, 40.0f)),
                     nullptr, &fdesc_br);
    }
    y += h2 + S(s, 14);

    // 3. Developer Profile Card
    int h3 = S(s, 88);
    draw_card(y, h3);
    int av_sz = S(s, 48);
    int av_x = cx + S(s, 20);
    int av_y = y + S(s, 20);
    if (st->avatar) {
        Gdiplus::GraphicsPath ap;
        ap.AddEllipse(av_x, av_y, av_sz, av_sz);
        Gdiplus::GraphicsState saved = g.Save();
        g.SetClip(&ap, Gdiplus::CombineModeIntersect);
        g.DrawImage(st->avatar.get(), av_x, av_y, av_sz, av_sz);
        g.Restore(saved);
    } else {
        Gdiplus::SolidBrush av_br(Gdiplus::Color(255, 37, 99, 235));
        g.FillEllipse(&av_br, av_x, av_y, av_sz, av_sz);
        g.DrawString(L"UK", -1, &f_sec,
                     Gdiplus::RectF(static_cast<float>(av_x), static_cast<float>(av_y),
                                    static_cast<float>(av_sz), static_cast<float>(av_sz)),
                     &sf_c, &white_br);
    }
    int dtx = av_x + av_sz + S(s, 14);
    g.DrawString(L"Utkarsh Kulshrestha", -1, &f_sec,
                 Gdiplus::PointF(static_cast<float>(dtx), static_cast<float>(y + S(s, 16))), &white_br);
    g.DrawString(L"@utkarsh05kul", -1, &f_fdesc,
                 Gdiplus::PointF(static_cast<float>(dtx), static_cast<float>(y + S(s, 38))), &cyan_br);
    g.DrawString(L"Crafted with \u2764 by Utkarsh Kulshrestha", -1, &f_fdesc,
                 Gdiplus::PointF(static_cast<float>(dtx), static_cast<float>(y + S(s, 56))), &body_br);

    int gbw = S(s, 148);
    int gbh = S(s, 32);
    st->rc_btn_github = RECT{cx + card_w - S(s, 20) - gbw, y + (h3 - gbh) / 2, cx + card_w - S(s, 20), y + (h3 + gbh) / 2};
    {
        Gdiplus::RectF gbf(static_cast<float>(st->rc_btn_github.left), static_cast<float>(st->rc_btn_github.top),
                           static_cast<float>(gbw), static_cast<float>(gbh));
        Gdiplus::GraphicsPath gp;
        AddRoundRect(gp, gbf, SF(s, 6.0f));
        Gdiplus::SolidBrush gfill(st->hover_github ? Gdiplus::Color(255, 63, 63, 70) : Gdiplus::Color(255, 39, 39, 42));
        Gdiplus::Pen gbdr(Gdiplus::Color(255, 63, 63, 70), 1.0f);
        g.FillPath(&gfill, &gp);
        g.DrawPath(&gbdr, &gp);
        g.DrawString(L"GitHub Repository", -1, &f_fdesc, gbf, &sf_c, &white_br);
    }
    y += h3 + S(s, 14);

    // 4. Privacy & Disclaimer Card
    int h4 = S(s, 180);
    draw_card(y, h4);
    g.DrawString(L"Privacy & Legal Disclaimer", -1, &f_sec,
                 Gdiplus::PointF(static_cast<float>(cx + S(s, 20)), static_cast<float>(y + S(s, 16))), &cyan_br);
    g.DrawString(
        L"LensAnywhere does not store, collect, or transmit captured screenshots to any external server or "
        L"third-party storage service. Screen captures are passed directly to Google Lens in your default browser.\n\n"
        L"LensAnywhere is an independent open-source utility and is not affiliated, endorsed by, or in any way "
        L"associated with Google LLC. Visual search functionality is provided entirely by Google Lens, and LensAnywhere "
        L"assumes no liability for changes to Google Lens policies, search accuracy, or service availability.",
        -1, &f_body,
        Gdiplus::RectF(static_cast<float>(cx + S(s, 20)), static_cast<float>(y + S(s, 42)),
                       static_cast<float>(card_w - S(s, 40)), static_cast<float>(h4 - S(s, 54))),
        nullptr, &body_br);

    // Scrollbar
    st->rc_sb_thumb = RECT{};
    if (st->max_scroll > 0) {
        int sb_w = S(s, 5);
        int sb_x = cw - m - sb_w;
        int content_h = S(s, 610);
        int thumb_h = std::max(S(s, 30), (vp_h * vp_h) / content_h);
        int thumb_y = vp_top + ((vp_h - thumb_h) * st->scroll_y) / st->max_scroll;
        st->rc_sb_thumb = RECT{sb_x, thumb_y, sb_x + sb_w, thumb_y + thumb_h};
        Gdiplus::GraphicsPath sbp;
        AddRoundRect(sbp, Gdiplus::RectF(static_cast<float>(sb_x), static_cast<float>(thumb_y),
                                         static_cast<float>(sb_w), static_cast<float>(thumb_h)), SF(s, 2.5f));
        Gdiplus::SolidBrush sb_br(Gdiplus::Color(80, 255, 255, 255));
        g.FillPath(&sb_br, &sbp);
    }

    g.ResetClip();

    // Footer sticky button
    int btn_h = S(s, 44);
    int btn_y = ch - S(s, 16) - btn_h;
    st->rc_btn_next = RECT{m, btn_y, cw - m, btn_y + btn_h};
    Gdiplus::RectF nf(static_cast<float>(st->rc_btn_next.left), static_cast<float>(st->rc_btn_next.top),
                      static_cast<float>(st->rc_btn_next.right - st->rc_btn_next.left), static_cast<float>(btn_h));
    Gdiplus::GraphicsPath np;
    AddRoundRect(np, nf, SF(s, 8.0f));
    if (st->unlocked) {
        Gdiplus::SolidBrush nb(st->hover_continue ? Gdiplus::Color(255, 29, 78, 216) : Gdiplus::Color(255, 37, 99, 235));
        g.FillPath(&nb, &np);
        g.DrawString(L"Continue \u2794", -1, &f_btn, nf, &sf_c, &white_br);
    } else {
        Gdiplus::SolidBrush nb(Gdiplus::Color(255, 39, 39, 42));
        Gdiplus::Pen nbp(Gdiplus::Color(255, 63, 63, 70), 1.0f);
        Gdiplus::SolidBrush dis_txt(Gdiplus::Color(255, 113, 113, 122));
        g.FillPath(&nb, &np);
        g.DrawPath(&nbp, &np);
        g.DrawString(L"Scroll Down", -1, &f_btn, nf, &sf_c, &dis_txt);
    }
}

LRESULT CALLBACK WelcomeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<WelcomeWinState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_APP_AVATAR_READY: {
            auto* raw = reinterpret_cast<std::vector<uint8_t>*>(lp);
            if (st && raw && !raw->empty()) {
                st->avatar = LoadPngFromMemory(raw->data(), raw->size());
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            delete raw;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!st) break;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            st->scroll_y = std::clamp(st->scroll_y - (delta * S(st->scale, 48)) / WHEEL_DELTA, 0, st->max_scroll);
            if (!st->unlocked && st->scroll_y >= st->max_scroll - S(st->scale, 20)) {
                st->unlocked = true;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR: {
            if (st && LOWORD(lp) == HTCLIENT) {
                bool hand = st->hover_github || (st->unlocked && st->hover_continue);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (st->dragging_sb && st->max_scroll > 0) {
                int vp_h = st->rc_viewport.bottom - st->rc_viewport.top;
                int th = st->rc_sb_thumb.bottom - st->rc_sb_thumb.top;
                int track = std::max(1, vp_h - th);
                int dy = pt.y - st->drag_start_y;
                st->scroll_y = std::clamp(st->drag_start_scroll + (dy * st->max_scroll) / track, 0, st->max_scroll);
                if (!st->unlocked && st->scroll_y >= st->max_scroll - S(st->scale, 20)) st->unlocked = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            bool hg = PtInRect(&st->rc_viewport, pt) && PtInRect(&st->rc_btn_github, pt);
            bool hc = PtInRect(&st->rc_btn_next, pt);
            if (hg != st->hover_github || hc != st->hover_continue) {
                st->hover_github = hg;
                st->hover_continue = hc;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!st) break;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (PtInRect(&st->rc_sb_thumb, pt)) {
                st->dragging_sb = true;
                st->drag_start_y = pt.y;
                st->drag_start_scroll = st->scroll_y;
                SetCapture(hwnd);
                return 0;
            }
            if (PtInRect(&st->rc_viewport, pt) && PtInRect(&st->rc_btn_github, pt)) {
                ShellExecuteW(nullptr, L"open", L"https://github.com/utkarsh05kul/LensAnywhere-Desktop", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (st->unlocked && PtInRect(&st->rc_btn_next, pt)) {
                AppSettings s = st->callbacks.get_settings ? st->callbacks.get_settings() : AppSettings{};
                s.onboarding_completed = true;
                SaveSettings(s);
                if (st->callbacks.on_settings_changed) st->callbacks.on_settings_changed(s);
                HINSTANCE hi = st->hInst;
                UiCallbacks cb = st->callbacks;
                DestroyWindow(hwnd);
                AppUi::ShowSettingsWindow(hi, cb);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (st && st->dragging_sb) {
                st->dragging_sb = false;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_PAINT: {
            if (!st) break;
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem_dc, bmp);
            PaintWelcomeWindow(st, mem_dc, rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old);
            DeleteObject(bmp);
            DeleteDC(mem_dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY: {
            if (st) {
                auto closed_cb = st->callbacks.on_window_closed;
                delete st;
                g_welcome_win = nullptr;
                if (closed_cb) closed_cb();
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace AppUi {

void ShowSettingsWindow(HINSTANCE hInst, const UiCallbacks& cb) {
    if (g_settings_win && g_settings_win->hwnd && IsWindow(g_settings_win->hwnd)) {
        ShowWindow(g_settings_win->hwnd, SW_RESTORE);
        SetForegroundWindow(g_settings_win->hwnd);
        return;
    }

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kSettingsClass;
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        reg = true;
    }

    auto* st = new SettingsWinState();
    st->hInst = hInst;
    st->callbacks = cb;
    st->settings = cb.get_settings ? cb.get_settings() : LoadSettings();
    st->anim_hotkey = st->settings.hotkey_enabled ? 1.0f : 0.0f;
    st->anim_clip = st->settings.save_to_clipboard ? 1.0f : 0.0f;
    st->anim_startup = st->settings.launch_on_startup ? 1.0f : 0.0f;
    st->logo = LoadEmbeddedLogo();

    UINT sys_dpi = GetDpiForSystem();
    float s = sys_dpi ? (static_cast<float>(sys_dpi) / 96.0f) : 1.0f;
    st->scale = s;

    RECT wr{0, 0, S(s, 500), S(s, 570)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_TOPMOST);
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kSettingsClass,
        L"LensAnywhere - Settings",
        style,
        sx, sy, win_w, win_h,
        nullptr, nullptr, hInst, st
    );
    if (!hwnd) {
        delete st;
        return;
    }

    st->hwnd = hwnd;
    st->scale = GetWindowScale(hwnd);
    g_settings_win = st;
    EnableDarkTitleBar(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void ShowWelcomeWindow(HINSTANCE hInst, const UiCallbacks& cb) {
    if (g_welcome_win && g_welcome_win->hwnd && IsWindow(g_welcome_win->hwnd)) {
        ShowWindow(g_welcome_win->hwnd, SW_RESTORE);
        SetForegroundWindow(g_welcome_win->hwnd);
        return;
    }

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WelcomeWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kWelcomeClass;
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        reg = true;
    }

    auto* st = new WelcomeWinState();
    st->hInst = hInst;
    st->callbacks = cb;
    st->logo = LoadEmbeddedLogo();

    UINT sys_dpi = GetDpiForSystem();
    float s = sys_dpi ? (static_cast<float>(sys_dpi) / 96.0f) : 1.0f;
    st->scale = s;

    RECT wr{0, 0, S(s, 520), S(s, 680)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_TOPMOST);
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kWelcomeClass,
        L"Welcome to LensAnywhere",
        style,
        sx, sy, win_w, win_h,
        nullptr, nullptr, hInst, st
    );
    if (!hwnd) {
        delete st;
        return;
    }

    st->hwnd = hwnd;
    st->scale = GetWindowScale(hwnd);
    g_welcome_win = st;
    EnableDarkTitleBar(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    std::thread([hwnd]() {
        auto bytes = DownloadGitHubAvatarBytes();
        if (!bytes.empty() && IsWindow(hwnd)) {
            auto* heap_vec = new std::vector<uint8_t>(std::move(bytes));
            if (!PostMessageW(hwnd, WM_APP_AVATAR_READY, 0, reinterpret_cast<LPARAM>(heap_vec))) {
                delete heap_vec;
            }
        }
    }).detach();
}

void CloseAll() {
    if (g_key_popup && g_key_popup->hwnd && IsWindow(g_key_popup->hwnd)) {
        DestroyWindow(g_key_popup->hwnd);
    }
    if (g_settings_win && g_settings_win->hwnd && IsWindow(g_settings_win->hwnd)) {
        DestroyWindow(g_settings_win->hwnd);
    }
    if (g_welcome_win && g_welcome_win->hwnd && IsWindow(g_welcome_win->hwnd)) {
        DestroyWindow(g_welcome_win->hwnd);
    }
}

}  // namespace AppUi
