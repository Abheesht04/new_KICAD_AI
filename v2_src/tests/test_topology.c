#ifdef NDEBUG
#undef NDEBUG
#endif
#include "topology.h"
#include "topology_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_topology_from_schematic_and_netlist(void) {
    Arena arena;
    SchematicIR schematic;
    SchematicDiag sd;
    KicadNetlist netlist;
    KicadNetlistDiag nd;
    CopperTopology topology;
    static const char net_text[] =
        "(export (version \"E\")"
        " (components (comp (ref \"R1\") (value \"10k\")"
        " (libsource (lib \"Device\") (part \"R\"))))"
        " (nets (net (code \"1\") (name \"SIGNAL\") (class \"Default\")"
        "   (node (ref \"R1\") (pin \"1\") (pintype \"passive\")))"
        "  (net (code \"2\") (name \"GND\") (class \"Default\")"
        "   (node (ref \"R1\") (pin \"2\") (pintype \"passive\")))))";
    const char *fixture = COPPER_TEST_SOURCE_DIR "/tests/fixtures/sample_resistor_lol/sample_resistor_lol.kicad_sch";
    arena_init(&arena, 65536);
    assert(schematic_load_kicad(fixture, &arena, &schematic, &sd) == SCHEMATIC_OK);
    assert(kicad_netlist_parse(&arena, net_text, sizeof(net_text)-1u, &netlist, &nd) == KICAD_NETLIST_OK);
    CopperTopologyStatus st = copper_topology_build(&arena, &schematic, &netlist, &topology);
    assert(st == COPPER_TOPOLOGY_OK);
    assert(topology.component_count == schematic.symbol_count);
    assert(topology.net_count == 2);
    assert(topology.connection_count == 2);
    const CopperTopologyTerminal *p1 = copper_topology_find_terminal(&topology, "R1", "1");
    const CopperTopologyTerminal *p2 = copper_topology_find_terminal(&topology, "R1", "2");
    assert(p1 && p1->net_index != COPPER_TOPOLOGY_NO_NET);
    assert(p2 && p2->net_index != COPPER_TOPOLOGY_NO_NET);
    assert(strcmp(topology.nets[p1->net_index].name, "SIGNAL") == 0);
    assert(strcmp(topology.nets[p2->net_index].name, "GND") == 0);
    size_t length = 0;
    char *json = copper_topology_json_to_string(&topology, &length);
    assert(json && length > 0);
    assert(strstr(json, "\"schema\":\"copper.topology\"") != NULL);
    assert(strstr(json, "\"reference\":\"R1\"") != NULL);
    free(json);
    arena_destroy(&arena);
}

static void test_topology_without_netlist_is_incomplete(void) {
    Arena arena;
    SchematicIR schematic;
    SchematicDiag diag;
    CopperTopology topology;
    const char *fixture = COPPER_TEST_SOURCE_DIR "/tests/fixtures/sample_resistor_lol/sample_resistor_lol.kicad_sch";
    arena_init(&arena, 65536);
    assert(schematic_load_kicad(fixture, &arena, &schematic, &diag) == SCHEMATIC_OK);
    assert(copper_topology_build(&arena, &schematic, NULL, &topology) == COPPER_TOPOLOGY_INCOMPLETE);
    assert(!topology.complete);
    assert(topology.component_count == schematic.symbol_count);
    assert(topology.net_count == 0);
    assert(topology.issue_count == 1);
    arena_destroy(&arena);
}


static void test_generated_topology_builder(void) {
    Arena arena;
    CopperTopology topology;
    CopperTopologyComponentSpec component = {
        "uuid-u1", "U1", "MCU", NULL, "MCU_Generic:MCU", "Package_QFP:TQFP-32"
    };
    CopperTopologyPinSpec pins[] = {
        {"1", "VDD", "power_in", "1"},
        {"2", "GPIO1", "bidirectional", "2"}
    };
    CopperTopologyNetSpec net = {"1", "VDD_3V3", "Power"};
    size_t net_index = 0;
    arena_init(&arena, 8192);
    assert(copper_topology_init(&arena, 2, 4, 2, 4, &topology) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_add_component(&topology, &component, pins, 2, NULL) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_add_net(&topology, &net, &net_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(&topology, "U1", "1", net_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_validate(&topology) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_find_terminal(&topology, "U1", "1")->net_index == net_index);
    assert(copper_topology_connect(&topology, "U1", "1", net_index) ==
           COPPER_TOPOLOGY_ERR_DUPLICATE_PIN_ASSIGNMENT);
    arena_destroy(&arena);
}


static void test_topology_from_resolved_catalogue_part(void) {
    Arena arena;
    CopperTopology topology;
    PartPinMap maps[2] = {
        {"anode", "1", "1"},
        {"cathode", "2", "2"}
    };
    PartRecord part;
    memset(&part, 0, sizeof(part));
    part.id = "led_red_0603";
    part.kind = PART_KIND_DIODE;
    part.symbol_id = "Device:LED";
    part.footprint_id = "LED_SMD:LED_0603_1608Metric";
    part.pins = maps;
    part.pin_count = 2;
    CopperTopologyNetSpec net = {"1", "LED_A", "Default"};
    size_t net_index;
    arena_init(&arena, 8192);
    assert(copper_topology_init(&arena, 1, 2, 1, 2, &topology) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_add_resolved_part(&topology, &part, "D1", "uuid-d1",
                                              "RED", NULL) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_add_net(&topology, &net, &net_index) == COPPER_TOPOLOGY_OK);
    assert(copper_topology_connect(&topology, "D1", "1", net_index) == COPPER_TOPOLOGY_OK);
    const CopperTopologyComponent *d1 = copper_topology_find_component(&topology, "D1");
    assert(d1 && strcmp(d1->part_id, "led_red_0603") == 0);
    const CopperTopologyTerminal *anode = copper_topology_find_terminal(&topology, "D1", "1");
    assert(anode && strcmp(anode->footprint_pad, "1") == 0);
    arena_destroy(&arena);
}

int main(void) {
    test_topology_from_schematic_and_netlist();
    test_topology_without_netlist_is_incomplete();
    test_generated_topology_builder();
    test_topology_from_resolved_catalogue_part();
    puts("Topology tests passed");
    return 0;
}
