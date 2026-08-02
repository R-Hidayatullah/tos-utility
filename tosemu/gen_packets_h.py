"""Turn packet_fields.json into packets.h, verified against captured bytes.

    python gen_packets_h.py capture_*.bin [more captures...]

ida_dump_structs.py gives offsets and widths from the client's own handler,
but Hex-Rays only tells us how wide a read was, not whether a 4-byte read is a
float or an int. The captures settle that: a field whose bytes parse as
plausible floats across every sample is a float.

Each field carries what backs it -- how many samples were checked, and whether
the value ever changed. A field that is always zero is flagged rather than
typed, because nothing in the capture constrains it.

Writes packets.h and prints a verification summary.
"""

import collections
import json
import os
import struct
import sys

from read_capture import read

HERE = os.path.dirname(os.path.abspath(__file__))
JSON_IN = os.path.join(HERE, "packet_fields.json")
H_OUT = os.path.join(HERE, "packets.h")

HDR = 10

# Hex-Rays width spelling -> unsigned C type of the same size.
UINT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}


def plausible_float(vals):
    """True if every sample at this offset reads as a sane game-world float.

    Denormals are the discriminator. A small integer reinterpreted as a float
    lands there -- handle 0x000339CC reads as 3e-40, the value 6 as 8e-45 --
    while genuine game floats never do. A plain magnitude floor cannot separate
    them: a normalised direction vector reads ~5e-8 on the component facing an
    axis (23 of 345 ZC_MOVE_DIR samples), which is a perfectly normal float.
    """
    ok = 0
    for v in vals:
        f = struct.unpack("<f", v)[0]
        if f != f or f in (float("inf"), float("-inf")):
            return False
        if f != 0.0 and abs(f) < 1.175494e-38:      # denormal: really an int
            return False
        if abs(f) < 1e9:
            ok += 1
    return ok == len(vals) and len(vals) > 0


def collect(paths):
    """opcode -> list of sample bodies, across every capture given."""
    out = collections.defaultdict(list)
    for p in paths:
        try:
            _, recs = read(p)
        except SystemExit:
            continue
        for r in recs:
            out[r.opcode].append(r.body)
    return out


def looks_textual(vals):
    """A NUL-padded ASCII field: printable run, then only zeros."""
    hits = 0
    for v in vals:
        i = 0
        while i < len(v) and 32 <= v[i] < 127:
            i += 1
        if i >= 2 and all(b == 0 for b in v[i:]):
            hits += 1
    return hits >= max(1, len(vals) * 0.6)


def type_field(off, width, samples):
    """Choose a C type for one field and say what backs the choice."""
    vals = [s[off:off + width] for s in samples
            if len(s) >= off + width]
    n = len(vals)
    if not n:
        return UINT.get(width, "uint8_t"), width if width not in UINT else 0, \
            "no samples"

    varies = len({bytes(v) for v in vals}) > 1
    allzero = not any(any(v) for v in vals)

    if width >= 4 and not allzero and looks_textual(vals):
        return "char", width, "%d samples, text" % n

    # floats come in runs: a _QWORD is often two, an _OWORD often four
    if width in (4, 8, 16) and not allzero:
        parts = width // 4
        cols = [[v[i * 4:(i + 1) * 4] for v in vals] for i in range(parts)]
        if all(plausible_float(c) for c in cols):
            note = "%d samples%s" % (n, "" if varies else ", constant")
            return "float", parts if parts > 1 else 0, note

    note = "%d samples%s" % (n, ", always 0" if allzero else
                             ("" if varies else ", constant"))
    if width in UINT:
        return UINT[width], 0, note
    return "uint8_t", width, note


def schema_entry(op, e, samples):
    """Resolved field list for the runtime packet log.

    packet_fields.json carries the Hex-Rays read width, which is not enough to
    log usefully: a _QWORD there may be two floats. The float/text refinement
    only exists once the captures are consulted, so bake the result into a
    second file the C++ server can read directly.
    """
    out = []
    for off, _hexrays, width in e["fields"]:
        ctype, arr, _note = type_field(off, width, samples)
        if ctype == "float":
            t = "f32[%d]" % arr if arr else "f32"
        elif ctype == "char":
            t = "str"
        elif ctype in ("uint8_t", "uint16_t", "uint32_t", "uint64_t"):
            t = {"uint8_t": "u8", "uint16_t": "u16",
                 "uint32_t": "u32", "uint64_t": "u64"}[ctype]
            if arr:
                t = "bytes"
        else:
            t = "bytes"
        out.append([off, t, width])
    return {"name": e["name"], "size": e["size"], "fields": out}


def emit(op, e, samples):
    name, size, fields = e["name"], e["size"], e["fields"]
    lines = []
    src = e["sources"][0] if e["sources"] else None

    lines.append("/* ---- %s  op=%d  %s ---------------------------------"
                 % (name, op, "size=%d" % size if size else "variable"))
    if src:
        if src.get("method"):
            lines.append(" * %s" % src["method"])
        if src.get("file"):
            lines.append(" * %s   [%s]" % (src["file"], src["func"]))
    lines.append(" * coverage: %s%s" % (
        e["coverage"],
        "  gaps: " + ", ".join("+0x%X(%d)" % (o, n) for o, n in e["gaps"])
        if e["gaps"] else ""))
    lines.append(" */")
    lines.append("typedef struct PKS_%s {" % name)
    lines.append("    uint16_t command;    /* +0x00  == %d */" % op)
    lines.append("    uint32_t sequence;   /* +0x02 */")
    lines.append("    uint32_t checksum;   /* +0x06 */")

    if not fields:
        if size > HDR:
            lines.append("    uint8_t  body[%d];   /* +0x0A  no handler found */"
                         % (size - HDR))
        elif not size:
            lines.append("    uint8_t  body[];     /* variable, no handler found */")
        lines.append("} PKS_%s;" % name)
        return "\n".join(lines)

    cur = HDR
    for off, _hexrays_t, width in fields:
        if off > cur:
            lines.append("    uint8_t  _pad_%02X[%d];%s/* +0x%02X  unread */"
                         % (cur, off - cur, " " * max(1, 6 - len(str(off - cur))),
                            cur))
        ctype, arr, note = type_field(off, width, samples)
        decl = "f_%02X" % off
        if arr:
            decl += "[%d]" % arr
        lines.append("    %-8s %-14s /* +0x%02X  %s */"
                     % (ctype, decl + ";", off, note))
        cur = off + width
    if size and cur < size:
        lines.append("    uint8_t  _tail[%d];   /* +0x%02X  unread */"
                     % (size - cur, cur))
    elif not size:
        lines.append("    uint8_t  _var[];     /* variable remainder */")
    lines.append("} PKS_%s;" % name)
    return "\n".join(lines)


def main():
    if not os.path.exists(JSON_IN):
        raise SystemExit("run ida_dump_structs.py inside IDA first")
    data = json.load(open(JSON_IN, encoding="utf-8"))
    samples = collect(sys.argv[1:])

    order = sorted(data.items(), key=lambda kv: int(kv[0]))
    body, stats, schema = [], collections.Counter(), {}
    verified = 0
    for k, e in order:
        op = int(k)
        s = samples.get(op, [])
        stats[e["coverage"]] += 1
        if e["fields"] and s:
            verified += 1
        body.append(emit(op, e, s))
        if e["fields"]:
            schema[k] = schema_entry(op, e, s)

    with open(os.path.join(HERE, "packet_schema.json"), "w",
              encoding="utf-8") as f:
        json.dump(schema, f, indent=1)

    with open(H_OUT, "w", encoding="utf-8") as f:
        f.write("/* packets.h -- Tree of Savior wire structs.\n"
                " *\n"
                " * GENERATED by gen_packets_h.py from packet_fields.json\n"
                " * (ida_dump_structs.py, run inside IDA against\n"
                " * Client_tos_x64.exe). Do not edit by hand.\n"
                " *\n"
                " * Offsets and widths come from the client's own handlers.\n"
                " * Types are refined against captured packets where samples\n"
                " * exist; each field records how many backed its type.\n"
                " *\n"
                " * 'unread' padding is real packet space the client's handler\n"
                " * never touches -- not necessarily unused, just unproven.\n"
                " */\n"
                "#ifndef TOS_PACKETS_H\n#define TOS_PACKETS_H\n\n"
                "#include <stdint.h>\n\n#pragma pack(push, 1)\n\n")
        f.write("\n\n".join(body))
        f.write("\n\n#pragma pack(pop)\n#endif /* TOS_PACKETS_H */\n")

    print("wrote %s  (%d opcodes)" % (H_OUT, len(order)))
    print("  exact (tiles to declared size) : %d" % stats["exact"])
    print("  partial (gaps the client skips): %d" % stats["partial"])
    print("  variable-size                  : %d" % stats["variable"])
    print("  no handler found               : %d" % stats["none"])
    print("  structs cross-checked vs bytes : %d" % verified)


if __name__ == "__main__":
    main()
