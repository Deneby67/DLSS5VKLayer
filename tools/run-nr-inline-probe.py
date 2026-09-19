#!/usr/bin/env python3
"""Run only the isolated NR HDR gate; never uses the RDR2 prefix."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import tempfile
import shutil

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binaries',type=Path,default=Path.home()/'.local/share/dlssnr/binaries')
p.add_argument('--proton',type=Path,default=Path('/opt/Proton-11.0-2c-Zen5-BP18-Mimalloc/proton'))
p.add_argument('--validation',action='store_true')
p.add_argument('--bridge',action='store_true')
p.add_argument('--inline',action='store_true',help='Exercise the application-local Vulkan shim and Color overlay')
p.add_argument('--case',choices=['active','launcher','disabled','missing-dll','bda-auto','real-sr','bootstrap-forward','bootstrap-track','bootstrap-bda','bootstrap-wsi','native-baseline','armed-cycle','groups-core','groups-khr','ext-bda','ext-bda-auto','ext-real-sr','descriptor-stress','rendering-settings','sr-E','sr-F','sr-J','sr-K','sr-L','sr-M'],default='active')
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
out=repo/'build/nr-inline'
run=out/'runs'/time.strftime('%Y%m%d-%H%M%S'); run.mkdir(parents=True)
ascii_run=Path(tempfile.mkdtemp(prefix='dlssnr-inline-'))
prefix=out/'prefix'; prefix.mkdir(exist_ok=True)
steam=Path.home()/'.steam/debian-installation'
env=os.environ.copy()
for key in ['WINEPREFIX','WINEDLLOVERRIDES','LD_PRELOAD','PROTON_LOG','DLSSNR_LAYER_OBJECT','DLSSFG_NGX_CAPTURE_DIR',
            'VK_LAYER_PATH','VK_INSTANCE_LAYERS','DLSSFG_VALIDATE']:
    env.pop(key,None)
env.update(STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam),STEAM_COMPAT_DATA_PATH=str(prefix),
    PROTON_ENABLE_NVAPI='1',DLSSNR_SKIP_NVAPI='1',WINEDEBUG='-all',WINESTEAMNOEXEC='1',
    VKLayer_DLSS5='0',VK_LOADER_LAYERS_DISABLE='~implicit~',DLSSNR_ENABLE='0',
    DLSSNR_LOG='Z:'+str(ascii_run/'nr.log'),DLSSNR_BIN_DIR='Z:'+str(a.binaries.resolve()))
if a.validation:
    env.update(VK_LAYER_PATH=str(repo/'build/fg/validation/root/usr/share/vulkan/explicit_layer.d'),
               VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',DLSSFG_VALIDATE='1')
env.pop('DLSSNR_PROBE_GROUPS',None)
env.pop('DLSSNR_PROBE_EXT_BDA',None)
env.pop('DLSSNR_ARM_FILE',None)
for name in ('DLSSNR_INLINE_SHM','DLSSNR_PROBE_STRESS','DLSSNR_PROBE_SETTINGS','DLSSNR_PROBE_SR_PRESET'):env.pop(name,None)
env.pop('DLSSNR_PROBE_ARM_CYCLE',None)
env.pop('DLSSNR_BOOTSTRAP',None)
env.pop('DLSSNR_BOOTSTRAP_DIR',None)
if a.bridge: env['DLSSNR_PROBE_BRIDGE']='1'
else: env.pop('DLSSNR_PROBE_BRIDGE',None)
cmd=[str(steam/'steamapps/common/SteamLinuxRuntime_4/_v2-entry-point'),'--verb=run','--',
     str(a.proton),'run',str(out/'nr_hdr_probe.exe')]
if a.inline:
    if not a.validation: p.error('--inline requires --validation')
    exe=run/('Launcher.exe' if a.case=='launcher' else 'RDR2.exe')
    shutil.copy2(out/('nr_bootstrap_wsi.exe' if a.case in ('bootstrap-wsi','native-baseline') else 'nr_inline_probe.exe'),exe)
    shutil.copy2(out/('dlssnr_system_vulkan.dll' if a.case=='native-baseline' else 'vulkan-1.dll'),run/'vulkan-1.dll')
    shutil.copy2(out/'dlssnr_system_vulkan.dll',run/'dlssnr_system_vulkan.dll')
    env.update(DLSSNR_INLINE='1',WINEDLLOVERRIDES='vulkan-1=n;dlssnr_system_vulkan=n')
    if a.case=='armed-cycle':env.update(DLSSNR_ARM_FILE='Z:'+str(ascii_run/'arm.txt'),DLSSNR_PROBE_ARM_CYCLE='1')
    if a.case=='descriptor-stress':env['DLSSNR_PROBE_STRESS']='1'
    if a.case=='rendering-settings':env.update(DLSSNR_INLINE_SHM='Z:'+str(ascii_run/'settings.bin'),DLSSNR_PROBE_SETTINGS='1')
    if a.case=='disabled':env['DLSSNR_INLINE']='0'
    if a.case=='missing-dll':env['DLSSNR_BIN_DIR']='Z:'+str(ascii_run/'missing')
    if a.case.startswith('bootstrap-'):
        env.update(DLSSNR_BOOTSTRAP=('forward' if a.case in ('bootstrap-wsi','native-baseline') else a.case.removeprefix('bootstrap-')),DLSSNR_BOOTSTRAP_DIR='Z:'+str(ascii_run))
    if a.case.startswith('sr-'):env['DLSSNR_PROBE_SR_PRESET']=str({'E':5,'F':6,'J':10,'K':11,'L':12,'M':13}[a.case[3:]])
    if a.case.startswith(('ext-bda','ext-real-sr','sr-')):env.update(DLSSNR_PROBE_EXT_BDA='1',DLSSNR_PROBE_GROUPS='1')
    if a.case.startswith('groups-'):env['DLSSNR_PROBE_GROUPS']='2' if a.case=='groups-khr' else '1'
    if a.case in ('bda-auto','ext-bda-auto') or a.case.startswith(('bootstrap-','groups-')):env['DLSSNR_PROBE_BDA_AUTO']='1'
    else:env.pop('DLSSNR_PROBE_BDA_AUTO',None)
    if a.case in ('real-sr','ext-real-sr') or a.case.startswith('sr-'):env['DLSSNR_PROBE_REAL_SR']='Z:'+str(steam/'steamapps/common/Red Dead Redemption 2/nvngx_dlss.dll')
    else:env.pop('DLSSNR_PROBE_REAL_SR',None)
    if a.case in ('launcher','disabled','missing-dll') or a.case.startswith('bootstrap-'):env['DLSSNR_EXPECT_BYPASS']='1'
    else:env.pop('DLSSNR_EXPECT_BYPASS',None)
    cmd[-1]=str(exe)
with (run/'runner.log').open('w') as output:
    proc=subprocess.Popen(cmd,cwd=run,env=env,stdout=output,stderr=output,start_new_session=True)
    try: code=proc.wait(timeout=180 if a.case=='descriptor-stress' else 60)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid,signal.SIGTERM)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: os.killpg(proc.pid,signal.SIGKILL); proc.wait()
        code=124
if (ascii_run/'nr.log').exists(): shutil.copy2(ascii_run/'nr.log',run/'nr.log')
for trace in ascii_run.glob('bootstrap-*.log'): shutil.copy2(trace,run/trace.name)
with (a.binaries/'nvngx_dlssnr.dll').open('rb') as stream:
    sha=hashlib.file_digest(stream,'sha256').hexdigest()
if a.case=='rendering-settings' and code==0:
    log=(run/'nr.log').read_text()
    required=('intensity=0.65 tone=0.90 structure=0.80 skin=0.25 automask=0',
              'Rendering configuration applied slot=1 intensity=1.50',
              'Rendering configuration applied slot=0 intensity=0.65',
              'Rendering state disabled','Rendering state unavailable',
              'PASS calls=8 callbacks=8 replacements=6')
    if not all(marker in log for marker in required):code=13
record=dict(exit_code=code,passed=code==0,validation=a.validation,bridge=a.bridge,inline=a.inline,case=a.case,dll_sha256=sha)
if a.inline:
    record['loader_kind']='rdr2_native_windows_loader'
    with (run/'dlssnr_system_vulkan.dll').open('rb') as stream: record['native_loader_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
    with (run/'vulkan-1.dll').open('rb') as stream: record['shim_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
    with exe.open('rb') as stream: record['probe_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
    if a.case in ('real-sr','ext-real-sr') or a.case.startswith('sr-'):
        with (steam/'steamapps/common/Red Dead Redemption 2/nvngx_dlss.dll').open('rb') as stream:
            record['sr_dll_sha256']=hashlib.file_digest(stream,'sha256').hexdigest()
(run/'result.json').write_text(json.dumps(record,indent=2)+'\n')
print(run,record)
for name in ('runner.log','nr.log'):
    if (run/name).exists(): print((run/name).read_text(errors='replace')[-9000:])
raise SystemExit(code)
