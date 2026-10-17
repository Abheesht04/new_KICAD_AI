import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'hwc_kicad.py'
spec = importlib.util.spec_from_file_location('hwc_kicad', SCRIPT)
hwc = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = hwc
spec.loader.exec_module(hwc)


class HwcKiCadTests(unittest.TestCase):
    def test_parse_dump_roundtrip(self):
        source = '(root (name "quoted \\"text\\"") (at 10 20 0))'
        parsed = hwc.parse_sexpr(source)
        reparsed = hwc.parse_sexpr(hwc.dump_sexpr(parsed))
        self.assertEqual(hwc.head(reparsed), 'root')
        self.assertEqual(str(hwc.one(reparsed, 'name')[1]), 'quoted "text"')

    def test_v1_prompt_values(self):
        self.assertEqual(
            hwc.values_from_prompt('resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm'),
            (5.0, 3.3, '10k', '20k'),
        )

    def test_malformed_sexpr_fails(self):
        with self.assertRaises(hwc.SExprError):
            hwc.parse_sexpr('(unclosed')


    def test_generate_divider_from_symbol_library(self):
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            (temp_path / 'Device.kicad_sym').write_text(
                '(kicad_symbol_lib (version 20231120) (generator "test") '
                '(symbol "R" '
                '(property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27)))) '
                '(property "Value" "R" (at 0 0 0) (effects (font (size 1.27 1.27)))) '
                '(symbol "R_0_1" (rectangle (start -1 -2) (end 1 2))) '
                '(symbol "R_1_1")) )',
                encoding='utf-8',
            )
            out = temp_path / 'out' / 'divider.kicad_sch'
            hwc.generate_divider(
                'resistor divider 5 V to 3.3 V using 10k ohm and 20k ohm',
                out, temp_path,
            )
            tree = hwc.parse_sexpr(out.read_text(encoding='utf-8'))
            self.assertEqual(hwc.head(tree), 'kicad_sch')
            self.assertEqual(len(hwc.direct(tree, 'symbol')), 2)
            self.assertEqual(len(hwc.direct(tree, 'wire')), 1)
            self.assertEqual([str(x[1]) for x in hwc.direct(tree, 'label')], ['VIN', 'VOUT', 'GND'])
            lib_symbols = hwc.one(tree, 'lib_symbols')
            self.assertEqual(str(hwc.direct(lib_symbols, 'symbol')[0][1]), 'Device:R')


    def test_footprint_catalog(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lib = root / 'Resistor_SMD.pretty'
            lib.mkdir()
            (lib / 'R_0603.kicad_mod').write_text(
                '(footprint "R_0603" (version 20240108) (generator "test") '
                '(descr "resistor 0603") (tags "resistor 0603") '
                '(pad "1" smd rect (at 0 0) (size 1 1)))', encoding='utf-8'
            )
            rows = hwc.build_footprint_catalog(root)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['id'], 'Resistor_SMD:R_0603')
            self.assertEqual(rows[0]['pad_count'], 1)

    def test_reference_designator_roles(self):
        self.assertEqual(hwc.ROLE_PREFIXES['resistor'], 'R')


if __name__ == '__main__':
    unittest.main()
