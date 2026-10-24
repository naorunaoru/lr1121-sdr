#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the GPL receiver capture utility without building the SDR++ GUI."""
import argparse
import os
from pathlib import Path
import platform
from common import ROOT,run
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build');a=p.parse_args()
out=a.build.resolve()/'tools';out.mkdir(parents=True,exist_ok=True)
module=ROOT/'sdrpp/lr1121_source'
libs=['-framework','IOKit','-framework','CoreFoundation'] if platform.system()=='Darwin' else ['-pthread']
run(os.environ.get('CXX','c++'),'-std=c++17','-O2','-I'+str(ROOT/'external/sdrplusplus/core/src'),
    '-I'+str(module/'src'),module/'src/probe.cpp',module/'src/device.cpp',*libs,'-o',out/'lr1121_probe')
print(out/'lr1121_probe')
