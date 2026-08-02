"""Sweep every opcode and dump the client's packet field accesses. RUNS INSIDE IDA.

    (IDA MCP)  py_exec_file  ida_dump_structs.py

Three sources are combined per opcode:

  1. the handler's FATAL_ASSERT -- `packet->command == PACKET_<NAME>` pins a
     function to an opcode, and also hands us the source file and C++ method
  2. the decompiled body -- `*(_DWORD *)(pkt + 0x2A)` gives offset and width
  3. RegisterAllPackets, already extracted to packet_opcodes.csv -- sizeof

The packet pointer is identified by the assert's own comparison
(`*(_WORD *)a1 != 3110`), which is exactly the variable holding the packet, so
no guessing about which parameter it is.

Writes packet_fields.json for gen_packets_h.py to turn into a C header.
"""

import csv
import json
import os
import re

import ida_hexrays
import idaapi
import idautils
import idc

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in dir() else \
    r"C:\Users\Ridwan Hidayatullah\Documents\tosemu"
CSV_IN = os.path.join(HERE, "packet_opcodes.csv")
JSON_OUT = os.path.join(HERE, "packet_fields.json")

# Hex-Rays spells widths several ways; map every spelling to a byte count.
WIDTH = {
    "_BYTE": 1, "_WORD": 2, "_DWORD": 4, "_QWORD": 8, "_OWORD": 16,
    "char": 1, "unsigned __int8": 1, "__int8": 1, "bool": 1,
    "short": 2, "unsigned __int16": 2, "__int16": 2,
    "int": 4, "unsigned int": 4, "float": 4, "__int32": 4,
    "unsigned __int32": 4, "unsigned __int64": 8, "__int64": 8,
    "double": 8, "long long": 8,
}
# Which spellings imply a real type rather than just a width.
SIGNED_FLOAT = {"float", "double"}

ASSERT_RE = re.compile(r"packet->command == PACKET_([A-Z0-9_]+)")
CASE_RE = re.compile(r"^\s*case (\d+):", re.M)


def load_table():
    sizes, names = {}, {}
    with open(CSV_IN, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            op = int(row["opcode_dec"])
            sizes[op] = int(row["size"])
            names[op] = row["name"]
    return sizes, names


def decompile(ea):
    try:
        cf = ida_hexrays.decompile(ea)
        return str(cf) if cf else None
    except Exception:
        return None


def packet_vars(text):
    """Variables compared against an opcode -- these hold the packet."""
    out = {}
    for m in re.finditer(r"\*\(_WORD \*\)(\w+)\s*(?:!=|==)\s*(\d+)", text):
        out.setdefault(m.group(1), set()).add(int(m.group(2)))
    return out


def slice_for(text, name):
    """Body belonging to one opcode inside a function that handles several.

    Several handlers cover two opcodes (ActionMoveDir and ActionPCMoveStop
    share a function), so the whole body would merge two layouts and produce
    fields past the declared size. Cut from this opcode's assert to the next.
    """
    marks = [(m.start(), m.group(1))
             for m in re.finditer(r"PACKET_([A-Z0-9_]+)", text)]
    if len(marks) < 2:
        return text
    for i, (pos, nm) in enumerate(marks):
        if nm == name:
            end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
            # the assert precedes the body, so reach back to the enclosing
            # statement and forward to just before the next handler's assert
            start = text.rfind("\n", 0, max(0, pos - 400))
            return text[max(0, start):end]
    return text


def tile(fields, size):
    """Widest-first non-overlapping tiling, dropping reads past the packet.

    Hex-Rays merges adjacent fields into one wide read (two floats become a
    _QWORD) and also emits narrow reads inside them. Keep the widest read at
    each offset and discard anything it already covers.
    """
    out = {}
    for off in sorted(fields):
        t, w = fields[off]
        if size and off >= size:
            continue                       # past the packet: wrong attribution
        if size and w and off + w > size:
            continue
        out[off] = (t, w)
    keep, end = {}, -1
    for off in sorted(out):
        t, w = out[off]
        if off < end:
            continue                       # inside a wider read already taken
        keep[off] = (t, w)
        end = off + max(w, 1)
    # A bare `pkt + N` has no width of its own -- it is a handle, string or
    # nested struct passed on by pointer. Stretch it to the next field so the
    # struct still tiles and the gap report stays honest.
    offs = sorted(keep)
    for i, off in enumerate(offs):
        t, w = keep[off]
        if w:
            continue
        nxt = offs[i + 1] if i + 1 < len(offs) else (size or off + 4)
        keep[off] = (t, max(nxt - off, 1))
    return keep


def uncovered(fields, size):
    """Bytes between the header and `size` that no field accounts for."""
    end, gap = 10, 0
    for off in sorted(fields):
        w = max(fields[off][1], 1)
        if off > end:
            gap += off - end
        end = max(end, off + w)
    if size and end < size:
        gap += size - end
    return gap


def best(cands, size):
    """Pick the candidate function whose fields tile the packet best."""
    pick, score = {}, None
    for fea, fields in sorted(cands.items()):
        if not fields:
            continue
        s = (uncovered(fields, size), -len(fields))
        if score is None or s < score:
            pick, score = fields, s
    return pick


def accesses(text, var):
    """Every `*(TYPE *)(var + N)` and bare `var + N` in `text`."""
    found = {}
    typed = re.compile(
        r"\*\(\s*((?:unsigned\s+)?[A-Za-z_][\w ]*?)\s*\*\s*\)\(\s*%s\s*\+\s*(\d+)\s*\)"
        % re.escape(var))
    for m in typed.finditer(text):
        t = " ".join(m.group(1).split())
        off = int(m.group(2))
        w = WIDTH.get(t)
        if w is None:
            continue
        prev = found.get(off)
        # widest read wins; a QWORD read of two floats is the real field extent
        if prev is None or w > prev[1]:
            found[off] = (t, w)
    # bare `var + N` -- a string or nested struct the handler passes along
    for m in re.finditer(r"(?<![\w)])%s \+ (\d+)" % re.escape(var), text):
        off = int(m.group(1))
        found.setdefault(off, ("ptr", 0))
    return found


def scan():
    sizes, names = load_table()
    by_name = {v: k for k, v in names.items()}

    # --- 1. index the FATAL_ASSERT strings -------------------------------
    asserts = {}                      # opcode -> string ea
    for s in idautils.Strings():
        m = ASSERT_RE.search(str(s))
        if m:
            op = by_name.get(m.group(1))
            if op is not None:
                asserts[op] = s.ea

    # --- 2. assert string -> handler functions ---------------------------
    targets = {}                      # opcode -> set(func ea)
    for op, sea in asserts.items():
        for xr in idautils.XrefsTo(sea):
            f = idaapi.get_func(xr.frm)
            if f:
                targets.setdefault(op, set()).add(f.start_ea)

    # --- 3. decompile once per function, extract per opcode --------------
    cache, meta = {}, {}
    result = {}
    for op, funcs in sorted(targets.items()):
        cands, srcs = {}, []
        for fea in sorted(funcs):
            if fea not in cache:
                cache[fea] = decompile(fea)
            text = cache[fea]
            if not text:
                continue
            pv = packet_vars(text)
            var = None
            for v, ops in pv.items():
                if op in ops:
                    var = v
                    break
            if var is None:
                continue

            # A Process-style switch handles many opcodes in one function;
            # keep only this opcode's case block or we would merge layouts.
            if len(CASE_RE.findall(text)) > 3:
                body = text
                cases = list(CASE_RE.finditer(text))
                for i, m in enumerate(cases):
                    if int(m.group(1)) == op:
                        end = cases[i + 1].start() if i + 1 < len(cases) else len(text)
                        body = text[m.start():end]
                        break
            else:
                body = slice_for(text, names[op])

            cands[fea] = tile(accesses(body, var), sizes.get(op, 0))

            fn = idc.get_func_name(fea)
            sm = re.search(r'"(F:\\\\?live[^"]+\.cpp)"', text)
            mm = re.search(r'"(void __cdecl [^"]+)"', text)
            srcs.append({"func": fn, "ea": "0x%x" % fea,
                         "file": (sm.group(1) if sm else "").replace("\\\\", "\\"),
                         "method": mm.group(1) if mm else ""})
        # Several functions reference the same assert (a dedicated handler plus
        # generic dispatchers). Merging them mixes layouts and produces widths
        # that belong to a different packet, so score each and keep the best.
        fields = best(cands, sizes.get(op, 0))
        if fields:
            result[op] = {"fields": fields, "sources": srcs}
        meta[op] = srcs

    # --- 4. emit, including opcodes we found nothing for -----------------
    out = {}
    for op in sorted(names):
        e = result.get(op)
        flds = (sorted([[o, t, w] for o, (t, w) in e["fields"].items()])
                if e else [])
        # Coverage tells you how far to trust each struct: "exact" means the
        # fields tile from the end of the header to the declared size with no
        # gap, so nothing was missed.
        cov, gaps = "none", []
        if flds:
            sz = sizes[op]
            end = 10
            for o, t, w in flds:
                if o > end:
                    gaps.append([end, o - end])
                end = max(end, o + max(w, 1))
            if sz:
                if end < sz:
                    gaps.append([end, sz - end])
                cov = "exact" if not gaps else "partial"
            else:
                cov = "variable"
        out[str(op)] = {
            "name": names[op],
            "size": sizes[op],
            "coverage": cov,
            "gaps": gaps,
            "fields": flds,
            "sources": (e["sources"] if e else meta.get(op, [])),
        }

    with open(JSON_OUT, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)

    import collections
    cov = collections.Counter(v["coverage"] for v in out.values())
    print("opcodes in table      : %d" % len(names))
    print("assert strings found  : %d" % len(asserts))
    print("handlers resolved     : %d" % len(targets))
    print("opcodes with fields   : %d" % sum(1 for v in out.values() if v["fields"]))
    print("  exact (tiles to size): %d" % cov["exact"])
    print("  partial (has gaps)   : %d" % cov["partial"])
    print("  variable-size        : %d" % cov["variable"])
    print("  no fields            : %d" % cov["none"])
    print("wrote %s" % JSON_OUT)


scan()
