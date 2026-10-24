#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse
import os
from pathlib import Path
import platform
import sys
from common import ROOT,run,prepare_sdrpp
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,default=ROOT/'build');a=p.parse_args();a.build=a.build.resolve()
source=prepare_sdrpp(a.build);out=a.build/'tests';out.mkdir(exist_ok=True)
cc=os.environ.get('CC','cc');cxx=os.environ.get('CXX','c++');module=ROOT/'sdrpp/lr1121_source'
for name,impl in [('test_protocol','protocol'),('test_stream_format','stream_format')]:
    run(cc,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'pico/firmware'),ROOT/f'pico/tests/{name}.c',ROOT/f'pico/firmware/{impl}.c','-o',out/name)
    run(out/name)
run(cc,'-std=c11','-Wall','-Wextra','-Werror',ROOT/'pico/tests/test_tx_protocol.c',ROOT/'pico/tx/protocol.c','-o',out/'test_tx_protocol')
run(out/'test_tx_protocol')
flags=['-std=c++17','-O2','-I'+str(source/'core/src'),'-I'+str(module/'src')]
libs=['-framework','IOKit','-framework','CoreFoundation'] if platform.system()=='Darwin' else ['-pthread']
for name in ('test_frames','test_retune','retune_probe'):
    run(cxx,*flags,module/f'tests/{name}.cpp',module/'src/device.cpp',*libs,'-o',out/name)
    if name!='retune_probe':run(out/name)
run(cxx,*flags,ROOT/'sdrpp/tests/test_waterfall_signal.cpp','-o',out/'test_waterfall_signal');run(out/'test_waterfall_signal')
for directory in ('host/tests','lr1121/tests'):
    run(sys.executable,'-m','unittest','discover','-s',ROOT/directory,'-v')
