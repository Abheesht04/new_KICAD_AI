# M0 — trustworthy acceptance evidence

Response to `COPPER_EXPECTATIONS_VS_REALITY_DFM_AUDIT_2026-10-23.md` (§3, §7 P0) and milestone M0 of
`COPPER_NEXT_DIRECTION_QWEN_KICAD_PROMPTS_2026-10-23.md`, using the owner's run outputs in `out.zip`
(`out/prompt-acceptance/20261023-181130` and `20261023-181221`). Every finding below was reproduced from those
files before it was changed; each change has a regression test. No tolerance was weakened and no product
finding was suppressed.

## Findings, root causes and fixes

| # | Reported | Root cause (from the archived bytes and logs) | Fix | Regression test |
|---|---|---|---|---|
| 1 | "rc filter with unicode units" and "micro sign and ohm sign" expected PASS, got INVALID_INPUT | **Harness:** the archived `prompt.txt` holds `C3 82 C2 B5` for "µ" and `C3 8E C2 A9` for "Ω" — UTF-8 that was decoded as Windows-1252 and re-encoded (Windows PowerShell 5.1 reads files as ANSI unless `-Encoding UTF8` is given). The parser received "Âµs"/"kÎ©". With correct UTF-8 both prompts already PASS through `pcba` (arguments are read with `CommandLineToArgvW`, children started with `CreateProcessW`) | New harness keeps prompts UTF-8 end to end and runs non-ASCII prompts both as arguments and through `spec parse --file`; the parser now **names** double-encoded text ("'Âµ' (probably 'µ')") instead of "not understood", never silently repairing it; `--file` accepts a UTF-8 BOM and reports non-UTF-8 files clearly | `test_spec.test_double_encoded_text_is_named_and_bom_ignored`; `test_prompt_acceptance.UnicodeRun` |
| 2 | (found while reproducing 1) | **Product:** `pcba`'s JSON envelope copied the backend message with escapes stripped, so non-ASCII became literal `u00c2` | `extract_message` decodes JSON escapes, including `\uXXXX` and surrogate pairs, to UTF-8 | `test_prompt_acceptance.test_double_encoded_text_is_named_not_misparsed` |
| 3 | "DC solve from English" printed `[FAIL]` with `PASS/0; dc_solved=True; VOUT=3.33333333333` | **Harness** assertion; the product result was correct | New harness compares status, exit code, `dc_solved` and V(OUT) with the analytical 5 V × 20 kΩ / 30 kΩ within 1e-9 relative | `prompt_acceptance.py` case `e2e:dc` |
| 4 | "KiCad board generated from English": `NO_JSON/70; NO_PCB_EXPORTED` | **Product:** `e2e_divider_build_kicad.stderr.log` says `Cannot escape pad on net OUT`. A project lowered from a spec has no `(at …)`, so every footprint sat at (0, 0) and the router could not leave the first pad | `project build` of a project with **no** placed part now runs the deterministic staged placement of `pcba place` (re-checked by copper-core) and records it in `manifest.json` (`placement`); a **partly** placed project is refused (INVALID_INPUT) rather than mixed; no feasible placement is FAIL with its findings | `test_native_gates.Placement`; harness case `e2e:board` (2 footprints, 4 pads, 3 named nets) |
| 5 | Optional ESP32 run printed `[PASS] optional:…` while `design.json.status = FAIL` | **Harness** taxonomy | Outcomes are PASS, FAIL, BLOCKED, SKIPPED, UNVERIFIED, NOT_IMPLEMENTED, OBSERVATIONAL; only PASS counts; the ESP32 run is OBSERVATIONAL with its gates listed | `test_prompt_acceptance.test_observational_failure_is_not_a_pass` |
| 6 | ESP32 `design.json`: gate 8 `kicad_erc_drc` FAIL (KiCad **9.0.6**, DRC exit 5) | `drc.json` lists **one warning** and nothing else: `lib_footprint_mismatch` — "Footprint 'ESP32-WROOM-32E' does not match copy in library 'RF_Module'". The gate equated any non-zero exit with FAIL, but `--exit-code-violations` counts warnings too. The board copy and the snapshotted library copy are identical in pads, keepout zone and 3D model (only Reference/Value text differs). Not reproducible with KiCad 10.0.5 (no warning there); the likely cause — KiCad 9.0.6 comparing the module's keepout zone on In1–In30 against a 2-layer board — is unconfirmed | Native gates classify from KiCad's own report: errors, unconnected items and schematic-parity issues → FAIL; warnings only → **UNVERIFIED** with the warning listed (not hidden, not PASS); exit non-zero without a readable report → UNVERIFIED. stdout, stderr and exit code are kept separately; stale reports are deleted before each run | `test_native_gates.Classification` (fake `kicad-cli`) |
| 7 | Raw-board DRC logs with `EXCEPTION: Legacy zone fill strategy is not supported anymore.` and fallback exit 9009 | That runner is the owner's PowerShell script (not in this repository). Copper's own gate never reads stderr as a result | Covered for Copper's gate: stderr noise with a clean report is PASS, a crash without a report is UNVERIFIED | `test_native_gates.test_stderr_noise_with_a_clean_report_passes`, `test_crash_without_report_is_unverified` |
| 8 | Empty boards passed raw DRC (0 pads, 0 nets) | Native DRC on an empty board proves nothing | `project verify` adds a `board_content` check before native gates: every physical part's footprint and every connected pin's pad on its IR net must be on the board; otherwise native checks are not run (UNVERIFIED) and the check FAILs; a design with no physical part is UNVERIFIED | `test_native_gates.Content` |
| 9 | (found while unifying) | `design.py` and `place` each built the "placed project"; the `design.py` copy crashed when the project had no `(rules …)` and its traces were widened | One `layout.placed_project` used by `place`, `design` and `build` | `test_layout.test_placed_project_records_widened_copper_even_without_rules` |

## The repository harness

`tools/prompt_acceptance.py` (wrapper: `tools/copper_prompt_acceptance.ps1`) replaces the owner's script as the
maintained acceptance suite. It runs the official corpus (`tests/fixtures/spec/corpus.json`), `.espec`
round-trips, semantic equivalence, and English → `.copper` → DC → KiCad project → (optional) native verify, plus
the optional ESP32 run. Each step's exact argv, working directory, exit code, stdout and stderr files are in
`results.json`.

Run on 2026-10-24 (this machine: committed KiCad 9 library snapshot, KiCad 10.0.5 CLI):
**PASS 34, FAIL 0, SKIPPED 1** (native: KiCad 10.0.5 is not the 9.x validation target).

```powershell
.\tools\copper_prompt_acceptance.ps1 -Pcba .\build\pcba.exe -VerifyNative -KicadCli "C:\Program Files\KiCad\9.0\bin\kicad-cli.exe" -RunEsp32Design
```

## What did not change, and what is still open

* The product verdicts are unchanged: the English divider board is UNVERIFIED (unknown ratings and heights);
  the ESP32 design's DRC gate is now UNVERIFIED with the `lib_footprint_mismatch` warning shown, not PASS.
* The cause of KiCad 9.0.6's `lib_footprint_mismatch` on the ESP32 module needs confirmation on a KiCad 9
  machine (open the board, run *Update Footprints from Library* on U1, re-run DRC, and diff the footprint).
* The divider request names no connector, so its 5 V source is off-board and the board carries R1 and R2 only;
  that is the request as stated, not a missing part.
* Later milestones (KiCad discovery service, local library index, Qwen adapter, per-rule DFM records) are not
  part of M0.
