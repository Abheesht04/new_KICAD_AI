#include "engineering_loop.h"
#include "engineering_loop_json.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void build_voltage_divider(Arena *arena, CopperTopology *topology,
                                  PartRecord *resistor,
                                  PartPinMap *pins, PartRating *ratings) {
    pins[0] = (PartPinMap){"positive", "1", "1"};
    pins[1] = (PartPinMap){"negative", "2", "2"};
    ratings[0] = (PartRating){PART_RATING_RESISTANCE, PART_BOUND_NOMINAL,
                              INT64_C(1000000000), PART_UNIT_UOHM, {0}};
    ratings[1] = (PartRating){PART_RATING_VOLTAGE, PART_BOUND_MAXIMUM,
                              INT64_C(12000000), PART_UNIT_UV, {0}};
    ratings[2] = (PartRating){PART_RATING_CURRENT, PART_BOUND_MAXIMUM,
                              INT64_C(10000), PART_UNIT_UA, {0}};
    ratings[3] = (PartRating){PART_RATING_POWER, PART_BOUND_MAXIMUM,
                              INT64_C(100000), PART_UNIT_UW, {0}};
    memset(resistor, 0, sizeof(*resistor));
    resistor->id = "RES_1K";
    resistor->kind = PART_KIND_RESISTOR;
    resistor->manufacturer = "TEST";
    resistor->mpn = "R-1K-TEST";
    resistor->symbol_id = "Device:R";
    resistor->footprint_id = "Resistor_SMD:R_0603_1608Metric";
    resistor->pins = pins;
    resistor->pin_count = 2u;
    resistor->ratings = ratings;
    resistor->rating_count = 4u;

    CopperTopologyStatus ts = copper_topology_init(arena, 2u, 4u, 2u, 4u, topology);
    assert(ts == COPPER_TOPOLOGY_OK);
    CopperTopologyComponentSpec source = {NULL, "V1", "5V", NULL, "Simulation_SPICE:VDC", NULL};
    CopperTopologyPinSpec source_pins[2] = {
        {"1", "+", "power_out", NULL}, {"2", "-", "power_in", NULL}
    };
    assert(copper_topology_add_component(topology, &source, source_pins, 2u, NULL) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_add_resolved_part(topology, resistor, "R1", NULL, "1k", NULL) == COPPER_TOPOLOGY_OK);
    CopperTopologyNetSpec vcc = {"1", "VCC", "Default"};
    CopperTopologyNetSpec gnd = {"2", "GND", "Default"};
    size_t vcc_index = 0, gnd_index = 0;
    assert(copper_topology_add_net(topology, &vcc, &vcc_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_add_net(topology, &gnd, &gnd_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(topology, "V1", "1", vcc_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(topology, "V1", "2", gnd_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(topology, "R1", "1", vcc_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(topology, "R1", "2", gnd_index) == COPPER_TOPOLOGY_OK);
}

static void test_dc_loop_and_json(void) {
    Arena arena; arena_init(&arena, 32768u);
    CopperTopology topology; PartRecord resistor; PartPinMap pins[2]; PartRating ratings[4];
    build_voltage_divider(&arena, &topology, &resistor, pins, ratings);

    EngineeringComponentBinding bindings[2];
    memset(bindings, 0, sizeof(bindings));
    bindings[0].reference = "V1";
    bindings[0].has_element_template = true;
    bindings[0].element_template.kind = CIRCUIT_VOLTAGE_SOURCE;
    bindings[0].element_template.value_si = 5.0;
    bindings[0].element_template.has_dc_value = true;
    bindings[0].pin_count = 2u;
    bindings[0].pin_numbers[0] = "1";
    bindings[0].pin_numbers[1] = "2";
    bindings[1].reference = "R1";
    bindings[1].part = &resistor;

    EngineeringLoopOptions options = engineering_loop_default_options();
    EngineeringLoopResult result;
    assert(engineering_loop_run(&arena, &topology, bindings, 2u,
                                NULL, NULL, NULL, &options, &result) == ENGINEERING_LOOP_OK);
    assert(result.circuit_built);
    assert(result.simulation_attempted);
    assert(result.simulation_converged);
    assert(fabs(result.operating_point.node_voltages_v[1] - 5.0) < 1e-6);
    assert(fabs(result.stresses[1].current_a - 0.005) < 1e-6);
    assert(fabs(result.stresses[1].power_w - 0.025) < 1e-6);
    assert(result.status == ENGINEERING_LOOP_PARTIAL); /* no board/DFM/Gerber release */
    assert(!result.design_ready);

    size_t json_length = 0;
    char *json = engineering_loop_json_to_string(&topology, &result, &json_length);
    assert(json && json_length > 100u);
    assert(strstr(json, "copper.engineering-design") != NULL);
    assert(strstr(json, "DC_OPERATING_POINT") != NULL);
    assert(strstr(json, "MANUFACTURING_EXPORT_NOT_INTEGRATED") != NULL);
    free(json);
    json = NULL;

    G2Point route_points[2] = {{1000000, 2000000}, {3000000, 4000000}};
    EngineeringRouteProposal route;
    memset(&route, 0, sizeof(route));
    route.topology_net_index = 0u;
    route.net_name = "VCC";
    route.status = ROUTER_OK;
    route.route.status = ROUTER_OK;
    route.route.points = route_points;
    route.route.point_count = 2u;
    route.route.expanded_nodes = 12u;
    route.route.cost = 2000u;
    strcpy(route.route.message, "fixture route proposal");
    result.routes_attempted = true;
    result.routes = &route;
    result.route_count = 1u;
    json = engineering_loop_json_to_string(&topology, &result, &json_length);
    assert(json && strstr(json, "\"routes_attempted\":true") != NULL);
    assert(strstr(json, "\"net_name\":\"VCC\"") != NULL);
    assert(strstr(json, "\"x_nm\":1000000") != NULL);
    free(json);

    size_t report_length = 0u;
    char *report = engineering_loop_report_to_text(&result, &report_length);
    assert(report && report_length > 0u);
    assert(strstr(report, "COMPONENT STRESS") != NULL);
    assert(strstr(report, "ROUTE PROPOSALS (not committed)") != NULL);
    free(report);
    arena_destroy(&arena);
}

static void test_rating_violation_rejects(void) {
    Arena arena; arena_init(&arena, 32768u);
    CopperTopology topology; PartRecord resistor; PartPinMap pins[2]; PartRating ratings[4];
    build_voltage_divider(&arena, &topology, &resistor, pins, ratings);
    ratings[3].value = INT64_C(10000); /* 10 mW; circuit dissipates 25 mW */

    EngineeringComponentBinding bindings[2];
    memset(bindings, 0, sizeof(bindings));
    bindings[0].reference = "V1";
    bindings[0].has_element_template = true;
    bindings[0].element_template.kind = CIRCUIT_VOLTAGE_SOURCE;
    bindings[0].element_template.value_si = 5.0;
    bindings[0].element_template.has_dc_value = true;
    bindings[0].pin_count = 2u;
    bindings[0].pin_numbers[0] = "1";
    bindings[0].pin_numbers[1] = "2";
    bindings[1].reference = "R1";
    bindings[1].part = &resistor;

    EngineeringLoopOptions options = engineering_loop_default_options();
    EngineeringLoopResult result;
    assert(engineering_loop_run(&arena, &topology, bindings, 2u,
                                NULL, NULL, NULL, &options, &result) == ENGINEERING_LOOP_OK);
    assert(result.simulation_converged);
    assert(result.stresses[1].power_status == ENGINEERING_FINDING_FAIL);
    assert(result.status == ENGINEERING_LOOP_REJECTED);
    arena_destroy(&arena);
}

static void test_missing_binding_is_partial(void) {
    Arena arena; arena_init(&arena, 16384u);
    CopperTopology topology;
    assert(copper_topology_init(&arena, 1u, 2u, 1u, 2u, &topology) == COPPER_TOPOLOGY_OK);
    CopperTopologyComponentSpec spec = {NULL, "R1", "1k", "RES_1K", "Device:R", NULL};
    CopperTopologyPinSpec p[2] = {{"1", "", "passive", NULL}, {"2", "", "passive", NULL}};
    assert(copper_topology_add_component(&topology, &spec, p, 2u, NULL) == COPPER_TOPOLOGY_OK);
    CopperTopologyNetSpec n = {"1", "N1", "Default"}; size_t ni = 0;
    assert(copper_topology_add_net(&topology, &n, &ni) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(&topology, "R1", "1", ni) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(&topology, "R1", "2", ni) == COPPER_TOPOLOGY_OK);
    EngineeringLoopOptions options = engineering_loop_default_options();
    EngineeringLoopResult result;
    assert(engineering_loop_run(&arena, &topology, NULL, 0u, NULL, NULL, NULL,
                                &options, &result) == ENGINEERING_LOOP_OK);
    assert(!result.circuit_built);
    assert(!result.simulation_attempted);
    assert(result.status == ENGINEERING_LOOP_PARTIAL);
    arena_destroy(&arena);
}

int main(void) {
    test_dc_loop_and_json();
    test_rating_violation_rejects();
    test_missing_binding_is_partial();
    puts("engineering loop tests: PASS");
    return 0;
}
