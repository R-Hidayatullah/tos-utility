// Shared per-chunk validation record used by all EMotionFX parsers.
//
// The FileChunk.mSizeInBytes field is the oracle: if structural parsing of a
// chunk consumes exactly that many bytes, our struct layout matches the file.
#pragma once

#include <cstdint>
#include <string>

namespace tos::emfx {

struct ChunkAudit {
    uint32_t id = 0;
    uint32_t version = 0;
    uint32_t declaredSize = 0;
    uint32_t consumedSize = 0;
    bool handled = false;   // did we have a reader for this (id,version)?
    bool exact = false;     // consumedSize == declaredSize
    bool overRead = false;  // consumedSize > declaredSize (exporter under-counted size)
    bool declOverEof = false; // declaredSize runs past EOF; structural parse reached EOF cleanly
    std::string label;
};

} // namespace tos::emfx
