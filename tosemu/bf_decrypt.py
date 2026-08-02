"""Decrypt captured Tree of Savior packets.

Transport (from CClientNet::Send / geCrypt, GeClientNet.cpp + Crypt.cpp):

    frame  = u16 padded_len (plaintext) || Blowfish-ECB(payload, padded to 8)
    payload= u16 opcode | u32 sequence | u32 checksum | body

The cipher is textbook Blowfish (16 rounds, big-endian blocks, standard F)
but seeded with a CUSTOM init table rather than the usual pi constants. That
table is assembled at runtime by geCrypt::Init from 4 scattered chunks of a
source table -- parts [16,2,256,768] taken from seeks [4,1056,24,284] --
which is why the standard-looking P-arrays elsewhere in .data are decoys.
bf_inittable.bin holds the assembled 1042 dwords, extracted from the IDB.

Key: 16 bytes, row `KEY_INDEX` of the 16x16 table at 0x141A70680
(geCrypt::MixKey selects a row when the mode byte is set, else a column).

    python bf_decrypt.py <capture.log>
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INIT_TABLE = os.path.join(HERE, "bf_inittable.bin")

KEY = b"hsunffalqyrqewes"   # 0x141A70680 row 11 (mode=1, index=11)
M32 = 0xFFFFFFFF


class Blowfish:
    def __init__(self, init_table, key):
        d = [int.from_bytes(init_table[i * 4:i * 4 + 4], "big")
             for i in range(1042)]
        self.P = d[:18]
        self.S = [d[18 + i * 256: 18 + (i + 1) * 256] for i in range(4)]

        j = 0
        for i in range(18):
            k = 0
            for _ in range(4):
                k = ((k << 8) | key[j % len(key)]) & M32
                j += 1
            self.P[i] ^= k

        L = R = 0
        for i in range(0, 18, 2):
            L, R = self.encrypt_block(L, R)
            self.P[i], self.P[i + 1] = L, R
        for box in range(4):
            for i in range(0, 256, 2):
                L, R = self.encrypt_block(L, R)
                self.S[box][i], self.S[box][i + 1] = L, R

    def _f(self, x):
        S = self.S
        a = (S[0][(x >> 24) & 0xFF] + S[1][(x >> 16) & 0xFF]) & M32
        return ((a ^ S[2][(x >> 8) & 0xFF]) + S[3][x & 0xFF]) & M32

    def encrypt_block(self, L, R):
        for i in range(16):
            L ^= self.P[i]
            R ^= self._f(L)
            L, R = R, L
        L, R = R, L
        return L ^ self.P[17], R ^ self.P[16]

    def decrypt_block(self, L, R):
        for i in range(17, 1, -1):
            L ^= self.P[i]
            R ^= self._f(L)
            L, R = R, L
        L, R = R, L
        return L ^ self.P[0], R ^ self.P[1]

    def decrypt(self, data):
        out = bytearray()
        for off in range(0, len(data) - 7, 8):
            L = int.from_bytes(data[off:off + 4], "big")
            R = int.from_bytes(data[off + 4:off + 8], "big")
            L, R = self.decrypt_block(L, R)
            out += L.to_bytes(4, "big") + R.to_bytes(4, "big")
        return bytes(out)


def checksum(buf):
    """Matches CClientNet::Send: xor on even index, add on odd."""
    s = 0
    for j, b in enumerate(buf):
        s = (b ^ s) if (j & 1) == 0 else (s + b) & M32
    return s & M32


def hexdump(data, limit=None):
    lines = []
    n = len(data) if limit is None else min(len(data), limit)
    for off in range(0, n, 16):
        chunk = data[off:off + 16]
        h = " ".join("%02X" % b for b in chunk)
        a = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append("  %04X  %-47s  |%s|" % (off, h, a))
    return "\n".join(lines)


def parse_frames(path):
    """Pull each hexdump block out of a capture_stub.py log."""
    frames, cur = [], []
    row = re.compile(r"^\s+([0-9A-Fa-f]{4})\s+((?:[0-9A-Fa-f]{2}\s+)+)\|")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = row.match(line)
        if m:
            if m.group(1) == "0000" and cur:
                frames.append(bytes(cur))
                cur = []
            cur += [int(x, 16) for x in m.group(2).split()]
        elif cur and line.strip() == "":
            frames.append(bytes(cur))
            cur = []
    if cur:
        frames.append(bytes(cur))
    return frames


def main():
    if not os.path.exists(INIT_TABLE):
        sys.exit("missing %s" % INIT_TABLE)
    bf = Blowfish(open(INIT_TABLE, "rb").read(), KEY)

    if len(sys.argv) < 2:
        sys.exit(__doc__)
    frames = parse_frames(sys.argv[1])
    print("parsed %d frame(s) from %s\n" % (len(frames), sys.argv[1]))

    seen = set()
    for i, f in enumerate(frames):
        if len(f) < 10:
            continue
        declared = int.from_bytes(f[0:2], "little")
        body = f[2:]
        if declared != len(body) or declared % 8:
            print("frame %d: len mismatch declared=%d actual=%d -- skipped"
                  % (i, declared, len(body)))
            continue
        if f in seen:
            continue
        seen.add(f)

        plain = bf.decrypt(body)
        opcode = int.from_bytes(plain[0:2], "little")
        seq = int.from_bytes(plain[2:6], "little")
        chk = int.from_bytes(plain[6:10], "little")

        print("frame %d: framed=%d payload=%d" % (i, len(f), declared))
        print("  opcode   = 0x%04X (%d)" % (opcode, opcode))
        print("  sequence = %d" % seq)
        print("  checksum = 0x%08X" % chk)
        print(hexdump(plain, 0x80))
        print()


if __name__ == "__main__":
    main()
