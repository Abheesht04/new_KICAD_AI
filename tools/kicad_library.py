"""KiCad environment and library index (docs/engineering/KICAD_LIBRARY.md).

Detects every installed KiCad, selects one by policy (KiCad 9.x is the native validation target), reads the global and
project sym-lib-table / fp-lib-table with ${VAR} expansion, and indexes symbols and footprints into a SQLite cache
with full-text search. The cache is regenerable: only libraries whose files changed are parsed again, and deleting it
loses nothing. Symbol-to-footprint pairing compares pin and pad number sets; footprint filters only rank candidates.
"""
from __future__ import annotations
import fnmatch, hashlib, json, os, re, shutil, sqlite3, subprocess, sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from hwc_kicad import SExprError, _unescape, direct, head, one, parse_sexpr

POLICY = 9          # the KiCad major version native validation targets
SCHEMA = 1          # index schema; a different stored value rebuilds the index
VERSION = re.compile(r'(\d+)\.(\d+)(?:\.(\d+))?')
VAR = re.compile(r'\$\{(\w+)\}|\$\((\w+)\)')
KINDS = {'symbol': ('sym-lib-table', 'sym_lib_table'), 'footprint': ('fp-lib-table', 'fp_lib_table')}
DIRS = (('symbols', 'symbols', 'SYMBOL_DIR'), ('footprints', 'footprints', 'FOOTPRINT_DIR'),
        ('models', '3dmodels', '3DMODEL_DIR'), ('templates', 'template', 'TEMPLATE_DIR'))


def version_key(text):
    m = VERSION.search(text or '')
    return tuple(int(x or 0) for x in m.groups()) if m else None


# ------------------------------------------------------------------ installs
def layout(root):
    """Programs and shared data of one install: share/kicad + bin (Windows, Linux), SharedSupport + MacOS (macOS)."""
    root = Path(root)
    for share, bindir in ((root / 'share' / 'kicad', root / 'bin'), (root / 'Contents' / 'SharedSupport', root / 'Contents' / 'MacOS')):
        if share.is_dir():
            cli = next((bindir / n for n in ('kicad-cli.exe', 'kicad-cli') if (bindir / n).is_file()), None)
            return cli, {key: share / sub for key, sub, _ in DIRS}
    return None


def system_roots(environ):
    roots = []
    for cli in (environ.get('KICAD_CLI'), shutil.which('kicad-cli', path=environ.get('PATH', ''))):
        if cli:
            exe = Path(cli).resolve()
            roots.append(exe.parent.parent.parent if exe.parent.name == 'MacOS' else exe.parent.parent)
    bases = [environ.get('ProgramFiles'), environ.get('ProgramW6432'),
             environ.get('LOCALAPPDATA') and str(Path(environ['LOCALAPPDATA']) / 'Programs')]
    for base in dict.fromkeys(b for b in bases if b):
        folder = Path(base) / 'KiCad'
        if folder.is_dir():
            roots += sorted(p for p in folder.iterdir() if p.is_dir())
    return roots + [Path('/usr'), Path('/usr/local'), Path('/Applications/KiCad/KiCad.app')]


def find_installs(environ=None, roots=None, probe=True):
    """Every KiCad install found, with its version: `kicad-cli version` when probe is set, else the folder name."""
    environ = os.environ if environ is None else environ
    installs, seen = [], set()
    for root in (system_roots(environ) if roots is None else map(Path, roots)):
        found = layout(root)
        key = str(root.resolve())
        if not found or key in seen:
            continue
        seen.add(key)
        cli, dirs = found
        version = source = None
        if probe and cli:
            try:
                r = subprocess.run([str(cli), 'version'], capture_output=True, text=True, timeout=60)
                if r.returncode == 0 and version_key(r.stdout):
                    version, source = r.stdout.strip(), 'kicad-cli version'
            except (OSError, subprocess.SubprocessError):
                pass
        if version is None and version_key(Path(key).name):
            version, source = Path(key).name, 'folder name'
        installs.append({'root': key, 'version': version, 'version_source': source, 'cli': str(cli) if cli else None,
                         **{k: str(v) if v.is_dir() else None for k, v in dirs.items()}})
    return installs


def select(installs, policy=POLICY):
    """(install, reason): the highest policy.x install with kicad-cli and both libraries; never another major."""
    usable = sorted((i for i in installs if (version_key(i['version']) or (0,))[0] == policy and i['cli'] and i['symbols']
                     and i['footprints']), key=lambda i: i['root'])
    if not usable:
        found = '; '.join(f"{i['version'] or 'unknown version'} at {i['root']}" for i in installs) or 'none'
        return None, f'no KiCad {policy}.x install with kicad-cli and libraries (found: {found})'
    best = max(usable, key=lambda i: (version_key(i['version']), i['version_source'] == 'kicad-cli version'))
    return best, f"KiCad {best['version']} at {best['root']}: highest {policy}.x of {len(usable)} usable"


def ordered(installs, policy=POLICY):
    """Installs in the order a library or CLI lookup should try them: policy.x first, then the rest; newest first."""
    return sorted(installs, key=lambda i: ((version_key(i['version']) or (0,))[0] != policy,
                                          tuple(-x for x in version_key(i['version']) or (0,)), i['root']))


def config_dir(install, environ):
    """KiCad keeps one settings folder per major version (e.g. 9.0); KICAD_CONFIG_HOME replaces the base folder."""
    if not install or not version_key(install['version']):
        return None
    if environ.get('KICAD_CONFIG_HOME'):
        base = Path(environ['KICAD_CONFIG_HOME'])
    elif os.name == 'nt':
        base = Path(environ.get('APPDATA', Path.home() / 'AppData' / 'Roaming')) / 'kicad'
    elif sys.platform == 'darwin':
        base = Path.home() / 'Library' / 'Preferences' / 'kicad'
    else:
        base = Path(environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'kicad'
    return base / f'{version_key(install["version"])[0]}.0'


# ------------------------------------------------------------------ variables and tables
def variables(install, environ, project=None):
    """name -> (value, source). KiCad's order: the process environment overrides the user's kicad_common.json, which
    overrides the install's own KICADn_*_DIR defaults. KIPRJMOD is the project folder."""
    found = {}
    major = (version_key(install['version']) or (0,))[0] if install else 0
    for key, _, suffix in DIRS:
        if install and install[key]:
            found[f'KICAD{major}_{suffix}'] = (install[key], 'install')
    cfg = config_dir(install, environ)
    if cfg:
        try:
            user = (json.loads((cfg / 'kicad_common.json').read_text(encoding='utf-8')).get('environment') or {}).get('vars') or {}
        except (OSError, ValueError):
            user = {}
        found.update((k, (str(v), 'kicad_common.json')) for k, v in user.items())
    found.update((k, (v, 'environment')) for k, v in environ.items() if k in found or k.startswith('KICAD'))
    if project:
        found['KIPRJMOD'] = (str(Path(project).resolve()), 'project')
    return found


def expand(uri, found, environ):
    """(expanded text, names that could not be resolved)."""
    missing = []

    def sub(m):
        name = m.group(1) or m.group(2)
        if name in found:
            return found[name][0]
        if name in environ:
            return environ[name]
        missing.append(name)
        return m.group(0)
    return VAR.sub(sub, uri), missing


def read_table(path, kind):
    root = parse_sexpr(Path(path).read_text(encoding='utf-8-sig'))
    if head(root) != KINDS[kind][1]:
        raise SExprError(f'{path}: expected ({KINDS[kind][1]} ...), found ({head(root)} ...)')

    def get(lib, key):
        node = one(lib, key)
        return str(node[1]) if node is not None and len(node) > 1 else ''
    return [{'name': get(lib, 'name'), 'type': get(lib, 'type'), 'uri': get(lib, 'uri'), 'descr': get(lib, 'descr'),
             'disabled': one(lib, 'disabled') is not None} for lib in direct(root, 'lib')]


def library_table(kind, install, environ, project=None):
    """The libraries KiCad would offer: the user's global table (or, before KiCad's first run, the install's template
    that KiCad copies), then the project table, whose nicknames override global ones. Each entry has a status:
    ok, disabled, missing, unresolved (an undefined ${VAR}), unsupported_type (only KiCad-format libraries are indexed),
    duplicate or overridden."""
    file_name = KINDS[kind][0]
    found = variables(install, environ, project)
    cfg = config_dir(install, environ)
    tables = []
    if cfg and (cfg / file_name).is_file():
        tables.append(('global', cfg / file_name))
    elif install and install['templates'] and (Path(install['templates']) / file_name).is_file():
        tables.append(('global (install template)', Path(install['templates']) / file_name))
    if project and (Path(project) / file_name).is_file():
        tables.append(('project', Path(project) / file_name))
    entries, by_name = [], {}
    for scope, path in tables:
        seen = set()
        for row in read_table(path, kind):
            expanded, missing = expand(row['uri'], found, environ)
            target = Path(expanded)
            exists = target.is_file() if kind == 'symbol' else target.is_dir()
            status = ('duplicate' if row['name'] in seen else 'disabled' if row['disabled'] else
                      'unsupported_type' if row['type'].lower() != 'kicad' else 'unresolved' if missing else
                      'ok' if exists else 'missing')
            seen.add(row['name'])
            entry = {**row, 'scope': scope, 'table': str(path), 'path': None if missing else str(target),
                     'unresolved': missing, 'status': status}
            if status != 'duplicate':
                if row['name'] in by_name:      # only a project entry can meet an earlier (global) one here
                    by_name[row['name']]['status'] = 'overridden'
                by_name[row['name']] = entry
            entries.append(entry)
    return {'tables': [{'scope': s, 'path': str(p)} for s, p in tables], 'entries': entries}


def environment(environ=None, roots=None, project=None, policy=POLICY):
    """The JSON snapshot: every install, the selected one and why, path variables and both library tables."""
    environ = dict(os.environ if environ is None else environ)
    installs = find_installs(environ, roots)
    chosen, reason = select(installs, policy)
    snap = {'schema': 1, 'policy': f'{policy}.x', 'selected': chosen, 'reason': reason, 'installs': installs,
            'config_dir': str(config_dir(chosen, environ)) if chosen else None,
            'project': str(Path(project).resolve()) if project else None,
            'variables': {k: {'value': v, 'source': s} for k, (v, s) in sorted(variables(chosen, environ, project).items())}}
    for kind in KINDS:
        snap[f'{kind}_libraries'] = library_table(kind, chosen, environ, project) if chosen else {'tables': [], 'entries': []}
    return snap


def table_libraries(snap, kind):
    """(nickname, path) of every usable library in a snapshot, in table order."""
    return [(e['name'], e['path']) for e in snap[f'{kind}_libraries']['entries'] if e['status'] == 'ok']


def directory_libraries(folder, kind):
    """(nickname, path) for every library directly in a folder: *.kicad_sym files or *.pretty directories."""
    folder = Path(folder)
    if kind == 'symbol':
        return [(p.stem, str(p)) for p in sorted(folder.glob('*.kicad_sym')) if p.is_file()]
    return [(p.name[:-len('.pretty')], str(p)) for p in sorted(folder.glob('*.pretty')) if p.is_dir()]


# ------------------------------------------------------------------ parsing (runs in worker processes)
# The index reads a few fields from very large files (a 16 MB MCU symbol library is 4 million tokens), so it does not
# build the whole tree. Quoted strings and comments are blanked first, keeping every offset, so parentheses in the
# blanked text are structural; the depth of each node of interest comes from C-level counts between them, and only
# small nodes (courtyard graphics, attributes) go through the strict parser. tests/test_kicad_library.py compares
# every field with a full-tree reading of the same files.
STRING = re.compile(r'"(?:[^"\\]|\\.)*"|;[^\n]*', re.S)
HEAD = re.compile(r'\(([^\s()"]+)')
VALUE = re.compile(r'"[^"]*"|[^\s"]+')


def blank(text):
    out = STRING.sub(lambda m: '"' + ' ' * (len(m.group()) - 2) + '"' if m.group()[0] == '"' else ' ' * len(m.group()), text)
    if out.count('"') % 2 or out.count('(') != out.count(')'):
        raise SExprError('unbalanced parentheses or an unterminated quoted string')
    return out


def nodes(masked, heads, start=0, end=None):
    """(head, depth, offset) of every list whose head is in heads, in file order; depth 0 is the list at start."""
    found, level, last = [], 0, start
    for m in re.compile(r'\((%s)(?=[\s()"])' % '|'.join(heads)).finditer(masked, start, len(masked) if end is None else end):
        level += masked.count('(', last, m.start()) - masked.count(')', last, m.start())
        last = m.start()
        found.append((m.group(1), level, m.start()))
    return found


def end_of(masked, start):
    """Offset just after the list that opens at start."""
    depth, pos = 0, start
    while True:
        close = masked.find(')', pos)
        if close < 0:
            raise SExprError(f'unclosed list at byte {start}')
        depth += masked.count('(', pos, close) - 1
        if depth == 0:
            return close + 1
        pos = close + 1


def leading(text, masked, start):
    """The values after a list's head, up to its first child list: (pad "1" smd rect (at ...)) -> ['1', 'smd', 'rect']."""
    begin = HEAD.match(masked, start).end()
    stop = min(i for i in (masked.find('(', begin), masked.find(')', begin)) if i >= 0)
    return [_unescape(text[m.start() + 1:m.end() - 1]) if m.group()[0] == '"' else m.group()
            for m in VALUE.finditer(masked, begin, stop)]


def first(text, masked, start, default=''):
    values = leading(text, masked, start)
    return values[0] if values else default


def root_head(masked):
    m = HEAD.match(masked, len(masked) - len(masked.lstrip()))
    return m.group(1) if m else None


def natural(numbers):
    return sorted(numbers, key=lambda s: (0, int(s), s) if s.isdigit() else (1, 0, s))


def parse_symbols(nickname, path):
    text = Path(path).read_text(encoding='utf-8')
    masked = blank(text)
    if root_head(masked) != 'kicad_symbol_lib':
        raise SExprError(f'not a KiCad symbol library: ({root_head(masked)} ...)')
    raw, version, sym, unit = {}, '', None, 0
    for name, depth, start in nodes(masked, ('symbol', 'property', 'pin', 'extends', 'power', 'version')):
        if depth == 1 and name == 'version':
            version = first(text, masked, start)
        elif depth == 1 and name == 'symbol':
            sym = raw.setdefault(first(text, masked, start), {'props': {}, 'pins': {}, 'extends': '', 'power': False})
            unit = 0
        elif sym is None or depth not in (2, 3):
            continue
        elif depth == 2 and name == 'symbol':
            m = re.search(r'_(\d+)_(\d+)$', first(text, masked, start))
            unit = int(m.group(1)) if m else 0
        elif depth == 2 and name == 'property':
            values = leading(text, masked, start) + ['', '']
            sym['props'][values[0]] = values[1]
        elif depth == 2 and name == 'extends':
            sym['extends'] = first(text, masked, start)
        elif depth == 2 and name == 'power':
            sym['power'] = True
        elif name == 'pin':
            fields = {}
            for field, _, at in nodes(masked, ('name', 'number'), start, end_of(masked, start)):
                fields.setdefault(field, first(text, masked, at))
            sym['pins'][(fields.get('number', ''), fields.get('name', ''), first(text, masked, start),
                         unit if depth == 3 else 0)] = None
    rows = []
    for name, own in raw.items():
        chain, seen = [own], {name}
        while chain[-1]['extends'] in raw and chain[-1]['extends'] not in seen:
            seen.add(chain[-1]['extends'])
            chain.append(raw[chain[-1]['extends']])
        props = {}
        for s in reversed(chain):          # a derived symbol's own properties override its parent's
            props.update(s['props'])
        pins = next(([dict(zip(('number', 'name', 'type', 'unit'), p)) for p in s['pins']] for s in chain if s['pins']), [])
        numbers = natural({p['number'] for p in pins if p['number']})
        rows.append({'id': f'{nickname}:{name}', 'library': nickname, 'name': name, 'extends': own['extends'],
                     'reference': props.get('Reference', ''), 'value': props.get('Value', ''),
                     'description': props.get('Description') or props.get('ki_description', ''),
                     'keywords': props.get('ki_keywords', ''), 'fp_filters': props.get('ki_fp_filters', '').split(),
                     'footprint': props.get('Footprint', ''), 'power': any(s['power'] for s in chain),
                     'units': max([p['unit'] for p in pins] + [1]), 'pins': pins,
                     'pin_key': '\x1f'.join(sorted(numbers)), 'pin_count': len(numbers)})
    return rows, version


def courtyard(items):
    """[xmin, ymin, xmax, ymax] of F/B courtyard graphics, or None when the footprint has no courtyard."""
    xs, ys = [], []

    def xy(node):
        return float(node[1]), float(node[2])
    for item in items:
        layer = one(item, 'layer')
        if layer is None or 'CrtYd' not in str(layer[1]):
            continue
        points = [xy(one(item, k)) for k in ('start', 'mid', 'end') if one(item, k) is not None]
        if head(item) == 'fp_circle' and one(item, 'center') is not None:
            (cx, cy), (ex, ey) = xy(one(item, 'center')), points[-1]
            r = ((ex - cx) ** 2 + (ey - cy) ** 2) ** 0.5
            points = [(cx - r, cy - r), (cx + r, cy + r)]
        pts = one(item, 'pts')
        points += [xy(p) for p in direct(pts, 'xy')] if pts is not None else []
        xs += [p[0] for p in points]
        ys += [p[1] for p in points]
    return [round(min(xs), 6), round(min(ys), 6), round(max(xs), 6), round(max(ys), 6)] if xs else None


GRAPHICS = ('fp_line', 'fp_rect', 'fp_circle', 'fp_arc', 'fp_poly')


def parse_footprints(nickname, path):
    rows, versions = [], set()
    for file in sorted(Path(path).glob('*.kicad_mod')):
        text = file.read_text(encoding='utf-8')
        masked = blank(text)
        if root_head(masked) not in ('footprint', 'module'):
            raise SExprError(f'{file.name}: not a footprint: ({root_head(masked)} ...)')
        info = {'descr': '', 'tags': '', 'attr': [], 'pads': [], 'graphics': [], 'models': 0}
        for name, depth, start in nodes(masked, ('pad', 'model', 'attr', 'descr', 'tags', 'version') + GRAPHICS):
            if depth != 1:
                continue
            if name == 'version':
                versions.add(first(text, masked, start))
            elif name in ('descr', 'tags'):
                info[name] = first(text, masked, start)
            elif name == 'pad':
                values = leading(text, masked, start) + ['', '']
                info['pads'].append({'number': values[0], 'type': values[1]})
            elif name == 'model':
                info['models'] += 1
            else:
                end = end_of(masked, start)
                if name == 'attr':
                    info['attr'] = [str(a) for a in parse_sexpr(text[start:end])[1:]]
                elif 'CrtYd' in text[start:end]:
                    info['graphics'].append(parse_sexpr(text[start:end]))
        numbers = natural({p['number'] for p in info['pads'] if p['number'] and p['type'] != 'np_thru_hole'})
        rows.append({'id': f'{nickname}:{file.stem}', 'library': nickname, 'name': file.stem, 'description': info['descr'],
                     'tags': info['tags'], 'attributes': info['attr'], 'pads': info['pads'],
                     'pad_key': '\x1f'.join(sorted(numbers)), 'pad_count': len(numbers),
                     'courtyard': courtyard(info['graphics']), 'models': info['models']})
    return rows, ','.join(sorted(versions - {''}))


def parse_library(kind, nickname, path):
    """One library -> (rows, file format version, error). Errors are recorded, never fatal for the whole index."""
    try:
        rows, version = (parse_symbols if kind == 'symbol' else parse_footprints)(nickname, path)
        return rows, version, None
    except (OSError, UnicodeError, SExprError, ValueError, IndexError) as exc:
        return [], '', f'{type(exc).__name__}: {exc}'


def signature(kind, path):
    """(digest, bytes): the digest changes when the library's files change (size and modification time of each)."""
    path = Path(path)
    files = [path] if kind == 'symbol' else sorted(path.glob('*.kicad_mod'))
    digest, size = hashlib.sha256(), 0
    for f in files:
        st = f.stat()
        size += st.st_size
        digest.update(f'{f.name}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode())
    return digest.hexdigest(), size


# ------------------------------------------------------------------ index
DDL = '''
create table meta(key text primary key, value text);
create table sources(kind text, nickname text, path text, signature text, format text, error text, entries integer,
                     primary key(kind, nickname));
create table symbols(id text primary key, library text, name text, extends text, reference text, value text,
                     description text, keywords text, fp_filters text, footprint text, power integer, units integer,
                     pins text, pin_key text, pin_count integer);
create index symbols_pins on symbols(pin_key);
create index symbols_library on symbols(library);
create table footprints(id text primary key, library text, name text, description text, tags text, attributes text,
                        pads text, pad_key text, pad_count integer, courtyard text, models integer);
create index footprints_pads on footprints(pad_key);
create index footprints_library on footprints(library);
create virtual table symbol_text using fts5(name, description, keywords);      -- rowid = symbols.rowid
create virtual table footprint_text using fts5(name, description, tags);       -- rowid = footprints.rowid
'''
TABLE = {'symbol': ('symbols', 'symbol_text', 'keywords'), 'footprint': ('footprints', 'footprint_text', 'tags')}


def open_index(db_path):
    db = sqlite3.connect(str(db_path))
    db.row_factory = sqlite3.Row
    stored = db.execute("select name from sqlite_master where name='meta'").fetchone() and \
        db.execute("select value from meta where key='schema'").fetchone()
    if not stored or stored[0] != str(SCHEMA):
        db.close()
        Path(db_path).unlink(missing_ok=True)
        db = sqlite3.connect(str(db_path))
        db.row_factory = sqlite3.Row
        db.executescript(DDL)
        db.execute("insert into meta values('schema', ?)", (str(SCHEMA),))
        db.commit()
    return db


def drop_library(db, kind, nickname):
    table, fts, _ = TABLE[kind]
    db.execute(f'delete from {fts} where rowid in (select rowid from {table} where library=?)', (nickname,))
    db.execute(f'delete from {table} where library=?', (nickname,))
    db.execute('delete from sources where kind=? and nickname=?', (kind, nickname))


def insert(db, kind, nickname, rows):
    table, fts, words = TABLE[kind]
    if kind == 'symbol':
        values = [(r['id'], r['library'], r['name'], r['extends'], r['reference'], r['value'], r['description'],
                   r['keywords'], json.dumps(r['fp_filters']), r['footprint'], int(r['power']), r['units'],
                   json.dumps(r['pins']), r['pin_key'], r['pin_count']) for r in rows]
    else:
        values = [(r['id'], r['library'], r['name'], r['description'], r['tags'], json.dumps(r['attributes']),
                   json.dumps(r['pads']), r['pad_key'], r['pad_count'], json.dumps(r['courtyard']), r['models']) for r in rows]
    if values:
        db.executemany(f'insert into {table} values({",".join("?" * len(values[0]))})', values)
    db.execute(f'insert into {fts}(rowid, name, description, {words}) '
               f'select rowid, name, description, {words} from {table} where library=?', (nickname,))


def build_index(db_path, libraries, jobs=None):
    """libraries: {'symbol': [(nickname, path)], 'footprint': [...]}. Parses only new or changed libraries (in parallel
    when jobs > 1) and writes them in a fixed order, so the index content does not depend on worker timing."""
    Path(db_path).parent.mkdir(parents=True, exist_ok=True)
    db = open_index(db_path)
    stats = {'parsed': [], 'unchanged': 0, 'removed': [], 'errors': []}
    todo = []
    for kind, libs in libraries.items():
        wanted = {}
        for nickname, path in libs:
            wanted.setdefault(nickname, path)   # a repeated nickname: the first (the table's) wins, as in KiCad
        stored = {r['nickname']: r for r in db.execute('select * from sources where kind=?', (kind,))}
        for nickname in sorted(set(stored) - set(wanted)):
            drop_library(db, kind, nickname)
            stats['removed'].append(f'{kind}:{nickname}')
        for nickname, path in wanted.items():
            sig, size = signature(kind, path)
            old = stored.get(nickname)
            if old and old['path'] == str(path) and old['signature'] == sig:
                stats['unchanged'] += 1
            else:
                todo.append((kind, nickname, str(path), sig, size))
    jobs = max(1, min(jobs or os.cpu_count() or 1, len(todo)))
    pool = ProcessPoolExecutor(jobs) if jobs > 1 else None
    try:
        # Largest libraries first keeps every worker busy; results are written in the fixed order of todo.
        futures = {}
        for item in sorted(todo, key=lambda t: -t[4]) if pool else []:
            futures[item[:2]] = pool.submit(parse_library, *item[:3])
        for kind, nickname, path, sig, _ in todo:
            rows, version, error = futures[(kind, nickname)].result() if pool else parse_library(kind, nickname, path)
            drop_library(db, kind, nickname)
            insert(db, kind, nickname, rows)
            db.execute('insert into sources values(?, ?, ?, ?, ?, ?, ?)', (kind, nickname, path, sig, version, error, len(rows)))
            stats['parsed'].append(f'{kind}:{nickname}')
            if error:
                stats['errors'].append({'library': f'{kind}:{nickname}', 'path': path, 'error': error})
    finally:
        if pool:
            pool.shutdown(cancel_futures=True)
    db.commit()
    stats['counts'] = {kind: db.execute(f'select count(*) from {TABLE[kind][0]}').fetchone()[0] for kind in TABLE}
    db.close()
    return stats


def row_dict(row, kind):
    d = dict(row)
    for key in ('fp_filters', 'pins') if kind == 'symbol' else ('attributes', 'pads', 'courtyard'):
        d[key] = json.loads(d[key])
    d.pop('pin_key' if kind == 'symbol' else 'pad_key')
    if kind == 'symbol':
        d['power'] = bool(d['power'])
    return d


def search(db_path, query, kind='symbol', limit=20):
    """Full-text search over name, description and keywords/tags; every word must match (as a prefix)."""
    table, fts, _ = TABLE[kind]
    words = re.findall(r'\w+', query)
    if not words:
        return []
    match = ' '.join('"%s"*' % w.replace('"', '""') for w in words)
    db = open_index(db_path)
    rows = db.execute(f'select t.* from {fts} f join {table} t on t.rowid = f.rowid where {fts} match ? '
                      f'order by bm25({fts}, 10, 2, 4), t.id limit ?', (match, limit)).fetchall()
    db.close()
    return [row_dict(r, kind) for r in rows]


def filter_match(filters, fp):
    """KiCad footprint filters are case-insensitive wildcards on the footprint name, or on LIB:NAME when they hold ':'."""
    return any(fnmatch.fnmatchcase((fp['id'] if ':' in f else fp['name']).lower(), f.lower()) for f in filters)


def pair(db_path, symbol_id, limit=50):
    """Footprints whose pad-number set equals the symbol's pin-number set. Ranked: the symbol's default footprint,
    then footprints its filters accept, then the rest; by id within each group. None when the symbol is unknown."""
    db = open_index(db_path)
    sym = db.execute('select * from symbols where id=?', (symbol_id,)).fetchone()
    if sym is None:
        db.close()
        return None
    rows = db.execute('select id, library, name, pad_count from footprints where pad_key=? order by id',
                      (sym['pin_key'],)).fetchall() if sym['pin_key'] else []
    default = db.execute('select id, pad_key from footprints where id=?', (sym['footprint'],)).fetchone() if sym['footprint'] else None
    db.close()
    key, sym = sym['pin_key'], row_dict(sym, 'symbol')
    found = [{**dict(r), 'default': r['id'] == sym['footprint'], 'filter_match': filter_match(sym['fp_filters'], r)} for r in rows]
    found.sort(key=lambda c: (not c['default'], not c['filter_match'], c['id']))
    return {'symbol': symbol_id, 'pins': natural({p['number'] for p in sym['pins'] if p['number']}),
            'fp_filters': sym['fp_filters'], 'default_footprint': sym['footprint'] or None,
            'default_footprint_pads_match': None if default is None else default['pad_key'] == key,
            'total': len(found), 'filter_matches': sum(c['filter_match'] for c in found), 'candidates': found[:limit]}
