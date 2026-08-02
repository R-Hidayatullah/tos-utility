# Packet framing: headers, strings, sizes, routing

**Status:** all four confirmed against real bytes
**Sources:** `capture_1785545696.bin`, `packet_opcodes.csv`, the live client

Four framing rules that are invisible when you get them wrong. Each produces a
packet of plausible length that the client silently mis-parses, so none of them
show up as an error — only as a symptom somewhere else entirely.

## 1. Client packets have a 22-byte header, not 10

Both directions share `u16 opcode | u32 sequence | u32 checksum`. Server
packets start their body immediately after, at `+0x0A`. **Client packets carry
twelve further bytes the server never reads**, so their body starts at `+0x16`.

This is why `07` found the inline size of a variable client packet at `+0x16`:
it is not a special case, it is simply the first field of the body.

The declared sizes prove it. `CB_START_BARRACK` is 87 bytes and its body is a
byte plus a `char[64]`:

```
06 00 | 04 00 00 00 | 1b 00 00 00 | 00 x12 | 00 | "GLOBAL" ...
opcode  sequence      checksum      ignored  origin  serviceNation[64]
                                             +0x16   +0x17
```

`CZ_CONNECT` is 1269, and its fields — 1024 + 64 + 56 + 48 + 8·4 + 4·3 + 2·3 +
5 = 1247 — only reach 1269 from 22. `CB_LOGIN` is 570 against a 548-byte body.

**The social link is the exception.** Those packets have no extra header, so
body and inline size both sit at `+0x0A` in both directions.

| link | body starts | inline size at |
|---|---|---|
| barrack, zone (client to server) | +0x16 | +0x16 |
| barrack, zone (server to client) | +0x0A | +0x0A |
| social (both directions) | +0x0A | +0x0A |

## 2. Length-prefixed strings count their NUL

`LpString` is `u16 length` then `length` bytes, and **the length includes the
terminating NUL**. Nothing follows it.

From the session key in a live `ZC_CONNECT_OK`:

```
u16 = 42, then "*9B84B7AEA7EF4ABD07C77B6560C13A9904AC267C\0"
              41 characters + NUL = 42
```

Writing the length *without* the NUL and then appending one gives a packet of
exactly the right total length whose every following field is shifted by a
byte. This is the worst failure mode in the protocol: the size check passes,
the field-offset check passes, and nothing looks wrong on the wire.

In `ZC_CONNECT_OK` that string sits immediately before the commander block, so
the handle, name, position and stats were all read one byte off. Decoding the
capture with the wrong rule yields name `kaichi` and handle `0x339`; with the
right one, `Akaichi` and `0x339CC`. The client could not build the character
and never sent `CZ_GAME_READY` — it sat on "loading world" with nothing further
on the wire.

## 3. Declared size is the strongest available check

`packet_opcodes.csv` carries the `sizeof` the client registers per opcode. For
a fixed-size packet the finished length must equal it exactly, and because a
field added, dropped or mis-sized anywhere changes the total, this one check
catches most layout mistakes on its own.

It is what caught `BC_COMMANDER_CREATE` being built with the 520-byte zone
appearance block: 530 bytes against a declared 618. The barrack form carries an
extra 88-byte tail. Since the client sizes that packet from its own table, the
missing 88 bytes swallowed the head of the next packet and login stalled.

The complementary check is field offsets from `packet_schema.json` (see `08`).
A client read that starts inside one of our fields and ends inside the next
means the layout has drifted. Reads *narrower* than the field they sit in are
normal — the extractor records byte-level accesses, so the client picking up
the high half of a short is expected and is not a signal.

## 4. Route on the opcode, never on the port

Which port a client dials for login versus for the zone is a matter of its own
configuration. The client tested here sends `CB_LOGIN` to **7002**, not 2000.

The opcode name says which link a packet belongs to and cannot be configured
wrong:

| prefix | link |
|---|---|
| `CB_` / `BC_` | barrack |
| `CZ_` / `ZC_` | zone |
| `CS_` / `SC_` | social |

Routing on the listen port produced `no zone handler for CB_LOGIN` and a dead
connection. Routing on the opcode makes every port behave identically and the
client's settings stop mattering.

## Opcode table provenance

`packet_opcodes.csv` (1259 entries, extracted from `RegisterAllPackets`) was
diffed against a reference implementation's table generated from the same
client family. They agree on **all 1259 opcodes**, with three names where our
extractor fell back to a generic label and one size difference:

```
21001  ZC_INTERACTION_INFO             (extractor: GENERIC)
21002  CZ_INTERACTION_CANCEL, size 64  (extractor: GENERIC_S, size 0)
21003  ZC_INTERACTION_RIDE_USE_SKILL   (extractor: GENERIC_CL)
```

That agreement is what makes a reference implementation's *layouts* usable as a
starting point rather than a guess — but every one still has to be verified
against `packet_schema.json` and the declared size, because the tables agreeing
says nothing about the field order inside a packet. Three of them turned out to
be wrong; see `12` and `13`.
