# tos_relay

Patch the client's config, proxy it, dump every packet, put the files back.

The client is pointed at this process by editing files inside the game
install. Those edits are the only thing here that can outlive the process, so
they are the thing the design is built around: **the relay never leaves the
game patched, even after a hard kill.**

## Build

```bash
cmake -S cpp/relay -B cpp/relay/build -G Ninja && cmake --build cpp/relay/build
```

or, without CMake:

```bash
g++ -std=c++17 -O2 -o tos_relay.exe cpp/relay/*.cpp -lws2_32 -static
```

## Run

From the directory holding `bf_inittable.bin` and `packet_opcodes.csv`:

```bash
tos_relay.exe --region=asia
```

Editing files under `Program Files` needs administrator rights. Without them
the relay still runs and still captures — it says so and leaves the game's
files alone.

Stop it with Ctrl+C. If it was killed instead, put the files back with:

```bash
tos_relay.exe --restore
```

The next normal run does this by itself; `--restore` is for when you are not
starting one.

| option | |
|---|---|
| `--region=asia\|na` | upstream preset |
| `--upstream0=ip:port`, `--upstream1=ip:port` | explicit upstreams (overrides the preset) |
| `--listen0=N`, `--listen1=N` | local ports, default 7001 / 7002 |
| `--http=N` | local HTTP port, default 8080, `0` disables |
| `--relay-base=N` | first port handed to zone relays, default 17001 |
| `--game=DIR` | game install to patch |
| `--data=DIR` | where `bf_inittable.bin` and `packet_opcodes.csv` live |
| `--out=DIR` | dumps, logs, backups; default `DATA\relay` |
| `--www=DIR` | extra files to serve over HTTP; default `DATA\www` |
| `--steam=auto\|yes\|no` | Steam login in the config we serve; `auto` follows the install |
| `--no-patch` | leave the game's files alone |
| `--no-redirect` | do not rewrite zone addresses |
| `--restore` | restore patched files and exit |

## What it touches

- `<game>\release\client.xml` — `ServerListURL` and `StaticConfigURL` point at
  the relay's own HTTP server. Every other byte of the file is left as it was,
  so the restore is byte-for-byte.
- `<game>\release\serverlist_recent.xml` — the client caches the last server
  list it used and will dial those addresses directly, so this one has to go
  too.

Both are backed up to `<out>\backup\` first.

## Steam login

The client does not only read `client.xml` — it fetches `static__Config.txt`
from `StaticConfigURL`, and **that** file carries `UseSteamClient`. Repointing
the URL at the relay and then serving nothing for it drops the client back to
its defaults, and a Steam player is suddenly asked for a username and password
they do not have.

So the relay serves that file, built from the install's own `client.xml`:
`ServiceNation`, `Dictionary`, the Nexon and anti-cheat flags, and
`UseSteamClient` are read out and handed straight back. It logs which login
path the client will take:

```
[INF] login: Steam (static__Config.txt UseSteamClient=YES, from client.xml)
```

`--steam=yes` or `--steam=no` overrides it. A file of the same name in `--www`
wins over the generated one, if you want to hand-tune it.

## Opcodes that are not in the table

`packet_opcodes.csv` comes from one client build. A newer client sends things
that are not in it, and those are usually the packets you are capturing to
find.

**Client→server** costs nothing: the frame carries its own length, so an
unknown packet is captured whole regardless. Only its declared size is
unavailable.

**Server→client** has no framing of its own — the length comes from the table —
so an unknown opcode used to end decoding for the rest of the connection. Now
the relay:

1. keeps forwarding immediately and unconditionally, so the client never waits
   on our decoder;
2. stores the bytes it cannot frame as `UNFRAMED` records, so nothing is lost;
3. scans forward for the next offset that starts a packet the table knows *and*
   is followed by another one it knows — a lone plausible opcode appears in
   payload data constantly, and resyncing onto one would produce confident
   nonsense;
4. resumes normal framing there.

When the resync lands cleanly, the gap length **is** the unknown packet's
length, and it is recorded as `len-inferred`. On the way out the relay prints
the candidates:

```
1 opcode(s) not in packet_opcodes.csv; 1 unframed record(s), 24 B captured
opcode_dec,opcode_hex,name,size   <- candidate rows, sizes are observed, not declared
  60000,0xEA60,UNKNOWN_60000,24   (1 seen, server->client, len 24..24)
```

Those sizes are observed, not declared — good enough to add a row and verify,
not good enough to trust. The one thing this costs: after framing is lost,
zone-address rewriting stops for that connection, because the bytes are already
on their way to the client. It is logged when it happens.

## How the restore survives a kill

`<out>\patch_journal.tsv` is written **before** the first edit and deleted as
the **last** step of a successful restore, so its presence always means "files
are still patched". Every exit path — Ctrl+C, console close, `SIGTERM`, an
unhandled exception — runs the same restore, and a run that starts on top of a
journal recovers it before doing anything else.

If a restore genuinely cannot happen (the file is locked, or a permission was
lost mid-run) the journal is kept, the log says which files are still patched
and where their backups are, and the next run retries.

## Output

```
<out>\dumps\capture_<YYYYmmdd_HHMMSS>.bin
<out>\logs\relay_<YYYYmmdd_HHMMSS>.log
<out>\backup\<file>.<YYYYmmdd_HHMMSS>.orig
<out>\patch_journal.tsv          (only while files are patched)
```

Read a dump with `python read_relay.py <file.bin>` — summary by opcode,
`--conns` for a per-connection table, or a packet name/opcode to hexdump it.
Each record carries wall-clock time, monotonic time, both IPs, both ports, the
listen port, connection id, opcode, packet name, declared size, sequence,
checksum and its verification, the link, and the full plaintext packet.

A dump ends with a trailer written on clean shutdown. If the trailer is
missing, the capture was cut short — the reader says so and reads it anyway.

## Threads

One thread accepts per port; each connection gets one per direction, so a slow
upstream one way cannot stall the other. Client→server bytes are relayed the
instant they arrive and decoded from a copy, so the capture can never add
latency to the client's input. Server→client bytes have to be decoded before
forwarding, because the zone address the barrack hands out is rewritten to
point back here. Socket threads never touch the disk: records go into a
staging buffer that one writer thread drains.
