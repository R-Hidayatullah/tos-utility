#include "hexview.h"

#include <windowsx.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "theme.h"

namespace view {

namespace {

const wchar_t* kClass = L"TosHexView";

struct State {
    const uint8_t* data = nullptr;
    size_t len = 0;
    Packet pkt;
    bool has_pkt = false;

    int scroll = 0;             // first visible row
    int caret = -1;             // byte offset, -1 for none
    int hl_off = 0, hl_len = 0; // field highlight, driven by the template pane
    int bpr = 16;               // bytes per row
    int char_w = 8, row_h = 16, info_h = 60;
};

State* get(HWND h) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

// Which part of the wire header a byte belongs to. Both directions share
// u16 opcode | u32 sequence | u32 checksum; client packets on the barrack and
// zone links then carry twelve bytes the server never reads.
enum Field { F_BODY, F_OP, F_SEQ, F_CHK, F_PAD, F_SIZE };

Field field_at(const State* s, size_t off) {
    if (!s->has_pkt) return F_BODY;
    // Unframed bytes are not a packet, so there is no header to shade and
    // pretending otherwise would invent structure that is not there.
    if (s->pkt.flags & PF_UNFRAMED) return F_BODY;
    if (off < 2) return F_OP;
    if (off < 6) return F_SEQ;
    if (off < 10) return F_CHK;
    size_t bs = body_start(s->pkt);
    if (off < bs) return F_PAD;
    if (s->pkt.variable && off < bs + 2) return F_SIZE;
    return F_BODY;
}

COLORREF field_color(Field f) {
    switch (f) {
        case F_OP:   return theme::hdr_op;
        case F_SEQ:  return theme::hdr_seq;
        case F_CHK:  return theme::hdr_chk;
        case F_PAD:  return theme::hdr_pad;
        case F_SIZE: return theme::s2c;
        default:     return theme::fg;
    }
}

void metrics(HWND h, State* s) {
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, theme::mono);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"0", 1, &sz);
    s->char_w = sz.cx ? sz.cx : 8;
    s->row_h = tm.tmHeight + theme::dpi_scale(h, 2);
    SelectObject(dc, theme::ui);
    GetTextMetricsW(dc, &tm);
    s->info_h = tm.tmHeight * 3 + theme::dpi_scale(h, 18);
    SelectObject(dc, old);
    ReleaseDC(h, dc);

    RECT rc;
    GetClientRect(h, &rc);
    int pad = theme::dpi_scale(h, 10);
    int avail = (rc.right - rc.left - pad * 2) / s->char_w;
    // "OOOO  " + 3 per byte + "  " + 1 per byte
    s->bpr = (avail >= 6 + 16 * 3 + 2 + 16) ? 16 : 8;
}

int total_rows(const State* s) {
    return int((s->len + s->bpr - 1) / s->bpr);
}

int visible_rows(HWND h, const State* s) {
    RECT rc;
    GetClientRect(h, &rc);
    int avail = rc.bottom - rc.top - s->info_h;
    return avail > 0 ? avail / s->row_h : 0;
}

void update_scrollbar(HWND h, State* s) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = total_rows(s) ? total_rows(s) - 1 : 0;
    si.nPage = UINT(visible_rows(h, s) > 0 ? visible_rows(h, s) : 1);
    si.nPos = s->scroll;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

void clamp_scroll(HWND h, State* s) {
    int max_top = total_rows(s) - visible_rows(h, s);
    if (max_top < 0) max_top = 0;
    if (s->scroll > max_top) s->scroll = max_top;
    if (s->scroll < 0) s->scroll = 0;
}

void scroll_to_caret(HWND h, State* s) {
    if (s->caret < 0) return;
    int row = s->caret / s->bpr;
    int vis = visible_rows(h, s);
    if (row < s->scroll) s->scroll = row;
    else if (vis > 0 && row >= s->scroll + vis) s->scroll = row - vis + 1;
    clamp_scroll(h, s);
}

void draw_info(HDC dc, HWND h, State* s, RECT rc) {
    SelectObject(dc, theme::ui);
    SetBkMode(dc, TRANSPARENT);
    int x = theme::dpi_scale(h, 10);
    int y = theme::dpi_scale(h, 6);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);

    if (!s->has_pkt) {
        SetTextColor(dc, theme::dim);
        const wchar_t* msg = L"Select a packet";
        TextOutW(dc, x, y, msg, int(wcslen(msg)));
        return;
    }
    const Packet& p = s->pkt;
    wchar_t b[320];

    SelectObject(dc, theme::ui_bold);
    SetTextColor(dc, p.dir ? theme::s2c : theme::c2s);
    std::wstring nm(p.name.begin(), p.name.end());
    _snwprintf_s(b, 320, _TRUNCATE, L"%s   op %u   %s", nm.c_str(), p.opcode,
                 p.dir ? L"server \x2192 client" : L"client \x2192 server");
    TextOutW(dc, x, y, b, int(wcslen(b)));
    SIZE title_sz;
    GetTextExtentPoint32W(dc, b, int(wcslen(b)), &title_sz);
    int title_end = x + title_sz.cx;
    y += tm.tmHeight + theme::dpi_scale(h, 2);

    SelectObject(dc, theme::ui);
    SetTextColor(dc, theme::dim);
    if (p.flags & PF_UNFRAMED) {
        _snwprintf_s(b, 320, _TRUNCATE,
                     L"%u byte(s) that could not be framed%s    first u16 = %u",
                     p.body_len,
                     p.flags & PF_INFERRED_LEN
                         ? L", delimited by the next valid packet"
                         : L", still looking for the next valid packet",
                     p.opcode);
    } else {
        _snwprintf_s(b, 320, _TRUNCATE,
                     L"seq %u    chk 0x%08X %s    declared %u%s    len %u    "
                     L"wire %u%s",
                     p.seq, p.checksum,
                     p.chk_ok == 1 ? L"ok" : p.chk_ok == 0 ? L"BAD" : L"",
                     p.declared, p.variable ? L" (inline)" : L"", p.body_len,
                     p.wire,
                     p.flags & PF_UNKNOWN_OP ? L"    opcode not in the table"
                                             : L"");
    }
    TextOutW(dc, x, y, b, int(wcslen(b)));
    y += tm.tmHeight + theme::dpi_scale(h, 2);

    std::wstring src = ip_to_w(p.src_ip), dst = ip_to_w(p.dst_ip);
    if (!src.empty())
        _snwprintf_s(b, 320, _TRUNCATE, L"%s   conn %u   %s:%u \x2192 %s:%u",
                     format_clock(p.time_us).c_str(), p.conn, src.c_str(),
                     p.src_port, dst.c_str(), p.dst_port);
    else
        _snwprintf_s(b, 320, _TRUNCATE, L"%s   conn %u   port %u",
                     format_clock(p.time_us).c_str(), p.conn, p.listen_port);
    SetTextColor(dc, theme::faint);
    TextOutW(dc, x, y, b, int(wcslen(b)));

    RECT line{rc.left, rc.top + s->info_h - 1, rc.right, rc.top + s->info_h};
    FillRect(dc, &line, theme::br_edge);

    // The badge is a convenience, not the only signal -- the line above
    // already says chk BAD -- so it is dropped rather than allowed to collide
    // with the packet name in a narrow panel.
    if (p.chk_ok == 0) {
        SelectObject(dc, theme::ui_bold);
        const wchar_t* w = L"checksum mismatch";
        SIZE tsz;
        GetTextExtentPoint32W(dc, w, int(wcslen(w)), &tsz);
        int bx = rc.right - tsz.cx - theme::dpi_scale(h, 10);
        if (bx > title_end + theme::dpi_scale(h, 16)) {
            SetTextColor(dc, theme::bad);
            TextOutW(dc, bx, rc.top + theme::dpi_scale(h, 6), w,
                     int(wcslen(w)));
        }
    }
}

void draw_hex(HDC dc, HWND h, State* s, RECT rc) {
    SelectObject(dc, theme::mono);
    SetBkMode(dc, TRANSPARENT);
    int x0 = theme::dpi_scale(h, 10);
    int top = rc.top + s->info_h + theme::dpi_scale(h, 4);
    int rows = visible_rows(h, s);
    int hex_x = x0 + s->char_w * 6;
    int asc_x = hex_x + s->char_w * (s->bpr * 3 + 1);
    bool focus = GetFocus() == h;

    for (int r = 0; r < rows; ++r) {
        size_t off = size_t(s->scroll + r) * s->bpr;
        if (off >= s->len) break;
        int y = top + r * s->row_h;

        wchar_t ob[16];
        _snwprintf_s(ob, 16, _TRUNCATE, L"%04X", unsigned(off));
        SetTextColor(dc, theme::faint);
        TextOutW(dc, x0, y, ob, 4);

        for (int i = 0; i < s->bpr && off + i < s->len; ++i) {
            size_t o = off + i;
            uint8_t v = s->data[o];
            Field f = field_at(s, o);
            int hx = hex_x + i * 3 * s->char_w;
            int ax = asc_x + i * s->char_w;

            // The field highlight sits under the caret so both stay legible
            // when the caret is inside the highlighted field, which it usually
            // is -- clicking a byte is what selected the field.
            if (s->hl_len && int(o) >= s->hl_off &&
                int(o) < s->hl_off + s->hl_len) {
                RECT fr{hx - 1, y, hx + s->char_w * 2 + 1, y + s->row_h};
                FillRect(dc, &fr, theme::br_header);
                RECT fr2{ax - 1, y, ax + s->char_w + 1, y + s->row_h};
                FillRect(dc, &fr2, theme::br_header);
            }
            if (int(o) == s->caret) {
                RECT sel{hx - 1, y, hx + s->char_w * 2 + 1, y + s->row_h};
                HBRUSH br = CreateSolidBrush(focus ? theme::accent : theme::edge);
                FillRect(dc, &sel, br);
                RECT sel2{ax - 1, y, ax + s->char_w + 1, y + s->row_h};
                FillRect(dc, &sel2, br);
                DeleteObject(br);
            }
            SetTextColor(dc, field_color(f));
            wchar_t hb[4];
            _snwprintf_s(hb, 4, _TRUNCATE, L"%02X", v);
            TextOutW(dc, hx, y, hb, 2);

            wchar_t ac[2] = {v >= 32 && v < 127 ? wchar_t(v) : L'.', 0};
            SetTextColor(dc, v >= 32 && v < 127 ? field_color(f) : theme::faint);
            TextOutW(dc, ax, y, ac, 1);
        }
    }

    if (s->len == 0) {
        SetTextColor(dc, theme::faint);
        const wchar_t* m = L"(no bytes)";
        TextOutW(dc, x0, top, m, int(wcslen(m)));
    }
}

void notify_caret(HWND h, State* s) {
    HWND parent = GetParent(h);
    if (parent)
        SendMessageW(parent, HEXN_CARET, WPARAM(s->caret < 0 ? -1 : s->caret),
                     LPARAM(h));
}

void copy_to_clipboard(HWND h) {
    std::wstring text = hexview_text(h);
    if (text.empty() || !OpenClipboard(h)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void* p = GlobalLock(g);
        std::memcpy(p, text.c_str(), bytes);
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    State* s = get(h);
    switch (m) {
        case WM_NCCREATE: {
            s = new State();
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(s));
            return TRUE;
        }
        case WM_CREATE:
            metrics(h, s);
            return 0;
        case WM_DESTROY:
            delete s;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
        case WM_SIZE:
            metrics(h, s);
            clamp_scroll(h, s);
            update_scrollbar(h, s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;                       // painted whole in WM_PAINT
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            // Double buffer: the hex grid is a lot of small TextOut calls and
            // flickers badly drawn straight to the window.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldb = SelectObject(mem, bmp);
            FillRect(mem, &rc, theme::br_panel);
            draw_info(mem, h, s, rc);
            draw_hex(mem, h, s, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldb);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_VSCROLL: {
            int page = visible_rows(h, s);
            switch (LOWORD(w)) {
                case SB_LINEUP: s->scroll--; break;
                case SB_LINEDOWN: s->scroll++; break;
                case SB_PAGEUP: s->scroll -= page; break;
                case SB_PAGEDOWN: s->scroll += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si{};
                    si.cbSize = sizeof(si);
                    si.fMask = SIF_TRACKPOS;
                    GetScrollInfo(h, SB_VERT, &si);
                    s->scroll = si.nTrackPos;
                    break;
                }
                default: break;
            }
            clamp_scroll(h, s);
            update_scrollbar(h, s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            s->scroll -= (delta / WHEEL_DELTA) * 3;
            clamp_scroll(h, s);
            update_scrollbar(h, s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            int x0 = theme::dpi_scale(h, 10);
            int top = s->info_h + theme::dpi_scale(h, 4);
            int hex_x = x0 + s->char_w * 6;
            int asc_x = hex_x + s->char_w * (s->bpr * 3 + 1);
            if (y >= top && s->len) {
                int row = (y - top) / s->row_h + s->scroll;
                int col = -1;
                if (x >= hex_x && x < asc_x - s->char_w)
                    col = (x - hex_x) / (3 * s->char_w);
                else if (x >= asc_x)
                    col = (x - asc_x) / s->char_w;
                if (col >= 0 && col < s->bpr) {
                    size_t off = size_t(row) * s->bpr + col;
                    if (off < s->len) {
                        s->caret = int(off);
                        notify_caret(h, s);
                        InvalidateRect(h, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            if (w == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                copy_to_clipboard(h);
                return 0;
            }
            if (!s->len) return 0;
            int c = s->caret < 0 ? 0 : s->caret;
            int page = visible_rows(h, s) * s->bpr;
            switch (w) {
                case VK_LEFT:  c -= 1; break;
                case VK_RIGHT: c += 1; break;
                case VK_UP:    c -= s->bpr; break;
                case VK_DOWN:  c += s->bpr; break;
                case VK_PRIOR: c -= page; break;
                case VK_NEXT:  c += page; break;
                case VK_HOME:  c = 0; break;
                case VK_END:   c = int(s->len) - 1; break;
                default: return 0;
            }
            if (c < 0) c = 0;
            if (c >= int(s->len)) c = int(s->len) - 1;
            s->caret = c;
            scroll_to_caret(h, s);
            update_scrollbar(h, s);
            notify_caret(h, s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void hexview_register() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

HWND hexview_create(HWND parent, int id) {
    HWND h = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                             0, 0, 100, 100, parent, HMENU(INT_PTR(id)),
                             GetModuleHandleW(nullptr), nullptr);
    theme::dark_scrollbars(h);
    return h;
}

void hexview_set(HWND h, const uint8_t* data, size_t len, const Packet* p) {
    State* s = get(h);
    if (!s) return;
    s->data = data;
    s->len = data ? len : 0;
    s->has_pkt = p != nullptr;
    if (p) s->pkt = *p;
    s->scroll = 0;
    s->caret = -1;
    s->hl_off = s->hl_len = 0;
    metrics(h, s);
    update_scrollbar(h, s);
    notify_caret(h, s);
    InvalidateRect(h, nullptr, FALSE);
}

void hexview_highlight(HWND h, int off, int len) {
    State* s = get(h);
    if (!s) return;
    s->hl_off = off;
    s->hl_len = len;
    // Scroll the field into view, but leave the caret alone: the caret is the
    // user's place in the bytes and the highlight is a consequence of it.
    if (len > 0 && off >= 0 && size_t(off) < s->len) {
        int row = off / s->bpr;
        int vis = visible_rows(h, s);
        if (row < s->scroll) s->scroll = row;
        else if (vis > 0 && row >= s->scroll + vis) s->scroll = row - vis + 1;
        clamp_scroll(h, s);
        update_scrollbar(h, s);
    }
    InvalidateRect(h, nullptr, FALSE);
}

std::wstring hexview_text(HWND h) {
    State* s = get(h);
    if (!s || !s->data || !s->len) return L"";
    std::wstring out;
    wchar_t b[160];
    if (s->has_pkt) {
        std::wstring nm(s->pkt.name.begin(), s->pkt.name.end());
        _snwprintf_s(b, 160, _TRUNCATE,
                     L"%s op=%u %s seq=%u len=%u\r\n", nm.c_str(), s->pkt.opcode,
                     s->pkt.dir ? L"s2c" : L"c2s", s->pkt.seq,
                     unsigned(s->len));
        out += b;
    }
    for (size_t off = 0; off < s->len; off += 16) {
        _snwprintf_s(b, 160, _TRUNCATE, L"%04X  ", unsigned(off));
        out += b;
        std::wstring ascii;
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < s->len) {
                uint8_t v = s->data[off + i];
                _snwprintf_s(b, 160, _TRUNCATE, L"%02X ", v);
                out += b;
                ascii.push_back(v >= 32 && v < 127 ? wchar_t(v) : L'.');
            } else {
                out += L"   ";
            }
        }
        out += L" |" + ascii + L"|\r\n";
    }
    return out;
}

}  // namespace view
