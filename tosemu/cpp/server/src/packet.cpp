#include "packet.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "deflate.h"

namespace tos {

// ---- table -------------------------------------------------------------

void Table::load_csv(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);

    std::string line;
    std::getline(f, line);                       // header
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string dec, hexcol, name, size;
        std::getline(ss, dec, ',');
        std::getline(ss, hexcol, ',');
        std::getline(ss, name, ',');
        std::getline(ss, size, ',');
        if (dec.empty() || size.empty()) continue;

        uint16_t o = uint16_t(std::stoul(dec));
        sizes_[o] = std::stoi(size);
        names_[o] = name;
        // Direction is encoded in the name: CZ_/CB_/CS_ are client->server.
        if (name.rfind("CZ_", 0) == 0 || name.rfind("CB_", 0) == 0 ||
            name.rfind("CS_", 0) == 0)
            client_.insert(o);

        // The same prefixes name the link the packet travels on.
        if (name.rfind("CB_", 0) == 0 || name.rfind("BC_", 0) == 0)
            links_[o] = Link::Barrack;
        else if (name.rfind("CZ_", 0) == 0 || name.rfind("ZC_", 0) == 0)
            links_[o] = Link::Zone;
        else if (name.rfind("CS_", 0) == 0 || name.rfind("SC_", 0) == 0)
            links_[o] = Link::Social;
    }

    // The extractor could not name these three; the client's own table (via
    // Melia's, which matches ours on all 1259 entries) supplies them.
    if (auto it = names_.find(21002); it != names_.end() && sizes_[21002] == 0)
        sizes_[21002] = 64;
}

const std::string& Table::name_of(uint16_t o) const {
    static thread_local std::string tmp;
    auto it = names_.find(o);
    if (it != names_.end()) return it->second;
    tmp = "UNKNOWN_" + std::to_string(o);
    return tmp;
}

Link Table::link_of(uint16_t o) const {
    auto it = links_.find(o);
    return it == links_.end() ? Link::Unknown : it->second;
}

int Table::size_of(uint16_t o) const {
    auto it = sizes_.find(o);
    return it == sizes_.end() ? -1 : it->second;
}

size_t Table::packet_size(const uint8_t* p, size_t avail) const {
    if (avail < 10) return 0;
    uint16_t o = rd16(p);
    int s = size_of(o);
    if (s < 0) return 0;
    if (s > 0) return size_t(s);

    // Variable packets carry their total inline, but WHERE depends on the
    // link. Server->client puts it at +0x0A. Client->server puts it at +0x16,
    // behind 12 bytes the client leaves zero -- every variable c2s packet in
    // the capture agrees (CZ_CHAT, CZ_CHAT_LOG, CZ_QUICKSLOT_LIST,
    // CZ_SAVE_INFO), and reading +0x0A for those returns 0.
    //
    // The social link is the exception: those packets have no extra header, so
    // both directions put the size at +0x0A.
    bool extra_header = is_client_side(o) && link_of(o) != Link::Social;
    size_t at = extra_header ? 0x16 : 0x0A;
    if (avail < at + 2) return 0;
    return rd16(p + at);
}

uint32_t checksum(const uint8_t* p, size_t n) {
    uint32_t s = 0;
    for (size_t j = 0; j < n; ++j)
        s = (j & 1) == 0 ? (uint32_t(p[j]) ^ s) : (s + p[j]);
    return s;
}

// ---- writer ------------------------------------------------------------

PacketWriter::PacketWriter(uint16_t o, const Table* table)
    : op_(o), table_(table), variable_(table && table->is_variable(o)) {
    buf_.resize(10, 0);
    wr16(&buf_[0], o);
    if (variable_) {
        size_field_ = buf_.size();
        buf_.resize(buf_.size() + 2, 0);        // filled in by build()
    }
    body_start_ = buf_.size();
}

void PacketWriter::u16v(uint16_t v) {
    uint8_t t[2];
    wr16(t, v);
    buf_.insert(buf_.end(), t, t + 2);
    bound();
}

void PacketWriter::u32v(uint32_t v) {
    uint8_t t[4];
    wr32(t, v);
    buf_.insert(buf_.end(), t, t + 4);
    bound();
}

void PacketWriter::u64v(uint64_t v) {
    uint8_t t[8];
    wr64(t, v);
    buf_.insert(buf_.end(), t, t + 8);
    bound();
}

void PacketWriter::f32(float v) { u32v(std::bit_cast<uint32_t>(v)); }

void PacketWriter::str(std::string_view s, size_t width) {
    size_t n = std::min(s.size(), width ? width - 1 : 0);
    buf_.insert(buf_.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
    buf_.insert(buf_.end(), width - n, 0);
    bound();
}

void PacketWriter::cstr(std::string_view s) {
    buf_.insert(buf_.end(), s.begin(), s.end());
    buf_.push_back(0);
    bound();
}

void PacketWriter::lpstr(std::string_view s) {
    // The length INCLUDES the terminating NUL, and there is no extra byte
    // after it. Verified against a live ZC_CONNECT_OK: its session key is
    // u16 = 42 followed by 41 characters and a NUL. Writing the length
    // without the NUL leaves the client one byte short and shifts every
    // field after this one.
    u16v(uint16_t(s.size() + 1));
    buf_.insert(buf_.end(), s.begin(), s.end());
    buf_.push_back(0);
    bound();
}

void PacketWriter::bin(const uint8_t* p, size_t n) {
    size_t from = buf_.size();
    buf_.insert(buf_.end(), p, p + n);
    bound();
    mark_opaque(from);
}

void PacketWriter::zeros(size_t n) {
    size_t from = buf_.size();
    buf_.insert(buf_.end(), n, 0);
    bound();
    mark_opaque(from);
}

Bytes PacketWriter::compress_data(
    const std::function<void(PacketWriter&)>& body) const {
    PacketWriter sub(op_, nullptr);
    sub.buf_.clear();                            // sub-packets carry no header
    sub.body_start_ = 0;
    body(sub);
    return deflate_stored(sub.buf_.data(), sub.buf_.size());
}

void PacketWriter::zlib(bool compress,
                        const std::function<void(PacketWriter&)>& body) {
    if (!compress) {
        u16v(0);                                 // "no zlib payload follows"
        body(*this);
        return;
    }
    Bytes packed = compress_data(body);
    u16v(0xFA8D);                                // the client's zlib marker
    u32v(uint32_t(packed.size()));
    bin(packed);
}

bool PacketWriter::size_ok(std::string& why) const {
    if (!table_) return true;
    int declared = table_->size_of(op_);
    if (declared <= 0) return true;              // variable or unknown
    if (size_t(declared) == buf_.size()) return true;
    why = table_->name_of(op_) + " built " + std::to_string(buf_.size()) +
          " bytes, client declares " + std::to_string(declared);
    return false;
}

Bytes PacketWriter::build(uint32_t sequence) {
    if (variable_) wr16(&buf_[size_field_], uint16_t(buf_.size()));
    wr32(&buf_[2], sequence);
    wr32(&buf_[6], 0);
    wr32(&buf_[6], checksum(buf_.data(), buf_.size()));
    return buf_;
}

// ---- reader ------------------------------------------------------------

PacketReader::PacketReader(const uint8_t* p, size_t n, size_t body_start)
    : p_(p), n_(n), pos_(body_start) {
    if (n >= 2) op_ = rd16(p);
    if (body_start > n) { pos_ = n; bad_ = true; }
}

bool PacketReader::want(size_t n) {
    if (pos_ + n > n_) { bad_ = true; return false; }
    return true;
}

uint8_t PacketReader::u8() { return want(1) ? p_[pos_++] : 0; }

uint16_t PacketReader::u16v() {
    if (!want(2)) return 0;
    uint16_t v = rd16(p_ + pos_);
    pos_ += 2;
    return v;
}

uint32_t PacketReader::u32v() {
    if (!want(4)) return 0;
    uint32_t v = rd32(p_ + pos_);
    pos_ += 4;
    return v;
}

uint64_t PacketReader::u64v() {
    if (!want(8)) return 0;
    uint64_t v = rd64(p_ + pos_);
    pos_ += 8;
    return v;
}

float PacketReader::f32() { return std::bit_cast<float>(u32v()); }

std::string PacketReader::str(size_t width) {
    if (!want(width)) return {};
    std::string s = tos::cstr(p_ + pos_, width);
    pos_ += width;
    return s;
}

std::string PacketReader::cstr(size_t max) {
    size_t room = std::min(max, n_ - pos_);
    size_t k = 0;
    while (k < room && p_[pos_ + k]) ++k;
    std::string s(reinterpret_cast<const char*>(p_ + pos_), k);
    pos_ += (k < room) ? k + 1 : k;
    return s;
}

std::string PacketReader::lpstr() {
    // Mirrors the writer: the length covers the NUL, which is trimmed off.
    uint16_t len = u16v();
    if (!want(len)) return {};
    std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
    pos_ += len;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

Bytes PacketReader::bin(size_t n) {
    if (!want(n)) return {};
    Bytes b(p_ + pos_, p_ + pos_ + n);
    pos_ += n;
    return b;
}

}  // namespace tos
