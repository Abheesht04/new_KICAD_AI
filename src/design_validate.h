#ifndef HWC_DESIGN_VALIDATE_H
#define HWC_DESIGN_VALIDATE_H

#include "design.h"
#include "diagnostic.h"

/* Check structural invariants connecting Component pins and Net connections.
 * Returns the number of ERROR diagnostics, or -1 on invalid arguments/OOM. */
int design_validate_structure(const Design *design, DiagnosticList *out);

#endif
