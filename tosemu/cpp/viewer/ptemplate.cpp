#include "ptemplate.h"

#include <windowsx.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "theme.h"

namespace view {

namespace {

const wchar_t* kClass = L"TosTemplateView";

struct State {
    const uint8_t* data = nullptr;
    size_t len = 0;
    Packet pkt;
    bool has_pkt = false;
    const TLayout* lay = nullptr;
    std::vector<TRow> rows;

    int scroll = 0;
    int sel = -1;
    int char_w = 8, row_h = 16, head_h = 20;
};

State* get(HWND h) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

// ------------------------------------------------------------- type parsing

struct Ty {
    enum Base { U, I, F, Str, Bytes } base = U;
    int width = 1;              // bytes per element
    int count = 1;              // elements; 0 means "to the end of the packet"
};

Ty parse_type(const std::string& t) {
    Ty ty;
    size_t br = t.find('[');
    std::string head = br == std::string::npos ? t : t.substr(0, br);
    int n = 1;
    if (br != std::string::npos) n = atoi(t.c_str() + br + 1);

    if (head == "str") {
        ty.base = Ty::Str;
        ty.width = 1;
        ty.count = n;
    } else if (head == "bytes") {
        ty.base = Ty::Bytes;
        ty.width = 1;
        ty.count = n;
    } else if (head[0] == 'f') {
        ty.base = Ty::F;
        ty.width = atoi(head.c_str() + 1) / 8;
        ty.count = n;
    } else {
        ty.base = head[0] == 'i' ? Ty::I : Ty::U;
        ty.width = atoi(head.c_str() + 1) / 8;
        ty.count = n;
    }
    if (ty.width <= 0) ty.width = 1;
    return ty;
}

// ------------------------------------------------------------- value display

std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::wstring fmtw(const wchar_t* f, ...) {
    wchar_t b[512];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf_s(b, 512, _TRUNCATE, f, ap);
    va_end(ap);
    return b;
}

uint64_t read_uint(const uint8_t* p, int w) {
    uint64_t v = 0;
    std::memcpy(&v, p, size_t(w));
    return v;
}

std::wstring scalar_text(const uint8_t* p, const Ty& ty, const std::string& fmt) {
    if (ty.base == Ty::F) {
        if (ty.width == 8) {
            double d;
            std::memcpy(&d, p, 8);
            return fmtw(L"%g", d);
        }
        float f;
        std::memcpy(&f, p, 4);
        // A server-clock reading is meaningless rounded to six digits, and
        // that is what %g would do to 335552.469.
        return fmt == "time" ? fmtw(L"%.3f", double(f)) : fmtw(L"%g", double(f));
    }
    uint64_t v = read_uint(p, ty.width);
    if (fmt == "ip" && ty.width == 4)
        return fmtw(L"%u.%u.%u.%u", unsigned(v & 0xFF), unsigned((v >> 8) & 0xFF),
                    unsigned((v >> 16) & 0xFF), unsigned((v >> 24) & 0xFF));
    if (fmt == "bool" && v <= 1) return v ? L"true" : L"false";
    if (fmt == "hex") {
        switch (ty.width) {
            case 1: return fmtw(L"0x%02X", unsigned(v));
            case 2: return fmtw(L"0x%04X", unsigned(v));
            case 8: return fmtw(L"0x%016llX", (unsigned long long)v);
            default: return fmtw(L"0x%08X", unsigned(v));
        }
    }
    if (ty.base == Ty::I) {
        int64_t s = 0;
        switch (ty.width) {
            case 1: s = int8_t(v); break;
            case 2: s = int16_t(v); break;
            case 4: s = int32_t(v); break;
            default: s = int64_t(v); break;
        }
        return fmtw(L"%lld", (long long)s);
    }
    return fmtw(L"%llu", (unsigned long long)v);
}

std::wstring text_value(const uint8_t* p, size_t n) {
    std::wstring out = L"\"";
    size_t i = 0;
    for (; i < n && p[i]; ++i) {
        if (i >= 96) { out += L"\x2026"; break; }
        out.push_back(p[i] >= 32 && p[i] < 127 ? wchar_t(p[i]) : L'.');
    }
    out += L"\"";
    return out;
}

std::wstring bytes_value(const uint8_t* p, size_t n) {
    std::wstring out;
    for (size_t i = 0; i < n && i < 12; ++i) out += fmtw(L"%02X ", p[i]);
    if (n > 12) out += L"\x2026";
    if (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// The value of a whole field, arrays joined: "1234.5, 0, 567.8".
std::wstring field_value(const uint8_t* p, size_t avail, const Ty& ty,
                         const std::string& fmt) {
    if (ty.base == Ty::Str) return text_value(p, avail);
    if (ty.base == Ty::Bytes) return bytes_value(p, avail);
    std::wstring out;
    int n = ty.count ? ty.count : 1;
    for (int i = 0; i < n; ++i) {
        if (size_t((i + 1) * ty.width) > avail) break;
        if (i) out += L", ";
        if (i >= 6) { out += L"\x2026"; break; }
        out += scalar_text(p + i * ty.width, ty, fmt);
    }
    return out;
}

// Axis names where the field is one -- "pos[1]" says less than "y".
const wchar_t* element_name(const std::string& parent, int i, int n) {
    static const wchar_t* xyz[] = {L"x", L"y", L"z", L"w"};
    static const wchar_t* dxz[] = {L"dx", L"dz"};
    if (n == 3 && (parent == "pos" || parent == "vec3")) return xyz[i];
    if (n == 2 && parent == "dir") return dxz[i];
    if (n <= 4 && (parent == "pos" || parent == "vec4")) return xyz[i];
    return nullptr;
}

// ------------------------------------------------------------------- rows

void add_gap(std::vector<TRow>& out, const uint8_t* data, size_t len,
             uint32_t from, uint32_t to) {
    if (to <= from || from >= len) return;
    if (to > len) to = uint32_t(len);
    TRow r;
    r.kind = TRow::Gap;
    r.name = L"(unread)";
    r.type = fmtw(L"bytes[%u]", to - from);
    r.off = from;
    r.size = to - from;
    r.value = bytes_value(data + from, to - from);
    out.push_back(r);
}

void add_header(std::vector<TRow>& out, const Packet& p, const uint8_t* data,
                size_t len, const TLayout* lay) {
    auto covered = [&](uint32_t off) {
        if (!lay) return false;
        for (const TField& f : lay->fields)
            if (f.off == off) return true;
        return false;
    };
    auto row = [&](const wchar_t* nm, const wchar_t* ty, uint32_t off,
                   uint32_t sz, std::wstring val) {
        if (off + sz > len) return;
        TRow r;
        r.kind = TRow::Header;
        r.name = nm;
        r.type = ty;
        r.off = off;
        r.size = sz;
        r.value = val;
        r.origin = L"wire";
        out.push_back(r);
    };

    std::wstring nm(p.name.begin(), p.name.end());
    row(L"command", L"u16", 0, 2,
        fmtw(L"%u  %s", unsigned(read_uint(data, 2)), nm.c_str()));
    row(L"sequence", L"u32", 2, 4, fmtw(L"%u", unsigned(read_uint(data + 2, 4))));
    row(L"checksum", L"u32", 6, 4,
        fmtw(L"0x%08X  %s", unsigned(read_uint(data + 6, 4)),
             p.chk_ok == 1 ? L"ok" : p.chk_ok == 0 ? L"BAD" : L"?"));

    size_t bs = body_start(p);
    if (bs > 0x0A)
        row(L"client_pad", L"bytes[12]", 0x0A, 12,
            bytes_value(data + 0x0A, 12) + L"   (the server never reads these)");
    // A variable packet carries its total inline, at the start of the body --
    // +0x0A server-side, +0x16 client-side (docs/07). Skip it when the layout
    // already names it, or ZC_CHAT would show its size twice.
    if (p.variable && !covered(uint32_t(bs)))
        row(L"size", L"u16", uint32_t(bs), 2,
            fmtw(L"%u", unsigned(read_uint(data + bs, 2))));
}

}  // namespace

// -------------------------------------------------------------- row building

std::vector<TRow> template_rows(const Packet& p, const uint8_t* data,
                                size_t len, const TLayout* lay) {
    std::vector<TRow> out;
    if (!data || !len) return out;

    // Unframed bytes are not a packet. Shading a header onto them would invent
    // structure that is not there -- the same call hexview makes.
    if (p.flags & PF_UNFRAMED) {
        TRow r;
        r.kind = TRow::Note;
        r.name = L"(unframed)";
        r.type = fmtw(L"bytes[%u]", unsigned(len));
        r.off = 0;
        r.size = uint32_t(len);
        r.value = bytes_value(data, len);
        out.push_back(r);
        return out;
    }

    add_header(out, p, data, len, lay);
    uint32_t cur = uint32_t(out.empty() ? 0 : out.back().off + out.back().size);

    if (!lay) {
        add_gap(out, data, len, cur, uint32_t(len));
        if (!out.empty() && out.back().kind == TRow::Gap) {
            out.back().name = L"body";
            out.back().origin = L"no layout";
        }
        return out;
    }

    for (const TField& f : lay->fields) {
        if (f.off >= len) break;
        if (f.off > cur) add_gap(out, data, len, cur, f.off);
        if (f.off < cur) continue;              // curated overlap, already shown

        Ty ty = parse_type(f.type);
        uint32_t size = f.size;
        if (!size) size = uint32_t(len) - f.off;      // str[0]: to the end
        uint32_t avail = uint32_t(len) - f.off;
        if (size > avail) size = avail;

        TRow r;
        r.kind = f.origin.empty() ? TRow::Guess : TRow::Named;
        r.name = wide(f.name);
        r.type = wide(f.type);
        r.off = f.off;
        r.size = size;
        r.origin = wide(f.origin);
        r.value = field_value(data + f.off, size, ty, f.fmt);
        if (!f.note.empty()) r.value += L"   \x2014 " + wide(f.note);
        out.push_back(r);

        // Break an array into its elements so a position's y can be picked out
        // of the hex on its own.
        if (ty.base != Ty::Str && ty.base != Ty::Bytes && ty.count > 1 &&
            ty.count <= 4) {
            for (int i = 0; i < ty.count; ++i) {
                if (uint32_t((i + 1) * ty.width) > size) break;
                TRow c;
                c.kind = r.kind;
                c.depth = 1;
                const wchar_t* en = element_name(f.name, i, ty.count);
                c.name = en ? std::wstring(en) : fmtw(L"[%d]", i);
                c.type = wide(f.type.substr(0, f.type.find('[')));
                c.off = f.off + uint32_t(i * ty.width);
                c.size = uint32_t(ty.width);
                Ty one = ty;
                one.count = 1;
                c.value = scalar_text(data + c.off, one, f.fmt);
                out.push_back(c);
            }
        }
        cur = f.off + size;
    }
    add_gap(out, data, len, cur, uint32_t(len));
    return out;
}

// ------------------------------------------------------------------ loading

bool Templates::load(const std::wstring& beside) {
    std::vector<std::wstring> roots;
    auto add_parents = [&](std::wstring d) {
        for (int i = 0; i < 4 && !d.empty(); ++i) {
            roots.push_back(d);
            size_t s = d.find_last_of(L"\\/");
            if (s == std::wstring::npos) break;
            d = d.substr(0, s);
        }
    };
    size_t s = beside.find_last_of(L"\\/");
    if (s != std::wstring::npos) add_parents(beside.substr(0, s));
    wchar_t exe[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring e(exe);
        size_t q = e.find_last_of(L"\\/");
        if (q != std::wstring::npos) add_parents(e.substr(0, q));
    }

    for (const auto& r : roots) {
        std::ifstream f((r + L"\\packet_template.tsv").c_str());
        if (!f) continue;
        by_op_.clear();
        std::string line;
        TLayout* cur = nullptr;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> c;
            std::stringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, '\t')) c.push_back(cell);
            if (c.size() >= 5 && c[0] == "P") {
                TLayout l;
                l.op = uint16_t(std::stoul(c[1]));
                l.name = c[2];
                l.size = uint32_t(std::stoul(c[3]));
                l.coverage = c[4];
                cur = &(by_op_[l.op] = l);
            } else if (c.size() >= 5 && c[0] == "F" && cur) {
                TField t;
                t.off = uint32_t(std::stoul(c[1]));
                t.type = c[2];
                t.size = uint32_t(std::stoul(c[3]));
                t.name = c[4];
                if (c.size() > 5) t.fmt = c[5];
                if (c.size() > 6) t.origin = c[6];
                if (c.size() > 7) t.note = c[7];
                cur->fields.push_back(t);
            }
        }
        if (!by_op_.empty()) return true;
    }
    return false;
}

const TLayout* Templates::find(uint16_t op) const {
    auto it = by_op_.find(op);
    return it == by_op_.end() ? nullptr : &it->second;
}

// -------------------------------------------------------------------- window

namespace {

struct Col { const wchar_t* title; int chars; };
const Col kCols[] = {{L"name", 19}, {L"type", 10}, {L"value", 16},
                     {L"offset", 8}, {L"size", 6}, {L"origin", 15}};
const int kColCount = 6;
const int kValueCol = 2;
// Dropped in this order when the panel is too narrow for all six. Origin goes
// first because it is the one thing a reader can get elsewhere (the template
// file says the same); the value never goes, because without it this is a
// list of field names and nothing more.
const int kDropOrder[] = {5, 4, 3};

void metrics(HWND h, State* s) {
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, theme::mono);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"0", 1, &sz);
    s->char_w = sz.cx ? sz.cx : 8;
    s->row_h = tm.tmHeight + theme::dpi_scale(h, 3);
    s->head_h = s->row_h + theme::dpi_scale(h, 4);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
}

int visible_rows(HWND h, const State* s) {
    RECT rc;
    GetClientRect(h, &rc);
    int avail = rc.bottom - rc.top - s->head_h;
    return avail > 0 ? avail / s->row_h : 0;
}

void update_scrollbar(HWND h, State* s) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = s->rows.empty() ? 0 : int(s->rows.size()) - 1;
    si.nPage = UINT(visible_rows(h, s) > 0 ? visible_rows(h, s) : 1);
    si.nPos = s->scroll;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

void clamp_scroll(HWND h, State* s) {
    int max_top = int(s->rows.size()) - visible_rows(h, s);
    if (max_top < 0) max_top = 0;
    if (s->scroll > max_top) s->scroll = max_top;
    if (s->scroll < 0) s->scroll = 0;
}

// The value column takes whatever the fixed columns leave, so a wide panel
// shows a long string instead of padding. When even the minimums do not fit,
// columns are dropped rather than pushed off the right edge -- a header
// reading "offset" with nothing under it is worse than no column at all.
void col_x(HWND h, const State* s, int* x, bool* show) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = theme::dpi_scale(h, 10);
    int avail = rc.right - rc.left - pad * 2;

    for (int i = 0; i < kColCount; ++i) show[i] = true;
    auto minimum = [&]() {
        int n = 0;
        for (int i = 0; i < kColCount; ++i)
            if (show[i]) n += kCols[i].chars * s->char_w;
        return n;
    };
    for (int d : kDropOrder) {
        if (minimum() <= avail) break;
        show[d] = false;
    }

    int fixed = 0;
    for (int i = 0; i < kColCount; ++i)
        if (show[i] && i != kValueCol) fixed += kCols[i].chars * s->char_w;
    int val = avail - fixed;
    if (val < kCols[kValueCol].chars * s->char_w)
        val = kCols[kValueCol].chars * s->char_w;

    int cx = pad;
    for (int i = 0; i < kColCount; ++i) {
        x[i] = cx;
        if (show[i]) cx += (i == kValueCol ? val : kCols[i].chars * s->char_w);
    }
    x[kColCount] = cx;
}

COLORREF row_ink(const TRow& r) {
    switch (r.kind) {
        case TRow::Header: return theme::hdr_seq;
        case TRow::Gap:    return theme::faint;
        case TRow::Guess:  return theme::dim;
        case TRow::Note:   return theme::hdr_op;
        default:           return theme::fg;
    }
}

void draw(HDC dc, HWND h, State* s) {
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, theme::br_panel);
    SetBkMode(dc, TRANSPARENT);

    int x[kColCount + 1];
    bool show[kColCount];
    col_x(h, s, x, show);

    RECT hr{rc.left, rc.top, rc.right, rc.top + s->head_h};
    FillRect(dc, &hr, theme::br_header);
    RECT hl{rc.left, hr.bottom - 1, rc.right, hr.bottom};
    FillRect(dc, &hl, theme::br_edge);
    SelectObject(dc, theme::mono);
    SetTextColor(dc, theme::dim);
    int hy = rc.top + theme::dpi_scale(h, 3);
    for (int i = 0; i < kColCount; ++i)
        if (show[i])
            TextOutW(dc, x[i], hy, kCols[i].title, int(wcslen(kCols[i].title)));

    if (s->rows.empty()) {
        SetTextColor(dc, theme::faint);
        const wchar_t* m = s->has_pkt ? L"(no bytes)" : L"Select a packet";
        TextOutW(dc, x[0], hr.bottom + theme::dpi_scale(h, 4), m,
                 int(wcslen(m)));
        return;
    }

    int rows = visible_rows(h, s);
    int top = hr.bottom + theme::dpi_scale(h, 2);
    bool focus = GetFocus() == h;
    for (int i = 0; i < rows; ++i) {
        int idx = s->scroll + i;
        if (idx >= int(s->rows.size())) break;
        const TRow& r = s->rows[size_t(idx)];
        int y = top + i * s->row_h;

        if (idx == s->sel) {
            RECT sr{rc.left, y, rc.right, y + s->row_h};
            HBRUSH br = CreateSolidBrush(focus ? theme::accent : theme::edge);
            FillRect(dc, &sr, br);
            DeleteObject(br);
        } else if (idx & 1) {
            RECT sr{rc.left, y, rc.right, y + s->row_h};
            FillRect(dc, &sr, theme::br_bg);
        }

        COLORREF ink = idx == s->sel ? theme::fg : row_ink(r);
        std::wstring nm = r.name;
        if (r.depth) nm = L"  \x2514 " + nm;

        struct { const std::wstring* t; int col; COLORREF c; } cells[] = {
            {&nm, 0, ink},
            {&r.type, 1, idx == s->sel ? theme::fg : theme::faint},
            {&r.value, 2, ink},
            {nullptr, 3, idx == s->sel ? theme::fg : theme::faint},
            {nullptr, 4, idx == s->sel ? theme::fg : theme::faint},
            {&r.origin, 5, idx == s->sel ? theme::fg : theme::faint},
        };
        wchar_t ob[24], sb[16];
        _snwprintf_s(ob, 24, _TRUNCATE, L"0x%04X", r.off);
        _snwprintf_s(sb, 16, _TRUNCATE, L"%u", r.size);

        for (auto& c : cells) {
            if (!show[c.col]) continue;
            const wchar_t* txt = c.t ? c.t->c_str() : (c.col == 3 ? ob : sb);
            RECT tr{x[c.col], y, x[c.col + 1] - theme::dpi_scale(h, 6),
                    y + s->row_h};
            SetTextColor(dc, c.c);
            DrawTextW(dc, txt, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        }
    }
}

void notify_pick(HWND h, State* s) {
    HWND parent = GetParent(h);
    if (!parent) return;
    if (s->sel < 0 || s->sel >= int(s->rows.size())) {
        SendMessageW(parent, TPLN_PICK, 0, 0);
        return;
    }
    const TRow& r = s->rows[size_t(s->sel)];
    SendMessageW(parent, TPLN_PICK, WPARAM(r.off), LPARAM(r.size));
}

void scroll_to_sel(HWND h, State* s) {
    if (s->sel < 0) return;
    int vis = visible_rows(h, s);
    if (s->sel < s->scroll) s->scroll = s->sel;
    else if (vis > 0 && s->sel >= s->scroll + vis) s->scroll = s->sel - vis + 1;
    clamp_scroll(h, s);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    State* s = get(h);
    switch (m) {
        case WM_NCCREATE:
            s = new State();
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(s));
            return TRUE;
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
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldb = SelectObject(mem, bmp);
            draw(mem, h, s);
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
        case WM_MOUSEWHEEL:
            s->scroll -= (GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA) * 3;
            clamp_scroll(h, s);
            update_scrollbar(h, s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            int y = GET_Y_LPARAM(l);
            if (y < s->head_h) return 0;
            int idx = (y - s->head_h - theme::dpi_scale(h, 2)) / s->row_h +
                      s->scroll;
            if (idx >= 0 && idx < int(s->rows.size())) {
                s->sel = idx;
                notify_pick(h, s);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN: {
            if (w == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                std::wstring t = ptemplate_text(h);
                if (!t.empty() && OpenClipboard(h)) {
                    EmptyClipboard();
                    size_t bytes = (t.size() + 1) * sizeof(wchar_t);
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (g) {
                        std::memcpy(GlobalLock(g), t.c_str(), bytes);
                        GlobalUnlock(g);
                        SetClipboardData(CF_UNICODETEXT, g);
                    }
                    CloseClipboard();
                }
                return 0;
            }
            if (s->rows.empty()) return 0;
            int c = s->sel < 0 ? 0 : s->sel;
            int page = visible_rows(h, s);
            switch (w) {
                case VK_UP: c--; break;
                case VK_DOWN: c++; break;
                case VK_PRIOR: c -= page; break;
                case VK_NEXT: c += page; break;
                case VK_HOME: c = 0; break;
                case VK_END: c = int(s->rows.size()) - 1; break;
                default: return 0;
            }
            if (c < 0) c = 0;
            if (c >= int(s->rows.size())) c = int(s->rows.size()) - 1;
            s->sel = c;
            scroll_to_sel(h, s);
            update_scrollbar(h, s);
            notify_pick(h, s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void ptemplate_register() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

HWND ptemplate_create(HWND parent, int id) {
    HWND h = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                             0, 0, 100, 100, parent, HMENU(INT_PTR(id)),
                             GetModuleHandleW(nullptr), nullptr);
    theme::dark_scrollbars(h);
    return h;
}

void ptemplate_set(HWND h, const uint8_t* data, size_t len, const Packet* p,
                   const TLayout* lay) {
    State* s = get(h);
    if (!s) return;
    s->data = data;
    s->len = data ? len : 0;
    s->has_pkt = p != nullptr;
    if (p) s->pkt = *p;
    s->lay = lay;
    s->rows = (data && p) ? template_rows(*p, data, len, lay)
                          : std::vector<TRow>();
    s->scroll = 0;
    s->sel = -1;
    metrics(h, s);
    update_scrollbar(h, s);
    InvalidateRect(h, nullptr, FALSE);
}

void ptemplate_sync(HWND h, int off) {
    State* s = get(h);
    if (!s) return;
    int want = -1;
    if (off >= 0) {
        // Last match wins, so clicking inside an array lands on the element
        // rather than on the parent that also covers it.
        for (size_t i = 0; i < s->rows.size(); ++i) {
            const TRow& r = s->rows[i];
            if (uint32_t(off) >= r.off && uint32_t(off) < r.off + r.size)
                want = int(i);
        }
    }
    if (want == s->sel) return;
    s->sel = want;
    scroll_to_sel(h, s);
    update_scrollbar(h, s);
    InvalidateRect(h, nullptr, FALSE);
}

std::wstring ptemplate_text(HWND h) {
    State* s = get(h);
    if (!s || s->rows.empty()) return L"";
    std::wstring out = L"name\ttype\tvalue\toffset\tsize\torigin\r\n";
    wchar_t b[64];
    for (const TRow& r : s->rows) {
        out += (r.depth ? L"  " : L"") + r.name;
        out += L"\t" + r.type + L"\t" + r.value;
        _snwprintf_s(b, 64, _TRUNCATE, L"\t0x%04X\t%u\t", r.off, r.size);
        out += b;
        out += r.origin + L"\r\n";
    }
    return out;
}

}  // namespace view
