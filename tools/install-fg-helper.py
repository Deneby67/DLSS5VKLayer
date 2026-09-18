#!/usr/bin/env python3
"""Back up the installed NR helper and install an isolated experimental helper.

Run as the desktop user, never root. NVIDIA binaries stay outside the repository.
The system package, NR DLL, Vulkan layer and game launch options are not modified.
"""
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil

repo=Path(__file__).resolve().parents[1]
home=Path.home()
if os.getuid()==0: raise SystemExit('Run as the desktop user, not root')
build=repo/'build/fg'
system=Path('/usr/lib64/dlssnr')
target=home/'.local/lib/dlssnr-fg'
profile_dir=home/'.config/dlssnr/fg-profiles'
bin_dir=home/'.local/bin'
desktop=home/'.local/share/applications/dlssnr.desktop'
mutations=[target,profile_dir,bin_dir/'dlssnr-helper',bin_dir/'dlssnr-gui',desktop]
required=[build/'dlssnr_helper.exe',build/'fg_probe.exe',build/'binaries/nvngx_dlssg.dll',
          system/'helper/dlssnr_helper.exe',system/'bin/dlssnr-shmctl',
          Path('/usr/bin/dlssnr-helper'),Path('/usr/bin/dlssnr-gui')]
for f in required:
    if not f.is_file(): raise SystemExit(f'Missing required file: {f}')
digest=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
expected='ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82'
if digest(required[2])!=expected: raise SystemExit('FG DLL differs from pinned Streamline SDK v2.14.1 binary')
if target.exists(): raise SystemExit('Experimental installation already exists; restore it before reinstalling')
stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
backup=home/'.local/share/dlssnr/backups'/('before-fg-'+stamp)
backup.mkdir(parents=True,mode=0o700,exist_ok=False)
def copy(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True)
    if src.is_dir() and not src.is_symlink(): shutil.copytree(src,dst,symlinks=True)
    else: shutil.copy2(src,dst,follow_symlinks=False)
entries=[]
for i,path in enumerate(mutations):
    saved=backup/'previous'/str(i)
    existed=path.exists() or path.is_symlink()
    if existed: copy(path,saved)
    entries.append(dict(path=str(path),saved=str(saved),existed=existed))
# These snapshots provide the original binaries/configuration for inspection;
# rollback only restores paths changed by this installer, leaving later GUI
# settings and the package manager's system files alone.
for path in [*required[3:],home/'.config/dlssnr',home/'.local/lib/dlssnr',
             home/'.config/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.x86_64.json']:
    if path.exists(): copy(path,backup/'snapshot'/str(path).lstrip('/'))
(backup/'restore.json').write_text(json.dumps(entries,indent=2)+'\n')
restore='''#!/usr/bin/env python3
import datetime,json,shutil
from pathlib import Path
root=Path(__file__).resolve().parent
saved_changes=root/('after-update-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
saved_changes.mkdir(exist_ok=False)
for i,e in enumerate(json.loads((root/'restore.json').read_text())):
    p=Path(e['path'])
    if p.exists() or p.is_symlink(): shutil.move(str(p),str(saved_changes/str(i)))
    if e['existed']:
        old=Path(e['saved']); p.parent.mkdir(parents=True,exist_ok=True)
        if old.is_dir() and not old.is_symlink(): shutil.copytree(old,p,symlinks=True)
        else: shutil.copy2(old,p,follow_symlinks=False)
print('Previous installation restored. Restart the Helper window; GUI settings were preserved.')
'''
(backup/'restore.py').write_text(restore)
(backup/'restore.py').chmod(0o700)
for sub in ['helper','bin','binaries','profiles']:
    (target/sub).mkdir(parents=True,exist_ok=True)
shutil.copy2(build/'dlssnr_helper.exe',target/'helper/dlssnr_helper.exe')
shutil.copy2(build/'fg_probe.exe',target/'helper/fg_probe.exe')
shutil.copy2(system/'bin/dlssnr-shmctl',target/'bin/dlssnr-shmctl')
shutil.copy2(required[2],target/'binaries/nvngx_dlssg.dll')
shutil.copy2(build/'binaries/manifest.json',target/'binaries/manifest.json')
profile_dir.mkdir(parents=True,exist_ok=True)
if not (profile_dir/'rdr2.json').exists(): shutil.copy2(repo/'profiles/fg/rdr2.json',profile_dir/'rdr2.json')
shutil.copy2(repo/'profiles/fg/README.md',target/'profiles/README.md')
bin_dir.mkdir(parents=True,exist_ok=True)
shared='''export DLSSNR_INSTALL_DIR="$HOME/.local/lib/dlssnr-fg"
export DLSSNR_HELPER_EXE="$DLSSNR_INSTALL_DIR/helper/dlssnr_helper.exe"
export DLSSFG_PROFILE="${DLSSFG_PROFILE:-Z:$HOME/.config/dlssnr/fg-profiles/rdr2.json}"
'''
(bin_dir/'dlssnr-helper').write_text('#!/usr/bin/env bash\nset -e\n'+shared+'exec /usr/bin/dlssnr-helper "$@"\n')
(bin_dir/'dlssnr-gui').write_text('#!/usr/bin/env bash\nset -e\n'+shared+
    'export DLSSNR_HELPER_CLI="$HOME/.local/bin/dlssnr-helper"\nexec /usr/bin/dlssnr-gui "$@"\n')
for name in ['dlssnr-helper','dlssnr-gui']: (bin_dir/name).chmod(0o755)
desktop.parent.mkdir(parents=True,exist_ok=True)
desktop.write_text('[Desktop Entry]\nType=Application\nName=DLSS5VKLayer Helper\n'
    'Comment=NR helper with experimental FG diagnostics and JSON profile loader\n'
    f'Exec="{bin_dir}/dlssnr-gui"\nTerminal=false\nCategories=System;Settings;\n'
    'Icon=preferences-desktop\nStartupNotify=false\n')
record=dict(backup=str(backup),installed=str(target),
    helper_sha256=digest(target/'helper/dlssnr_helper.exe'),
    previous_helper_sha256=digest(system/'helper/dlssnr_helper.exe'),
    fg_dll_sha256=expected,game_fg_enabled=False)
(backup/'installation.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
