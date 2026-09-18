#!/usr/bin/env python3
"""Install the experimental layer separately; atomically update per-user selection."""
import argparse, datetime, hashlib, json, os, shutil, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__); p.add_argument('--layer',type=Path,required=True); a=p.parse_args()
repo=Path(__file__).resolve().parents[1]; home=Path.home()
manifest=home/'.config/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.x86_64.json'
wrapper=home/'.local/bin/dlssfg-capture-rdr2'
marker=home/'.config/dlssnr/fg-discovery.enabled'
if not a.layer.is_file() or not manifest.is_file() or not wrapper.is_file(): p.error('Existing per-user layer and Steam wrapper required')
m=json.loads(manifest.read_text()); layer=m['layer']
if layer['name']!='VK_LAYER_NV_dlssnr': p.error('Unexpected layer name')
old=Path(layer['library_path'])
if not old.is_file(): p.error('Existing selected library missing')
backup=home/'.local/share/dlssnr/backups'/('discovery-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
backup.mkdir(parents=True,mode=0o700)
paths=[manifest,wrapper,marker]; restore=[]
for i,dest in enumerate(paths):
    saved=backup/str(i)
    if dest.exists(): shutil.copy2(dest,saved)
    restore.append({'destination':str(dest),'backup':str(saved) if saved.exists() else None})
shutil.copy2(old,backup/'previous-layer.so')
(backup/'restore.json').write_text(json.dumps(restore,indent=2))
(backup/'restore.py').write_text('''#!/usr/bin/env python3
import json, os, shutil, tempfile
from pathlib import Path
for entry in json.loads(Path(__file__).with_name('restore.json').read_text()):
    dest=Path(entry['destination'])
    if entry['backup']:
        fd,tmp=tempfile.mkstemp(dir=dest.parent); os.close(fd)
        shutil.copy2(entry['backup'],tmp); os.replace(tmp,dest)
    else: dest.unlink(missing_ok=True)
print('Restored. Restart the game to use the previous layer.')
''')
def install(source,dest):
    dest.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(dir=dest.parent); os.close(fd)
    try: shutil.copy2(source,tmp); os.replace(tmp,dest)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)
digest=hashlib.sha256(a.layer.read_bytes()).hexdigest()
out=home/'.local/lib/dlssnr-fg/layer'/('libVkLayer_NV_dlssnr-'+digest[:16]+'.so')
install(a.layer,out)
layer['library_path']=str(out)
modified=backup/'new-manifest.json'; modified.write_text(json.dumps(m,indent=2)+'\n')
install(modified,manifest)
install(repo/'tools/steam-fg-capture.sh',wrapper); wrapper.chmod(0o755)
marker.parent.mkdir(parents=True,exist_ok=True)
marker.write_text('Metadata only; remove this file or set DLSSFG_DISCOVERY=0 to disable.\n')
print('Layer:',out,'SHA256:',hashlib.sha256(out.read_bytes()).hexdigest())
print('Rollback: python3',backup/'restore.py')
print('Takes effect on the next normal Steam launch; running games are untouched.')
