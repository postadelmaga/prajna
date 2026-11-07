#!/usr/bin/env python3
"""bench_compare.py — before/after table from prajna_bench.py JSON files.

    bench_compare.py --a 'base-syn-*.json' --b 'new-syn-*.json' [--md]

Every value is the median over the runs of each side (one result per
scenario per run); the change is (b - a) / a.
"""
import argparse
import glob
import json
import statistics


def load(pattern):
    out = {}
    for path in sorted(glob.glob(pattern)):
        for r in json.load(open(path)).get("results", []):
            out.setdefault(r["scenario"], []).append(r)
    return out


METRICS = [
    ("CPU tot %", lambda r: r["cpu"].get("total")),
    ("WebProcess %", lambda r: r["cpu"].get("WPEWebProcess")),
    ("web main %", lambda r: r["threads"].get("main")),
    ("engine %", lambda r: r["cpu"].get("prajna-engine")),
    ("GPU %", lambda r: r.get("gpu_pct")),
    ("fps", lambda r: r.get("fps")),
    ("full frames", lambda r: r.get("full_frames")),
    ("damage MB/s", lambda r: r.get("damage_mb_s")),
    ("lat p50 ms", lambda r: r["lat_ms"].get("p50") if r.get("lat_ms") else None),
    ("lat p90 ms", lambda r: r["lat_ms"].get("p90") if r.get("lat_ms") else None),
    ("lat p99 ms", lambda r: r["lat_ms"].get("p99") if r.get("lat_ms") else None),
    ("lag max ms", lambda r: r["lag_ms"].get("max") if r.get("lag_ms") else None),
    ("web RSS MB", lambda r: r["mem"].get("WPEWebProcess", {}).get("rss")),
    ("engine RSS MB", lambda r: r["mem"].get("prajna-engine", {}).get("rss")),
    ("engine shm MB", lambda r: r["mem"].get("prajna-engine", {}).get("shmem")),
    ("first frame ms", lambda r: r.get("first_frame_ms")),
]


def med(rs, f):
    vals = []
    for r in rs:
        try:
            v = f(r)
        except (KeyError, TypeError):
            v = None
        if v is not None:
            vals.append(v)
    return statistics.median(vals) if vals else None


def fmt(v):
    if v is None:
        return "–"
    return f"{v:.0f}" if abs(v) >= 10 else f"{v:.1f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--scenarios")
    ap.add_argument("--metrics")
    a = ap.parse_args()
    A, B = load(a.a), load(a.b)
    scen = a.scenarios.split(",") if a.scenarios else [s for s in A if s in B]
    want = a.metrics.split(",") if a.metrics else None
    for s in scen:
        if s not in A or s not in B:
            continue
        print(f"\n### {s}  (runs: {len(A[s])} vs {len(B[s])})")
        print("| metric | before | after | change |")
        print("|---|---:|---:|---:|")
        for name, f in METRICS:
            if want and name not in want:
                continue
            x, y = med(A[s], f), med(B[s], f)
            if x is None and y is None:
                continue
            ch = f"{100 * (y - x) / x:+.0f}%" if x not in (None, 0) and y is not None else ""
            print(f"| {name} | {fmt(x)} | {fmt(y)} | {ch} |")


if __name__ == "__main__":
    main()
