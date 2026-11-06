# Board model (V2 track B)

`src/v2_src/board.{c,h}` reads a `.kicad_pcb` file through IR into one integer-nanometre model. It lives in the `copper_v2` library and follows CHARTER §3.1:
- C17;
- Arena memory;
- `BoardStatus` results;
- no floating point, `printf` or `exit`.

From the command line, `copper-core board FILE` prints the model as JSON. Lengths are integer nm and angles are micro-degrees.

## What it reads

| Item | Model |
|---|---|
| `(net N "name")`, or `(net "name")` (KiCad 10) | `nets`; an item that names a net missing from the table adds it |
| `(footprint …)` (also old `module`) | lib id, reference and value (`property`, or old `fp_text`), position, rotation, side |
| `(pad …)` | number, type, shape, position in the footprint and on the board, absolute angle, size, round or oval drill, `roundrect_rratio`, layers, net |
| `(segment …)`, `(arc …)` | tracks: start/(mid)/end, width, layer, net |
| `(via [blind\|micro] …)` | position, size, drill, layers, net |
| `(zone …)`, also inside footprints | net (by number or `net_name`), layers, keepout flag, outline vertices, count of filled polygons |
| Edge.Cuts: `gr_line`, `gr_arc`, `gr_rect`, `gr_circle`, `gr_poly`, and the same `fp_*` items in footprints | edges chained into loops; closure and bounds |

**Pad positions:**
- Board position = footprint position + the pad's footprint-frame offset, rotated the way KiCad rotates it: `x' = x cos a + y sin a`, `y' = −x sin a + y cos a`, with y pointing down. Back-side pads are already mirrored in the file.
- **Rotations that are multiples of 90°:** exact integer arithmetic.
- **Any other rotation:**
  - the position is rounded to the nearest nm from a Q62 fixed-point sine and cosine, with an error under 1 nm (128-bit integers, a GCC/Clang extension);
  - the pad is flagged `exact = false`, and an `INEXACT_ROTATION` issue names the footprint;
  - on a compiler without 128-bit integers, such a board is refused with `RANGE_ERROR` instead of being guessed.

**Outline:**
- **Loops:** edges join only where end points are exactly equal.
- **Closure:** `outline.closed` is true only when every loop closes exactly.
- **Gaps:** when an outline is open, the loose ends are paired nearest-first:
  - `outline.max_gap` is the widest of those gaps;
  - `board_outline_closed_within(board, tolerance)` reports whether every gap is within the tolerance;
  - KiCad itself joins ends across a small chaining gap, but the model never assumes a value: the caller passes it as data.
- **Bounds:** unions of all items. An arc's extremes come from its circle (centre rounded to the nearest nm, radius rounded up, plus 1 nm), so the bounds are never smaller than the outline. A circle's bounds are its centre ± (radius + 1 nm).

**Issues** (things read but not modelled exactly; nothing is dropped silently):
- `INEXACT_ROTATION`;
- `ZONE_ARC`: only an arc's end points are kept in a zone outline;
- `EDGE_UNSUPPORTED`: for example `gr_curve`;
- `ARC_DEGENERATE`: collinear points, treated as a line;
- `OUTLINE_OPEN`;
- `NO_OUTLINE`;
- `ARC_BOUNDS_UNAVAILABLE`.

**Errors:**
- `PARSE_ERROR` with line, column and offset;
- `NOT_A_BOARD`;
- `INVALID_CONTENT` or `RANGE_ERROR` with the top-level item number and a context string such as `footprint R9 pad 2: size`;
- `IO_ERROR`, `OUT_OF_MEMORY`.

**Lookups:**
- `board_find_footprint(board, ref)`;
- `board_find_pad(board, ref, number)`;
- `board_find_net(board, name)`;
- `board_pads_on_net(board, net, out, capacity)`.

## Evidence

| Test | Board | What it proves |
|---|---|---|
| `board_test` | `tests/golden/board/r0603x5.kicad_pcb`, **Copper-generated** (not a KiCad export): five 0603 resistors at 0/90/180/270/0° and a 3-pin header | pad positions equal hand-computed values at every rotation; nets, sizes, drills, layers, tracks and outline |
| `board_test` | `r0603x5.kicad10.kicad_pcb`: the same board re-saved by KiCad 10.0.5 (`kicad-cli pcb upgrade --force`; a development preview, not KiCad 9 evidence) | KiCad's own writer gives an identical model, even though it uses format 20260206, nets by name only, −90° for 270°, and its own track order |
| `board_test` | `examples/generated/divider/design.kicad_pcb` against `manufacturing_audit/drill_test/design.drl` (**real KiCad 9.0.6 output** from that board) | every hole KiCad 9.0.6 drilled is a through-hole pad in the model, to the nanometre |
| `board_test` | inline boards | 45° and 90° footprints, a back side, an oval drill, a nameless np_thru_hole pad, arc tracks, blind vias, zones and keepouts, a rounded-rectangle outline with a round cut-out, open outlines with measured gaps, and every error kind |
| `board_kicad_demos` (`tests/test_board_demos.py`) | every **KiCad 9.0-saved** demo board in a KiCad install, up to 12 MB (13 boards here; read in place, not committed, as their licences vary) | (1) it loads; (2) footprints, pads, tracks, vias and zones equal an independent count by the reference S-expression parser; (3) **4,294 round through holes** equal KiCad's drill export of the same boards, read back by `copper-core drill`, to the Excellon file's 1 µm resolution |

**Notes on the `board_kicad_demos` evidence:**
- **Drill export:** on this machine it was run by the KiCad 10.0.5 CLI, so step (3) is a development cross-check. The board files themselves are KiCad 9.0 output. The test says which CLI ran.
- **Open outlines:** two of the 13 outlines do not close exactly:
  - tinytapeout misses by 3 nm, where footprint edges meet board edges;
  - RoyalBlue54L-NFC-Antenna misses by 3.5 µm.

  KiCad accepts both. The model reports them as open and states the gap.
- **Blind vias:** RoyalBlue54L-Feather's 183 blind vias are counted but not compared, because this export does not write layer-pair files.

**Sanitizers:**
- Under UBSan in trap mode, `board_test` and every demo board up to 12 MB load cleanly (formats from KiCad 8.99 to 10.0).
- ASan was not run; it is unavailable on MinGW.

**Fixed along the way:**
- **What was wrong:** `IR` sized each quoted string's buffer as the whole rest of the input, so memory grew with the square of the file size. The 5 MB demo boards ran out of memory, and the bug never showed on small files.
- **The fix:** the buffer is now sized from the string's own span. It is applied to both copies, `src/IR.c` and `src/v2_src/IR.c`, as a separate, noted commit (IR is a protected file).

## Not yet modelled

- Pad drill offsets.
- Custom pad primitives: only the anchor pad is kept.
- Chamfered corners.
- Zone arcs: their end points only.
- Bezier Edge.Cuts: reported as an issue.
- Text, dimensions, groups, and the board setup and stackup apart from thickness.
- Blind and buried via drill files are not cross-checked.
