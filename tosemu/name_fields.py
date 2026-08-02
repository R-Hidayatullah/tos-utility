"""Give every packet field a name, and write the template tos_view parses.

    python name_fields.py relay/dumps/capture_*.bin [more...]

Offsets and widths already exist (`packet_fields.json`, from IDA). What was
missing is what to *call* each field, which is what turns a hex panel into a
010-Editor-style value list. Four sources, in descending order of trust:

  1. field_names.tsv  -- hand-verified, mostly by rebuilding captured replies
     byte-for-byte. Also the only source for client->server packets, which the
     binary cannot supply (docs/08).
  2. packet_usage.json -- how the client *uses* the field, from
     ida_field_usage.py. A helper called with the same offset from dozens of
     handlers is doing one job; naming that helper names the field everywhere.
     `sub_140AC71B0` is `GetActorByHandle` (its sibling `sub_140AD7290` is
     literally `return *(_DWORD *)GetMyPC() == *a1`), and it takes `pkt + 10`
     in 35 of its 46 packet call sites -- so +0x0A is the handle, which is
     also what docs/02 and docs/04 concluded from captures alone.
  3. the captures -- a float triple in world-coordinate range is a position, a
     float that only ever climbs is on the server clock (docs/05), a u32 that
     shows up at the head of a dozen different opcodes in one session is a
     handle.
  4. f_XX, the honest fallback.

Every name carries its origin so the pane can show it, because a guess and a
byte-exact rebuild should not look alike on screen.

Writes packet_template.tsv.
"""

import collections
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIELDS_IN = os.path.join(HERE, "packet_fields.json")
USAGE_IN = os.path.join(HERE, "packet_usage.json")
CURATED_IN = os.path.join(HERE, "field_names.tsv")
CSV_IN = os.path.join(HERE, "packet_opcodes.csv")
OUT = os.path.join(HERE, "packet_template.tsv")

HDR = 10

# Helper functions identified by hand in IDA, keyed by the name Hex-Rays gives
# them. Each maps an argument slot to what that argument is. This is the whole
# bridge between a stripped binary and a field name: three functions cover the
# single most common field in the protocol.
HELPERS = {
    # sub_140AEB170(GetActorManager(), h) -- the actor lookup, 1592 call sites
    ("sub_140AC71B0", 0): ("handle", "hex"),
    # return *(_DWORD *)GetMyPC() == *a1   -- IsMyHandle, unambiguous
    ("sub_140AD7290", 0): ("handle", "hex"),
    # if (!g) return 1; if (IsMyHandle(a1)) return 0; ...  -- handle predicate
    ("sub_140AD5920", 0): ("handle", "hex"),
}

SIZES = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4,
         "u64": 8, "i64": 8, "f32": 4, "f64": 8}
UINT = {1: "u8", 2: "u16", 4: "u32", 8: "u64"}


# --------------------------------------------------------------- capture side

def read_dump(path):
    """Records from either dump format, whichever this file is."""
    try:
        import read_relay
        return read_relay.read(path)[1]
    except SystemExit:
        import read_capture
        return read_capture.read(path)[1]


def collect(paths):
    out = collections.defaultdict(list)
    for p in paths:
        for r in read_dump(p):
            out[r.opcode].append(r.body)
    return out


def plausible_float(vals):
    """Same denormal test gen_packets_h.py uses -- see its docstring."""
    if not vals:
        return False
    for v in vals:
        f = struct.unpack("<f", v)[0]
        if f != f or f in (float("inf"), float("-inf")):
            return False
        if f != 0.0 and abs(f) < 1.175494e-38:
            return False
        if abs(f) >= 1e9:
            return False
    return True


def looks_textual(vals):
    hits = 0
    for v in vals:
        i = 0
        while i < len(v) and 32 <= v[i] < 127:
            i += 1
        if i >= 2 and all(b == 0 for b in v[i:]):
            hits += 1
    return hits >= max(1, len(vals) * 0.6)


def wire_type(off, width, samples):
    """The type to read the bytes as, decided by the samples where there are any."""
    vals = [s[off:off + width] for s in samples if len(s) >= off + width]
    if not vals:
        return UINT.get(width, "bytes[%d]" % width), []
    allzero = not any(any(v) for v in vals)
    if width >= 4 and not allzero and looks_textual(vals):
        return "str[%d]" % width, vals
    if width in (4, 8, 16) and not allzero:
        cols = [[v[i * 4:(i + 1) * 4] for v in vals] for i in range(width // 4)]
        if all(plausible_float(c) for c in cols):
            n = width // 4
            return ("f32" if n == 1 else "f32[%d]" % n), vals
    return UINT.get(width, "bytes[%d]" % width), vals


# ------------------------------------------------------------- handle census
#
# A handle is a session-scoped actor id, so the same 32-bit value turns up in
# many unrelated packets. Nothing else in the protocol behaves that way: a map
# id or an item count appears in one or two opcodes, not fifteen.

def handle_census(samples):
    seen = collections.defaultdict(set)
    for op, bodies in samples.items():
        for b in bodies[:200]:
            for off in range(HDR, min(len(b) - 3, 64)):
                v = struct.unpack_from("<I", b, off)[0]
                if 0x1000 <= v <= 0x00FFFFFF:
                    seen[v].add(op)
    return {v for v, ops in seen.items() if len(ops) >= 8}


def is_handleish(vals, handles):
    if not vals or len(vals[0]) != 4:
        return False
    hit = sum(1 for v in vals if struct.unpack("<I", v)[0] in handles)
    return hit >= max(1, len(vals) * 0.8)


# ---------------------------------------------------------------- heuristics

def climbing(vals):
    """A float that only ever goes up is a clock reading, not a measurement."""
    fs = [struct.unpack("<f", v)[0] for v in vals]
    if len(fs) < 8 or min(fs) < 1000.0:
        return False
    ups = sum(1 for a, b in zip(fs, fs[1:]) if b >= a)
    return ups >= len(fs) * 0.9 and max(fs) - min(fs) > 1.0


def guess(off, wtype, vals, uses, handles):
    """(name, fmt, origin) for one field. Origin is what backs the guess."""
    for u in uses:
        key = (u.get("callee"), u.get("arg"))
        if key in HELPERS:
            nm, fmt = HELPERS[key]
            return nm, fmt, "ida:%s" % u["callee"]

    if wtype == "u32" and is_handleish(vals, handles):
        return "handle", "hex", "census"

    if wtype.startswith("str["):
        return "text", "text", "bytes"

    if wtype == "f32" and vals and climbing(vals):
        return "server_time", "time", "bytes"

    if wtype.startswith("f32["):
        n = int(wtype[4:-1])
        if n == 3:
            return "pos", "", "bytes"
        if n == 2:
            # A normalised pair is a direction; an arbitrary pair is not.
            unit = 0
            for v in vals:
                a, b = struct.unpack("<ff", v[:8])
                if 0.98 <= a * a + b * b <= 1.02:
                    unit += 1
            if vals and unit >= len(vals) * 0.8:
                return "dir", "", "bytes"
        return "vec%d" % n, "", "bytes"

    if vals:
        allzero = not any(any(v) for v in vals)
        if allzero:
            return "zero", "", "bytes"
        distinct = {bytes(v) for v in vals}
        if wtype in ("u8", "u16") and distinct <= {b"\x00", b"\x01",
                                                  b"\x00\x00", b"\x01\x00"}:
            return "flag", "bool", "bytes"
        if len(distinct) == 1:
            return "const", "hex" if wtype in ("u32", "u64") else "", "bytes"

    # A comparison against a small constant is the client treating the field as
    # an enum even when the samples never exercised it.
    for u in uses:
        if "cmp" in u and 0 <= u["const"] <= 32:
            return "mode", "", "ida:cmp"

    return "f_%02X" % off, "", ""


# ------------------------------------------------------------------- curated

def load_curated():
    out = collections.defaultdict(dict)
    with open(CURATED_IN, encoding="utf-8") as fh:
        for ln in fh:
            ln = ln.rstrip("\n")
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            c = ln.split("\t")
            if len(c) < 4:
                continue
            c += [""] * (6 - len(c))
            # "docs/02 constant 35.0" -- the citation is the origin, the rest
            # is a note, and the pane has a column for each.
            origin, _, note = (c[5] or "curated").partition(" ")
            out[int(c[0])][int(c[1])] = {
                "type": c[2], "name": c[3], "fmt": c[4],
                "origin": origin, "note": note}
    return out


def load_opcodes():
    import csv
    sizes, names = {}, {}
    with open(CSV_IN, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            op = int(row["opcode_dec"])
            sizes[op] = int(row["size"])
            names[op] = row["name"]
    return sizes, names


def type_bytes(t):
    if t in SIZES:
        return SIZES[t]
    if t.startswith("f32["):
        return 4 * int(t[4:-1])
    if t.startswith(("str[", "bytes[")):
        return int(t[t.index("[") + 1:-1])
    return 0


# ------------------------------------------------------------------ assembly

def main():
    if not os.path.exists(FIELDS_IN):
        raise SystemExit("run ida_dump_structs.py inside IDA first")
    fields = json.load(open(FIELDS_IN, encoding="utf-8"))
    usage = (json.load(open(USAGE_IN, encoding="utf-8"))
             if os.path.exists(USAGE_IN) else {})
    curated = load_curated()
    sizes, names = load_opcodes()
    samples = collect(sys.argv[1:])
    handles = handle_census(samples)

    lines = [
        "# tos_view packet template -- GENERATED by name_fields.py, do not edit.",
        "# Names are guesses unless the origin column says otherwise; see",
        "# docs/14-field-names.md for what each origin is worth.",
        "#",
        "# P<TAB>opcode<TAB>name<TAB>size<TAB>coverage",
        "# F<TAB>offset<TAB>type<TAB>bytes<TAB>name<TAB>fmt<TAB>origin<TAB>note",
    ]

    stat = collections.Counter()
    emitted = 0
    ops = sorted({int(k) for k in fields} | set(curated))
    for op in ops:
        e = fields.get(str(op))
        cur = curated.get(op, {})
        size = e["size"] if e else sizes.get(op, 0)
        nm = e["name"] if e else names.get(op, "op%d" % op)
        cov = e["coverage"] if e else ("curated" if cur else "none")
        uses = usage.get(str(op), {}).get("uses", {})
        smp = samples.get(op, [])

        rows = []
        taken = []          # byte ranges already claimed, curated wins them

        for off in sorted(cur):
            c = cur[off]
            # str[0] runs to the end of the packet, and how far that is depends
            # on the packet in hand, not on the layout -- so it stays 0 here
            # and the viewer stretches it to whatever it is looking at.
            n = type_bytes(c["type"])
            rows.append((off, c["type"], n, c["name"], c["fmt"],
                         c["origin"], c["note"]))
            taken.append((off, off + max(n, 1)))
            stat["curated"] += 1

        def emit(off, width):
            wtype, vals = wire_type(off, width, smp)
            u = uses.get(str(off), [])
            name, fmt, origin = guess(off, wtype, vals, u, handles)
            note = "no samples" if not smp else (
                "%d samples" % len(vals) if vals else "")
            # A handle is four bytes. The width here came from a bare
            # `pkt + 10` stretched to the next known field (ida_dump_structs
            # has no other choice), so honouring it would draw a 32-byte
            # handle. Cut it back and leave the remainder as its own field
            # rather than swallowing bytes into a name that does not cover
            # them.
            if name == "handle" and width > 4:
                rows.append((off, "u32", 4, name, fmt, origin, note))
                stat[origin.split(":")[0]] += 1
                emit(off + 4, width - 4)
                return
            rows.append((off, wtype, width, name, fmt, origin, note))
            stat[origin.split(":")[0] or "unnamed"] += 1

        for off, _hexrays, width in (e["fields"] if e else []):
            if any(a <= off < b for a, b in taken):
                continue
            emit(off, width)

        if not rows:
            continue
        rows.sort()
        emitted += 1
        lines.append("P\t%d\t%s\t%d\t%s" % (op, nm, size, cov))
        for off, t, n, name, fmt, origin, note in rows:
            lines.append("F\t%d\t%s\t%d\t%s\t%s\t%s\t%s"
                         % (off, t, n, name, fmt, origin, note))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    named = sum(v for k, v in stat.items() if k != "unnamed")
    print("wrote %s" % OUT)
    print("  packets with a layout : %d" % emitted)
    print("  fields named          : %d of %d" % (named, sum(stat.values())))
    for k, v in stat.most_common():
        print("    %-10s %d" % (k, v))
    print("  handle values in census: %d" % len(handles))


if __name__ == "__main__":
    main()
