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
