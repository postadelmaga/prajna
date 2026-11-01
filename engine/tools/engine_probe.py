#!/usr/bin/env python3
"""engine_probe.py — drive chatai-webwpe without Qt, for testing.

Spawns the engine on a socketpair (fd 3) + shm file, loads a URL, prints the
events it emits, optionally runs a small script of actions, and saves the
last frame as PNG.

    engine_probe.py <engine> <url> [--out frame.png] [--seconds 8]
                    [--size 900x700] [--scale 100] [--js "expr"] ...
"""
import argparse
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HDR = struct.Struct("<IB3x")
MAX_W, MAX_H = 3840, 2400
SLOT = MAX_W * MAX_H * 4
SHM_SIZE = 16 + 2 * SLOT

EV = {64: "TITLE", 65: "URL", 66: "READY", 67: "TAB", 68: "TAB_CLOSED", 69: "CLIPBOARD",
      70: "JS_RESULT", 96: "FRAME", 97: "CURSOR", 98: "FILE_CHOOSER", 99: "PERMISSION",
      100: "NOTIFICATION", 101: "LOAD_STATE", 102: "LINK_HOVER", 103: "CONTEXT_MENU",
      104: "OPEN_EXTERNAL", 105: "DOWNLOAD", 106: "FIND_RESULT", 107: "CLIPBOARD_SET",
      108: "CRASHED"}


def png(path, w, h, bgra):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = bgra[y * w * 4:(y + 1) * w * 4]
        for x in range(0, len(row), 4):
            raw += bytes((row[x + 2], row[x + 1], row[x], 255))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


class Probe:
    def __init__(self, engine, url, w, h, scale, cache, extra=()):
        self.shm_path = tempfile.mktemp(prefix="chatai-probe-", dir="/dev/shm")
        with open(self.shm_path, "wb") as f:
            f.truncate(SHM_SIZE)
        self.parent, child = socket.socketpair()
        self.proc = subprocess.Popen(
            [engine, f"--zurf-control-fd={child.fileno()}", f"--zurf-shm={self.shm_path}",
             f"--zurf-cache={cache}", f"--zurf-url={url}", f"--zurf-w={w}",
             f"--zurf-h={h}", f"--zurf-scale={scale}", "--zurf-frame-events", *extra],
            pass_fds=(child.fileno(),))
        child.close()
        self.buf = b""
        self.frames = 0
        self.js_results = {}
        self.events = []

    def send(self, op, payload=b""):
        self.parent.sendall(HDR.pack(len(payload), op) + payload)

    def ints(self, op, *vals, tail=b""):
        self.send(op, struct.pack("<%di" % len(vals), *vals) + tail)

    def pump(self, seconds, quiet=False):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.parent], [], [], max(0, end - time.time()))
            if not r:
                continue
            data = self.parent.recv(1 << 20)
            if not data:
                print("engine closed the socket")
                return False
            self.buf += data
            while len(self.buf) >= 8:
                n, op = HDR.unpack_from(self.buf)
                if len(self.buf) < 8 + n:
                    break
                pl = self.buf[8:8 + n]
                self.buf = self.buf[8 + n:]
                self.on_event(op, pl, quiet)
        return True

    def on_event(self, op, pl, quiet):
        name = EV.get(op, str(op))
        self.events.append((name, pl))
        if op == 96:
            self.frames += 1
            return
        if op == 70:
            rid = struct.unpack_from("<i", pl)[0]
            self.js_results[rid] = pl[4:].decode(errors="replace")
        if quiet and op in (101, 102, 97):
            return
        if op in (64, 65, 67, 68):
            desc = pl[4:].decode(errors="replace") if op in (64, 65) else ""
            print(f"  {name} tab={struct.unpack_from('<i', pl)[0]} {desc[:100]}")
        elif op == 101:
            print(f"  {name} {struct.unpack('<4i', pl[:16])}")
        elif op in (98, 99, 103, 105, 106):
            print(f"  {name} {pl[:120]!r}")
        else:
            print(f"  {name} {pl[:120].decode(errors='replace')!r}")

    def eval(self, js, rid=[100]):
        rid[0] += 1
        self.ints(17, rid[0], tail=js.encode())
        end = time.time() + 5
        while rid[0] not in self.js_results and time.time() < end:
            self.pump(0.1, quiet=True)
        return self.js_results.get(rid[0])

    def frame(self):
        with open(self.shm_path, "rb") as f:
            hdr = f.read(16)
            seq, w, h, _ = struct.unpack("<4I", hdr)
            slot = (seq // 2) % 2
            f.seek(16 + slot * SLOT)
            return w, h, f.read(w * h * 4)

    def close(self):
        try:
            self.send(12)
            self.proc.wait(5)
        except Exception:
            self.proc.kill()
        os.unlink(self.shm_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("url")
    ap.add_argument("--out", default="frame.png")
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--size", default="900x700")
    ap.add_argument("--scale", type=int, default=100)
    ap.add_argument("--cache", default=os.path.join(tempfile.gettempdir(), "chatai-probe-cache"))
    ap.add_argument("--js", action="append", default=[])
    a = ap.parse_args()
    w, h = map(int, a.size.split("x"))
    p = Probe(a.engine, a.url, w, h, a.scale, a.cache)
    t0 = time.time()
    try:
        p.pump(a.seconds)
        for js in a.js:
            print(f"js {js!r} -> {p.eval(js)!r}")
        fw, fh, px = p.frame()
        print(f"frames={p.frames} last={fw}x{fh} in {time.time() - t0:.1f}s")
        if fw and fh:
            png(a.out, fw, fh, px)
            print(f"saved {a.out}")
    finally:
        p.close()


if __name__ == "__main__":
    sys.exit(main())
