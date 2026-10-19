# AC small-signal analysis (Prompt 3)

`pcba simulate ac <file.copper> --start HZ --stop HZ --points N [--sweep log|lin] [--out CSV]`
solves the linearised circuit in the frequency domain:

```text
A(jω) x = b(jω),   ω = 2πf
```

It runs on the project's actual circuit, with the same parsed components, device models and
pin maps as DC and transient. No DC value is reused as an "AC magnitude".

## Contract

1. **Model coverage.** Every component must have a device model with the AC capability. A
   kind-U part reports `unsupported_model`, and a model without AC reports
   `unsupported_analysis`. In both cases the sweep is blocked (UNVERIFIED in pcba), and no
   device is ever omitted. J connectors are ideal boundaries and contribute nothing.
2. **Excitation.** Independent sources contribute **zero** AC excitation unless the component
   declares `(ac MAGNITUDE PHASE_DEG)`, which is valid only on V and I sources. A sweep with
   no non-zero excitation is rejected as `no_excitation` (INVALID_INPUT), because every
   response would be zero.
3. **Operating point.** If any model sets `needs_operating_point` (today only the diode), the
   DC operating point is solved first. Failure gives `operating_point` (FAIL). Linear
   circuits skip it, so a node that only capacitors join (singular at DC) still has a
   well-defined response at f > 0.
4. **Assembly and solve.** At each frequency the system is cleared, every model's `stamp_ac`
   runs, and it is solved. A frequency with no unique solution is reported with status
   `singular` and empty values. The run still writes every row, then exits 2 with `singular`
   (FAIL).
5. **Output.** Output is CSV, one row per frequency in increasing order:
   - `frequency_hz` and `status`;
   - for every non-ground net, `V(net)_re, _im, _mag, _phase_deg`;
   - for every branch-current device (V, L), `I(ref)_re, _im, _mag, _phase_deg`.

   Values are printed to 12 significant digits. Phase is in degrees, (−180, 180], and 0 for a
   zero phasor. NaN/Inf never appear: a non-finite stamp or excitation is reported as a status.

## Conventions

| Item | Convention |
|---|---|
| Unknown order | Node voltages for nets 2..N in declaration order (`GND` is node 0 and has no row), then branch currents in component order (V sources and inductors, one each) |
| Node rows | Sum of currents *leaving* the node through devices; identical to DC MNA |
| Branch current | Flows from the model's first terminal through the device to the second. For a source delivering power it is negative (e.g. `I(V1)` at 180° in a resistive load) |
| Resistor | Admittance `1/R` |
| Capacitor | Admittance `jωC`; an open circuit at ω = 0 |
| Inductor | Branch row `V(p) − V(n) − jωL·I = 0`; a short at ω = 0 |
| Voltage source | Branch row `V(+) − V(−) = E`, with E = `MAG∠PHASE` |
| Current source | Phasor current leaves terminal p, passes through the source, enters n; right-hand side −I at p, +I at n (DC device convention) |
| Diode | `g = dI/dV` of the same Shockley equation DC uses, at the solved operating point. The model stores no charge, so there is **no capacitive term** |

For a general nonlinear DAE `F(x) + dq(x)/dt = 0` the small-signal matrix is
`J_F + jω J_q`. Each model contributes its own `J_q` through `stamp_ac`; a capacitive term is
never inferred from the DC Jacobian. Models with junction/diffusion charge are Prompt 4 work.

## Linear solver: real 2N block (decision)

The complex system is rewritten as

```text
[Ar  −Ai] [xr]   [br]
[Ai   Ar] [xi] = [bi]
```

and solved by the existing dense `matrix_solve` (Gaussian elimination with partial pivoting).

| | Real 2N block (chosen) | Native complex LU |
|---|---|---|
| Code | Reuses the one tested linear solver | A second LU to write, test and maintain |
| Cost | 4× memory, about 2× flops of complex LU | Minimal |
| Pivoting | Partial pivoting on the equivalent real system | Complex-magnitude pivoting |
| Portability | Plain doubles | `Cplx` arithmetic only |

At today's sizes (≤ 256 nets, dense) the cost doesn't matter; correctness and one shared solver
do. Device equations only see `MnaAcSystem`, so the storage or solver can change later
without touching models (see Prompt 10's sparse roadmap).

**Pivot and scaling policy:** inherited from `matrix_solve`. There is an absolute pivot
threshold of 1e-12 and no scaling (audit issue I-01). A node whose total admittance stays
below about 1e-12 S during elimination is reported singular even if the exact system is
solvable. For example, a 0.1 pF capacitor at 1 Hz has ωC ≈ 6.3e-13 S. Prompt 10 covers
scaling.

## Statuses

| Rule (stderr `AC <rule>:`) | pcba status | Meaning |
|---|---|---|
| — | PASS | every frequency solved |
| `invalid_sweep` | INVALID_INPUT | non-finite bounds, stop < start, log with start ≤ 0, points ∉ 1..100000, 1 point with start ≠ stop |
| `no_excitation` | INVALID_INPUT | no source declares a non-zero `(ac ...)` |
| `unsupported_model`, `unsupported_analysis` | UNVERIFIED | a device lacks an (AC) model |
| `operating_point` | FAIL | DC operating point needed for linearisation did not solve |
| `singular` | FAIL | at least one frequency has no unique solution (rows marked) |
| `model_stamp` | FAIL | a model could not be linearised (allocation, missing operating point) |

## Tests (expected values are closed forms written in the tests)

- `tests/cplx_test.c`:
  - exact arithmetic;
  - Smith division with huge and tiny parts;
  - zero and non-finite denominators;
  - quadrant phases;
  - polar form.
- `tests/mna_ac_test.c`:
  - resistor divider at 30°, including the branch-current sign;
  - RC low-pass: `|H| = 1/√(1+(ωRC)²)`, phase `−atan(ωRC)`, f_c = 159.154943 Hz (1/√2, −45°);
  - RC high-pass;
  - RL, including the inductor current;
  - series RLC: `|I| = 1/√(R²+(ωL−1/ωC)²)`, a peak of 1/R at f₀ = 1/(2π√LC), and `|V_C| = Q`;
  - superposition with phased sources;
  - a DC-only source giving zero AC;
  - current-source phase;
  - ω = 0 (C open, L short);
  - a capacitor-only node, singular at 0 and valid above;
  - a floating node;
  - out-of-range stamps and NaN excitation;
  - sweep-grid validation and determinism;
  - diode `1/(1+R·g_d)`, with g_d recomputed from the solved V_d.
- `tests/test_v2.py` `AcAnalysis` checks the project path, using `examples/v2/rc_ac.copper`
  and generated projects:
  - closed forms across 41 frequencies;
  - RLC resonance on a project;
  - diode linearisation;
  - 0 Hz and singular rows;
  - every fail-closed rule;
  - backend status mapping;
  - column layout and deterministic output.
- `tests/test_pcba_cli.py` covers `pcba simulate ac`:
  - table output and CSV;
  - JSON at f_c;
  - usage errors;
  - no excitation → 2;
  - unsupported model → 3.

## Not implemented

- Noise analysis.
- Transfer-function or pole-zero output.
- Sweeps per decade.
- Sparse storage.
- Device capacitances: junction, diffusion, ESR, ESL. These belong to the device models
  (Prompt 4 and later). The AC result is exact for the ideal models as stated, not for real
  parts.
