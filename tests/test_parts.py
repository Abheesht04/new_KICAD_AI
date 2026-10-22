"""Part registry, MCU behavioral boundary, footprint keepouts and library import (Prompt 7).

Usage: test_parts.py COPPER_CORE [unittest args]
Exit status: 0 all ran and passed, 1 failure, 77 something was skipped (native KiCad 9.x tests).
The Native class needs KICAD_SYMBOL_DIR / KICAD_FOOTPRINT_DIR with KiCad 9.x libraries, and KiCad 9.x
kicad-cli for the ERC/DRC/parity gates; without them it is skipped (UNVERIFIED), never passed.
"""
import contextlib, copy, io, json, os, subprocess, sys, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CORE=str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv)>1 and not sys.argv[1].startswith('-') else str(ROOT/'build-local/copper-core')
os.environ['COPPER_CORE']=CORE
sys.path.insert(0,str(ROOT/'tools'))
import copper
from hwc_kicad import parse_sexpr, one, direct, text_at, Atom as A, Quoted as Q
sx=copper.sx
MCU_CATALOGUE=ROOT/'profiles/mcu_modules.parts'
NMOS=ROOT/'tests/fixtures/parts/nmos_variant.parts'
EXAMPLE=ROOT/'examples/v2/esp32_io.copper'
# Committed exact KiCad library snapshots (Device, Connector_Generic, Resistor_SMD, PinHeader): enough for
# import parity tests without an installed KiCad.
SNAP_SYM=ROOT/'examples/generated/divider/libraries/symbols'
SNAP_FP=ROOT/'examples/generated/divider/libraries/footprints'

def component(tree,ref):return next(c for c in direct(one(tree,'components'),'component') if str(c[1])==ref)
def part(tree,pid):return next(p for p in direct(one(tree,'parts'),'part') if str(p[1])==pid)
def rules(report,severity=None):return [i['rule'] for i in report['issues'] if severity in (None,i['severity'])]

class Base(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.dir=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def core(self,mode,path,*extra):return subprocess.run([CORE,mode,str(path),*map(str,extra)],capture_output=True,text=True,timeout=60)
    def catalogue(self,text):
        path=self.dir/'c.parts';path.write_text(text,encoding='utf-8');return self.core('catalogue',path)
    def analyze(self,tree):
        path=self.dir/'p.copper';copper.write_tree(path,tree);r=self.core('analyze',path)
        if not r.stdout:return r.returncode,r.stderr
        return r.returncode,json.loads(r.stdout)
    def example(self):return copper.load(EXAMPLE)
    def mcu_part(self):return copy.deepcopy(next(p for p in direct(copper.load(MCU_CATALOGUE),'part') if str(p[1])=='esp32_wroom_32e'))

class Registry(Base):
    def test_mcu_module_definition(self):
        r=self.core('catalogue',MCU_CATALOGUE);self.assertEqual(r.returncode,0,r.stderr)
        p={x['id']:x for x in json.loads(r.stdout)['parts']}['esp32_wroom_32e']
        self.assertEqual((p['kind'],p['family'],p['role'],p['alternates'],p['model']),('U','esp32','mcu_module',['esp32_wroom_32ue'],None))
        m=p['mcu'];self.assertEqual(m['operating_v'],[3.0,3.6]);self.assertEqual((m['supply'],m['ground']),(['2'],['1','15','38','39']))
        self.assertEqual(m['reset'],{'pin':'3','active_low':True});self.assertIn('not simulated',m['behavior'])
        gpio={g['pin']:g for g in m['gpio']};self.assertEqual(len(gpio),26)
        self.assertFalse(gpio['6']['output'],'IO34 is input-only');self.assertTrue(gpio['26']['output'])
        self.assertEqual(sorted(k for k,g in gpio.items() if g['strap']),['14','23','24','25','29'])
        self.assertFalse(m['output_ohm']['known'],'no output resistance is invented');self.assertFalse(m['pull_ohm']['known'])
    def test_mosfet_model_variant(self):
        r=self.core('catalogue',NMOS);self.assertEqual(r.returncode,0,r.stderr);p=json.loads(r.stdout)['parts'][0]
        self.assertEqual((p['model'],p['terminals'],p['family']),('level1.nmos',{'drain':'3','gate':'1','source':'2'},'mosfet'))
        self.assertEqual(p['capabilities'],{'dc':True,'transient':True,'ac':True})
        self.assertFalse(any(v['known'] for v in p['ratings'].values()),'ratings stay unknown, never zero')
    def test_schema_rejections(self):
        good=copper.dump_sexpr(self.mcu_part())
        cases=[(good.replace('(kind U)','(kind R)'),'only valid on kind U'),
               (good.replace('(behavior','(behaviour'),'Unknown field'),
               (good.replace('(operating_v 3 3.6)','(operating_v 3.6 3)').replace('(operating_v 3.0 3.6)','(operating_v 3.6 3)'),'operating_v'),
               (good.replace('(vil_ratio 0.25)','(vil_ratio 0.8)'),'vil_ratio must be below'),
               (good.replace('(pin 26 IO4 in out)','(pin 26 IO4 bidir)'),'capability must be in or out'),
               (good.replace('(pin 26 IO4 in out)','(pin 2 IO4 in out)'),'declared twice'),
               (good.replace('(straps','(straps (2 "not a gpio")'),'mcu straps'),
               (good.replace('(program 35 34 25 3)','(program 35 99)'),'not a declared pin'),
               (good.replace('(alternates esp32_wroom_32ue)','(alternates esp32_wroom_32e)'),'invalid alternate'),
               (good.replace('(family esp32)','(family "esp 32")'),'takes one identifier'),
               (good.replace('(max_source_a 0.04)','(max_source_a 0)'),'zero is not a rating')]
        for text,message in cases:
            with self.subTest(message):
                self.assertNotEqual(text,good,'case did not change the part')
                r=self.catalogue(f'(catalogue {text})');self.assertEqual(r.returncode,2,r.stdout);self.assertIn(message,r.stderr)
        r=self.catalogue(f'(catalogue {good})');self.assertEqual(r.returncode,2);self.assertIn('alternate esp32_wroom_32ue is not in this catalogue',r.stderr)
    def test_unknown_required_pin_mapping_is_rejected(self):
        r=self.catalogue('(catalogue (part q (kind M) (model level1.nmos) (symbol "Transistor_FET:2N7002") (footprint "Package_TO_SOT_SMD:SOT-23") (vto 1)))')
        self.assertEqual(r.returncode,2);self.assertIn('terminal order is never assumed',r.stderr)
        r=self.catalogue('(catalogue (part q (kind M) (model level1.nmos) (symbol "Transistor_FET:2N7002") (footprint "Package_TO_SOT_SMD:SOT-23") (vto 1) (pinmap (drain 3) (gate 1))))')
        self.assertEqual(r.returncode,2);self.assertIn('exactly once',r.stderr)

class McuBoundary(Base):
    def test_example_is_constrained_not_simulated(self):
        rc,r=self.analyze(self.example());self.assertEqual(rc,0,r);self.assertTrue(r['dc_solved'])
        nodes={n['name']:n['voltage_v'] for n in r['nodes']}
        self.assertAlmostEqual(nodes['IO4'],3.3,places=12);self.assertAlmostEqual(nodes['EN'],3.3,places=12)
        self.assertIsNone(nodes['TXD0'],'a net only boundary pins touch has no claimed voltage')
        self.assertEqual(r['errors'],0)
        for rule in ('mcu_supply_current','mcu_output_ideal','boundary_net','mcu_programming'):self.assertIn(rule,rules(r,'warning'))
        u1=next(c for c in r['components'] if c['ref']=='U1');self.assertIsNone(u1['model'])
        self.assertEqual(u1['drive'],{'26':'high'});self.assertIn('firmware execution and transistor-level silicon are not simulated',u1['mcu']['simulated'])
        self.assertEqual(copper.report_scopes(r)['electrical'],'UNVERIFIED','an MCU boundary is never a PASS')
    def test_pin_current_limits(self):
        t=self.example();one(component(t,'R2'),'value')[1]=A('47')
        rc,r=self.analyze(t);self.assertEqual(rc,2);i=next(i for i in r['issues'] if i['rule']=='mcu_pin_current')
        self.assertAlmostEqual(i['measured'],3.3/47,places=12);self.assertEqual(i['limit'],0.04);self.assertIn('sources',i['message'])
        t=self.example();c=component(t,'R2');one(c,'pins')[2][1]=A('3V3');one(c,'value')[1]=A('100');one(component(t,'U1'),'drive')[1][1]=A('low')
        rc,r=self.analyze(t);i=next(i for i in r['issues'] if i['rule']=='mcu_pin_current')
        self.assertAlmostEqual(i['measured'],0.033,places=12);self.assertEqual(i['limit'],0.028);self.assertIn('sinks',i['message'])
        one(c,'value')[1]=A('1000');rc,r=self.analyze(t);self.assertNotIn('mcu_pin_current',rules(r))
    def test_output_resistance_only_when_sourced(self):
        t=self.example();one(part(t,'esp32_wroom_32e'),'mcu').append(sx('output_ohm',A('30')))
        rc,r=self.analyze(t);self.assertNotIn('mcu_output_ideal',rules(r))
        self.assertAlmostEqual({n['name']:n['voltage_v'] for n in r['nodes']}['IO4'],3.3*1000/1030,places=10)
    def test_supply_range(self):
        for volts,expect in (('5','mcu_supply_voltage'),('2.5','mcu_supply_voltage'),('3.3',None)):
            t=self.example();one(component(t,'V1'),'value')[1]=A(volts);rc,r=self.analyze(t)
            if expect:self.assertEqual(rc,2,volts);self.assertIn(expect,rules(r,'error'))
            else:self.assertNotIn('mcu_supply_voltage',rules(r))
    def test_input_threshold_strap_and_reset(self):
        t=self.example();u=component(t,'U1');one(u,'drive')[1][1]=A('input')    # IO4 input; R2 alone pulls it to GND
        rc,r=self.analyze(t);self.assertNotIn('mcu_input_undefined',rules(r))
        comps=one(t,'components');comps.append(parse_sexpr('(component R9 (part resistor) (value 1000) (pins (1 3V3) (2 IO4)) (at 30 58 0))'))
        rc,r=self.analyze(t);i=next(i for i in r['issues'] if i['rule']=='mcu_input_undefined');self.assertAlmostEqual(i['measured'],1.65,places=10)
        t=self.example();u=component(t,'U1');one(u,'nc').remove(A('25'));one(u,'pins').append(sx('25',A('IO0')));one(t,'nets').append(A('IO0'))
        one(t,'components').append(parse_sexpr('(component R8 (part resistor) (value 10000) (pins (1 3V3) (2 IO0)) (at 30 58 0))'))
        rc,r=self.analyze(t);self.assertIn('mcu_strap_pin',rules(r,'warning'));self.assertNotIn('mcu_programming',rules(r))
        t=self.example();c1=component(t,'R1');one(c1,'pins')[1][1]=A('GND')         # EN pulled to ground: held in reset
        rc,r=self.analyze(t);self.assertIn('mcu_reset',rules(r,'warning'))
    def test_contract_errors(self):
        def mutate(fn):
            t=self.example();fn(t);path=self.dir/'p.copper';copper.write_tree(path,t);return self.core('analyze',path)
        def drive(pin,state):return lambda t:one(component(t,'U1'),'drive').append(sx(pin,A(state)))
        def input_only(t):  # IO34 (pin 6) is input-only: connect it, then try to drive it
            u=component(t,'U1');one(u,'nc').remove(A('6'));one(u,'pins').append(sx('6',A('IO4')));one(u,'drive').append(sx('6',A('high')))
        cases=[(input_only,'cannot drive an output'),(drive('8','high'),'not connected in (pins ...)'),(drive('26','low'),'driven twice'),
               (drive('2','high'),'is not a GPIO'),(lambda t:component(t,'R1').append(parse_sexpr('(drive (1 high))')),'needs a part with an (mcu'),
               (lambda t:one(component(t,'U1'),'nc').append(A('26')),'both connected and (nc'),
               (lambda t:one(component(t,'U1'),'pins').__setitem__(slice(1,2),[]),'ground pin 1'),
               (lambda t:one(component(t,'U1'),'pins')[1].__setitem__(1,A('EN')),'ground pins of esp32_wroom_32e are on different nets')]
        for fn,message in cases:
            with self.subTest(message):r=mutate(fn);self.assertEqual(r.returncode,2);self.assertIn(message,r.stderr)
    def test_transient_and_ac_are_blocked_not_faked(self):
        path=self.dir/'p.copper';copper.write_tree(path,self.example())
        r=self.core('transient',path,1e-6,3);self.assertEqual(r.returncode,2);self.assertIn('DC constraint checks only',r.stderr)
    def test_target_on_undefined_net_fails(self):
        t=self.example();t.append(parse_sexpr('(targets (voltage TXD0 3 3.6))'));rc,r=self.analyze(t)
        self.assertEqual(rc,2);self.assertIn('voltage_target',rules(r,'error'))
    def test_floating_net_without_boundary_still_fails(self):
        t=self.example();one(t,'nets').append(A('FLOAT'));rc,r=self.analyze(t);self.assertFalse(r['dc_solved'])

class Geometry(Base):
    """Keepouts, no-connect pads, pad drills and stacked pads, on hand-written geometry."""
    def board(self,*items):
        t=copper.load(ROOT/'examples/v2/divider.copper')
        geo=[parse_sexpr(x) for x in ('(pad R1 1 VIN 10 10 1 0 0 1 1 1 0)','(pad R1 2 OUT 12 10 1 0 0 1 1 1 0)','(pad R2 1 OUT 20 10 1 0 0 1 1 1 0)',
             '(pad R2 2 GND 22 10 1 0 0 1 1 1 0)','(pad J1 1 VIN 10 20 1.7 1 0 1.7 1.7 1 0)','(pad J1 2 OUT 12.54 20 1.7 1 0 1.7 1.7 1 0)',
             '(pad J1 3 GND 15.08 20 1.7 1 0 1.7 1.7 1 0)','(body R1 11 10 3 1.5 0)','(body R2 21 10 3 1.5 0)','(body J1 12.5 20 8 3 0)')]
        t.append(sx('geometry',*geo,*[parse_sexpr(x) for x in items]));return t
    def test_keepout_enforced_on_copper_and_bodies(self):
        keep='(keepout R2 25 2 35 8 3 15)'
        rc,r=self.analyze(self.board(keep));self.assertNotIn('keepout_violation',rules(r))
        for item,what in (('(trace VIN 26 3 30 3 0.25 2)','track'),('(via OUT 30 5 0.6 0.3)','via'),('(body R1 30 5 2 2 0)','assembly envelope')):
            rc,r=self.analyze(self.board(keep,item));v=[i for i in r['issues'] if i['rule']=='keepout_violation']
            self.assertTrue(v,item);self.assertIn(what,v[0]['message'])
        rc,r=self.analyze(self.board('(keepout R2 25 2 35 8 1 1)','(trace VIN 26 3 30 3 0.25 2)'))
        self.assertNotIn('keepout_violation',rules(r),'a front-only track keepout does not restrict B.Cu')
        rc,r=self.analyze(self.board('(keepout R2 21 9 23 11 3 15)'));self.assertNotIn('keepout_violation',rules(r),'the owner\'s own pads and body are exempt')
        for bad in ('(keepout R2 35 2 25 8 3 15)','(keepout R2 25 2 35 8 0 15)','(keepout R2 25 2 35 8 3 16)','(keepout NOPE 25 2 35 8 3 15)'):
            self.assertEqual(self.analyze(self.board(bad))[0],2,bad)
    def test_nc_pads_and_drills(self):
        t=self.board('(pad J1 4 ~ 17.62 20 1.7 1 0 1.7 1.7 1 0)');rc,err=self.analyze(t)
        self.assertEqual(rc,2);self.assertIn('not an (nc ...) pin',err)
        one(component(t,'J1'),'pins')  # J1 has pins 1..3; declare pin 4 deliberately unconnected
        component(t,'J1').append(parse_sexpr('(nc 4)'));rc,r=self.analyze(t);self.assertNotIn('pad_drill',rules(r))
        t=self.board('(pad J1 3 GND 25 25 0.6 0.2 0 0.6 0.6 0 0)');rc,r=self.analyze(t);self.assertIn('pad_drill',rules(r,'error'))
        t.append(parse_sexpr('(rules (drill_mm 0.2))'));rc,r=self.analyze(t);self.assertNotIn('pad_drill',rules(r))
    def test_stacked_pad_copies_are_one_feature(self):
        rc,r=self.analyze(self.board('(pad R2 2 GND 22.6 10 1 0 0 1 1 1 0)'));self.assertNotIn('mask_web',rules(r))
        rc,r=self.analyze(self.board('(pad R2 1 OUT 21.3 10 1 0 0 1 1 1 0)'));self.assertTrue({'mask_web','copper_clearance','short_circuit'}&set(rules(r,'error')))

class Import(Base):
    def importer(self,text,*extra,sym=SNAP_SYM,fp=SNAP_FP,into=None):
        src=self.dir/'in.parts';src.write_text(text,encoding='utf-8');out=self.dir/'r.json'
        argv=['--format','quiet','--result-json',str(out),'part-import',str(src),'--symbol-dir',str(sym),'--footprint-dir',str(fp),'--core',CORE,*extra]
        if into:argv+=['--into',str(into)]
        with contextlib.redirect_stderr(io.StringIO()):rc=copper.main(argv)
        return rc,json.loads(out.read_text())
    R='(part prec_r (kind R) (symbol "Device:R") (footprint "Resistor_SMD:R_0603_1608Metric") (tolerance 0.001) (provenance "synthetic test entry"))'
    def test_accept_and_merge(self):
        into=self.dir/'mine.parts';rc,doc=self.importer(f'(catalogue {self.R})',into=into)
        self.assertEqual((rc,doc['status'],doc['data']['accepted']),(0,'PASS',['prec_r']));self.assertTrue(into.is_file())
        self.assertEqual(self.core('catalogue',into).returncode,0)
        rc,doc=self.importer(f'(catalogue {self.R})',into=into);self.assertEqual((doc['status'],doc['data']['duplicates']),('INVALID_INPUT',['prec_r']))
    def test_rejections(self):
        cases=[('(part p1 (kind R) (symbol "Device:R") (footprint "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical"))','pad parity'),
               ('(part p2 (kind D) (symbol "Device:D") (footprint "Resistor_SMD:R_0603_1608Metric") (pinmap (cathode 2) (anode 1)))','swapped'),
               ('(part p3 (kind R) (symbol "Device:R") (footprint "Resistor_SMD:R_0603_1608Metric") (power_w 0.1))','need (provenance'),
               ('(part p4 (kind R) (symbol "Device:NoSuchSymbol") (footprint "Resistor_SMD:R_0603_1608Metric"))','Symbol not found'),
               ('(part p5 (kind R) (symbol "Device:R") (footprint "Resistor_SMD:R_9999"))','not found'),
               ('(part p6 (kind X))','unknown kind')]
        for text,message in cases:
            with self.subTest(message):
                rc,doc=self.importer(f'(catalogue {text})');self.assertEqual((rc,doc['status']),(2,'INVALID_INPUT'))
                self.assertIn(message,' '.join(doc['data']['rejected'][0]['reasons']))
        into=self.dir/'mine.parts';rc,doc=self.importer(f'(catalogue {self.R} {cases[0][0]})',into=into)
        self.assertEqual(doc['data']['accepted'],['prec_r']);self.assertFalse(into.exists(),'nothing is written unless every part is accepted')
        rc,doc=self.importer('(catalogue (part resistor (kind R) (symbol "Device:R") (footprint "Resistor_SMD:R_0603_1608Metric")))')
        self.assertEqual(doc['data']['duplicates'],['resistor'],'the bundled ids are taken')
    def test_mcu_pins_checked_against_symbol(self):
        # Device:R has pins 1 and 2: an MCU definition naming other pins is rejected, not trusted.
        text='(catalogue (part u (kind U) (symbol "Device:R") (footprint "Resistor_SMD:R_0603_1608Metric") (provenance "synthetic") (mcu (behavior "test only: not simulated") (supply 1) (ground 2) (operating_v 1 2) (gpio (pin 3 IO3 in)))))'
        rc,doc=self.importer(text);self.assertEqual(doc['status'],'INVALID_INPUT')
        reasons=' '.join(doc['data']['rejected'][0]['reasons']);self.assertIn('mcu gpio pin 3 is not a symbol pin',reasons)
    def test_missing_libraries_are_unverified(self):
        into=self.dir/'mine.parts';rc,doc=self.importer(f'(catalogue {self.R})',sym=self.dir/'none',fp=self.dir/'none',into=into)
        self.assertEqual((rc,doc['status']),(2,'UNVERIFIED'));self.assertFalse(into.exists())
    def test_bundled_catalogue_is_read_only(self):
        rc,doc=self.importer(f'(catalogue {self.R})',into=copper.BUNDLED_CATALOGUE);self.assertEqual(doc['status'],'INVALID_INPUT')

class Native(Base):
    """The ESP32 module and the MOSFET variant through project parse, model validation, PCB generation and,
    with KiCad 9.x, native ERC/DRC and independent netlist parity."""
    def libraries(self):
        sym,fp=os.environ.get('KICAD_SYMBOL_DIR'),os.environ.get('KICAD_FOOTPRINT_DIR')
        if not sym or not fp:self.skipTest('UNVERIFIED: set KICAD_SYMBOL_DIR and KICAD_FOOTPRINT_DIR to KiCad 9.x libraries')
        return Path(sym),Path(fp)
    def mosfet_project(self):
        t=self.example();one(t,'parts').append(direct(copper.load(NMOS),'part')[0])
        one(t,'nets').extend([A('GATE'),A('DRAIN')]);r2=component(t,'R2');one(r2,'pins')[2][1]=A('GATE');one(r2,'value')[1]=A('100')
        for text in ('(component R3 (part resistor) (value 100000) (pins (1 GATE) (2 GND)) (at 42 55 0))',
                     '(component Q1 (part nmos_switch_test) (pins (1 GATE) (2 GND) (3 DRAIN)) (at 36 58 0))',
                     '(component R4 (part resistor) (value 1000) (pins (1 3V3) (2 DRAIN)) (at 44 59 0))'):one(t,'components').append(parse_sexpr(text))
        return t
    def test_mosfet_project_solves(self):
        rc,r=self.analyze(self.mosfet_project());self.assertTrue(r['dc_solved']);self.assertEqual(r['errors'],0,r['issues'])
        nodes={n['name']:n['voltage_v'] for n in r['nodes']};self.assertLess(nodes['DRAIN'],0.5,'the GPIO-driven MOSFET is on')
    def build(self,tree,name):
        sym,fp=self.libraries();src=self.dir/f'{name}.copper';copper.write_tree(src,tree)
        imported={}
        copper.part_import(type('A',(),dict(file=str(MCU_CATALOGUE),into=None,symbol_dir=str(sym),footprint_dir=str(fp),core=CORE,format='quiet',outcome=imported))())
        if imported['status']=='UNVERIFIED':self.skipTest('UNVERIFIED: '+imported['message'])
        self.assertEqual(imported['status'],'PASS',imported['message'])  # symbol/footprint pad parity of the module
        args=type('Args',(),dict(project=str(src),out=str(self.dir/name),core=CORE,symbol_dir=str(sym),footprint_dir=str(fp),no_route=False))()
        try:
            with contextlib.redirect_stdout(io.StringIO()):rc=copper.export_project(args)
        except copper.Unverified as exc:self.skipTest(f'UNVERIFIED: {exc}')
        return self.dir/name,rc
    def test_build_and_native_gates(self):
        for name,tree in (('esp32',self.example()),('mosfet',self.mosfet_project())):
            with self.subTest(name):
                out,rc=self.build(tree,name);self.assertEqual(rc,0)
                report=json.loads((out/'report.json').read_text());self.assertEqual(report['errors'],0,[i for i in report['issues'] if i['severity']=='error'])
                manifest=json.loads((out/'manifest.json').read_text());self.assertEqual(manifest['status'],copper.STATUS_UNVERIFIED,'unknown ratings block a verified build')
                self.assertIn('(keepout U1',(out/'resolved.copper').read_text())
                cli=copper.discover_cli();version=subprocess.run([cli,'version'],capture_output=True,text=True).stdout.strip() if cli else ''
                if not version.startswith('9.'):self.skipTest(f'UNVERIFIED: KiCad 9.x CLI required for native gates (found {version or "none"})')
                with contextlib.redirect_stdout(io.StringIO()):code=copper.verify(type('V',(),dict(out=str(out),kicad_cli=cli,core=CORE,project=None,format='quiet'))())
                v=json.loads((out/'verification.json').read_text())
                self.assertEqual({g['gate']:g['status'] for g in v['gates']},{'erc':'PASS','drc':'PASS','netlist':'PASS','netlist_parity':'PASS'})
                self.assertEqual(v['status'],'UNVERIFIED','unknown ratings keep the verification UNVERIFIED');self.assertEqual(code,2)

if __name__=='__main__':
    result=unittest.main(verbosity=2,exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
