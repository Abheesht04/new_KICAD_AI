#include <assert.h>
#include <stdio.h>

#include "design.h"
#include "node_map.h"

/*
 * Three-resistor chain:
 *
 *     VIN --[R1]-- N2 --[R2]-- N3 --[R3]-- GND
 *
 * Nets are added in this order: VIN, N2, N3, GND.
 * So net ids are 0, 1, 2, 3 respectively (design_add_net assigns
 * id = vec_len(design->nets) at push time).
 *
 * GND is net id 3 here -- deliberately NOT the first net added, so this
 * test actually exercises "skip whichever net is ground", not just
 * "skip net 0".
 */
static void build_three_resistor_chain(Design *design,
                                       uint32_t *vin_id,
                                       uint32_t *n2_id,
                                       uint32_t *n3_id,
                                       uint32_t *gnd_id)
{
    assert(design_init(design) == 0);

    assert(design_add_net(design, "VIN", vin_id) == 0);
    assert(design_add_net(design, "N2", n2_id) == 0);
    assert(design_add_net(design, "N3", n3_id) == 0);
    assert(design_add_net(design, "GND", gnd_id) == 0);
}

static void test_ground_skipped_and_compacted(void)
{
    Design design;
    uint32_t vin_id, n2_id, n3_id, gnd_id;
    build_three_resistor_chain(&design, &vin_id, &n2_id, &n3_id, &gnd_id);

    NodeMap map;
    assert(node_map_build(&design, gnd_id, &map) == true);

    /* 4 nets total, 3 of them non-ground */
    assert(map.net_count == 4);
    assert(map.unknown_count == 3);

    /* ground itself gets no row */
    assert(node_map_row(&map, gnd_id) == SIZE_MAX);

    /* the three non-ground nets get distinct, compact rows in 0..2,
     * in the order they appear in design->nets */
    size_t row_vin = node_map_row(&map, vin_id);
    size_t row_n2 = node_map_row(&map, n2_id);
    size_t row_n3 = node_map_row(&map, n3_id);

    assert(row_vin == 0);
    assert(row_n2 == 1);
    assert(row_n3 == 2);

    /* every non-ground row is < unknown_count, and all distinct */
    assert(row_vin < map.unknown_count);
    assert(row_n2 < map.unknown_count);
    assert(row_n3 < map.unknown_count);
    assert(row_vin != row_n2 && row_n2 != row_n3 && row_vin != row_n3);

    node_map_free(&map);
    design_free(&design);

    printf("PASS: ground skipped and remaining nets compacted\n");
}

static void test_out_of_range_lookup_is_safe(void)
{
    Design design;
    uint32_t vin_id, n2_id, n3_id, gnd_id;
    build_three_resistor_chain(&design, &vin_id, &n2_id, &n3_id, &gnd_id);

    NodeMap map;
    assert(node_map_build(&design, gnd_id, &map) == true);

    /* a net id that was never added */
    assert(node_map_row(&map, 999) == SIZE_MAX);

    node_map_free(&map);
    design_free(&design);

    printf("PASS: out-of-range net id lookup returns SIZE_MAX\n");
}

static void test_null_arguments_are_rejected(void)
{
    NodeMap map;
    assert(node_map_build(NULL, 0, &map) == false);

    Design design;
    uint32_t vin_id, n2_id, n3_id, gnd_id;
    build_three_resistor_chain(&design, &vin_id, &n2_id, &n3_id, &gnd_id);
    assert(node_map_build(&design, gnd_id, NULL) == false);
    design_free(&design);

    printf("PASS: NULL arguments rejected\n");
}

int main(void)
{
    test_ground_skipped_and_compacted();
    test_out_of_range_lookup_is_safe();
    test_null_arguments_are_rejected();

    printf("\n================================\n");
    printf("NODE_MAP TEST SUITE: PASS\n");
    printf("================================\n");
    return 0;
}
