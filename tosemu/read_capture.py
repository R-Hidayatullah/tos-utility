"""Reader for the structured .bin written by cpp/tos_capture.exe.

    python read_capture.py capture_*.bin              # summary by opcode
    python read_capture.py capture_*.bin ZC_CONNECT_OK # dump one packet type
    python read_capture.py capture_*.bin 3002          # by opcode number

Layout (see cpp/tos_capture.cpp):

    FileHeader { magic[8] "TOSCAP\\0\\0", u32 version, u32 header_size,
                 u64 epoch_us, char region[16], u64 reserved }
    repeated   { RecordHeader, u8 body[body_len] }

Every body is the FULL plaintext packet including the 10-byte header, so
opcode/sequence/checksum can be re-derived independently of the recorded
fields. checksum_ok is the built-in correctness check: if the Blowfish key or
init table were wrong, client->server records would decode with chk=BAD.
"""

import collections
import os
import struct
import sys

FILE_HDR = "<8sIIQ16sQ"
FILE_HDR_SZ = struct.calcsize(FILE_HDR)
REC_HDR = "<IQHHIHHIIBBHI"
REC_HDR_SZ = struct.calcsize(REC_HDR)
REC_MAGIC = 0x504B5452

Rec = collections.namedtuple(
    "Rec", "time_us direction port conn opcode declared seq checksum "
           "chk_ok variable body")


def read(path):
    with open(path, "rb") as f:
        blob = f.read()
    magic, ver, hsz, epoch, region, _ = struct.unpack_from(FILE_HDR, blob, 0)
    if not magic.startswith(b"TOSCAP"):
        raise SystemExit("not a TOSCAP file: %r" % magic[:8])
    meta = {"version": ver, "region": region.rstrip(b"\x00").decode(),
            "epoch_us": epoch}
    off = FILE_HDR_SZ
    recs = []
    while off + REC_HDR_SZ <= len(blob):
        (m, t, d, port, conn, op, decl, seq, chk, ok, var, _r,
         blen) = struct.unpack_from(REC_HDR, blob, off)
        if m != REC_MAGIC:              # resync on a truncated/corrupt tail
            off += 1
            continue
        off += REC_HDR_SZ
        body = blob[off:off + blen]
        off += blen
        recs.append(Rec(t, d, port, conn, op, decl, seq, chk, ok, var, body))
    return meta, recs


def hexdump(data, limit=256):
    out = []
    for o in range(0, min(len(data), limit), 16):
        c = data[o:o + 16]
        out.append("  %04X  %-47s |%s|"
                   % (o, " ".join("%02X" % b for b in c),
                      "".join(chr(b) if 32 <= b < 127 else "." for b in c)))
    return "\n".join(out)


def names():
    import csv
    here = os.path.dirname(os.path.abspath(__file__))
    out = {}
    with open(os.path.join(here, "packet_opcodes.csv"), newline="",
              encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            out[int(row["opcode_dec"])] = row["name"]
    return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    meta, recs = read(sys.argv[1])
    NM = names()
    print("region=%s version=%d  %d records"
          % (meta["region"], meta["version"], len(recs)))

    if len(sys.argv) > 2:
        want = sys.argv[2]
        sel = [r for r in recs
               if str(r.opcode) == want or NM.get(r.opcode, "") == want]
        print("%d matching %s\n" % (len(sel), want))
        for r in sel[:10]:
            print("%s op=%d %s seq=%d declared=%d len=%d chk=%s%s"
                  % ("c2s" if r.direction == 0 else "s2c", r.opcode,
                     NM.get(r.opcode, "?"), r.seq, r.declared, len(r.body),
                     {1: "ok", 0: "BAD", 2: "?"}[r.chk_ok],
                     " VAR" if r.variable else ""))
            print(hexdump(r.body))
            print()
        return

    stats = collections.defaultdict(
        lambda: {"n": 0, "bad": 0, "min": 1 << 30, "max": 0, "var": False})
    for r in recs:
        k = (r.direction, r.opcode)
        s = stats[k]
        s["n"] += 1
        s["bad"] += (r.chk_ok == 0)
        s["min"] = min(s["min"], len(r.body))
        s["max"] = max(s["max"], len(r.body))
        s["var"] = s["var"] or bool(r.variable)

    print("\n%-4s %-34s %6s %5s %6s %6s %4s %s"
          % ("dir", "name", "op", "n", "min", "max", "var", "bad"))
    for (d, op), s in sorted(stats.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        print("%-4s %-34s %6d %5d %6d %6d %4s %s"
              % ("c2s" if d == 0 else "s2c", NM.get(op, "UNKNOWN"), op,
                 s["n"], s["min"], s["max"], "Y" if s["var"] else "",
                 s["bad"] or ""))

    bad = sum(1 for r in recs if r.chk_ok == 0)
    print("\nchecksum failures: %d / %d" % (bad, len(recs)))
    if bad:
        print("  (c2s failures would mean the Blowfish key/table is wrong)")


if __name__ == "__main__":
    main()
