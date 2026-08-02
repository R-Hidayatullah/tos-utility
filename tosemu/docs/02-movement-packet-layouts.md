# Movement packet layouts

**Status:** verified by rebuilding captured replies byte-for-byte
**Source:** `capture_1785545696.bin`, zone connection

All offsets are from the start of the packet, header included
(`u16 opcode | u32 sequence | u32 checksum | body`).

The client packets carry **no handle** — the zone knows who is asking. Every
echo inserts our handle at +0x0A (or +0x0C for `ZC_CHAT`) and otherwise passes
the same floats straight back, which is why the builders in `handlers.py` are
byte slices rather than unpack/repack.

## CZ_KEYBOARD_MOVE (3166, 73 B) → ZC_MOVE_DIR (3110, 76 B)

| field | CZ | ZC |
|---|---|---|
| handle u32 | — | 0x0A |
| x, y, z floats | 0x16 | 0x0E |
| dx, dz floats | 0x22 | 0x1A |
| moving flag u8 | 0x48 | 0x22 |
| speed float | — | 0x23 |
| server time float | — | 0x27 |
| stance u8 | — | 0x43 |
| trailing u8 = 1 | — | 0x4B |

So `ZC[0x0E:0x22] = CZ[0x16:0x2A]` in one slice.

The client's own float at CZ+0x2A is **dropped** — the zone substitutes the
authoritative speed, constant `35.0` (`00 00 0C 42`) across the whole capture.

`ZC+0x22` mirrors `CZ+0x48` exactly in all 164 pairs.

`ZC+0x43` is **zone-side stance and not derivable from the request**: 6 in 156
pairs, 1 in 7 pairs with identical input, 0 only when the moving flag is clear.
`handlers.py` takes the common case (`6 if moving else 0`). If certain
mid-run animations look wrong, this is the field.

## CZ_MOVE_STOP (3172, 71 B) → ZC_PC_MOVE_STOP (3514, 63 B)

| field | CZ | ZC |
|---|---|---|
| handle u32 | — | 0x0A |
| x, y, z floats | **0x17** | 0x0E |
| flag u8 = 1 | — | 0x1A |
| dx, dz floats | 0x23 | 0x1B |
| server time float | — | 0x23 |

**Gotcha:** `CZ_MOVE_STOP` carries one more leading pad byte than
`CZ_KEYBOARD_MOVE`, so its position starts at **0x17, not 0x16**. Copying the
`CZ_KEYBOARD_MOVE` offsets here produces garbage positions.

Rebuild result: byte-exact on both captured pairs.

## CZ_ROTATE (3184, 34 B) → ZC_ROTATE (3145, 28 B)

| field | CZ | ZC |
|---|---|---|
| handle u32 | — | 0x0A |
| two direction floats | 0x1A | 0x0E |
| flag u8 = 1 | — | 0x16 |

Both floats pass through in the **same order**. An early read of a single
unpaired sample suggested they swap — they do not. Rebuild result: byte-exact
on all 4 captured pairs.

## CZ_JUMP (3168, 71 B) → ZC_JUMP (3116, 71 B)

| field | CZ | ZC |
|---|---|---|
| handle u32 | — | 0x0A |
| jump power float | — | 0x0E |
| x, y, z, dx, dz | 0x17 | **0x17** |
| server time float | — | 0x2B |

Body offsets line up between request and reply; only the head differs. Jump
power is constant `350.0` (`00 00 AF 43`) across both captured jumps.

Rebuild result: byte-exact on both captured pairs.

## Verification method

Pair each client packet with the next server packet of the expected opcode
carrying our handle, rebuild the reply from the request, then compare with the
sequence/checksum and timestamp fields masked out. See the validation snippets
in the session that produced `handlers.py`.

Caveat: naive next-match pairing mismatches some `ZC_MOVE_DIR` because the
server also pushes unsolicited moves. Position-float differences in that
comparison are pairing noise, not builder bugs — position is copied straight
through.
