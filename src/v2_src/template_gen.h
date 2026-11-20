#ifndef COPPER_TEMPLATE_GEN_H
#define COPPER_TEMPLATE_GEN_H

/*
 * template_gen.h - deterministic design generation from analysed requirements.
 *
 * Division of work (owner decision 2026-11-20): a language model (Manus) only ANALYSES the user's prompt into
 * copper.requirements v1 (circuit type, voltages, currents, named parts, options), each value backed by a verbatim
 * quote from the prompt. Everything else is this code: it chooses the circuit from a template (data file
 * design_templates/copper_templates.v1.json), resolves every pin from the installed KiCad libraries, computes values
 * with integer formulas and writes a copper.design-intent v1 for the grounding gate and the compiler.
 *
 *   - A requirement value is used only when an "evidence" quote for it occurs in the prompt (case- and
 *     space-insensitive) and, for numbers, the quote contains that number (also as k/M/m multiples). Anything else
 *     is IGNORED and reported, so the analysis cannot add requirements the user never stated.
 *   - Missing required values become questions (NEEDS_INPUT); an unknown circuit is NOT_IMPLEMENTED; values that
 *     cannot work (an LED supply below its forward voltage) are INVALID_INPUT. Nothing is guessed.
 *   - Every value carries its provenance: stated (from the prompt), default (template, with its reason), computed
 *     (formula shown) or assumed (template assumption, also listed as UNVERIFIED).
 *   - Part ratings are never invented: what a part must withstand is listed as UNVERIFIED for a human to check.
 *
 * Integer arithmetic only (micro-units); Arena memory; no printf, no exit.
 */

#include "arena.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TemplateGenStatus {
    TGEN_GENERATED = 0,     /* intent written */
    TGEN_NEEDS_INPUT,       /* required values missing: see questions */
    TGEN_NOT_IMPLEMENTED,   /* no template for this circuit, or outside a template's scope */
    TGEN_INVALID_INPUT,     /* malformed requirements, or values that cannot work */
    TGEN_TEMPLATE_ERROR,    /* the template does not match the installed libraries */
    TGEN_ERR_ARGUMENT,
    TGEN_ERR_MEMORY
} TemplateGenStatus;

typedef struct TemplateGenOutput {
    const char *intent_json;   /* copper.design-intent v1, only when TGEN_GENERATED */
    size_t intent_len;
    const char *report_json;   /* copper.generation/1: status, values with provenance, checks, questions, ignored */
    size_t report_len;
} TemplateGenOutput;

/* `prompt` may be NULL: evidence is then not checked, and the report says so. */
TemplateGenStatus template_generate(Arena *arena, const char *requirements_json, size_t requirements_len,
                                    const char *prompt, const char *templates_json, size_t templates_len,
                                    const char *symbols_root, const char *footprints_root, TemplateGenOutput *out);

const char *template_gen_status_name(TemplateGenStatus status);

#ifdef __cplusplus
}
#endif

#endif /* COPPER_TEMPLATE_GEN_H */
