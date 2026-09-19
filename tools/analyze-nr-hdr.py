#!/usr/bin/env python3
"""Compare same-frame raw NR output against the existing HDR residual bridge.

PNG comparisons use the bridge's own display proxy, NOT the game's final tone
mapping. Original FP16 pixels and exposure float are preserved in the dump.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw


def linear(x):
    x=np.maximum(x,0)
    return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)


def srgb(x):
    x=np.maximum(x,0)
    return np.where(x<=.0031308,12.92*x,1.055*x**(1/2.4)-.055)


def proxy(x,scale):
    x=np.maximum(x,0)*scale
    return srgb(x/(1+x.max(axis=2,keepdims=True)))


def load(directory):
    m=json.loads((directory/'hdr-snapshot.json').read_text())
    w,h=m['width'],m['height']
    if m['version']!=1 or m['format']!='rgba16f-le' or not (64<=w<=4096 and 64<=h<=2160):
        raise ValueError('Unsupported snapshot format/extent')
    size=(directory/'hdr-snapshot.bin').stat().st_size
    if size!=m['bytes'] or m['first']<16 or m['stride']<w*h*8 or m['first']+4*m['stride']!=size:
        raise ValueError('Incomplete/invalid snapshot size')
    raw=np.memmap(directory/'hdr-snapshot.bin',dtype=np.uint8,mode='r')
    meta=np.frombuffer(raw[:16],dtype='<f4').copy()
    if meta[3]!=1:raise ValueError('Missing GPU metadata marker')
    planes=[np.ndarray((h,w,4),dtype='<f2',buffer=raw,offset=m['first']+i*m['stride']).astype(np.float32) for i in range(4)]
    return m,meta,planes


def metrics(planes,meta):
    original,before,after,restored=[x[:,:,:3] for x in planes]
    exposure,pre,sharpness,_=meta
    valid=bool(np.isfinite(exposure) and exposure>0 and np.isfinite(pre) and pre>0)
    result={'exposure':float(exposure) if np.isfinite(exposure) else None,
            'pre_exposure':float(pre) if np.isfinite(pre) else None,'exposure_valid':valid,
            'effective_sharpness':float(sharpness),'nonfinite_components':[int((~np.isfinite(x)).sum()) for x in planes],
            'nr_output_outside_sdr_fraction':float(((after<0)|(after>1)).mean()),
            'alpha_changed_fraction':float((planes[0][:,:,3]!=planes[3][:,:,3]).mean())}
    if not valid or any(result['nonfinite_components']):return result,None
    scale=float(exposure/pre)
    exposed=np.maximum(original,0)*scale
    peak=exposed.max(axis=2,keepdims=True)
    before_linear=linear(before);after_linear=linear(np.clip(after,0,1))
    edit=after_linear-before_linear
    # Reference the actual shader formula, not an alternative/inverse compositor.
    expected=original+np.clip(edit*(1+peak)/scale,-np.maximum(abs(original)*2,.1/scale),np.maximum(abs(original)*2,.1/scale))
    expected=np.clip(expected,-65504,65504).astype(np.float16).astype(np.float32)
    reencoded=proxy(restored,scale)
    retained=linear(reencoded)-before_linear
    energy=float(np.sum(edit.astype(np.float64)**2))
    result.update(scale=scale,model_srgb_mae=float(np.abs(after-before).mean()),
                  restored_proxy_srgb_mae=float(np.abs(reencoded-before).mean()),
                  hdr_changed_pixel_fraction=float(np.any(original!=restored,axis=2).mean()),
                  input_proxy_reference_mae=float(np.abs(proxy(original,scale)-before).mean()),
                  restored_shader_reference_mae=float(np.abs(restored-expected).mean()),
                  restored_shader_reference_max_error=float(np.abs(restored-expected).max()),
                  retained_linear_edit_projection=float(np.sum(retained.astype(np.float64)*edit)/energy) if energy>1e-12 else None)
    rows=[]
    for low,high in ((0,1),(1,4),(4,16),(16,float('inf'))):
        mask=(peak[:,:,0]>=low)&(peak[:,:,0]<high)
        e=edit[mask].astype(np.float64);r=retained[mask].astype(np.float64)
        denom=float(np.sum(e*e))
        rows.append(dict(exposed_peak_range=[low,high if np.isfinite(high) else None],pixels=int(mask.sum()),
                         retained_edit_projection=float(np.sum(r*e)/denom) if denom>1e-12 else None))
    result['brightness_bins']=rows
    return result,(before,np.clip(after,0,1),reencoded,np.clip(.5+(reencoded-np.clip(after,0,1))*4,0,1))


def analyze(directory,output=None):
    m,meta,planes=load(directory)
    report,previews=metrics(planes,meta)
    report.update(frame=m['frame'],width=m['width'],height=m['height'],token=m['token'],
                  scope='One frame before native SR; display proxies are not the final game tone mapping.')
    output=output or directory/'analysis';output.mkdir(parents=True,exist_ok=True)
    (output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    if previews is not None:
        names=['NR input (HDR proxy)','Raw NR output (clamped SDR)','Restored HDR (same proxy)','Restored - raw NR, x4 + 0.5']
        thumbs=[]
        for i,(pixels,name) in enumerate(zip(previews,names)):
            im=Image.fromarray(np.rint(np.clip(pixels,0,1)*255).astype(np.uint8))
            im.save(output/f'{i+1}.png');im.thumbnail((960,600));thumbs.append(im)
        tw,th=thumbs[0].size
        montage=Image.new('RGB',(tw*2,(th+32)*2),(24,24,24));draw=ImageDraw.Draw(montage)
        for i,im in enumerate(thumbs):
            x=(i%2)*tw;y=(i//2)*(th+32)
            montage.paste(im,(x,y+32));draw.text((x+8,y+8),names[i],fill='white')
        montage.save(output/'comparison.png')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args();print(json.dumps(analyze(a.directory,a.output),indent=2))
if __name__=='__main__':main()
