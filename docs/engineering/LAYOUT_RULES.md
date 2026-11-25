# Copper layout rules: schematic, placement, routing, DFM and DRC

Status: **PROPOSED** (2026-11-25). Nothing here is enforced until the owner approves each step in section 8.
Steps 3a to 3d are done (owner approved, 2026-11-25).

**Target fab (owner decision, 2026-11-25):** the target is a parameter, and the default is **JLCPCB**. "Global" (the
strictest of all profiles) is always reported next to the target as a per-fab verdict, never used as the default gate.
The reason: global can only get stricter as fabs are added, and it would already rule out the ESP32-S3 module and the
fine-pitch parts. Adding a fab is adding a data file.
Every number below is either a fabricator's published limit (with its page) or a Copper design choice, which
must sit above the strictest fabricator limit and says why. No number is invented. Where a source was not read
directly, it is marked UNVERIFIED.

## 1. Why the composed boards fail today (measured, 2026-11-21..25)

| Finding | Evidence |
|---|---|
| The generated `.kicad_pro` holds **no design rules**, so KiCad DRC checks KiCad's defaults (min hole 0.3 mm, clearance 0.2 mm), not the fabricator's or Copper's rules. | `Esp32S3Leds.kicad_pro` is `{"meta":…}` only. DRC: "board setup constraints min hole 0.3000 mm; actual 0.2000 mm" ×12 (ESP32-S3 module's thermal vias). |
| The router uses one clearance (0.3 mm) for every pad. Two library footprints have pads closer than that, so their pads have no free grid cell. | Pad gaps measured from the KiCad 10.0.5 footprints: SOT-563 **0.150 mm**; USB-C GCT USB4105 **0.200 mm**; ESP32-S3-WROOM-1 0.370 mm; all others ≥ 0.40 mm. |
| ERC errors: `power_pin_not_driven` ×2 (the LDO input and the 3V3 net are fed from a connector, which KiCad sees as passive). | ERC report, both ESP32 boards (template and composed). |
| ERC/DRC warnings: `lib_symbol_issues` ×19, `footprint_link_issues` ×19, `lib_footprint_issues` ×19. The project has no library tables. | ERC/DRC reports. |
| Silkscreen: reference text over pads (`silk_over_copper` ×13) and over other parts (`silk_overlap` ×6). | DRC report, board `esp32s3_leds`. |
| Unused block ports leave parts with a floating pin (R2/R3, 22 Ω USB resistors). | G-077. |

## 2. Fabricator limits (2-layer, 1 oz, standard service)

Each value is quoted from the page named. "Global" is the strictest of the three: a board that meets it can be
ordered from any of them. None of the pages shows a last-updated date; re-check before relying on them.

| Limit | JLCPCB [1] | PCBWay [2] | OSH Park [3] | **Global** |
|---|---|---|---|---|
| Track width min | 0.10 mm | 0.10 mm | 0.1524 mm | **0.1524 mm** |
| Copper spacing min | 0.10 mm | 0.10 mm | 0.1524 mm | **0.1524 mm** |
| SMD pad to pad (different nets) | 0.15 mm | not stated | not stated | **0.15 mm** (JLCPCB only; UNVERIFIED for the others) |
| Pad to track | 0.10 mm | not stated | not stated | **0.10 mm** (JLCPCB only; UNVERIFIED for the others) |
| Default copper clearance (pairs with no specific row) | 0.10 mm | 0.10 mm | 0.1524 mm | **0.1524 mm** |
| Min drill | 0.15 mm | 0.15 mm | 0.254 mm | **0.254 mm** |
| PTH pad annular ring | 0.18 mm (absolute min) | not stated for pads | 0.127 mm | **0.18 mm** |
| Via ring | via diameter ≥ hole + 0.10 mm (ring 0.05 mm) | 0.15 mm ("pads with vias in the middle") | 0.127 mm | **0.15 mm** |
| Via hole / via pad | 0.15 / 0.25 mm | (drill 0.15) | (drill 0.254) | **0.254 mm hole** |
| Copper to routed edge | 0.20 mm | not in standard table | 0.381 mm | **0.381 mm** |
| Hole to edge | not stated (castellated only) | not in standard table | not stated | **UNVERIFIED** (no source) |
| Via hole to hole | 0.20 mm | not in standard table | 0.127 mm recommended | **0.20 mm** |
| Pad hole to hole | 0.45 mm | not in standard table | — | **0.45 mm** |
| Solder mask web (bridge), green, 1 oz | 0.10 mm | 0.1016 mm (4 mil, advanced table only) | 0.1016 mm | **0.1016 mm** |
| Silkscreen line width | 0.15 mm | 0.15 mm (character width) | 0.127 mm recommended | **0.15 mm** |
| Silkscreen text height | 1.0 mm | 0.8 mm | not stated | **1.0 mm** |
| Pad to silkscreen | 0.15 mm | not stated | not stated | **0.15 mm** |

Sources:
1. JLCPCB, Rigid PCB Manufacturing Capabilities, https://jlcpcb.com/capabilities/pcb-capabilities, read 2026-11-06 and 2026-11-25. The page also says "Min. drill diameter for 2- or more-layer PCBs is 0.1 mm (more costly!)"; the stricter 0.15 mm is used.
2. PCBWay, PCB Capabilities – Standard PCB, https://www.pcbway.com/capabilities.html, read 2026-11-25.
3. OSH Park, 2 Layer Prototype Service, https://docs.oshpark.com/services/two-layer/, read 2026-11-25.

IPC-2221B electrical spacing (B2, external uncoated, 0–15 V: 0.1 mm) was seen only in secondary sources and is
**UNVERIFIED**; the 0.1524 mm global spacing is above it either way. Boards above 15 V need that table checked.

**Consequences, reported, not hidden:**
- The ESP32-S3-WROOM-1 KiCad footprint has 0.2 mm thermal-via holes: within JLCPCB and PCBWay (0.15 mm), **below OSH Park (0.254 mm)**. Under "global" it is FAIL until the footprint is changed or the fab is chosen.
- SOT-563 pads are 0.150 mm apart: JLCPCB PASS (0.15 mm). OSH Park and PCBWay publish no pad-to-pad limit, so it is
  UNVERIFIED for them. (OSH Park's 0.1524 mm "Trace Spacing" is not read as a pad rule; the profiles do not stretch a row.)
- Measured (Step 3a, `copper-core dfm` on golden board `esp32s3_leds`): JLCPCB UNVERIFIED (hole_to_edge only);
  PCBWay UNVERIFIED (6 rules it does not publish); OSH Park **FAIL** (drill ×12); global **FAIL** (drill ×12).

## 3. Copper design rules (above every fab limit)

Kept in `routing_profiles/*.json`. `copper design route` already refuses rules below the fab profile (INVALID_INPUT).

| Rule | Value | Why |
|---|---|---|
| R1 default track | 0.25 mm | 1.6× global minimum |
| R2 default clearance | 0.30 mm | above every copper limit, the largest being JLCPCB PTH-to-track 0.28 mm |
| R3 edge clearance | 0.50 mm | above the global 0.381 mm |
| R4 via | 0.6 / 0.3 mm | hole ≥ global 0.254 mm; ring 0.15 mm = the global via-ring minimum (no margin: kept, but reported as at the limit) |
| R5 **fine-pitch escapes** (Step 3c) | A footprint whose own pads (different nets) are closer than R2 is fine-pitch. Before any net is routed, each of its connected pads gets an escape stub: a straight track `fine_pitch_neck_mm` (0.2) wide from the pad centre to the nearest free grid cell. The stub keeps `fine_pitch_clearance_mm` (0.15) to the other SMD pads of its own footprint, and R2 to everything else. Later nets treat the stubs and their end cells as obstacles, so no path can wall a pin in. Nothing else is relaxed. | The only way to connect SOT-563 and USB-C pads. Both values must be at or above every fab copper limit; `copper design route` and `copper-core project-rules` refuse them otherwise (so the default rules are refused for OSH Park, whose default clearance is 0.1524 mm). KiCad checks the same pairs: rule `copper_fine_pitch_<REF>_escape`, condition `A.memberOfFootprint(REF) && B.Type == 'Track' && B.intersectsCourtyard(REF)`. |
| R6 power nets | width from the net's current (existing `net_widths_mm`) | unchanged |

A footprint whose own pads are below the chosen fab's pad-to-pad limit is **FAIL** for that fab (fail closed); R5
never lowers anything below the fab profile.

## 4. Schematic rules (ERC must be clean of errors)

| Rule | What Copper does |
|---|---|
| S1 power flags (Step 3d) | A block port marked `external_supply` (the header's `vout` and `gnd`, USB-C's `vbus` and `gnd`) puts its net in `layout_hints.json` `power_flags`. `copper design schematic-rules` then adds one `power:PWR_FLAG` on each such net: the symbol is copied verbatim from KiCad's power library, in KiCad 9's own instance form, with a label at its pin. Nowhere else, so a power net no output drives is still an ERC error. Fixes `power_pin_not_driven`. Composed designs only; the template path has no block data, and its ESP32 board keeps its 2 ERC errors. |
| S2 no floating pins (Step 3d) | Already met: the compiler gives every unused pin a no-connect flag (33 on the sensor board), and ERC reports no unconnected pin. Unused block ports that leave a two-terminal part with a floating pin are reported by the composer as UNVERIFIED `UNCONNECTED_PORT_PARTS` (G-077), naming the part's role and pin. They are not dropped silently and not refused: the part is fitted. |
| S3 library tables (Step 3d) | `copper design schematic-rules` writes `sym-lib-table` and `fp-lib-table` with one entry per library the schematic and board use, in KiCad 9's standard form (`${KICAD9_SYMBOL_DIR}`, `${KICAD9_FOOTPRINT_DIR}`; kicad-cli 10.0.5 resolves them too). A table Copper did not write is never overwritten. Fixes the library-not-in-configuration warnings. KiCad then compares each part with its library copy, which found two compiler bugs, both fixed (S4). |
| S4 symbols and footprints match their libraries (Step 3d) | Legacy compiler fixes: (1) it added `(pin_numbers (hide yes))` and `(pin_names (offset 1.016))` to symbols that omit them, which are not KiCad's defaults (`lib_symbol_mismatch` on U1, U2, U3); (2) it dropped the library footprint's other properties (Datasheet, Description) and the labels' `(unlocked yes)`. One mismatch remains: KiCad 10.0.5 flags the ESP32-S3-WROOM-1 footprint even when the library text is copied verbatim (G-085). |
| S4 net names | Every block port net has a stated name (already true). |

## 5. Placement and silkscreen rules

| Rule | What Copper does |
|---|---|
| P1 courtyards | No two courtyards overlap (placer, already). |
| P2 keep-outs | Block keep-outs (antenna) and edge strips (already). |
| P2a escape room (Step 3c) | A fine-pitch part (own pads closer than `fine_pitch_gap_mm`, 0.3) has its placement body grown by `fine_pitch_escape_mm` (1.0) on every side. Without it, the placer put a pull-up 1.2 mm from the SOT-563 and its middle pin could not escape. |
| P3 decoupling | Decoupling capacitor next to its IC pin (already a placer cost). |
| P4 silkscreen box | Each part's reference text box is placed with the part. It must be ≥ 0.15 mm from every pad (fab pad-to-silk limit) and must not overlap another part's silk or text. If it cannot fit, the reference goes on the Fab layer only and this is reported. |
| P5 text size | Reference text ≥ 1.0 mm high, stroke ≥ 0.15 mm (global). |

## 6. DRC rules carried in the KiCad project

The KiCad project must check the **same** limits Copper used, never KiCad's defaults:
- `.kicad_pro` board design settings: minimum clearance, track width, hole, via ring, via, copper-to-edge, hole-to-hole, hole clearance and text size, from the target fab profile. The `Default` net class gets Copper's routing rules: clearance 0.3, track 0.25, via 0.6/0.3 mm.
- `.kicad_dru`: the R5 fine-pitch rules, one per fine-pitch footprint, with the condition `A.memberOfFootprint('REF') && B.memberOfFootprint('REF')`, at the fab's pad limit (pad-to-pad, or the default copper clearance when the fab publishes no pad limit).

**Implemented in Step 3b:** `src/v2_src/project_rules.c` and `copper-core project-rules BOARD FAB RULES`, run by `copper.ps1 compile` after routing. Key names come from the real KiCad 9.0 project `src/v2_src/tests/fixtures/sample_resistor_lol`. Facts checked with kicad-cli 10.0.5:
- the rules apply even with the compiler's `meta.version 1`;
- `min_silk_clearance` is silkscreen to silkscreen (raising it raised `silk_overlap` from 4 to 9), so the fab's pad-to-silk limit is **not** mapped to it;
- `memberOfFootprint` relaxes only the footprint's own pad pairs;
- **kicad-cli silently ignores a rule file it cannot parse**, which fails closed, because the stricter net-class clearance then applies. The ctest `project_rules_drc` proves the rule is evaluated: with OSH Park's floor above the 0.15 mm gap, KiCad reports the 4 pairs under Copper's rule name.

**Not carried into KiCad, and reported** (`other_fab_limits` in `validation/project_rules.json`):
- pad hole to hole: KiCad has one hole-to-hole minimum, set from the via limit;
- solder mask web: a `.kicad_pcb` setup value;
- pad to silkscreen: KiCad has no such minimum;
- hole to edge: no fab publishes it.

Copper's own DFM (`copper-core dfm`) still checks pad hole to hole.

KiCad's rule-expression properties are taken from KiCad's own demo files and checked by running `kicad-cli pcb drc`.
The KiCad 9 documentation page could not be fetched (404), so nothing is taken from memory. KiCad 10.0.5 runs are a
development check, not KiCad 9 evidence.

## 7. Recommendation on G-076

**(a), with (c) for what (a) cannot fix.** (b) alone does not help, because the USB-C connector has no coarser
package and has the same problem at 0.200 mm. (b) remains useful later for an OSH-Park-compatible sensor
(TMP1075 comes in SOIC-8 and VSSOP-8 per TI SBOS854F; it is LM75/TMP75-compatible, **not** a drop-in TMP102, so it
would be a new block with its own datasheet data).

## 8. Steps (each needs the owner's approval, H13; C only, CLAUDE.md §1)

| Step | Change | Done when |
|---|---|---|
| 3a | Fab profiles: add `pcbway_2layer.json` and `oshpark_2layer.json` with quotes; C `src/v2_src/fab_global.c` and `copper-core fab-global OUT PROFILE...` build the "global" profile from them (no hand-copied numbers). Solder-mask and silkscreen rows (`mask_silk_limits`) added to all three. | **DONE**: `fab_global` test; global values equal section 2 |
| 3b | KiCad project rules: new C module writes `.kicad_pro` design settings and `.kicad_dru` from the fab profile and the routing rules. `copper.ps1 compile` runs it after the compiler. | **DONE**: `drill_out_of_range` 12 → 0 on `esp32` and `esp32s3_leds` (JLCPCB); the LED + TMP102 board's 4 SOT-563 clearance errors → 0 (rule at JLCPCB's 0.15 mm). The global/OSH Park FAIL stays in Copper's DFM (Step 3a). |
| 3c | Router R5 fine-pitch escapes (fan-out first) in `autoroute.c` (base module; note in G-076), escape room in placement (`place_intent.c`), and the escape rule in `project_rules.c`. Data in both profiles. | **DONE**: golden board `esp32s3_sensor` (ESP32-S3 + LED + TMP102) ROUTED, 0 unconnected, 0 DRC errors; the other golden boards are byte-identical |
| 3d | Schematic S1–S4: `external_supply` ports (data), power flags and library tables written by C (`sch_rules.c`, `copper design schematic-rules`), ERC re-run on the final schematic, G-077 reporting, two legacy compiler fixes. | **DONE**: golden board `esp32s3_sensor` ERC 0 findings; DRC 0 errors (warnings: silkscreen, which is Step 3e, plus G-085) |
| 3e | Silkscreen P4–P5 in the placer problem and apply step. | DRC `silk_overlap` 0; `silk_over_copper` 0 or each reported |

The legacy Python compiler is fixed only if a step cannot be done in C beside it. Where that happens, the step says so.
