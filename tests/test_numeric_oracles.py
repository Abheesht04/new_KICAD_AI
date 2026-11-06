"""Numerical baseline against analytical oracles (docs/engineering/NUMERICAL_ROADMAP.md).

Usage: test_numeric_oracles.py COPPER_CORE NUMERIC_BENCH [unittest args]

Asserts what the current solver achieves. A measured limitation the roadmap is built on is pinned as an expected
failure, so fixing it fails the run until the roadmap and this test are updated together (step 7 did that
for the diode supply limit). No oracle here is the implementation under test.
"""
import json, math, os, subprocess, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = str(Path(sys.argv.pop(1)).resolve()); BENCH = str(Path(sys.argv.pop(1)).resolve())
os.environ['COPPER_CORE'] = CORE
sys.path.insert(0, str(ROOT / 'tools'))
import numeric_bench as nb

KERNELS = json.loads(subprocess.run([BENCH, '--quick'], capture_output=True, text=True, timeout=600, check=True).stdout)


class LinearKernel(unittest.TestCase):
    def test_ladder_matches_closed_form(self):
        for r in KERNELS['ladder']:
            self.assertEqual(r['status'], 'success'); self.assertLess(r['max_error_v'], 1e-12, r)
            self.assertLess(r['fill_fraction'], 0.25 if r['nodes'] >= 10 else 1)
    def test_scaling_across_fifteen_decades(self):
        for r in KERNELS['divider_scaling']:
            self.assertEqual(r['status'], 'success', r); self.assertLess(r['error_v'], 1e-12, r)
        self.assertLess(KERNELS['mixed_scale']['error_v'], 1e-12)
        for r in KERNELS['teraohm_chain']:
            self.assertEqual(r['status'], 'success'); self.assertLess(r['max_error_v'], 1e-12)
    def test_degenerate_systems_are_refused(self):
        status = {r['case']: r['status'] for r in KERNELS['degenerate']}
        self.assertEqual(status['parallel_sources_conflict'], 'singular_matrix')
        self.assertEqual(status['floating_node'], 'singular_matrix')
        self.assertEqual(status['parallel_sources_equal'], 'singular_matrix')


class Newton(unittest.TestCase):
    cases = {c['supply_v']: c for c in KERNELS['diode_newton']['cases']}
    def test_every_supply_converges_to_lambert_w(self):
        # Before step 7 (global 0.25 V clamp) 25 V and above did not converge; junction limiting (roadmap N1).
        self.assertEqual(sorted(self.cases), [0.3, 0.7, 1, 5, 10, 20, 24, 25, 26, 30, 50, 100, 1000])
        for v, c in self.cases.items():
            self.assertEqual(c['status'], 'success', c); self.assertLess(c['relative_error'], 1e-9, c)
    def test_iterations_do_not_grow_with_supply(self):
        # Was ~4 per volt (98 at 24 V); now bounded independently of the supply.
        self.assertLessEqual(max(c['iterations'] for c in self.cases.values()), 20)


def analyze(text):
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / 'p.copper'; path.write_text(text, encoding='utf-8')
        r = subprocess.run([CORE, 'analyze', str(path)], capture_output=True, text=True, timeout=120)
        return json.loads(r.stdout)


PARTS = ('(parts (part resistor (kind R) (symbol "Device:R") (footprint "R:R")) (part capacitor (kind C) (symbol "Device:C") (footprint "C:C")) '
         '(part inductor (kind L) (symbol "Device:L") (footprint "L:L")) (part voltage (kind V)) '
         '(part npn (kind Q) (model ebers_moll.npn) (beta_f 100) (pinmap (collector 3) (base 1) (emitter 2)) (symbol "Q:Q") (footprint "Q:Q")))')


def project(nets, *components):
    body = []
    for ref, part, value, pins in components:
        body.append(f'(component {ref} (part {part}) ' + ('' if value is None else f'(value {value}) ') + f'(pins {pins}))')
    return f'(copper_project (version 2) (name "t") (nets GND {nets}) {PARTS} (components {" ".join(body)}))'


def two(ref, part, a, b, value):
    return (ref, part, value, f'(1 {a}) (2 {b})')


class Diagnosis(unittest.TestCase):
    """Singular systems are refused naming the cause in the circuit's own terms (roadmap S2)."""
    def failure(self, text):
        d = analyze(text); self.assertFalse(d['dc_solved'])
        return next(i['message'] for i in d['issues'] if i['rule'] == 'mna_failure')

    def test_parallel_sources_floating_net_source_loop_inductor_short(self):
        m = self.failure(project('VIN', two('V1', 'voltage', 'VIN', 'GND', 5), two('V2', 'voltage', 'VIN', 'GND', 5),
                                 two('R1', 'resistor', 'VIN', 'GND', 1000)))
        self.assertIn('the current of V1, the current of V2', m); self.assertIn('in parallel or in a loop', m)
        m = self.failure(project('VIN X', two('V1', 'voltage', 'VIN', 'GND', 5), two('R1', 'resistor', 'VIN', 'GND', 1000),
                                 two('C1', 'capacitor', 'VIN', 'X', 1e-6), two('C2', 'capacitor', 'X', 'GND', 1e-6)))
        self.assertIn('net X has no DC path', m)
        m = self.failure(project('A B', two('V1', 'voltage', 'A', 'GND', 5), two('V2', 'voltage', 'B', 'GND', 3),
                                 two('V3', 'voltage', 'A', 'B', 2), two('R1', 'resistor', 'A', 'GND', 1000)))
        self.assertIn('the current of V1, the current of V2, the current of V3', m)
        m = self.failure(project('VIN', two('V1', 'voltage', 'VIN', 'GND', 5), two('L1', 'inductor', 'VIN', 'GND', 1e-3)))
        self.assertIn('the current of V1, the current of L1', m)

    def test_condition_estimate_and_ill_conditioning(self):
        d = analyze((ROOT / 'examples/v2/divider.copper').read_text(encoding='utf-8'))
        self.assertTrue(1 < d['condition_estimate'] < 1e6); self.assertNotIn('ill_conditioned', {i['rule'] for i in d['issues']})
        # 1 nOhm into B and 1 TOhm only from B to ground: kappa_1 ~ 4e9 exactly (B is pinned by the stiff resistor),
        # below the warning. A node C held only by two 1 TOhm resistors: kappa_1 ~ 1e21, warned.
        tight = [two('V1', 'voltage', 'A', 'GND', 1), two('R1', 'resistor', 'A', 'B', 1e-9), two('R2', 'resistor', 'B', 'GND', 1e12)]
        d = analyze(project('A B', *tight))
        self.assertAlmostEqual(d['condition_estimate'], 4e9, delta=4e7); self.assertNotIn('ill_conditioned', {i['rule'] for i in d['issues']})
        d = analyze(project('A B C', *tight, two('R3', 'resistor', 'B', 'C', 1e12), two('R4', 'resistor', 'C', 'GND', 1e12)))
        self.assertTrue(d['dc_solved']); self.assertIn('ill_conditioned', {i['rule'] for i in d['issues']})
        self.assertGreater(d['condition_estimate'], 1e20)


class RelativePivot(unittest.TestCase):
    def test_tiny_capacitive_divider_is_solvable(self):
        # Two 0.01 pF capacitors at 1 Hz put ~1.3e-13 S on the diagonal: the old absolute 1e-12 pivot threshold
        # reported this solvable system singular (audit I-01); the relative test solves it exactly (OUT = IN / 2).
        text = project('IN OUT', ('V1', 'voltage', 0, '(1 IN) (2 GND)) (ac 1 0'), two('C1', 'capacitor', 'IN', 'OUT', 1e-14),
                       two('C2', 'capacitor', 'OUT', 'GND', 1e-14))
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'p.copper'; path.write_text(text, encoding='utf-8')
            r = subprocess.run([CORE, 'ac', str(path), '1', '1', '1', 'lin'], capture_output=True, text=True, timeout=120)
        rows = [line.split(',') for line in r.stdout.strip().splitlines()]
        row = dict(zip(rows[0], rows[1]))
        self.assertEqual((r.returncode, row['status']), (0, 'ok'), r.stderr)
        self.assertAlmostEqual(float(row['V(OUT)_mag']), 0.5, places=12)


class Bjt(unittest.TestCase):
    def test_bias_point_at_high_supply(self):
        # Before step 7 the 0.25 V clamp could not reach these supplies within 100 iterations.
        for vcc in (12, 48, 400):
            d = analyze(project('VCC C B E', two('V1', 'voltage', 'VCC', 'GND', vcc), two('RC', 'resistor', 'VCC', 'C', 4700),
                                two('RB1', 'resistor', 'VCC', 'B', 1e5), two('RB2', 'resistor', 'B', 'GND', 1e4),
                                two('RE', 'resistor', 'E', 'GND', 1000), ('Q1', 'npn', None, '(1 B) (2 E) (3 C)')))
            self.assertTrue(d['dc_solved'], vcc); v = {n['name']: n['voltage_v'] for n in d['nodes']}
            # Forward active: Ic = beta_f / (beta_f + 1) * Ie with Ie = VE / RE; one junction drop from base to emitter.
            self.assertAlmostEqual(v['C'], vcc - 4700 * v['E'] / 1000 * 100 / 101, delta=1e-6 * vcc)
            self.assertTrue(0.6 < v['B'] - v['E'] < 0.95, v)


class Transient(unittest.TestCase):
    rc = nb.rc_order(CORE, True); lc = nb.lc_amplitude(CORE, True); stiff = nb.stiff(CORE)
    def test_backward_euler_is_first_order_and_trapezoidal_second(self):
        self.assertAlmostEqual(self.rc['be'][-1]['observed_order'], 1.0, delta=0.1)
        self.assertGreater(self.rc['trap'][-1]['observed_order'], 1.7)
    def test_trapezoidal_preserves_lc_amplitude_and_euler_damps_it(self):
        self.assertLess(abs(self.lc['trap']['peak_error_fraction']), 0.01)
        self.assertLess(self.lc['be']['peak_error_fraction'], -0.1)
    def test_backward_euler_does_not_ring_on_stiff_nodes(self):
        self.assertEqual(self.stiff['be']['fast_sign_changes'], 0)
    @unittest.expectedFailure
    def test_trapezoidal_does_not_ring_on_stiff_nodes(self):
        self.assertEqual(self.stiff['trap']['fast_sign_changes'], 0)  # roadmap T2: trapezoidal is not L-stable


class SmallSignalAndCorners(unittest.TestCase):
    def test_ac_matches_transfer_function(self):
        r = nb.ac_rc(CORE); self.assertEqual(r['points'], 41)
        self.assertLess(r['max_magnitude_error'], 1e-9); self.assertLess(r['max_phase_error_deg'], 1e-6)
    def test_corners_are_exhaustive_up_to_eight(self):
        rows = nb.corners(CORE, True)
        for r in rows:
            self.assertEqual(r['corners'], 2 ** r['resistors']); self.assertFalse(r['refused'])


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
