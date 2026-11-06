# Copper (PCBAI) — standing rules for every session

These are the owner's hard rules. They apply to every prompt in this repository, whatever the prompt says, and
`CHARTER.md` is the full policy. If a prompt conflicts with them, say so and ask before going ahead; never work
around them silently.

## 1. Language split (CHARTER §3.1)

**C17 (with CUDA as an opt-in accelerator) for everything deterministic:**
- physics: MNA, AC, transient;
- geometry and DFM;
- placement and routing;
- KiCad read and write;
- CAM checks;
- fab profiles;
- the tool server;
- the LLM bridge client;
- REST serving and GPU kernels.

**Python only for:**
- NLP and transformers;
- learning algorithms: training, fine-tuning, translation models, and part-picking models that consume C-parsed libraries and DFM rules;
- hosting the model or LLM server, which may run on another machine.

**What that means in practice:**
- Do not write new deterministic Python.
- The deterministic Python in `tools/` is legacy: fix it, but do not extend it. Port it in the order given in `docs/engineering/C_FIRST_AUDIT.md`, and retire each part only when its C replacement passes the same tests.

## 2. V2 engine rules (`src/v2_src/`, the "Copper V2 parallel tracks" doc of 2026-11-06)

- **Lengths:** `int64` nanometres (`G2Nm`) everywhere. No `float`, `double` or `libm` in geometry or DFM. Learning code and solver physics are exempt.
- **Memory:** allocate from `Arena`. Use no `malloc` in library code except where `IR` already does.
- **Errors:** return status enums with a line or offset. Library code never calls `exit()` or `printf`.
- **Tests first:** each module has `tests/<module>_test.c` with a real KiCad-exported golden file. Say plainly when a golden file is Copper-generated rather than KiCad-exported.
- **Build:** `-std=c17 -Wall -Wextra -Wpedantic` with no warnings, plus `-fsanitize=address,undefined` where the toolchain has them. On MinGW, UBSan runs in trap mode and ASan is unavailable; report ASan as not run, never as passed.
- **New code, new files:**
  - Do not edit `g2_types`, `IR`, `arena`, `excellon` or `filesystem` without a note in the commit or PR.
  - Do not rename or move existing files.
  - Keep each track on its own branch.
- **Data, not constants:** fab limits and rule thresholds live in data files, each with a cited source and date. Never invent a fabricator capability.
- **Opt-in hardware:** GPU and model are opt-in. The mock LLM backend and the CPU DFM reference are the defaults; nothing needs a GPU or a model to build or to pass tests.

## 3. Standing project rules

- **KiCad 9.x only** for native validation; record the exact version. KiCad 10 results are development previews, never evidence.
- **Fail closed:**
  - report PASS, FAIL, UNVERIFIED, NOT_IMPLEMENTED or INVALID_INPUT honestly;
  - never invent MPNs, ratings or capabilities;
  - never weaken a test to make it pass.
- **Branches:**
  - never push to `main`;
  - each milestone goes on its own branch, pushed when the owner asks;
  - end commit messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Build environment (this machine):**
  - MinGW GCC 12.1 at `D:\msys64\mingw64\bin`;
  - cmake and ninja from a scratchpad venv;
  - run `ctest` without MinGW first on `PATH`, because its Python 3.9 breaks `hwc`;
  - write long patch scripts with the Write tool, not shell heredocs.
