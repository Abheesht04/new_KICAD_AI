"""One passing and one failing board per DFM rule (audit checklist), and the rule table they generate.

Usage: test_dfm_rules.py COPPER_CORE [unittest args]
       test_dfm_rules.py COPPER_CORE --markdown   (prints docs/engineering/DFM_RULES.md)

The base board is the routed divider (examples/v2/divider.copper built with the committed KiCad 9 library
snapshot). Each row mutates it, or the profile it is checked against, so the rule passes near its limit and
fails just past it, and checks copper-core's findings. A rule that no valid board can reach says why (load-time
rejection, internal self-check, or an UNVERIFIED report) instead of pretending to be tested.
"""
import argparse, contextlib, copy, io, os, re, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
os.environ['COPPER_CORE'] = CORE
sys.path.insert(0, str(ROOT / 'tools'))
import copper
from hwc_kicad import direct, one, parse_sexpr
SNAP = ROOT / 'examples/generated/divider/libraries'
DOC = ROOT / 'docs/engineering/DFM_RULES.md'


# --------------------------------------------------------------------------- board mutations
def rule(key, value):
    def f(t):
        rules = one(t, 'rules')
        if not rules:
            rules = copper.sx('rules'); t.append(rules)
        rules[:] = [x for x in rules if not (isinstance(x, list) and str(x[0]) == key)] + [copper.sx(key, copper.A(str(value)))]
    return f


def add(*items):
    def f(t):
        one(t, 'geometry').extend(parse_sexpr(i) for i in items)
    return f


def drop(predicate):
    def f(t):
        geo = one(t, 'geometry'); kept, gone = [], False
        for x in geo:
            if not gone and isinstance(x, list) and predicate(x):
                gone = True; continue
            kept.append(x)
        geo[:] = kept
    return f


def board(text):
    def f(t):
        one(t, 'board')[:] = parse_sexpr(text)
    return f


def heights(t):
    """Synthetic, explicitly labelled test heights so height-dependent rules have data."""
    for c in direct(one(t, 'components'), 'component'):
        if str(c[1]) != 'V1':
            c.append(parse_sexpr('(override (height_mm 0.55) (provenance "synthetic test height"))'))


def chain(*fns):
    def f(t):
        for fn in fns:
            fn(t)
    return f


def no_geometry(t):
    t[:] = [x for x in t if not (isinstance(x, list) and str(x[0]) == 'geometry')]


def nothing(t):
    pass


VIA_ON_VIN = '(via VIN 12 12 0.6 0.3)'  # on the VIN trace (y = 12), clear of OUT (y = 11) and GND (y = 13)

# rule, family, how it is exercised, passing mutation, failing mutation, rules that may report the failure, note.
# kind: board (pass/fail boards), load (an invalid board is refused before analysis), unverified (the report
# must say UNVERIFIED, never PASS), internal (an engine self-check no valid board can trigger).
RULES = [
    ('trace_width', 'copper', 'board', rule('trace_width_mm', 0.2), rule('trace_width_mm', 0.25), {'trace_width'}, 'routed traces are 0.20 mm'),
    ('copper_clearance', 'copper', 'board', rule('clearance_mm', 0.2), rule('clearance_mm', 1.0), {'copper_clearance', 'trace_clearance'}, 'exact copper kernel, all copper pairs'),
    ('trace_clearance', 'copper', 'board', rule('clearance_mm', 0.2), rule('clearance_mm', 1.0), {'copper_clearance', 'trace_clearance'}, 'trace-to-trace subset of copper clearance'),
    ('short_circuit', 'connectivity', 'board', nothing, add('(trace GND 10 12 12 12 0.2 0)'), {'short_circuit', 'copper_clearance', 'trace_clearance'}, 'a GND trace laid over VIN'),
    ('open_connection', 'connectivity', 'board', nothing, drop(lambda x: str(x[0]) == 'trace' and str(x[1]) == 'VIN' and abs(float(x[2]) - float(x[4])) > 1), {'open_connection'}, 'the long VIN segment removed'),
    ('pad_missing', 'connectivity', 'board', nothing, drop(lambda x: str(x[0]) == 'pad' and str(x[1]) == 'J1' and str(x[2]) == '1'), {'pad_missing'}, 'J1 pad 1 removed from geometry'),
    ('edge_clearance', 'outline', 'board', rule('edge_mm', 0.3), rule('edge_mm', 10), {'edge_clearance'}, 'J1 pads sit 7.15 mm from the left edge'),
    ('via_diameter', 'drills', 'board', add(VIA_ON_VIN), chain(add(VIA_ON_VIN), rule('via_diameter_mm', 0.7)), {'via_diameter'}, '0.6 mm via'),
    ('via_drill', 'drills', 'board', add(VIA_ON_VIN), chain(add(VIA_ON_VIN), rule('drill_mm', 0.35)), {'via_drill'}, '0.3 mm via drill'),
    ('via_annular_ring', 'drills', 'board', add(VIA_ON_VIN), chain(add(VIA_ON_VIN), rule('annular_mm', 0.2)), {'via_annular_ring'}, '0.15 mm annular ring'),
    ('pad_annular_ring', 'drills', 'board', rule('annular_mm', 0.15), rule('annular_mm', 0.4), {'pad_annular_ring'}, 'header pads: 1.7 mm pad, 1.0 mm drill, 0.35 mm ring'),
    ('pad_drill', 'drills', 'board', rule('drill_mm', 0.3), rule('drill_mm', 1.2), {'pad_drill'}, '1.0 mm header drills against the minimum hole'),
    ('hole_clearance', 'drills', 'board', rule('hole_gap_mm', 0.25), rule('hole_gap_mm', 2.0), {'hole_clearance', 'all_hole_clearance'}, 'header holes 1.54 mm apart'),
    ('all_hole_clearance', 'drills', 'board', rule('hole_gap_mm', 0.25), rule('hole_gap_mm', 2.0), {'hole_clearance', 'all_hole_clearance'}, 'pad and via holes together'),
    ('drill_aspect', 'drills', 'board', rule('max_aspect', 8), rule('max_aspect', 1.5), {'drill_aspect'}, '1.6 mm board / 1.0 mm drill = 1.6'),
    ('mask_web', 'solder mask', 'board', rule('mask_web_mm', 0.1), rule('mask_web_mm', 2.0), {'mask_web'}, 'between neighbouring pad openings'),
    ('courtyard_gap', 'assembly', 'board', rule('courtyard_gap_mm', 0.25), rule('courtyard_gap_mm', 10), {'courtyard_gap'}, 'rectangular courtyard envelopes'),
    ('component_height', 'assembly', 'board', chain(heights, rule('max_height_mm', 10)), chain(heights, rule('max_height_mm', 0.5)), {'component_height'}, 'synthetic 0.55 mm heights'),
    ('keepout_violation', 'keepouts', 'board', add('(keepout R1 30 25 35 28 3 15)'), add('(keepout R1 10 11.8 12 12.2 3 15)'), {'keepout_violation'}, 'a rule area over the VIN trace'),
    ('current_density_budget', 'current', 'board', rule('max_current_density_a_mm2', 20), rule('max_current_density_a_mm2', 0.01), {'current_density_budget'}, '0.17 mA over 0.007 mm2 = 0.024 A/mm2; an estimate, not IPC-2152'),
    ('trace_drop_budget', 'current', 'board', rule('max_drop_v', 0.05), rule('max_drop_v', 1e-9), {'trace_drop_budget'}, 'I x rho x L / A per segment'),
    ('board_size', 'outline', 'load', nothing, board('(board 600 30 1.6 0.035 2)'), None, 'boards over 500 mm are refused at load (INVALID_INPUT)'),
    ('layer_count', 'stack-up', 'load', nothing, board('(board 40 30 1.6 0.035 4)'), None, 'only two-layer boards are supported; others are refused at load'),
    ('invalid_geometry', 'geometry', 'load', nothing, add('(trace VIN 12 12 13 12 0 0)'), None, 'a zero-width trace is refused at load'),
    ('height_unknown', 'assembly', 'unverified', heights, nothing, {'height_unknown'}, 'a part without a sourced height keeps assembly checks UNVERIFIED'),
    ('geometry_missing', 'geometry', 'unverified', nothing, no_geometry, {'geometry_missing'}, 'a project without board geometry: physical DFM did not run'),
    ('engine_inconsistency', 'self-check', 'internal', None, None, None, "the DFM engine's audit of its own findings; covered by tests/dfm_test.c and tests/defect_test.c"),
    ('dfm_engine', 'self-check', 'internal', None, None, None, 'the physical DFM engine returned an error; never reachable with a valid board'),
    ('keepout_unverified', 'keepouts', 'internal', None, None, None, 'a copper object could not be compared with a keepout; unsupported shapes are refused earlier'),
]
NOT_IMPLEMENTED = ['zones, copper pours, planes and thermal relief', 'non-rectangular board outlines, cutouts, slots, castellations',
                   'arcs and 45-degree tracks', 'more than two copper layers', 'silkscreen checks (only placement keeps silk apart)',
                   'acute-angle (acid trap) and sliver checks, teardrops, via-in-pad, tenting',
                   'solder-mask slivers and mask-to-trace clearance', 'paste apertures, stencil ratio, fiducials, tooling holes, panel rails',
                   'creepage and voltage-dependent clearance', 'IPC-2152 ampacity and temperature rise (current checks are estimates)',
                   'impedance, differential pairs, length matching, return paths, EMC, RF, thermal',
                   'Gerber/drill structural re-reading']


def errors(report):
    return {i['rule'] for i in report['issues'] if i['severity'] == 'error' and i['rule'] not in copper.ELECTRICAL_RULES}


def warnings(report):
    return {i['rule'] for i in report['issues'] if i['severity'] == 'warning' and i['rule'] not in copper.ELECTRICAL_RULES}


class Base(unittest.TestCase):
    base = None

    @classmethod
    def setUpClass(cls):
        if Base.base is None:
            with tempfile.TemporaryDirectory() as d:
                args = argparse.Namespace(project=str(ROOT / 'examples/v2/divider.copper'), out=str(Path(d) / 'b'), core=CORE,
                                          symbol_dir=str(SNAP / 'symbols'), footprint_dir=str(SNAP / 'footprints'), no_route=False, draft=False)
                with contextlib.redirect_stdout(io.StringIO()):
                    copper.export_project(args)
                Base.base = copper.load(Path(d) / 'b/resolved.copper')

    def check(self, mutate):
        t = copy.deepcopy(Base.base); mutate(t)
        return copper.core_check(t, CORE)


class Rules(Base):
    def test_base_board_is_clean(self):
        r = self.check(nothing)
        self.assertEqual(errors(r), set(), r['issues']); self.assertTrue(r['physical_checked'])

    def test_every_rule_passes_and_fails(self):
        for name, family, kind, passing, failing, reports, note in RULES:
            with self.subTest(name):
                if kind == 'internal':
                    continue
                if kind == 'load':
                    self.assertEqual(errors(self.check(passing)), set())
                    with self.assertRaises(copper.InvalidInput):
                        self.check(failing)
                    continue
                good, bad = self.check(passing), self.check(failing)
                if kind == 'unverified':
                    self.assertFalse(reports & warnings(good), f'{name}: reported although its data is present')
                    self.assertTrue(reports & warnings(bad), f'{name}: not reported as UNVERIFIED')
                    self.assertEqual(copper.report_scopes(bad)['physical'], 'UNVERIFIED')
                    continue
                self.assertEqual(errors(good), set(), f'{name} passing board: {errors(good)}')
                self.assertTrue(reports & errors(bad), f'{name} failing board reported {errors(bad)}, expected one of {reports}')
                self.assertEqual(copper.report_scopes(bad)['physical'], 'FAIL')

    def test_every_rule_the_engine_can_report_is_in_the_table(self):
        source = (ROOT / 'src/dfm_pcb.c').read_text(encoding='utf-8') + (ROOT / 'src/v2/project.c').read_text(encoding='utf-8')
        emitted = set(re.findall(r'case PCB_DFM_(?!OBJECT_|INFO|WARNING|ERROR)[A-Z_]+: return "([a-z_]+)"', source))
        emitted |= {m for m in re.findall(r'issue\(&?r,[12],"([a-z_]+)"', source) if m not in copper.ELECTRICAL_RULES}
        self.assertLessEqual(emitted, {r[0] for r in RULES}, 'a DFM rule without a passing/failing test row')

    def test_rule_table_document_is_current(self):
        self.assertEqual(DOC.read_text(encoding='utf-8'), markdown(), 'regenerate: test_dfm_rules.py CORE --markdown > docs/engineering/DFM_RULES.md')


def markdown():
    status = {'board': 'tested: passing and failing board', 'load': 'enforced at load (INVALID_INPUT)',
              'unverified': 'reports UNVERIFIED when data is missing', 'internal': 'internal self-check (no board can trigger it)'}
    lines = ['# DFM rules', '',
             'Generated from `tests/test_dfm_rules.py`, where every row below is executed against copper-core on the routed',
             'divider board (`examples/v2/divider.copper`, committed KiCad 9 library snapshot). A board row has one passing',
             'and one failing board; the test also fails if the engine can report a rule that has no row. Native KiCad DRC is',
             'a separate, independent gate (`project verify`) and is not counted here.', '',
             '| Rule | Family | Status | Exercised by |', '|---|---|---|---|']
    lines += [f'| `{n}` | {f} | {status[k]} | {note} |' for n, f, k, _, _, _, note in RULES]
    lines += ['', '## Not implemented (never reported as PASS)', ''] + [f'- {x}' for x in NOT_IMPLEMENTED]
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    if '--markdown' in sys.argv:
        sys.stdout.reconfigure(encoding='utf-8'); sys.stdout.write(markdown()); sys.exit(0)
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
