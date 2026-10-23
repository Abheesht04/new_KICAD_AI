#include "transient.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TransientStatus fail(TransientFailure *f, TransientStatus status, size_t device, const char *fmt, ...)
{
    if (f) {
        va_list ap;
        f->status = status;
        f->device = device;
        va_start(ap, fmt);
        vsnprintf(f->message, sizeof f->message, fmt, ap);
        va_end(ap);
    }
    return status;
}

const char *transient_method_name(unsigned method)
{
    switch (method) {
        case TRANSIENT_BACKWARD_EULER: return "backward_euler";
        case TRANSIENT_TRAPEZOIDAL:    return "trapezoidal";
        default:                       return "unknown";
    }
}

const char *transient_status_name(TransientStatus status)
{
    switch (status) {
        case TRANSIENT_OK:                 return "ok";
        case TRANSIENT_INVALID_ARGUMENT:   return "invalid_argument";
        case TRANSIENT_UNSUPPORTED:        return "unsupported_analysis";
        case TRANSIENT_STAMP_FAILED:       return "model_stamp";
        case TRANSIENT_SOLVE_FAILED:       return "solve_failed";
        case TRANSIENT_COMMIT_FAILED:      return "state_commit";
        case TRANSIENT_ALLOCATION_FAILURE: return "allocation_failure";
        default:                           return "unknown";
    }
}

TransientStatus transient_init(TransientRun *run, const TransientDevice *devices, size_t device_count,
                               size_t node_count, TransientMethod method, double dt_s, TransientFailure *failure)
{
    memset(run, 0, sizeof *run);
    if (failure) {
        memset(failure, 0, sizeof *failure);
        failure->device = SIZE_MAX;
    }
    if (!isfinite(dt_s) || dt_s <= 0.0) {
        return fail(failure, TRANSIENT_INVALID_ARGUMENT, SIZE_MAX, "time step must be finite and > 0 (got %g)", dt_s);
    }
    if (method != TRANSIENT_BACKWARD_EULER && method != TRANSIENT_TRAPEZOIDAL) {
        return fail(failure, TRANSIENT_INVALID_ARGUMENT, SIZE_MAX, "unknown integration method %d", (int)method);
    }
    if (device_count && !devices) {
        return fail(failure, TRANSIENT_INVALID_ARGUMENT, SIZE_MAX, "no device array");
    }
    for (size_t i = 0; i < device_count; ++i) {
        const DeviceModelSpec *m = devices[i].model;
        const char *label = devices[i].label ? devices[i].label : "device";
        if (!m || !device_model_supports(m, DEVICE_ANALYSIS_TRANSIENT)) {
            return fail(failure, TRANSIENT_UNSUPPORTED, i, "%s: model %s has no transient capability", label,
                        m ? m->id : "(none)");
        }
        /* Trapezoidal still takes its first step with Backward Euler, so a stateful model needs both. */
        unsigned needed = method == TRANSIENT_TRAPEZOIDAL ? TRANSIENT_BACKWARD_EULER | TRANSIENT_TRAPEZOIDAL
                                                           : TRANSIENT_BACKWARD_EULER;
        if (m->state_count && (m->integration_methods & needed) != needed) {
            return fail(failure, TRANSIENT_UNSUPPORTED, i, "%s: model %s does not implement %s integration", label,
                        m->id, transient_method_name(method));
        }
        for (size_t t = 0; t < m->terminal_count; ++t) {
            if (devices[i].nodes[t] > node_count) {
                return fail(failure, TRANSIENT_INVALID_ARGUMENT, i, "%s: terminal node out of range", label);
            }
        }
        for (size_t k = 0; k < m->param_count; ++k) {
            if (!isfinite(devices[i].params[k])) {
                return fail(failure, TRANSIENT_INVALID_ARGUMENT, i, "%s: parameter %s is not finite", label,
                            m->params[k].name);
            }
        }
    }
    run->accepted = calloc(device_count ? device_count : 1, sizeof *run->accepted);
    if (!run->accepted) {
        return fail(failure, TRANSIENT_ALLOCATION_FAILURE, SIZE_MAX, "out of memory");
    }
    run->devices = devices;
    run->device_count = device_count;
    run->node_count = node_count;
    run->method = method;
    run->dt_s = dt_s;
    mna_options_default(&run->options);
    return TRANSIENT_OK;
}

void transient_free(TransientRun *run)
{
    free(run->accepted);
    memset(run, 0, sizeof *run);
}

TransientStatus transient_step(TransientRun *run, MnaResult *solution, TransientFailure *failure)
{
    size_t step = run->last.step + 1;
    double time_s = (double)step * run->dt_s;
    /* The first step follows the t = 0+ source discontinuity: always Backward Euler. */
    unsigned method = step == 1 ? TRANSIENT_BACKWARD_EULER : (unsigned)run->method;
    MnaCircuit circuit;
    MnaResult result = {0};
    MnaValidation validation = {0};
    double (*tentative)[DEVICE_MODEL_MAX_STATE] = NULL;
    TransientStatus status = TRANSIENT_OK;

    if (failure) {
        memset(failure, 0, sizeof *failure);
        failure->step = step;
        failure->time_s = time_s;
        failure->device = SIZE_MAX;
    }
    mna_circuit_init(&circuit, run->node_count);
    for (size_t i = 0; i < run->device_count; ++i) {
        const TransientDevice *d = &run->devices[i];
        DeviceStampContext ctx = {DEVICE_ANALYSIS_TRANSIENT, run->dt_s, run->accepted[i], method};
        if (!d->model->stamp_transient(&circuit, d->nodes, d->params, &ctx)) {
            status = fail(failure, TRANSIENT_STAMP_FAILED, i, "%s: model %s could not be stamped at t=%g s",
                          d->label ? d->label : "device", d->model->id, time_s);
            goto done;
        }
    }
    mna_solve(&circuit, &run->options, &result);
    if (failure) {
        failure->mna_status = result.status;
        failure->iterations = result.iterations;
        failure->max_residual = result.maximum_residual;
    }
    if (result.status != MNA_STATUS_SUCCESS ||
        !mna_validate(&circuit, &result, &run->options, &validation) || !validation.valid) {
        status = fail(failure, TRANSIENT_SOLVE_FAILED, SIZE_MAX,
                      "step %zu (t=%g s) did not solve: MNA status %d after %zu iterations, max residual %g", step,
                      time_s, (int)result.status, result.iterations, result.maximum_residual);
        goto done;
    }
    /* Every device's next state is computed before anything is committed. */
    tentative = calloc(run->device_count ? run->device_count : 1, sizeof *tentative);
    if (!tentative) {
        status = fail(failure, TRANSIENT_ALLOCATION_FAILURE, SIZE_MAX, "out of memory");
        goto done;
    }
    for (size_t i = 0; i < run->device_count; ++i) {
        const TransientDevice *d = &run->devices[i];
        if (!d->model->state_count) continue;
        DeviceStampContext ctx = {DEVICE_ANALYSIS_TRANSIENT, run->dt_s, run->accepted[i], method};
        if (!d->model->commit_transient(&result, d->nodes, d->params, &ctx, tentative[i])) {
            status = fail(failure, TRANSIENT_COMMIT_FAILED, i, "%s: model %s could not compute its state at t=%g s",
                          d->label ? d->label : "device", d->model->id, time_s);
            goto done;
        }
        for (size_t k = 0; k < d->model->state_count; ++k) {
            if (!isfinite(tentative[i][k])) {
                status = fail(failure, TRANSIENT_COMMIT_FAILED, i, "%s: state %zu is not finite at t=%g s",
                              d->label ? d->label : "device", k, time_s);
                goto done;
            }
        }
    }
    /* Commit: the step is accepted. */
    for (size_t i = 0; i < run->device_count; ++i) {
        if (run->devices[i].model->state_count) memcpy(run->accepted[i], tentative[i], sizeof tentative[i]);
    }
    run->last.step = step;
    run->last.time_s = time_s;
    run->last.method = method;
    *solution = result;
    result = (MnaResult){0};

done:
    free(tentative);
    mna_result_free(&result);
    mna_circuit_free(&circuit);
    return status;
}
