#include "IR.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_parse_and_serialize(void)
{
    Arena arena;
    arena_init(&arena, 256);

    const char source[] =
        "# first-line comment\n"
        "(root (at 0 3.81 270) (name \"R\\\"1\\\\x\\nline\") ; tail comment\n"
        "      (flag yes))\n";
    IRError error;
    IRNode *root = ir_parse(&arena, source, sizeof(source) - 1, &error);
    assert(root != NULL);
    assert(error.code == IR_STATUS_OK);
    assert(root->kind == IR_NODE_LIST);
    assert(ir_validate_tree(root, &error) == IR_STATUS_OK);

    const IRNode *at = ir_list_find(root, "at");
    assert(at != NULL);
    assert(ir_list_length(at) == 4); /* head + three number atoms */
    const IRNode *y = ir_child_at(at, 2);
    double y_value = 0.0;
    assert(y && strcmp(y->text, "3.81") == 0);
    assert(ir_atom_to_double(y, &y_value) == IR_STATUS_OK);
    assert(fabs(y_value - 3.81) < 1e-12);

    const IRNode *name = ir_list_find(root, "name");
    assert(name != NULL);
    const IRNode *string = ir_child_at(name, 1);
    assert(string && string->kind == IR_NODE_STRING);
    assert(strcmp(string->text, "R\"1\\x\nline") == 0);

    IRWriteOptions compact = { .pretty = false, .indent_width = 0, .final_newline = false };
    char *output = NULL;
    size_t output_len = 0;
    assert(ir_to_string(root, &compact, &output, &output_len, &error) == IR_STATUS_OK);
    assert(output_len == strlen(output));
    assert(strstr(output, "(at 0 3.81 270)") != NULL);
    assert(strstr(output, "\\\"") != NULL);

    Arena arena2;
    arena_init(&arena2, 128);
    IRNode *round_trip = ir_parse(&arena2, output, output_len, &error);
    assert(round_trip != NULL);
    assert(ir_validate_tree(round_trip, &error) == IR_STATUS_OK);
    const IRNode *name2 = ir_list_find(round_trip, "name");
    assert(name2 != NULL);
    assert(strcmp(ir_child_at(name2, 1)->text, string->text) == 0);

    ir_string_free(output);
    arena_destroy(&arena2);
    arena_destroy(&arena);
}

static void test_numbers(void)
{
    Arena arena;
    arena_init(&arena, 256);
    IRError error;

    IRNode *i = ir_node_atom_i64(&arena, INT64_MIN);
    int64_t iv = 0;
    assert(i && ir_atom_to_i64(i, &iv) == IR_STATUS_OK && iv == INT64_MIN);

    IRNode *u = ir_node_atom_u64(&arena, UINT64_MAX);
    uint64_t uv = 0;
    assert(u && ir_atom_to_u64(u, &uv) == IR_STATUS_OK && uv == UINT64_MAX);

    const double values[] = { 0.0, -0.0, 1.0, -12.5, 1000.0, 0.000001, 1.0e-20, 1.0e30 };
    for (size_t n = 0; n < sizeof(values) / sizeof(values[0]); ++n) {
        IRNode *node = ir_node_atom_double(&arena, values[n]);
        double parsed = NAN;
        assert(node != NULL);
        assert(strchr(node->text, 'e') == NULL && strchr(node->text, 'E') == NULL);
        assert(ir_atom_to_double(node, &parsed) == IR_STATUS_OK);
        if (values[n] == 0.0) assert(parsed == 0.0);
        else assert(fabs((parsed - values[n]) / values[n]) < 1e-15);
    }

    IRNode *bad = ir_node_atom(&arena, "1.2.3");
    double dv = 0.0;
    assert(bad && ir_atom_to_double(bad, &dv) == IR_STATUS_INVALID_NUMBER);
    IRNode *overflow = ir_node_atom(&arena, "1e9999");
    assert(overflow && ir_atom_to_double(overflow, &dv) == IR_STATUS_RANGE_ERROR);
    IRNode *truth = ir_node_atom_bool(&arena, true);
    bool bv = false;
    assert(truth && ir_atom_to_bool(truth, &bv) == IR_STATUS_OK && bv);

    (void)error;
    arena_destroy(&arena);
}

static void test_ownership_clone_and_find(void)
{
    Arena arena;
    arena_init(&arena, 128);
    IRError error;

    IRNode *root = ir_node_list_named(&arena, "root");
    IRNode *meta = ir_node_list_named(&arena, "meta");
    IRNode *value = ir_node_string(&arena, "Example");
    assert(root && meta && value);
    assert(ir_list_append(meta, value) == IR_STATUS_OK);
    assert(ir_list_append(root, meta) == IR_STATUS_OK);
    assert(ir_list_append(root, meta) == IR_STATUS_OWNERSHIP_ERROR);
    assert(ir_list_append(meta, root) == IR_STATUS_CYCLE);

    IRNode *copy = ir_node_clone(&arena, meta, &error);
    assert(copy && error.code == IR_STATUS_OK);
    assert(copy != meta && copy->first_child != meta->first_child);
    assert(ir_list_append(root, copy) == IR_STATUS_OK);
    assert(ir_list_find_named(root, "meta", "Example") == meta);
    assert(ir_validate_tree(root, &error) == IR_STATUS_OK);

    assert(ir_node_detach(copy) == IR_STATUS_OK);
    assert(copy->parent == NULL && copy->next == NULL);
    assert(ir_validate_tree(root, &error) == IR_STATUS_OK);
    assert(ir_list_append(root, copy) == IR_STATUS_OK);

    arena_destroy(&arena);
}

static void test_parse_errors(void)
{
    Arena arena;
    arena_init(&arena, 64);
    IRError error;

    const char trailing[] = "(a) (b)";
    assert(ir_parse(&arena, trailing, sizeof(trailing) - 1, &error) == NULL);
    assert(error.code == IR_STATUS_PARSE_ERROR);
    assert(error.offset == 4);

    const char missing[] = "(a \"oops)";
    assert(ir_parse(&arena, missing, sizeof(missing) - 1, &error) == NULL);
    assert(error.code == IR_STATUS_PARSE_ERROR);

    const char unmatched[] = ")";
    assert(ir_parse(&arena, unmatched, sizeof(unmatched) - 1, &error) == NULL);
    assert(error.code == IR_STATUS_PARSE_ERROR);

    const char malformed_hex[] = "(x \"\\xG0\")";
    assert(ir_parse(&arena, malformed_hex, sizeof(malformed_hex) - 1, &error) == NULL);
    assert(error.code == IR_STATUS_PARSE_ERROR);

    arena_destroy(&arena);
}

static void test_kicad_symbol_fixtures(void)
{
#ifndef IR_TEST_FIXTURE_DIR
#define IR_TEST_FIXTURE_DIR "tests/fixtures/kicad9/symbols"
#endif
    Arena arena;
    arena_init(&arena, 512);
    IRError error;
    const char *paths[] = {
        IR_TEST_FIXTURE_DIR "/R.symbol.txt",
        IR_TEST_FIXTURE_DIR "/C.symbol.txt"
    };
    const char *expected_names[] = { "R", "C" };
    const char *expected_unit_names[] = { "R_1_1", "C_1_1" };

    for (size_t i = 0; i < 2; ++i) {
        IRNode *symbol = ir_parse_file(&arena, paths[i], &error);
        assert(symbol != NULL);
        assert(error.code == IR_STATUS_OK);
        assert(ir_validate_tree(symbol, &error) == IR_STATUS_OK);
        assert(strcmp(ir_child_at(symbol, 0)->text, "symbol") == 0);
        assert(strcmp(ir_child_at(symbol, 1)->text, expected_names[i]) == 0);

        const IRNode *unit = ir_list_find_named(symbol, "symbol", expected_unit_names[i]);
        assert(unit != NULL);
        const IRNode *pin = ir_list_find(unit, "pin");
        assert(pin != NULL);
        const IRNode *at = ir_list_find(pin, "at");
        const IRNode *number = ir_list_find(pin, "number");
        assert(at != NULL && number != NULL);
        assert(ir_list_length(at) == 4); /* head + x + y + angle */

        char *serialized = NULL;
        size_t serialized_len = 0;
        IRWriteOptions compact = { .pretty = false, .indent_width = 0, .final_newline = false };
        assert(ir_to_string(symbol, &compact, &serialized, &serialized_len, &error) == IR_STATUS_OK);
        IRNode *round_trip = ir_parse(&arena, serialized, serialized_len, &error);
        assert(round_trip != NULL);
        assert(ir_validate_tree(round_trip, &error) == IR_STATUS_OK);
        ir_string_free(serialized);
    }

    arena_destroy(&arena);
}

int main(void)
{
    test_parse_and_serialize();
    test_numbers();
    test_ownership_clone_and_find();
    test_parse_errors();
    test_kicad_symbol_fixtures();
    puts("IR tests passed");
    return 0;
}
