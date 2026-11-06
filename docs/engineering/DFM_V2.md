# V2 DFM and fab profiles (track C)

`src/v2_src/dfm.{c,h}` checks a board model (track B) against a fab profile. Everything is in exact integer nanometres; the module lives in `copper_v2`. Run it from the command line with `copper-core dfm BOARD.kicad_pcb fab_profiles/NAME.json`, which prints JSON.

`src/v2_src/json.{c,h}` is the strict JSON reader the profile uses, and tracks E and F will use it too:
- duplicate keys, trailing commas, control characters, unpaired surrogates and `\u0000` are all errors, each with its line and column;
- numbers keep their source text;
- `json_to_nm` reads a decimal mm value exactly and refuses exponents or more than 6 decimals.

This is not the legacy `src/dfm.c`, which uses a rule registry in `double` mm. The V2 module has its own include guard and shares no symbols with it. Its test is `tests/dfm_v2_test.c`, because `tests/dfm_test.c` is the legacy one.

## Fab profiles are data

`fab_profiles/jlcpcb_2layer.json` (`copper.fab_profile/1`):

- **Source:** JLCPCB's [PCB capabilities page](https://jlcpcb.com/capabilities/pcb-capabilities), retrieved 2026-11-06. The page shows no update date, so re-check it before relying on it.
- **Every limit carries its source:**
  - the row label and the page's own wording;
  - the profile author's interpretation, where one was needed (e.g. which pairs the general 0.10 mm spacing covers).
- **Unpublished limits are `null`:** hole-to-edge for ordinary holes is not on the page, so its check is UNVERIFIED.
- **The loader refuses incomplete or ambiguous profiles:**
  - a missing or unknown limit key (a misspelling cannot silently drop a rule);
  - a missing source URL or retrieval date;
  - a published value without its row;
  - any value that is not a plain decimal.
- **Conflicts on the page** are listed in `conflicts_on_page`. In each case the stricter value is used:

  | Item | Page says | Profile uses |
  |---|---|---|
  | Minimum drill | 0.15 mm, and elsewhere 0.1 mm | 0.15 mm |
  | Maximum 2-layer size | 670 × 600 mm, and elsewhere 1020 × 600 mm | 670 × 600 mm |

| Rule | Limits (JLCPCB 2-layer, 1 oz) |
|---|---|
| `track_width` | 0.10 |
| `clearance` | tracks and vias 0.10; SMD pad to track 0.10; PTH pad to track 0.28; SMD pad to pad 0.15; any other pair 0.10 |
| `annular_ring` | PTH 0.18 (absolute minimum; 0.25 is recommended but not enforced); NPTH with copper 0.45; via diameter ≥ hole + 0.10 |
| `drill` | via hole 0.15, via diameter 0.25, PTH 0.15, NPTH 0.50, plated slot 0.50, NPTH slot 1.00 |
| `hole_to_hole` | via holes 0.20; any pair with a pad hole 0.45 |
| `hole_to_track` | via hole to another net's track 0.20; NPTH to any track 0.20 |
| `copper_to_edge` | 0.20 (routed edge) |
| `hole_to_edge` | not published: UNVERIFIED |
| `board_size` | 3 × 3 to 670 × 600 mm (from 0.8 mm thick); 599 × 497 mm max when thinner or of unknown thickness; minimum unverifiable below 0.6 mm (JLCPCB reviews such boards manually) |
| `zone_clearance` | NOT_IMPLEMENTED: zone fills are not in the board model |

All values are in mm.

## How a pair is decided

- **Exact shapes:** tracks are capsules; vias and round pads are circles; oval pads are capsules; rect and roundrect pads are rounded rectangles, with KiCad's radius (ratio × the short side). Every size is rounded outward to whole nm, so a shape is never smaller than the copper.
- **Pair search:** a BVH over the copper gives every pair within reach of the largest limit.
- **The test:** each such pair goes through the exact g2 distance comparison, `g2_compare_shape_distance`.
- **Verdict per pair:** a violation is reported only when the exact distance is *below* the limit; a distance exactly at the limit passes.
- **Skipped pairs:** items on different layers, items on the same net, and pads of one footprint that share a number. KiCad's net 0 (`""`) counts as no net, so two unconnected items still need their clearance.
- **Items with no exact shape** are replaced by a shape that contains them:
  - arc tracks, by their conservative bounding box;
  - pads at angles that are not multiples of 90°, by their circumscribed circle;
  - Edge.Cuts arcs and circles, by their box.

  If the containing shape keeps the limit, the pair passes. Otherwise it is UNVERIFIED, never a FAIL. Custom and trapezoid pads are UNVERIFIED as a whole.
- **Report:** violations are sorted (rule, limit, y, x, items). Each one gives the limit, the measured size (for widths, drills and rings), the two items, a location, the shared layers and the profile's quote.
- **Overall verdict:** FAIL if any rule fails; otherwise UNVERIFIED if any rule is not PASS; otherwise PASS.

## Evidence

**`dfm_v2_test`:**
- **Boundaries:** every limit is tested on boards written in the test, once exactly at the limit (PASS) and once 1 nm beyond it (FAIL). This covers track width, track spacing, SMD pad to pad, pad to track, PTH to track, PTH and NPTH annular rings, via diameter over hole, via hole, via diameter, PTH drill, NPTH drill, plated slot, via and pad hole-to-hole, via-hole and NPTH to track, and copper to edge.
- **Other cases:** same-net and no-net cases, measured values and their order, board size (both orientations, the thin-board limit, unknown thickness, below 0.6 mm), UNVERIFIED for arc tracks, 45° pads, custom pads and corner arcs (each also passing when far away), NOT_IMPLEMENTED for zone fills, and profile errors.
- **Golden boards:** the Copper-generated `r0603x5` board and its KiCad 10 re-save give identical reports: every published limit PASSes, and hole-to-edge is UNVERIFIED.

**`dfm_kicad_demos`** (`tests/test_dfm_demos.py`):
- **Method:** on six KiCad 9.0-saved demo boards, every copper pair the C checker decides exactly is re-measured by brute force: all pairs, plain floating-point geometry, tracks taken from the file through the reference parser.
- **Result:** the oracle's 199 violations are exactly the C checker's. Nothing is missed and nothing is extra.
- **Fixes it forced:**
  - the oracle's own wrong assumption that an SMD pad sits on its footprint's side (edge connectors have pads on both);
  - a real gap in both implementations, where items on KiCad's no-net (net 0) were treated as connected.

**Hand-checked samples:**
- **video:** two PTH-to-track violations have copper gaps of 0.266 and 0.2175 mm (limit 0.28).
- **complex_hierarchy:** U101's 1.1 mm pad around a 0.75 mm drill leaves a 0.175 mm ring (limit 0.18).
- **RoyalBlue54L-Feather:** its blind vias have `(drill 0.00001)` in the file, reported as below the 0.15 mm minimum.

**Speed and sanitizers:** every demo board up to 12 MB is checked in at most 1.1 s; the largest checked, video (7,932 tracks), takes 1.1 s. Under UBSan in trap mode the tests and all demo boards run clean. ASan was not run (unavailable on MinGW).

## What these results mean

**The FAILs on the demo boards are real against JLCPCB's limits.** The KiCad demos were not designed for JLCPCB, which needs, for example, 0.28 mm from a plated pad to a track. A FAIL here is not a claim that KiCad's own DRC fails.

**Not checked yet:**
- zone fills;
- copper that lies outside the outline (only the distance to edges is checked);
- pad drill offsets;
- blind and micro via layer pairs beyond their copper span;
- solder mask and silkscreen rules.
