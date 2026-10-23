"""The prompt-acceptance harness itself: classification rules and a real run of its Unicode cases.

Usage: test_prompt_acceptance.py PCBA [unittest args]
"""
import json, os, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
PCBA = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None
sys.path.insert(0, str(ROOT / 'tools'))
import prompt_acceptance as pa


def step(status, exit_code):
    return dict(argv=['pcba'], cwd='.', exit=exit_code, seconds=0, stdout='o', stderr='e', status=status,
                envelope=None if status == 'NO_JSON' else {'status': status, 'message': '', 'result': {}})


class Classification(unittest.TestCase):
    def test_status_and_exit_must_both_match(self):
        self.assertEqual(pa.expect_status(step('PASS', 0), 'PASS')[0], 'PASS')
        self.assertEqual(pa.expect_status(step('PASS', 3), 'PASS')[0], 'FAIL')        # status/exit disagree
        self.assertEqual(pa.expect_status(step('UNVERIFIED', 3), 'UNVERIFIED')[0], 'PASS')
        self.assertEqual(pa.expect_status(step('INVALID_INPUT', 2), 'PASS')[0], 'FAIL')
        outcome, actual = pa.expect_status(step('NO_JSON', 70), 'PASS')
        self.assertEqual(outcome, 'FAIL'); self.assertIn('no JSON', actual)
    def test_product_status_is_never_promoted(self):
        self.assertEqual(pa.product_outcome('UNVERIFIED'), 'UNVERIFIED')
        self.assertEqual(pa.product_outcome('FAIL'), 'FAIL')
        self.assertEqual(pa.product_outcome('ERROR'), 'FAIL')
    def test_unknown_outcomes_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            run = pa.Runner('pcba', d)
            with self.assertRaises(AssertionError):
                run.record('x', 'g', 'e', 'PASSED', 'a')
    def test_observational_failure_is_not_a_pass(self):
        class Fake(pa.Runner):
            def call(self, name, *args):
                return step('FAIL', 1)
        with tempfile.TemporaryDirectory() as d:
            run = Fake('pcba', d)
            pa.esp32(run, type('A', (), dict(symbols=None, footprints=None, kicad_cli=None))())
            self.assertEqual(run.results[0]['outcome'], 'OBSERVATIONAL'); self.assertIn('FAIL/1', run.results[0]['actual'])
            counts = pa.write(run)
            self.assertEqual(counts['PASS'], 0); self.assertEqual(counts['OBSERVATIONAL'], 1)
    def test_exit_status_follows_the_worst_outcome(self):
        with tempfile.TemporaryDirectory() as d:
            for outcomes, code in ((['PASS', 'SKIPPED', 'OBSERVATIONAL'], 0), (['PASS', 'BLOCKED'], 3), (['PASS', 'UNVERIFIED'], 3), (['PASS', 'FAIL', 'BLOCKED'], 1)):
                run = pa.Runner('pcba', d)
                for i, o in enumerate(outcomes):
                    run.record(str(i), 'g', 'e', o, 'a')
                c = pa.write(run)
                self.assertEqual(1 if c['FAIL'] else 3 if c['BLOCKED'] or c['UNVERIFIED'] or c['NOT_IMPLEMENTED'] else 0, code, outcomes)


@unittest.skipUnless(PCBA, 'needs pcba')
class UnicodeRun(unittest.TestCase):
    def test_unicode_prompts_pass_as_arguments_and_utf8_files(self):
        with tempfile.TemporaryDirectory() as d:
            rc = pa.main(['--pcba', PCBA, '--out', d, '--only', 'rc filter with unicode units,micro sign and ohm sign'])
            results = json.loads((Path(d) / 'results.json').read_text(encoding='utf-8'))['results']
            self.assertEqual(rc, 0, results)
            self.assertEqual({r['name']: r['outcome'] for r in results if r['group'].startswith('english-corpus')},
                             {'prompt:rc filter with unicode units': 'PASS', 'prompt-file:rc filter with unicode units': 'PASS',
                              'prompt:micro sign and ohm sign': 'PASS', 'prompt-file:micro sign and ohm sign': 'PASS'})
            prompt = (Path(d) / 'corpus/micro-sign-and-ohm-sign/prompt.txt').read_bytes()
            self.assertIn('kΩ'.encode('utf-8'), prompt); self.assertNotIn(b'\xc3\x8e', prompt)  # UTF-8, never double-encoded
    def test_double_encoded_text_is_named_not_misparsed(self):
        import subprocess
        r = subprocess.run([PCBA, 'spec', 'parse', 'a 2.2 ÂµF capacitor between VIN and GND', '--json'], capture_output=True)
        doc = json.loads(r.stdout.decode('utf-8'))
        self.assertEqual(doc['status'], 'INVALID_INPUT'); self.assertIn('double-encoded', doc['message'])
        self.assertIn('µ', doc['message'], 'the envelope message keeps non-ASCII text (no literal \\u escapes)')


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
