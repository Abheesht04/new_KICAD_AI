#!/usr/bin/env python3
"""Export authoritative KiCad symbol-pin and footprint-pad metadata for a task prompt.

The prompt should name exact library IDs using forms such as:
  symbol Device:LED
  footprint LED_SMD:LED_0603_1608Metric
The resulting JSON is supplied to the local LLM as read-only library context.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import copper_design_compile as compiler

# Do not absorb sentence-ending punctuation into a library ID. Dots remain
# supported internally (e.g. names with version-like segments), but a dot
# must be followed by another valid name character.
_LIBRARY_ID = r"[A-Za-z0-9_-]+:[A-Za-z0-9_+-]+(?:\.[A-Za-z0-9_+-]+)*"
SYMBOL_RE = re.compile(r"\bsymbol\s+(" + _LIBRARY_ID + r")")
FOOTPRINT_RE = re.compile(r"\bfootprint\s+(" + _LIBRARY_ID + r")")


def node_value(node: list | None, index: int = 1) -> str | None:
    if node is None or len(node) <= index:
        return None
    return str(node[index])


def describe_symbol(symbol_root: Path, lib_id: str) -> dict:
    symbol, pin_positions = compiler.load_symbol(symbol_root, lib_id)
    nickname, entry = compiler.split_lib_id(lib_id, 'symbol_id')
    lib_id = f'{nickname}:{entry}'
    library = compiler.parse_sexpr(compiler.read_kicad_text(symbol_root / f'{nickname}.kicad_sym'))
    definition = next((x for x in library[1:] if compiler.is_list(x, 'symbol') and len(x) > 1 and str(x[1]) == entry), None)
    pins = []
    for item in compiler.walk(definition):
        if not compiler.is_list(item, 'pin'):
            continue
        number = node_value(compiler.child(item, 'number'))
        name = node_value(compiler.child(item, 'name'))
        at = compiler.child(item, 'at')
        if number is None:
            continue
        pins.append({
            'number': number,
            'name': name or '',
            'electrical_type': str(item[1]) if len(item) > 1 else 'unknown',
            'position': [float(at[1]), float(at[2]), float(at[3])] if at and len(at) >= 4 else None,
        })
    pins.sort(key=lambda p: (len(p['number']), p['number'], p['name']))
    return {'id': lib_id, 'library_file': str(symbol_root / f'{nickname}.kicad_sym'), 'pins': pins}


def describe_footprint(footprint_root: Path, lib_id: str) -> dict:
    footprint, pad_numbers = compiler.load_footprint(footprint_root, lib_id)
    nickname, entry = compiler.split_lib_id(lib_id, 'footprint_id')
    lib_id = f'{nickname}:{entry}'
    pads = []
    for pad in compiler.children(footprint, 'pad'):
        at = compiler.child(pad, 'at')
        pads.append({
            'number': str(pad[1]),
            'type': str(pad[2]) if len(pad) > 2 else 'unknown',
            'shape': str(pad[3]) if len(pad) > 3 else 'unknown',
            'position': [float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0] if at and len(at) >= 3 else None,
        })
    pads.sort(key=lambda p: (len(p['number']), p['number']))
    return {
        'id': lib_id,
        'library_directory': str(footprint_root / f'{nickname}.pretty'),
        'pads': pads,
    }


def build_context(prompt_text: str, symbols_root: Path, footprints_root: Path) -> dict:
    symbol_ids = sorted(set(SYMBOL_RE.findall(prompt_text)))
    footprint_ids = sorted(set(FOOTPRINT_RE.findall(prompt_text)))
    context = {
        'schema': 'copper.kicad-library-context',
        'schema_version': 1,
        'source': 'locally installed KiCad library files',
        'symbols': [],
        'footprints': [],
        'notices': [],
    }
    for lib_id in symbol_ids:
        context['symbols'].append(describe_symbol(symbols_root, lib_id))
    for lib_id in footprint_ids:
        context['footprints'].append(describe_footprint(footprints_root, lib_id))
    if not symbol_ids or not footprint_ids:
        context['notices'].append(
            'The task prompt did not declare exact symbol and footprint IDs. '
            'Do not invent IDs or pin mappings; request clarification or choose only IDs that are explicitly provided.'
        )
    return context


def main() -> int:
    parser = argparse.ArgumentParser(description='Extract local KiCad pin/pad metadata referenced by a design task')
    parser.add_argument('--prompt-file', required=True, type=Path)
    parser.add_argument('--symbols-root', required=True, type=Path)
    parser.add_argument('--footprints-root', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    try:
        prompt = args.prompt_file.read_text(encoding='utf-8-sig')
        context = build_context(prompt, args.symbols_root, args.footprints_root)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(context, indent=2) + '\n', encoding='utf-8')
        print(f"KiCad asset context written: {args.out}")
        print(f"Symbols: {len(context['symbols'])}; footprints: {len(context['footprints'])}")
        return 0
    except (OSError, compiler.CompileError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
