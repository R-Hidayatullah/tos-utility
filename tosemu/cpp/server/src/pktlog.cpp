#include "pktlog.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace tos {
namespace {

void json_str(std::ostringstream& o, const std::string& s) {
    o << '"';
    for (char ch : s) {
        switch (ch) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if ((unsigned char)ch < 0x20)
                    o << "\\u" << std::hex << int((unsigned char)ch) << std::dec;
                else
                    o << ch;
        }
    }
    o << '"';
}

void json_float(std::ostringstream& o, float f) {
    if (std::isnan(f) || std::isinf(f)) { o << "null"; return; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", double(f));
    o << buf;
}

void render(std::ostringstream& o, const Field& f, const uint8_t* p, size_t n) {
    if (f.type == "str") {
        size_t i = f.off, e = i;
        size_t stop = f.width ? std::min(n, size_t(f.off) + f.width) : n;
        while (e < stop && p[e]) ++e;
        json_str(o, std::string(reinterpret_cast<const char*>(p + i), e - i));
        return;
    }
    if (f.off + f.width > n) { o << "null"; return; }
    if (f.type == "u8")  { o << unsigned(p[f.off]); return; }
    if (f.type == "u16") { o << rd16(p + f.off); return; }
    if (f.type == "u32") { o << rd32(p + f.off); return; }
    if (f.type == "u64") { o << rd64(p + f.off); return; }
    if (f.type == "f32") { json_float(o, rdf(p + f.off)); return; }
    if (f.type.rfind("f32[", 0) == 0) {
        uint32_t parts = f.width / 4;
        o << '[';
        for (uint32_t i = 0; i < parts; ++i) {
            if (i) o << ',';
            json_float(o, rdf(p + f.off + i * 4));
        }
        o << ']';
        return;
    }
    // Opaque run: hex, capped so a 1280-byte name block cannot flood the log.
    o << '"' << hex(p + f.off, f.width, 32);
    if (f.width > 32) o << "+" << (f.width - 32);
    o << '"';
}

}  // namespace

void PacketLog::load(const std::string& path) { schema_.load(path); }

bool PacketLog::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    out_.open(path, std::ios::out | std::ios::trunc);
    return out_.is_open();
}

std::string PacketLog::to_json(Dir d, const Table& t, uint32_t session,
                               const uint8_t* p, size_t n) const {
    uint16_t op = rd16(p);
    std::ostringstream o;
    o << "{\"dir\":\"" << (d == Dir::C2S ? "c2s" : "s2c") << "\"";
    o << ",\"session\":" << session;
    o << ",\"op\":" << op;
    o << ",\"name\":"; json_str(o, t.name_of(op));
    o << ",\"size\":" << n;
    if (n >= 10) {
        o << ",\"seq\":" << rd32(p + 2);
        o << ",\"checksum\":" << rd32(p + 6);
    }

    const std::vector<Field>* fs = schema_.fields_for(op);
    if (fs && !fs->empty()) {
        o << ",\"fields\":{";
        bool first = true;
        for (const Field& f : *fs) {
            if (f.off >= n) continue;
            if (!first) o << ',';
            first = false;
            json_str(o, f.name);
            o << ':';
            render(o, f, p, n);
        }
        o << '}';
    } else {
        o << ",\"fields\":null";
    }
    o << '}';
    return o.str();
}

void PacketLog::write(Dir d, const Table& t, uint32_t session,
                      const uint8_t* p, size_t n) {
    if (n < 2) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!out_.is_open()) return;
    }
    std::string line = to_json(d, t, session, p, n);
    std::lock_guard<std::mutex> lk(mu_);
    if (!out_.is_open()) return;
    out_ << line << '\n';
    out_.flush();
}

}  // namespace tos
