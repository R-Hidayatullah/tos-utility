// IES table reader -- the client's own tabular data (maps, monsters, items,
// skills, spawn anchors).
//
// Format (little-endian, packed):
//   header:  idspace[64], keyspace[64] (plain, NUL-padded),
//            u16 version, u16 pad, u32 infoSize, u32 dataSize, u32 totalSize,
//            u8 useClassId, u8 pad, u16 numRow, u16 numColumn,
//            u16 numColumnNumber, u16 numColumnString, u16 pad
//   columns: numColumn * { column[64], name[64] (XOR 0x01), u16 typeData,
//                          u16 accessData, u16 syncData, u16 declIdx }
//   rows:    numRow * { i32 classId, Text className,
//                       numColumnNumber * f32, numColumnString * Text,
//                       numColumnString * i8 }
//   Text:    u16 length, length bytes (XOR 0x01)
//
// The part that is easy to get wrong: **row values are not in file column
// order**. Numeric values are ordered by the numeric columns' declIdx, string
// values by the string columns' declIdx. Verified against ground truth --
// map.ies row 1021 (f_siauliai_west) decodes to ClassID 1021 and DefGenX/Y/Z
// (-599, 260, -1377) only under that ordering; file order and name order both
// produce garbage.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"

namespace tos::ies {

struct Column {
    std::string name;        // internal id, e.g. "ClassID"
    std::string caption;     // display name
    uint16_t type = 0;       // 0 == number, 1 == string
    uint16_t decl = 0;       // declaration index -- the value ordering key
};

struct Row {
    int32_t class_id = 0;    // the row index; property tables key off this
    std::string class_name;
    std::vector<float> numbers;
    std::vector<std::string> texts;
};

class Table {
public:
    bool parse(const uint8_t* data, size_t size);
    bool parse(const Bytes& b) { return parse(b.data(), b.size()); }

    const std::string& idspace() const { return idspace_; }
    const std::vector<Row>& rows() const { return rows_; }
    const std::vector<Column>& columns() const { return columns_; }
    size_t size() const { return rows_.size(); }

    bool has(const std::string& column) const;
    // Named access; `def` is returned when the column or row value is absent.
    float number(const Row& r, const std::string& column, float def = 0) const;
    int32_t integer(const Row& r, const std::string& column, int32_t def = 0) const;
    std::string text(const Row& r, const std::string& column,
                     const std::string& def = "") const;

    const Row* by_class_id(int32_t id) const;
    const Row* by_class_name(const std::string& name) const;

private:
    std::string idspace_, keyspace_;
    std::vector<Column> columns_;
    std::vector<Row> rows_;
    // column name -> index into Row::numbers / Row::texts
    std::unordered_map<std::string, size_t> num_index_, txt_index_;
    std::unordered_map<int32_t, size_t> by_id_;
    std::unordered_map<std::string, size_t> by_name_;
};

}  // namespace tos::ies
