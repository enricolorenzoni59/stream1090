#!/bin/sh
# Two simultaneous captures on the shared antenna: one dongle at 3.2, one at
# 2.4, same seconds, same gain. Same sky, so the two corpora are comparable.
d=${3:-/private/tmp/claude-501/-Users-samknows-stream1090/2603f8fe-ae11-4eba-9764-2486135eaa79/scratchpad/captures}
mkdir -p "$d"
secs=${1:-300}
rtl_sdr -d 0 -f 1090000000 -s 3200000 -g 49.6 -n $((3200000*secs)) "$d/dev0_3200k_${secs}s.cu8" 2>"$d/dev0.log" &
p0=$!
rtl_sdr -d 1 -f 1090000000 -s 2400000 -g 49.6 -n $((2400000*secs)) "$d/dev1_2400k_${secs}s.cu8" 2>"$d/dev1.log" &
p1=$!
wait $p0 $p1
ls -la "$d"
