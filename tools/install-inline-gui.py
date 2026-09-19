#!/usr/bin/env python3
"""Install the NR control GUI locally with an atomic replacement and exact rollback."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import tempfile

repo=Path(__file__).resolve().parents[1]
home=Path.home();base=home/'.local/lib/dlssnr-fg/bin'
wrapper=home/'.local/bin/dlssnr-gui'
old=wrapper.read_text()
executable=base/'dlssnr-gui'
controller=base/'dlssnr-inline-control'
if 'exec /usr/bin/dlssnr-gui "$@"' in old:
    new=old.replace('exec /usr/bin/dlssnr-gui "$@"','exec '+shlex.quote(str(executable))+' "$@"')
elif 'exec '+shlex.quote(str(executable))+' "$@"' in old:
    new=old
else:
    raise SystemExit('Unrecognized GUI launcher; preserving it')
backup=home/'.local/share/dlssnr/backups'/('inline-gui-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
backup.mkdir(parents=True,mode=0o700);base.mkdir(parents=True,exist_ok=True)
(backup/'new-wrapper').write_text(new);(backup/'new-wrapper').chmod(0o755)
sources={executable:repo/'build/gui-native/dlssnr_gui',controller:repo/'tools/control-nr-inline.py',wrapper:backup/'new-wrapper'}
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
changes=[]
for i,(dest,src) in enumerate(sources.items()):
    if dest.is_symlink():raise SystemExit('Symlink preserved: '+str(dest))
    saved=backup/f'saved-{i}'
    if dest.exists():shutil.copy2(dest,saved)
    changes.append(dict(destination=str(dest),backup=str(saved) if saved.exists() else None,
                        old_sha256=sha(saved) if saved.exists() else None,new_sha256=sha(src)))
(backup/'restore.json').write_text(json.dumps(changes,indent=2)+'\n')
(backup/'restore.py').write_text('''import hashlib,json,os,shutil,tempfile
from pathlib import Path
rows=json.loads(Path(__file__).with_name('restore.json').read_text())
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
for row in rows:
 p=Path(row['destination'])
 if p.is_symlink() or (p.exists() and sha(p) not in (row['new_sha256'],row['old_sha256'])):raise SystemExit('Changed file preserved: '+str(p))
for row in rows:
 p=Path(row['destination'])
 if row['backup']:
  fd,tmp=tempfile.mkstemp(dir=p.parent);os.close(fd);shutil.copy2(row['backup'],tmp);os.replace(tmp,p)
 else:p.unlink(missing_ok=True)
print('Previous GUI restored')
''')
for dest,src in sources.items():
    fd,tmp=tempfile.mkstemp(dir=dest.parent);os.close(fd)
    try:
        shutil.copy2(src,tmp);os.chmod(tmp,0o755);os.replace(tmp,dest)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
    assert sha(dest)==sha(src)
print(json.dumps(dict(launcher=str(wrapper),controller=str(controller),rollback=str(backup/'restore.py')),indent=2))
