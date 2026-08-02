#include "schema.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "packet.h"

namespace tos {
namespace {

// packet_schema.json has one fixed shape:
//   { "<opcode>": { "name": s, "size": n, "fields": [[off, type, width], ...] } }
// A reader for exactly that beats taking on a JSON library for one file.
struct Cursor {
    const std::string& s;
    size_t i = 0;

    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }

    bool eat(char c) {
        ws();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }

    std::string str() {
        ws();
        std::string o;
        if (i >= s.size() || s[i] != '"') return o;
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;
            o += s[i++];
        }
        ++i;
        return o;
    }

    long num() {
        ws();
        size_t st = i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '-')) ++i;
        return st == i ? 0 : std::stol(s.substr(st, i - st));
    }

    void skip_value() {
        ws();
        if (i >= s.size()) return;
        if (s[i] == '"') { str(); return; }
        if (s[i] == '[' || s[i] == '{') {
            char open = s[i], close = open == '[' ? ']' : '}';
            int d = 0;
            for (; i < s.size(); ++i) {
                if (s[i] == '"') { str(); --i; continue; }
                if (s[i] == open) ++d;
                else if (s[i] == close && --d == 0) { ++i; return; }
            }
            return;
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    }
};

// ---- client -> server layouts ------------------------------------------
//
// The client builds these rather than parsing them, so there is no reader
// handler to decompile and the extractor produces nothing. Offsets come from
// matched request/response pairs in capture_1785545696.bin (docs/02) and the
// chat work (docs/03). Kept here so the packet log can name their fields and
// the handlers have one place to look up where a position starts.
void builtin_c2s(std::unordered_map<uint16_t, SchemaEntry>& out) {
    auto put = [&](uint16_t op, const char* name, std::vector<Field> fs) {
        SchemaEntry& e = out[op];
        if (e.name.empty()) e.name = name;
        e.fields = std::move(fs);
    };

    put(3166, "CZ_KEYBOARD_MOVE", {
        {0x16, 12, "f32[3]", "pos"},
        {0x22, 8,  "f32[2]", "dir"},
        {0x2A, 4,  "f32",    "client_time"},
        {0x48, 1,  "u8",     "moving"},
    });
    // CZ_MOVE_STOP carries one more leading pad byte than CZ_KEYBOARD_MOVE, so
    // its position starts at +0x17 rather than +0x16.
    put(3172, "CZ_MOVE_STOP", {
        {0x17, 12, "f32[3]", "pos"},
        {0x23, 8,  "f32[2]", "dir"},
        {0x2B, 4,  "f32",    "client_time"},
    });
    put(3184, "CZ_ROTATE", {
        {0x1A, 8, "f32[2]", "dir"},
    });
    put(3168, "CZ_JUMP", {
        {0x17, 12, "f32[3]", "pos"},
        {0x23, 8,  "f32[2]", "dir"},
        {0x2B, 4,  "f32",    "client_time"},
    });
    put(3188, "CZ_CHAT", {
        {0x16, 2, "u16", "size"},
        {0x18, 0, "str", "text"},          // width 0 == read to the NUL
    });
    put(3190, "CZ_CHAT_LOG", {
        {0x16, 2, "u16", "size"},
        {0x18, 0, "str", "text"},
    });
}

}  // namespace

bool PacketSchema::load(const std::string& path) {
    builtin_c2s(entries_);

    Bytes raw;
    if (!read_file(path, raw)) return false;

    std::string txt(raw.begin(), raw.end());
    Cursor c{txt};
    if (!c.eat('{')) return false;

    while (true) {
        c.ws();
        if (c.eat('}')) break;
        std::string key = c.str();
        if (key.empty()) break;
        c.eat(':');
        if (!c.eat('{')) break;

        uint16_t op = uint16_t(std::stoul(key));
        SchemaEntry e;
        while (true) {
            c.ws();
            if (c.eat('}')) break;
            std::string k = c.str();
            c.eat(':');
            if (k == "fields") {
                c.eat('[');
                while (true) {
                    c.ws();
                    if (c.eat(']')) break;
                    if (!c.eat('[')) break;
                    Field f;
                    f.off = uint32_t(c.num());
                    c.eat(',');
                    f.type = c.str();
                    c.eat(',');
                    f.width = uint32_t(c.num());
                    c.eat(']');
                    c.eat(',');
                    // The extractor recovers offsets, not names -- the client
                    // has none to recover. Offset-derived labels at least stay
                    // stable across regenerations.
                    char nm[16];
                    std::snprintf(nm, sizeof nm, "f_%02X", f.off);
                    f.name = nm;
                    e.fields.push_back(std::move(f));
                }
            } else if (k == "name") {
                e.name = c.str();
            } else if (k == "size") {
                e.size = int(c.num());
            } else {
                c.skip_value();
            }
            c.eat(',');
        }

        // A built-in client->server entry already sitting here wins: it was
        // derived from real traffic, the JSON half only covers s2c.
        auto it = entries_.find(op);
        if (it == entries_.end() || it->second.fields.empty())
            entries_[op] = std::move(e);
        c.eat(',');
    }
    return true;
}

const SchemaEntry* PacketSchema::find(uint16_t op) const {
    auto it = entries_.find(op);
    return it == entries_.end() ? nullptr : &it->second;
}

const std::vector<Field>* PacketSchema::fields_for(uint16_t op) const {
    const SchemaEntry* e = find(op);
    return e && !e->fields.empty() ? &e->fields : nullptr;
}

int PacketSchema::verify(const PacketWriter& w,
                         std::vector<std::string>& problems) const {
    const SchemaEntry* e = find(w.op());
    if (!e || e->fields.empty()) return 0;

    const std::vector<uint32_t>& b = w.bounds();
    // The header, and the inline size field for variable packets, are written
    // by the writer itself and are boundaries by construction.
    auto is_bound = [&](uint32_t off) {
        if (off <= uint32_t(w.body_start())) return true;
        return std::find(b.begin(), b.end(), off) != b.end();
    };
    auto in_opaque = [&](uint32_t off) {
        for (const PacketWriter::Range& r : w.opaque())
            if (off >= r.begin && off < r.end) return true;
        return false;
    };

    // A client read that starts mid-field is only a defect if it also runs
    // PAST that field. Reading part of what we wrote is normal -- the
    // extractor records byte-level accesses, so the client picking up the high
    // byte of a short on its own is common and harmless. A read that starts
    // inside one field and ends inside the next is the signature of a real
    // misalignment, and that is what this reports.
    auto contained = [&](uint32_t off, uint32_t width) {
        uint32_t start = uint32_t(w.body_start());
        for (uint32_t end : b) {
            if (off >= start && off < end) return off + width <= end;
            start = end;
        }
        return false;
    };

    int bad = 0;
    for (const Field& f : e->fields) {
        if (f.off + f.width > w.size()) {
            // Past what we wrote: for fixed-size packets the length check
            // already reports the real problem.
            continue;
        }
        if (is_bound(f.off) || in_opaque(f.off)) continue;
        // "str" widths come from a sample, not from the client's read, so they
        // cannot be used to judge containment.
        if (f.type == "str" || contained(f.off, f.width)) continue;

        char t[16];
        std::snprintf(t, sizeof t, "%X", f.off);
        ++bad;
        problems.push_back(e->name + ": client reads " + std::to_string(f.width) +
                           " bytes at +0x" + t + " (" + f.type +
                           ") which straddles two of our fields");
    }
    return bad;
}

}  // namespace tos
