#!/usr/bin/env python3
"""Prompt acceptance: the English corpus and the English -> KiCad path, classified honestly.

Usage: prompt_acceptance.py --pcba PATH [--out DIR] [--native --kicad-cli PATH] [--esp32]
                            [--symbols DIR] [--footprints DIR] [--only NAME,...]

Every step runs pcba with an argument list (no shell) and records the exact command, working directory, exit code,
stdout and stderr separately. Prompts are UTF-8 end to end: passed as arguments and, for non-ASCII prompts, also
through `spec parse --file` with a UTF-8 file. Outcomes:

  PASS             the check ran and met its expectation
  FAIL             the check ran and did not
  BLOCKED          an earlier step this check depends on did not succeed
  SKIPPED          not run on purpose (a reason is given), e.g. no KiCad 9.x for native checks
  UNVERIFIED       ran, but the product itself reports that it could not establish the result
  NOT_IMPLEMENTED  the product reports the capability does not exist
  OBSERVATIONAL    recorded for information (the ESP32 design run); never counted as a pass

Exit status: 0 only when every case is PASS, SKIPPED or OBSERVATIONAL; 1 when any case FAILs; 3 otherwise.
"""
from __future__ import annotations
import argparse, csv, json, math, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
OUTCOMES = ('PASS', 'FAIL', 'BLOCKED', 'SKIPPED', 'UNVERIFIED', 'NOT_IMPLEMENTED', 'OBSERVATIONAL')
STATUS_EXIT = {'PASS': 0, 'FAIL': 1, 'INVALID_INPUT': 2, 'UNVERIFIED': 3, 'NOT_IMPLEMENTED': 4, 'USAGE': 64, 'ERROR': 70}


class Runner:
    def __init__(self, pcba, out):
        self.pcba, self.out = str(pcba), Path(out); self.logs = self.out / 'logs'; self.logs.mkdir(parents=True, exist_ok=True)
        self.results = []

    def call(self, name, *args):
        """Run pcba ARGS --json; returns a step record with the parsed envelope (or None)."""
        argv = [self.pcba, *map(str, args), '--json']
        t0 = time.perf_counter()
        r = subprocess.run(argv, capture_output=True, cwd=str(ROOT), timeout=1800)
        seconds = time.perf_counter() - t0
        stdout = self.logs / f'{name}.stdout.json'; stderr = self.logs / f'{name}.stderr.log'
        stdout.write_bytes(r.stdout); stderr.write_bytes(r.stderr)
        try:
            doc = json.loads(r.stdout.decode('utf-8'))
        except (UnicodeDecodeError, ValueError):
            doc = None
        return dict(argv=argv, cwd=str(ROOT), exit=r.returncode, seconds=round(seconds, 3), stdout=str(stdout), stderr=str(stderr),
                    envelope=doc, status=doc['status'] if doc else 'NO_JSON')

    def record(self, name, group, expected, outcome, actual, details='', steps=()):
        assert outcome in OUTCOMES, outcome
        self.results.append(dict(name=name, group=group, expected=expected, outcome=outcome, actual=actual, details=details,
                                 steps=[{k: s[k] for k in ('argv', 'cwd', 'exit', 'seconds', 'stdout', 'stderr', 'status')} for s in steps]))


def expect_status(step, expected):
    """PASS when pcba's status and exit code both equal the expectation; FAIL otherwise (including no JSON)."""
    if step['status'] == 'NO_JSON':
        return 'FAIL', f'no JSON envelope (exit {step["exit"]}); see {step["stderr"]}'
    ok = step['status'] == expected and step['exit'] == STATUS_EXIT[expected]
    return ('PASS' if ok else 'FAIL'), f'{step["status"]}/{step["exit"]}'


def product_outcome(status):
    """A product status as an outcome where a non-PASS status is information, not a harness failure."""
    return {'PASS': 'PASS', 'FAIL': 'FAIL', 'UNVERIFIED': 'UNVERIFIED', 'NOT_IMPLEMENTED': 'NOT_IMPLEMENTED'}.get(status, 'FAIL')


def slug(name):
    return ''.join(c if c.isalnum() else '-' for c in name.lower()).strip('-')


def corpus(run, only):
    cases = json.loads((ROOT / 'tests/fixtures/spec/corpus.json').read_text(encoding='utf-8'))['cases']
    fingerprints = {}
    for case in cases:
        if only and case['name'] not in only:
            continue
        s = slug(case['name']); folder = run.out / 'corpus' / s; folder.mkdir(parents=True, exist_ok=True)
        prompt = folder / 'prompt.txt'; prompt.write_text(case['text'], encoding='utf-8')  # UTF-8, no BOM
        espec = folder / 'spec.espec'
        step = run.call(f'corpus_{s}', 'spec', 'parse', case['text'], '--out', espec, '--force')
        outcome, actual = expect_status(step, case['status'])
        detail = ''
        if outcome == 'PASS' and case['status'] == 'PASS' and not espec.is_file():
            outcome, detail = 'FAIL', 'PASS reported but no .espec was written'
        if step['envelope'] and case['status'] != 'PASS':
            detail = step['envelope']['message'][:300]
        run.record(f'prompt:{case["name"]}', 'english-corpus', f'{case["status"]}/{STATUS_EXIT[case["status"]]}', outcome, actual, detail, [step])
        if not case['text'].isascii():
            fstep = run.call(f'corpus_{s}_file', 'spec', 'parse', '--file', prompt)
            o, a = expect_status(fstep, case['status'])
            run.record(f'prompt-file:{case["name"]}', 'english-corpus-utf8-file', f'{case["status"]}/{STATUS_EXIT[case["status"]]}', o, a, 'prompt read from a UTF-8 file', [fstep])
        if case['status'] == 'PASS' and espec.is_file() and step['envelope']:
            fingerprints[case['name']] = step['envelope']['result']['data']['fingerprint']
            check = run.call(f'espec_check_{s}', 'spec', 'check', espec)
            same = check['envelope'] and check['envelope']['result']['data']['fingerprint'] == fingerprints[case['name']]
            run.record(f'espec-roundtrip:{case["name"]}', 'ir-roundtrip', 'PASS/0, same fingerprint',
                       'PASS' if check['status'] == 'PASS' and same else 'FAIL', f'{check["status"]}/{check["exit"]}; fingerprint match={bool(same)}', '', [check])
    a, b = fingerprints.get('divider'), fingerprints.get('divider reordered with aliases')
    if a and b:
        run.record('semantic-equivalence:divider', 'normalization', 'identical fingerprints', 'PASS' if a == b else 'FAIL', f'match={a == b}', f'{a} / {b}')
    return cases


def english_to_board(run, cases, args):
    """The primary product path: English -> .espec -> .copper -> DC -> KiCad project -> (native verify)."""
    divider = next(c for c in cases if c['name'] == 'divider')
    work = run.out / 'english-divider'; work.mkdir(parents=True, exist_ok=True)
    espec, project, build = work / 'divider.espec', work / 'divider.copper', work / 'build'
    parse = run.call('e2e_parse', 'spec', 'parse', divider['text'], '--out', espec, '--force')
    lower = run.call('e2e_lower', 'spec', 'lower', espec, '--out', project, '--force') if parse['status'] == 'PASS' else None
    if not lower or lower['status'] != 'PASS':
        run.record('e2e:lower', 'end-to-end', 'PASS/0 and a .copper file', 'FAIL', (lower or parse)['status'], '', [s for s in (parse, lower) if s])
        for name in ('e2e:dc', 'e2e:board', 'e2e:native'):
            run.record(name, 'end-to-end', '-', 'BLOCKED', '-', 'English -> .copper did not succeed')
        return
    run.record('e2e:lower', 'end-to-end', 'PASS/0 and a .copper file', 'PASS' if project.is_file() else 'FAIL', 'PASS/0', '', [parse, lower])
    # DC: the analytical divider, V(OUT) = 5 * 20k / (10k + 20k), compared with a tolerance.
    dc = run.call('e2e_dc', 'simulate', 'dc', project)
    oracle = 5.0 * 20000 / (10000 + 20000)
    report = dc['envelope']['result']['data']['report'] if dc['envelope'] and dc['envelope'].get('result') else None
    vout = next((n['voltage_v'] for n in (report or {}).get('nodes', []) if n['name'] == 'OUT'), None)
    ok = dc['status'] == 'PASS' and dc['exit'] == 0 and report and report['dc_solved'] and vout is not None and abs(vout - oracle) <= 1e-9 * oracle
    run.record('e2e:dc', 'end-to-end', f'PASS/0, dc_solved, V(OUT) = {oracle:.12g} V within 1e-9 relative', 'PASS' if ok else 'FAIL',
               f'{dc["status"]}/{dc["exit"]}; dc_solved={report and report["dc_solved"]}; V(OUT)={vout}', '', [dc])
    # Board: the build must produce a KiCad project whose board holds the designed content.
    cmd = ['project', 'build', project, '--out', build]
    if args.symbols:
        cmd += ['--symbols', args.symbols]
    if args.footprints:
        cmd += ['--footprints', args.footprints]
    b = run.call('e2e_build', *cmd)
    content = None
    if (build / 'design.kicad_pcb').is_file() and (build / 'resolved.copper').is_file():
        import copper
        content = copper.board_content(build)
    if content and content['status'] == 'PASS':
        outcome = 'PASS'
        actual = f'{b["status"]}/{b["exit"]}; {content["message"]}'
    else:
        outcome = 'FAIL'; actual = f'{b["status"]}/{b["exit"]}; ' + (content['message'] if content else 'no .kicad_pcb was written')
    run.record('e2e:board', 'end-to-end', 'a .kicad_pcb holding every designed footprint and pad net', outcome, actual,
               f'build status {b["status"]} is the product verdict (unknown ratings keep it UNVERIFIED); see {b["stderr"]}', [b])
    if not args.native:
        run.record('e2e:native', 'native', 'KiCad 9.x ERC/DRC/parity', 'SKIPPED', '-', 'not requested (--native)')
    elif outcome != 'PASS':
        run.record('e2e:native', 'native', 'KiCad 9.x ERC/DRC/parity', 'BLOCKED', '-', 'no populated board to check')
    else:
        v = run.call('e2e_verify', 'project', 'verify', '--out', build, *(['--kicad-cli', args.kicad_cli] if args.kicad_cli else []))
        ver = v['envelope']['result']['data']['verification'] if v['envelope'] and v['envelope'].get('result') and v['envelope']['result'].get('data') else None
        gates = {g['gate']: g for g in (ver or {}).get('gates', [])}
        if ver and (not ver.get('kicad_version') or not str(ver['kicad_version']).startswith('9.')):
            run.record('e2e:native', 'native', 'KiCad 9.x ERC/DRC/parity', 'SKIPPED', ver.get('kicad_version') or 'no kicad-cli',
                       next(iter(gates.values()), {}).get('message', ''), [v])
        else:
            summary = '; '.join(f"{k}={g['status']}" for k, g in gates.items())
            run.record('e2e:native', 'native', 'KiCad 9.x ERC/DRC/parity', product_outcome(ver['status']) if ver else 'FAIL',
                       f'{v["status"]}/{v["exit"]}; {summary}', 'UNVERIFIED here means KiCad or the data could not establish a result, not a pass', [v])


def esp32(run, args):
    out = run.out / 'esp32-led-design'
    cmd = ['design', 'Build me an ESP32 LED PCB. Generate the KiCad PCB.', '--out', out, '--profile', ROOT / 'profiles/esp32_led.design', '--force']
    for flag, value in (('--symbols', args.symbols), ('--footprints', args.footprints), ('--kicad-cli', args.kicad_cli)):
        if value:
            cmd += [flag, value]
    step = run.call('esp32_led_design', *cmd)
    gates = {}
    if (out / 'design.json').is_file():
        report = json.loads((out / 'design.json').read_text(encoding='utf-8'))
        gates = {k: v['status'] for k, v in sorted(report.get('gates', {}).items(), key=lambda kv: kv[1]['gate'])}
    run.record('observational:esp32-led-design', 'observational', 'recorded, not judged', 'OBSERVATIONAL',
               f'design status {step["status"]}/{step["exit"]}', '; '.join(f'{k}={v}' for k, v in gates.items()), [step])


def write(run):
    counts = {o: sum(1 for r in run.results if r['outcome'] == o) for o in OUTCOMES}
    (run.out / 'results.json').write_text(json.dumps(dict(counts=counts, results=run.results), indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    with (run.out / 'results.csv').open('w', newline='', encoding='utf-8') as f:
        w = csv.writer(f); w.writerow(['name', 'group', 'expected', 'outcome', 'actual', 'details'])
        for r in run.results:
            w.writerow([r['name'], r['group'], r['expected'], r['outcome'], r['actual'], r['details']])
    lines = ['# Prompt acceptance', '', ' '.join(f'**{o}** {n}' for o, n in counts.items() if n), '',
             '| Case | Outcome | Expected | Actual |', '|---|---|---|---|']
    lines += [f"| {r['name']} | {r['outcome']} | {r['expected']} | {str(r['actual']).replace('|', '/')} |" for r in run.results]
    (run.out / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return counts


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pcba', required=True); ap.add_argument('--out')
    ap.add_argument('--native', action='store_true'); ap.add_argument('--kicad-cli'); ap.add_argument('--esp32', action='store_true')
    ap.add_argument('--symbols'); ap.add_argument('--footprints'); ap.add_argument('--only', help='corpus case names, comma-separated')
    args = ap.parse_args(argv)
    out = Path(args.out or ROOT / 'out/prompt-acceptance' / time.strftime('%Y%m%d-%H%M%S'))
    run = Runner(Path(args.pcba).resolve(), out)
    cases = corpus(run, set(args.only.split(',')) if args.only else None)
    if not args.only:
        english_to_board(run, cases, args)
        if args.esp32:
            esp32(run, args)
    counts = write(run)
    for r in run.results:
        print(f"[{r['outcome']}] {r['name']}: {r['actual']}")
    print(' '.join(f'{o}={n}' for o, n in counts.items()) + f'   -> {out}')
    return 1 if counts['FAIL'] else 3 if counts['BLOCKED'] or counts['UNVERIFIED'] or counts['NOT_IMPLEMENTED'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
