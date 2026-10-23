# Numerical roadmap

A measured baseline of the compiler's numerics and a ranked plan for changing them. Nothing here has been
implemented beyond the measurement tools: every solver change below needs approval first, and no new
dependency is proposed for the default build.

* Measure: `numeric_bench` (C, `apps/numeric_bench.c`: kernels against closed forms) and
  `python tools/numeric_bench.py --out baseline.json` (adds project-level transient, AC and corner checks
  through copper-core). Recorded 2026-10-23, MinGW GCC 12.1 `-O2` (Release), Windows 10; times are processor
  times and only their growth matters.
* Guard: `tests/test_numeric_oracles.py` (CTest `numeric_oracles`) asserts today's accuracy and pins the two
  limitations below as expected failures, so fixing one fails the test until this document is updated.
* Oracles: closed forms (ladder voltages, dividers, Lambert-W diode current, RC/LC responses, RC transfer
  function); ngspice as a differential reference when installed (not installed on the recording machine:
  UNVERIFIED); KiCad 9.x for EDA artifacts. The implementation under test is never its own oracle.

## Baseline

| Area | Current method (owner) |
|---|---|
| Linear solve | dense Gaussian elimination, partial pivoting, absolute pivot threshold 1e-12 (`src/matrix.c`); MNA assembled dense |
| Residual / convergence | max-norm residual over all rows (KCL in A and branch rows in V), `abs 1e-10 + rel 1e-10 × initial`; plus max update `1e-9 V + 1e-9 × max|V|` for nonlinear circuits (`src/mna.c`) |
| Nonlinear | Newton; the whole update vector scaled so no node moves more than 0.25 V; 100 iterations; `mna_limexp` above x = 40 |
| AC | complex MNA as a real 2N block, linearised at the DC point (`src/mna_ac.c`); no device capacitances |
| Transient | fixed step, Backward Euler or trapezoidal (first step BE), transactional state commit/rollback (`src/transient.c`) |
| Uncertainty | exhaustive resistor-tolerance corners, at most 8 resistors (2^8 solves), with target/rating checks |

## Measurements

**Dense solve (resistor ladder, error against the exact node voltages):**

| Nodes | Unknowns | Structural fill | Dense matrix | Mean solve | Max error |
|---|---|---|---|---|---|
| 10 | 11 | 24.8 % | 968 B | 5 µs | 4.4e-16 V |
| 100 | 101 | 2.9 % | 82 kB | 0.24 ms | 2.9e-15 V |
| 400 | 401 | 0.75 % | 1.3 MB | 13 ms | 8.4e-14 V |
| 800 | 801 | 0.37 % | 5.1 MB | 252 ms | 8.2e-14 V |
| 1600 | 1601 | 0.19 % | 20.5 MB | 2.26 s | 3.4e-13 V |

Growth is cubic (×19 and ×9 per doubling at the top), memory quadratic. 2-norm condition numbers of the same
matrices: 1.0e4 (10), 1.0e6 (100), 1.6e7 (400), 2.6e8 (1600) — harmless in double precision. The project
format caps a design at 256 nets and 256 components (≲ 512 unknowns), where a dense solve costs ≲ 30 ms.

**Scaling:** dividers of two equal resistors from 1 mΩ to 1 TΩ, a 1 mΩ/1 GΩ series mix and chains of 1 TΩ
resistors all solve with error ≤ 2.2e-16 V. The absolute 1e-12 pivot threshold is not reached inside the
device contract's ranges (R ≤ 1 TΩ), but it is unit-dependent: it would misjudge a correctly scaled system
whose entries are all below 1e-12.

**Degenerate systems:** two equal or conflicting voltage sources in parallel, and a node nothing touches, are
reported `singular_matrix` (refused, correctly) without naming the sources or the node; a 1 nΩ source loop
solves.

**Nonlinear (diode Is = 1e-14 A, n = 1, R = 1 kΩ from V, against Lambert W):**

| Supply | 0.3 V | 0.7 V | 1 V | 5 V | 10 V | 20 V | 24 V | 25 V | 30–1000 V |
|---|---|---|---|---|---|---|---|---|---|
| Iterations | 4 | 11 | 12 | 23 | 42 | 82 | 98 | — | — |
| Result | 2e-11 rel | 5e-16 | 1e-16 | 0 | 2e-16 | 2e-16 | 0 | **not converged** | **not converged** |

Converged answers are exact to round-off, but iterations grow ≈ 4 per volt because the 0.25 V clamp scales
the *whole* update, including linearly driven nodes, so any diode circuit above ~24 V fails from a zero
start. The shipped `examples/v2/diode.copper` solves at 24 V and fails (`mna_failure`, FAIL) at 25 V and 48 V.
It fails honestly, but 24 V and 48 V rails are common.

**Transient (closed forms):** RC step — Backward Euler observed order 0.94 → 0.99, trapezoidal 1.78 → 1.94
(its first step is BE). Series LC after 20 periods at 100 steps/period: BE peak 1.02 V of 2.0 V (−49 %
numerical damping), trapezoidal −0.11 %. Stiff RC pair (τ = 1 µs and 1 s, step 100 µs): BE has no ringing;
trapezoidal flips the sign of the fast node's error at 19 of 20 steps (not L-stable).

**AC:** RC low-pass over 41 points, 1 Hz–100 kHz: magnitude error 6e-13, phase 9e-11°.

**Corners:** 2^k exhaustive solves, 256 corners at k = 8 in ≈ 33 ms (process start dominates); k = 9 is
refused (`corner_capacity`), never silently truncated.

## Decision table

| # | Proposal | Measured limitation it addresses | Recommendation | Needs approval |
|---|---|---|---|---|
| N1 | Junction-voltage limiting + damped Newton (backtracking line search) in place of the global 0.25 V clamp | diode circuits above ~24 V do not converge; iterations ∝ supply | **Do next** | yes (solver behaviour) |
| N2 | gmin and source-stepping continuation as a reported fallback | none measured yet beyond N1; needed for multi-transistor operating points | after N1, if a fixture needs it | yes |
| S1 | Relative pivot threshold + 1-norm condition estimate in results | absolute 1e-12 threshold is unit-dependent; no conditioning diagnostic | **Do** (small) | yes (result schema) |
| S2 | Structural diagnosis of singular systems (source loops, floating nodes) before factorisation | singular systems are refused without naming the cause | **Do** | no (diagnostic only) |
| S3 | Per-row-type tolerances (A rows vs V rows) | mixed-unit max-norm; not yet a measured error | defer; document | — |
| L1 | Factor reuse for linear transient (one LU, many solves) | every step refactors O(n³) | do when a transient benchmark exceeds 1 s | no |
| L2 | Sparse unsymmetric LU (own CSR + AMD-ordered LU, or optional KLU) | cubic growth; 99 % zeros above 100 nodes | **defer**: designs are capped at ≲ 512 unknowns (≲ 30 ms) | yes (dependency) |
| T1 | Local truncation error estimate and step acceptance/rejection | fixed step only; no accuracy control | **Do** after N1 | yes |
| T2 | BDF2 (and TR-BDF2) as separately validated integrators | trapezoidal rings on stiff nodes; BE damps LC by 49 % | **Do** with T1 | yes |
| T3 | Breakpoints for discontinuous sources | only DC steps at 0+ exist today | when PWL/pulse sources are added | — |
| T4 | SUNDIALS IDA as an optional benchmark backend | no general DAE need yet | **defer** | yes (dependency) |
| A1 | Junction and charge-based device capacitances | transistor/diode AC is operating-point conductance only | required before any transistor AC fidelity claim | yes (model contract) |
| U1 | Sensitivity-guided worst case for monotone networks, verified at the chosen vertices | exhaustive corners stop at 8 resistors | **Do** for > 8 toleranced parts | yes |
| U2 | Seeded Monte Carlo with confidence intervals, optional | none: worst case is required, statistics are optional | later, never replacing U1/corners | yes |

## Recommendations

Each item: formulation, why it beats the baseline, references and licences, portability, benchmarks,
independent tests, complexity and failure modes, migration.

### 1. Scaling and residuals

**S1 — relative pivot threshold and conditioning.** Declare column k singular when
`|u_kk| ≤ ε · n · max_j |a_jk|` (scaled by the column's original norm) instead of `|u_kk| < 1e-12`; after
factorisation estimate `κ₁(A) ≈ ‖A‖₁ ‖A⁻¹‖₁` with Hager/Higham's estimator (a few extra O(n²) solves), report
it, and mark a solution whose `κ₁ · ε` exceeds the stated tolerance as UNVERIFIED rather than PASS.
*Why:* removes a unit-dependent constant; gives users a number for "near-singular". *References:* Higham,
*Accuracy and Stability of Numerical Algorithms* (2nd ed., §15.3); Hager (1984). No dependency.
*Portability:* pure C17. *Benchmarks:* the scaling set above plus matrices scaled by 1e±15 (identical answers
expected), and Hilbert-like near-singular ladders. *Independent tests:* exact answers of scaled systems;
known condition numbers of small matrices computed in closed form (2×2, tridiagonal Toeplitz eigenvalues).
*Complexity/failure:* O(n²) extra; the estimator can under-estimate by a modest factor (documented bound).
*Migration:* add the estimate to `MnaResult` and reports first (no behaviour change), then switch the
singular test behind a regression comparison of every fixture.

**S2 — structural singularity diagnosis.** Before factorising, build the bipartite graph of equations and
unknowns from the stamp pattern and find a maximum matching (Hopcroft–Karp); unmatched unknowns are
floating nodes, unmatched branch equations are dependent sources (parallel sources, source loops).
*Why:* the report can say "V1 and V2 are in parallel" or "node X touches only capacitors in DC" instead of
"singular". *References:* Duff, Erisman & Reid, *Direct Methods for Sparse Matrices*; Pothen & Fan (1990).
No dependency. *Benchmarks/tests:* the four degenerate cases measured above, each with the expected
offending element named. *Failure:* structural rank does not catch numerical cancellation; S1 still applies.
*Migration:* diagnostic only, added to the existing `mna_failure` finding.

**S3 — per-row tolerances.** Use separate absolute tolerances for current rows (A) and voltage rows (V)
(SPICE `abstol`/`vntol` style) inside the same relative test. *Why:* today one 1e-10 constant mixes units;
no error has been measured from it, so this stays documented, not scheduled.

### 2. Sparse MNA

**L2 — sparse unsymmetric LU, optional.** Store the stamp pattern in CSR, order with AMD (or COLAMD), factor
with threshold partial pivoting (left-looking, Gilbert–Peierls), reuse the symbolic analysis across Newton
iterations and time steps; KLU (SuiteSparse; designed for circuit matrices, block-triangular form + AMD)
as an optional backend. *Why/when:* fill is < 1 % above 400 nodes and time is cubic, but current designs are
capped at ≲ 512 unknowns where dense costs ≲ 30 ms; the benefit is real only if the caps are raised or
transient runs become long. *References:* Davis & Palamadai Natarajan, "Algorithm 907: KLU" (ACM TOMS 2010);
Gilbert & Peierls (1988); Amestoy, Davis & Duff (AMD, 1996). *Licences:* KLU/AMD/COLAMD are LGPL-2.1+ (as
part of SuiteSparse); keep optional and dynamically linked, or write a small in-tree LU. *Portability:* KLU
needs a C toolchain and BLAS-free build; MSVC/MinGW packaging must be reviewed. *Benchmarks:* ladders and 2-D
resistor grids from 10 to 10⁴ nodes (dimension, nonzeros, fill-in, factor time, memory, residual) against
the dense solver. *Independent tests:* closed-form ladders; 2-D grid effective resistances (known series for
infinite grids, or a refined-mesh limit); and **the dense solver kept as the reference** for every circuit
≤ 200 unknowns, compared to round-off. *Failure:* pivoting for stability breaks the symbolic ordering
(threshold pivoting mitigates). *Migration:* behind an interface with the dense path as default and reference.

**L1 — factor reuse.** For linear circuits the transient Jacobian is constant for a fixed step: factor once,
then O(n²) solves per step. *Why:* removes O(n³) per step; trivial and dependency-free. *Tests:* identical
waveforms to round-off versus the current path. *Migration:* first optimisation to land if a measured
transient exceeds 1 s.

### 3. Nonlinear solve

**N1 — junction limiting and damped Newton.** Replace the global vector clamp with:
(a) device-level limiting of each junction voltage between iterations, SPICE `pnjlim`:
if `v_new > v_crit = n Vt ln(n Vt / (√2 Is))` and `|v_new − v_old| > 2 n Vt`, set
`v_new = v_old + n Vt ln(1 + (v_new − v_old)/(n Vt))` when `v_old > 0` (falling back to `v_crit` if the
argument is not positive), otherwise `v_new = n Vt ln(v_new / (n Vt))` (and `fetlim` for MOSFET gate voltages);
(b) a backtracking line search on the full Newton step `x + α Δx`, halving α until
`‖F(x + αΔx)‖ ≤ (1 − 10⁻⁴ α) ‖F(x)‖` (Armijo), with α ≥ 2⁻¹⁰ before declaring failure.
Convergence stays the existing residual **and** update test. *Why:* linear nodes then move in one step, so
iteration counts stop growing with supply voltage; junction limiting keeps exponentials in range without
starving the rest of the circuit. *References:* Nagel, *SPICE2* (UCB/ERL M520, 1975), limiting functions;
Kelley, *Solving Nonlinear Equations with Newton's Method* (SIAM 2003), line search. No dependency.
*Portability:* C17. *Benchmarks:* the diode sweep 0.3–1000 V (target: converged everywhere, iterations
bounded independently of V), BJT and MOSFET bias fixtures from `nonlinear_models_test`. *Independent tests:*
Lambert-W diode currents; closed-form MOSFET saturation/triode bias points; the existing central
finite-difference Jacobian tests (diode, BJT, MOSFET) stay as the derivative oracle. *Complexity/failure:*
limiting changes the iterate path, not the solution; a line search can stall on a non-smooth model (report,
never accept). *Migration:* implement behind `MnaOptions`, run every fixture and every committed example with
both paths, require identical converged answers to tolerance, then make it the default; the 0.25 V clamp
remains available for comparison until removed.

**N2 — continuation fallbacks.** gmin stepping (add `g` from every node to ground, solve, reduce g by ×10 to
zero, reusing each solution) and source stepping (scale independent sources 0 → 1). Results report which
continuation was needed. *Why:* standard remedy for multi-transistor operating points that defeat Newton.
*Tests:* the final solution must satisfy the un-augmented circuit's residual and KCL test (never accept
a gmin-augmented answer). *Migration:* only when a fixture needs it after N1.

**Verification already in place:** device-level and global KCL residuals in every result; central
finite-difference Jacobian tests for diode, BJT and MOSFET (`tests/nonlinear_models_test.c`); consistent
residual/Jacobian assembly through the device model contract.

### 4. AC

Complex MNA is already exact against closed forms (6e-13). **A1:** before any claim of transistor or diode
AC fidelity, add charge-based capacitances to the device contract — junction depletion
`C_j = C_j0 / (1 − V/φ)^m` (limited near φ) and diffusion `C_d = τ_T g_d` for diodes/BJTs, Meyer or a
charge-conserving model for MOSFETs — with derivatives `∂Q/∂V` stamped as `jωC` in AC and as charges in
transient. *References:* Massobrio & Antognetti, *Semiconductor Device Modeling with SPICE*; Ward & Dutton
(charge conservation). *Tests:* single-pole RC-equivalent responses of a reverse-biased diode against
`C_j(V)`; transient charge conservation on a capacitor-only loop. *Migration:* new model versions, never a
silent change to existing ones.

### 5. Transient DAE

**T1 — local truncation error control.** Estimate LTE from the difference between the corrector and an
explicit predictor (Milne's device): for trapezoidal, `LTE ≈ (x_c − x_p) / 3` with a second-order predictor;
accept a step when `‖LTE‖ ≤ reltol·|x| + abstol` per unknown, otherwise reject (state rollback already
exists) and retry with `h ← h · 0.9 (tol/LTE)^{1/(p+1)}`, bounded growth (≤ 2×) and a minimum step that fails
the analysis. *References:* Gear, *Numerical Initial Value Problems in ODEs* (1971); Hairer & Wanner,
*Solving ODEs II* (stiff/DAE). *Tests:* RC/LC closed forms at requested tolerances; step-count bounds;
reproducibility (deterministic step sequence). *Failure:* step collapse at discontinuities (T3 breakpoints).

**T2 — BDF2 / TR-BDF2.** BDF2 (`x_{n+1} − 4/3 x_n + 1/3 x_{n−1} = 2/3 h f(x_{n+1})`, variable-step form) is
L-stable and second order: no ringing on stiff nodes (where trapezoidal measurably rings) and far less LC
damping than BE. TR-BDF2 combines both. Each is added as its own validated integrator with its own order test,
never by changing the existing two. *References:* Bank et al., "Transient simulation of silicon devices and
circuits" (IEEE TCAD 1985) for TR-BDF2. *Tests:* observed order 2 on RC, no sign changes on the stiff pair, LC
amplitude within a stated bound, energy behaviour documented.

**T4 — SUNDIALS IDA.** Optional benchmark backend for general DAEs (variable order BDF). Licence BSD-3-Clause;
adds a sizeable dependency and its own tolerance semantics. *Recommendation:* defer; use it, if at all, as a
differential reference in benchmarks, not as the engine.

### 6. Component and model uncertainty

**U1 — sensitivity-guided worst case.** For each checked output y (node voltage, component power), compute
`∂y/∂p_i` by the adjoint method (one extra linear solve per output with the transposed matrix), set each
parameter to the bound that maximises (or minimises) y, then **verify by solving at that vertex** and, for
monotone linear networks, prove optimality by the sign pattern; otherwise fall back to exhaustive corners up
to the cap and report UNVERIFIED beyond it. *Why:* exhaustive corners stop at 8 parts (2^8 solves); adjoint
sensitivity is O(outputs) solves. *References:* Director & Rohrer, "The generalized adjoint network and network
sensitivities" (IEEE TCT 1969). *Tests:* agreement with exhaustive corners for every network ≤ 8 parts;
closed-form dividers. *Failure:* non-monotone nonlinear responses (interior extrema) — never claimed proven.

**U2 — Monte Carlo (optional).** Seeded, reproducible sampling with stated distributions from part data, and
results reported with confidence intervals (Clopper–Pearson for yield). Never a replacement for required
worst-case constraints; temperature and model uncertainty only from explicit data.

## Approvals requested

1. **N1** (junction limiting + damped Newton) — fixes the measured 25 V convergence limit; changes solver
   behaviour on every nonlinear circuit, so it lands behind an option with a both-paths comparison first.
2. **S1 + S2** (relative pivot test, condition estimate, structural diagnosis) — adds fields to results.
3. **T1 + T2** (LTE control, BDF2) — new transient options and result fields.

No dependency (KLU, SuiteSparse, SUNDIALS, ngspice) is proposed for the default build; L2/T4 stay deferred
until a measured benchmark at a raised project cap justifies them and packaging is reviewed.
