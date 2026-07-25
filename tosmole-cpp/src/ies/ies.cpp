#include "tos/ies/ies.h"
#include "tos/io/byte_reader.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace tos::ies {

using tos::io::ByteReader;

namespace {

constexpr uint8_t XOR_KEY = 1;

// Trim trailing bytes that are neither ASCII-graphic nor ASCII-whitespace
// (mirrors the Rust trim_end_matches predicate: strips NULs / control bytes).
std::string trimTrailing(std::string s) {
    while (!s.empty()) {
        unsigned char c = (unsigned char)s.back();
        bool graphic = (c >= 0x21 && c <= 0x7E);
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0c');
        if (graphic || ws) break;
        s.pop_back();
    }
    return s;
}

// XOR-decrypt then trim (column names, row text).
std::string decrypt(const uint8_t* p, size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = (char)(p[i] ^ XOR_KEY);
    return trimTrailing(std::move(s));
}

// Plain (un-encrypted) fixed field, just trim padding (idspace / keyspace).
std::string plain(const uint8_t* p, size_t n) {
    return trimTrailing(std::string(reinterpret_cast<const char*>(p), n));
}

IESRowText readRowText(ByteReader& r) {
    IESRowText t;
    t.length = r.read_u16();
    if (!r.can_read(t.length)) throw std::out_of_range("IES row text past end");
    t.text = decrypt(r.ptr(), t.length);
    r.skip(t.length);
    return t;
}

} // namespace

IESRoot IESRoot::fromBytes(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    IESRoot root;
    IESHeader& h = root.header;

    if (!r.can_read(64 + 64)) throw std::runtime_error("IES too small for header");
    h.idspace = plain(r.ptr(), 64); r.skip(64);
    h.keyspace = plain(r.ptr(), 64); r.skip(64);
    h.version = r.read_u16();
    h.padding = r.read_u16();
    h.infoSize = r.read_u32();
    h.dataSize = r.read_u32();
    h.totalSize = r.read_u32();
    h.useClassId = r.read_u8();
    h.padding2 = r.read_u8();
    h.numField = r.read_u16();
    h.numColumn = r.read_u16();
    h.numColumnNumber = r.read_u16();
    h.numColumnString = r.read_u16();
    h.padding3 = r.read_u16();

    root.columns.reserve(h.numColumn);
    for (uint16_t i = 0; i < h.numColumn; ++i) {
        IESColumn c;
        if (!r.can_read(64 + 64)) throw std::out_of_range("IES column past end");
        c.column = decrypt(r.ptr(), 64); r.skip(64);
        c.name = decrypt(r.ptr(), 64); r.skip(64);
        c.typeData = r.read_u16();
        c.accessData = r.read_u16();
        c.syncData = r.read_u16();
        c.declIdx = r.read_u16();
        root.columns.push_back(std::move(c));
    }

    root.data.reserve(h.numField);
    for (uint16_t i = 0; i < h.numField; ++i) {
        IESColumnData row;
        row.index = r.read_i32();
        row.rowText = readRowText(r);
        row.floats.reserve(h.numColumnNumber);
        for (uint16_t k = 0; k < h.numColumnNumber; ++k) row.floats.push_back(r.read_f32());
        row.texts.reserve(h.numColumnString);
        for (uint16_t k = 0; k < h.numColumnString; ++k) row.texts.push_back(readRowText(r));
        row.padding.reserve(h.numColumnString);
        for (uint16_t k = 0; k < h.numColumnString; ++k) row.padding.push_back((int8_t)r.read_u8());
        root.data.push_back(std::move(row));
    }

    return root;
}

IESRoot IESRoot::fromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open IES file: " + path);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return fromBytes(buf.data(), buf.size());
}

std::unordered_map<std::string, std::string> IESRoot::extractMeshPathMap() const {
    // Sort columns by (declIdx, typeData) while remembering their original order.
    std::vector<const IESColumn*> sorted;
    sorted.reserve(columns.size());
    for (const auto& c : columns) sorted.push_back(&c);
    std::stable_sort(sorted.begin(), sorted.end(), [](const IESColumn* a, const IESColumn* b) {
        if (a->declIdx != b->declIdx) return a->declIdx < b->declIdx;
        return a->typeData < b->typeData;
    });

    // Find the sorted positions of the "Mesh" and "Path" columns; the text-column
    // index used at row time is (position - 1), mirroring the Rust reference.
    int meshIdx = -1, pathIdx = -1;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i]->column == "Mesh") meshIdx = (int)i - 1;
        else if (sorted[i]->column == "Path") pathIdx = (int)i - 1;
    }
    std::unordered_map<std::string, std::string> map;
    if (meshIdx < 0 || pathIdx < 0) return map;

    auto textAt = [](const IESColumnData& row, int idx) -> std::string {
        if (idx < 0 || idx >= (int)row.texts.size()) return "";
        std::string s = row.texts[idx].text;
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    };
    for (const auto& row : data) {
        std::string mesh = textAt(row, meshIdx);
        std::string path = textAt(row, pathIdx);
        if (mesh.empty() || path.empty()) continue;
        for (char& c : mesh) c = (char)tolower((unsigned char)c);
        map[mesh] = path;
    }
    return map;
}

} // namespace tos::ies
