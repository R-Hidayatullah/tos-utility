"""Loopback capture stub for Tree of Savior client.

Listens on the barrack (7001) and zone (7002) ports, accepts whatever the
client connects with, and hex-dumps every byte received. Never replies --
the goal is to observe the real wire format of CB_LOGIN, including the
header bytes the builder leaves for the transport layer to fill in.

Cross-references the leading u16 against packet_opcodes.csv so each dump is
labelled with a packet name and its expected size.

    python capture_stub.py

Point the client here first:
    static__Config.txt : UseSteamClient=NO, UseLiveXigncode=NO
    client.xml         : ServerListURL -> a local serverlist.xml
    serverlist.xml     : Server0_IP="127.0.0.1" Server0_Port="7001"
    Client_tos_x64.exe -SERVICE -ID test -PW test
"""

import csv
import os
import socket
import struct
import sys
import threading
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "packet_opcodes.csv")
PORTS = [7001, 7002]


def load_table():
    """opcode -> (name, declared_size); size 0 means variable-length."""
    table = {}
    if not os.path.exists(TABLE):
        print("[!] %s not found -- dumps will be unlabelled" % TABLE)
        return table
    with open(TABLE, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            table[int(row["opcode_dec"])] = (row["name"], int(row["size"]))
    return table


OPCODES = load_table()


def hexdump(data, indent="    "):
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join("%02X" % b for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append("%s%04X  %-47s  |%s|" % (indent, off, hexpart, asciipart))
    return "\n".join(lines)


def describe(data):
    """Label the buffer using the leading u16, if it looks like an opcode."""
    if len(data) < 2:
        return "(too short to hold an opcode)"
    op = struct.unpack_from("<H", data, 0)[0]
    if op not in OPCODES:
        return "opcode 0x%04X (%d) -- not in table; header may be offset" % (op, op)
    name, size = OPCODES[op]
    if size == 0:
        return "opcode 0x%04X %s  variable-length, got=%d" % (op, name, len(data))
    verdict = "MATCH" if size == len(data) else "MISMATCH"
    return "opcode 0x%04X %s  expect=%d got=%d  [%s]" % (
        op, name, size, len(data), verdict)


def handle(conn, addr, port):
    tag = "%s:%d" % addr
    print("\n[+] %d <- connection from %s" % (port, tag))
    total = 0
    try:
        while True:
            data = conn.recv(65535)
            if not data:
                break
            total += len(data)
            stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            print("\n[%s] port %d  %s  %d bytes (session total %d)"
                  % (stamp, port, tag, len(data), total))
            print("    %s" % describe(data))
            print(hexdump(data))
            sys.stdout.flush()
    except ConnectionResetError:
        print("[-] %d  %s reset the connection" % (port, tag))
    finally:
        conn.close()
        print("[-] %d  %s closed after %d bytes" % (port, tag, total))


def listen(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", port))
    except OSError as exc:
        print("[!] cannot bind port %d: %s" % (port, exc))
        return
    srv.listen(8)
    print("[*] listening on 0.0.0.0:%d" % port)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr, port),
                         daemon=True).start()


def main():
    print("[*] loaded %d opcodes from packet_opcodes.csv" % len(OPCODES))
    for port in PORTS:
        threading.Thread(target=listen, args=(port,), daemon=True).start()
    print("[*] capturing -- Ctrl+C to stop")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        print("\n[*] stopped")


if __name__ == "__main__":
    main()
