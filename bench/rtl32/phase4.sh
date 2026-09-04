#!/bin/sh
# Does the deficit belong to the sample rate or to the front-end state?
# At 3.2 the tuner necessarily sits in the 6 MHz TV filter (there is nothing
# between 2.43 and 6 MHz). These two runs put that same wide state on a 2.4
# Msps arm, where everything else is the known-good reference.
cd /Users/samknows/stream1090
S=${1:-120}

# 4.0 how much does the FIR carry at 2.4? If -q is worth far more at 3.2 than
# at 2.4, the extra noise bandwidth of the wider rate is what the filter is
# buying back, and the deficit is a noise story rather than a signal one.
python3 bench/rtl32/ab.py --a 2.4:12,fir=off --b 2.4:12 --seconds $S \
  --rounds 2 --swap --name "fase4.0 FIR @2.4->12: off vs taps built-in" || exit 1

# 4.1 the open question left by EXPERIMENTS.md section 28.4: at 2.4 Msps, what
# does tuner_bandwidth = 3000000 (i.e. the 6 MHz state) actually cost?
python3 bench/rtl32/ab.py --a 2.4:12 --b 2.4:12,bw=3000000 --seconds $S \
  --rounds 2 --swap --name "fase4.1 2.4->12: bw default(2.43MHz) vs 3000000(stato 6MHz)" || exit 1

# 4.2 2.56 Msps already sits in the 6 MHz state by default (28.6). Same
# question, reached by a different road, and it grades a shipped preset.
python3 bench/rtl32/ab.py --a 2.4:12 --b 2.56:12 --seconds $S \
  --rounds 2 --swap --name "fase4.2 2.56->12 (stato 6MHz di default) vs 2.4->12" || exit 1

# 4.3 and the same knob at 2.56, which is the one case where narrowing lands
# inside Nyquist without cutting it (2.43 vs +-1.28 MHz).
python3 bench/rtl32/ab.py --a 2.56:12 --b 2.56:12,bw=2430000 --seconds $S \
  --rounds 2 --swap --name "fase4.3 2.56->12: bw default(6MHz) vs 2430000" || exit 1
