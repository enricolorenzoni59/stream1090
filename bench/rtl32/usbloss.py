#!/usr/bin/env python3
"""Measure USB sample loss by timing a fixed-size capture.

The dongle produces samples at a fixed rate. If the host cannot keep up, the
lost samples never reach the file, so collecting N samples takes longer than
N/rate. loss = 1 - (N/rate)/elapsed. Both dongles are exercised at the same
time because that is the condition the A/B runs actually use.
"""
import subprocess, sys, threading, time

def run(serial, rate, seconds, out):
    n = int(rate * seconds)
    cmd = ["rtl_sdr", "-d", serial, "-f", "1090000000", "-s", str(rate),
           "-g", "49.6", "-n", str(n), "/dev/null"]
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True)
    el = time.time() - t0
    ideal = n / rate
    err = r.stderr.decode(errors="replace")
    noisy = [l for l in err.splitlines()
             if "transfer status" in l or "lost" in l.lower() or "reattach" in l]
    out[serial] = {"rate": rate, "n": n, "ideal_s": ideal, "elapsed_s": el,
                   "loss_pct": 100.0 * (1 - ideal / el) if el else 0.0,
                   "warnings": noisy[:5]}

def main():
    # device index, not serial: rtl_sdr -d takes an index
    pairs = [(a, b) for a, b in [tuple(x.split(":")) for x in sys.argv[1:]]]
    out = {}
    ths = [threading.Thread(target=run, args=(d, int(float(r) * 1e6), 20.0, out))
           for d, r in pairs]
    for t in ths: t.start()
    for t in ths: t.join()
    for d, res in sorted(out.items()):
        print(f"dev {d} @ {res['rate']/1e6:.2f} Msps: ideale {res['ideal_s']:.2f}s, "
              f"reale {res['elapsed_s']:.2f}s -> perdita {res['loss_pct']:+.2f}%"
              f"  {res['warnings'] if res['warnings'] else ''}")

main()
