#!/usr/bin/env python3
"""Simultaneous two-dongle A/B for stream1090 on a shared antenna (splitter).

Both dongles see the same sky at the same time, so a single run compares two
configurations without traffic drift between arms. Roles can be swapped to
cancel the asymmetry between the two receivers.

Each arm is one stream1090 process reading its own dongle (selected by serial
in a generated .ini). We count AVR frames and distinct payloads on stdout and
take per-process CPU time from wait4(), so the CPU column is the real cost of
that configuration, not of the harness.
"""
from __future__ import annotations

import argparse, json, os, signal, subprocess, sys, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


INI = """[rtlsdr]
serial = {serial}
frequency = 1090000000
agc = {agc}
{gain_line}
bias_tee = false
{bw_line}
"""


def write_ini(path: Path, serial: str, gain: float | None, agc: bool,
              bw: int | None) -> Path:
    path.write_text(INI.format(
        serial=serial,
        agc="true" if agc else "false",
        gain_line="" if gain is None else f"gain = {gain}",
        bw_line="" if bw is None else f"tuner_bandwidth = {bw}"))
    return path


def notify(title: str, msg: str) -> None:
    try:
        print(msg)
    except Exception as e:
        print(f"telegram failed: {e}", file=sys.stderr, flush=True)


class Arm:
    """One stream1090 process on one dongle."""

    def __init__(self, label: str, binary: Path, serial: str, rate: str,
                 up: str, taps: str | None, fir: bool, gain: float | None,
                 agc: bool, bw: int | None, tmp: Path):
        self.label = label
        self.serial = serial
        self.rate, self.up, self.taps, self.fir = rate, up, taps, fir
        self.binary = binary
        self.gain, self.agc, self.bw = gain, agc, bw
        self.ini = write_ini(tmp / f"{label}.ini", serial, gain, agc, bw)
        self.frames = 0
        self.payloads: set[str] = set()
        self.df17 = 0
        self.cpu = 0.0
        self.proc: subprocess.Popen | None = None
        self.stderr_tail: list[str] = []

    def desc(self) -> str:
        bits = [f"{self.rate}->{self.up}"]
        bits.append("fir=" + ("taps-file" if self.taps else ("built-in" if self.fir else "off")))
        bits.append("bw=" + ("default" if self.bw is None else str(self.bw)))
        bits.append("agc" if self.agc else f"gain={self.gain}")
        parts = self.binary.parent.parts
        bits.append("/".join(parts[-2:]) if len(parts) > 1 else self.binary.parent.name)
        return " ".join(bits)

    def cmd(self) -> list[str]:
        c = [str(self.binary), "-s", self.rate, "-u", self.up, "-d", str(self.ini)]
        if self.taps:
            c += ["-f", self.taps]
        elif self.fir:
            c += ["-q"]
        return c

    def start(self) -> None:
        self.proc = subprocess.Popen(self.cmd(), stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, cwd=str(ROOT))
        self._out = threading.Thread(target=self._read_stdout, daemon=True)
        self._err = threading.Thread(target=self._read_stderr, daemon=True)
        self._out.start(); self._err.start()

    def _read_stdout(self) -> None:
        for raw in self.proc.stdout:
            line = raw.decode(errors="replace").strip().rstrip(";")
            if len(line) < 15 or line[0] not in "*@<":
                continue
            body = line[15:] if line[0] == "<" else (
                line[13:] if line[0] == "@" else line[1:])
            body = body.split(";")[0].strip().upper()
            if len(body) not in (14, 28):
                continue
            self.frames += 1
            self.payloads.add(body)
            try:
                if (int(body[0:2], 16) >> 3) == 17:
                    self.df17 += 1
            except ValueError:
                pass

    def _read_stderr(self) -> None:
        for raw in self.proc.stderr:
            s = raw.decode(errors="replace").rstrip()
            self.stderr_tail.append(s)
            del self.stderr_tail[:-40]

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
        pid = self.proc.pid
        try:
            _, status, ru = os.wait4(pid, 0)
        except ChildProcessError:
            ru = None
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.returncode = 0
        if ru is not None:
            self.cpu = ru.ru_utime + ru.ru_stime
        self._out.join(timeout=5); self._err.join(timeout=5)

    def result(self, seconds: float) -> dict:
        return {"label": self.label, "serial": self.serial, "desc": self.desc(),
                "frames": self.frames, "unique": len(self.payloads),
                "df17": self.df17, "seconds": seconds,
                "frames_s": self.frames / seconds,
                "unique_s": len(self.payloads) / seconds,
                "cpu_s": self.cpu, "cpu_core_pct": 100.0 * self.cpu / seconds}


def spec(arg: str) -> dict:
    """rate:up[,fir=on|off|<file>][,bw=<hz>][,gain=<db>|agc][,build=<dir>]"""
    parts = arg.split(",")
    rate, up = parts[0].split(":")
    out = {"rate": rate, "up": up, "fir": True, "taps": None, "bw": None,
           "gain": 49.6, "agc": False, "build": "build-32"}
    for p in parts[1:]:
        if p == "agc":
            out["agc"] = True; out["gain"] = None
        elif p.startswith("fir="):
            v = p[4:]
            if v == "off": out["fir"] = False
            elif v == "on": out["fir"] = True
            else: out["taps"] = v
        elif p.startswith("bw="):
            out["bw"] = None if p[3:] in ("default", "auto") else int(p[3:])
        elif p.startswith("gain="):
            out["gain"] = float(p[5:]); out["agc"] = False
        elif p.startswith("build="):
            out["build"] = p[6:]
    return out


def run_pair(a: dict, b: dict, serial_a: str, serial_b: str, seconds: float,
             tmp: Path, warmup: float) -> tuple[dict, dict]:
    arms = []
    for label, s, serial in (("A", a, serial_a), ("B", b, serial_b)):
        arms.append(Arm(label, ROOT / s["build"] / "stream1090", serial,
                        s["rate"], s["up"], s["taps"], s["fir"], s["gain"],
                        s["agc"], s["bw"], tmp))
    for arm in arms:
        arm.start()
    time.sleep(warmup)
    for arm in arms:                       # discard tuning/startup transient
        arm.frames = 0; arm.payloads.clear(); arm.df17 = 0
    t0 = time.time()
    time.sleep(seconds)
    elapsed = time.time() - t0
    for arm in arms:
        arm.stop()
    return arms[0].result(elapsed), arms[1].result(elapsed)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="arm A spec (reference)")
    ap.add_argument("--b", required=True, help="arm B spec (candidate)")
    ap.add_argument("--seconds", type=float, default=180.0)
    ap.add_argument("--warmup", type=float, default=8.0)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--swap", action="store_true",
                    help="second half of the rounds swaps the two dongles")
    ap.add_argument("--serial-a", default="00000001")
    ap.add_argument("--serial-b", default="00000002")
    ap.add_argument("--name", default="run")
    ap.add_argument("--out", type=Path, default=HERE / "results.jsonl")
    ap.add_argument("--no-telegram", action="store_true")
    args = ap.parse_args()

    a, b = spec(args.a), spec(args.b)
    tmp = HERE / "tmp"; tmp.mkdir(exist_ok=True)
    agg = {"A": {"frames": 0, "unique": 0, "cpu": 0.0, "sec": 0.0, "df17": 0},
           "B": {"frames": 0, "unique": 0, "cpu": 0.0, "sec": 0.0, "df17": 0}}
    rows = []
    for rd in range(args.rounds):
        swapped = args.swap and rd >= args.rounds / 2
        sa, sb = ((args.serial_b, args.serial_a) if swapped
                  else (args.serial_a, args.serial_b))
        ra, rb = run_pair(a, b, sa, sb, args.seconds, tmp, args.warmup)
        ra["swapped"] = rb["swapped"] = swapped
        rows += [ra, rb]
        for key, r in (("A", ra), ("B", rb)):
            agg[key]["frames"] += r["frames"]; agg[key]["unique"] += r["unique"]
            agg[key]["cpu"] += r["cpu_s"]; agg[key]["sec"] += r["seconds"]
            agg[key]["df17"] += r["df17"]
        print(f"[{args.name}] round {rd+1}/{args.rounds} swapped={swapped}", flush=True)
        for r in (ra, rb):
            print(f"  {r['label']} {r['desc']:<62} frames={r['frames']:6d} "
                  f"uniq={r['unique']:6d} df17={r['df17']:5d} "
                  f"cpu={r['cpu_core_pct']:5.1f}% of a core", flush=True)

    def pct(x, y):
        return 100.0 * (x - y) / y if y else float("nan")

    summary = {"name": args.name, "a": a, "b": b, "rounds": args.rounds,
               "seconds": args.seconds, "rows": rows,
               "agg": agg,
               "gain_frames_pct": pct(agg["B"]["frames"], agg["A"]["frames"]),
               "gain_unique_pct": pct(agg["B"]["unique"], agg["A"]["unique"]),
               "cpu_ratio": (agg["B"]["cpu"] / agg["A"]["cpu"]) if agg["A"]["cpu"] else 0.0,
               "ts": time.strftime("%Y-%m-%d %H:%M:%S")}
    with args.out.open("a") as f:
        f.write(json.dumps(summary) + "\n")

    ta = agg["A"]; tb = agg["B"]
    msg = (f"{args.name}\n"
           f"A (rif): {rows[0]['desc']}\n"
           f"B (test): {rows[1]['desc']}\n"
           f"{args.rounds}x{args.seconds:.0f}s"
           f"{' (ruoli scambiati a meta)' if args.swap else ''}\n\n"
           f"frame   A {ta['frames']:6d}  B {tb['frames']:6d}   "
           f"{summary['gain_frames_pct']:+.1f}%\n"
           f"unici   A {ta['unique']:6d}  B {tb['unique']:6d}   "
           f"{summary['gain_unique_pct']:+.1f}%\n"
           f"DF17    A {ta['df17']:6d}  B {tb['df17']:6d}\n"
           f"CPU     A {100*ta['cpu']/ta['sec']:5.1f}%  B {100*tb['cpu']/tb['sec']:5.1f}%  "
           f"(x{summary['cpu_ratio']:.2f}) di un core")
    print(msg, flush=True)
    if not args.no_telegram:
        notify("stream1090 3.2 Msps A/B", msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
