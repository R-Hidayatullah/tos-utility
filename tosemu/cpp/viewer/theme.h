// Dark palette, fonts, and the bits of Win32 that have to be talked into
// looking modern.
//
// Common Controls v6 gives modern metrics but a light theme; the dark look is
// ours: the window frame via DWM, the list and header via custom draw, and
// everything else drawn by hand. Nothing here is required for the viewer to
// work, so every call degrades to "looks like a normal window" rather than
// failing, which matters because the dark-mode entry points are version
// dependent.
#pragma once

#include <windows.h>

namespace theme {

// Surfaces
extern COLORREF bg;        // window background
extern COLORREF panel;     // list / hex background
extern COLORREF header;    // column header
extern COLORREF edge;      // separators, splitter
// Text
extern COLORREF fg;        // primary
extern COLORREF dim;       // secondary
extern COLORREF faint;     // offsets, padding
// Accents
extern COLORREF accent;    // selection, focus
extern COLORREF c2s;       // client -> server
extern COLORREF s2c;       // server -> client
extern COLORREF bad;       // failures
extern COLORREF hdr_op;    // opcode field in the hex panel
extern COLORREF hdr_seq;   // sequence field
extern COLORREF hdr_chk;   // checksum field
extern COLORREF hdr_pad;   // the twelve bytes the server never reads

extern HFONT ui, ui_bold, mono, mono_bold;
extern HBRUSH br_bg, br_panel, br_header, br_edge;

void init(HWND ref);            // creates fonts at the window's DPI
void dark_titlebar(HWND h);     // DWM immersive dark mode, where available
void dark_scrollbars(HWND h);   // DarkMode_Explorer theme, where available

int dpi_scale(HWND h, int px);

}  // namespace theme
