# Chat packet layouts

**Status:** `ZC_CHAT` rebuild is byte-exact (0 differences)
**Source:** `capture_1785545696.bin`, zone connection

Typed chat text only appears in the client's chat window once the zone echoes
`ZC_CHAT` back. Same gate as movement — see
`01-server-authoritative-movement.md`.

## CZ_CHAT (3188, variable)

| offset | field |
|---|---|
| 0x00 | u16 opcode = 3188 |
| 0x02 | u32 sequence |
| 0x06 | u32 checksum |
| 0x0A | 12 bytes, always zero |
| **0x16** | **u16 total size** |
| 0x18 | text, NUL-terminated (UTF-8) |

The size field is at **+0x16, not +0x0A**. See
`07-variable-packet-size-field.md` — this breaks the generic rule in
`tos_proto.packet_size`.

Observed sizes are the offset of the NUL terminator plus one; the bytes after
that up to the frame length are Blowfish padding to a multiple of 8, not part
of the packet.

## ZC_CHAT (3124, variable)

A **277-byte fixed part followed by NUL-terminated text**. The u16 at +0x0A is
the resulting total.

| offset | field |
|---|---|
| 0x00 | u16 opcode = 3124 |
| 0x0A | u16 total size |
| 0x0C | u32 speaker handle |
| 0x10 | char name[64] |
| 0x50 | char team name[64] |
| 0x92 | u16 class id (`0x07D3` observed) |
| 0x94 | u16 job id (`0x07D1` observed) |
| 0x98 | u8 chat channel (8 = say) |
| 0x9C | u8 = 1 |
| 0x9E | u8 = 0x1A |
| 0xB4 | u32 = 0x03E9 |
| 0xB8 | u32 = 0x01CC |
| 0xBC | name colour RGBA (`80 80 80 FF`) |
| 0xC0 | u16 ×3 = 0x07D1, 0x07D2, 0x07D3 |
| 0xD4 | u8 = 1 |
| 0xD5 | char balloon skin[64] (`"GLOBAL"`, `"None"`) |
| **0x115 (277)** | **text, NUL-terminated** |

The balloon-skin `char[64]` at 0xD5 is what puts the text at 277.

`size = 277 + len(text) + 1`. Confirmed against three captured packets of
lengths 280, 306 and 310.

## Slash commands are not speech

The live zone answered 21 `CZ_CHAT` with only 4 `ZC_CHAT`. Every echoed one was
plain text; everything beginning with `/` (e.g. `/memberinfoForAct <name>`) is
addon RPC and got no echo. `handlers.py` skips those — echoing them back would
spam the chat window with internal calls.

## Known-thin in our implementation

The class/job/colour block above is transcribed from **one** captured packet
sent by our own PC. Any chat we synthesise for another sender will come out
labelled with our name and colours. Driving those fields per-sender is
unfinished work.
