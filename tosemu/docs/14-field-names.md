# Naming packet fields

**Status:** pipeline built; 228 of 504 fields named, the rest honestly `f_XX`
**Tools:** `ida_field_usage.py` (runs in IDA) + `field_names.tsv` (hand-written)
→ `name_fields.py` → `packet_template.tsv` → the **fields** panel in `tos_view`

`08-struct-extraction.md` answered *what is at +0x0E and how wide is it*. This
answers *what to call it*, which is what turns a hex dump into a value list.

## Why the binary does not simply say

The client is stripped. There is no `SetMoveSpeed` to read a name off — every
callee is `sub_140AC71B0`. The 128 `PKS_*` strings in the image come from
`VariablePacketReader<struct PKS_ZC_NORMAL>::CheckValidPosition` template
instantiations, so they name the **structs** and never their members. Only one
string in the whole binary mentions a member, and it is
`packet->len >= sizeof(PKS_VARSIZE)`.

## What does say something: the shape of the call

A helper called with the same field offset from dozens of unrelated handlers
is doing one job. Name the helper once and the field is named everywhere it
appears. `ida_field_usage.py` records, per field, which function received it
and in which argument slot, following one level of aliasing because Hex-Rays
hoists nearly every field into a local first.

Three functions carry the single most common field in the protocol:

| function | what it is | evidence |
|---|---|---|
| `sub_140AC71B0` | `GetActorByHandle` | `sub_140AEB170(GetActorManager(), a1)`; 1592 call sites |
| `sub_140AD7290` | `IsMyHandle` | the whole body is `return *(_DWORD *)GetMyPC() == *a1;` |
| `sub_140AD5920` | a handle predicate | calls `IsMyHandle` |

All three take a `_DWORD *`, and across the 106 handlers with recorded usage
they receive `pkt + 10` in 35 of 46 packet call sites. So **+0x0A is a u32
handle** — which is also what `02` and `04` concluded from captures alone,
without touching the binary. Two independent methods, same answer.

## The four sources, and what each is worth

| origin | shown as | worth |
|---|---|---|
| `docs/NN` | white | verified by rebuilding a captured reply byte-for-byte |
| `ida:sub_…` | white | the client's own use of the field |
| `census` | white | the value behaves like a handle across the session |
| `bytes` | dim | the bytes look like a float / a flag / text |
| *(none)* | dim | `f_XX`, nothing is claimed |

The pane keeps the origin in its own column rather than flattening everything
into one confident-looking name. A field named from a byte-exact rebuild and a
field named because four bytes parse as a plausible float must not read the
same on screen.

### The capture heuristics

- **handle** — a u32 whose value appears at the head of eight or more
  *different* opcodes in one session. Nothing else in the protocol repeats
  that way; a map id or an item count shows up in one or two.
- **pos / dir** — a float triple is a position; a float pair is `dir` only
  when `dx² + dz² ≈ 1` in 80% of samples, otherwise it stays `vec2`.
- **server_time** — a float above 1000 that only ever climbs (`05`).
- **flag / zero / const** — a u8 that is only ever 0 or 1, a field that is
  always zero, a field that never changes.

Float-vs-int uses the same denormal test as `gen_packets_h.py`: a small int
reinterpreted as a float lands in the denormals, a real game float never does.

## The client→server side is hand-written

`08` explains why the binary yields no `CZ_*` layouts: the client *builds*
outbound packets rather than parsing them, so there is no reader handler to
decompile. `field_names.tsv` is where those live, transcribed from `02`, `03`,
`05` and `09` — the layouts that were established by rebuilding captured
replies until they matched byte for byte. That file outranks everything the
generator infers, and it is the only reason `CZ_KEYBOARD_MOVE` shows named
fields at all.

It also carries the client packets' **22-byte header** (`11`), which is why its
body offsets start at 0x16 and not 0x0A.

## Gaps are shown, not hidden

Bytes no layout accounts for get `(unread)` rows with their own offset and
length. `ZC_CHAT` has six of them between `team_name` and `unk_B4`. Those gaps
are what a capture gets opened to find, and a template that quietly stretched a
neighbouring field over them would be worse than no template.

## Running it

```bash
python name_fields.py relay/dumps/capture_*.bin
```

The IDA half runs first and only when the client is updated:

```
(IDA MCP)  py_exec_file  ida_dump_structs.py     # offsets and widths
(IDA MCP)  py_exec_file  ida_field_usage.py      # how each field is used
```

`field_names.tsv` is hand-maintained and never written by the generator.

## Known-thin

- **269 fields are still `f_XX`.** The helper-consensus method only pays off
  where a helper is called from many handlers; a field passed to a function
  used once tells you nothing on its own.
- **Repeated records are not modelled.** `ZC_PARTY_LIST` at 6790 bytes and
  `ZC_ITEM_INVENTORY_DIVISION_LIST` are count-prefixed arrays, and the pane
  shows their bodies as one `(unread)` run. A repeat construct is the obvious
  next step, and it is the one thing a real 010 Editor template has that this
  does not.
- **`docs/02` said `ZC_MOVE_DIR+0x23` is constant 35.0.** It reads 48.0 in the
  2026-08-01 capture. The field is the speed the zone substitutes, and it is
  per-session, not a protocol constant — the note in `field_names.tsv` now says
  so.
