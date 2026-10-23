# Transient analysis (Prompt 5)

`pcba simulate transient <file.copper> --dt S --steps N [--method be|trap] [--out CSV]`
runs fixed-step transient analysis on the project's circuit, through `src/transient.c`.

## Formulation

The circuit is the DAE F(t, x, ẋ) = 0. Nodes are MNA node voltages, plus branch currents for
voltage sources. Reactive models carry their own state and replace the derivative with a
**companion model**, i = G·v + I_h (current from terminal p to terminal n), for one step:

| Element and state | Backward Euler (order 1) | Trapezoidal (order 2) |
|---|---|---|
| Capacitor, state [v, i] | G = C/dt, I_h = −G·v₋ | G = 2C/dt, I_h = −(G·v₋ + i₋) |
| Inductor, state [i, v] | G = dt/L, I_h = i₋ | G = dt/2L, I_h = i₋ + G·v₋ |

Here `v₋` and `i₋` are the previously **accepted** state. Both methods track both state
values, so either method can follow the other. Memoryless models (R, sources, diode, BJT,
MOSFET) stamp the same equations as DC; they store no charge.

DC and transient are distinct stamps. At DC a capacitor is an open circuit and an inductor a
0 V branch (a short); in transient both are companion models. A node joined only by
capacitors is singular at DC but defined in transient (tested).

## Step contract (transactional)

1. Stamp every device from the accepted state into a fresh MNA circuit.
2. Solve with the Newton engine, then validate the residual and KCL independently.
3. Compute every device's tentative next state, and reject non-finite values.
4. **Only if 1–3 all succeed**, commit all states at once and advance time.

A failed step leaves the accepted state, step count and time **unchanged**. The test injects
stamp, solve and commit failures; in the commit case another device's commit had already
run. After each fault clears, the run continues bit-identically to an uninterrupted run.

The failure report has the rule, the 1-based step, the time it was solving for, the MNA
status, the iteration count, the maximum residual, the device label and a message:

```text
TRANSIENT solve_failed: step 1 at t=0.0001 s, MNA status 3, 1 iterations, max residual 0: ...
```

Rows printed before a failure are accepted steps. pcba reports the run as FAIL with
`steps_accepted` and never claims a successful waveform.

## Initial condition and discontinuity

- **Zero stored energy:** every state value starts at 0, so capacitor voltages and inductor
  currents are 0.
- **Sources switch on at t = 0⁺:** this is a step discontinuity. The first output row is
  t = dt; there is no t = 0 row.
- **Trapezoidal integration takes its first step with Backward Euler,** as SPICE does after
  a discontinuity: trapezoidal cannot start from a step without ringing.
  `first_step_method` in the result says so, and `TransientStep.method` reports each step's
  method.

Starting from the DC operating point, user initial conditions, time-varying sources,
adaptive timestep, local truncation error control and BDF2 are **not implemented**.

## Numerical behaviour (tested exactly)

| Case | Backward Euler | Trapezoidal |
|---|---|---|
| Constant current into C | V = I·t/C exactly | exactly |
| RC step, a = dt/τ | v_n = 1 − (1+a)⁻ⁿ | v_n(1+a/2) = v_{n−1}(1−a/2) + a after step 1 |
| Error vs exp at t = τ, a = 0.01 | ≈ 1.8e-3 | more than 50× smaller |
| Lossless LC, h = dt/√(LC) | Energy × 1/(1+h²) per step (artificial damping) | Energy constant after step 1 (to 1e-9 relative over 2000 steps) |

Choose dt well below the shortest time constant. Backward Euler damps oscillations;
trapezoidal preserves them but can ring on stiff circuits after a discontinuity.

## Settings reported with every result

`method`, `first_step_method`, `initial_state`, `dt_s`, `steps_requested`, `steps_accepted`,
and the Newton `tolerances`. Those tolerances are:
- residual: absolute 1e-10, relative 1e-10;
- update (nonlinear circuits): absolute 1e-9 V, relative 1e-9;
- step limit: 0.25 V per iteration;
- 100 iterations.

## Statuses

| Rule | pcba status |
|---|---|
| — | PASS |
| `invalid_argument` (dt ≤ 0, non-finite, > 1000 s; steps ∉ 1..100000) | INVALID_INPUT |
| `unsupported_model`, `unsupported_analysis` (no model, no transient capability, method not implemented by a stateful model) | UNVERIFIED |
| `solve_failed`, `model_stamp`, `state_commit` | FAIL |

## Device contract (v3)

`DeviceModelSpec.integration_methods` lists the methods a stateful model implements, and
`state_meaning` says what each state value is (for example `[0] voltage V(p)-V(n) in V, [1] current
p->n in A`). Registration rejects a stateful model without either, and a memoryless model that
declares either. Trapezoidal requires a stateful model to implement Backward Euler too, for
its first step. Capacitor and inductor are model version 3 (state [v, i] / [i, v], both
methods).

## Tests

- `tests/transient_test.c`:
  - constant-current charging;
  - RC and RL against both recurrences and the exact exponential;
  - LC energy for both methods;
  - diode + capacitor with KCL at every step and settling to the DC operating point;
  - the first-step method;
  - rollback after stamp, solve and commit faults, with an exact continuation;
  - invalid dt, method, node and parameter;
  - a DC-only model and a Backward-Euler-only model;
  - DC versus companion.
- `tests/test_v2.py` `TransientEngine`:
  - the trapezoidal recurrence through a project;
  - reported settings;
  - failure details with no accepted rows;
  - UNVERIFIED for unmodelled parts.
- `tests/test_pcba_cli.py`: `--method trap` in JSON, and `--method rk4` → usage error.

**Optional benchmark (not done):** SUNDIALS IDA for stiff/general DAEs, only after this
contract is stable and a benchmark shows a benefit (Prompt 10).
