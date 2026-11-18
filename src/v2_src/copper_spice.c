#include "copper_spice.h"
#include "copper_spice_expand.h"
#include "arena.h"
#include "nmath.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CSP_TOKEN_CAP 128u
#define CSP_LINE_CAP 4096u

#if COPPER_TRACE
static void trace_spice_line(size_t line_number, const char *detail,
                             const char *const *tokens, size_t token_count,
                             CspStatus status)
{
    CopperTraceEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = COPPER_TRACE_PARSE_LINE;
    event.module = "copper_spice";
    event.detail = detail;
    event.tokens = tokens;
    event.token_count = token_count;
    event.line_number = line_number;
    event.status = (int)status;
    COPPER_TRACE_EVENT(event);
}
#else
#define trace_spice_line(line_, detail_, tokens_, count_, status_) ((void)0)
#endif

static int eq_ci(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a && *b) {
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
      return 0;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

static char *arena_copy(Arena *a, const char *s, size_t n) {
  char *p = arena_alloc_array(a, 1, n + 1);
  if (!p)
    return NULL;
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

static size_t tokenize(char *line, char **tokens, size_t cap) {
  size_t n = 0;
  char *p = line;
  while (*p) {
    while (*p &&
           (isspace((unsigned char)*p) || *p == ',' || *p == '(' || *p == ')'))
      ++p;
    if (!*p)
      break;
    if (n == cap)
      return cap + 1;
    tokens[n++] = p;
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '(' &&
           *p != ')')
      ++p;
    if (*p)
      *p++ = 0;
  }
  return n;
}

static int parse_number(const char *s, double *out) {
  if (!s || !out)
    return 0;
  char *end = NULL;
  double value = strtod(s, &end);
  if (end == s || !nm_isfinite(value))
    return 0;
  double scale = 1.0;
  if (*end) {
    char first = (char)tolower((unsigned char)*end++);
    switch (first) {
    case 't': scale = 1e12; break;
    case 'g': scale = 1e9; break;
    case 'k': scale = 1e3; break;
    case 'm':
      if (tolower((unsigned char)end[0]) == 'e' && end[1] != '\0' &&
          tolower((unsigned char)end[1]) == 'g') {
        scale = 1e6;
        end += 2;
      } else if (tolower((unsigned char)end[0]) == 'i' && end[1] != '\0' &&
                 tolower((unsigned char)end[1]) == 'l') {
        /* One mil is one thousandth of an inch. */
        scale = 25.4e-6;
        end += 2;
      } else {
        scale = 1e-3;
      }
      break;
    case 'u': scale = 1e-6; break;
    case 'n': scale = 1e-9; break;
    case 'p': scale = 1e-12; break;
    case 'f': scale = 1e-15; break;
    default:
      /* SPICE ignores alphabetic units such as V, A, Ohm and F. */
      --end;
      scale = 1.0;
      break;
    }
    while (*end && isalpha((unsigned char)*end))
      ++end;
  }
  if (*end)
    return 0;
  value *= scale;
  if (!nm_isfinite(value))
    return 0;
  *out = value;
  return 1;
}

static int parse_uint(const char *s, uint32_t *out) {
  double x;
  if (!parse_number(s, &x) || x < 1.0 || x > (double)UINT32_MAX ||
      floor(x) != x)
    return 0;
  *out = (uint32_t)x;
  return 1;
}

static int node_add(Arena *a, CspNetlist *net, const char *name, size_t cap) {
  /* ngspice treats both node 0 and the conventional gnd alias as
     * reference ground. Keep them out of the unknown-node table. */
  if (eq_ci(name, "0") || eq_ci(name, "gnd"))
    return 0;
  for (size_t i = 0; i < net->node_count; ++i)
    if (eq_ci(net->node_names[i], name))
      return (int)(i + 1u);
  if (net->node_count >= cap)
    return -1;
  char *copy = arena_copy(a, name, strlen(name));
  if (!copy)
    return -4;
  net->node_names[net->node_count] = copy;
  ++net->node_count;
  return (int)net->node_count;
}

static int add_model(Arena *a, CspNetlist *net, size_t cap, char **t,
                     size_t nt) {
  if (nt < 3 || net->model_count >= cap)
    return -1;
  for (size_t i = 0; i < net->model_count; ++i)
    if (eq_ci(net->models[i].name, t[1]))
      return -3;
  CspModelKind kind;
  if (eq_ci(t[2], "D"))
    kind = CSP_MODEL_DIODE;
  else if (eq_ci(t[2], "NPN"))
    kind = CSP_MODEL_NPN;
  else if (eq_ci(t[2], "PNP"))
    kind = CSP_MODEL_PNP;
  else if (eq_ci(t[2], "NMOS"))
    kind = CSP_MODEL_NMOS;
  else if (eq_ci(t[2], "PMOS"))
    kind = CSP_MODEL_PMOS;
  else if (eq_ci(t[2], "NJF"))
    kind = CSP_MODEL_NJF;
  else if (eq_ci(t[2], "PJF"))
    kind = CSP_MODEL_PJF;
  else
    return -2;
  CspModel *m = &net->models[net->model_count];
  m->name = arena_copy(a, t[1], strlen(t[1]));
  m->kind = kind;
  m->params = arena_alloc_array(a, sizeof(CspModelParam), CSP_MAX_MODEL_PARAMS);
  if (!m->name || !m->params)
    return -4;
  m->param_count = 0;
  for (size_t i = 3; i < nt; ++i) {
    char *eq = strchr(t[i], '=');
    if (!eq)
      return -1;
    *eq = 0;
    if (m->param_count >= CSP_MAX_MODEL_PARAMS)
      return -1;
    double v;
    if (!parse_number(eq + 1, &v))
      return -1;
    for (size_t k = 0; k < m->param_count; ++k)
      if (eq_ci(m->params[k].name, t[i]))
        return -3;
    char *key = arena_copy(a, t[i], strlen(t[i]));
    if (!key)
      return -4;
    m->params[m->param_count++] = (CspModelParam){key, v};
  }
  ++net->model_count;
  return 0;
}

static int parse_waveform(char **t, size_t nt, size_t at, CircuitWaveform *wave,
                          double *initial) {
  if (at >= nt)
    return 0;
  CircuitWaveformKind kind;
  size_t need;
  if (eq_ci(t[at], "PULSE")) {
    kind = CIRCUIT_WAVEFORM_PULSE;
    need = 7;
  } else if (eq_ci(t[at], "SIN")) {
    kind = CIRCUIT_WAVEFORM_SIN;
    need = 6;
  } else if (eq_ci(t[at], "EXP")) {
    kind = CIRCUIT_WAVEFORM_EXP;
    need = 6;
  } else
    return 0;
  if (nt - at - 1 < need)
    return -1;
  memset(wave, 0, sizeof(*wave));
  wave->kind = kind;
  for (size_t i = 0; i < need; ++i)
    if (!parse_number(t[at + 1 + i], &wave->p[i]))
      return -1;
  if (kind == CIRCUIT_WAVEFORM_SIN) {
    wave->p[5] *= 0.017453292519943295769236907684886;
  }
  *initial = wave->p[0];
  return (int)(need + 1);
}

static int parse_source(char **t, size_t nt, CspElement *e) {
  size_t i = 3;
  int have_dc = 0, have_ac = 0, have_waveform = 0;
  double dc = 0.0, wave_initial = 0.0;
  while (i < nt) {
    if (eq_ci(t[i], "DC")) {
      ++i;
      if (have_dc || i >= nt || !parse_number(t[i++], &dc))
        return 0;
      have_dc = 1;
    } else if (eq_ci(t[i], "AC")) {
      double magnitude = 0.0, phase = 0.0;
      ++i;
      if (have_ac || i >= nt || !parse_number(t[i++], &magnitude))
        return 0;
      if (i < nt && !eq_ci(t[i], "DC") && !eq_ci(t[i], "PULSE") &&
          !eq_ci(t[i], "SIN") && !eq_ci(t[i], "EXP")) {
        if (!parse_number(t[i++], &phase))
          return 0;
      }
      e->ac_value = cplx_polar_deg(magnitude, phase);
      have_ac = 1;
    } else if (eq_ci(t[i], "PULSE") || eq_ci(t[i], "SIN") ||
               eq_ci(t[i], "EXP")) {
      int consumed = parse_waveform(t, nt, i, &e->waveform, &wave_initial);
      if (consumed <= 0 || have_waveform)
        return 0;
      i += (size_t)consumed;
      have_waveform = 1;
    } else {
      /* A bare leading number is the SPICE DC value. */
      if (have_dc || have_ac || have_waveform || !parse_number(t[i++], &dc))
        return 0;
      have_dc = 1;
    }
  }
  e->has_dc_value = have_dc != 0;
  e->value_si = have_dc ? dc : (have_waveform ? wave_initial : 0.0);
  return 1;
}

static int parse_element(Arena *a, CspNetlist *net, size_t node_cap, char **t,
                         size_t nt) {
  if (nt < 4 || nt > CSP_TOKEN_CAP)
    return -1;
  for (size_t i = 0; i < net->element_count; ++i)
    if (eq_ci(net->elements[i].name, t[0]))
      return -3;
  CspElement e;
  memset(&e, 0, sizeof(e));
  e.instance_factor = 1.0;
  e.instance_width_m = 1e-4;
  e.instance_length_m = 1e-4;
  e.instance_multiplier = 1.0;
  char lead = (char)toupper((unsigned char)t[0][0]);
  size_t required = 0;
  switch (lead) {
  case 'R':
    e.kind = CSP_R;
    required = 4;
    break;
  case 'C':
    e.kind = CSP_C;
    required = 4;
    break;
  case 'L':
    e.kind = CSP_L;
    required = 4;
    break;
  case 'V':
    e.kind = CSP_V;
    required = 4;
    break;
  case 'I':
    e.kind = CSP_I;
    required = 4;
    break;
  case 'D':
    e.kind = CSP_D;
    required = 4;
    break;
  case 'Q':
    e.kind = CSP_Q;
    required = 5;
    break;
  case 'M':
    e.kind = CSP_M;
    required = 6;
    break;
  case 'J':
    e.kind = CSP_J;
    required = 5;
    break;
  case 'E':
    e.kind = CSP_E;
    required = 6;
    break;
  case 'G':
    e.kind = CSP_G;
    required = 6;
    break;
  case 'F':
    e.kind = CSP_F;
    required = 5;
    break;
  case 'H':
    e.kind = CSP_H;
    required = 5;
    break;
  default:
    return -2;
  }
  if (nt < required)
    return -1;
  e.name = arena_copy(a, t[0], strlen(t[0]));
  if (!e.name)
    return -4;
  size_t node_tokens = 2;
  if (e.kind == CSP_R || e.kind == CSP_C || e.kind == CSP_L ||
      e.kind == CSP_V || e.kind == CSP_I || e.kind == CSP_D) {
    node_tokens = 2;
  } else if (e.kind == CSP_Q || e.kind == CSP_M || e.kind == CSP_J ||
             e.kind == CSP_E || e.kind == CSP_G) {
    node_tokens = (e.kind == CSP_Q || e.kind == CSP_J)                      ? 3
                  : (e.kind == CSP_M || e.kind == CSP_E || e.kind == CSP_G) ? 4
                                                                            : 3;
  }
  for (size_t i = 0; i < node_tokens; ++i) {
    int id = node_add(a, net, t[1 + i], node_cap);
    if (id < 0)
      return id;
    e.nodes[i] = arena_copy(a, t[1 + i], strlen(t[1 + i]));
    if (!e.nodes[i])
      return -4;
  }
  if (e.kind == CSP_R || e.kind == CSP_C || e.kind == CSP_L) {
    if (nt != 4 || !parse_number(t[3], &e.value_si))
      return -1;
    if ((e.kind == CSP_R && e.value_si <= 0.0) || e.value_si < 0.0)
      return -5;
  } else if (e.kind == CSP_V || e.kind == CSP_I) {
    if (!parse_source(t, nt, &e))
      return -1;
  } else if (e.kind == CSP_D) {
    if (nt != 4)
      return -1;
    e.model_name = arena_copy(a, t[3], strlen(t[3]));
  } else if (e.kind == CSP_Q || e.kind == CSP_J) {
    if (nt != 5)
      return -1;
    e.model_name = arena_copy(a, t[4], strlen(t[4]));
  } else if (e.kind == CSP_M) {
    if (nt < 6)
      return -1;
    e.nodes[3] = arena_copy(a, t[4], strlen(t[4]));
    int id = node_add(a, net, t[4], node_cap);
    if (id < 0)
      return id;
    e.model_name = arena_copy(a, t[5], strlen(t[5]));
    double width = 1e-4, length = 1e-4, multiplier = 1.0;
    int have_w = 0, have_l = 0, have_m = 0;
    for (size_t k = 6; k < nt; ++k) {
      char *eq = strchr(t[k], '=');
      if (!eq)
        return -1;
      *eq = 0;
      double value;
      if (!parse_number(eq + 1, &value))
        return -1;
      if (eq_ci(t[k], "W")) {
        if (have_w || value <= 0.0)
          return -5;
        width = value;
        have_w = 1;
      } else if (eq_ci(t[k], "L")) {
        if (have_l || value <= 0.0)
          return -5;
        length = value;
        have_l = 1;
      } else if (eq_ci(t[k], "M")) {
        if (have_m || value <= 0.0)
          return -5;
        multiplier = value;
        have_m = 1;
      } else
        return -2;
    }
    e.instance_width_m = width;
    e.instance_length_m = length;
    e.instance_multiplier = multiplier;
    e.instance_factor = multiplier * (width / length);
    if (!nm_isfinite(e.instance_factor) || e.instance_factor <= 0.0)
      return -5;
  } else if (e.kind == CSP_E || e.kind == CSP_G) {
    if (nt != 6 || !parse_number(t[5], &e.value_si))
      return -1;
  } else if (e.kind == CSP_F || e.kind == CSP_H) {
    if (nt != 5 || !parse_number(t[4], &e.value_si))
      return -1;
    e.control_name = arena_copy(a, t[3], strlen(t[3]));
  }
  if ((e.kind == CSP_D || e.kind == CSP_Q || e.kind == CSP_M ||
       e.kind == CSP_J) &&
      !e.model_name)
    return -4;
  if ((e.kind == CSP_F || e.kind == CSP_H) && !e.control_name)
    return -4;
  net->elements[net->element_count++] = e;
  return 0;
}

static int add_node_value(Arena *a, CspNodeValue *items, size_t *count,
                          size_t capacity, const char *node, double value) {
  if (!a || !items || !count || !node || !nm_isfinite(value) ||
      *count >= capacity)
    return -4;
  char *copy = arena_copy(a, node, strlen(node));
  if (!copy)
    return -4;
  items[*count].node_name = copy;
  items[*count].voltage_v = value;
  ++*count;
  return 0;
}

static int parse_directive(Arena *a, CspNetlist *net, char **t, size_t nt) {
  if (nt == 0)
    return 0;
  if (eq_ci(t[0], ".END"))
    return 1;
  if (eq_ci(t[0], ".IC") || eq_ci(t[0], ".NODESET")) {
    const int is_ic = eq_ci(t[0], ".IC");
    const char *node = NULL;
    const char *value_text = NULL;
    if (nt == 3) {
      node = t[1];
      value_text = t[2];
    } else if (nt == 4 && eq_ci(t[1], "V")) {
      node = t[2];
      value_text = t[3];
    } else {
      return -1;
    }
    if (value_text[0] == '=')
      ++value_text;
    double value;
    if (!parse_number(value_text, &value))
      return -1;
    return is_ic ? add_node_value(a, net->initial_conditions,
                                  &net->initial_condition_count,
                                  net->source_line_capacity,
                                  node, value)
                 : add_node_value(a, net->nodesets, &net->nodeset_count,
                                  net->source_line_capacity,
                                  node, value);
  }
  if (eq_ci(t[0], ".TEMP")) {
    double value;
    if (nt != 2 || !parse_number(t[1], &value))
      return -1;
    if (value <= -273.15)
      return -5;
    net->temperature_c = value;
    return 0;
  }
  if (eq_ci(t[0], ".OP")) {
    net->analysis.kind = CSP_ANALYSIS_OP;
    return nt == 1 ? 0 : -1;
  }
  if (eq_ci(t[0], ".AC")) {
    if (nt != 5)
      return -1;
    if (eq_ci(t[1], "DEC"))
      net->analysis.sweep = CSP_SWEEP_DEC;
    else if (eq_ci(t[1], "OCT"))
      net->analysis.sweep = CSP_SWEEP_OCT;
    else if (eq_ci(t[1], "LIN"))
      net->analysis.sweep = CSP_SWEEP_LIN;
    else
      return -1;
    if (!parse_uint(t[2], &net->analysis.points) ||
        !parse_number(t[3], &net->analysis.start) ||
        !parse_number(t[4], &net->analysis.stop))
      return -1;
    if (net->analysis.start <= 0.0 ||
        net->analysis.stop < net->analysis.start || net->analysis.points == 0)
      return -5;
    net->analysis.kind = CSP_ANALYSIS_AC;
    return 0;
  }
  if (eq_ci(t[0], ".TRAN")) {
    if (nt < 3 || nt > 5)
      return -1;
    if (!parse_number(t[1], &net->analysis.step) ||
        !parse_number(t[2], &net->analysis.stop))
      return -1;
    net->analysis.start_time = 0.0;
    net->analysis.max_step = net->analysis.step;
    if (nt >= 4 && !parse_number(t[3], &net->analysis.start_time))
      return -1;
    if (nt == 5 && !parse_number(t[4], &net->analysis.max_step))
      return -1;
    if (net->analysis.step <= 0.0 || net->analysis.stop < 0.0 ||
        net->analysis.start_time < 0.0 ||
        net->analysis.start_time > net->analysis.stop ||
        net->analysis.max_step <= 0.0)
      return -5;
    net->analysis.kind = CSP_ANALYSIS_TRAN;
    return 0;
  }
  if (eq_ci(t[0], ".DC")) {
    if (nt != 5)
      return -1;
    double start, stop, step;
    if (!parse_number(t[2], &start) || !parse_number(t[3], &stop) ||
        !parse_number(t[4], &step))
      return -1;
    if (step == 0.0 || ((stop - start) * step) < 0.0)
      return -5;
    net->analysis.kind = CSP_ANALYSIS_DC;
    net->analysis.source_name = arena_copy(a, t[1], strlen(t[1]));
    if (!net->analysis.source_name)
      return -4;
    net->analysis.start = start;
    net->analysis.stop = stop;
    net->analysis.step = step;
    return 0;
  }
  if (eq_ci(t[0], ".MODEL"))
    return 2; /* parsed by caller */
  if (eq_ci(t[0], ".OPTIONS")) {
    for (size_t i = 1; i < nt; ++i) {
      char *eq = strchr(t[i], '=');
      if (!eq)
        return -1;
      *eq = 0;
      if (eq_ci(t[i], "METHOD")) {
        if (eq_ci(eq + 1, "BE"))
          net->analysis.integration_method = MNA_INTEGRATION_BACKWARD_EULER;
        else if (eq_ci(eq + 1, "TRAP") || eq_ci(eq + 1, "TRAPEZOIDAL"))
          net->analysis.integration_method = MNA_INTEGRATION_TRAPEZOIDAL;
        else if (eq_ci(eq + 1, "GEAR") || eq_ci(eq + 1, "GEAR2"))
          net->analysis.integration_method = MNA_INTEGRATION_GEAR2;
        else
          return -2;
        continue;
      }
      double v;
      if (!parse_number(eq + 1, &v))
        return -1;
      if (eq_ci(t[i], "TNOM")) {
        if (v <= -273.15)
          return -5;
        net->nominal_temperature_c = v;
      } else if (eq_ci(t[i], "RELTOL")) {
        if (v <= 0.0)
          return -5;
        net->solver_options.relative_tolerance = v;
      } else if (eq_ci(t[i], "VNTOL")) {
        if (v <= 0.0)
          return -5;
        net->solver_options.voltage_absolute_tolerance_v = v;
      } else if (eq_ci(t[i], "ABSTOL")) {
        if (v <= 0.0)
          return -5;
        net->solver_options.current_absolute_tolerance_a = v;
      } else if (eq_ci(t[i], "GMIN")) {
        if (v < 0.0)
          return -5;
        net->solver_options.gmin_start_s = net->solver_options.gmin_end_s = v;
      } else if (eq_ci(t[i], "ITL1")) {
        if (v < 1.0 || v > (double)UINT32_MAX || floor(v) != v)
          return -5;
        net->solver_options.max_iterations = (uint32_t)v;
      } else if (eq_ci(t[i], "SRCSTEPS")) {
        if (v < 0.0 || v > (double)UINT32_MAX || floor(v) != v)
          return -5;
        net->solver_options.source_stepping_steps = (uint32_t)v;
      } else if (eq_ci(t[i], "GMINSTEPS")) {
        if (v < 1.0 || v > (double)UINT32_MAX || floor(v) != v)
          return -5;
        net->solver_options.gmin_steps = (uint32_t)v;
      } else
        return -2;
    }
    return 0;
  }
  return -2;
}

static int process_line(Arena *a, CspNetlist *net, size_t line_no, char *line,
                        size_t max_lines, size_t node_cap, size_t *error_line) {
  char *p = line;
  while (isspace((unsigned char)*p))
    ++p;
  if (!*p || *p == '*') {
    trace_spice_line(line_no, p, NULL, 0, CSP_OK);
    return 0;
  }
  for (char *q = p; *q; ++q)
    if (*q == '$' || *q == ';') {
      *q = 0;
      break;
    }
  while (*p && isspace((unsigned char)p[strlen(p) - 1]))
    p[strlen(p) - 1] = 0;
  if (!*p) {
    trace_spice_line(line_no, p, NULL, 0, CSP_OK);
    return 0;
  }
  char *tokens[CSP_TOKEN_CAP];
  size_t nt = tokenize(p, tokens, CSP_TOKEN_CAP);
  if (nt == 0) {
    trace_spice_line(line_no, p, NULL, 0, CSP_OK);
    return 0;
  }
  if (nt > CSP_TOKEN_CAP) {
    if (error_line)
      *error_line = line_no;
    trace_spice_line(line_no, p, NULL, 0, CSP_ERR_SYNTAX);
    return -1;
  }
  int rc;
  if (tokens[0][0] == '.') {
    if (eq_ci(tokens[0], ".MODEL"))
      rc = add_model(a, net, max_lines, tokens, nt);
    else
      rc = parse_directive(a, net, tokens, nt);
  } else
    rc = parse_element(a, net, node_cap, tokens, nt);
  if (rc < 0 && error_line)
    *error_line = line_no;
#if COPPER_TRACE
  CspStatus trace_status = rc == -2   ? CSP_ERR_UNSUPPORTED
                           : rc == -3 ? CSP_ERR_DUPLICATE
                           : rc == -4 ? CSP_ERR_MEMORY
                           : rc == -5 ? CSP_ERR_RANGE
                           : rc < 0   ? CSP_ERR_SYNTAX
                                      : CSP_OK;
  trace_spice_line(line_no, p, (const char *const *)tokens, nt, trace_status);
#endif
  return rc;
}

CspStatus csp_parse(Arena *a, const char *text, CspNetlist *out,
                    size_t *error_line) {
  if (!a || !text || !out)
    return CSP_ERR_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (error_line)
    *error_line = 0;
  out->solver_options = mna_default_options();
  out->analysis.integration_method = MNA_INTEGRATION_BACKWARD_EULER;
  out->temperature_c = 27.0;
  out->nominal_temperature_c = 27.0;
  size_t bytes = strlen(text), physical_lines = 1;
  for (size_t i = 0; i < bytes; ++i)
    if (text[i] == '\n')
      ++physical_lines;
  size_t node_cap = physical_lines * 4u + 1u;
  out->source_line_capacity = physical_lines;
  out->elements = arena_alloc_array(a, sizeof(CspElement), physical_lines);
  out->models = arena_alloc_array(a, sizeof(CspModel), physical_lines);
  out->initial_conditions = arena_alloc_array(a, sizeof(CspNodeValue), physical_lines);
  out->nodesets = arena_alloc_array(a, sizeof(CspNodeValue), physical_lines);
  out->node_names = arena_alloc_array(a, sizeof(char *), node_cap);
  char *logical = arena_alloc_array(a, 1, bytes + 1u);
  if (!out->elements || !out->models || !out->initial_conditions ||
      !out->nodesets || !out->node_names || !logical)
    return CSP_ERR_MEMORY;
  memset(out->elements, 0, sizeof(CspElement) * physical_lines);
  memset(out->models, 0, sizeof(CspModel) * physical_lines);

  const char *p = text;
  size_t line_no = 0;

  /* 1. Handle the first physical line unconditionally as the title. */
  if (*p) {
    ++line_no;
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len && p[len - 1] == '\r')
      --len;
    out->title = arena_copy(a, p, len);
    if (!out->title)
      return CSP_ERR_MEMORY;
    trace_spice_line(line_no, out->title, NULL, 0, CSP_OK);
    p = end ? end + 1 : p + len;
  }

  size_t logical_start = 2, used = 0;
  int stop = 0;

  while (*p && !stop) {
    ++line_no;
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len && p[len - 1] == '\r')
      --len;
    size_t lead = 0;
    while (lead < len && isspace((unsigned char)p[lead]))
      ++lead;
    int continuation = (lead < len && p[lead] == '+');
    if (continuation) {
      if (used == 0) {
        if (error_line)
          *error_line = line_no;
        return CSP_ERR_SYNTAX;
      }
      ++lead;
      while (lead < len && isspace((unsigned char)p[lead]))
        ++lead;
      if (used + 1u + len - lead > bytes)
        return CSP_ERR_RANGE;
      logical[used++] = ' ';
      memcpy(logical + used, p + lead, len - lead);
      used += len - lead;
      logical[used] = 0;
    } else {
      if (used) {
        int rc = process_line(a, out, logical_start, logical, physical_lines,
                              node_cap, error_line);
        if (rc == 1)
          stop = 1;
        else if (rc < 0) {
          out->error_line = error_line ? *error_line : logical_start;
          return rc == -2   ? CSP_ERR_UNSUPPORTED
                 : rc == -3 ? CSP_ERR_DUPLICATE
                 : rc == -4 ? CSP_ERR_MEMORY
                 : rc == -5 ? CSP_ERR_RANGE
                            : CSP_ERR_SYNTAX;
        }
        used = 0;
      }
      logical_start = line_no;
      if (lead < len) {
        memcpy(logical, p + lead, len - lead);
        used = len - lead;
        logical[used] = 0;
      }
    }
    p = end ? end + 1 : p + len;
  }
  if (used && !stop) {
    int rc = process_line(a, out, logical_start, logical, physical_lines,
                          node_cap, error_line);
    if (rc < 0)
      return rc == -2   ? CSP_ERR_UNSUPPORTED
             : rc == -3 ? CSP_ERR_DUPLICATE
             : rc == -4 ? CSP_ERR_MEMORY
             : rc == -5 ? CSP_ERR_RANGE
                        : CSP_ERR_SYNTAX;
  }
  if (out->element_count == 0) {
    if (error_line)
      *error_line = 1;
    return CSP_ERR_SYNTAX;
  }
  /* Resolve model and current-controlled-source references before compilation.
   */
  for (size_t i = 0; i < out->element_count; ++i) {
    CspElement *e = &out->elements[i];
    if (e->model_name) {
      int found = 0;
      for (size_t j = 0; j < out->model_count; ++j)
        if (eq_ci(e->model_name, out->models[j].name))
          found = 1;
      if (!found) {
        if (error_line)
          *error_line = 0;
        return CSP_ERR_UNRESOLVED;
      }
    }
    if (e->control_name) {
      int found = 0;
      for (size_t j = 0; j < out->element_count; ++j)
        if (eq_ci(e->control_name, out->elements[j].name))
          found = 1;
      if (!found) {
        if (error_line)
          *error_line = 0;
        return CSP_ERR_UNRESOLVED;
      }
    }
  }
  if (out->analysis.kind == CSP_ANALYSIS_DC) {
    size_t i = 0;
    while (i < out->element_count &&
           !eq_ci(out->analysis.source_name, out->elements[i].name))
      ++i;
    if (i == out->element_count ||
        (out->elements[i].kind != CSP_V && out->elements[i].kind != CSP_I) ||
        out->elements[i].waveform.kind != CIRCUIT_WAVEFORM_NONE) {
      if (error_line)
        *error_line = 0;
      return CSP_ERR_UNRESOLVED;
    }
  }
  return CSP_OK;
}

CspStatus csp_parse_with_includes(Arena *arena, const char *source_name,
                                  const char *text, size_t text_size,
                                  const CspExpandOptions *options,
                                  CspNetlist *out, size_t *error_line) {
  if (!arena || !text || !out)
    return CSP_ERR_ARGUMENT;
  char *expanded = NULL;
  size_t expanded_size = 0;
  if (!csp_expand_deck(arena, source_name, text, text_size, options,
                       &expanded, &expanded_size, error_line)) {
    memset(out, 0, sizeof(*out));
    return CSP_ERR_SYNTAX;
  }
  (void)expanded_size;
  return csp_parse(arena, expanded, out, error_line);
}

static double model_value(const CspModel *m, const char *name, double fallback,
                          int *known) {
  for (size_t i = 0; i < m->param_count; ++i)
    if (eq_ci(m->params[i].name, name))
      return m->params[i].value;
  if (known)
    *known = 0;
  return fallback;
}

static int model_params_supported(const CspModel *m) {
  for (size_t i = 0; i < m->param_count; ++i) {
    const char *p = m->params[i].name;
    int ok = 0;
    switch (m->kind) {
    case CSP_MODEL_DIODE:
      ok = eq_ci(p, "IS") || eq_ci(p, "N") || eq_ci(p, "RS") ||
           eq_ci(p, "BV") || eq_ci(p, "IBV") || eq_ci(p, "CJO") ||
           eq_ci(p, "VJ") || eq_ci(p, "M") || eq_ci(p, "TT") ||
           eq_ci(p, "XTI") || eq_ci(p, "EG");
      break;
    case CSP_MODEL_NPN:
    case CSP_MODEL_PNP:
      ok = eq_ci(p, "IS") || eq_ci(p, "BF") || eq_ci(p, "BR") ||
           eq_ci(p, "VAF") || eq_ci(p, "CJE") || eq_ci(p, "VJE") ||
           eq_ci(p, "MJE") || eq_ci(p, "CJC") || eq_ci(p, "VJC") ||
           eq_ci(p, "MJC") || eq_ci(p, "TF") || eq_ci(p, "XTI") ||
           eq_ci(p, "EG");
      break;
    case CSP_MODEL_NMOS:
    case CSP_MODEL_PMOS:
      ok = eq_ci(p, "KP") || eq_ci(p, "VTO") || eq_ci(p, "LAMBDA") ||
           eq_ci(p, "LEVEL") || eq_ci(p, "GAMMA") || eq_ci(p, "PHI") ||
           eq_ci(p, "TOX") || eq_ci(p, "CGSO") || eq_ci(p, "CGDO") ||
           eq_ci(p, "CGBO");
      break;
    case CSP_MODEL_NJF:
    case CSP_MODEL_PJF:
      ok = eq_ci(p, "BETA") || eq_ci(p, "IDSS") || eq_ci(p, "VTO") ||
           eq_ci(p, "LAMBDA");
      break;
    }
    if (!ok)
      return 0;
  }
  if (m->kind == CSP_MODEL_NMOS || m->kind == CSP_MODEL_PMOS) {
    for (size_t i = 0; i < m->param_count; ++i)
      if (eq_ci(m->params[i].name, "LEVEL") && m->params[i].value != 1.0)
        return 0;
  }
  return 1;
}

static const CspModel *find_model(const CspNetlist *net, const char *name) {
  for (size_t i = 0; i < net->model_count; ++i)
    if (eq_ci(net->models[i].name, name))
      return &net->models[i];
  return NULL;
}

static int find_node_id(const CspNetlist *net, const char *name, uint32_t *id) {
  if (eq_ci(name, "0") || eq_ci(name, "gnd")) {
    *id = 0;
    return 1;
  }
  for (size_t i = 0; i < net->node_count; ++i)
    if (eq_ci(net->node_names[i], name)) {
      *id = (uint32_t)(i + 1u);
      return 1;
    }
  return 0;
}

static double semiconductor_is_at_temperature(double is_nominal,
                                                  double temperature_k,
                                                  double nominal_k,
                                                  double xti, double eg_ev,
                                                  double emission) {
  const double k_over_q = 8.617333262145e-5;
  if (!(is_nominal > 0.0) || !(temperature_k > 0.0) ||
      !(nominal_k > 0.0) || !(emission > 0.0))
    return is_nominal;
  double ratio = temperature_k / nominal_k;
  double exponent = -eg_ev / (emission * k_over_q) *
                    (1.0 / temperature_k - 1.0 / nominal_k);
  if (exponent > 80.0) exponent = 80.0;
  if (exponent < -80.0) exponent = -80.0;
  return is_nominal * pow(ratio, xti) * exp(exponent);
}

CspStatus csp_build_circuit(Arena *a, const CspNetlist *net, Circuit *out) {
  if (!a || !net || !out || !net->elements || net->element_count == 0 ||
      net->node_count == 0)
    return CSP_ERR_ARGUMENT;
  if (circuit_init(a, out, (uint32_t)net->node_count, net->element_count) !=
      CIRCUIT_OK)
    return CSP_ERR_MEMORY;
  for (size_t i = 0; i < net->element_count; ++i) {
    const CspElement *src = &net->elements[i];
    CircuitElement e;
    memset(&e, 0, sizeof(e));
    e.reference = src->name;
    e.value_si = src->value_si;
    e.has_dc_value = src->has_dc_value;
    e.ac_value = src->ac_value;
    e.waveform = src->waveform;
    size_t nodes = 2;
    if (src->kind == CSP_Q || src->kind == CSP_J)
      nodes = 3;
    else if (src->kind == CSP_M)
      nodes = 4;
    else if (src->kind == CSP_E || src->kind == CSP_G)
      nodes = 4;
    for (size_t k = 0; k < nodes; ++k)
      if (!find_node_id(net, src->nodes[k],
                        k == 0   ? &e.n1
                        : k == 1 ? &e.n2
                        : k == 2 ? &e.n3
                                 : &e.n4))
        return CSP_ERR_UNRESOLVED;
    switch (src->kind) {
    case CSP_R:
      e.kind = CIRCUIT_RESISTOR;
      break;
    case CSP_C:
      e.kind = CIRCUIT_CAPACITOR;
      break;
    case CSP_L:
      e.kind = CIRCUIT_INDUCTOR;
      break;
    case CSP_V:
      e.kind = CIRCUIT_VOLTAGE_SOURCE;
      break;
    case CSP_I:
      e.kind = CIRCUIT_CURRENT_SOURCE;
      break;
    case CSP_E:
      e.kind = CIRCUIT_VCVS;
      break;
    case CSP_G:
      e.kind = CIRCUIT_VCCS;
      break;
    case CSP_F:
      e.kind = CIRCUIT_CCCS;
      break;
    case CSP_H:
      e.kind = CIRCUIT_CCVS;
      break;
    case CSP_D:
    case CSP_Q:
    case CSP_M:
    case CSP_J: {
      const CspModel *m = find_model(net, src->model_name);
      if (!m)
        return CSP_ERR_UNRESOLVED;
      if (!model_params_supported(m))
        return CSP_ERR_UNSUPPORTED;
      const double temperature_k = net->temperature_c + 273.15;
      const double nominal_k = net->nominal_temperature_c + 273.15;
      const double vt = 8.617333262145e-5 * temperature_k;
      if (src->kind == CSP_D && m->kind == CSP_MODEL_DIODE) {
        e.kind = CIRCUIT_DIODE;
        e.model.diode.emission_coefficient = model_value(m, "N", 1.0, NULL);
        e.model.diode.saturation_current_a = semiconductor_is_at_temperature(
            model_value(m, "IS", 1e-14, NULL), temperature_k, nominal_k,
            model_value(m, "XTI", 3.0, NULL),
            model_value(m, "EG", 1.11, NULL),
            e.model.diode.emission_coefficient);
        e.model.diode.thermal_voltage_v = vt;
        e.model.diode.series_resistance_ohm = model_value(m, "RS", 0.0, NULL);
        e.model.diode.breakdown_voltage_v = model_value(m, "BV", 0.0, NULL);
        e.model.diode.breakdown_current_a = model_value(m, "IBV", 1e-3, NULL);
        e.model.diode.junction_capacitance_f = model_value(m, "CJO", 0.0, NULL);
        e.model.diode.junction_potential_v = model_value(m, "VJ", 1.0, NULL);
        e.model.diode.grading_coefficient = model_value(m, "M", 0.5, NULL);
        e.model.diode.transit_time_s = model_value(m, "TT", 0.0, NULL);
        e.model.diode.temperature_exponent = model_value(m, "XTI", 3.0, NULL);
        e.model.diode.bandgap_ev = model_value(m, "EG", 1.11, NULL);
        e.model.diode.nominal_temperature_k = nominal_k;
        e.model.diode.operating_temperature_k = temperature_k;
      } else if (src->kind == CSP_Q &&
                 (m->kind == CSP_MODEL_NPN || m->kind == CSP_MODEL_PNP)) {
        e.kind = CIRCUIT_BJT;
        e.model.bjt.saturation_current_a = semiconductor_is_at_temperature(
            model_value(m, "IS", 1e-16, NULL), temperature_k, nominal_k,
            model_value(m, "XTI", 3.0, NULL),
            model_value(m, "EG", 1.11, NULL), 1.0);
        e.model.bjt.beta_forward = model_value(m, "BF", 100.0, NULL);
        e.model.bjt.beta_reverse = model_value(m, "BR", 1.0, NULL);
        e.model.bjt.early_voltage_v = model_value(m, "VAF", 0.0, NULL);
        e.model.bjt.thermal_voltage_v = vt;
        e.model.bjt.cje_f = model_value(m, "CJE", 0.0, NULL);
        e.model.bjt.vje_v = model_value(m, "VJE", 0.75, NULL);
        e.model.bjt.mje = model_value(m, "MJE", 0.33, NULL);
        e.model.bjt.cjc_f = model_value(m, "CJC", 0.0, NULL);
        e.model.bjt.vjc_v = model_value(m, "VJC", 0.75, NULL);
        e.model.bjt.mjc = model_value(m, "MJC", 0.33, NULL);
        e.model.bjt.tf_s = model_value(m, "TF", 0.0, NULL);
        e.model.bjt.temperature_exponent = model_value(m, "XTI", 3.0, NULL);
        e.model.bjt.bandgap_ev = model_value(m, "EG", 1.11, NULL);
        e.model.bjt.nominal_temperature_k = nominal_k;
        e.model.bjt.operating_temperature_k = temperature_k;
        e.model.bjt.polarity =
            m->kind == CSP_MODEL_NPN ? CIRCUIT_BJT_NPN : CIRCUIT_BJT_PNP;
      } else if (src->kind == CSP_M &&
                 (m->kind == CSP_MODEL_NMOS || m->kind == CSP_MODEL_PMOS)) {
        double instance_factor = src->instance_factor;
        /* Zero-initialised public CspElement values mean default geometry. */
        if (instance_factor == 0.0)
          instance_factor = 1.0;
        if (!nm_isfinite(instance_factor) || instance_factor <= 0.0)
          return CSP_ERR_RANGE;
        e.kind = CIRCUIT_MOSFET;
        e.model.mos.kp_a_per_v2 =
            model_value(m, "KP", 2e-5, NULL) * instance_factor;
        e.model.mos.threshold_voltage_v = model_value(m, "VTO", 0.0, NULL);
        e.model.mos.channel_modulation_per_v =
            model_value(m, "LAMBDA", 0.0, NULL);
        e.model.mos.polarity =
            m->kind == CSP_MODEL_NMOS ? CIRCUIT_MOS_N : CIRCUIT_MOS_P;
        e.model.mos.body_effect_gamma = model_value(m, "GAMMA", 0.0, NULL);
        e.model.mos.surface_potential_v = model_value(m, "PHI", 0.6, NULL);
        double tox = model_value(m, "TOX", 0.0, NULL);
        const double eps_oxide_f_per_m = 3.453133e-11;
        if (tox > 0.0)
          e.model.mos.gate_oxide_capacitance_f =
              eps_oxide_f_per_m / tox * src->instance_width_m *
              src->instance_length_m * src->instance_multiplier;
        e.model.mos.gate_overlap_gs_f =
            model_value(m, "CGSO", 0.0, NULL) * src->instance_width_m *
            src->instance_multiplier;
        e.model.mos.gate_overlap_gd_f =
            model_value(m, "CGDO", 0.0, NULL) * src->instance_width_m *
            src->instance_multiplier;
        e.model.mos.gate_overlap_gb_f =
            model_value(m, "CGBO", 0.0, NULL) * src->instance_length_m *
            src->instance_multiplier;
      } else if (src->kind == CSP_J &&
                 (m->kind == CSP_MODEL_NJF || m->kind == CSP_MODEL_PJF)) {
        double vp = model_value(m, "VTO", -2.0, NULL);
        double idss = model_value(m, "IDSS", 0.0, NULL);
        double beta = model_value(m, "BETA", 0.0, NULL);
        if (idss <= 0.0) {
          if (beta <= 0.0)
            return CSP_ERR_RANGE;
          idss = 0.5 * beta * vp * vp;
        }
        e.kind = CIRCUIT_JFET;
        e.model.jfet.idss_a = idss;
        e.model.jfet.pinch_off_voltage_v = vp;
        e.model.jfet.channel_modulation_per_v =
            model_value(m, "LAMBDA", 0.0, NULL);
        e.model.jfet.polarity =
            m->kind == CSP_MODEL_NJF ? CIRCUIT_JFET_N : CIRCUIT_JFET_P;
      } else
        return CSP_ERR_UNSUPPORTED;
      break;
    }
    }
    if (src->kind == CSP_F || src->kind == CSP_H) {
      size_t j;
      for (j = 0; j < net->element_count; ++j)
        if (eq_ci(src->control_name, net->elements[j].name))
          break;
      if (j == net->element_count)
        return CSP_ERR_UNRESOLVED;
      e.control_element_index = (uint32_t)j;
    }
    if (circuit_add(out, &e) != CIRCUIT_OK)
      return CSP_ERR_RANGE;
  }
  return circuit_validate(out) == CIRCUIT_OK ? CSP_OK : CSP_ERR_RANGE;
}

static int checked_product(size_t a, size_t b, size_t *out) {
  if (!out || (b && a > SIZE_MAX / b))
    return 0;
  *out = a * b;
  return 1;
}

static CspStatus map_mna_status(MnaStatus status) {
  return status == MNA_ERR_MEMORY     ? CSP_ERR_MEMORY
         : status == MNA_ERR_RANGE    ? CSP_ERR_RANGE
         : status == MNA_ERR_ARGUMENT ? CSP_ERR_ARGUMENT
         : status == MNA_OK           ? CSP_OK
                                      : CSP_ERR_SOLVER;
}

static int find_element_index(const Circuit *c, const char *name,
                              size_t *index) {
  if (!c || !name || !index)
    return 0;
  for (size_t i = 0; i < c->element_count; ++i) {
    if (eq_ci(c->elements[i].reference, name)) {
      *index = i;
      return 1;
    }
  }
  return 0;
}

static CspStatus run_ac_sweep(Arena *result_arena, const CspNetlist *net,
                              const Circuit *c, const MnaOptions *solver_options,
                              CspAcSweepResult *out) {
  const CspAnalysis *a = &net->analysis;
  size_t intervals = 0, points = 0;
  if (a->sweep == CSP_SWEEP_LIN) {
    if (a->points == 1u) {
      if (a->start != a->stop)
        return CSP_ERR_RANGE;
      points = 1;
    } else
      points = (size_t)a->points;
  } else {
    double base = a->sweep == CSP_SWEEP_DEC ? 10.0 : 2.0;
    double raw = ceil(log(a->stop / a->start) / log(base) * (double)a->points);
    if (!nm_isfinite(raw) || raw > (double)(SIZE_MAX - 1u))
      return CSP_ERR_RANGE;
    intervals = (size_t)raw;
    points = intervals + 1u;
  }
  size_t nv = (size_t)c->node_count + 1u, ni = c->element_count, nv_total,
         ni_total;
  if (points == 0 || !checked_product(points, nv, &nv_total) ||
      !checked_product(points, ni, &ni_total))
    return CSP_ERR_RANGE;
  out->points = points;
  out->node_voltage_count = nv;
  out->element_current_count = ni;
  out->frequency_hz = arena_alloc_array(result_arena, sizeof(double), points);
  out->node_voltages_v =
      arena_alloc_array(result_arena, sizeof(Cplx), nv_total);
  out->element_currents_a =
      ni ? arena_alloc_array(result_arena, sizeof(Cplx), ni_total) : NULL;
  if (!out->frequency_hz || !out->node_voltages_v ||
      (ni && !out->element_currents_a))
    return CSP_ERR_MEMORY;

  Arena operating_arena;
  arena_init(&operating_arena, 4096);
  MnaOperatingPoint op;
  MnaStatus ms = mna_solve_dc(&operating_arena, c, solver_options, &op);
  if (ms != MNA_OK) {
    arena_destroy(&operating_arena);
    return map_mna_status(ms);
  }
  for (size_t k = 0; k < points; ++k) {
    double frequency;
    if (points == 1)
      frequency = a->start;
    else if (a->sweep == CSP_SWEEP_LIN)
      frequency =
          a->start + (a->stop - a->start) * (double)k / (double)(points - 1u);
    else if (k == intervals)
      frequency = a->stop;
    else
      frequency =
          a->start * pow(a->stop / a->start, (double)k / (double)intervals);
    Arena point_arena;
    arena_init(&point_arena, 4096);
    MnaAcResult ac;
    ms = mna_solve_ac(&point_arena, c, &op, frequency, &ac);
    if (ms == MNA_OK) {
      out->frequency_hz[k] = frequency;
      if (ac.relative_residual > out->maximum_relative_residual)
        out->maximum_relative_residual = ac.relative_residual;
      if (ac.pivot_growth_estimate > out->maximum_pivot_growth_estimate)
        out->maximum_pivot_growth_estimate = ac.pivot_growth_estimate;
      memcpy(out->node_voltages_v + k * nv, ac.node_voltages_v,
             nv * sizeof(Cplx));
      if (ni)
        memcpy(out->element_currents_a + k * ni, ac.element_currents_a,
               ni * sizeof(Cplx));
    }
    arena_destroy(&point_arena);
    if (ms != MNA_OK) {
      arena_destroy(&operating_arena);
      return map_mna_status(ms);
    }
  }
  arena_destroy(&operating_arena);
  return CSP_OK;
}

static CspStatus run_dc_sweep(Arena *result_arena, const CspNetlist *net,
                              Circuit *c, const MnaOptions *solver_options,
                              CspDcSweepResult *out) {
  size_t source_index;
  if (!find_element_index(c, net->analysis.source_name, &source_index))
    return CSP_ERR_UNRESOLVED;
  CircuitElement *source = &c->elements[source_index];
  if ((source->kind != CIRCUIT_VOLTAGE_SOURCE &&
       source->kind != CIRCUIT_CURRENT_SOURCE) ||
      source->waveform.kind != CIRCUIT_WAVEFORM_NONE)
    return CSP_ERR_UNSUPPORTED;
  double start = net->analysis.start, stop = net->analysis.stop,
         step = net->analysis.step;
  double distance = fabs(stop - start), increment = fabs(step);
  double raw = floor(distance / increment + 32.0 * DBL_EPSILON);
  if (!nm_isfinite(raw) || raw > (double)(SIZE_MAX - 1u))
    return CSP_ERR_RANGE;
  size_t points = (size_t)raw + 1u;
  size_t nv = (size_t)c->node_count + 1u, ni = c->element_count, nv_total,
         ni_total;
  if (!checked_product(points, nv, &nv_total) ||
      !checked_product(points, ni, &ni_total))
    return CSP_ERR_RANGE;
  out->points = points;
  out->node_voltage_count = nv;
  out->element_current_count = ni;
  out->sweep_values = arena_alloc_array(result_arena, sizeof(double), points);
  out->node_voltages_v =
      arena_alloc_array(result_arena, sizeof(double), nv_total);
  out->element_currents_a =
      ni ? arena_alloc_array(result_arena, sizeof(double), ni_total) : NULL;
  if (!out->sweep_values || !out->node_voltages_v ||
      (ni && !out->element_currents_a))
    return CSP_ERR_MEMORY;

  Arena op_arena;
  arena_init(&op_arena, 4096);
  for (size_t k = 0; k < points; ++k) {
    double value = start + step * (double)k;
    double end_tol = 32.0 * DBL_EPSILON * fmax(1.0, fabs(stop));
    if (step > 0.0 && value > stop) {
      if (value - stop <= end_tol)
        value = stop;
      else
        break;
    }
    if (step < 0.0 && value < stop) {
      if (stop - value <= end_tol)
        value = stop;
      else
        break;
    }
    source->value_si = value;
    arena_reset(&op_arena);
    MnaOperatingPoint op;
    MnaStatus ms = mna_solve_dc(&op_arena, c, solver_options, &op);
    if (ms != MNA_OK) {
      arena_destroy(&op_arena);
      return map_mna_status(ms);
    }
    out->sweep_values[k] = value;
    if (op.scaled_residual > out->maximum_scaled_residual)
      out->maximum_scaled_residual = op.scaled_residual;
    if (op.iterations > out->maximum_newton_iterations)
      out->maximum_newton_iterations = op.iterations;
    memcpy(out->node_voltages_v + k * nv, op.node_voltages_v,
           nv * sizeof(double));
    if (ni)
      memcpy(out->element_currents_a + k * ni, op.element_currents_a,
             ni * sizeof(double));
  }
  arena_destroy(&op_arena);
  return CSP_OK;
}

static void interpolate_transient(const MnaTransientResult *raw, double time,
                                  double *voltages, double *currents) {
  size_t lo = 0;
  while (lo + 1u < raw->steps && raw->time_s[lo + 1u] < time)
    ++lo;
  size_t hi = lo + 1u < raw->steps ? lo + 1u : lo;
  double t0 = raw->time_s[lo], t1 = raw->time_s[hi];
  double alpha = (hi == lo || t1 <= t0) ? 0.0 : (time - t0) / (t1 - t0);
  size_t nv = raw->node_count + 1u, ni = raw->element_count;
  for (size_t j = 0; j < nv; ++j) {
    double a = raw->node_voltages_v[lo * nv + j];
    double b = raw->node_voltages_v[hi * nv + j];
    voltages[j] = a + alpha * (b - a);
  }
  for (size_t j = 0; j < ni; ++j) {
    double a = raw->element_currents_a[lo * ni + j];
    double b = raw->element_currents_a[hi * ni + j];
    currents[j] = a + alpha * (b - a);
  }
}

static CspStatus run_transient(Arena *result_arena, const CspNetlist *net,
                               const Circuit *c, const MnaOptions *solver_options,
                               const double *initial_node_voltages_v,
                               const uint8_t *initial_node_mask,
                               size_t initial_node_voltage_count,
                               MnaTransientResult *out) {
  const CspAnalysis *a = &net->analysis;
  double internal_step =
      fmin(a->step, a->max_step > 0.0 ? a->max_step : a->step);
  MnaTransientOptions options = mna_default_transient_options();
  options.time_step_s = internal_step;
  options.stop_time_s = a->stop;
  options.max_steps = 0;
  options.nonlinear = *solver_options;
  options.integration_method = a->integration_method;
  options.use_initial_operating_point = true;
  options.initial_node_voltages_v = initial_node_voltages_v;
  options.initial_node_mask = initial_node_mask;
  options.initial_node_voltage_count = initial_node_voltage_count;
  Arena raw_arena;
  arena_init(&raw_arena, 4096);
  MnaTransientResult raw;
  MnaStatus ms = mna_solve_transient(&raw_arena, c, &options, &raw);
  if (ms != MNA_OK) {
    arena_destroy(&raw_arena);
    return map_mna_status(ms);
  }

  double span = a->stop - a->start_time;
  double raw_intervals = floor(span / a->step + 32.0 * DBL_EPSILON);
  if (!nm_isfinite(raw_intervals) || raw_intervals > (double)(SIZE_MAX - 2u)) {
    arena_destroy(&raw_arena);
    return CSP_ERR_RANGE;
  }
  size_t points = (size_t)raw_intervals + 1u;
  double last = a->start_time + (double)(points - 1u) * a->step;
  double tol = 32.0 * DBL_EPSILON * fmax(1.0, fabs(a->stop));
  int append_stop = (a->stop - last) > tol;
  if (append_stop)
    ++points;
  size_t nv = raw.node_count + 1u, ni = raw.element_count, nv_total, ni_total;
  if (!checked_product(points, nv, &nv_total) ||
      !checked_product(points, ni, &ni_total)) {
    arena_destroy(&raw_arena);
    return CSP_ERR_RANGE;
  }
  out->steps = points;
  out->node_count = raw.node_count;
  out->element_count = ni;
  out->maximum_scaled_residual = raw.maximum_scaled_residual;
  out->maximum_newton_iterations = raw.maximum_newton_iterations;
  out->time_s = arena_alloc_array(result_arena, sizeof(double), points);
  out->node_voltages_v =
      arena_alloc_array(result_arena, sizeof(double), nv_total);
  out->element_currents_a =
      ni ? arena_alloc_array(result_arena, sizeof(double), ni_total) : NULL;
  if (!out->time_s || !out->node_voltages_v ||
      (ni && !out->element_currents_a)) {
    arena_destroy(&raw_arena);
    return CSP_ERR_MEMORY;
  }
  for (size_t k = 0; k < points; ++k) {
    double time;
    if (append_stop && k == points - 1u)
      time = a->stop;
    else
      time = a->start_time + (double)k * a->step;
    out->time_s[k] = time;
    interpolate_transient(&raw, time, out->node_voltages_v + k * nv,
                          ni ? out->element_currents_a + k * ni : NULL);
  }
  arena_destroy(&raw_arena);
  return CSP_OK;
}

static CspStatus resolve_node_values(Arena *arena, const CspNetlist *net,
                                     const CspNodeValue *conditions,
                                     size_t condition_count, double **values_out,
                                     uint8_t **mask_out) {
  size_t count = (size_t)net->node_count + 1u;
  double *values = (double *)arena_alloc_array(arena, sizeof(double), count);
  uint8_t *mask = (uint8_t *)arena_alloc_array(arena, sizeof(uint8_t), count);
  if (!values || !mask)
    return CSP_ERR_MEMORY;
  memset(values, 0, sizeof(double) * count);
  memset(mask, 0, sizeof(uint8_t) * count);
  for (size_t i = 0; i < condition_count; ++i) {
    uint32_t node = 0;
    if (!find_node_id(net, conditions[i].node_name, &node))
      return CSP_ERR_UNRESOLVED;
    if (node == 0 && conditions[i].voltage_v != 0.0)
      return CSP_ERR_RANGE;
    values[node] = conditions[i].voltage_v;
    mask[node] = 1u;
  }
  *values_out = values;
  *mask_out = mask;
  return CSP_OK;
}

CspStatus csp_run_analysis(Arena *result_arena, const CspNetlist *net,
                           CspAnalysisResult *out) {
  if (!result_arena || !net || !out)
    return CSP_ERR_ARGUMENT;
  memset(out, 0, sizeof(*out));
  Arena circuit_arena;
  arena_init(&circuit_arena, 4096);
  Circuit circuit;
  CspStatus built = csp_build_circuit(&circuit_arena, net, &circuit);
  if (built != CSP_OK) {
    arena_destroy(&circuit_arena);
    return built;
  }
  double *nodeset_values = NULL, *initial_values = NULL;
  uint8_t *nodeset_mask = NULL, *initial_mask = NULL;
  CspStatus status = resolve_node_values(
      result_arena, net, net->nodesets, net->nodeset_count,
      &nodeset_values, &nodeset_mask);
  if (status == CSP_OK)
    status = resolve_node_values(result_arena, net, net->initial_conditions,
                                 net->initial_condition_count, &initial_values,
                                 &initial_mask);
  if (status != CSP_OK) {
    arena_destroy(&circuit_arena);
    return status;
  }
  MnaOptions solver_options = net->solver_options;
  if (net->nodeset_count) {
    solver_options.initial_node_voltages_v = nodeset_values;
    solver_options.initial_node_mask = nodeset_mask;
    solver_options.initial_node_voltage_count = (size_t)net->node_count + 1u;
  }
  CspAnalysisKind kind = net->analysis.kind == CSP_ANALYSIS_NONE
                             ? CSP_ANALYSIS_OP
                             : net->analysis.kind;
  out->kind = kind;
  if (kind == CSP_ANALYSIS_OP) {
    MnaStatus ms = mna_solve_dc(result_arena, &circuit, &solver_options,
                                &out->data.op);
    status = map_mna_status(ms);
  } else if (kind == CSP_ANALYSIS_AC) {
    status = run_ac_sweep(result_arena, net, &circuit, &solver_options,
                          &out->data.ac);
  } else if (kind == CSP_ANALYSIS_TRAN) {
    status = run_transient(
        result_arena, net, &circuit, &solver_options,
        net->initial_condition_count ? initial_values : NULL,
        net->initial_condition_count ? initial_mask : NULL,
        net->initial_condition_count ? (size_t)net->node_count + 1u : 0u,
        &out->data.tran);
  } else if (kind == CSP_ANALYSIS_DC) {
    Circuit sweep = circuit;
    sweep.elements = arena_alloc_array(result_arena, sizeof(CircuitElement),
                                       circuit.element_count);
    if (!sweep.elements) {
      arena_destroy(&circuit_arena);
      return CSP_ERR_MEMORY;
    }
    memcpy(sweep.elements, circuit.elements,
           sizeof(CircuitElement) * circuit.element_count);
    status = run_dc_sweep(result_arena, net, &sweep, &solver_options,
                          &out->data.dc);
  } else {
    status = CSP_ERR_UNSUPPORTED;
  }
  arena_destroy(&circuit_arena);
  return status;
}

const char *csp_status_name(CspStatus s) {
  static const char *const n[] = {"OK",          "ARGUMENT",   "SYNTAX",
                                  "UNSUPPORTED", "RANGE",      "MEMORY",
                                  "DUPLICATE",   "UNRESOLVED", "SOLVER_ERROR",
                                  "IO_ERROR"};
  return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "UNKNOWN";
}
