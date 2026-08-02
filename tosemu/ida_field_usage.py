"""Record how the client *uses* each packet field, not just how wide it is. RUNS INSIDE IDA.

    (IDA MCP)  py_exec_file  ida_field_usage.py

`ida_dump_structs.py` answers "what is at +0x0E and how many bytes is it".
This answers "what does the client do with it", which is what a name has to
come from. A stripped binary has no `SetMoveSpeed` to read the name off, so
the evidence is the shape of the call instead:

    sub_140AC71B0(pkt + 10)        <- 1592 call sites across the binary

A helper called with the same field offset from a hundred different handlers
is doing one job, and naming that one helper names the field in all hundred.
So per field this records the callee, the argument slot it lands in, and the
width it was read at; `name_fields.py` turns the frequency table into names.

One level of aliasing is followed -- `v5 = *(float *)(pkt + 14);` then `f(v5)`
counts as a use of +0x0E by `f` -- because Hex-Rays hoists nearly every field
into a local before passing it.

Writes packet_usage.json.  Cheap to re-run: the decompiler cache is per
function, and handlers that cover several opcodes are decompiled once.
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
JSON_OUT = os.path.join(HERE, "packet_usage.json")

WIDTH = {
    "_BYTE": 1, "_WORD": 2, "_DWORD": 4, "_QWORD": 8, "_OWORD": 16,
    "char": 1, "unsigned __int8": 1, "__int8": 1, "bool": 1,
    "short": 2, "unsigned __int16": 2, "__int16": 2,
    "int": 4, "unsigned int": 4, "float": 4, "__int32": 4,
    "unsigned __int32": 4, "unsigned __int64": 8, "__int64": 8,
    "double": 8, "long long": 8,
}

ASSERT_RE = re.compile(r"packet->command == PACKET_([A-Z0-9_]+)")
CASE_RE = re.compile(r"^\s*case (\d+):", re.M)
# A direct call: name(args). The argument body allows one level of nesting so
# `f(g(x), y)` is not cut at the inner paren.
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\(((?:[^()]|\([^()]*\))*)\)")
# A vtable call: (*(...)(v6 + 88))(v4, ...). The slot is the only stable name
# such a call has, and it is stable enough -- the same slot on the same kind of
# object is the same method.
VCALL_RE = re.compile(r"\(\*\([^)]*\)\((\w+) \+ (\d+)LL?\)\)\(((?:[^()]|\([^()]*\))*)\)")


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
    out = {}
    for m in re.finditer(r"\*\(_WORD \*\)(\w+)\s*(?:!=|==)\s*(\d+)", text):
        out.setdefault(m.group(1), set()).add(int(m.group(2)))
    return out


def slice_for(text, name):
    marks = [(m.start(), m.group(1))
             for m in re.finditer(r"PACKET_([A-Z0-9_]+)", text)]
    if len(marks) < 2:
        return text
    for i, (pos, nm) in enumerate(marks):
        if nm == name:
            end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
            start = text.rfind("\n", 0, max(0, pos - 400))
            return text[max(0, start):end]
    return text


def body_for(text, op, name):
    """The part of a multi-opcode handler that belongs to this opcode."""
    if len(CASE_RE.findall(text)) > 3:
        cases = list(CASE_RE.finditer(text))
        for i, m in enumerate(cases):
            if int(m.group(1)) == op:
                end = cases[i + 1].start() if i + 1 < len(cases) else len(text)
                return text[m.start():end]
    return slice_for(text, name)


def split_args(s):
    """Top-level comma split -- `f(g(a, b), c)` is two arguments, not three."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def field_refs(expr, var):
    """(offset, width, is_ptr) for every packet access inside one expression."""
    refs = []
    typed = re.compile(
        r"\*\(\s*((?:unsigned\s+)?[A-Za-z_][\w ]*?)\s*\*\s*\)\(\s*%s\s*\+\s*(\d+)\s*\)"
        % re.escape(var))
    taken = set()
    for m in typed.finditer(expr):
        t = " ".join(m.group(1).split())
        w = WIDTH.get(t)
        if w is None:
            continue
        refs.append((int(m.group(2)), t, w, False))
        taken.add(m.start())
    for m in re.finditer(r"(?<![\w)])%s \+ (\d+)" % re.escape(var), expr):
        # skip the `a1 + 14` inside an already-counted `*(float *)(a1 + 14)`
        if any(m.start() > t and m.start() < t + 40 for t in taken):
            continue
        refs.append((int(m.group(1)), "ptr", 0, True))
    # the packet pointer itself, bare -- `f(a1)` is a use of the whole packet
    if re.search(r"(?<![\w])%s(?![\w +])" % re.escape(var), expr):
        refs.append((0, "self", 0, True))
    return refs


def aliases(body, var):
    """local -> (offset, type, width) for `vN = *(T *)(pkt + K);`."""
    out = {}
    pat = re.compile(
        r"\b(\w+) = \*\(\s*((?:unsigned\s+)?[A-Za-z_][\w ]*?)\s*\*\s*\)\(\s*%s\s*\+\s*(\d+)\s*\)\s*;"
        % re.escape(var))
    for m in pat.finditer(body):
        t = " ".join(m.group(2).split())
        w = WIDTH.get(t)
        if w is None:
            continue
        lv = m.group(1)
        # A local reassigned from two different fields tells us nothing.
        if lv in out and out[lv][0] != int(m.group(3)):
            out[lv] = None
        elif lv not in out:
            out[lv] = (int(m.group(3)), t, w)
    return {k: v for k, v in out.items() if v}


def scan_uses(body, var, size):
    """offset -> list of use records."""
    uses = {}
    alias = aliases(body, var)

    def add(off, rec):
        if size and off >= size:
            return
        uses.setdefault(off, []).append(rec)

    def handle_call(callee, argstr):
        for i, a in enumerate(split_args(argstr)):
            for off, t, w, isptr in field_refs(a, var):
                if t == "self":
                    continue
                add(off, {"callee": callee, "arg": i, "type": t,
                          "w": w, "ptr": isptr})
            for lv, (off, t, w) in alias.items():
                if re.search(r"(?<![\w])%s(?![\w])" % re.escape(lv), a):
                    add(off, {"callee": callee, "arg": i, "type": t,
                              "w": w, "ptr": False, "via": lv})

    for m in CALL_RE.finditer(body):
        callee = m.group(1)
        if callee in ("if", "while", "for", "switch", "return", "sizeof"):
            continue
        handle_call(callee, m.group(2))
    for m in VCALL_RE.finditer(body):
        handle_call("vcall+%s" % m.group(2), m.group(3))

    # Comparisons carry meaning too: a field tested against a small constant is
    # a flag or an enum, and that shows up in the name as much as any call does.
    cmp_pat = re.compile(
        r"\*\(\s*(?:unsigned\s+)?[A-Za-z_][\w ]*?\s*\*\s*\)\(\s*%s\s*\+\s*(\d+)\s*\)\s*(==|!=|>|<|>=|<=)\s*(-?\d+)"
        % re.escape(var))
    for m in cmp_pat.finditer(body):
        add(int(m.group(1)), {"cmp": m.group(2), "const": int(m.group(3))})
    return uses


def scan():
    sizes, names = load_table()
    by_name = {v: k for k, v in names.items()}

    asserts = {}
    for s in idautils.Strings():
        m = ASSERT_RE.search(str(s))
        if m:
            op = by_name.get(m.group(1))
            if op is not None:
                asserts[op] = s.ea

    targets = {}
    for op, sea in asserts.items():
        for xr in idautils.XrefsTo(sea):
            f = idaapi.get_func(xr.frm)
            if f:
                targets.setdefault(op, set()).add(f.start_ea)

    cache = {}
    out = {}
    for op, funcs in sorted(targets.items()):
        best_uses, best_score = None, None
        for fea in sorted(funcs):
            if fea not in cache:
                cache[fea] = decompile(fea)
            text = cache[fea]
            if not text:
                continue
            pv = packet_vars(text)
            var = next((v for v, ops in pv.items() if op in ops), None)
            if var is None:
                continue
            body = body_for(text, op, names[op])
            uses = scan_uses(body, var, sizes.get(op, 0))
            # Same tie-break as ida_dump_structs: the candidate that says the
            # most about this packet wins, so a generic dispatcher does not
            # drown out the dedicated handler.
            score = (-len(uses), -sum(len(v) for v in uses.values()))
            if best_score is None or score < best_score:
                best_uses, best_score = uses, score
        if best_uses:
            out[str(op)] = {"name": names[op], "size": sizes.get(op, 0),
                            "uses": {str(k): v for k, v in sorted(best_uses.items())}}

    with open(JSON_OUT, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)

    import collections
    callees = collections.Counter()
    sites = collections.Counter()
    for e in out.values():
        for off, recs in e["uses"].items():
            for r in recs:
                if "callee" in r:
                    callees[(r["callee"], r["arg"])] += 1
                    sites[r["callee"]] += 1
    print("opcodes with usage : %d" % len(out))
    print("distinct callees   : %d" % len(sites))
    print("\ntop callee/arg pairs (these are what naming hangs on):")
    for (c, a), n in callees.most_common(25):
        print("  %-24s arg%d  %4d uses" % (c, a, n))
    print("\nwrote %s" % JSON_OUT)


scan()
