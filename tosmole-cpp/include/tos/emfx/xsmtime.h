// .xsmtime — Tree-of-Savior companion file to a .xsm skeletal motion.
//
// NOT an EMotionFX format (no chunked FileChunk layout, no alignment padding).
// It is a flat, tightly-packed list of key/event TIMES for the matching motion:
//
//   int32  marker = 0xFFFFFFFF        // sentinel / version
//   uint32 count                      // number of time entries
//   count * { float mTime;            // time in seconds
//             uint16 mValue; }        // 6 bytes each, packed (value 0 = plain marker)
//
// The count matches the number of keyframes in the motion's animated tracks, so
// this is the motion's shared sample/event timeline (ToS de-duplicates identical
// .xsmtime files via release/xsmtime_duplicates.xml).
#pragma once

#include "tos/io/byte_reader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tos::emfx {

struct XsmTimeKey {
    float time = 0;      // seconds
    uint16_t value = 0;  // event/tag id (0 = plain time marker)
};

struct XsmTime {
    int32_t marker = 0;
    std::vector<XsmTimeKey> keys;
    bool sizeExact = false; // 8 + count*6 == file size

    float duration() const { return keys.empty() ? 0.f : keys.back().time; }
};

XsmTime parseXsmTime(const uint8_t* data, size_t size);
XsmTime parseXsmTimeFile(const std::string& path);

} // namespace tos::emfx
