"""The prompt-acceptance harness itself: classification rules and a real run of its Unicode cases; and Slice 1,
one English description to a divider board that passes KiCad 9.x ERC and DRC.

Usage: test_prompt_acceptance.py PCBA [unittest args]
Exit status: 0 all ran and passed, 1 failure, 77 skipped (UNVERIFIED: no KiCad 9.x CLI or libraries).
Slice 1 writes its project to $COPPER_SLICE1_OUT when set (open design.kicad_pro in KiCad), else a temporary folder.
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


class Slice1(unittest.TestCase):
    """English -> .espec -> .copper -> KiCad project -> native ERC, DRC, netlist and pin/net parity, all PASS on KiCad 9.x.
    The overall verdict stays UNVERIFIED (generic parts have no ratings or heights); this slice asserts the board gates."""
    def test_english_divider_board_passes_kicad9_erc_and_drc(self):
        import subprocess, copper
        cli = copper.discover_cli(os.environ.get('KICAD_CLI'))
        version = subprocess.run([cli, 'version'], capture_output=True, text=True).stdout.strip() if cli else ''
        if not version.startswith('9.'):
            self.skipTest(f'UNVERIFIED: KiCad 9.x CLI required (found {version or "none"}); set KICAD_CLI')
        try:
            libs = ['--symbols', str(copper.discover_symbol_dir(os.environ.get('KICAD_SYMBOL_DIR'))),
                    '--footprints', str(copper.discover_footprint_dir(os.environ.get('KICAD_FOOTPRINT_DIR')))]
        except FileNotFoundError as exc:
            self.skipTest(f'UNVERIFIED: KiCad 9.x libraries required: {exc}')
        text = next(c['text'] for c in json.loads((ROOT / 'tests/fixtures/spec/corpus.json').read_text(encoding='utf-8'))['cases']
                    if c['name'] == 'divider board')
        keep = os.environ.get('COPPER_SLICE1_OUT'); tmp = None if keep else tempfile.TemporaryDirectory()
        out = Path(keep or tmp.name); out.mkdir(parents=True, exist_ok=True)
        try:
            def pcba(*args):
                r = subprocess.run([PCBA, *map(str, args), '--json'], capture_output=True, cwd=str(ROOT), timeout=900)
                return r.returncode, json.loads(r.stdout.decode('utf-8'))
            for args in (('spec', 'parse', text, '--out', out / 'divider.espec', '--force'),
                         ('spec', 'lower', out / 'divider.espec', '--out', out / 'divider.copper', '--force')):
                rc, doc = pcba(*args); self.assertEqual((doc['status'], rc), ('PASS', 0), doc['message'])
            rc, doc = pcba('project', 'build', out / 'divider.copper', '--out', out / 'build', *libs)
            self.assertIn(doc['status'], ('PASS', 'UNVERIFIED'), doc['message'])
            self.assertTrue((out / 'build/design.kicad_pro').is_file(), 'no KiCad project was written')
            rc, doc = pcba('project', 'verify', '--out', out / 'build', '--kicad-cli', cli)
            ver = doc['result']['data']['verification']
            self.assertTrue(str(ver['kicad_version']).startswith('9.'), ver['kicad_version'])
            gates = {g['gate']: g['status'] for g in ver['gates']}
            self.assertEqual(gates, {'erc': 'PASS', 'drc': 'PASS', 'netlist': 'PASS', 'netlist_parity': 'PASS'},
                             [(g['gate'], g.get('message', '')) for g in ver['gates']])
            checks = {c['check']: c for c in ver['checks']}
            self.assertEqual(checks['board_content']['status'], 'PASS', checks['board_content']['message'])
            self.assertEqual(checks['board_content']['counts'], {'footprints': 3, 'pads': 7, 'nets': 3})
            self.assertEqual(checks['output_hashes']['status'], 'PASS')
        finally:
            if tmp:
                tmp.cleanup()


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
