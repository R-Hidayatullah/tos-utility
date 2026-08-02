# Recovering packet structs from the client

**Status:** pipeline built and validated
**Tools:** `ida_dump_structs.py` (runs in IDA) → `gen_packets_h.py` → `packets.h`

## Three sources, cross-checked

| source | gives | limits |
|---|---|---|
| **Handler decompilation** | exact offsets and read widths | only fields the client reads |
| **`RegisterAllPackets`** → `packet_opcodes.csv` | `sizeof` per opcode | total only |
| **Captured samples** | float vs int, which fields vary | needs samples |

The three agree or they don't, and disagreement is the bug signal.

## How handlers are found

Every parsing handler opens with a `FATAL_ASSERT`:

```c
if ( *(_WORD *)a1 != 3110 )
    sub_1402A0560("F:\\live\\Source\\geClient\\Source\\GeActionPacket.cpp",
                  "void __cdecl ActionMoveDir(const char *)", 131,
                  "FATAL_ASSERT", "packet->command == PACKET_ZC_MOVE_DIR", v7);
```

That single line hands over four things: the opcode, the source file, the C++
method name, and — via `*(_WORD *)a1` — **which variable holds the packet**, so
no guessing which parameter to trace. 155 opcodes carry this pattern.

## Verification criterion

Fields must tile from the end of the 10-byte header to the declared size with
no gap. `ZC_MOVE_DIR`'s last access is `*(_BYTE *)(a1 + 75)` → 76, which is
exactly the table size. **Perfect closure means nothing was missed.** Anything
that overflows the declared size means fields from two packets got merged.

Coverage is recorded per opcode as `exact`, `partial` (with byte ranges the
client never reads), `variable`, or `none`.

## Three bugs the criterion caught

1. **Merged layouts.** `ActionMoveDir` and `ActionPCMoveStop` share a function,
   so `ZC_PC_MOVE_STOP` came out with fields past its 63-byte size. Fixed by
   slicing the body between consecutive `PACKET_*` asserts.
2. **Mixed candidates.** Four functions reference each assert (a dedicated
   handler plus generic dispatchers). Merging them produced widths belonging to
   other packets. Fixed by scoring each candidate on how well it tiles and
   keeping the best — `exact` count went 15 → 52.
3. **Float detection.** A magnitude floor cannot separate floats from ints: a
   normalised direction component reads ~5e-8 (23 of 345 `ZC_MOVE_DIR` samples)
   while handle `0x000339CC` reads as 3e-40. **Denormals are the discriminator**
   — small ints reinterpreted as floats land there, real game floats never do.

## Result

`packets.h`, 1259 structs, compiles clean.

| | count |
|---|---|
| exact (tiles to declared size) | 52 |
| partial (gaps the client skips) | 26 |
| variable-size | 43 |
| no handler found | 1138 |
| cross-checked against real bytes | 51 |

## The big limitation: direction

The 121 recovered structs are **all server→client**.

| direction | with fields | without |
|---|---|---|
| server→client (`ZC_`/`BC_`/`SC_`) | 121 | 509 |
| client→server (`CZ_`/`CB_`/`CS_`) | **0** | 626 |

The client *parses* what it receives and *builds* what it sends, so there are
no reader handlers for `CZ_*`. `PKS_CZ_KEYBOARD_MOVE` comes out as an opaque
63-byte body even though `02-movement-packet-layouts.md` documents it fully —
that layout came from capture diffing, not from the binary.

Recovering `CZ_*` layouts means going after the **writer** side:
`VariablePacketWriter<PKS_CZ_*>` instantiations and the `packet__SendCBLogin`
family. Not attempted yet.

Meanwhile `struct_infer.py` (capture diffing) stays the source for `CZ_*`, and
the two approaches are complementary: the binary is authoritative about
offsets, the capture is authoritative about which bytes actually carry data.

## Validation

The pipeline independently reproduces every layout derived by hand in
`02-movement-packet-layouts.md` and `03-chat-packet-layouts.md`, including
`ZC_CHAT`'s balloon-skin `char[64]` at `+0xD5` putting the text at 277.

It also **corrected one**: `ZC_MOVE_DIR+0x43` is a `uint32_t`, not the `uint8_t`
assumed from capture diffing alone. Writing the low byte gives the right result
for the observed value 6, but the field is four bytes wide.

## Running it

```bash
python gen_packets_h.py capture_*.bin
```

The IDA half must run first, from the MCP: `py_exec_file ida_dump_structs.py`.
It writes `packet_fields.json`, which the generator consumes — so regenerating
the header after a new capture needs no IDA session.
