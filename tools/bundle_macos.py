#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bundle the dedicated arm64 build; never removes a pre-existing app directory."""
import argparse,hashlib,json,os,plistlib,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('checkout',type=Path);p.add_argument('build',type=Path);p.add_argument('destination',type=Path);a=p.parse_args()
root=a.checkout.resolve();build=a.build.resolve();app=a.destination.resolve()
if app.exists():raise SystemExit(f'Refusing to replace {app}; choose a new destination')
contents=app/'Contents';mac=contents/'MacOS';libs=contents/'Frameworks';plugins=contents/'Plugins';res=contents/'Resources'
for d in (mac,libs,plugins):d.mkdir(parents=True)
shutil.copytree(root/'root/res',res)
shutil.copy2(root/'license',res/'SDRPlusPlus-LICENSE.txt')
project=Path(__file__).resolve().parents[1]
for source,name in [('LICENSE','LR1121-MIT-LICENSE.txt'),
                    ('sdrpp/LICENSE','LR1121-SDRPP-GPL3-LICENSE.txt'),
                    ('THIRD_PARTY_NOTICES.md','THIRD_PARTY_NOTICES.md'),
                    ('docs/licensing.md','LR1121-LICENSING.md')]:
 shutil.copy2(project/source,res/name)
shutil.copytree(Path(__file__).resolve().parent/'third-party-licenses',res/'ThirdPartyLicenses')
plist={'CFBundleName':'SDR++ LR1121','CFBundleDisplayName':'SDR++ LR1121','CFBundleIdentifier':'local.lr1121.sdrplusplus','CFBundleVersion':'1.4.0','CFBundleShortVersionString':'1.4.0','CFBundleExecutable':'launch','CFBundlePackageType':'APPL','NSHighResolutionCapable':True,'LSMinimumSystemVersion':'27.0'}
(contents/'Info.plist').write_bytes(plistlib.dumps(plist))
launcher=mac/'launch';launcher.write_text('''#!/bin/sh
APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$APP_DIR/sdrpp" -r "$APP_DIR/../../../config" "$@"
''');launcher.chmod(0o755)
def run(*args):return subprocess.check_output(list(map(str,args)),text=True)
def deps(path):
 identity=run('otool','-D',path).splitlines()[1:]
 lines=run('otool','-L',path).splitlines()[1:]
 return [x.strip().split(' (compatibility version')[0] for x in lines if x.strip().split(' (compatibility version')[0] not in identity]
def rpaths(path):return re.findall(r'cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset',run('otool','-l',path))
def resolve(dep,original):
 if dep.startswith('/') and Path(dep).exists():return Path(dep)
 if dep.startswith('@loader_path/'):return original.parent/dep[len('@loader_path/'):]
 if dep.startswith('@rpath/'):
  suffix=dep[len('@rpath/'):]
  for directory in rpaths(original):
   candidate=Path(directory.replace('@loader_path',str(original.parent)))/suffix
   if candidate.exists():return candidate
  matches=list(build.rglob(Path(suffix).name))
  if matches:return matches[0]
 raise RuntimeError(f'Cannot resolve {dep} in {original}')
pairs=[];copied={};origins={}
def add(src,dest):
 src=src.resolve();dest.parent.mkdir(parents=True,exist_ok=True)
 if dest in origins:
  if origins[dest]!=src:raise RuntimeError(f'Dependency name collision: {dest}')
  return
 shutil.copy2(src,dest);dest.chmod(0o755);origins[dest]=src;pairs.append((src,dest))
add(build/'sdrpp',mac/'sdrpp')
add(build/'source_modules/lr1121_source/lr1121_probe',mac/'lr1121_probe')
for name,relative in [('lr1121_source','source_modules/lr1121_source'),('radio','decoder_modules/radio'),('audio_sink','sink_modules/portaudio_sink'),('frequency_manager','misc_modules/frequency_manager'),('recorder','misc_modules/recorder')]:add(build/relative/(name+'.dylib'),plugins/(name+'.dylib'))
for original,target in pairs:
 for dep in deps(original):
  if dep.startswith(('/System/Library/','/usr/lib/')):continue
  source=resolve(dep,original);destination=libs/source.name;add(source,destination)
  replacement='@loader_path/'+os.path.relpath(destination,target.parent)
  subprocess.run(['install_name_tool','-change',dep,replacement,str(target)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
 if target.suffix=='.dylib':subprocess.run(['install_name_tool','-id','@rpath/'+target.name,str(target)],check=True,stderr=subprocess.PIPE)
 for rp in rpaths(target):subprocess.run(['install_name_tool','-delete_rpath',rp,str(target)],check=True,stderr=subprocess.PIPE)
for original,target in pairs:subprocess.run(['codesign','--force','--sign','-',str(target)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
for attr in ('com.apple.ResourceFork','com.apple.FinderInfo'):
 subprocess.run(['xattr','-dr',attr,str(app)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
subprocess.run(['xattr','-d','com.apple.FinderInfo',str(app)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
signed=subprocess.run(['codesign','--force','--sign','-',str(app)],text=True,capture_output=True)
if signed.returncode and 'resource fork, Finder information' in signed.stderr:
 # macOS File Provider may add FinderInfo between cleanup and signing.
 subprocess.run(['xattr','-dr','com.apple.FinderInfo',str(app)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 subprocess.run(['xattr','-dr','com.apple.ResourceFork',str(app)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 signed=subprocess.run(['codesign','--force','--sign','-',str(app)],text=True,capture_output=True)
if signed.returncode:raise RuntimeError(signed.stderr)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
manifest=[{'file':str(dst.relative_to(app)),'original':str(src),'sha256':hashlib.sha256(dst.read_bytes()).hexdigest()} for src,dst in pairs]
(app.parent/(app.stem+'-manifest.json')).write_text(json.dumps(manifest,indent=2)+'\n')
print(app)
