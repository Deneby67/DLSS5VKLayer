#!/usr/bin/env python3
"""Test the PE proxy in a private Proton prefix; never edits the game prefix."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--proton', type=Path, default=Path('/opt/Proton-11.0-2c-Zen5-BP18-Mimalloc/proton'))
p.add_argument('--real-only', action='store_true')
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
out = repo/'build/ngx-capture'
stamp = out/'runs'/time.strftime('%Y%m%d-%H%M%S')
stamp.mkdir(parents=True, mode=0o700)
steam = Path.home()/'.steam/debian-installation'
prefix = out/'prefix'
prefix.mkdir(exist_ok=True)
cases = [('armed', 'RDR2.exe', True, '--armed'),
         ('idle', 'RDR2.exe', True, '--bench'),
         ('baseline', 'Launcher.exe', True, '--bench'),
         ('launcher', 'Launcher.exe', True, '--armed'),
         ('disabled', 'RDR2.exe', False, ''),
         ('real', 'RDR2.exe', True, '--real')]
if a.real_only:
    cases = cases[-1:]
results = []
for name, executable, enabled, argument in cases:
    folder = stamp/name
    folder.mkdir(mode=0o700)
    logs = folder/'logs'
    logs.mkdir(mode=0o700)
    shutil.copy2(out/'RDR2.exe', folder/executable)
    shutil.copy2(out/'version.dll', folder/'version.dll')
    shutil.copy2(a.proton.parent/'files/lib/wine/x86_64-windows/version.dll', folder/'dlssfg_system_version.dll')
    if name != 'real':
        shutil.copy2(out/'nvngx.dll', folder/'nvngx.dll')
        shutil.copy2(out/'_nvngx.dll', folder/'_nvngx.dll')
    env = os.environ.copy()
    for key in ['WINEPREFIX', 'LD_PRELOAD', 'PROTON_LOG', 'DLSSFG_DISCOVERY_DIR',
                'DLSSFG_NGX_CAPTURE_DIR', 'DLSSNR_LAYER_OBJECT', 'VK_INSTANCE_LAYERS', 'VK_LAYER_PATH']:
        env.pop(key, None)
    env.update(STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam), STEAM_COMPAT_DATA_PATH=str(prefix),
               PROTON_ENABLE_NVAPI='1', WINESTEAMNOEXEC='1', PROTON_ENABLE_WAYLAND='0', WINEDEBUG='-all',
               WINEDLLOVERRIDES='version=n,b', DLSSNR_ENABLE='0', VKLayer_DLSS5='0',
               VK_LOADER_LAYERS_DISABLE='~implicit~', DLSSNR_INLINE='0', DLSSNR_LOG=str(folder/'adapter.log'))
    if enabled:
        env['DLSSFG_NGX_CAPTURE_DIR'] = 'Z:' + str(logs)
    cmd = [str(steam/'steamapps/common/SteamLinuxRuntime_4/_v2-entry-point'), '--verb=run', '--',
           str(a.proton), 'run', str(folder/executable)] + ([argument] if argument else [])
    with (folder/'run.log').open('w') as log:
        proc = subprocess.Popen(cmd, cwd=folder, env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            code = proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGTERM)
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            code = 124
    print(name, code, (folder/'run.log').read_text(errors='replace')[-1400:], flush=True)
    assert code == 0, f'{name}: process failed; see {folder}'
    report = json.loads((folder/'probe-result.json').read_text())
    assert report['passed'] and report['real_nvidia_parameters'] == (name == 'real')
    if name == 'real':
        assert report['snapshot']['values']['Width']['value'] == 640
        assert report['snapshot']['resources']['MotionVectors']['resource']['format'] == 83
    rows = [json.loads(line) for path in logs.glob('ngx-*.jsonl') for line in path.read_text().splitlines()]
    before = [r for r in rows if r['event'] == 'evaluate_before']
    after = [r for r in rows if r['event'] == 'evaluate_after']
    if name == 'armed':
        assert len(before) == len(after) == 2, rows
        assert [r['result'] for r in after] == [1, 0xbad00005]
        assert all(r['before']['resources']['MotionVectors']['resource']['format'] == 83 for r in before)
        assert all(r['before']['values']['Jitter.Offset.X']['value'] == .25 for r in before)
        assert all(r['request'] == 123456 and r['feature'] == 1 for r in before)
        assert before[0]['handle_generation'] > 0
        assert len([r for r in rows if r['event'] == 'capture_end']) == 1
    elif name == 'idle':
        assert not before and any(r['event'] == 'intercept' for r in rows), rows
    elif name == 'real':
        assert len([r for r in rows if r['event'] == 'intercept']) == 4, rows
    elif name in ('disabled', 'launcher', 'baseline'):
        assert not rows, rows
    results.append(dict(case=name, passed=True, idle_evaluate_ns=report.get('idle_evaluate_ns'), log=str(folder/'run.log')))
(stamp/'result.json').write_text(json.dumps(results, indent=2)+'\n')
print('PASS: NGX capture tests:', stamp)
