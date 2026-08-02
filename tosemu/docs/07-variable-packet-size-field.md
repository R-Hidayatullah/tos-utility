# Variable-size packets: the size field is not always at +0x0A

**Status:** confirmed for `CZ_CHAT`; latent risk on the chat link
**Source:** `capture_1785545696.bin`, `tos_proto.py`

## The generic rule

`tos_proto.packet_size` implements the rule reversed from `gePacketTable`:
when the table entry is 0 (variable), the total size is a u16 at **+0x0A**.

```python
if size > 0:
    return size
return struct.unpack_from("<H", plain, 10)[0]
```

That holds for the server→client direction. `ZC_CHAT` (3124), for example,
carries its total at +0x0A and the values match the recorded body lengths
(280, 306, 310).

## Where it does not hold

`CZ_CHAT` (3188), client→server, has **twelve zero bytes** at +0x0A and its
real total at **+0x16**:

```
0000  74 0C 4A 00 00 00 CB 06 00 00 00 00 00 00 00 00
0010  00 00 00 00 00 00 31 00 2F 6D 65 6D 62 65 72 69   ....."1."/memberi
                        ^^^^^ size = 0x31 = 49
```

`packet_size` returns **0** for these. Confirmed across five captured
`CZ_CHAT`: sizes 49, 55, 49, 47, 55 at +0x16, always `0` at +0x0A.

The bytes between the NUL terminator and the frame length are Blowfish padding
to a multiple of 8, not packet content — a 49-byte packet arrives in a 56-byte
frame.

## Why it has not broken anything yet

On the **zone** link (`framing == "enc"`) the receive loop frames on the u16
padded length that precedes each Blowfish block, so `packet_size` is only used
for the log line. A wrong answer prints `size=0 chk=?` and nothing more.

## The latent risk

On the **chat/social** link (`framing == "plain"`) there is no length prefix,
so `serve()` genuinely frames on `packet_size`, and a wrong answer drops the
connection:

```python
size = P.packet_size(bytes(buf[:12]))
if not size or size < 10 or size > 0x8000:
    ctx.log("plain: cannot size op=%d -- dropping" % op)
    return          # -> conn.close()
```

Two client→server opcodes on that link are variable-size: **`CS_CHAT` (15908)**
and **`CS_PARTY_CLIENT_INFO_SEND` (15918)**. If either puts its size somewhere
other than +0x0A — as its zone counterpart `CZ_CHAT` does — the chat connection
dies the first time the player types into a chat channel that routes there.

Not yet observed, because our capture proxy parses client→server as Blowfish
and so never decoded the plaintext `CS_*` side. Those connections show up in
the capture as server-only, which is also why `extract_replay.py` grabs the
`CHAT` group wholesale rather than by trigger.

## If it bites

The log line `plain: cannot size op=<n> -- dropping` is the signature. The fix
is a per-opcode size-offset override rather than the single +0x0A rule; the
zone side already needs one for `CZ_CHAT` if `packet_size` is ever used for
framing there.

For reference: 259 opcodes in the table are variable-size, 56 of them `CZ_*`
client→server. Only `CZ_CHAT` has been checked.


## Why +0x16, resolved

This note found the offset empirically and left it unexplained. The reason is
that **client packets carry a 22-byte header, not 10**: twelve bytes past the
standard `opcode | sequence | checksum` that the server does not read. The
inline size therefore sits at the start of the body, +0x16, exactly where it
sits at +0x0A for server packets.

The declared sizes confirm it. `CZ_CONNECT` is 1269 bytes, and its fields --
1024 + 64 + 56 + 48 + 8 + 8 + 8 + 8 + 4*3 + 2*3 + 5 -- add up to 1247, which
only reaches 1269 from 22. `CB_LOGIN` is 570 and its body is 548. Reading a
client packet from +0x0A shifts every field by twelve bytes.
