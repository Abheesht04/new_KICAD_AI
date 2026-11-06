# Gerber reader (V2 track A)

`src/v2_src/gerber.{c,h}` reads Gerber RS-274X / X2 files, as KiCad 9 writes them, into exact integer nanometres. It is in the `copper_v2` library. From the command line, `copper-core gerber FILE` prints JSON.

## What it reads

| Command | Handling |
|---|---|
| `FS` | `FSLAXnmYnm` only: leading zeros omitted, absolute coordinates, the same format for X and Y. Incremental or trailing-zero formats are an ERROR, never guessed. |
| `MO` | `MM` is exact. `IN` is multiplied by 25.4 and rounded to the nearest nm; the rounding is reported as `ROUNDED`. More than 6 decimals is rounded the same way. |
| `AD` | `C`, `R`, `O` and `P`, and macro apertures. Holes in standard apertures are reported, not subtracted. A zero-size aperture draws nothing. |
| `AM` | Primitive 0 (comment), 1 (circle), 4 (outline), 20 (vector line) and 21 (centre line). Variables `$n`, definitions `$n=…`, and `+ - x /` with parentheses (128-bit, any rounding reported). Line breaks inside a word are ignored; KiCad wraps long outlines. Primitives 5 and 7 are reported as unsupported, and that aperture is marked inexact. |
| `D01/D02/D03`, `Dnn`, `G01`, `G54Dnn` | Draws, moves, flashes and aperture selection. Coordinates are modal. The deprecated combined form `G01X…D01` is accepted. |
| `G02/G03` | Recorded as an `ARC` object (start, end, I/J) with an `ARC_UNSUPPORTED` warning on its line; never drawn as a straight line. Inside a region, the chord is kept and reported. |
| `G36/G37` | Region contours as G2 polygons. A contour that does not end at its start is an ERROR; so is `D03` inside a region. |
| `LPD/LPC` | Every object records its polarity. |
| `TF/TA/TO/TD` | File attributes; aperture attributes, attached at `AD`; object attributes `.N` (net), `.P` (pin) and `.C` (component), attached to each object; `TD` deletes one attribute or all. |
| `SR` | ERROR: step-and-repeat is not supported. |
| Other | `IP`, `G70`/`G71`, `M00` and the like: reported with the line number and ignored. |

**Shapes:** apertures are built at the origin as G2 shapes:
- circles;
- rectangles, as rounded rectangles with radius 0;
- obrounds, as capsules;
- macro outlines, as polygons;
- vector and centre lines, as quads.

Exactness rules:
- Sizes are exact for mm data and even nanometre sizes.
- Rotations are exact for multiples of 90°; other rotations are rounded and flagged.
- A slanted vector line becomes the capsule that contains it, and its aperture is marked inexact.
- Outlines are cleaned of repeated vertices and back-tracking spikes, which KiCad's FreePoly pads contain. An outline that is still not a simple polygon is replaced by its bounding box, its aperture is marked inexact, and this is reported.

**Draws** with a circular aperture are G2 capsules. Draws with any other aperture are recorded without a shape and reported.

`gerber_outline` chains the draws of a Profile file into loops by exact end points and gives their bounds.

## Evidence

**`gerber` test:** the oracle is `manufacturing_audit/gerber_test`, real KiCad 9.0.6 output for a divider variant (J1, R1, D1), plus `manufacturing_audit/drill_test` from the same export.
- **All 17 files:** each parses with no error and no warning.
  - Each has `.GenerationSoftware,KiCad,Pcbnew,9.0.6`.
  - Every aperture is exact.
  - All 10 region contours are closed.
- **Edge.Cuts:** it is `Profile,NP`, four 0.05 mm draws, and **closes into one 40 × 30 mm loop** (x 0…40, y −30…0).
- **F.Cu:**
  - 7 flashes, plus tracks as 0.1 mm-radius capsules with their nets;
  - the `RoundRect` macro (an outline, 4 circles and 4 vector lines) evaluates to the 0603 pad, 0.8 × 0.95 mm, exactly;
  - net and pin attributes are attached (J1.1 is `/VIN`).
- **Against the KiCad 9.0.6 drill file:** J1's flashes on the front and the back sit exactly on its three drilled holes.
- **Against the C board model:** every flash of R1 and J1 (also present in `examples/generated/divider`) is that pad at (x, −y), with that pad's size, to the nanometre.
- **Inline files** cover:
  - G02 arcs;
  - polarity, modal coordinates and combined forms;
  - every error kind, with its line number;
  - inch units (exact and rounded);
  - macro arithmetic;
  - polygon apertures (exact square, rounded hexagon);
  - FreePoly cleanup and zero-size apertures;
  - attribute deletion.

**`gerber_kicad_demos`** (`gerber_test --real`, with `KICAD_CLI` and `KICAD_DEMOS_DIR` set; it skips as UNVERIFIED otherwise):
- **Method:** ten KiCad 9.0-saved demo boards are exported through the track D runner, and every Gerber file is read back.
- **Result:** **237 files and 172,233 objects with no error.** The only issues are the 558 G02/G03 arcs, reported as unsupported.
- **What it caught:** this stress test found the two gaps above, a line break inside a macro word and FreePoly outlines with repeated vertices, before they were fixed.
- **Caveat:** the Gerber writer here was the KiCad 10.0.5 CLI, so this is a development check. The file-by-file evidence above is KiCad 9.0.6.

**Sanitizers:** both modes run clean under UBSan in trap mode. ASan was not run; it is unavailable on MinGW.

## Not yet

- **Circular interpolation:** G02/G03 is reported as unsupported, as the tracks doc asks. KiCad uses it for arc tracks, round pads drawn on silkscreen, and courtyards.
- **Partly modelled features:** clear polarity is recorded but not subtracted, and neither are aperture holes; step-and-repeat is not supported.
- **The CAM re-read** (`copper.py cam_check`, legacy Python) still checks only the Gerber job file. Comparing Gerber copper against the design with this reader is the next CAM step.
