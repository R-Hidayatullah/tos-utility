# Running the proxy and the server

**Status:** operational guide, current as of the tree it ships with
**Covers:** capturing live traffic, running the emulator, reading what came out

There are two things you can point the game client at, and they answer opposite
questions:

| | you want | use |
|---|---|---|
| **proxy** | what does the *real* server do? | `tos_relay.exe`, or `tos_proxy.py` |
| **emulator** | does *our* server hold the client up? | `tosemu_server.exe` |

Both work the same way from the client's side: it is told to connect to
`127.0.0.1` instead of the live address. Nothing is hooked or injected.

---

## Part 1 — capturing live traffic

### Use the relay

`cpp/relay/tos_relay.exe` is the one to reach for. It patches the client's
config, proxies both links, dumps every packet, and puts the files back on the
way out — including after a hard kill. Full reference in
[`cpp/relay/README.md`](../cpp/relay/README.md); the short version:

```bash
cmake -S cpp/relay -B cpp/relay/build -G Ninja && cmake --build cpp/relay/build
```

A prebuilt `cpp/relay/tos_relay.exe` is also committed; it is a separate copy
from the build output and is not refreshed by building, same trap as the
server below.

Run it from the directory holding `bf_inittable.bin` and `packet_opcodes.csv`
(the repo root), so its `--data` default resolves:

```bash
./cpp/relay/build/tos_relay.exe --region=asia
```

Then start the game normally and play. Ctrl+C to stop.

Editing files under `Program Files` needs administrator rights. Without them
the relay still captures — it says so and leaves the game alone, but you then
have to point the client yourself as in the manual path below.

If it was killed rather than stopped, put the client's files back:

```bash
./cpp/relay/build/tos_relay.exe --restore
```

The presence of `relay/patch_journal.tsv` always means "the game is still
patched". A normal run recovers it automatically.

Output lands in `relay/dumps/capture_<timestamp>.bin`.

### The manual path: tos_proxy.py

Older, Python, and it does **not** patch or restore anything — you edit the
client's config yourself and put it back yourself. It exists because it is easy
to read and modify when you are chasing one specific packet.

It listens on 7001 and 7002 and maps each to a fixed upstream, so those two
port numbers are not negotiable.

1. Serve this directory:

```bash
python -m http.server 8080
```

2. In `<game>\release\client.xml`, point both URLs at it:

   - `ServerListURL` → `http://127.0.0.1:8080/serverlist_proxy.xml`
   - `StaticConfigURL` → `http://127.0.0.1:8080/`

3. Delete or rename `<game>\release\serverlist_recent.xml`. The client caches
   the last list it used and will dial the live addresses straight past you.

4. Run it:

```bash
python tos_proxy.py asia
```

Use **`serverlist_proxy.xml`**, not `serverlist.xml`. The latter is the
emulator's and points at 2000/7002; the proxy needs 7001/7002. That is the one
mistake this path invites.

Output: `capture/live_<timestamp>.jsonl`, plus raw per-direction streams.

### Why the client must be told twice

Repointing `ServerListURL` alone is not enough. The client also fetches
`static__Config.txt` from `StaticConfigURL`, and **that** file carries
`UseSteamClient`. Serve nothing for it and the client falls back to defaults —
a Steam player is suddenly asked for a username and password that do not exist.

The relay handles this by generating the file from the install's own
`client.xml` and logging which login path the client will take:

```
[INF] login: Steam (static__Config.txt UseSteamClient=YES, from client.xml)
```

For the manual path, `static__Config.txt` in the repo root is what gets served.
See `12` for the complete list of keys the client actually reads — several
plausible-looking ones are not keys at all.

---

## Part 2 — running the emulator

### Build

```bash
cmake -S cpp/server -B cpp/server/build -G Ninja && cmake --build cpp/server/build
```

That produces `cpp/server/build/tosemu_server.exe`. There is a **second copy at
the repo root**, and nothing keeps the two in sync — no post-build rule copies
it. `devrun.py` runs the root one, so a build alone will not change what
`devrun.py` launches:

```bash
cp cpp/server/build/tosemu_server.exe tosemu_server.exe
```

Do that after every build you intend to test through `devrun.py`, or run the
built binary directly and skip the root copy entirely.

### Run

From the repo root, so `--data=.` finds `packet_opcodes.csv`, `properties.txt`,
`bf_inittable.bin` and `packet_schema.json`:

```bash
./cpp/server/build/tosemu_server.exe --data=. --game="C:/Program Files (x86)/Steam/steamapps/common/TreeOfSavior"
```

A healthy start looks like this:

```
data      1259 opcodes
data      127 recovered packet layouts
data      client: 1220 archives, 472768 files
data      543 maps, 6304 monsters, 22100 items, 1483 skills, 137 jobs
data      32956 property ids
world     start map 1001 c_Klaipe
net       listening on 0.0.0.0:2000
net       listening on 0.0.0.0:7002
net       listening on 0.0.0.0:9001
net       listening on 0.0.0.0:9002
```

Check those numbers. They are the fastest signal that something is wrong:

| line | what a bad value means |
|---|---|
| `6304 monsters` | both `monster.ies` and `monster_npc.ies` loaded. 4288 means the NPC table is missing and no town NPC will spawn (`09`) |
| `543 maps` | the client data opened. Missing entirely means `--game` is wrong, and the server runs bare — no maps, monsters or spawns |
| `32956 property ids` | `properties.txt` was found. A warning here means stats will not appear (`12`) |
| `127 recovered packet layouts` | outbound packets are being checked against the client's own parsing. `--no-verify` turns this off |

| option | |
|---|---|
| `--data=DIR` | server data files; default `.` |
| `--game=DIR` | the Tree of Savior install, for its `.ipf` data |
| `--barrack-port=N` | default 2000 |
| `--zone-port=N` | default 7002 |
| `--map=N` | starting map id, default 1001 (Klaipeda) |
| `--log-json=FILE` | write a decoded packet log |
| `--no-spawns` | do not populate maps from the spawn tables |
| `--no-verify` | skip checking built packets against the extracted layouts |
| `--debug` | log every packet |

The listen ports do not have to match what the client dials. **The server routes
on the opcode, not the port** (`11`), so every port behaves identically and the
client's own configuration cannot get this wrong.

### Point the client at it

`serverlist.xml` in the repo root is already the emulator's, and matches the
ports above. Serve it the same way as for the proxy:

```bash
python -m http.server 8080
```

- `ServerListURL` → `http://127.0.0.1:8080/serverlist.xml`
- `StaticConfigURL` → `http://127.0.0.1:8080/`
- delete `<game>\release\serverlist_recent.xml`

Then start the client. Or let `devrun.py` start both and screenshot the result:

```bash
python devrun.py
```

```bash
python devrun.py --no-client        # server only
```

```bash
python devrun.py --stop             # kill whatever is still running
```

`--stop` kills `tosemu_server.exe` and the client by image name, so it also
cleans up a server you started by hand. Worth knowing: the server does not
always exit on a terminal signal, and a surviving process holds a lock on its
own binary — the next build will link fine but the copy to the repo root fails
with "Device or resource busy".

It writes timestamped screenshots and a server log with matching timestamps, so
a frame can be lined up against the packets that produced it. It cannot play
the game — get to the point you care about by hand and let the shots run.

### Chat commands

Typed into the game's chat box, once you are in the world:

| command | |
|---|---|
| `/where` | your map, position and handle |
| `/map <id\|classname>` | run the map-change handshake (`09`) |
| `/spawn <monster id\|classname> [count]` | place monsters around you, default 5 |
| `/give <item id\|classname> [count]` | default 1 |
| `/items` | list what you are carrying |
| `/skills` | list what you know |
| `/who` | who else is on the map |
| `/heal` | full HP/SP |
| `/level <1-500>` | set level |

### Testing without the client

```bash
python test_server.py
```

Drives full barrack → social → zone sessions against a running server and
checks the replies. Nothing is replayed from a capture — every client packet is
built from the opcode table and the layouts in these notes, which is the point.

---

## Part 3 — reading what came out

Each capture tool writes its own format, so each has its own reader.

```bash
python read_relay.py relay/dumps/capture_20260801_080512.bin
```

```bash
python read_relay.py relay/dumps/capture_*.bin ZC_MOVE_DIR    # hexdump one type
python read_relay.py relay/dumps/capture_*.bin 3110           # or by opcode
python read_relay.py relay/dumps/capture_*.bin --conns        # per-connection table
```

`read_capture.py` takes the same arguments and reads the `TOSCAP` files written
by `tos_capture.exe`:

```bash
python read_capture.py capture_1785545696.bin ZC_ENTER_MONSTER
```

Both print a per-opcode summary when given no packet name. `checksum_ok` in the
summary is the correctness check that matters: if the Blowfish key or the init
table were wrong, client→server records would decode with `chk=BAD`.

A dump ends with a trailer written on clean shutdown. If it is missing the
capture was cut short — the reader says so and reads it anyway.

### Opcodes the table does not know

`packet_opcodes.csv` came from one client build, and a newer client sends
things that are not in it — usually exactly the packets you are capturing to
find. The relay keeps them rather than losing framing, and prints candidate
rows on the way out:

```
opcode_dec,opcode_hex,name,size   <- sizes are observed, not declared
  60000,0xEA60,UNKNOWN_60000,24   (1 seen, server->client, len 24..24)
```

Observed sizes are good enough to add a row and verify, not good enough to
trust.

---

## Which config file is which

Four files with similar names, and mixing them up is the most common way to
lose an afternoon:

| file | for | ports |
|---|---|---|
| `serverlist.xml` | the emulator | 2000 / 7002 |
| `serverlist_proxy.xml` | `tos_proxy.py` | 7001 / 7002 |
| `static__Config.txt` | both, served over HTTP | — |
| `www/*_custom.*` | hand-tuned overrides for `tos_relay.exe --www` | — |

`tos_relay.exe` needs none of them: it generates its server list from its own
listen ports, and that generated copy deliberately **wins over** any file in
`--www`, so a stale list cannot break the client. `static__Config.txt` in
`--www` does win over the generated one, if you want to hand-tune it — but it
has to be named exactly that, which the shipped `_custom` copies are not.

## When nothing happens

The client almost never reports an error; it sits still or retries forever, and
the symptom shows up nowhere near the cause. The table in the
[docs README](README.md) maps what you see to what is actually missing, and
`13` walks the whole session in order.

The one diagnostic that settles the most common case fastest — check the server
log for inbound `CZ_KEYBOARD_MOVE`:

- **arriving** → the client is fine and our echo is wrong or missing
- **not arriving** → a genuine client-side input lock, a different hunt entirely
