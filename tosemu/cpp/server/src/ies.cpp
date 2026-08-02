#include "ies.h"

#include <algorithm>
#include <cmath>

namespace tos::ies {
namespace {

// Strings are XOR 0x01 and NUL-padded, so the padding decrypts to 0x01 rather
// than 0x00 -- trimming has to drop every trailing non-printable byte, not
// just cut at the first NUL.
std::string decrypt(const uint8_t* p, size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = char(p[i] ^ 0x01);
    while (!s.empty() && (unsigned char)s.back() < 0x21) s.pop_back();
    return s;
}

std::string plain(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (unsigned char)s.back() < 0x21) s.pop_back();
    return s;
}

struct Cursor {
    const uint8_t* p;
    size_t n, i = 0;
    bool bad = false;

    bool room(size_t k) {
        if (i + k > n) { bad = true; return false; }
        return true;
    }
    uint8_t u8() { return room(1) ? p[i++] : 0; }
    uint16_t u16() { if (!room(2)) return 0; uint16_t v = rd16(p + i); i += 2; return v; }
    uint32_t u32() { if (!room(4)) return 0; uint32_t v = rd32(p + i); i += 4; return v; }
    int32_t i32() { return int32_t(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }

    std::string text() {                       // u16 length + XOR'd bytes
        uint16_t len = u16();
        if (!room(len)) return {};
        std::string s = decrypt(p + i, len);
        i += len;
        return s;
    }
};

}  // namespace

bool Table::parse(const uint8_t* data, size_t size) {
    Cursor c{data, size};
    if (!c.room(128 + 28)) return false;

    idspace_ = plain(c.p + c.i, 64); c.i += 64;
    keyspace_ = plain(c.p + c.i, 64); c.i += 64;
    c.u16();                                   // version
    c.u16();                                   // padding
    c.u32();                                   // infoSize
    c.u32();                                   // dataSize
    c.u32();                                   // totalSize
    c.u8();                                    // useClassId
    c.u8();                                    // padding
    uint16_t num_row = c.u16();
    uint16_t num_col = c.u16();
    uint16_t num_number = c.u16();
    uint16_t num_string = c.u16();
    c.u16();                                   // padding

    columns_.clear();
    columns_.reserve(num_col);
    for (uint16_t i = 0; i < num_col; ++i) {
        if (!c.room(64 + 64 + 8)) return false;
        Column col;
        col.name = decrypt(c.p + c.i, 64); c.i += 64;
        col.caption = decrypt(c.p + c.i, 64); c.i += 64;
        col.type = c.u16();
        c.u16();                               // accessData
        c.u16();                               // syncData
        col.decl = c.u16();
        columns_.push_back(std::move(col));
    }

    // Row values are ordered by declIdx within each type group -- see the
    // header comment; getting this wrong reads every field one column over.
    auto ordered = [&](uint16_t type) {
        std::vector<const Column*> v;
        for (const Column& col : columns_)
            if ((col.type == 0) == (type == 0)) v.push_back(&col);
        std::stable_sort(v.begin(), v.end(), [](const Column* a, const Column* b) {
            return a->decl < b->decl;
        });
        return v;
    };

    num_index_.clear();
    txt_index_.clear();
    {
        auto nums = ordered(0);
        for (size_t i = 0; i < nums.size(); ++i) num_index_[nums[i]->name] = i;
        auto txts = ordered(1);
        for (size_t i = 0; i < txts.size(); ++i) txt_index_[txts[i]->name] = i;
    }

    rows_.clear();
    rows_.reserve(num_row);
    for (uint16_t i = 0; i < num_row; ++i) {
        Row r;
        r.class_id = c.i32();
        r.class_name = c.text();
        r.numbers.reserve(num_number);
        for (uint16_t k = 0; k < num_number; ++k) r.numbers.push_back(c.f32());
        r.texts.reserve(num_string);
        for (uint16_t k = 0; k < num_string; ++k) r.texts.push_back(c.text());
        c.i += num_string;                     // one i8 pad per string column
        if (c.bad) return false;
        rows_.push_back(std::move(r));
    }

    by_id_.clear();
    by_name_.clear();
    for (size_t i = 0; i < rows_.size(); ++i) {
        by_id_.emplace(rows_[i].class_id, i);
        if (!rows_[i].class_name.empty()) by_name_.emplace(rows_[i].class_name, i);
    }
    return !c.bad;
}

bool Table::has(const std::string& column) const {
    return num_index_.count(column) || txt_index_.count(column);
}

float Table::number(const Row& r, const std::string& column, float def) const {
    auto it = num_index_.find(column);
    if (it == num_index_.end() || it->second >= r.numbers.size()) return def;
    float v = r.numbers[it->second];
    return std::isfinite(v) ? v : def;
}

int32_t Table::integer(const Row& r, const std::string& column, int32_t def) const {
    auto it = num_index_.find(column);
    if (it == num_index_.end() || it->second >= r.numbers.size()) return def;
    float v = r.numbers[it->second];
    if (!std::isfinite(v)) return def;
    // Values are stored as floats and some genuinely exceed int32 -- Vis
    // (silver) has MaxStack 5e9. Clamping keeps those usable instead of
    // wrapping to a negative, which silently turned a stackable item into a
    // single coin.
    double d = std::floor(double(v) + 0.5);
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return int32_t(d);
}

std::string Table::text(const Row& r, const std::string& column,
                        const std::string& def) const {
    auto it = txt_index_.find(column);
    if (it == txt_index_.end() || it->second >= r.texts.size()) return def;
    return r.texts[it->second];
}

const Row* Table::by_class_id(int32_t id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &rows_[it->second];
}

const Row* Table::by_class_name(const std::string& name) const {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &rows_[it->second];
}

}  // namespace tos::ies
