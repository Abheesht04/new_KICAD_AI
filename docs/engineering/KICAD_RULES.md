# KiCad design rules and Copper profiles

`tools/kicad_rules.py` owns the mapping between a Copper rule profile, `(rules ...)`, and KiCad's design rules, in
both directions. One table drives both:

| Copper field | `.kicad_pro` board constraint | `.kicad_dru` constraint | `.kicad_pcb` setup |
|---|---|---|---|
| `trace_width_mm` | `min_track_width` | `track_width` | |
| `clearance_mm` | `min_clearance` (and net-class `clearance`) | `clearance` | |
| `edge_mm` | `min_copper_edge_clearance` | `edge_clearance` | |
| `via_diameter_mm` | `min_via_diameter` | `via_diameter` | |
| `drill_mm` | `min_through_hole_diameter` | `hole_size` | |
| `annular_mm` | `min_via_annular_width` | `annular_width` | |
| `hole_gap_mm` | `min_hole_to_hole` | `hole_to_hole` | |
| `courtyard_gap_mm` | | `courtyard_clearance` | |
| `mask_expansion_mm` | | | `pad_to_mask_clearance` |
| `mask_web_mm` | | | `solder_mask_min_width` |

`max_aspect`, `max_drop_v`, `max_current_density_a_mm2`, the deratings and `max_height_mm` have no KiCad
equivalent.

## Export: a build hands KiCad its own limits

`pcba project build` writes the profile into:
- `design.kicad_pro` (`board.design_settings.rules`);
- `design.kicad_dru` (one unconditional rule per limit);
- the board's `setup`.

So `pcba project verify` runs KiCad DRC against the same limits copper-core checked, not against KiCad's defaults.

Before 2026-10-26, builds wrote no board constraints (KiCad used its defaults) and exported `hole_gap_mm` as KiCad's `hole_clearance`. That is hole-to-copper, while Copper's `hole_gap` is drill-to-drill, KiCad's `hole_to_hole`. `test_kicad_rules.RoundTrip` now fails on either. Committed example outputs under `examples/generated/` were produced earlier and still show the old files.

## Import: `pcba rules import <project>`

The importer reads:
- `.kicad_pro`: board constraints and net classes;
- `.kicad_dru`: custom rules, with `#` comment lines allowed;
- `.kicad_pcb` `setup`: mask settings.

**The strictest limit wins.** Copper has one global value per limit, so each field takes the largest of:
- the board constraint;
- every net-class clearance, for `clearance_mm`;
- every unconditional custom rule's `min`;
- the board setup.

A single value lower than KiCad's would let Copper pass boards KiCad rejects. A higher one can only fail a board KiCad would accept, which is the safe direction. The report lists every value considered for each field.

**Lengths:** `.kicad_dru` lengths may be in `mm`, `mil`/`th`, `in` or `um`; a bare number is mm.

**Where values come from when KiCad sets none:**
- A board constraint of `0` means KiCad checks nothing. That field then comes from `--base` (for example `profiles/conservative.rules`) or from copper-core's default, and the report says which.
- `--base` also supplies the limits KiCad has no notion of, such as current, deratings and height.

**Provenance:** every limit written gets an entry `(provenance (FIELD "SOURCE" "YYYY-MM-DD") ...)`.
- `SOURCE` names the file and key, for example `board.kicad_pro: net_settings.classes[HV].clearance` or `board.kicad_dru: rule "wide" track_width`.
- `copper-core` accepts the block in any project and checks it: the date format, a known field, and a field the profile actually sets, once each.
- The block is recorded, never used as a value.
- The report also records the SHA-256 of each file read.

**Not imported, and why** (listed in the report; never dropped silently):

| KiCad rule | Why |
|---|---|
| Custom rule with a `condition` or a `layer` | applies to a subset; one global profile cannot express it |
| `max` / `opt` limits | not part of a Copper profile; KiCad DRC checks `max` |
| Constraint types without a Copper limit (`length`, `diff_pair_gap`, `skew`, ...) | Copper does not check them |
| `severity ignore` rules | KiCad does not check them either |
| `min_hole_clearance`, text size, silkscreen and mask-to-copper clearance, microvias, minimum connection | KiCad DRC checks them; copper-core has no such limit |

**Result:**
- PASS when everything KiCad checks is in the profile.
- UNVERIFIED when some KiCad limit is not in it, because KiCad DRC still checks it and Copper cannot. Most real projects land here: `min_hole_clearance` and text height alone do it.
- INVALID_INPUT for a value outside the range copper-core accepts, an unreadable length, a file that is not KiCad 9 format (`(version 1)` custom rules), or malformed JSON or S-expressions.

**Validation before writing:** the profile is accepted by copper-core before it is written. Its header comment says it holds the KiCad project's limits, not a supplier capability claim.

## Checked against

- `tests/test_kicad_rules.py` (CTest `kicad_rules`):
  - a synthetic KiCad 9 project exercising every rule above;
  - the build round trip: profile → KiCad files → profile, all ten mapped fields identical, PASS;
  - copper-core's provenance checks;
  - the CLI.
- Development preview (not native validation): KiCad 10.0.5's own demo projects (`vme-wren`, `CM5_MINIMA_3`, `complex_hierarchy`, `jetson-agx-thor-baseboard`) import, and copper-core accepts each profile. The Slice 1 divider and the ESP32 LED design, built with the new export, still give ERC and DRC `PASS` through a wrapper that reports 9.0.6.
