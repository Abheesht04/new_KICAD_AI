# Copper iterative optimization overlay

This overlay adds an iterative, evidence-gated optimization loop to the Copper C17 source tree. It is built against the user's uploaded `v2_src (3).zip` baseline and includes the previously established catalogue, topology and engineering-loop integration files required by the current CLI/CMake tree.

## Install

1. Back up your current `v2_src` directory.
2. Extract this ZIP at your Copper project root so the included `v2_src/` directory merges into your existing `v2_src/` directory.
3. Allow replacement of `CMakeLists.txt` and `copper_cli.c`; the other files are the corresponding new/required integration modules, tests and schemas.
4. Configure and build:

```powershell
Remove-Item .\build -Recurse -Force -ErrorAction SilentlyContinue
cmake -S v2_src -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## Optimization model

The loop separates three responsibilities:

- **Copper evaluator:** generates candidate actions, applies them to cloned board state, runs structural checks, configured DFM, and an optional physics callback, then reports metrics and evidence.
- **Decision provider:** chooses an eligible candidate ID or stops. This is the integration boundary for the AI. Candidate eligibility means the candidate passed configured admissibility/safety gates; it does not mean Copper prefers it or that it is manufacturing-release-ready.
- **Copper applier:** rechecks the board hash and candidate identity against the current board/configuration, then applies the selected action to an arena-owned in-memory Board copy.

The provider may choose any eligible trade-off. Copper does not silently replace the provider's decision with a hard-coded objective. The `greedy` policy is only a developer test harness.

## CLI workflow

### 1. Evaluate and produce candidate JSON

```powershell
.\build\Release\copper.exe user optimize evaluate `
  .\validation\boards\sample_resistor_lol.kicad_pcb `
  --rules .\v2_src\jlcpcb_rules.rules `
  --today 2026-11-17 `
  --copper-oz-x10 10 `
  --edge routed `
  --step-nm 100000 `
  --propose-routes `
  --route-grid-nm 500000 `
  --trace-width-nm 200000 `
  --clearance-nm 200000 `
  --route-edge-clearance-nm 200000 `
  --out .\validation\optimization-evaluation.json
```

The evaluation JSON contains the input board hash, evaluation settings, baseline metrics, every proposed action, candidate metrics, eligibility, reasons and `decision_authority: external_agent_or_user`.

### 2. Let the user/AI choose

Read `candidates[]` and consider only entries where `eligible_for_iteration` is true. The decision provider can choose any eligible `action.candidate_id`, including a candidate that trades HPWL for routing progress or another metric. It can also stop without selecting an action.

Example to select the first eligible entry for a smoke test (not a production AI policy):

```powershell
$evaluation = Get-Content .\validation\optimization-evaluation.json -Raw | ConvertFrom-Json
$selected = $evaluation.candidates | Where-Object { $_.eligible_for_iteration } | Select-Object -First 1
$selected.action.candidate_id
```

### 3. Apply the selected action

Repeat the **same board, rules, date and candidate-generation options** used for evaluation. Candidate IDs include a deterministic evaluation-context hash over the board, parser coverage diagnostics, fabrication rules and their provenance, candidate-generation/DFM/router options, and the physics-provider identity. A changed board, rules set, or evaluation configuration makes the ID stale. The 64-bit ID/hash is a deterministic stale-action guard, not a cryptographic authorization token.

```powershell
.\build\Release\copper.exe user optimize apply `
  .\validation\boards\sample_resistor_lol.kicad_pcb `
  --candidate-id $selected.action.candidate_id `
  --rules .\v2_src\jlcpcb_rules.rules `
  --today 2026-11-17 `
  --copper-oz-x10 10 `
  --edge routed `
  --step-nm 100000 `
  --propose-routes `
  --route-grid-nm 500000 `
  --trace-width-nm 200000 `
  --clearance-nm 200000 `
  --route-edge-clearance-nm 200000 `
  --out .\validation\optimization-apply.json
```

**Important:** apply commits only to an in-memory Board copy and writes a deliberately partial JSON snapshot containing footprint positions and tracks. It does not modify the original `.kicad_pcb` and is not a native KiCad export.

### Developer-only greedy smoke test

```powershell
.\build\Release\copper.exe dev optimize run `
  .\validation\boards\sample_resistor_lol.kicad_pcb `
  --policy greedy --iterations 3 `
  --rules .\v2_src\jlcpcb_rules.rules `
  --today 2026-11-17 --copper-oz-x10 10 --edge routed `
  --step-nm 100000 --propose-routes `
  --route-grid-nm 500000 --trace-width-nm 200000 `
  --clearance-nm 200000 --route-edge-clearance-nm 200000 `
  --out .\validation\optimization-run.json
```

The greedy policy is just a deterministic baseline for testing. It is not the AI decision policy.

## C API for AI integration

`optimization_loop.h` exposes:

- `copper_opt_evaluate(...)` — evaluate and enumerate candidate actions.
- `copper_opt_apply(...)` — stale-check and apply one selected action.
- `copper_opt_run(..., CopperOptDecisionFn decision, ...)` — in-process multi-iteration loop using a caller-supplied decision callback.
- `CopperOptDecisionFn` — receives baseline metrics and all candidates that pass the configured safety/evidence gates; returns an eligible candidate ID or zero to stop.
- `CopperOptPhysicsEvaluateFn` — optional per-candidate physics evidence provider. It may call MNA, stress, thermal or future extracted-parasitic solvers. It must be deterministic for fixed candidate/options and must not mutate the Board.

When a physics callback is configured, `physics_provider_id` must identify its stable model/version (for example `copper-mna-dc-v1`); this identity enters the evaluation hash so an action cannot be reused with different physics evidence. When `require_physics_for_acceptance` is enabled, missing physics evidence blocks candidate eligibility. The current CLI does not attach a built-in candidate-specific physics callback; an AI host or later engineering adapter must configure one through the C API. This is intentionally visible in JSON as `physics_available: false` rather than being treated as a pass.

## What the current candidate operators support

- Footprint translation proposals (no rotation optimizer yet).
- A* two-terminal net routing proposals on the configured single copper layer.
- Candidate cloning and board-hash stale-action checks.
- Structural validation, DFM evidence and optional physics evidence.
- Deterministic candidate IDs and JSON result documents.

## Explicit limitations

This milestone is an iterative **candidate/evaluate/decide/apply** engine, not a complete autonomous PCB optimizer or manufacturing release system.

- It does not yet optimize footprint rotations.
- The router proposals are limited to supported two-pad nets, one layer and supported board geometry; no via insertion or multilayer routing.
- Zones, arcs and other unsupported geometry can produce UNVERIFIED or blocked proposals.
- The optional physics callback interface exists, but the CLI does not yet attach a candidate-specific MNA/parasitic/thermal evaluator. The supplied evaluation therefore reports physics as unavailable unless an integrating host configures the callback.
- There is no post-layout parasitic extraction or thermal simulation in this milestone.
- There is no native `.kicad_pcb` export, Gerber or Excellon generation from applied candidate snapshots.
- Every optimization/apply/run JSON deliberately reports `manufacturing_release_ready: false`; apply/run snapshots declare `is_partial_snapshot: true`.

Do not use these partial snapshots as fabrication inputs.

## JSON schemas

- `schemas/copper.optimization-evaluation.v1.schema.json`
- `schemas/copper.optimization-apply.v1.schema.json`
- `schemas/copper.optimization-run.v1.schema.json`

The schemas make the decision authority, evaluation configuration, candidate eligibility, partial snapshot scope and manufacturing-release limitation explicit.

## Verification performed in the build environment

- GCC strict C17 build: passed.
- Clang strict C17 build: passed.
- Clang AddressSanitizer + UndefinedBehaviorSanitizer build and tests: passed.
- CTest: 6/6 test executables passed under GCC, Clang and ASan/UBSan.
- Real sample PCB evaluation, apply and three-iteration developer run executed.
- Evaluation/apply/run JSON independently parsed and validated against their JSON Schema documents.
- MSVC 2026 and KiCad CLI must still be run on the user's Windows environment.

Real sample PCB evaluation observed 5 footprints, 0 tracks, HPWL 14,990,000 nm, 0 DFM FAIL and 28 DFM UNVERIFIED. 44 candidates were generated with the configured move and route options. A selected eligible placement action was applied to the in-memory copy and changed HPWL to 15,090,000 nm; it remained eligible because candidate admissibility is based on configured safety/evidence gates, not a hidden HPWL preference. The developer greedy test accepted three actions and reduced its two-pad-unrouted-net heuristic. These are optimizer smoke tests, not evidence of full electrical or manufacturing sign-off.
