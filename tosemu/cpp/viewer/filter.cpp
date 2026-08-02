#include "filter.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace view {

const ColumnDef kColumns[COL_COUNT] = {
    {L"#",        70,  true},
    {L"time",     80,  true},
    {L"clock",    92,  false},
    {L"dir",      46,  false},
    {L"conn",     50,  true},
    {L"listen",   56,  true},
    {L"source",   140, false},
    {L"dest",     140, false},
    {L"op",       58,  true},
    {L"name",     250, false},
    {L"len",      54,  true},
    {L"decl",     54,  true},
    {L"wire",     54,  true},
    {L"seq",      70,  true},
    {L"chk",      46,  false},
    {L"link",     66,  false},
    {L"flag",     92,  false},
};

static const wchar_t* kLink[] = {L"", L"barrack", L"zone", L"social"};

std::wstring cell_text(const Packet& p, const Packet& first, int col) {
    wchar_t b[128];
    switch (col) {
        case COL_INDEX:
            _snwprintf_s(b, 128, _TRUNCATE, L"%llu", (unsigned long long)p.index);
            return b;
        case COL_TIME: {
            double t = p.time_us >= first.time_us
                           ? double(p.time_us - first.time_us) / 1e6
                           : 0.0;
            _snwprintf_s(b, 128, _TRUNCATE, L"%.3f", t);
            return b;
        }
        case COL_CLOCK: return format_clock(p.time_us);
        case COL_DIR:   return p.dir ? L"s2c" : L"c2s";
        case COL_CONN:
            _snwprintf_s(b, 128, _TRUNCATE, L"%u", p.conn);
            return b;
        case COL_LISTEN:
            _snwprintf_s(b, 128, _TRUNCATE, L"%u", p.listen_port);
            return b;
        case COL_SRC:
        case COL_DST: {
            uint32_t ip = col == COL_SRC ? p.src_ip : p.dst_ip;
            uint16_t pt = col == COL_SRC ? p.src_port : p.dst_port;
            if (!ip) return L"";
            _snwprintf_s(b, 128, _TRUNCATE, L"%s:%u", ip_to_w(ip).c_str(), pt);
            return b;
        }
        case COL_OPCODE:
            _snwprintf_s(b, 128, _TRUNCATE, L"%u", p.opcode);
            return b;
        case COL_NAME:
            return std::wstring(p.name.begin(), p.name.end());
        case COL_LEN:
            _snwprintf_s(b, 128, _TRUNCATE, L"%u", p.body_len);
            return b;
        case COL_DECL:
            if (!p.declared) return p.variable ? L"var" : L"";
            _snwprintf_s(b, 128, _TRUNCATE, L"%u%s", p.declared,
                         p.variable ? L"*" : L"");
            return b;
        case COL_WIRE:
            _snwprintf_s(b, 128, _TRUNCATE, L"%u", p.wire);
            return b;
        case COL_SEQ:
            _snwprintf_s(b, 128, _TRUNCATE, L"%u", p.seq);
            return b;
        case COL_CHK:
            return p.chk_ok == 1 ? L"ok" : p.chk_ok == 0 ? L"BAD" : L"-";
        case COL_LINK:
            return kLink[p.link & 3];
        case COL_FLAGS: {
            std::wstring s;
            if (p.flags & PF_UNFRAMED) s += L"unframed ";
            else if (p.flags & PF_UNKNOWN_OP) s += L"new-op ";
            if (p.flags & PF_INFERRED_LEN) s += L"len? ";
            if (!s.empty()) s.pop_back();
            return s;
        }
        default:
            return L"";
    }
}

// ---------------------------------------------------------------- filtering

static std::wstring lower(const std::wstring& s) {
    std::wstring o = s;
    for (auto& c : o) c = wchar_t(towlower(c));
    return o;
}

static bool parse_num(const std::wstring& s, uint64_t& out) {
    if (s.empty()) return false;
    wchar_t* endp = nullptr;
    unsigned long long v = (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X'))
                               ? wcstoull(s.c_str() + 2, &endp, 16)
                               : wcstoull(s.c_str(), &endp, 10);
    if (!endp || *endp) return false;
    out = v;
    return true;
}

void Filter::set(const std::wstring& query) {
    terms_.clear();
    std::wstring cur;
    std::vector<std::wstring> words;
    for (wchar_t c : query + L" ") {
        if (c == L' ' || c == L'\t') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }

    for (std::wstring w : words) {
        Term t;
        if (w.size() > 1 && w[0] == L'-') { t.negate = true; w.erase(0, 1); }

        // len>100 / len<500 / size>=... -- the comparison forms first, since
        // they are the only ones without a colon.
        size_t rel = w.find_first_of(L"<>");
        std::wstring head = lower(w.substr(0, rel == std::wstring::npos ? w.size() : rel));
        if (rel != std::wstring::npos && (head == L"len" || head == L"size")) {
            uint64_t n = 0;
            std::wstring rest = w.substr(rel + 1);
            if (!rest.empty() && rest[0] == L'=') rest.erase(0, 1);
            if (parse_num(rest, n)) {
                t.kind = Term::LenCmp;
                t.cmp = w[rel] == L'<' ? -1 : 1;
                t.num = n;
                terms_.push_back(t);
                continue;
            }
        }

        size_t colon = w.find(L':');
        if (colon != std::wstring::npos && colon > 0) {
            std::wstring key = lower(w.substr(0, colon));
            std::wstring val = w.substr(colon + 1);
            std::wstring lv = lower(val);
            uint64_t n = 0;
            bool numeric = parse_num(val, n);
            t.text = lv;
            t.num = n;
            if (key == L"dir" && (lv == L"c2s" || lv == L"s2c")) {
                t.kind = Term::Dir;
                t.num = lv == L"s2c" ? 1 : 0;
            } else if ((key == L"op" || key == L"opcode") && numeric) {
                t.kind = Term::Op;
            } else if (key == L"conn" && numeric) {
                t.kind = Term::Conn;
            } else if (key == L"seq" && numeric) {
                t.kind = Term::Seq;
            } else if (key == L"link") {
                t.kind = Term::Link;
            } else if (key == L"ip") {
                t.kind = Term::Ip;
            } else if (key == L"port" && numeric) {
                t.kind = Term::Port;
            } else if ((key == L"len" || key == L"size") && numeric) {
                t.kind = Term::LenCmp;
                t.cmp = 0;
            } else if (key == L"chk") {
                t.kind = Term::Chk;
            } else if (key == L"flag" || key == L"is") {
                t.kind = Term::Flag;
            } else {
                t.kind = Term::Text;      // unknown key: treat the whole word
                t.text = lower(w);        // as text, so typing narrows anyway
            }
            terms_.push_back(t);
            continue;
        }

        t.kind = Term::Text;
        t.text = lower(w);
        terms_.push_back(t);
    }
}

bool Filter::match(const Packet& p) const {
    for (const Term& t : terms_) {
        bool hit = false;
        switch (t.kind) {
            case Term::Text: {
                std::wstring nm = lower(std::wstring(p.name.begin(), p.name.end()));
                hit = nm.find(t.text) != std::wstring::npos;
                if (!hit) {
                    wchar_t b[16];
                    _snwprintf_s(b, 16, _TRUNCATE, L"%u", p.opcode);
                    hit = t.text == b;
                }
                break;
            }
            case Term::Dir:  hit = p.dir == uint16_t(t.num); break;
            case Term::Op:   hit = p.opcode == uint16_t(t.num); break;
            case Term::Conn: hit = p.conn == uint32_t(t.num); break;
            case Term::Seq:  hit = p.seq == uint32_t(t.num); break;
            case Term::Link: {
                static const wchar_t* names[] = {L"?", L"barrack", L"zone", L"social"};
                hit = t.text == names[p.link & 3];
                break;
            }
            case Term::Ip: {
                std::wstring a = ip_to_w(p.src_ip), b = ip_to_w(p.dst_ip);
                hit = (!a.empty() && a.find(t.text) != std::wstring::npos) ||
                      (!b.empty() && b.find(t.text) != std::wstring::npos);
                break;
            }
            case Term::Port:
                hit = p.listen_port == uint16_t(t.num) ||
                      p.src_port == uint16_t(t.num) ||
                      p.dst_port == uint16_t(t.num);
                break;
            case Term::LenCmp:
                hit = t.cmp < 0   ? p.body_len < t.num
                      : t.cmp > 0 ? p.body_len > t.num
                                  : p.body_len == t.num;
                break;
            case Term::Chk:
                hit = (t.text == L"ok" && p.chk_ok == 1) ||
                      (t.text == L"bad" && p.chk_ok == 0) ||
                      ((t.text == L"?" || t.text == L"none") && p.chk_ok == 2);
                break;
            case Term::Flag:
                hit = ((t.text == L"new" || t.text == L"unknown") &&
                       (p.flags & PF_UNKNOWN_OP)) ||
                      (t.text == L"unframed" && (p.flags & PF_UNFRAMED)) ||
                      (t.text == L"inferred" && (p.flags & PF_INFERRED_LEN)) ||
                      (t.text == L"known" && !(p.flags & PF_UNKNOWN_OP));
                break;
        }
        if (hit == t.negate) return false;
    }
    return true;
}

// ------------------------------------------------------------------ sorting

void sort_view(std::vector<uint32_t>& view, const std::vector<Packet>& pkts,
               int col, bool asc) {
    auto key_num = [&](const Packet& p) -> long long {
        switch (col) {
            case COL_INDEX:  return (long long)p.index;
            case COL_TIME:
            case COL_CLOCK:  return (long long)p.time_us;
            case COL_DIR:    return p.dir;
            case COL_CONN:   return p.conn;
            case COL_LISTEN: return p.listen_port;
            case COL_OPCODE: return p.opcode;
            case COL_LEN:    return p.body_len;
            case COL_DECL:   return p.declared;
            case COL_WIRE:   return p.wire;
            case COL_SEQ:    return p.seq;
            case COL_CHK:    return p.chk_ok;
            case COL_LINK:   return p.link;
            case COL_FLAGS:  return p.flags;
            default:         return 0;
        }
    };
    const bool textual = col == COL_NAME || col == COL_SRC || col == COL_DST;

    std::stable_sort(view.begin(), view.end(), [&](uint32_t a, uint32_t b) {
        const Packet& pa = pkts[a];
        const Packet& pb = pkts[b];
        int c;
        if (textual) {
            if (col == COL_NAME) {
                c = pa.name.compare(pb.name);
            } else {
                // Sort addresses by value, not by their text: 52.5.58.238 must
                // not land between 52.40.x and 52.6.x.
                uint32_t ia = col == COL_SRC ? pa.src_ip : pa.dst_ip;
                uint32_t ib = col == COL_SRC ? pb.src_ip : pb.dst_ip;
                uint32_t ha = _byteswap_ulong(ia), hb = _byteswap_ulong(ib);
                uint16_t sa = col == COL_SRC ? pa.src_port : pa.dst_port;
                uint16_t sb = col == COL_SRC ? pb.src_port : pb.dst_port;
                c = ha < hb ? -1 : ha > hb ? 1 : (sa < sb ? -1 : sa > sb ? 1 : 0);
            }
        } else {
            long long ka = key_num(pa), kb = key_num(pb);
            c = ka < kb ? -1 : ka > kb ? 1 : 0;
        }
        if (c == 0) return a < b;            // stable within equal keys
        return asc ? c < 0 : c > 0;
    });
}

}  // namespace view
