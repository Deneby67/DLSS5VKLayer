#!/usr/bin/env python3
"""Exercise the updated NR service on synthetic frames in an isolated prefix/SHM."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import time
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--proton',type=Path,required=True)
p.add_argument('--binaries',type=Path,required=True)
p.add_argument('--steam',type=Path,default=Path.home()/'.steam/debian-installation')
p.add_argument('--shmctl',type=Path,default=Path('/usr/lib64/dlssnr/bin/dlssnr-shmctl'))
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
build=repo/'build/fg'
work=Path(tempfile.mkdtemp(prefix='dlssnr-smoke-'))
print(f'Diagnostic directory: {work}',flush=True)
(work/'prefix').mkdir(exist_ok=True)
shm=work/'shm.bin'
log=work/'helper.log'
if log.exists(): log.rename(work/f'helper-{time.time_ns()}.log')
subprocess.run([a.shmctl,shm,'reset'],check=True,stdout=subprocess.DEVNULL)
env=os.environ.copy()
for key in ['WINEPREFIX','WINEDLLOVERRIDES','LD_PRELOAD','PROTON_LOG']:
    env.pop(key,None)
env.update(STEAM_COMPAT_CLIENT_INSTALL_PATH=str(a.steam),STEAM_COMPAT_DATA_PATH=str(work/'prefix'),
    PROTON_ENABLE_NVAPI='1',DLSSNR_SKIP_NVAPI='1',WINEDEBUG='-all',WINESTEAMNOEXEC='1',
    VKLayer_DLSS5='0',VK_LOADER_LAYERS_DISABLE='~implicit~',DLSSNR_LOG='helper.log',
    DLSSNR_BIN_DIR='Z:'+str(a.binaries.absolute()),DLSSNR_SHM=str(shm),
    DLSSFG_PROFILE='Z:'+str(repo/'profiles/fg/rdr2.json'))
cmd=[str(a.steam/'steamapps/common/SteamLinuxRuntime_4/_v2-entry-point'),'--verb=run','--',
     str(a.proton),'run',str(build/'dlssnr_helper.exe')]
# The helper normally changes cwd to its executable directory. Use an absolute
# Wine log path here so the test can follow its progress without sharing logs.
env['DLSSNR_LOG']='Z:'+str(log)
with (work/'runner.log').open('w') as output:
    process=subprocess.Popen(cmd,cwd=work,env=env,stdout=output,stderr=output,start_new_session=True)
    try:
        deadline=time.monotonic()+35
        while True:
            text=log.read_text(errors='replace') if log.exists() else ''
            if 'context ready' in text and '[fg-profile]' in text: break
            if process.poll() is not None: raise RuntimeError('helper exited before becoming ready')
            if time.monotonic()>deadline: raise RuntimeError('helper startup/profile load timed out')
            time.sleep(.2)
        subprocess.run([str(build/'shm-frames'),'1280','720','6'],env=env,check=True,timeout=60)
        print('PASS: NR service, optical flow and disabled RDR2 profile coexist')
    finally:
        subprocess.run([a.shmctl,shm,'quit'],stdout=subprocess.DEVNULL,check=False)
        try: process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGTERM)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL)
                process.wait()
