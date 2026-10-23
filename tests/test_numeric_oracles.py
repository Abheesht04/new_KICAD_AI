"""Numerical baseline against analytical oracles (docs/engineering/NUMERICAL_ROADMAP.md).

Usage: test_numeric_oracles.py COPPER_CORE NUMERIC_BENCH [unittest args]

Asserts what the current solver achieves, and pins the measured limitations the roadmap is built on as
expected failures: when a limitation is fixed the test reports an unexpected success, which fails the run,
so the roadmap and this test must be updated together. No oracle here is the implementation under test.
"""
import json, math, os, subprocess, sys, unittest
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
    def test_converged_cases_match_lambert_w(self):
        for v, c in self.cases.items():
            if c['status'] == 'success':
                self.assertLess(c['relative_error'], 1e-9, c)
    def test_iterations_grow_with_supply(self):
        # The global 0.25 V update clamp makes iterations ~ 4 per volt of supply (roadmap N1).
        self.assertGreaterEqual(self.cases[20]['iterations'], 80)
    @unittest.expectedFailure
    def test_diode_on_25_volts_converges(self):
        self.assertEqual(self.cases[25]['status'], 'success')  # roadmap N1: fails today (not_converged at 100 iterations)


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
