#!/bin/sh
# Knob-by-knob at 3.2 Msps. Both arms are 3.2 and differ by exactly one thing,
# which is far more sensitive than routing every question through the 2.4
# reference. $1 = upsample rate chosen in phase 2.
cd /Users/samknows/stream1090
U=${1:-16}
CUB=/Users/samknows/stream1090-cubic/build-cub
CUBALL=/Users/samknows/stream1090-cubic/build-cub-all
S=${2:-120}

# 3.1 tuner bandwidth: default (the 6 MHz TV filter, the only state above
# 2.43 MHz) against the widest state that still fits inside 3.2 Nyquist.
python3 bench/rtl32/ab.py --a 3.2:$U --b 3.2:$U,bw=2430000 --seconds $S \
  --rounds 2 --swap --name "fase3.1 tuner bw @3.2->$U: default(6MHz) vs 2430000" || exit 1

# 3.2 the IQ FIR itself: is -q worth it at 3.2?
python3 bench/rtl32/ab.py --a 3.2:$U,fir=off --b 3.2:$U --seconds $S \
  --rounds 2 --swap --name "fase3.2 FIR @3.2->$U: off vs taps built-in 27" || exit 1

# 3.3 interpolation kernel: linear (main) vs the PR #41 sharpening cubic
if [ "$U" = "16" ]; then B="3.2:$U,build=$CUB"; else B="3.2:$U,build=$CUBALL"; fi
python3 bench/rtl32/ab.py --a 3.2:$U --b "$B" --seconds $S \
  --rounds 2 --swap --name "fase3.3 kernel @3.2->$U: lineare vs cubic PR41" || exit 1

# 3.4 gain: 49.6 is the top of the table, but at 3.2 the front end is open to
# +-3 MHz, so saturation is a real possibility.
python3 bench/rtl32/ab.py --a 3.2:$U --b 3.2:$U,gain=44.5 --seconds $S \
  --rounds 2 --swap --name "fase3.4 gain @3.2->$U: 49.6 vs 44.5" || exit 1
