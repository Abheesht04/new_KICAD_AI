"""Native KiCad gate classification and board-content qualification, without KiCad (fake kicad-cli).

Usage: test_native_gates.py COPPER_CORE [unittest args]

The fake CLI reports version 9.0.6 and writes scripted ERC/DRC/netlist reports, so every classification path is
exercised: clean, warnings only, errors, unconnected items, parity issues, stderr noise with a clean report,
a crash without a report, and a stale report left by an earlier run. Real KiCad remains the authority; this only
proves that Copper reads KiCad's results honestly.
"""
import argparse, contextlib, io, json, os, shutil, sys, tempfile, textwrap, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
os.environ['COPPER_CORE'] = CORE
sys.path.insert(0, str(ROOT / 'tools'))
import copper
from hwc_kicad import direct, one
SNAP = ROOT / 'examples/generated/divider/libraries'

FAKE = textwrap.dedent('''
    import json, os, sys
    sys.path.insert(0, os.environ['FAKE_TOOLS'])
    scenario = json.loads(open(os.environ['FAKE_SCENARIO']).read())
    args = sys.argv[1:]
    if args == ['version']:
        print('9.0.6'); sys.exit(0)
    out = args[args.index('-o') + 1]; kind = args[0] + ' ' + args[1]
    sys.stderr.write(scenario.get('stderr', ''))
    if kind == 'pcb export':  # the real KiCad 9.0.6 CAM output of the divider, optionally tampered with
        import shutil
        source = os.path.join(scenario['cam'], 'gerber_test' if args[2] == 'gerbers' else 'drill_test')
        for name in os.listdir(source):
            shutil.copyfile(os.path.join(source, name), os.path.join(out, name))
        if args[2] == 'drill' and scenario.get('move_hole'):
            path = os.path.join(out, 'design.drl'); text = open(path).read().replace('Y-14.54', 'Y-14.64')
            open(path, 'w').write(text)
        sys.exit(0)
    if kind == 'sch erc':
        if scenario.get('erc_write', True):
            json.dump({'sheets': [{'violations': scenario.get('erc', [])}]}, open(out, 'w'))
        sys.exit(scenario.get('erc_rc', 5 if scenario.get('erc') else 0))
    if kind == 'pcb drc':
        if scenario.get('drc_write', True):
            json.dump({'violations': scenario.get('drc', []), 'unconnected_items': scenario.get('unconnected', []),
                       'schematic_parity': scenario.get('parity', [])}, open(out, 'w'))
        sys.exit(scenario.get('drc_rc', 5 if scenario.get('drc') or scenario.get('unconnected') else 0))
    if kind == 'sch export':
        import copper
        build = os.path.dirname(out)
        data = [c for c in copper.component_data(copper.load(os.path.join(build, 'resolved.copper'))) if c['kind'] not in ('V', 'I')]
        nets = {}
        for c in data:
            for pin, net in c['pins'].items():
                nets.setdefault(net, []).append((c['ref'], pin))
        xml = ['<export><nets>'] + [f'<net name="/{n}">' + ''.join(f'<node ref="{r}" pin="{p}"/>' for r, p in nodes) + '</net>' for n, nodes in nets.items()] + ['</nets></export>']
        open(out, 'w').write(''.join(xml)); sys.exit(0)
    sys.exit(2)
''')
WARN = {'severity': 'warning', 'type': 'lib_footprint_mismatch', 'description': "Footprint 'X' does not match copy in library", 'items': []}
ERR = {'severity': 'error', 'type': 'clearance', 'description': 'Clearance violation', 'items': []}


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.dir = Path(cls.tmp.name)
        cls.source = cls.dir / 'divider.copper'; shutil.copyfile(ROOT / 'examples/v2/divider.copper', cls.source)
        args = argparse.Namespace(project=str(cls.source), out=str(cls.dir / 'build'), core=CORE, symbol_dir=str(SNAP / 'symbols'),
                                  footprint_dir=str(SNAP / 'footprints'), no_route=False, draft=False)
        with contextlib.redirect_stdout(io.StringIO()):
            copper.export_project(args)
        (cls.dir / 'fake_kicad.py').write_text(FAKE, encoding='utf-8')
        if os.name == 'nt':
            cls.cli = cls.dir / 'kicad-cli.cmd'
            cls.cli.write_text(f'@"{sys.executable}" "{cls.dir / "fake_kicad.py"}" %*\n', encoding='utf-8')
        else:
            cls.cli = cls.dir / 'kicad-cli'
            cls.cli.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{cls.dir / "fake_kicad.py"}" "$@"\n'); cls.cli.chmod(0o755)
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def run_gates(self, scenario, fresh=True):
        build = self.dir / ('run' if fresh else 'build')
        if fresh:
            shutil.rmtree(build, ignore_errors=True); shutil.copytree(self.dir / 'build', build)
        (self.dir / 'scenario.json').write_text(json.dumps(scenario))
        os.environ['FAKE_SCENARIO'] = str(self.dir / 'scenario.json'); os.environ['FAKE_TOOLS'] = str(ROOT / 'tools')
        version, gates = copper.native_gates(build, str(self.cli))
        return version, {g['gate']: g for g in gates}, build


class Classification(Base):
    def test_clean_reports_pass(self):
        version, g, _ = self.run_gates({})
        self.assertEqual(version, '9.0.6')
        self.assertEqual({k: v['status'] for k, v in g.items()}, {'erc': 'PASS', 'drc': 'PASS', 'netlist': 'PASS', 'netlist_parity': 'PASS'})
    def test_warnings_only_are_unverified_not_fail_and_not_pass(self):
        _, g, _ = self.run_gates({'drc': [WARN]})
        self.assertEqual(g['drc']['status'], 'UNVERIFIED'); self.assertEqual(g['drc']['returncode'], 5)
        self.assertEqual(g['drc']['counts']['warnings'], 1); self.assertIn('lib_footprint_mismatch', g['drc']['message'])
    def test_errors_unconnected_and_parity_fail(self):
        for scenario in ({'drc': [ERR, WARN]}, {'unconnected': [{'type': 'unconnected_items', 'items': []}]},
                         {'parity': [{'type': 'net_conflict', 'description': 'x', 'items': []}], 'drc_rc': 5},
                         {'erc': [{'severity': 'error', 'type': 'pin_not_driven', 'description': 'x', 'items': []}]}):
            _, g, _ = self.run_gates(scenario)
            failed = [k for k, v in g.items() if v['status'] == 'FAIL']
            self.assertTrue(failed, scenario)
    def test_stderr_noise_with_a_clean_report_passes(self):
        _, g, _ = self.run_gates({'stderr': 'EXCEPTION: Legacy zone fill strategy is not supported anymore.\n'})
        self.assertEqual(g['drc']['status'], 'PASS'); self.assertIn('Legacy zone fill', g['drc']['stderr'])
    def test_crash_without_report_is_unverified(self):
        _, g, _ = self.run_gates({'drc_write': False, 'drc_rc': 9009})
        self.assertEqual(g['drc']['status'], 'UNVERIFIED'); self.assertIn('no readable drc.json', g['drc']['message'])
    def test_stale_report_is_never_reused(self):
        _, g, build = self.run_gates({})
        self.assertEqual(g['drc']['status'], 'PASS')
        (self.dir / 'scenario.json').write_text(json.dumps({'drc_write': False, 'drc_rc': 1}))
        _, gates = copper.native_gates(build, str(self.cli))
        self.assertEqual({x['gate']: x for x in gates}['drc']['status'], 'UNVERIFIED')


class Content(Base):
    def copy(self):
        build = self.dir / 'content'; shutil.rmtree(build, ignore_errors=True); shutil.copytree(self.dir / 'build', build); return build
    def test_built_board_holds_the_design(self):
        r = copper.board_content(self.dir / 'build')
        self.assertEqual(r['status'], 'PASS', r); self.assertEqual(r['counts']['footprints'], 3)
    def test_missing_footprint_or_wrong_net_fails(self):
        build = self.copy(); pcb = copper.load(build / 'design.kicad_pcb')
        pcb[:] = [x for x in pcb if not (isinstance(x, list) and str(x[0]) == 'footprint' and 'R_0603' in str(x[1]) and
                  next(str(p[2]) for p in direct(x, 'property') if str(p[1]) == 'Reference') == 'R2')]
        copper.write_tree(build / 'design.kicad_pcb', pcb)
        r = copper.board_content(build); self.assertEqual(r['status'], 'FAIL'); self.assertIn('R2', r['message'])
        build = self.copy(); pcb = copper.load(build / 'design.kicad_pcb')
        for fp in direct(pcb, 'footprint'):
            for pad in direct(fp, 'pad'):
                if one(pad, 'net'):
                    one(pad, 'net')[2] = copper.Q('/WRONG')
        copper.write_tree(build / 'design.kicad_pcb', pcb)
        self.assertEqual(copper.board_content(build)['status'], 'FAIL')
    def test_board_without_physical_parts_is_unverified(self):
        build = self.copy(); tree = copper.load(build / 'resolved.copper')
        comps = one(tree, 'components'); comps[:] = [c for c in comps if not (isinstance(c, list) and str(c[1]) != 'V1')]
        copper.write_tree(build / 'resolved.copper', tree)
        self.assertEqual(copper.board_content(build)['status'], 'UNVERIFIED')


class SecondJudge(Base):
    """design.kicad_judge: a layout candidate is built through the shipping export and judged by KiCad DRC only."""
    def test_candidates_are_judged_by_kicad_drc(self):
        import design, layout
        tree = copper.load(ROOT / 'tests/fixtures/layout/mixed_current.copper')
        best, cands, problem = layout.optimize(tree, SNAP / 'footprints', CORE, (1,), 30)
        os.environ['FAKE_TOOLS'] = str(ROOT / 'tools')
        (self.dir / 'scenario.json').write_text('{}'); os.environ['FAKE_SCENARIO'] = str(self.dir / 'scenario.json')
        judge = design.kicad_judge(problem, SNAP / 'symbols', SNAP / 'footprints', str(self.cli), CORE)
        self.assertEqual(design.kicad9(str(self.cli)), (True, '9.0.6'))
        for scenario, status in (({}, 'PASS'), ({'drc': [ERR]}, 'FAIL'), ({'drc': [WARN]}, 'UNVERIFIED')):
            (self.dir / 'scenario.json').write_text(json.dumps(scenario)); os.environ['FAKE_SCENARIO'] = str(self.dir / 'scenario.json')
            self.assertEqual(judge(best)['status'], status, scenario)
        (self.dir / 'scenario.json').write_text(json.dumps({'drc': [ERR]}))
        chosen, verdicts = layout.second_judge(cands, judge, limit=2)
        self.assertEqual([v['status'] for v in verdicts], ['FAIL', 'FAIL']); self.assertIs(chosen, min((c for c in cands if c['feasible']), key=layout.candidate_key))


class Cam(Base):
    """CAM output re-read without KiCad (copper.cam_check): the real KiCad 9.0.6 Gerber job and Excellon drill files
    exported from this divider (manufacturing_audit/{gerber_test,drill_test}) must agree with the design; any tampering is FAIL."""
    CAM = ROOT / 'manufacturing_audit'

    def tampered(self, edit):
        cam = self.dir / 'cam-edit'; shutil.rmtree(cam, ignore_errors=True); shutil.copytree(self.CAM, cam, ignore=shutil.ignore_patterns('*.zip'))
        edit(cam)
        return {c['check']: c for c in copper.cam_check(self.dir / 'build', cam, CORE)}

    def test_kicad_906_cam_output_matches_the_design(self):
        checks = {c['check']: c for c in copper.cam_check(self.dir / 'build', self.CAM, CORE)}
        self.assertEqual(checks['drill_holes']['status'], 'PASS', checks)
        self.assertEqual(checks['drill_holes']['counts'], {'design': 3, 'drill_files': 3})
        self.assertEqual((checks['gerber_job']['status'], checks['gerber_job']['kicad_version']), ('PASS', '9.0.6'))

    def test_any_disagreement_fails(self):
        def edit(name, old, new):
            def f(cam):
                path = next(cam.rglob(name)); path.write_text(path.read_text().replace(old, new, 1))
            return f
        for change, check, needle in ((edit('design.drl', 'Y-14.54', 'Y-14.64'), 'drill_holes', 'missing hole at x 8.0 y -14.54'),
                                      (edit('design.drl', 'T1C1.000', 'T1C0.900'), 'drill_holes', 'drill 1.0 mm'),
                                      (edit('design.drl', 'X8.0Y-17.08\n', ''), 'drill_holes', 'missing hole'),
                                      (edit('design-job.gbrjob', '"LayerNumber": 2', '"LayerNumber": 4'), 'gerber_job', 'LayerNumber 4'),
                                      (edit('design-job.gbrjob', '"X": 40.05', '"X": 45.05'), 'gerber_job', 'board size'),
                                      (lambda cam: next(cam.rglob('design-B_Cu.gbl')).unlink(), 'gerber_job', 'design-B_Cu.gbl is missing')):
            result = self.tampered(change)[check]
            self.assertEqual(result['status'], 'FAIL', (needle, result)); self.assertIn(needle, result['message'])
        self.assertEqual(self.tampered(lambda cam: next(cam.rglob('design-job.gbrjob')).unlink())['gerber_job']['status'], 'UNVERIFIED')

    def test_fabricate_rereads_what_it_exports(self):
        tree = copper.load(ROOT / 'examples/v2/divider.copper')
        for p in direct(one(tree, 'parts'), 'part'):   # synthetic, explicitly supplied test ratings (as tests/test_v2.py)
            fields = {'resistor': (('power_w', '0.1'), ('voltage_v', '50'), ('height_mm', '0.55')), 'header3': (('height_mm', '2.5'),)}
            for key, value in fields.get(str(p[1]), ()):
                p[:] = [x for x in p if not (isinstance(x, list) and str(x[0]) == key)] + [copper.sx(key, copper.A(value))]
        source = self.dir / 'rated.copper'; copper.write_tree(source, tree)
        args = argparse.Namespace(project=str(source), out=str(self.dir / 'rated'), core=CORE, symbol_dir=str(SNAP / 'symbols'),
                                  footprint_dir=str(SNAP / 'footprints'), no_route=False, draft=False)
        shutil.rmtree(self.dir / 'rated', ignore_errors=True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(copper.export_project(args), 0)
        os.environ['FAKE_TOOLS'] = str(ROOT / 'tools')
        for scenario, rc, status in (({'cam': str(self.CAM)}, 0, 'PASS'), ({'cam': str(self.CAM), 'move_hole': True}, 2, 'FAIL')):
            (self.dir / 'scenario.json').write_text(json.dumps(scenario)); os.environ['FAKE_SCENARIO'] = str(self.dir / 'scenario.json')
            shutil.rmtree(self.dir / 'rated' / 'fabrication', ignore_errors=True)
            result = self.dir / 'fab.json'
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                got = copper.main(['--result-json', str(result), 'fabricate', '--out', str(self.dir / 'rated'), '--kicad-cli', str(self.cli), '--core', CORE])
            doc = json.loads(result.read_text())
            self.assertEqual((got, doc['status']), (rc, status), doc['message'])
        self.assertIn('CAM output disagrees with the design', doc['message']); self.assertIn('-14.54', doc['message'])


class Placement(unittest.TestCase):
    """A project lowered from a spec has no placements: build places it instead of stacking footprints at (0, 0)."""
    def test_unplaced_project_is_placed_and_partial_is_refused(self):
        tree = copper.load(ROOT / 'examples/v2/divider.copper')
        for c in direct(one(tree, 'components'), 'component'):
            c[:] = [x for x in c if not (isinstance(x, list) and str(x[0]) == 'at')]
        placed, record = copper.auto_place(tree, SNAP / 'footprints', CORE)
        self.assertIn('generated', record['source'])
        self.assertTrue(all(one(c, 'at') for c in direct(one(placed, 'components'), 'component') if str(c[1]) != 'V1'))
        partial = copper.load(ROOT / 'examples/v2/divider.copper')
        r1 = next(c for c in direct(one(partial, 'components'), 'component') if str(c[1]) == 'R1')
        r1[:] = [x for x in r1 if not (isinstance(x, list) and str(x[0]) == 'at')]
        with self.assertRaises(copper.InvalidInput):
            copper.auto_place(partial, SNAP / 'footprints', CORE)
        same, record = copper.auto_place(copper.load(ROOT / 'examples/v2/divider.copper'), SNAP / 'footprints', CORE)
        self.assertIsNone(record, 'a fully placed project keeps its placements')


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
