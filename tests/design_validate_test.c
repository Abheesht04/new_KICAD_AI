#include "design_validate.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "component.h"
#include "diagnostic.h"
#include "range.h"

static void make_resistor(Component *r, const char *reference) {
    component_init(r);
    (void)snprintf(r->reference, sizeof(r->reference), "%s", reference);
    r->model.kind = COMPONENT_RESISTOR;
    r->model.data.resistor.resistance_ohm = range_exact(10000.0);
    component_set_value(r, 10000.0, 0.0);
    assert(component_add_pin(r, 1, "1") == 0);
    assert(component_add_pin(r, 2, "2") == 0);
}

int main(void) {
    Design design;
    assert(design_init(&design) == 0);
    Component r;
    make_resistor(&r, "R1");
    uint32_t rid = 0;
    assert(design_add_component(&design, &r, &rid) == 0);
    component_free(&r);

    DiagnosticList diag;
    diagnostic_list_init(&diag);
    int errors = design_validate_structure(&design, &diag);
    assert(errors == 2); /* two unconnected pins are explicit errors */
    diagnostic_list_free(&diag);
    design_free(&design);

    /* Both terminals on one net is an electrical short, not a valid resistor. */
    assert(design_init(&design) == 0);
    make_resistor(&r, "R1");
    assert(design_add_component(&design, &r, &rid) == 0);
    component_free(&r);
    uint32_t net_id = UINT32_MAX;
    assert(design_add_net(&design, "N_SHORT", &net_id) == 0);
    assert(design_connect_pin(&design, rid, 1, net_id) == 0);
    assert(design_connect_pin(&design, rid, 2, net_id) == 0);
    diagnostic_list_init(&diag);
    errors = design_validate_structure(&design, &diag);
    assert(errors >= 1);
    int saw_short = 0;
    for (size_t i = 0; i < diagnostic_count(&diag); ++i) {
        const Diagnostic *d = diagnostic_get(&diag, i);
        if (d && d->rule_name && strcmp(d->rule_name, "shorted_two_terminal_component") == 0)
            saw_short = 1;
    }
    assert(saw_short);
    diagnostic_list_free(&diag);
    design_free(&design);

    puts("PASS: structural validator catches floating pins and shorted two-terminal devices");
    return 0;
}
