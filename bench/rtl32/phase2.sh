#!/bin/sh
cd /Users/samknows/stream1090
for u in 8 12 16 24; do
  python3 bench/rtl32/ab.py --a 2.4:12 --b 3.2:$u --seconds 120 --warmup 8 \
    --rounds 2 --swap --name "fase2 upsample: 3.2->$u contro 2.4->12" || exit 1
done
