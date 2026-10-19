/*
 * Built-in device models for the V2 project path (device model contract v1).
 *
 * Each spec states its terminals, parameters, assumptions and analyses, and
 * stamps its equations through the existing MnaDeviceOps engine. The
 * DC/transient equations are exactly those previously inlined in src/v2/project.c;
 * AC stamps (contract v2) linearise the same equations.
 * Transient integration is fixed-step Backward Euler (see Prompt 5 for the
 * planned DAE contract); the state of C/L instances is one value each.
 */
#include "model_registry.h"

#include <stdlib.h>

/* ---- constant current source, current flowing from terminal p to n through the source ---- */
typedef struct {
    size_t a, b;
    double i;
} CurrentSource;

static size_t no_unknowns(const void *data)
{
    (void)data;
    return 0;
}

static int current_load(void *data, const MnaDevice *device, MnaAssembly *assembly)
{
    const CurrentSource *c = data;
    (void)device;
    return (!c->a || mna_assembly_add_residual(assembly, c->a - 1, c->i)) &&
           (!c->b || mna_assembly_add_residual(assembly, c->b - 1, -c->i));
}

static const MnaDeviceOps CURRENT_OPS = {MNA_DEVICE_LINEAR, no_unknowns, current_load, free, NULL};

static int add_current(MnaCircuit *circuit, size_t a, size_t b, double current)
{
    CurrentSource *c = malloc(sizeof *c);
    if (!c) {
        return 0;
    }
    *c = (CurrentSource){a, b, current};
    if (!mna_add_device(circuit, &CURRENT_OPS, c)) {
        free(c);
        return 0;
    }
    return 1;
}

static double branch_voltage(const MnaResult *result, const size_t *nodes)
{
    return mna_node_voltage(result, nodes[0]) - mna_node_voltage(result, nodes[1]);
}

/* ---- stamps ---- */
static int resistor_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    (void)ctx;
    return mna_add_resistor(m, n[0], n[1], p[0]);
}

static int voltage_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    (void)ctx;
    return mna_add_voltage_source(m, n[0], n[1], p[0]);
}

static int current_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    (void)ctx;
    return add_current(m, n[0], n[1], p[0]);
}

static int diode_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    (void)ctx; /* terminals: 0 = cathode, 1 = anode */
    return mna_add_diode(m, n[1], n[0], p[0], p[1], 300.0);
}

static int capacitor_dc(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    (void)m, (void)n, (void)p, (void)ctx;
    return 1; /* open circuit */
}

/* Backward Euler: i = C/dt * (v - v_prev) -> conductance C/dt plus a history current. */
static int capacitor_transient(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    return mna_add_resistor(m, n[0], n[1], ctx->dt_s / p[0]) &&
           add_current(m, n[0], n[1], -p[0] / ctx->dt_s * ctx->state[0]);
}

static int capacitor_commit(const MnaResult *r, const size_t *n, const double *p, const DeviceStampContext *ctx,
                            double *next)
{
    (void)p, (void)ctx;
    next[0] = branch_voltage(r, n);
    return 1;
}

static int inductor_dc(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    (void)p, (void)ctx;
    return mna_add_voltage_source(m, n[0], n[1], 0.0); /* ideal short with a branch current */
}

/* Backward Euler: i = i_prev + dt/L * v -> conductance dt/L plus the previous current. */
static int inductor_transient(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *ctx)
{
    return mna_add_resistor(m, n[0], n[1], p[0] / ctx->dt_s) && add_current(m, n[0], n[1], ctx->state[0]);
}

static int inductor_commit(const MnaResult *r, const size_t *n, const double *p, const DeviceStampContext *ctx,
                           double *next)
{
    next[0] = ctx->state[0] + ctx->dt_s / p[0] * branch_voltage(r, n);
    return 1;
}

/* ---- AC small-signal stamps (contract v2) ---- */
static int resistor_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c)
{
    (void)c;
    mna_ac_stamp_admittance(s, n[0], n[1], cplx(1.0 / p[0], 0.0));
    return 1;
}

static int capacitor_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c)
{
    mna_ac_stamp_admittance(s, n[0], n[1], cplx(0.0, c->omega * p[0])); /* Y = jwC; open at w = 0 */
    return 1;
}

/* Branch current I flows p -> n through the inductor: V(p) - V(n) - jwL I = 0 (a short at w = 0). */
static int inductor_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c)
{
    mna_ac_stamp_branch(s, n[0], n[1], c->branch);
    mna_ac_add_branch_entry(s, c->branch, c->branch, cplx(0.0, -c->omega * p[0]));
    return 1;
}

/* Linearised at the DC operating point: g = dI/dV of the same Shockley equation DC uses.
 * The model stores no charge, so there is no capacitive term (see its assumptions). */
static int diode_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c)
{
    if (!c->operating_point) {
        return 0;
    }
    MnaDiode d = {n[1], n[0], p[0], p[1], 300.0};
    double current, conductance;
    mna_diode_evaluate(&d, mna_node_voltage(c->operating_point, n[1]) - mna_node_voltage(c->operating_point, n[0]),
                       &current, &conductance);
    mna_ac_stamp_admittance(s, n[1], n[0], cplx(conductance, 0.0));
    return 1;
}

/* A DC source contributes zero AC excitation unless the component declares (ac MAG PHASE). */
static int voltage_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c)
{
    (void)p;
    mna_ac_stamp_branch(s, n[0], n[1], c->branch);
    mna_ac_add_branch_rhs(s, c->branch, c->excitation);
    return 1;
}

static int current_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c)
{
    (void)p;
    mna_ac_inject_current(s, n[0], n[1], c->excitation);
    return 1;
}

/* ---- transistors (src/mna.c holds the equations; docs/engineering/NONLINEAR_MODELS.md) ---- */
static const double ROOM_K = 300.0;

/* Terminals 0 = collector, 1 = base, 2 = emitter; params is_a, beta_f, beta_r, nf, nr. */
static MnaBjt bjt_params(const size_t *n, const double *p, int polarity)
{
    MnaBjt b = {n[0], n[1], n[2], polarity, p[0], p[1], p[2], p[3], p[4], ROOM_K};
    return b;
}

/* Terminals 0 = drain, 1 = gate, 2 = source; params vto, kp, w, l, lambda. */
static MnaMosfet mos_params(const size_t *n, const double *p, int polarity)
{
    MnaMosfet m = {n[0], n[1], n[2], polarity, p[0], p[1], p[2], p[3], p[4]};
    return m;
}

static int npn_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)c;
    MnaBjt b = bjt_params(n, p, 1);
    return mna_add_bjt(m, &b);
}

static int pnp_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)c;
    MnaBjt b = bjt_params(n, p, -1);
    return mna_add_bjt(m, &b);
}

static int nmos_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)c;
    MnaMosfet t = mos_params(n, p, 1);
    return mna_add_mosfet(m, &t);
}

static int pmos_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)c;
    MnaMosfet t = mos_params(n, p, -1);
    return mna_add_mosfet(m, &t);
}

/* Quasi-static small-signal: the 3x3 conductance matrix d(I)/d(V) at the DC operating point. */
static void stamp_conductance_matrix(MnaAcSystem *s, const size_t *n, double d[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) mna_ac_stamp_node_entry(s, n[i], n[j], cplx(d[i][j], 0.0));
}

static double op_voltage(const DeviceAcContext *c, size_t node) { return mna_node_voltage(c->operating_point, node); }

static int bjt_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c, int polarity)
{
    if (!c->operating_point) return 0;
    MnaBjt b = bjt_params(n, p, polarity);
    MnaBjtEval e;
    mna_bjt_evaluate(&b, op_voltage(c, n[0]), op_voltage(c, n[1]), op_voltage(c, n[2]), &e);
    stamp_conductance_matrix(s, n, e.d);
    return 1;
}

static int mos_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c, int polarity)
{
    if (!c->operating_point) return 0;
    MnaMosfet t = mos_params(n, p, polarity);
    MnaMosfetEval e;
    mna_mosfet_evaluate(&t, op_voltage(c, n[0]), op_voltage(c, n[1]), op_voltage(c, n[2]), &e);
    double d[3][3] = {{e.d_vd, e.d_vg, e.d_vs}, {0, 0, 0}, {-e.d_vd, -e.d_vg, -e.d_vs}};
    stamp_conductance_matrix(s, n, d);
    return 1;
}

static int npn_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c) { return bjt_ac(s, n, p, c, 1); }
static int pnp_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c) { return bjt_ac(s, n, p, c, -1); }
static int nmos_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c) { return mos_ac(s, n, p, c, 1); }
static int pmos_ac(MnaAcSystem *s, const size_t *n, const double *p, const DeviceAcContext *c) { return mos_ac(s, n, p, c, -1); }

/* ---- specs ---- */
static const DeviceTerminal PASSIVE[] = {{"p", "passive terminal 1 (KiCad pin 1)", NULL},
                                         {"n", "passive terminal 2 (KiCad pin 2)", NULL}};
static const DeviceTerminal SOURCE[] = {{"positive", "positive terminal; V(positive) - V(negative) = value", NULL},
                                        {"negative", "negative terminal", NULL}};
static const DeviceTerminal CURRENT[] = {{"p", "current leaves the network here and returns through the source", NULL},
                                         {"n", "current re-enters the network here", NULL}};
static const DeviceTerminal DIODE[] = {{"cathode", "cathode", "K|C|CATHODE|KATHODE"},
                                       {"anode", "anode", "A|ANODE"}};

static const DeviceTerminal BJT[] = {{"collector", "collector", "C|COLLECTOR"},
                                     {"base", "base", "B|BASE"},
                                     {"emitter", "emitter", "E|EMITTER"}};
static const DeviceTerminal MOS[] = {{"drain", "drain", "D|DRAIN"},
                                     {"gate", "gate (no DC current)", "G|GATE"},
                                     {"source", "source; the body is tied to it", "S|SOURCE"}};
/* Defaults are the generic SPICE defaults, not datasheet values. Vto has no default: it defines the device. */
static const DeviceParam EBERS_MOLL[] = {
    {"is_a", DEVICE_UNIT_AMPERE, 1e-30, 1.0, 0, 1, 1e-16, "transport saturation current (SPICE default)"},
    {"beta_f", DEVICE_UNIT_DIMENSIONLESS, 0.01, 1e5, 0, 1, 100.0, "forward common-emitter current gain (SPICE default)"},
    {"beta_r", DEVICE_UNIT_DIMENSIONLESS, 0.001, 1e5, 0, 1, 1.0, "reverse common-emitter current gain (SPICE default)"},
    {"nf", DEVICE_UNIT_DIMENSIONLESS, 0.1, 10.0, 0, 1, 1.0, "forward emission coefficient"},
    {"nr", DEVICE_UNIT_DIMENSIONLESS, 0.1, 10.0, 0, 1, 1.0, "reverse emission coefficient"},
};
static const DeviceParam LEVEL1[] = {
    {"vto", DEVICE_UNIT_VOLT, -100.0, 100.0, 0, 0, 0.0, "threshold voltage (negative for an enhancement PMOS); required"},
    {"kp", DEVICE_UNIT_AMPERE_PER_VOLT2, 0.0, 1e3, 1, 1, 2e-5, "transconductance parameter (SPICE default)"},
    {"w", DEVICE_UNIT_METRE, 0.0, 1.0, 1, 1, 1e-4, "channel width (SPICE default)"},
    {"l", DEVICE_UNIT_METRE, 0.0, 1.0, 1, 1, 1e-4, "channel length (SPICE default)"},
    {"lambda", DEVICE_UNIT_PER_VOLT, 0.0, 10.0, 0, 1, 0.0, "channel-length modulation"},
};
static const DeviceParam RESISTANCE[] = {{"resistance", DEVICE_UNIT_OHM, 0.0, 1e12, 1, 0, 0.0, "resistance"}};
static const DeviceParam CAPACITANCE[] = {{"capacitance", DEVICE_UNIT_FARAD, 0.0, 1e12, 1, 0, 0.0, "capacitance"}};
static const DeviceParam INDUCTANCE[] = {{"inductance", DEVICE_UNIT_HENRY, 0.0, 1e12, 1, 0, 0.0, "inductance"}};
static const DeviceParam VOLTAGE[] = {{"voltage", DEVICE_UNIT_VOLT, -1e12, 1e12, 0, 0, 0.0, "constant source voltage"}};
static const DeviceParam CURRENT_PARAM[] = {{"current", DEVICE_UNIT_AMPERE, -1e12, 1e12, 0, 0, 0.0,
                                             "constant source current, p to n through the source"}};
static const DeviceParam SHOCKLEY[] = {
    {"is_a", DEVICE_UNIT_AMPERE, 1e-30, 1.0, 0, 1, 1e-14, "saturation current (generic default, not a datasheet value)"},
    {"ideality", DEVICE_UNIT_DIMENSIONLESS, 0.1, 10.0, 0, 1, 1.0, "emission coefficient n"},
};

#define COUNT(a) (sizeof a / sizeof a[0])
#define ALL (DEVICE_CAP_DC | DEVICE_CAP_TRANSIENT | DEVICE_CAP_AC)
#define BJT_ASSUMPTIONS                                                                                                 "First-order Ebers-Moll (transport form) at 300 K: no junction or diffusion charge (memoryless in transient; "      "AC is the quasi-static conductance matrix only), no Early effect, high injection, base/emitter/collector "         "resistance or breakdown. A learning/reference model, not Gummel-Poon."
#define MOS_ASSUMPTIONS                                                                                               "Level-1 Shichman-Hodges with the body tied to the source: no body effect, subthreshold conduction, "              "gate/junction capacitance (memoryless in transient; AC is gm/gds only), mobility degradation or breakdown. "      "Symmetric channel: drain and source swap when Vds changes sign."

static const DeviceModelSpec BUILTIN[] = {
    {"ideal.resistor", 2, "resistor", "Ideal linear resistor, V = I*R",
     "No tolerance drift, temperature coefficient, parasitic inductance/capacitance or noise.",
     PASSIVE, COUNT(PASSIVE), RESISTANCE, COUNT(RESISTANCE), 0, ALL, 0, resistor_stamp, resistor_stamp, NULL,
     resistor_ac, 0, 0, 0},
    {"ideal.capacitor", 2, "capacitor", "Ideal capacitor, i = C dv/dt; open circuit at DC",
     "No ESR, ESL, leakage, dielectric absorption or voltage coefficient. Transient: Backward Euler, "
     "zero initial voltage.",
     PASSIVE, COUNT(PASSIVE), CAPACITANCE, COUNT(CAPACITANCE), 0, ALL, 1, capacitor_dc, capacitor_transient,
     capacitor_commit, capacitor_ac, 0, 0, 0},
    {"ideal.inductor", 2, "inductor", "Ideal inductor, v = L di/dt; short circuit at DC",
     "No DCR, core saturation, losses or self-resonance. Transient: Backward Euler, zero initial current.",
     PASSIVE, COUNT(PASSIVE), INDUCTANCE, COUNT(INDUCTANCE), 0, ALL, 1, inductor_dc, inductor_transient,
     inductor_commit, inductor_ac, 1, 0, 0},
    {"shockley.diode", 2, "diode", "Shockley diode, I = Is*(exp(V/(n*Vt)) - 1) at 300 K",
     "No series resistance, junction or diffusion capacitance (memoryless in transient; AC is the operating-point "
     "conductance only), breakdown or temperature dependence; the exponent is clamped at +/-40.",
     DIODE, COUNT(DIODE), SHOCKLEY, COUNT(SHOCKLEY), -1, ALL, 0, diode_stamp, diode_stamp, NULL, diode_ac, 0, 0, 1},
    {"ideal.voltage_source", 2, "voltage_source", "Ideal constant voltage source (simulation boundary, not a BOM part)",
     "Zero output impedance and unlimited current; switched on at t=0+ in transient.",
     SOURCE, COUNT(SOURCE), VOLTAGE, COUNT(VOLTAGE), 0, ALL, 0, voltage_stamp, voltage_stamp, NULL, voltage_ac, 1, 1, 0},
    {"ideal.current_source", 2, "current_source", "Ideal constant current source (simulation boundary, not a BOM part)",
     "Infinite output impedance and unlimited compliance voltage; switched on at t=0+ in transient.",
     CURRENT, COUNT(CURRENT), CURRENT_PARAM, COUNT(CURRENT_PARAM), 0, ALL, 0, current_stamp, current_stamp, NULL,
     current_ac, 0, 1, 0},
    {"ebers_moll.npn", 1, "bjt", "NPN bipolar transistor, first-order Ebers-Moll", BJT_ASSUMPTIONS,
     BJT, COUNT(BJT), EBERS_MOLL, COUNT(EBERS_MOLL), -1, ALL, 0, npn_stamp, npn_stamp, NULL, npn_ac, 0, 0, 1},
    {"ebers_moll.pnp", 1, "bjt", "PNP bipolar transistor, first-order Ebers-Moll (polarity transform of NPN)",
     BJT_ASSUMPTIONS, BJT, COUNT(BJT), EBERS_MOLL, COUNT(EBERS_MOLL), -1, ALL, 0, pnp_stamp, pnp_stamp, NULL, pnp_ac, 0,
     0, 1},
    {"level1.nmos", 1, "mosfet", "N-channel MOSFET, Level-1 Shichman-Hodges", MOS_ASSUMPTIONS,
     MOS, COUNT(MOS), LEVEL1, COUNT(LEVEL1), -1, ALL, 0, nmos_stamp, nmos_stamp, NULL, nmos_ac, 0, 0, 1},
    {"level1.pmos", 1, "mosfet", "P-channel MOSFET, Level-1 Shichman-Hodges (polarity transform of NMOS)",
     MOS_ASSUMPTIONS, MOS, COUNT(MOS), LEVEL1, COUNT(LEVEL1), -1, ALL, 0, pmos_stamp, pmos_stamp, NULL, pmos_ac, 0, 0, 1},
};

int model_registry_register_builtin_devices(ModelRegistry *registry, char *err, size_t cap)
{
    for (size_t i = 0; i < COUNT(BUILTIN); ++i) {
        if (!model_registry_register_device(registry, &BUILTIN[i], err, cap)) {
            return 0;
        }
    }
    return 1;
}
