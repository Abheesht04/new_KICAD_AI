# CHARTER.md
## PCBAI / Copper Studio — V2 Engineering Charter

**Status:** Normative engineering policy  
**Repository:** `Abheesht04/new_KICAD_AI`  
**Current native-validation target:** KiCad **9.x only**. Record the exact detected version in every native-validation report. KiCad 10.x support is a future, explicit migration.

This charter governs Claude Code, Cursor, and human contributors. It supplements the staged V2 prompt pack. If a prompt conflicts with this charter, stop and report the conflict rather than guessing.

## 1. Mission

Build a deterministic, extensible electronics design compiler that can eventually turn a request such as “Build me an ESP32 LED PCB and generate the KiCad PCB” into a validated engineering specification, explicit circuit topology, selected parts, electrical analyses, a DFM-aware layout, and native KiCad artifacts.

Develop through understandable, reviewable increments. Do not create disconnected demos, parallel pipelines, placeholder physics, or abstractions more complicated than the capabilities they support. Do not claim “complete EDA” until the acceptance tests support that claim.

## 2. Non-negotiable principles

### 2.1 Preserve the engineering kernel

Preserve existing module ownership, folder structure, public interfaces, and working behavior unless a documented change is necessary. Do not reorganize the repository as a side effect of implementing a feature.

Audit before changing:
- `u128` and overflow-safe integer arithmetic.
- Numerical helper functions.
- Integer geometry and copper predicates.
- MNA assembly and device residual/Jacobian contracts.
- PCB representation and DFM checks.
- KiCad symbol/footprint adapters.
- Project format, tests, fixtures, and reports.

Do not replace correct code with a generic library merely to reduce code. External libraries may be proposed when their mathematical or maintenance benefits are demonstrated and the dependency is isolated.

### 2.2 One canonical engineering path

All frontends—CLI specifications, future offline NLP, optional model-assisted NLP, and imported project files—must normalize into the same versioned engineering specification and downstream path. No frontend may own a separate physics, binding, DFM, or KiCad generator.

```text
Prompt / project file / explicit specification
                  |
                  v
       Semantic normalization
                  |
                  v
       Engineering Spec IR
                  |
                  v
          Topology / Design IR
                  |
          +-------+--------+
          |                |
          v                v
   Parameter synthesis  Device-model mapping
          |                |
          v                |
     Catalogue binding     |
          +-------+--------+
                  |
                  v
     Electrical and physical design
                  |
          +-------+--------+
          |                |
          v                v
       MNA/analysis       PCB/DFM
          |                |
          +-------+--------+
                  |
                  v
     KiCad artifacts + reports + logs
```

This is a separation of responsibilities, not permission to create a new directory for every box. Fit responsibilities into the existing structure.

### 2.3 Fail closed; no false success

A check must not pass if it did not run or required data/model support is absent.

Use explicit statuses:
- `PASS`: the check ran and passed.
- `FAIL`: the check ran and failed.
- `UNVERIFIED`: required tool, model, data, or supported geometry was unavailable.
- `NOT_IMPLEMENTED`: the capability does not exist yet.
- `INVALID_INPUT`: the request violates a declared contract.

Never silently:
- Drop a component, pin, net, connection, constraint, analysis, or DFM rule.
- Replace an invalid explicit value with a default.
- Omit an unsupported device from a simulation and report a complete successful solve.
- Claim structural validation is electrical verification.
- Claim generated KiCad files passed ERC/DRC when those checks were not run.
- Treat missing ratings as unlimited or unsupported geometry as clear.
- Turn failed verification into a successful process exit code.
- Change tests merely to make failures green.

Warnings are not substitutes for failure when missing information could invalidate the claimed result.

### 2.4 Determinism and provenance

For the same normalized input, catalogue snapshot, DFM profile, tool versions, and analysis settings, deterministic stages should produce stable, reproducible results.

Record provenance for inferred, defaulted, user-specified, catalogue-derived, and computed values. Isolate network/API access from deterministic tests. Never log credentials or commit API keys.

### 2.5 Mathematical transparency

Every device model and numerical method must document governing equations, assumptions, units, sign conventions, terminal order, residual/Jacobian where relevant, boundary/singular cases, convergence criteria, failure behavior, and independent analytical/reference tests.

A solver must report singularity, non-finite values, invalid parameters, and non-convergence rather than return plausible-looking numbers.

## 3. Toolchain and language policy

Follow the C11/C23 decision already agreed by the repository owner and current V2 prompt pack. Do not reopen that decision or change language standards without explicit approval.

- Keep platform-specific code isolated.
- Preserve Windows/MSVC compatibility where required.
- Use CMake to express source files, feature flags, and test targets.
- Keep GCC/Clang and sanitizer builds useful where available.
- Do not use compiler-specific integer extensions without guards or a portable fallback.
- Do not commit machine-specific paths, generated build trees, binaries, caches, or secrets.

## 4. KiCad compatibility policy

**The current native-validation target is KiCad 9.x only.**

- Detect and record the exact KiCad version for each ERC/DRC/export run.
- If KiCad CLI is unavailable, report native validation as `UNVERIFIED`; do not silently skip it while claiming success.
- If the detected version is not 9.x, report an unsupported-version result unless the user explicitly selects a separately implemented compatibility mode.
- Do not claim KiCad 10.x support until a deliberate migration and its test suite are completed.
- Keep KiCad parsing and serialization behind adapters so future upgrades do not contaminate the core IR or solver.

Use native KiCad symbol libraries (`.kicad_sym`) and footprint libraries (`.pretty` / `.kicad_mod`) as the source of EDA library definitions. Preserve library identifiers, inheritance, units, pin numbers/types, pad numbers/types, geometry, and provenance.

A symbol is not a simulation model. A footprint is not a simulation model. Validate logical terminal → symbol pin → footprint pad mappings explicitly, especially for multi-unit and multi-terminal devices.

Generated files must be parsed/round-tripped and, where available, validated with KiCad 9.x CLI/ERC/DRC. Plausible-looking S-expressions are not proof of validity.

## 5. IR and topology contracts

IRs are versioned contracts, not loosely structured dictionaries. Document schema version, required/optional fields, types, units, ranges, stable IDs, reference semantics, validation errors, provenance, and serialization behavior.

Keep separate:
1. Engineering intent.
2. Logical device and terminal definitions.
3. Circuit topology and net connectivity.
4. Simulation model and parameters.
5. Manufacturer/catalogue part.
6. KiCad symbol and footprint.
7. Physical PCB geometry and constraints.

A logical design must be valid before an MPN or supplier SKU is selected. Legacy formats may be supported by explicit adapters; procurement fields must not be mandatory at the semantic input layer.

Every requested component and connection must either appear downstream or produce an explicit error. Do not silently change net names, use two-pin fallback for multi-terminal devices, or infer pin mappings from display names alone.

## 6. Device model extensibility

Users must eventually be able to add supported devices and model variants without editing unrelated solver/compiler code.

A model contract should identify:
- Stable model/device ID and model version.
- Terminal count, names, and order.
- Parameter schema, units, ranges, defaults, and provenance.
- DC operating-point behavior.
- Nonlinear residual/Jacobian capability where needed.
- AC small-signal linearization capability where supported.
- Transient state and integration behavior where supported.
- Optional noise/thermal behavior only when implemented.
- Symbol/footprint/catalogue mappings separate from equations.
- Tests and declared limitations.

Do not call a device supported merely because its enum, symbol, parser, or stub exists. End-to-end support requires input validation, model implementation, solver integration, independent tests, CLI exposure, and honest reporting.

Add device families incrementally. Prefer small, independently testable models over one giant switch statement or an untyped universal parameter bag. Model variants must be explicit and versioned.

## 7. MNA, AC, and transient mathematics

### 7.1 MNA contract

Document unknown-vector ordering, ground handling, KCL sign convention, branch-current convention, matrix ownership, scaling, tolerances, and singular-system behavior.

For linear MNA:

\[
\begin{bmatrix}G&B\\C&D\end{bmatrix}
\begin{bmatrix}v\\j\end{bmatrix}
=
\begin{bmatrix}i\\e\end{bmatrix}
\]

For nonlinear DC operating point, define \(F(x)=0\), compute \(J(x_k)\), and solve:

\[
J(x_k)\Delta x=-F(x_k), \qquad x_{k+1}=x_k+\alpha\Delta x
\]

Use explicit residual and update norms. Add damping/line search only with tests for convergence and failure behavior.

### 7.2 AC analysis

AC is a distinct complex-valued analysis, not a renamed DC solve:

\[
A(j\omega)x(j\omega)=b(j\omega)
\]

Use frequency-dependent device stamps. For nonlinear devices, compute a valid DC operating point first and linearize about it. Report magnitude, phase, frequency, units, and validity status.

Initial tests must include:
- RC low-pass: \(f_c=1/(2\pi RC)\).
- RC high-pass with the correct transfer function.
- RL and RLC networks with analytical references.
- DC limit and frequency-sweep ordering.
- Invalid/singular cases and unsupported models.
- Complex residuals and reference comparisons.

Do not report AC support for a device until its complex/small-signal stamp exists.

### 7.3 Transient DAE

Use explicit state ownership and commit/rollback semantics. Begin with tested Backward Euler. Compare against analytical RC/RL responses and test timestep sensitivity. Higher-order trapezoidal/BDF methods, adaptive timestep, local error estimation, and rejected-step rollback are later stages, not placeholders.

### 7.4 Sparse numerical computing

Benchmark the dense solver first. Measure matrix dimensions, nonzero count, assembly/factorization time, memory, residual, and end-to-end runtime. Only then decide whether to add a sparse unsymmetric solver or backend.

Keep matrix assembly independent of storage. Compare any sparse backend against the dense/reference solver on identical circuits and report numerical differences.

## 8. DFM and placement/routing

DFM is a first-class subsystem with explicit rules, measurements, provenance, and evidence.

Keep separate:
- Composition compatibility.
- Electrical verification.
- PCB geometry/manufacturing DFM.
- Supplier-specific manufacturing constraints.

Each finding should include rule ID/version, severity, status, measured value/unit, threshold/unit, affected geometry/component, profile source, and remediation guidance. Do not invent measurements.

Use the existing integer geometry and copper engine where applicable. Define units and rounding at conversion boundaries, guard overflow, and test exact boundaries against independent references. Unsupported geometry must be `UNVERIFIED`, not `PASS`.

Placement/routing policy:
- Treat clearance, board edge, keepouts, pad access, layer restrictions, width, drill, and connectivity as hard constraints when required.
- Optimize wirelength, vias, congestion, manufacturability margin, and assembly convenience only after satisfying hard constraints.
- Do not claim global optimality when using heuristics.
- Make route candidates deterministic for identical inputs/configuration.
- Run independent connectivity, short, clearance, and design-rule checks after routing.
- Compare candidates with measurable metrics and retain the best verified candidate, not merely the shortest route.

## 9. KiCad-first libraries and user-owned data

KiCad is the default source for symbols and footprints. Users must be able to import catalogue data (including JLCPCB/LCSC exports) and their own DFM profiles through explicit paths and validated schemas.

Suggested precedence:

```text
Project/user override
    > selected project catalogue/profile
    > bundled defaults
    > generic/unbound logical device
```

Precedence must be deterministic, documented, project-scoped, and tested. Imports must not silently mutate global defaults or destroy data on malformed input. Report accepted/rejected/duplicate/updated records and preserve provenance.

Part selection must be explainable: report candidates, selected part, rejection reasons, missing ratings, and unresolved constraints. Never invent MPNs, price, stock, ratings, or supplier capability.

## 10. CLI product policy

The terminal is a first-class product interface and must work without the desktop UI.

The CLI should provide:
- A compact colored PCBAI logo/banner, with plain/no-color mode.
- `--help` and subcommand help.
- Consistent naming, argument parsing, exit codes, and diagnostics.
- Explicit paths; no machine-specific hardcoded paths.
- Human-readable output plus optional JSON reports.
- Clear distinction between generated, simulated, verified, and unverified artifacts.
- Redacted logs and causal error messages.
- A command list in the banner/help, marking experimental commands.

Do not claim commands exist until registered and tested. Keep compatibility aliases only with documented deprecation.

## 11. NLP policy

Do not reuse the old NLP implementation by default. Build a new semantic frontend only after engineering specification and topology contracts are stable.

NLP is an untrusted proposal generator. It must emit the same versioned Spec IR as explicit CLI input. It cannot select unverified manufacturer parts by intuition, claim physics/DFM passed, or bypass schema validation.

Ambiguity that changes topology, safety, ratings, or analysis must prompt for clarification. Mark inferred/defaulted values. Offline deterministic tests must not require network/API access. Live model tests must be opt-in and never log credentials.

## 12. Testing requirements

Tests must assert engineering properties, not just process success.

Required layers:
1. Unit tests for core data structures and numerical helpers.
2. IR schema, invalid-input, and round-trip tests.
3. Topology and pin-mapping tests.
4. Catalogue import/override tests.
5. Analytical MNA tests.
6. Nonlinear residual/Jacobian and convergence tests.
7. AC analytical and complex-residual tests once AC exists.
8. Transient analytical and timestep tests.
9. DFM exact-boundary and profile tests.
10. KiCad 9.x symbol/footprint and native CLI tests when installed.
11. Routing connectivity/short/clearance tests.
12. CLI end-to-end tests.
13. Offline NLP tests once the new frontend exists.
14. Optional live API tests separated from deterministic CI.

For each end-to-end test, report input, IR/topology summary, catalogue resolution, DB status, model support, solver/analysis status, DFM findings, emitted artifacts, and skipped/unverified checks.

Never weaken a test merely to make it pass. Fixture lookup is not general NLP or arbitrary circuit synthesis.

## 13. Required implementation workflow

For every stage:
1. Inspect relevant files, callers, interfaces, CMake targets, and tests.
2. State current behavior and reproduce the defect/gap.
3. Propose the smallest coherent change and list expected files.
4. Implement only that stage.
5. Add regression tests.
6. Run targeted tests and then the applicable full suite.
7. Report actual commands, counts, failures, skipped tests, and environment limitations.
8. Stop at the stage boundary for human review.

Do not perform broad cleanup during a feature. Record unrelated issues separately. If prompt and code disagree, inspect and report rather than guessing.

## 14. Immediate work order: fix I-05 through I-08

Before starting Prompt 1, fix the four known false-success issues **I-05 through I-08** from the audit report.

For each issue:
- Locate the exact source and caller.
- Reproduce it with a failing regression test.
- Fix the root cause with the smallest patch.
- Verify invalid/incomplete analysis cannot return success.
- Confirm valid existing cases still pass.
- Run the full baseline suite.

Do not alter unrelated CLI presentation or refactor the architecture during this correctness gate. After these fixes are reviewed, proceed to the production CLI stage.

Use **KiCad 9.x only** for current native validation and record the exact version. Do not attempt the KiCad 10.x migration in this phase.

## 15. Definition of done

A feature is complete only when its owner/interface is documented; inputs and outputs are validated; folder/module ownership is preserved or a migration approved; normal, boundary, invalid, and failure cases are tested; numerical/geometry claims have independent evidence where applicable; unsupported cases fail explicitly; builds and relevant tests pass; reports distinguish `PASS`, `FAIL`, `UNVERIFIED`, `NOT_IMPLEMENTED`, and `INVALID_INPUT`; and documentation matches the connected implementation.

## 16. Mandatory stage report

```text
STAGE:
CURRENT BEHAVIOR:
ROOT CAUSE:
FILES INSPECTED:
FILES CHANGED:
INTERFACES CHANGED:
TESTS ADDED:
COMMANDS RUN:
BUILD RESULT:
TEST RESULT:
KICAD VERSION / NATIVE CHECK STATUS:
FEATURES VERIFIED END-TO-END:
FEATURES NOT IMPLEMENTED:
FEATURES UNVERIFIED:
KNOWN RISKS:
NEXT STAGE PROPOSAL:
```

Do not say “complete” without evidence.

## Initial instruction

Read this charter and the V2 prompt pack completely. Then perform only Section 14: inspect and fix I-05 through I-08 with regression tests. Do not start Prompt 1 until those fixes are complete and results are reported. Preserve the current folder structure and engineering kernel. Use KiCad 9.x as the only supported native-validation target for this phase.
