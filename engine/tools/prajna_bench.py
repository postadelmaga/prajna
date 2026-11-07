#!/usr/bin/env python3
"""prajna_bench.py — repeatable headless benchmark of prajna-engine.

Drives the engine like the plasmoid does (engine_probe.Probe: socketpair +
shm, real input ops) through a fixed list of scenarios and reports, per
scenario: CPU per process and per web-process thread, input->frame latency
percentiles, frame rate and pacing, bytes moved per second, RSS, page
main-thread lag and clear_background.js statistics.

    prajna_bench.py --engine build/bin/prajna-engine [--gpu] [--scale 200]
        [--fps 30] [--transparent 1] [--runs 3] [--url chatgpt|<url>]
        [--scenarios static,type12,...] [--json out.json] [--env K=V ...]
    prajna_bench.py --engine ... --long 1800 --json long.json

Default page: bench_page.html (synthetic, offline, deterministic). Results
are HEADLESS: plasmashell (copy + upload) and KWin (composition, blur) are
not part of them; see bench_desktop.py for those.
"""
import argparse
import glob
import json
import os
import statistics
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from engine_probe import Probe  # noqa: E402

CLK = os.sysconf("SC_CLK_TCK")
OP_MOUSE_BUTTON, OP_EVAL_RET, OP_SET_VISIBLE, OP_KEY_NATIVE = 7, 17, 19, 32
OP_SET_PREFS, OP_FOCUS, OP_SCROLL = 38, 40, 43
EV_FRAME = 96
KEYS = [(ord(c), kc) for c, kc in zip("asdfghjkl", (38, 39, 40, 41, 42, 43, 44, 45, 46))]

# Helpers for pages that aren't bench_page.html (e.g. chatgpt.com).
HELPERS = r"""(() => {
if (!window.benchLag) {
  window.benchLag = {max: 0, sum: 0, n: 0, over50: 0};
  let last = performance.now();
  setInterval(() => { const now = performance.now(), late = now - last - 50; last = now;
    const L = window.benchLag; if (late > L.max) L.max = late; L.sum += Math.max(0, late); L.n++;
    if (late > 50) L.over50++; }, 50);
}
if (!window.benchBuild) {
  const root = () => document.querySelector('main') || document.body;
  window.benchBuild = (n) => {
    document.querySelectorAll('.prajna-bench').forEach(e => e.remove());
    const blk = '<div><div class="md"><p>Lorem ipsum dolor sit amet consectetur <strong>x</strong></p>' +
      '<ul><li>a <code>x</code></li><li>b</li></ul><pre><code>let x = 1;</code></pre>' +
      '<div><div><span>t</span></div></div></div></div>';
    const d = document.createElement('div'); d.className = 'prajna-bench';
    d.innerHTML = blk.repeat(Math.max(0, n) * 4); root().prepend(d);
    return document.getElementsByTagName('*').length; };
}
if (!window.benchStream) {
  let t = 0;
  window.benchStream = (cps) => { clearInterval(t); if (!cps) return;
    const m = document.createElement('div'); m.className = 'prajna-bench';
    m.innerHTML = '<div class="md"><p></p></div>';
    (document.querySelector('main') || document.body).appendChild(m);
    let p = m.firstChild.firstChild, w = 0; const per = Math.max(1, Math.round(cps / 20));
    t = setInterval(() => { for (let k = 0; k < per; k++) { p.append(' word' + (w++));
      if (w % 40 === 0) { p = document.createElement('p'); m.firstChild.appendChild(p); } } }, 50); };
}
return 'ok';
})()"""

COMPOSER = ("(()=>{const e=document.querySelector('#prompt-textarea')||[...document.querySelectorAll("
            "'[contenteditable=true],textarea')].find(x=>x.offsetParent);if(!e)return 'null';"
            "const b=e.closest('form')?.getBoundingClientRect()||e.getBoundingClientRect();"
            "const c=e.getBoundingClientRect();return JSON.stringify([Math.round(c.x+c.width/2),"
            "Math.round(c.y+c.height/2),b.x,b.y,b.width,b.height])})()")


def pct(vals, p):
    if not vals:
        return None
    s = sorted(vals)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]


def proc_tree(root):
    out = {root}
    changed = True
    while changed:
        changed = False
        for st in glob.glob("/proc/[0-9]*/stat"):
            try:
                pid = int(st.split("/")[2])
                ppid = int(open(st).read().rsplit(")", 1)[1].split()[1])
            except Exception:
                continue
            if ppid in out and pid not in out:
                out.add(pid)
                changed = True
    return sorted(out)


def comm(pid):
    try:
        return open(f"/proc/{pid}/comm").read().strip()
    except Exception:
        return "?"


def cpu_by_proc(pids):
    t = {}
    for p in pids:
        try:
            f = open(f"/proc/{p}/stat").read().rsplit(")", 1)[1].split()
            t[comm(p)] = t.get(comm(p), 0) + (int(f[11]) + int(f[12])) / CLK
        except Exception:
            pass
    return t


def cpu_by_thread(pids, only="WPEWebProcess"):
    t = {}
    for p in pids:
        if comm(p) != only:
            continue
        for st in glob.glob(f"/proc/{p}/task/*/stat"):
            try:
                raw = open(st).read()
                name = raw[raw.index("(") + 1:raw.rindex(")")]
                f = raw.rsplit(")", 1)[1].split()
                name = "main" if st.split("/")[4] == str(p) else name
                t[name] = t.get(name, 0) + (int(f[11]) + int(f[12])) / CLK
            except Exception:
                pass
    return t


def mem(pids):
    r = {}
    for p in pids:
        try:
            d = {}
            for line in open(f"/proc/{p}/status"):
                if line.startswith(("VmRSS", "RssShmem", "RssAnon")):
                    k, v = line.split(":")
                    d[k] = int(v.split()[0]) // 1024
            name = comm(p)
            acc = r.setdefault(name, {"rss": 0, "anon": 0, "shmem": 0})
            acc["rss"] += d.get("VmRSS", 0)
            acc["anon"] += d.get("RssAnon", 0)
            acc["shmem"] += d.get("RssShmem", 0)
        except Exception:
            pass
    return r


def gpu_ns(pids, only="WPEWebProcess"):
    """GPU time (ns) used by the web process, from DRM fdinfo (summed over
    engines and over the fds sharing one client id)."""
    total, seen = 0, set()
    for p in pids:
        if comm(p) != only:
            continue
        for fi in glob.glob(f"/proc/{p}/fdinfo/*"):
            try:
                txt = open(fi).read()
            except Exception:
                continue
            if "drm-client-id" not in txt:
                continue
            kv = dict(line.split(":", 1) for line in txt.splitlines() if ":" in line)
            cid = kv.get("drm-client-id", "").strip()
            if cid in seen:
                continue
            seen.add(cid)
            for k, v in kv.items():
                if k.startswith("drm-engine-") and v.strip().endswith("ns"):
                    total += int(v.split()[0])
    return total


def intersects(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


class Bench:
    def __init__(self, a, run):
        self.a = a
        url = a.url
        if url in (None, "", "synthetic"):
            url = "file://" + os.path.join(HERE, "bench_page.html") + "#n=4"
        elif url == "chatgpt":
            url = "https://chatgpt.com/"
        self.synthetic = url.startswith("file://")
        extra = (["--gpu"] if a.gpu else []) + [f"--max-fps={a.fps}"] + a.extra
        for kv in a.env:
            k, v = kv.split("=", 1)
            os.environ[k] = v
        self.cache = a.cache or tempfile.mkdtemp(prefix="prajna-bench-")
        self.p = Probe(a.engine, url, a.width, a.height, a.scale, self.cache, extra)
        self.frames = []  # (t, x, y, w, h) device px; w=-1 → full
        orig = self.p.on_event

        def on_event(op, pl, quiet):
            if op == EV_FRAME:
                d = struct.unpack("<4i", pl[:16]) if len(pl) >= 16 else (0, 0, -1, -1)
                self.frames.append((time.monotonic(),) + d)
            if op == 70:  # JS result: store without printing
                self.p.js_results[struct.unpack_from("<i", pl)[0]] = pl[4:].decode(errors="replace")
                return
            orig(op, pl, True)
        self.p.on_event = on_event
        self.dev = (a.width * a.scale // 100, a.height * a.scale // 100)

    def js(self, src):
        return self.p.eval(src)

    def start(self):
        p, a = self.p, self.a
        p.pump(1.0, quiet=True)
        p.send(OP_SET_PREFS, f"dark-mode=1\ntransparent={a.transparent}\nmax-fps={a.fps}\n".encode())
        p.ints(OP_SET_VISIBLE, 1)
        p.ints(OP_FOCUS, 1)
        t0 = time.monotonic()
        while time.monotonic() - t0 < a.load_wait:
            p.pump(0.25, quiet=True)
            if self.synthetic and self.frames and self.js("document.readyState") == "complete":
                break
        self.settle()
        self.js(HELPERS)
        self.focus_composer()

    def settle(self, quiet_s=1.5, limit_s=25):
        """Wait until the page stops producing frames (sites animate for a
        few seconds after load) for quiet_s, at most limit_s."""
        t0 = time.monotonic()
        while time.monotonic() - t0 < limit_s:
            n = len(self.frames)
            self.p.pump(quiet_s, quiet=True)
            if len(self.frames) == n:
                return

    def focus_composer(self):
        r = self.js(COMPOSER)
        try:
            cx, cy, bx, by, bw, bh = json.loads(r)
        except Exception:
            self.composer = None
            return
        s = self.a.scale / 100
        self.composer = (int(bx * s), int(by * s), int(bw * s) + 1, int(bh * s) + 1)
        self.p.ints(OP_MOUSE_BUTTON, cx, cy, 0, 1, 1, 0)
        self.p.ints(OP_MOUSE_BUTTON, cx, cy, 0, 0, 1, 0)
        self.p.pump(0.3, quiet=True)
        self.clear_composer()

    def clear_composer(self):
        """Ctrl+A, BackSpace: through the editor, so sites that keep a
        draft (ChatGPT) forget it too."""
        p = self.p
        p.ints(OP_KEY_NATIVE, 0xffe3, 37, 1, 0)
        p.ints(OP_KEY_NATIVE, ord("a"), 38, 1, 2)
        p.ints(OP_KEY_NATIVE, ord("a"), 38, 0, 2)
        p.ints(OP_KEY_NATIVE, 0xffe3, 37, 0, 0)
        p.ints(OP_KEY_NATIVE, 0xff08, 22, 1, 0)
        p.ints(OP_KEY_NATIVE, 0xff08, 22, 0, 0)
        p.pump(0.4, quiet=True)

    def build(self, n):
        nodes = self.js(f"benchBuild({n})")
        self.p.pump(2.5, quiet=True)  # let catch-up scans and reflow settle
        self.settle(limit_s=8)
        self.focus_composer()
        return int(nodes or 0)

    def measure(self, label, seconds, driver=None, **meta):
        p = self.p
        pids = proc_tree(p.proc.pid)
        self.js("(benchLag.max=0,benchLag.sum=0,benchLag.n=0,benchLag.over50=0,"
                "window.__prajnaClear&&__prajnaClear.stats&&__prajnaClear.stats.reset&&__prajnaClear.stats.reset(),1)")
        c0, th0, g0 = cpu_by_proc(pids), cpu_by_thread(pids), gpu_ns(pids)
        f0 = len(self.frames)
        t0 = time.monotonic()
        lat, misses = [], 0
        if driver:
            lat, misses = driver(seconds)
        rem = seconds - (time.monotonic() - t0)
        if rem > 0:
            p.pump(rem, quiet=True)
        dt = time.monotonic() - t0
        c1, th1, g1 = cpu_by_proc(pids), cpu_by_thread(pids), gpu_ns(pids)
        frames = self.frames[f0:]
        W, H = self.dev
        px = sum((W * H if f[3] < 0 else f[3] * f[4]) for f in frames)
        full = sum(1 for f in frames if f[3] < 0 or f[3] * f[4] >= W * H * 0.95)
        iv = [(b[0] - a[0]) * 1000 for a, b in zip(frames, frames[1:])]
        lag = json.loads(self.js("JSON.stringify(benchLag)") or "{}")
        stats = self.js("JSON.stringify(window.__prajnaClear&&__prajnaClear.stats||null)")
        r = {
            "scenario": label, **meta, "seconds": round(dt, 2),
            "cpu": {k: round(100 * (c1[k] - c0.get(k, 0)) / dt, 1) for k in c1},
            "threads": {k: round(100 * (v - th0.get(k, 0)) / dt, 1) for k, v in th1.items()
                        if v - th0.get(k, 0) > 0.01 * dt},
            "gpu_pct": round(100 * (g1 - g0) / 1e9 / dt, 1),
            "frames": len(frames), "fps": round(len(frames) / dt, 1), "full_frames": full,
            "interval_ms": {"p50": pct(iv, 50), "p90": pct(iv, 90), "max": max(iv) if iv else None},
            "damage_mb_s": round(px * 4 / dt / 1e6, 1),
            "lat_ms": {"n": len(lat), "miss": misses, "p50": pct(lat, 50), "p90": pct(lat, 90),
                       "p99": pct(lat, 99), "max": max(lat) if lat else None},
            "lag_ms": {"max": round(lag.get("max", 0), 1), "over50": lag.get("over50", 0),
                       "mean": round(lag.get("sum", 0) / max(1, lag.get("n", 1)), 2)},
            "mem": mem(pids),
            "clear": json.loads(stats) if stats and stats != "null" else None,
        }
        r["cpu"]["total"] = round(sum(v for k, v in r["cpu"].items()), 1)
        if driver and lat:
            r["typed"] = int(self.js("(()=>{const e=document.querySelector('#prompt-textarea')||"
                                     "document.activeElement;return (e.value||e.textContent||'').length})()") or 0)
        return r

    # ---- drivers ----
    def typing(self, cps):
        def drive(seconds):
            p, lat, misses = self.p, [], 0
            period = 1.0 / cps
            t_end = time.monotonic() + seconds
            i = 0
            while time.monotonic() < t_end - 0.3:
                ks, kc = KEYS[i % len(KEYS)]
                i += 1
                n0 = len(self.frames)
                t = time.monotonic()
                p.ints(OP_KEY_NATIVE, ks, kc, 1, 0)
                p.ints(OP_KEY_NATIVE, ks, kc, 0, 0)
                got = None
                deadline = t + max(period, 0.3)
                while time.monotonic() < deadline:
                    p.pump(0.002, quiet=True)
                    if got is None:
                        for f in self.frames[n0:]:
                            if f[3] < 0 or self.composer is None or intersects(f[1:], self.composer):
                                got = (f[0] - t) * 1000
                                break
                    if got is not None and time.monotonic() >= t + period:
                        break
                if got is None:
                    misses += 1
                else:
                    lat.append(got)
            return lat, misses
        return drive

    def scrolling(self, seconds):
        p = self.p
        cx, cy = self.a.width // 2, self.a.height // 3
        t_end, k = time.monotonic() + seconds, 0
        while time.monotonic() < t_end:
            dy = -240 if (k // 25) % 2 == 0 else 240  # 25 notches down, 25 up
            p.ints(OP_SCROLL, cx, cy, 0, dy * 100, 0)
            k += 1
            p.pump(1 / 60, quiet=True)
        return [], 0

    def open_close(self, cycles):
        def drive(seconds):
            p, lat, misses = self.p, [], 0
            for _ in range(cycles):
                p.ints(OP_SET_VISIBLE, 0)
                p.pump(0.35, quiet=True)
                n0 = len(self.frames)
                t = time.monotonic()
                p.ints(OP_SET_VISIBLE, 1)
                got = None
                while time.monotonic() < t + 0.6:
                    p.pump(0.002, quiet=True)
                    if got is None and len(self.frames) > n0:
                        got = (self.frames[n0][0] - t) * 1000
                (lat.append(got) if got is not None else None)
                misses += got is None
            return lat, misses
        return drive

    def frame_ok(self):
        """The visible slot holds a non-blank frame of the current size."""
        w, h, px = self.p.frame()
        if (w, h) != self.dev or not px:
            return False
        step = max(4, len(px) // 4096 // 4 * 4)
        return any(px[i:i + 4] != b"\0\0\0\0" for i in range(0, len(px), step))

    def close(self):
        self.p.close()


def run_scenarios(a, run):
    b = Bench(a, run)
    out = []
    try:
        b.start()
        want = a.scenarios.split(",")
        nodes = int(b.js("document.getElementsByTagName('*').length") or 0)

        def go(name, seconds, driver=None, **meta):
            if name in want:
                r = b.measure(name, seconds, driver, nodes=meta.pop("nodes", nodes), **meta)
                r["run"] = run
                out.append(r)
                brief(r)
                b.clear_composer()

        go("static", 6)
        go("type12", 8, b.typing(12))
        go("typefast", 6, b.typing(30))
        if "dom4k" in want or "stream" in want:
            nodes = b.build(58 if b.synthetic else 40)
            go("dom4k", 8, b.typing(12), nodes=nodes)
            b.js("benchStream(60)")
            go("stream", 8, nodes=nodes)
            b.js("benchStream(0)")
            b.p.pump(1.6, quiet=True)
        if "dom19k" in want or "scroll" in want:
            nodes = b.build(278 if b.synthetic else 200)
            go("dom19k", 8, b.typing(12), nodes=nodes)
            go("scroll", 6, b.scrolling, nodes=nodes)
        go("openclose", 1, b.open_close(10))
        if "reopen" in want:
            b.p.ints(OP_SET_VISIBLE, 0)
            r = b.measure("hidden", 6)
            r["run"] = run
            out.append(r)
            brief(r)
            n0 = len(b.frames)
            t = time.monotonic()
            b.p.ints(OP_SET_VISIBLE, 1)
            while time.monotonic() < t + 2 and len(b.frames) == n0:
                b.p.pump(0.005, quiet=True)
            first = (b.frames[n0][0] - t) * 1000 if len(b.frames) > n0 else None
            b.p.pump(0.3, quiet=True)
            r = {"scenario": "reopen", "run": run, "first_frame_ms": first, "frame_ok": b.frame_ok(),
                 "mem": mem(proc_tree(b.p.proc.pid))}
            out.append(r)
            print(f"  [reopen] first frame {first and round(first)} ms, frame ok={r['frame_ok']}")
    finally:
        b.close()
    return out


def brief(r):
    c = r["cpu"]
    th = sorted(r["threads"].items(), key=lambda kv: -kv[1])[:5]
    lat = r["lat_ms"]
    ls = (f" lat p50/p90/p99={lat['p50'] and round(lat['p50'])}/{lat['p90'] and round(lat['p90'])}/"
          f"{lat['p99'] and round(lat['p99'])} miss={lat['miss']}") if lat["n"] or lat["miss"] else ""
    print(f"  [{r['scenario']:<9}] nodes={r.get('nodes')} cpu total={c['total']} web={c.get('WPEWebProcess')} "
          f"eng={c.get('prajna-engine')} gpu={r['gpu_pct']}% fps={r['fps']} "
          f"full={r['full_frames']} dmg={r['damage_mb_s']}MB/s{ls} lag max={r['lag_ms']['max']} "
          f">50={r['lag_ms']['over50']} typed={r.get('typed')} web rss={r['mem'].get('WPEWebProcess', {}).get('rss')} | "
          + " ".join(f"{k}={v}" for k, v in th))


def long_session(a):
    """Cycles of typing, streaming, scrolling and hide/show; RSS every 30 s."""
    b = Bench(a, 0)
    samples = []
    try:
        b.start()
        b.build(58 if b.synthetic else 40)
        t_end = time.monotonic() + a.long
        t0 = time.monotonic()
        cycle = 0
        while time.monotonic() < t_end:
            cycle += 1
            b.typing(12)(5)
            b.clear_composer()
            b.js("benchStream(60)")
            b.p.pump(8, quiet=True)
            b.js("benchStream(0)")
            b.scrolling(3)
            if cycle % 3 == 0:
                b.p.ints(OP_SET_VISIBLE, 0)
                b.p.pump(5, quiet=True)
                b.p.ints(OP_SET_VISIBLE, 1)
                b.p.pump(1, quiet=True)
            if cycle % 10 == 0:  # the thread keeps growing like a long chat
                b.build(58 + cycle * 2 if b.synthetic else 40 + cycle)
            pids = proc_tree(b.p.proc.pid)
            s = {"t": round(time.monotonic() - t0), "cycle": cycle, "mem": mem(pids),
                 "nodes": int(b.js("document.getElementsByTagName('*').length") or 0),
                 "alive": b.p.proc.poll() is None}
            samples.append(s)
            print(f"  t={s['t']}s cycle={cycle} nodes={s['nodes']} "
                  + " ".join(f"{k}={v['rss']}MB(shm {v['shmem']})" for k, v in s["mem"].items()), flush=True)
    finally:
        b.close()
    return samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True)
    ap.add_argument("--url", default="synthetic")
    ap.add_argument("--gpu", action="store_true")
    ap.add_argument("--scale", type=int, default=200)
    ap.add_argument("--width", type=int, default=600)
    ap.add_argument("--height", type=int, default=900)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--transparent", default="1")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--load-wait", type=float, default=20)
    ap.add_argument("--scenarios", default="static,type12,typefast,dom4k,stream,dom19k,scroll,openclose,reopen")
    ap.add_argument("--cache")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--extra", action="append", default=[])
    ap.add_argument("--long", type=int, default=0, help="long-session mode: seconds")
    ap.add_argument("--json")
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    res = {"label": a.label, "args": vars(a), "results": []}
    if a.long:
        res["long"] = long_session(a)
    else:
        for run in range(a.runs):
            print(f"run {run + 1}/{a.runs} {a.label}", flush=True)
            res["results"] += run_scenarios(a, run)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
