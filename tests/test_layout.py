"""Placement and routing optimizer (Prompt 8): instrumentation, determinism, hard-before-soft selection,
exact re-check, and honest infeasibility on the layout fixtures.

Usage: test_layout.py COPPER_CORE [unittest args]
Exit status: 0 all ran and passed, 1 failure, 77 something was skipped (a fixture's footprints missing).
Uses the committed KiCad 9 library snapshot (R_0603, PinHeader_1x03); the RC fixture also needs
Capacitor_SMD and runs only with KICAD_FOOTPRINT_DIR.
"""
import copy, json, os, sys, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
os.environ['COPPER_CORE'] = CORE
sys.path.insert(0, str(ROOT / 'tools'))
import copper, layout
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
            self.assertEqual(set(n), {'net', 'connections', 'routed', 'explored', 'length_mm', 'bends', 'vias', 'failures', 'time_s'})
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
        self.assertGreaterEqual(float(copper.text_at(copper.one(placed, 'rules'), 'trace_width_mm')), 0.7)
        self.assertIsNone(copper.one(placed, 'geometry'))
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
