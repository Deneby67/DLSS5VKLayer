#!/usr/bin/env python3
"""Relate recorded resources to shader layouts; never infer execution or camera semantics."""
import argparse, collections, json
from pathlib import Path

def relate(session, report):
    images={}; views={}; buffers={}; layouts={}; sets={}; pipelines=[]
    shader_info={s['sha256']:s for s in report['shaders']}
    candidates={x['id']:x for k in ('depth_attachment_candidates','motion_format_candidates_unverified') for x in report[k]}
    # Layout identifiers and resource identifiers are session generations, not raw handles.
    # Exact layout identity is intentionally required. Untracked updates/command state
    # cannot be recovered by assuming that structurally similar layouts were bound.
    with (session/'events.jsonl').open() as f:
        for line in f:
            try:r=json.loads(line)
            except json.JSONDecodeError:break
            event=r['event']
            if event=='image': images[r['id']]=r
            elif event=='image_view': views[r['id']]=r
            elif event=='buffer': buffers[r['id']]=r
            elif event=='descriptor_set': sets[r['id']]=r
            elif event=='pipeline_layout': layouts[r['id']]=r
            elif event in ('graphics_pipeline','compute_pipeline'): pipelines.append(r)
    matrix_bindings=collections.defaultdict(list)
    for p in pipelines:
        layout=layouts.get(p['layout']['id'])
        if not layout:continue
        for stage in p.get('stages',[p.get('stage',{})]):
            shader=shader_info.get(stage.get('sha256'))
            if not shader:continue
            for d in shader['descriptors']:
                if not d['matrix_members'] or d['set']>=len(layout['sets']):continue
                sid=layout['sets'][d['set']]['id']
                if not sid:continue
                info={'shader_sha256':shader['sha256'],'pipeline_id':p['id'],'stage':stage['stage'],
                      'set':d['set'],'binding':d['binding'],'matrix_members':d['matrix_members']}
                matrix_bindings[(sid,d['binding'])].append(info)
    matrix_updates={}; image_updates={}; copies=0; budget=[]; render_passes={}; framebuffers=[]
    def observe(table,key,r,base):
        if key not in table: table[key]={**base,'writes':0,'first_present_marker':r['present_marker'],
                                      'last_present_marker':r['present_marker'],'examples':[]}
        item=table[key]; item['writes']+=1; item['last_present_marker']=r['present_marker']
        if len(item['examples'])<3:item['examples'].append({'seq':r['seq'],'set_id':r['set']['id'],'binding':r['binding']})
        return item
    with (session/'events.jsonl').open() as f:
        for line in f:
            try:r=json.loads(line)
            except json.JSONDecodeError:break
            event=r['event']
            if 'budget' in event:budget.append(r)
            if event=='descriptor_copy':copies+=1
            if event=='render_pass':render_passes[r['id']]=r
            if event=='framebuffer':framebuffers.append(r)
            if event!='descriptor_write':continue
            ds=sets.get(r['set']['id']); sid=ds['layout']['id'] if ds else 0
            for index,res in enumerate(r['resources']):
                # Spill writes across adjacent bindings need full layout traversal;
                # these provisional links only describe the first binding/element.
                if index!=0:continue
                if 'view' in res:
                    view=views.get(res['view']['id']); iid=view['image']['id'] if view else 0
                    if iid in candidates:
                        observe(image_updates,(iid,r['binding'],sid),r,{'image':candidates[iid],
                            'set_layout_id':sid,'binding':r['binding'],'descriptor_type':r['type'],
                            'view_id':res['view']['id'],'verified_motion_or_depth':False})
                elif 'buffer' in res and (sid,r['binding']) in matrix_bindings:
                    bid=res['buffer']['id']; b=buffers.get(bid)
                    if not b:continue
                    start=res['offset']; size=res['range']
                    if size==0xffffffffffffffff:size=max(0,b['bytes']-start)
                    key=(bid,start,size,sid,r['binding'])
                    info=matrix_bindings[(sid,r['binding'])]
                    observe(matrix_updates,key,r,{'buffer_id':bid,'buffer_bytes':b['bytes'],
                        'offset':start,'range':size,'set_layout_id':sid,'binding':r['binding'],
                        'descriptor_type':r['type'],'dynamic_offset_unknown':r['type'] in (8,9),
                        'shader_layout_examples':info[:3],'shader_layout_match_count':len(info),
                        'camera_verified':False})
    attachment_links=[]
    for fb in framebuffers:
        rp=render_passes.get(fb['render_pass']['id'])
        if not rp:continue
        for sub in rp['subpasses']:
            index=sub['depth']
            if index>=len(fb['views']):continue
            v=views.get(fb['views'][index]['id']); iid=v['image']['id'] if v else 0
            if iid in candidates: attachment_links.append({'framebuffer_id':fb['id'],'render_pass_id':rp['id'],
                'image':candidates[iid],'attachment_index':index,'executed':None})
    return {'schema':1,'validated_profile':False,'budget_events':budget,
        'limits':['exact recorded layout identity only','descriptor writes do not prove binding or draw execution',
                  'first resource element only; spill bindings not traversed','descriptor copies not propagated',
                  'dynamic offsets not recorded','resource contents and GPU completion unknown'],
        'descriptor_copy_count':copies,'matrix_buffer_candidates':sorted(matrix_updates.values(),key=lambda x:-x['writes']),
        'sampled_image_candidates':sorted(image_updates.values(),key=lambda x:-x['writes']),
        'framebuffer_depth_candidates':attachment_links}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('session',type=Path)
    p.add_argument('--report',type=Path,required=True); p.add_argument('--output',type=Path,required=True)
    a=p.parse_args(); out=relate(a.session,json.loads(a.report.read_text()))
    a.output.write_text(json.dumps(out,indent=2)+'\n')
    print('Matrix buffer candidates:',len(out['matrix_buffer_candidates']),
          'sampled image candidates:',len(out['sampled_image_candidates']),
          'framebuffer depth candidates:',len(out['framebuffer_depth_candidates']))
