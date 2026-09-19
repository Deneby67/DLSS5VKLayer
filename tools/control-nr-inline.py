#!/usr/bin/env python3
"""Session-bound NR-before-DLSS control; also the JSON backend for the Qt panel."""
import argparse
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path


def read(path):
    return path.read_text(errors='replace')


def live_sessions(home, proc_root=Path('/proc')):
    root=(home/'.local/state/dlssnr/native-inline').resolve()
    sessions=[]
    for proc in proc_root.iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if read(proc/'comm').strip().lower()!='rdr2.exe':
                continue
            env=(proc/'environ').read_bytes().split(b'\0')
            arm=next((s.split(b'=',1)[1].decode() for s in env if s.startswith(b'DLSSNR_ARM_FILE=Z:')),None)
            if not arm:
                continue
            path=Path(arm[2:]).resolve()
            if path.name!='arm.txt' or not path.parent.is_relative_to(root):
                continue
            log=path.with_name('adapter.log')
            if not log.exists():
                continue
            st=log.stat()
            for fd in (proc/'fd').iterdir():
                try:
                    fs=fd.stat()
                    if (fs.st_dev,fs.st_ino)==(st.st_dev,st.st_ino):
                        sessions.append((proc,log))
                        break
                except OSError:
                    pass
        except (OSError,UnicodeError,StopIteration):
            pass
    return sessions


def btrfs_path(proc, path):
    # Select the longest enclosing mount; /proc/maps can report the Btrfs
    # superblock device while stat reports a subvolume's anonymous device.
    try:
        candidates=[]
        for line in read(proc/'mountinfo').splitlines():
            left,right=line.split(' - ',1)
            raw=left.split()[4]
            mount=Path(re.sub(r'\\([0-7]{3})',lambda m:chr(int(m.group(1),8)),raw))
            if path.is_relative_to(mount):
                candidates.append((len(mount.parts),right.split()[0]))
        return bool(candidates) and max(candidates)[1]=='btrfs'
    except (OSError,ValueError,IndexError):
        return False


def mapped_install(proc, record):
    """A replaced DLL must take effect before this UI can enable processing."""
    mappings=read(proc/'maps').splitlines()
    for name in record.get('installed_hashes',{}):
        path=Path(name)
        if path.name not in ('vulkan-1.dll','dlssnr_system_vulkan.dll','version.dll'):
            continue
        st=path.stat()
        found=False
        for line in mappings:
            fields=line.split(None,5)
            if len(fields)!=6 or fields[5]!=name:
                continue
            major,minor=(int(s,16) for s in fields[3].split(':'))
            if int(fields[4])!=st.st_ino:
                continue
            if (major,minor)==(os.major(st.st_dev),os.minor(st.st_dev)):
                found=True
                break
            # Do not relax inode or exact path/deleted-file checks. Restrict the
            # device-number exception to Btrfs and the recorded installed bytes.
            if btrfs_path(proc,path):
                with path.open('rb') as f:
                    found=hashlib.file_digest(f,'sha256').hexdigest()==record['installed_hashes'][name]
                if found:break
        if not found:
            return False
    return bool(record.get('installed_hashes'))


def status(home, selected='auto', proc_root=Path('/proc')):
    result=dict(ready=False,can_enable=False,requested_on=False,last_observed_state='disabled',
                nr_recorded=False,status='not_installed',reason='Install the RDR2 integration first.',log='')
    record_path=home/'.config/dlssnr/native-inline-install.json'
    if not record_path.exists():
        return result
    record=json.loads(read(record_path))
    candidates=live_sessions(home,proc_root)
    if selected!='auto':
        wanted=Path(selected).resolve()
        root=(home/'.local/state/dlssnr/native-inline').resolve()
        if wanted.name!='adapter.log' or not wanted.is_relative_to(root):
            raise ValueError('Expected a private native-inline launch log')
        candidates=[(p,l) for p,l in candidates if l==wanted]
    if len(candidates)!=1:
        result.update(status='waiting' if not candidates else 'ambiguous',
                      reason='Start RDR2 with native DLSS and load a scene.' if not candidates else 'Multiple RDR2 sessions found; close the extra session.')
        return result
    proc,log=candidates[0]
    # Keep polling bounded. Even a pathological log must still allow Off.
    with log.open('rb') as f:
        f.seek(0,2);size=f.tell();oversized=size>1024*1024
        f.seek(0)
        if oversized:
            prefix=f.read(65536);f.seek(-524288,2)
            text=(prefix+b'\n'+f.read()).decode(errors='replace')
        else:text=f.read().decode(errors='replace')
    result.update(linux_pid=int(proc.name),log=str(log),status='waiting_sr',reason='Waiting for the first native DLSS evaluation in the scene.')
    ready=set(re.findall(r'\[nr-inline\] arm-ready pid=(\d+) token=(\d+);',text))
    if len(ready)!=1:
        return result
    winpid,token=next(iter(ready))
    arm=log.with_name('arm.txt')
    request=read(arm).strip() if arm.exists() else ''
    states=re.findall(r'\[nr-inline\] arm-state (enabled|disabled)',text)
    observed=states[-1] if states else 'disabled'
    result.update(ready=True,windows_pid=int(winpid),token=token,
                  requested_on=request==f'{winpid} {token} 1',last_observed_state=observed,
                  nr_recorded='recorded NR-before-SR calls=' in text)
    if not mapped_install(proc,record):
        result.update(status='restart',reason='Integration updated. Save and restart RDR2 to load the installed DLLs.')
        return result
    if oversized:
        result.update(status='blocked',reason='Adapter log grew unexpectedly large. Turn NR off and inspect the log before retrying.')
        return result
    # Terminal failures remain relevant after a toggle; transient bypasses only
    # explain the current activation, not a successful later interval.
    terminal=re.findall(r'\[nr-inline\] (disabled[^\r\n]*|initialization failed[^\r\n]*)',text)
    current=text[text.rfind('[nr-inline] arm-state enabled'):] if observed=='enabled' else ''
    rendering=re.findall(r'\[nr-inline\] Rendering state (enabled|disabled|unavailable)',text)
    bypasses=re.findall(r'\[nr-inline\] bypass: ([^\r\n]+)',current)
    result['can_enable']=not bool(terminal)
    if terminal:
        result.update(status='blocked',reason=terminal[-1]+' — restart RDR2 before retrying.')
    elif result['requested_on']!=(observed=='enabled'):
        result.update(status='pending',reason='Waiting for the next DLSS evaluation to apply the request.')
    elif observed=='disabled':
        result.update(status='off',reason='Ready. NR is off for this session.')
    elif rendering and rendering[-1]!='enabled':
        result.update(status='bypassed',reason='Rendering settings are '+rendering[-1]+'. Enable Neural rendering or press F2 in the game.')
    elif bypasses:
        result.update(status='bypassed',reason=bypasses[-1])
    elif result['nr_recorded']:
        result.update(status='recording',reason='NR → DLSS commands recorded. Check image quality and FPS in the game.')
    else:
        result.update(status='starting',reason='NR enabled; waiting for the first processed frame.')
    return result


def control(home, selected, mode, proc_root=Path('/proc'), expected_token=None):
    s=status(home,selected,proc_root)
    if mode=='status':
        return s
    if selected=='auto':
        raise ValueError('Select the current log explicitly before changing a session')
    if not s['ready'] or (mode=='on' and not s['can_enable']):
        raise ValueError(s['reason'])
    if expected_token is not None and expected_token!=s['token']:
        raise ValueError('Session changed; refresh the game connection')
    arm=Path(s['log']).with_name('arm.txt')
    fd,tmp=tempfile.mkstemp(dir=arm.parent,prefix='.arm-')
    try:
        with os.fdopen(fd,'w') as f:
            f.write(f'{s["windows_pid"]} {s["token"]} {int(mode=="on")}\n')
            f.flush();os.fsync(f.fileno())
        os.replace(tmp,arm)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
    return status(home,selected,proc_root)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log',nargs='?',default='auto',help='auto for discovery, or an explicit adapter.log')
    p.add_argument('mode',choices=('status','on','off'),default='status',nargs='?')
    p.add_argument('--token',help='Expected current session token')
    a=p.parse_args()
    try:
        print(json.dumps(control(Path.home(),a.log,a.mode,expected_token=a.token),indent=2))
    except (OSError,ValueError,KeyError) as e:
        print(json.dumps(dict(status='error',reason=str(e),ready=False,can_enable=False)))
        return 1
    return 0


if __name__=='__main__':
    raise SystemExit(main())
