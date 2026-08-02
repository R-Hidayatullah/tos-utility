"""Reader for the .bin written by cpp/relay/tos_relay.exe (TOSRLY v2).

    python read_relay.py relay/dumps/capture_*.bin              # summary
    python read_relay.py capture_*.bin ZC_CONNECT_OK            # one packet type
    python read_relay.py capture_*.bin 3002                     # by opcode
    python read_relay.py capture_*.bin --conns                  # per connection

Layout (see cpp/relay/dump.h):

    FileHeader { magic[8] "TOSRLY\\0\\0", u32 version, u32 file_header_size,
                 u32 record_header_size, u32 flags, u64 start_us,
                 u64 start_mono_ns, char region[16], char note[32], u32 pid,
                 u32 reserved }
    repeated   { RecordHeader, char name[name_len], u8 body[body_len] }
    FileTrailer                    -- present only after a clean shutdown

Every record carries a magic and its own length, so a dump cut short by a
crash still reads: on a bad magic the reader resyncs to the next one. A
missing trailer is how you know the capture was cut short rather than ended.

Bodies are the FULL plaintext packet including the 10-byte header, so opcode,
sequence and checksum can be re-derived independently of the fields recorded
beside them. checksum_ok is the built-in correctness check: c2s records
decoding with chk=BAD mean the Blowfish key or init table is wrong.

The older TOSCAP files from cpp/tos_capture.exe are read by read_capture.py.
"""

import collections
import socket
import struct
import sys
import time

FILE_HDR = "<8sIIIIQQ16s32sII"
FILE_HDR_SZ = struct.calcsize(FILE_HDR)
REC_HDR = "<IIQQQIIIHHHHHHIIIIBBBBBB2s"
REC_HDR_SZ = struct.calcsize(REC_HDR)
TRAILER = "<IIQQQ"
TRAILER_SZ = struct.calcsize(TRAILER)
REC_MAGIC = 0x504B5452          # 'PKTR'
END_MAGIC = 0x444E4554          # 'TEND'

LINKS = {0: "?", 1: "barrack", 2: "zone", 3: "social"}

F_UNKNOWN_OP = 1        # opcode is not in packet_opcodes.csv
F_UNFRAMED = 2          # bytes that could not be framed, stored verbatim
F_INFERRED_LEN = 4      # length came from where the next valid packet started

Rec = collections.namedtuple(
    "Rec", "time_us mono_ns index conn src_ip dst_ip src_port dst_port "
           "listen_port direction opcode declared seq checksum wire link "
           "chk_ok variable encrypted flags name body")


def flag_text(f):
    out = []
    if f & F_UNFRAMED:
        out.append("UNFRAMED")
    if f & F_UNKNOWN_OP:
        out.append("unknown-op")
    if f & F_INFERRED_LEN:
        out.append("len-inferred")
    return ",".join(out)


def ip(v):
    return socket.inet_ntoa(struct.pack("<I", v))


def read(path):
    with open(path, "rb") as f:
        blob = f.read()
    (magic, ver, fhsz, rhsz, flags, start_us, start_mono, region, note, pid,
     _r) = struct.unpack_from(FILE_HDR, blob, 0)
    if not magic.startswith(b"TOSRLY"):
        raise SystemExit("not a TOSRLY file: %r (try read_capture.py)" % magic[:8])

    meta = {"version": ver, "region": region.rstrip(b"\0").decode(),
            "note": note.rstrip(b"\0").decode(), "start_us": start_us,
            "pid": pid, "clean": False, "trailer": None}

    end = len(blob)
    if end >= TRAILER_SZ:
        tm, clean, end_us, nrec, nbytes = struct.unpack_from(
            TRAILER, blob, end - TRAILER_SZ)
        if tm == END_MAGIC:
            meta["clean"] = bool(clean)
            meta["trailer"] = {"end_us": end_us, "records": nrec,
                               "bytes": nbytes}
            end -= TRAILER_SZ

    off, recs, resyncs = fhsz, [], 0
    while off + rhsz <= end:
        (m, rlen, t, mono, idx, conn, sip, dip, sport, dport, lport, d, op,
         decl, seq, chk, wire, blen, link, ok, var, enc, nlen, flags,
         _pad) = struct.unpack_from(REC_HDR, blob, off)
        if m != REC_MAGIC or rlen < rhsz or off + rlen > end:
            off += 1                     # torn tail: scan for the next record
            resyncs += 1
            continue
        p = off + rhsz
        name = blob[p:p + nlen].decode("ascii", "replace")
        body = blob[p + nlen:p + nlen + blen]
        off += rlen
        recs.append(Rec(t, mono, idx, conn, sip, dip, sport, dport, lport, d,
                        op, decl, seq, chk, wire, link, ok, var, enc, flags,
                        name, body))
    meta["resyncs"] = resyncs
    return meta, recs


def hexdump(data, limit=256):
    out = []
    for o in range(0, min(len(data), limit), 16):
        c = data[o:o + 16]
        out.append("  %04X  %-47s |%s|"
                   % (o, " ".join("%02X" % b for b in c),
                      "".join(chr(b) if 32 <= b < 127 else "." for b in c)))
    return "\n".join(out)


def banner(meta, recs, path):
    print("%s" % path)
    print("  region=%s note=%s pid=%d  started %s"
          % (meta["region"], meta["note"], meta["pid"],
             time.strftime("%Y-%m-%d %H:%M:%S",
                           time.localtime(meta["start_us"] / 1e6))))
    print("  %d records" % len(recs), end="")
    if meta["clean"]:
        t = meta["trailer"]
        print("  clean shutdown (trailer says %d records, %d bytes)"
              % (t["records"], t["bytes"]))
    else:
        print("  ** NO TRAILER: this capture was cut short (crash or kill) **")
    if meta["resyncs"]:
        print("  %d byte(s) skipped resyncing past damage" % meta["resyncs"])


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None
    meta, recs = read(path)
    banner(meta, recs, path)
    if not recs:
        return

    t0 = recs[0].time_us

    if want == "--conns":
        per = collections.OrderedDict()
        for r in recs:
            k = (r.conn, r.listen_port)
            e = per.setdefault(k, {"n": 0, "peer": "", "up": "", "first": r.time_us,
                                   "last": 0, "bytes": 0})
            e["n"] += 1
            e["bytes"] += len(r.body)
            e["last"] = r.time_us
            if r.direction == 0:
                e["peer"] = "%s:%d" % (ip(r.src_ip), r.src_port)
                e["up"] = "%s:%d" % (ip(r.dst_ip), r.dst_port)
            elif not e["peer"]:
                e["peer"] = "%s:%d" % (ip(r.dst_ip), r.dst_port)
                e["up"] = "%s:%d" % (ip(r.src_ip), r.src_port)
        print("\n%-5s %-7s %-22s %-22s %8s %10s %8s"
              % ("conn", "listen", "client", "upstream", "packets", "bytes",
                 "seconds"))
        for (conn, lport), e in per.items():
            print("%-5d %-7d %-22s %-22s %8d %10d %8.1f"
                  % (conn, lport, e["peer"], e["up"], e["n"], e["bytes"],
                     (e["last"] - e["first"]) / 1e6))
        return

    if want:
        sel = [r for r in recs if str(r.opcode) == want or r.name == want]
        print("\n%d matching %s\n" % (len(sel), want))
        for r in sel[:10]:
            print("+%.3fs conn=%d %s %s:%d -> %s:%d  op=%d %s"
                  % ((r.time_us - t0) / 1e6, r.conn,
                     "c2s" if r.direction == 0 else "s2c",
                     ip(r.src_ip), r.src_port, ip(r.dst_ip), r.dst_port,
                     r.opcode, r.name))
            print("        seq=%d declared=%d len=%d wire=%d link=%s chk=%s%s%s"
                  % (r.seq, r.declared, len(r.body), r.wire, LINKS[r.link],
                     {1: "ok", 0: "BAD", 2: "?"}[r.chk_ok],
                     " VAR" if r.variable else "",
                     "  " + flag_text(r.flags) if r.flags else ""))
            print(hexdump(r.body))
            print()
        return

    stats = collections.defaultdict(
        lambda: {"n": 0, "bad": 0, "min": 1 << 30, "max": 0, "var": False,
                 "name": "", "bytes": 0})
    for r in recs:
        s = stats[(r.direction, r.opcode)]
        s["n"] += 1
        s["bad"] += (r.chk_ok == 0)
        s["min"] = min(s["min"], len(r.body))
        s["max"] = max(s["max"], len(r.body))
        s["bytes"] += len(r.body)
        s["var"] = s["var"] or bool(r.variable)
        s["name"] = r.name

    print("\n%-4s %-38s %6s %7s %6s %6s %10s %4s %s"
          % ("dir", "name", "op", "n", "min", "max", "bytes", "var", "bad"))
    for (d, op), s in sorted(stats.items(), key=lambda kv: -kv[1]["n"]):
        print("%-4s %-38s %6d %7d %6d %6d %10d %4s %s"
              % ("c2s" if d == 0 else "s2c", s["name"], op, s["n"], s["min"],
                 s["max"], s["bytes"], "Y" if s["var"] else "", s["bad"] or ""))

    bad = sum(1 for r in recs if r.chk_ok == 0)
    span = (recs[-1].time_us - t0) / 1e6
    print("\n%d records over %.1fs, %d distinct opcodes, %d connection(s)"
          % (len(recs), span, len(stats), len({r.conn for r in recs})))
    print("checksum failures: %d" % bad)
    if bad:
        print("  (c2s failures would mean the Blowfish key/table is wrong)")

    # Opcodes the table does not have. These are the rows packet_opcodes.csv is
    # missing, which is usually the whole point of capturing against a client
    # newer than the table.
    unknown = collections.defaultdict(
        lambda: {"n": 0, "min": 1 << 30, "max": 0, "dir": "", "inferred": False})
    for r in recs:
        if not (r.flags & F_UNKNOWN_OP):
            continue
        u = unknown[r.opcode]
        u["n"] += 1
        u["min"] = min(u["min"], len(r.body))
        u["max"] = max(u["max"], len(r.body))
        u["dir"] = "c2s" if r.direction == 0 else "s2c"
        u["inferred"] = u["inferred"] or bool(r.flags & F_INFERRED_LEN)
    if unknown:
        unframed = sum(len(r.body) for r in recs if r.flags & F_UNFRAMED)
        print("\n%d opcode(s) not in packet_opcodes.csv, %d bytes unframed"
              % (len(unknown), unframed))
        print("%-4s %8s %6s %6s %6s  %s"
              % ("dir", "opcode", "n", "min", "max", "length"))
        for op, u in sorted(unknown.items()):
            print("%-4s %8d %6d %6d %6d  %s"
                  % (u["dir"], op, u["n"], u["min"], u["max"],
                     "fixed" if u["min"] == u["max"] else "varies",))
        print("\n  sizes above are observed, not declared -- treat them as "
              "candidates for the table, not facts")


if __name__ == "__main__":
    main()
