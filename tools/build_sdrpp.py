#!/usr/bin/env python3
"""Stage and build the pinned SDR++ tree with only the LR1121 receiver modules."""
import argparse
from pathlib import Path
import platform
import re
import sys
from common import ROOT, run, prepare_sdrpp

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT / 'build')
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--prefix', default='/opt/homebrew' if platform.system() == 'Darwin' else '')
parser.add_argument('--bundle', action='store_true')
args = parser.parse_args()
if args.bundle and platform.system() != 'Darwin':
    parser.error('--bundle requires macOS')
build = args.build.resolve()
source = prepare_sdrpp(build)
keep = {
    'OPT_BUILD_LR1121_SOURCE', 'OPT_BUILD_RADIO', 'OPT_BUILD_PORTAUDIO_SINK',
    'OPT_BUILD_FREQUENCY_MANAGER', 'OPT_BUILD_RECORDER',
}
options = re.findall(r'option\((OPT_BUILD_\w+)', (source / 'CMakeLists.txt').read_text())
command = ['cmake', '-S', source, '-B', build / 'sdrpp', '-DCMAKE_BUILD_TYPE=Release']
if args.prefix:
    command.append('-DCMAKE_PREFIX_PATH=' + args.prefix)
command.append('-DUSE_BUNDLE_DEFAULTS=' + ('ON' if args.bundle else 'OFF'))
command += ['-D' + option + '=' + ('ON' if option in keep else 'OFF') for option in options]
run(*command)
run('cmake', '--build', build / 'sdrpp', '-j', args.jobs)
if args.bundle:
    run(sys.executable, ROOT / 'tools/bundle_macos.py', source,
        build / 'sdrpp', build / 'SDR++ LR1121.app')
