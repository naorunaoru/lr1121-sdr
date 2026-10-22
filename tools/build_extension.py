#!/usr/bin/env python3
"""Build pinned ARC binutils and the CBC-layout-compatible FIFO1 extension."""
import argparse
import hashlib
import json
from pathlib import Path
from common import ROOT, run

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT / 'build')
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
build = args.build.resolve()
tools = build / 'arc-binutils'
tools.mkdir(parents=True, exist_ok=True)
if not (tools / 'Makefile').exists():
    run(
        ROOT / 'external/binutils/configure', '--target=arc-elf',
        '--disable-gdb', '--disable-sim', '--disable-gprof', '--disable-nls',
        '--disable-werror', '--without-zstd', '--with-system-zlib', '--disable-shared',
        cwd=tools,
    )
run('make', '-j', args.jobs, 'MAKEINFO=true', 'all-gas', 'all-ld', 'all-binutils', cwd=tools)
out = build / 'lr1121'
out.mkdir(exist_ok=True)
run(tools / 'gas/as-new', '-mcpu=em4', '-o', out / 'stream.o', 'stream.s',
    cwd=ROOT / 'lr1121/extension')
run(tools / 'ld/ld-new', '-T', ROOT / 'lr1121/extension/extension.ld',
    '-o', out / 'stream.elf', out / 'stream.o')
run(tools / 'binutils/objcopy', '-O', 'binary', out / 'stream.elf', out / 'stream.bin')
code = (out / 'stream.bin').read_bytes()
if not 0 < len(code) <= 4080 or any(any(code[i:i+16]) for i in range(240, len(code), 256)):
    raise RuntimeError('Payload size/CBC holes invalid')
print(json.dumps(dict(payload_bytes=len(code), sha256=hashlib.sha256(code).hexdigest())))
