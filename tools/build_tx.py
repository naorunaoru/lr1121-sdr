#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the experimental TX player using the pinned ARC toolchain."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
from common import ROOT, run


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT/'build')
    p.add_argument('--jobs', type=int, default=4)
    a = p.parse_args()
    build = a.build.resolve()
    run(sys.executable, ROOT/'tools/build_extension.py', '--build', build, '--jobs', a.jobs)
    arc = build/'arc-binutils'
    out = build/'lr1121-tx'
    out.mkdir(exist_ok=True)
    run(arc/'gas/as-new', '-mcpu=em4', '-I', ROOT/'lr1121/extension',
        '-o', out/'player.o', ROOT/'lr1121/tx/player.s')
    run(arc/'ld/ld-new', '-T', ROOT/'lr1121/extension/extension.ld',
        '-o', out/'player.elf', out/'player.o')
    run(arc/'binutils/objcopy', '-O', 'binary', out/'player.elf', out/'player.bin')
    code = (out/'player.bin').read_bytes()
    prefix = (build/'lr1121/stream.bin').read_bytes()
    if (not code.startswith(prefix) or not 0 < len(code) <= 4080 or
            any(any(code[i:i+16]) for i in range(240, len(code), 256))):
        raise RuntimeError('TX payload prefix, size or CBC padding mismatch')
    print(json.dumps(dict(payload_bytes=len(code), sha256=hashlib.sha256(code).hexdigest())))


if __name__ == '__main__':
    main()
