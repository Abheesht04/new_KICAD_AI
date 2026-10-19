/*
 * Built-in device models for the V2 project path (device model contract v1).
 *
 * Each spec states its terminals, parameters, assumptions and analyses, and
 * stamps its equations through the existing MnaDeviceOps engine. The
 * equations are exactly those previously inlined in src/v2/project.c.
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

/* ---- specs ---- */
static const DeviceTerminal PASSIVE[] = {{"p", "passive terminal 1 (KiCad pin 1)", NULL},
                                         {"n", "passive terminal 2 (KiCad pin 2)", NULL}};
static const DeviceTerminal SOURCE[] = {{"positive", "positive terminal; V(positive) - V(negative) = value", NULL},
                                        {"negative", "negative terminal", NULL}};
static const DeviceTerminal CURRENT[] = {{"p", "current leaves the network here and returns through the source", NULL},
                                         {"n", "current re-enters the network here", NULL}};
static const DeviceTerminal DIODE[] = {{"cathode", "cathode", "K|C|CATHODE|KATHODE"},
                                       {"anode", "anode", "A|ANODE"}};

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
#define DC_TRAN (DEVICE_CAP_DC | DEVICE_CAP_TRANSIENT)

static const DeviceModelSpec BUILTIN[] = {
    {"ideal.resistor", 1, "resistor", "Ideal linear resistor, V = I*R",
     "No tolerance drift, temperature coefficient, parasitic inductance/capacitance or noise.",
     PASSIVE, COUNT(PASSIVE), RESISTANCE, COUNT(RESISTANCE), 0, DC_TRAN, 0, resistor_stamp, resistor_stamp, NULL},
    {"ideal.capacitor", 1, "capacitor", "Ideal capacitor, i = C dv/dt; open circuit at DC",
     "No ESR, ESL, leakage, dielectric absorption or voltage coefficient. Transient: Backward Euler, "
     "zero initial voltage.",
     PASSIVE, COUNT(PASSIVE), CAPACITANCE, COUNT(CAPACITANCE), 0, DC_TRAN, 1, capacitor_dc, capacitor_transient,
     capacitor_commit},
    {"ideal.inductor", 1, "inductor", "Ideal inductor, v = L di/dt; short circuit at DC",
     "No DCR, core saturation, losses or self-resonance. Transient: Backward Euler, zero initial current.",
     PASSIVE, COUNT(PASSIVE), INDUCTANCE, COUNT(INDUCTANCE), 0, DC_TRAN, 1, inductor_dc, inductor_transient,
     inductor_commit},
    {"shockley.diode", 1, "diode", "Shockley diode, I = Is*(exp(V/(n*Vt)) - 1) at 300 K",
     "No series resistance, junction or diffusion capacitance (memoryless in transient), breakdown or temperature "
     "dependence; the exponent is clamped at +/-40.",
     DIODE, COUNT(DIODE), SHOCKLEY, COUNT(SHOCKLEY), -1, DC_TRAN, 0, diode_stamp, diode_stamp, NULL},
    {"ideal.voltage_source", 1, "voltage_source", "Ideal constant voltage source (simulation boundary, not a BOM part)",
     "Zero output impedance and unlimited current; switched on at t=0+ in transient.",
     SOURCE, COUNT(SOURCE), VOLTAGE, COUNT(VOLTAGE), 0, DC_TRAN, 0, voltage_stamp, voltage_stamp, NULL},
    {"ideal.current_source", 1, "current_source", "Ideal constant current source (simulation boundary, not a BOM part)",
     "Infinite output impedance and unlimited compliance voltage; switched on at t=0+ in transient.",
     CURRENT, COUNT(CURRENT), CURRENT_PARAM, COUNT(CURRENT_PARAM), 0, DC_TRAN, 0, current_stamp, current_stamp, NULL},
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
