"""Design compiler acceptance (Prompt 9): "Build me an ESP32 LED PCB. Generate the KiCad PCB."

Usage: test_design.py COPPER_CORE [unittest args]
Exit status: 0 all ran and passed, 1 failure, 77 something was skipped (Native needs KiCad 9.x libraries).

Compiler classes substitute the library pin-name lookup with the pin table of RF_Module:ESP32-WROOM-32E
(numbers and names as read from the KiCad symbol; GND on 1/15/38/39 as separate pins) and of
Connector_Generic:Conn_01x06, so the compiler's decisions are tested without installed libraries.
Everything else, including copper-core's DC analysis of the result, runs for real.
"""
import copy, json, os, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
os.environ['COPPER_CORE'] = CORE
sys.path.insert(0, str(ROOT / 'tools'))
import copper, design, spec_ir, spec_nlp

REQUEST = 'Build me an ESP32 LED PCB. Generate the KiCad PCB.'
PROFILE = ROOT / 'profiles/esp32_led.design'
ESP32_PINS = {'1': 'GND', '2': 'VDD', '3': 'EN', '4': 'SENSOR_VP', '5': 'SENSOR_VN', '6': 'IO34', '7': 'IO35', '8': 'IO32',
              '9': 'IO33', '10': 'IO25', '11': 'IO26', '12': 'IO27', '13': 'IO14', '14': 'IO12', '15': 'GND', '16': 'IO13',
              '17': 'NC', '18': 'NC', '19': 'NC', '20': 'NC', '21': 'NC', '22': 'NC', '23': 'IO15', '24': 'IO2', '25': 'IO0',
              '26': 'IO4', '27': 'IO16', '28': 'IO17', '29': 'IO5', '30': 'IO18', '31': 'IO19', '32': 'NC', '33': 'IO21',
              '34': 'RXD0/IO3', '35': 'TXD0/IO1', '36': 'IO22', '37': 'IO23', '38': 'GND', '39': 'GND'}
HEADER_PINS = {str(i): f'Pin_{i}' for i in range(1, 7)}


def pin_table(part, symdir):
    symbol = copper.text_at(part, 'symbol')
    return {'RF_Module:ESP32-WROOM-32E': ESP32_PINS, 'RF_Module:ESP32-WROOM-32UE': ESP32_PINS,
            'Connector_Generic:Conn_01x06': HEADER_PINS}[symbol]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.dir = Path(self.tmp.name)
        self.saved = design.symbol_pin_names; design.symbol_pin_names = pin_table
    def tearDown(self):
        design.symbol_pin_names = self.saved; self.tmp.cleanup()
    def compile(self, text=REQUEST, answers='', profile=PROFILE):
        out = self.dir / f'c{len(list(self.dir.iterdir()))}'; out.mkdir()
        return design.compile_spec(text, design.load_profile(profile) if profile else None, design.parse_answers(answers), None, out, CORE)
    def stop(self, *args, **kw):
        with self.assertRaises(design.Stop) as ctx:
            self.compile(*args, **kw)
        return ctx.exception


class Intent(unittest.TestCase):
    """The request is read by the general grammar, not matched as a sentence."""
    def test_phrasings_give_the_same_intent(self):
        shapes = set()
        for text in (REQUEST, 'Make an ESP32 LED board.', 'design a board with an ESP32 and an LED', 'an ESP32 LED PCB'):
            spec, errors = spec_nlp.parse(text); self.assertEqual(errors, [], text)
            shapes.add(tuple(sorted((c.role, c.attrs['family'].value if 'family' in c.attrs else '') for c in spec.components)))
        self.assertEqual(shapes, {(('led', ''), ('mcu', 'ESP32'))})
    def test_output_request_and_pcb_extension(self):
        spec, _ = spec_nlp.parse(REQUEST); self.assertEqual(spec.outputs, ['kicad'])
        spec, _ = spec_nlp.parse('an ESP32 LED board. give me a .pcb file')
        self.assertEqual(spec.outputs, ['kicad']); self.assertIn('.kicad_pcb', spec.source['notes'][0])
        self.assertNotIn('outputs', json.dumps(spec_ir.semantic(spec)), 'requested outputs are not engineering content')
    def test_unknown_requests_are_errors(self):
        for text in ('build me a quantum flux PCB', 'make me a board with two LEDs'):
            self.assertTrue(spec_nlp.parse(text)[1], text)


class Questions(Base):
    def test_no_profile_asks_instead_of_guessing(self):
        e = self.stop(profile=None)
        self.assertEqual(e.status, 'UNVERIFIED')
        fields = {q['field'] for q in e.decisions.questions}
        self.assertEqual(fields, {'mcu_part', 'led_function', 'led_part', 'led_current', 'supply_kind', 'supply_voltage',
                                  'programming', 'board_width_mm', 'board_height_mm', 'layers', 'dfm'})
        mcu = next(q for q in e.decisions.questions if q['field'] == 'mcu_part')
        self.assertEqual(mcu['options'], ['esp32_wroom_32e', 'esp32_wroom_32ue']); self.assertIn('--answers mcu_part=', mcu['answer_with'])
    def test_answers_settle_questions_and_are_recorded(self):
        answers = ('mcu_part=esp32_wroom_32e,led_function=gpio_indicator,led_part=led_red_0603,led_current=0.005,supply_kind=header,'
                   'supply_voltage=3.3,programming=uart_header,programming_part=conn_1x06,board_width_mm=50,board_height_mm=70,layers=2,'
                   'dfm=' + str(ROOT / 'profiles/esp32_2layer.rules'))
        intent, spec, d, notes, _ = self.compile(answers=answers, profile=None)
        self.assertTrue(all(x['source'] != 'profile' for x in d.items))
        self.assertEqual(next(x for x in d.items if x['field'] == 'U1.part')['source'], 'answer')
    def test_text_beats_profile(self):
        _, spec, d, _, _ = self.compile('Build me a board with an ESP32-WROOM-32UE module and a red LED. GPIO4 drives the LED. '
                                        'The current through the LED must be at most 2 mA.')
        part = {x['field']: x for x in d.items}
        self.assertEqual((part['U1.part']['value'], part['U1.part']['source']), ('esp32_wroom_32ue', 'catalogue'))
        self.assertEqual((part['D1.gpio']['value'], part['D1.gpio']['source']), ('GPIO4', 'text'))
        self.assertEqual((part['D1.current_a']['value'], part['D1.current_a']['source']), (0.002, 'text'))
        _, _, _, notes, _ = self.compile('Build me an ESP32 LED PCB. GPIO2 drives the LED')
        self.assertTrue(any('strapping pin' in n for n in notes), notes)  # allowed when asked for, never silently


class Contradictions(Base):
    def test_invalid_and_unsupported(self):
        cases = [('', 'led_function=addressable', 'NOT_IMPLEMENTED', 'only gpio_indicator'),
                 ('', 'mcu_part=led_red_0603', 'INVALID_INPUT', 'not a catalogue MCU'),
                 ('', 'supply_voltage=5', 'INVALID_INPUT', 'outside esp32_wroom_32e operating range'),
                 ('', 'led_current=0.05', 'INVALID_INPUT', 'per-pin limit'),
                 ('', 'led_current=-1', 'INVALID_INPUT', 'not valid'),
                 ('', 'programming=none', 'NOT_IMPLEMENTED', 'needs the header'),
                 ('', 'frobnicate=1', 'INVALID_INPUT', 'unknown answer key'),
                 ('Build me an ESP32 and a green LED board', '', 'INVALID_INPUT', 'does not say that colour'),
                 ('a board with an ESP32 and an ESP32 and an LED', '', 'NOT_IMPLEMENTED', 'one microcontroller'),
                 ('Build me an ESP32 LED PCB. GPIO34 drives the LED', '', 'INVALID_INPUT', 'input-only'),
                 ('Build me an ESP32 LED PCB. GPIO1 drives the LED', '', 'INVALID_INPUT', 'already used')]
        for text, answers, status, message in cases:
            with self.subTest(text or answers):
                e = self.stop(text or REQUEST, answers)
                self.assertEqual(e.status, status, str(e))
                if message:
                    self.assertIn(message, str(e))


class Compiler(Base):
    def test_complete_design_from_catalogue_data(self):
        intent, spec, d, notes, catalogue = self.compile()
        roles = sorted((c.ref, c.role) for c in spec.components)
        self.assertEqual(roles, [('C1', 'capacitor'), ('C2', 'capacitor'), ('C3', 'capacitor'), ('D1', 'led'), ('J1', 'connector'),
                                 ('R1', 'resistor'), ('R2', 'resistor'), ('U1', 'mcu'), ('V1', 'supply')])
        nets = {(k.ref, k.terminal): k.net for k in spec.connections}
        self.assertEqual({p: nets[('U1', p)] for p in ('1', '15', '38', '39', '2', '3')}, {'1': 'GND', '15': 'GND', '38': 'GND', '39': 'GND', '2': 'VDD', '3': 'EN'})
        # LED polarity and series resistor: GPIO -> R2 -> anode; cathode -> GND.
        self.assertEqual((nets[('D1', '2')], nets[('D1', '1')]), ('D1_A', 'GND'))
        self.assertEqual({nets[('R2', '1')], nets[('R2', '2')]}, {nets[('U1', '8')], 'D1_A'})
        self.assertEqual(spec.component('R2').value.value, 300.0)
        # The GPIO avoids strapping, programming and reset pins; programming pins reach the header in catalogue order.
        gpio = next(x for x in d.items if x['field'] == 'D1.gpio'); self.assertEqual((gpio['value'], gpio['source']), ('IO32', 'rule'))
        self.assertEqual([nets[('J1', str(i))] for i in range(1, 7)], ['VDD', 'GND', 'TXD0_IO1', 'RXD0_IO3', 'IO0', 'EN'])
        self.assertEqual(len(spec.no_connect['U1']), 39 - 10)
        self.assertEqual(sorted(c.value.value for c in spec.components if c.role == 'capacitor'), [1e-07, 1e-06, 1e-05])
        self.assertTrue(all(x['reason'] for x in d.items)); self.assertEqual(spec_ir.validate(copy.deepcopy(spec)), ([], []))
    def test_led_current_is_verified_by_copper_core(self):
        _, spec, d, _, catalogue = self.compile()
        tree, notes = spec_ir.lower(spec, str(catalogue), CORE, base=catalogue.parent)
        report = copper.core_check(tree, CORE); self.assertTrue(report['dc_solved']); self.assertEqual(report['errors'], 0)
        current = next(c['current_a'] for c in report['calculations'] if c['ref'] == 'R2')
        self.assertLessEqual(current, 0.005); self.assertGreater(current, 0.004)
        u1 = next(c for c in report['components'] if c['ref'] == 'U1'); self.assertEqual(u1['drive'], {'8': 'high'})
        self.assertIn('mcu_supply_current', [i['rule'] for i in report['issues']])
    def test_compilation_is_deterministic(self):
        a = self.compile(); b = self.compile()
        self.assertEqual(spec_ir.dumps(a[1]), spec_ir.dumps(b[1])); self.assertEqual(a[2].items, b[2].items)


class Native(unittest.TestCase):
    """The whole acceptance run with installed KiCad 9.x libraries; gates 8 and 9 also need KiCad 9.x CLI."""
    def test_acceptance(self):
        sym, fp = os.environ.get('KICAD_SYMBOL_DIR'), os.environ.get('KICAD_FOOTPRINT_DIR')
        if not sym or not fp:
            self.skipTest('UNVERIFIED: set KICAD_SYMBOL_DIR and KICAD_FOOTPRINT_DIR to KiCad 9.x libraries')
        with tempfile.TemporaryDirectory() as d:
            r = design.run(REQUEST, Path(d) / 'out', PROFILE, None, sym, fp, None, CORE)
            if r['status'] in ('UNVERIFIED',) and r['gates'].get('parts', {}).get('status') == 'UNVERIFIED':
                self.skipTest('UNVERIFIED: ' + r['gates']['parts']['detail'])
            g = {k: v['status'] for k, v in r['gates'].items()}
            for name in ('spec', 'topology', 'parts', 'models', 'routing', 'manifest', 'determinism'):
                self.assertEqual(g[name], 'PASS', (name, r['gates'][name]))
            self.assertEqual(g['electrical'], 'UNVERIFIED', 'unknown ratings keep the electrical scope UNVERIFIED')
            for f in ('design.kicad_sch', 'design.kicad_pcb', 'design.kicad_pro', 'bom.csv', 'report.json', 'manifest.json'):
                self.assertTrue((Path(d) / 'out/build' / f).is_file(), f)
            cli = copper.discover_cli(); version = r['verification'].get('kicad_version') or ''
            if not version.startswith('9.'):
                self.skipTest(f'UNVERIFIED: KiCad 9.x CLI required for gates 8 and 9 (found {version or "none"})')
            self.assertEqual((g['kicad_erc_drc'], g['connectivity']), ('PASS', 'PASS'), r['gates'])


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
