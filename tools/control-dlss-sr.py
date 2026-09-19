#!/usr/bin/env python3
"""Persist DLSS SR presets and freeze them into the next RDR2 launch environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

PRESETS={'game':{'auto':0},'transformer':{'J':10,'K':11,'L':12,'M':13},'cnn':{'E':5,'F':6}}
DEFAULT={'schema':1,'model':'transformer','preset':'K'}
LATEST_SHA='3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'

def validated(value):
    if not isinstance(value,dict) or value.get('schema')!=1:raise ValueError('Unsupported DLSS SR configuration')
    model=value.get('model');preset=value.get('preset')
    if model not in PRESETS or preset not in PRESETS[model]:raise ValueError('Preset does not belong to the selected DLSS model')
    return {'schema':1,'model':model,'preset':preset}

def config_path(home):return home/'.config/dlssnr/dlss-sr.json'

def load(home):
    path=config_path(home)
    return validated(json.loads(path.read_text())) if path.exists() else dict(DEFAULT)

def save(home,model,preset):
    config=validated(dict(schema=1,model=model,preset=preset))
    path=config_path(home);path.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(dir=path.parent,prefix='.sr-',text=True)
    try:
        with os.fdopen(fd,'w') as f:
            json.dump(config,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
        os.replace(tmp,path)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)
    return config

def live(home,proc_root):
    root=str(home/'.local/state/dlssnr/native-inline')+'/'
    found=[]
    for proc in proc_root.iterdir():
        if not proc.name.isdigit():continue
        try:
            if (proc/'comm').read_text().strip().lower()!='rdr2.exe':continue
            env=dict(part.split('=',1) for part in (proc/'environ').read_bytes().decode(errors='replace').split('\0') if '=' in part)
            arm=env.get('DLSSNR_ARM_FILE','').removeprefix('Z:').replace('\\','/')
            if not arm.startswith(root) or Path(arm).name!='arm.txt':continue
            selected=env.get('DLSSNR_SR_PRESET','0')
            row={'pid':int(proc.name),'preset':int(selected) if selected.isdigit() else -1}
            report=Path(arm).with_name('sr-status.json')
            if report.exists():
                report_data=json.loads(report.read_text())
                if report_data.get('schema')==1:row['ngx']=report_data
            found.append(row)
        except (OSError,ValueError):continue
    return found

def status(home,proc_root=Path('/proc')):
    config=load(home);selected=PRESETS[config['model']][config['preset']]
    record=home/'.config/dlssnr/native-inline-install.json'
    install=json.loads(record.read_text()) if record.exists() else {}
    source=install.get('sr_source',{})
    installed=source.get('sha256')==LATEST_SHA
    sessions=live(home,proc_root)
    result={'config':config,'installed':installed,'dll_version':source.get('version',''),
        'dll_sha256':source.get('sha256',''),'sessions':sessions,'restart_required':False}
    message='Saved for the next game launch. F2 continues to control NR only.'
    if not installed:message='Install the DLSS SR integration first.'
    elif len(sessions)>1:message='Multiple RDR2 sessions found; settings apply to the next launch.'
    elif sessions:
        session=sessions[0];ngx=session.get('ngx',{})
        if session['preset']!=selected:
            result['restart_required']=True;message='Saved. Restart RDR2 to apply the selected model/preset.'
        elif ngx.get('result') not in (None,1):message='NGX feature creation failed. See the game log.'
        elif selected and ngx.get('preset')==selected and ngx.get('preset_reads',0)>0 and ngx.get('result')==1:
            message='NGX read the selected preset and created DLSS successfully.'
        elif not selected:message='Game-selected preset. Latest DLSS DLL remains loaded.'
        else:message='Waiting for native DLSS feature creation; load a scene with DLSS enabled.'
    result['message']=message
    return result

def launch_environment(home,environment):
    config=load(home)
    record=json.loads((home/'.config/dlssnr/native-inline-install.json').read_text())
    source=record.get('sr_source',{})
    if source.get('sha256')!=LATEST_SHA:raise ValueError('DLSS SR installation is not validated')
    game=home/'.steam/debian-installation/steamapps/common/Red Dead Redemption 2/nvngx_dlss.dll'
    with game.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    if digest!=source['sha256']:raise ValueError('Game DLSS DLL changed; preserving it. Reinstall the validated SR integration.')
    env=dict(environment);env['DLSSNR_SR_PRESET']=str(PRESETS[config['model']][config['preset']])
    arm=env.get('DLSSNR_ARM_FILE','')
    if not arm.startswith('Z:') or not arm.endswith('/arm.txt'):raise ValueError('Missing private RDR2 launch session')
    env['DLSSNR_SR_STATUS']=arm[:-len('arm.txt')]+'sr-status.json'
    return env

def main():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest='action',required=True)
    sub.add_parser('status')
    write=sub.add_parser('save');write.add_argument('--model',required=True);write.add_argument('--preset',required=True)
    run=sub.add_parser('launch');run.add_argument('command',nargs=argparse.REMAINDER)
    args=p.parse_args();home=Path.home()
    try:
        if args.action=='save':save(home,args.model,args.preset)
        if args.action=='launch':
            command=args.command[1:] if args.command and args.command[0]=='--' else args.command
            if not command:raise ValueError('Missing game launch command')
            env=launch_environment(home,os.environ)
            os.execvpe(command[0],command,env)
        print(json.dumps(status(home),indent=2));return 0
    except (OSError,ValueError,KeyError) as e:
        if args.action=='launch':print('DLSS SR: '+str(e),file=__import__('sys').stderr)
        else:print(json.dumps({'error':str(e)}))
        return 1

if __name__=='__main__':raise SystemExit(main())
