#!/usr/bin/env python3
"""Hardware Compiler <-> KiCad bridge (stdlib-only, Windows friendly).

V1 scope: index/search installed .kicad_sym libraries, deterministic resistor-
divider prompt -> .kicad_sch, and invoke kicad-cli for real validation/export.
The generated schematic embeds symbol definitions copied from the installed
KiCad libraries; it does not invent symbol geometry or pretend to do ERC.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


class Atom(str):
    """Unquoted S-expression atom."""


class Quoted(str):
    """Quoted S-expression string."""


SExpr = Atom | Quoted | list["SExpr"]


class SExprError(ValueError):
    pass


def parse_sexpr(text: str) -> SExpr:
    """Small strict parser for KiCad's parenthesized S-expression files."""
    n = len(text)
    i = 0

    def skip() -> None:
        nonlocal i
        while i < n:
            if text[i].isspace():
                i += 1
            elif text[i] == ';':
                j = text.find('\n', i)
                i = n if j < 0 else j + 1
            else:
                break

    def read_string() -> Quoted:
        nonlocal i
        assert text[i] == '"'
        i += 1
        out: list[str] = []
        while i < n:
            c = text[i]
            i += 1
            if c == '"':
                return Quoted(''.join(out))
            if c == '\\':
                if i >= n:
                    raise SExprError('backslash at end of quoted string')
                esc = text[i]
                i += 1
                # KiCad strings use backslash escapes. Preserve escaped quote and
                # backslash; for other escaped chars retain the character.
                out.append({'n': '\n', 'r': '\r', 't': '\t'}.get(esc, esc))
            else:
                out.append(c)
        raise SExprError('unterminated quoted string')

    def read_node() -> SExpr:
        nonlocal i
        skip()
        if i >= n:
            raise SExprError('unexpected end of input')
        c = text[i]
        if c == '(':
            i += 1
            result: list[SExpr] = []
            while True:
                skip()
                if i >= n:
                    raise SExprError('unclosed list')
                if text[i] == ')':
                    i += 1
                    return result
                result.append(read_node())
        if c == ')':
            raise SExprError(f'unexpected closing parenthesis at byte {i}')
        if c == '"':
            return read_string()
        start = i
        while i < n and not text[i].isspace() and text[i] not in '();':
            i += 1
        if i == start:
            raise SExprError(f'unexpected token at byte {i}')
        return Atom(text[start:i])

    node = read_node()
    skip()
    if i != n:
        raise SExprError(f'extra content after expression at byte {i}')
    return node


def quote_string(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def dump_sexpr(node: SExpr, indent: int = 0) -> str:
    if isinstance(node, Atom):
        return str(node)
    if isinstance(node, Quoted):
        return quote_string(str(node))
    if not node:
        return '()'
    # Compact all-atom forms: (at 10 20 0), (version 20250114), etc.
    if all(not isinstance(child, list) for child in node):
        return '(' + ' '.join(dump_sexpr(child, indent) for child in node) + ')'
    pad = ' ' * indent
    child_pad = ' ' * (indent + 2)
    lines = ['(' + dump_sexpr(node[0], indent + 1)]
    for child in node[1:]:
        lines.append(child_pad + dump_sexpr(child, indent + 2))
    lines.append(pad + ')')
    return '\n'.join(lines)


def head(node: Any) -> str | None:
    if isinstance(node, list) and node and isinstance(node[0], str):
        return str(node[0])
    return None


def direct(node: Any, key: str) -> list[list[Any]]:
    if not isinstance(node, list):
        return []
    return [x for x in node if isinstance(x, list) and head(x) == key]


def one(node: Any, key: str) -> list[Any] | None:
    found = direct(node, key)
    return found[0] if found else None


def text_at(node: Any, key: str, index: int = 1, default: str = '') -> str:
    item = one(node, key)
    if item is not None and len(item) > index:
        return str(item[index])
    return default



def discover_symbol_dir(explicit: str | None = None) -> Path:
    # An explicit path is authoritative; never silently substitute another library.
    if explicit:
        path = Path(explicit)
        if path.is_dir() and any(path.glob('*.kicad_sym')):
            return path.resolve()
        raise FileNotFoundError(
            f'Explicit KiCad symbol directory is missing or invalid: {path}'
        )

    candidates: list[Path] = []
    env = os.environ.get('KICAD_SYMBOL_DIR') or os.environ.get('KICAD9_SYMBOL_DIR')
    if env:
        candidates.append(Path(env))

    pf = os.environ.get('ProgramFiles', r'C:\Program Files')
    candidates.extend([
        Path(pf) / 'KiCad' / '9.0' / 'share' / 'kicad' / 'symbols',
        Path(pf) / 'KiCad' / '10.0' / 'share' / 'kicad' / 'symbols',
        Path(pf) / 'KiCad' / '8.0' / 'share' / 'kicad' / 'symbols',
        Path('/usr/share/kicad/symbols'),
        Path('/usr/local/share/kicad/symbols'),
        Path('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols'),
    ])

    for path in candidates:
        if path.is_dir() and any(path.glob('*.kicad_sym')):
            return path.resolve()

    looked = '\n  '.join(str(path) for path in candidates)
    raise FileNotFoundError(
        'Cannot find KiCad symbol directory. Supply --symbol-dir. Checked:\n  '
        + looked
    )



def discover_footprint_dir(explicit: str | None = None) -> Path:
    # An explicit path is authoritative; never silently substitute another library.
    if explicit:
        path = Path(explicit)
        if path.is_dir() and any(path.glob('*.pretty')):
            return path.resolve()
        raise FileNotFoundError(
            f'Explicit KiCad footprint directory is missing or invalid: {path}'
        )

    candidates: list[Path] = []
    for env_name in ('KICAD_FOOTPRINT_DIR', 'KISYSMOD'):
        env = os.environ.get(env_name)
        if env:
            candidates.append(Path(env))

    pf = os.environ.get('ProgramFiles', r'C:\Program Files')
    candidates.extend([
        Path(pf) / 'KiCad' / '9.0' / 'share' / 'kicad' / 'footprints',
        Path(pf) / 'KiCad' / '10.0' / 'share' / 'kicad' / 'footprints',
        Path(pf) / 'KiCad' / '8.0' / 'share' / 'kicad' / 'footprints',
        Path('/usr/share/kicad/footprints'),
        Path('/usr/local/share/kicad/footprints'),
        Path('/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints'),
    ])

    for path in candidates:
        if path.is_dir() and any(path.glob('*.pretty')):
            return path.resolve()

    looked = '\n  '.join(str(path) for path in candidates)
    raise FileNotFoundError(
        'Cannot find KiCad footprint directory. Supply --footprint-dir. Checked:\n  '
        + looked
    )


def discover_cli(explicit: str | None = None) -> str | None:
    explicit = explicit or os.environ.get('KICAD_CLI')
    if explicit:
        return explicit
    found = shutil.which('kicad-cli') or shutil.which('kicad-cli.exe')
    if found:
        return found
    pf = Path(os.environ.get('ProgramFiles', r'C:\Program Files'))
    for version in ('9.0', '10.0', '8.0'):  # prefer the KiCad 9.x validation target
        candidate = pf / 'KiCad' / version / 'bin' / 'kicad-cli.exe'
        if candidate.is_file():
            return str(candidate)
    return None


def symbol_definitions(lib_dir: Path, requested: str, seen: set[str] | None = None) -> list[list[Any]]:
    """Return a flattened list of embedded definitions, bases before children."""
    if seen is None:
        seen = set()
    if requested in seen:
        raise ValueError(f'cyclic symbol inheritance for {requested}')
    seen = set(seen)
    seen.add(requested)
    library, _, symbol_name = requested.partition(':')
    if not library or not symbol_name:
        raise ValueError(f'symbol must be Library:Name, got {requested!r}')
    path = lib_dir / f'{library}.kicad_sym'
    if not path.is_file():
        raise FileNotFoundError(f'Library file not found for {requested}: {path}')
    root = parse_sexpr(path.read_text(encoding='utf-8'))
    for definition in direct(root, 'symbol'):
        if len(definition) >= 2 and str(definition[1]) == symbol_name:
            clone = json_roundtrip_tree(definition)
            definitions: list[list[Any]] = []
            parent_name = text_at(clone, 'extends')
            if parent_name:
                # Parent symbols are looked up within the same library file.
                definitions.extend(symbol_definitions(lib_dir, f'{library}:{parent_name}', seen))
            clone[1] = Quoted(requested)
            definitions.append(clone)
            return definitions
    raise KeyError(f'{requested} not found in {path.name}')


def json_roundtrip_tree(node: Any) -> Any:
    if isinstance(node, list):
        return [json_roundtrip_tree(item) for item in node]
    if isinstance(node, Quoted):
        return Quoted(str(node))
    if isinstance(node, Atom):
        return Atom(str(node))
    return node


def symbol_properties(definition: list[Any]) -> dict[str, str]:
    props: dict[str, str] = {}
    for prop in direct(definition, 'property'):
        if len(prop) >= 3:
            props[str(prop[1])] = str(prop[2])
    return props


def symbol_pin_summary(definition: list[Any]) -> list[dict[str, str]]:
    pins: list[dict[str, str]] = []
    # Pins are nested in per-unit child symbol definitions; collect all pin blocks.
    def walk(node: Any) -> None:
        if not isinstance(node, list):
            return
        if head(node) == 'pin':
            number = one(node, 'number')
            name = one(node, 'name')
            pins.append({
                'electrical_type': str(node[1]) if len(node) > 1 else '',
                'shape': str(node[2]) if len(node) > 2 else '',
                'name': str(name[1]) if name and len(name) > 1 else '',
                'number': str(number[1]) if number and len(number) > 1 else '',
            })
            return
        for child in node:
            walk(child)
    walk(definition)
    # Multiple units/alternate body styles may duplicate pin numbers. Preserve
    # distinct (number,name,type) triples, don't falsely merge multi-unit pins.
    uniq: dict[tuple[str, str, str], dict[str, str]] = {}
    for p in pins:
        uniq[(p['number'], p['name'], p['electrical_type'])] = p
    return list(uniq.values())


def build_catalog(symbol_dir: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    paths = sorted(symbol_dir.glob('*.kicad_sym'))
    for idx, path in enumerate(paths, 1):
        try:
            root = parse_sexpr(path.read_text(encoding='utf-8'))
        except (OSError, UnicodeError, SExprError) as exc:
            print(f'WARN: skipped {path.name}: {exc}', file=sys.stderr)
            continue
        library = path.stem
        for sym in direct(root, 'symbol'):
            if len(sym) < 2:
                continue
            name = str(sym[1])
            properties = symbol_properties(sym)
            rows.append({
                'id': f'{library}:{name}',
                'library': library,
                'name': name,
                'reference_prefix': properties.get('Reference', '').strip(),
                'value': properties.get('Value', '').strip(),
                'footprint_filters': properties.get('ki_fp_filters', '').strip(),
                'description': properties.get('Description', '').strip(),
                'keywords': properties.get('ki_keywords', '').strip(),
                'extends': text_at(sym, 'extends'),
                'pins': symbol_pin_summary(sym),
                'source_file': path.name,
            })
        if idx % 40 == 0:
            print(f'Indexed {idx}/{len(paths)} libraries ...', file=sys.stderr)
    return rows


def cmd_catalog(args: argparse.Namespace) -> int:
    symbol_dir = discover_symbol_dir(args.symbol_dir)
    print(f'KiCad symbol directory: {symbol_dir}')
    rows = build_catalog(symbol_dir)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        'schema_version': 1,
        'source': 'installed KiCad .kicad_sym libraries',
        'symbol_dir': str(symbol_dir),
        'symbol_count': len(rows),
        'symbols': rows,
    }
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Wrote {len(rows):,} symbols -> {out.resolve()}')
    if args.csv:
        csv_path = Path(args.csv)
        csv_path.parent.mkdir(parents=True, exist_ok=True)
        with csv_path.open('w', newline='', encoding='utf-8-sig') as handle:
            writer = csv.DictWriter(handle, fieldnames=['id', 'library', 'name', 'reference_prefix', 'value', 'footprint_filters', 'description', 'keywords', 'extends', 'source_file'])
            writer.writeheader()
            for row in rows:
                writer.writerow({k: row.get(k, '') for k in writer.fieldnames})
        print(f'Wrote CSV -> {csv_path.resolve()}')
    return 0



def build_footprint_catalog(footprint_dir: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    libraries = sorted(p for p in footprint_dir.glob('*.pretty') if p.is_dir())
    all_mods = [mod for lib in libraries for mod in lib.rglob('*.kicad_mod')]
    for index, path in enumerate(sorted(all_mods), 1):
        try:
            root = parse_sexpr(path.read_text(encoding='utf-8'))
        except (OSError, UnicodeError, SExprError) as exc:
            print(f'WARN: skipped footprint {path}: {exc}', file=sys.stderr)
            continue
        if head(root) not in ('footprint', 'module'):
            continue
        library = path.parent.name[:-len('.pretty')] if path.parent.name.endswith('.pretty') else path.parent.name
        name = str(root[1]) if len(root) > 1 else path.stem
        tags_node = one(root, 'tags')
        rows.append({
            'id': f'{library}:{name}',
            'library': library,
            'name': name,
            'description': text_at(root, 'descr'),
            'tags': str(tags_node[1]) if tags_node and len(tags_node) > 1 else '',
            'pad_count': len(direct(root, 'pad')),
            'model_count': len(direct(root, 'model')),
            'source_file': str(path.relative_to(footprint_dir)),
        })
        if index % 500 == 0:
            print(f'Indexed {index}/{len(all_mods)} footprints ...', file=sys.stderr)
    return rows


def cmd_footprints(args: argparse.Namespace) -> int:
    footprint_dir = discover_footprint_dir(args.footprint_dir)
    print(f'KiCad footprint directory: {footprint_dir}')
    rows = build_footprint_catalog(footprint_dir)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        'schema_version': 1,
        'source': 'installed KiCad .pretty footprint libraries',
        'footprint_dir': str(footprint_dir),
        'footprint_count': len(rows),
        'footprints': rows,
    }
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Wrote {len(rows):,} footprints -> {out.resolve()}')
    if args.csv:
        csv_path = Path(args.csv)
        csv_path.parent.mkdir(parents=True, exist_ok=True)
        with csv_path.open('w', newline='', encoding='utf-8-sig') as handle:
            writer = csv.DictWriter(handle, fieldnames=['id', 'library', 'name', 'description', 'tags', 'pad_count', 'model_count', 'source_file'])
            writer.writeheader()
            writer.writerows(rows)
        print(f'Wrote CSV -> {csv_path.resolve()}')
    return 0


def cmd_search_footprint(args: argparse.Namespace) -> int:
    terms = [term.casefold() for term in args.query if term]
    if not terms:
        raise ValueError('provide one or more footprint search terms')
    path = Path(args.catalog)
    raw = json.loads(path.read_text(encoding='utf-8'))
    rows = raw.get('footprints') if isinstance(raw, dict) else None
    if not isinstance(rows, list):
        raise ValueError(f'{path} is not a hwc_kicad footprint catalogue JSON')
    matches = []
    for row in rows:
        haystack = ' '.join(str(row.get(key, '')) for key in ('id', 'description', 'tags', 'name', 'library')).casefold()
        if all(term in haystack for term in terms):
            matches.append(row)
    matches.sort(key=lambda row: str(row.get('id', '')).casefold())
    for row in matches[:args.limit]:
        print(f"{row.get('id',''):<54} pads={row.get('pad_count',0):<3} {row.get('description') or row.get('tags','')}")
    print(f'{len(matches)} matches (showing {min(len(matches), args.limit)})')
    return 0 if matches else 1


def load_catalog(path: Path) -> list[dict[str, Any]]:
    raw = json.loads(path.read_text(encoding='utf-8'))
    symbols = raw.get('symbols') if isinstance(raw, dict) else None
    if not isinstance(symbols, list):
        raise ValueError(f'{path} is not a hwc_kicad catalog JSON')
    return symbols


def cmd_search(args: argparse.Namespace) -> int:
    terms = [t.casefold() for t in args.query if t]
    if not terms:
        raise ValueError('provide one or more search terms')
    symbols = load_catalog(Path(args.catalog))
    matches = []
    for row in symbols:
        text = ' '.join(str(row.get(k, '')) for k in ('id', 'description', 'keywords', 'value', 'footprint_filters')).casefold()
        # AND semantics for more precise command-line search.
        if all(term in text for term in terms):
            matches.append(row)
    matches.sort(key=lambda r: (str(r.get('id', '')).casefold()))
    for row in matches[:args.limit]:
        desc = row.get('description') or row.get('keywords') or ''
        print(f"{row.get('id',''):<42} ref={row.get('reference_prefix',''):<5} value={row.get('value',''):<18} {desc}")
    print(f'{len(matches)} matches (showing {min(len(matches), args.limit)})')
    return 0 if matches else 1


def values_from_prompt(prompt: str) -> tuple[float | None, float | None, str, str]:
    """Extract V-in/target and two resistor tokens from current V1 prompt forms."""
    text = prompt.replace(',', ' ').replace('Ω', 'ohm').replace('Ω', 'ohm')
    voltages: list[float] = []
    resistances: list[tuple[float, str]] = []
    num = r'(?<![A-Za-z0-9_])([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)'
    for match in re.finditer(num + r'\s*(?:V|volt|volts)\b', text, re.IGNORECASE):
        voltages.append(float(match.group(1)))
    rx = re.compile(num + r'\s*(k(?:ilo)?\s*(?:ohm|ohms|Ω)?|M(?:ega)?\s*(?:ohm|ohms|Ω)?|m\s*(?:ohm|ohms|Ω)|ohms?|Ω)?\b', re.IGNORECASE)
    for match in rx.finditer(text):
        raw = match.group(1)
        unit = (match.group(2) or '').strip().casefold().replace(' ', '')
        # Only consider explicit resistance suffixes. Exclude voltage values.
        if unit.startswith('k'):
            value = float(raw) * 1e3
        elif unit.startswith('m') and not unit.startswith('mega'):
            value = float(raw) * 1e-3
        elif unit.startswith('mega') or (match.group(2) or '').startswith('M'):
            value = float(raw) * 1e6
        elif unit.startswith('ohm') or unit == 'ω':
            value = float(raw)
        else:
            continue
        resistances.append((value, format_resistance(float(raw), match.group(2) or 'ohm')))
    # de-duplicate tokens that overlapped odd unicode/text patterns
    unique_r: list[tuple[float, str]] = []
    for item in resistances:
        if item not in unique_r:
            unique_r.append(item)
    vin = voltages[0] if len(voltages) > 0 else None
    vtarget = voltages[1] if len(voltages) > 1 else None
    r1 = unique_r[0][1] if len(unique_r) > 0 else ''
    r2 = unique_r[1][1] if len(unique_r) > 1 else ''
    return vin, vtarget, r1, r2


def format_resistance(raw: float, unit: str) -> str:
    u = unit.strip()
    if u.startswith(('k', 'K')):
        return f'{raw:g}k'
    if u.startswith(('M', 'mega', 'Mega')):
        return f'{raw:g}M'
    if u.startswith('m'):
        return f'{raw:g}m'
    return f'{raw:g}'


def fmt_num(n: float) -> str:
    return f'{n:.6f}'.rstrip('0').rstrip('.') if n != int(n) else str(int(n))


def mk_effects(hide: bool = False, justify: str | None = None) -> list[Any]:
    fx: list[Any] = [Atom('effects'), [Atom('font'), [Atom('size'), Atom('1.27'), Atom('1.27')]]]
    if justify:
        fx.append([Atom('justify'), Atom(justify)])
    if hide:
        fx.append([Atom('hide'), Atom('yes')])
    return fx


def make_property(name: str, value: str, x: float, y: float, angle: int, hide: bool = False) -> list[Any]:
    return [Atom('property'), Quoted(name), Quoted(value), [Atom('at'), Atom(fmt_num(x)), Atom(fmt_num(y)), Atom(str(angle))], mk_effects(hide)]


def make_label(name: str, x: float, y: float) -> list[Any]:
    return [Atom('label'), Quoted(name), [Atom('at'), Atom(fmt_num(x)), Atom(fmt_num(y)), Atom('0')],
            [Atom('effects'), [Atom('font'), [Atom('size'), Atom('1.27'), Atom('1.27')]], [Atom('justify'), Atom('left'), Atom('bottom')]],
            [Atom('uuid'), Quoted(str(uuid.uuid4()))]]


def make_text(value: str, x: float, y: float) -> list[Any]:
    return [Atom('text'), Quoted(value), [Atom('at'), Atom(fmt_num(x)), Atom(fmt_num(y)), Atom('0')],
            [Atom('effects'), [Atom('font'), [Atom('size'), Atom('1.27'), Atom('1.27')]], [Atom('justify'), Atom('left'), Atom('bottom')]],
            [Atom('uuid'), Quoted(str(uuid.uuid4()))]]


def make_wire(x1: float, y1: float, x2: float, y2: float) -> list[Any]:
    return [Atom('wire'), [Atom('pts'), [Atom('xy'), Atom(fmt_num(x1)), Atom(fmt_num(y1))], [Atom('xy'), Atom(fmt_num(x2)), Atom(fmt_num(y2))]],
            [Atom('stroke'), [Atom('width'), Atom('0')], [Atom('type'), Atom('default')]], [Atom('uuid'), Quoted(str(uuid.uuid4()))]]


def make_resistor(ref: str, value: str, x: float, y: float, root_uuid: str, project_name: str, footprint: str) -> list[Any]:
    # Device:R has two pins at local y +/-3.81 mm. Default orientation 0.
    return [
        Atom('symbol'), [Atom('lib_id'), Quoted('Device:R')], [Atom('at'), Atom(fmt_num(x)), Atom(fmt_num(y)), Atom('0')], [Atom('unit'), Atom('1')],
        [Atom('exclude_from_sim'), Atom('no')], [Atom('in_bom'), Atom('yes')], [Atom('on_board'), Atom('yes')], [Atom('dnp'), Atom('no')],
        [Atom('fields_autoplaced'), Atom('yes')], [Atom('uuid'), Quoted(str(uuid.uuid4()))],
        make_property('Reference', ref, x + 4.5, y - 1.27, 0),
        make_property('Value', value, x + 4.5, y + 1.27, 0),
        make_property('Footprint', footprint, x, y, 0, True),
        make_property('Datasheet', '~', x, y, 0, True),
        make_property('Description', 'Resistor', x, y, 0, True),
        [Atom('pin'), Quoted('1'), [Atom('uuid'), Quoted(str(uuid.uuid4()))]],
        [Atom('pin'), Quoted('2'), [Atom('uuid'), Quoted(str(uuid.uuid4()))]],
        [Atom('instances'), [Atom('project'), Quoted(project_name), [Atom('path'), Quoted('/' + root_uuid), [Atom('reference'), Quoted(ref)], [Atom('unit'), Atom('1')]]]],
    ]


def generate_divider(prompt: str, out_path: Path, symbol_dir: Path, project: str | None = None, footprint: str = '') -> None:
    lower = prompt.casefold()
    if 'resistor' not in lower or 'divider' not in lower:
        raise ValueError('V1 prompt parser currently supports a resistor-divider request; include the words "resistor divider".')
    vin, target, r1_value, r2_value = values_from_prompt(prompt)
    if vin is None or target is None:
        raise ValueError('Need two voltage quantities in the prompt, e.g. "5 V to 3.3 V".')
    if not r1_value or not r2_value:
        raise ValueError('Need two resistor values with units, e.g. "10k ohm and 20k ohm".')

    symbol_sources = symbol_definitions(symbol_dir, 'Device:R')
    embedded = [Atom('lib_symbols')]
    embedded.extend(symbol_sources)
    root_uuid = str(uuid.uuid4())
    project_name = project or out_path.stem
    root: list[Any] = [
        Atom('kicad_sch'), [Atom('version'), Atom('20250114')], [Atom('generator'), Quoted('hardware_compiler')],
        [Atom('generator_version'), Quoted('0.2.0')], [Atom('uuid'), Quoted(root_uuid)], [Atom('paper'), Quoted('A4')], embedded,
    ]
    # Divider topology, vertically stacked. Local labels create named nets.
    x, y1, y2 = 100.0, 60.0, 85.0
    pin_top_r1, pin_bottom_r1 = y1 - 3.81, y1 + 3.81
    pin_top_r2, pin_bottom_r2 = y2 - 3.81, y2 + 3.81
    root.append(make_text(f'Resistor divider | VIN = {fmt_num(vin)} V | target VOUT = {fmt_num(target)} V', 90.0, 42.0))
    root.append(make_wire(x, pin_bottom_r1, x, pin_top_r2))
    root.append(make_label('VIN', x, pin_top_r1))
    root.append(make_label('VOUT', x, (pin_bottom_r1 + pin_top_r2) / 2))
    root.append(make_label('GND', x, pin_bottom_r2))
    root.append(make_resistor('R1', r1_value, x, y1, root_uuid, project_name, footprint))
    root.append(make_resistor('R2', r2_value, x, y2, root_uuid, project_name, footprint))
    root.extend([
        [Atom('sheet_instances'), [Atom('path'), Quoted('/'), [Atom('page'), Quoted('1')]]],
        [Atom('embedded_fonts'), Atom('no')],
    ])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(dump_sexpr(root) + '\n', encoding='utf-8')
    print(f'Generated {out_path.resolve()}')
    print(f'  topology: resistor divider | VIN={fmt_num(vin)} V | target={fmt_num(target)} V')
    print(f'  R1={r1_value} (VIN -> VOUT), R2={r2_value} (VOUT -> GND)')
    print('  Note: VOUT is a named net; this v1 bridge does not yet synthesize/route arbitrary circuits.')


def cmd_generate(args: argparse.Namespace) -> int:
    symbol_dir = discover_symbol_dir(args.symbol_dir)
    generate_divider(args.prompt, Path(args.out), symbol_dir, args.project, args.footprint)
    return 0


def run_cli(cli: str, command: list[str], *, must_succeed: bool = True) -> subprocess.CompletedProcess[str]:
    argv = [cli] + command
    print('> ' + subprocess.list2cmdline(argv))
    cp = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, errors='replace')
    if cp.stdout:
        print(cp.stdout.rstrip())
    if must_succeed and cp.returncode != 0:
        raise subprocess.CalledProcessError(cp.returncode, argv, output=cp.stdout)
    return cp


def cmd_validate(args: argparse.Namespace) -> int:
    cli = discover_cli(args.kicad_cli)
    if not cli:
        raise FileNotFoundError('Cannot find kicad-cli. Add it to PATH or pass --kicad-cli "C:\\Program Files\\KiCad\\9.0\\bin\\kicad-cli.exe".')
    sch = Path(args.schematic).resolve()
    if not sch.is_file():
        raise FileNotFoundError(f'schematic not found: {sch}')
    if args.version:
        run_cli(cli, ['version'])
    outdir = Path(args.outdir).resolve() if args.outdir else sch.parent / 'kicad_validation'
    outdir.mkdir(parents=True, exist_ok=True)
    erc_out = outdir / (sch.stem + '_erc.rpt')
    erc = run_cli(cli, ['sch', 'erc', '--output', str(erc_out), str(sch)], must_succeed=False)
    print(f'ERC exit code: {erc.returncode}; report path: {erc_out}')
    svg_dir = outdir / 'svg'
    svg_dir.mkdir(parents=True, exist_ok=True)
    svg = run_cli(cli, ['sch', 'export', 'svg', '--output', str(svg_dir), str(sch)], must_succeed=False)
    print(f'SVG export exit code: {svg.returncode}; output dir: {svg_dir}')
    if erc.returncode != 0 or svg.returncode != 0:
        return 2
    print('KiCad CLI read/export completed. Review ERC report for actual electrical violations.')
    return 0


def cmd_doctor(args: argparse.Namespace) -> int:
    symdir = None
    try:
        symdir = discover_symbol_dir(args.symbol_dir)
    except FileNotFoundError as exc:
        print(f'SYMBOL LIBRARIES: NOT FOUND\n{exc}')
    else:
        print(f'SYMBOL LIBRARIES: {symdir}')
        print(f'  .kicad_sym files: {len(list(symdir.glob("*.kicad_sym")))}')
    try:
        footprint_dir = discover_footprint_dir(args.footprint_dir)
    except FileNotFoundError as exc:
        print(f'FOOTPRINT LIBRARIES: NOT FOUND\n{exc}')
    else:
        print(f'FOOTPRINT LIBRARIES: {footprint_dir}')
        print(f'  .pretty libraries: {len(list(footprint_dir.glob("*.pretty")))}')
    cli = discover_cli(args.kicad_cli)
    if cli:
        print(f'KICAD CLI: {cli}')
        try:
            run_cli(cli, ['version'])
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f'CLI check failed: {exc}')
            return 2
    else:
        print('KICAD CLI: NOT FOUND (set PATH or pass --kicad-cli)')
        return 2
    return 0 if symdir else 2


def set_symbol_property(definition: list[Any], key: str, value: str, *, add_if_missing: bool = True) -> None:
    for prop in direct(definition, 'property'):
        if len(prop) >= 3 and str(prop[1]) == key:
            prop[2] = Quoted(value)
            return
    if add_if_missing:
        # User-defined properties are hidden by default so they carry metadata
        # without making the symbol cluttered when placed on a schematic.
        definition.append(make_property(key, value, 0.0, 0.0, 0, True))


def cmd_custom_symbol(args: argparse.Namespace) -> int:
    if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.+-]*', args.name):
        raise ValueError('custom symbol name must use letters, digits, underscore, dot, plus or hyphen')
    symbol_dir = discover_symbol_dir(args.symbol_dir)
    defs = [json_roundtrip_tree(item) for item in symbol_definitions(symbol_dir, args.source)]
    # Library files use local symbol names, whereas embedded schematics use
    # the fully qualified Library:Name convention.
    for item in defs:
        item[1] = Quoted(str(item[1]).split(':')[-1])
    definition = defs[-1]
    definition[1] = Quoted(args.name)
    set_symbol_property(definition, 'Reference', args.reference)
    if args.value is not None:
        set_symbol_property(definition, 'Value', args.value)
    if args.footprint is not None:
        set_symbol_property(definition, 'Footprint', args.footprint)
    if args.description is not None:
        set_symbol_property(definition, 'Description', args.description)
    if args.datasheet is not None:
        set_symbol_property(definition, 'Datasheet', args.datasheet)
    if args.manufacturer:
        set_symbol_property(definition, 'Manufacturer', args.manufacturer)
    if args.mpn:
        set_symbol_property(definition, 'MPN', args.mpn)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    existing: list[Any] = []
    if out.is_file():
        old = parse_sexpr(out.read_text(encoding='utf-8'))
        if head(old) != 'kicad_symbol_lib':
            raise ValueError(f'{out} exists but is not a KiCad symbol library')
        existing = [json_roundtrip_tree(x) for x in direct(old, 'symbol')]
        # Replace an existing custom symbol by name; otherwise append.
        existing = [x for x in existing if len(x) < 2 or str(x[1]) != args.name]
    root: list[Any] = [Atom('kicad_symbol_lib'), [Atom('version'), Atom('20250114')], [Atom('generator'), Quoted('hardware_compiler')]]
    root.extend(existing)
    root.extend(defs)
    out.write_text(dump_sexpr(root) + '\n', encoding='utf-8')
    print(f'Custom symbol written: {out.resolve()}')
    print(f'  source: {args.source} -> {out.stem}:{args.name}')
    print(f'  Reference={args.reference} Value={args.value or symbol_properties(definition).get("Value", "")} Footprint={args.footprint or symbol_properties(definition).get("Footprint", "")}')
    if args.manufacturer or args.mpn:
        print(f'  Manufacturer={args.manufacturer or "(unset)"} MPN={args.mpn or "(unset)"}')
    return 0


ROLE_PREFIXES = {
    'resistor': 'R', 'capacitor': 'C', 'inductor': 'L', 'diode': 'D', 'led': 'D',
    'transistor': 'Q', 'mosfet': 'Q', 'integrated-circuit': 'U', 'ic': 'U',
    'connector': 'J', 'switch': 'SW', 'fuse': 'F', 'crystal': 'Y', 'testpoint': 'TP',
    'voltage-source': 'V', 'battery': 'BT', 'motor': 'M',
}


def cmd_name(args: argparse.Namespace) -> int:
    if args.index < 1:
        raise ValueError('reference index must be a positive integer')
    role = args.role.casefold().replace('_', '-').replace(' ', '-')
    prefix = ROLE_PREFIXES.get(role)
    if not prefix:
        raise ValueError(f'Unknown role {args.role!r}. Known roles: {", ".join(sorted(ROLE_PREFIXES))}')
    print(f'{prefix}{args.index}')
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog='hwc-kicad', description='Hardware Compiler / KiCad symbol catalogue and schematic bridge')
    sub = parser.add_subparsers(dest='command', required=True)

    p = sub.add_parser('doctor', help='find KiCad symbol libraries and kicad-cli')
    p.add_argument('--symbol-dir')
    p.add_argument('--footprint-dir')
    p.add_argument('--kicad-cli')
    p.set_defaults(func=cmd_doctor)

    p = sub.add_parser('catalog', help='index every top-level symbol from installed KiCad libraries')
    p.add_argument('--symbol-dir')
    p.add_argument('--out', default='build/kicad_catalog.json')
    p.add_argument('--csv', default='build/kicad_catalog.csv')
    p.set_defaults(func=cmd_catalog)

    p = sub.add_parser('footprints', help='index installed KiCad .pretty footprint libraries')
    p.add_argument('--footprint-dir')
    p.add_argument('--out', default='build/kicad_footprints.json')
    p.add_argument('--csv', default='build/kicad_footprints.csv')
    p.set_defaults(func=cmd_footprints)

    p = sub.add_parser('search-footprint', help='search a generated KiCad footprint catalogue')
    p.add_argument('query', nargs='+')
    p.add_argument('--catalog', default='build/kicad_footprints.json')
    p.add_argument('--limit', type=int, default=40)
    p.set_defaults(func=cmd_search_footprint)

    p = sub.add_parser('search', help='search a generated catalogue by library/name/keywords/description')
    p.add_argument('query', nargs='+')
    p.add_argument('--catalog', default='build/kicad_catalog.json')
    p.add_argument('--limit', type=int, default=40)
    p.set_defaults(func=cmd_search)

    p = sub.add_parser('generate', help='generate a KiCad schematic from a V1 resistor-divider prompt')
    p.add_argument('--prompt', required=True, help='for example: "resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm"')
    p.add_argument('--out', default='out/resistor_divider.kicad_sch')
    p.add_argument('--project', default=None)
    p.add_argument('--footprint', default='')
    p.add_argument('--symbol-dir')
    p.set_defaults(func=cmd_generate)

    p = sub.add_parser('validate', help='run kicad-cli ERC and SVG export against a .kicad_sch')
    p.add_argument('schematic')
    p.add_argument('--kicad-cli')
    p.add_argument('--outdir')
    p.add_argument('--version', action='store_true')
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser('custom-symbol', help='clone an installed symbol into your own reusable .kicad_sym library')
    p.add_argument('--source', required=True, help='installed source symbol, e.g. Device:R')
    p.add_argument('--name', required=True, help='name inside your custom library, e.g. Resistor_10k_0603')
    p.add_argument('--out', default='custom/CustomParts.kicad_sym')
    p.add_argument('--reference', default='R')
    p.add_argument('--value', default=None)
    p.add_argument('--footprint', default=None)
    p.add_argument('--description', default=None)
    p.add_argument('--datasheet', default=None)
    p.add_argument('--manufacturer', default=None)
    p.add_argument('--mpn', default=None)
    p.add_argument('--symbol-dir')
    p.set_defaults(func=cmd_custom_symbol)

    p = sub.add_parser('name', help='assign standard schematic reference designators')
    p.add_argument('--role', required=True, help='resistor, capacitor, ic, connector, transistor, ...')
    p.add_argument('--index', type=int, required=True)
    p.set_defaults(func=cmd_name)

    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except (ValueError, OSError, KeyError, SExprError, subprocess.CalledProcessError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
