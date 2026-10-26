"""KiCad design rules <-> Copper rule profiles (docs/engineering/KICAD_RULES.md).

Usage: test_kicad_rules.py PCBA [unittest args]   (copper-core is taken from the same folder)
Exit status: 0 all ran and passed, 1 failure.
"""
import copy, json, os, subprocess, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
PCBA = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None
if PCBA:
    os.environ['COPPER_CORE'] = str(Path(PCBA).with_name('copper-core' + Path(PCBA).suffix))
sys.path.insert(0, str(ROOT / 'tools'))
import copper, kicad_rules as kr
from hwc_kicad import Atom as A, Quoted as Q, one, parse_sexpr
SNAP = ROOT / 'examples/generated/divider/libraries'
DATE = '2026-10-26'

PRO = {'meta': {'filename': 'board.kicad_pro', 'version': 3},
       'board': {'design_settings': {'rules': {
           'min_clearance': 0.0, 'min_track_width': 0.15, 'min_copper_edge_clearance': 0.5, 'min_via_diameter': 0.5,
           'min_through_hole_diameter': 0.3, 'min_via_annular_width': 0.1, 'min_hole_to_hole': 0.25, 'min_hole_clearance': 0.25,
           'min_text_height': 0.8, 'min_connection': 0.0, 'max_error': 0.005}}},
       'net_settings': {'classes': [{'name': 'Default', 'clearance': 0.2, 'track_width': 0.25, 'via_diameter': 0.6, 'via_drill': 0.3},
                                    {'name': 'HV', 'clearance': 0.5, 'track_width': 0.5}]}}
DRU = '''(version 1)
# a comment line with (rule "fake" (constraint clearance (min 9mm)))
(rule "global_clearance" (constraint clearance (min 0.3mm)))
(rule "wide" (constraint track_width (min 8mil) (max 2mm)))
(rule "edge" (constraint edge_clearance (min 0.0254in)))
(rule "holes" (constraint hole_to_hole (min 300um)) (constraint length (max 50mm)))
(rule "bga" (constraint clearance (min 0.08mm)) (condition "A.insideCourtyard('U1')"))
(rule "inner" (layer inner) (constraint track_width (min 0.09mm)))
(rule "off" (severity ignore) (constraint via_diameter (min 2mm)))
(rule "court" (constraint courtyard_clearance (min 0.2)))
(rule "opt_only" (constraint track_width (opt 0.3mm)))
'''
PCB = '(kicad_pcb (version 20241229) (generator "pcbnew") (setup (pad_to_mask_clearance 0.04) (solder_mask_min_width 0.12)))'


def write(path, text):
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text, encoding='utf-8')
    return path


def project(folder, pro=PRO, dru=DRU, pcb=PCB, name='board'):
    write(Path(folder) / f'{name}.kicad_pro', json.dumps(pro))
    if dru is not None:
        write(Path(folder) / f'{name}.kicad_dru', dru)
    if pcb is not None:
        write(Path(folder) / f'{name}.kicad_pcb', pcb)
    return Path(folder) / f'{name}.kicad_pro'


def fields(rules):
    return {str(n[0]): float(n[1]) for n in rules[1:] if isinstance(n, list) and str(n[0]) != 'provenance'}


def provenance(rules):
    return {str(n[0]): (str(n[1]), str(n[2])) for n in one(rules, 'provenance')[1:]}


class Import(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_strictest_global_limit_with_its_source(self):
        rules, report = kr.import_project(project(self.dir), date=DATE)
        self.assertEqual(fields(rules), {'trace_width_mm': 0.2032, 'clearance_mm': 0.5, 'edge_mm': 0.64516, 'via_diameter_mm': 0.5,
                                         'drill_mm': 0.3, 'annular_mm': 0.1, 'hole_gap_mm': 0.3, 'courtyard_gap_mm': 0.2,
                                         'mask_expansion_mm': 0.04, 'mask_web_mm': 0.12})
        src = provenance(rules)
        self.assertEqual(src['clearance_mm'], ('board.kicad_pro: net_settings.classes[HV].clearance', DATE))
        self.assertEqual(src['trace_width_mm'][0], 'board.kicad_dru: rule "wide" track_width')
        self.assertEqual(src['edge_mm'][0], 'board.kicad_dru: rule "edge" edge_clearance')
        self.assertEqual(src['mask_web_mm'][0], 'board.kicad_pcb: setup.solder_mask_min_width')
        self.assertEqual(set(src), set(fields(rules)), 'every limit has a source and date')
        considered = [c['value'] for c in report['limits']['clearance_mm']['considered']]
        self.assertEqual(considered, [0.2, 0.5, 0.3])           # Default class, HV class, unconditional custom rule
        self.assertEqual(set(report['files']), {'board.kicad_pro', 'board.kicad_dru', 'board.kicad_pcb'})

    def test_what_one_profile_cannot_express_is_reported(self):
        _, report = kr.import_project(project(self.dir), date=DATE)
        reasons = {n['source']: n for n in report['not_imported']}
        self.assertIn('KiCad checks no minimum', reasons['board.kicad_pro: board.design_settings.rules.min_clearance']['reason'])
        self.assertTrue(reasons['board.kicad_pro: board.design_settings.rules.min_hole_clearance']['enforced_by_kicad'])
        self.assertIn('has a condition', reasons['board.kicad_dru: rule "bga" clearance']['reason'])
        self.assertIn('layer inner', reasons['board.kicad_dru: rule "inner" track_width']['reason'])
        self.assertFalse(reasons['board.kicad_dru: rule "off" via_diameter']['enforced_by_kicad'])
        self.assertIn('no Copper limit', reasons['board.kicad_dru: rule "holes" length']['reason'])
        self.assertTrue(reasons['board.kicad_dru: rule "wide" track_width max']['enforced_by_kicad'])
        self.assertIn('has no min', reasons['board.kicad_dru: rule "opt_only" track_width']['reason'])
        self.assertNotIn('fake', json.dumps(report), 'comment lines are not rules')
        self.assertEqual(report['status'], 'UNVERIFIED')
        self.assertEqual([c['name'] for c in report['net_classes']], ['Default', 'HV'])

    def test_base_profile_fills_limits_kicad_does_not_have(self):
        pro = json.loads(json.dumps(PRO)); pro['net_settings']['classes'] = []
        rules, report = kr.import_project(project(self.dir, pro=pro, dru=None, pcb=None), base=ROOT / 'profiles/conservative.rules', date=DATE)
        got, src = fields(rules), provenance(rules)
        self.assertEqual(got['clearance_mm'], 0.25); self.assertEqual(src['clearance_mm'][0], 'base profile conservative.rules')
        self.assertEqual(got['trace_width_mm'], 0.15)          # KiCad's board minimum wins over the base where KiCad sets one
        self.assertEqual((got['max_drop_v'], got['power_derating']), (0.05, 0.5))
        self.assertEqual(src['max_height_mm'][0], 'base profile conservative.rules')
        rules, report = kr.import_project(project(self.dir, pro=pro, dru=None, pcb=None), date=DATE)
        self.assertNotIn('clearance_mm', fields(rules)); self.assertIn('copper-core default', report['limits']['clearance_mm']['source'])

    def test_invalid_projects_are_rejected(self):
        bad = json.loads(json.dumps(PRO)); bad['board']['design_settings']['rules']['min_track_width'] = 0.01
        cases = [(dict(pro=bad, dru=None), 'outside the range'), (dict(dru='(version 1) (rule "x" (constraint clearance (min 2furlong)))'), 'cannot read the length'),
                 (dict(dru='(rule "x" (constraint clearance (min 1mm)))'), 'version 1'), (dict(dru='(version 1) (rule "x"'), 'kicad_dru')]
        for i, (kw, needle) in enumerate(cases):
            with self.subTest(needle), self.assertRaises(ValueError) as cm:
                kr.import_project(project(self.dir / str(i), **kw), date=DATE)
            self.assertIn(needle, str(cm.exception))
        write(self.dir / 'json/board.kicad_pro', '{not json')
        with self.assertRaises(ValueError):
            kr.import_project(self.dir / 'json')
        project(self.dir / 'two', name='a'); project(self.dir / 'two', name='b')
        with self.assertRaises(ValueError):
            kr.import_project(self.dir / 'two')
        with self.assertRaises(ValueError):
            kr.import_project(self.dir / 'missing.kicad_pro')


class RoundTrip(unittest.TestCase):
    """A build's profile, exported into KiCad's files, imports back to the same limits."""
    @unittest.skipUnless(PCBA, 'needs copper-core')
    def test_build_exports_its_profile_to_kicad_and_back(self):
        with tempfile.TemporaryDirectory() as d:
            src = copper.load(ROOT / 'examples/v2/divider.copper')
            src = [x for x in src if not (isinstance(x, list) and str(x[0]) == 'rules')] + [parse_sexpr((ROOT / 'profiles/esp32_2layer.rules').read_text(encoding='utf-8'))]
            copper.write_tree(Path(d) / 'p.copper', src)
            r = subprocess.run([sys.executable, str(ROOT / 'tools/copper.py'), 'build', str(Path(d) / 'p.copper'), '--out', str(Path(d) / 'b'),
                                '--symbol-dir', str(SNAP / 'symbols'), '--footprint-dir', str(SNAP / 'footprints')], capture_output=True, text=True)
            self.assertIn(r.returncode, (0, 2), r.stderr)
            pro = json.loads((Path(d) / 'b/design.kicad_pro').read_text())
            self.assertEqual(pro['board']['design_settings']['rules']['min_hole_to_hole'], 0.25)
            dru = (Path(d) / 'b/design.kicad_dru').read_text()
            self.assertIn('(constraint hole_to_hole (min 0.25mm))', dru)
            self.assertNotIn('hole_clearance', dru, 'Copper hole_gap is drill-to-drill (KiCad hole_to_hole), not hole-to-copper')
            rules, report = kr.import_project(Path(d) / 'b', date=DATE)
            expected = fields(parse_sexpr((ROOT / 'profiles/esp32_2layer.rules').read_text(encoding='utf-8')))
            self.assertEqual(fields(rules), {f: expected[f] for f, *_ in kr.LIMITS})
            self.assertEqual(report['status'], 'PASS', report['not_imported'])

    @unittest.skipUnless(PCBA, 'needs copper-core')
    def test_copper_core_checks_provenance(self):
        rules, _ = kr.import_project(project(tempfile.mkdtemp(), dru=None), date=DATE)
        base = [x for x in copper.load(ROOT / 'examples/v2/divider.copper') if not (isinstance(x, list) and str(x[0]) == 'rules')]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'p.copper'
            copper.write_tree(path, base + [rules]); copper.core_run(path)       # accepted
            for edit, needle in ((lambda p: p[1].__setitem__(2, A('26/10/2026')), 'YYYY-MM-DD'),
                                 (lambda p: p.append([A('bogus_mm'), Q('x'), Q(DATE)]), "Unknown field 'bogus_mm'"),
                                 (lambda p: p.append([A('max_drop_v'), Q('x'), Q(DATE)]), 'sets no max_drop_v'),
                                 (lambda p: p[1].pop(), 'expected')):
                broken = copy.deepcopy(rules); edit(one(broken, 'provenance'))
                copper.write_tree(path, base + [broken])
                with self.subTest(needle), self.assertRaises(copper.InvalidInput) as cm:
                    copper.core_run(path)
                self.assertIn(needle, str(cm.exception))


class Cli(unittest.TestCase):
    @unittest.skipUnless(PCBA, 'needs the pcba executable')
    def test_rules_import(self):
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d) / 'kicad'; project(folder)
            out = Path(d) / 'imported.rules'

            def run(*args):
                r = subprocess.run([PCBA, 'rules', 'import', *map(str, args), '--json'], capture_output=True, timeout=300)
                return r.returncode, json.loads(r.stdout.decode('utf-8'))
            rc, doc = run(folder, '--out', out, '--date', DATE, '--report', Path(d) / 'report.json')
            self.assertEqual((doc['status'], rc), ('UNVERIFIED', 3), doc['message'])
            text = out.read_text(encoding='utf-8'); self.assertTrue(text.startswith('; Imported from board.kicad_pro on 2026-10-26'))
            self.assertEqual(fields(parse_sexpr(text))['clearance_mm'], 0.5)
            self.assertTrue(json.loads((Path(d) / 'report.json').read_text())['not_imported'])
            rc, doc = run(folder, '--out', out, '--date', DATE)
            self.assertEqual((doc['status'], rc), ('INVALID_INPUT', 2))           # no --force
            pro = json.loads(json.dumps(PRO)); pro['board']['design_settings']['rules'] = {'min_track_width': 0.2}
            clean = Path(d) / 'clean'; project(clean, pro=pro, dru='(version 1)\n', pcb=None)
            rc, doc = run(clean, '--out', out, '--force')
            self.assertEqual((doc['status'], rc), ('PASS', 0), doc['message'])
            rc, doc = run(Path(d) / 'nowhere', '--out', Path(d) / 'x.rules')
            self.assertEqual((doc['status'], rc), ('INVALID_INPUT', 2))
            rc, doc = run(clean, '--out', Path(d) / 'y.rules', '--date', '2026-13')
            self.assertEqual((doc['status'], rc), ('USAGE', 64))


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
