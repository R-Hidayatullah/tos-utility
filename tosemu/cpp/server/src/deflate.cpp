#include "deflate.h"

#include <algorithm>
#include <array>

namespace tos {
namespace {

// ---- bit reader --------------------------------------------------------

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;      // byte index
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool bad = false;

    int bits(int want) {
        while (bitcnt < want) {
            if (pos >= n) { bad = true; return 0; }
            bitbuf |= uint32_t(p[pos++]) << bitcnt;
            bitcnt += 8;
        }
        int v = int(bitbuf & ((1u << want) - 1));
        bitbuf >>= want;
        bitcnt -= want;
        return v;
    }

    void align() { bitbuf = 0; bitcnt = 0; }
};

// Canonical Huffman decoder, the "counts + symbols" form from puff.c: for each
// code length keep how many codes have it and the symbols in canonical order,
// then walk lengths accumulating one bit at a time.
struct Huffman {
    std::array<uint16_t, 16> count{};
    std::vector<uint16_t> symbol;

    void build(const uint8_t* lengths, size_t n) {
        count.fill(0);
        for (size_t i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;

        std::array<uint16_t, 16> offs{};
        for (int i = 1; i < 16; ++i) offs[i] = uint16_t(offs[i - 1] + count[i - 1]);

        symbol.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = uint16_t(i);
    }

    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.bits(1);
            if (br.bad) return -1;
            int cnt = count[len];
            if (code - first < cnt) return symbol[size_t(index + (code - first))];
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                               15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                               67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,    13,
                                17,   25,   33,   49,   65,   97,    129,  193,
                                257,  385,  513,  769,  1025, 1537,  2049, 3073,
                                4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_block(BitReader& br, const Huffman& lit, const Huffman& dist,
                   Bytes& out) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(uint8_t(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = size_t(kLenBase[sym]) + size_t(br.bits(kLenExtra[sym]));

            int dsym = dist.decode(br);
            if (dsym < 0 || dsym >= 30) return false;
            size_t d = size_t(kDistBase[dsym]) + size_t(br.bits(kDistExtra[dsym]));
            if (br.bad || d > out.size()) return false;

            size_t from = out.size() - d;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
        if (br.bad) return false;
    }
}

void fixed_tables(Huffman& lit, Huffman& dist) {
    uint8_t ll[288];
    for (int i = 0; i < 144; ++i) ll[i] = 8;
    for (int i = 144; i < 256; ++i) ll[i] = 9;
    for (int i = 256; i < 280; ++i) ll[i] = 7;
    for (int i = 280; i < 288; ++i) ll[i] = 8;
    lit.build(ll, 288);

    uint8_t dl[30];
    for (int i = 0; i < 30; ++i) dl[i] = 5;
    dist.build(dl, 30);
}

bool dynamic_tables(BitReader& br, Huffman& lit, Huffman& dist) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};
    int hlit = br.bits(5) + 257;
    int hdist = br.bits(5) + 1;
    int hclen = br.bits(4) + 4;
    if (br.bad || hlit > 286 || hdist > 30) return false;

    uint8_t clen[19] = {0};
    for (int i = 0; i < hclen; ++i) clen[order[i]] = uint8_t(br.bits(3));
    Huffman cl;
    cl.build(clen, 19);

    uint8_t lengths[288 + 30] = {0};
    int i = 0;
    while (i < hlit + hdist) {
        int sym = cl.decode(br);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = uint8_t(sym);
        } else {
            int rep, val = 0;
            if (sym == 16) {
                if (i == 0) return false;
                val = lengths[i - 1];
                rep = 3 + br.bits(2);
            } else if (sym == 17) {
                rep = 3 + br.bits(3);
            } else {
                rep = 11 + br.bits(7);
            }
            if (i + rep > hlit + hdist) return false;
            while (rep--) lengths[i++] = uint8_t(val);
        }
        if (br.bad) return false;
    }

    lit.build(lengths, size_t(hlit));
    dist.build(lengths + hlit, size_t(hdist));
    return true;
}

}  // namespace

bool inflate_raw(const uint8_t* in, size_t in_len, size_t expected, Bytes& out) {
    out.clear();
    if (expected) out.reserve(expected);

    BitReader br{in, in_len};
    for (;;) {
        int final_block = br.bits(1);
        int type = br.bits(2);
        if (br.bad) return false;

        if (type == 0) {
            br.align();
            if (br.pos + 4 > br.n) return false;
            uint16_t len = rd16(in + br.pos);
            uint16_t nlen = rd16(in + br.pos + 2);
            br.pos += 4;
            if (uint16_t(~len) != nlen) return false;
            if (br.pos + len > br.n) return false;
            out.insert(out.end(), in + br.pos, in + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            Huffman lit, dist;
            fixed_tables(lit, dist);
            if (!inflate_block(br, lit, dist, out)) return false;
        } else if (type == 2) {
            Huffman lit, dist;
            if (!dynamic_tables(br, lit, dist)) return false;
            if (!inflate_block(br, lit, dist, out)) return false;
        } else {
            return false;
        }

        if (final_block) break;
    }
    return true;
}

Bytes deflate_stored(const uint8_t* in, size_t in_len) {
    Bytes out;
    out.reserve(in_len + (in_len / 0xFFFF + 1) * 5 + 5);

    size_t off = 0;
    do {
        size_t chunk = std::min<size_t>(in_len - off, 0xFFFF);
        bool last = (off + chunk >= in_len);

        out.push_back(uint8_t(last ? 1 : 0));   // BFINAL, BTYPE=00, byte-aligned
        uint8_t hdr[4];
        wr16(hdr, uint16_t(chunk));
        wr16(hdr + 2, uint16_t(~uint16_t(chunk)));
        out.insert(out.end(), hdr, hdr + 4);
        out.insert(out.end(), in + off, in + off + chunk);

        off += chunk;
    } while (off < in_len);

    return out;
}

}  // namespace tos
