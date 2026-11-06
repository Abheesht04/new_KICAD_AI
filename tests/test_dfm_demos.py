#!/usr/bin/env python3
"""Track C clearance against an independent brute-force oracle, on real KiCad 9.0-saved boards.

For each board, every pair of copper items the C checker decides exactly (straight tracks; round, oval, rect and
roundrect pads at multiples of 90 degrees; vias) is measured here from the board file read by the reference
S-expression parser (tracks) and the model's pad positions (already proven against KiCad's drill export by
test_board_demos), with plain floating-point geometry, all pairs, no BVH. The set of pairs closer than their JLCPCB
limit must equal the C checker's clearance violations exactly: no false FAIL and no missed one. Pairs within 2 nm of
their limit are left out (the C side rounds sizes outward to whole nm). Arc tracks, rotated, custom and trapezoid
pads are UNVERIFIED in C and skipped here.

Usage: test_dfm_demos.py COPPER_CORE with KICAD_DEMOS_DIR set. Exit 77 (UNVERIFIED) without it.
This is a test harness (allowed in Python by CHARTER 3.1); the checker itself is C.
"""
import json, math, os, subprocess, sys, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hwc_kicad import parse_sexpr  # noqa: E402

CORE = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT / 'build-local/copper-core')
DEMOS = os.environ.get('KICAD_DEMOS_DIR')
PROFILE = ROOT / 'fab_profiles/jlcpcb_2layer.json'
BOARDS = ['ecc83-pp.kicad_pcb', 'sonde xilinx.kicad_pcb', 'interf_u.kicad_pcb', 'complex_hierarchy.kicad_pcb',
          'pic_programmer.kicad_pcb', 'multichannel_mixer.kicad_pcb']
CU_ALL = (1 << 32) - 1


def layer_bit(name):
    if name in ('F.Cu',):
        return 1
    if name == 'B.Cu':
        return 1 << 31
    if name.startswith('In') and name.endswith('.Cu'):
        return 1 << int(name[2:-3])
    if name in ('*.Cu', 'F&B.Cu'):
        return CU_ALL if name == '*.Cu' else 1 | 1 << 31
    return 0


def seg_point(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def seg_seg(a, b, c, d):
    def orient(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    o1, o2, o3, o4 = orient(a, b, c), orient(a, b, d), orient(c, d, a), orient(c, d, b)
    if ((o1 > 0) != (o2 > 0)) and ((o3 > 0) != (o4 > 0)) and o1 and o2 and o3 and o4:
        return 0.0
    return min(seg_point(c, a, b), seg_point(d, a, b), seg_point(a, c, d), seg_point(b, c, d))


def seg_rect(a, b, r):   # r = (x1, y1, x2, y2)
    x1, y1, x2, y2 = r
    inside = lambda p: x1 <= p[0] <= x2 and y1 <= p[1] <= y2
    if inside(a) or inside(b):
        return 0.0
    corners = [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]
    return min(seg_seg(a, b, corners[i], corners[(i + 1) % 4]) for i in range(4))


class Shape:
    """A segment/rect core with a radius: capsule (seg), rounded rect (rect) or circle (seg of length 0)."""
    def __init__(self, kind, geom, radius):
        self.kind, self.geom, self.r = kind, geom, radius

    def box(self):
        if self.kind == 'seg':
            (ax, ay), (bx, by) = self.geom
            return (min(ax, bx) - self.r, min(ay, by) - self.r, max(ax, bx) + self.r, max(ay, by) + self.r)
        x1, y1, x2, y2 = self.geom
        return (x1 - self.r, y1 - self.r, x2 + self.r, y2 + self.r)


def distance(s, t):
    if s.kind == 'seg' and t.kind == 'seg':
        core = seg_seg(*s.geom, *t.geom)
    elif s.kind == 'seg':
        core = seg_rect(*s.geom, t.geom)
    elif t.kind == 'seg':
        core = seg_rect(*t.geom, s.geom)
    else:
        a, b = s.geom, t.geom
        dx = max(a[0] - b[2], b[0] - a[2], 0.0); dy = max(a[1] - b[3], b[1] - a[3], 0.0)
        core = math.hypot(dx, dy)
    return core - s.r - t.r


def pad_shape(p):
    ang = round(p['angle_udeg'] / 1e6) % 360 if p['angle_udeg'] % 90000000 == 0 else None
    if p['shape'] in ('custom', 'trapezoid', 'other') or (ang is None and p['shape'] != 'circle'):
        return None
    w, h = (p['size_nm'][1], p['size_nm'][0]) if ang in (90, 270) else tuple(p['size_nm'])
    x, y = p['x_nm'] / 1e6, p['y_nm'] / 1e6
    w, h = w / 1e6, h / 1e6
    if p['shape'] == 'circle':
        return Shape('seg', ((x, y), (x, y)), p['size_nm'][0] / 2e6)
    if p['shape'] == 'oval':
        if w >= h:
            return Shape('seg', ((x - (w - h) / 2, y), (x + (w - h) / 2, y)), h / 2)
        return Shape('seg', ((x, y - (h - w) / 2), (x, y + (h - w) / 2)), w / 2)
    r = 0.0
    if p['shape'] == 'roundrect':
        r = min(w, h) * p.get('rratio', 0.25)
    return Shape('rect', (x - w / 2 + r, y - h / 2 + r, x + w / 2 - r, y + h / 2 - r), r)


class Oracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DEMOS or not Path(DEMOS).is_dir():
            raise unittest.SkipTest("UNVERIFIED: set KICAD_DEMOS_DIR to a KiCad installation's demos folder")
        cls.limits = {k: v['value'] for k, v in json.loads(PROFILE.read_text(encoding='utf-8'))['limits'].items()}

    def limit(self, a, b):
        L = self.limits
        k = tuple(sorted((a['kind'], b['kind'])))
        if k in (('track', 'track'), ('track', 'via')):
            return L['track_spacing_min_mm'], 'track_spacing_min_mm'
        if k == ('smd', 'track'):
            return L['pad_to_track_min_mm'], 'pad_to_track_min_mm'
        if k == ('pth', 'track'):
            return L['pth_to_track_min_mm'], 'pth_to_track_min_mm'
        if k == ('smd', 'smd'):
            return L['smd_pad_to_pad_min_mm'], 'smd_pad_to_pad_min_mm'
        return L['clearance_default_min_mm'], 'clearance_default_min_mm'

    def items(self, path, model):
        tree = parse_sexpr(path.read_text(encoding='utf-8'))
        nets = {}
        for c in tree[1:]:
            if isinstance(c, list) and str(c[0]) == 'net':
                nets[str(c[1])] = str(c[2])
        out, rratio = [], {}
        # roundrect ratios from the file (the model JSON does not carry them)
        fi = 0
        for c in tree[1:]:
            if isinstance(c, list) and str(c[0]) == 'footprint':
                for p in c[1:]:
                    if isinstance(p, list) and str(p[0]) == 'pad':
                        rr = [x for x in p if isinstance(x, list) and str(x[0]) == 'roundrect_rratio']
                        rratio[fi] = float(rr[0][1]) if rr else 0.25
                        fi += 1
        ti = 0
        for c in tree[1:]:
            if not (isinstance(c, list) and str(c[0]) in ('segment', 'arc')):
                continue
            idx = ti; ti += 1
            if str(c[0]) == 'arc':
                continue
            g = {str(x[0]): x[1:] for x in c[1:] if isinstance(x, list)}
            net = g.get('net', [None])[0]
            name = nets.get(str(net), str(net)) if net is not None else None
            a = tuple(map(float, g['start'])); b = tuple(map(float, g['end']))
            out.append(dict(kind='track', shape=Shape('seg', (a, b), float(g['width'][0]) / 2), net=name,
                            layers=layer_bit(str(g['layer'][0])), ref=('track', idx), fp=None, num=None))
        pi = 0
        for f in model['footprints']:
            for p in f['pads']:
                idx = pi; pi += 1
                p = dict(p, rratio=rratio.get(idx, 0.25))
                if p['type'] == 'np_thru_hole':
                    continue        # copper-less here (none of these boards has an NPTH pad larger than its hole)
                s = pad_shape(p)
                if s is None:
                    continue
                kind = 'pth' if p['type'] == 'thru_hole' else 'smd'
                layers = CU_ALL if kind == 'pth' else p['layers'] & CU_ALL   # an SMD pad may sit on either side
                out.append(dict(kind=kind, shape=s, net=p['net'], layers=layers, ref=('pad', idx), fp=f['ref'], num=p['number']))
        for i, v in enumerate(model['vias']):
            if v['type'] != 'through':
                continue
            x, y = v['x_nm'] / 1e6, v['y_nm'] / 1e6
            out.append(dict(kind='via', shape=Shape('seg', ((x, y), (x, y)), v['size_nm'] / 2e6), net=v['net'],
                            layers=CU_ALL, ref=('via', i), fp=None, num=None))
        return out

    def test_violations_equal_brute_force(self):
        ran = 0
        for name in BOARDS:
            path = next(Path(DEMOS).rglob(name), None)
            if path is None:
                continue
            with self.subTest(board=name):
                model = json.loads(subprocess.run([CORE, 'board', str(path)], capture_output=True, text=True, encoding='utf-8').stdout)
                items = self.items(path, model)
                reach = max(self.limits[k] for k in ('track_spacing_min_mm', 'pad_to_track_min_mm', 'pth_to_track_min_mm',
                                                      'smd_pad_to_pad_min_mm', 'clearance_default_min_mm'))
                boxes = [it['shape'].box() for it in items]
                want, near = set(), set()
                order = sorted(range(len(items)), key=lambda i: boxes[i][0])
                for oi, i in enumerate(order):
                    a = items[i]
                    for j in order[oi + 1:]:
                        if boxes[j][0] > boxes[i][2] + reach:
                            break
                        b = items[j]
                        if boxes[j][1] > boxes[i][3] + reach or boxes[i][1] > boxes[j][3] + reach:
                            continue
                        if not (a['layers'] & b['layers']) or (a['net'] and a['net'] == b['net']):
                            continue
                        if a['fp'] and a['fp'] == b['fp'] and a['num'] and a['num'] == b['num']:
                            continue
                        lim, key = self.limit(a, b)
                        d = distance(a['shape'], b['shape'])
                        pair = frozenset((a['ref'], b['ref']))
                        if abs(d - lim) < 2e-6:
                            near.add(pair)
                        elif d < lim:
                            want.add(pair)
                report = json.loads(subprocess.run([CORE, 'dfm', str(path), str(PROFILE)], capture_output=True, text=True, encoding='utf-8').stdout)
                got = {frozenset((tuple(v['a']), tuple(v['b']))) for v in report['violations'] if v['rule'] == 'clearance'}
                got_exactly = {p for p in got if p not in near}
                exact_refs = {it['ref'] for it in items}
                got_exactly = {p for p in got_exactly if all(r in exact_refs for r in p)}
                self.assertEqual(sorted(map(sorted, got_exactly - want)), [], f'{name}: C reports pairs the oracle finds clear')
                self.assertEqual(sorted(map(sorted, want - got)), [], f'{name}: violations the C checker missed')
                print(f'  {name}: {len(want)} clearance violations agree with the brute-force oracle '
                      f'({len(items)} items, {len(near)} pairs within 2 nm of a limit left out)', file=sys.stderr)
                ran += 1
        if not ran:
            self.skipTest('UNVERIFIED: none of the oracle boards is in KICAD_DEMOS_DIR')


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
