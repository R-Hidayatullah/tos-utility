"""Tree of Savior wire protocol core -- crypto, framing, packet table.

Stable layer. Reversed from Client_tos_x64.exe:

    client -> server : u16 padded_len | Blowfish-ECB(payload padded to 8)
    server -> client : payload                (plaintext, no prefix, no padding)

Both directions share the header:

    u16 opcode | u32 sequence | u32 checksum | body

Packet length comes from the size table (gePacketTable), or from a u16 at
+0x0A when the table entry is 0 (variable). Either way it is the TOTAL size,
header included -- CClientNet::MoveToRecvQueue compares it against the number
of bytes buffered.

Edit handlers.py for packet logic; this file should rarely need to change.
"""

import csv
import os
import struct

from bf_decrypt import KEY, Blowfish, checksum   # noqa: F401  (re-exported)

HERE = os.path.dirname(os.path.abspath(__file__))
M32 = 0xFFFFFFFF


def _load_table():
    sizes, names = {}, {}
    with open(os.path.join(HERE, "packet_opcodes.csv"), newline="",
              encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            op = int(row["opcode_dec"])
            sizes[op] = int(row["size"])       # 0 == variable
            names[op] = row["name"]
    return sizes, names


SIZES, NAMES = _load_table()
BF = Blowfish(open(os.path.join(HERE, "bf_inittable.bin"), "rb").read(), KEY)


def name_of(op):
    return NAMES.get(op, "UNKNOWN_%d" % op)


def bf_encrypt(data):
    out = bytearray()
    for off in range(0, len(data), 8):
        L = int.from_bytes(data[off:off + 4], "big")
        R = int.from_bytes(data[off + 4:off + 8], "big")
        L, R = BF.encrypt_block(L, R)
        out += L.to_bytes(4, "big") + R.to_bytes(4, "big")
    return bytes(out)


def seal(payload):
    """Server -> client: raw packet bytes. No prefix, no padding, no crypto."""
    return payload


def unseal(frame_body):
    """Client -> server: decrypt one Blowfish frame body."""
    return BF.decrypt(frame_body)


def packet_size(plain):
    """Declared TOTAL size of a decrypted packet, or None if unknown."""
    if len(plain) < 10:
        return None
    op = struct.unpack_from("<H", plain, 0)[0]
    size = SIZES.get(op)
    if size is None:
        return None
    if size > 0:
        return size
    if len(plain) < 12:
        return None
    return struct.unpack_from("<H", plain, 10)[0]


def stamp(payload, seq):
    """Write sequence and checksum into a built packet."""
    buf = bytearray(payload)
    struct.pack_into("<I", buf, 2, seq)
    struct.pack_into("<I", buf, 6, 0)
    struct.pack_into("<I", buf, 6, checksum(buf))
    return bytes(buf)


def verify(plain, size):
    """Recompute the checksum over `size` bytes with the field zeroed."""
    if not size or size > len(plain):
        return None
    stored = struct.unpack_from("<I", plain, 6)[0]
    v = bytearray(plain[:size])
    struct.pack_into("<I", v, 6, 0)
    return checksum(v) == stored


def hexdump(data, limit=96):
    out = []
    for off in range(0, min(len(data), limit), 16):
        c = data[off:off + 16]
        out.append("      %04X  %-47s |%s|"
                   % (off, " ".join("%02X" % b for b in c),
                      "".join(chr(b) if 32 <= b < 127 else "." for b in c)))
    return "\n".join(out)


def first_string(plain, start=10, end=120):
    """First printable NUL-terminated run in [start,end); returns (bytes, off)."""
    i = start
    while i < min(len(plain), end):
        if 32 <= plain[i] < 127:
            j = i
            while j < len(plain) and 32 <= plain[j] < 127:
                j += 1
            if j - i >= 2:
                return plain[i:j], i
            i = j
        i += 1
    return b"", -1
