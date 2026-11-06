"""KiCad environment detection, library tables and the library index (docs/engineering/KICAD_LIBRARY.md).

Usage: test_kicad_library.py PCBA [unittest args]
Exit status: 0 all ran and passed, 1 failure.

Installs are fake folder trees, so policy and table handling are tested without KiCad. The index's selective reader
is compared field by field with a full-tree reading (below) on the committed KiCad 9 library snapshot, on synthetic
edge cases, and on every library under KICAD_SYMBOL_DIR / KICAD_FOOTPRINT_DIR when those are set.
"""
import json, os, re, sqlite3, subprocess, sys, tempfile, time, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
PCBA = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None
sys.path.insert(0, str(ROOT / 'tools'))
import kicad_library as kl
from hwc_kicad import SExprError, direct, head, one, parse_sexpr
SNAP = ROOT / 'examples/generated/divider/libraries'


# ------------------------------------------------------------------ oracle: the same fields from the full tree
def text(node, key):
    found = one(node, key)
    return str(found[1]) if found is not None and len(found) > 1 else ''


def symbol_pins(sym):
    pins = {}
    for unit in [sym] + direct(sym, 'symbol'):
        m = re.search(r'_(\d+)_(\d+)$', str(unit[1])) if unit is not sym else None
        for pin in direct(unit, 'pin'):
            pins[(text(pin, 'number'), text(pin, 'name'), str(pin[1]) if len(pin) > 1 else '', int(m.group(1)) if m else 0)] = None
    return [dict(zip(('number', 'name', 'type', 'unit'), p)) for p in pins]


def oracle_symbols(nickname, path):
    root = parse_sexpr(Path(path).read_text(encoding='utf-8'))
    if head(root) != 'kicad_symbol_lib':
        raise SExprError('not a symbol library')
    raw = {str(s[1]): s for s in direct(root, 'symbol') if len(s) > 1}
    rows = []
    for name, sym in raw.items():
        chain, seen = [sym], {name}
        while text(chain[-1], 'extends') in raw and text(chain[-1], 'extends') not in seen:
            seen.add(text(chain[-1], 'extends')); chain.append(raw[text(chain[-1], 'extends')])
        props = {}
        for s in reversed(chain):
            props.update({str(p[1]): str(p[2]) for p in direct(s, 'property') if len(p) > 2})
        pins = next((p for p in map(symbol_pins, chain) if p), [])
        numbers = kl.natural({p['number'] for p in pins if p['number']})
        rows.append({'id': f'{nickname}:{name}', 'library': nickname, 'name': name, 'extends': text(sym, 'extends'),
                     'reference': props.get('Reference', ''), 'value': props.get('Value', ''),
                     'description': props.get('Description') or props.get('ki_description', ''),
                     'keywords': props.get('ki_keywords', ''), 'fp_filters': props.get('ki_fp_filters', '').split(),
                     'footprint': props.get('Footprint', ''), 'power': any(one(s, 'power') is not None for s in chain),
                     'units': max([p['unit'] for p in pins] + [1]), 'pins': pins,
                     'pin_key': '\x1f'.join(sorted(numbers)), 'pin_count': len(numbers)})
    return rows, text(root, 'version')


def oracle_footprints(nickname, path):
    rows, versions = [], set()
    for file in sorted(Path(path).glob('*.kicad_mod')):
        fp = parse_sexpr(file.read_text(encoding='utf-8'))
        versions.add(text(fp, 'version'))
        pads = [{'number': str(p[1]), 'type': str(p[2])} for p in direct(fp, 'pad') if len(p) > 2]
        numbers = kl.natural({p['number'] for p in pads if p['number'] and p['type'] != 'np_thru_hole'})
        attr = one(fp, 'attr')
        rows.append({'id': f'{nickname}:{file.stem}', 'library': nickname, 'name': file.stem, 'description': text(fp, 'descr'),
                     'tags': text(fp, 'tags'), 'attributes': [str(a) for a in attr[1:]] if attr is not None else [],
                     'pads': pads, 'pad_key': '\x1f'.join(sorted(numbers)), 'pad_count': len(numbers),
                     'courtyard': kl.courtyard([x for x in fp if isinstance(x, list) and head(x) in kl.GRAPHICS]),
                     'models': len(direct(fp, 'model'))})
    return rows, ','.join(sorted(versions - {''}))


EDGE_SYMBOLS = r'''(kicad_symbol_lib (version 20241209) (generator "test")
  ; a comment with (pin "99") inside
  (symbol "Base" (pin_names (offset 1)) (exclude_from_sim no)
    (property "Reference" "U" (at 0 0 0) (effects (font (size 1.27 1.27))))
    (property "Description" "has (pin \"7\") and \"quotes\" and a\nnewline" (at 0 0 0))
    (property "ki_keywords" "amp (op)" (at 0 0 0))
    (property "ki_fp_filters" "SOIC*3.9x4.9mm* DIP*W7.62mm*" (at 0 0 0))
    (symbol "Base_0_1" (rectangle (start -5 5) (end 5 -5) (stroke (width 0.254) (type default)) (fill (type background))))
    (symbol "Base_1_1" (pin input line (at -7.62 0 0) (length 2.54) (name "+" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27))))))
    (symbol "Base_2_1" (pin output line (at 7.62 0 180) (length 2.54) (name "OUT") (number "1")
                         (alternate "ALT" bidirectional line)))
    (symbol "Base_1_2" (pin input line (at -7.62 0 0) (length 2.54) (name "+") (number "3"))))
  (symbol "Derived" (extends "Base") (property "Reference" "U" (at 0 0 0)) (property "Description" "derived" (at 0 0 0)))
  (symbol "GND" (power) (property "Reference" "#PWR" (at 0 0 0))
    (symbol "GND_0_1" (pin power_in line (at 0 0 270) (length 0) hide (name "GND") (number "1"))))
  (symbol "NoPins" (property "Reference" "X" (at 0 0 0)) (property "Value" "") ))
'''
EDGE_FOOTPRINT = r'''(footprint "Edge" (version 20241229) (generator "test") (layer "F.Cu")
  (descr "a (pad \"9\") inside text") (tags "edge test")
  (attr smd exclude_from_bom)
  (fp_line (start -2 -1) (end 2 -1) (stroke (width 0.05) (type solid)) (layer "F.CrtYd"))
  (fp_circle (center 0 0) (end 3 0) (stroke (width 0.05) (type solid)) (layer "B.CrtYd"))
  (fp_poly (pts (xy -1 4) (xy 1 4) (xy 0 5)) (stroke (width 0.05) (type solid)) (layer "F.CrtYd"))
  (fp_line (start -9 -9) (end 9 9) (stroke (width 0.1) (type solid)) (layer "F.SilkS"))
  (pad "1" smd roundrect (at -1 0) (size 1 1) (layers "F.Cu" "F.Paste" "F.Mask"))
  (pad "2" smd roundrect (at 1 0) (size 1 1) (layers "F.Cu" "F.Paste" "F.Mask"))
  (pad "" np_thru_hole circle (at 0 2) (size 1 1) (drill 1) (layers "*.Cu" "*.Mask"))
  (pad "MP" thru_hole circle (at 0 -2) (size 2 2) (drill 1) (layers "*.Cu" "*.Mask"))
  (model "${KICAD9_3DMODEL_DIR}/x.wrl" (offset (xyz 0 0 0))))
'''


def write(path, content):
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True); path.write_text(content, encoding='utf-8')
    return path


def fake_install(base, version, cli=True):
    """A KiCad folder tree: libraries from the committed snapshot, an executable placeholder (never run unprobed)."""
    root = Path(base) / version
    share = root / 'share' / 'kicad'
    for sub in ('symbols', 'footprints', 'template', '3dmodels'):
        (share / sub).mkdir(parents=True, exist_ok=True)
    for f in (SNAP / 'symbols').glob('*.kicad_sym'):
        write(share / 'symbols' / f.name, f.read_text(encoding='utf-8'))
    for d in (SNAP / 'footprints').glob('*.pretty'):
        for f in d.glob('*.kicad_mod'):
            write(share / 'footprints' / d.name / f.name, f.read_text(encoding='utf-8'))
    if cli:
        write(root / 'bin' / ('kicad-cli.exe' if os.name == 'nt' else 'kicad-cli'), '')
    major = version.split('.')[0]
    write(share / 'template' / 'sym-lib-table', '(sym_lib_table (version 7)\n'
          f'  (lib (name "Device") (type "KiCad") (uri "${{KICAD{major}_SYMBOL_DIR}}/Device.kicad_sym") (options "") (descr "Devices"))\n'
          f'  (lib (name "Connector_Generic") (type "KiCad") (uri "${{KICAD{major}_SYMBOL_DIR}}/Connector_Generic.kicad_sym") (options "") (descr ""))\n)\n')
    write(share / 'template' / 'fp-lib-table', '(fp_lib_table (version 7)\n'
          f'  (lib (name "Resistor_SMD") (type "KiCad") (uri "${{KICAD{major}_FOOTPRINT_DIR}}/Resistor_SMD.pretty") (options "") (descr ""))\n'
          f'  (lib (name "Connector_PinHeader_2.54mm") (type "KiCad") (uri "${{KICAD{major}_FOOTPRINT_DIR}}/Connector_PinHeader_2.54mm.pretty") (options "") (descr ""))\n)\n')
    return root


class Reader(unittest.TestCase):
    def libraries(self):
        found = [('symbol', p) for p in sorted((SNAP / 'symbols').glob('*.kicad_sym'))]
        found += [('footprint', p) for p in sorted((SNAP / 'footprints').glob('*.pretty'))]
        for env, kind in (('KICAD_SYMBOL_DIR', 'symbol'), ('KICAD_FOOTPRINT_DIR', 'footprint')):
            if os.environ.get(env):
                found += [(kind, Path(p)) for _, p in kl.directory_libraries(os.environ[env], kind)]
        return found

    def test_same_fields_as_the_full_tree(self):
        libs = self.libraries(); self.assertGreaterEqual(len(libs), 4)
        for kind, path in libs:
            oracle = oracle_symbols if kind == 'symbol' else oracle_footprints
            fast = kl.parse_symbols if kind == 'symbol' else kl.parse_footprints
            self.assertEqual(fast('L', path), oracle('L', path), str(path))

    def test_edge_cases(self):
        with tempfile.TemporaryDirectory() as d:
            sym = write(Path(d) / 'Edge.kicad_sym', EDGE_SYMBOLS)
            write(Path(d) / 'Edge.pretty' / 'Edge.kicad_mod', EDGE_FOOTPRINT)
            self.assertEqual(kl.parse_symbols('E', sym), oracle_symbols('E', sym))
            self.assertEqual(kl.parse_footprints('E', Path(d) / 'Edge.pretty'), oracle_footprints('E', Path(d) / 'Edge.pretty'))
            rows = {r['name']: r for r in kl.parse_symbols('E', sym)[0]}
            self.assertEqual(rows['Base']['description'], 'has (pin "7") and "quotes" and a\nnewline')
            self.assertEqual([(p['number'], p['unit']) for p in rows['Base']['pins']], [('3', 1), ('1', 2)])
            self.assertEqual((rows['Base']['units'], rows['Base']['pin_count']), (2, 2))
            self.assertEqual(rows['Derived']['pins'], rows['Base']['pins']); self.assertEqual(rows['Derived']['description'], 'derived')
            self.assertEqual(rows['Derived']['fp_filters'], ['SOIC*3.9x4.9mm*', 'DIP*W7.62mm*'])
            self.assertTrue(rows['GND']['power']); self.assertEqual(rows['NoPins']['pin_key'], '')
            fp = kl.parse_footprints('E', Path(d) / 'Edge.pretty')[0][0]
            self.assertEqual(fp['courtyard'], [-3.0, -3.0, 3.0, 5.0]); self.assertEqual(fp['attributes'], ['smd', 'exclude_from_bom'])
            self.assertEqual(fp['pad_key'].split('\x1f'), ['1', '2', 'MP'], 'the unnumbered mechanical hole is not a pad')
            self.assertEqual(fp['description'], 'a (pad "9") inside text')

    def test_malformed_libraries_are_errors_not_rows(self):
        with tempfile.TemporaryDirectory() as d:
            for name, content in (('Open', '(kicad_symbol_lib (symbol "A" (pin input line'), ('Quote', '(kicad_symbol_lib (symbol "A))'),
                                  ('Other', '(footprint "x")')):
                rows, _, error = kl.parse_library('symbol', name, write(Path(d) / f'{name}.kicad_sym', content))
                self.assertEqual(rows, []); self.assertTrue(error, name)


class Installs(unittest.TestCase):
    def test_policy_selects_the_highest_9x_only(self):
        with tempfile.TemporaryDirectory() as d:
            roots = [fake_install(d, v) for v in ('8.0', '9.0', '9.1', '10.0')] + [fake_install(Path(d) / 'nocli', '9.9', cli=False)]
            installs = kl.find_installs({}, roots, probe=False)
            self.assertEqual([i['version'] for i in installs], ['8.0', '9.0', '9.1', '10.0', '9.9'])
            self.assertTrue(all(i['version_source'] == 'folder name' for i in installs))
            chosen, reason = kl.select(installs)
            self.assertEqual(chosen['version'], '9.1', reason); self.assertIn('highest 9.x', reason)
            chosen, reason = kl.select([i for i in installs if not i['version'].startswith('9.')])
            self.assertIsNone(chosen); self.assertIn('10.0 at', reason); self.assertIn('no KiCad 9.x', reason)
            self.assertIsNone(kl.select([])[0])
            # Library and CLI lookups (hwc_kicad.discover_*) try 9.x first, then the others, newest first.
            self.assertEqual([i['version'] for i in kl.ordered(installs)], ['9.9', '9.1', '9.0', '10.0', '8.0'])

    def test_a_probed_version_ranks_by_patch_level(self):
        a = {'root': 'a', 'version': '9.0.2', 'version_source': 'kicad-cli version', 'cli': 'x', 'symbols': 's', 'footprints': 'f'}
        b = dict(a, root='b', version='9.0.6')
        self.assertEqual(kl.select([b, a])[0]['root'], 'b'); self.assertEqual(kl.select([a, b])[0]['root'], 'b')

    def test_real_cli_is_probed(self):
        cli = os.environ.get('KICAD_CLI')
        if not cli:
            self.skipTest('set KICAD_CLI to probe a real kicad-cli')
        installs = kl.find_installs({'KICAD_CLI': cli, 'PATH': ''}, None)
        mine = [i for i in installs if i['cli'] and Path(i['cli']).resolve() == Path(cli).resolve()]
        self.assertTrue(mine and mine[0]['version_source'] == 'kicad-cli version', installs)


class Tables(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.dir = Path(self.tmp.name)
        self.install = kl.find_installs({}, [fake_install(self.dir, '9.0')], probe=False)[0]
        self.env = {'KICAD_CONFIG_HOME': str(self.dir / 'config')}

    def tearDown(self):
        self.tmp.cleanup()

    def test_template_until_kicad_writes_the_user_table(self):
        table = kl.library_table('symbol', self.install, self.env)
        self.assertEqual([t['scope'] for t in table['tables']], ['global (install template)'])
        self.assertEqual({e['name']: e['status'] for e in table['entries']}, {'Device': 'ok', 'Connector_Generic': 'ok'})
        write(self.dir / 'config/9.0/sym-lib-table', '(sym_lib_table (version 7) (lib (name "Device") (type "KiCad") (uri "${KICAD9_SYMBOL_DIR}/Device.kicad_sym") (options "") (descr "")))')
        table = kl.library_table('symbol', self.install, self.env)
        self.assertEqual([t['scope'] for t in table['tables']], ['global']); self.assertEqual([e['name'] for e in table['entries']], ['Device'])

    def test_variables_project_overrides_and_statuses(self):
        user = self.dir / 'user_libs'; write(user / 'Mine.kicad_sym', '(kicad_symbol_lib (version 20241209))')
        write(self.dir / 'project' / 'local.kicad_sym', '(kicad_symbol_lib (version 20241209))')
        write(self.dir / 'config/9.0/kicad_common.json', json.dumps({'environment': {'vars': {'MY_LIBS': str(user), 'KICAD9_SYMBOL_DIR': str(self.dir / 'wrong')}}}))
        write(self.dir / 'config/9.0/sym-lib-table', '''(sym_lib_table (version 7)
          (lib (name "Device") (type "KiCad") (uri "${KICAD9_SYMBOL_DIR}/Device.kicad_sym") (options "") (descr ""))
          (lib (name "Mine") (type "KiCad") (uri "${MY_LIBS}/Mine.kicad_sym") (options "") (descr ""))
          (lib (name "Gone") (type "KiCad") (uri "${UNDEFINED_VAR}/x.kicad_sym") (options "") (descr ""))
          (lib (name "Absent") (type "KiCad") (uri "/no/such/file.kicad_sym") (options "") (descr ""))
          (lib (name "Off") (type "KiCad") (uri "${MY_LIBS}/Mine.kicad_sym") (options "") (descr "") (disabled))
          (lib (name "Eagle") (type "Eagle") (uri "${MY_LIBS}/x.lbr") (options "") (descr ""))
          (lib (name "Mine") (type "KiCad") (uri "${MY_LIBS}/Mine.kicad_sym") (options "") (descr ""))
          (lib (name "Shadowed") (type "KiCad") (uri "${MY_LIBS}/Mine.kicad_sym") (options "") (descr "")))''')
        write(self.dir / 'project' / 'sym-lib-table', '(sym_lib_table (version 7) (lib (name "Shadowed") (type "KiCad") (uri "${KIPRJMOD}/local.kicad_sym") (options "") (descr "")))')
        env = dict(self.env, KICAD9_SYMBOL_DIR=str(self.install['symbols']))   # the process environment wins
        table = kl.library_table('symbol', self.install, env, self.dir / 'project')
        status = [(e['scope'], e['name'], e['status']) for e in table['entries']]
        self.assertEqual(status, [('global', 'Device', 'ok'), ('global', 'Mine', 'ok'), ('global', 'Gone', 'unresolved'),
                                  ('global', 'Absent', 'missing'), ('global', 'Off', 'disabled'), ('global', 'Eagle', 'unsupported_type'),
                                  ('global', 'Mine', 'duplicate'), ('global', 'Shadowed', 'overridden'), ('project', 'Shadowed', 'ok')])
        gone = next(e for e in table['entries'] if e['name'] == 'Gone'); self.assertEqual(gone['unresolved'], ['UNDEFINED_VAR'])
        shadowed = [e for e in table['entries'] if e['name'] == 'Shadowed' and e['status'] == 'ok'][0]
        self.assertEqual(Path(shadowed['path']), (self.dir / 'project' / 'local.kicad_sym').resolve())
        found = kl.variables(self.install, env, self.dir / 'project')
        self.assertEqual(found['KICAD9_SYMBOL_DIR'][1], 'environment'); self.assertEqual(found['MY_LIBS'][1], 'kicad_common.json')
        self.assertEqual(found['KICAD9_FOOTPRINT_DIR'][1], 'install'); self.assertEqual(found['KIPRJMOD'][1], 'project')
        snap = {'symbol_libraries': table}
        self.assertEqual([n for n, _ in kl.table_libraries(snap, 'symbol')], ['Device', 'Mine', 'Shadowed'])

    def test_snapshot_records_policy_selection_and_tables(self):
        snap = kl.environment(self.env, [self.install['root'], fake_install(self.dir, '10.0')])
        json.dumps(snap)
        self.assertEqual(snap['selected']['version'], '9.0'); self.assertEqual(len(snap['installs']), 2)
        self.assertEqual([e['name'] for e in snap['footprint_libraries']['entries']], ['Resistor_SMD', 'Connector_PinHeader_2.54mm'])
        none = kl.environment(self.env, [fake_install(self.dir, '10.0')])
        self.assertIsNone(none['selected']); self.assertEqual(none['symbol_libraries']['entries'], [])


class Index(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.dir = Path(self.tmp.name)
        self.libs = {'symbol': kl.directory_libraries(SNAP / 'symbols', 'symbol'), 'footprint': kl.directory_libraries(SNAP / 'footprints', 'footprint')}
        self.db = self.dir / 'index.sqlite'

    def tearDown(self):
        self.tmp.cleanup()

    def dump(self, db):
        con = sqlite3.connect(str(db)); rows = list(con.iterdump()); con.close()
        return rows

    def test_counts_and_deterministic_content(self):
        stats = kl.build_index(self.db, self.libs, jobs=1)
        self.assertEqual(stats['errors'], []); self.assertEqual(len(stats['parsed']), 4)
        expected = {k: sum(len(kl.parse_library(k, n, p)[0]) for n, p in v) for k, v in self.libs.items()}
        self.assertEqual(stats['counts'], expected)
        kl.build_index(self.dir / 'parallel.sqlite', self.libs, jobs=2)
        self.assertEqual(self.dump(self.db), self.dump(self.dir / 'parallel.sqlite'))

    def test_only_changed_libraries_are_parsed_again(self):
        work = self.dir / 'libs'
        for kind, libs in self.libs.items():
            for name, path in libs:
                src = Path(path)
                for f in ([src] if kind == 'symbol' else src.glob('*.kicad_mod')):
                    write(work / (f.name if kind == 'symbol' else f'{src.name}/{f.name}'), f.read_text(encoding='utf-8'))
        libs = {'symbol': kl.directory_libraries(work, 'symbol'), 'footprint': kl.directory_libraries(work, 'footprint')}
        kl.build_index(self.db, libs)
        again = kl.build_index(self.db, libs)
        self.assertEqual((again['parsed'], again['unchanged']), ([], 4))
        device = work / 'Device.kicad_sym'
        device.write_text(device.read_text(encoding='utf-8').replace('(symbol "R"', '(symbol "R_RENAMED"', 1), encoding='utf-8')
        os.utime(device, ns=(time.time_ns(), time.time_ns() + 10**9))
        changed = kl.build_index(self.db, libs)
        self.assertEqual(changed['parsed'], ['symbol:Device'])
        self.assertTrue(kl.search(self.db, 'R_RENAMED')); self.assertIsNone(kl.pair(self.db, 'Device:R'))
        libs['footprint'] = libs['footprint'][:1]
        removed = kl.build_index(self.db, libs)
        self.assertEqual(removed['removed'], ['footprint:Resistor_SMD']); self.assertEqual(kl.search(self.db, '0603', 'footprint'), [])

    def test_a_different_schema_rebuilds(self):
        kl.build_index(self.db, self.libs)
        con = sqlite3.connect(str(self.db)); con.execute("update meta set value='0' where key='schema'"); con.commit(); con.close()
        self.assertEqual(len(kl.build_index(self.db, self.libs)['parsed']), 4)

    def test_search_and_pairing(self):
        kl.build_index(self.db, self.libs)
        self.assertEqual(kl.search(self.db, 'resistor')[0]['id'], 'Device:R')
        self.assertEqual([r['id'] for r in kl.search(self.db, 'pin header 1x03', 'footprint')], ['Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical'])
        self.assertEqual(kl.search(self.db, '"; drop table symbols; --'), [])
        r = kl.pair(self.db, 'Device:R')
        self.assertEqual(r['pins'], ['1', '2']); self.assertEqual([c['id'] for c in r['candidates']], ['Resistor_SMD:R_0603_1608Metric'])
        self.assertTrue(r['candidates'][0]['filter_match'])
        conn = kl.pair(self.db, 'Connector_Generic:Conn_01x03')
        self.assertEqual([c['id'] for c in conn['candidates']], ['Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical'])
        self.assertIsNone(kl.pair(self.db, 'Device:NoSuchSymbol'))
        four = kl.pair(self.db, 'Connector_Generic:Conn_01x04')
        if four:
            self.assertEqual(four['candidates'], [], 'a 3-pad header never pairs with a 4-pin symbol')


class Cli(unittest.TestCase):
    def run_pcba(self, *args):
        r = subprocess.run([PCBA, *map(str, args), '--json'], capture_output=True, timeout=600)
        return r.returncode, json.loads(r.stdout.decode('utf-8'))

    @unittest.skipUnless(PCBA, 'needs the pcba executable')
    def test_scan_search_pair_env(self):
        with tempfile.TemporaryDirectory() as d:
            rc, doc = self.run_pcba('library', 'scan', '--symbols', SNAP / 'symbols', '--footprints', SNAP / 'footprints', '--out', d)
            self.assertEqual((doc['status'], rc), ('PASS', 0), doc['message'])
            self.assertTrue((Path(d) / 'index.sqlite').is_file()); self.assertTrue((Path(d) / 'environment.json').is_file())
            index = Path(d) / 'index.sqlite'
            rc, doc = self.run_pcba('library', 'search', 'resistor', '--index', index)
            self.assertEqual((doc['status'], rc), ('PASS', 0)); self.assertEqual(doc['result']['data']['results'][0]['id'], 'Device:R')
            rc, doc = self.run_pcba('library', 'search', 'zzzz_no_match', '--index', index, '--kind', 'footprint')
            self.assertEqual((doc['status'], rc, doc['result']['data']['results']), ('PASS', 0, []))   # as parts search
            rc, doc = self.run_pcba('library', 'pair', 'Device:R', '--index', index)
            self.assertEqual((doc['status'], rc), ('PASS', 0)); self.assertEqual(doc['result']['data']['candidates'][0]['id'], 'Resistor_SMD:R_0603_1608Metric')
            rc, doc = self.run_pcba('library', 'pair', 'Device:Nope', '--index', index)
            self.assertEqual((doc['status'], rc), ('INVALID_INPUT', 2))
            rc, doc = self.run_pcba('library', 'search', 'x', '--index', Path(d) / 'missing.sqlite')
            self.assertEqual((doc['status'], rc), ('INVALID_INPUT', 2))
            rc, doc = self.run_pcba('library', 'env', '--out', Path(d) / 'env.json')
            self.assertIn(doc['status'], ('PASS', 'UNVERIFIED')); self.assertEqual(rc, {'PASS': 0, 'UNVERIFIED': 3}[doc['status']])
            snap = json.loads((Path(d) / 'env.json').read_text(encoding='utf-8')); self.assertEqual(snap['policy'], '9.x')


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
