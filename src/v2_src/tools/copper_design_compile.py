#!/usr/bin/env python3
"""Compile copper.design-intent.v1 JSON into native KiCad 9 project files.

This is a deliberately strict first-generation compiler. It uses the user's
installed KiCad symbol/footprint libraries as source assets; it does not ask an
LLM to author KiCad S-expressions. Only single-unit symbols and PCB footprints
with explicit symbol-pin-to-footprint-pad mappings are accepted.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import sys
import tempfile
import subprocess
import uuid
from pathlib import Path
from typing import Any, Iterable

SCHEMA = "copper.design-intent"
SCHEMA_VERSION = 1


class Quoted(str):
    """S-expression string token (as distinct from an unquoted atom)."""


class CompileError(Exception):
    pass


def sexpr_tokens(text: str) -> list[str]:
    tokens: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        if c == ';':
            while i < n and text[i] not in '\r\n':
                i += 1
            continue
        if c in '()':
            tokens.append(c)
            i += 1
            continue
        if c == '"':
            i += 1
            out: list[str] = []
            while i < n:
                c = text[i]
                if c == '"':
                    i += 1
                    break
                if c == '\\':
                    i += 1
                    if i >= n:
                        raise CompileError("unterminated escape in KiCad library file")
                    esc = text[i]
                    out.append({'n': '\n', 'r': '\r', 't': '\t'}.get(esc, esc))
                    i += 1
                else:
                    out.append(c)
                    i += 1
            else:
                raise CompileError("unterminated quoted string in KiCad library file")
            tokens.append('Q:' + ''.join(out))
            continue
        start = i
        while i < n and not text[i].isspace() and text[i] not in '();':
            i += 1
        tokens.append(text[start:i])
    return tokens


def parse_sexpr(text: str) -> list[Any]:
    tokens = sexpr_tokens(text)
    stack: list[list[Any]] = []
    root: list[Any] | None = None
    for tok in tokens:
        if tok == '(':
            node: list[Any] = []
            if stack:
                stack[-1].append(node)
            elif root is None:
                root = node
            else:
                raise CompileError("multiple top-level S-expressions")
            stack.append(node)
        elif tok == ')':
            if not stack:
                raise CompileError("unbalanced closing parenthesis in S-expression")
            stack.pop()
        else:
            if not stack:
                raise CompileError("atom outside S-expression")
            stack[-1].append(Quoted(tok[2:]) if tok.startswith('Q:') else tok)
    if stack or root is None:
        raise CompileError("incomplete S-expression")
    return root


def quote(value: Any) -> str:
    s = str(value)
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '\\r') + '"'


def atom(value: Any) -> str:
    if isinstance(value, Quoted):
        return quote(value)
    if isinstance(value, bool):
        return 'yes' if value else 'no'
    if isinstance(value, (int, float)):
        return format(value, '.6f').rstrip('0').rstrip('.') if isinstance(value, float) else str(value)
    s = str(value)
    if not s or any(ch.isspace() or ch in '()";' for ch in s):
        return quote(s)
    return s


def dump_sexpr(node: Any, depth: int = 0) -> str:
    if not isinstance(node, list):
        return atom(node)
    if not node:
        return '()'
    head = '(' + dump_sexpr(node[0], depth + 1)
    pieces = [head]
    for child in node[1:]:
        if isinstance(child, list):
            pieces.append('\n' + '  ' * (depth + 1) + dump_sexpr(child, depth + 1))
        else:
            pieces.append(' ' + dump_sexpr(child, depth + 1))
    return ''.join(pieces) + ')'


def is_list(node: Any, head: str | None = None) -> bool:
    return isinstance(node, list) and bool(node) and (head is None or node[0] == head)


def children(node: list[Any], head: str) -> list[list[Any]]:
    return [x for x in node[1:] if is_list(x, head)]


def child(node: list[Any], head: str) -> list[Any] | None:
    return next((x for x in node[1:] if is_list(x, head)), None)


def walk(node: Any) -> Iterable[list[Any]]:
    if isinstance(node, list):
        yield node
        for x in node:
            if isinstance(x, list):
                yield from walk(x)


def deterministic_uuid(seed: str, label: str) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, seed + '::' + label))


def find_library_roots(args: argparse.Namespace) -> tuple[Path, Path]:
    candidates_symbols = [
        args.symbols_root,
        os.environ.get('KICAD_SYMBOL_DIR'),
        r'C:\Program Files\KiCad\9.0\share\kicad\symbols',
        r'C:\Program Files\KiCad\9.0\share\kicad\symbols',
        '/usr/share/kicad/symbols', '/usr/local/share/kicad/symbols',
    ]
    candidates_footprints = [
        args.footprints_root,
        os.environ.get('KICAD_FOOTPRINT_DIR'),
        r'C:\Program Files\KiCad\9.0\share\kicad\footprints',
        '/usr/share/kicad/footprints', '/usr/local/share/kicad/footprints',
    ]
    sym = next((Path(x) for x in candidates_symbols if x and Path(x).is_dir()), None)
    fp = next((Path(x) for x in candidates_footprints if x and Path(x).is_dir()), None)
    if sym is None:
        raise CompileError("KiCad symbol library root not found; pass --symbols-root")
    if fp is None:
        raise CompileError("KiCad footprint library root not found; pass --footprints-root")
    return sym, fp


def read_kicad_text(path: Path) -> str:
    """Read KiCad library/source text robustly on Windows.

    KiCad's distributed libraries are normally UTF-8, but locally installed or
    third-party library files may contain Windows-1252 bytes (for example 0xD7
    for the multiplication sign). Decode UTF-8 first, then use CP1252 as a
    deliberate legacy fallback. Do not use errors='replace': that silently
    changes library metadata and can make generated assets misleading.
    """
    raw = path.read_bytes()
    try:
        return raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        try:
            return raw.decode('cp1252')
        except UnicodeDecodeError as exc:
            raise CompileError(
                f"KiCad text file is neither valid UTF-8 nor Windows-1252: {path}; "
                f"decode failed near byte {exc.start}"
            ) from exc


def split_lib_id(value: str, label: str) -> tuple[str, str]:
    """Parse a fully qualified KiCad library ID.

    Natural-language prompts often put a sentence-ending period immediately
    after an ID (for example ``LED_SMD:LED_0603_1608Metric.``). Some models
    copy that punctuation into JSON. Remove exactly one terminal full stop
    before resolving the library entry; keep periods inside legitimate names.
    The canonical ID is rebuilt by callers as ``nickname:entry``.
    """
    if not isinstance(value, str) or value.count(':') != 1:
        raise CompileError(f"{label} must be a fully-qualified KiCad ID (Nickname:Entry)")
    nickname, entry = value.split(':', 1)
    if entry.endswith('.'):
        entry = entry[:-1]
    if (not nickname or not entry or entry.endswith('.') or
            any(x in nickname for x in ('..', '/', '\\')) or
            any(x in entry for x in ('..', '/', '\\'))):
        raise CompileError(f"unsafe or invalid {label}: {value!r}")
    if entry.lower().endswith('.kicad_mod') or entry.lower().endswith('.kicad_sym'):
        raise CompileError(f"{label} must be a library ID, not a filename: {value!r}")
    return nickname, entry


def load_symbol(symbol_root: Path, lib_id: str) -> tuple[list[Any], dict[str, tuple[float, float, float]]]:
    nickname, entry = split_lib_id(lib_id, 'symbol_id')
    path = symbol_root / f'{nickname}.kicad_sym'
    if not path.is_file():
        raise CompileError(f"symbol library not found: {path}")
    root = parse_sexpr(read_kicad_text(path))
    if not is_list(root, 'kicad_symbol_lib'):
        raise CompileError(f"not a KiCad symbol library: {path}")
    lib = next((x for x in root[1:] if is_list(x, 'symbol') and len(x) > 1 and x[1] == entry), None)
    if lib is None:
        raise CompileError(f"symbol {lib_id} not found in {path}")
    if child(lib, 'extends'):
        raise CompileError(f"derived symbol {lib_id} is not supported in compiler v1; choose its resolved base symbol")
    # KiCad groups electrical units as nested symbol entries such as
    # Foo_1_1, Foo_2_1, etc. The compiler places one instance with unit=1;
    # accepting pins from unit 2+ would create a misleading/incomplete symbol.
    for unit_symbol in children(lib, 'symbol'):
        if len(unit_symbol) < 2 or not isinstance(unit_symbol[1], str):
            continue
        suffix = re.fullmatch(re.escape(entry) + r'_(\d+)_(\d+)', str(unit_symbol[1]))
        if suffix and int(suffix.group(1)) > 1:
            raise CompileError(f"multi-unit symbol {lib_id} is not supported in compiler v1")
    embedded = copy.deepcopy(lib)
    embedded[1] = Quoted(lib_id)
    # Build an exact pin-number -> (x,y,angle) table from all body units.
    pin_positions: dict[str, tuple[float, float, float]] = {}
    for n in walk(lib):
        if not is_list(n, 'pin'):
            continue
        at = child(n, 'at')
        number_node = child(n, 'number')
        if not at or len(at) < 4 or not number_node or len(number_node) < 2:
            continue
        number = str(number_node[1])
        pos = (float(at[1]), float(at[2]), float(at[3]))
        if number in pin_positions and pin_positions[number] != pos:
            raise CompileError(f"symbol {lib_id} has multiple physical positions for pin {number}; multi-unit/stacked pins are not supported in compiler v1")
        pin_positions[number] = pos
    if not pin_positions:
        raise CompileError(f"symbol {lib_id} has no extractable physical pins")
    # Make library defaults explicit for generated instances.
    if not child(embedded, 'pin_numbers'):
        embedded.insert(2, ['pin_numbers', ['hide', 'yes']])
    if not child(embedded, 'pin_names'):
        embedded.insert(3, ['pin_names', ['offset', '1.016']])
    if not child(embedded, 'exclude_from_sim'):
        embedded.insert(4, ['exclude_from_sim', 'no'])
    if not child(embedded, 'in_bom'):
        embedded.insert(5, ['in_bom', 'yes'])
    if not child(embedded, 'on_board'):
        embedded.insert(6, ['on_board', 'yes'])
    return embedded, pin_positions


def load_footprint(footprint_root: Path, lib_id: str) -> tuple[list[Any], set[str]]:
    nickname, entry = split_lib_id(lib_id, 'footprint_id')
    path = footprint_root / f'{nickname}.pretty' / f'{entry}.kicad_mod'
    if not path.is_file():
        raise CompileError(f"footprint not found: {path}")
    root = parse_sexpr(read_kicad_text(path))
    if not is_list(root, 'footprint'):
        raise CompileError(f"not a KiCad footprint: {path}")
    pad_numbers: set[str] = set()
    for p in children(root, 'pad'):
        if len(p) < 2:
            continue
        number = str(p[1])
        if number in pad_numbers:
            raise CompileError(f"footprint {lib_id} has duplicate pad number {number}")
        pad_numbers.add(number)
    if not pad_numbers:
        raise CompileError(f"footprint {lib_id} contains no pads")
    return copy.deepcopy(root), pad_numbers


def sx_quote(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def build_ephemeral_catalogue(components: list[dict[str, Any]]) -> str:
    """Create unverified temporary PartCatalogue records so Copper's own
    deterministic resolver validates each symbol/footprint/pin-pad mapping.
    This is a transient adapter, not a new source-of-truth catalogue.
    """
    kind_map = {
        'R': 'R', 'C': 'C', 'L': 'L', 'D': 'D', 'Q': 'Q',
        'M': 'M', 'U': 'U', 'J': 'J', 'S': 'S',
    }
    lines = ['(copper_parts', '  (schema 1)']
    for comp in components:
        prefix = comp['reference'][0].upper()
        kind = kind_map.get(prefix)
        if not kind:
            raise CompileError(f"cannot map reference prefix {prefix!r} to a Copper PartKind; use R/C/L/D/Q/M/U/J/S references in compiler v1")
        part_id = 'intent_' + comp['reference'].lower()
        package = comp['footprint_id'].split(':', 1)[1]
        lines.extend([
            '  (part',
            f'    (id {sx_quote(part_id)})',
            f'    (kind {kind})',
            '    (manufacturer "UNVERIFIED-AI-INTENT")',
            f'    (mpn {sx_quote("COPPER-" + comp["reference"] + "-UNVERIFIED")})',
            f'    (package {sx_quote(package)})',
            f'    (description {sx_quote("AI-selected design-intent component; commercial identity is unverified")})',
            f'    (symbol {sx_quote(comp["symbol_id"])})',
            f'    (footprint {sx_quote(comp["footprint_id"])})',
            '    (identity_source (url "not-provided") (document "AI-generated design intent") (status "unverified"))',
            '    (pins',
        ])
        for index, pin in enumerate(comp['pins'], start=1):
            lines.append(f'      (pin {sx_quote("terminal_" + str(index))} {sx_quote(pin["symbol_pin"])} {sx_quote(pin["pad"])})')
        lines.extend(['    )', '  )'])
    lines.append(')')
    return '\n'.join(lines) + '\n'


def resolve_components_with_copper(components: list[dict[str, Any]],
                                   symbol_root: Path, footprint_root: Path,
                                   copper_cli: str | None, catalogue_text: str | None = None) -> dict[str, dict[str, Any]]:
    """Ask the existing C CLI resolver to verify each AI-selected library part.

    Returns saved `copper.design` resolution JSON objects indexed by refdes.
    If the Copper executable is not supplied, Python's native library checks
    still run, but the C resolver is explicitly skipped.
    """
    if not copper_cli:
        return {}
    cli = Path(copper_cli)
    if not cli.is_file():
        raise CompileError(f'Copper executable not found: {cli}')
    results: dict[str, dict[str, Any]] = {}
    with tempfile.TemporaryDirectory(prefix='copper-intent-resolution-') as td:
        temp = Path(td)
        catalogue = temp / 'intent_components.parts'
        catalogue.write_text(catalogue_text if catalogue_text is not None else build_ephemeral_catalogue(components), encoding='utf-8')
        for comp in components:
            part_id = 'intent_' + comp['reference'].lower()
            result_path = temp / f'{comp["reference"]}.resolution.json'
            command = [str(cli.resolve()), 'design', 'resolve-part', str(catalogue),
                       '--part-id', part_id, '--source', 'user',
                       '--symbol-root', str(symbol_root),
                       '--footprint-root', str(footprint_root), '--out', str(result_path)]
            proc = subprocess.run(command, text=True, capture_output=True)
            if proc.returncode != 0:
                detail = (proc.stdout + '\n' + proc.stderr).strip()
                raise CompileError(f"Copper resolver rejected {comp['reference']} (exit {proc.returncode}): {detail}")
            try:
                resolution = json.loads(result_path.read_text(encoding='utf-8'))
                resolved = resolution['components'][0]
                assets = resolved['assets']
                pin_map = assets['pin_mapping']
                if assets['symbol']['status'] != 'FOUND' or assets['footprint']['status'] != 'FOUND' or not pin_map['explicit_valid']:
                    raise CompileError(f"Copper resolver did not explicitly validate assets/pin map for {comp['reference']}")
            except (OSError, KeyError, IndexError, json.JSONDecodeError) as exc:
                raise CompileError(f"could not read Copper resolution result for {comp['reference']}: {exc}") from exc
            results[comp['reference']] = resolution
    return results


def validate_intent(data: Any) -> dict[str, Any]:
    if not isinstance(data, dict):
        raise CompileError('design intent root must be a JSON object')
    if data.get('schema') != SCHEMA or data.get('schema_version') != SCHEMA_VERSION:
        raise CompileError(f"expected {SCHEMA} version {SCHEMA_VERSION}")
    project = data.get('project')
    if not isinstance(project, dict):
        raise CompileError('project must be an object')
    name = project.get('name')
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,63}', name):
        raise CompileError('project.name must match [A-Za-z][A-Za-z0-9_-]{0,63}')
    board = project.get('board', {})
    if not isinstance(board, dict):
        raise CompileError('project.board must be an object')
    width = float(board.get('width_mm', 50.0))
    height = float(board.get('height_mm', 35.0))
    layers = int(board.get('layers', 2))
    if not (20 <= width <= 300 and 15 <= height <= 300):
        raise CompileError('board dimensions must be width 20..300 mm and height 15..300 mm')
    if layers != 2:
        raise CompileError('compiler v1 supports 2-layer boards only; 4-layer stackups are deferred until explicit stackup generation is implemented')
    components = data.get('components')
    if not isinstance(components, list) or not components or len(components) > 64:
        raise CompileError('components must contain between 1 and 64 items')
    refs: set[str] = set()
    normalized: list[dict[str, Any]] = []
    for i, comp in enumerate(components):
        if not isinstance(comp, dict):
            raise CompileError(f'components[{i}] must be an object')
        ref, value = comp.get('reference'), comp.get('value')
        if not isinstance(ref, str) or not re.fullmatch(r'[A-Z][A-Z0-9]*', ref):
            raise CompileError(f'components[{i}].reference must be like R1, D1 or J1')
        if ref in refs:
            raise CompileError(f'duplicate component reference {ref}')
        refs.add(ref)
        if not isinstance(value, str) or not value.strip():
            raise CompileError(f'{ref}.value must be a non-empty string')
        symbol_id = comp.get('symbol_id')
        footprint_id = comp.get('footprint_id')
        symbol_nickname, symbol_entry = split_lib_id(symbol_id, f'{ref}.symbol_id')
        footprint_nickname, footprint_entry = split_lib_id(footprint_id, f'{ref}.footprint_id')
        # Store canonical IDs so symbol properties, resolver catalogues and
        # manifests never retain prose punctuation from the model response.
        symbol_id = f'{symbol_nickname}:{symbol_entry}'
        footprint_id = f'{footprint_nickname}:{footprint_entry}'
        pins = comp.get('pins')
        if not isinstance(pins, list) or not pins:
            raise CompileError(f'{ref}.pins must be a non-empty array')
        pin_items: list[dict[str, str]] = []
        seen_symbol: set[str] = set()
        seen_pad: set[str] = set()
        for j, pin in enumerate(pins):
            if not isinstance(pin, dict):
                raise CompileError(f'{ref}.pins[{j}] must be an object')
            sp = str(pin.get('symbol_pin', '')).strip()
            pad = str(pin.get('pad', '')).strip()
            net = pin.get('net')
            if not sp or not pad:
                raise CompileError(f'{ref}.pins[{j}] needs symbol_pin and pad')
            if sp in seen_symbol:
                raise CompileError(f'{ref} repeats symbol pin {sp}')
            if pad in seen_pad:
                raise CompileError(f'{ref} repeats footprint pad {pad}')
            seen_symbol.add(sp); seen_pad.add(pad)
            if net is not None and (not isinstance(net, str) or not re.fullmatch(r'[A-Za-z0-9_+./:-]{1,64}', net)):
                raise CompileError(f'{ref} pin {sp} has invalid net name')
            pin_items.append({'symbol_pin': sp, 'pad': pad, 'net': net if net is not None else ''})
        pos = comp.get('position', {})
        if pos is not None and not isinstance(pos, dict):
            raise CompileError(f'{ref}.position must be an object')
        normalized.append({'reference': ref, 'value': value.strip(), 'symbol_id': symbol_id,
                           'footprint_id': footprint_id, 'pins': pin_items, 'position': pos or {}})
    # Every electrically connected net must have at least two pins. NC is represented by net:null.
    net_counts: dict[str, int] = {}
    for comp in normalized:
        for pin in comp['pins']:
            if pin['net']:
                net_counts[pin['net']] = net_counts.get(pin['net'], 0) + 1
    for net, count in net_counts.items():
        if count < 2:
            raise CompileError(f'net {net!r} has only one endpoint; use net:null for an intentionally unconnected pin')
    return {'name': name, 'width': width, 'height': height, 'layers': layers, 'components': normalized}


def fmt_mm(v: float) -> str:
    return f'{v:.4f}'.rstrip('0').rstrip('.')


def make_property(name: str, value: str, x: float, y: float, angle: float,
                  *, hidden: bool = False, layer: str | None = None,
                  include_uuid: str | None = None) -> list[Any]:
    node: list[Any] = ['property', Quoted(name), Quoted(value),
                       ['at', fmt_mm(x), fmt_mm(y), fmt_mm(angle)]]
    if layer:
        node.append(['layer', Quoted(layer)])
    if include_uuid:
        node.append(['uuid', Quoted(include_uuid)])
    effects: list[Any] = ['effects', ['font', ['size', '1.0', '1.0'], ['thickness', '0.15']]]
    if hidden:
        effects.append(['hide', 'yes'])
    node.append(effects)
    return node


def make_schematic(name: str, seed: str, components: list[dict[str, Any]],
                   symbol_data: dict[str, tuple[list[Any], dict[str, tuple[float, float, float]]]]) -> tuple[str, str]:
    root_uuid = deterministic_uuid(seed, 'schematic-root')
    lib_symbols: list[Any] = ['lib_symbols']
    seen: set[str] = set()
    for comp in components:
        lib_id = comp['symbol_id']
        if lib_id not in seen:
            lib_symbols.append(copy.deepcopy(symbol_data[lib_id][0]))
            seen.add(lib_id)
    root: list[Any] = ['kicad_sch', ['version', '20250114'], ['generator', Quoted('eeschema')],
                       ['generator_version', Quoted('9.0')], ['uuid', Quoted(root_uuid)], ['paper', Quoted('A4')], lib_symbols]
    label_count = 0
    for idx, comp in enumerate(components):
        lib_id = comp['symbol_id']
        _, pins = symbol_data[lib_id]
        x0 = 45.72 + (idx % 4) * 38.1
        y0 = 50.8 + (idx // 4) * 25.4
        # Explicit schematic position overrides are deliberately deferred to v2.
        pin_map = {p['symbol_pin']: p for p in comp['pins']}
        missing = set(pins) - set(pin_map)
        extra = set(pin_map) - set(pins)
        if missing or extra:
            raise CompileError(f"{comp['reference']} pin mapping mismatch for {lib_id}: missing={sorted(missing)}, unknown={sorted(extra)}")
        inst_uuid = deterministic_uuid(seed, f"symbol:{comp['reference']}")
        sym: list[Any] = ['symbol', ['lib_id', Quoted(lib_id)], ['at', fmt_mm(x0), fmt_mm(y0), '0'],
                          ['unit', '1'], ['exclude_from_sim', 'no'], ['in_bom', 'yes'],
                          ['on_board', 'yes'], ['dnp', 'no'], ['fields_autoplaced', 'yes'],
                          ['uuid', Quoted(inst_uuid)]]
        sym.append(make_property('Reference', comp['reference'], x0 + 5.08, y0 - 3.81, 0))
        sym.append(make_property('Value', comp['value'], x0 + 5.08, y0 - 1.27, 0))
        sym.append(make_property('Footprint', comp['footprint_id'], x0, y0 + 5.08, 0, hidden=True))
        sym.append(make_property('Datasheet', '~', x0, y0, 0, hidden=True))
        # Every physical symbol pin gets its own stable UUID. Local labels at
        # pin anchors make same-sheet net connectivity explicit without model-written wires.
        for pin_no in sorted(pins, key=lambda s: (len(s), s)):
            sym.append(['pin', Quoted(pin_no), ['uuid', Quoted(deterministic_uuid(seed, f"symbol-pin:{comp['reference']}:{pin_no}"))]])
        sym.append(['instances', ['project', Quoted(name),
                                  ['path', Quoted('/' + root_uuid),
                                   ['reference', Quoted(comp['reference'])], ['unit', '1']]]])
        root.append(sym)
        for pin_no, (lx, ly, _ang) in pins.items():
            net = pin_map[pin_no]['net']
            px, py = x0 + lx, y0 - ly
            if not net:
                root.append(['no_connect', ['at', fmt_mm(px), fmt_mm(py)],
                             ['uuid', Quoted(deterministic_uuid(seed, f"no-connect:{comp['reference']}:{pin_no}"))]])
                continue
            label = ['label', Quoted(net), ['at', fmt_mm(px), fmt_mm(py), '0'],
                     ['effects', ['font', ['size', '1.0', '1.0']], ['justify', 'left', 'bottom']],
                     ['uuid', Quoted(deterministic_uuid(seed, f"label:{comp['reference']}:{pin_no}"))]]
            root.append(label)
            label_count += 1
    root.append(['sheet_instances', ['path', Quoted('/'), ['page', Quoted('1')]]])
    root.append(['embedded_fonts', 'no'])
    text = dump_sexpr(root) + '\n'
    return text, root_uuid


def make_board(name: str, seed: str, normalized: dict[str, Any],
               footprint_data: dict[str, tuple[list[Any], set[str]]], root_uuid: str) -> tuple[str, dict[str, int]]:
    width, height, layers = normalized['width'], normalized['height'], normalized['layers']
    # Copy the exact 2-layer set used by copper_project_create() in the current
    # C source. These are the native KiCad layer ordinals already used by the
    # project's tested scaffold; do not infer layer IDs from enum values.
    layer_nodes: list[Any] = [
        ['0', Quoted('F.Cu'), 'signal'], ['2', Quoted('B.Cu'), 'signal'],
        ['3', Quoted('B.Adhes'), 'user'], ['5', Quoted('F.SilkS'), 'user'],
        ['7', Quoted('B.SilkS'), 'user'], ['9', Quoted('F.Mask'), 'user'],
        ['11', Quoted('B.Mask'), 'user'], ['13', Quoted('Dwgs.User'), 'user'],
        ['25', Quoted('Edge.Cuts'), 'user'], ['31', Quoted('F.CrtYd'), 'user'],
        ['33', Quoted('B.CrtYd'), 'user'], ['35', Quoted('F.Fab'), 'user'],
        ['37', Quoted('B.Fab'), 'user'], ['1', Quoted('F.Adhes'), 'user'],
        ['19', Quoted('F.Paste'), 'user'], ['21', Quoted('B.Paste'), 'user'],
    ]
    board: list[Any] = ['kicad_pcb', ['version', '20241229'], ['generator', Quoted('pcbnew')],
                        ['generator_version', Quoted('9.0')], ['general', ['thickness', '1.6']],
                        ['paper', Quoted('A4')], ['layers', *layer_nodes],
                        ['setup', ['pad_to_mask_clearance', '0.05']], ['net', '0', Quoted('')]]
    net_names = sorted({p['net'] for c in normalized['components'] for p in c['pins'] if p['net']})
    net_ids = {n: i + 1 for i, n in enumerate(net_names)}
    for net in net_names:
        board.append(['net', str(net_ids[net]), Quoted(net)])
    # Deterministic grid packing with 5 mm perimeter and 12.5 mm pitch.
    cols = max(1, int((width - 10) // 12.5))
    for idx, comp in enumerate(normalized['components']):
        fp_id = comp['footprint_id']
        fp = copy.deepcopy(footprint_data[fp_id][0])
        pad_numbers = footprint_data[fp_id][1]
        pin_to_pad = {p['pad']: p['net'] for p in comp['pins']}
        if set(pin_to_pad) != pad_numbers:
            missing = sorted(pad_numbers - set(pin_to_pad))
            unknown = sorted(set(pin_to_pad) - pad_numbers)
            raise CompileError(f"{comp['reference']} footprint pad mapping mismatch for {fp_id}: missing={missing}, unknown={unknown}")
        pos = comp['position']
        x = float(pos.get('x_mm', 7.5 + (idx % cols) * 12.5))
        y = float(pos.get('y_mm', 7.5 + (idx // cols) * 12.5))
        angle = float(pos.get('rotation_deg', 0))
        if not (2.5 <= x <= width - 2.5 and 2.5 <= y <= height - 2.5):
            raise CompileError(f"{comp['reference']} board position ({x},{y}) is outside board bounds")
        # Remove library-only version/generator records and set full qualified ID.
        fp[1] = Quoted(fp_id)
        fp[:] = [n for n in fp if not (is_list(n, 'version') or is_list(n, 'generator') or is_list(n, 'at') or is_list(n, 'uuid') or is_list(n, 'property') or is_list(n, 'path') or is_list(n, 'sheetname') or is_list(n, 'sheetfile') or is_list(n, 'attr'))]
        # Keep original layer and descriptions/tags; add board placement and identity.
        insert_at = 2
        fp.insert(insert_at, ['at', fmt_mm(x), fmt_mm(y), fmt_mm(angle)])
        fp.insert(insert_at + 1, ['uuid', Quoted(deterministic_uuid(seed, f"footprint:{comp['reference']}"))])
        fp.insert(insert_at + 2, make_property('Reference', comp['reference'], x, y - 2.0, 0,
                                                layer='F.SilkS', include_uuid=deterministic_uuid(seed, f"fp-ref:{comp['reference']}")))
        fp.insert(insert_at + 3, make_property('Value', comp['value'], x, y + 2.0, 0,
                                                layer='F.Fab', include_uuid=deterministic_uuid(seed, f"fp-value:{comp['reference']}")))
        fp.insert(insert_at + 4, ['path', Quoted('/' + deterministic_uuid(seed, f"symbol:{comp['reference']}"))])
        fp.insert(insert_at + 5, ['sheetname', Quoted('/')])
        fp.insert(insert_at + 6, ['sheetfile', Quoted(name + '.kicad_sch')])
        # Derive footprint attribute from the first physical pad.
        first_pad = next(iter(children(fp, 'pad')))
        pad_type = str(first_pad[2]) if len(first_pad) > 2 else 'smd'
        fp.insert(insert_at + 7, ['attr', 'smd' if pad_type == 'smd' else 'through_hole'])
        for pad in children(fp, 'pad'):
            pad_no = str(pad[1])
            net_name = pin_to_pad[pad_no]
            net_id = net_ids[net_name] if net_name else 0
            pad.append(['net', str(net_id), Quoted(net_name)])
            pad.append(['uuid', Quoted(deterministic_uuid(seed, f"pad:{comp['reference']}:{pad_no}"))])
        # Add UUIDs to graphical primitives if absent; many library files omit them.
        for n in fp[1:]:
            if isinstance(n, list) and n and n[0] in ('fp_line', 'fp_arc', 'fp_circle', 'fp_rect', 'fp_poly', 'fp_text') and not child(n, 'uuid'):
                n.append(['uuid', Quoted(deterministic_uuid(seed, f"{comp['reference']}:primitive:{fp.index(n)}"))])
        board.append(fp)
    # Fabrication outline (Edge.Cuts) and explicit unrouted-state marker in manifest.
    board.append(['gr_rect', ['start', '0', '0'], ['end', fmt_mm(width), fmt_mm(height)],
                  ['stroke', ['width', '0.05'], ['type', 'default']], ['fill', 'none'],
                  ['layer', Quoted('Edge.Cuts')], ['uuid', Quoted(deterministic_uuid(seed, 'board-outline'))]])
    board.append(['embedded_fonts', 'no'])
    return dump_sexpr(board) + '\n', net_ids


def compile_design(intent_path: Path, output_parent: Path, args: argparse.Namespace) -> Path:
    try:
        raw_bytes = intent_path.read_bytes()
        try:
            data = json.loads(raw_bytes.decode('utf-8-sig'))
        except UnicodeDecodeError as exc:
            raise CompileError(f'design intent must be UTF-8 JSON: {exc}') from exc
        normalized = validate_intent(data)
        symbol_root, footprint_root = find_library_roots(args)
        seed = hashlib.sha256(raw_bytes).hexdigest()
        copper_cli = getattr(args, 'copper_cli', None)
        resolver_catalogue_text = build_ephemeral_catalogue(normalized['components']) if copper_cli else None
        resolution_results = resolve_components_with_copper(
            normalized['components'], symbol_root, footprint_root, copper_cli, resolver_catalogue_text)
        symbol_data: dict[str, tuple[list[Any], dict[str, tuple[float, float, float]]]] = {}
        footprint_data: dict[str, tuple[list[Any], set[str]]] = {}
        for comp in normalized['components']:
            if comp['symbol_id'] not in symbol_data:
                symbol_data[comp['symbol_id']] = load_symbol(symbol_root, comp['symbol_id'])
            if comp['footprint_id'] not in footprint_data:
                footprint_data[comp['footprint_id']] = load_footprint(footprint_root, comp['footprint_id'])
            symbol_pins = set(symbol_data[comp['symbol_id']][1])
            mapped = {p['symbol_pin'] for p in comp['pins']}
            if mapped != symbol_pins:
                raise CompileError(f"{comp['reference']} symbol pin mapping mismatch for {comp['symbol_id']}: missing={sorted(symbol_pins - mapped)}, unknown={sorted(mapped - symbol_pins)}")
            pads = footprint_data[comp['footprint_id']][1]
            mapped_pads = {p['pad'] for p in comp['pins']}
            if mapped_pads != pads:
                raise CompileError(f"{comp['reference']} footprint mapping mismatch for {comp['footprint_id']}: missing={sorted(pads - mapped_pads)}, unknown={sorted(mapped_pads - pads)}")
        project_name = normalized['name']
        project_dir = output_parent / project_name
        if project_dir.exists():
            raise CompileError(f'output project already exists; use a new --project-name or remove it explicitly: {project_dir}')
        output_parent.mkdir(parents=True, exist_ok=True)
        project_dir.mkdir(parents=True)
        root_uuid = deterministic_uuid(seed, 'schematic-root')
        sch_text, _ = make_schematic(project_name, seed, normalized['components'], symbol_data)
        pcb_text, net_ids = make_board(project_name, seed, normalized, footprint_data, root_uuid)
        project_json = {'meta': {'filename': project_name + '.kicad_pro', 'version': 1}}
        (project_dir / f'{project_name}.kicad_pro').write_text(json.dumps(project_json, indent=2) + '\n', encoding='utf-8')
        (project_dir / f'{project_name}.kicad_sch').write_text(sch_text, encoding='utf-8', newline='\n')
        (project_dir / f'{project_name}.kicad_pcb').write_text(pcb_text, encoding='utf-8', newline='\n')
        (project_dir / '.copper').mkdir()
        (project_dir / '.copper' / 'intent.json').write_bytes(raw_bytes)
        if resolver_catalogue_text is not None:
            (project_dir / '.copper' / 'intent_component_catalogue.parts').write_text(resolver_catalogue_text, encoding='utf-8')
        resolution_dir = project_dir / '.copper' / 'component-resolution'
        if resolution_results:
            resolution_dir.mkdir(parents=True, exist_ok=True)
            for ref, resolution in resolution_results.items():
                (resolution_dir / f'{ref}.json').write_text(json.dumps(resolution, indent=2) + '\n', encoding='utf-8')
        manifest = {
            'schema': 'copper.design-compile-manifest', 'schema_version': 1,
            'project': project_name, 'intent_sha256': seed,
            'compiler': 'copper_design_compile.py', 'compiler_version': '0.1.0',
            'copper_part_resolver': 'passed' if resolution_results else 'not_run',
            'asset_validation': 'python_asset_parse_passed',
            'source_libraries': {'symbols': str(symbol_root), 'footprints': str(footprint_root)},
            'components': [{'reference': c['reference'], 'symbol_id': c['symbol_id'], 'footprint_id': c['footprint_id'], 'copper_resolution': ('.copper/component-resolution/' + c['reference'] + '.json') if resolution_results else 'not_run'} for c in normalized['components']],
            'nets': [{'name': n, 'id': i} for n, i in net_ids.items()],
            'routing_status': 'not_routed',
            'notes': ['Native KiCad files were assembled from installed library assets.',
                      'No PCB tracks or vias were generated; connectivity is represented by pad net assignments.',
                      'Run KiCad ERC/DRC and Copper import before treating the design as valid.']
        }
        (project_dir / 'generation_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        return project_dir
    except Exception:
        # Keep incomplete output only if the project was already created and useful for diagnostics.
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description='Compile Copper design-intent JSON into a native KiCad 9 project')
    parser.add_argument('--intent', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path, help='Parent directory; compiler creates <project.name> beneath it')
    parser.add_argument('--symbols-root', help='KiCad symbols directory')
    parser.add_argument('--footprints-root', help='KiCad footprints directory')
    parser.add_argument('--copper-cli', help='Optional Copper executable; run design import after generation')
    parser.add_argument('--kicad-cli', help='Optional kicad-cli executable; run ERC/DRC/netlist exports after generation')
    args = parser.parse_args()
    try:
        project = compile_design(args.intent.resolve(), args.out.resolve(), args)
        print(f'KiCad project generated: {project}')
        import subprocess
        netlist_path = None
        if args.kicad_cli:
            cli = str(Path(args.kicad_cli).resolve())
            sch = project / f'{project.name}.kicad_sch'
            pcb = project / f'{project.name}.kicad_pcb'
            validation = project / 'validation'
            validation.mkdir(exist_ok=True)
            netlist_path = validation / 'design.kicad_net'
            commands = [
                [cli, 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '--output', str(netlist_path), str(sch)],
                [cli, 'sch', 'erc', '--format', 'json', '--output', str(validation / 'erc.json'), str(sch)],
                [cli, 'pcb', 'drc', '--format', 'json', '--output', str(validation / 'drc.json'), str(pcb)],
            ]
            outcomes = []
            for cmd in commands:
                r = subprocess.run(cmd, text=True, capture_output=True)
                outcomes.append({'command': cmd[1:], 'returncode': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr})
            (validation / 'cli_results.json').write_text(json.dumps(outcomes, indent=2) + '\n', encoding='utf-8')
            # ERC/DRC may return nonzero for findings (for example, unrouted
            # nets) and those reports are retained. Netlist export must succeed
            # because Copper uses that file to compare schematic nets to PCB pads.
            if outcomes[0]['returncode'] != 0 or not netlist_path.is_file() or netlist_path.stat().st_size == 0:
                print('KiCad CLI netlist export failed; see validation/cli_results.json', file=sys.stderr)
                return 3
        if args.copper_cli:
            cli_args = [str(Path(args.copper_cli).resolve()), 'design', 'import', str(project)]
            if netlist_path:
                cli_args.extend(['--netlist', str(netlist_path)])
            result = subprocess.run(cli_args, text=True, capture_output=True)
            (project / 'copper_import.stdout.log').write_text(result.stdout, encoding='utf-8')
            (project / 'copper_import.stderr.log').write_text(result.stderr, encoding='utf-8')
            print(result.stdout, end='')
            if result.returncode:
                print(result.stderr, file=sys.stderr, end='')
                print(f'Copper import failed with exit code {result.returncode}', file=sys.stderr)
                return result.returncode
        print('Routing status: NOT ROUTED (nets are present; no copper tracks/vias were generated).')
        return 0
    except (CompileError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
