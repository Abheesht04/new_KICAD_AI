#!/usr/bin/env python3
"""Numerical baseline of the compiler (docs/engineering/NUMERICAL_ROADMAP.md).

Usage: numeric_bench.py [--bench PATH] [--core PATH] [--quick] [--out FILE.json]

Runs the C kernel benchmark (numeric_bench: dense solve, scaling, degenerate systems, diode Newton against a
Lambert-W oracle) and project-level analyses through copper-core against closed-form answers: RC transient
convergence order, series LC amplitude, a stiff two-time-constant RC, RC AC response and tolerance-corner
cost. ngspice is used as an extra differential reference only when installed; numpy only for condition
numbers. Nothing here uses the implementation under test as its own oracle.
"""
from __future__ import annotations
import argparse, copy, csv, io, json, math, shutil, subprocess, sys, tempfile, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import copper
from copper import sx, A, Q, number

PARTS = {str(p[1]): p for p in copper.catalogue()}


def project(nets, comps, rules=None):
    """A .copper tree from (ref, part, value, [(pin, net)]) using the bundled generic parts."""
    used = sorted({c[1] for c in comps})
    nodes = []
    for ref, part, value, pins in comps:
        n = sx('component', A(ref), sx('part', A(part)))
        if value is not None:
            n.append(sx('value', number(value)))
        n.append(sx('pins', *[sx(p, A(net)) for p, net in pins])); nodes.append(n)
    tree = sx('copper_project', sx('version', A('2')), sx('name', Q('bench')), sx('nets', *map(A, nets)),
              sx('board', *map(number, [40, 30, 1.6, .035, 2])), sx('parts', *[copy.deepcopy(PARTS[p]) for p in used]), sx('components', *nodes))
    return tree


def run_core(core, tree, mode, *extra):
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / 'p.copper'; copper.write_tree(path, tree)
        t0 = time.perf_counter(); r = subprocess.run([core, mode, str(path), *map(str, extra)], capture_output=True, text=True, timeout=600)
        return r, time.perf_counter() - t0


def waveform(core, tree, dt, steps, method):
    r, _ = run_core(core, tree, 'transient', repr(dt), steps, method)
    if r.returncode:
        raise RuntimeError(r.stderr.strip())
    rows = list(csv.reader(io.StringIO(r.stdout))); head = rows[0]
    return [dict(zip(head, map(float, row))) for row in rows[1:]]


def rc_order(core, quick):
    """V = 1 V step into R = 1 kOhm, C = 1 uF (tau = 1 ms); oracle 1 - exp(-t / tau) up to t = 5 ms."""
    tree = project(['GND', 'VIN', 'OUT'], [('V1', 'voltage', 1, [('1', 'VIN'), ('2', 'GND')]),
                                           ('R1', 'resistor', 1000, [('1', 'VIN'), ('2', 'OUT')]),
                                           ('C1', 'capacitor', 1e-6, [('1', 'OUT'), ('2', 'GND')])])
    out = {}
    for method in ('be', 'trap'):
        rows = []
        for dt in ((1e-4, 5e-5) if quick else (2e-4, 1e-4, 5e-5, 2.5e-5)):
            w = waveform(core, tree, dt, round(5e-3 / dt), method)
            err = max(abs(s['OUT_V'] - (1 - math.exp(-s['time_s'] / 1e-3))) for s in w)
            rows.append(dict(dt_s=dt, max_error_v=err))
        for a, b in zip(rows, rows[1:]):
            b['observed_order'] = math.log(a['max_error_v'] / b['max_error_v']) / math.log(a['dt_s'] / b['dt_s'])
        out[method] = rows
    return out


def lc_amplitude(core, quick):
    """1 V step into series L = 1 mH, C = 1 uF, no resistance: V(C) = 1 - cos(t / sqrt(LC)), peak 2 V forever."""
    tree = project(['GND', 'VIN', 'MID', 'OUT'], [('V1', 'voltage', 1, [('1', 'VIN'), ('2', 'GND')]),
                                                  ('L1', 'inductor', 1e-3, [('1', 'VIN'), ('2', 'MID')]),
                                                  ('R1', 'resistor', 1e-6, [('1', 'MID'), ('2', 'OUT')]),
                                                  ('C1', 'capacitor', 1e-6, [('1', 'OUT'), ('2', 'GND')])])
    period = 2 * math.pi * math.sqrt(1e-3 * 1e-6); dt = period / 100; periods = 5 if quick else 20
    out = {}
    for method in ('be', 'trap'):
        w = waveform(core, tree, dt, periods * 100, method)
        last = [s['OUT_V'] for s in w if s['time_s'] > (periods - 1) * period]
        out[method] = dict(dt_s=dt, periods=periods, last_period_peak_v=max(last), oracle_peak_v=2.0,
                           peak_error_fraction=(max(last) - 2.0) / 2.0)
    return out


def stiff(core):
    """Two RCs from one 1 V source: tau 1 us and 1 s, dt = 100 us. The fast node is at its final value after one
    step in reality; a non-L-stable method can ring there."""
    tree = project(['GND', 'VIN', 'FAST', 'SLOW'], [('V1', 'voltage', 1, [('1', 'VIN'), ('2', 'GND')]),
                                                    ('R1', 'resistor', 1, [('1', 'VIN'), ('2', 'FAST')]),
                                                    ('C1', 'capacitor', 1e-6, [('1', 'FAST'), ('2', 'GND')]),
                                                    ('R2', 'resistor', 1e6, [('1', 'VIN'), ('2', 'SLOW')]),
                                                    ('C2', 'capacitor', 1e-6, [('1', 'SLOW'), ('2', 'GND')])])
    out = {}
    for method in ('be', 'trap'):
        w = waveform(core, tree, 1e-4, 20, method)
        fast = [s['FAST_V'] - (1 - math.exp(-s['time_s'] / 1e-6)) for s in w]
        slow = [abs(s['SLOW_V'] - (1 - math.exp(-s['time_s'] / 1.0))) for s in w]
        out[method] = dict(dt_s=1e-4, fast_tau_s=1e-6, slow_tau_s=1.0, fast_max_error_v=max(map(abs, fast)),
                           fast_sign_changes=sum(1 for a, b in zip(fast, fast[1:]) if a * b < 0), slow_max_error_v=max(slow))
    return out


def ac_rc(core):
    """RC low-pass, R = 1 kOhm, C = 1 uF: H = 1 / (1 + j w R C)."""
    tree = project(['GND', 'VIN', 'OUT'], [('V1', 'voltage', 0, [('1', 'VIN'), ('2', 'GND')]),
                                           ('R1', 'resistor', 1000, [('1', 'VIN'), ('2', 'OUT')]),
                                           ('C1', 'capacitor', 1e-6, [('1', 'OUT'), ('2', 'GND')])])
    v = next(c for c in copper.direct(copper.one(tree, 'components'), 'component') if str(c[1]) == 'V1'); v.append(sx('ac', A('1'), A('0')))
    r, seconds = run_core(core, tree, 'ac', 1, 1e5, 41, 'log')
    rows = list(csv.DictReader(io.StringIO(r.stdout)))
    mag = ph = 0.0
    for row in rows:
        f = float(row['frequency_hz']); h = 1 / complex(1, 2 * math.pi * f * 1e-3)
        mag = max(mag, abs(float(row['V(OUT)_mag']) - abs(h))); ph = max(ph, abs(float(row['V(OUT)_phase_deg']) - math.degrees(math.atan2(h.imag, h.real))))
    return dict(points=len(rows), max_magnitude_error=mag, max_phase_error_deg=ph, seconds=seconds)


def corners(core, quick):
    """k resistors in series from 1 V with 1 % tolerance: 2^k corners are solved exhaustively; k > 8 is refused."""
    out = []
    for k in (range(1, 6) if quick else range(1, 10)):
        chain = ['VIN'] + [f'N{i}' for i in range(1, k)] + ['GND']
        comps = [('V1', 'voltage', 1, [('1', 'VIN'), ('2', 'GND')])]
        comps += [(f'R{i + 1}', 'resistor', 1000, [('1', chain[i]), ('2', chain[i + 1])]) for i in range(k)]
        tree = copy.deepcopy(project(['GND', 'VIN'] + chain[1:-1], comps))
        for p in copper.direct(copper.one(tree, 'parts'), 'part'):
            if str(p[1]) == 'resistor':
                p.append(sx('tolerance', A('0.01')))
        r, seconds = run_core(core, tree, 'analyze')
        rep = json.loads(r.stdout)
        out.append(dict(resistors=k, corners=rep['tolerance_corner_count'], refused='corner_capacity' in [i['rule'] for i in rep['issues']], seconds=seconds))
    return out


def conditioning(quick):
    """2-norm condition number of the ladder MNA matrix (numpy only; optional)."""
    try:
        import numpy as np
    except ImportError:
        return dict(status='UNVERIFIED', reason='numpy not installed')
    rows = []
    for n in ((10, 100) if quick else (10, 100, 400, 1600)):
        m = np.zeros((n + 1, n + 1)); g = 1e-3
        for k in range(n):  # resistor k: node k+1 to k+2 (last to ground); node indices 0..n-1
            a, b = k, k + 1 if k + 1 < n else None
            m[a, a] += g
            if b is not None:
                m[b, b] += g; m[a, b] -= g; m[b, a] -= g
        m[0, n] = m[n, 0] = 1.0
        rows.append(dict(nodes=n, condition_2norm=float(np.linalg.cond(m))))
    return dict(status='PASS', rows=rows)


def ngspice_check(quick):
    exe = shutil.which('ngspice')
    if not exe:
        return dict(status='UNVERIFIED', reason='ngspice not installed; differential checks against it were not run')
    return dict(status='UNVERIFIED', reason=f'ngspice found at {exe}; differential comparison is planned, not implemented in this baseline')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bench'); ap.add_argument('--core'); ap.add_argument('--quick', action='store_true'); ap.add_argument('--out')
    args = ap.parse_args(argv)
    core = copper.core_path(args.core)
    bench = args.bench or str(Path(core).with_name('numeric_bench' + Path(core).suffix))
    kernels = json.loads(subprocess.run([bench] + (['--quick'] if args.quick else []), capture_output=True, text=True, timeout=1200, check=True).stdout)
    doc = dict(tool='numeric_bench.py', kernels=kernels, transient_rc=rc_order(core, args.quick), transient_lc=lc_amplitude(core, args.quick),
               transient_stiff=stiff(core), ac_rc=ac_rc(core), tolerance_corners=corners(core, args.quick),
               conditioning=conditioning(args.quick), ngspice=ngspice_check(args.quick))
    text = json.dumps(doc, indent=2)
    if args.out:
        Path(args.out).write_text(text + '\n', encoding='utf-8')
    print(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
