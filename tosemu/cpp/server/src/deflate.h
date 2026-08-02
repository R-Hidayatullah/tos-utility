// Raw DEFLATE (RFC 1951), enough for both directions the server needs.
//
// Two consumers, and they need opposite halves:
//
//   * IPF archives store every entry as raw deflate, so reading the client's
//     own data files needs a real inflater.
//   * ZC_ITEM_INVENTORY_LIST / ZC_SKILL_LIST / ZC_QUICK_SLOT_LIST carry a
//     deflate stream the client inflates. Stored (BTYPE=00) blocks are valid
//     deflate, so the compressor emits those -- a real Huffman encoder buys
//     nothing here and is a lot of surface to get subtly wrong.
//
// Written out rather than pulled in so the server keeps its "winsock and the
// standard library only" build.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core.h"

namespace tos {

// Decompress a raw deflate stream. `expected` is a size hint (0 = unknown).
// Returns false on malformed input.
bool inflate_raw(const uint8_t* in, size_t in_len, size_t expected, Bytes& out);

// Compress as raw deflate using stored blocks. Never fails.
Bytes deflate_stored(const uint8_t* in, size_t in_len);

}  // namespace tos
