"""Infer packet structure by diffing every captured sample of one opcode.

Offsets that never vary across samples are padding, constants or unused;
offsets that do vary are fields. Runs of varying bytes are grouped and typed
by inspection (plausible float / small int / ascii / handle), giving a C
struct skeleton to check against the client's handler in IDA.

    python struct_infer.py capture_*.bin CZ_KEYBOARD_MOVE
    python struct_infer.py capture_*.bin 3166
    python struct_infer.py capture_*.bin --list        # opcodes by sample count
"""

import collections
import struct
import sys

from read_capture import read, names

HDR = 10          # opcode u16 | sequence u32 | checksum u32


def classify(cols, off, width, samples):
    """Guess a type for a run of `width` varying bytes at `off`."""
    vals = [s[off:off + width] for s in samples if len(s) >= off + width]
    if not vals:
        return "uint8_t", width

    # ascii?
    printable = sum(1 for v in vals
                    if all(32 <= b < 127 or b == 0 for b in v) and v[0] != 0)
    if width >= 4 and printable >= max(1, len(vals) * 0.6):
        return "char", width

    if width >= 4:
        fl = []
        for v in vals[:20]:
            f = struct.unpack_from("<f", v, 0)[0]
            fl.append(f)
        ok = [f for f in fl
              if f == f and abs(f) > 1e-6 and abs(f) < 1e9]
        if len(ok) >= max(1, len(fl) * 0.6):
            return "float", 4

    if width >= 8:
        return "uint64_t", 8
    if width >= 4:
        return "uint32_t", 4
    if width >= 2:
        return "uint16_t", 2
    return "uint8_t", 1


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    _, recs = read(sys.argv[1])
    NM = names()
    want = sys.argv[2]

    if want == "--list":
        cnt = collections.Counter((r.direction, r.opcode) for r in recs)
        print("%-4s %-36s %-7s %s" % ("dir", "name", "op", "samples"))
        for (d, op), n in cnt.most_common(40):
            print("%-4s %-36s %-7d %d"
                  % ("c2s" if d == 0 else "s2c", NM.get(op, "?"), op, n))
        return

    sel = [r for r in recs
           if str(r.opcode) == want or NM.get(r.opcode, "") == want]
    if not sel:
        raise SystemExit("no samples for %r" % want)

    op = sel[0].opcode
    d = sel[0].direction
    samples = [r.body for r in sel]
    lens = collections.Counter(len(s) for s in samples)
    n = len(samples)
    width = min(lens)

    print("%s  op=%d  %s  %d samples" % (NM.get(op, "?"), op,
                                         "c2s" if d == 0 else "s2c", n))
    print("lengths: %s" % dict(lens))
    if len(lens) > 1:
        print("  (variable length -- analysing the common prefix of %d bytes)"
              % width)

    # which offsets vary?
    varies = []
    for off in range(width):
        col = {s[off] for s in samples}
        varies.append(len(col) > 1)

    print("\noffset map ('.' constant, 'X' varies), header shown for context:")
    for base in range(0, width, 32):
        row = "".join("X" if varies[o] else "."
                      for o in range(base, min(base + 32, width)))
        print("  %04X  %s" % (base, row))

    # group consecutive varying offsets into fields
    print("\n#pragma pack(push,1)")
    print("struct %s {" % NM.get(op, "op%d" % op))
    print("/* +0x00 */ uint16_t opcode;        // = %d" % op)
    print("/* +0x02 */ uint32_t sequence;")
    print("/* +0x06 */ uint32_t checksum;")
    off = HDR
    while off < width:
        if not varies[off]:
            j = off
            while j < width and not varies[j]:
                j += 1
            const = samples[0][off:j]
            note = "constant" if j - off > 1 else "constant"
            if any(const):
                note += " = " + const.hex(" ")
            print("/* +0x%02X */ uint8_t  _const_%02X[%d];   // %s"
                  % (off, off, j - off, note))
            off = j
            continue
        j = off
        while j < width and varies[j]:
            j += 1
        run = j - off
        while run > 0:
            t, w = classify(varies, off, run, samples)
            if t == "char":
                sample = samples[0][off:off + w].split(b"\x00")[0]
                print("/* +0x%02X */ char     f_%02X[%d];     // e.g. %r"
                      % (off, off, w, sample[:24]))
            else:
                vals = []
                for s in samples[:4]:
                    if len(s) >= off + w:
                        fmt = {1: "<B", 2: "<H", 4: "<I", 8: "<Q"}.get(w)
                        if t == "float":
                            vals.append(round(struct.unpack_from("<f", s, off)[0], 3))
                        elif fmt:
                            vals.append(struct.unpack_from(fmt, s, off)[0])
                print("/* +0x%02X */ %-8s f_%02X;%s   // %s"
                      % (off, t, off, " " * 7, vals))
            off += w
            run -= w
    if len(lens) > 1:
        print("/* +0x%02X */ uint8_t  tail[];       // variable remainder" % width)
    print("};")
    print("#pragma pack(pop)")


if __name__ == "__main__":
    main()
