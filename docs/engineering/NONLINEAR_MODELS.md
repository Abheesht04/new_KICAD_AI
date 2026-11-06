# Nonlinear device models (Prompt 4)

The equations live in `src/mna.c` (`mna_diode_evaluate`, `mna_bjt_evaluate`,
`mna_mosfet_evaluate`). They stamp through the existing `MnaDeviceOps` engine and are
registered as device-model specs in `src/device_models.c`. No second solver was added. The
same evaluation functions feed the DC/transient stamp, the AC linearisation and the tests.

All models are **reference/learning models**: no charge storage, at 300 K. Transient treats
them as memoryless. AC uses the quasi-static conductance matrix at the DC operating point,
which is valid only well below the frequencies where device capacitances matter. Nothing
here models a real MCU or any specific manufacturer part.

Constants: k = 1.380649e-23 J/K, q = 1.602176634e-19 C, V_T = kT/q ≈ 25.85 mV at 300 K.

## Exponential limiting (diode hardening, audit I-10)

`mna_limexp(x)` = exp(x) for x ≤ 40, and e⁴⁰·(1 + (x − 40)) above that, with slope e⁴⁰.
Value and slope are continuous (C1), so the Jacobian always matches the current. The old
clamp held the current constant above the limit while still reporting a large slope, so the
two disagreed. Large negative x is exact: exp underflows to 0. It is used by the diode and
the BJT.

## Diode — `shockley.diode` v2 (kind D)

| | |
|---|---|
| Terminals | 0 = cathode (KiCad pin `K`), 1 = anode (`A`); current positive anode → cathode |
| Equation | I = Is·(limexp(V/(n·V_T)) − 1), V = V_anode − V_cathode; g = dI/dV |
| Parameters | `is_a` A [1e-30, 1], default 1e-14; `ideality` [0.1, 10], default 1 (generic, not datasheet) |
| Analyses | DC, transient (memoryless), AC (g at the operating point) |
| Not modelled | series resistance, junction/diffusion capacitance, breakdown, temperature |

## BJT — `ebers_moll.npn`, `ebers_moll.pnp` v1 (kind Q)

First-order Ebers–Moll, transport form, for the NPN, with V_BE = V_B − V_E and V_BC = V_B − V_C:

```text
I_F = Is·(limexp(V_BE/(Nf·V_T)) − 1)      I_R = Is·(limexp(V_BC/(Nr·V_T)) − 1)
I_C = I_F − I_R·(1 + 1/β_R)               I_B = I_F/β_F + I_R/β_R
I_E = −(I_C + I_B)                        (currents INTO each terminal; KCL exact)
```

Analytical Jacobian (rows I_C, I_B, I_E; columns V_C, V_B, V_E), with g_F = dI_F/dV_BE and
g_R = dI_R/dV_BC:

```text
dI_C = [ g_R(1+1/β_R),  g_F − g_R(1+1/β_R),  −g_F ]
dI_B = [ −g_R/β_R,      g_F/β_F + g_R/β_R,   −g_F/β_F ]
dI_E = −(dI_C + dI_B)
```

**PNP polarity transform:** apply the NPN equations to the negated terminal voltages and
negate the currents. The Jacobian keeps the same form, evaluated at the transformed voltages.

| | |
|---|---|
| Terminals | 0 = collector (`C`), 1 = base (`B`), 2 = emitter (`E`) |
| Parameters | `is_a` A, default 1e-16; `beta_f` default 100; `beta_r` default 1; `nf`, `nr` default 1. These are the generic SPICE defaults, not datasheet values |
| Regions | All four regions are tested: forward active (Ic/Ib = β_F), saturation (Ic/Ib < β_F), cutoff (leakage of order Is), reverse active (Ic/Ib = −(β_R + 1)) |
| Not modelled | Early effect (output conductance only via g_R), high injection, Gummel-Poon effects, terminal resistances, junction/diffusion charge, breakdown. **No Gummel-Poon fidelity is claimed** |

## MOSFET — `level1.nmos`, `level1.pmos` v1 (kind M)

Level-1 (Shichman–Hodges) with the body tied to the source. For the NMOS, with β = Kp·W/L
and V_ov = V_GS − V_TO, and for V_DS ≥ 0:

```text
cutoff      V_ov ≤ 0              I_D = 0
triode      0 ≤ V_DS < V_ov       I_D = β·(V_ov·V_DS − V_DS²/2)·(1 + λ·V_DS)
saturation  V_DS ≥ V_ov > 0       I_D = β/2·V_ov²·(1 + λ·V_DS)
```

Derivatives:
- triode: g_m = β·V_DS·(1 + λV_DS), g_ds = β·(V_ov − V_DS)·(1 + λV_DS) + β·(V_ov·V_DS − V_DS²/2)·λ;
- saturation: g_m = β·V_ov·(1 + λV_DS), g_ds = β/2·V_ov²·λ.

I_D, g_m and g_ds are continuous at V_DS = V_ov and at V_ov = 0 (both tested).

**Source/drain symmetry:** for V_DS < 0 the roles swap. In that case
I_D = −f(V_GD, V_SD), dI/dV_D = g_m′ + g_ds′, dI/dV_G = −g_m′, dI/dV_S = −g_ds′. This is
continuous at V_DS = 0, which is tested.

**PMOS polarity transform:** apply the NMOS equations to the negated voltages and negated
V_TO, and negate the current. An enhancement PMOS has V_TO < 0. The PMOS is checked with
PMOS hand formulas, not inferred from the NMOS passing: saturation gives
|I_D| = β/2·(V_SG − |V_TP|)²·(1 + λV_SD), with current leaving the drain.

| | |
|---|---|
| Terminals | 0 = drain (`D`), 1 = gate (`G`), 2 = source (`S`); the gate draws no current |
| Parameters | `vto` V, **required** (it defines the device); `kp` A/V², default 2e-5; `w`, `l` m, default 1e-4; `lambda` 1/V, default 0 (SPICE defaults) |
| Not modelled | body effect, subthreshold conduction, gate/junction capacitance, mobility degradation, breakdown. A transistor that is off contributes no conductance (there is no g_min), so a node connected only through an off transistor is reported singular |

## Project use (kinds Q, M)

A Q or M part **must** name its model (`ebers_moll.npn|pnp`, `level1.nmos|pmos`).
Polarity is never inferred from the kind. The pin map is explicit. For example, KiCad
`Transistor_BJT:BC547` uses pins 1 C / 2 B / 3 E:
`(pinmap (collector 1) (base 2) (emitter 3))`. `Transistor_FET:2N7002` uses 1 G / 2 S / 3 D.
During build, KiCad pin names are checked against the terminals, so a swapped map is rejected.
The voltage-rating check of a three-terminal part uses its largest terminal-to-terminal
voltage.

## Newton (DC operating point)

| Item | Behaviour |
|---|---|
| Initial guess | x₀ = 0, every unknown; deterministic, so repeated solves are bit-identical (tested) |
| Damping | SPICE junction limiting (`pnjlim`) inside the diode and BJT models: each junction is evaluated at a limited voltage relative to its previous linearisation, while linear nodes take full steps. The old 0.25 V global clamp is `maximum_voltage_step_v` (default 0, off). See NUMERICAL_ROADMAP.md, step 7 results |
| Residual test | max\|F\| ≤ 1e-10 + 1e-10·max(1, \|F₀\|) |
| Update test (new) | Nonlinear circuits must also satisfy max\|ΔV\| ≤ 1e-9 V + 1e-9·max\|V\|. A small residual alone can hide a large voltage error at a high-impedance node. Linear circuits need only the residual test (one exact Newton step) |
| Guards | Non-finite residual or Jacobian → `DEVICE_FAILURE`; singular matrix → `SINGULAR_MATRIX`; iteration limit (default 100) → `NOT_CONVERGED`. None of these ever reports success |
| Trace | Optional callback per linear system, step, residual and convergence |

Source stepping and g_min continuation are not implemented. A hard operating point fails
with `NOT_CONVERGED` instead of being hidden.

**Test change:** two existing MNA tests asserted exactly 20 iterations for a damped circuit
flagged as nonlinear. With the update test they now take 21 (20 damped steps plus one
confirming step). They also assert that the final update is within tolerance.

## Tests and tolerances

`tests/nonlinear_models_test.c` covers:
- `limexp` continuity;
- the diode current, and its Jacobian beyond the limit;
- BJT currents in all four regions, by hand formula;
- PNP checked on its own;
- NMOS cutoff, triode, saturation and reverse;
- boundary continuity (relative error 1e-7 to 1e-8 at ±1e-9 V);
- PMOS cutoff, saturation and triode by PMOS formula, and gate-direction sense.

Its **central finite-difference Jacobian** checks use h = 1e-7·max(1, |V|) and agree within
1e-6 of the column scale. They run at 6 to 9 points per device, including both polarities. It
also solves a CE NPN amplifier, an NMOS low-side and a PMOS high-side switch, each verified by
KCL at the solution against the model equations (relative error 1e-9). Finally it covers
determinism, `NOT_CONVERGED` with 2 iterations, and parameter rejection.

`tests/test_v2.py` `Transistors` checks the same devices through `.copper` projects:
- results against an **independent Python re-implementation** of the equations;
- the CE small-signal gain against a hand-built 2×2 nodal system, and against −g_m·R_C from
  the base;
- polarity inference rejected, and a required `vto`;
- BJT pin-swap detection.

**ngspice comparison: UNVERIFIED.** ngspice is not installed in this environment. That
comparison remains optional and is not part of the deterministic suite.
