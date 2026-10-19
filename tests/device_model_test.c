/* Device model contract v1: registration rules, parameter validation,
 * capability queries, terminal pin names, and that built-in stamps solve a
 * circuit to its closed-form answer. Specs built here are test-only. */
#include "model_registry.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int dummy_stamp(MnaCircuit *m, const size_t *n, const double *p, const DeviceStampContext *c)
{
    (void)m, (void)n, (void)p, (void)c;
    return 1;
}

static int dummy_commit(const MnaResult *r, const size_t *n, const double *p, const DeviceStampContext *c, double *s)
{
    (void)r, (void)n, (void)p, (void)c;
    s[0] = 0;
    return 1;
}

static const DeviceTerminal TWO[] = {{"a", "terminal a", NULL}, {"b", "terminal b", NULL}};
static const DeviceTerminal DUP[] = {{"a", "terminal a", NULL}, {"a", "again", NULL}};
static const DeviceParam GOOD[] = {{"r", DEVICE_UNIT_OHM, 0.0, 10.0, 1, 1, 5.0, "test resistance"}};
static const DeviceParam BAD_RANGE[] = {{"r", DEVICE_UNIT_OHM, 0.0, INFINITY, 0, 0, 0.0, "infinite range"}};
static const DeviceParam BAD_DEFAULT[] = {{"r", DEVICE_UNIT_OHM, 0.0, 10.0, 1, 1, 0.0, "default on exclusive bound"}};
static const DeviceParam BAD_UNIT[] = {{"r", DEVICE_UNIT_INVALID, 0.0, 10.0, 0, 0, 0.0, "no unit"}};

static DeviceModelSpec valid_spec(void)
{
    DeviceModelSpec s = {"test.two", 1, "resistor", "test-only spec", "none", TWO, 2, GOOD, 1, 0,
                         DEVICE_CAP_DC | DEVICE_CAP_TRANSIENT, 0, dummy_stamp, dummy_stamp, NULL};
    return s;
}

static void expect_invalid(DeviceModelSpec s, const char *why)
{
    char err[256] = "";
    if (device_model_validate_spec(&s, err, sizeof err)) {
        fprintf(stderr, "accepted invalid spec: %s\n", why);
        assert(0);
    }
    assert(err[0] != '\0');
}

static void test_spec_rules(void)
{
    char err[256];
    DeviceModelSpec s = valid_spec();
    assert(device_model_validate_spec(&s, err, sizeof err));

    s = valid_spec(); s.id = ""; expect_invalid(s, "empty id");
    s = valid_spec(); s.id = "bad id"; expect_invalid(s, "id with space");
    s = valid_spec(); s.version = 0; expect_invalid(s, "version 0");
    s = valid_spec(); s.terminals = DUP; expect_invalid(s, "duplicate terminal");
    s = valid_spec(); s.terminal_count = 0; expect_invalid(s, "no terminals");
    s = valid_spec(); s.params = BAD_RANGE; expect_invalid(s, "non-finite range");
    s = valid_spec(); s.params = BAD_DEFAULT; expect_invalid(s, "default on exclusive minimum");
    s = valid_spec(); s.params = BAD_UNIT; expect_invalid(s, "unknown unit");
    s = valid_spec(); s.primary_param = 1; expect_invalid(s, "primary out of range");
    s = valid_spec(); s.capabilities |= DEVICE_CAP_AC; expect_invalid(s, "AC has no hook in v1");
    s = valid_spec(); s.capabilities |= DEVICE_CAP_TEMPERATURE; expect_invalid(s, "temperature has no hook in v1");
    s = valid_spec(); s.stamp_dc = NULL; expect_invalid(s, "DC capability without stamp");
    s = valid_spec(); s.capabilities = DEVICE_CAP_DC; expect_invalid(s, "transient stamp without capability");
    s = valid_spec(); s.capabilities = DEVICE_CAP_TRANSIENT; s.stamp_dc = NULL; expect_invalid(s, "transient without DC");
    s = valid_spec(); s.state_count = 1; expect_invalid(s, "state without commit");
    s = valid_spec(); s.commit_transient = dummy_commit; expect_invalid(s, "commit without state");
    s = valid_spec(); s.state_count = 1; s.commit_transient = dummy_commit;
    assert(device_model_validate_spec(&s, err, sizeof err));
}

static void test_registry(void)
{
    ModelRegistry reg;
    char err[256];
    model_registry_init(&reg);
    assert(model_registry_register_builtin_devices(&reg, err, sizeof err));
    assert(model_registry_device_count(&reg) == 6);

    /* Duplicate ids fail, whether built-in or user-supplied. */
    assert(!model_registry_register_builtin_devices(&reg, err, sizeof err));
    assert(strstr(err, "duplicate"));
    DeviceModelSpec same = valid_spec();
    same.id = "ideal.resistor";
    assert(!model_registry_register_device(&reg, &same, err, sizeof err));
    DeviceModelSpec fresh = valid_spec();
    assert(model_registry_register_device(&reg, &fresh, err, sizeof err));
    assert(model_registry_find_device(&reg, "test.two") == &fresh);
    assert(model_registry_find_device(&reg, "no.such.model") == NULL);

    /* Every built-in declares DC and transient, and none claims AC. */
    for (size_t i = 0; i < 6; ++i) {
        const DeviceModelSpec *m = model_registry_device_at(&reg, i);
        assert(device_model_validate_spec(m, err, sizeof err));
        assert(device_model_supports(m, DEVICE_ANALYSIS_DC));
        assert(device_model_supports(m, DEVICE_ANALYSIS_TRANSIENT));
        assert(!device_model_supports(m, DEVICE_ANALYSIS_AC));
    }
    assert(!device_model_supports(NULL, DEVICE_ANALYSIS_DC));
    model_registry_free(&reg);
}

static void test_params_and_terminals(void)
{
    ModelRegistry reg;
    char err[256];
    model_registry_init(&reg);
    assert(model_registry_register_builtin_devices(&reg, err, sizeof err));
    const DeviceModelSpec *r = model_registry_find_device(&reg, "ideal.resistor");
    const DeviceModelSpec *d = model_registry_find_device(&reg, "shockley.diode");
    double v[DEVICE_MODEL_MAX_PARAMS] = {0}, out[DEVICE_MODEL_MAX_PARAMS] = {0};
    int present[DEVICE_MODEL_MAX_PARAMS] = {0};

    assert(!device_model_resolve_params(r, v, present, out, err, sizeof err)); /* required, no default */
    present[0] = 1;
    v[0] = 0.0;      assert(!device_model_resolve_params(r, v, present, out, err, sizeof err)); /* (0, ...] */
    v[0] = -1.0;     assert(!device_model_resolve_params(r, v, present, out, err, sizeof err));
    v[0] = NAN;      assert(!device_model_resolve_params(r, v, present, out, err, sizeof err));
    v[0] = INFINITY; assert(!device_model_resolve_params(r, v, present, out, err, sizeof err));
    v[0] = 2e12;     assert(!device_model_resolve_params(r, v, present, out, err, sizeof err));
    v[0] = 4700.0;   assert(device_model_resolve_params(r, v, present, out, err, sizeof err) && out[0] == 4700.0);

    int none[DEVICE_MODEL_MAX_PARAMS] = {0};
    assert(device_model_resolve_params(d, v, none, out, err, sizeof err));
    assert(out[0] == 1e-14 && out[1] == 1.0); /* documented generic defaults */
    present[1] = 1; v[0] = 1e-12; v[1] = 20.0;
    assert(!device_model_resolve_params(d, v, present, out, err, sizeof err)); /* ideality > 10 */

    assert(device_model_terminal_index(d, "cathode") == 0 && device_model_terminal_index(d, "anode") == 1);
    assert(device_model_terminal_index(d, "gate") == -1);
    assert(device_terminal_accepts_pin_name(&d->terminals[0], "K"));
    assert(device_terminal_accepts_pin_name(&d->terminals[0], "k"));
    assert(!device_terminal_accepts_pin_name(&d->terminals[0], "A"));
    assert(device_terminal_accepts_pin_name(&d->terminals[1], "A"));
    assert(device_terminal_accepts_pin_name(&r->terminals[0], "anything")); /* passive: no name rule */

    assert(device_unit_parse("ohm") == DEVICE_UNIT_OHM && device_unit_parse("F") == DEVICE_UNIT_FARAD);
    assert(device_unit_parse("kohm") == DEVICE_UNIT_INVALID && device_unit_parse("") == DEVICE_UNIT_INVALID);
    model_registry_free(&reg);
}

/* Stamps solve to closed-form results: 5 V across 10k + 20k gives 10/3 V; a forward
 * diode from 5 V through 1k satisfies its own Shockley equation at the solution. */
static void test_stamps_solve(void)
{
    ModelRegistry reg;
    char err[256];
    model_registry_init(&reg);
    assert(model_registry_register_builtin_devices(&reg, err, sizeof err));
    const DeviceModelSpec *v = model_registry_find_device(&reg, "ideal.voltage_source");
    const DeviceModelSpec *r = model_registry_find_device(&reg, "ideal.resistor");
    const DeviceModelSpec *d = model_registry_find_device(&reg, "shockley.diode");
    DeviceStampContext dc = {DEVICE_ANALYSIS_DC, 0.0, NULL};
    MnaOptions opt;
    mna_options_default(&opt);

    MnaCircuit m;
    MnaResult res = {0};
    mna_circuit_init(&m, 2);
    size_t vin[2] = {1, 0}, top[2] = {1, 2}, bottom[2] = {2, 0};
    double five = 5.0, r1 = 10000.0, r2 = 20000.0;
    assert(v->stamp_dc(&m, vin, &five, &dc) && r->stamp_dc(&m, top, &r1, &dc) && r->stamp_dc(&m, bottom, &r2, &dc));
    mna_solve(&m, &opt, &res);
    assert(res.status == MNA_STATUS_SUCCESS);
    assert(fabs(mna_node_voltage(&res, 2) - 5.0 * 20000.0 / 30000.0) < 1e-9);
    mna_result_free(&res);
    mna_circuit_free(&m);

    /* Terminal order is (cathode, anode): anode on node 2, cathode on ground. */
    mna_circuit_init(&m, 2);
    size_t diode[2] = {0, 2};
    double one_k = 1000.0, shockley[2] = {1e-14, 1.0};
    assert(v->stamp_dc(&m, vin, &five, &dc) && r->stamp_dc(&m, top, &one_k, &dc) && d->stamp_dc(&m, diode, shockley, &dc));
    mna_solve(&m, &opt, &res);
    assert(res.status == MNA_STATUS_SUCCESS);
    double vd = mna_node_voltage(&res, 2), vt = 1.380649e-23 * 300.0 / 1.602176634e-19;
    double i_resistor = (5.0 - vd) / 1000.0, i_diode = 1e-14 * (exp(vd / vt) - 1.0);
    assert(vd > 0.5 && vd < 0.85);
    assert(fabs(i_resistor - i_diode) < 1e-9);
    mna_result_free(&res);
    mna_circuit_free(&m);
    model_registry_free(&reg);
}

int main(void)
{
    test_spec_rules();
    test_registry();
    test_params_and_terminals();
    test_stamps_solve();
    puts("device_model: PASS");
    return 0;
}
