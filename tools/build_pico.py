#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build pinned picotool and the Pico bridge, optionally bound to a verified LR1121."""
import argparse
from pathlib import Path
from common import ROOT, run

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT / 'build')
parser.add_argument('--binding', type=Path)
parser.add_argument('--toolchain', type=Path, help='Arm GNU root; omit when arm-none-eabi-gcc is on PATH')
parser.add_argument('--jobs', type=int, default=4)
args = parser.parse_args()
build = args.build.resolve()
sdk = ROOT / 'external/pico-sdk'
prefix = build / 'tools'
run(
    'cmake', '-S', ROOT / 'external/picotool', '-B', build / 'picotool',
    '-DPICO_SDK_PATH=' + str(sdk), '-DPICOTOOL_FLAT_INSTALL=ON',
    '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_BUILD_TYPE=Release',
)
run('cmake', '--build', build / 'picotool', '-j', args.jobs)
run('cmake', '--install', build / 'picotool')
command = [
    'cmake', '-S', ROOT / 'pico', '-B', build / 'pico',
    '-DPICO_SDK_PATH=' + str(sdk), '-Dpicotool_DIR=' + str(prefix / 'picotool'),
    '-DCMAKE_BUILD_TYPE=Release',
]
if args.binding:
    command += ['-C', args.binding.resolve()]
else:
    command += ['-DLR1121_VERIFIED_UID=']  # Clear an earlier cached binding.
if args.toolchain:
    command += ['-DPICO_TOOLCHAIN_PATH=' + str(args.toolchain.resolve())]
run(*command)
run('cmake', '--build', build / 'pico', '-j', args.jobs)
