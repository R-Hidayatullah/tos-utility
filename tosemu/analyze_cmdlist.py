"""Decode a real BC_COMMANDER_LIST against the layout from CBarrackNet::CommanderList.

The handler walks a cursor starting at byte 98:

    prop_blob : prop_blob_len bytes                (account properties)
    char_count x {
        608-byte character record
        u64
        u16 n ; n x { cstring, cstring }            (job/name pairs)
    }
    u16 m ; m x { u32, u16 }
    u16 ; u32 ; u16

If the walk lands exactly on the declared length, the layout is right.

    python analyze_cmdlist.py capture_*.bin
"""

import struct
import sys

from read_capture import read, hexdump

BC_COMMANDER_LIST = 23


def cstr(buf, off):
    end = buf.index(b"\x00", off)
    return buf[off:end], end + 1


def walk(p):
    out = []
    total = struct.unpack_from("<H", p, 10)[0]
    acct = struct.unpack_from("<Q", p, 12)[0]
    unk20 = p[20]
    nchar = p[21]
    prop_len = struct.unpack_from("<H", p, 86)[0]
    unk88 = struct.unpack_from("<H", p, 88)[0]
    unk90 = struct.unpack_from("<H", p, 90)[0]
    unk92 = struct.unpack_from("<I", p, 92)[0]
    unk96, unk97 = p[96], p[97]
    out.append("declared_len = %d   (record is %d bytes)" % (total, len(p)))
    out.append("account_id   = 0x%016X" % acct)
    out.append("char_count   = %d      unk20=%d unk96=%d unk97=%d"
               % (nchar, unk20, unk96, unk97))
    out.append("prop_blob    = %d bytes   unk88=%d unk90=%d unk92=%d"
               % (prop_len, unk88, unk90, unk92))

    o = 98
    prop = p[o:o + prop_len]
    o += prop_len
    out.append("\n-- property blob @98 (%d bytes) --" % prop_len)
    out.append(hexdump(prop, 160))

    for i in range(nchar):
        rec_start = o
        rec = p[o:o + 608]
        o += 608
        handle = struct.unpack_from("<Q", p, o)[0]; o += 8
        n = struct.unpack_from("<H", p, o)[0]; o += 2
        pairs = []
        for _ in range(n):
            a, o = cstr(p, o)
            b, o = cstr(p, o)
            pairs.append((a.decode("latin1"), b.decode("latin1")))
        # printable runs inside the fixed record -- name, family name, etc.
        strings = []
        j = 0
        while j < len(rec):
            if 32 <= rec[j] < 127:
                k = j
                while k < len(rec) and 32 <= rec[k] < 127:
                    k += 1
                if k - j >= 3:
                    strings.append((j, rec[j:k].decode("latin1")))
                j = k
            j += 1
        out.append("\n== character %d @%d ==" % (i, rec_start))
        out.append("  handle=0x%016X  kv_pairs=%d" % (handle, n))
        out.append("  strings in record: %s" % strings[:8])
        out.append("  kv: %s" % pairs[:12])
        out.append(hexdump(rec, 128))

    m = struct.unpack_from("<H", p, o)[0]; o += 2
    tail = []
    for _ in range(m):
        a = struct.unpack_from("<I", p, o)[0]; o += 4
        b = struct.unpack_from("<H", p, o)[0]; o += 2
        tail.append((a, b))
    out.append("\n-- tail --")
    out.append("  m=%d %s" % (m, tail[:12]))
    t1 = struct.unpack_from("<H", p, o)[0]; o += 2
    t2 = struct.unpack_from("<I", p, o)[0]; o += 4
    t3 = struct.unpack_from("<H", p, o)[0]; o += 2
    out.append("  trailing u16=%d u32=%d u16=%d" % (t1, t2, t3))
    out.append("\ncursor ended at %d, declared %d  -> %s"
               % (o, total, "EXACT MATCH" if o == total else "MISMATCH (%+d)" % (o - total)))
    return "\n".join(out)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    _, recs = read(sys.argv[1])
    sel = [r for r in recs if r.opcode == BC_COMMANDER_LIST and r.direction == 1]
    print("%d BC_COMMANDER_LIST records\n" % len(sel))
    seen = set()
    for r in sel:
        if r.body in seen:
            continue
        seen.add(r.body)
        print("=" * 72)
        try:
            print(walk(r.body))
        except Exception as exc:
            print("walk failed: %r" % exc)
        print()


if __name__ == "__main__":
    main()
