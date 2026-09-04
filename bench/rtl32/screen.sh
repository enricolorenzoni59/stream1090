#!/bin/sh
# Offline filter screening on a real 3.2 Msps capture. No dongle needed, so it
# can run while the live A/B keeps both radios busy.
cd /Users/samknows/stream1090
CAP=$1
OUT=${2:-/private/tmp/claude-501/-Users-samknows-stream1090/2603f8fe-ae11-4eba-9764-2486135eaa79/scratchpad}
python3 bench/rtl32/replay.py --data "$CAP" --rate 3.2 --up 16 \
  --skip-s 20 --window-s 60 --json "$OUT/screen32.json" \
  --arms off builtin bench/rtl32/taps32/ctl_2_56_verbatim.txt \
    bench/rtl32/taps32/sinc_15t_700k.txt bench/rtl32/taps32/sinc_15t_800k.txt \
    bench/rtl32/taps32/sinc_15t_900k.txt bench/rtl32/taps32/sinc_15t_1000k.txt \
    bench/rtl32/taps32/sinc_15t_1100k.txt bench/rtl32/taps32/sinc_15t_1200k.txt \
    bench/rtl32/taps32/sinc_15t_1400k.txt \
    bench/rtl32/taps32/sinc_21t_700k.txt bench/rtl32/taps32/sinc_21t_800k.txt \
    bench/rtl32/taps32/sinc_21t_900k.txt bench/rtl32/taps32/sinc_21t_1000k.txt \
    bench/rtl32/taps32/sinc_21t_1100k.txt bench/rtl32/taps32/sinc_21t_1200k.txt \
    bench/rtl32/taps32/sinc_21t_1400k.txt \
    bench/rtl32/taps32/sinc_27t_700k.txt bench/rtl32/taps32/sinc_27t_800k.txt \
    bench/rtl32/taps32/sinc_27t_900k.txt bench/rtl32/taps32/sinc_27t_1000k.txt \
    bench/rtl32/taps32/sinc_27t_1100k.txt bench/rtl32/taps32/sinc_27t_1200k.txt \
    bench/rtl32/taps32/sinc_27t_1400k.txt
