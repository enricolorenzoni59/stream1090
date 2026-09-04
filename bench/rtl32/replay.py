#!/usr/bin/env python3
"""Decode one capture with several filters and compare, offline.

Same binary, same capture, same window for every arm, so the only thing that
moves is the filter. Reports frames, distinct payloads, DF17 and the CPU cost
of the decode itself (wait4), which is what the -q flag actually buys.
"""
from __future__ import annotations
import argparse, json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def decode(binary: Path, data: bytes, rate: str, up: str,
           taps: Path | None, builtin: bool) -> dict:
    cmd = [str(binary), "-s", rate, "-u", up]
    if taps is not None:
        cmd += ["-f", str(taps)]
    elif builtin:
        cmd += ["-q"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    t0 = time.time()
    out, _ = p.communicate(data)
    wall = time.time() - t0
    frames = 0; df17 = 0; uniq = set()
    for line in out.decode(errors="replace").splitlines():
        line = line.strip().rstrip(";")
        if len(line) < 15 or line[0] not in "*@<":
            continue
        body = line[15:] if line[0] == "<" else (
            line[13:] if line[0] == "@" else line[1:])
        body = body.split(";")[0].strip().upper()
        if len(body) not in (14, 28):
            continue
        frames += 1; uniq.add(body)
        try:
            if (int(body[0:2], 16) >> 3) == 17:
                df17 += 1
        except ValueError:
            pass
    return {"frames": frames, "unique": len(uniq), "df17": df17, "wall_s": wall}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--rate", default="3.2")
    ap.add_argument("--up", default="16")
    ap.add_argument("--binary", type=Path, default=ROOT / "build-32/stream1090")
    ap.add_argument("--skip-s", type=float, default=0.0)
    ap.add_argument("--window-s", type=float, default=0.0, help="0 = all")
    ap.add_argument("--arms", nargs="+", required=True,
                    help="off | builtin | <taps file path>")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    rate_hz = int(float(args.rate) * 1e6)
    size = args.data.stat().st_size
    off = int(args.skip_s * rate_hz) * 2
    end = size if not args.window_s else off + int(args.window_s * rate_hz) * 2
    with args.data.open("rb") as f:          # only the window, these files are GBs
        f.seek(off)
        data = f.read(min(end, size) - off)
    secs = len(data) / 2 / rate_hz
    print(f"{args.data.name}: {secs:.1f}s @ {args.rate} Msps -> {args.up}, "
          f"binary {args.binary.parent.name}", flush=True)

    rows = []
    base = None
    for arm in args.arms:
        if arm == "off":
            r = decode(args.binary, data, args.rate, args.up, None, False)
        elif arm == "builtin":
            r = decode(args.binary, data, args.rate, args.up, None, True)
        else:
            r = decode(args.binary, data, args.rate, args.up, Path(arm), False)
        r["arm"] = arm
        if base is None:
            base = r
        r["vs_base_unique_pct"] = 100.0 * (r["unique"] - base["unique"]) / base["unique"]
        r["vs_base_frames_pct"] = 100.0 * (r["frames"] - base["frames"]) / base["frames"]
        rows.append(r)
        print(f"  {Path(arm).name:<44} frames {r['frames']:6d} ({r['vs_base_frames_pct']:+5.1f}%)"
              f"  unici {r['unique']:6d} ({r['vs_base_unique_pct']:+5.1f}%)"
              f"  df17 {r['df17']:5d}  decode {r['wall_s']:5.1f}s", flush=True)
    if args.json:
        args.json.write_text(json.dumps(rows, indent=1))
    return 0


sys.exit(main())
