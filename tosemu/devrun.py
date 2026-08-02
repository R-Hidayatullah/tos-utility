#!/usr/bin/env python3
"""Start the server and the game client together, and screenshot the result.

    python devrun.py                 # server + client, shots every 15s for 3min
    python devrun.py --shots 12 --every 10
    python devrun.py --no-client     # server only
    python devrun.py --stop          # kill whatever is still running

Why this exists: the interesting failures only show up against the real client,
and reading them means correlating what the client is drawing with what the
server just sent. Doing that by hand -- start server, start client, alt-tab,
screenshot, scroll the log -- loses the timing. This starts both, captures the
screen on a timer, and writes a server log with the same timestamps, so a shot
can be lined up against the packets that produced it.

What it cannot do: play the game. The login and character screens need real
input. Get to the point you care about and let the shots run.

The client requires administrator rights, so launching it raises a UAC prompt.
If that is in the way, start the client yourself and use --no-client -- the
screenshots and the log line up just the same.
"""

import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(ROOT, "tosemu_server.exe")
GAME = r"C:/Program Files (x86)/Steam/steamapps/common/TreeOfSavior"
CLIENT = os.path.join(GAME, "release", "Client_tos_x64.exe")

SHOT_PS = r"""
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save('{path}', [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"""


def screenshot(path):
    subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command",
         SHOT_PS.replace("{path}", path.replace("\\", "\\\\"))],
        capture_output=True)
    return os.path.exists(path)


def kill(name):
    subprocess.run(["taskkill", "/F", "/IM", name],
                   capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "devrun"),
                    help="where shots and the log go")
    ap.add_argument("--shots", type=int, default=12)
    ap.add_argument("--every", type=float, default=15.0)
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--no-client", action="store_true")
    ap.add_argument("--no-server", action="store_true")
    ap.add_argument("--stop", action="store_true")
    ap.add_argument("--debug", action="store_true", default=True)
    args = ap.parse_args()

    if args.stop:
        kill("tosemu_server.exe")
        kill("Client_tos_x64.exe")
        kill("Client_tos.exe")
        print("stopped")
        return 0

    os.makedirs(args.out, exist_ok=True)
    log_path = os.path.join(args.out, "server.log")

    # Always start from a clean slate: a stale server still holds the ports,
    # and a stale client would be screenshotted instead of the new one.
    kill("tosemu_server.exe")
    kill("Client_tos_x64.exe")
    kill("Client_tos.exe")
    time.sleep(1)

    server = None
    if not args.no_server:
        cmd = [SERVER, "--data=" + ROOT, "--game=" + args.game]
        if args.debug:
            cmd.append("--debug")
        log = open(log_path, "w", encoding="utf-8", errors="replace")
        server = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                  cwd=ROOT)
        print("server pid %d -> %s" % (server.pid, log_path))

        # Indexing the client's archives takes a moment; wait for the server
        # to say it is listening rather than guessing at a sleep.
        for _ in range(60):
            time.sleep(1)
            try:
                with open(log_path, encoding="utf-8", errors="replace") as f:
                    if "listening on" in f.read():
                        break
            except OSError:
                pass
        print("server ready")

    if not args.no_client:
        if not os.path.exists(CLIENT):
            print("client not found: " + CLIENT)
        else:
            # -SERVICE lets the client start from the game directory without
            # the launcher. It needs administrator rights (BlackCipher), so a
            # plain spawn fails with WinError 740 and we go through
            # Start-Process -Verb RunAs, which raises the UAC prompt.
            try:
                subprocess.Popen([CLIENT, "-SERVICE"],
                                 cwd=os.path.dirname(CLIENT))
                print("client started")
            except OSError as e:
                if getattr(e, "winerror", None) != 740:
                    raise
                print("client needs elevation; requesting it (accept the UAC "
                      "prompt)")
                subprocess.run(
                    ["powershell", "-NoProfile", "-Command",
                     "Start-Process -Verb RunAs -FilePath '%s' "
                     "-ArgumentList '-SERVICE' -WorkingDirectory '%s'"
                     % (CLIENT, os.path.dirname(CLIENT))],
                    capture_output=True)

    for i in range(args.shots):
        time.sleep(args.every)
        path = os.path.join(args.out, "shot%02d.png" % i)
        ok = screenshot(path)
        print("[%5.1fs] %s%s" % ((i + 1) * args.every, path,
                                 "" if ok else "  (FAILED)"))

    print("\ndone -- shots in %s, log in %s" % (args.out, log_path))
    print("stop everything with:  python devrun.py --stop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
