"""Placement and routing optimizer (Prompt 8): instrumentation, determinism, hard-before-soft selection,
exact re-check, and honest infeasibility on the layout fixtures.

Usage: test_layout.py COPPER_CORE [unittest args]
Exit status: 0 all ran and passed, 1 failure, 77 something was skipped (a fixture's footprints missing).
Uses the committed KiCad 9 library snapshot (R_0603, PinHeader_1x03); the RC fixture also needs
Capacitor_SMD and runs only with KICAD_FOOTPRINT_DIR.
"""
import copy, json, os, subprocess, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
os.environ['COPPER_CORE'] = CORE
sys.path.insert(0, str(ROOT / 'tools'))
import copper, layout
from hwc_kicad import parse_sexpr as parse
FP = ROOT / 'examples/generated/divider/libraries/footprints'
FIX = ROOT / 'tests/fixtures/layout'
SEEDS, ITER = (1, 2), 200


def stable(value):
    """Strip wall-clock fields: everything else must be identical between runs."""
    if isinstance(value, dict):
        return {k: stable(v) for k, v in value.items() if k not in ('time_s', 'routing_time_s', 'resolved')}
    if isinstance(value, (list, tuple)):
        return [stable(v) for v in value]
    return value


class Router(unittest.TestCase):
    def setUp(self):
        tree = copper.load(ROOT / 'examples/v2/divider.copper'); self.problem = layout.problem_from(tree, FP)
        shapes = {r: self.problem['footprints'].placed(r, a) for r, a in self.problem['given'].items()}
        self.pads = [p for s in shapes.values() for p in s['pads']]
    def route(self, **kw):
        p = self.problem
        return layout.route(self.pads, p['width'], p['height'], p['trace_width'], p['clearance'], p['edge'], p['via'], **kw)
    def test_instrumentation(self):
        traces, vias, stats = self.route()
        self.assertEqual(stats['unrouted'], []); self.assertEqual(stats['routed_connections'], stats['connections'])
        self.assertEqual(stats['objects'], {'traces': len(traces), 'vias': len(vias)})
        self.assertAlmostEqual(stats['length_mm'], sum(n['length_mm'] for n in stats['nets']), places=9)
        self.assertEqual(stats['explored'], sum(n['explored'] for n in stats['nets']))
        self.assertGreater(stats['explored'], 0); self.assertIn('hotspots', stats['congestion'])
        for n in stats['nets']:
            self.assertEqual(set(n), {'net', 'connections', 'routed', 'explored', 'length_mm', 'bends', 'vias', 'failures', 'width_mm', 'time_s'})
    def test_deterministic(self):
        self.assertEqual(stable(self.route()), stable(self.route()))
    def test_failures_are_reported_not_raised(self):
        traces, vias, stats = self.route(max_states=5)
        self.assertTrue(stats['unrouted']); self.assertEqual({u['reason'] for u in stats['unrouted']}, {'state_limit'})
        self.assertLess(stats['routed_connections'], stats['connections'])
    def test_build_wrapper_never_claims_a_partial_route(self):
        p = self.problem; keep = [dict(x1=0, y1=0, x2=p['width'], y2=p['height'], layers=3, flags=3)]
        with self.assertRaises(copper.Failed):
            copper.route(self.pads, p['width'], p['height'], p['trace_width'], p['clearance'], p['edge'], p['via'], keep)
    def test_ripup_reports_its_passes(self):
        p = self.problem
        _, _, stats = layout.route_with_ripup(self.pads, p['width'], p['height'], p['trace_width'], p['clearance'], p['edge'], p['via'])
        self.assertEqual(len(stats['ripup_passes']), 1); self.assertEqual(stats['ripup_passes'][0]['unrouted'], 0)


class Dfm(unittest.TestCase):
    """The DFM-aware router: 45-degree routing without acute corners, per-net widths from DC current bounds,
    neck-downs, clearance margin, and builds that reproduce the candidate copper-core proved."""
    def placement(self, name):
        tree = copper.load(FIX / f'{name}.copper'); problem = layout.problem_from(tree, FP)
        return tree, problem, layout.initial_placement(problem)[0]

    def test_octilinear_is_shorter_with_45_degree_steps_and_no_acute_corner(self):
        tree, problem, pl = self.placement('multi_connector')
        man = layout.evaluate(tree, problem, pl, FP, CORE, 0, 'm', style='manhattan')
        octo = layout.evaluate(tree, problem, pl, FP, CORE, 1, 'o', style='octilinear')
        self.assertTrue(man['feasible'] and octo['feasible'], (man['hard_by_rule'], octo['hard_by_rule']))
        self.assertLess(octo['routing']['length_mm'], man['routing']['length_mm'])
        self.assertNotIn('error:acute_angle', octo['findings_by_rule'])
        segs = [tuple(map(float, t[2:6])) for t in copper.direct(copper.one(octo['resolved'], 'geometry'), 'trace')]
        grid = lambda v: abs(v / layout.GRID - round(v / layout.GRID)) < 1e-9
        routed = [s for s in segs if all(map(grid, s))]           # pad stubs end at pad centres, off the grid
        self.assertTrue(any(abs(s[2] - s[0]) == abs(s[3] - s[1]) > 0 for s in routed), 'some 45-degree segments')
        for x1, y1, x2, y2 in routed:
            dx, dy = abs(x2 - x1), abs(y2 - y1)
            self.assertTrue(dx == 0 or dy == 0 or abs(dx - dy) < 1e-9, (x1, y1, x2, y2))
        self.assertEqual(copper.text_at(copper.one(octo['resolved'], 'routing'), 'style'), 'octilinear')

    def test_octilinear_proves_no_path_exactly_where_manhattan_does(self):
        tree = copper.load(FIX / 'unroutable.copper'); problem = layout.problem_from(tree, FP); pl = dict(problem['given'])
        a, b = (layout.evaluate(tree, problem, pl, FP, CORE, 0, s, style=s)['routing']['unrouted'] for s in layout.STYLES)
        self.assertEqual([(u['net'], u['connection']) for u in a], [(u['net'], u['connection']) for u in b])
        self.assertEqual({u['reason'] for u in b}, {'no_path'})

    def test_per_net_width_from_the_current_bound(self):
        tree, problem, pl = self.placement('mixed_current')
        report, _ = copper.core_run(FIX / 'mixed_current.copper', CORE)
        bounds = {n['name']: n['bound_a'] for n in report['net_current_bounds']}
        self.assertAlmostEqual(bounds['VIN'], 5 / 10 + 5 / 200000, places=12); self.assertAlmostEqual(bounds['SIG'], 2.5e-5, places=12)
        self.assertAlmostEqual(bounds['GND'], bounds['VIN'], places=12)
        best = layout.evaluate(tree, problem, pl, FP, CORE, 0, 'x', style='manhattan')
        self.assertTrue(best['feasible'], best['hard_by_rule'])
        self.assertEqual(set(best['widths']), {'VIN', 'GND'}); self.assertTrue(all(w >= 0.5 / (20 * 0.035) for w in best['widths'].values()))
        geo = copper.one(best['resolved'], 'geometry')
        self.assertEqual({float(t[6]) for t in copper.direct(geo, 'trace') if str(t[1]) == 'SIG'}, {0.2}, 'the 25 uA net stays at the rule width')
        calc = copper.core_check(best['resolved'], CORE)['trace_calculations']
        self.assertEqual({c['current_basis'] for c in calc}, {'net'})

    def test_neck_down_never_below_the_current_floor(self):
        pads = [dict(ref='A', pin='1', net='P', x=5, y=5, w=.3, h=.3, through=False),
                dict(ref='B', pin='1', net='P', x=15, y=5, w=.3, h=.3, through=False)]
        traces, _, stats = layout.route(pads, 20, 10, .2, .2, .3, widths={'P': .6}, min_widths={'P': .4})
        self.assertEqual(stats['unrouted'], [])
        widths = sorted({t[5] for t in traces}); self.assertEqual(widths, [.4, .6])
        zone = .15 + layout.NECK_ZONE + 1e-9    # a necked segment touches a pad's neck-down zone
        for t in traces:
            if t[5] == .4:
                self.assertTrue(any(abs(x - p['x']) <= zone and abs(y - p['y']) <= zone for x, y in ((t[1], t[2]), (t[3], t[4])) for p in pads), t)

    def test_margin_metric(self):
        resolved = copper.sx('copper_project', copper.sx('geometry',
            *[parse('(trace A 0 0 10 0 0.2 0)'), parse('(trace B 0 0.7 10 0.7 0.2 0)'), parse('(trace C 0 0 0 5 0.2 2)')]))
        worst, near = layout.clearance_margins(resolved, 0.2)
        self.assertAlmostEqual(worst, 0.7 - 0.2 - 0.2, places=9); self.assertEqual(near, 0)   # C is on the other layer
        self.assertEqual(layout.clearance_margins(resolved, 0.48), (round(0.5 - 0.48, 6), 1))

    def test_kicad_is_the_second_judge(self):
        cands = [dict(label=l, feasible=f, judged=True, hard_violations=0, unrouted=0, soft_score=s, index=i)
                 for i, (l, f, s) in enumerate([('a', True, 3.0), ('b', True, 1.0), ('c', False, 0.0), ('d', True, 2.0)])]
        script = {'b': 'FAIL', 'd': 'UNVERIFIED', 'a': 'PASS'}; asked = []
        judge = lambda c: (asked.append(c['label']), dict(status=script[c['label']], message=c['label']))[1]
        best, verdicts = layout.second_judge(cands, judge)
        self.assertEqual(asked, ['b', 'd'], 'copper-core ranking order; infeasible candidates are never judged')
        self.assertEqual(best['label'], 'd'); self.assertEqual([v['status'] for v in verdicts], ['FAIL', 'UNVERIFIED'])
        best, verdicts = layout.second_judge(cands, lambda c: dict(status='FAIL'), limit=2)
        self.assertEqual((best['label'], len(verdicts), best['kicad_drc']['status']), ('b', 2, 'FAIL'))

    def test_a_build_reproduces_the_chosen_candidate(self):
        tree = copper.load(FIX / 'mixed_current.copper')
        best, _, problem = layout.optimize(tree, FP, CORE, (1,), 50)
        self.assertTrue(best['feasible'])
        placed = layout.placed_project(best, problem)
        with tempfile.TemporaryDirectory() as d:
            copper.write_tree(Path(d) / 'p.copper', placed)
            r = subprocess.run([sys.executable, str(ROOT / 'tools/copper.py'), 'build', str(Path(d) / 'p.copper'), '--out', str(Path(d) / 'b'),
                                '--core', CORE, '--symbol-dir', str(ROOT / 'examples/generated/divider/libraries/symbols'), '--footprint-dir', str(FP)],
                               capture_output=True, text=True)
            self.assertIn(r.returncode, (0, 2), r.stderr)
            built = [x for x in copper.one(copper.load(Path(d) / 'b/resolved.copper'), 'geometry') if str(x[0]) in ('trace', 'via')]
            proven = [x for x in copper.one(best['resolved'], 'geometry') if str(x[0]) in ('trace', 'via')]
            self.assertEqual(copper.dump_sexpr(copper.sx('g', *built)), copper.dump_sexpr(copper.sx('g', *proven)))
            dru = (Path(d) / 'b/design.kicad_dru').read_text()
            for net, w in best['widths'].items():
                self.assertIn(f"(condition \"A.NetName == '/{net}'\") (constraint track_width (min {w:g}mm))", dru)


class Parallel(unittest.TestCase):
    """Annealing seeds and candidates run on worker processes; the result never depends on how many or which finishes
    first (results are collected in submission order and ties are broken by candidate index)."""
    def test_identical_for_any_number_of_processes(self):
        for name in ('multi_connector', 'mixed_current', 'unroutable'):
            tree = copper.load(FIX / f'{name}.copper')
            runs = [layout.optimize(tree, FP, CORE, SEEDS, ITER, jobs=j) for j in (1, 3)]
            self.assertEqual(stable([layout.brief(c) for c in runs[0][1]]), stable([layout.brief(c) for c in runs[1][1]]), name)
            self.assertEqual(copper.dump_sexpr(runs[0][0]['resolved']), copper.dump_sexpr(runs[1][0]['resolved']), name)

    def test_jobs_setting(self):
        old = os.environ.get('COPPER_JOBS')
        try:
            os.environ['COPPER_JOBS'] = '1'; self.assertEqual(layout.default_jobs(), 1)
            os.environ.pop('COPPER_JOBS'); self.assertEqual(layout.default_jobs(), os.cpu_count() or 1)
        finally:
            if old is not None:
                os.environ['COPPER_JOBS'] = old


class Legality(unittest.TestCase):
    def test_fast_legality_equals_the_full_violation_list(self):
        # Placement uses Board.legal (early exit, envelope pre-filter); it must agree with violations() exactly.
        import random
        for name in ('multi_connector', 'edge_keepout', 'tight_clearance'):
            problem = layout.problem_from(copper.load(FIX / f'{name}.copper'), FP); fp = problem['footprints']
            board = layout.Board(problem['width'], problem['height'], problem['edge'], problem['gap'], problem['keepouts'])
            rng = random.Random(5); refs = sorted(c['ref'] for c in problem['components'])
            for _ in range(400):
                shapes = {r: fp.placed(r, (rng.uniform(-2, problem['width'] + 2), rng.uniform(-2, problem['height'] + 2), rng.choice((0, 90, 180, 270)))) for r in refs}
                for r in refs:
                    self.assertEqual(board.legal(r, shapes[r], shapes), not board.violations(r, shapes[r], shapes), name)


class Selection(unittest.TestCase):
    def test_hard_violations_are_never_traded_for_soft_gains(self):
        cands = [dict(index=0, judged=True, hard_violations=1, unrouted=0, soft_score=0.0),
                 dict(index=1, judged=True, hard_violations=0, unrouted=1, soft_score=1.0),
                 dict(index=2, judged=True, hard_violations=0, unrouted=0, soft_score=1e9),
                 dict(index=3, judged=False, hard_violations=0, unrouted=0, soft_score=0.0)]
        self.assertEqual(min(cands, key=layout.candidate_key)['index'], 2)
    def test_exact_check_is_the_only_judge(self):
        # Two resistors forced onto the same spot: the placer would never do it, so copper-core must catch it.
        tree = copper.load(ROOT / 'examples/v2/divider.copper'); problem = layout.problem_from(tree, FP)
        placement = dict(problem['given']); placement['R2'] = placement['R1']
        c = layout.evaluate(tree, problem, placement, FP, CORE, 0, 'overlapping')
        self.assertFalse(c['feasible']); self.assertGreater(c['hard_violations'], 0)
        self.assertTrue({'courtyard_gap', 'short_circuit', 'copper_clearance'} & set(c['hard_by_rule']), c['hard_by_rule'])


class Fixtures(unittest.TestCase):
    """Expected outcome per fixture; every number comes from copper-core."""
    cache = {}
    def run_fixture(self, name, path=None):
        if name not in self.cache:
            tree = copper.load(path or FIX / f'{name}.copper')
            base = layout.baseline(tree, FP, CORE); best, cands, problem = layout.optimize(tree, FP, CORE, SEEDS, ITER)
            self.cache[name] = (base, best, cands, problem)
        return self.cache[name]
    def assert_selection(self, best, cands):
        self.assertEqual(best, min(cands, key=layout.candidate_key))
        if any(c['feasible'] for c in cands):
            self.assertTrue(best['feasible'], 'a feasible candidate exists, so the chosen one must be feasible')
    def test_divider(self):
        base, best, cands, _ = self.run_fixture('divider', ROOT / 'examples/v2/divider.copper')
        self.assertTrue(base['feasible']); self.assertTrue(best['feasible']); self.assert_selection(best, cands)
        self.assertLessEqual(best['soft_score'], base['soft_score'])
    def test_multi_connector(self):
        base, best, cands, _ = self.run_fixture('multi_connector')
        self.assertTrue(best['feasible']); self.assert_selection(best, cands)
        self.assertLess(best['routing']['length_mm'], base['routing']['length_mm'])
    def test_tight_clearance_is_reported_infeasible(self):
        base, best, cands, _ = self.run_fixture('tight_clearance')
        self.assertFalse(best['feasible']); self.assertFalse(any(c['feasible'] for c in cands))
        self.assertIn('copper_clearance', best['hard_by_rule'])
    def test_edge_keepout(self):
        base, best, cands, problem = self.run_fixture('edge_keepout')
        self.assertFalse(base['feasible']); self.assertIn('keepout_violation', base['hard_by_rule'])
        self.assertTrue(best['feasible']); self.assert_selection(best, cands)
        for ref, (x, y, rot) in best['placement'].items():
            body = problem['footprints'].placed(ref, (x, y, rot))['body']; self.assertGreaterEqual(body[0], 10, ref)
    def test_high_current_widens_copper(self):
        base, best, cands, _ = self.run_fixture('high_current')
        self.assertIn('current_density_budget', base['hard_by_rule']); self.assertTrue(best['feasible'])
        widths = [r['trace_width_mm'] for r in best['width_rounds']]
        self.assertEqual(widths[0], 0.2); self.assertGreaterEqual(widths[-1], 0.5 / (20 * 0.035))
        self.assertNotIn('error:current_density_budget', best['findings_by_rule'])
    def test_unroutable_is_reported_and_fixed_parts_stay(self):
        base, best, cands, problem = self.run_fixture('unroutable')
        self.assertFalse(best['feasible']); self.assertGreater(best['unrouted'], 0)
        self.assertTrue(all(u['reason'] in ('no_path', 'pad_escape', 'state_limit') for u in best['routing']['unrouted']))
        self.assertEqual(problem['fixed'], {'J1', 'J2', 'R1', 'R2'})
        for c in cands:
            self.assertEqual({r: tuple(a) for r, a in c['placement'].items()}, {r: tuple(a) for r, a in problem['given'].items()})
    def test_placed_project_records_widened_copper_even_without_rules(self):
        tree = copper.load(FIX / 'high_current.copper'); base, best, cands, problem = self.run_fixture('high_current')
        self.assertIsNone(copper.one(tree, 'rules'), 'fixture has no (rules ...) block')
        placed = layout.placed_project(best, problem)
        widths = {str(w[1]): float(w[2]) for w in copper.direct(copper.one(placed, 'routing'), 'width')}
        self.assertTrue(widths, 'the widened nets are recorded per net in (routing (width NET MM))')
        self.assertTrue(all(w >= 0.7 for w in widths.values()), widths)
        self.assertEqual(widths, best['widths']); self.assertIsNone(copper.one(placed, 'geometry'))
    def test_optimizer_is_deterministic(self):
        tree = copper.load(FIX / 'multi_connector.copper')
        a = layout.optimize(tree, FP, CORE, SEEDS, ITER); b = layout.optimize(tree, FP, CORE, SEEDS, ITER)
        self.assertEqual(stable([layout.brief(c) for c in a[1]]), stable([layout.brief(c) for c in b[1]]))
        self.assertEqual(copper.dump_sexpr(a[0]['resolved']), copper.dump_sexpr(b[0]['resolved']))
    def test_reports_carry_every_term(self):
        _, best, _, _ = self.run_fixture('divider', ROOT / 'examples/v2/divider.copper'); r = layout.brief(best)
        self.assertEqual(set(r['soft']), set(layout.SOFT_TERMS) - {'blocked_expansions'} | {'blocked_expansions'})
        for key in ('hard_by_rule', 'unrouted_detail', 'per_net', 'congestion_hotspots', 'width_rounds', 'findings', 'explored'):
            self.assertIn(key, r)
        json.dumps(r)



class Libraries(unittest.TestCase):
    """Fixtures whose footprints are not in the committed snapshot (installed KiCad 9.x libraries)."""
    assert_selection = Fixtures.assert_selection
    def test_rc_network(self):
        fp = os.environ.get('KICAD_FOOTPRINT_DIR')
        if not fp:
            self.skipTest('UNVERIFIED: the RC fixture needs Capacitor_SMD; set KICAD_FOOTPRINT_DIR')
        tree = copper.load(ROOT / 'examples/v2/rc.copper'); best, cands, _ = layout.optimize(tree, Path(fp), CORE, SEEDS, ITER)
        self.assertTrue(best['feasible']); self.assert_selection(best, cands)


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
