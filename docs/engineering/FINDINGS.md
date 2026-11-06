# Findings and the report (V2 track E)

`src/v2_src/finding.{c,h}` gives every checker one result type, `Finding`, and writes one sorted JSON report (`copper.findings/1`). The JSON writer is in `src/v2_src/json.c`, beside the reader. From the command line, `copper-core report BOARD PROFILE [DRILL|GERBER...]` writes the report for a board's DFM, its model issues and any drill and Gerber files.

## One Finding

`Finding` has these fields:
- `source`, `rule`, `status`, `severity`, `file`, `line`;
- `at_nm` and `layers`;
- `limit_nm` and `measured_nm`;
- up to two `items` (e.g. `pad R1.2`, `track 12`);
- `message`;
- `fabricator_says`: the profile's quoted wording for the DFM limit involved.

Lengths are integer nm.

| Source | Becomes |
|---|---|
| `dfm` (`DfmReport`) | every violation is a FAIL, with its rule and limit (`clearance/pth_to_track_min_mm`), the limit, the measured size, the items, the location and the fabricator's wording; every rule that is UNVERIFIED or NOT_IMPLEMENTED is one finding giving the reason and the count; passing rules are counted in the source summary |
| `excellon` (`ExcFile`) | every issue, by name (`excellon/undefined_tool`), with its line: ERROR → INVALID_INPUT, WARNING → UNVERIFIED, INFO → INFO |
| `gerber` (`GerberFile`) | every issue, by name (`gerber/arc_unsupported`), with its line: ERROR → INVALID_INPUT, WARNING → UNVERIFIED |
| `board` (`Board`) | every model issue (`board/inexact_rotation`, …) → UNVERIFIED |

**Statuses** are the project's honest set: FAIL, INVALID_INPUT, UNVERIFIED, NOT_IMPLEMENTED, INFO.

**The report's verdict** follows this precedence:
1. FAIL, if anything failed;
2. otherwise INVALID_INPUT, if an input could not be read correctly;
3. otherwise UNVERIFIED, if anything is unverified or not implemented;
4. otherwise PASS.

A list that ran out of memory reports `complete: false` with verdict INVALID_INPUT, never PASS.

**Sorting:** findings are sorted by a total order (source, file, rule, line, position, items, message), and sources by (source, file). A report depends only on its inputs, not on the order they were added; the test adds them in two orders and compares.

**The writer:** 2-space indentation, members in call order, integers only, strings escaped per RFC 8259 with UTF-8 passed through. Misuse fails closed, with no output: a value where a key is due, unbalanced ends, or two top-level values.

## Golden report

[tests/golden/report.json](../../tests/golden/report.json) is built from these inputs:

| Input | Kind | Contributes |
|---|---|---|
| `tests/golden/findings/board.kicad_pcb` vs `fab_profiles/jlcpcb_2layer.json` | test-authored board, real JLCPCB limits | FAIL track width (0.08 < 0.10 mm), FAIL track spacing, FAIL via hole (0.12 < 0.15 mm); UNVERIFIED for an arc track near another net and for hole-to-edge (not published by JLCPCB); NOT_IMPLEMENTED zone fills; board UNVERIFIED for a 30° footprint |
| `manufacturing_audit/drill_test/design.drl` | real KiCad 9.0.6 | nothing (PASS) |
| `tests/golden/findings/broken.drl` | test-authored | INVALID_INPUT undefined tool, UNVERIFIED unknown command and missing end |
| `manufacturing_audit/gerber_test/design-Edge_Cuts.gm1` | real KiCad 9.0.6 | nothing (PASS) |
| `tests/golden/findings/arc.gbr` | test-authored | UNVERIFIED for an arc, an ignored IP command and a missing M02 |

**Verdict:** FAIL, with 13 findings. `finding_test` compares the report byte for byte (line endings aside) and checks its content independently. Regenerate it with `finding_test --write-golden tests/golden/report.json`, then review the diff.

**Fixed while building it:** the protected Excellon reader returned `EXC_ERR_ARGUMENT` and stopped reading at any non-coordinate line after an undefined tool. As a result, `broken.drl` reported one issue instead of three. This is fixed, with a regression test in `kicad_native_test`, and noted in its own commit.

## Not yet

- The kicad-cli runner's results (track D) and KiCad's DRC JSON are not mapped into findings yet.
- `tools/copper.py` still writes its own reports; moving them onto this format belongs to port steps 9–10 in [C_FIRST_AUDIT.md](C_FIRST_AUDIT.md).
