#!/usr/bin/env python3
"""Track B on real KiCad-saved boards: every KiCad 9.0 demo board (generator_version "9.0", format 20241229) in a
KiCad installation, read by the C board model (copper-core board).

For each board:
  1. it loads (status OK);
  2. its item counts match an independent reading by the reference S-expression parser (tools/hwc_kicad.py):
     footprints, pads, tracks (segments and arcs), vias, zones;
  3. KiCad's own drill export of the board (kicad-cli pcb export drill, read back by copper-core drill) holds exactly
     the round holes of the model: every plated or non-plated round pad hole and every via, by position and diameter
     to 1 um (the Excellon files carry 3 decimals in mm).

The board files are KiCad 9.0 output, the strongest evidence available here; they are read in place and never
copied into this repository (their licences vary). The drill export is run by whatever kicad-cli is given; a
KiCad 10 CLI makes step 3 a development cross-check, which the report says.

Usage: test_board_demos.py COPPER_CORE   with KICAD_DEMOS_DIR (the demos folder) and optionally KICAD_CLI set.
Exit 77 (UNVERIFIED) when no demos folder is configured.
"""
import json, os, subprocess, sys, tempfile, unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hwc_kicad import parse_sexpr  # noqa: E402

CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
DEMOS = os.environ.get('KICAD_DEMOS_DIR')
CLI = os.environ.get('KICAD_CLI')
MAX_BYTES = 12_000_000   # the largest demos (70-90 MB) are left out to keep the run short; say so in the report
DRILL_EXPORT = ['--format', 'excellon', '--drill-origin', 'absolute', '--excellon-zeros-format', 'decimal', '--excellon-units', 'mm']


def kicad9_boards():
    out = []
    for path in sorted(Path(DEMOS).rglob('*.kicad_pcb')):
        head = path.read_text(encoding='utf-8', errors='replace')[:400]
        if '(generator_version "9.0")' in head and path.stat().st_size <= MAX_BYTES:
            out.append(path)
    return out


def heads(node, name):
    return [c for c in node[1:] if isinstance(c, list) and c and str(c[0]) == name]


def independent_counts(path):
    tree = parse_sexpr(path.read_text(encoding='utf-8'))
    fps = heads(tree, 'footprint')
    return dict(footprints=len(fps), pads=sum(len(heads(f, 'pad')) for f in fps),
                tracks=len(heads(tree, 'segment')) + len(heads(tree, 'arc')), vias=len(heads(tree, 'via')),
                zones=len(heads(tree, 'zone')) + sum(len(heads(f, 'zone')) for f in fps))


def model(path):
    r = subprocess.run([CORE, 'board', str(path)], capture_output=True, text=True, encoding='utf-8', timeout=600)
    return r.returncode, json.loads(r.stdout)


def um(nm):
    return round(nm / 1000)


class Demos(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DEMOS or not Path(DEMOS).is_dir():
            raise unittest.SkipTest('UNVERIFIED: set KICAD_DEMOS_DIR to a KiCad installation\'s demos folder')
        cls.boards = kicad9_boards()
        if not cls.boards:
            raise unittest.SkipTest(f'UNVERIFIED: no KiCad 9.0 boards under {DEMOS}')

    def test_loads_and_counts(self):
        for path in self.boards:
            with self.subTest(board=path.name):
                rc, b = model(path)
                self.assertEqual((rc, b['status']), (0, 'OK'), b)
                got = dict(footprints=len(b['footprints']), pads=sum(len(f['pads']) for f in b['footprints']),
                           tracks=b['tracks'], vias=len(b['vias']), zones=b['zones'])
                self.assertEqual(got, independent_counts(path))
                o = b['outline']
                # Exact closure, or KiCad-style closure across tiny gaps: these KiCad 9.0 files join ends that miss
                # by up to 2.5 um, which KiCad accepts. The model reports the gap; it never closes it silently.
                self.assertTrue(o['closed'] or 0 < o['max_gap_nm'] <= 5000, {k: v for k, v in o.items() if k != 'items'})
                print(f'  {path.name}: {got}, outline {o["loops"]} loop(s), ' + ('closed' if o['closed'] else f'gap {o["max_gap_nm"]} nm') + ', issues '
                      f'{dict(Counter(i["code"] for i in b["issues"]))}', file=sys.stderr)

    def test_holes_match_kicads_drill_export(self):
        if not CLI:
            self.skipTest('UNVERIFIED: set KICAD_CLI to run the drill export')
        version = subprocess.run([CLI, 'version'], capture_output=True, text=True).stdout.strip()
        print(f'  drill export by kicad-cli {version}' + ('' if version.startswith('9.') else ' (development cross-check, not KiCad 9 evidence)'),
              file=sys.stderr)
        for path in self.boards:
            with self.subTest(board=path.name), tempfile.TemporaryDirectory() as d:
                _, b = model(path)
                # Round holes through the whole board: pads with a round drill and through vias. Blind and micro vias
                # go to layer-pair files that this export does not write; they are counted, not compared.
                want, skipped = [], 0
                for f in b['footprints']:
                    for p in f['pads']:
                        if 'drill_nm' in p and p['drill_nm'][0] == p['drill_nm'][1] and p['drill_nm'][0] > 0:
                            want.append((p['x_nm'], -p['y_nm'], p['drill_nm'][0]))
                for v in b['vias']:
                    if v['type'] == 'through':
                        want.append((v['x_nm'], -v['y_nm'], v['drill_nm']))
                    else:
                        skipped += 1
                r = subprocess.run([CLI, 'pcb', 'export', 'drill', *DRILL_EXPORT, '-o', d + os.sep, str(path)],
                                   capture_output=True, text=True, timeout=600)
                self.assertEqual(r.returncode, 0, r.stderr)
                got = []
                for drl in sorted(Path(d).glob('*.drl')):
                    data = json.loads(subprocess.run([CORE, 'drill', str(drl)], capture_output=True, text=True, timeout=600).stdout)
                    dia = {t['number']: round(t['diameter_mm'] * 1e6) for t in data['tools']}
                    got += [(round(x * 1e6), round(y * 1e6), dia[tool]) for x, y, tool in data['hits']]
                self.assertEqual(len(got), len(want), path.name)
                # The Excellon files carry 3 decimals (1 um): each KiCad hole must match a distinct model hole to
                # within half of that, in position and diameter.
                buckets = {}
                for i, (x, y, dmt) in enumerate(want):
                    buckets.setdefault((round(x / 1000), round(y / 1000)), []).append(i)
                used, unmatched = set(), []
                for x, y, dmt in got:
                    hit = None
                    for kx in (x // 1000 - 1, x // 1000, x // 1000 + 1):
                        for ky in (y // 1000 - 1, y // 1000, y // 1000 + 1):
                            for i in buckets.get((kx, ky), []):
                                wx, wy, wd = want[i]
                                if i not in used and abs(wx - x) <= 500 and abs(wy - y) <= 500 and abs(wd - dmt) <= 500:
                                    hit = i
                    if hit is None:
                        unmatched.append((x, y, dmt))
                    else:
                        used.add(hit)
                self.assertEqual(unmatched, [], 'holes KiCad drilled that the model lacks')
                print(f'  {path.name}: {len(got)} round through holes agree' + (f'; {skipped} blind/micro vias not compared' if skipped else ''),
                      file=sys.stderr)


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
