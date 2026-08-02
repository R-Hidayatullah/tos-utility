// Sequential packet writer and reader.
//
// The old code poked absolute byte offsets into a pre-sized vector, which
// meant every layout was encoded twice -- once in the offsets, once in the
// size -- and a field inserted in the middle silently corrupted everything
// after it. Writing sequentially makes the code read like the client's own
// field order, and `Table` then checks the finished length against the size
// the client declares for that opcode, which catches exactly the mistake the
// offsets were hiding.
//
// Wire header, both directions:
//   u16 opcode | u32 sequence | u32 checksum | [u16 size if variable] | body
#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"

namespace tos {

// ---- opcode table ------------------------------------------------------

// Which of the client's three links an opcode belongs to. Encoded in the name
// the client registers it under -- CB_/BC_ barrack, CZ_/ZC_ zone, CS_/SC_
// social -- which makes it a far more reliable router than the listen port,
// since the port a given client dials is a matter of its own configuration.
enum class Link { Unknown, Barrack, Zone, Social };

class Table {
public:
    void load_csv(const std::string& path);   // packet_opcodes.csv

    const std::string& name_of(uint16_t op) const;
    Link link_of(uint16_t op) const;
    // Declared sizeof for the opcode; 0 == variable, -1 == unknown opcode.
    int size_of(uint16_t op) const;
    bool is_variable(uint16_t op) const { return size_of(op) == 0; }
    bool is_client_side(uint16_t op) const { return client_.count(op) != 0; }

    // Total size of a decrypted packet sitting at `p`, or 0 if undeterminable.
    size_t packet_size(const uint8_t* p, size_t avail) const;

    size_t count() const { return sizes_.size(); }

private:
    std::unordered_map<uint16_t, int> sizes_;
    std::unordered_map<uint16_t, std::string> names_;
    std::unordered_map<uint16_t, Link> links_;
    std::set<uint16_t> client_;   // CZ_/CB_/CS_: inline size sits at +0x16
};

// Matches CClientNet::Send: xor on even index, add on odd.
uint32_t checksum(const uint8_t* p, size_t n);

// ---- writer ------------------------------------------------------------

class PacketWriter {
public:
    // `table` may be null, in which case no size validation happens and the
    // packet is assumed variable if `variable` is set explicitly.
    PacketWriter(uint16_t op, const Table* table);

    uint16_t op() const { return op_; }
    size_t body_size() const { return buf_.size() - body_start_; }
    size_t size() const { return buf_.size(); }

    void u8(uint8_t v) { buf_.push_back(v); bound(); }
    void i8(int8_t v) { u8(uint8_t(v)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16v(uint16_t v);
    void i16v(int16_t v) { u16v(uint16_t(v)); }
    void u32v(uint32_t v);
    void i32v(int32_t v) { u32v(uint32_t(v)); }
    void u64v(uint64_t v);
    void i64v(int64_t v) { u64v(uint64_t(v)); }
    void f32(float v);

    // Fixed-width NUL-padded field. Truncates rather than overflowing, because
    // a name one byte too long would otherwise shift every following field.
    void str(std::string_view s, size_t width);
    // NUL-terminated, no padding.
    void cstr(std::string_view s);
    // u16 length prefix, then the bytes, then a NUL (the client's LpString).
    void lpstr(std::string_view s);

    void bin(const uint8_t* p, size_t n);
    void bin(const Bytes& b) { bin(b.data(), b.size()); }
    void zeros(size_t n);

    void position(float x, float y, float z) { f32(x); f32(y); f32(z); }
    void direction(float cos_v, float sin_v) { f32(cos_v); f32(sin_v); }

    // Overwrite an already-written u16/u32 (for back-patching a count).
    size_t mark() const { return buf_.size(); }
    void patch_u16(size_t at, uint16_t v) { wr16(&buf_[at], v); }
    void patch_u32(size_t at, uint32_t v) { wr32(&buf_[at], v); }

    // Melia's Packet.Zlib: a u16 marker then, when compressed, a u32 length
    // and a raw deflate stream. 0 marks the payload as stored inline.
    void zlib(bool compress, const std::function<void(PacketWriter&)>& body);
    // Melia's CompressData: build a sub-packet body and return it deflated.
    Bytes compress_data(const std::function<void(PacketWriter&)>& body) const;

    // Finish the packet: fill in the inline size for variable opcodes, stamp
    // the sequence and checksum, and hand back the bytes.
    Bytes build(uint32_t sequence);

    // True when the finished length disagrees with the client's declared size.
    // Reported rather than thrown: a wrong length is worth a loud log line,
    // but dropping the packet only turns a visible bug into a silent one.
    bool size_ok(std::string& why) const;

    // Offset after every field written, in order. `PacketSchema::verify` walks
    // this against the offsets recovered from the client's own handlers: a
    // field the client starts reading at +0x2D must be a boundary here too, so
    // an inserted, removed or mis-sized field shows up as a misalignment
    // instead of as garbage on screen.
    const std::vector<uint32_t>& bounds() const { return bounds_; }
    size_t body_start() const { return body_start_; }

    // Byte ranges written as padding or as an opaque blob. The client often
    // reads individual fields out of these -- they are only "opaque" to us --
    // so the verifier must not treat a read inside one as a misalignment.
    struct Range { uint32_t begin, end; };
    const std::vector<Range>& opaque() const { return opaque_; }

private:
    void bound() { bounds_.push_back(uint32_t(buf_.size())); }
    void mark_opaque(size_t from) {
        opaque_.push_back({uint32_t(from), uint32_t(buf_.size())});
    }

    uint16_t op_;
    const Table* table_;
    bool variable_;
    size_t body_start_;
    size_t size_field_ = 0;
    Bytes buf_;
    std::vector<uint32_t> bounds_;
    std::vector<Range> opaque_;
};

// ---- reader ------------------------------------------------------------

class PacketReader {
public:
    PacketReader(const uint8_t* p, size_t n, size_t body_start);

    uint16_t op() const { return op_; }
    size_t left() const { return n_ - pos_; }
    size_t pos() const { return pos_; }
    void seek(size_t at) { pos_ = at < n_ ? at : n_; }
    void skip(size_t n) { pos_ = pos_ + n < n_ ? pos_ + n : n_; }
    bool ok() const { return !bad_; }

    uint8_t u8();
    bool boolean() { return u8() != 0; }
    uint16_t u16v();
    uint32_t u32v();
    uint64_t u64v();
    float f32();
    int32_t i32v() { return int32_t(u32v()); }
    int64_t i64v() { return int64_t(u64v()); }

    std::string str(size_t width);      // fixed-width, NUL-trimmed
    std::string cstr(size_t max);       // NUL-terminated, consumes the NUL
    std::string lpstr();                // u16 length prefix
    Bytes bin(size_t n);

    const uint8_t* raw() const { return p_; }
    size_t total() const { return n_; }

private:
    bool want(size_t n);

    const uint8_t* p_;
    size_t n_;
    size_t pos_;
    uint16_t op_ = 0;
    bool bad_ = false;
};

}  // namespace tos
