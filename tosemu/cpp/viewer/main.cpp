// tos_view -- browse a packet dump.
//
// Two panels: the table on the left, the selected packet's bytes on the right.
// Every column sorts, and the filter box narrows the table as you type. The
// table is virtual (LVS_OWNERDATA) and the filter keeps an index vector, so a
// capture with tens of thousands of records stays responsive -- nothing is
// copied per keystroke, only a vector of uint32 rebuilt.
//
// Build:  cmake -S cpp/viewer -B cpp/viewer/build -G Ninja
//         cmake --build cpp/viewer/build
//
// Run:    tos_view.exe [dump.bin]        (or drop a file on the window)

#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#include "capture.h"
#include "filter.h"
#include "hexview.h"
#include "ptemplate.h"
#include "theme.h"

using namespace view;

namespace {

enum { ID_LIST = 100, ID_HEX, ID_FILTER, ID_OPEN, ID_TPL };
enum { ACC_OPEN = 200, ACC_FILTER, ACC_COPY, ACC_ESC };

struct App {
    HWND main = nullptr, list = nullptr, hex = nullptr, edit = nullptr,
         open_btn = nullptr, tpl = nullptr;
    Capture cap;
    Filter filter;
    Templates templates;
    std::vector<uint32_t> view;        // indices into cap.packets(), filtered
    int sort_col = COL_INDEX;
    bool sort_asc = true;
    double split = 0.62;               // fraction of width given to the table
    double vsplit = 0.5;               // of the right column, hex above fields
    bool dragging = false;
    bool vdragging = false;
    bool drawing_selected = false;      // set in item prepaint, read in subitem
    std::vector<char> selected;         // per visible row, refreshed each paint
    std::wstring status_left, status_right;
    int top_h = 38, bottom_h = 24, split_w = 6;
};

App g;

// ------------------------------------------------------------------ helpers

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t b[512];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf_s(b, 512, _TRUNCATE, f, ap);
    va_end(ap);
    return b;
}

const Packet& first_packet() {
    static Packet dummy;
    return g.cap.empty() ? dummy : g.cap.packets()[0];
}

void set_status() {
    size_t total = g.cap.packets().size();
    size_t shown = g.view.size();
    if (!total) {
        g.status_left = L"No file loaded  \x2014  Ctrl+O to open a dump, or drop "
                        L"one on the window";
    } else {
        const Meta& m = g.cap.meta();
        std::wstring name = g.cap.path();
        size_t s = name.find_last_of(L"\\/");
        if (s != std::wstring::npos) name = name.substr(s + 1);
        g.status_left = fmt(L"%s   %s   %llu of %llu packets", name.c_str(),
                            m.v2 ? (m.clean ? L"complete"
                                            : L"CUT SHORT (no trailer)")
                                 : L"legacy TOSCAP",
                            (unsigned long long)shown,
                            (unsigned long long)total);
        if (m.resyncs)
            g.status_left += fmt(L"   %llu bytes skipped past damage",
                                 (unsigned long long)m.resyncs);
    }
    InvalidateRect(g.main, nullptr, FALSE);
}

void rebuild_view(bool keep_selection) {
    int64_t keep = -1;
    if (keep_selection) {
        int sel = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
        if (sel >= 0 && sel < int(g.view.size())) keep = g.view[sel];
    }

    g.view.clear();
    const auto& pk = g.cap.packets();
    g.view.reserve(pk.size());
    for (uint32_t i = 0; i < pk.size(); ++i)
        if (g.filter.empty() || g.filter.match(pk[i])) g.view.push_back(i);
    sort_view(g.view, pk, g.sort_col, g.sort_asc);

    ListView_SetItemCountEx(g.list, int(g.view.size()), LVSICF_NOSCROLL);
    if (keep >= 0) {
        for (size_t i = 0; i < g.view.size(); ++i) {
            if (int64_t(g.view[i]) == keep) {
                ListView_SetItemState(g.list, int(i), LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g.list, int(i), FALSE);
                break;
            }
        }
    }
    InvalidateRect(g.list, nullptr, FALSE);
    set_status();
}

void show_packet(int row) {
    if (row < 0 || row >= int(g.view.size())) {
        hexview_set(g.hex, nullptr, 0, nullptr);
        ptemplate_set(g.tpl, nullptr, 0, nullptr, nullptr);
        return;
    }
    const Packet& p = g.cap.packets()[g.view[row]];
    hexview_set(g.hex, g.cap.body(p), p.body_len, &p);
    ptemplate_set(g.tpl, g.cap.body(p), p.body_len, &p,
                  g.templates.find(p.opcode));
}

void update_sort_arrows() {
    HWND hdr = ListView_GetHeader(g.list);
    for (int i = 0; i < COL_COUNT; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g.sort_col) hi.fmt |= g.sort_asc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hi);
    }
}

void load(const std::wstring& path) {
    std::wstring err;
    Capture next;
    if (!next.load(path, err)) {
        MessageBoxW(g.main, err.c_str(), L"tos_view", MB_OK | MB_ICONWARNING);
        return;
    }
    hexview_set(g.hex, nullptr, 0, nullptr);
    ptemplate_set(g.tpl, nullptr, 0, nullptr, nullptr);
    g.cap = std::move(next);
    // The template file lives with the rest of the generated protocol data, so
    // it is looked for relative to the dump the same way the opcode table is.
    if (!g.templates.ready()) g.templates.load(path);
    rebuild_view(false);
    if (!g.view.empty()) {
        ListView_SetItemState(g.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        show_packet(0);
    }
    std::wstring t = L"tos_view \x2014 " + path;
    SetWindowTextW(g.main, t.c_str());
}

void open_dialog() {
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.main;
    ofn.lpstrFilter = L"Packet dumps (*.bin)\0*.bin\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) load(file);
}

void copy_rows() {
    std::wstring out;
    for (int i = 0; i < COL_COUNT; ++i) {
        out += kColumns[i].title;
        out += i + 1 < COL_COUNT ? L"\t" : L"\r\n";
    }
    int n = 0;
    int row = -1;
    while ((row = ListView_GetNextItem(g.list, row, LVNI_SELECTED)) >= 0) {
        if (row >= int(g.view.size())) break;
        const Packet& p = g.cap.packets()[g.view[row]];
        for (int c = 0; c < COL_COUNT; ++c) {
            out += cell_text(p, first_packet(), c);
            out += c + 1 < COL_COUNT ? L"\t" : L"\r\n";
        }
        if (++n >= 5000) break;
    }
    if (!n || !OpenClipboard(g.main)) return;
    EmptyClipboard();
    size_t bytes = (out.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        memcpy(GlobalLock(h), out.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
    g.status_right = fmt(L"copied %d row(s)", n);
    InvalidateRect(g.main, nullptr, FALSE);
}

// ------------------------------------------------- dark header, via subclass
//
// The header sends NM_CUSTOMDRAW to its parent, and its parent is the list
// view -- so the only place to intercept it is inside the list view's own
// window procedure.

LRESULT CALLBACK list_subclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                               DWORD_PTR) {
    if (m == WM_NOTIFY) {
        NMHDR* nh = reinterpret_cast<NMHDR*>(l);
        if (nh->code == NM_CUSTOMDRAW && nh->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                // Paint the whole strip, not just the cells: the area past the
                // last column belongs to no item, so per-item drawing leaves
                // it in the default light theme.
                RECT hr;
                GetClientRect(nh->hwndFrom, &hr);
                FillRect(cd->hdc, &hr, theme::br_header);
                RECT under{hr.left, hr.bottom - 1, hr.right, hr.bottom};
                FillRect(cd->hdc, &under, theme::br_edge);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                int idx = int(cd->dwItemSpec);
                RECT rc = cd->rc;
                bool hot = (cd->uItemState & CDIS_HOT) != 0;
                HBRUSH br = CreateSolidBrush(hot ? theme::edge : theme::header);
                FillRect(cd->hdc, &rc, br);
                DeleteObject(br);
                RECT line{rc.right - 1, rc.top + 4, rc.right, rc.bottom - 4};
                FillRect(cd->hdc, &line, theme::br_edge);
                RECT under{rc.left, rc.bottom - 1, rc.right, rc.bottom};
                FillRect(cd->hdc, &under, theme::br_edge);

                wchar_t text[64] = {0};
                HDITEMW hi{};
                hi.mask = HDI_TEXT;
                hi.pszText = text;
                hi.cchTextMax = 63;
                Header_GetItem(nh->hwndFrom, idx, &hi);

                bool sorted = idx == g.sort_col;
                SelectObject(cd->hdc, sorted ? theme::ui_bold : theme::ui);
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, sorted ? theme::fg : theme::dim);
                RECT tr = rc;
                tr.left += 8;
                tr.right -= sorted ? 18 : 8;
                DrawTextW(cd->hdc, text, -1, &tr,
                          (kColumns[idx].right ? DT_RIGHT : DT_LEFT) |
                              DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                if (sorted) {
                    int cx = rc.right - 12, cy = (rc.top + rc.bottom) / 2;
                    POINT tri[3];
                    if (g.sort_asc) {
                        tri[0] = {cx - 4, cy + 2};
                        tri[1] = {cx + 4, cy + 2};
                        tri[2] = {cx, cy - 3};
                    } else {
                        tri[0] = {cx - 4, cy - 2};
                        tri[1] = {cx + 4, cy - 2};
                        tri[2] = {cx, cy + 3};
                    }
                    HBRUSH ab = CreateSolidBrush(theme::dim);
                    HGDIOBJ ob = SelectObject(cd->hdc, ab);
                    HGDIOBJ op = SelectObject(cd->hdc, GetStockObject(NULL_PEN));
                    Polygon(cd->hdc, tri, 3);
                    SelectObject(cd->hdc, op);
                    SelectObject(cd->hdc, ob);
                    DeleteObject(ab);
                }
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(h, m, w, l);
}

// ----------------------------------------------------------------- layout

void layout() {
    RECT rc;
    GetClientRect(g.main, &rc);
    int top = theme::dpi_scale(g.main, g.top_h);
    int bot = theme::dpi_scale(g.main, g.bottom_h);
    int sw = theme::dpi_scale(g.main, g.split_w);
    int pad = theme::dpi_scale(g.main, 8);

    int btn_w = theme::dpi_scale(g.main, 78);
    int eh = top - pad;
    MoveWindow(g.open_btn, pad, pad / 2, btn_w, eh - pad / 2, TRUE);
    MoveWindow(g.edit, pad * 2 + btn_w, pad / 2,
               rc.right - pad * 3 - btn_w, eh - pad / 2, TRUE);

    int body_top = top;
    int body_h = rc.bottom - top - bot;
    if (body_h < 40) body_h = 40;
    int lw = int((rc.right - sw) * g.split);
    if (lw < 160) lw = 160;
    if (lw > rc.right - sw - 160) lw = rc.right - sw - 160;
    int hh = int((body_h - sw) * g.vsplit);
    if (hh < 80) hh = 80;
    if (hh > body_h - sw - 80) hh = body_h - sw - 80;
    MoveWindow(g.list, 0, body_top, lw, body_h, TRUE);
    MoveWindow(g.hex, lw + sw, body_top, rc.right - lw - sw, hh, TRUE);
    MoveWindow(g.tpl, lw + sw, body_top + hh + sw, rc.right - lw - sw,
               body_h - hh - sw, TRUE);
}

int splitter_x() {
    RECT rc;
    GetClientRect(g.main, &rc);
    int sw = theme::dpi_scale(g.main, g.split_w);
    int lw = int((rc.right - sw) * g.split);
    if (lw < 160) lw = 160;
    if (lw > rc.right - sw - 160) lw = rc.right - sw - 160;
    return lw;
}

// Top of the horizontal splitter between the hex panel and the field panel,
// in client coordinates.
int splitter_y() {
    RECT rc;
    GetClientRect(g.main, &rc);
    int top = theme::dpi_scale(g.main, g.top_h);
    int bot = theme::dpi_scale(g.main, g.bottom_h);
    int sw = theme::dpi_scale(g.main, g.split_w);
    int body_h = rc.bottom - top - bot;
    if (body_h < 40) body_h = 40;
    int hh = int((body_h - sw) * g.vsplit);
    if (hh < 80) hh = 80;
    if (hh > body_h - sw - 80) hh = body_h - sw - 80;
    return top + hh;
}

void paint_main(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int top = theme::dpi_scale(h, g.top_h);
    int bot = theme::dpi_scale(h, g.bottom_h);
    int sw = theme::dpi_scale(h, g.split_w);

    RECT bar{0, 0, rc.right, top};
    FillRect(dc, &bar, theme::br_bg);
    RECT sep{0, top - 1, rc.right, top};
    FillRect(dc, &sep, theme::br_edge);

    // Border around the filter box, since a dark edit has none of its own.
    RECT er;
    GetWindowRect(g.edit, &er);
    MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&er), 2);
    InflateRect(&er, 1, 1);
    FrameRect(dc, &er, theme::br_edge);

    int lw = splitter_x();
    RECT sp{lw, top, lw + sw, rc.bottom - bot};
    FillRect(dc, &sp, theme::br_bg);
    RECT grip{lw + sw / 2 - 1, (top + rc.bottom - bot) / 2 - 12,
              lw + sw / 2, (top + rc.bottom - bot) / 2 + 12};
    FillRect(dc, &grip, theme::br_edge);

    int sy = splitter_y();
    RECT hsp{lw + sw, sy, rc.right, sy + sw};
    FillRect(dc, &hsp, theme::br_bg);
    RECT hgrip{(lw + sw + rc.right) / 2 - 12, sy + sw / 2 - 1,
               (lw + sw + rc.right) / 2 + 12, sy + sw / 2};
    FillRect(dc, &hgrip, theme::br_edge);

    RECT sb{0, rc.bottom - bot, rc.right, rc.bottom};
    FillRect(dc, &sb, theme::br_bg);
    RECT sbl{0, rc.bottom - bot, rc.right, rc.bottom - bot + 1};
    FillRect(dc, &sbl, theme::br_edge);

    SelectObject(dc, theme::ui);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme::dim);
    RECT tl{theme::dpi_scale(h, 10), rc.bottom - bot, rc.right / 2, rc.bottom};
    DrawTextW(dc, g.status_left.c_str(), -1, &tl,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(dc, theme::faint);
    RECT tr{rc.right / 2, rc.bottom - bot, rc.right - theme::dpi_scale(h, 10),
            rc.bottom};
    DrawTextW(dc, g.status_right.c_str(), -1, &tr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    EndPaint(h, &ps);
}

// ------------------------------------------------------------- notifications

LRESULT on_notify(NMHDR* nh) {
    if (nh->idFrom == ID_LIST) {
        switch (nh->code) {
            case LVN_GETDISPINFOW: {
                NMLVDISPINFOW* di = reinterpret_cast<NMLVDISPINFOW*>(nh);
                if (!(di->item.mask & LVIF_TEXT)) return 0;
                int row = di->item.iItem;
                if (row < 0 || row >= int(g.view.size())) return 0;
                static thread_local std::wstring buf;
                buf = cell_text(g.cap.packets()[g.view[row]], first_packet(),
                                di->item.iSubItem);
                di->item.pszText = const_cast<wchar_t*>(buf.c_str());
                return 0;
            }
            case LVN_COLUMNCLICK: {
                NMLISTVIEW* lv = reinterpret_cast<NMLISTVIEW*>(nh);
                if (lv->iSubItem == g.sort_col) g.sort_asc = !g.sort_asc;
                else { g.sort_col = lv->iSubItem; g.sort_asc = true; }
                update_sort_arrows();
                rebuild_view(true);
                return 0;
            }
            case LVN_ITEMCHANGED: {
                NMLISTVIEW* lv = reinterpret_cast<NMLISTVIEW*>(nh);
                if ((lv->uChanged & LVIF_STATE) &&
                    (lv->uNewState & LVIS_SELECTED))
                    show_packet(lv->iItem);
                return 0;
            }
            case NM_CUSTOMDRAW: {
                NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nh);
                switch (cd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT: {
                        // Snapshot the selection for this paint. Custom draw's
                        // uItemState is not dependable for a list view, and
                        // ListView_GetItemState on a virtual list calls back
                        // into LVN_GETDISPINFO, which only supplies text --
                        // both report every row as selected. LVM_GETNEXTITEM
                        // reads the control's own selection and does neither.
                        g.selected.assign(g.view.size(), 0);
                        int r = -1;
                        while ((r = ListView_GetNextItem(g.list, r,
                                                         LVNI_SELECTED)) >= 0) {
                            if (r >= int(g.selected.size())) break;
                            g.selected[size_t(r)] = 1;
                        }
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    case CDDS_ITEMPREPAINT: {
                        int row = int(cd->nmcd.dwItemSpec);
                        if (row < 0 || row >= int(g.view.size()))
                            return CDRF_DODEFAULT;
                        const Packet& p = g.cap.packets()[g.view[row]];
                        // The control paints a selected row in the system's
                        // colours and ignores clrTextBk for it -- glaringly
                        // light against this theme. Clearing the state bits
                        // hands the painting back to us. The flag is carried
                        // to the subitem stage rather than re-queried: asking
                        // a virtual list for item state calls back into
                        // LVN_GETDISPINFO, which only fills in text.
                        g.drawing_selected =
                            row < int(g.selected.size()) && g.selected[size_t(row)];
                        cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                        // An opcode the table does not have is the thing you
                        // opened the capture to find, so it gets its own
                        // colour rather than blending in with its direction.
                        COLORREF ink = p.flags & (PF_UNKNOWN_OP | PF_UNFRAMED)
                                           ? theme::hdr_op
                                           : (p.dir ? theme::s2c : theme::c2s);
                        cd->clrText = g.drawing_selected ? theme::fg : ink;
                        // Banded rows: the table is wide and the eye loses the
                        // line otherwise.
                        cd->clrTextBk = g.drawing_selected
                                            ? theme::accent
                                            : ((row & 1) ? theme::panel : theme::bg);
                        return CDRF_NOTIFYSUBITEMDRAW;
                    }
                    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                        int row = int(cd->nmcd.dwItemSpec);
                        if (row < 0 || row >= int(g.view.size()))
                            return CDRF_DODEFAULT;
                        const Packet& p = g.cap.packets()[g.view[row]];
                        int col = cd->iSubItem;
                        cd->clrTextBk = g.drawing_selected
                                            ? theme::accent
                                            : ((row & 1) ? theme::panel : theme::bg);
                        if (g.drawing_selected) {
                            cd->clrText = col == COL_CHK && p.chk_ok == 0
                                              ? theme::bad
                                              : theme::fg;
                            return CDRF_NEWFONT;
                        }
                        bool odd = (p.flags & (PF_UNKNOWN_OP | PF_UNFRAMED)) != 0;
                        if (col == COL_CHK && p.chk_ok == 0)
                            cd->clrText = theme::bad;
                        else if (col == COL_FLAGS)
                            cd->clrText = odd ? theme::hdr_op : theme::faint;
                        else if (odd)
                            cd->clrText = theme::hdr_op;
                        else if (col == COL_NAME)
                            cd->clrText = theme::fg;
                        else if (col == COL_INDEX || col == COL_TIME ||
                                 col == COL_CLOCK)
                            cd->clrText = theme::faint;
                        else
                            cd->clrText = p.dir ? theme::s2c : theme::c2s;
                        return CDRF_NEWFONT;
                    }
                    default:
                        return CDRF_DODEFAULT;
                }
            }
            default:
                break;
        }
    }
    return 0;
}

// ------------------------------------------------------------ window proc

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            g.main = h;
            theme::init(h);
            theme::dark_titlebar(h);

            g.open_btn = CreateWindowExW(
                0, L"BUTTON", L"Open\x2026",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, h,
                HMENU(INT_PTR(ID_OPEN)), GetModuleHandleW(nullptr), nullptr);

            g.edit = CreateWindowExW(0, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0,
                                     0, 10, 10, h, HMENU(INT_PTR(ID_FILTER)),
                                     GetModuleHandleW(nullptr), nullptr);
            SendMessageW(g.edit, WM_SETFONT, WPARAM(theme::ui), TRUE);
            SendMessageW(g.edit, EM_SETCUEBANNER, TRUE,
                         LPARAM(L"Filter:  ZC_MOVE   dir:c2s   op:3106   "
                                L"conn:2   ip:52.5.58.238   len>100   -chk:ok"));

            g.list = CreateWindowExW(
                0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA |
                    LVS_SHOWSELALWAYS,
                0, 0, 10, 10, h, HMENU(INT_PTR(ID_LIST)),
                GetModuleHandleW(nullptr), nullptr);
            ListView_SetExtendedListViewStyle(
                g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                            LVS_EX_HEADERDRAGDROP);
            SendMessageW(g.list, WM_SETFONT, WPARAM(theme::ui), TRUE);
            ListView_SetBkColor(g.list, theme::bg);
            ListView_SetTextBkColor(g.list, theme::bg);
            ListView_SetTextColor(g.list, theme::fg);
            theme::dark_scrollbars(g.list);
            SetWindowSubclass(g.list, list_subclass, 1, 0);
            // Strip the header's own theme so our custom draw is what shows.
            SetWindowTheme(ListView_GetHeader(g.list), L"", L"");
            SendMessageW(ListView_GetHeader(g.list), WM_SETFONT,
                         WPARAM(theme::ui), TRUE);

            for (int i = 0; i < COL_COUNT; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                c.fmt = kColumns[i].right ? LVCFMT_RIGHT : LVCFMT_LEFT;
                c.cx = theme::dpi_scale(h, kColumns[i].width);
                c.pszText = const_cast<wchar_t*>(kColumns[i].title);
                ListView_InsertColumn(g.list, i, &c);
            }
            update_sort_arrows();

            g.hex = hexview_create(h, ID_HEX);
            g.tpl = ptemplate_create(h, ID_TPL);
            DragAcceptFiles(h, TRUE);
            set_status();
            return 0;
        }

        case WM_SIZE:
            layout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            paint_main(h);
            return 0;

        case WM_CTLCOLOREDIT: {
            HDC dc = HDC(w);
            SetTextColor(dc, theme::fg);
            SetBkColor(dc, theme::panel);
            return LRESULT(theme::br_panel);
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* di = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (di->CtlID != ID_OPEN) break;
            bool down = (di->itemState & ODS_SELECTED) != 0;
            HBRUSH br = CreateSolidBrush(down ? theme::accent : theme::header);
            FillRect(di->hDC, &di->rcItem, br);
            DeleteObject(br);
            FrameRect(di->hDC, &di->rcItem, theme::br_edge);
            SelectObject(di->hDC, theme::ui);
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, theme::fg);
            DrawTextW(di->hDC, L"Open\x2026", -1, &di->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == ID_OPEN && HIWORD(w) == BN_CLICKED) { open_dialog(); return 0; }
            if (id == ID_FILTER && HIWORD(w) == EN_CHANGE) {
                wchar_t buf[512] = {0};
                GetWindowTextW(g.edit, buf, 511);
                g.filter.set(buf);
                rebuild_view(true);
                return 0;
            }
            if (id == ACC_OPEN) { open_dialog(); return 0; }
            if (id == ACC_FILTER) {
                SetFocus(g.edit);
                SendMessageW(g.edit, EM_SETSEL, 0, -1);
                return 0;
            }
            if (id == ACC_ESC) {
                SetWindowTextW(g.edit, L"");
                SetFocus(g.list);
                return 0;
            }
            if (id == ACC_COPY) {
                HWND f = GetFocus();
                if (f == g.hex || f == g.tpl)
                    SendMessageW(f, WM_KEYDOWN, 'C', 0);
                else copy_rows();
                return 0;
            }
            return 0;
        }

        case WM_NOTIFY:
            return on_notify(reinterpret_cast<NMHDR*>(l));

        case TPLN_PICK: {
            int off = int(w), len = int(l);
            hexview_highlight(g.hex, off, len);
            if (len) g.status_right = fmt(L"field at 0x%04X, %d byte(s)", off,
                                         len);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        case HEXN_CARET: {
            int off = int(w);
            // Clicking a byte should say which field it belongs to -- that is
            // the question a hex panel cannot answer on its own.
            ptemplate_sync(g.tpl, off);
            if (off < 0 || g.view.empty()) {
                g.status_right.clear();
            } else {
                int row = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
                if (row >= 0 && row < int(g.view.size())) {
                    const Packet& p = g.cap.packets()[g.view[row]];
                    const uint8_t* b = g.cap.body(p);
                    size_t left = p.body_len - size_t(off);
                    std::wstring s = fmt(L"offset 0x%04X (%d)   u8 %u", off, off,
                                         b[off]);
                    if (left >= 2) {
                        uint16_t v;
                        memcpy(&v, b + off, 2);
                        s += fmt(L"   u16 %u", v);
                    }
                    if (left >= 4) {
                        uint32_t v;
                        float f;
                        memcpy(&v, b + off, 4);
                        memcpy(&f, b + off, 4);
                        s += fmt(L"   u32 %u   f32 %g", v, double(f));
                    }
                    g.status_right = s;
                }
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP d = HDROP(w);
            wchar_t path[MAX_PATH] = {0};
            if (DragQueryFileW(d, 0, path, MAX_PATH)) load(path);
            DragFinish(d);
            return 0;
        }

        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            RECT rc;
            GetClientRect(h, &rc);
            int lw = splitter_x();
            int sw = theme::dpi_scale(h, g.split_w);
            int top = theme::dpi_scale(h, g.top_h);
            if (g.dragging ||
                (pt.x >= lw && pt.x < lw + sw && pt.y >= top &&
                 pt.y < rc.bottom - theme::dpi_scale(h, g.bottom_h))) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            int sy = splitter_y();
            if (g.vdragging ||
                (pt.x >= lw + sw && pt.y >= sy && pt.y < sy + sw)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            int lw = splitter_x();
            int sw = theme::dpi_scale(h, g.split_w);
            int sy = splitter_y();
            if (x >= lw && x < lw + sw) {
                g.dragging = true;
                SetCapture(h);
            } else if (x >= lw + sw && y >= sy && y < sy + sw) {
                g.vdragging = true;
                SetCapture(h);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!g.dragging && !g.vdragging) break;
            RECT rc;
            GetClientRect(h, &rc);
            if (g.dragging && rc.right > 0) {
                g.split = double(GET_X_LPARAM(l)) / rc.right;
                if (g.split < 0.15) g.split = 0.15;
                if (g.split > 0.85) g.split = 0.85;
            } else if (g.vdragging) {
                int top = theme::dpi_scale(h, g.top_h);
                int bot = theme::dpi_scale(h, g.bottom_h);
                int body_h = rc.bottom - top - bot;
                if (body_h > 0) {
                    g.vsplit = double(GET_Y_LPARAM(l) - top) / body_h;
                    if (g.vsplit < 0.15) g.vsplit = 0.15;
                    if (g.vsplit > 0.85) g.vsplit = 0.85;
                }
            }
            layout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
            if (g.dragging || g.vdragging) {
                g.dragging = g.vdragging = false;
                ReleaseCapture();
            }
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize.x = 780;
            mm->ptMinTrackSize.y = 420;
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int show) {
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    hexview_register();
    ptemplate_register();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"TosViewMain";
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, wc.lpszClassName, L"tos_view",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1400, 820, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    ShowWindow(h, show);
    UpdateWindow(h);

    // Not lpCmdLine: MinGW's wWinMain does not reliably deliver it, and the
    // real command line needs unquoting anyway.
    (void)cmd;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1) load(argv[1]);
        LocalFree(argv);
    }

    ACCEL acc[] = {
        {FVIRTKEY | FCONTROL, 'O', ACC_OPEN},
        {FVIRTKEY | FCONTROL, 'F', ACC_FILTER},
        {FVIRTKEY | FCONTROL, 'C', ACC_COPY},
        {FVIRTKEY, VK_ESCAPE, ACC_ESC},
    };
    HACCEL ha = CreateAcceleratorTableW(acc, int(sizeof(acc) / sizeof(acc[0])));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(h, ha, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return int(msg.wParam);
}
