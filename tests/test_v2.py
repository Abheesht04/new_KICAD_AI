"""Behavioral tests: numerical oracles, fail-closed inputs, and real workflow contracts."""
import csv, io, json, math, os, subprocess, sys, tempfile, unittest, copy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CORE=str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv)>1 and not sys.argv[1].startswith('-') else str(ROOT/'build-local/copper-core')
sys.path.insert(0,str(ROOT/'tools'))
import copper
from hwc_kicad import parse_sexpr, dump_sexpr, one, direct, text_at, Atom as A, Quoted as Q
class V2(unittest.TestCase):
 def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.dir=Path(self.tmp.name)
 def tearDown(self):self.tmp.cleanup()
 def tree(self,name='divider'):return copper.load(ROOT/f'examples/v2/{name}.copper')
 def run_core(self,tree,mode='analyze',extra=()):
  p=self.dir/'input.copper';copper.write_tree(p,tree)
  r=subprocess.run([CORE,mode,str(p),*map(str,extra)],capture_output=True,text=True,timeout=40)
  return r
 def analyze(self,tree):
  r=self.run_core(tree);self.assertTrue(r.stdout,r.stderr);return r.returncode,json.loads(r.stdout)
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
 def test_native_workflow(self):
  sym=os.environ.get('KICAD_SYMBOL_DIR');fp=os.environ.get('KICAD_FOOTPRINT_DIR')
  if not sym or not fp:self.skipTest('Set KICAD_SYMBOL_DIR and KICAD_FOOTPRINT_DIR for installed-library integration')
  args=type('Args',(),dict(project=str(ROOT/'examples/v2/divider.copper'),out=str(self.dir/'build'),core=CORE,symbol_dir=sym,footprint_dir=fp,no_route=False))()
  rc=copper.export_project(args);self.assertEqual(rc,0)
  report=json.loads((self.dir/'build/report.json').read_text());self.assertTrue(report['physical_checked']);self.assertEqual(report['errors'],0)
  sch=copper.load(self.dir/'build/design.kicad_sch');pcb=copper.load(self.dir/'build/design.kicad_pcb');self.assertEqual(len(direct(sch,'symbol')),3);self.assertEqual(len(direct(pcb,'footprint')),3)
  # Regeneration must be byte-identical (UUIDs, ordering and routing).
  before=(self.dir/'build/design.kicad_pcb').read_bytes();self.assertEqual(copper.export_project(args),0);self.assertEqual(before,(self.dir/'build/design.kicad_pcb').read_bytes())
  # User process limits are executable checks, not stored-only metadata.
  for field,value,expected in [('max_aspect',1,'drill_aspect'),('mask_web_mm',2,'mask_web'),('courtyard_gap_mm',10,'courtyard_gap'),('max_current_density_a_mm2',.01,'current_density_budget'),('max_drop_v',1e-9,'trace_drop_budget')]:
   changed=copper.load(self.dir/'build/resolved.copper');changed.append(copper.sx('rules',copper.sx(field,A(str(value)))));code,findings=self.analyze(changed);self.assertEqual(code,2);self.assertIn(expected,[v['rule'] for v in findings['issues']])
  # Native parser, ERC, DRC, schematic parity and pin/net parity must really execute.
  cli=copper.discover_cli()
  if cli and subprocess.run([cli,'version'],capture_output=True,text=True).stdout.startswith('9.'):
   self.assertEqual(copper.verify(type('Args',(),dict(out=str(self.dir/'build'),kicad_cli=cli))()),0)
   rotated=self.tree()
   for c in direct(one(rotated,'components'),'component'):
    if c[1]=='J1':one(c,'at')[2]=A('20');one(c,'at')[3]=A('90')
   copper.write_tree(self.dir/'rotated.copper',rotated)
   rotated_args=type('Args',(),dict(project=str(self.dir/'rotated.copper'),out=str(self.dir/'rotated'),core=CORE,symbol_dir=sym,footprint_dir=fp,no_route=False))()
   self.assertEqual(copper.export_project(rotated_args),0)
   self.assertEqual(copper.verify(type('Args',(),dict(out=str(self.dir/'rotated'),kicad_cli=cli))()),0)

  # Open traces and a deliberate inter-net short must fail physical checks.
  resolved=copper.load(self.dir/'build/resolved.copper');geo=one(resolved,'geometry');geo[:]=[x for x in geo if not isinstance(x,list) or x[0]!='trace'];rc,bad=self.analyze(resolved);self.assertEqual(rc,2);self.assertIn('open_connection',[v['rule'] for v in bad['issues']])
  pads=direct(geo,'pad');a=next(p for p in pads if p[3]=='VIN');b=next(p for p in pads if p[3]=='GND');geo.append(copper.sx('trace',A('VIN'),a[4],a[5],b[4],b[5],A('.25'),A('0')));rc,bad=self.analyze(resolved);self.assertEqual(rc,2);self.assertTrue(any(v['rule'] in ('short_circuit','copper_clearance') for v in bad['issues']))
 def test_inherited_symbol_flattening(self):
  lib=self.dir/'Fixture.kicad_sym';lib.write_text('(kicad_symbol_lib (symbol "Base" (property "Reference" "R") (symbol "Base_1_1" (pin passive line (at 0 0 0) (length 2) (name "p") (number "1")))) (symbol "Child" (extends "Base") (property "Value" "child")))')
  d=copper.resolve_symbol(self.dir,'Fixture:Child',{});self.assertIsNone(one(d,'extends'));self.assertEqual(copper.symbol_pins(d)[0][-1][1],'1')
 def test_path_traversal_library_id_rejected(self):
  for s in ['../../evil:foo','Device:../../x','/etc:passwd','Device:x/y']:
   with self.assertRaises(ValueError):copper.safe_lib_id(s)
if __name__=='__main__':unittest.main(verbosity=2)
