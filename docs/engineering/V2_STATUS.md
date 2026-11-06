# Copper V2: status against the owner's request (2026-11-07)

This document checks the owner's request of 2026-11-06 and the "Copper V2 parallel feature tracks" document, item by item.

The status words mean:
- **Done:** built and tested on this machine.
- **Partly:** built, but part of the evidence could not be produced here, which the row says.
- **Not done:** not built.

Everything is on branch `v2/complete` (tracks A–H, REST, and the C placer merged), with one branch per step as well.

## The owner's request

| Item | Status | Where / evidence |
|---|---|---|
| A hard rule, applied to every later prompt: deterministic work in C/CUDA, Python only for NLP, transformers, learning and model hosting | **Done** | `CLAUDE.md` (read at every session), `CHARTER.md` §3.1, the session memory |
| Check what was already implemented and how it is structured | **Done** | [C_FIRST_AUDIT.md](C_FIRST_AUDIT.md): a compliance table, the tracks' state before and after, and the port order for the legacy Python |
| Implement the parallel-tracks prompt | **Done (H partly)** | tracks A–H below |
| Port placement to C, more sophisticated, working with the existing C | **Done** | `src/v2_src/placement.c`, `copper-core place`; `layout.optimize` uses it. Same constructive start as Python; it beats the Python annealer on 6 of 7 fixtures and ties the 7th; zero `Board` violations. See [C_FIRST_AUDIT.md](C_FIRST_AUDIT.md) §3. |
| Server-side REST in C | **Done** | `src/v2_src/rest_server.c`, `copper-core serve-http`, tested over real HTTP ([TOOL_SERVER.md](TOOL_SERVER.md)) |
| CUDA | **Partly** | track H: the kernel is written and the CPU reference is verified. The GPU was **not run**: there is no CUDA toolkit or GPU here. |
| Python only to host the model on another machine | **Done** for the client side | the LLM client is C (track G) and talks to any Ollama URL; no Copper Python runs the model |
| Learning models (translation, part picking from C-parsed libraries and DFM rules) | **Not done** | No training code was written. The C side they would consume now exists: board, DFM rules, findings and the tool server. KiCad *library* parsing in C is still port step 5 (`tools/kicad_library.py` is legacy Python). |
| Update the overall `.md` docs | **Done** | the README, CHANGELOG, CLI.md and AUDIT_CHECKLIST, plus one document per track (listed below) |

## The tracks document

| Track | Status | Files | Evidence |
|---|---|---|---|
| **Ground rules** (new files, int64 nm, Arena, status enums, no printf or exit, tests, strict build, data-file limits) | **Done** | — | `build-tools/v2_rules_check.py` passes on all 17 library modules. Builds are warning-free under `-std=c17 -Wall -Wextra -Wpedantic`. UBSan (trap mode) is clean; **ASan was not run** (unavailable on MinGW). Protected files were edited only in separate, noted commits (see below). |
| **A** Gerber reader | **Done** | `gerber.c/.h` | All 17 KiCad 9.0.6 Gerbers parse with no issue. Edge.Cuts closes into one 40 × 30 mm loop. Flashes equal the drill file and the board model. 237 KiCad-written demo Gerbers parse with no error. G02/G03 is reported as unsupported. ([GERBER.md](GERBER.md)) |
| **B** Board model | **Done** | `board.c/.h` | The 5×0603 board (Copper-generated, labelled as such) plus its KiCad 10 re-save; the real KiCad 9.0.6 drill file; 13 KiCad 9.0 demo boards matching KiCad's drill export (4,294 holes). ([BOARD_MODEL.md](BOARD_MODEL.md)) |
| **C** DFM and fab profiles | **Done** | `dfm.c/.h`, `fab_profiles/jlcpcb_2layer.json` | Every limit is quoted from JLCPCB's page. Hole-to-edge is not published, so it is UNVERIFIED. Every limit is tested at the limit and 1 nm beyond it. Clearance equals a brute-force oracle on 6 KiCad 9.0 boards. ([DFM_V2.md](DFM_V2.md)) |
| **D** kicad-cli runner | **Done** | `kicad_cli.c/.h` | Tested against a fake CLI (every failure path, a timeout kill, 4 MiB streams, never overwriting) and against the real KiCad 10.0.5 CLI (a development check). ([KICAD_CLI.md](KICAD_CLI.md)) |
| **E** Findings and report | **Done** | `finding.c/.h`, writer in `json.c` | The golden `tests/golden/report.json` matches byte for byte and does not depend on input order. ([FINDINGS.md](FINDINGS.md)) |
| **F** Tool server | **Done** | `tool_server.c/.h` | A 25-message scripted transcript and the real process over stdio; structured errors; a JSONL session log. ([TOOL_SERVER.md](TOOL_SERVER.md)) |
| **G** LLM bridge | **Done (live model not run)** | `llm.h/.c`, `llm_mock.c`, `llm_ollama.c`, `schema_gbnf.c`, `agent_loop.c`, `eval/prompts.jsonl`, `scripts/run_llm_eval.{sh,ps1}` | All tests pass with the mock, and the eval scores 10/10 with the mock. The Ollama client is tested without a server. **The live Ollama test was not run (no Ollama here): UNVERIFIED.** ([LLM_BRIDGE.md](LLM_BRIDGE.md)) |
| **H** CUDA DFM kernel | **Partly** | `dfm_cpu_ref.c`, `dfm_kernel.h`, `cuda/dfm_gpu.cu`, `dfm_gpu.h`, `apps/dfm_bench.c` | The CPU reference equals g2's exact capsule distance on 1000 random boards. The CPU benchmark ran at N = 64, 1024 and 16384. **The GPU path was not built and not run** (no nvcc, no GPU). Its 1000-board GPU-vs-CPU test reports NOT RUN. The `.cu` file was only syntax-checked with g++ against a stub. ([CUDA_DFM.md](CUDA_DFM.md)) |

## Edits to protected files

Each edit is its own commit with a note.

| File | Commit | Change |
|---|---|---|
| `g2_types.h` | `1c547c4` | the missing include guard (the tracks document asks for this as a separate change) |
| `IR.c` (both copies) | `2975247` | quoted strings no longer reserve the rest of the input; memory had grown with the square of the file size |
| `excellon.c` | `d31896d` | a non-coordinate line after an undefined tool no longer stops parsing with `EXC_ERR_ARGUMENT` |

Fixes made while integrating `v2-native-kicad-integration` are noted in [KICAD_NATIVE.md](KICAD_NATIVE.md).

## What was not done or not verified

**Not verified, because the tools are not on this machine:**
- KiCad 9.x native gates: 5 tests skip as UNVERIFIED (only KiCad 10.0.5 is installed, which counts as a development preview);
- the live Ollama model;
- CUDA on a GPU;
- ASan.

**Not done:**
- **Python learning models:** translation, and part picking from C-parsed libraries.
- **The rest of the legacy deterministic Python:** routing, the project build and KiCad export, the library index, rules import, blocks, spec lowering and orchestration. The order is in [C_FIRST_AUDIT.md](C_FIRST_AUDIT.md) §2; only placement (step 1) is ported.
- **Single-PR merging:** the tracks document asks that each track be one PR adding new files only. Each track is its own commit and branch. Wiring each one into the build (`CMakeLists.txt`, `apps/copper_core.c`) and the shared docs also touched existing files.

**Per-session setup step:** the tracks document asks each session to "summarise the API surface in 15 lines before writing anything". The API was read before each track, but no separate 15-line summary was written down.
