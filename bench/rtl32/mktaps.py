#!/usr/bin/env python3
"""Emit windowed-sinc tap files for a sweep of cutoffs and tap counts."""
import sys
from pathlib import Path
from scipy.signal import firwin

fs = float(sys.argv[1]); outdir = Path(sys.argv[2]); outdir.mkdir(exist_ok=True, parents=True)
for ntaps in (15, 21, 27):
    for cut_khz in (700, 800, 900, 1000, 1100, 1200, 1400):
        t = firwin(ntaps, cut_khz * 1e3, fs=fs, window="hamming")
        t = t / t.sum()
        p = outdir / f"sinc_{ntaps}t_{cut_khz}k.txt"
        p.write_text("# windowed sinc, fs=%g, cutoff=%d kHz, %d taps\n" % (fs, cut_khz, ntaps)
                     + "\n".join("%.12g" % v for v in t) + "\n")
        print(p)
