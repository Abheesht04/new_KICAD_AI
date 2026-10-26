"""Circuit blocks (docs/engineering/BLOCKS.md): every value has a source, missing data is a question, and the
lowered project is checked by copper-core, including the ideal regulator model and its limits.

Usage: test_blocks.py COPPER_CORE [unittest args]
Exit status: 0 all ran and passed, 1 failure.
Parts other than the bundled generics come from tests/fixtures/blocks/test_parts.parts: TEST ONLY stand-ins with
synthetic numbers, and the ESP32 module catalogue for the MCU blocks.
"""
import os, re, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
os.environ['COPPER_CORE'] = CORE
sys.path.insert(0, str(ROOT / 'tools'))
import blocks, copper, spec_ir, spec_nlp
from hwc_kicad import direct, one, parse_sexpr, text_at
TEST_PARTS = ROOT / 'tests/fixtures/blocks/test_parts.parts'
I2C = 'Name: bus. A 3.3 V supply V1 on VDD. use block i2c_pullups: vdd=VDD scl=SCL sda=SDA mode=fast bus_capacitance=100pF'


def parse(text):
    spec, errors = spec_nlp.parse(text)
    assert not errors, errors
    return spec


def comps(tree):
    return {str(c[1]): c for c in direct(one(tree, 'components'), 'component')}


def pins(c):
    return {str(p[0]): str(p[1]) for p in one(c, 'pins')[1:]}


def analyze(tree):
    with tempfile.TemporaryDirectory() as d:
        copper.write_tree(Path(d) / 'p.copper', tree)
        return copper.core_run(Path(d) / 'p.copper', CORE)[0]


def errors_of(report):
    return {i['rule'] for i in report['issues'] if i['severity'] == 'error'}


class I2cPullups(unittest.TestCase):
    def test_values_follow_the_specification(self):
        for mode, cb, expected in (('fast', '100pF', 3300), ('standard', '400pF', 2700), ('fast_plus', '550pF', 240)):
            tree, notes = spec_ir.lower(parse(I2C.replace('mode=fast', f'mode={mode}').replace('100pF', cb)))
            values = {float(text_at(c, 'value')) for r, c in comps(tree).items() if r.startswith('R')}
            self.assertEqual(values, {expected}, (mode, cb))
            self.assertTrue(any('NXP UM10204' in n and f'{mode} mode' in n for n in notes), notes)
        c = comps(spec_ir.lower(parse(I2C))[0])
        self.assertEqual({(pins(c[r])['1'], pins(c[r])['2']) for r in ('R1', 'R2')}, {('VDD', 'SCL'), ('VDD', 'SDA')})

    def test_impossible_bus_and_missing_data(self):
        with self.assertRaises(copper.InvalidInput) as cm:
            spec_ir.lower(parse(I2C.replace('100pF', '1nF')))
        self.assertIn('no E24 value between', str(cm.exception))
        _, questions = spec_ir.validate(parse(I2C.replace(' bus_capacitance=100pF', '')))
        self.assertEqual([q.field for q in questions], ['block.i2c_pullups.bus_capacitance'], 'never a default bus capacitance')
        errors, _ = spec_ir.validate(parse(I2C + ' speed=9'))
        self.assertTrue(any("unknown parameter 'speed'" in e for e in errors), errors)
        errors, _ = spec_ir.validate(parse(I2C.replace('mode=fast', 'mode=turbo')))
        self.assertTrue(any('must be one of' in e for e in errors), errors)
        no_supply = I2C.replace('A 3.3 V supply V1 on VDD. ', 'R9 is a 1k resistor from VDD to GND. ')
        with self.assertRaises(copper.Unverified):
            spec_ir.lower(parse(no_supply))
        tree, _ = spec_ir.lower(parse(no_supply + ' vdd_voltage=3.3V'))
        self.assertEqual({float(text_at(c, 'value')) for r, c in comps(tree).items() if r != 'R9'}, {3300})

    def test_round_trip_and_fingerprint(self):
        spec = parse(I2C); again = spec_ir.loads(spec_ir.dumps(spec))
        self.assertEqual(spec_ir.fingerprint(again), spec_ir.fingerprint(spec))
        self.assertNotEqual(spec_ir.fingerprint(parse(I2C.replace('100pF', '200pF'))), spec_ir.fingerprint(spec))
        self.assertEqual(spec_ir.dumps(again), spec_ir.dumps(spec))


class PartBlocks(unittest.TestCase):
    def lower(self, text, catalogue=TEST_PARTS):
        return spec_ir.lower(parse(text), str(catalogue))

    def test_usb_c_sink(self):
        tree, notes = self.lower('Name: usb. A 5 V supply V1 on VBUS. use block usb_c_sink: connector=test_usb_c vbus=VBUS gnd=GND')
        c = comps(tree)
        self.assertEqual(pins(c['J1']), {'1': 'VBUS', '2': 'GND', '3': 'J1_CC1', '4': 'J1_CC2'})
        self.assertEqual([str(x) for x in one(c['J1'], 'nc')[1:]], ['5', '6'])
        self.assertEqual({(pins(c[r])['1'], float(text_at(c[r], 'value'))) for r in ('R1', 'R2')}, {('J1_CC1', 5100.0), ('J1_CC2', 5100.0)})
        self.assertTrue(any('USB Type-C Cable and Connector Specification' in n for n in notes))
        report = analyze(tree); self.assertTrue(report['dc_solved']); self.assertFalse(errors_of(report))

    def test_ldo_is_analysed_with_its_limits(self):
        base = 'Name: ldo. A {v} V supply V1 on VIN. use block ldo: regulator=test_ldo vin=VIN vout=VOUT gnd=GND. R1 is a {r} resistor from VOUT to GND.'
        tree, notes = self.lower(base.format(v=5, r='100'))
        c = comps(tree)
        self.assertEqual(float(text_at(c['U1'], 'value')), 3.3); self.assertEqual(pins(c['U1']), {'1': 'VIN', '2': 'VOUT', '3': 'GND'})
        caps = sorted((pins(c[r])['1'], float(text_at(c[r], 'value'))) for r in c if r.startswith('C'))
        self.assertEqual(caps, [('VIN', 1e-6), ('VOUT', 1e-6)])
        report = analyze(tree)
        volts = {n['name']: n['voltage_v'] for n in report['nodes']}
        self.assertAlmostEqual(volts['VOUT'], 3.3, places=9); self.assertFalse(errors_of(report))
        bounds = {n['name']: n['bound_a'] for n in report['net_current_bounds']}
        self.assertAlmostEqual(bounds['VIN'], 0.033 + 5e-5, places=12, msg='load plus quiescent current')
        self.assertIn('rating_unknown', {i['rule'] for i in report['issues'] if i['object'] == 'U1'})
        self.assertIn('regulator_dropout', errors_of(analyze(self.lower(base.format(v=3.5, r='100'))[0])))
        self.assertIn('regulator_current', errors_of(analyze(self.lower(base.format(v=5, r='10'))[0])))
        reverse = self.lower(base.format(v=5, r='100') + ' A 5 V supply V2 on VX. R2 is a 10 resistor from VX to VOUT.')[0]
        self.assertIn('regulator_reverse', errors_of(analyze(reverse)))

    def test_usb_c_to_ldo_to_sensor(self):
        text = ('Name: sensor_board. A 5 V supply V1 on VBUS. use block usb_c_sink: connector=test_usb_c vbus=VBUS gnd=GND. '
                'use block ldo: regulator=test_ldo vin=VBUS vout=VDD gnd=GND. '
                'use block i2c_sensor: device=test_sensor vdd=VDD gnd=GND scl=SCL sda=SDA. '
                'use block i2c_pullups: vdd=VDD scl=SCL sda=SDA mode=fast bus_capacitance=100pF vdd_voltage=3.3V')
        tree, notes = self.lower(text)
        c = comps(tree)
        self.assertEqual(pins(c['U2']), {'1': 'VDD', '2': 'GND', '3': 'SCL', '4': 'SDA'})
        self.assertTrue(any(r.startswith('C') and pins(c[r]) == {'1': 'VDD', '2': 'GND'} and float(text_at(c[r], 'value')) == 1e-7 for r in c),
                        'the sensor datasheet decoupling')
        sources = {n.split('(')[1].split(':')[0] for n in notes if n.startswith('block ')}
        self.assertEqual(sources, {'standard', 'part'}); self.assertEqual(len([n for n in notes if n.startswith('block ')]), 10)
        report = analyze(tree)
        self.assertTrue(report['dc_solved']); self.assertFalse(errors_of(report))
        self.assertIn('ic_supply_current', {i['rule'] for i in report['issues'] if i['object'] == 'U2'}, 'the unmodelled sensor is a declared boundary')

    def test_parts_without_datasheet_data_are_refused(self):
        with self.assertRaises(copper.Unverified) as cm:
            spec_ir.lower(parse('Name: x. A 5 V supply V1 on VIN. use block usb_c_sink: connector=resistor vbus=VIN gnd=GND'))
        self.assertIn('no (reference ...) datasheet data', str(cm.exception))
        with self.assertRaises(copper.Unverified):
            spec_ir.lower(parse('Name: x. A 5 V supply V1 on VIN. use block ldo: regulator=nope vin=VIN vout=V3 gnd=GND'), str(TEST_PARTS))
        with self.assertRaises(copper.InvalidInput):
            spec_ir.lower(parse('Name: x. A 5 V supply V1 on VIN. use block ldo: regulator=test_usb_c vin=VIN vout=V3 gnd=GND'), str(TEST_PARTS))


class Mcu(unittest.TestCase):
    def catalogue(self, d):
        merged = Path(d) / 'merged.parts'
        body = lambda p: p.read_text(encoding='utf-8').split('(catalogue', 1)[1].rsplit(')', 1)[0]
        merged.write_text('(catalogue' + body(ROOT / 'profiles/mcu_modules.parts') + body(TEST_PARTS) + ')\n', encoding='utf-8')
        return merged

    def test_reference_circuit_and_reset_boot(self):
        with tempfile.TemporaryDirectory() as d:
            text = ('Name: mcu. A 3.3 V supply V1 on VDD. U1 is an ESP32-WROOM-32E module. U1 uses part esp32_wroom_32e. '
                    'U1 pin 2 is on VDD. U1 pin 1 is on GND. use block reference_circuit: component=U1. '
                    'use block reset_boot: mcu=U1 button=test_button boot_pin=25')
            tree, notes = spec_ir.lower(parse(text), str(self.catalogue(d)))
            c = comps(tree)
            caps = sorted(float(text_at(x, 'value')) for r, x in c.items() if r.startswith('C'))
            self.assertEqual(caps, [1e-7, 1e-6, 1e-5], 'the module datasheet reference: 10 uF + 0.1 uF, EN 1 uF')
            self.assertTrue(any('Espressif ESP32-WROOM-32E/32UE datasheet' in n for n in notes))
            en = pins(c['U1'])['3']
            sw = {pins(c[r])['1']: pins(c[r])['2'] for r in c if r.startswith('SW')}
            self.assertEqual(sw, {en: 'GND', pins(c['U1'])['25']: 'GND'})
            self.assertTrue(any('IO0 selects the boot mode' in n for n in notes), 'the catalogue strapping note is the source')
            with self.assertRaises(copper.InvalidInput) as cm:
                spec_ir.lower(parse(text.replace('boot_pin=25', 'boot_pin=26')), str(self.catalogue(d)))
            self.assertIn('not a strapping pin', str(cm.exception))


class Kernel(unittest.TestCase):
    def check(self, parts_text):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'c.parts'; path.write_text(parts_text, encoding='utf-8')
            return copper.validate_catalogue(path, CORE)

    def test_reference_needs_provenance_and_u_binds_only_a_regulator(self):
        good = '(catalogue (part x (kind J) (symbol "S:x") (footprint "F:x") (reference (a 1) (provenance "datasheet"))))'
        self.check(good)
        with self.assertRaises(copper.InvalidInput):
            self.check(good.replace(' (provenance "datasheet")', ''))
        with self.assertRaises(copper.InvalidInput):
            self.check('(catalogue (part x (kind U) (model ideal.resistor) (symbol "S:x") (footprint "F:x")))')
        with self.assertRaises(copper.InvalidInput):
            self.check('(catalogue (part x (kind U) (model ideal.ldo) (iout_max_a 1) (symbol "S:x") (footprint "F:x")))')  # dropout required

    def test_blocks_are_listed(self):
        names = [b['name'] for b in blocks.describe()]
        self.assertEqual(names, ['reference_circuit', 'i2c_pullups', 'usb_c_sink', 'ldo', 'i2c_sensor', 'reset_boot'])
        self.assertTrue(all(p['meaning'] for b in blocks.describe() for p in b['params']))


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
