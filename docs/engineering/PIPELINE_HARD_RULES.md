# Pipeline hard rules

These rules hold in every condition, for every step of the prompt → board pipeline:

```
prompt → intent → constraints → parts → topology → physics → KiCad files → placement/routing
       → DFM → extraction → post-layout physics → corrections → KiCad 9 gates → CAM → revision → sign-off
```

They add to `CLAUDE.md` and `CHARTER.md`; they never relax either. If a rule here seems to conflict with one of
them, the stricter rule applies and the owner is asked. Changing this file needs the owner's explicit approval.

Adopted by the owner on 2026-11-21.

| # | Rule | What it means in practice |
|---|---|---|
| **H1** | **The AI proposes; C decides.** | Model output (Manus or any other) never goes into a design file directly. Every model output is a schema-checked document that a C validator accepts or rejects. A model never sets PASS, ROUTED, verified or approved. |
| **H2** | **No data without a source.** | Every rating, MPN, fab limit, block formula and default carries a source: a document or URL, plus the date it was retrieved. A value without a source is UNVERIFIED. Distributor data (for example jlcparts or LCSC listings) is always labelled as distributor data and never reported as verified. |
| **H3** | **Never silently drop part of a request.** | Anything the pipeline does not build is listed as NOT_IMPLEMENTED or NOT_HANDLED. A partial result is never reported as complete. |
| **H4** | **Automatic fixes may only improve.** | A fix is kept only if the FAIL count goes down and no new FAIL or UNVERIFIED appears. The number of tries is limited, and every attempt, whether kept or rejected, is logged. When the limit is reached, the honest result is FAIL. |
| **H5** | **Deterministic.** | The same inputs, data versions, tool versions and seed give byte-identical output files. The seed and versions are recorded in the output. |
| **H6** | **A generator never grades itself.** | Every generator (block composition, schematic and PCB writers, placer, router, fixer) is checked by code that is independent of it (a separate checker in the tests, the DFM engine, or KiCad). |
| **H7** | **Claims come only from tool results.** | PASS, ROUTED, "connected" and similar words appear only when a tool output says so. KiCad 9.x is the only native evidence; KiCad 10 results are development checks. |
| **H8** | **A human approves manufacturing.** | Copper never marks a board manufacturing-approved. The last gate needs a named person's sign-off, recorded with the revision. |
| **H9** | **No regressions.** | A step is complete only when the full testing loop (below) passes and no baseline number is worse. Tests are never weakened to pass. A golden-file change needs a stated reason and the owner's approval. |
| **H10** | **Keep the base; add beside it.** | Existing modules and the base architecture stay as they are: no rewriting, moving or deleting of working code (CLAUDE.md §2). New work adds new files. No new engine is added for a job that already has one; new code uses the engine recorded for that job in `CONSOLIDATION_PLAN.md`. |
| **H11** | **Traceable outputs.** | Every output file is hashed (SHA-256) and linked to its inputs: prompt, analysis, data versions, tool versions. |
| **H12** | **Secrets and servers stay safe.** | API keys never go in the repository, logs, reports or chat. Servers bind to loopback only. Live model runs that spend credits happen only when the owner asks. |
| **H13** | **Owner approval per step.** | Every step is planned (files, what is new, risks, done-when), approved by the owner, then implemented and tested, and its results reported, before the next step starts. |

## Testing loop (run after every step)

| # | Check | Pass when |
|---|---|---|
| T1 | Build every build tree with `-Wall -Wextra -Wpedantic -Werror`, plus one clean build with `-DCOPPER_V2_SANITIZE=ON` (UBSan in trap mode on `copper_v2` and `copper_design`) | 0 warnings. On MinGW, ASan is reported as NOT RUN, never as passed. |
| T2 | Full `ctest`, without MinGW first on `PATH` | 100 % pass; every skipped test is listed by name with its reason |
| T3 | New tests for the step, written first | each new module has `tests/<module>_test.c` and a golden file, labelled Copper-generated or KiCad-exported |
| T4 | `manus/selftest.ps1` (mock only; spends no credits) | every step passes; the count is not lower than the baseline |
| T5 | `manus/golden_boards.ps1 -Workspace <ws> -Out <new dir>` regenerates the golden boards (LED indicator, low-side MOSFET switch, ESP32 + LED) twice and compares them with `manus/golden/baseline.json` | `GOLDEN_BOARDS PASS`: ROUTED; KiCad DRC 0 unconnected items; no ERC or DRC count above the baseline; two runs byte-identical. `CHANGED` (a different hash) needs a stated reason and the owner's approval |
| T6 | Rule lint: no float in geometry, DFM or routing; no `malloc`, `printf` or `exit` in new library code | no new hits |
| T7 | Documents | `GAP_REGISTER.md`, `PROMPT_RESULTS.md` and `V2_STATUS.md` updated |
| T8 | Report to the owner | PASS, FAIL or UNVERIFIED, with numbers and the KiCad version used |

The baseline numbers are recorded in `PIPELINE_BASELINE.md`.
