#!/usr/bin/env python3
"""Install the tested RDR2-only observer, retaining an exact reversible backup."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--game', type=Path, default=Path.home()/'.steam/debian-installation/steamapps/common/Red Dead Redemption 2')
p.add_argument('--proton', type=Path, default=Path('/opt/Proton-11.0-2c-Zen5-BP18-Mimalloc'))
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
home = Path.home()
proxy = repo/'build/ngx-capture/version.dll'
original = a.proton/'files/lib/wine/x86_64-windows/version.dll'
wrapper = home/'.local/bin/dlssfg-capture-rdr2'
marker = home/'.config/dlssnr/fg-ngx.enabled'
discovery = home/'.config/dlssnr/fg-discovery.enabled'
record = home/'.config/dlssnr/ngx-install.json'
logs = home/'.local/state/dlssnr/ngx'
game_proxy = a.game/'version.dll'
game_original = a.game/'dlssfg_system_version.dll'
for source in [proxy, original, wrapper, a.game/'RDR2.exe']:
    if not source.is_file(): p.error(f'Missing required file: {source}')
def digest(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()
reports = sorted((repo/'build/ngx-capture/runs').glob('*/result.json'))
if not reports: p.error('Run tools/test-ngx-capture.py before installation')
tested = reports[-1]
tests = json.loads(tested.read_text())
if {r['case'] for r in tests if r.get('passed')} != {'armed','idle','baseline','launcher','disabled','real'}:
    p.error('Latest test report is incomplete')
for case in tests:
    tested_proxy = tested.parent/case['case']/'version.dll'
    if digest(tested_proxy) != digest(proxy): p.error('Build differs from the tested proxy')
if digest(tested.parent/'real/dlssfg_system_version.dll') != digest(original):
    p.error('Selected Proton version.dll differs from the tested one')
old_record = json.loads(record.read_text()) if record.exists() else {}
for path in [game_proxy, game_original]:
    if path.is_symlink(): p.error(f'Refusing to replace symlink: {path}')
    if path.exists() and old_record.get('installed_hashes', {}).get(str(path)) != digest(path):
        p.error(f'Existing unrecognized DLL must be preserved: {path}')
backup = home/'.local/share/dlssnr/backups'/('ngx-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
backup.mkdir(parents=True, mode=0o700)
changes = []
for i, path in enumerate([game_proxy, game_original, wrapper, marker, discovery, record]):
    saved = backup/str(i)
    if path.exists(): shutil.copy2(path, saved)
    changes.append(dict(destination=str(path), backup=str(saved) if saved.exists() else None))
def install(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dst.parent)
    os.close(fd)
    try:
        shutil.copy2(src, tmp)
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)
# All inputs/backups are checked before changing selection. A rollback script is
# written first, so even an interrupted installation can be restored manually.
(backup/'restore.json').write_text(json.dumps(changes, indent=2)+'\n')
(backup/'restore.py').write_text('''#!/usr/bin/env python3
import json, os, shutil, tempfile
from pathlib import Path
for item in json.loads(Path(__file__).with_name('restore.json').read_text()):
    target=Path(item['destination'])
    if item['backup']:
        fd,tmp=tempfile.mkstemp(dir=target.parent); os.close(fd)
        shutil.copy2(item['backup'],tmp); os.replace(tmp,target)
    else: target.unlink(missing_ok=True)
print('Restored. Changes take effect on the next game launch.')
''')
logs.mkdir(parents=True, exist_ok=True, mode=0o700)
logs.chmod(0o700)
install(original, game_original)
install(proxy, game_proxy)
install(repo/'tools/steam-fg-capture.sh', wrapper)
wrapper.chmod(0o755)
discovery.unlink(missing_ok=True)
marker.write_text('RDR2-only NGX observations. Per-frame snapshots require an explicit bounded request.\n')
hashes = {str(path): digest(path) for path in [game_proxy, game_original, wrapper]}
record.write_text(json.dumps(dict(installed_hashes=hashes, game=str(a.game), proton=str(a.proton),
                                 pipeline_order=['NR', 'SR', 'FG'], mode='observe_only',
                                 test_report=str(tested),
                                 rollback=str(backup/'restore.py')), indent=2)+'\n')
print(json.dumps(dict(installed_hashes=hashes, logs=str(logs), rollback=str(backup/'restore.py'),
                     note='Next launch only. No game resources changed; old draw discovery disabled.'), indent=2))
