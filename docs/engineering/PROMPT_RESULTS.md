# Prompt results log (P01–P52)

One entry per prompt, newest last. Each entry records:
- the revision and environment;
- what changed, with the files;
- the exact commands, with their counts and exit codes;
- the remaining limits and the next dependencies.

The rules are in [IMPLEMENTATION_CONTRACT.md](IMPLEMENTATION_CONTRACT.md). The gaps are in [GAP_REGISTER.md](GAP_REGISTER.md).

---

## P00: baseline inventory and tracking files (2026-11-08)

*First logged as "P01". The owner numbers the F1 fix P01, so this inventory is P00.*

**Scope:** establish the actual baseline before any fix. **No product behaviour was changed in P00.**

### Revision and environment

| Item | Value |
|---|---|
| Repository | `Abheesht04/new_KICAD_AI`, local `D:\New folder (5)` |
| Branch / HEAD | `v2/complete` @ `7199ef6b1e13a3df579c57b3d7cd874d38cb60ee` (committed 2026-11-07 02:24 +0530) |
| Relation to the earlier assessment | **Same commit.** No commits since. Nothing was reset or discarded. |
| Uncommitted at start | untracked: `CLAUDE_CODE_V2_EVOLUTION_PROMPTS.pdf`, the two owner documents dated 2026-10-23, `Copper_V2_Summary.pdf`, `docs/engineering/ARCHITECTURE_PLAN.md` |
| OS | Windows 10 Pro 10.0.19043 |
| Toolchain | MinGW-w64 GCC 12.1.0 (MSYS2 Rev2); CMake 4.4.4; Ninja 1.13.2; Python 3.12.0 |
| KiCad | kicad-cli **10.0.5** only (development preview). **No KiCad 9.x.** |
| Not present | KiCad 9.x, `nvcc` and a GPU (`nvidia-smi`), Ollama, MSVC, CI configuration, ASan runtime |
| Shell variables | `TMP` and `TEMP` = `D:\copper-build-tmp` (outside the repository); MinGW not first on `PATH` for ctest |

### Documents read

`CLAUDE.md`, `CHARTER.md` §3.1, `README.md`, `CMakeLists.txt`, `CMakePresets.json`, and in `docs/engineering/`: `C_FIRST_AUDIT.md`, `V2_STATUS.md`, `RELEASE_READINESS.md`, `M0_ACCEPTANCE_EVIDENCE.md`, `ARCHITECTURE_PLAN.md` and `CLI.md` (exit codes). Also the two owner reports dated 2026-10-23: `COPPER_EXPECTATIONS_VS_REALITY_DFM_AUDIT_2026-10-23.md` and `COPPER_NEXT_DIRECTION_QWEN_KICAD_PROMPTS_2026-10-23.md`.

There are no `AGENTS.md` files in the repository.

The policy conflict check found nothing to resolve. `README.md`, `C_FIRST_AUDIT.md` and the contract all follow the dated C17 rule in `CHARTER.md` §3.1 and `CLAUDE.md` (2026-11-06).

**F1–F8:** their texts were not found in any of these sources or in `CLAUDE_CODE_V2_EVOLUTION_PROMPTS.pdf` (text extracted with `pdftotext`; no F-numbered findings and no P-numbered prompts). They are registered as BLOCKED_EXTERNAL until the owner supplies them.

### Commands run, with results

| # | Command | Result | Exit |
|---|---|---|---|
| 1 | `cmake --build build-v2` (MinGW on `PATH`) | `ninja: no work to do`: the tree was current at HEAD | 0 |
| 2 | `ctest --test-dir build-v2 -j6` (no KiCad or demo variables) | **76/76 passed, 11 skipped**, 0 failed, 321 s. Skipped (exit 77): `kicad_cli_real`, `gerber_kicad_demos`, `board_kicad_demos`, `dfm_kicad_demos`, `llm_ollama_live`, `dfm_gpu_compare`, `v2_native`, `parts_native`, `layout_native`, `design_acceptance`, `slice1_divider` | 0 |
| 3 | `KICAD_CLI=…\kicad-extract\bin\kicad-cli.exe KICAD_DEMOS_DIR=…\share\kicad\demos ctest --test-dir build-v2 -R "kicad_cli_real\|gerber_kicad_demos\|board_kicad_demos\|dfm_kicad_demos"` | **4/4 passed**, 125 s. The KiCad 9.0-saved demo board files were read natively; the exports came from kicad-cli 10.0.5, a **development check** and not native-9 evidence | 0 |
| 4 | `python build-tools/v2_rules_check.py .` | `V2 rules: OK (17 library modules, 8 geometry modules, 15 tested)` | 0 |
| 5 | `build-v2/copper-core.exe dfm examples/generated/divider/design.kicad_pcb fab_profiles/jlcpcb_2layer.json` | `verdict FAIL`: `clearance` 2 violations (`pth_to_track_min_mm`, 280000 nm, `measured_nm` −1); `hole_to_edge` UNVERIFIED (12 holes, no published limit); the other 8 rules PASS. This is G-001 and G-006. | 2 |
| 6 | `build-v2/copper-core.exe dfm nonexistent.kicad_pcb fab_profiles/jlcpcb_2layer.json` | unreadable board | 1 |
| 7 | `build-v2/copper-core.exe report examples/generated/divider/design.kicad_pcb fab_profiles/jlcpcb_2layer.json` | FAIL report. The exit code does not tell FAIL from INVALID_INPUT (G-041). | 2 |
| 8 | `build-v2/pcba.exe help`, `build-v2/copper-core.exe`, `build-v2/pcba.exe blocks`, `ctest -N` | command and test inventory ([IMPLEMENTATION_CONTRACT.md](IMPLEMENTATION_CONTRACT.md) §4) | 0 / 64 (usage) / 0 / 0 |
| 10 | `git status`, then `cmake -S . -B D:\copper-build-tmp\p01-worktree -G Ninja -DCMAKE_BUILD_TYPE=Debug` and `cmake --build D:\copper-build-tmp\p01-worktree` | `CMakeLists.txt` was modified in the working tree during the session (11:15:38), not by P00: `rest_server.c`, `placement.c` and their tests were removed. Configure exits 0; **the build exits 1** with undefined `rest_serve` and `pl_*` in `copper-core`. This is G-042. Runs 1–3 measured HEAD (the `build-v2` tree was built before the edit). | 0 / **1** |
| 9 | `grep` checks: unregistered C tests; absolute paths in fixtures; `--host` in `serve-http`; the DFM profile used by `tools/design.py` | G-021, G-031, G-029, G-002 | — |

**Not re-run in P00, with the reason:**
- The UBSan tree (`build-v2san`). No code changed in P00; it was last clean on 2026-11-07.
- The prompt-acceptance PowerShell suite. It is covered by `prompt_acceptance_harness` in run 2.
- The placement benchmark. No code changed.

### Files changed

| File | Change |
|---|---|
| `docs/engineering/IMPLEMENTATION_CONTRACT.md` | **New.** Working rules for P01–P52, the status vocabularies, the release scope, the inventory (commands, tests, options, environment, format limits) and the capability matrix. |
| `docs/engineering/GAP_REGISTER.md` | **New.** F1–F8 (BLOCKED_EXTERNAL: texts missing); 9 confirmed defects including documentation corrections (G-042, G-001, G-006, G-021, G-023, G-029, G-031, G-040, G-041); missing in-scope features; unverified integrations; DEFERRED out-of-scope capabilities; proposed extensions; 9 historical M0 findings verified by passing regressions. |
| `docs/engineering/PROMPT_RESULTS.md` | **New.** This log. |
| `docs/engineering/V2_STATUS.md` | A dated reconciliation note: demo-board exports were made by KiCad 10.0.5; the skip count depends on environment variables; G-001 and G-002 apply. The 2026-11-07 text is kept. |
| `docs/engineering/RELEASE_READINESS.md` | A dated reconciliation note: the 2026-10-23 review is historical; the current counts; which issues are still open. |
| `docs/engineering/ARCHITECTURE_PLAN.md` (untracked) | The skip count in the scorecard corrected. |

Nothing was committed, pushed or tagged.

### Already fixed against still reproducible

- **Already fixed and verified at `7199ef6`:** the October 2026 findings H-01 to H-09 (Unicode, the JSON escapes, the DC false negative, `NO_JSON/70`, observational PASS, DRC warning classification, stderr handling, empty boards, the duplicate placed-project code). Their regression tests passed in run 2.
- **Still reproducible:**
  - G-042: the working tree does not build (an uncommitted edit made by someone else);
  - G-001: the divider FAILs JLCPCB;
  - G-041: exit codes collapse verdicts;
  - G-029: unauthenticated REST can bind any host;
  - G-006: no measured distance in clearance violations;
  - G-021, G-023, G-031: housekeeping;
  - G-040: documentation over-statement, corrected in the text but awaiting review.
- **Blocked here:**
  - G-010: KiCad 9 native;
  - G-014: GPU;
  - G-015: live model;
  - G-020: CI, MSVC, Linux and ASan;
  - G-009, G-019, G-028, G-005.

### Remaining limits and next dependencies

0. **G-042 first.** The uncommitted `CMakeLists.txt` edit breaks the build of the working tree. The owner needs to say whether it is intended; P00 did not revert it. Until it is resolved, any rebuild of `build-v2` will fail to link.
1. **The owner needs to supply the F1–F8 texts** and the P02–P52 prompt texts. Until then no gap has a target prompt.
2. **The S1 items within scope** are G-001, G-002, G-018 and G-010. Of these, G-001 and G-002 can be worked on here. G-010 needs KiCad 9.x, and G-018 needs rated part data.
3. **P00 stops here** for review.

---

## P01: F1, strict C17 POSIX portability and bounded GBNF formatting (2026-11-08)

**Scope:** owner finding F1.
- The private `dup(Arena*,…)` collides with POSIX `dup(int)`.
- The `DIR` macro collides with `<dirent.h>`.
- `setenv`, `readlink` and `lstat` are undeclared under strict C17/GCC.
- There is an unused `name_length` in `filesystem.c`, and possible `snprintf` truncation in `schema_gbnf.c`.

**Result:** F1 is **IMPLEMENTED_UNVERIFIED**. It is fixed and verified with both Windows GCC toolchains available here. The Linux/glibc build is **BLOCKED_EXTERNAL**: no POSIX toolchain is installed.

### Revision, branch and environment

| Item | Value |
|---|---|
| Base | `v2/complete` @ `7199ef6` |
| Work | separate worktree `D:\copper-wt\f1 posix` (a path with a space), branch `p01/f1-posix-portability`, commit **`7a29caa`**. Not pushed. |
| Main checkout | untouched. The other party's uncommitted `CMakeLists.txt` edit (G-042) is still there and still unreverted. |
| Compilers run | MSYS2 MinGW64 GCC 12.1.0 (msvcrt) and MSYS2 UCRT64 GCC 12.1.0 (ucrt), both C17 with `CMAKE_C_EXTENSIONS OFF` |
| Not available | **Linux/glibc GCC or Clang, any POSIX target, MSVC.** MSYS2's POSIX `gcc` package was not installed: a `pacman -S --print gcc` dry run showed it would upgrade `msys2-runtime` from 3.3.5 to 3.6.10, which changes the owner's toolchain. |
| Other tools | CMake 4.4.4, Ninja 1.13.2, Python 3.12.0; `KICAD_CLI` = kicad-cli 10.0.5 (a development preview); `KICAD_DEMOS_DIR` = the KiCad 9.0-saved demo boards |

### Reproductions at `7199ef6` (before any change)

| Finding | How | Result |
|---|---|---|
| `dup` collision | HEAD `kicad_cli.c` with `-include unistd.h` (the real MinGW `<io.h>`), `gcc -std=c17 -fsyntax-only` | `kicad_cli.c:78: error: conflicting types for 'dup'` (previous declaration `io.h:332 int(int)`): **reproduced** |
| `DIR` collision | HEAD `gerber_test.c` lines 1–20, then `#include <dirent.h>` (the POSIX branch order), real MinGW header | `error: expected identifier or '(' before string constant` inside `dirent.h`: **reproduced** |
| `setenv` / `readlink` / `lstat` | MinGW has neither glibc's feature macros nor these functions, and `-U_WIN32` is refused by MinGW's headers (`#error Only Win32 target is supported`) | **Not reproducible here.** Found by audit: `apps/pcba/cli_os.c` (`setenv`, `readlink`), `tests/gerber_test.c` and `tests/tool_server_test.c` (`lstat`) are the only compiled files that call feature-guarded POSIX functions without `_POSIX_C_SOURCE` |
| `name_length` | code reading: set before the loop, read only under `#ifdef _WIN32` | POSIX-only `-Wunused-but-set-variable`. **No runtime effect.** Not observable with a Windows compiler. |
| `schema_gbnf.c:97` truncation warning | `-O2 -Wformat-truncation=2` | UCRT64 GCC warns (8233 bytes into 8192); MinGW64 does not. HEAD's UCRT64 build also warns with the default flags. |
| (a) array rule over 8 KiB | `gbnf_repro.c` (scratch) against HEAD: `llm_schema_to_gbnf` on an array of one 4070-character enum | status `LLM_OK`, but the rule does not end in `ws "]"`: **runtime defect reproduced** (silently broken grammar) |
| (b) `rule_name` overrun | HEAD's `rule_name` included verbatim; a 16-byte buffer followed by a canary; a parent that fills the buffer | canary clobbered: **one-byte out-of-bounds write reproduced**. It is reachable through the public API with a property key over 249 bytes. |

### Changes (commit `7a29caa`, 16 files)

| File | Change |
|---|---|
| `src/v2_src/kicad_cli.c`, `finding.c`, `tool_server.c` | private `dup` renamed `kcli_dup` / `finding_dup` / `ts_dup`; no other change |
| `tests/gerber_test.c` | `DIR` renamed `GERBER_TEST_DIR`; `_POSIX_C_SOURCE 200809L` before the first include, non-Windows only |
| `tests/tool_server_test.c`, `apps/pcba/cli_os.c` | `_POSIX_C_SOURCE 200809L` before the first include, non-Windows only. No `_GNU_SOURCE` or `_DEFAULT_SOURCE`, and no diagnostic disabled. |
| `src/v2_src/filesystem.c` (**protected**, noted in the commit) | `name_length` computed inside the `_WIN32` block that reads it |
| `src/v2_src/schema_gbnf.c` | `rule_name` refuses a name that does not fit (`LLM_INVALID_ARGUMENT`, "…too deep for a grammar rule name (255 bytes)"); the array and root rules are written through the growing output buffer, so they have no fixed size |
| `tests/llm_test.c` | `test_gbnf_bounds`: the exact 8196-byte array rule in full; key lengths 248 (accepted: the exact boundary), 249 and 250 (refused) |
| `apps/copper_core.c` | 3 misleading-indentation one-liners split (`-Wmisleading-indentation`); output unchanged |
| `tests/mna_ac_test.c`, `nonlinear_models_test.c`, `device_model_test.c`, `provider_test.c` | partial initializers completed with the zero values C17 6.7.9p21 already gave (`-Wmissing-field-initializers`) |
| `tests/fake_kicad_cli.c` | `flood` writes each 1024-byte line with one `fwrite` instead of one character at a time (see UCRT64 below); output byte-identical (`cmp` on both runtimes) |
| `tests/kicad_cli_test.c` | the `at()` path helper stops (exit 2) instead of silently cutting a path (`-Wformat-truncation`, UCRT64) |

### Verification

| # | Command (all clean: new build directory, no reused cache or binary) | Result | Exit |
|---|---|---|---|
| 1 | HEAD, MinGW64: `cmake -S <worktree@7199ef6> -B f1-head-mingw64-rel -G Ninja -DCMAKE_BUILD_TYPE=Release` and build | 0 warnings with the project's own flags (the two F1 warnings do not appear with this compiler) | 0 |
| 2 | Fixed, MinGW64: `-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic"` (strict flags on **every** translation unit, tests and apps included) | first pass: 39 warnings outside F1's files (listed under Changes), all then fixed | 0 |
| 3 | Re-run of the reproductions against the fixed sources | `dup`: 0 errors in all 3 modules with `<unistd.h>` forced in; `DIR`: 0 errors; `-Wformat-truncation=2` on `schema_gbnf.c` and `filesystem.c`: 0 warnings with both compilers | 0 |
| 4 | `llm_test` object linked with HEAD's `schema_gbnf.o` and run, against the fixed converter | HEAD: 4 checks fail (lines 146, 147, 166 ×2); fixed: "all checks passed". The regression is meaningful. | 1 / 0 |
| 5 | Fixed, UCRT64, clean strict build and `ctest -j6` (first final pass) | build: 1 warning (`kicad_cli_test.c:115`); `kicad_cli` FAILED, deterministically, in about 22 s | 8 |
| 6 | The same `kicad_cli_test` built from **HEAD** with UCRT64 (detached worktree) | the same 2 checks fail: **pre-existing at `7199ef6`**, not caused by F1 | 1 |
| 7 | `FAKE_KICAD_MODE=flood fake_kicad_cli` timed | UCRT64 20 568 ms against MinGW64 872 ms. Character-at-a-time writes to unbuffered stderr exceed the runner's timeout, so the runner correctly times out. After the fix: 415 ms and 247 ms, output byte-identical. | 2 (by design) |
| 8 | **Final, MinGW64:** clean strict Release build (`f1-final2-mingw64-rel`); `KICAD_CLI` and `KICAD_DEMOS_DIR` set; `ctest -j6` | **0 warnings**. 75/76 passed; `pcba_cli` failed because P01's script put `D:\msys64\mingw64\bin` (Python 3.9) first on `PATH` for ctest (`TypeError` in `hwc_kicad.py`, the known G-023 trap). `ctest -R ^pcba_cli$` re-run from **the same clean build** with Python 3.12 first: **passed** (83 s). In total: **76/76, 7 skipped**. | 8, then 0 |
| 9 | **Final, UCRT64:** clean strict Release build (`f1-final2-ucrt64-rel`), the same variables, `ctest -j6` | **0 warnings; 76/76 passed, 7 skipped**, 207 s | 0 |
| 10 | `python build-tools/v2_rules_check.py <worktree>` | `V2 rules: OK (17 library modules, 8 geometry modules, 15 tested)` | 0 |

The 7 skips in runs 8 and 9 are exit-77 skips that need hardware or external tools: `llm_ollama_live`, `dfm_gpu_compare`, `v2_native`, `parts_native`, `layout_native`, `design_acceptance` and `slice1_divider`.

The fake-runner tests (`kicad_cli`, `tool_server`, `tool_server_stdio`) and the real-runner and CAM tests against kicad-cli 10.0.5 (`kicad_cli_real`, `gerber_kicad_demos`, `board_kicad_demos`, `dfm_kicad_demos`) ran and passed in both final runs. The real-runner tests are development checks, not KiCad 9 evidence.

**Not run:** UBSan for these changes. UBSan can't see a one-byte overrun through a pointer parameter, and ASan isn't available on MinGW, so the `rule_name` fix is covered by the exact-boundary regression instead.

### New gaps found

- **G-043** (OPEN, S3): GBNF rule names collide. Sibling keys `a_b` and `a.b` produce two `root-a-b` definitions with status OK (reproduced with `gbnf_collision.c`). It is outside F1's scope and was not fixed.
- **Fixed here, but pre-existing:** the UCRT64 `kicad_cli` flood timeout and the `kicad_cli_test.c:115` silent path truncation (runs 5–7). Neither showed with MinGW64 (msvcrt), which is the toolchain every earlier run used.

### Remaining limits and next dependencies

1. **Linux/glibc evidence for F1** (BLOCKED_EXTERNAL): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic" && cmake --build build && ctest --test-dir build` on branch `p01/f1-posix-portability`, with GCC and ideally Clang. F1 moves to VERIFIED only on that result.
2. **The branch is not merged into `v2/complete` and not pushed**, pending owner review and the G-042 decision.
3. **Next:** P02 (F5), the `kicad_cli_test` scratch directory that collides with the test executable on Linux.

---

## Port of `feature/update` into `src/v2_src` (2026-11-18, owner request; not a numbered prompt)

**Request:** "pull this branch into this project". The owner chose to port it into `src/v2_src`.

**What `feature/update` is.**
- It is a single **orphan** commit, `71c22bb` (Abheesht04, 2026-11-17), with no shared history.
- It holds a standalone `v2_src` tree (87 C files) built from an earlier export of this repository's `src/v2_src`.
- It also holds 214 committed `build/` files, about 550 `validation/` run files, zip archives and packaging metadata.
- A `git pull` or merge was not possible without replacing the project.
- A local branch `feature/update` now tracks `origin/feature/update`; nothing was merged into it.

**Result:** branch `port/feature-update` @ **`e3b3d8b`**, based on `p01/f1-posix-portability` @ `7a29caa`, in worktree `D:\copper-wt\port update`. 116 files. Not pushed.

| Item | Decision |
|---|---|
| New overlay modules (topology, part catalogue, KiCad assets, design resolver, design parity, KiCad adapters, DFM check, placer, router, SPICE, engineering and optimization loops) | added to `src/v2_src` as the new `copper_design` library, plus the `copper` CLI (`apps/copper_cli.c`) |
| Overlay `board.{c,h}` and `mna.{c,h}` | renamed on import to `design_board` and `spice_mna` (file, include lines and guard): they are different modules from track B's `board` and hwcore's `mna` |
| Tests, fixtures, schemas, `jlcpcb_rules.rules`, local-PC scripts | kept in the overlay's layout under `src/v2_src` (its tests resolve paths relative to it); 6 tests with assertions forced on (`-UNDEBUG`) |
| Protected files | `IR.c` (both copies: `spaces[]`), `excellon.h` (include guard), `g2_types.h` (declarations of functions that already exist; a duplicate declaration removed), `filesystem.{c,h}` (the overlay's superset: 15 new functions; uses `malloc`, G-045) |
| Other shared files | `nmath`, `u128.h` (both copies) and `copper_project` taken from the overlay. Our `copper_project.h` include guard restored, because the overlay's clashed with `src/v2/project.h`. Behaviour change: KiCad's install path is no longer guessed from Program Files. |
| Kept ours | `board.{c,h}`, `excellon.c` (the overlay lacks `d31896d`), `kicad_library_table.c`, `g2_types.c`, the base modules |
| Windows fixes in imported code | `_dupenv_s` only on MSVC or UCRT runtimes; POSIX-only helpers compiled only on POSIX; the MSVC `#pragma comment` guarded. No diagnostic was disabled. |
| Not taken | `build/`, `validation/`, the zips, `manifest.json` |

**Findings while porting.**
- **The overlay as shipped does not build on Windows MinGW with its own `-Werror`.** The errors were `#pragma comment`, unused POSIX helpers and `_dupenv_s` missing from msvcrt. Its scan report recorded only Linux GCC/Clang builds and "MSVC not available".
- **Its original `v2_src/CMakeLists.txt` let `test_optimization_loop.c` compile with no checks in Release** (assertions removed by NDEBUG). Its corrected root `CMakeLists.txt`, and this port, undefine NDEBUG for the tests. The objects were checked and do call `_assert`.
- **Its DFM is narrower than ours** (G-046): on the generated divider it reports UNVERIFIED (8/0/25), while `dfm.c` reports the two G-001 FAILs.
- **The two JLCPCB data files agree on every limit they share.**

**Verification (clean configure, new build directories, `-DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic"`, the overlay targets with `-Werror`; `KICAD_CLI` 10.0.5 and `KICAD_DEMOS_DIR` set; Python 3.12 first on `PATH`):**

| Run | Build | ctest |
|---|---|---|
| MinGW64 GCC 12.1 Release (`port-final-mingw64-rel`) | exit 0, **0 warnings** | **82/82 passed** (76 existing and 6 ported), 7 skipped (KiCad 9 native, GPU, live Ollama), 200 s, exit 0 |
| UCRT64 GCC 12.1 Release (`port-final-ucrt64-rel`) | exit 0, **0 warnings** | **82/82 passed**, 7 skipped, 210 s, exit 0 |
| `copper --help` | — | exit 0 |
| `copper user dfm-check <divider> --rules jlcpcb_rules.rules` | — | exit 3, UNVERIFIED (G-046) |

**Not verified:** Linux (no POSIX toolchain here; this is the same block as F1), MSVC, ASan, and KiCad 9.

**New gaps:** G-044 (two parallel engines, an owner choice), G-045 (`malloc`), G-046 (narrower DFM), G-047 (CLI naming).

**Next dependencies:**
- the owner reviews both branches (`p01/f1-posix-portability`, `port/feature-update`) before any merge into `v2/complete`;
- G-042 (the uncommitted `CMakeLists.txt` edit in the main checkout) is still unresolved;
- the G-044 unification order is to be decided.

## Lab branch: pipeline hard rules, baseline and Steps A, B, 0a, 0b (2026-11-21, owner request)

**Branch** `lab/copper-lab-integrated` (worktree `D:\copper-wt\copper lab`).

| Step | What | Commit |
|---|---|---|
| A | `PIPELINE_HARD_RULES.md` (H1–H13, testing loop T1–T8) adopted by the owner; pointer in `CLAUDE.md`; baseline in `PIPELINE_BASELINE.md` | in `ac99bcf` |
| B | Commit of the earlier session work: C router, ESP32 template, scope check, Manus key file, Ollama removal | `ac99bcf` |
| 0a | Read-only audit: `CONSOLIDATION_PLAN.md`, one engine per job for new work, nothing deleted (H10) | Step 0b commit |
| 0b | UBSan for `copper_design` (`CMakeLists.txt`, under the existing `COPPER_V2_SANITIZE`); `manus/golden_boards.ps1` and `manus/golden/`; record files copied onto the branch | Step 0b commit |

**Environment:** MinGW-w64 GCC 12.1, Release; KiCad **10.0.5** (development check, not KiCad 9 evidence); no GPU.

**Testing loop after Step 0b:**

| Check | Result |
|---|---|
| T1: clean build `-DCOPPER_V2_SANITIZE=ON` | exit 0, 247 targets, **0 warnings**; UBSan (trap) on `copper_v2` and `copper_design`; ASan NOT RUN |
| T2: ctest with `KICAD_CLI` and `KICAD_DEMOS_DIR` set (KiCad 10.0.5) | **84/84 passed**: 78 run, 6 skipped (KiCad 9.x: `v2_native`, `parts_native`, `layout_native`, `design_acceptance`, `slice1_divider`; GPU: `dfm_gpu_compare`), 0 failed |
| T4: `manus/selftest.ps1` (mock, no credits) | **23 passed, 0 failed, 0 blocked** |
| T5: `manus/golden_boards.ps1` | **PASS**: LED, MOSFET and ESP32 identical to the baseline (hashes), ROUTED, 0 unconnected, deterministic |
| T5 check of the checker | with a deliberately altered baseline: CHANGED (hash) and FAIL (`drc_error 12 > 11`), exit 1 |
| T6: lint | no C library code changed in Step 0b |
| T7: records | G-003 and G-044 notes; G-061 to G-066 (`GAP_REGISTER.md` §8) |

**Not verified:** KiCad 9 native gates (G-010), Linux, MSVC, ASan, GPU.

## Lab branch: Step 1a, `copper catalogue check` (2026-11-21, owner approved)

**Changes:**
- new `src/v2_src/parts_check.{c,h}` (copper_design; on the no-float lint list);
- `copper catalogue check` in `apps/copper_cli.c`, a new command beside `catalogue validate`, which is unchanged;
- the test `src/v2_src/tests/test_parts_check.c` with `tests/fixtures/parts_check/sample_library.sx`. The catalogues
  are Copper-written test data; the KiCad libraries are the template_gen fixtures extracted from KiCad 10.0.5;
- `PIPELINE_HARD_RULES.md` H2: only a person marks data verified;
- `CLI.md` section.

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees (one with UBSan on `copper_v2` and `copper_design`), exit 0, **0 warnings** |
| T2 | **85/85 passed** (79 run, 6 skipped: KiCad 9.x ×5, GPU ×1), 241 s, UBSan build |
| T3 | `copper_parts_check_tests`: 11 test groups pass. Mutation check: with the KiCad check disabled, the test fails (`test_every_kicad_problem_is_reported`) |
| T3 (CLI) | exit codes checked: 3 UNVERIFIED (with and without roots), 2 INVALID_INPUT (other format, missing file), 64 (half the roots or an unknown option), 1 FAIL (`catalogue_resolution/parts.sx`: 9 FAIL and 5 UNVERIFIED findings in one run) |
| T4 | self-test **23/23** |
| T5 | golden boards **PASS**, identical to the baseline |
| T6 | `parts_check.c`: no `malloc`, `printf` or `exit`; no float (CMake lint) |
| T7 | G-018 note, G-067 (two catalogue formats); this entry; CLI.md; H2 |

**Not verified:** KiCad 9 (G-010); Linux, MSVC, ASan.

## Lab branch: Step 1b, first parts library (2026-11-21, owner approved; format `copper_parts` chosen by the owner)

**Added:**
- `parts_library/` (README and 5 files): 19 parts from Vishay, Murata, Kingbright, Alpha and Omega, Advanced
  Monolithic Systems, Espressif, TI, E-Switch, Samtec and GCT;
- `tests/parts_library_check.cmake`, run as ctest `parts_library_parse`.

**How the data was taken:**
- datasheets were downloaded from the manufacturers' sites;
- text was extracted per page with PyMuPDF (a local tool, not part of Copper);
- every table a value was taken from was also rendered to an image and read, so table columns could not be mixed up;
- every source is `unverified`; no value was estimated;
- two corrections came from the documents themselves:
  - Samtec's current catalogue page has no "G" plating code, so the headers are `-T` (matte tin);
  - `SS14-E3/61T` follows the ordering example on page 2 of the Vishay document.

**Results (`copper catalogue check`, KiCad 10.0.5 libraries, a development check):**

| File | Parts | KiCad | Verdict |
|---|---|---|---|
| resistors | 5 | 5 PASS | UNVERIFIED (exit 3) |
| capacitors | 4 | 4 PASS | UNVERIFIED (exit 3) |
| semiconductors | 3 | 3 PASS | UNVERIFIED (exit 3) |
| ics | 3 | 2 PASS, 1 FAIL (ESP32 GND stacked pins in KiCad 10, G-069) | FAIL (exit 1) |
| electromechanical | 4 | 4 PASS | UNVERIFIED (exit 3) |

The Step 1b "done when" criterion of 0 FAIL is **not fully met**: the one FAIL is G-069, a KiCad 10 library
difference, not a data error.

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | **86/86 passed** (80 run, 6 skipped: KiCad 9.x ×5, GPU ×1), UBSan build |
| T3 | `parts_library_parse` passes on all 5 files. With a duplicate-MPN copy of a file it fails, as it should |
| T4 | self-test 23/23 |
| T5 | golden boards PASS, identical to the baseline |
| T6 | no C code changed |
| T7 | G-018 note 2, G-068 (parser rejects negative ratings), G-069 (ESP32 stacked GND pins in KiCad 10) |

**Found:** G-068, a defect in a base module. It is not fixed: it needs the owner's approval.

## Lab branch: G-068 fixed (2026-11-21, owner approved)

**Change:** `src/v2_src/part_catalogue.c` (base module; noted in the commit). The condition is now
`name != temperature && value <= 0`; before, `value < 0` rejected every negative rating, temperatures included.

**Test first:** `test_signed_temperature_only` failed before the fix (`-55000 mC`: RANGE_ERROR) and passes after it.
Negative or zero resistance, voltage and current are still INVALID_INPUT.

**Data:** 19 `temperature minimum` ratings added to `parts_library/`, each with the same source page as the
maximum. The "minimum in the description only" notes were removed.

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | 86/86 passed (6 skipped) |
| T4 | self-test 23/23 |
| T5 | golden boards PASS (identical) |
| `copper catalogue check` | verdicts unchanged (4 files UNVERIFIED, `ics` FAIL only for G-069) |

## Lab branch: Step 2a, block format and checker (2026-11-21, owner approved; JSON format)

**Added:**
- `src/v2_src/blocks.{c,h}` (copper_design; on the no-float lint list): loads and checks `copper.block/1`;
- `copper block check` in `apps/copper_cli.c`;
- `docs/engineering/BLOCK_FORMAT.md` and a CLI.md section;
- the test `src/v2_src/tests/test_blocks.c` with `tests/fixtures/blocks/led_indicator.block.json` (Copper-written),
  checked against the real `parts_library` catalogues.

**Espressif research (the owner asked for recommended designs):**
- **Today's template module is NRND.** The ESP32-WROOM-32 datasheet carries the "NOT RECOMMENDED FOR NEW DESIGNS"
  watermark.
- **Recommended modules.** The WROOM-32E (v2.1), S3-WROOM-1 (v1.8), C3-WROOM-02 (v1.7) and S3-MINI-1 (v1.7)
  datasheets do not.
- **Design guide.** Espressif's *ESP32 Hardware Design Guidelines* (release master, Nov 12, 2026) was downloaded as
  the source for the `esp32_core` block (2b).
- **Recorded as:** G-070.

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | **87/87 passed** (81 run, 6 skipped: KiCad 9.x ×5, GPU ×1), UBSan build |
| T3 | `copper_blocks_tests`: valid fixture VALID with catalogues and UNVERIFIED without; 29 single-change cases each give their finding. Mutation check: with the terminal check disabled, the test fails (the first mutant attempt did not compile and was redone) |
| T3 (CLI) | exit 0 VALID, 3 UNVERIFIED, 2 INVALID (misspelt terminal: 2 TERMINAL_MISMATCH), 64 usage |
| T4 | self-test 23/23 |
| T5 | golden boards PASS, identical to the baseline |
| T6 | `blocks.c`: no `malloc`, `printf` or `exit`; no float (CMake lint) |
| T7 | G-066 note, G-070; this entry |

## Lab branch: Step 2b, the first blocks (2026-11-21, owner approved; ESP32-S3-WROOM-1)

**Sources retrieved (all from the publishers):**
- ESP32-S3-WROOM-1 datasheet v1.8;
- ESP32-S3 Hardware Design Guidelines (release master, Nov 12, 2026);
- USB Type-C specification R2.0 (Aug 2019);
- TI SLVA689A (I2C pull-up calculation; NXP's site returned "page not available" for UM10204);
- Vishay 293D (KEMET's datasheet server presented an expired TLS certificate, so it was not used);
- Murata GRM188R71H103KA01.

Key tables were read as rendered page images.

**Corrections made while checking:**
- The USB-C VBUS range is from §4.8.1.2 (chargers with captive cables), not a general rule, and the block says so.
- The module datasheet's strapping section is "4 Boot Configurations".
- The AMS1117 datasheet asks for a **solid tantalum** 22 uF output capacitor, so a tantalum part was added rather than
  using a ceramic one.
- The AMS1117 datasheet gives no input capacitor value, so the 10 uF at the entrance comes from Espressif's guide.

**Added:**
- `blocks_library/` (7 blocks and a README);
- 5 parts in `parts_library/` (all unverified; KiCad PASS against the 10.0.5 libraries);
- `tests/blocks_library_check.cmake`, run as ctest `blocks_library_valid`.

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | **88/88 passed** (82 run, 6 skipped: KiCad 9.x ×5, GPU ×1), UBSan build |
| T3 | 7/7 blocks VALID against the 5 parts files. With a block copy carrying an unknown key, `blocks_library_check.cmake` fails (UNKNOWN_KEY) |
| T4 | self-test 23/23 |
| T5 | golden boards PASS, identical to the baseline |
| T6 | no C code changed |
| T7 | G-066 note 2, G-071 (no ESD part), G-072 (AMS1117 headroom on USB 5 V) |

## Lab branch: Step 2c, the composer (2026-11-21, owner approved; no low-dropout regulator added)

**Added:**
- `src/v2_src/compose.{c,h}` (copper_design; on the no-float lint list);
- `copper design compose` in `apps/copper_cli.c`;
- `src/v2_src/tests/test_compose.c` with the fixture `tests/fixtures/compose/esp32s3_led_sensor.compose.json` and
  `tests/fixtures/compose/kicad/`: 7 symbol libraries' needed symbols and 12 footprints, copied unchanged from KiCad
  10.0.5.

**Block format additions** (blocks.c and its test; 7 new single-change cases): `from` on parameters,
`current`/`current_max`/`current_source`, `current_follows`, `level_port`, `pullup`. All 7 blocks were updated, with a
source for every new value:
- ESP32-S3 needs 0.5 A (datasheet table 6-2);
- AMS1117 quiescent current 11 mA, and an output limit of 0.8 A because dropout is specified only up to 0.8 A;
- USB default 500 mA (USB-C R2.0, page 34);
- TMP102 10 uA typical (no maximum given);
- GPIO0 pull-up 360 uA worst case (Ohm's law).

**Example result:**
- 21 components; the grounding gate says GROUNDED;
- LED resistor: at least 650 ohm, so 680 ohm (CRCW0603680RFKEA);
- I2C pull-ups: 966.7 ohm to 3540.7 ohm at Cb 100 pF, so 1 kohm;
- LDO load: 500.37 mA; LDO dissipation: 850.6 mW (UNVERIFIED: no thermal check);
- +5V source capacity: unknown (UNVERIFIED).

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | **89/89 passed** (83 run, 6 skipped: KiCad 9.x ×5, GPU ×1), UBSan build |
| T3 | `copper_compose_tests`: COMPOSED and GROUNDED, deterministic; USB-C → AMS1117 REFUSED (VOLTAGE_RANGE and CURRENT_BUDGET); 17 single-change cases (NEEDS_INPUT for a missing supply voltage or bus capacitance, NO_VALID_PULLUP, UNDERIVABLE, SAME_BLOCK_SHORT, DUPLICATE_CONNECTION, UNCONNECTED_POWER, SINGLE_PORT_NET, UNKNOWN_BLOCK, UNKNOWN_PARAMETER, PARAMETER_RANGE, VOLTAGE_RANGE...); DUPLICATE_PULLUP. Mutation check: with the voltage check disabled, the test fails (the first mutant attempt did not compile and was redone). The CLI on the real KiCad 10 libraries: COMPOSED, then `check-intent` GROUNDED |
| T4 | self-test 23/23 |
| T5 | golden boards PASS, identical to the baseline |
| T6 | `compose.c` and `blocks.c`: no `malloc`, `printf` or `exit`; no float |
| T7 | G-066 note 3, G-072 update, G-073, G-074 |

**Not committed** (the owner said not to).

## Lab branch: Step 2d, the Manus side of composition (2026-11-21, owner approved)

**C:**
- `compose.c`: evidence. With a prompt, every stated value (block parameters, board size) needs a quote in the
  prompt that contains it (the template_gen.c rule; its static helpers are mirrored, and that file is unchanged).
  Unbacked values are IGNORED_VALUE, then a default or a question. A missing board size is now a question
  (NEEDS_INPUT), never invented. The evidence state lives in the call's context, not in globals.
- `copper design compose --prompt FILE`.
- `copper block list FILE...` (copper.block-list/1).

**Manus workspace:**
- `copper.ps1 blocks` and `copper.ps1 compose`, which runs the gate after COMPOSED;
- config keys `blocks_dir` and `parts_dir` (setup script, and the owner's `D:\copper-manus`, which doctor reports
  READY);
- `copper-prompt.ps1`: the block list goes into Manus's instructions; for `circuit: "compose"` it writes
  `design.compose.json` (a plain mapping) and runs compose. Composed designs are reported as "not yet placed, compiled
  or routed";
- `manus_requirements.schema.json`: `compose` and `composition`;
- MANUS.md: Manus may choose blocks and port connections only;
- `manus/tests/compose_requirements.json` (a mock analysis with one value the prompt never states).

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | 89/89 passed (6 skipped: KiCad 9.x ×5, GPU ×1), UBSan build |
| T3 | `test_evidence`: backed values compose; a wrong value, a quote not in the prompt, or a missing board quote each give IGNORED_VALUE and NEEDS_INPUT; no board gives NEEDS_INPUT; without a prompt, 12 V composes with 4.35 W dissipation reported UNVERIFIED |
| T4 | **self-test 27/27**, 4 new steps: `blocks` (7); unstated values give NEEDS_INPUT and nothing is written; USB-C → AMS1117 REFUSED; prompt → mock Manus composition → COMPOSED, GROUNDED, the unstated LED current ignored, the real key never sent |
| T5 | golden boards PASS, identical to the baseline |
| T6 | `compose.c`: no `malloc`, `printf` or `exit` |

**Not run:** a live Manus run of a composition (it spends credits; run it only when the owner asks).
**Not committed** (the owner's instruction).

## Lab branch: Step 2e, placing and routing composed designs (2026-11-21, owner approved; live test first)

**New (beside the base, H10):**
- `src/v2_src/place_intent.{c,h}`:
  - `place_problem` turns a composed intent and `layout_hints.json` into a `placement.h` problem. Bodies come from the
    KiCad courtyards, with 4 orientations per part. It adds block keep-outs, board edge strips and decoupling pairs.
  - `place_apply` takes the lowest-cost legal placer result (NO_LEGAL_POSITION if there is none) and writes the
    positions and `routing.json` keep-outs.
- `copper design place-problem` and `copper design place-apply` (exit codes 0 PLACED, 2 INVALID, 5 NO_LEGAL_POSITION).
- `routing_profiles/copper_placement_default.json`, each value with its reason.
- Block parts may carry `keepouts` (checked); `esp32s3_core` has the module's antenna keep-out from the KiCad footprint.
- `copper.ps1 compose` now runs: compose, gate, place-problem, `copper-core place`, place-apply, gate.
  `copper-prompt.ps1` compiles and reports a placed composition.
- `manus/golden/esp32s3_leds/{prompt.txt,design.compose.json}`. `golden_boards.ps1` accepts compose inputs.

**Fixed:**
- Legacy compiler (fixed, not extended): unnumbered pads are skipped, and rotated parts get absolute pad and text
  angles.
- `board_kicad.c` (base module, G-075 note): pad angles are absolute. Before this, 90°/270° parts were modelled with
  turned pads and the router left vias 0.05 mm from a real pad.

**Live results (KiCad 10.0.5, a development check, not KiCad 9 evidence):**

| Board | Placement | Routing | KiCad DRC | DFM |
|---|---|---|---|---|
| ESP32-S3 + 2 LEDs + reset/boot (70 × 60 mm) | PLACED, GROUNDED | ROUTED, 189 segments, 13 vias | 0 unconnected; errors 12, all drill_out_of_range from the module footprint's thermal vias (the same as template golden `esp32`); 0 clearance | UNVERIFIED (hole_to_edge) |
| ESP32-S3 + LED + TMP102 | PLACED, GROUNDED | PARTIALLY_ROUTED, 7/11 nets | not reported as a pass | — (G-076, fine pitch) |

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | 84 passed, 6 skipped (KiCad 9.x ×5, GPU ×1), 0 failed; UBSan build; includes `copper_place_intent_tests` |
| T3 | `test_place_intent`; `test_part_keepouts`; `test_pad_angle_is_absolute` failed before the `board_kicad.c` fix and passes after |
| T4 | self-test 27/27 |
| T5 | golden boards 4/4 PASS; the three template boards are byte-identical to the baseline; the new `esp32s3_leds` (blocks path) is deterministic over two runs and recorded |
| T6 | `place_intent.c` is on the no-float lint list: no `malloc`, `printf` or `exit` |

**Open:** G-076 (fine pitch, the owner's choice), G-077 (dangling USB resistors), and comparing the blocks and template paths.
**Not committed** (the owner's instruction).

## Lab branch: Step 3a, fab profiles and the global profile (2026-11-25, owner approved)

**Decision (owner):** the target fab is a parameter, and the default is JLCPCB. "Global" (the strictest of all
profiles) is reported next to it, not used as the gate. Adding a fab is adding a data file. Rules:
docs/engineering/LAYOUT_RULES.md.

**Data (Copper-written from the fabs' pages; not KiCad exports):**
- `fab_profiles/pcbway_2layer.json` (pcbway.com/capabilities.html, standard table only) and
  `fab_profiles/oshpark_2layer.json` (docs.oshpark.com/services/two-layer). Both read 2026-11-25. Every limit is
  present, quoted, and null where the page gives none.
- A `mask_silk_limits` section in all three profiles: solder mask web, silkscreen line width, text height, pad to
  silk. JLCPCB's values are from its capabilities page, read 2026-11-25.

**New C (beside the base; `dfm.c` unchanged):**
- `src/v2_src/fab_global.{c,h}` (in copper_v2; on the no-float lint list) reads `mask_silk_limits` as strictly as
  `dfm.c` reads `limits`, and builds the strictest profile:
  - a minimum takes the largest value; a maximum board size takes the smallest;
  - ties go to the earlier profile;
  - each limit names its deciding fab and the fabs that publish no value.
- `copper-core fab-global OUT PROFILE...` writes the global `copper.fab_profile/1`, which `copper-core dfm` accepts.

**Global values:**
- track and spacing 0.1524 mm (OSH Park);
- drill and via hole 0.254 mm (OSH Park);
- copper to edge 0.381 mm (OSH Park);
- PTH ring 0.18 mm (JLCPCB);
- via ring 0.15 mm (PCBWay);
- solder mask web 0.1016 mm (OSH Park);
- silkscreen 0.15 mm line and 1.0 mm text (JLCPCB);
- hole to edge: null (no fab publishes it).

**DFM of golden board `esp32s3_leds`:**

| Profile | Verdict |
|---|---|
| JLCPCB | UNVERIFIED (hole_to_edge) |
| PCBWay | UNVERIFIED (6 unpublished rules) |
| OSH Park | FAIL (drill ×12, the module's 0.2 mm vias; G-078) |
| global | FAIL (drill ×12) |

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | 85 passed, 6 skipped (KiCad 9.x ×5, GPU ×1), 0 failed (new: `fab_global`) |
| T3 | `fab_global_test`: real profiles give the section 2 values; direction, ties, missing fabs and none-published; the output re-parses through `dfm_profile_parse` with equal values; deterministic; strict `mask_silk_limits` parsing (misspelt key, missing key, no quote, no row, negative value, no date). Mutants: "maximum treated as minimum" and "ties to the later profile" both compile and both are caught. |
| T4 | self-test 27/27 |
| T5 | golden boards 4/4 PASS, byte-identical (adding `mask_silk_limits` to the JLCPCB profile changes no verdict) |
| T6 | `fab_global.c` on the no-float lint list; no `malloc`, `printf` or `exit` in it |

**CLI fix found in testing:** `fab-global` with no profile used to fall through to the project parser
("PROJECT ERROR", exit 2). It now prints a usage message and exits 64.
**Not committed** (the owner's instruction).

## Lab branch: Step 3b, KiCad project rules from the fab profile (2026-11-25, owner approved)

**Problem:** the generated `.kicad_pro` had no rules, so KiCad DRC checked KiCad's defaults (min hole 0.3 mm,
clearance 0.2 mm), not the fab's limits or Copper's design rules (G-080).

**New C (beside the base):**
- `src/v2_src/project_rules.{c,h}` (copper_v2; on the no-float lint list). It was first named `kicad_rules`; that name
  is taken by an existing ctest, so the new module was renamed rather than touching the old test.
- `copper-core project-rules BOARD FAB RULES` writes `.kicad_pro` and `.kicad_dru`:
  - board minimums come from the fab profile;
  - the `Default` net class comes from the routing rules;
  - one fine-pitch rule per footprint whose own pads are closer than the Default clearance, at the fab's pad limit;
  - all other `.kicad_pro` members are kept;
  - a `.kicad_dru` without Copper's header is never overwritten;
  - rules below the fab's limits are refused.
- The report goes to `validation/project_rules.json`.
- `copper.ps1 compile` runs it after routing and before DRC (`project_rules` in COPPER_RESULT).

**KiCad facts, checked by hand with kicad-cli 10.0.5** (key names from the real KiCad 9.0 project
`sample_resistor_lol`):
- the rules apply with the compiler's `meta.version 1`;
- `min_silk_clearance` is silkscreen to silkscreen, so the fab's pad-to-silk limit is not mapped to it;
- `memberOfFootprint` on both items relaxes only the footprint's own pads;
- a broken rule file is ignored silently (G-082; this fails closed).

**Results (JLCPCB target, KiCad 10.0.5 development check):**

| Board | DRC errors before | after |
|---|---|---|
| `esp32` (template) | 12 drill_out_of_range | 0 |
| `esp32s3_leds` (composed) | 12 drill_out_of_range | 0 (0 unconnected) |
| LED + TMP102 (composed, partly routed) | 4 clearance (U3's own pads) + 12 drill | 0 (33 unconnected remain: routing, Step 3c) |

Not carried into KiCad, and reported (G-081): pad hole to hole, solder mask web, pad to silkscreen, hole to edge.

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | 87 passed, 6 skipped (KiCad 9.x ×5, GPU ×1), 0 failed, of 93 (new: `project_rules`, and `project_rules_drc`, which says SKIPPED without KICAD_CLI) |
| T3 | `project_rules_test`: the real KiCad 9.0 project merges with every other member kept; JLCPCB/OSH Park/PCBWay values; SOT-563 at 0° and 90° (gap 0.15 mm); same-net pads ignored; JLCPCB PASS, OSH Park FAIL, PCBWay PASS; an unquotable reference stays UNVERIFIED; odd via ring rounded up; rules below the fab refused; deterministic. Mutants "same-net pads counted" and "default clearance before pad limit" both compile and both are caught. `project_rules_drc` (kicad-cli): JLCPCB 0 violations; OSH Park exactly 4, under `copper_fine_pitch_U3`. |
| T4 | self-test 27/27 |
| T5 | golden boards 4/4 PASS; no file hash changed. The baseline `drc_error` for `esp32` and `esp32s3_leds` was lowered from 12 to 0, which tightens the gate. |
| T6 | `project_rules.c`: no float, `malloc`, `printf` or `exit` |

The owner's `D:\copper-manus` workspace was refreshed and doctor reports READY. **Not committed** (the owner's instruction).

## Lab branch: Step 3c, fine-pitch routing (2026-11-25, owner approved; G-076 option a)

**Problem:** the router inflates every other-net pad by half a track, plus 0.3 mm clearance, plus grid slack, so the
pads of a SOT-563 (0.15 mm apart) had no free grid cell. The TMP102 board routed 7 of 11 nets.

**Changes:**
- **Router (`autoroute.c`/`.h`, base modules; note in G-076):**
  - optional `fine_clearance_nm` and `fine_neck_nm` (0 = off, the old behaviour);
  - a footprint whose own pads are closer than the clearance is fine-pitch;
  - **fan-out first:** before any net is routed, each of its connected pads reserves an escape stub (0.2 mm wide) to
    a free cell; the stub keeps 0.15 mm to its own footprint's SMD pads and 0.3 mm to everything else;
  - later nets avoid the stubs and their end cells; a failed net's stubs are removed (no dangling copper);
  - the "no free grid cell" failure names the pad.
- **Routing profile:** `fine_pitch_clearance_mm` 0.15 and `fine_pitch_neck_mm` 0.2, each with its reason.
  `copper design route` refuses them below the fab's copper limits or minimum track width.
- **Placement (`place_intent.c`):** a fine-pitch part's body grows by `fine_pitch_escape_mm` (1.0). Measured reason:
  the placer had put pull-up R6 1.2 mm from the SOT-563, which walled in pin 2. Paste-only and unnumbered pads are
  ignored, so the module is not fine-pitch.
- **`project_rules.c`:** a second KiCad rule per fine-pitch footprint, `copper_fine_pitch_<REF>_escape`
  (`A.memberOfFootprint(REF) && B.Type == 'Track' && B.intersectsCourtyard(REF)`, at the routing rules' 0.15 mm).
  Checked by hand with kicad-cli 10.0.5: both A/B orders are evaluated; a 0.23 mm control flags all 5 stubs.
- **New golden board** `manus/golden/esp32s3_sensor`: the prompt states every value, including bus capacitance 100 pF.

**Results (JLCPCB, KiCad 10.0.5 development check):**

| Board | Before | After |
|---|---|---|
| ESP32-S3 + LED + TMP102 | PARTIALLY_ROUTED 7/11; 4 clearance + 12 drill DRC errors | **ROUTED 11/11**, 226 segments, 15 vias, **0 unconnected, 0 DRC errors**; ERC 2 (power_pin_not_driven, as on every ESP32 board) |
| the 4 existing golden boards | — | byte-identical |

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | 87 passed, 6 skipped (KiCad 9.x ×5, GPU ×1), 0 failed, of 93 |
| T3 | `test_fine_pitch` (`copper_autoroute_tests`) on a Copper-written board whose SOT-563 pads are copied from KiCad 10.0.5: without the rules, 4 nets FAILED "no free grid cell"; with them, ROUTED; checked independently (stubs 0.2 mm, 0.15 mm to own pads only, 0.3 mm everywhere else); connected; deterministic; the LED board byte-identical with the rules on; bad rules refused. Mutants "never fine-pitch" and "no neck-down" both compile and are caught. `place_intent`: U3 body ±2.2 × ±1.95 mm, R6 unchanged, the module not fine-pitch, both-or-neither keys. `project_rules`: escape rule written; refused for OSH Park with the default rules. `project_rules_drc`: JLCPCB 0, OSH Park + default rules refused, OSH Park without fine keys 4. |
| T4 | self-test 27/27 |
| T5 | golden boards 5/5 PASS; `esp32s3_sensor` added (deterministic over two runs) |
| T6 | `autoroute.c`, `place_intent.c`, `project_rules.c` on the no-float lint list; no `malloc`, `printf` or `exit` |

**Open:** G-083 (straight stubs only), G-084 (the default routing profile is refused for OSH Park, by design).
**Not committed** (the owner's instruction).

## Lab branch: Step 3d, schematic rules (2026-11-25, owner approved)

**Changes:**
- **Block format:** `external_supply: true` (power_out or ground ports only, checked by `blocks.c`), set on the
  header's `vout`/`gnd` and USB-C's `vbus`/`gnd`. The composer lists those nets under `power_flags` in
  `layout_hints.json`.
- **G-077:** the composer reports UNVERIFIED `UNCONNECTED_PORT_PARTS` when an unused port leaves a two-terminal part
  with a floating pin (the two 22 Ω USB resistors).
- **New C `src/v2_src/sch_rules.{c,h}` and `copper design schematic-rules`:**
  - one `power:PWR_FLAG` per external-supply net, copied verbatim from KiCad's power library, in KiCad 9's instance
    form (from the real KiCad 9 sample), with a label at its pin; idempotent;
  - `sym-lib-table`/`fp-lib-table` in KiCad 9's standard form; never overwrites a table Copper did not write.
  - `copper.ps1 compile` runs it after the compiler and then runs ERC again (the compiler's `erc.json` predated it).
- **Legacy compiler (fixed, not extended), found once KiCad could compare against its libraries:**
  - no invented `pin_numbers`/`pin_names` on symbols (`lib_symbol_mismatch` U1/U2/U3);
  - library footprint properties (Datasheet, Description) and the labels' `unlocked` are kept.

**KiCad findings (10.0.5, development check), errors/warnings:**

| Golden board | ERC before → after | DRC before → after |
|---|---|---|
| led | 0/6 → **0/0** | 0/3 → **0/0** |
| mosfet | 0/12 → **0/0** | 0/6 → **0/0** |
| esp32 (template) | 2/26 → 2/0 (power flags: G-086) | 0/13 → 0/1 (G-085) |
| esp32s3_leds | 2/38 → **0/0** | 0/38 → 0/20 (silkscreen, Step 3e) |
| esp32s3_sensor | 2/42 → **0/0** | 0/53 → 0/33 (silkscreen 32, G-085 1) |

**Testing loop:**

| Check | Result |
|---|---|
| T1 | 3 build trees, 0 warnings |
| T2 | 88 passed, 6 skipped (KiCad 9.x ×5, GPU ×1), 0 failed, of 94 (new: `copper_sch_rules_tests`) |
| T3 | `test_sch_rules` (a Copper-written schematic, the real KiCad 9.0 sample, PWR_FLAG copied verbatim from KiCad 10.0.5): flags placed, labelled, unique deterministic UUIDs, idempotent, refusals; exact library tables; foreign tables protected. `test_blocks` and `test_compose` cover `external_supply`, `power_flags` and `UNCONNECTED_PORT_PARTS`. Mutants "no idempotence guard" and "any library name" both compile and are caught. |
| T4 | self-test 28/28. The claimcheck step was stale: it expected "ERC is clean"/"DRC passed" to be flagged on the LED board, which is now truly clean. It now follows the facts both ways (true claims must not be flagged), and a new step flags both claims on a facts variant with findings. |
| T5 | golden boards: all 5 ROUTED and deterministic; every ERC/DRC count is equal or lower. The two compiler fixes and the composed boards' power flags changed all 5 pcb and sch hashes; **the owner approved them (H9) and the baseline records them: 5/5 PASS**. |
| T6 | `sch_rules.c` on the no-float lint list; no `malloc`, `printf` or `exit` |

**Not committed** (the owner's instruction).
