#include "kicad_generic_provider.h"
#include "part_registry.h"
#include "range.h"
#include "vec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * ==========================================================================
 * Debug helpers
 * ==========================================================================
 */

static void print_separator(void) {
  printf("\n");
  printf("============================================================\n");
}

static void print_request(const char *label, const PartRequest *request) {
  printf("[%s] Request\n", label);

  printf("    role            : %s\n",
         request->role ? request->role : "<null>");

  printf("    value nominal   : %.9g\n", request->value.nominal);

  printf("    value minimum   : %.9g\n", request->value.minimum);

  printf("    value maximum   : %.9g\n", request->value.maximum);

  printf("    package         : %s\n",
         request->required_package ? request->required_package : "<null>");

  printf("    min voltage     : %.9g V\n", request->min_voltage_v);

  printf("    min current     : %.9g A\n", request->min_current_a);

  printf("    min power       : %.9g W\n", request->min_power_w);

  printf("    exact           : %s\n", request->exact ? "PRESENT" : "NULL");

  if (request->exact != NULL) {
    printf("    manufacturer    : %s\n", request->exact->manufacturer
                                             ? request->exact->manufacturer
                                             : "<null>");

    printf("    part number     : %s\n",
           request->exact->manufacturer_part_number
               ? request->exact->manufacturer_part_number
               : "<null>");
  }
}

static void print_result(const PartResult *result) {
  printf("[RESULT]\n");

  printf("    manufacturer    : %s\n",
         result->manufacturer ? result->manufacturer : "<none>");

  printf("    part number     : %s\n", result->manufacturer_part_number
                                           ? result->manufacturer_part_number
                                           : "<none>");

  printf("    package         : %s\n",
         result->package ? result->package : "<none>");

  printf("    symbol          : %s\n",
         result->symbol ? result->symbol : "<none>");

  printf("    footprint       : %s\n",
         result->footprint ? result->footprint : "<none>");
}

/*
 * ==========================================================================
 * Test provider
 *
 * This provider lets us SEE what request the registry actually gives it.
 *
 * exact != NULL
 *     -> Phase 1
 *
 * exact == NULL
 *     -> Phase 2
 *
 * It deliberately succeeds only during Phase 2.
 * ==========================================================================
 */

typedef struct {
  size_t calls;
  size_t exact_calls;
  size_t capability_calls;

} TraceProviderContext;

static int trace_provider_resolve(void *context, const PartRequest *request,
                                  PartResult *out) {
  TraceProviderContext *trace = (TraceProviderContext *)context;

  if (trace == NULL || request == NULL || out == NULL) {
    return 0;
  }

  trace->calls++;

  printf("\n");
  printf("[TRACE_PROVIDER] call #%zu\n", trace->calls);

  print_request("TRACE_PROVIDER", request);

  if (request->exact != NULL) {
    trace->exact_calls++;

    printf("[TRACE_PROVIDER] classification = PHASE 1 / EXACT\n");
    printf("[TRACE_PROVIDER] result         = NO MATCH\n");

    return 0;
  }

  trace->capability_calls++;

  printf("[TRACE_PROVIDER] classification = PHASE 2 / CAPABILITY\n");
  printf("[TRACE_PROVIDER] result         = MATCH\n");

  out->manufacturer = "TraceCorp";

  out->manufacturer_part_number = "TRACE-0603";

  out->package = "0603";

  out->symbol = "Trace:R";

  out->footprint = "Trace:R_0603";

  return 1;
}

static PartProvider make_trace_provider(TraceProviderContext *context) {
  PartProvider provider;

  provider.name = "trace_provider";

  provider.context = context;

  provider.resolve = trace_provider_resolve;

  return provider;
}

/*
 * ==========================================================================
 * Test 1
 *
 * Registry initialization and provider registration.
 * ==========================================================================
 */

static void test_registry_registration(void) {
  PartRegistry registry;

  TraceProviderContext context = {0, 0, 0};

  print_separator();

  printf("TEST 1: registry initialization / registration\n");

  assert(part_registry_init(&registry) == 0);

  printf("[REGISTRY_TEST] initial provider count = %zu\n",
         vec_len(registry.providers));

  assert(vec_len(registry.providers) == 0);

  assert(part_registry_register(&registry, make_trace_provider(&context)) == 0);

  printf("[REGISTRY_TEST] provider count after register = %zu\n",
         vec_len(registry.providers));

  assert(vec_len(registry.providers) == 1);

  printf("[REGISTRY_TEST] provider[0] = %s\n", registry.providers[0].name);

  assert(strcmp(registry.providers[0].name, "trace_provider") == 0);

  part_registry_free(&registry);

  printf("[TEST 1] PASS\n");
}

/*
 * ==========================================================================
 * Test 2
 *
 * Exact == NULL.
 *
 * Therefore Phase 1 must be skipped.
 * The trace provider should receive exactly one call, classified as
 * capability lookup.
 * ==========================================================================
 */

static void test_capability_lookup_skips_phase_one(void) {
  PartRegistry registry;

  TraceProviderContext context = {0, 0, 0};

  PartRequest request = {.role = "resistor",
                         .value = range_exact(10000.0),
                         .required_package = "0603",
                         .min_voltage_v = 24.0,
                         .min_current_a = 1.0,
                         .min_power_w = 0.25,
                         .exact = NULL};

  PartResult result;

  print_separator();

  printf("TEST 2: capability lookup with no exact identity\n");

  print_request("BEFORE_LOOKUP", &request);

  assert(part_registry_init(&registry) == 0);

  assert(part_registry_register(&registry, make_trace_provider(&context)) == 0);

  printf("\n[REGISTRY_TEST] calling part_registry_find()\n");

  assert(part_registry_find(&registry, &request, &result));

  printf("\n[REGISTRY_TEST] lookup returned SUCCESS\n");

  print_result(&result);

  printf("\n[REGISTRY_TEST] trace statistics\n");
  printf("    total calls      : %zu\n", context.calls);

  printf("    exact calls      : %zu\n", context.exact_calls);

  printf("    capability calls : %zu\n", context.capability_calls);

  /*
   * Phase 1 must not happen when exact == NULL.
   */
  assert(context.exact_calls == 0);

  /*
   * Exactly one capability lookup.
   */
  assert(context.capability_calls == 1);

  /*
   * Original request remains untouched.
   */
  assert(request.exact == NULL);

  part_registry_free(&registry);

  printf("[TEST 2] PASS\n");
}

/*
 * ==========================================================================
 * Test 3
 *
 * Exact identity exists, but trace provider deliberately rejects it.
 *
 * Registry must then create a LOCAL capability request:
 *
 *     original request:
 *         exact = &identity
 *
 *     phase-2 request:
 *         exact = NULL
 *
 * The trace provider exposes this directly.
 * ==========================================================================
 */

static void test_exact_to_capability_fallback(void) {
  PartRegistry registry;

  TraceProviderContext context = {0, 0, 0};

  PartIdentity identity = {.manufacturer = "FakeManufacturer",
                           .manufacturer_part_number = "FAKE-123"};

  PartRequest request = {.role = "resistor",
                         .value = range_exact(4700.0),
                         .required_package = "0603",
                         .min_voltage_v = 24.0,
                         .min_current_a = 1.0,
                         .min_power_w = 0.25,
                         .exact = &identity};

  PartResult result;

  print_separator();

  printf("TEST 3: exact lookup -> capability fallback\n");

  print_request("BEFORE_LOOKUP", &request);

  printf("\n[REGISTRY_TEST] original exact pointer = %p\n",
         (void *)request.exact);

  assert(part_registry_init(&registry) == 0);

  assert(part_registry_register(&registry, make_trace_provider(&context)) == 0);

  printf("\n[REGISTRY_TEST] calling part_registry_find()\n");

  assert(part_registry_find(&registry, &request, &result));

  printf("\n[REGISTRY_TEST] lookup returned SUCCESS\n");

  print_result(&result);

  printf("\n[REGISTRY_TEST] trace statistics\n");

  printf("    total calls      : %zu\n", context.calls);

  printf("    exact calls      : %zu\n", context.exact_calls);

  printf("    capability calls : %zu\n", context.capability_calls);

  /*
   * We expect:
   *
   *     call 1 -> Phase 1
   *     call 2 -> Phase 2
   */
  assert(context.calls == 2);

  assert(context.exact_calls == 1);

  assert(context.capability_calls == 1);

  /*
   * CRITICAL:
   *
   * The original request still contains the identity.
   */
  printf("\n[REGISTRY_TEST] original exact pointer after lookup = %p\n",
         (void *)request.exact);

  assert(request.exact == &identity);

  /*
   * And it must still contain the same identity.
   */
  assert(strcmp(request.exact->manufacturer, "FakeManufacturer") == 0);

  assert(strcmp(request.exact->manufacturer_part_number, "FAKE-123") == 0);

  part_registry_free(&registry);

  printf("[TEST 3] PASS\n");
}

/*
 * ==========================================================================
 * Test 4
 *
 * Exact match succeeds in Phase 1.
 *
 * This proves the registry returns immediately and never asks the same
 * provider for Phase 2.
 * ==========================================================================
 */

typedef struct {
  size_t calls;
  size_t exact_calls;
  size_t capability_calls;

} ExactMatchContext;

static int exact_match_resolve(void *context, const PartRequest *request,
                               PartResult *out) {
  ExactMatchContext *trace = (ExactMatchContext *)context;

  if (trace == NULL || request == NULL || out == NULL) {
    return 0;
  }

  trace->calls++;

  printf("\n");
  printf("[EXACT_PROVIDER] call #%zu\n", trace->calls);

  print_request("EXACT_PROVIDER", request);

  if (request->exact == NULL) {
    trace->capability_calls++;

    printf("[EXACT_PROVIDER] capability request received unexpectedly\n");

    return 0;
  }

  trace->exact_calls++;

  printf("[EXACT_PROVIDER] exact identity matched\n");

  out->manufacturer = request->exact->manufacturer;

  out->manufacturer_part_number = request->exact->manufacturer_part_number;

  out->package = "0603";

  out->symbol = "Exact:Device";

  out->footprint = "Exact:Footprint";

  return 1;
}

static PartProvider make_exact_match_provider(ExactMatchContext *context) {
  PartProvider provider;

  provider.name = "exact_match_provider";

  provider.context = context;

  provider.resolve = exact_match_resolve;

  return provider;
}

static void test_exact_match_stops_search(void) {
  PartRegistry registry;

  ExactMatchContext context = {0, 0, 0};

  PartIdentity identity = {.manufacturer = "RealManufacturer",
                           .manufacturer_part_number = "REAL-123"};

  PartRequest request = {.role = "resistor",
                         .value = range_exact(10000.0),
                         .required_package = "0603",
                         .min_voltage_v = 24.0,
                         .min_current_a = 1.0,
                         .min_power_w = 0.25,
                         .exact = &identity};

  PartResult result;

  print_separator();

  printf("TEST 4: successful exact match stops search\n");

  assert(part_registry_init(&registry) == 0);

  assert(part_registry_register(&registry,
                                make_exact_match_provider(&context)) == 0);

  printf("[REGISTRY_TEST] calling lookup\n");

  assert(part_registry_find(&registry, &request, &result));

  printf("\n[REGISTRY_TEST] selected result\n");

  print_result(&result);

  printf("\n[REGISTRY_TEST] trace statistics\n");

  printf("    total calls      : %zu\n", context.calls);

  printf("    exact calls      : %zu\n", context.exact_calls);

  printf("    capability calls : %zu\n", context.capability_calls);

  assert(context.calls == 1);

  assert(context.exact_calls == 1);

  assert(context.capability_calls == 0);

  assert(strcmp(result.symbol, "Exact:Device") == 0);

  /*
   * Caller request remains untouched.
   */
  assert(request.exact == &identity);

  part_registry_free(&registry);

  printf("[TEST 4] PASS\n");
}

/*
 * ==========================================================================
 * Test 5
 *
 * Exercise the REAL KiCad generic provider.
 *
 * This is not the fake trace provider.
 * We are checking the actual provider currently in the project.
 * ==========================================================================
 */

static void test_real_kicad_provider(void) {
  PartRegistry registry;

  PartRequest request = {.role = "resistor",
                         .value = range_exact(10000.0),
                         .required_package = "0603",
                         .min_voltage_v = 24.0,
                         .min_current_a = 1.0,
                         .min_power_w = 0.25,
                         .exact = NULL};

  PartResult result;

  print_separator();

  printf("TEST 5: REAL KiCad generic provider lookup\n");

  print_request("KICAD_REQUEST", &request);

  assert(part_registry_init(&registry) == 0);

  assert(part_registry_register(&registry, kicad_generic_provider()) == 0);

  printf("\n[KICAD_TEST] registered provider = %s\n",
         registry.providers[0].name);

  printf("[KICAD_TEST] provider count = %zu\n", vec_len(registry.providers));

  printf("\n[KICAD_TEST] calling part_registry_find()\n");

  assert(part_registry_find(&registry, &request, &result));

  printf("\n[KICAD_TEST] REAL PROVIDER RESOLVED\n");

  print_result(&result);

  assert(result.symbol != NULL);
  assert(result.footprint != NULL);

  assert(strcmp(result.symbol, "Device:R") == 0);

  assert(strcmp(result.footprint, "Resistor_SMD:R_0603_1608Metric") == 0);

  /*
   * Real provider did not modify caller request.
   */
  assert(request.exact == NULL);

  part_registry_free(&registry);

  printf("[TEST 5] PASS\n");
}

/*
 * ==========================================================================
 * Test 6
 *
 * Real provider cannot satisfy the package.
 * ==========================================================================
 */

static void test_real_kicad_no_match(void) {
  PartRegistry registry;

  PartRequest request = {.role = "resistor",
                         .value = range_exact(10000.0),
                         .required_package = "THIS_PACKAGE_DOES_NOT_EXIST",
                         .min_voltage_v = 24.0,
                         .min_current_a = 1.0,
                         .min_power_w = 0.25,
                         .exact = NULL};

  PartResult result;

  print_separator();

  printf("TEST 6: REAL KiCad provider -> no match\n");

  print_request("KICAD_REQUEST", &request);

  assert(part_registry_init(&registry) == 0);

  assert(part_registry_register(&registry, kicad_generic_provider()) == 0);

  printf("\n[KICAD_TEST] calling part_registry_find()\n");

  assert(!part_registry_find(&registry, &request, &result));

  printf("\n[KICAD_TEST] provider returned NO MATCH\n");

  part_registry_free(&registry);

  printf("[TEST 6] PASS\n");
}

/*
 * ==========================================================================
 * Main
 * ==========================================================================
 */

int main(void) {
  printf("\n");
  printf("############################################################\n");
  printf("#               PART REGISTRY DEBUG SUITE                 #\n");
  printf("############################################################\n");

  test_registry_registration();

  test_capability_lookup_skips_phase_one();

  test_exact_to_capability_fallback();

  test_exact_match_stops_search();

  test_real_kicad_provider();

  test_real_kicad_no_match();

  print_separator();

  printf("PART REGISTRY TEST SUITE: PASS\n");

  printf("============================================================\n");

  return 0;
}
