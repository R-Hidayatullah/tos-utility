// IES table parser — C++ port of tosmole's src/ies.rs.
//
// IES is Tree-of-Savior's tabular data format (client-side databases: items,
// monsters, maps, ...). Layout (all little-endian, no struct padding — fields
// are packed exactly as written):
//   Header:  idspace[64], keyspace[64] (plain, NUL-padded),
//            u16 version, u16 pad, u32 infoSize, u32 dataSize, u32 totalSize,
//            u8 useClassId, u8 pad2, u16 numField (= row count),
//            u16 numColumn, u16 numColumnNumber, u16 numColumnString, u16 pad3
//   Columns: numColumn * { column[64], name[64] (XOR 0x01 encrypted),
//                          u16 typeData, u16 accessData, u16 syncData, u16 declIdx }
//   Rows:    numField  * { i32 index, RowText key,
//                          numColumnNumber * f32,
//                          numColumnString * RowText,
//                          numColumnString * i8 padding }
//   RowText: u16 length, length bytes (XOR 0x01 encrypted)
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tos::ies {

struct IESColumn {
    std::string column;   // internal column id (decrypted)
    std::string name;     // display name (decrypted)
    uint16_t typeData = 0;
    uint16_t accessData = 0;
    uint16_t syncData = 0;
    uint16_t declIdx = 0;
};

struct IESRowText {
    uint16_t length = 0;
    std::string text;     // decrypted, trailing NUL/control bytes trimmed
};

struct IESColumnData {
    int32_t index = 0;
    IESRowText rowText;                 // the row's primary key string
    std::vector<float> floats;          // numeric columns
    std::vector<IESRowText> texts;      // string columns
    std::vector<int8_t> padding;        // one i8 per string column
};

struct IESHeader {
    std::string idspace;
    std::string keyspace;
    uint16_t version = 0;
    uint16_t padding = 0;
    uint32_t infoSize = 0;
    uint32_t dataSize = 0;
    uint32_t totalSize = 0;
    uint8_t useClassId = 0;
    uint8_t padding2 = 0;
    uint16_t numField = 0;          // number of data rows
    uint16_t numColumn = 0;
    uint16_t numColumnNumber = 0;   // numeric columns per row
    uint16_t numColumnString = 0;   // string columns per row
    uint16_t padding3 = 0;
};

struct IESRoot {
    IESHeader header;
    std::vector<IESColumn> columns;
    std::vector<IESColumnData> data;

    // Parse a full IES file from memory. Throws std::runtime_error on malformed
    // input (out-of-range read).
    static IESRoot fromBytes(const uint8_t* data, size_t size);
    static IESRoot fromFile(const std::string& path);

    // Port of IESRoot::extract_mesh_path_map: map "Mesh" -> "Path" text columns
    // (lowercased mesh name -> forward-slash path). Empty if either column is
    // absent.
    std::unordered_map<std::string, std::string> extractMeshPathMap() const;
};

} // namespace tos::ies
