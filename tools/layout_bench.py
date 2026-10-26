#!/usr/bin/env python3
"""Layout benchmark: the pre-optimizer baseline, the Manhattan-only optimizer and the DFM-aware optimizer
(Manhattan and octilinear candidates, per-net widths, clearance margin) on the layout fixtures.

Usage: layout_bench.py [--symbol-dir DIR] [--footprint-dir DIR] [--out FILE.json] [--markdown FILE.md]
                       [--seeds 1,2,3] [--iterations N] [--kicad-cli PATH] [--core PATH]

Every number comes from the same exact copper-core check. Fixtures whose footprints are not in the given
library are reported as UNVERIFIED, never skipped silently. With a KiCad 9.x CLI the chosen candidate is
also built and checked with KiCad DRC; otherwise that column is UNVERIFIED.
"""
from __future__ import annotations
import argparse, contextlib, io, json, platform, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import copper, layout

FIXTURES = [('divider', ROOT / 'examples/v2/divider.copper'), ('rc_network', ROOT / 'examples/v2/rc.copper'),
            ('multi_connector', ROOT / 'tests/fixtures/layout/multi_connector.copper'),
            ('tight_clearance', ROOT / 'tests/fixtures/layout/tight_clearance.copper'),
            ('edge_keepout', ROOT / 'tests/fixtures/layout/edge_keepout.copper'),
            ('high_current', ROOT / 'tests/fixtures/layout/high_current.copper'),
            ('mixed_current', ROOT / 'tests/fixtures/layout/mixed_current.copper'),
            ('unroutable', ROOT / 'tests/fixtures/layout/unroutable.copper')]
SNAPSHOT = ROOT / 'examples/generated/divider/libraries'


def kicad_drc(best, fixture, symdir, fpdir, cli, core):
    """Build the chosen candidate and run KiCad DRC (KiCad 9.x only; anything else is UNVERIFIED)."""
    version = ''
    if cli:
        try:
            version = subprocess.run([cli, 'version'], capture_output=True, text=True, timeout=60).stdout.strip()
        except OSError:
            version = ''
    if not version.startswith('9.'):
        return dict(status='UNVERIFIED', reason=f'KiCad 9.x CLI required (found {version or "none"})')
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / f'{fixture}.copper'; tree = best['resolved']
        tree[:] = [v for v in tree if not (isinstance(v, list) and str(v[0]) == 'geometry')]
        copper.write_tree(src, tree)
        args = argparse.Namespace(project=str(src), out=str(Path(d) / 'out'), core=core, symbol_dir=str(symdir), footprint_dir=str(fpdir), no_route=False, draft=True)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                copper.export_project(args)
        except (copper.Unverified, ValueError) as exc:
            return dict(status='UNVERIFIED' if isinstance(exc, copper.Unverified) else 'FAIL', reason=str(exc))
        r = subprocess.run([cli, 'pcb', 'drc', '--format', 'json', '--all-track-errors', '-o', str(Path(d) / 'drc.json'),
                            str(Path(d) / 'out/design.kicad_pcb')], capture_output=True, text=True, timeout=300)
        drc = json.loads((Path(d) / 'drc.json').read_text()) if (Path(d) / 'drc.json').is_file() else {}
        return dict(status='PASS' if r.returncode == 0 and not drc.get('violations') and not drc.get('unconnected_items') else 'FAIL',
                    kicad_version=version, violations=len(drc.get('violations', [])), unconnected=len(drc.get('unconnected_items', [])))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--symbol-dir'); ap.add_argument('--footprint-dir', default=str(SNAPSHOT / 'footprints'))
    ap.add_argument('--out'); ap.add_argument('--markdown'); ap.add_argument('--seeds', default='1,2,3')
    ap.add_argument('--iterations', type=int, default=1500); ap.add_argument('--kicad-cli'); ap.add_argument('--core')
    ap.add_argument('--only', help='comma-separated fixture names')
    args = ap.parse_args(argv)
    fpdir = Path(args.footprint_dir); symdir = Path(args.symbol_dir) if args.symbol_dir else None
    seeds = tuple(int(s) for s in args.seeds.split(',')); cli = copper.discover_cli(args.kicad_cli)
    results = []
    for name, path in FIXTURES:
        if args.only and name not in args.only.split(','):
            continue
        tree = copper.load(path); entry = dict(fixture=name, source=str(path.relative_to(ROOT)).replace('\\', '/'))
        try:
            try:
                base = layout.baseline(tree, fpdir, args.core)
            except ValueError as exc:       # an unplaced project has no pre-optimizer baseline
                if 'needs a placement' not in str(exc):
                    raise
                base = None
            manhattan, _, _ = layout.optimize(tree, fpdir, args.core, seeds, args.iterations, ('manhattan',))
            best, candidates, problem = layout.optimize(tree, fpdir, args.core, seeds, args.iterations)
        except FileNotFoundError as exc:
            entry.update(status='UNVERIFIED', reason=f'footprint {Path(exc.filename or "?").name} is not in {fpdir}'); results.append(entry); continue
        except (copper.Unverified, ValueError) as exc:
            entry.update(status='UNVERIFIED', reason=str(exc).splitlines()[0]); results.append(entry); continue
        entry.update(baseline=layout.brief(base) if base else None, manhattan=layout.brief(manhattan), optimized=layout.brief(best), candidates=[layout.brief(c) for c in candidates],
                     placement_failures=problem['placement_failures'],
                     status='FEASIBLE' if best and best['feasible'] else 'NO_FEASIBLE_BOARD',
                     kicad_drc=kicad_drc(best, name, symdir, fpdir, cli, args.core) if best and best['feasible'] and symdir else
                     dict(status='UNVERIFIED', reason='no feasible candidate' if not (best and best['feasible']) else 'no --symbol-dir given'))
        results.append(entry)
    doc = dict(tool='layout_bench', python=platform.python_version(), footprints=str(fpdir), seeds=list(seeds),
               iterations=args.iterations, weights=layout.WEIGHTS, results=results)
    if args.out:
        Path(args.out).write_text(json.dumps(doc, indent=2) + '\n', encoding='utf-8')
    table = markdown(doc)
    if args.markdown:
        Path(args.markdown).write_text(table, encoding='utf-8')
    print(table)
    return 0


def markdown(doc):
    rows = ['| Fixture | Run | Style | Feasible | Hard (by rule) | Unrouted | Length mm | Vias | Bends | Widths mm | Margin mm | Near limit | Explored | Soft score |',
            '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for r in doc['results']:
        if r.get('status') == 'UNVERIFIED' and 'baseline' not in r:
            rows.append(f"| {r['fixture']} | — | | UNVERIFIED | {r['reason']} | | | | | | | | | |"); continue
        for run in ('baseline', 'manhattan', 'optimized'):
            c = r[run]
            if c is None:
                rows.append(f"| {r['fixture']} | {run} | | — | the project has no placement of its own | | | | | | | | | |"); continue
            hard = ', '.join(f'{k} {v}' for k, v in c['hard_by_rule'].items()) or '0'
            widths = ', '.join(f'{n} {w:.2f}' for n, w in c['widths'].items()) or f"{c['trace_width_mm']:.2f}"
            margin = '—' if c['clearance_margin_mm'] is None else f"{c['clearance_margin_mm']:.3f}"
            rows.append(f"| {r['fixture']} | {run} | {c['style']} | {'yes' if c['feasible'] else 'NO'} | {hard} | {c['unrouted']} | {c['length_mm']:.2f} | "
                        f"{c['vias']} | {c['bends']} | {widths} | {margin} | {c['near_limit_pairs']} | {c['explored']} | {c['soft_score']:.2f} |")
    return '\n'.join(rows) + '\n'


if __name__ == '__main__':
    raise SystemExit(main())
