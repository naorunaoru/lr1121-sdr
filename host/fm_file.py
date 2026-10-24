#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert GNU Radio float32-LE frequency deviations (Hz) to LR1121 signed16-BE offsets.

No hardware access. Input must contain whole 2048-update banks; no implicit padding,
clipping or resampling. Pass the reported offset shift and requested rate to tx.py.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import tx


def encode(raw, rate, shift=None):
    timing = tx.timing_settings(rate)
    if len(raw) % 4 or not 4096 <= len(raw)//4 <= 3200000 or len(raw)//4 % tx.BLOCK:
        raise ValueError('Input must be 4096..3200000 float32 values in whole 2048-update banks')
    values = [x[0] for x in struct.iter_unpack('<f', raw)]
    if not all(math.isfinite(x) for x in values):
        raise ValueError('Input contains non-finite deviations')
    if shift is None:
        shift = tx.resolve_shift(None, max(abs(x) for x in values))
    enc = tx.offset_encoding(shift)
    if min(values) < enc['offset_min_hz'] or max(values) > enc['offset_max_hz']:
        raise ValueError('Deviation exceeds selected encoding; no clipping performed')
    offsets = [round(x/enc['offset_step_hz']) for x in values]
    if any(not -32768 <= x <= 32767 for x in offsets):
        raise ValueError('Quantized deviation exceeds signed16')
    report = dict(**timing, **enc, count=len(offsets), input_format='f32le_hz',
                  output_format='s16be_offsets', peak_deviation_hz=max(abs(x) for x in values),
                  max_quantization_error_hz=max(abs(x-q*enc['offset_step_hz']) for x,q in zip(values,offsets)))
    return struct.pack(f'>{len(offsets)}h', *offsets), report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--rate',type=float,required=True)
    p.add_argument('--offset-shift',type=int)
    a=p.parse_args()
    try: raw,report=encode(a.input.read_bytes(),a.rate,a.offset_shift)
    except ValueError as e:p.error(str(e))
    with a.output.open('xb') as f:f.write(raw)
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
