#!/usr/bin/env python3
"""Install the validated native-loader NR adapter, initially disarmed, with exact rollback."""
import argparse,datetime,hashlib,json,os,shlex,shutil,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--dry-run',action='store_true');a=p.parse_args()
repo=Path(__file__).resolve().parents[1];home=Path.home();out=repo/'build/nr-inline'
steam=home/'.steam/debian-installation';game=steam/'steamapps/common/Red Dead Redemption 2'
prefix=steam/'steamapps/compatdata/1174180/pfx'
wrapper=home/'.local/bin/dlssfg-capture-rdr2';record=home/'.config/dlssnr/native-inline-install.json'
NATIVE='9de5d9a7a1c14152bac98318c63d540520da16b8826bf96a76bef90a5d223906'
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def require(ok,msg):
    if not ok:p.error(msg)
require(not record.exists(),'Native adapter already installed; restore it before installing another build')
require(sha(game/'RDR2.exe')=='b56c9548f670654a9b73bf25def3cd73af12e269f6e47dba28a34079adaf465e','Unverified game executable')
require(sha(prefix/'drive_c/windows/system32/vulkan-1.dll')==NATIVE,'Game prefix Vulkan loader changed')
require(sha(out/'dlssnr_system_vulkan.dll')==NATIVE,'Build backend does not match the working game loader')
require(json.loads((out/'loader.json').read_text())==dict(kind='rdr2_native_windows_loader',sha256=NATIVE),'Wrong build backend metadata')
old=json.loads((home/'.config/dlssnr/ngx-install.json').read_text())
for f in (game/'version.dll',game/'dlssfg_system_version.dll'):
    require(not f.is_symlink() and sha(f)==old['installed_hashes'].get(str(f)),f'Existing observer differs: {f}')
require(not wrapper.is_symlink(),'Refusing a symlinked launch wrapper')
base=wrapper
if sha(wrapper)!=old['installed_hashes'].get(str(wrapper)):
    control=json.loads((home/'.config/dlssnr/bootstrap-install.json').read_text())
    require(control.get('mode')=='builtin_vulkan_both_nr_paths_disabled','Unrecognized launch selection')
    require(sha(wrapper)==control['installed_hashes'].get(str(wrapper)),'Control wrapper changed')
    base=Path(control['rollback']).parent/'previous-wrapper'
require(sha(base)==old['installed_hashes'].get(str(wrapper)),'Known working base wrapper unavailable')
for name in ('vulkan-1.dll','dlssnr_system_vulkan.dll'):
    require(not (game/name).exists() and not (game/name).is_symlink(),f'Preserving existing {name}')
# Match the exact native backend, adapter, model and fixtures in every gate.
reports={}
for path in sorted((out/'runs').glob('*/result.json')):
    r=json.loads(path.read_text())
    if r.get('inline'):reports[r.get('case')]=(path,r)
cases=('native-baseline','bootstrap-wsi','bootstrap-forward','bootstrap-track','bootstrap-bda',
       'launcher','disabled','missing-dll','bda-auto','armed-cycle','real-sr','groups-core','groups-khr','ext-bda','ext-bda-auto','ext-real-sr','descriptor-stress','rendering-settings','hdr-snapshot','hdr-snapshot-stale','sr-E','sr-F','sr-J','sr-K','sr-L','sr-M')
for case in cases:
    require(case in reports,f'Missing native validation gate: {case}')
    path,r=reports[case]
    fixture='nr_bootstrap_wsi.exe' if case in ('native-baseline','bootstrap-wsi') else 'nr_inline_probe.exe'
    expected=NATIVE if case=='native-baseline' else sha(out/'vulkan-1.dll')
    require(r.get('passed') and r.get('validation') and r.get('loader_kind')=='rdr2_native_windows_loader' and
            r.get('native_loader_sha256')==NATIVE and r.get('shim_sha256')==expected and
            r.get('probe_sha256')==sha(out/fixture),f'Failed or stale gate: {path}')
    require(r.get('dll_sha256')==sha(home/'.local/share/dlssnr/binaries/nvngx_dlssnr.dll'),'NR model changed since validation')
    if case in ('real-sr','ext-real-sr') or case.startswith('sr-'):require(r.get('sr_dll_sha256')==sha(game/'nvngx_dlss.dll'),'Game SR DLL changed')
sr_source=json.loads((repo/'build/dlss-sr/source.json').read_text())
require(sr_source['sha256']=='3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983','Untested SR release')
require(sha(repo/'build/dlss-sr/nvngx_dlss.dll')==sr_source['sha256'],'SR download differs from verified NVIDIA release')
proxy=repo/'build/ngx-capture/version.dll'
ngx_reports=sorted((repo/'build/ngx-capture/runs').glob('*/result.json'))
require(bool(ngx_reports),'NGX proxy has not been tested');ngx_report=ngx_reports[-1]
rows=json.loads(ngx_report.read_text())
require({r['case'] for r in rows if r.get('passed')}=={'armed','idle','baseline','launcher','disabled','real','sr-cnn','sr-transformer'},'Incomplete NGX proxy gate')
for row in rows:require(sha(ngx_report.parent/row['case']/'version.dll')==sha(proxy),'Untested NGX proxy build')
require(sha(ngx_report.parent/'real/dlssfg_system_version.dll')==sha(game/'dlssfg_system_version.dll'),'Different forwarded version DLL')
backup=home/'.local/share/dlssnr/backups'/('native-inline-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
logs=home/'.local/state/dlssnr/native-inline'
settings_path=Path(f'/tmp/dlssnr-{os.getuid()}/shm.bin')
config=home/'.config/dlssnr/config.ini'
if config.exists():
    for line in config.read_text().splitlines():
        if line.startswith('shm=') and line[4:].strip():settings_path=Path(line[4:].strip())
require(settings_path.is_absolute(),'GUI shared-memory path must be absolute')
plan=dict(sr_source=sr_source,settings_path=str(settings_path),mode='native_loader_inline_nr_initially_disarmed',native_loader_sha256=NATIVE,
    log_root=str(logs),rollback=str(backup/'restore.py'),tests={c:str(reports[c][0]) for c in cases},ngx_test=str(ngx_report))
if a.dry_run:print(json.dumps(plan,indent=2));raise SystemExit(0)
backup.mkdir(parents=True,mode=0o700);logs.mkdir(parents=True,exist_ok=True,mode=0o700)
shutil.copy2(base,backup/'run-base')
script='\n'.join(['#!/usr/bin/env bash','set -euo pipefail',
    'export DLSSNR_INLINE=1 DLSSFG_NGX_CAPTURE=1 DLSSNR_SKIP_NVAPI=1',
    'export VKLayer_DLSS5=0 DLSSNR_ENABLE=0 DLSSFG_DISCOVERY=0 DLSSFG_ENABLE_RENDERDOC=0',
    'unset DLSSNR_BOOTSTRAP DLSSNR_BOOTSTRAP_DIR',
    'native_log_root='+shlex.quote(str(logs)),
    'native_run=$(mktemp -d "$native_log_root/run-XXXXXXXX")',
    'export DLSSNR_ARM_FILE="Z:$native_run/arm.txt" DLSSNR_LOG="Z:$native_run/adapter.log"',
    'export DLSSNR_INLINE_SHM='+shlex.quote('Z:'+str(settings_path)),
    'export DLSSNR_BIN_DIR='+shlex.quote('Z:'+str(home/'.local/share/dlssnr/binaries')),
    'export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}vulkan-1=n;dlssnr_system_vulkan=n;version=n,b"',
    'exec python3 '+shlex.quote(str(home/'.local/lib/dlssnr-fg/bin/dlssnr-sr-control'))+' launch -- '+shlex.quote(str(backup/'run-base'))+' "$@"',''])
(backup/'new-wrapper').write_text(script);(backup/'new-wrapper').chmod(0o755)
sources={game/'nvngx_dlss.dll':repo/'build/dlss-sr/nvngx_dlss.dll',game/'version.dll':proxy,game/'vulkan-1.dll':out/'vulkan-1.dll',
         game/'dlssnr_system_vulkan.dll':out/'dlssnr_system_vulkan.dll',wrapper:backup/'new-wrapper'}
changes=[]
for i,(dest,src) in enumerate(sources.items()):
    saved=backup/f'saved-{i}'
    if dest.exists():shutil.copy2(dest,saved)
    changes.append(dict(destination=str(dest),backup=str(saved) if saved.exists() else None,
                        old_sha256=sha(saved) if saved.exists() else None,new_sha256=sha(src)))
plan.update(installed_hashes={str(d):sha(s) for d,s in sources.items()},changes=changes,record=str(record))
(backup/'restore.json').write_text(json.dumps(plan,indent=2)+'\n')
(backup/'restore.py').write_text('''import hashlib,json,os,shutil,tempfile
from pathlib import Path
r=json.loads(Path(__file__).with_name('restore.json').read_text())
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
for c in r['changes']:
    d=Path(c['destination'])
    if d.is_symlink() or (d.exists() and sha(d) not in (c['old_sha256'],c['new_sha256'])):raise SystemExit('Changed file preserved: '+str(d))
for c in r['changes']:
    d=Path(c['destination'])
    if c['backup']:
        fd,tmp=tempfile.mkstemp(dir=d.parent);os.close(fd);shutil.copy2(c['backup'],tmp);os.replace(tmp,d)
    else:d.unlink(missing_ok=True)
Path(r['record']).unlink(missing_ok=True)
print('Previous working configuration restored for next game start')
''')
record.write_text(json.dumps(plan,indent=2)+'\n')
for dest,src in sources.items():
    fd,tmp=tempfile.mkstemp(dir=dest.parent);os.close(fd)
    try:shutil.copy2(src,tmp);os.replace(tmp,dest)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
print(json.dumps(plan,indent=2))
