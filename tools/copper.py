#!/usr/bin/env python3
"""Copper V2: deterministic project orchestration and KiCad 9 adapters.
Engineering authority lives in copper-core. This file resolves local libraries,
places/routes candidate geometry, emits native KiCad files, and runs native gates.
No shell execution, network requests, or LLM engineering decisions.
"""
from __future__ import annotations
import argparse, copy, csv, hashlib, heapq, json, math, os, re, shutil, subprocess, sys, uuid
from pathlib import Path
from hwc_kicad import (Atom as A, Quoted as Q, parse_sexpr, dump_sexpr, direct, one,
                      head, text_at, discover_symbol_dir, discover_footprint_dir, discover_cli)
ROOT=Path(__file__).resolve().parents[1]
NS=uuid.UUID('aa7c164c-f2ec-5244-b7eb-6726f0ee52d8')
# Build statuses. Any analysis warning means required data was missing (unknown rating,
# height or geometry), so the build is UNVERIFIED rather than PASS.
STATUS_PASS='INTERNAL_CHECKS_PASS_NATIVE_KICAD_REQUIRED'
STATUS_UNVERIFIED='INTERNAL_CHECKS_UNVERIFIED_MISSING_DATA'
STATUS_DRAFT='DRAFT_FAILED_CHECKS'
def sha256_file(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def uid(s): return str(uuid.uuid5(NS,s))
def sx(tag,*v): return [A(tag),*v]
def number(v): return A(f'{float(v):.9g}')
def write_tree(path,tree):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_text(dump_sexpr(tree)+'\n',encoding='utf-8');tmp.replace(path)
def load(path): return parse_sexpr(Path(path).read_text(encoding='utf-8'))
def core_path(explicit=None):
    choices=[explicit,os.environ.get('COPPER_CORE')]+[str(ROOT/x/y) for x in ['build','build/Debug','build/Release','build-local','build-desktop'] for y in ['copper-core','copper-core.exe']]
    for p in choices:
        if p and Path(p).is_file():return str(Path(p).resolve())
    raise FileNotFoundError('Build copper-core first or set COPPER_CORE to its executable path.')
def core_run(project,core=None,mode='analyze',extra=()):
    result=subprocess.run([core_path(core),mode,str(Path(project).resolve()),*map(str,extra)],capture_output=True,text=True,timeout=120)
    if result.returncode not in (0,2) or not result.stdout.strip():raise ValueError(result.stderr or f'core failed: {result.returncode}')
    return (json.loads(result.stdout) if mode=='analyze' else result.stdout),result.returncode

def catalogue(path=None):
    tree=load(path or ROOT/'profiles/default.parts');return copy.deepcopy(direct(tree,'part'))
def new_project(args):
    parts=catalogue(args.catalogue);template=args.template
    if template=='divider':
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('R2','resistor',args.r2,[('1','OUT'),('2','GND')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    elif template=='rc':
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('C1','capacitor',args.c,[('1','OUT'),('2','GND')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    elif template=='rl':
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('L1','inductor',args.l,[('1','OUT'),('2','GND')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    else:
        comps=[('V1','voltage',args.vin,[('1','VIN'),('2','GND')],(0,0)),('R1','resistor',args.r,[('1','VIN'),('2','OUT')],(16,12)),('D1','diode',0,[('1','GND'),('2','OUT')],(24,12)),('J1','header3',0,[('1','VIN'),('2','OUT'),('3','GND')],(8,12))]
    tree=sx('copper_project',sx('version',A('2')),sx('name',Q(template)),sx('nets',A('GND'),A('VIN'),A('OUT')),sx('board',*map(number,[40,30,1.6,.035,2])),sx('parts',*parts),sx('components',*[sx('component',A(ref),sx('part',A(part)),sx('value',number(value)),sx('pins',*[sx(pin,A(net)) for pin,net in pins]),sx('at',number(pos[0]),number(pos[1]),A('0'))) for ref,part,value,pins,pos in comps]))
    if args.rules:
        rules=load(args.rules)
        if head(rules)!='rules':raise ValueError('Rule profile must have (rules ...) root')
        tree.append(rules)
    if args.target is not None:tree.append(sx('targets',sx('voltage',A('OUT'),number(args.target-args.target_tolerance),number(args.target+args.target_tolerance))))
    if Path(args.out).exists() and not args.force:raise FileExistsError(f'{args.out} exists; use --force to replace it')
    write_tree(args.out,tree);print(f'Created {args.out}. Component values use SI units; ratings come from the selected catalogue.')
    return 0

def component_data(tree):
    parts={str(p[1]):p for p in direct(one(tree,'parts'),'part')};result=[]
    for c in direct(one(tree,'components'),'component'):
        part=parts[text_at(c,'part')];at=one(c,'at') or [None,0,0,0]
        result.append(dict(ref=str(c[1]),part=part,kind=text_at(part,'kind'),value=float(text_at(c,'value',default='0')),symbol=text_at(part,'symbol'),footprint=text_at(part,'footprint'),pins={str(x[0]):str(x[1]) for x in one(c,'pins')[1:]},x=float(at[1]),y=float(at[2]),rotation=float(at[3])))
    return result

def safe_lib_id(value):
    if not re.fullmatch(r'[A-Za-z0-9_+.-]+:[A-Za-z0-9_+().,~ -]+',value):raise ValueError(f'Invalid library ID: {value}')
    return value.split(':',1)
def resolve_symbol(libdir,libid,cache,seen=()):
    if libid in cache:return copy.deepcopy(cache[libid])
    if libid in seen:raise ValueError('Cyclic symbol inheritance')
    lib,name=safe_lib_id(libid);tree=load(libdir/f'{lib}.kicad_sym')
    child=next((copy.deepcopy(s) for s in direct(tree,'symbol') if s[1]==name),None)
    if child is None:raise ValueError(f'Symbol not found: {libid}')
    parent=text_at(child,'extends')
    if parent:
        base=resolve_symbol(libdir,f'{lib}:{parent}',cache,(*seen,libid))
        # Inheritance is flattened; child properties override by key and graphics by unit suffix.
        props={str(v[1]):v for v in direct(base,'property')};props.update({str(v[1]):v for v in direct(child,'property')})
        units={str(v[1]).rsplit('_',2)[-2:][0]+'_'+str(v[1]).rsplit('_',1)[-1]:v for v in direct(base,'symbol')}
        for v in direct(child,'symbol'):units['_'.join(str(v[1]).rsplit('_',2)[-2:])]=v
        other={head(v):v for v in base[2:] if isinstance(v,list) and head(v) not in ('property','symbol','extends')}
        other.update({head(v):v for v in child[2:] if isinstance(v,list) and head(v) not in ('property','symbol','extends')})
        child=sx('symbol',Q(libid),*other.values(),*props.values(),*units.values())
    child[1]=Q(libid)
    for unit in direct(child,'symbol'):unit[1]=Q(name+'_'+'_'.join(str(unit[1]).rsplit('_',2)[-2:]))
    cache[libid]=copy.deepcopy(child);return child

def symbol_pins(symbol):
    result=[]
    for unit in direct(symbol,'symbol'):
        match=re.search(r'_(\d+)_(\d+)$',str(unit[1]))
        if not match:raise ValueError('Malformed symbol unit')
        un,style=map(int,match.groups())
        if un>1:raise ValueError('Multi-unit symbols require an explicit unit mapping; V2 exporter refuses to guess')
        if style>1:continue
        result.extend(direct(unit,'pin'))
    numbers=[text_at(p,'number') for p in result]
    if len(set(numbers))!=len(numbers):raise ValueError('Duplicate/stacked symbol pins need explicit handling')
    return result

def effects(hidden=False):
    e=sx('effects',sx('font',sx('size',A('1.27'),A('1.27'))))
    if hidden:e.append(sx('hide',A('yes')))
    return e

def schematic(tree,components,symbol_dir,stem):
    rootid=uid(stem+'/schematic');lib=sx('lib_symbols');cache={}
    sch=sx('kicad_sch',sx('version',A('20250114')),sx('generator',Q('copper_v2')),sx('uuid',Q(rootid)),sx('paper',Q('A4')),lib)
    physical=[c for c in components if c['kind'] not in ('V','I')]
    for index,c in enumerate(physical):
        definition=resolve_symbol(symbol_dir,c['symbol'],cache);pins=symbol_pins(definition)
        if set(c['pins'])!={text_at(p,'number') for p in pins}:raise ValueError(f"{c['ref']}: project pins differ from symbol pins")
        if not any(s[1]==c['symbol'] for s in lib[1:]):lib.append(definition)
        x,y=50.8+(index%4)*50.8,50.8+(index//4)*50.8
        instance=sx('symbol',sx('lib_id',Q(c['symbol'])),sx('at',number(x),number(y),A('0')),sx('unit',A('1')),sx('exclude_from_sim',A('no')),sx('in_bom',A('yes')),sx('on_board',A('yes')),sx('dnp',A('no')),sx('uuid',Q(uid(stem+'/'+c['ref']))))
        for key,value,offset,hide in [('Reference',c['ref'],-5,False),('Value',f"{c['value']:.9g}",5,False),('Footprint',c['footprint'],0,True)]:
            instance.append(sx('property',Q(key),Q(value),sx('at',number(x+7),number(y+offset),A('0')),effects(hide)))
        instance.append(sx('instances',sx('project',Q(stem),sx('path',Q('/'+rootid),sx('reference',Q(c['ref'])),sx('unit',A('1'))))))
        for pin in pins:
            no=text_at(pin,'number');at=one(pin,'at');px,py=x+float(at[1]),y-float(at[2]);net=c['pins'][no]
            instance.append(sx('pin',Q(no),sx('uuid',Q(uid(stem+'/'+c['ref']+'/pin/'+no)))))
            # Labels sit exactly on actual pin endpoints; symbol coordinates have inverted Y.
            sch.append(sx('label',Q(net),sx('at',number(px),number(py),A('0')),effects(),sx('uuid',Q(uid(stem+'/'+c['ref']+'/label/'+no)))))
        sch.append(instance)
    sch.append(sx('sheet_instances',sx('path',Q('/'),sx('page',Q('1')))))
    sch.append(sx('embedded_fonts',A('no')))
    return sch

def rotate(x,y,angle):
    a=math.radians(angle);return (x*math.cos(a)+y*math.sin(a),-x*math.sin(a)+y*math.cos(a))
def footprint_geometry(c,fpdir,netids,stem):
    lib,name=safe_lib_id(c['footprint']);fp=load(fpdir/f'{lib}.pretty'/f'{name}.kicad_mod')
    if head(fp)!='footprint':raise ValueError('Expected KiCad footprint')
    if one(fp,'zone') or one(fp,'embedded_files'):raise ValueError('Footprint zones/embedded files are outside V2 geometry coverage')
    for obj in fp:
        if isinstance(obj,list) and head(obj)!='pad' and text_at(obj,'layer') in ('F.Cu','B.Cu','*.Cu') and head(obj)!='layer':raise ValueError('Footprint copper graphics are not represented by the supported pad geometry')
    if any(one(fp,key) for key in ('solder_mask_margin','solder_paste_margin','solder_paste_ratio')):raise ValueError('Per-footprint mask/paste overrides require explicit process support')
    fp[1]=Q(c['footprint']);fp[:]=[x for x in fp if not isinstance(x,list) or head(x) not in ('at','uuid','path')]
    fp.extend([sx('at',number(c['x']),number(c['y']),number(c['rotation'])),sx('uuid',Q(uid(stem+'/'+c['ref']+'/footprint'))),sx('path',Q('/'+uid(stem+'/schematic')+'/'+uid(stem+'/'+c['ref'])))])
    found_ref=False
    for prop in direct(fp,'property'):
        if prop[1]=='Reference':prop[2]=Q(c['ref']);found_ref=True
        if prop[1]=='Value':prop[2]=Q(f"{c['value']:.9g}")
    for t in direct(fp,'fp_text'):
        if t[1]=='reference':t[2]=Q(c['ref']);found_ref=True
        if t[1]=='value':t[2]=Q(f"{c['value']:.9g}")
    if not found_ref:fp.append(sx('fp_text',A('reference'),Q(c['ref']),sx('at',A('0'),A('-3')),sx('layer',Q('F.SilkS')),effects()))
    pads=[];mapped=set();geo=[]
    for pad in direct(fp,'pad'):
        no=str(pad[1]);kind=str(pad[2]);shape=str(pad[3]);at=one(pad,'at');size=one(pad,'size');drill=one(pad,'drill');layers=one(pad,'layers')
        if any(one(pad,key) for key in ('solder_mask_margin','solder_paste_margin','solder_paste_margin_ratio','chamfer','chamfer_ratio')):raise ValueError('Pad mask/paste/chamfer overrides are outside supported geometry')
        if not at or not size or not layers:raise ValueError('Pad lacks position, size or layers')
        if (kind=='thru_hole') != bool(drill):raise ValueError('Pad drill/type mismatch')
        if kind not in ('smd','thru_hole') or shape not in ('circle','rect','roundrect','oval'):raise ValueError(f"{c['ref']}:{no}: unsupported pad type/shape {kind}/{shape}")
        if no not in c['pins']:raise ValueError(f"{c['ref']}: physical pad {no} missing from electrical map")
        if drill and (len(drill)!=2 or not re.fullmatch(r'[0-9.]+',str(drill[1]))):raise ValueError('Slotted/offset drills require native geometry support')
        if any(str(x) not in ('F.Cu','F.Paste','F.Mask','*.Cu','*.Mask') for x in layers[1:]):raise ValueError('Only top-side SMD or through-hole pads are supported')
        dx,dy=rotate(float(at[1]),float(at[2]),c['rotation']);x,y=c['x']+dx,c['y']+dy
        angle=c['rotation']+(float(at[3]) if len(at)>3 else 0)
        if angle%90:raise ValueError('Pad rotation must be a multiple of 90 degrees')
        w,h=float(size[1]),float(size[2]);w,h=(h,w) if round(angle/90)%2 else (w,h)
        if w<=0 or h<=0:raise ValueError('Pad dimensions must be positive')
        circular=shape=='circle' and abs(w-h)<1e-9
        # C retains the exact axis-aligned core and rounded-corner radius.
        net=c['pins'][no];mapped.add(no);pad[:]=[v for v in pad if not isinstance(v,list) or head(v) not in ('net','uuid')]
        pad.extend([sx('net',A(str(netids[net])),Q('/'+net)),sx('uuid',Q(uid(stem+'/'+c['ref']+'/pad/'+no+'/'+str(len(pads)))))])
        pads.append(dict(ref=c['ref'],pin=no,net=net,x=x,y=y,w=w,h=h,through=kind=='thru_hole'))
        # diameter is conservative for legacy edge/annular reporting, exact adapter uses width/height.
        diameter=min(w,h)
        corner=min(w,h)/2 if shape=='oval' else (float(text_at(pad,'roundrect_rratio',default='0'))*min(w,h) if shape=='roundrect' else 0)
        if corner<0 or corner>min(w,h)/2:raise ValueError('Invalid rounded pad radius')
        geo.append(sx('pad',A(c['ref']),A(no),A(net),*map(number,[x,y,diameter,float(drill[1]) if drill else 0,0,w,h,0 if circular else 1,corner])))
    if mapped!=set(c['pins']):raise ValueError(f"{c['ref']}: symbol pin/footprint pad mismatch")
    # Use actual courtyard extents; a footprint lacking a courtyard is not silently invented.
    pts=[]
    for obj in fp:
        if isinstance(obj,list) and text_at(obj,'layer')=='F.CrtYd':
            for key in ('start','end','center'):
                at=one(obj,key)
                if at:
                    dx,dy=rotate(float(at[1]),float(at[2]),c['rotation']);pts.append((c['x']+dx,c['y']+dy))
            if head(obj) not in ('fp_rect','fp_line'):raise ValueError('Non-rectilinear courtyard requires explicit geometry support')
    if not pts:raise ValueError(f"{c['ref']}: footprint has no supported courtyard")
    lo=[min(p[i] for p in pts) for i in range(2)];hi=[max(p[i] for p in pts) for i in range(2)]
    geo.append(sx('body',A(c['ref']),*map(number,[(lo[0]+hi[0])/2,(lo[1]+hi[1])/2,hi[0]-lo[0],hi[1]-lo[1],float(text_at(c['part'],'height_mm',default='0'))])))
    # Regenerate all nested UUIDs deterministically; avoid duplicate library UUIDs.
    def uuids(n,path=''):
        if isinstance(n,list):
            for i,v in enumerate(n):
                if isinstance(v,list) and head(v)=='uuid':v[1]=Q(uid(stem+'/'+c['ref']+'/nested/'+path+str(i)))
                else:uuids(v,path+str(i)+'/')
    uuids(fp);return fp,pads,geo

def route(pads,width,height,trace_width,clearance,edge,via_diameter=.6):
    """Deterministic 2-layer Manhattan A* candidate router, with bounded search.
    Conservative obstacle envelopes; C exact geometry and KiCad DRC judge result.
    """
    grid=.25;nx=round(width/grid);ny=round(height/grid);traces=[];vias=[];occupied=[{},{}]
    radius=trace_width/2+clearance
    def node(p):return (round(p['x']/grid),round(p['y']/grid),0)
    def blocked(x,y,z,net,via=False):
        xx,yy=x*grid,y*grid;r=(via_diameter/2+clearance) if via else radius
        if min(xx,yy,width-xx,height-yy)<edge+r:return True
        for p in pads:
            if p['net']==net or (z==1 and not p['through']):continue
            if abs(xx-p['x'])<=p['w']/2+r and abs(yy-p['y'])<=p['h']/2+r:return True
        reach=math.ceil((r+trace_width/2)/grid)
        for a in range(x-reach,x+reach+1):
            for b in range(y-reach,y+reach+1):
                other=occupied[z].get((a,b))
                if other and other!=net and math.hypot(a-x,b-y)*grid<r+trace_width/2:return True
        for vx,vy,vnet in vias:
            if vnet!=net and math.hypot(xx-vx,yy-vy)<r+via_diameter/2:return True
        return False
    nets=sorted({p['net'] for p in pads})
    for net in nets:
        targets=[p for p in pads if p['net']==net]
        if len(targets)<2:continue
        joined=[targets.pop(0)]
        while targets:
            _,i,j=min((abs(a['x']-b['x'])+abs(a['y']-b['y']),i,j) for i,a in enumerate(joined) for j,b in enumerate(targets))
            a,b=joined[i],targets.pop(j);start,goal=node(a),node(b)
            if blocked(*start,net) or blocked(*goal,net):raise ValueError(f'Cannot escape pad on net {net}; edit placement or constraints')
            heap=[(0,0,start)];cost={start:0};prev={};found=False
            while heap:
                _,g,u=heapq.heappop(heap)
                if g!=cost[u]:continue
                if u==goal:found=True;break
                if len(cost)>250000:break
                x,y,z=u
                for v,extra in [((x+1,y,z),1),((x-1,y,z),1),((x,y+1,z),1),((x,y-1,z),1),((x,y,1-z),20)]:
                    xx,yy,zz=v
                    if not (0<=xx<=nx and 0<=yy<=ny) or blocked(xx,yy,zz,net):continue
                    if zz!=z and (blocked(x,y,z,net,True) or blocked(x,y,zz,net,True)):continue
                    ng=g+extra
                    if ng<cost.get(v,1e30):cost[v]=ng;prev[v]=u;h=abs(xx-goal[0])+abs(yy-goal[1])+(20 if zz!=goal[2] else 0);heapq.heappush(heap,(ng+h,ng,v))
            if not found:raise ValueError(f'Router could not connect {net}; no route is claimed')
            path=[goal]
            while path[-1]!=start:path.append(prev[path[-1]])
            path.reverse()
            for x,y,z in path:occupied[z][x,y]=net
            # Compress collinear grid runs to avoid thousands of segments.
            points=[path[0]]
            for k in range(1,len(path)-1):
                d1=tuple(path[k][q]-path[k-1][q] for q in range(3));d2=tuple(path[k+1][q]-path[k][q] for q in range(3))
                if d1!=d2:points.append(path[k])
            if len(path)>1:points.append(path[-1])
            traces.append((net,a['x'],a['y'],start[0]*grid,start[1]*grid,trace_width,0))
            for u,v in zip(points,points[1:]):
                if u[2]!=v[2]:vias.append((u[0]*grid,u[1]*grid,net))
                else:traces.append((net,u[0]*grid,u[1]*grid,v[0]*grid,v[1]*grid,trace_width,0 if u[2]==0 else 2))
            traces.append((net,goal[0]*grid,goal[1]*grid,b['x'],b['y'],trace_width,0));joined.append(b)
    return [t for t in traces if math.hypot(t[1]-t[3],t[2]-t[4])>1e-9],sorted(set(vias))

def rule(tree,key,default):return float(text_at(one(tree,'rules'),key,default=str(default)))
def export_project(args):
    project=Path(args.project).resolve();tree=load(project);out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    for marker in ('manifest.json','verification.json'):
        (out/marker).unlink(missing_ok=True)
    report,rc=core_run(project,args.core)
    if rc and not getattr(args,'draft',False):raise ValueError('Electrical analysis failed; run check to inspect the findings before layout.')
    components=component_data(tree);nets=list(map(str,one(tree,'nets')[1:]));netids={n:i+1 for i,n in enumerate(nets)}
    symdir=discover_symbol_dir(args.symbol_dir);fpdir=discover_footprint_dir(args.footprint_dir)
    stem='design';sch=schematic(tree,components,symdir,stem)
    dims=list(map(float,(one(tree,'board') or [None,40,30,1.6,.035,2])[1:]));w,h,thick,copper,layers=dims
    board=sx('kicad_pcb',sx('version',A('20241229')),sx('generator',Q('copper_v2')),sx('general',sx('thickness',number(thick))),sx('paper',Q('A4')),sx('layers',*[sx(str(i),Q(name),A(kind)) for i,name,kind in [(0,'F.Cu','signal'),(2,'B.Cu','signal'),(3,'B.Adhes','user'),(5,'F.SilkS','user'),(7,'B.SilkS','user'),(9,'F.Mask','user'),(11,'B.Mask','user'),(13,'Dwgs.User','user'),(25,'Edge.Cuts','user'),(31,'F.CrtYd','user'),(33,'B.CrtYd','user'),(35,'F.Fab','user'),(37,'B.Fab','user'),(1,'F.Adhes','user'),(19,'F.Paste','user'),(21,'B.Paste','user')]]),sx('setup',sx('pad_to_mask_clearance',number(rule(tree,'mask_expansion_mm',.05)))),sx('net',A('0'),Q('')))
    board.extend(sx('net',A(str(i)),Q('/'+n)) for n,i in netids.items())
    board.append(sx('gr_rect',sx('start',A('0'),A('0')),sx('end',number(w),number(h)),sx('stroke',sx('width',A('.05')),sx('type',A('default'))),sx('fill',A('none')),sx('layer',Q('Edge.Cuts')),sx('uuid',Q(uid('outline')))))
    pads=[];geometry=sx('geometry')
    for c in components:
        if c['kind'] in ('V','I'):continue
        fp,pp,gg=footprint_geometry(c,fpdir,netids,stem);board.append(fp);pads.extend(pp);geometry.extend(gg)
    traces=[];vias=[]
    if not args.no_route:traces,vias=route(pads,w,h,rule(tree,'trace_width_mm',.2),rule(tree,'clearance_mm',.2),rule(tree,'edge_mm',.3),rule(tree,'via_diameter_mm',.6))
    for i,(net,x1,y1,x2,y2,width,layer) in enumerate(traces):
        board.append(sx('segment',sx('start',number(x1),number(y1)),sx('end',number(x2),number(y2)),sx('width',number(width)),sx('layer',Q('F.Cu' if layer==0 else 'B.Cu')),sx('net',A(str(netids[net]))),sx('uuid',Q(uid('track/'+str(i))))))
        geometry.append(sx('trace',A(net),*map(number,[x1,y1,x2,y2,width,layer])))
    for i,(x,y,net) in enumerate(vias):
        diameter=rule(tree,'via_diameter_mm',.6);drill=rule(tree,'drill_mm',.3)
        board.append(sx('via',sx('at',number(x),number(y)),sx('size',number(diameter)),sx('drill',number(drill)),sx('layers',Q('F.Cu'),Q('B.Cu')),sx('net',A(str(netids[net]))),sx('uuid',Q(uid('via/'+str(i))))))
        geometry.append(sx('via',A(net),*map(number,[x,y,diameter,drill])))
    tree[:]=[x for x in tree if not isinstance(x,list) or head(x)!='geometry'];tree.append(geometry)
    write_tree(out/'resolved.copper',tree)
    report,rc=core_run(out/'resolved.copper',args.core)
    write_tree(out/'design.kicad_sch',sch);write_tree(out/'design.kicad_pcb',board)
    # Snapshot the exact selected source libraries and footprints, with local tables.
    # Native ERC/DRC can now resolve links without modifying global KiCad settings.
    symtable=sx('sym_lib_table',sx('version',A('7')));fptable=sx('fp_lib_table',sx('version',A('7')))
    for library in sorted({safe_lib_id(c['symbol'])[0] for c in components if c['kind'] not in ('V','I')}):
        target=out/'libraries/symbols'/f'{library}.kicad_sym';target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(symdir/f'{library}.kicad_sym',target)
        symtable.append(sx('lib',sx('name',Q(library)),sx('type',Q('KiCad')),sx('uri',Q('${KIPRJMOD}/libraries/symbols/'+library+'.kicad_sym')),sx('options',Q('')),sx('descr',Q('Exact local library snapshot'))))
    for library in sorted({safe_lib_id(c['footprint'])[0] for c in components if c['kind'] not in ('V','I')}):
        target=out/'libraries/footprints'/f'{library}.pretty';target.mkdir(parents=True,exist_ok=True)
        for c in components:
            if c['kind'] in ('V','I'):continue
            lib,foot=safe_lib_id(c['footprint'])
            if lib==library:shutil.copyfile(fpdir/f'{lib}.pretty'/f'{foot}.kicad_mod',target/f'{foot}.kicad_mod')
        fptable.append(sx('lib',sx('name',Q(library)),sx('type',Q('KiCad')),sx('uri',Q('${KIPRJMOD}/libraries/footprints/'+library+'.pretty')),sx('options',Q('')),sx('descr',Q('Exact selected footprints'))))
    write_tree(out/'sym-lib-table',symtable);write_tree(out/'fp-lib-table',fptable)
    (out/'design.kicad_pro').write_text(json.dumps({'meta':{'filename':'design.kicad_pro','version':1}},indent=2)+'\n')
    (out/'libraries/NOTICE.txt').write_text('Library files are exact snapshots from the selected local KiCad libraries. KiCad official library content is licensed CC-BY-SA-4.0 with the KiCad libraries exception; https://www.kicad.org/libraries/license/ . Custom libraries retain their original authorship and licensing. No ownership transfer is implied.\n')
    shutil.copyfile(ROOT/'docs/v2/KICAD_LIBRARY_LICENSE.md',out/'libraries/KICAD_LIBRARY_LICENSE.md')
    dru='(version 1)\n'
    for rule_name,constraint,limit in [('clearance','clearance',rule(tree,'clearance_mm',.2)),('width','track_width',rule(tree,'trace_width_mm',.2)),('edge','edge_clearance',rule(tree,'edge_mm',.3)),('drill','hole_size',rule(tree,'drill_mm',.3)),('annular','annular_width',rule(tree,'annular_mm',.15)),('hole_gap','hole_clearance',rule(tree,'hole_gap_mm',.25))]:
        dru+=f'(rule "Copper_{rule_name}" (constraint {constraint} (min {limit}mm)))\n'
    (out/'design.kicad_dru').write_text(dru)

    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    with (out/'bom.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.writer(f);writer.writerow(['Reference','Part','Value_SI','Symbol','Footprint','Power_W','Voltage_V','Height_mm','Manufacturer','MPN','Datasheet'])
        for c in components:
            if c['kind'] not in ('V','I'):writer.writerow([c['ref'],c['part'][1],c['value'],c['symbol'],c['footprint'],text_at(c['part'],'power_w'),text_at(c['part'],'voltage_v'),text_at(c['part'],'height_mm'),text_at(c['part'],'manufacturer'),text_at(c['part'],'mpn'),text_at(c['part'],'datasheet')])
    lines=['Copper V2 calculation report',f'DC solved: {report["dc_solved"]}',f'Errors: {report["errors"]}; warnings: {report["warnings"]}']
    for n in report['nodes']:lines.append(f'{n["name"]}: {n["voltage_v"]} V')
    for c in report['calculations']:lines.append(f'{c["ref"]}: {c["formula"]}; V={c["voltage_v"]} V, R={c["resistance_ohm"]} ohm, I={c["current_a"]} A, P={c["power_w"]} W')
    lines.extend(f'{v["severity"].upper()} {v["rule"]} {v["object"]}: {v["message"]}' for v in report['issues']);lines.append(report['coverage'])
    (out/'CALCULATIONS.txt').write_text('\n'.join(lines)+'\n')
    # Hash the actual source, resolved IR and outputs, never a statement of certification.
    files=['resolved.copper','design.kicad_sch','design.kicad_pcb','design.kicad_pro','design.kicad_dru','sym-lib-table','fp-lib-table','bom.csv','report.json']
    files+=sorted(str(p.relative_to(out)) for p in (out/'libraries').rglob('*') if p.is_file())
    unverified=sorted({v['rule'] for v in report['issues'] if v['severity']!='error'})
    try:source_path=os.path.relpath(project,out)
    except ValueError:source_path=str(project)  # Windows: different drive
    manifest={'schema':2,'source_sha256':sha256_file(project),'source_path':Path(source_path).as_posix(),'status':STATUS_DRAFT if rc else (STATUS_UNVERIFIED if unverified else STATUS_PASS),'unverified_rules':unverified,'files':{name:sha256_file(out/name) for name in files},'symbols':str(symdir),'footprints':str(fpdir)}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # Lightweight exact board preview for external viewing, independent of desktop renderer.
    import html
    svg=[f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="-2 -2 {w+4} {h+4}"><rect x="0" y="0" width="{w}" height="{h}" rx=".3" fill="#122c2b" stroke="#74d3bb" stroke-width=".1"/>']
    for net,x1,y1,x2,y2,width,layer in traces:svg.append(f'<path d="M{x1} {y1} L{x2} {y2}" stroke="{"#ef8069" if layer==0 else "#6a9edc"}" stroke-width="{width}"/>')
    for p in pads:svg.append(f'<rect x="{p["x"]-p["w"]/2}" y="{p["y"]-p["h"]/2}" width="{p["w"]}" height="{p["h"]}" fill="#edca7c"><title>{html.escape(p["ref"]+"."+p["pin"]+" "+p["net"])}</title></rect>')
    svg.append('</svg>');(out/'board.svg').write_text(''.join(svg))
    print(f'{manifest["status"]}: {out}\n{len(pads)} pads, {len(traces)} tracks, {len(vias)} vias; {report["errors"]} errors, {report["warnings"]} warnings.');return rc

def overall_status(statuses):
    return 'FAIL' if 'FAIL' in statuses else 'UNVERIFIED' if 'UNVERIFIED' in statuses else 'PASS'
def native_gates(out,cli):
    """KiCad 9.x ERC/DRC/netlist plus independent pin/net parity. Returns (version, gates)."""
    version=None;error='KiCad CLI not found; set --kicad-cli'
    if cli:
        try:version=subprocess.run([cli,'version'],capture_output=True,text=True,timeout=30)
        except (OSError,subprocess.SubprocessError) as exc:error=f'Cannot run KiCad CLI {cli}: {exc}'
    if version is None or version.returncode:
        return None,[{'gate':'kicad_cli','status':'UNVERIFIED','message':error if version is None else 'kicad-cli version failed: '+version.stderr.strip()}]
    found=version.stdout.strip()
    if not re.match(r'^9\.',found):return found,[{'gate':'kicad_version','status':'UNVERIFIED','message':f'Native validation targets KiCad 9.x only; found {found}'}]
    gates=[]
    for label,cmd in [('erc',['sch','erc','--format','json','--exit-code-violations','-o',str(out/'erc.json'),str(out/'design.kicad_sch')]),('drc',['pcb','drc','--format','json','--all-track-errors','--schematic-parity','--exit-code-violations','-o',str(out/'drc.json'),str(out/'design.kicad_pcb')]),('netlist',['sch','export','netlist','--format','kicadxml','-o',str(out/'netlist.xml'),str(out/'design.kicad_sch')])]:
        r=subprocess.run([cli,*cmd],capture_output=True,text=True,timeout=180);gates.append({'gate':label,'status':'PASS' if r.returncode==0 else 'FAIL','returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
    # Compare all electrical physical-pin mappings against KiCad's own extracted netlist.
    if gates[-1]['returncode']==0:
        import xml.etree.ElementTree as ET
        root=ET.parse(out/'netlist.xml').getroot();actual={}
        for net in root.findall('./nets/net'):
            for node in net.findall('node'):actual[(node.attrib['ref'],node.attrib['pin'])]=net.attrib['name'].removeprefix('/')
        expected={(c['ref'],pin):net for c in component_data(load(out/'resolved.copper')) if c['kind'] not in ('V','I') for pin,net in c['pins'].items()}
        gates.append({'gate':'netlist_parity','status':'PASS' if expected==actual else 'FAIL','returncode':0 if expected==actual else 2,'message':'Matches electrical IR' if expected==actual else f'Pin/net mismatch: expected {expected}, got {actual}'})
    else:gates.append({'gate':'netlist_parity','status':'UNVERIFIED','message':'KiCad netlist export failed; parity not compared'})
    return found,gates
def verify(args):
    out=Path(args.out).resolve();marker=out/'verification.json'
    marker.unlink(missing_ok=True)  # A failed run must never leave an earlier PASS behind.
    manifest=json.loads((out/'manifest.json').read_text());checks=[];gates=[];version=None
    def check(name,status,message):checks.append({'check':name,'status':status,'message':message})
    changed=sorted(name for name,digest in manifest['files'].items() if not (out/name).is_file() or sha256_file(out/name)!=digest)
    check('output_hashes','FAIL' if changed else 'PASS',f'Changed since build: {changed}; rebuild' if changed else 'All built files match the manifest')
    if not changed:
        project=getattr(args,'project',None) or (manifest.get('source_path') and str(out/manifest['source_path']))
        if not project:check('source_freshness','UNVERIFIED','Manifest has no source path; pass --project')
        elif not Path(project).is_file():check('source_freshness','UNVERIFIED',f'Source not found: {project}')
        elif sha256_file(project)!=manifest['source_sha256']:check('source_freshness','FAIL',f'{project} changed since build; rebuild')
        else:check('source_freshness','PASS','Source matches the build')
        build=manifest.get('status')
        check('build_status',{STATUS_PASS:'PASS',STATUS_UNVERIFIED:'UNVERIFIED'}.get(build,'FAIL'),f'Build status {build}')
        # Re-run the C engine on the built IR; the stored report must be reproduced exactly.
        try:fresh,rc=core_run(out/'resolved.copper',getattr(args,'core',None))
        except FileNotFoundError as exc:check('internal_reanalysis','UNVERIFIED',str(exc))
        except ValueError as exc:check('internal_reanalysis','FAIL',str(exc))
        else:
            stored=json.loads((out/'report.json').read_text())
            if fresh!=stored:check('internal_reanalysis','FAIL','Re-analysis differs from report.json')
            elif rc or fresh['errors']:check('internal_reanalysis','FAIL',f'{fresh["errors"]} internal errors')
            else:check('internal_reanalysis','PASS','Re-analysis reproduces report.json with no errors')
            missing=sorted({v['rule'] for v in fresh['issues'] if v['severity']!='error'})
            check('data_completeness','UNVERIFIED' if missing else 'PASS',f'Missing data: {missing}' if missing else 'No unknown ratings, heights or geometry')
        version,gates=native_gates(out,discover_cli(args.kicad_cli))
    status=overall_status([c['status'] for c in checks+gates])
    result={'status':status,'pass':status=='PASS','kicad_version':version,'checks':checks,'gates':gates,'hashes':manifest['files']}
    marker.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));return 0 if status=='PASS' else 2


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('new');p.add_argument('template',choices=['divider','rc','rl','diode']);p.add_argument('--out',required=True);p.add_argument('--catalogue');p.add_argument('--rules');p.add_argument('--vin',type=float,default=5);p.add_argument('--r',type=float,default=10000);p.add_argument('--r2',type=float,default=20000);p.add_argument('--c',type=float,default=1e-6);p.add_argument('--l',type=float,default=.01);p.add_argument('--target',type=float);p.add_argument('--target-tolerance',type=float,default=.05);p.add_argument('--force',action='store_true')
    for cmd in ['check','transient']:
        p=sub.add_parser(cmd);p.add_argument('project');p.add_argument('--core')
        if cmd=='transient':p.add_argument('--dt',type=float,default=1e-4);p.add_argument('--steps',type=int,default=1000);p.add_argument('--out',required=True)
    p=sub.add_parser('build');p.add_argument('project');p.add_argument('--out',default='out/v2');p.add_argument('--core');p.add_argument('--symbol-dir');p.add_argument('--footprint-dir');p.add_argument('--no-route',action='store_true');p.add_argument('--draft',action='store_true',help='Allow electrical failures to produce an explicitly failed draft, never a verified package')
    for cmd in ['verify','fabricate']:
        p=sub.add_parser(cmd);p.add_argument('--out',default='out/v2');p.add_argument('--kicad-cli');p.add_argument('--core');p.add_argument('--project',help='Source .copper to check for freshness (default: path recorded at build)')
    p=sub.add_parser('classify');p.add_argument('text');p.add_argument('--core')
    p=sub.add_parser('catalogue');p.add_argument('--file');p.add_argument('--search',default='')
    p=sub.add_parser('part-add');p.add_argument('--file',required=True);p.add_argument('--id',required=True);p.add_argument('--kind',choices=list('RCLDVIJU'),required=True);p.add_argument('--symbol',default='');p.add_argument('--footprint',default='');p.add_argument('--power',type=float,default=0);p.add_argument('--voltage',type=float,default=0);p.add_argument('--tolerance',type=float,default=0);p.add_argument('--height',type=float,default=0);p.add_argument('--manufacturer',default='');p.add_argument('--mpn',default='');p.add_argument('--datasheet',default='')

    args=parser.parse_args(argv)
    try:
        if args.command=='new':return new_project(args)
        if args.command=='check':r,rc=core_run(args.project,args.core);print(json.dumps(r,indent=2));return rc
        if args.command=='transient':
            data,rc=core_run(args.project,args.core,'transient',(args.dt,args.steps))
            if rc:raise ValueError('Transient failed; no successful waveform is claimed')
            Path(args.out).parent.mkdir(parents=True,exist_ok=True);Path(args.out).write_text(data);print(args.out);return 0
        if args.command=='build':return export_project(args)
        if args.command=='verify':return verify(args)
        if args.command=='fabricate':
            if verify(args):raise ValueError('Native verification failed; no fabrication outputs generated')
            out=Path(args.out).resolve();report=json.loads((out/'report.json').read_text())
            if report['errors'] or report['warnings']:raise ValueError('Resolve all internal findings, including unknown ratings, before fabrication export')
            cli=discover_cli(args.kicad_cli);cam=out/'fabrication';cam.mkdir(exist_ok=True)
            for command in [['pcb','export','gerbers','-o',str(cam)+os.sep,str(out/'design.kicad_pcb')],['pcb','export','drill','-o',str(cam)+os.sep,str(out/'design.kicad_pcb')]]:
                subprocess.run([cli,*command],check=True,timeout=180)
            print('CAM files exported to '+str(cam)+'. Engineering review of models, process and assembly remains required.');return 0
        if args.command=='classify':return subprocess.run([core_path(args.core),'classify',args.text],timeout=30).returncode
        if args.command=='part-add':
            tree=load(args.file) if Path(args.file).exists() else sx('catalogue')
            if head(tree)!='catalogue' or any(p[1]==args.id for p in direct(tree,'part')):raise ValueError('Invalid catalogue or duplicate part ID')
            if not re.fullmatch(r'[A-Za-z0-9_.-]+',args.id):raise ValueError('Invalid part ID')
            if not all(math.isfinite(v) and v>=0 for v in [args.power,args.voltage,args.tolerance,args.height]) or args.tolerance>=1:raise ValueError('Invalid ratings/tolerance')
            if args.kind not in ('V','I'):safe_lib_id(args.symbol);safe_lib_id(args.footprint)
            tree.append(sx('part',A(args.id),sx('kind',A(args.kind)),sx('symbol',Q(args.symbol)),sx('footprint',Q(args.footprint)),sx('power_w',number(args.power)),sx('voltage_v',number(args.voltage)),sx('tolerance',number(args.tolerance)),sx('height_mm',number(args.height)),sx('manufacturer',Q(args.manufacturer)),sx('mpn',Q(args.mpn)),sx('datasheet',Q(args.datasheet))))
            write_tree(args.file,tree);print('Added '+args.id+' to '+args.file);return 0
        if args.command=='catalogue':
            for p in catalogue(args.file):
                if args.search.lower() in dump_sexpr(p).lower():print(dump_sexpr(p))
            return 0
    except (ValueError,KeyError,IndexError,OSError,subprocess.SubprocessError) as exc:print(f'COPPER ERROR: {exc}',file=sys.stderr);return 2
if __name__=='__main__':raise SystemExit(main())
