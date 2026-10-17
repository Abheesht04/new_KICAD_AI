#include "NLP.h"
#include "arena.h"
#include "component_model.h"
#include "design.h"
#include "mna.h"
#include "range.h"
#include "vec.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  double input_voltage_v;
  double target_voltage_v;
  double r_top_ohm;
  double r_bottom_ohm;
} PromptSpec;

static const char *skip_space(const char *p) {
  while (*p && isspace((unsigned char)*p)) {
    ++p;
  }

  return p;
}

static int is_voltage_unit(const char *p) { return p[0] == 'v' || p[0] == 'V'; }

static int parse_resistance_unit(const char *p, double *multiplier) {
  p = skip_space(p);

  if (*p == 'k' || *p == 'K') {
    *multiplier = 1e3;
    return 1;
  }

  if (*p == 'M') {
    *multiplier = 1e6;
    return 1;
  }

  if (*p == 'm') {
    *multiplier = 1e-3;
    return 1;
  }

  if (strncmp(p, "\xCE\xA9", 2) == 0) {
    *multiplier = 1.0;
    return 1;
  }

  if (strncmp(p, "ohm", 3) == 0 || strncmp(p, "Ohm", 3) == 0 ||
      strncmp(p, "OHM", 3) == 0) {
    *multiplier = 1.0;
    return 1;
  }

  return 0;
}

static int find_voltage_values(const char *text, double *first,
                               double *second) {
  size_t count = 0;
  const char *p = text;

  while (*p) {
    char *end = NULL;
    double value = strtod(p, &end);

    if (end != p) {
      const char *unit = skip_space(end);

      if (is_voltage_unit(unit)) {
        if (count == 0) {
          *first = value;
          ++count;
        } else if (count == 1) {
          *second = value;
          ++count;
          return 1;
        }
      }

      p = end;
    } else {
      ++p;
    }
  }

  return 0;
}

static int find_resistance_values(const char *text, double *first,
                                  double *second) {
  size_t count = 0;
  const char *p = text;

  while (*p) {
    char *end = NULL;
    double value = strtod(p, &end);

    if (end != p) {
      const char *unit = skip_space(end);
      double multiplier = 1.0;

      if (parse_resistance_unit(unit, &multiplier)) {
        value *= multiplier;

        if (count == 0) {
          *first = value;
          ++count;
        } else if (count == 1) {
          *second = value;
          return 1;
        }
      }

      p = end;
    } else {
      ++p;
    }
  }

  return 0;
}

static int prompt_parse(const char *text, PromptSpec *spec) {
  if (!text || !spec) {
    return 0;
  }

  if (!strstr(text, "resistor") || !strstr(text, "divider")) {
    return 0;
  }

  memset(spec, 0, sizeof(*spec));

  if (!find_voltage_values(text, &spec->input_voltage_v,
                           &spec->target_voltage_v)) {
    return 0;
  }

  if (!find_resistance_values(text, &spec->r_top_ohm, &spec->r_bottom_ohm)) {
    return 0;
  }

  if (spec->input_voltage_v <= 0.0 || spec->target_voltage_v <= 0.0 ||
      spec->r_top_ohm <= 0.0 || spec->r_bottom_ohm <= 0.0) {
    return 0;
  }

  return 1;
}

static int build_design(const PromptSpec *spec, Design *design) {
  Component r1;
  Component r2;

  uint32_t r1_id;
  uint32_t r2_id;

  uint32_t vin_net;
  uint32_t vout_net;
  uint32_t gnd_net;

  component_init(&r1);
  component_init(&r2);

  strcpy(r1.reference, "R1");
  strcpy(r2.reference, "R2");

  r1.model.kind = COMPONENT_RESISTOR;
  r2.model.kind = COMPONENT_RESISTOR;

  r1.model.data.resistor.resistance_ohm = range_exact(spec->r_top_ohm);

  r2.model.data.resistor.resistance_ohm = range_exact(spec->r_bottom_ohm);

  component_set_value(&r1, spec->r_top_ohm, 0.0);

  component_set_value(&r2, spec->r_bottom_ohm, 0.0);

  component_add_pin(&r1, 1, "top");
  component_add_pin(&r1, 2, "bottom");

  component_add_pin(&r2, 1, "top");
  component_add_pin(&r2, 2, "bottom");

  if (design_add_component(design, &r1, &r1_id) != 0) {
    component_free(&r1);
    component_free(&r2);
    return 0;
  }

  if (design_add_component(design, &r2, &r2_id) != 0) {
    component_free(&r1);
    component_free(&r2);
    return 0;
  }

  component_free(&r1);
  component_free(&r2);

  if (design_add_net(design, "VIN", &vin_net) != 0 ||
      design_add_net(design, "VOUT", &vout_net) != 0 ||
      design_add_net(design, "GND", &gnd_net) != 0) {
    return 0;
  }

  if (design_connect_pin(design, r1_id, 1, vin_net) != 0 ||
      design_connect_pin(design, r1_id, 2, vout_net) != 0 ||
      design_connect_pin(design, r2_id, 1, vout_net) != 0 ||
      design_connect_pin(design, r2_id, 2, gnd_net) != 0) {
    return 0;
  }

  return 1;
}

static int solve_design(const PromptSpec *spec, const Design *design) {
  (void)design;

  MnaCircuit circuit;
  MnaOptions options;
  MnaResult result;

  memset(&result, 0, sizeof(result));

  /*
   * Nodes:
   *
   *   0 = GND
   *   1 = VIN
   *   2 = VOUT
   */
  mna_circuit_init(&circuit, 2);

  if (!mna_add_voltage_source(&circuit, 1, 0, spec->input_voltage_v)) {
    mna_circuit_free(&circuit);
    return 0;
  }

  if (!mna_add_resistor(&circuit, 1, 2, spec->r_top_ohm)) {
    mna_circuit_free(&circuit);
    return 0;
  }

  if (!mna_add_resistor(&circuit, 2, 0, spec->r_bottom_ohm)) {
    mna_circuit_free(&circuit);
    return 0;
  }

  mna_options_default(&options);

  mna_solve(&circuit, &options, &result);

  if (result.status != MNA_STATUS_SUCCESS) {
    printf("\nMNA FAILED: status=%d\n", (int)result.status);

    mna_result_free(&result);
    mna_circuit_free(&circuit);

    return 0;
  }

  double vin = mna_node_voltage(&result, 1);

  double vout = mna_node_voltage(&result, 2);

  double divider_current =
      spec->input_voltage_v / (spec->r_top_ohm + spec->r_bottom_ohm);

  double error = fabs(vout - spec->target_voltage_v);

  printf("\nMNA\n");
  printf("  VIN         = %.6f V\n", vin);

  printf("  VOUT        = %.6f V\n", vout);

  printf("  divider I   = %.6f mA\n", divider_current * 1000.0);

  printf("  target err  = %.9f V\n", error);

  printf("  residual    = %.9e\n", result.maximum_residual);

  printf("  KCL         = %.9e A\n", result.maximum_kcl_residual_a);

  if (error < 1e-9) {
    printf("\nRESULT\n");
    printf("  VERIFIED\n");
  } else {
    printf("\nRESULT\n");
    printf("  TARGET MISMATCH\n");
  }

  mna_result_free(&result);
  mna_circuit_free(&circuit);

  return error < 1e-9;
}

int main(int argc, char **argv) {
  char prompt[1024];

  if (argc > 1) {
    prompt[0] = '\0';

    for (int i = 1; i < argc; ++i) {
      if (i > 1) {
        strncat(prompt, " ", sizeof(prompt) - strlen(prompt) - 1);
      }

      strncat(prompt, argv[i], sizeof(prompt) - strlen(prompt) - 1);
    }
  } else {
    printf("Prompt> ");

    if (!fgets(prompt, sizeof(prompt), stdin)) {
      return 1;
    }

    prompt[strcspn(prompt, "\r\n")] = '\0';
  }

  printf("\nPROMPT\n");
  printf("  %s\n", prompt);

  /*
   * Bring up the NLP runtime so this executable is already
   * connected to the semantic frontend boundary.
   */
  Arena arena;
  arena_init(&arena, 4096);

  NlpContext nlp;
  memset(&nlp, 0, sizeof(nlp));

  NlpConfig config;
  nlp_config_default(&config);

  if (nlp_init(&nlp, &arena, &config) == NLP_STATUS_OK) {
    if (nlp_load_model(&nlp, "models/distilbert_ner/model.onnx") ==
        NLP_STATUS_OK) {
      printf("\nNLP\n");
      printf("  runtime      = READY\n");
      printf("  semantic pass = pending\n");
    }
  }

  PromptSpec spec;

  if (!prompt_parse(prompt, &spec)) {
    fprintf(stderr, "\nPROMPT PARSE FAILED\n"
                    "V1 currently accepts a resistor-divider request.\n");

    nlp_free(&nlp);
    arena_destroy(&arena);

    return 1;
  }

  printf("\nSPEC\n");
  printf("  topology       = resistor_divider\n");
  printf("  input voltage  = %.6f V\n", spec.input_voltage_v);
  printf("  target voltage = %.6f V\n", spec.target_voltage_v);
  printf("  Rtop           = %.6f ohm\n", spec.r_top_ohm);
  printf("  Rbottom        = %.6f ohm\n", spec.r_bottom_ohm);

  Design design;

  if (design_init(&design) != 0) {
    fprintf(stderr, "\nDESIGN INIT FAILED\n");

    nlp_free(&nlp);
    arena_destroy(&arena);

    return 1;
  }

  if (!build_design(&spec, &design)) {
    fprintf(stderr, "\nDESIGN BUILD FAILED\n");

    design_free(&design);
    nlp_free(&nlp);
    arena_destroy(&arena);

    return 1;
  }

  printf("\nDESIGN\n");
  printf("  components = %zu\n", vec_len(design.components));
  printf("  nets       = %zu\n", vec_len(design.nets));

  for (size_t i = 0; i < vec_len(design.components); ++i) {

    const Component *component = &design.components[i];

    printf("  %s = %.6f ohm\n", component->reference, component->value);
  }

  int solved = solve_design(&spec, &design);

  design_free(&design);
  nlp_free(&nlp);
  arena_destroy(&arena);

  return solved ? 0 : 2;
}
