"""Tree of Savior emulator -- socket loop and hot-reload driver.

Framing and crypto live in tos_proto.py; packet logic lives in handlers.py,
which is reloaded before every inbound packet. Edit handlers.py with the
client still connected and the change applies to the next packet -- no server
restart, no 90-second client relaunch.

    python tosemu_server.py
"""

import importlib
import socket
import struct
import sys
import threading
import traceback
from datetime import datetime

import handlers
import tos_proto as P

PORTS = [7001, 7002]
_reload_lock = threading.Lock()
_last_good = None          # last handlers module that imported cleanly


def reload_handlers(log):
    """Reload handlers.py; on failure keep serving the previous version."""
    global _last_good
    with _reload_lock:
        try:
            importlib.reload(handlers)
            _last_good = handlers
        except Exception:
            log("  !! handlers.py failed to reload -- using previous version")
            for line in traceback.format_exc().splitlines():
                log("     " + line)
        return _last_good or handlers


class Ctx:
    """Per-connection state. Survives handler reloads."""

    def __init__(self, conn, addr, port):
        self.conn, self.addr, self.port = conn, addr, port
        self.seq = 0
        self.account_id = 0x0000000100000001
        self.team_name = b"tosemu"
        self.framing = None    # "enc" (blowfish) or "plain" (chat/social)
        self.state = {}        # scratch for hot-added handler state

    def log(self, msg):
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print("[%s] %d %s:%d  %s"
              % (stamp, self.port, self.addr[0], self.addr[1], msg))
        sys.stdout.flush()

    def send(self, payload):
        buf = P.stamp(payload, self.seq)
        self.seq = (self.seq + 1) & P.M32
        op = struct.unpack_from("<H", buf, 0)[0]
        self.log("SEND %-26s op=%-5d size=%d" % (P.name_of(op), op, len(buf)))
        self.conn.sendall(P.seal(buf))

    def send_raw(self, payload, tag="", quiet=False):
        """Send bytes untouched -- no sequence, no checksum restamp.

        Captured packets carry sequence 0xFFFFFFFF and checksum 0, which is
        exactly what the live server sends and the client accepts. Restamping
        them would only make us differ from the reference.
        """
        if not quiet:
            op = struct.unpack_from("<H", payload, 0)[0]
            self.log("RPLY %-26s op=%-5d size=%d %s"
                     % (P.name_of(op), op, len(payload), tag))
        self.conn.sendall(payload)


def dispatch(ctx, plain):
    op = struct.unpack_from("<H", plain, 0)[0]
    seq = struct.unpack_from("<I", plain, 2)[0]
    size = P.packet_size(plain)
    ok = P.verify(plain, size)
    ctx.log("RECV %-26s op=%-5d seq=%-4d size=%s chk=%s"
            % (P.name_of(op), op, seq, size,
               {True: "ok", False: "BAD", None: "?"}[ok]))
    mod = reload_handlers(ctx.log)
    try:
        mod.handle(ctx, op, plain)
    except Exception:
        ctx.log("  !! handler raised")
        for line in traceback.format_exc().splitlines():
            ctx.log("     " + line)


def serve(conn, addr, port):
    ctx = Ctx(conn, addr, port)
    ctx.log("connected")
    buf = bytearray()
    try:
        while True:
            chunk = conn.recv(65535)
            if not chunk:
                break
            buf += chunk

            # Framing is per-connection, not global. Barrack and zone links
            # are Blowfish-framed (leading u16 is a padded length, always a
            # multiple of 8). The chat/social link is plaintext both ways, so
            # its leading u16 is the opcode itself (CS_LOGIN = 15901).
            if ctx.framing is None and len(buf) >= 2:
                lead = struct.unpack_from("<H", buf, 0)[0]
                ctx.framing = "enc" if (lead and lead % 8 == 0) else "plain"
                ctx.log("framing=%s (lead=%d)" % (ctx.framing, lead))

            if ctx.framing == "plain":
                while len(buf) >= 10:
                    size = P.packet_size(bytes(buf[:12]))
                    if not size or size < 10 or size > 0x8000:
                        op = struct.unpack_from("<H", buf, 0)[0]
                        ctx.log("plain: cannot size op=%d -- dropping" % op)
                        return
                    if len(buf) < size:
                        break
                    plain = bytes(buf[:size])
                    del buf[:size]
                    dispatch(ctx, plain)
                continue

            while len(buf) >= 2:
                padded = struct.unpack_from("<H", buf, 0)[0]
                if padded == 0 or padded % 8:
                    ctx.log("bad frame length %d -- dropping" % padded)
                    return
                if len(buf) < 2 + padded:
                    break
                plain = P.unseal(bytes(buf[2:2 + padded]))
                del buf[:2 + padded]
                dispatch(ctx, plain)
    except ConnectionResetError:
        ctx.log("reset")
    except Exception:
        traceback.print_exc()
    finally:
        conn.close()
        ctx.log("closed")


def listen(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(8)
    print("[*] listening on 0.0.0.0:%d" % port)
    while True:
        c, a = srv.accept()
        threading.Thread(target=serve, args=(c, a, port), daemon=True).start()


def main():
    global _last_good
    _last_good = handlers
    print("[*] %d opcodes, blowfish key %r" % (len(P.SIZES), P.KEY.decode()))
    print("[*] handlers.py is hot-reloaded per packet -- edit it live")
    for p in PORTS:
        threading.Thread(target=listen, args=(p,), daemon=True).start()
    print("[*] ready -- Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        print("\n[*] stopped")


if __name__ == "__main__":
    main()
