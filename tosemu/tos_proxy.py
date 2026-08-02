"""Transparent MITM proxy for capturing live Tree of Savior traffic.

Relays bytes to the real server completely unmodified, while decoding a copy
of each direction using the protocol we reversed:

    client -> server : u16 padded_len | Blowfish-ECB(payload)   -> decrypted
    server -> client : raw packets, framed by the packet-size table

No hooking, no injection -- the client just connects to us because we control
serverlist.xml. Everything the live server sends is plaintext on the wire, and
we hold the Blowfish key for the other direction, so the capture is fully
readable offline.

Point the client here first. Serve this directory over HTTP and use
`serverlist_proxy.xml` -- NOT `serverlist.xml`, which is the emulator's and
points at different ports:

    python -m http.server 8080

    client.xml     : ServerListURL   -> http://127.0.0.1:8080/serverlist_proxy.xml
                     StaticConfigURL -> http://127.0.0.1:8080/   (UseSteamClient=YES)

    python tos_proxy.py asia     (or: na)

`cpp/relay/tos_relay.exe` does all of this for you, including putting the
client's files back afterwards. Prefer it; see docs/15.

Writes capture/live_<ts>.jsonl (one decoded packet per line, hex body) plus
raw_<ts>_<dir>.bin streams for anything that needs re-parsing later.
"""

import json
import os
import socket
import struct
import sys
import threading
import time
import traceback
from datetime import datetime

import tos_proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
CAPDIR = os.path.join(HERE, "capture")

# local port -> (real host, real port), matching serverlist Server0/Server1
UPSTREAMS = {
    "na":   {7001: ("52.5.58.238", 7001),    7002: ("52.4.126.228", 7002)},
    "asia": {7001: ("54.180.190.119", 7001), 7002: ("54.180.208.135", 7002)},
}

_lock = threading.Lock()
_jsonl = None
_seen = {}          # (direction, opcode) -> count


def log(msg):
    print("[%s] %s" % (datetime.now().strftime("%H:%M:%S.%f")[:-3], msg))
    sys.stdout.flush()


def record(direction, port, op, seq, size, body):
    """Append one decoded packet to the JSONL capture."""
    key = (direction, op)
    with _lock:
        first = key not in _seen
        _seen[key] = _seen.get(key, 0) + 1
        _jsonl.write(json.dumps({
            "t": time.time(), "dir": direction, "port": port,
            "op": op, "name": P.name_of(op), "seq": seq, "size": size,
            "hex": body.hex(),
        }) + "\n")
        _jsonl.flush()
    if first:
        log("%s %-30s op=%-5d size=%-5s  [first]"
            % (direction, P.name_of(op), op, size))


def split_c2s(buf, port):
    """Client->server: u16 padded length, then that many encrypted bytes."""
    while len(buf) >= 2:
        padded = struct.unpack_from("<H", buf, 0)[0]
        if padded == 0 or padded % 8 or padded > 0x8000:
            log("c2s: bad frame len %d -- resyncing off" % padded)
            del buf[:2]
            continue
        if len(buf) < 2 + padded:
            return
        plain = P.unseal(bytes(buf[2:2 + padded]))
        del buf[:2 + padded]
        op = struct.unpack_from("<H", plain, 0)[0]
        seq = struct.unpack_from("<I", plain, 2)[0]
        size = P.packet_size(plain)
        record("c2s", port, op, seq, size, plain[:size or len(plain)])


def split_s2c(buf, port):
    """Server->client: raw packets, length from the size table or +0x0A."""
    while len(buf) >= 10:
        size = P.packet_size(bytes(buf[:12]))
        if not size or size < 10 or size > 0x8000:
            op = struct.unpack_from("<H", buf, 0)[0]
            log("s2c: unknown/invalid size for op=%d (%s) -- resyncing"
                % (op, size))
            del buf[:2]
            continue
        if len(buf) < size:
            return
        pkt = bytes(buf[:size])
        del buf[:size]
        op = struct.unpack_from("<H", pkt, 0)[0]
        seq = struct.unpack_from("<I", pkt, 2)[0]
        record("s2c", port, op, seq, size, pkt)


def pump(src, dst, direction, port, splitter, rawfile):
    buf = bytearray()
    try:
        while True:
            chunk = src.recv(65535)
            if not chunk:
                break
            dst.sendall(chunk)          # relay untouched, first and always
            rawfile.write(chunk)
            rawfile.flush()
            buf += chunk
            try:
                splitter(buf, port)
            except Exception:
                log("%s decode error:\n%s" % (direction, traceback.format_exc()))
                buf.clear()
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def serve(client, addr, port, upstream):
    log("client %s:%d -> upstream %s:%d" % (addr[0], addr[1], *upstream))
    try:
        up = socket.create_connection(upstream, timeout=15)
    except OSError as exc:
        log("upstream connect failed: %s" % exc)
        client.close()
        return
    ts = datetime.now().strftime("%H%M%S")
    fc = open(os.path.join(CAPDIR, "raw_%s_%d_c2s.bin" % (ts, port)), "wb")
    fs = open(os.path.join(CAPDIR, "raw_%s_%d_s2c.bin" % (ts, port)), "wb")
    t1 = threading.Thread(target=pump,
                          args=(client, up, "c2s", port, split_c2s, fc),
                          daemon=True)
    t2 = threading.Thread(target=pump,
                          args=(up, client, "s2c", port, split_s2c, fs),
                          daemon=True)
    t1.start(); t2.start(); t1.join(); t2.join()
    fc.close(); fs.close()
    client.close(); up.close()
    log("closed %s:%d" % addr)


def listen(port, upstream):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(8)
    log("listening 0.0.0.0:%d -> %s:%d" % (port, *upstream))
    while True:
        c, a = srv.accept()
        threading.Thread(target=serve, args=(c, a, port, upstream),
                         daemon=True).start()


def main():
    global _jsonl
    region = sys.argv[1] if len(sys.argv) > 1 else "asia"
    if region not in UPSTREAMS:
        sys.exit("usage: python tos_proxy.py [%s]" % "|".join(UPSTREAMS))
    os.makedirs(CAPDIR, exist_ok=True)
    path = os.path.join(CAPDIR, "live_%s.jsonl"
                        % datetime.now().strftime("%Y%m%d_%H%M%S"))
    _jsonl = open(path, "w", encoding="utf-8")
    log("region=%s  capture=%s" % (region, path))
    for port, up in UPSTREAMS[region].items():
        threading.Thread(target=listen, args=(port, up), daemon=True).start()
    log("proxying -- Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        log("stopped; %d distinct packet types seen" % len(_seen))
        _jsonl.close()


if __name__ == "__main__":
    main()
