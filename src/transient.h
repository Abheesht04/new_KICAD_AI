#ifndef TRANSIENT_H
#define TRANSIENT_H
/*
 * Fixed-step transient analysis over device-model specs (docs/engineering/TRANSIENT.md).
 *
 * The circuit is the DAE F(t, x, dx/dt) = 0. Each stateful model replaces its
 * derivative with a companion model for the chosen integration method and
 * keeps its own state (state_count values with a declared meaning). Every step
 * is transactional:
 *   1. stamp all devices from the ACCEPTED state into a fresh MNA circuit;
 *   2. solve it with the existing Newton engine and validate the solution;
 *   3. compute every device's tentative next state;
 *   4. only if 1-3 all succeed, commit the tentative state and advance time.
 * A failed step leaves the accepted state, step count and time untouched and
 * fills a TransientFailure (time, step, MNA status, iterations, residual,
 * device).
 *
 * Initial condition: zero stored energy (every state value 0) with all
 * sources switched on at t = 0+, a source discontinuity. Trapezoidal
 * integration therefore takes its first step with Backward Euler, as SPICE
 * does after a discontinuity; TransientStep.method reports the method used.
 * There is no adaptive step, local truncation error control or BDF2.
 */
#include <stddef.h>

#include "model_registry.h"

typedef enum {
    TRANSIENT_BACKWARD_EULER = DEVICE_INTEGRATION_BACKWARD_EULER,
    TRANSIENT_TRAPEZOIDAL = DEVICE_INTEGRATION_TRAPEZOIDAL
} TransientMethod;

typedef enum {
    TRANSIENT_OK = 0,
    TRANSIENT_INVALID_ARGUMENT, /* dt, method, device or parameter not acceptable */
    TRANSIENT_UNSUPPORTED,      /* a device lacks transient capability or the method */
    TRANSIENT_STAMP_FAILED,     /* a device could not be stamped */
    TRANSIENT_SOLVE_FAILED,     /* Newton did not converge, singular, non-finite, or validation failed */
    TRANSIENT_COMMIT_FAILED,    /* a device could not compute its next state */
    TRANSIENT_ALLOCATION_FAILURE
} TransientStatus;

typedef struct {
    const DeviceModelSpec *model;
    size_t nodes[DEVICE_MODEL_MAX_TERMINALS]; /* MNA node per terminal, 0 = ground */
    double params[DEVICE_MODEL_MAX_PARAMS];   /* validated, in spec order */
    const char *label;                         /* for failure reports, e.g. "C1" */
} TransientDevice;

typedef struct {
    TransientStatus status;
    size_t step;          /* 1-based index of the step that failed */
    double time_s;        /* time that step was solving for */
    MnaStatus mna_status;
    size_t iterations;
    double max_residual;
    size_t device;        /* index of the device involved, or SIZE_MAX */
    char message[256];
} TransientFailure;

typedef struct {
    size_t step;          /* accepted steps so far */
    double time_s;        /* time of the last accepted step */
    unsigned method;      /* method this step used */
} TransientStep;

typedef struct {
    const TransientDevice *devices; /* borrowed; must outlive the run */
    size_t device_count;
    size_t node_count;              /* non-ground MNA nodes */
    TransientMethod method;
    double dt_s;
    MnaOptions options;
    double (*accepted)[DEVICE_MODEL_MAX_STATE]; /* owned: accepted state per device */
    TransientStep last;
} TransientRun;

/* Validates the request (dt finite and > 0, known method, every device has
 * the transient capability and, if stateful, the method) and sets the zero
 * initial state. Returns TRANSIENT_OK or a failure (details in *failure). */
TransientStatus transient_init(TransientRun *run, const TransientDevice *devices, size_t device_count,
                               size_t node_count, TransientMethod method, double dt_s,
                               TransientFailure *failure);
void transient_free(TransientRun *run);

/* Attempts one step. On TRANSIENT_OK, *solution (caller frees with
 * mna_result_free) holds the accepted solution and run->last advances. On any
 * failure the run is unchanged and *failure describes the step. */
TransientStatus transient_step(TransientRun *run, MnaResult *solution, TransientFailure *failure);

const char *transient_status_name(TransientStatus status);
const char *transient_method_name(unsigned method);

#endif
