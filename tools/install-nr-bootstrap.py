#!/usr/bin/env python3
"""Install only the NR-disabled forward-mode startup diagnostic, with exact rollback."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--dry-run',action='store_true')
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
home=Path.home()
game=home/'.steam/debian-installation/steamapps/common/Red Dead Redemption 2'
wrapper=home/'.local/bin/dlssfg-capture-rdr2'
shim=repo/'build/nr-inline/vulkan-1.dll'
dest=game/'vulkan-1.dll'
record=home/'.config/dlssnr/bootstrap-install.json'

def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

if record.exists():p.error('A bootstrap installation already exists; restore it before installing another')
if dest.exists() or dest.is_symlink():p.error('Preserving existing game-local vulkan-1.dll; restore its owner first')
if wrapper.is_symlink():p.error('Refusing to replace a symlinked launch wrapper')
if (home/'.config/dlssnr/nr-inline.enabled').exists():p.error('Inline NR must be rolled back first')
if sha(game/'RDR2.exe')!='b56c9548f670654a9b73bf25def3cd73af12e269f6e47dba28a34079adaf465e':
    p.error('Game version differs from the researched executable')
if sha(game/'version.dll')!='b946a9e66edaff34c88b4cf8c901731a9bea01276741dae389cf1d75772ce9a7':
    p.error('This diagnostic expects the confirmed working NGX observer to remain installed')
previous=json.loads((home/'.config/dlssnr/ngx-install.json').read_text())
if previous['installed_hashes'].get(str(wrapper))!=sha(wrapper):p.error('Launch wrapper differs from the working installation record')
reports={}
for path in sorted((repo/'build/nr-inline/runs').glob('*/result.json')):
    report=json.loads(path.read_text())
    if report.get('inline'):reports[report.get('case')]=(path,report)
for case in ('bootstrap-forward','bootstrap-track','bootstrap-bda','bootstrap-wsi','launcher','disabled'):
    if case not in reports:p.error(f'Missing validation gate: {case}')
    path,result=reports[case]
    fixture='nr_bootstrap_wsi.exe' if case=='bootstrap-wsi' else 'nr_inline_probe.exe'
    if not result.get('passed') or not result.get('validation') or result.get('shim_sha256')!=sha(shim) or result.get('probe_sha256')!=sha(repo/'build/nr-inline'/fixture):
        p.error(f'Failed or stale validation gate: {path}')
    if case.startswith('bootstrap-'):
        traces=list(path.parent.glob('bootstrap-*.log'))
        if len(traces)!=1:p.error(f'Missing startup trace: {path}')
        text=traces[0].read_text()
        if 'api=vkCreateDevice call=1 exit result=0' not in text:p.error(f'Device startup not validated: {path}')
        if case=='bootstrap-wsi' and 'api=vkQueuePresentKHR call=3 exit result=0' not in text:
            p.error('WSI gate did not present three frames')

stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
backup=home/'.local/share/dlssnr/backups'/('bootstrap-'+stamp)
logs=home/'.local/state/dlssnr'/('bootstrap-'+stamp)
plan=dict(mode='forward_only_no_nr_no_bda_augmentation',shim_sha256=sha(shim),
          previous_wrapper_sha256=sha(wrapper),logs=str(logs),rollback=str(backup/'restore.py'),
          tests={case:str(reports[case][0]) for case in ('bootstrap-forward','bootstrap-track','bootstrap-bda','bootstrap-wsi','launcher','disabled')})
if a.dry_run:
    print(json.dumps(plan,indent=2));raise SystemExit(0)
backup.mkdir(parents=True,mode=0o700)
logs.mkdir(parents=True,mode=0o700)
shutil.copy2(wrapper,backup/'previous-wrapper')
# Force NR off independently in both the observer and shim. Existing presentation
# selection, Proton, game arguments and version.dll are retained unchanged.
script='\n'.join(['#!/usr/bin/env bash','set -euo pipefail',
    'export DLSSNR_INLINE=0 DLSSNR_BOOTSTRAP=forward',
    'bootstrap_root='+shlex.quote(str(logs)),
    'bootstrap_run=$(mktemp -d "$bootstrap_root/run-XXXXXXXX")',
    'export DLSSNR_BOOTSTRAP_DIR="Z:$bootstrap_run"',
    'export DLSSNR_LOG="Z:$bootstrap_run/adapter.log"',
    'export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}vulkan-1=n,b"',
    'exec '+shlex.quote(str(backup/'previous-wrapper'))+' "$@"',''])
(backup/'new-wrapper').write_text(script)
(backup/'new-wrapper').chmod(0o755)
plan['installed_hashes']={str(dest):sha(shim),str(wrapper):sha(backup/'new-wrapper')}
plan.update(wrapper=str(wrapper),shim=str(dest),record=str(record))
(backup/'restore.json').write_text(json.dumps(plan,indent=2)+'\n')
(backup/'restore.py').write_text('''#!/usr/bin/env python3
import hashlib,json,os,shutil,tempfile
from pathlib import Path
base=Path(__file__).parent
plan=json.loads((base/'restore.json').read_text())
def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
wrapper=Path(plan['wrapper']);shim=Path(plan['shim'])
# Allow an interrupted installation, but never overwrite a later user's edit.
for target in (wrapper,shim):
    allowed={plan['installed_hashes'][str(target)]}
    if target==wrapper:allowed.add(plan['previous_wrapper_sha256'])
    if target.is_symlink() or (target.exists() and digest(target) not in allowed):
        raise SystemExit('Changed file preserved; inspect before restoring: '+str(target))
fd,tmp=tempfile.mkstemp(dir=wrapper.parent);os.close(fd)
shutil.copy2(base/'previous-wrapper',tmp);os.replace(tmp,wrapper)
shim.unlink(missing_ok=True);Path(plan['record']).unlink(missing_ok=True)
print('Previous launch restored. Logs retained. Effective on next game start.')
''')
# Record rollback before mutations; all replacements use same-directory rename.
record.write_text(json.dumps(plan,indent=2)+'\n')
def install(src,dst):
    fd,tmp=tempfile.mkstemp(dir=dst.parent);os.close(fd)
    try:shutil.copy2(src,tmp);os.replace(tmp,dst)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
install(shim,dest)
install(backup/'new-wrapper',wrapper)
print(json.dumps(plan,indent=2))
