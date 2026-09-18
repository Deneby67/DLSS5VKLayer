#!/usr/bin/env python3
"""Run the offscreen FG gate in its own Proton prefix, never the game's prefix."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--proton', type=Path, default=Path('/opt/Proton-11.0-2c-Zen5-BP18-Mimalloc/proton'))
p.add_argument('--steam', type=Path, default=Path.home()/'.steam/debian-installation')
p.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1]/'build/fg')
p.add_argument('--timeout', type=int, default=90)
p.add_argument('--validation', action='store_true')
p.add_argument('--helper', type=Path, help='Test the embedded gate in this installed NR helper')
p.add_argument('--dll', type=Path, help='Test this installed FG DLL instead of the build copy')
args = p.parse_args()
r = args.output.absolute()
exe = args.helper.absolute() if args.helper else r/'fg_probe.exe'
dll = args.dll.absolute() if args.dll else r/'binaries/nvngx_dlssg.dll'
for f in [exe, dll, args.proton]:
    if not f.is_file(): p.error(f'Missing {f}')
(r/'prefix').mkdir(exist_ok=True)
env = os.environ.copy()
for name in ['WINEPREFIX', 'WINEDLLOVERRIDES', 'LD_PRELOAD', 'PROTON_LOG', 'DLSSNR_LAYER_OBJECT']:
    env.pop(name, None)
env.update(STEAM_COMPAT_CLIENT_INSTALL_PATH=str(args.steam),
           STEAM_COMPAT_DATA_PATH=str(r/'prefix'), PROTON_ENABLE_NVAPI='1',
           WINESTEAMNOEXEC='1', PROTON_ENABLE_WAYLAND='0', WINEDEBUG='-all',
           DLSSNR_ENABLE='0', VKLayer_DLSS5='0', VK_LOADER_LAYERS_DISABLE='~implicit~',
           DLSSNR_LOG='ngx.log', DXVK_NVAPI_LOG_LEVEL='info', DXVK_NVAPI_LOG_PATH=str(r))
command = [str(args.steam/'steamapps/common/SteamLinuxRuntime_4/_v2-entry-point'),
           '--verb=run', '--', str(args.proton), 'run', str(exe)]
if args.helper: command.append('--fg-self-test')
command.append('Z:'+str(dll))
if args.validation:
    layers=r/'validation/root/usr/share/vulkan/explicit_layer.d'
    if not layers.is_dir(): p.error('Extract vulkan-validationlayers into build/fg/validation/root first')
    env.update(VK_LAYER_PATH=str(layers), VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
               VK_LOADER_DEBUG='error,warn,layer', DLSSFG_VALIDATE='1')
started = time.time()
archive=r/'runs'/time.strftime('%Y%m%d-%H%M%S')
archive.mkdir(parents=True,exist_ok=False)
for name in ['run.log', 'ngx.log', 'run-result.json']:
    if (r/name).exists(): (r/name).rename(archive/name)
with (r/'run.log').open('w') as log:
    proc = subprocess.Popen(command, cwd=r, env=env, stdout=log, stderr=log, start_new_session=True)
    try:
        code = proc.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGTERM)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        code = 124
record = dict(exit_code=code, seconds=round(time.time()-started, 3), command=command,
              proton=str(args.proton), passed=code == 0)
(r/'run-result.json').write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, ensure_ascii=False))
for name in ['run.log', 'ngx.log']:
    if (r/name).exists(): print((r/name).read_text(errors='replace')[-6000:])
raise SystemExit(code)
