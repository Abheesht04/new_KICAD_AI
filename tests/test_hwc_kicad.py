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


def reference_parse(text):
    """The original character-by-character parser, kept verbatim as the oracle for the fast tokenizer."""
    n = len(text)
    i = 0

    def skip():
        nonlocal i
        while i < n:
            if text[i].isspace():
                i += 1
            elif text[i] == ';':
                j = text.find('\n', i)
                i = n if j < 0 else j + 1
            else:
                break

    def read_string():
        nonlocal i
        i += 1
        out = []
        while i < n:
            c = text[i]
            i += 1
            if c == '"':
                return hwc.Quoted(''.join(out))
            if c == '\\':
                if i >= n:
                    raise hwc.SExprError('backslash at end')
                esc = text[i]
                i += 1
                out.append({'n': '\n', 'r': '\r', 't': '\t'}.get(esc, esc))
            else:
                out.append(c)
        raise hwc.SExprError('unterminated')

    def read_node():
        nonlocal i
        skip()
        if i >= n:
            raise hwc.SExprError('end')
        c = text[i]
        if c == '(':
            i += 1
            result = []
            while True:
                skip()
                if i >= n:
                    raise hwc.SExprError('unclosed')
                if text[i] == ')':
                    i += 1
                    return result
                result.append(read_node())
        if c == ')':
            raise hwc.SExprError('close')
        if c == '"':
            return read_string()
        start = i
        while i < n and not text[i].isspace() and text[i] not in '();':
            i += 1
        if i == start:
            raise hwc.SExprError('token')
        return hwc.Atom(text[start:i])

    node = read_node()
    skip()
    if i != n:
        raise hwc.SExprError('extra')
    return node


def typed(node):
    """A tree with each value's type, so Atom vs Quoted differences are caught too."""
    if isinstance(node, list):
        return ['list', [typed(x) for x in node]]
    return [type(node).__name__, str(node)]


class ParserEquivalence(unittest.TestCase):
    def files(self):
        root = SCRIPT.parents[1]; found = []
        for pattern in ('*.kicad_sym', '*.kicad_mod', '*.kicad_sch', '*.kicad_pcb', '*.copper', '*.parts', '*.espec', '*.rules', '*.design', '*.kicad_net'):
            found += [p for p in root.rglob(pattern) if 'node_modules' not in p.parts and '.git' not in p.parts]
        import os
        for env in ('KICAD_SYMBOL_DIR', 'KICAD_FOOTPRINT_DIR'):
            if os.environ.get(env):
                found += sorted(Path(os.environ[env]).rglob('*.kicad_sym' if 'SYMBOL' in env else '*.kicad_mod'))[:400]
        return found

    def test_same_trees_as_the_reference_parser(self):
        files = self.files(); self.assertGreater(len(files), 20)
        for path in files:
            text = path.read_text(encoding='utf-8', errors='replace')
            try:
                expected = typed(reference_parse(text))
            except hwc.SExprError:
                with self.assertRaises(hwc.SExprError, msg=str(path)):
                    hwc.parse_sexpr(text)
                continue
            self.assertEqual(typed(hwc.parse_sexpr(text)), expected, str(path))

    def test_edge_cases_agree(self):
        cases = ['(a b c)', '  (a)  ', 'atom', '(a "q \\"x\\" \\n \\t \\\\ \\z") ', '(a b"c d)', '(a ; comment\n b)', '(a); tail',
                 '(a)\n; only a comment', '(a b)', '(a b)', '((()))', '(a "multi\nline")', '("" x)',
                 '(unclosed', ')', '(a))', '(a) (b)', '', '   ', '(a "unterminated)', '(a "x\\', '"start', '(a "b"c)']
        for text in cases:
            try:
                expected = typed(reference_parse(text))
            except hwc.SExprError:
                with self.assertRaises(hwc.SExprError, msg=repr(text)):
                    hwc.parse_sexpr(text)
                continue
            self.assertEqual(typed(hwc.parse_sexpr(text)), expected, repr(text))


if __name__ == '__main__':
    unittest.main()
