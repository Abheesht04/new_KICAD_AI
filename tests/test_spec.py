"""EngineeringSpec IR and constrained-English frontend (docs/engineering/ENGINEERING_SPEC.md).

Usage: test_spec.py COPPER_CORE [unittest args]
Exit status: 0 all ran and passed, 1 failure.
"""
import ast, copy, json, math, os, random, sys, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CORE=str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv)>1 and not sys.argv[1].startswith('-') else None
if CORE:os.environ['COPPER_CORE']=CORE
sys.path.insert(0,str(ROOT/'tools'))
import copper, spec_ir, spec_nlp
from hwc_kicad import one, direct, text_at
FIXTURES=ROOT/'tests/fixtures/spec'
CORPUS=json.loads((FIXTURES/'corpus.json').read_text(encoding='utf-8'))['cases']

def outcome(text):
    """(status, errors, question fields) exactly as the CLI decides them."""
    spec,parse_errors=spec_nlp.parse(text);errors,questions=spec_ir.validate(spec);errors=parse_errors+errors
    return ('INVALID_INPUT' if errors else 'UNVERIFIED' if questions else 'PASS'),errors,[q.field for q in questions],spec

def value_of(text,ref):
    spec,errors=spec_nlp.parse(text);c=spec.component(ref)
    assert not errors and c is not None,(text,errors)
    return c.value

class Corpus(unittest.TestCase):
    def test_corpus(self):
        for case in CORPUS:
            with self.subTest(case['name']):
                status,errors,questions,spec=outcome(case['text'])
                self.assertEqual(status,case['status'],(errors,questions))
                for field in case.get('questions',[]):self.assertIn(field,questions)
                for text in case.get('errors',[]):self.assertTrue(any(text in e for e in errors),(text,errors))
                if 'same_as' in case:
                    hand=spec_ir.load(FIXTURES/case['same_as']);self.assertEqual(spec_ir.validate(hand),([],[]))
                    self.assertEqual(spec_ir.fingerprint(spec),spec_ir.fingerprint(hand),
                                     json.dumps([spec_ir.semantic(spec),spec_ir.semantic(hand)],indent=1))
    def test_deterministic(self):
        for case in CORPUS:
            a,_=spec_nlp.parse(case['text']);b,_=spec_nlp.parse(case['text'])
            self.assertEqual(spec_ir.dumps(a),spec_ir.dumps(b),case['name'])

class Quantities(unittest.TestCase):
    def test_capacitance_aliases(self):
        for written in ('1uF','1 uF','1 µF','1 μF','1u','1e-6 F','1000nF','1000 nF','0.001mF','1uf','1UF'):
            q=value_of(f'a {written} capacitor C1 between VIN and GND',"C1")
            self.assertAlmostEqual(q.value,1e-6,delta=1e-18,msg=written);self.assertEqual(q.unit,'F',written)
        self.assertAlmostEqual(value_of('a 100n capacitor C1 between SIG and GND','C1').value,100e-9,delta=1e-20)
        self.assertAlmostEqual(value_of('a 4n7 capacitor C1 between SIG and GND','C1').value,4.7e-9,delta=1e-20)
    def test_voltage_and_resistance_aliases(self):
        for written in ('3V3','3.3 V','3.3V','3.3 volts','3300 mV'):
            q=value_of(f'a {written} supply V1 on VDD','V1');self.assertAlmostEqual(q.value,3.3,places=12,msg=written);self.assertEqual(q.unit,'V')
        for written,ohms in (('4k7',4700),('4K7',4700),('4.7k',4700),('4.7 kohm',4700),('4.7kΩ',4700),('4.7 kΩ',4700),
                             ('4r7',4.7),('1M',1e6),('1 Mohm',1e6),('1meg',1e6),('10m',0.01),('100',100)):
            q=value_of(f'a {written} resistor R1 between SIG and GND','R1');self.assertAlmostEqual(q.value,ohms,delta=ohms*1e-12,msg=written);self.assertEqual(q.unit,'ohm')
    def test_requirement_bounds(self):
        base='a 5 V supply on VIN. a 1k resistor from VIN to OUT. a 2k resistor from OUT to GND. '
        for written,(lo,hi) in (('OUT must be 3.3 V ± 3%',(3.201,3.399)),('OUT must be 3.3 V +/- 0.1 V',(3.2,3.4)),
                                ('OUT must be between 3.2 V and 3.4 V',(3.2,3.4)),('OUT must be at least 3 V',(3,None)),('OUT must be below 4V',(None,4))):
            status,errors,_,spec=outcome(base+written);self.assertEqual(status,'PASS',(written,errors));r=spec.requirements[0]
            for q,expected in ((r.minimum,lo),(r.maximum,hi)):
                if expected is None:self.assertIsNone(q,written)
                else:self.assertAlmostEqual(q.value,expected,places=9,msg=written)
    def test_unit_provenance(self):
        self.assertEqual(value_of('a 10k resistor R1 between SIG and GND','R1').provenance,'inferred')
        self.assertEqual(value_of('a 10 kohm resistor R1 between SIG and GND','R1').provenance,'explicit')
    def test_explicit_invalid_is_an_error_omitted_is_a_question(self):
        status,errors,questions,_=outcome('a resistor R1 between VIN and GND')
        self.assertEqual((status,errors,questions),('UNVERIFIED',[],['R1.value']))
        for bad,message in (('0 ohm','greater than zero'),('-10k','greater than zero'),('NaN ohm','not a finite'),('inf ohm','not a finite'),
                            ('-inf','not a finite'),('5 V','unit V where ohm'),('1 uF','unit F where ohm')):
            status,errors,_,_=outcome(f'a {bad} resistor R1 between VIN and GND')
            self.assertEqual(status,'INVALID_INPUT',bad);self.assertTrue(any(message in e for e in errors),(bad,errors))
        status,errors,_,_=outcome('a -1uF capacitor between VIN and GND');self.assertEqual(status,'INVALID_INPUT')
        status,errors,_,_=outcome('a 10k resistor R1 between VIN and GND. a 1k LED from VIN to GND')
        self.assertTrue(any('takes no value' in e for e in errors),errors)
    def test_two_values_or_references_are_rejected(self):
        for text in ('a 10k 20k resistor between SIG and GND','R1 is a resistor R2 between SIG and GND','a 10k resistor R1. a 20k resistor R1'):
            self.assertEqual(outcome(text)[0],'INVALID_INPUT',text)

class Ambiguity(unittest.TestCase):
    def test_esp32_family_is_not_a_part(self):
        for text in ('use an ESP32','an ESP32 module U1','an ESP32 board U1','a microcontroller U1'):
            status,_,questions,_=outcome(text+'. U1 GPIO2 drives D1. a red LED D1 from SIG to GND' if 'U1' in text else text)
            self.assertEqual(status,'UNVERIFIED',text);self.assertIn('U1.variant',questions,text)
        status,_,questions,spec=outcome('an ESP32-S3-WROOM-1 module U1. U1 GPIO4 drives D1. a red LED D1 from IO4 to GND')
        self.assertEqual(status,'PASS',questions);self.assertEqual(spec.component('U1').attrs['variant'].value,'ESP32-S3-WROOM-1')
        self.assertEqual(spec.gpio[0].pin,'GPIO4')
    def test_power_input_needs_voltage_and_net(self):
        for text in ('powered by USB','the board is powered from a battery','a USB input','a supply V1 on VIN'):
            status,_,questions,_=outcome(text);self.assertEqual(status,'UNVERIFIED',text)
            self.assertTrue({'power.input','V1.value'}&set(questions),(text,questions))
        status,_,_,spec=outcome('powered from 5 V on VIN. a 1k resistor from VIN to GND');self.assertEqual(status,'PASS')
        self.assertEqual([(k.terminal,k.net,k.provenance) for k in spec.connections if k.ref=='V1'],[('1','VIN','explicit'),('2','GND','inferred')])
    def test_coreference(self):
        status,errors,_,_=outcome('a red LED from SIG to GND. a green LED from SIG2 to GND. GPIO2 drives the LED. an ESP32-WROOM-32E module')
        self.assertEqual(status,'INVALID_INPUT');self.assertTrue(any('ambiguous' in e for e in errors),errors)
        status,_,_,spec=outcome('a red LED from SIG to GND. a green LED from SIG2 to GND. GPIO2 drives the green LED. an ESP32-WROOM-32E module')
        self.assertEqual(status,'PASS');self.assertEqual(spec.gpio[0].load,'D2')
    def test_led_direction(self):
        _,_,_,spec=outcome('a red LED D1 from OUT to GND')
        self.assertEqual({k.terminal:k.net for k in spec.connections},{'2':'OUT','1':'GND'})   # anode (2) on OUT
        _,_,_,spec=outcome('a red LED D1. D1 anode to OUT. D1 cathode to GND')
        self.assertEqual({k.terminal:k.net for k in spec.connections},{'2':'OUT','1':'GND'})

class Connections(unittest.TestCase):
    def test_malformed_and_contradictory(self):
        for text,message in (('a 1k resistor R1. R1 pin 3 to VIN',"no terminal '3'"),('a 1k resistor R1 from VIN to GND. R1 pin 1 to OUT','connected to both'),
                             ('a 1k resistor R1. R1 pin 1 to 3V3','not a net name'),('a 1k resistor R1 from GND to GND','both terminals'),
                             ('a 1k resistor R1 from VIN','cannot read the connection'),('R9 pin 1 to VIN','R9 is not declared'),
                             ('a 5 V supply V1. V1 anode to VIN',"no terminal 'anode'")):
            status,errors,_,_=outcome(text)
            if message=='both terminals':
                # A part shorted onto one net is legal in the spec but rejected by the compiler.
                self.assertEqual(status,'PASS',errors);continue
            self.assertEqual(status,'INVALID_INPUT',text);self.assertTrue(any(message in e for e in errors),(text,errors))
    def test_repeated_connection_is_idempotent(self):
        a=outcome('a 1k resistor R1 from VIN to GND')[3];b=outcome('a 1k resistor R1 from VIN to GND. R1 pin 1 to VIN. R1.2 to GND')[3]
        self.assertEqual(spec_ir.fingerprint(a),spec_ir.fingerprint(b))
    def test_unconnected_terminal_is_a_question(self):
        self.assertEqual(outcome('a 1k resistor R1. R1 pin 1 to GND')[2],['R1.2'])

class Format(unittest.TestCase):
    def setUp(self):self.hand=(FIXTURES/'divider.espec').read_text(encoding='utf-8')
    def test_roundtrip_and_canonical(self):
        spec=spec_ir.loads(self.hand);text=spec_ir.dumps(spec)
        self.assertEqual(spec_ir.dumps(spec_ir.loads(text)),text)
        self.assertEqual(spec_ir.fingerprint(spec_ir.loads(text)),spec_ir.fingerprint(spec_ir.load(FIXTURES/'divider.espec')))
    def test_fingerprint_is_order_and_provenance_independent(self):
        spec=spec_ir.loads(self.hand);base=spec_ir.fingerprint(spec)
        other=copy.deepcopy(spec);rng=random.Random(7)
        rng.shuffle(other.components);rng.shuffle(other.connections)
        for c in other.components:c.provenance='defaulted';c.value and setattr(c.value,'provenance','inferred')
        other.name=spec_ir.Attr('renamed');other.source={'kind':'nlp','text':'anything'}
        self.assertEqual(spec_ir.fingerprint(other),base)
        changed=copy.deepcopy(spec);changed.component('R2').value.value=22000
        self.assertNotEqual(spec_ir.fingerprint(changed),base)
        changed=copy.deepcopy(spec);changed.connections[0].net='VX'
        self.assertNotEqual(spec_ir.fingerprint(changed),base)
    def test_structural_errors(self):
        cases=[self.hand.replace('(version 1)','(version 2)'),self.hand.replace('(name "divider")','(name "divider") (colour red)'),
               self.hand.replace('(value 5 V)','(value 5 volts)'),self.hand.replace('(value 5 V)','(value 5 V guessed)'),
               self.hand.replace('(value 5 V)','(value five V)'),self.hand.replace('(role supply)',''),'(engineering_spec (version 1)',
               '(copper_project (version 2))',self.hand.replace('(connect V1 + VIN)','(connect V1 +)')]
        for text in cases:
            with self.assertRaises(spec_ir.SpecFormatError,msg=text[:200]):spec_ir.loads(text)
    def test_semantic_errors_in_handwritten_spec(self):
        for old,new,message in (('(role supply)','(role transformer)','unknown role'),('(value 10000 ohm)','(value 10000 F)','unit F where ohm'),
                                ('(value 10000 ohm)','(value nan ohm)','not a finite'),('(connect R2 2 GND)','(connect R2 2 GND) (connect R2 2 OUT)','connected to both'),
                                ('(tolerance 0.01)','(tolerance 1.5)','tolerance must be'),('(layers 2)','(layers 2.5)','whole number'),
                                ('(min 3.2 V) (max 3.4 V)','(min 3.6 V) (max 3.4 V)','minimum exceeds maximum'),('(start 10 Hz)','(start 1e6 Hz)','start exceeds stop'),
                                ('(component R2','(component R1','duplicate component')):
            errors,_=spec_ir.validate(spec_ir.loads(self.hand.replace(old,new)))
            self.assertTrue(any(message in e for e in errors),(old,errors))
    def test_carried_question_closes_when_answered(self):
        spec,_=spec_nlp.parse('a 5 V supply on VIN. a diode D1 between VIN and OUT. a 1k resistor from OUT to GND')
        _,questions=spec_ir.validate(spec);self.assertEqual([q.field for q in questions],['D1.polarity'])
        spec.unresolved=questions;text=spec_ir.dumps(spec)
        reloaded=spec_ir.loads(text);self.assertEqual([q.field for q in spec_ir.validate(reloaded)[1]],['D1.polarity'])
        answered=spec_ir.loads(text.replace('(connections','(connections\n    (connect D1 anode VIN)\n    (connect D1 cathode OUT)'))
        self.assertEqual(spec_ir.validate(answered),([],[]))

@unittest.skipUnless(CORE,'needs copper-core')
class Lowering(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.dir=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def test_divider_lowers_to_a_project_the_core_accepts(self):
        tree,notes=spec_ir.lower(spec_ir.load(FIXTURES/'divider.espec'))
        self.assertEqual([str(x) for x in one(tree,'nets')[1:]],['GND','OUT','VIN'])
        comps={str(c[1]):c for c in direct(one(tree,'components'),'component')}
        self.assertEqual(sorted(comps),['R1','R2','V1']);self.assertIsNone(one(comps['R1'],'at'),'no invented placement')
        self.assertEqual({str(p[0]):str(p[1]) for p in one(comps['V1'],'pins')[1:]},{'1':'VIN','2':'GND'})
        self.assertEqual([str(x) for x in one(tree,'targets')[1]],['voltage','OUT','3.2','3.4'])
        self.assertTrue(any('tolerance' in n for n in notes));self.assertTrue(any('placement' in n for n in notes))
        path=self.dir/'d.copper';copper.write_tree(path,tree);report,rc=copper.core_run(path)
        self.assertTrue(report['dc_solved']);self.assertAlmostEqual(next(n['voltage_v'] for n in report['nodes'] if n['name']=='OUT'),10/3,places=9)
    def test_nlp_and_handwritten_lower_identically(self):
        nlp,_=spec_nlp.parse(CORPUS[0]['text'])
        self.assertEqual(copper.dump_sexpr(spec_ir.lower(nlp)[0]),copper.dump_sexpr(spec_ir.lower(spec_ir.load(FIXTURES/'divider.espec'))[0]))
    def test_incomplete_or_unbindable_specs_are_not_lowered(self):
        for text,exc in (('a 5 V supply on VIN. a capacitor from VIN to GND',copper.Unverified),
                         ('a 5 V supply on VIN. a 1k resistor from VIN to SIG. a red LED from SIG to GND',copper.Unverified),
                         ('a 5 V supply on VIN. a 1k resistor R1 from VIN to GND. the current through R1 must be below 10 mA',copper.Unverified),
                         ('a 0 ohm resistor from VIN to GND',copper.InvalidInput)):
            with self.assertRaises(exc,msg=text):spec_ir.lower(spec_nlp.parse(text)[0])
        hand=spec_ir.loads((FIXTURES/'divider.espec').read_text(encoding='utf-8').replace('(role resistor) (value 10000','(role resistor) (part "no_such_part") (value 10000'))
        with self.assertRaises(copper.Unverified):spec_ir.lower(hand)
        hand=spec_ir.loads((FIXTURES/'divider.espec').read_text(encoding='utf-8').replace('(layers 2)','(layers 4)'))
        with self.assertRaises(copper.Unverified):spec_ir.lower(hand)
    def test_explicit_part_binding_uses_the_catalogue(self):
        catalogue=self.dir/'user.parts'
        catalogue.write_text('(catalogue (part red_led (kind D) (symbol "Device:LED") (footprint "LED_SMD:LED_0603_1608Metric") (is_a 1e-20) (ideality 2)))\n',encoding='utf-8')
        spec,_=spec_nlp.parse('a 5 V supply on VIN. a 1k resistor from VIN to SIG. a red LED D1 from SIG to GND')
        spec.component('D1').attrs['part']=spec_ir.Attr('red_led')
        tree,_=spec_ir.lower(spec,str(catalogue))
        d1=next(c for c in direct(one(tree,'components'),'component') if str(c[1])=='D1');self.assertEqual(text_at(d1,'part'),'red_led')
        path=self.dir/'led.copper';copper.write_tree(path,tree);report,_=copper.core_run(path);self.assertTrue(report['dc_solved'])

class Independence(unittest.TestCase):
    RETIRED=('nlp_rules','transformer','Extras/','tokenizer')
    def test_frontend_shares_nothing_with_the_retired_nlp(self):
        for name in ('spec_nlp.py','spec_ir.py'):
            source=(ROOT/'tools'/name).read_text(encoding='utf-8')
            imports=set()
            for node in ast.walk(ast.parse(source)):
                if isinstance(node,ast.Import):imports|={a.name for a in node.names}
                if isinstance(node,ast.ImportFrom):imports.add(node.module)
            self.assertLessEqual(imports,{'__future__','hashlib','json','math','re','unicodedata','dataclasses','pathlib','typing',
                                          'hwc_kicad','spec_ir','copper'},name)
            code='\n'.join(l for l in source.splitlines() if not l.lstrip().startswith('#'))
            for word in self.RETIRED:self.assertNotIn(word,code.replace('retired NLP code',''),f'{name} mentions {word}')

if __name__=='__main__':
    result=unittest.main(exit=False,verbosity=2).result
    sys.exit(0 if result.wasSuccessful() else 1)
