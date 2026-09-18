#!/usr/bin/env python3
"""Test native Vulkan device creation and actual backend mappings per argv[0]."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--manifest',type=Path,default=Path.home()/'.local/share/vulkan/implicit_layer.d/renderdoc_capture.json')
p.add_argument('--backend',type=Path,required=True)
a=p.parse_args()
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='capture-gate-test-') as scratch:
    work=Path(scratch)
    m=json.loads(a.manifest.read_text()); layer=m['layer']
    layer['library_path']=str(r/'build/capture/libdlssfg_capture_gate.so')
    layer['functions']={'vkGetInstanceProcAddr':'DLSSFG_GetInstanceProcAddr',
        'vkGetDeviceProcAddr':'DLSSFG_GetDeviceProcAddr','vkNegotiateLoaderLayerInterfaceVersion':'DLSSFG_Negotiate'}
    layer['pre_instance_functions']={'vkEnumerateInstanceExtensionProperties':'DLSSFG_EnumerateInstanceExtensionProperties'}
    manifests=work/'data/vulkan/implicit_layer.d'; manifests.mkdir(parents=True)
    (manifests/'renderdoc_capture.json').write_text(json.dumps(m))
    env=os.environ.copy()
    env.update(XDG_DATA_HOME=str(work/'data'),XDG_CONFIG_HOME=str(work/'config'),
        ENABLE_VULKAN_RENDERDOC_CAPTURE='1',DLSSFG_RENDERDOC_LIBRARY=str(a.backend.absolute()),VKLayer_DLSS5='0')
    for k in ['DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46','VK_LOADER_LAYERS_DISABLE','VK_INSTANCE_LAYERS']:
        env.pop(k,None)
    cases=[(r'C:\Program Files\Rockstar Games\Launcher\Launcher.exe',False),
        ('SocialClubHelper.exe',False),('RDR2.exe.bak',False),('/tmp/RDR2.exe/Launcher.exe',False),
        (r'S:\steamapps\common\Red Dead Redemption 2\RDR2.exe',True),('rdr2.EXE',True)]
    for name,captured in cases:
        result=subprocess.run([name,'captured' if captured else 'bypass'],
            executable=str(r/'build/capture/capture-gate-probe'),env=env,
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
        print(name,result.returncode,result.stdout,flush=True)
        if result.returncode: raise SystemExit(result.returncode)
    env['DLSSFG_RENDERDOC_LIBRARY']=str(work/'missing.so')
    subprocess.run(['RDR2.exe','bypass'],executable=str(r/'build/capture/capture-gate-probe'),env=env,check=True,timeout=30)
print('PASS: excluded processes, exact basename matching, capture target, missing backend fallback')
