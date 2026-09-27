#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Known CRC-valid DF17 through raw signed-16 -> FIR -> sampler -> decoder."""
import importlib.util
import pathlib
import random
import struct
import subprocess
import sys

spec = importlib.util.spec_from_file_location('lab',pathlib.Path(__file__).parents[1]/'scripts/sdrplay_lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)
frame = b'8D40621D58C382D690C8AC2863A7'
assert lab.frame_summary([frame])['df17_18_valid_output_crc'] == 1
assert lab.frame_summary([frame[:-1]+b'6'])['df17_18_valid_output_crc'] == 0
bits = bin(int(frame,16))[2:].zfill(112)
rng = random.Random(42)
iq = bytearray()
for n in range(120000):
    t = (n/4)%1000-100
    pulse = any(a <= t < a+.5 for a in (0,1,3.5,4.5))
    if 8 <= t < 120:
        bit = int(t-8)
        phase = t-8-bit
        pulse = phase < .5 if bits[bit] == '1' else phase >= .5
    iq += struct.pack('<hh',(12000 if pulse else 0)+rng.randint(-25,25),rng.randint(-25,25))
for rate in ('8','12'):
    for options in ([],['--no-iq-filter']):
        result = subprocess.run([sys.argv[1],'--device','stdin','--input-format','cs16','-s','4','-u',rate,*options],
                                input=iq,capture_output=True,timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr.decode())
        decoded = [lab.avr_payload(line) for line in result.stdout.splitlines()]
        if len(decoded) < 25 or any(p != frame for p in decoded):
            raise RuntimeError((rate,options,decoded))
bad = subprocess.run([sys.argv[1],'--device','stdin','--input-format','cs16','-s','4'],
                     input=b'123',capture_output=True,timeout=30)
if bad.returncode != 1:
    raise RuntimeError('Truncated IQ accepted')
