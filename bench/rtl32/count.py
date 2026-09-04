#!/usr/bin/env python3
"""Count frames/uniq/df17 from an AVR stream on stdin."""
import sys

frames = uniq = df17 = 0
seen = set()
for line in sys.stdin:
    line = line.strip().rstrip(";")
    if len(line) < 15 or line[0] not in "*@<":
        continue
    body = line[15:] if line[0] == "<" else (line[13:] if line[0] == "@" else line[1:])
    body = body.split(";")[0].strip().upper()
    if len(body) not in (14, 28):
        continue
    frames += 1
    seen.add(body)
    try:
        if (int(body[0:2], 16) >> 3) == 17:
            df17 += 1
    except ValueError:
        pass
print(f"frames {frames} uniq {len(seen)} df17 {df17}")
