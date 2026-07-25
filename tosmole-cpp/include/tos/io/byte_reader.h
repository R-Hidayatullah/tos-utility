// tosmole-cpp — a C++ reimplementation of the tosmole Tree-of-Savior asset toolkit.
//
// ByteReader: a lightweight cursor over an in-memory byte buffer.
//
// All Tree-of-Savior / EMotionFX asset files are little-endian. This reader
// assumes a little-endian host (x86/x64), so fixed-size POD structs can be read
// with a single memcpy that preserves the exact on-disk layout — including the
// C++ natural-alignment padding that the original MSVC exporter wrote via
// `file->Write(&s, sizeof(s))`. Reproducing that padding is essential: the
// Rust reference (tosmole) mis-modelled several structs by placing padding
// after a field where the compiler actually pads *before* the next 4-byte
// aligned field (e.g. XAC_Info2::mRetargetRootOffset, XAC_Node3::mOBB).
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace tos::io {

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : data_(v.data()), size_(v.size()) {}

    size_t tell() const { return pos_; }
    size_t size() const { return size_; }
    size_t remaining() const { return size_ - pos_; }
    bool eof() const { return pos_ >= size_; }
    const uint8_t* ptr() const { return data_ + pos_; }

    void seek(size_t absolute) {
        if (absolute > size_) throw std::out_of_range("ByteReader::seek past end");
        pos_ = absolute;
    }
    void skip(size_t n) { seek(pos_ + n); }

    bool can_read(size_t n) const { return pos_ + n <= size_; }

    // Read a fixed-size trivially-copyable struct as a raw block (matches the
    // on-disk layout, padding included). Little-endian host assumed.
    template <typename T>
    T read() {
        static_assert(std::is_trivially_copyable_v<T>, "read<T> requires POD");
        if (!can_read(sizeof(T)))
            throw std::out_of_range("ByteReader::read<T> past end");
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    uint8_t  read_u8()  { return read<uint8_t>(); }
    uint16_t read_u16() { return read<uint16_t>(); }
    uint32_t read_u32() { return read<uint32_t>(); }
    int32_t  read_i32() { return read<int32_t>(); }
    float    read_f32() { return read<float>(); }

    // EMotionFX string: uint32 length prefix followed by `length` raw bytes
    // (no null terminator).
    std::string read_string() {
        uint32_t len = read_u32();
        if (!can_read(len))
            throw std::out_of_range("ByteReader::read_string past end");
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }

    std::vector<uint8_t> read_bytes(size_t n) {
        if (!can_read(n)) throw std::out_of_range("ByteReader::read_bytes past end");
        std::vector<uint8_t> out(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return out;
    }

    template <typename T>
    std::vector<T> read_array(size_t count) {
        static_assert(std::is_trivially_copyable_v<T>, "read_array<T> requires POD");
        std::vector<T> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) out.push_back(read<T>());
        return out;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
};

} // namespace tos::io
