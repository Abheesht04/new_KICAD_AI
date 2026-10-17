// mna_from_design.h
#ifndef MNA_FROM_DESIGN_H
#define MNA_FROM_DESIGN_H

#include <stdbool.h>
#include <stdint.h>

#include "design.h"
#include "diagnostic.h"
#include "mna.h"

bool design_to_mna(const Design *design, uint32_t ground_net_id,
                   MnaCircuit *out, DiagnosticList *diag);

#endif
