#include "IR.h"
#include "arena.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int64_t parse(Arena *a, const char *t)
{
    IRNode *n = ir_node_atom(a, t);
    int64_t v = 0;
    assert(n && ir_atom_to_nm(n, &v) == IR_STATUS_OK);
    return v;
}

static void roundtrip(Arena *a, int64_t nm, const char *expect)
{
    IRNode *n = ir_node_atom_nm(a, nm);
    assert(n && strcmp(n->text, expect) == 0);
    int64_t back = 0;
    assert(ir_atom_to_nm(n, &back) == IR_STATUS_OK && back == nm);
}

int main(void)
{
    Arena a;
    arena_init(&a, 4096);
    assert(parse(&a, "0.15") == 150000);
    assert(parse(&a, "3.81") == 3810000);
    assert(parse(&a, "-3.81") == -3810000);
    assert(parse(&a, "1") == 1000000);
    assert(parse(&a, "0.825") == 825000);
    assert(parse(&a, "0.0000004") == 0);        /* 7th digit < 5 */
    assert(parse(&a, "0.0000005") == 1);        /* half away from zero */
    assert(parse(&a, "-0.0000005") == -1);
    assert(parse(&a, "166.370001") == 166370001);
    assert(parse(&a, ".5") == 500000);

    int64_t v;
    IRNode *bad = ir_node_atom(&a, "1e3");
    assert(ir_atom_to_nm(bad, &v) == IR_STATUS_INVALID_NUMBER);
    bad = ir_node_atom(&a, "abc");
    assert(ir_atom_to_nm(bad, &v) == IR_STATUS_INVALID_NUMBER);
    bad = ir_node_atom(&a, "99999999999999999999");
    assert(ir_atom_to_nm(bad, &v) == IR_STATUS_RANGE_ERROR);

    roundtrip(&a, 0, "0");
    roundtrip(&a, 150000, "0.15");
    roundtrip(&a, -3810000, "-3.81");
    roundtrip(&a, 1000000, "1");
    roundtrip(&a, 1, "0.000001");
    roundtrip(&a, 166370001, "166.370001");
    roundtrip(&a, INT64_MIN + 1, "-9223372036854.775807");

    arena_destroy(&a);
    puts("IR NM TEST SUITE: PASS");
    return 0;
}
