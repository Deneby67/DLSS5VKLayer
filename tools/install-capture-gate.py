#!/usr/bin/env python3
"""Install the RDR2-only RenderDoc gate. Capture stays off unless explicitly opted in."""
import argparse
import datetime
import json
from pathlib import Path
import shutil

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--backend',type=Path,required=True,help='Official RenderDoc 1.46 librenderdoc.so')
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
home=Path.home()
manifest=home/'.local/share/vulkan/implicit_layer.d/renderdoc_capture.json'
binary=repo/'build/capture/libdlssfg_capture_gate.so'
for f in [a.backend,binary,manifest]:
    if not f.is_file(): p.error('Missing '+str(f))
m=json.loads(manifest.read_text())
if m['layer']['name']!='VK_LAYER_RENDERDOC_Capture': p.error('Unexpected layer manifest')
out=home/'.local/lib/dlssnr-fg/capture'
wrapper=home/'.local/bin/dlssfg-capture-rdr2'
backup=home/'.local/share/dlssnr/backups'/('capture-gate-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
backup.mkdir(parents=True,mode=0o700,exist_ok=False)
shutil.copy2(manifest,backup/'renderdoc_capture.json')
if wrapper.exists(): shutil.copy2(wrapper,backup/'dlssfg-capture-rdr2')
if out.exists(): shutil.copytree(out,backup/'capture',symlinks=True)
out.mkdir(parents=True,exist_ok=True)
def install(source,destination):
    tmp=destination.with_name(destination.name+'.tmp')
    shutil.copy2(source,tmp); tmp.replace(destination)
install(binary,out/binary.name)
(out/'renderdoc.path').write_text(str(a.backend.absolute())+'\n')
layer=m['layer']; layer['library_path']=str(out/binary.name)
layer['functions']={'vkGetInstanceProcAddr':'DLSSFG_GetInstanceProcAddr',
    'vkGetDeviceProcAddr':'DLSSFG_GetDeviceProcAddr','vkNegotiateLoaderLayerInterfaceVersion':'DLSSFG_Negotiate'}
layer['pre_instance_functions']={'vkEnumerateInstanceExtensionProperties':'DLSSFG_EnumerateInstanceExtensionProperties'}
tmp=manifest.with_suffix('.tmp'); tmp.write_text(json.dumps(m,indent=2)+'\n'); tmp.replace(manifest)
wrapper.parent.mkdir(parents=True,exist_ok=True)
install(repo/'tools/steam-fg-capture.sh',wrapper); wrapper.chmod(0o755)
print('Installed; capture disabled by default. Backup:',backup)
