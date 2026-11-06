#include "cli.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *status;
    char detail[1300];
    const char *hint;
    int required;
} RuntimeCheck;

static void add_check(PcbaBuf *json, const RuntimeCheck *c) {
    pcba_buf_add(json, "{\"check\":");
    pcba_buf_json(json, c->name);
    pcba_buf_add(json, ",\"status\":");
    pcba_buf_json(json, c->status);
    pcba_buf_add(json, c->required ? ",\"kind\":\"core\",\"detail\":" : ",\"kind\":\"optional\",\"detail\":");
    pcba_buf_json(json, c->detail);
    pcba_buf_add(json, ",\"hint\":");
    pcba_buf_json(json, c->hint);
    pcba_buf_add(json, "}");
}

/* Runtime checks owned by pcba, then pipeline checks delegated to the backend. */
int pcba_cmd_doctor(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *args) {
    RuntimeCheck checks[5];
    size_t n = 0;
    char self[1024] = "", root[1024] = "", script[1200], core[1200];
    PcbaPython py;
    memset(checks, 0, sizeof checks);

    cli_self_path(self, sizeof self);
    checks[n] = (RuntimeCheck){"pcba", "PASS", "", "", 1};
    snprintf(checks[n++].detail, sizeof checks[0].detail, "%s (%s, %s, C%s, %s)", self, pcba_version_string(),
             PCBA_COMPILER, PCBA_C_STANDARD, PCBA_BUILD_TYPE);

    int have_root = cli_tool_root(root, sizeof root) &&
                    snprintf(script, sizeof script, "%s/tools/copper.py", root) > 0 && cli_is_file(script);
    checks[n] = (RuntimeCheck){"source_root", have_root ? "PASS" : "FAIL", "",
                               "Set PCBA_ROOT to the source tree that contains tools/copper.py", 1};
    snprintf(checks[n++].detail, sizeof checks[0].detail, "%s%s", root, have_root ? "" : " (tools/copper.py missing)");

    int have_python = pcba_find_python(&py);
    checks[n] = (RuntimeCheck){"python_launcher", have_python ? "PASS" : "FAIL", "",
                               "Install Python 3.10 or newer, or set PCBA_PYTHON to its executable", 1};
    snprintf(checks[n++].detail, sizeof checks[0].detail, "%s%s%s%s", have_python ? py.program : "not found",
             *py.version ? " (" : "", py.version, *py.version ? ")" : "");

    int have_core = cli_getenv("COPPER_CORE", core, sizeof core) && cli_is_file(core);
    checks[n] = (RuntimeCheck){"copper_core", have_core ? "PASS" : "FAIL", "",
                               "Build the copper-core target next to pcba, or set COPPER_CORE", 1};
    snprintf(checks[n++].detail, sizeof checks[0].detail, "%s", have_core ? core : "not found");

    int have_ctest = cli_is_file(PCBA_CTEST);
    checks[n] = (RuntimeCheck){"ctest", have_ctest ? "PASS" : "UNVERIFIED", "", "Only 'pcba test' needs CTest", 0};
    snprintf(checks[n++].detail, sizeof checks[0].detail, "%s", PCBA_CTEST);

    const char *local = "PASS";
    PcbaBuf json = {0};
    pcba_buf_add(&json, "{\"runtime\":[");
    if (!ctx->json) printf("%spcba runtime:%s\n", pcba_style(ctx, PCBA_STYLE_BOLD), pcba_style(ctx, PCBA_STYLE_RESET));
    for (size_t i = 0; i < n; ++i) {
        if (!ctx->json) pcba_print_check(ctx, checks[i].status, checks[i].name, checks[i].detail, checks[i].hint);
        if (checks[i].required && !strcmp(checks[i].status, "FAIL")) local = "FAIL";
        if (i) pcba_buf_add(&json, ",");
        add_check(&json, &checks[i]);
    }
    pcba_buf_add(&json, "],\"pipeline\":");

    char status[32] = "UNVERIFIED", message[1024] = "pipeline checks skipped: Python or tools/copper.py unavailable";
    char *raw = NULL;
    if (have_python && have_root) {
        static const char *const map[][2] = {{"--out", "--out"}, {"--kicad-cli", "--kicad-cli"},
                                             {"--symbols", "--symbol-dir"}, {"--footprints", "--footprint-dir"}};
        const char *bargs[12] = {"doctor"};
        size_t k = 1;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) {
            const char *v = pcba_opt(cmd, args, map[i][0]);
            if (v) { bargs[k++] = map[i][1]; bargs[k++] = v; }
        }
        bargs[k] = NULL;
        if (!ctx->json) printf("%sPipeline (tools/copper.py):%s\n", pcba_style(ctx, PCBA_STYLE_BOLD),
                               pcba_style(ctx, PCBA_STYLE_RESET));
        pcba_call_backend(ctx, bargs, status, sizeof status, &raw, message, sizeof message);
    }
    pcba_buf_add(&json, raw ? raw : "null");
    pcba_buf_add(&json, "}");
    free(raw);

    /* Overall: a broken core requirement is FAIL; otherwise the pipeline status (UNVERIFIED without KiCad 9.x). */
    const char *overall = !strcmp(local, "FAIL") ? "FAIL" : status;
    if (!*message) {
        snprintf(message, sizeof message, "%s",
                 !strcmp(overall, "PASS") ? "ready for native KiCad 9.x validation"
                 : !strcmp(overall, "UNVERIFIED") ? "core tools work; native KiCad 9.x validation is not possible here"
                                                  : "a required component is missing; see the hints above");
    }
    if (!strcmp(local, "FAIL")) snprintf(message, sizeof message, "a required component is missing; see the hints above");
    int code = pcba_finish(ctx, "doctor", overall, message, json.data);
    pcba_buf_free(&json);
    return code;
}

static void parent_dir(char *path) {
    char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > slash) slash = back;
    if (slash) *slash = '\0';
}

int pcba_cmd_test(PcbaContext *ctx, const PcbaCommand *cmd, const PcbaArgs *args) {
    int full = pcba_opt(cmd, args, "--full") != NULL;
    if (full && pcba_opt(cmd, args, "--quick")) return pcba_usage_error(ctx, cmd, "choose --quick or --full, not both");
    char dir[1100], marker[1200], message[1400];
    if (!cli_is_file(PCBA_CTEST)) {
        snprintf(message, sizeof message, "ctest not found at %s", PCBA_CTEST);
        return pcba_finish(ctx, "test", "UNVERIFIED", message, NULL);
    }
    snprintf(dir, sizeof dir, "%s", ctx->exe_dir);
    snprintf(marker, sizeof marker, "%s/CTestTestfile.cmake", dir);
    if (!cli_is_file(marker)) { /* multi-config generators put executables in <build>/<Config> */
        parent_dir(dir);
        snprintf(marker, sizeof marker, "%s/CTestTestfile.cmake", dir);
    }
    if (!cli_is_file(marker)) return pcba_finish(ctx, "test", "UNVERIFIED", "pcba test must run from its CMake build tree", NULL);
    const char *argv[12] = {PCBA_CTEST, "--test-dir", dir, "-C", PCBA_BUILD_TYPE, "--output-on-failure", NULL};
    if (!full) {
        argv[6] = "-E";
        argv[7] = "^(v2_integration|v2_native|bridge_regression|pcba_cli|hwc_cli_integration|hwc_help)$";
        argv[8] = NULL;
    }
    CliProcResult r;
    int launched = cli_run(argv, CLI_STDOUT_CAPTURE_ALL, &r);
    if (r.output) fputs(r.output, ctx->json ? stderr : stdout);
    int skipped = r.output && (strstr(r.output, "(Skipped)") || strstr(r.output, "Not Run"));
    const char *status = !launched ? "UNVERIFIED" : r.exit_code ? "FAIL" : skipped ? "UNVERIFIED" : "PASS";
    snprintf(message, sizeof message, "%s suite: %s", full ? "full" : "quick",
             !launched ? "ctest could not be started" : r.exit_code ? "some tests failed"
             : skipped ? "some suites were skipped (see output)" : "all selected tests ran and passed");
    PcbaBuf json = {0};
    char code_text[64];
    pcba_buf_add(&json, full ? "{\"mode\":\"full\",\"test_dir\":" : "{\"mode\":\"quick\",\"test_dir\":");
    pcba_buf_json(&json, dir);
    snprintf(code_text, sizeof code_text, ",\"ctest_exit_code\":%d,\"skipped\":%s}", r.exit_code, skipped ? "true" : "false");
    pcba_buf_add(&json, code_text);
    cli_proc_result_free(&r);
    int code = pcba_finish(ctx, "test", status, message, json.data);
    pcba_buf_free(&json);
    return code;
}
