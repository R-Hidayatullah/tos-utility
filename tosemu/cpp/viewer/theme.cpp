#include "theme.h"

#include <uxtheme.h>

namespace theme {

COLORREF bg      = RGB(24, 26, 30);
COLORREF panel   = RGB(30, 33, 38);
COLORREF header  = RGB(38, 42, 48);
COLORREF edge    = RGB(52, 57, 65);
COLORREF fg      = RGB(226, 230, 236);
COLORREF dim     = RGB(150, 158, 170);
COLORREF faint   = RGB(104, 112, 124);
COLORREF accent  = RGB(38, 79, 120);
COLORREF c2s     = RGB(126, 200, 227);
COLORREF s2c     = RGB(181, 208, 130);
COLORREF bad     = RGB(232, 116, 116);
COLORREF hdr_op  = RGB(230, 180, 100);
COLORREF hdr_seq = RGB(150, 180, 230);
COLORREF hdr_chk = RGB(196, 150, 220);
COLORREF hdr_pad = RGB(96, 104, 116);

HFONT ui = nullptr, ui_bold = nullptr, mono = nullptr, mono_bold = nullptr;
HBRUSH br_bg = nullptr, br_panel = nullptr, br_header = nullptr, br_edge = nullptr;

namespace {

using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

// GetProcAddress returns a generic function pointer; casting it straight to a
// typed one is a warning, so route through void(*)() as the standard allows.
template <typename Fn>
Fn proc_address(HMODULE m, const char* name) {
    if (!m) return nullptr;
    return reinterpret_cast<Fn>(
        reinterpret_cast<void (*)()>(GetProcAddress(m, name)));
}

GetDpiForWindowFn dpi_fn() {
    static GetDpiForWindowFn f = proc_address<GetDpiForWindowFn>(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    return f;
}

HFONT make_font(const wchar_t* face, int pt, int dpi, bool boldish) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0,
                       boldish ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

}  // namespace

int dpi_scale(HWND h, int px) {
    UINT d = dpi_fn() ? dpi_fn()(h) : 96;
    return MulDiv(px, int(d ? d : 96), 96);
}

void init(HWND ref) {
    if (ui) return;
    UINT d = dpi_fn() && ref ? dpi_fn()(ref) : 96;
    if (!d) d = 96;
    ui = make_font(L"Segoe UI", 9, int(d), false);
    ui_bold = make_font(L"Segoe UI", 9, int(d), true);
    // Cascadia Mono ships with Windows Terminal and looks far better than
    // Consolas; CreateFont falls back on its own if it is not installed.
    mono = make_font(L"Cascadia Mono", 9, int(d), false);
    mono_bold = make_font(L"Cascadia Mono", 9, int(d), true);
    br_bg = CreateSolidBrush(bg);
    br_panel = CreateSolidBrush(panel);
    br_header = CreateSolidBrush(header);
    br_edge = CreateSolidBrush(edge);
}

void dark_titlebar(HWND h) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    auto set = proc_address<DwmSetWindowAttributeFn>(dwm, "DwmSetWindowAttribute");
    if (set) {
        BOOL on = TRUE;
        // 20 on current builds, 19 on 1809-1903. Trying both is cheaper than
        // working out which build we are on.
        if (FAILED(set(h, 20, &on, sizeof(on)))) set(h, 19, &on, sizeof(on));
    }
    FreeLibrary(dwm);
}

void dark_scrollbars(HWND h) {
    // Present since 1809. On older builds this is a no-op and the scrollbars
    // stay light, which is ugly but harmless.
    SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
}

}  // namespace theme
