# New generic S-expression IR (`IR.c` / `IR.h`)

## Scope

This module is a separate implementation from `sexpr.c` / `sexpr.h`. The existing
module is intentionally left unchanged. `IR` is a generic syntax-tree layer that
can be used later by KiCad symbol-library ingestion, schematic IR construction,
and other structured S-expression tooling. It is not yet the typed schematic
placement/routing IR or a complete KiCad grammar validator.

## Node model

- `IR_NODE_LIST`: ordered children linked by `next`.
- `IR_NODE_ATOM`: unquoted token text.
- `IR_NODE_STRING`: decoded quoted string content.
- `parent` establishes one-parent ownership; a node may only be attached to one
  child list at a time.
- `text_len` is the byte count, excluding its terminating NUL. Input strings are
  copied as bytes; the parser does not currently validate arbitrary byte streams
  for UTF-8 correctness. KiCad documents are expected to be UTF-8.

Numbers, booleans, pin numbers, UUIDs, references, and library identifiers are
syntactically atoms. Numeric helper functions interpret valid numeric atoms on
demand and never change the stored lexical representation.

## Memory contract

The caller owns the `Arena` lifetime. All nodes and their text are allocated in
the arena. `ir_node_detach()` only updates tree links and does not free memory.
`ir_node_clone()` deep-copies one subtree into a destination arena. Use cloning
rather than attaching one node in two locations.

The writer validates parent/child/sibling invariants before writing. The parser
requires exactly one complete expression, allows whitespace/comments around it,
and reports the first parse error with byte offset, line, and byte-column.

## Initial capabilities

- Parse a single expression or a complete file.
- `#` / `;` line comments outside strings.
- Quoted strings with quote/backslash, common C-style control escapes, two-digit
  hexadecimal escapes, and up-to-three-digit octal escapes. Decoded NUL is
  rejected because the node text contract is NUL-terminated C strings.
- Atom/string/list constructors; named-list constructor.
- Parent-aware append, insert-before, detach, and deep clone.
- Immediate-child lookup and positional access.
- Strict signed/unsigned integer, decimal double, and boolean atom conversion.
- Floating-point atom generation without exponent notation, for readability in
  KiCad-oriented output. Formatting uses 17 significant digits before decimal
  expansion to allow round-trip conversion back to `double`.
- Pretty/compact stream output and allocated string output.

## Limitations to keep visible

- This is syntax parsing, not schema validation for `.kicad_sym`, `.kicad_sch`, or
  `.kicad_pcb`. Higher layers must validate required fields and KiCad-version rules.
- Numeric conversion supports ordinary decimal notation and exponent notation on
  input, but does not parse `nan`, `inf`, hexadecimal floats, or locale-specific
  source strings. Non-finite generated doubles are rejected.
- Unicode bytes are preserved but UTF-8 validity is not checked yet.
- The parser has nesting-depth guards; this version uses recursive descent.
- No UUID generation is included. UUIDs belong to the schematic/document layer,
  not generic S-expression syntax; the syntax layer will preserve them as atoms.
- Complete official-library compatibility is not yet claimed. The supplied R/C
  symbol expressions are regression fixtures; inheritance, every escape edge
  case, and the complete installed KiCad 9.0.6 symbol corpus still need a corpus
  validation run.

## Build and tests

The `ir` test target is added to CMake. It exercises parsing, comments, tree
ownership, clone/detach, numeric conversion, string escaping, serialization
round-trips, parse error handling, and the supplied KiCad 9 resistor/capacitor
symbol expressions.
