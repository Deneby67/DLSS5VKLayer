#!/usr/bin/env python3
"""Offline NR color-input comparison. Never changes the installed game adapter."""
import argparse,hashlib,importlib.util,json,os,shutil,signal,subprocess,tempfile,time
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw

spec=importlib.util.spec_from_file_location('analysis',Path(__file__).with_name('analyze-nr-hdr.py'))
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def inputs(planes,meta):
    original,encoded,_,_=planes
    scale=float(meta[0]/meta[1]);exposed=np.maximum(original[:,:,:3],0)*scale
    def with_alpha(rgb):
        return np.concatenate((rgb,np.ones((*rgb.shape[:2],1),dtype=np.float32)),axis=2)
    yield 'baseline',encoded.copy(),False,'Exact captured NR input; game auxiliary alpha retained.'
    yield 'opaque-alpha',with_alpha(encoded[:,:,:3]),False,'Only alpha changes to 1.'
    for ev in (-4,-2,2):
        value=a.proxy(original[:,:,:3],scale*2**ev)
        yield f'exposure-{ev:+d}',with_alpha(value),False,f'Current proxy shifted {ev:+d} EV; hypothetical, not RDR2 tonemapping.'
    value=np.clip(exposed,0,1)
    yield 'clip-srgb',with_alpha(a.srgb(value)),False,'Exposed linear RGB clipped then sRGB encoded; highlights lost.'
    value=(exposed*(2.51*exposed+.03))/(exposed*(2.43*exposed+.59)+.14)
    yield 'filmic-fit',with_alpha(a.srgb(np.clip(value,0,1))),False,'Generic filmic rational curve; not the RDR2 tone mapper or full ACES.'
    yield 'linear-sdr',with_alpha(exposed/(1+exposed.max(axis=2,keepdims=True))),False,'Transfer-function control: omit sRGB encode, retain current compression.'
    yield 'native-hdr',with_alpha(exposed),True,'Exposed linear HDR with requested flags; unverified HDR contract, NOT capability-confirmed.'

def preview(rgb,hdr=False):
    if hdr:rgb=a.proxy(rgb,1)
    return Image.fromarray(np.rint(np.clip(rgb,0,1)*255).astype('u1'))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('snapshot',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--verbose',action='store_true',help='Record NGX parameter reads in the offline process')
    p.add_argument('--flags',type=lambda s:int(s,0),help='Isolated create-flag numbering control, 0..255')
    p.add_argument('--frames',type=int,default=16);p.add_argument('--cases',nargs='*')
    p.add_argument('--display-image',type=Path,action='append',default=[],help='Additional final-screen control, different frame and unknown NR toggle state')
    args=p.parse_args()
    if not 2<=args.frames<=32:p.error('frames must be between 2 and 32')
    if args.flags is not None and not 0<=args.flags<=255:p.error('flags must be 0..255')
    repo=Path(__file__).resolve().parents[1];out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if (out/'manifest.json').exists():p.error('Preserve existing study; choose a fresh output directory')
    m,meta,planes=a.load(args.snapshot)
    if not np.all(np.isfinite(meta)) or meta[0]<=0 or meta[1]<=0:p.error('Invalid source exposure')
    exe=repo/'build/nr-color-study/nr_color_study.exe'
    binaries=Path.home()/'.local/share/dlssnr/binaries'
    model=binaries/'nvngx_dlssnr.dll'
    if sha(model)!='e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e':p.error('NR model differs from the captured/tested build')
    steam=Path.home()/'.steam/debian-installation';prefix=repo/'build/nr-color-study/prefix';prefix.mkdir(exist_ok=True)
    temporary=Path(tempfile.mkdtemp(prefix='nr-color-study-'))
    env=os.environ.copy()
    for key in list(env):
        if key.startswith(('DLSSNR_','DLSSFG_','VK_','VKLayer_','WINE','PROTON_','STEAM_COMPAT_')) or key in ('LD_PRELOAD','NR_COLOR_STUDY_FLAGS'):env.pop(key,None)
    env.update(STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam),STEAM_COMPAT_DATA_PATH=str(prefix),
      PROTON_ENABLE_NVAPI='1',DLSSNR_SKIP_NVAPI='1',WINEDEBUG='-all',WINESTEAMNOEXEC='1',
      VKLayer_DLSS5='0',VK_LOADER_LAYERS_DISABLE='~implicit~',DLSSNR_ENABLE='0',DLSSNR_BIN_DIR='Z:'+str(binaries),
      VK_LAYER_PATH=str(repo/'build/fg/validation/root/usr/share/vulkan/explicit_layer.d'),
      VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',DLSSFG_VALIDATE='1')
    if args.flags is not None:env['NR_COLOR_STUDY_FLAGS']=str(args.flags)
    if args.verbose:env['DLSSNR_VERBOSE']='1'
    manifest=dict(version=1,explicit_create_flags=args.flags,source=str(args.snapshot),source_sha256=sha(args.snapshot/'hdr-snapshot.bin'),
       source_metadata_sha256=sha(args.snapshot/'hdr-snapshot.json'),model_sha256=sha(model),exe_sha256=sha(exe),
       frames=args.frames,settings=m,temporary=str(temporary),
       limitations='Static-image test: zero motion/jitter, null depth, fresh feature per variant. Not an exact replay of game history. Generic tone curves are not validated game tonemapping.',cases=[])
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    variants=list(inputs(planes,meta))
    manifest['display_controls']=[]
    for i,path in enumerate(args.display_image):
        im=Image.open(path).convert('RGB');im.thumbnail((m['width'],m['height']),Image.Resampling.LANCZOS)
        rgb=np.asarray(im,dtype='f4')/255
        rgba=np.concatenate((rgb,np.ones((*rgb.shape[:2],1),dtype='f4')),axis=2)
        name=f'display-{i}'
        variants.append((name,rgba,False,'User final-screen image (including overlays), different frame; original NR toggle state unknown. Resized with preserved aspect ratio.'))
        manifest['display_controls'].append(dict(name=name,path=str(path),sha256=sha(path),width=im.width,height=im.height))
    for name,pixels,hdr,description in variants:
        if args.cases and name not in args.cases:continue
        folder=out/name;folder.mkdir();run=temporary/name;run.mkdir()
        # float16 input is exactly what will reach the GPU; reject overflow.
        pixels=pixels.astype('<f2')
        if not np.isfinite(pixels).all():raise ValueError('Nonfinite input '+name)
        pixels.tofile(run/'input.rgba16f');shutil.copy2(run/'input.rgba16f',folder/'input.rgba16f')
        fields=[pixels.shape[1],pixels.shape[0],int(hdr),args.frames,m['sharpness'],m['intensity'],m['tone'],m['structure'],m['skin'],m['style'],m['preset'],m['automask']]
        (run/'settings.txt').write_text(' '.join(map(str,fields))+'\n');shutil.copy2(run/'settings.txt',folder/'settings.txt')
        local=env|{'DLSSNR_LOG':'Z:'+str(run/'nr.log')}
        cmd=[str(steam/'steamapps/common/SteamLinuxRuntime_4/_v2-entry-point'),'--verb=run','--',
             '/opt/Proton-11.0-2c-Zen5-BP18-Mimalloc/proton','run',str(exe)]
        start=time.monotonic()
        with (folder/'runner.log').open('w') as log:
            process=subprocess.Popen(cmd,cwd=run,env=local,stdout=log,stderr=log,start_new_session=True)
            try:code=process.wait(timeout=150)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
                code=124
        for f in run.glob('output-*.rgba16f'):shutil.copy2(f,folder/f.name)
        if (run/'nr.log').exists():shutil.copy2(run/'nr.log',folder/'nr.log')
        log=(folder/'nr.log').read_text(errors='replace') if (folder/'nr.log').exists() else ''
        result=dict(name=name,description=description,hdr_capability_confirmed=False,width=pixels.shape[1],height=pixels.shape[0],hdr=hdr,exit_code=code,seconds=time.monotonic()-start,
                    input_sha256=sha(folder/'input.rgba16f'),passed=code==0 and '[color-study] PASS' in log)
        if result['passed']:
            target=folder/f'output-{args.frames-1:02d}.rgba16f'
            output=np.fromfile(target,dtype='<f2').reshape(pixels.shape).astype('f4')
            rgb=pixels[:,:,:3].astype('f4');model=output[:,:,:3]
            result.update(output_sha256=sha(target),mae=float(np.abs(model-rgb).mean()),
                rms=float(np.sqrt(np.mean((model-rgb)**2))),output_min=float(model.min()),output_max=float(model.max()))
            preview(rgb,hdr).save(folder/'input.png');preview(model,hdr).save(folder/'output.png')
            diff=(a.proxy(model,1)-a.proxy(rgb,1)) if hdr else model-rgb
            preview(.5+diff*4).save(folder/'difference-x4.png')
            middle=np.fromfile(folder/f'output-{args.frames//2:02d}.rgba16f',dtype='<f2').reshape(pixels.shape).astype('f4')
            result['middle_to_last_mae']=float(np.abs(output[:,:,:3]-middle[:,:,:3]).mean())
        (folder/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        manifest['cases'].append(result);(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        print(name,'PASS' if result['passed'] else f'FAILED ({code})',flush=True)
    if not manifest['cases']:raise ValueError('No cases selected')
    print(out,flush=True)
if __name__=='__main__':main()
