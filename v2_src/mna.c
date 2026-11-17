#include "mna.h"
#include "copper_linalg.h"
#include "nmath.h"

#include <float.h>
#include <math.h>
#include <string.h>

#if COPPER_TRACE
static void trace_mna_event(CopperTraceKind kind, const char *detail,
                            MnaStatus status, uint32_t iteration,
                            double residual, double alpha, double max_delta_v,
                            double gmin_s, double source_scale,
                            double time_s, double dt_s, int method)
{
    CopperTraceEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.module = "mna";
    event.detail = detail;
    event.status = (int)status;
    event.iteration = iteration;
    event.residual = residual;
    event.alpha = alpha;
    event.max_delta_v = max_delta_v;
    event.gmin_s = gmin_s;
    event.source_scale = source_scale;
    event.time_s = time_s;
    event.dt_s = dt_s;
    event.method = method;
    COPPER_TRACE_EVENT(event);
}
#define MNA_TRACE_EVENT(...) trace_mna_event(__VA_ARGS__)
#else
#define MNA_TRACE_EVENT(...) ((void)0)
#endif

typedef struct {
  size_t n;
  size_t branch_count;
  size_t *branch_index; /* SIZE_MAX when element has no MNA branch unknown */
  size_t *branch_element;
} MnaLayout;

static bool size_multiply(size_t a, size_t b, size_t *out) {
  if (!out || (b != 0 && a > SIZE_MAX / b))
    return false;
  *out = a * b;
  return true;
}

static double clampd(double x, double lo, double hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

static size_t node_index(uint32_t node) {
  return node == 0 ? SIZE_MAX : (size_t)node - 1u;
}

static double node_voltage(const double *x, uint32_t node) {
  return node == 0 ? 0.0 : x[(size_t)node - 1u];
}

static Cplx node_voltage_c(const Cplx *x, uint32_t node) {
  return node == 0 ? cplx(0.0, 0.0) : x[(size_t)node - 1u];
}

/* Public result arrays reserve element zero for ground, unlike the solver's
 * compact unknown vector. Keep this accessor distinct to avoid an off-by-one
 * when reporting nonlinear-device AC terminal currents. */
static Cplx result_node_voltage_c(const Cplx *x, uint32_t node) {
  return node == 0 ? cplx(0.0, 0.0) : x[node];
}

static bool alloc_layout(const Circuit *c, bool include_inductors,
                         Arena *workspace, MnaLayout *out) {
  if (!c || !workspace || !out)
    return false;
  memset(out, 0, sizeof(*out));
  if (c->element_count != 0) {
    out->branch_index = (size_t *)arena_alloc_array(
        workspace, sizeof(size_t), c->element_count);
    out->branch_element = (size_t *)arena_alloc_array(
        workspace, sizeof(size_t), c->element_count);
    if (!out->branch_index || !out->branch_element)
      return false;
  }
  for (size_t i = 0; i < c->element_count; ++i) {
    out->branch_index[i] = SIZE_MAX;
    if (c->elements[i].kind == CIRCUIT_VOLTAGE_SOURCE ||
        c->elements[i].kind == CIRCUIT_VCVS ||
        c->elements[i].kind == CIRCUIT_CCVS ||
        (include_inductors && c->elements[i].kind == CIRCUIT_INDUCTOR)) {
      out->branch_index[i] = c->node_count + out->branch_count;
      out->branch_element[out->branch_count++] = i;
    }
  }
  out->n = (size_t)c->node_count + out->branch_count;
  return true;
}

static void stamp_g(double *g, size_t n, uint32_t p, uint32_t q, double y) {
  size_t pi = node_index(p), qi = node_index(q);
  if (pi != SIZE_MAX)
    g[pi * n + pi] += y;
  if (qi != SIZE_MAX)
    g[qi * n + qi] += y;
  if (pi != SIZE_MAX && qi != SIZE_MAX) {
    g[pi * n + qi] -= y;
    g[qi * n + pi] -= y;
  }
}

static void stamp_g_complex(Cplx *g, size_t n, uint32_t p, uint32_t q, Cplx y) {
  size_t pi = node_index(p), qi = node_index(q);
  if (pi != SIZE_MAX)
    g[pi * n + pi] = cplx_add(g[pi * n + pi], y);
  if (qi != SIZE_MAX)
    g[qi * n + qi] = cplx_add(g[qi * n + qi], y);
  if (pi != SIZE_MAX && qi != SIZE_MAX) {
    g[pi * n + qi] = cplx_sub(g[pi * n + qi], y);
    g[qi * n + pi] = cplx_sub(g[qi * n + pi], y);
  }
}

static void stamp_branch(double *g, double *b, size_t n, uint32_t p, uint32_t q,
                         size_t branch, double branch_diagonal, double rhs) {
  size_t pi = node_index(p), qi = node_index(q);
  if (pi != SIZE_MAX) {
    g[pi * n + branch] += 1.0;
    g[branch * n + pi] += 1.0;
  }
  if (qi != SIZE_MAX) {
    g[qi * n + branch] -= 1.0;
    g[branch * n + qi] -= 1.0;
  }
  g[branch * n + branch] += branch_diagonal;
  b[branch] += rhs;
}

static void stamp_branch_complex(Cplx *g, Cplx *b, size_t n, uint32_t p,
                                 uint32_t q, size_t branch, Cplx rhs) {
  size_t pi = node_index(p), qi = node_index(q);
  Cplx one = cplx(1.0, 0.0), minus_one = cplx(-1.0, 0.0);
  if (pi != SIZE_MAX) {
    g[pi * n + branch] = cplx_add(g[pi * n + branch], one);
    g[branch * n + pi] = cplx_add(g[branch * n + pi], one);
  }
  if (qi != SIZE_MAX) {
    g[qi * n + branch] = cplx_add(g[qi * n + branch], minus_one);
    g[branch * n + qi] = cplx_add(g[branch * n + qi], minus_one);
  }
  b[branch] = cplx_add(b[branch], rhs);
}

static void stamp_vcvs_real(double *g, double *b, size_t n, uint32_t p,
                            uint32_t q, size_t branch, uint32_t cp, uint32_t cq,
                            double gain) {
  stamp_branch(g, b, n, p, q, branch, 0.0, 0.0);
  size_t cpi = node_index(cp), cqi = node_index(cq);
  if (cpi != SIZE_MAX)
    g[branch * n + cpi] -= gain;
  if (cqi != SIZE_MAX)
    g[branch * n + cqi] += gain;
}

static void stamp_vccs_real(double *g, size_t n, uint32_t p, uint32_t q,
                            uint32_t cp, uint32_t cq, double gain) {
  size_t pi = node_index(p), qi = node_index(q), cpi = node_index(cp),
         cqi = node_index(cq);
  if (pi != SIZE_MAX && cpi != SIZE_MAX)
    g[pi * n + cpi] += gain;
  if (pi != SIZE_MAX && cqi != SIZE_MAX)
    g[pi * n + cqi] -= gain;
  if (qi != SIZE_MAX && cpi != SIZE_MAX)
    g[qi * n + cpi] -= gain;
  if (qi != SIZE_MAX && cqi != SIZE_MAX)
    g[qi * n + cqi] += gain;
}

static void stamp_cccs_real(double *g, size_t n, uint32_t p, uint32_t q,
                            size_t control_branch, double gain) {
  size_t pi = node_index(p), qi = node_index(q);
  if (pi != SIZE_MAX)
    g[pi * n + control_branch] += gain;
  if (qi != SIZE_MAX)
    g[qi * n + control_branch] -= gain;
}

static double exp_clamped(double x, double limit) {
  return exp(clampd(x, -limit, limit));
}

static void stamp_terminal_jacobian(double *g, double *b, size_t n,
                                    const uint32_t *nodes, size_t count,
                                    const double *currents, double jac[4][4],
                                    const double *x) {
  for (size_t r = 0; r < count; ++r) {
    size_t ri = node_index(nodes[r]);
    if (ri == SIZE_MAX)
      continue;
    double linearized_rhs = -currents[r];
    for (size_t c = 0; c < count; ++c) {
      size_t ci = node_index(nodes[c]);
      if (ci != SIZE_MAX) {
        double v = jac[r][c];
        g[ri * n + ci] += v;
        linearized_rhs += v * x[ci];
      }
    }
    b[ri] += linearized_rhs;
  }
}

static void stamp_terminal_jacobian_complex(Cplx *g, size_t n,
                                            const uint32_t *nodes, size_t count,
                                            double jac[4][4]) {
  for (size_t r = 0; r < count; ++r) {
    size_t ri = node_index(nodes[r]);
    if (ri == SIZE_MAX)
      continue;
    for (size_t c = 0; c < count; ++c) {
      size_t ci = node_index(nodes[c]);
      if (ci != SIZE_MAX)
        g[ri * n + ci] = cplx_add(g[ri * n + ci], cplx(jac[r][c], 0.0));
    }
  }
}

static void diode_intrinsic_iv(const CircuitDiodeModel *model,
                               double junction_voltage, double exponent_limit,
                               double *current, double *conductance)
{
  double vt = model->emission_coefficient * model->thermal_voltage_v;
  double limit = exponent_limit > 0.0 ? exponent_limit : 40.0;
  double forward_exponent = clampd(junction_voltage / vt, -limit, limit);
  double reverse_exponent = model->breakdown_voltage_v > 0.0
      ? clampd((-junction_voltage - model->breakdown_voltage_v) / vt,
               -limit, limit)
      : -limit;
  double forward = model->saturation_current_a * expm1(forward_exponent);
  double reverse = model->breakdown_voltage_v > 0.0
      ? -model->breakdown_current_a * exp(reverse_exponent)
      : 0.0;
  double derivative = model->saturation_current_a * exp(forward_exponent);
  if (model->breakdown_voltage_v > 0.0)
    derivative += model->breakdown_current_a * exp(reverse_exponent);
  *current = forward + reverse;
  *conductance = derivative / vt;
}

/* Solve Vj + Rs*Id(Vj) = Vterminal. The scalar function is monotone because
 * dF/dVj = 1 + Rs*g_d >= 1. A bracketed Newton step falls back to bisection,
 * so an exhausted local Newton sequence cannot silently leak an unconverged
 * junction voltage into the outer MNA iteration. */
static bool diode_series_junction_voltage(const CircuitDiodeModel *model,
                                          double terminal_voltage,
                                          double exponent_limit,
                                          double *junction_voltage)
{
  double intrinsic_current, intrinsic_g;
  double vt = model->emission_coefficient * model->thermal_voltage_v;
  double rs = model->series_resistance_ohm;
  double residual_at_terminal;
  double low, high, voltage;

  if (!junction_voltage || !(vt > 0.0) || !(rs > 0.0) ||
      !isfinite(terminal_voltage))
    return false;

  diode_intrinsic_iv(model, terminal_voltage, exponent_limit,
                     &intrinsic_current, &intrinsic_g);
  (void)intrinsic_g;
  residual_at_terminal = rs * intrinsic_current;
  if (!isfinite(residual_at_terminal))
    return false;
  if (residual_at_terminal == 0.0) {
    *junction_voltage = terminal_voltage;
    return true;
  }

  if (residual_at_terminal > 0.0) {
    high = terminal_voltage;
    low = terminal_voltage - residual_at_terminal;
  } else {
    low = terminal_voltage;
    high = terminal_voltage - residual_at_terminal;
  }
  if (!isfinite(low) || !isfinite(high) || !(low < high))
    return false;

  voltage = 0.5 * (low + high);
  for (unsigned iteration = 0; iteration < 96u; ++iteration) {
    double current, conductance;
    double residual;
    double derivative;
    double candidate;
    double tolerance = 16.0 * DBL_EPSILON *
                       (1.0 + fabs(terminal_voltage) + fabs(voltage));

    diode_intrinsic_iv(model, voltage, exponent_limit,
                       &current, &conductance);
    residual = voltage + rs * current - terminal_voltage;
    derivative = 1.0 + rs * conductance;
    if (!isfinite(residual) || !isfinite(derivative) || derivative < 1.0)
      return false;
    if (fabs(residual) <= tolerance) {
      *junction_voltage = voltage;
      return true;
    }

    if (residual > 0.0)
      high = voltage;
    else
      low = voltage;

    candidate = voltage - residual / derivative;
    if (!isfinite(candidate) || candidate <= low || candidate >= high)
      candidate = low + 0.5 * (high - low);

    voltage = candidate;
    if (high - low <= tolerance) {
      *junction_voltage = low + 0.5 * (high - low);
      return true;
    }
  }

  /* One final midpoint is the bounded bisection fallback; verify it before
   * reporting success. */
  voltage = low + 0.5 * (high - low);
  diode_intrinsic_iv(model, voltage, exponent_limit,
                     &intrinsic_current, &intrinsic_g);
  if (fabs(voltage + rs * intrinsic_current - terminal_voltage) <=
      64.0 * DBL_EPSILON * (1.0 + fabs(terminal_voltage) + fabs(voltage))) {
    *junction_voltage = voltage;
    return true;
  }
  return false;
}

/* Evaluate nonlinear terminal currents (positive means current leaving each
 * terminal) and their Jacobian versus terminal voltage. */
static size_t device_eval(const CircuitElement *e, const double *x,
                          double currents[4], double jac[4][4],
                          double exponent_limit) {
  memset(currents, 0, sizeof(double) * 4);
  memset(jac, 0, sizeof(double) * 16);
  if (e->kind == CIRCUIT_DIODE) {
    const CircuitDiodeModel *model = &e->model.diode;
    double vt = model->emission_coefficient * model->thermal_voltage_v;
    double terminal_voltage = node_voltage(x, e->n1) - node_voltage(x, e->n2);
    double junction_voltage = terminal_voltage;
    double limit = exponent_limit > 0.0 ? exponent_limit : 40.0;

    if (!(vt > 0.0) || !(model->saturation_current_a > 0.0) ||
        !isfinite(vt) || !isfinite(model->saturation_current_a))
      return 0;
    if (model->series_resistance_ohm > 0.0 &&
        !diode_series_junction_voltage(model, terminal_voltage, limit,
                                       &junction_voltage))
      return 0;

    double current, intrinsic_g;
    diode_intrinsic_iv(model, junction_voltage, limit,
                       &current, &intrinsic_g);
    double conductance = intrinsic_g /
        (1.0 + model->series_resistance_ohm * intrinsic_g);
    if (!isfinite(current) || !isfinite(conductance))
      return 0;
    currents[0] = current;
    currents[1] = -current;
    jac[0][0] = conductance;
    jac[0][1] = -conductance;
    jac[1][0] = -conductance;
    jac[1][1] = conductance;
    return 2;
  }
  if (e->kind == CIRCUIT_BJT) {
    const CircuitBjtModel *m = &e->model.bjt;
    double polarity = m->polarity == CIRCUIT_BJT_NPN ? 1.0 : -1.0;
    double vc = node_voltage(x, e->n1), vb = node_voltage(x, e->n2),
           ve = node_voltage(x, e->n3);
    double vbe = polarity * (vb - ve), vbc = polarity * (vb - vc),
           vce = polarity * (vc - ve);
    double ef = exp_clamped(vbe / m->thermal_voltage_v, exponent_limit);
    double er = exp_clamped(vbc / m->thermal_voltage_v, exponent_limit);
    double is = m->saturation_current_a;
    double ifwd = is * (ef - 1.0), irev = is * (er - 1.0);
    double alpha_f = m->beta_forward / (m->beta_forward + 1.0);
    double beta_r = m->beta_reverse > 0.0 ? m->beta_reverse : m->beta_forward;
    double alpha_r = beta_r / (beta_r + 1.0);
    double early_raw =
        m->early_voltage_v > 0.0 ? 1.0 + vce / m->early_voltage_v : 1.0;
    double early = fmax(0.0, early_raw);
    double a = is * ef / m->thermal_voltage_v;
    double b = is * er / m->thermal_voltage_v;
    /* Apply base-width modulation to the transported forward injection
       at both collector and emitter. This preserves the terminal-current
       sum instead of allowing the Early term to create negative base current.
     */
    double icn = alpha_f * ifwd * early - irev;
    double ien = -ifwd * early + alpha_r * irev;
    double collector_early_g = (m->early_voltage_v > 0.0 && early_raw > 0.0)
                                   ? alpha_f * ifwd / m->early_voltage_v
                                   : 0.0;
    double emitter_early_g = (m->early_voltage_v > 0.0 && early_raw > 0.0)
                                 ? ifwd / m->early_voltage_v
                                 : 0.0;
    double dic_dvc = b + collector_early_g;
    double dic_dvb = alpha_f * a * early - b;
    double dic_dve = -alpha_f * a * early - collector_early_g;
    double die_dvc = -alpha_r * b - emitter_early_g;
    double die_dvb = -a * early + alpha_r * b;
    double die_dve = a * early + emitter_early_g;
    double ic = polarity * icn, ie = polarity * ien, ib = -(ic + ie);
    currents[0] = ic;
    currents[1] = ib;
    currents[2] = ie;
    jac[0][0] = dic_dvc;
    jac[0][1] = dic_dvb;
    jac[0][2] = dic_dve;
    jac[2][0] = die_dvc;
    jac[2][1] = die_dvb;
    jac[2][2] = die_dve;
    for (size_t k = 0; k < 3; ++k)
      jac[1][k] = -(jac[0][k] + jac[2][k]);
    return 3;
  }
  if (e->kind == CIRCUIT_MOSFET || e->kind == CIRCUIT_JFET) {
    double polarity, beta, threshold, lambda, gamma = 0.0, phi = 0.6;
    if (e->kind == CIRCUIT_MOSFET) {
      const CircuitMosModel *m = &e->model.mos;
      polarity = m->polarity == CIRCUIT_MOS_N ? 1.0 : -1.0;
      beta = m->kp_a_per_v2;
      threshold = polarity * m->threshold_voltage_v;
      lambda = m->channel_modulation_per_v;
      gamma = m->body_effect_gamma;
      phi = m->surface_potential_v > 0.0 ? m->surface_potential_v : 0.6;
    } else {
      const CircuitJfetModel *m = &e->model.jfet;
      polarity = m->polarity == CIRCUIT_JFET_N ? 1.0 : -1.0;
      double vp = fabs(m->pinch_off_voltage_v);
      beta = 2.0 * m->idss_a / (vp * vp);
      threshold = vp;
      lambda = m->channel_modulation_per_v;
    }
    double vd = node_voltage(x, e->n1), vg = node_voltage(x, e->n2);
    double vs = node_voltage(x, e->n3), vb = node_voltage(x, e->n4);
    bool reverse = polarity * (vd - vs) < 0.0;
    double vsp = reverse ? vd : vs;
    double vgs = polarity * (vg - vsp);
    double vds = fabs(polarity * (vd - vs));
    double body_slope = 0.0;
    if (e->kind == CIRCUIT_MOSFET && gamma > 0.0) {
      double vbs = polarity * (vsp - vb);
      double root_arg = phi + vbs;
      if (root_arg < DBL_MIN)
        root_arg = DBL_MIN;
      double root = sqrt(root_arg);
      threshold += gamma * (root - sqrt(phi));
      body_slope = gamma / (2.0 * root);
    }
    double overdrive =
        e->kind == CIRCUIT_JFET ? vgs + threshold : vgs - threshold;
    double f = 0.0, dvod = 0.0, dvds = 0.0;
    if (overdrive > 0.0) {
      if (vds < overdrive) {
        double base = beta * (overdrive * vds - 0.5 * vds * vds);
        f = base * (1.0 + lambda * vds);
        dvod = beta * vds * (1.0 + lambda * vds);
        dvds = beta * ((overdrive - vds) * (1.0 + lambda * vds) +
                       lambda * (overdrive * vds - 0.5 * vds * vds));
      } else {
        f = 0.5 * beta * overdrive * overdrive * (1.0 + lambda * vds);
        dvod = beta * overdrive * (1.0 + lambda * vds);
        dvds = 0.5 * beta * overdrive * overdrive * lambda;
      }
    }
    double direction = reverse ? -polarity : polarity;
    double did_gate = direction * dvod * polarity;
    double did_dprime = direction * dvds * polarity;
    double did_sprime = direction * (-polarity * dvod - polarity * dvds -
                                     polarity * dvod * body_slope);
    double did_body = direction * dvod * body_slope * polarity;
    double did_d = reverse ? did_sprime : did_dprime;
    double did_s = reverse ? did_dprime : did_sprime;
    double id = direction * f;
    currents[0] = id;
    currents[2] = -id;
    jac[0][0] = did_d;
    jac[0][1] = did_gate;
    jac[0][2] = did_s;
    jac[2][0] = -did_d;
    jac[2][1] = -did_gate;
    jac[2][2] = -did_s;
    if (e->kind == CIRCUIT_MOSFET) {
      jac[0][3] = did_body;
      jac[2][3] = -did_body;
      return 4;
    }
    return 3;
  }
  return 0;
}

typedef struct {
  uint32_t positive_node;
  uint32_t negative_node;
  double capacitance_f;
} MnaDeviceCap;

static double junction_depletion_cap(double c0, double vj, double grading,
                                     double voltage) {
  if (!(c0 > 0.0))
    return 0.0;
  if (!(vj > 0.0) || grading < 0.0 || grading >= 1.0)
    return 0.0;
  const double fc = 0.5;
  double ratio = 1.0 - voltage / vj;
  if (voltage <= fc * vj) {
    if (ratio < DBL_MIN)
      ratio = DBL_MIN;
    return c0 * pow(ratio, -grading);
  }
  double at_fc = c0 * pow(1.0 - fc, -grading);
  double slope = grading * at_fc / (vj * (1.0 - fc));
  return at_fc + slope * (voltage - fc * vj);
}

static size_t device_ac_capacitances(const CircuitElement *e, const double *x,
                                     const double currents[4],
                                     double jac[4][4], MnaDeviceCap caps[4]) {
  size_t count = 0;
  if (e->kind == CIRCUIT_DIODE) {
    const CircuitDiodeModel *m = &e->model.diode;
    double v = node_voltage(x, e->n1) - node_voltage(x, e->n2);
    double vt = m->emission_coefficient * m->thermal_voltage_v;
    double gd = fmax(0.0, jac[0][0]);
    double depletion = junction_depletion_cap(
        m->junction_capacitance_f, m->junction_potential_v,
        m->grading_coefficient, v);
    double diffusion = m->transit_time_s * gd;
    double total = depletion + diffusion;
    if (total > 0.0 && isfinite(total))
      caps[count++] = (MnaDeviceCap){e->n1, e->n2, total};
    (void)vt;
  } else if (e->kind == CIRCUIT_BJT) {
    const CircuitBjtModel *m = &e->model.bjt;
    double pol = m->polarity == CIRCUIT_BJT_NPN ? 1.0 : -1.0;
    double vc = node_voltage(x, e->n1);
    double vb = node_voltage(x, e->n2);
    double ve = node_voltage(x, e->n3);
    double cbe = junction_depletion_cap(m->cje_f, m->vje_v, m->mje,
                                        pol * (vb - ve));
    double cbc = junction_depletion_cap(m->cjc_f, m->vjc_v, m->mjc,
                                        pol * (vb - vc));
    /* TF is the forward transit time. Its first-order small-signal diffusion
     * contribution is TF times the collector-current transconductance. */
    cbe += m->tf_s * fabs(jac[0][1]);
    if (cbe > 0.0 && isfinite(cbe))
      caps[count++] = (MnaDeviceCap){e->n2, e->n3, cbe};
    if (cbc > 0.0 && isfinite(cbc))
      caps[count++] = (MnaDeviceCap){e->n2, e->n1, cbc};
    (void)currents;
  } else if (e->kind == CIRCUIT_MOSFET) {
    const CircuitMosModel *m = &e->model.mos;
    double pol = m->polarity == CIRCUIT_MOS_N ? 1.0 : -1.0;
    double vd = node_voltage(x, e->n1);
    double vg = node_voltage(x, e->n2);
    double vs = node_voltage(x, e->n3);
    double vb = node_voltage(x, e->n4);
    double vds_signed = pol * (vd - vs);
    double reverse = vds_signed < 0.0 ? 1.0 : 0.0;
    double source = reverse ? vd : vs;
    double vgs = pol * (vg - source);
    double vds = fabs(vds_signed);
    double threshold = m->threshold_voltage_v;
    if (m->body_effect_gamma > 0.0) {
      double vbs = pol * (source - vb);
      double arg = fmax(DBL_MIN, m->surface_potential_v + vbs);
      threshold += m->body_effect_gamma *
                   (sqrt(arg) - sqrt(m->surface_potential_v));
    }
    double overdrive = vgs - threshold;
    double cox = m->gate_oxide_capacitance_f;
    double cgs_channel = 0.0, cgd_channel = 0.0, cgb_channel = cox;
    if (overdrive > 0.0) {
      cgb_channel = 0.0;
      if (vds < overdrive) {
        cgs_channel = 0.5 * cox;
        cgd_channel = 0.5 * cox;
      } else {
        cgs_channel = (2.0 / 3.0) * cox;
      }
    }
    double cgs = cgs_channel + m->gate_overlap_gs_f;
    double cgd = cgd_channel + m->gate_overlap_gd_f;
    double cgb = cgb_channel + m->gate_overlap_gb_f;
    if (cgs > 0.0 && isfinite(cgs))
      caps[count++] = (MnaDeviceCap){e->n2, e->n3, cgs};
    if (cgd > 0.0 && isfinite(cgd))
      caps[count++] = (MnaDeviceCap){e->n2, e->n1, cgd};
    if (cgb > 0.0 && isfinite(cgb))
      caps[count++] = (MnaDeviceCap){e->n2, e->n4, cgb};
  }
  return count;
}

static Cplx device_first_terminal_ac_current(const CircuitElement *e,
                                             const double *op_x,
                                             const Cplx *ac_x, double omega) {
  uint32_t nodes[4] = {e->n1, e->n2, e->n3, e->n4};
  double currents[4], jac[4][4];
  size_t terminal_count = device_eval(e, op_x, currents, jac, 40.0);
  Cplx value = cplx(0.0, 0.0);
  for (size_t k = 0; k < terminal_count; ++k)
    value = cplx_add(value,
                     cplx_scale(result_node_voltage_c(ac_x, nodes[k]), jac[0][k]));
  MnaDeviceCap caps[4];
  size_t cap_count = device_ac_capacitances(e, op_x, currents, jac, caps);
  for (size_t k = 0; k < cap_count; ++k) {
    Cplx dv = cplx_sub(result_node_voltage_c(ac_x, caps[k].positive_node),
                       result_node_voltage_c(ac_x, caps[k].negative_node));
    Cplx branch_current = cplx_mul(cplx(0.0, omega * caps[k].capacitance_f), dv);
    if (caps[k].positive_node == e->n1)
      value = cplx_add(value, branch_current);
    else if (caps[k].negative_node == e->n1)
      value = cplx_sub(value, branch_current);
  }
  return value;
}

#ifdef COPPER_TESTING
size_t mna_test_device_eval(const CircuitElement *element,
                            const double *node_solution,
                            double terminal_currents[4],
                            double terminal_jacobian[4][4],
                            double exponent_limit)
{
    if (!element || !node_solution || !terminal_currents ||
        !terminal_jacobian)
        return 0;
    return device_eval(element, node_solution, terminal_currents,
                       terminal_jacobian, exponent_limit);
}
#endif

static bool add_device_real(const CircuitElement *e, double *g, double *b,
                            size_t n, const double *x, double exponent_limit) {
  uint32_t nodes[4] = {e->n1, e->n2, e->n3, e->n4};
  double currents[4], jac[4][4];
  size_t count = device_eval(e, x, currents, jac, exponent_limit);
  if (count == 0u)
    return false;
  stamp_terminal_jacobian(g, b, n, nodes, count, currents, jac, x);
  return true;
}

static bool assemble_real(const Circuit *c, const MnaLayout *layout,
                          bool transient, double dt, const double *previous,
                          const double *x, double time_s, double source_scale,
                          double gmin_s, MnaIntegrationMethod method,
                          const double *previous2,
                          const double *capacitor_current, double previous_dt,
                          double *g, double *b, double exponent_limit) {
  size_t n = layout->n;
  memset(g, 0, sizeof(double) * n * n);
  memset(b, 0, sizeof(double) * n);
  for (size_t i = 0; i < c->element_count; ++i) {
    const CircuitElement *e = &c->elements[i];
    size_t branch = layout->branch_index[i];
    switch (e->kind) {
    case CIRCUIT_RESISTOR:
      stamp_g(g, n, e->n1, e->n2, 1.0 / e->value_si);
      break;
    case CIRCUIT_CAPACITOR:
      if (transient && e->value_si > 0.0) {
        double vprev =
            node_voltage(previous, e->n1) - node_voltage(previous, e->n2);
        double y = 0.0, history = 0.0;
        if (method == MNA_INTEGRATION_TRAPEZOIDAL) {
          y = 2.0 * e->value_si / dt;
          history =
              y * vprev + (capacitor_current ? capacitor_current[i] : 0.0);
        } else if (method == MNA_INTEGRATION_GEAR2 && previous2 &&
                   previous_dt > 0.0) {
          double ratio = dt / previous_dt;
          double a0 = (1.0 + 2.0 * ratio) / (1.0 + ratio);
          double a1 = 1.0 + ratio;
          double a2 = ratio * ratio / (1.0 + ratio);
          double vprev2 =
              node_voltage(previous2, e->n1) - node_voltage(previous2, e->n2);
          y = a0 * e->value_si / dt;
          history = (e->value_si / dt) * (a1 * vprev - a2 * vprev2);
        } else {
          y = e->value_si / dt;
          history = y * vprev;
        }
        stamp_g(g, n, e->n1, e->n2, y);
        size_t p = node_index(e->n1), q = node_index(e->n2);
        if (p != SIZE_MAX)
          b[p] += history;
        if (q != SIZE_MAX)
          b[q] -= history;
      }
      break;
    case CIRCUIT_INDUCTOR:
      if (branch == SIZE_MAX)
        return false;
      if (transient) {
        double iprev = previous[branch], req = 0.0, rhs = 0.0;
        double vprev =
            node_voltage(previous, e->n1) - node_voltage(previous, e->n2);
        if (method == MNA_INTEGRATION_TRAPEZOIDAL) {
          req = 2.0 * e->value_si / dt;
          rhs = -req * iprev - vprev;
        } else if (method == MNA_INTEGRATION_GEAR2 && previous2 &&
                   previous_dt > 0.0) {
          double ratio = dt / previous_dt;
          double a0 = (1.0 + 2.0 * ratio) / (1.0 + ratio);
          double a1 = 1.0 + ratio;
          double a2 = ratio * ratio / (1.0 + ratio);
          req = a0 * e->value_si / dt;
          rhs = -(e->value_si / dt) * (a1 * iprev - a2 * previous2[branch]);
        } else {
          req = e->value_si / dt;
          rhs = -req * iprev;
        }
        stamp_branch(g, b, n, e->n1, e->n2, branch, -req, rhs);
      } else {
        stamp_branch(g, b, n, e->n1, e->n2, branch, 0.0, 0.0);
      }
      break;
    case CIRCUIT_VOLTAGE_SOURCE:
      stamp_branch(g, b, n, e->n1, e->n2, branch, 0.0,
                   source_scale * (transient ? circuit_source_value(e, time_s)
                                             : circuit_source_dc_value(e)));
      break;
    case CIRCUIT_CURRENT_SOURCE: {
      size_t p = node_index(e->n1), q = node_index(e->n2);
      double value = source_scale * (transient ? circuit_source_value(e, time_s)
                                               : circuit_source_dc_value(e));
      if (p != SIZE_MAX)
        b[p] -= value;
      if (q != SIZE_MAX)
        b[q] += value;
      break;
    }
    case CIRCUIT_VCVS:
      stamp_vcvs_real(g, b, n, e->n1, e->n2, branch, e->n3, e->n4, e->value_si);
      break;
    case CIRCUIT_VCCS:
      stamp_vccs_real(g, n, e->n1, e->n2, e->n3, e->n4, e->value_si);
      break;
    case CIRCUIT_CCCS:
      if (layout->branch_index[e->control_element_index] == SIZE_MAX)
        return false;
      stamp_cccs_real(g, n, e->n1, e->n2,
                      layout->branch_index[e->control_element_index],
                      e->value_si);
      break;
    case CIRCUIT_CCVS: {
      size_t cb = layout->branch_index[e->control_element_index];
      if (cb == SIZE_MAX)
        return false;
      stamp_branch(g, b, n, e->n1, e->n2, branch, 0.0, 0.0);
      g[branch * n + cb] -= e->value_si;
      break;
    }
    case CIRCUIT_DIODE:
    case CIRCUIT_BJT:
    case CIRCUIT_MOSFET:
    case CIRCUIT_JFET:
      if (!add_device_real(e, g, b, n, x, exponent_limit))
        return false;
      break;
    default:
      return false;
    }
  }
  if (gmin_s > 0.0) {
    for (size_t i = 0; i < c->node_count; ++i)
      g[i * n + i] += gmin_s;
  }
  return true;
}

static void max_differences(const Circuit *c, const MnaLayout *layout,
                            const double *a, const double *b,
                            double *voltage_delta, double *current_delta,
                            double *voltage_scale, double *current_scale) {
  *voltage_delta = 0.0;
  *current_delta = 0.0;
  *voltage_scale = 0.0;
  *current_scale = 0.0;
  for (uint32_t i = 0; i < c->node_count; ++i) {
    double av = fabs(a[i]), bv = fabs(b[i]);
    double d = fabs(a[i] - b[i]);
    if (d > *voltage_delta)
      *voltage_delta = d;
    if (av > *voltage_scale)
      *voltage_scale = av;
    if (bv > *voltage_scale)
      *voltage_scale = bv;
  }
  for (size_t i = c->node_count; i < layout->n; ++i) {
    double av = fabs(a[i]), bv = fabs(b[i]);
    double d = fabs(a[i] - b[i]);
    if (d > *current_delta)
      *current_delta = d;
    if (av > *current_scale)
      *current_scale = av;
    if (bv > *current_scale)
      *current_scale = bv;
  }
}

/* Limit a PN-junction voltage before evaluating exponential device laws.
 * Vcrit follows the usual SPICE relation. The negative-side rule prevents
 * large reverse-voltage jumps from destabilising the next Jacobian. */
static double pnjlim_critical_voltage(double vt, double saturation_current)
{
  if (!(vt > 0.0) || !(saturation_current > 0.0) ||
      !isfinite(vt) || !isfinite(saturation_current))
    return 0.0;

  double logarithm = log(vt) - log(sqrt(2.0)) - log(saturation_current);
  if (!isfinite(logarithm) || logarithm <= 0.0)
    return 0.0;
  return vt * logarithm;
}

static double pnjlim_voltage(double proposed, double previous,
                             double thermal_voltage,
                             double saturation_current)
{
  if (!(thermal_voltage > 0.0) || !isfinite(proposed) ||
      !isfinite(previous))
    return proposed;

  double vcrit = pnjlim_critical_voltage(thermal_voltage,
                                         saturation_current);
  if (proposed > vcrit && fabs(proposed - previous) > 2.0 * thermal_voltage) {
    if (previous > 0.0) {
      double argument = 1.0 + (proposed - previous) / thermal_voltage;
      proposed = argument > 0.0
                     ? previous + thermal_voltage * log(argument)
                     : vcrit;
    } else if (proposed > 0.0) {
      proposed = thermal_voltage * log(proposed / thermal_voltage);
    }
  } else if (proposed < 0.0) {
    double reverse_floor = previous > 0.0 ? -previous - 1.0
                                          : 2.0 * previous - 1.0;
    if (proposed < reverse_floor)
      proposed = reverse_floor;
  }
  return proposed;
}

/* SPICE FET limiter. The threshold remains signed in the polarity-normalised
 * gate-source coordinate; this is important for depletion-mode devices. */
static double fetlim_voltage(double proposed, double previous,
                             double threshold)
{
  double upper_step = fabs(2.0 * (previous - threshold)) + 2.0;
  double lower_step = fabs(previous - threshold) + 1.0;
  double threshold_high = threshold + 3.5;
  double delta = proposed - previous;

  if (previous >= threshold) {
    if (previous >= threshold_high) {
      if (delta <= 0.0) {
        if (proposed >= threshold_high) {
          if (-delta > lower_step)
            proposed = previous - lower_step;
        } else if (proposed < threshold_high) {
          proposed = fmax(proposed, threshold + 2.0);
        }
      } else if (delta >= upper_step) {
        proposed = previous + upper_step;
      }
    } else {
      proposed = delta <= 0.0 ? fmax(proposed, threshold - 0.5)
                              : fmin(proposed, threshold + 4.0);
    }
  } else if (delta <= 0.0) {
    if (-delta > upper_step)
      proposed = previous - upper_step;
  } else {
    double turn_on = threshold + 0.5;
    if (proposed <= turn_on) {
      if (delta > lower_step)
        proposed = previous + lower_step;
    } else {
      proposed = turn_on;
    }
  }
  return proposed;
}

static double limit_scalar_step(double proposed, double previous,
                                double maximum_step)
{
  if (maximum_step > 0.0 && fabs(proposed - previous) > maximum_step)
    return previous + copysign(maximum_step, proposed - previous);
  return proposed;
}

static void set_pair_voltage(double *solution, uint32_t positive,
                             uint32_t negative, double voltage)
{
  if (positive == 0u && negative != 0u)
    solution[(size_t)negative - 1u] = -voltage;
  else if (negative == 0u && positive != 0u)
    solution[(size_t)positive - 1u] = voltage;
  else if (positive != 0u && negative != 0u)
    solution[(size_t)positive - 1u] =
        node_voltage(solution, negative) + voltage;
}

static void limit_nonlinear_steps(const Circuit *c, double *next,
                                  const double *current,
                                  double maximum_step)
{
  for (size_t index = 0; index < c->element_count; ++index) {
    const CircuitElement *element = &c->elements[index];

    if (element->kind == CIRCUIT_DIODE) {
      const CircuitDiodeModel *model = &element->model.diode;
      double old_voltage = node_voltage(current, element->n1) -
                           node_voltage(current, element->n2);
      double new_voltage = node_voltage(next, element->n1) -
                           node_voltage(next, element->n2);
      double vt = model->emission_coefficient * model->thermal_voltage_v;
      double limited = pnjlim_voltage(new_voltage, old_voltage, vt,
                                      model->saturation_current_a);
      limited = limit_scalar_step(limited, old_voltage, maximum_step);
      set_pair_voltage(next, element->n1, element->n2, limited);
    } else if (element->kind == CIRCUIT_BJT) {
      const CircuitBjtModel *model = &element->model.bjt;
      double polarity = model->polarity == CIRCUIT_BJT_NPN ? 1.0 : -1.0;
      double old_base = node_voltage(current, element->n2);
      double new_base = node_voltage(next, element->n2);
      double old_emitter = node_voltage(current, element->n3);
      double old_collector = node_voltage(current, element->n1);
      double new_emitter = node_voltage(next, element->n3);
      double new_collector = node_voltage(next, element->n1);
      double old_be = polarity * (old_base - old_emitter);
      double old_bc = polarity * (old_base - old_collector);
      double new_be = polarity * (new_base - new_emitter);
      double new_bc = polarity * (new_base - new_collector);
      double limited_be = pnjlim_voltage(new_be, old_be,
                                         model->thermal_voltage_v,
                                         model->saturation_current_a);
      double limited_bc = pnjlim_voltage(new_bc, old_bc,
                                         model->thermal_voltage_v,
                                         model->saturation_current_a);
      double base_from_be = new_emitter + polarity * limited_be;
      double base_from_bc = new_collector + polarity * limited_bc;
      double correction_be = fabs(base_from_be - new_base);
      double correction_bc = fabs(base_from_bc - new_base);
      double limited_base = correction_be >= correction_bc
                                ? base_from_be : base_from_bc;
      limited_base = limit_scalar_step(limited_base, old_base, maximum_step);
      if (element->n2 != 0u)
        next[(size_t)element->n2 - 1u] = limited_base;
    } else if (element->kind == CIRCUIT_MOSFET) {
      const CircuitMosModel *model = &element->model.mos;
      double polarity = model->polarity == CIRCUIT_MOS_N ? 1.0 : -1.0;
      double old_vgs = polarity * (node_voltage(current, element->n2) -
                                   node_voltage(current, element->n3));
      double new_vgs = polarity * (node_voltage(next, element->n2) -
                                   node_voltage(next, element->n3));
      /* VTO is polarity-normalised, not absolutised: negative VTO must remain
       * negative for depletion-mode NMOS/PMOS devices. */
      double vto = polarity * model->threshold_voltage_v;
      double limited_vgs = fetlim_voltage(new_vgs, old_vgs, vto);
      limited_vgs = limit_scalar_step(limited_vgs, old_vgs, maximum_step);
      double limited_gate = node_voltage(next, element->n3) +
                            polarity * limited_vgs;
      if (element->n2 != 0u)
        next[(size_t)element->n2 - 1u] = limited_gate;
    } else if (element->kind == CIRCUIT_JFET) {
      const CircuitJfetModel *model = &element->model.jfet;
      double polarity = model->polarity == CIRCUIT_JFET_N ? 1.0 : -1.0;
      double old_vgs = polarity * (node_voltage(current, element->n2) -
                                   node_voltage(current, element->n3));
      double new_vgs = polarity * (node_voltage(next, element->n2) -
                                   node_voltage(next, element->n3));
      double threshold = -fabs(model->pinch_off_voltage_v);
      double limited_vgs = fetlim_voltage(new_vgs, old_vgs, threshold);
      limited_vgs = limit_scalar_step(limited_vgs, old_vgs, maximum_step);
      double limited_gate = node_voltage(next, element->n3) +
                            polarity * limited_vgs;
      if (element->n2 != 0u)
        next[(size_t)element->n2 - 1u] = limited_gate;
    }
  }
}

#ifdef COPPER_TESTING
double mna_test_pnjlim(double proposed, double previous, double vt, double is)
{
  return pnjlim_voltage(proposed, previous, vt, is);
}

double mna_test_fetlim(double proposed, double previous, double threshold)
{
  return fetlim_voltage(proposed, previous, threshold);
}
#endif

static double element_current_dc(const CircuitElement *e, const double *x,
                                 const MnaLayout *layout, size_t element_index,
                                 double exponent_limit) {
  double vp = node_voltage(x, e->n1), vn = node_voltage(x, e->n2);
  size_t branch = layout->branch_index[element_index];
  switch (e->kind) {
  case CIRCUIT_RESISTOR:
    return (vp - vn) / e->value_si;
  case CIRCUIT_CAPACITOR:
    return 0.0;
  case CIRCUIT_INDUCTOR:
  case CIRCUIT_VOLTAGE_SOURCE:
  case CIRCUIT_VCVS:
  case CIRCUIT_CCVS:
    return branch == SIZE_MAX ? 0.0 : x[branch];
  case CIRCUIT_CURRENT_SOURCE:
    return e->value_si;
  case CIRCUIT_VCCS:
    return e->value_si * (node_voltage(x, e->n3) - node_voltage(x, e->n4));
  case CIRCUIT_CCCS: {
    size_t cb = layout->branch_index[e->control_element_index];
    return cb == SIZE_MAX ? 0.0 : e->value_si * x[cb];
  }
  case CIRCUIT_DIODE:
  case CIRCUIT_BJT:
  case CIRCUIT_MOSFET:
  case CIRCUIT_JFET: {
    double currents[4], jac[4][4];
    if (device_eval(e, x, currents, jac, exponent_limit) == 0u)
      return NAN;
    return currents[0];
  }
  default:
    return 0.0;
  }
}

MnaOptions mna_default_options(void) {
  MnaOptions o;
  memset(&o, 0, sizeof(o));
  o.voltage_absolute_tolerance_v = 1e-9;
  o.current_absolute_tolerance_a = 1e-12;
  o.relative_tolerance = 1e-8;
  o.max_iterations = 120;
  o.max_junction_step_v = 0.2;
  o.device_exponent_limit = 40.0;
  o.residual_tolerance = 1.0;
  o.gmin_start_s = 1e-3;
  o.gmin_end_s = 0.0;
  o.gmin_steps = 10;
  o.source_stepping_steps = 8;
  o.line_search_steps = 12;
  return o;
}

MnaTransientOptions mna_default_transient_options(void) {
  MnaTransientOptions o;
  memset(&o, 0, sizeof(o));
  o.time_step_s = 1e-6;
  o.stop_time_s = 1e-3;
  o.max_steps = 10000;
  o.nonlinear = mna_default_options();
  o.integration_method = MNA_INTEGRATION_BACKWARD_EULER;
  o.use_initial_operating_point = false;
  return o;
}

const char *mna_status_name(MnaStatus status) {
  static const char *const names[] = {
      "OK",           "ARGUMENT_ERROR",  "INVALID_CIRCUIT",
      "MEMORY_ERROR", "SINGULAR_MATRIX", "NONLINEAR_NOT_CONVERGED",
      "RANGE_ERROR",  "UNSUPPORTED",     "NUMERIC_ERROR"};
  return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status]
                                                             : "UNKNOWN";
}

static double scaled_residual_norm(size_t n, size_t node_count, const double *g,
                                   const double *b, const double *x,
                                   double rel_tol, double i_abs_tol,
                                   double v_abs_tol) {
  double merit = 0.0;
  for (size_t r = 0; r < n; ++r) {
    double sum = 0.0, correction = 0.0, scale = fabs(b[r]);
    for (size_t c = 0; c < n; ++c) {
      double term = g[r * n + c] * x[c];
      double y = term - correction, t = sum + y;
      correction = (t - sum) - y;
      sum = t;
      scale += fabs(g[r * n + c]) * fabs(x[c]);
    }
    double residual = fabs(sum - b[r]);
    double abs_tol = r < node_count ? i_abs_tol : v_abs_tol;
    double tol = abs_tol + rel_tol * scale;
    if (!(tol > 0.0))
      tol = DBL_MIN;
    double q = residual / tol;
    if (!nm_isfinite(q))
      return DBL_MAX;
    if (q > merit)
      merit = q;
  }
  return merit;
}

static MnaStatus solve_one_newton_stage(
    const Circuit *c, const MnaOptions *o, const double *previous,
    bool transient, double dt, double time_s, double source_scale, double gmin,
    MnaIntegrationMethod method, const double *previous2,
    const double *capacitor_current, double previous_dt, MnaLayout *layout,
    double *solution, double *g, double *b, double *next, double *trial,
    double *trial_g, double *trial_b, uint32_t *iteration_count,
    double *max_delta, double *final_residual,
    ClaWorkspace *linear_workspace) {
  const size_t n = layout->n;
  const uint32_t max_iter = o->max_iterations ? o->max_iterations : 120;
  const double vabs = o->voltage_absolute_tolerance_v > 0.0
                          ? o->voltage_absolute_tolerance_v
                          : 1e-9;
  const double iabs = o->current_absolute_tolerance_a > 0.0
                          ? o->current_absolute_tolerance_a
                          : 1e-12;
  const double rtol =
      o->relative_tolerance > 0.0 ? o->relative_tolerance : 1e-8;
  const double residual_target =
      o->residual_tolerance > 0.0 ? o->residual_tolerance : 1.0;
  const double exponent =
      o->device_exponent_limit > 0.0 ? o->device_exponent_limit : 40.0;
  bool nonlinear = false;
  for (size_t i = 0; i < c->element_count; ++i) {
    CircuitElementKind k = c->elements[i].kind;
    if (k == CIRCUIT_DIODE || k == CIRCUIT_BJT || k == CIRCUIT_MOSFET ||
        k == CIRCUIT_JFET) {
      nonlinear = true;
      break;
    }
  }
  *final_residual = DBL_MAX;
  for (uint32_t it = 0; it < max_iter; ++it) {
    if (!assemble_real(c, layout, transient, dt, previous, solution, time_s,
                       source_scale, gmin, method, previous2, capacitor_current,
                       previous_dt, g, b, exponent)) {
      MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "assemble", MNA_ERR_UNSUPPORTED,
                      it, *final_residual, 0.0, *max_delta, gmin, source_scale,
                      time_s, dt, (int)method);
      return MNA_ERR_UNSUPPORTED;
    }
    double current_merit = scaled_residual_norm(n, c->node_count, g, b,
                                                solution, rtol, iabs, vabs);
    *final_residual = current_merit;
    memset(next, 0, sizeof(double) * n);
    ClaStats linear_stats;
    ClaStatus ls = cla_solve_real_with_workspace(
        n, g, b, next, NULL, &linear_stats, linear_workspace);
    if (ls == CLA_ERR_MEMORY) {
      MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "linear-solve", MNA_ERR_MEMORY,
                      it, current_merit, 0.0, *max_delta, gmin, source_scale,
                      time_s, dt, (int)method);
      return MNA_ERR_MEMORY;
    }
    if (ls == CLA_ERR_RANGE) {
      MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "linear-solve", MNA_ERR_RANGE,
                      it, current_merit, 0.0, *max_delta, gmin, source_scale,
                      time_s, dt, (int)method);
      return MNA_ERR_RANGE;
    }
    if (ls == CLA_ERR_SINGULAR) {
      MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "linear-solve", MNA_ERR_SINGULAR,
                      it, current_merit, 0.0, *max_delta, gmin, source_scale,
                      time_s, dt, (int)method);
      return MNA_ERR_SINGULAR;
    }
    if (ls != CLA_OK) {
      MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "linear-solve", MNA_ERR_NUMERIC,
                      it, current_merit, 0.0, *max_delta, gmin, source_scale,
                      time_s, dt, (int)method);
      return MNA_ERR_NUMERIC;
    }
    limit_nonlinear_steps(c, next, solution, o->max_junction_step_v);

    double alpha = 1.0;
    bool accepted = false;
    uint32_t searches =
        nonlinear ? (o->line_search_steps ? o->line_search_steps : 12) : 1;
    double accepted_merit = DBL_MAX;
    for (uint32_t ls_i = 0; ls_i < searches; ++ls_i) {
      for (size_t k = 0; k < n; ++k)
        trial[k] = solution[k] + alpha * (next[k] - solution[k]);
      if (!assemble_real(c, layout, transient, dt, previous, trial, time_s,
                         source_scale, gmin, method, previous2,
                         capacitor_current, previous_dt, trial_g, trial_b,
                         exponent)) {
        MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "line-search-assemble",
                        MNA_ERR_UNSUPPORTED, it, current_merit, alpha,
                        *max_delta, gmin, source_scale, time_s, dt,
                        (int)method);
        return MNA_ERR_UNSUPPORTED;
      }
      double merit = scaled_residual_norm(n, c->node_count, trial_g, trial_b,
                                          trial, rtol, iabs, vabs);
      if ((!nonlinear && merit <= residual_target) ||
          (nonlinear && (merit < current_merit * (1.0 - 1e-4 * alpha) ||
                         merit <= residual_target))) {
        accepted = true;
        accepted_merit = merit;
        break;
      }
      alpha *= 0.5;
    }
    if (!accepted) {
      MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "line-search-rejected",
                      MNA_ERR_NOT_CONVERGED, it, current_merit, alpha,
                      *max_delta, gmin, source_scale, time_s, dt,
                      (int)method);
      return MNA_ERR_NOT_CONVERGED;
    }

    double vd = 0.0, cd = 0.0, vs = 0.0, cs = 0.0;
    max_differences(c, layout, solution, trial, &vd, &cd, &vs, &cs);
    memcpy(solution, trial, sizeof(double) * n);
    *max_delta = vd;
    ++(*iteration_count);
    *final_residual = accepted_merit;
    MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "accepted", MNA_OK,
                    *iteration_count, accepted_merit, alpha, vd, gmin,
                    source_scale, time_s, dt, (int)method);
    if (!nonlinear || (vd <= vabs + rtol * vs && cd <= iabs + rtol * cs &&
                       accepted_merit <= residual_target))
      return MNA_OK;
  }
  MNA_TRACE_EVENT(COPPER_TRACE_NEWTON_ITERATION, "iteration-limit",
                  MNA_ERR_NOT_CONVERGED, *iteration_count, *final_residual,
                  0.0, *max_delta, gmin, source_scale, time_s, dt,
                  (int)method);
  return MNA_ERR_NOT_CONVERGED;
}

static MnaStatus
dc_solve_internal(const Circuit *c, const MnaOptions *options,
                  const double *previous, bool transient, double dt,
                  double time_s, MnaIntegrationMethod method,
                  const double *previous2, const double *capacitor_current,
                  double previous_dt, MnaLayout *layout, double *solution,
                  uint32_t *iterations, double *max_delta, double *residual_out,
                  double *gmin_out, Arena *scratch,
                  ClaWorkspace *linear_workspace) {
  size_t n = layout->n, matrix_count;
  if (!size_multiply(n, n, &matrix_count) ||
      matrix_count > SIZE_MAX / sizeof(double))
    return MNA_ERR_RANGE;

  if (!scratch || !linear_workspace)
    return MNA_ERR_ARGUMENT;
  double *g = (double *)arena_alloc_array(scratch, sizeof(double), matrix_count);
  double *b = (double *)arena_alloc_array(scratch, sizeof(double), n);
  double *next = (double *)arena_alloc_array(scratch, sizeof(double), n);
  double *trial = (double *)arena_alloc_array(scratch, sizeof(double), n);
  double *trial_g = (double *)arena_alloc_array(scratch, sizeof(double), matrix_count);
  double *trial_b = (double *)arena_alloc_array(scratch, sizeof(double), n);
  if (!g || !b || !next || !trial || !trial_g || !trial_b)
    return MNA_ERR_MEMORY;
  memset(g, 0, matrix_count * sizeof(double));
  memset(b, 0, n * sizeof(double));
  memset(next, 0, n * sizeof(double));
  memset(trial, 0, n * sizeof(double));
  memset(trial_g, 0, matrix_count * sizeof(double));
  memset(trial_b, 0, n * sizeof(double));
  *iterations = 0;
  *max_delta = 0.0;
  *residual_out = DBL_MAX;
  *gmin_out = 0.0;
  MnaStatus status = MNA_ERR_NOT_CONVERGED;
  if (transient) {
    status = solve_one_newton_stage(
        c, options, previous, true, dt, time_s, 1.0, 0.0, method, previous2,
        capacitor_current, previous_dt, layout, solution, g, b, next, trial,
        trial_g, trial_b, iterations, max_delta, residual_out,
        linear_workspace);
  } else {
    bool nonlinear = false;
    for (size_t i = 0; i < c->element_count; ++i) {
      CircuitElementKind k = c->elements[i].kind;
      if (k == CIRCUIT_DIODE || k == CIRCUIT_BJT || k == CIRCUIT_MOSFET ||
          k == CIRCUIT_JFET) {
        nonlinear = true;
        break;
      }
    }
    double gstart = nonlinear ? options->gmin_start_s : options->gmin_end_s;
    double gfinal = options->gmin_end_s;
    if (gstart < 0.0)
      gstart = 0.0;
    if (gfinal < 0.0)
      gfinal = 0.0;
    uint32_t ss = nonlinear ? options->source_stepping_steps : 1u;
    if (ss == 0)
      ss = 1;
    uint32_t gs = nonlinear ? options->gmin_steps : 1u;
    if (gs == 0)
      gs = 1;
    double g_first = gstart;
    for (uint32_t i = 1; i <= ss; ++i) {
      double scale = (double)i / (double)ss;
      status = solve_one_newton_stage(
          c, options, NULL, false, 0.0, time_s, scale, g_first, method, NULL,
          NULL, previous_dt, layout, solution, g, b, next, trial, trial_g,
          trial_b, iterations, max_delta, residual_out, linear_workspace);
      if (status != MNA_OK)
        break;
    }
    if (status == MNA_OK && nonlinear) {
      for (uint32_t i = 1; i <= gs; ++i) {
        double fraction = (double)i / (double)gs;
        double gm = (gstart > 0.0 && gfinal > 0.0)
                        ? gstart * pow(gfinal / gstart, fraction)
                        : (gstart + (gfinal - gstart) * fraction);
        status = solve_one_newton_stage(
            c, options, NULL, false, 0.0, time_s, 1.0, gm, method, NULL, NULL,
            previous_dt, layout, solution, g, b, next, trial, trial_g, trial_b,
            iterations, max_delta, residual_out, linear_workspace);
        if (status != MNA_OK)
          break;
        *gmin_out = gm;
      }
    } else if (status == MNA_OK) {
      *gmin_out = gfinal;
    }
    if (status == MNA_OK && nonlinear) {
      /* Final unregularized physical equation: do not leave hidden GMIN in
       * results. */
      status = solve_one_newton_stage(
          c, options, NULL, false, 0.0, time_s, 1.0, 0.0, method, NULL, NULL,
          previous_dt, layout, solution, g, b, next, trial, trial_g, trial_b,
          iterations, max_delta, residual_out, linear_workspace);
      *gmin_out = gfinal;
    }
  }
  return status;
}

MnaStatus mna_solve_dc(Arena *result_arena, const Circuit *c,
                       const MnaOptions *options, MnaOperatingPoint *out) {
  Arena workspace;
  Arena newton_scratch;
  MnaLayout layout;
  ClaWorkspace linear_workspace;
  MnaOptions opt;
  double *x;
  uint32_t iterations = 0;
  double max_delta = 0.0;
  double residual = 0.0;
  double gmin_used = 0.0;
  MnaStatus status;

  if (!result_arena || !c || !out)
    return MNA_ERR_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (circuit_validate(c) != CIRCUIT_OK)
    return MNA_ERR_CIRCUIT;

  opt = options ? *options : mna_default_options();
  if (opt.max_iterations == 0)
    opt.max_iterations = 120;

  arena_init(&workspace, 16u * 1024u);
  arena_init(&newton_scratch, 16u * 1024u);
  if (!alloc_layout(c, true, &workspace, &layout)) {
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }
  if (layout.n == 0u) {
    status = MNA_ERR_RANGE;
    goto cleanup;
  }
  x = (double *)arena_alloc_array(&workspace, sizeof(double), layout.n);
  if (x == NULL) {
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }
  memset(x, 0, sizeof(double) * layout.n);
  if (cla_workspace_init_real(&workspace, layout.n, &linear_workspace) !=
      CLA_OK) {
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }

  if (opt.initial_node_voltages_v) {
    if (opt.initial_node_voltage_count != (size_t)c->node_count + 1u) {
      status = MNA_ERR_ARGUMENT;
      goto cleanup;
    }
    for (uint32_t node = 1; node <= c->node_count; ++node) {
      if (!opt.initial_node_mask || opt.initial_node_mask[node]) {
        double seed = opt.initial_node_voltages_v[node];
        if (!isfinite(seed)) {
          status = MNA_ERR_ARGUMENT;
          goto cleanup;
        }
        x[node - 1u] = seed;
      }
    }
  }

  status = dc_solve_internal(
      c, &opt, NULL, false, 0.0, 0.0, MNA_INTEGRATION_BACKWARD_EULER, NULL,
      NULL, 0.0, &layout, x, &iterations, &max_delta, &residual, &gmin_used,
      &newton_scratch, &linear_workspace);
  if (status != MNA_OK)
    goto cleanup;

  out->node_voltage_count = (size_t)c->node_count + 1u;
  out->element_current_count = c->element_count;
  out->node_voltages_v = (double *)arena_alloc_array(
      result_arena, sizeof(double), out->node_voltage_count);
  out->element_currents_a = (double *)arena_alloc_array(
      result_arena, sizeof(double), c->element_count);
  if (!out->node_voltages_v ||
      (c->element_count != 0u && !out->element_currents_a)) {
    memset(out, 0, sizeof(*out));
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }

  out->node_voltages_v[0] = 0.0;
  for (uint32_t node = 1; node <= c->node_count; ++node)
    out->node_voltages_v[node] = x[node - 1u];
  for (size_t index = 0; index < c->element_count; ++index) {
    out->element_currents_a[index] = element_current_dc(
        &c->elements[index], x, &layout, index,
        opt.device_exponent_limit > 0.0 ? opt.device_exponent_limit : 40.0);
  }
  out->iterations = iterations;
  out->max_delta_v = max_delta;
  out->converged = true;
  out->scaled_residual = residual;
  out->gmin_used_s = gmin_used;
  status = MNA_OK;

cleanup:
  arena_destroy(&newton_scratch);
  arena_destroy(&workspace);
  return status;
}

static void stamp_vcvs_complex(Cplx *g, Cplx *b, size_t n, uint32_t p,
                               uint32_t q, size_t branch, uint32_t cp,
                               uint32_t cq, double gain) {
  stamp_branch_complex(g, b, n, p, q, branch, cplx(0.0, 0.0));
  size_t cpi = node_index(cp), cqi = node_index(cq);
  if (cpi != SIZE_MAX)
    g[branch * n + cpi] = cplx_sub(g[branch * n + cpi], cplx(gain, 0.0));
  if (cqi != SIZE_MAX)
    g[branch * n + cqi] = cplx_add(g[branch * n + cqi], cplx(gain, 0.0));
}

static void stamp_vccs_complex(Cplx *g, size_t n, uint32_t p, uint32_t q,
                               uint32_t cp, uint32_t cq, double gain) {
  size_t pi = node_index(p), qi = node_index(q), cpi = node_index(cp),
         cqi = node_index(cq);
  Cplx v = cplx(gain, 0.0);
  if (pi != SIZE_MAX && cpi != SIZE_MAX)
    g[pi * n + cpi] = cplx_add(g[pi * n + cpi], v);
  if (pi != SIZE_MAX && cqi != SIZE_MAX)
    g[pi * n + cqi] = cplx_sub(g[pi * n + cqi], v);
  if (qi != SIZE_MAX && cpi != SIZE_MAX)
    g[qi * n + cpi] = cplx_sub(g[qi * n + cpi], v);
  if (qi != SIZE_MAX && cqi != SIZE_MAX)
    g[qi * n + cqi] = cplx_add(g[qi * n + cqi], v);
}

static void stamp_cccs_complex(Cplx *g, size_t n, uint32_t p, uint32_t q,
                               size_t cb, double gain) {
  size_t pi = node_index(p), qi = node_index(q);
  Cplx v = cplx(gain, 0.0);
  if (pi != SIZE_MAX)
    g[pi * n + cb] = cplx_add(g[pi * n + cb], v);
  if (qi != SIZE_MAX)
    g[qi * n + cb] = cplx_sub(g[qi * n + cb], v);
}

static void stamp_current_complex(Cplx *b, uint32_t p, uint32_t q, Cplx value) {
  size_t pi = node_index(p), qi = node_index(q);
  if (pi != SIZE_MAX)
    b[pi] = cplx_sub(b[pi], value);
  if (qi != SIZE_MAX)
    b[qi] = cplx_add(b[qi], value);
}

MnaStatus mna_solve_ac(Arena *result_arena, const Circuit *c,
                       const MnaOperatingPoint *op, double frequency_hz,
                       MnaAcResult *out) {
  if (!result_arena || !c || !op || !out || !op->converged ||
      !op->node_voltages_v || !(frequency_hz > 0.0) || !isfinite(frequency_hz))
    return MNA_ERR_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (circuit_validate(c) != CIRCUIT_OK ||
      op->node_voltage_count != (size_t)c->node_count + 1)
    return MNA_ERR_CIRCUIT;

  Arena workspace;
  ClaWorkspace linear_workspace;
  MnaLayout layout;
  arena_init(&workspace, 16u * 1024u);
  if (!alloc_layout(c, true, &workspace, &layout)) {
    arena_destroy(&workspace);
    return MNA_ERR_MEMORY;
  }
  size_t n = layout.n, matrix_count;
  if (!size_multiply(n, n, &matrix_count) ||
      matrix_count > SIZE_MAX / sizeof(Cplx)) {
    arena_destroy(&workspace);
    return MNA_ERR_RANGE;
  }
  Cplx *g = (Cplx *)arena_alloc_array(&workspace, sizeof(Cplx), matrix_count);
  Cplx *b = (Cplx *)arena_alloc_array(&workspace, sizeof(Cplx), n);
  Cplx *x = (Cplx *)arena_alloc_array(&workspace, sizeof(Cplx), n);
  double *op_x = (double *)arena_alloc_array(&workspace, sizeof(double), n);
  if (!g || !b || !x || !op_x) {
    arena_destroy(&workspace);
    return MNA_ERR_MEMORY;
  }
  memset(g, 0, matrix_count * sizeof(Cplx));
  memset(b, 0, n * sizeof(Cplx));
  memset(x, 0, n * sizeof(Cplx));
  memset(op_x, 0, n * sizeof(double));
  if (cla_workspace_init_complex(&workspace, n, &linear_workspace) != CLA_OK) {
    arena_destroy(&workspace);
    return MNA_ERR_MEMORY;
  }
  for (uint32_t node = 1; node <= c->node_count; ++node)
    op_x[node - 1] = op->node_voltages_v[node];

  const double omega = 2.0 * 3.14159265358979323846 * frequency_hz;
  for (size_t i = 0; i < c->element_count; ++i) {
    const CircuitElement *e = &c->elements[i];
    switch (e->kind) {
    case CIRCUIT_RESISTOR:
      stamp_g_complex(g, n, e->n1, e->n2, cplx(1.0 / e->value_si, 0.0));
      break;
    case CIRCUIT_CAPACITOR:
      stamp_g_complex(g, n, e->n1, e->n2, cplx(0.0, omega * e->value_si));
      break;
    case CIRCUIT_INDUCTOR:
      if (e->value_si > 0.0) {
        size_t branch = layout.branch_index[i];
        stamp_branch_complex(g, b, n, e->n1, e->n2, branch, cplx(0.0, 0.0));
        g[branch * n + branch] = cplx(0.0, -omega * e->value_si);
      } else {
        arena_destroy(&workspace);
        return MNA_ERR_RANGE;
      }
      break;
    case CIRCUIT_VOLTAGE_SOURCE:
      stamp_branch_complex(g, b, n, e->n1, e->n2, layout.branch_index[i],
                           e->ac_value);
      break;
    case CIRCUIT_CURRENT_SOURCE:
      stamp_current_complex(b, e->n1, e->n2, e->ac_value);
      break;
    case CIRCUIT_VCVS:
      stamp_vcvs_complex(g, b, n, e->n1, e->n2, layout.branch_index[i], e->n3,
                         e->n4, e->value_si);
      break;
    case CIRCUIT_VCCS:
      stamp_vccs_complex(g, n, e->n1, e->n2, e->n3, e->n4, e->value_si);
      break;
    case CIRCUIT_CCCS:
      stamp_cccs_complex(g, n, e->n1, e->n2,
                         layout.branch_index[e->control_element_index],
                         e->value_si);
      break;
    case CIRCUIT_CCVS: {
      size_t cb = layout.branch_index[e->control_element_index];
      stamp_branch_complex(g, b, n, e->n1, e->n2, layout.branch_index[i],
                           cplx(0.0, 0.0));
      g[layout.branch_index[i] * n + cb] =
          cplx_sub(g[layout.branch_index[i] * n + cb], cplx(e->value_si, 0.0));
      break;
    }
    case CIRCUIT_DIODE:
    case CIRCUIT_BJT:
    case CIRCUIT_MOSFET:
    case CIRCUIT_JFET: {
      uint32_t nodes[4] = {e->n1, e->n2, e->n3, e->n4};
      double currents[4], jac[4][4];
      size_t count = device_eval(e, op_x, currents, jac, 40.0);
      if (count == 0u) {
        arena_destroy(&workspace);
        return MNA_ERR_NUMERIC;
      }
      stamp_terminal_jacobian_complex(g, n, nodes, count, jac);
      MnaDeviceCap caps[4];
      size_t cap_count = device_ac_capacitances(e, op_x, currents, jac, caps);
      for (size_t cap_index = 0; cap_index < cap_count; ++cap_index)
        stamp_g_complex(g, n, caps[cap_index].positive_node,
                        caps[cap_index].negative_node,
                        cplx(0.0, omega * caps[cap_index].capacitance_f));
      break;
    }
    default:
      arena_destroy(&workspace);
      return MNA_ERR_UNSUPPORTED;
    }
  }
  ClaStats linear_stats;
  ClaStatus linear_status = cla_solve_complex_with_workspace(
      n, g, b, x, NULL, &linear_stats, &linear_workspace);
  if (linear_status != CLA_OK) {
    arena_destroy(&workspace);
    if (linear_status == CLA_ERR_MEMORY)
      return MNA_ERR_MEMORY;
    if (linear_status == CLA_ERR_RANGE)
      return MNA_ERR_RANGE;
    if (linear_status == CLA_ERR_SINGULAR)
      return MNA_ERR_SINGULAR;
    return MNA_ERR_NUMERIC;
  }
  out->node_voltage_count = (size_t)c->node_count + 1;
  out->element_current_count = c->element_count;
  out->frequency_hz = frequency_hz;
  out->relative_residual = linear_stats.relative_residual;
  out->pivot_growth_estimate = linear_stats.pivot_growth_estimate;
  out->node_voltages_v = (Cplx *)arena_alloc_array(result_arena, sizeof(Cplx),
                                                   out->node_voltage_count);
  out->element_currents_a =
      (Cplx *)arena_alloc_array(result_arena, sizeof(Cplx), c->element_count);
  if (!out->node_voltages_v || (c->element_count && !out->element_currents_a)) {
    arena_destroy(&workspace);
    memset(out, 0, sizeof(*out));
    return MNA_ERR_MEMORY;
  }
  out->node_voltages_v[0] = cplx(0.0, 0.0);
  for (uint32_t node = 1; node <= c->node_count; ++node)
    out->node_voltages_v[node] = x[node - 1];
  for (size_t i = 0; i < c->element_count; ++i) {
    const CircuitElement *e = &c->elements[i];
    Cplx dv = cplx_sub(node_voltage_c(out->node_voltages_v, e->n1),
                       node_voltage_c(out->node_voltages_v, e->n2));
    switch (e->kind) {
    case CIRCUIT_RESISTOR:
      out->element_currents_a[i] = cplx_scale(dv, 1.0 / e->value_si);
      break;
    case CIRCUIT_CAPACITOR:
      out->element_currents_a[i] = cplx_mul(cplx(0.0, omega * e->value_si), dv);
      break;
    case CIRCUIT_VOLTAGE_SOURCE:
    case CIRCUIT_INDUCTOR:
    case CIRCUIT_VCVS:
    case CIRCUIT_CCVS:
      out->element_currents_a[i] = x[layout.branch_index[i]];
      break;
    case CIRCUIT_CURRENT_SOURCE:
      out->element_currents_a[i] = e->ac_value;
      break;
    case CIRCUIT_VCCS:
      out->element_currents_a[i] =
          cplx_scale(cplx_sub(node_voltage_c(out->node_voltages_v, e->n3),
                              node_voltage_c(out->node_voltages_v, e->n4)),
                     e->value_si);
      break;
    case CIRCUIT_CCCS: {
      Cplx ic = x[layout.branch_index[e->control_element_index]];
      out->element_currents_a[i] = cplx_scale(ic, e->value_si);
      break;
    }
    case CIRCUIT_DIODE:
    case CIRCUIT_BJT:
    case CIRCUIT_MOSFET:
    case CIRCUIT_JFET: {
      out->element_currents_a[i] = device_first_terminal_ac_current(
          e, op_x, out->node_voltages_v, omega);
      break;
    }
    default:
      out->element_currents_a[i] = cplx(0.0, 0.0);
      break;
    }
  }
  arena_destroy(&workspace);
  return MNA_OK;
}

MnaStatus mna_solve_transient(Arena *result_arena, const Circuit *c,
                              const MnaTransientOptions *options,
                              MnaTransientResult *out) {
  Arena workspace;
  Arena newton_scratch;
  MnaLayout layout;
  ClaWorkspace linear_workspace;
  size_t steps;
  size_t node_row_width;
  size_t n;
  double *previous;
  double *previous2;
  double *x;
  double *capacitor_current;
  MnaOptions opt;
  MnaStatus status = MNA_OK;

  if (!result_arena || !c || !options || !out ||
      !(options->time_step_s > 0.0) || !(options->stop_time_s >= 0.0) ||
      !isfinite(options->time_step_s) || !isfinite(options->stop_time_s) ||
      options->integration_method < MNA_INTEGRATION_BACKWARD_EULER ||
      options->integration_method > MNA_INTEGRATION_GEAR2)
    return MNA_ERR_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (circuit_validate(c) != CIRCUIT_OK)
    return MNA_ERR_CIRCUIT;

  double raw_steps = ceil(options->stop_time_s / options->time_step_s);
  if (!isfinite(raw_steps) || raw_steps > (double)UINT32_MAX)
    return MNA_ERR_RANGE;
  steps = (size_t)raw_steps + 1u;
  if (options->max_steps != 0u && steps > options->max_steps)
    return MNA_ERR_RANGE;

  arena_init(&workspace, 32u * 1024u);
  arena_init(&newton_scratch, 32u * 1024u);
  if (!alloc_layout(c, true, &workspace, &layout)) {
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }
  n = layout.n;
  node_row_width = (size_t)c->node_count + 1u;
  if (steps > SIZE_MAX / node_row_width ||
      steps > SIZE_MAX / (c->element_count ? c->element_count : 1u)) {
    status = MNA_ERR_RANGE;
    goto cleanup;
  }

  previous = (double *)arena_alloc_array(&workspace, sizeof(double), n);
  previous2 = (double *)arena_alloc_array(&workspace, sizeof(double), n);
  x = (double *)arena_alloc_array(&workspace, sizeof(double), n);
  capacitor_current = (double *)arena_alloc_array(
      &workspace, sizeof(double), c->element_count ? c->element_count : 1u);
  if (!previous || !previous2 || !x || !capacitor_current) {
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }
  memset(previous, 0, n * sizeof(double));
  memset(previous2, 0, n * sizeof(double));
  memset(x, 0, n * sizeof(double));
  memset(capacitor_current, 0,
         (c->element_count ? c->element_count : 1u) * sizeof(double));

  if (cla_workspace_init_real(&workspace, n, &linear_workspace) != CLA_OK) {
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }

  out->steps = steps;
  out->node_count = c->node_count;
  out->element_count = c->element_count;
  out->time_s = (double *)arena_alloc_array(result_arena, sizeof(double), steps);
  out->node_voltages_v = (double *)arena_alloc_array(
      result_arena, sizeof(double), steps * node_row_width);
  out->element_currents_a = (double *)arena_alloc_array(
      result_arena, sizeof(double), steps * c->element_count);
  if (!out->time_s || !out->node_voltages_v ||
      (c->element_count != 0u && !out->element_currents_a)) {
    memset(out, 0, sizeof(*out));
    status = MNA_ERR_MEMORY;
    goto cleanup;
  }

  memset(out->time_s, 0, steps * sizeof(double));
  memset(out->node_voltages_v, 0,
         steps * node_row_width * sizeof(double));
  if (c->element_count != 0u)
    memset(out->element_currents_a, 0,
           steps * c->element_count * sizeof(double));

  opt = options->nonlinear;
  if (opt.max_iterations == 0u)
    opt.max_iterations = 120u;

  if (options->use_initial_operating_point) {
    Arena initial_arena;
    MnaOperatingPoint initial_op;
    arena_init(&initial_arena, 8192u);
    status = mna_solve_dc(&initial_arena, c, &opt, &initial_op);
    if (status != MNA_OK) {
      arena_destroy(&initial_arena);
      goto cleanup;
    }
    for (uint32_t node = 1; node <= c->node_count; ++node) {
      previous[node - 1u] = initial_op.node_voltages_v[node];
      out->node_voltages_v[node] = initial_op.node_voltages_v[node];
    }
    for (size_t index = 0; index < c->element_count; ++index) {
      if (layout.branch_index[index] != SIZE_MAX)
        previous[layout.branch_index[index]] =
            initial_op.element_currents_a[index];
      out->element_currents_a[index] = initial_op.element_currents_a[index];
    }
    memcpy(previous2, previous, n * sizeof(double));
    memcpy(x, previous, n * sizeof(double));
    arena_destroy(&initial_arena);
  }

  if (options->initial_node_voltages_v != NULL) {
    if (options->initial_node_voltage_count != (size_t)c->node_count + 1u) {
      status = MNA_ERR_ARGUMENT;
      goto cleanup;
    }
    for (uint32_t node = 1; node <= c->node_count; ++node) {
      if (!options->initial_node_mask || options->initial_node_mask[node]) {
        double initial = options->initial_node_voltages_v[node];
        if (!isfinite(initial)) {
          status = MNA_ERR_ARGUMENT;
          goto cleanup;
        }
        previous[node - 1u] = initial;
        previous2[node - 1u] = initial;
        x[node - 1u] = initial;
        out->node_voltages_v[node] = initial;
      }
    }
  }

  for (size_t step = 1u; step < steps; ++step) {
    double previous_time = (double)(step - 1u) * options->time_step_s;
    double remaining_time = options->stop_time_s - previous_time;
    double dt = fmin(options->time_step_s, remaining_time);
    uint32_t iterations = 0u;
    double delta = 0.0;
    double residual = 0.0;
    double gmin_used = 0.0;
    MnaIntegrationMethod method = options->integration_method;

    memcpy(x, previous, n * sizeof(double));
    if (step == 1u && method != MNA_INTEGRATION_BACKWARD_EULER)
      method = MNA_INTEGRATION_BACKWARD_EULER;

    /* Rewind only per-step Newton matrices. The transient history arrays and
     * reusable LU workspace stay alive in `workspace`. */
    arena_reset(&newton_scratch);
    status = dc_solve_internal(
        c, &opt, previous, true, dt,
        fmin(options->stop_time_s, (double)step * options->time_step_s),
        method, previous2, capacitor_current,
        step > 1u ? options->time_step_s : 0.0, &layout, x, &iterations,
        &delta, &residual, &gmin_used, &newton_scratch, &linear_workspace);
    if (status != MNA_OK) {
      MNA_TRACE_EVENT(COPPER_TRACE_TRANSIENT_STEP, "step-failed", status,
                      iterations, residual, 0.0, delta, gmin_used, 1.0,
                      (double)step * options->time_step_s, dt, (int)method);
      goto cleanup;
    }

    MNA_TRACE_EVENT(COPPER_TRACE_TRANSIENT_STEP, "step", MNA_OK, iterations,
                    residual, 1.0, delta, gmin_used, 1.0,
                    (double)step * options->time_step_s, dt, (int)method);
    if (residual > out->maximum_scaled_residual)
      out->maximum_scaled_residual = residual;
    if (iterations > out->maximum_newton_iterations)
      out->maximum_newton_iterations = iterations;

    out->time_s[step] =
        fmin(options->stop_time_s, (double)step * options->time_step_s);
    double *voltage_row = out->node_voltages_v + step * node_row_width;
    double *current_row = c->element_count != 0u
                              ? out->element_currents_a + step * c->element_count
                              : NULL;
    voltage_row[0] = 0.0;
    for (uint32_t node = 1; node <= c->node_count; ++node)
      voltage_row[node] = x[node - 1u];

    for (size_t index = 0; index < c->element_count; ++index) {
      const CircuitElement *element = &c->elements[index];
      if (element->kind == CIRCUIT_CAPACITOR) {
        double old_voltage = node_voltage(previous, element->n1) -
                             node_voltage(previous, element->n2);
        double new_voltage = node_voltage(x, element->n1) -
                             node_voltage(x, element->n2);
        if (method == MNA_INTEGRATION_TRAPEZOIDAL) {
          current_row[index] =
              2.0 * element->value_si * (new_voltage - old_voltage) / dt -
              capacitor_current[index];
        } else if (method == MNA_INTEGRATION_GEAR2 && step > 1u) {
          double old2_voltage = node_voltage(previous2, element->n1) -
                                node_voltage(previous2, element->n2);
          double ratio = dt / options->time_step_s;
          double a0 = (1.0 + 2.0 * ratio) / (1.0 + ratio);
          double a1 = 1.0 + ratio;
          double a2 = ratio * ratio / (1.0 + ratio);
          current_row[index] = (element->value_si / dt) *
                               (a0 * new_voltage - a1 * old_voltage +
                                a2 * old2_voltage);
        } else {
          current_row[index] =
              element->value_si * (new_voltage - old_voltage) / dt;
        }
        capacitor_current[index] = current_row[index];
      } else {
        current_row[index] = element_current_dc(
            element, x, &layout, index,
            opt.device_exponent_limit > 0.0 ? opt.device_exponent_limit : 40.0);
      }
    }

    memcpy(previous2, previous, n * sizeof(double));
    memcpy(previous, x, n * sizeof(double));
  }

cleanup:
  arena_destroy(&newton_scratch);
  arena_destroy(&workspace);
  if (status != MNA_OK)
    memset(out, 0, sizeof(*out));
  return status;
}

