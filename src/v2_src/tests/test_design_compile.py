"""Unit tests for the JSON -> native KiCad design-intent compiler."""
from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve()
V2_ROOT = HERE.parents[1]
FIXTURE_ROOT = V2_ROOT / 'tests' / 'fixtures' / 'catalogue_resolution' / 'kicad'
COMPILER_PATH = V2_ROOT / 'tools' / 'copper_design_compile.py'
ASSET_CONTEXT_PATH = V2_ROOT / 'tools' / 'kicad_asset_context.py'
FIXTURE_INTENT = HERE.parent / 'fixture_design_intent.json'

spec = importlib.util.spec_from_file_location('copper_design_compile', COMPILER_PATH)
compiler = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(compiler)


class DesignCompilerTests(unittest.TestCase):
    def setUp(self):
        self.base = json.loads(FIXTURE_INTENT.read_text(encoding='utf-8'))
        self.args = SimpleNamespace(
            symbols_root=str(FIXTURE_ROOT / 'symbols'),
            footprints_root=str(FIXTURE_ROOT / 'footprints'),
            copper_cli=os.environ.get('COPPER_CLI'),
        )

    def test_extracts_actual_kicad_pin_and_pad_context(self):
        with tempfile.TemporaryDirectory() as tmp:
            prompt = Path(tmp) / 'task.txt'
            output = Path(tmp) / 'asset_context.json'
            prompt.write_text(
                'symbol Device:R\nfootprint Resistor_SMD:R_0603_1608Metric\n',
                encoding='utf-8')
            result = subprocess.run([
                sys.executable, str(ASSET_CONTEXT_PATH),
                '--prompt-file', str(prompt),
                '--symbols-root', str(FIXTURE_ROOT / 'symbols'),
                '--footprints-root', str(FIXTURE_ROOT / 'footprints'),
                '--out', str(output),
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            context = json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual(context['symbols'][0]['id'], 'Device:R')
            self.assertEqual({p['number'] for p in context['symbols'][0]['pins']}, {'1', '2'})
            self.assertEqual(context['footprints'][0]['id'], 'Resistor_SMD:R_0603_1608Metric')
            self.assertEqual({p['number'] for p in context['footprints'][0]['pads']}, {'1', '2'})

    def test_full_compile_normalizes_sentence_period_in_library_ids(self):
        data = json.loads(json.dumps(self.base))
        data['components'][0]['symbol_id'] += '.'
        data['components'][0]['footprint_id'] += '.'
        with tempfile.TemporaryDirectory() as tmp:
            intent = Path(tmp) / 'intent.json'
            intent.write_text(json.dumps(data), encoding='utf-8')
            project = compiler.compile_design(intent, Path(tmp) / 'out', self.args)
            manifest = json.loads((project / 'generation_manifest.json').read_text(encoding='utf-8'))
            component = manifest['components'][0]
            self.assertEqual(component['symbol_id'], self.base['components'][0]['symbol_id'])
            self.assertEqual(component['footprint_id'], self.base['components'][0]['footprint_id'])
            board_text = (project / f'{project.name}.kicad_pcb').read_text(encoding='utf-8')
            self.assertNotIn('R_0603_1608Metric..kicad_mod', board_text)
            self.assertIn('Resistor_SMD:R_0603_1608Metric', board_text)

    def test_reads_windows_1252_kicad_library_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            asset = Path(tmp) / 'legacy.kicad_mod'
            asset.write_bytes(b'(footprint \"Legacy\" (descr \"10 \xd7 5 mm\"))')
            decoded = compiler.read_kicad_text(asset)
            self.assertIn('10 × 5 mm', decoded)
            tree = compiler.parse_sexpr(decoded)
            self.assertEqual(tree[0], 'footprint')


    def test_normalizes_sentence_period_from_footprint_id(self):
        # Prompt punctuation copied by the model must not become a second dot
        # before the .kicad_mod extension.
        raw_id = 'Resistor_SMD:R_0603_1608Metric.'
        nickname, entry = compiler.split_lib_id(raw_id, 'footprint_id')
        self.assertEqual((nickname, entry), ('Resistor_SMD', 'R_0603_1608Metric'))
        pads = compiler.load_footprint(FIXTURE_ROOT / 'footprints', raw_id)[1]
        self.assertEqual(pads, {'1', '2'})

    def test_validate_intent_canonicalizes_symbol_and_footprint_ids(self):
        data = json.loads(json.dumps(self.base))
        data['components'][0]['symbol_id'] += '.'
        data['components'][0]['footprint_id'] += '.'
        normalized = compiler.validate_intent(data)
        self.assertFalse(normalized['components'][0]['symbol_id'].endswith('.'))
        self.assertFalse(normalized['components'][0]['footprint_id'].endswith('.'))

    def test_asset_context_finds_library_nicknames_with_dots(self):
        # Many stock KiCad libraries have a dot in the nickname; they were silently dropped from the context.
        spec = importlib.util.spec_from_file_location('kicad_asset_context_ids', ASSET_CONTEXT_PATH)
        mod = importlib.util.module_from_spec(spec)
        sys.path.insert(0, str(ASSET_CONTEXT_PATH.parent))
        try:
            spec.loader.exec_module(mod)
        finally:
            sys.path.pop(0)
        text = ('symbol Connector_Generic:Conn_01x02 footprint Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical. '
                'footprint Resistor_SMD:R_0603_1608Metric and symbol Device:R.')
        self.assertEqual(sorted(mod.FOOTPRINT_RE.findall(text)),
                         ['Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical', 'Resistor_SMD:R_0603_1608Metric'])
        self.assertEqual(sorted(mod.SYMBOL_RE.findall(text)), ['Connector_Generic:Conn_01x02', 'Device:R'])

    def test_asset_context_ignores_sentence_period_after_footprint_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            prompt = Path(tmp) / 'task.txt'
            output = Path(tmp) / 'asset_context.json'
            prompt.write_text(
                'symbol Device:R.\nfootprint Resistor_SMD:R_0603_1608Metric.\n',
                encoding='utf-8')
            result = subprocess.run([
                sys.executable, str(ASSET_CONTEXT_PATH),
                '--prompt-file', str(prompt),
                '--symbols-root', str(FIXTURE_ROOT / 'symbols'),
                '--footprints-root', str(FIXTURE_ROOT / 'footprints'),
                '--out', str(output),
            ], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            context = json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual(context['symbols'][0]['id'], 'Device:R')
            self.assertEqual(context['footprints'][0]['id'], 'Resistor_SMD:R_0603_1608Metric')

    def test_rejects_wrong_schema(self):
        bad = dict(self.base)
        bad['schema'] = 'not-copper'
        with self.assertRaises(compiler.CompileError):
            compiler.validate_intent(bad)

    def test_rejects_duplicate_references(self):
        bad = json.loads(json.dumps(self.base))
        bad['components'][1]['reference'] = 'R1'
        with self.assertRaisesRegex(compiler.CompileError, 'duplicate component reference'):
            compiler.validate_intent(bad)

    def test_rejects_single_ended_nets(self):
        bad = json.loads(json.dumps(self.base))
        bad['components'][0]['pins'][0]['net'] = 'ONLY_ONE'
        with self.assertRaisesRegex(compiler.CompileError, 'only one endpoint'):
            compiler.validate_intent(bad)

    def test_rejects_unsupported_stackup(self):
        bad = json.loads(json.dumps(self.base))
        bad['project']['board']['layers'] = 4
        with self.assertRaisesRegex(compiler.CompileError, '2-layer boards only'):
            compiler.validate_intent(bad)

    def test_compiles_from_real_kicad_library_fixtures(self):
        with tempfile.TemporaryDirectory() as tmp:
            intent = Path(tmp) / 'intent.json'
            intent.write_text(json.dumps(self.base), encoding='utf-8')
            out = Path(tmp) / 'out'
            project = compiler.compile_design(intent, out, self.args)
            sch = project / 'CopperCompilerFixture.kicad_sch'
            pcb = project / 'CopperCompilerFixture.kicad_pcb'
            pro = project / 'CopperCompilerFixture.kicad_pro'
            self.assertTrue(sch.is_file())
            self.assertTrue(pcb.is_file())
            self.assertTrue(pro.is_file())
            sch_tree = compiler.parse_sexpr(sch.read_text(encoding='utf-8'))
            pcb_tree = compiler.parse_sexpr(pcb.read_text(encoding='utf-8'))
            self.assertEqual(sch_tree[0], 'kicad_sch')
            self.assertEqual(pcb_tree[0], 'kicad_pcb')
            self.assertEqual(len(compiler.children(sch_tree, 'symbol')), 2)
            self.assertEqual(len(compiler.children(sch_tree, 'label')), 4)
            self.assertEqual(len(compiler.children(pcb_tree, 'footprint')), 2)
            self.assertEqual(len(compiler.children(pcb_tree, 'net')), 3)  # net 0 + VIN + GND
            manifest = json.loads((project / 'generation_manifest.json').read_text(encoding='utf-8'))
            self.assertEqual(manifest['routing_status'], 'not_routed')

    def test_generated_project_imports_into_copper_when_binary_supplied(self):
        copper_cli = os.environ.get('COPPER_CLI')
        if not copper_cli:
            self.skipTest('Set COPPER_CLI to run the optional native Copper CLI integration test')
        with tempfile.TemporaryDirectory() as tmp:
            intent = Path(tmp) / 'intent.json'
            intent.write_text(json.dumps(self.base), encoding='utf-8')
            project = compiler.compile_design(intent, Path(tmp) / 'out', self.args)
            # Exercise the actual Copper parser and schematic-to-PCB pad/net
            # parity path. This netlist uses the same KiCad S-expression
            # structure exported by `kicad-cli sch export netlist`.
            net_names = sorted({pin['net'] for comp in self.base['components']
                                for pin in comp['pins'] if pin['net']})
            net_lines = ['(export (version \"E\")', ' (components']
            for comp in self.base['components']:
                ref = comp['reference']
                value = comp['value']
                footprint = comp['footprint_id']
                net_lines.append(
                    f'  (comp (ref \"{ref}\") (value \"{value}\") '
                    f'(footprint \"{footprint}\"))')
            net_lines.append(' )')
            net_lines.append(' (nets')
            for code, net_name in enumerate(net_names, start=1):
                net_lines.append(f'  (net (code \"{code}\") (name \"{net_name}\") (class \"Default\")')
                for comp in self.base['components']:
                    for pin in comp['pins']:
                        if pin['net'] == net_name:
                            ref = comp['reference']
                            pin_number = pin['symbol_pin']
                            net_lines.append(
                                f'   (node (ref \"{ref}\") '
                                f'(pin \"{pin_number}\") (pintype \"passive\"))')
                net_lines.append('  )')
            net_lines.extend([' )', ')'])
            netlist = Path(tmp) / 'validation.kicad_net'
            netlist.write_text('\n'.join(net_lines) + '\n', encoding='utf-8')
            result = subprocess.run([copper_cli, 'design', 'import', str(project),
                                     '--netlist', str(netlist)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('pad-net-checks=4 mismatches=0', result.stdout)
            self.assertTrue((project / '.copper' / 'design.json').is_file())
            self.assertTrue((project / '.copper' / 'component-resolution' / 'R1.json').is_file())
            self.assertTrue((project / '.copper' / 'component-resolution' / 'R2.json').is_file())
            manifest = json.loads((project / 'generation_manifest.json').read_text(encoding='utf-8'))
            self.assertEqual(manifest['copper_part_resolver'], 'passed')

    def test_rejects_multi_unit_symbol(self):
        with tempfile.TemporaryDirectory() as tmp:
            symbols = Path(tmp) / 'symbols'
            symbols.mkdir()
            symbol_text = """(kicad_symbol_lib
  (version 20231120)
  (generator "test")
  (symbol "DualUnit"
    (symbol "DualUnit_1_1"
      (pin input line
        (at 0 0 0)
        (length 2.54)
        (name "A")
        (number "1")))
    (symbol "DualUnit_2_1"
      (pin input line
        (at 0 2.54 0)
        (length 2.54)
        (name "B")
        (number "2"))))
)
"""
            (symbols / 'Test.kicad_sym').write_text(symbol_text, encoding='utf-8')
            with self.assertRaisesRegex(compiler.CompileError, 'multi-unit symbol'):
                compiler.load_symbol(symbols, 'Test:DualUnit')

    def test_rejects_unknown_symbol_library(self):
        bad = json.loads(json.dumps(self.base))
        bad['components'][0]['symbol_id'] = 'MissingLibrary:NotARealSymbol'
        with tempfile.TemporaryDirectory() as tmp:
            intent = Path(tmp) / 'intent.json'
            intent.write_text(json.dumps(bad), encoding='utf-8')
            with self.assertRaisesRegex(compiler.CompileError, 'Copper resolver rejected|symbol library not found'):
                compiler.compile_design(intent, Path(tmp) / 'out', self.args)

    def test_rejects_incomplete_pad_mapping(self):
        bad = json.loads(json.dumps(self.base))
        bad['components'][0]['pins'][0]['pad'] = '99'
        with tempfile.TemporaryDirectory() as tmp:
            intent = Path(tmp) / 'intent.json'
            intent.write_text(json.dumps(bad), encoding='utf-8')
            with self.assertRaisesRegex(compiler.CompileError, 'Copper resolver rejected|footprint mapping mismatch'):
                compiler.compile_design(intent, Path(tmp) / 'out', self.args)

    def test_sexpr_round_trip_preserves_quoted_strings(self):
        tree = compiler.parse_sexpr('(root (name "A project") (layer "F.Cu") (signal yes))')
        emitted = compiler.dump_sexpr(tree)
        self.assertIn('"A project"', emitted)
        self.assertIn('"F.Cu"', emitted)
        self.assertIn('yes', emitted)


if __name__ == '__main__':
    unittest.main(verbosity=2)
