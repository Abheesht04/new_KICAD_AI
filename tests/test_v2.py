"""Behavioral tests: numerical oracles, fail-closed inputs, and real workflow contracts.
Exit status: 0 all ran and passed, 1 failure, 77 something was skipped (CTest SKIP_RETURN_CODE).
Select a class with: test_v2.py CORE V2 | V2Native
"""
import contextlib, csv, io, json, math, os, subprocess, sys, tempfile, unittest, copy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CORE=str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv)>1 and not sys.argv[1].startswith('-') else str(ROOT/'build-local/copper-core')
sys.path.insert(0,str(ROOT/'tools'))
import copper
from hwc_kicad import parse_sexpr, dump_sexpr, one, direct, text_at, Atom as A, Quoted as Q
# Committed exact library snapshots: enough for the divider without an installed KiCad.
FIX_SYM=ROOT/'examples/generated/divider/libraries/symbols'
FIX_FP=ROOT/'examples/generated/divider/libraries/footprints'
def set_field(node,key,value):
 field=one(node,key)
 if field:field[1]=A(value)
 else:node.append(copper.sx(key,A(value)))
def rated(tree):
 """Synthetic, explicitly supplied test ratings; not data for any real purchasable part."""
 for p in direct(one(tree,'parts'),'part'):
  if p[1]=='resistor':set_field(p,'power_w','0.1');set_field(p,'voltage_v','50');set_field(p,'height_mm','0.55')
  if p[1]=='header3':set_field(p,'height_mm','2.5')
 return tree
class Base(unittest.TestCase):
 def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.dir=Path(self.tmp.name)
 def tearDown(self):self.tmp.cleanup()
 def tree(self,name='divider'):return copper.load(ROOT/f'examples/v2/{name}.copper')
 def run_core(self,tree,mode='analyze',extra=()):
  p=self.dir/'input.copper';copper.write_tree(p,tree)
  r=subprocess.run([CORE,mode,str(p),*map(str,extra)],capture_output=True,text=True,timeout=40)
  return r
 def analyze(self,tree):
  r=self.run_core(tree);self.assertTrue(r.stdout,r.stderr);return r.returncode,json.loads(r.stdout)
 def build(self,tree,name='build',sym=FIX_SYM,fp=FIX_FP):
  src=self.dir/f'{name}.copper';copper.write_tree(src,tree)
  args=type('Args',(),dict(project=str(src),out=str(self.dir/name),core=CORE,symbol_dir=str(sym),footprint_dir=str(fp),no_route=False))()
  with contextlib.redirect_stdout(io.StringIO()):rc=copper.export_project(args)
  return src,self.dir/name,rc
 def verify(self,out,kicad_cli=None,project=None):
  # A nonexistent CLI path makes the native gate deterministically UNVERIFIED.
  args=type('Args',(),dict(out=str(out),kicad_cli=kicad_cli or str(self.dir/'no-kicad-cli'),core=CORE,project=project))()
  with contextlib.redirect_stdout(io.StringIO()):rc=copper.verify(args)
  return rc,json.loads((Path(out)/'verification.json').read_text())
 def check(self,report,name):return next(c['status'] for c in report['checks'] if c['check']==name)
class V2(Base):
 def test_divider_matches_closed_form(self):
  rc,r=self.analyze(self.tree());self.assertEqual(rc,0);self.assertAlmostEqual(r['nodes'][2]['voltage_v'],10/3,places=9);self.assertLess(r['kcl_residual_a'],1e-10);self.assertAlmostEqual(sum(c['power_w'] for c in r['calculations']),25/30000,places=10)
 def test_rc_backward_euler_and_analytic_bound(self):
  r=self.run_core(self.tree('rc'),'transient',(.0001,100));self.assertEqual(r.returncode,0,r.stderr);rows=list(csv.DictReader(io.StringIO(r.stdout)));v=float(rows[-1]['OUT_V']);self.assertAlmostEqual(v,5*(1-(1/1.01)**100),places=8);self.assertLess(abs(v-5*(1-math.exp(-1))),.01)
 def test_rl_backward_euler(self):
  r=self.run_core(self.tree('rl'),'transient',(1e-6,100));self.assertEqual(r.returncode,0,r.stderr);v=float(list(csv.DictReader(io.StringIO(r.stdout)))[-1]['OUT_V']);self.assertAlmostEqual(v,5*(1/1.01)**100,places=8)
 def test_diode_polarity_and_nonlinear_kcl(self):
  rc,r=self.analyze(self.tree('diode'));self.assertEqual(rc,0);v=r['nodes'][2]['voltage_v'];self.assertGreater(v,.5);self.assertLess(v,.85);self.assertLess(r['kcl_residual_a'],1e-8)
 def test_current_source_sign(self):
  t=self.tree();one(t,'targets') and t.remove(one(t,'targets'))
  supply=next(p for p in direct(one(t,'parts'),'part') if p[1]=='voltage');one(supply,'kind')[1]=A('I')
  source=next(c for c in direct(one(t,'components'),'component') if c[1]=='V1');one(source,'value')[1]=A('0.0001')
  rc,r=self.analyze(t);self.assertEqual(rc,0);self.assertAlmostEqual(r['nodes'][1]['voltage_v'],-3);self.assertAlmostEqual(r['nodes'][2]['voltage_v'],-2)
 def test_voltage_target_is_gate(self):
  t=self.tree();one(t,'targets')[1][2]=A('4');one(t,'targets')[1][3]=A('4.1');rc,r=self.analyze(t);self.assertEqual(rc,2);self.assertIn('voltage_target',[v['rule'] for v in r['issues']])
 def test_unknown_model_blocks_solve(self):
  t=self.tree();one(direct(one(t,'parts'),'part')[0],'kind')[1]=A('U');rc,r=self.analyze(t);self.assertEqual(rc,2);self.assertFalse(r['dc_solved'])
 def test_negative_nan_and_duplicate_rejected(self):
  for value in ['-1','nan','inf','1e999']:
   t=self.tree();one(direct(one(t,'components'),'component')[1],'value')[1]=A(value);r=self.run_core(t);self.assertEqual(r.returncode,2);self.assertIn('PROJECT ERROR',r.stderr)
  t=self.tree();one(t,'components').append(copy.deepcopy(one(t,'components')[2]));self.assertEqual(self.run_core(t).returncode,2)
 def test_unknown_rules_not_ignored(self):
  t=self.tree();t.append(copper.sx('rules',copper.sx('clearnce_mm',A('.5'))));self.assertEqual(self.run_core(t).returncode,2)
 def test_duplicate_rule_not_last_wins(self):
  t=self.tree();t.append(parse_sexpr('(rules (clearance_mm 0.2) (clearance_mm 0))'));self.assertEqual(self.run_core(t).returncode,2)
 def test_duplicate_pin_rejected(self):
  t=self.tree();c=one(t,'components')[2];one(c,'pins')[2][0]=A('1');self.assertEqual(self.run_core(t).returncode,2)
 def test_unknown_net_rejected(self):
  t=self.tree();one(one(t,'components')[2],'pins')[1][1]=A('MISSING');self.assertEqual(self.run_core(t).returncode,2)
 def test_floating_reference_singular(self):
  t=self.tree();one(t,'nets').append(A('FLOAT'));rc,r=self.analyze(t);self.assertEqual(rc,2);self.assertFalse(r['dc_solved'])
 def test_power_rating(self):
  t=self.tree();direct(one(t,'parts'),'part')[0].append(parse_sexpr('(power_w 0.0001)'));rc,r=self.analyze(t);self.assertEqual(rc,2);self.assertIn('power_derating',[v['rule'] for v in r['issues']])
 def test_no_geometry_not_claimed_checked(self):
  rc,r=self.analyze(self.tree());self.assertFalse(r['physical_checked']);self.assertIn('geometry_missing',[v['rule'] for v in r['issues']])
 def test_transient_invalid_parameters(self):
  for dt,steps in [(0,10),(-1,5),(float('nan'),3),(1,100001),(1,0)]:self.assertEqual(self.run_core(self.tree('rc'),'transient',(dt,steps)).returncode,2)
 def test_transformer_categories_and_abstention(self):
  phrases={'please insert a resistor':'R','select a bypass capacitor':'C','give me a coil':'L','find a diode':'D','add a schottky diode':'D','use a light emitting diode':'LED','select a terminal block':'J','add a battery':'V','use a mosfet':'Q','create a microcontroller':'U','quantum banana reactor':'UNKNOWN','resistor capacitor':'UNKNOWN','resistor inductor':'UNKNOWN','capacitor transistor':'UNKNOWN','':'UNKNOWN'}
  for text,label in phrases.items():
   r=subprocess.run([CORE,'classify',text],capture_output=True,text=True);j=json.loads(r.stdout);self.assertEqual(j['category'],label,text);self.assertEqual(j['accepted'],label!='UNKNOWN')
 def test_resistor_tolerance_corners(self):
  t=self.tree();direct(one(t,'parts'),'part')[0].append(parse_sexpr('(tolerance 0.01)'))
  rc,r=self.analyze(t);self.assertEqual(rc,0);self.assertEqual(r['tolerance_corner_count'],4)
  bounds=r['corner_nodes'][2];self.assertAlmostEqual(bounds['min_v'],5*19800/(10100+19800),places=8);self.assertAlmostEqual(bounds['max_v'],5*20200/(9900+20200),places=8)
  one(t,'targets')[1][2]=A('3.332');one(t,'targets')[1][3]=A('3.334');rc,r=self.analyze(t);self.assertEqual(rc,2);self.assertIn('corner_target',[v['rule'] for v in r['issues']])
 def test_kicad_rotation_convention(self):
  # KiCad PCB coordinates are screen-oriented: +90 maps down to right.
  x,y=copper.rotate(0,2.54,90);self.assertAlmostEqual(x,2.54);self.assertAlmostEqual(y,0)
  x,y=copper.rotate(.825,0,90);self.assertAlmostEqual(x,0);self.assertAlmostEqual(y,-.825)
 # I-05: an omitted required value is INVALID_INPUT, never a silent 0.
 def test_missing_required_value_rejected(self):
  cases=[('divider','V1'),('divider','R1'),('rc','C1'),('rl','L1')]
  for name,ref in cases:
   t=self.tree(name);c=next(c for c in direct(one(t,'components'),'component') if c[1]==ref);c.remove(one(c,'value'))
   r=self.run_core(t);self.assertEqual(r.returncode,2,ref);self.assertIn('PROJECT ERROR',r.stderr);self.assertIn('value',r.stderr);self.assertFalse(r.stdout.strip(),ref)
  t=self.tree();one(next(p for p in direct(one(t,'parts'),'part') if p[1]=='voltage'),'kind')[1]=A('I')
  c=next(c for c in direct(one(t,'components'),'component') if c[1]=='V1');c.remove(one(c,'value'));self.assertEqual(self.run_core(t).returncode,2)
 def test_optional_value_kinds_and_explicit_zero(self):
  t=self.tree();c=next(c for c in direct(one(t,'components'),'component') if c[1]=='J1');c.remove(one(c,'value'))
  rc,r=self.analyze(t);self.assertEqual(rc,0);self.assertTrue(r['dc_solved'])
  t=self.tree();t.remove(one(t,'targets'));one(next(c for c in direct(one(t,'components'),'component') if c[1]=='V1'),'value')[1]=A('0')
  rc,r=self.analyze(t);self.assertEqual(rc,0);self.assertEqual(r['nodes'][1]['voltage_v'],0)
 # I-06: unknown ratings/heights are UNVERIFIED in the build and in verify, never PASS.
 def test_unknown_ratings_never_verify_as_pass(self):
  _,out,rc=self.build(self.tree());self.assertEqual(rc,0)
  manifest=json.loads((out/'manifest.json').read_text());self.assertEqual(manifest['status'],copper.STATUS_UNVERIFIED)
  self.assertIn('rating_unknown',manifest['unverified_rules']);self.assertIn('height_unknown',manifest['unverified_rules'])
  code,report=self.verify(out);self.assertEqual(code,2);self.assertFalse(report['pass']);self.assertNotEqual(report['status'],'PASS')
  self.assertEqual(self.check(report,'data_completeness'),'UNVERIFIED');self.assertEqual(self.check(report,'build_status'),'UNVERIFIED')
 def test_complete_data_leaves_only_native_unverified(self):
  _,out,rc=self.build(rated(self.tree()));self.assertEqual(rc,0)
  self.assertEqual(json.loads((out/'manifest.json').read_text())['status'],copper.STATUS_PASS)
  code,report=self.verify(out);self.assertEqual(code,2);self.assertEqual(report['status'],'UNVERIFIED')
  for name in ('output_hashes','source_freshness','build_status','internal_reanalysis','data_completeness'):self.assertEqual(self.check(report,name),'PASS',name)
  self.assertEqual([g['status'] for g in report['gates']],['UNVERIFIED'])
 # I-07: verify detects stale source, tampered reports, and never leaves an old PASS marker.
 def test_verify_rejects_stale_source(self):
  src,out,rc=self.build(rated(self.tree()));self.assertEqual(rc,0)
  self.assertEqual(self.check(self.verify(out)[1],'source_freshness'),'PASS')
  t=copper.load(src);one(next(c for c in direct(one(t,'components'),'component') if c[1]=='R2'),'value')[1]=A('22000');copper.write_tree(src,t)
  code,report=self.verify(out);self.assertEqual(code,2);self.assertEqual(report['status'],'FAIL');self.assertEqual(self.check(report,'source_freshness'),'FAIL')
 def test_verify_explicit_project_and_missing_source(self):
  src,out,rc=self.build(rated(self.tree()));other=self.dir/'other.copper';copper.write_tree(other,self.tree('rc'))
  self.assertEqual(self.check(self.verify(out,project=str(other))[1],'source_freshness'),'FAIL')
  src.unlink();code,report=self.verify(out);self.assertEqual(code,2);self.assertEqual(self.check(report,'source_freshness'),'UNVERIFIED')
 def test_verify_reruns_internal_analysis(self):
  _,out,rc=self.build(rated(self.tree()));report=json.loads((out/'report.json').read_text())
  report['nodes'][2]['voltage_v']=3.3;(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
  manifest=json.loads((out/'manifest.json').read_text());manifest['files']['report.json']=copper.sha256_file(out/'report.json');(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
  code,result=self.verify(out);self.assertEqual(code,2);self.assertEqual(self.check(result,'output_hashes'),'PASS');self.assertEqual(self.check(result,'internal_reanalysis'),'FAIL')
 def test_verify_hash_mismatch_writes_fail_marker(self):
  _,out,rc=self.build(rated(self.tree()));self.verify(out)
  with (out/'design.kicad_pcb').open('a') as f:f.write('\n')
  code,result=self.verify(out);self.assertEqual(code,2);self.assertEqual(result['status'],'FAIL');self.assertEqual(self.check(result,'output_hashes'),'FAIL')
 def test_draft_build_never_verifies(self):
  t=rated(self.tree());one(t,'targets')[1][2]=A('4');one(t,'targets')[1][3]=A('4.1')
  args=type('Args',(),dict(project=str(self.dir/'draft.copper'),out=str(self.dir/'draft'),core=CORE,symbol_dir=str(FIX_SYM),footprint_dir=str(FIX_FP),no_route=False,draft=True))()
  copper.write_tree(self.dir/'draft.copper',t)
  with contextlib.redirect_stdout(io.StringIO()):self.assertEqual(copper.export_project(args),2)
  code,result=self.verify(self.dir/'draft');self.assertEqual(code,2);self.assertEqual(self.check(result,'build_status'),'FAIL');self.assertEqual(result['status'],'FAIL')
 # I-08: a run that skipped anything must not exit as a plain pass.
 def test_skipped_suite_is_not_reported_as_pass(self):
  env={k:v for k,v in os.environ.items() if k not in ('KICAD_SYMBOL_DIR','KICAD_FOOTPRINT_DIR','KICAD9_SYMBOL_DIR')}
  r=subprocess.run([sys.executable,str(Path(__file__).resolve()),CORE,'V2Native'],capture_output=True,text=True,env=env,timeout=120)
  self.assertEqual(r.returncode,77,r.stderr);self.assertIn('skipped',r.stderr)
 def test_inherited_symbol_flattening(self):
  lib=self.dir/'Fixture.kicad_sym';lib.write_text('(kicad_symbol_lib (symbol "Base" (property "Reference" "R") (symbol "Base_1_1" (pin passive line (at 0 0 0) (length 2) (name "p") (number "1")))) (symbol "Child" (extends "Base") (property "Value" "child")))')
  d=copper.resolve_symbol(self.dir,'Fixture:Child',{});self.assertIsNone(one(d,'extends'));self.assertEqual(copper.symbol_pins(d)[0][-1][1],'1')
 def test_path_traversal_library_id_rejected(self):
  for s in ['../../evil:foo','Device:../../x','/etc:passwd','Device:x/y']:
   with self.assertRaises(ValueError):copper.safe_lib_id(s)
def part_named(tree,name):return next(p for p in direct(one(tree,'parts'),'part') if p[1]==name)
def component_named(tree,ref):return next(c for c in direct(one(tree,'components'),'component') if c[1]==ref)
class ModelContract(Base):
 """Prompt 2: device model / part / pin-map / rating / provenance contracts on the V2 project path."""
 def core_error(self,tree):
  r=self.run_core(tree);self.assertEqual(r.returncode,2,r.stdout);self.assertIn('PROJECT ERROR',r.stderr);self.assertFalse(r.stdout.strip());return r.stderr
 def test_models_are_queryable(self):
  r=subprocess.run([CORE,'models'],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr);d=json.loads(r.stdout)
  self.assertEqual(d['contract_version'],2);models={m['id']:m for m in d['models']}
  self.assertEqual(set(models),{'ideal.resistor','ideal.capacitor','ideal.inductor','shockley.diode','ideal.voltage_source','ideal.current_source'})
  for m in models.values():self.assertEqual(m['capabilities'],{'dc':True,'transient':True,'ac':True});self.assertEqual(m['version'],2);self.assertTrue(m['assumptions'])
  self.assertEqual({i for i,m in models.items() if m['ac_excitation']},{'ideal.voltage_source','ideal.current_source'})
  self.assertEqual({i for i,m in models.items() if m['needs_operating_point']},{'shockley.diode'})
  self.assertEqual([t['name'] for t in models['shockley.diode']['terminals']],['cathode','anode'])
  self.assertEqual(models['ideal.resistor']['params'][0],{'name':'resistance','unit':'ohm','minimum':0,'maximum':1e12,'min_exclusive':True,'default':None,'bound_to_value':True,'meaning':'resistance'})
 def test_report_binds_every_component(self):
  rc,r=self.analyze(self.tree());c={x['ref']:x for x in r['components']}
  self.assertEqual(c['R1']['model'],{'id':'ideal.resistor','version':2,'source':'kind_default'});self.assertEqual(c['R1']['terminals'],{'p':'1','n':'2'})
  self.assertEqual(c['R1']['params'],{'resistance':10000});self.assertIsNone(c['J1']['model']);self.assertEqual(c['J1']['capabilities']['dc'],False)
  self.assertTrue(c['R1']['capabilities']['ac'])
 def test_explicit_model_and_pin_map_are_name_based(self):
  base=self.analyze(self.tree('diode'))[1]['nodes'][2]['voltage_v']
  t=self.tree('diode');p=part_named(t,'diode');p.append(parse_sexpr('(model shockley.diode)'));p.append(parse_sexpr('(pinmap (anode 2) (cathode 1 1))'))
  d=component_named(t,'D1');pins=one(d,'pins');pins[1:]=reversed(pins[1:])  # pin order in the component no longer matters
  rc,r=self.analyze(t);self.assertEqual(rc,0);self.assertEqual(r['nodes'][2]['voltage_v'],base)
  b=next(x for x in r['components'] if x['ref']=='D1');self.assertEqual(b['model']['source'],'explicit');self.assertEqual(b['terminals'],{'cathode':'1','anode':'2'})
 def test_invalid_bindings_are_rejected(self):
  cases={'(model shockley.diode)':('resistor','is a diode'),'(model no.such)':('resistor','unknown device model'),
         '(pinmap (p 1))':('resistor','exactly once'),'(pinmap (p 1) (x 2))':('resistor','no terminal'),
         '(pinmap (p 1) (n 1))':('resistor','mapped to two terminals'),'(pinmap (p 1) (p 2))':('resistor','mapped twice'),
         '(pinmap (p 1 2) (n 2))':('resistor','pad'),'(pinmap (1 1) (2 2) (3 3))':('header3','needs a device model'),
         '(model ideal.resistor)':('header3','has no device model'),'(is_a 1e-12)':('resistor','Unknown field')}
  for extra,(part,message) in cases.items():
   t=self.tree();part_named(t,part).append(parse_sexpr(extra));self.assertIn(message,self.core_error(t),extra)
  t=self.tree();part_named(t,'resistor').append(parse_sexpr('(pinmap (p 1) (n 3))'));self.assertIn('missing from (pins',self.core_error(t))
  t=self.tree();one(component_named(t,'J1'),'pins')[2][0]=A('1');self.assertIn('duplicate pin',self.core_error(t))
  t=self.tree();one(component_named(t,'R1'),'pins').append(parse_sexpr('(3 OUT)'));self.assertIn('exactly the 2 pins',self.core_error(t))
 def test_unknown_ratings_stay_unknown_and_zero_is_rejected(self):
  rc,r=self.analyze(self.tree());c={x['ref']:x for x in r['components']}
  self.assertEqual(c['R1']['ratings'],{'power_w':{'known':False},'voltage_v':{'known':False},'height_mm':{'known':False}})
  t=self.tree();part_named(t,'resistor').append(parse_sexpr('(power_w unknown)'));rc,r=self.analyze(t)
  self.assertIn('rating_unknown',[i['rule'] for i in r['issues'] if i['object']=='R1'])
  for field in ('power_w','voltage_v','height_mm'):
   t=self.tree();part_named(t,'resistor').append(copper.sx(field,A('0')));self.assertIn('zero is not a rating',self.core_error(t),field)
   t=self.tree();part_named(t,'resistor').append(copper.sx(field,A('-1')));self.core_error(t)
 def test_override_precedence_and_provenance(self):
  t=rated(self.tree());part_named(t,'resistor').append(parse_sexpr('(provenance "synthetic test value")'))
  component_named(t,'R1').append(parse_sexpr('(override (power_w 0.25) (provenance "synthetic project override"))'))
  rc,r=self.analyze(t);c={x['ref']:x for x in r['components']}
  self.assertEqual(c['R1']['ratings']['power_w'],{'known':True,'value':0.25,'source':'override:R1'});self.assertEqual(c['R1']['override_provenance'],'synthetic project override')
  self.assertEqual(c['R2']['ratings']['power_w'],{'known':True,'value':0.1,'source':'part:resistor'});self.assertEqual(c['R2']['provenance'],'synthetic test value')
  t=rated(self.tree());component_named(t,'R1').append(parse_sexpr('(override (power_w 0.25))'));self.assertIn('provenance',self.core_error(t))
  t=rated(self.tree());component_named(t,'R1').append(parse_sexpr('(override (symbol "Device:C") (provenance "x"))'));self.assertIn('Unknown field',self.core_error(t))
 def test_semantic_identity_survives_save_and_build(self):
  t=rated(self.tree());p=part_named(t,'resistor');p.append(parse_sexpr('(model ideal.resistor)'));p.append(parse_sexpr('(pinmap (p 1) (n 2))'))
  p.append(parse_sexpr('(provenance "synthetic test value")'));component_named(t,'R2').append(parse_sexpr('(override (height_mm 0.6) (provenance "synthetic"))'))
  before=self.analyze(t)[1]['components'];copper.write_tree(self.dir/'saved.copper',t)
  self.assertEqual(self.analyze(copper.load(self.dir/'saved.copper'))[1]['components'],before)
  _,out,rc=self.build(t);self.assertEqual(rc,0)
  self.assertEqual(json.loads((out/'report.json').read_text())['components'],before)  # resolved.copper keeps identity
  bom={row['Reference']:row for row in csv.DictReader(io.StringIO((out/'bom.csv').read_text(encoding='utf-8')))}
  self.assertEqual(bom['R2']['Height_mm'],'0.6');self.assertEqual(bom['R1']['Height_mm'],'0.55')
  _,out,rc=self.build(self.tree(),name='generic')
  bom={row['Reference']:row for row in csv.DictReader(io.StringIO((out/'bom.csv').read_text(encoding='utf-8')))}
  self.assertEqual((bom['R1']['Power_W'],bom['J1']['Height_mm']),('UNKNOWN','UNKNOWN'))
 def test_swapped_pin_names_are_caught(self):
  t=self.tree('diode');p=part_named(t,'diode');p.append(parse_sexpr('(pinmap (cathode 2) (anode 1))'))
  args=type('Args',(),dict(project=str(self.write(t)),out=str(self.dir/'swap'),core=CORE,symbol_dir=str(FIX_SYM),footprint_dir=str(FIX_FP),no_route=False))()
  with self.assertRaises(copper.InvalidInput) as caught,contextlib.redirect_stdout(io.StringIO()):copper.export_project(args)
  self.assertIn('swapped',str(caught.exception));self.assertIn("named 'A'",str(caught.exception))
 def write(self,tree,name='p.copper'):path=self.dir/name;copper.write_tree(path,tree);return path
 def test_catalogue_overlay_precedence(self):
  user=self.dir/'user.parts'
  user.write_text('(catalogue (part resistor (kind R) (symbol "Device:R") (footprint "Resistor_SMD:R_0603_1608Metric") (power_w 0.1) (provenance "synthetic test value"))\n (part extra_r (kind R) (symbol "Device:R") (footprint "Resistor_SMD:R_0603_1608Metric")))\n')
  args=lambda out:type('Args',(),dict(template='divider',out=str(out),catalogue=str(user),rules=None,vin=5,r=10000,r2=20000,c=1e-6,l=.01,target=None,target_tolerance=.05,force=False,core=CORE))()
  with contextlib.redirect_stdout(io.StringIO()):copper.new_project(args(self.dir/'a.copper'));copper.new_project(args(self.dir/'b.copper'))
  self.assertEqual((self.dir/'a.copper').read_bytes(),(self.dir/'b.copper').read_bytes())
  parts={p[1]:p for p in direct(one(copper.load(self.dir/'a.copper'),'parts'),'part')}
  self.assertEqual(list(parts)[:2],['resistor','capacitor']);self.assertEqual(list(parts)[-1],'extra_r')  # user replaces in place; new ids follow
  self.assertEqual(text_at(parts['resistor'],'power_w'),'0.1');self.assertEqual(text_at(parts['resistor'],'catalogue'),'user:user.parts')
  self.assertEqual(text_at(parts['capacitor'],'catalogue'),'bundled:default.parts')
 def test_malformed_catalogues_change_nothing(self):
  bad={'dup':'(catalogue (part r (kind R) (symbol "Device:R") (footprint "F:x")) (part r (kind R) (symbol "Device:R") (footprint "F:x")))',
       'zero':'(catalogue (part r (kind R) (symbol "Device:R") (footprint "F:x") (power_w 0)))',
       'model':'(catalogue (part r (kind R) (symbol "Device:R") (footprint "F:x") (model shockley.diode)))',
       'root':'(parts (part r (kind R)))','syntax':'(catalogue (part r'}
  for name,text in bad.items():
   path=self.dir/f'{name}.parts';path.write_text(text);out=self.dir/f'{name}.copper'
   args=type('Args',(),dict(template='divider',out=str(out),catalogue=str(path),rules=None,vin=5,r=1,r2=1,c=1,l=1,target=None,target_tolerance=.05,force=False,core=CORE))()
   with self.assertRaises((ValueError,copper.Unverified),msg=name):copper.new_project(args)
   self.assertFalse(out.exists(),name)
  default=copper.BUNDLED_CATALOGUE.read_bytes()
  self.assertEqual(copper.main(['--format','quiet','part-add','--file',str(copper.BUNDLED_CATALOGUE),'--id','x','--kind','R','--symbol','Device:R','--footprint','R:x']),2)
  self.assertEqual(copper.BUNDLED_CATALOGUE.read_bytes(),default)
 def test_part_add_and_remove(self):
  cat=self.dir/'mine.parts';run=lambda *a:copper.main(['--format','quiet',*a])
  self.assertEqual(run('part-add','--file',str(cat),'--id','g','--kind','R','--symbol','Device:R','--footprint','Resistor_SMD:R_0603_1608Metric','--core',CORE),0)
  part=direct(copper.load(cat),'part')[0];self.assertIsNone(one(part,'power_w'));self.assertIsNone(one(part,'height_mm'))  # unknown is omitted, never 0
  self.assertEqual(run('part-add','--file',str(cat),'--id','d','--kind','D','--model','shockley.diode','--pinmap','cathode=1,anode=2','--symbol','Device:D','--footprint','Diode_SMD:D_SOD-123','--provenance','synthetic','--core',CORE),0)
  saved=cat.read_bytes()
  for extra in (['--pinmap','cathode=1'],['--model','ideal.resistor'],['--power','0'],['--pinmap','gate=1,anode=2']):
   self.assertEqual(run('part-add','--file',str(cat),'--id','bad','--kind','D','--symbol','Device:D','--footprint','Diode_SMD:D_SOD-123','--core',CORE,*extra),2,extra)
   self.assertEqual(cat.read_bytes(),saved,extra)
  self.assertEqual(run('part-remove','--file',str(cat),'--id','g'),0);self.assertEqual([p[1] for p in direct(copper.load(cat),'part')],['d'])
  self.assertEqual(run('part-remove','--file',str(cat),'--id','g'),2)
 def test_transient_uses_model_state(self):
  r=self.run_core(self.tree('rc'),'transient',(.0001,100));self.assertEqual(r.returncode,0,r.stderr)
  v=float(list(csv.DictReader(io.StringIO(r.stdout)))[-1]['OUT_V']);self.assertAlmostEqual(v,5*(1-(1/1.01)**100),places=8)
class AcAnalysis(Base):
 """Prompt 3: AC sweep on project circuits, checked against closed forms computed here in Python."""
 def ac(self,tree,start,stop,points,sweep='log'):
  p=self.dir/'ac.copper';copper.write_tree(p,tree)
  r=subprocess.run([CORE,'ac',str(p),str(start),str(stop),str(points),sweep],capture_output=True,text=True,timeout=60)
  return r,list(csv.DictReader(io.StringIO(r.stdout)))
 def with_ac(self,tree,ref,mag,phase):
  c=component_named(tree,ref);c.append(parse_sexpr(f'(ac {mag} {phase})'));return tree
 def backend_status(self,tree,*extra):
  p=self.dir/'b.copper';copper.write_tree(p,tree);result=self.dir/'r.json'
  copper.main(['--format','quiet','--result-json',str(result),'ac',str(p),'--core',CORE,*extra]);return json.loads(result.read_text())
 def test_rc_low_pass_matches_closed_form(self):
  r,rows=self.ac(copper.load(ROOT/'examples/v2/rc_ac.copper'),1,1e4,41);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(len(rows),41)
  R,C=10000,1e-6
  for row in rows:
   f=float(row['frequency_hz']);wrc=2*math.pi*f*R*C;self.assertEqual(row['status'],'ok')
   self.assertAlmostEqual(float(row['V(OUT)_mag']),1/math.sqrt(1+wrc*wrc),places=10)
   self.assertAlmostEqual(float(row['V(OUT)_phase_deg']),-math.degrees(math.atan(wrc)),places=8)
   self.assertTrue(math.isclose(math.hypot(float(row['V(OUT)_re']),float(row['V(OUT)_im'])),float(row['V(OUT)_mag']),rel_tol=1e-11))  # CSV has 12 significant digits
  fc=1/(2*math.pi*R*C);r,rows=self.ac(copper.load(ROOT/'examples/v2/rc_ac.copper'),fc,fc,1)
  self.assertAlmostEqual(float(rows[0]['V(OUT)_mag']),1/math.sqrt(2),places=10);self.assertAlmostEqual(float(rows[0]['V(OUT)_phase_deg']),-45,places=8)
 def test_series_rlc_resonance_on_a_project(self):
  t=self.tree();t.remove(one(t,'targets'));one(t,'nets')[1:]=[A('GND'),A('VIN'),A('A'),A('B')]
  one(t,'components')[1:]=[parse_sexpr(x) for x in ['(component V1 (part voltage) (value 0) (ac 1 0) (pins (1 VIN) (2 GND)))',
   '(component R1 (part resistor) (value 10) (pins (1 VIN) (2 A)))','(component L1 (part inductor) (value 0.01) (pins (1 A) (2 B)))',
   '(component C1 (part capacitor) (value 1e-6) (pins (1 B) (2 GND)))']]
  f0=1/(2*math.pi*math.sqrt(0.01*1e-6));r,rows=self.ac(t,f0/10,f0*10,201);self.assertEqual(r.returncode,0,r.stderr)
  for row in rows:
   w=2*math.pi*float(row['frequency_hz']);x=w*0.01-1/(w*1e-6)
   self.assertAlmostEqual(float(row['I(L1)_mag']),1/math.hypot(10,x),places=9)
  r,rows=self.ac(t,f0,f0,1);self.assertAlmostEqual(float(rows[0]['I(L1)_mag']),0.1,places=9);self.assertAlmostEqual(float(rows[0]['I(L1)_phase_deg']),0,places=5)
  self.assertAlmostEqual(float(rows[0]['I(V1)_mag']),0.1,places=9);self.assertAlmostEqual(abs(float(rows[0]['I(V1)_phase_deg'])),180,places=5)  # branch current + to - through the source
 def test_diode_is_linearised_at_the_operating_point(self):
  t=self.with_ac(self.tree('diode'),'V1',0.001,0);rc,report=self.analyze(t);vd=report['nodes'][2]['voltage_v']
  vt=1.380649e-23*300/1.602176634e-19;gd=1e-14/vt*math.exp(vd/vt)
  r,rows=self.ac(t,1e3,1e3,1);self.assertEqual(r.returncode,0,r.stderr)
  self.assertAlmostEqual(float(rows[0]['V(OUT)_re'])/(1e-3/(1+1000*gd)),1.0,places=8)
 def test_zero_hz_and_singular_frequencies(self):
  r,rows=self.ac(copper.load(ROOT/'examples/v2/rc_ac.copper'),0,100,3,'lin');self.assertEqual(r.returncode,0,r.stderr)
  self.assertEqual([float(x['frequency_hz']) for x in rows],[0,50,100]);self.assertAlmostEqual(float(rows[0]['V(OUT)_mag']),1,places=12)
  t=self.with_ac(self.tree('rc'),'V1',1,0);c=component_named(t,'R1');one(c,'part')[1]=A('capacitor');one(c,'value')[1]=A('1e-6')
  r,rows=self.ac(t,0,100,2,'lin');self.assertEqual(r.returncode,2);self.assertIn('AC singular',r.stderr)
  self.assertEqual([x['status'] for x in rows],['singular','ok']);self.assertEqual(rows[0]['V(OUT)_mag'],'');self.assertAlmostEqual(float(rows[1]['V(OUT)_mag']),0.5,places=12)
 def test_fails_closed(self):
  r,_=self.ac(self.tree('rc'),1,10,2);self.assertEqual(r.returncode,2);self.assertIn('AC no_excitation',r.stderr)  # a DC value is not AC excitation
  t=self.with_ac(self.tree(),'V1',1,0);one(part_named(t,'resistor'),'kind')[1]=A('U')
  r,rows=self.ac(t,1,10,2);self.assertEqual(r.returncode,2);self.assertIn('AC unsupported_model',r.stderr);self.assertEqual(rows,[])
  for bad in [(0,10,2,'log'),(10,1,2,'log'),(1,10,1,'log'),(1,1,3,'log'),(1,10,0,'log'),(1,10,100001,'log'),(float('nan'),10,2,'lin')]:
   r,_=self.ac(self.with_ac(self.tree(),'V1',1,0),*bad);self.assertEqual(r.returncode,2,bad);self.assertIn('AC invalid_sweep',r.stderr,bad)
  for extra in ['(ac 1 0)','(ac -1 0)','(ac 1)','(ac nan 0)']:
   t=self.tree();component_named(t,'R1' if extra=='(ac 1 0)' else 'V1').append(parse_sexpr(extra));r=self.run_core(t);self.assertEqual(r.returncode,2,extra);self.assertIn('PROJECT ERROR',r.stderr)
 def test_backend_statuses(self):
  t=self.with_ac(self.tree(),'V1',1,0)
  ok=self.backend_status(t,'--start','1','--stop','1e3','--points','4');self.assertEqual(ok['status'],'PASS');self.assertEqual(len(ok['data']['rows']),4)
  self.assertEqual(self.backend_status(self.tree(),'--start','1','--stop','10','--points','2')['status'],'INVALID_INPUT')
  u=self.with_ac(self.tree(),'V1',1,0);one(part_named(u,'resistor'),'kind')[1]=A('U')
  self.assertEqual(self.backend_status(u,'--start','1','--stop','10','--points','2')['status'],'UNVERIFIED')
  s=self.with_ac(self.tree('rc'),'V1',1,0);c=component_named(s,'R1');one(c,'part')[1]=A('capacitor');one(c,'value')[1]=A('1e-6')
  bad=self.backend_status(s,'--start','0','--stop','100','--points','2','--sweep','lin');self.assertEqual(bad['status'],'FAIL');self.assertEqual(bad['data']['rows'][0]['status'],'singular')
 def test_columns_and_determinism(self):
  t=copper.load(ROOT/'examples/v2/rc_ac.copper');r1,_=self.ac(t,1,1e5,11);r2,_=self.ac(t,1,1e5,11)
  self.assertEqual(r1.stdout,r2.stdout)
  header=r1.stdout.splitlines()[0].split(',');self.assertEqual(header[:2],['frequency_hz','status'])
  self.assertEqual(header[2:],[f'{q}_{p}' for q in ('V(VIN)','V(OUT)','I(V1)') for p in ('re','im','mag','phase_deg')])
  freqs=[float(l.split(',')[0]) for l in r1.stdout.splitlines()[1:]];self.assertEqual(freqs,sorted(freqs));self.assertEqual((freqs[0],freqs[-1]),(1,1e5))
  self.assertNotIn('nan',r1.stdout.lower());self.assertNotIn('inf',r1.stdout.lower())
class V2Native(Base):
 """Needs installed KiCad libraries (KICAD_SYMBOL_DIR/KICAD_FOOTPRINT_DIR); gates also need KiCad 9.x CLI."""
 def libraries(self):
  sym=os.environ.get('KICAD_SYMBOL_DIR');fp=os.environ.get('KICAD_FOOTPRINT_DIR')
  if not sym or not fp:self.skipTest('UNVERIFIED: set KICAD_SYMBOL_DIR and KICAD_FOOTPRINT_DIR for installed-library integration')
  return sym,fp
 def test_library_build_workflow(self):
  sym,fp=self.libraries();_,out,rc=self.build(self.tree(),sym=sym,fp=fp);self.assertEqual(rc,0)
  report=json.loads((out/'report.json').read_text());self.assertTrue(report['physical_checked']);self.assertEqual(report['errors'],0)
  sch=copper.load(out/'design.kicad_sch');pcb=copper.load(out/'design.kicad_pcb');self.assertEqual(len(direct(sch,'symbol')),3);self.assertEqual(len(direct(pcb,'footprint')),3)
  # Regeneration must be byte-identical (UUIDs, ordering and routing).
  before=(out/'design.kicad_pcb').read_bytes();self.assertEqual(self.build(self.tree(),sym=sym,fp=fp)[2],0);self.assertEqual(before,(out/'design.kicad_pcb').read_bytes())
  # User process limits are executable checks, not stored-only metadata.
  for field,value,expected in [('max_aspect',1,'drill_aspect'),('mask_web_mm',2,'mask_web'),('courtyard_gap_mm',10,'courtyard_gap'),('max_current_density_a_mm2',.01,'current_density_budget'),('max_drop_v',1e-9,'trace_drop_budget')]:
   changed=copper.load(out/'resolved.copper');changed.append(copper.sx('rules',copper.sx(field,A(str(value)))));code,findings=self.analyze(changed);self.assertEqual(code,2);self.assertIn(expected,[v['rule'] for v in findings['issues']])
  # Open traces and a deliberate inter-net short must fail physical checks.
  resolved=copper.load(out/'resolved.copper');geo=one(resolved,'geometry');geo[:]=[x for x in geo if not isinstance(x,list) or x[0]!='trace'];rc,bad=self.analyze(resolved);self.assertEqual(rc,2);self.assertIn('open_connection',[v['rule'] for v in bad['issues']])
  pads=direct(geo,'pad');a=next(p for p in pads if p[3]=='VIN');b=next(p for p in pads if p[3]=='GND');geo.append(copper.sx('trace',A('VIN'),a[4],a[5],b[4],b[5],A('.25'),A('0')));rc,bad=self.analyze(resolved);self.assertEqual(rc,2);self.assertTrue(any(v['rule'] in ('short_circuit','copper_clearance') for v in bad['issues']))
 def test_kicad9_native_gates(self):
  sym,fp=self.libraries();cli=copper.discover_cli()
  version=subprocess.run([cli,'version'],capture_output=True,text=True).stdout.strip() if cli else ''
  if not version.startswith('9.'):self.skipTest(f'UNVERIFIED: KiCad 9.x CLI required for native gates (found {version or "none"})')
  # Native parser, ERC, DRC, schematic parity and pin/net parity must really execute.
  _,out,rc=self.build(self.tree(),sym=sym,fp=fp);self.assertEqual(rc,0)
  code,report=self.verify(out,kicad_cli=cli);self.assertEqual(code,2);self.assertEqual(report['status'],'UNVERIFIED')
  self.assertEqual({g['gate']:g['status'] for g in report['gates']},{'erc':'PASS','drc':'PASS','netlist':'PASS','netlist_parity':'PASS'})
  rotated=rated(self.tree())
  for c in direct(one(rotated,'components'),'component'):
   if c[1]=='J1':one(c,'at')[2]=A('20');one(c,'at')[3]=A('90')
  for name,tree in [('rated',rated(self.tree())),('rotated',rotated)]:
   _,out,rc=self.build(tree,name=name,sym=sym,fp=fp);self.assertEqual(rc,0,name)
   code,report=self.verify(out,kicad_cli=cli);self.assertEqual(code,0,report);self.assertEqual(report['status'],'PASS');self.assertTrue(report['kicad_version'].startswith('9.'))
if __name__=='__main__':
 result=unittest.main(verbosity=2,exit=False).result
 sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
