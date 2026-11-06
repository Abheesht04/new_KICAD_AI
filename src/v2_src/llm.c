/* llm.c - names shared by the LLM backends (see llm.h). */
#include "llm.h"

static const char *const STATUS[] = {"OK", "INVALID_ARGUMENT", "UNAVAILABLE", "TIMEOUT", "HTTP_ERROR", "BAD_RESPONSE",
                                     "SCRIPT_EXHAUSTED", "OUT_OF_MEMORY"};
static const char *const ROLES[] = {"system", "user", "assistant", "tool"};

const char *llm_status_name(LlmStatus s) { return (unsigned)s < sizeof STATUS / sizeof *STATUS ? STATUS[s] : "UNKNOWN"; }
const char *llm_role_name(LlmRole r) { return (unsigned)r < sizeof ROLES / sizeof *ROLES ? ROLES[r] : "user"; }
