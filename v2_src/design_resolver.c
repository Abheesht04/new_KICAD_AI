#include "design_resolver.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool text_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool text_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return false;
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

static bool part_text_match(const PartRecord *p, const char *needle)
{
    if (!needle) return true;
    if (text_contains_ci(p->id, needle) ||
        text_contains_ci(p->manufacturer, needle) ||
        text_contains_ci(p->mpn, needle) ||
        text_contains_ci(p->package, needle) ||
        text_contains_ci(p->description, needle) ||
        text_contains_ci(p->datasheet, needle) ||
        text_contains_ci(p->symbol_id, needle) ||
        text_contains_ci(p->footprint_id, needle)) return true;
    for (size_t i = 0; i < p->alias_count; ++i)
        if (text_contains_ci(p->aliases[i], needle)) return true;
    return false;
}

static const PartRating *find_rating(const PartRecord *p,
                                     const DesignRatingRequirement *r)
{
    for (size_t i = 0; i < p->rating_count; ++i) {
        const PartRating *v = &p->ratings[i];
        if (v->name == r->name && v->bound == r->bound &&
            v->unit == r->unit) return v;
    }
    return NULL;
}

static bool ratings_match(const PartRecord *p,
                          const DesignPartQuery *q,
                          bool *complete)
{
    *complete = true;
    for (size_t i = 0; i < q->rating_count; ++i) {
        const DesignRatingRequirement *r = &q->ratings[i];
        const PartRating *v = find_rating(p, r);
        if (!v) {
            if (r->required) *complete = false;
            continue;
        }
        if ((r->minimum_value && v->value < r->minimum_value) ||
            (r->maximum_value && v->value > r->maximum_value)) return false;
    }
    return true;
}

static bool has_nominal_rating(const PartRecord *p, PartRatingName name)
{
    for (size_t i = 0; i < p->rating_count; ++i)
        if (p->ratings[i].name == name &&
            (p->ratings[i].bound == PART_BOUND_NOMINAL ||
             p->ratings[i].bound == PART_BOUND_TYPICAL)) return true;
    return false;
}

static uint32_t missing_fields(const PartRecord *p)
{
    uint32_t m = 0;
    if (p->kind == PART_KIND_INVALID) m |= DESIGN_MISSING_KIND;
    if (p->id_generated) m |= DESIGN_GENERATED_ID;
    if (!p->manufacturer || !p->manufacturer[0]) m |= DESIGN_MISSING_MANUFACTURER;
    if (!p->mpn || !p->mpn[0]) m |= DESIGN_MISSING_MPN;
    if (!p->symbol_id || !p->symbol_id[0]) m |= DESIGN_MISSING_SYMBOL;
    if (!p->footprint_id || !p->footprint_id[0]) m |= DESIGN_MISSING_FOOTPRINT;
    if (!p->model_id || !p->model_id[0]) m |= DESIGN_MISSING_MODEL;
    if (!p->pin_count) m |= DESIGN_MISSING_PIN_MAP;
    if (!p->identity_source.url || p->identity_source.status != PART_SOURCE_VERIFIED)
        m |= DESIGN_MISSING_PROVENANCE;
    return m;
}

static int32_t score_candidate(const PartRecord *p, const DesignPartQuery *q)
{
    int32_t score = 0;
    if (q->part_id && p->id && text_equal_ci(q->part_id, p->id)) score += 2000;
    if (q->kind != PART_KIND_INVALID && p->kind == q->kind) score += 100;
    if (q->mpn && p->mpn && text_equal_ci(q->mpn, p->mpn)) score += 1000;
    if (q->manufacturer && p->manufacturer &&
        text_equal_ci(q->manufacturer, p->manufacturer)) score += 150;
    if (q->package && p->package && text_equal_ci(q->package, p->package)) score += 100;
    if (q->symbol_id && p->symbol_id &&
        text_equal_ci(q->symbol_id, p->symbol_id)) score += 250;
    if (q->footprint_id && p->footprint_id &&
        text_equal_ci(q->footprint_id, p->footprint_id)) score += 250;
    if (q->model_id && p->model_id && text_equal_ci(q->model_id, p->model_id)) score += 200;
    if (p->identity_source.status == PART_SOURCE_VERIFIED) score += 40;
    if (p->pin_count) score += 30;
    if (p->rating_count) score += 10;
    return score;
}

static bool query_match(const PartRecord *p, const DesignPartQuery *q,
                        bool *ratings_complete)
{
    if (q->part_id && (!p->id || !text_equal_ci(q->part_id, p->id))) return false;
    if (q->kind != PART_KIND_INVALID && p->kind != q->kind) return false;
    if (q->manufacturer && (!p->manufacturer ||
        !text_equal_ci(q->manufacturer, p->manufacturer))) return false;
    if (q->mpn && (!p->mpn || !text_equal_ci(q->mpn, p->mpn))) return false;
    if (q->package && (!p->package || !text_equal_ci(q->package, p->package))) return false;
    if (q->symbol_id && (!p->symbol_id ||
        !text_equal_ci(q->symbol_id, p->symbol_id))) return false;
    if (q->footprint_id && (!p->footprint_id ||
        !text_equal_ci(q->footprint_id, p->footprint_id))) return false;
    if (q->model_id && (!p->model_id || !text_equal_ci(q->model_id, p->model_id))) return false;
    if (!part_text_match(p, q->text)) return false;
    if (!ratings_match(p, q, ratings_complete)) return false;
    if (q->require_verified_identity &&
        p->identity_source.status != PART_SOURCE_VERIFIED) return false;
    return true;
}

static int candidate_compare(const void *lhs, const void *rhs)
{
    const DesignResolveCandidate *a = (const DesignResolveCandidate *)lhs;
    const DesignResolveCandidate *b = (const DesignResolveCandidate *)rhs;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->priority != b->priority) return a->priority > b->priority ? -1 : 1;
    if (a->part->id && b->part->id) {
        int id_cmp = strcmp(a->part->id, b->part->id);
        if (id_cmp) return id_cmp;
    }
    if (a->source != b->source) return a->source < b->source ? -1 : 1;
    if (a->catalogue_name && b->catalogue_name) {
        int name_cmp = strcmp(a->catalogue_name, b->catalogue_name);
        if (name_cmp) return name_cmp;
    }
    return 0;
}

static int candidate_id_compare(const void *lhs, const void *rhs)
{
    const DesignResolveCandidate *a = (const DesignResolveCandidate *)lhs;
    const DesignResolveCandidate *b = (const DesignResolveCandidate *)rhs;
    if (a->part->id && b->part->id) return strcmp(a->part->id, b->part->id);
    if (a->part->id) return -1;
    if (b->part->id) return 1;
    return 0;
}

const char *design_resolve_status_name(DesignResolveStatus s)
{
    switch (s) {
    case DESIGN_RESOLVE_OK: return "RESOLVED";
    case DESIGN_RESOLVE_NOT_FOUND: return "NOT_FOUND";
    case DESIGN_RESOLVE_AMBIGUOUS: return "AMBIGUOUS";
    case DESIGN_RESOLVE_INCOMPLETE: return "INCOMPLETE";
    case DESIGN_RESOLVE_ERR_ARGUMENT: return "ARGUMENT_ERROR";
    case DESIGN_RESOLVE_ERR_MEMORY: return "MEMORY_ERROR";
    default: return "UNKNOWN";
    }
}

const char *design_catalogue_source_name(DesignCatalogueSource s)
{
    switch (s) {
    case DESIGN_CATALOGUE_KICAD: return "KICAD";
    case DESIGN_CATALOGUE_USER: return "USER";
    case DESIGN_CATALOGUE_SUPPLIER: return "SUPPLIER";
    default: return "UNKNOWN";
    }
}

int32_t design_catalogue_default_priority(DesignCatalogueSource source)
{
    switch (source) {
    case DESIGN_CATALOGUE_KICAD: return 300;
    case DESIGN_CATALOGUE_USER: return 200;
    case DESIGN_CATALOGUE_SUPPLIER: return 100;
    default: return 0;
    }
}

bool design_candidate_meets_requirements(const DesignResolveCandidate *c,
                                         const DesignPartQuery *q)
{
    if (!c || !q) return false;
    if (q->require_symbol && !c->usable_for_symbol) return false;
    if (q->require_footprint && !c->usable_for_footprint) return false;
    if (q->require_model && !c->usable_for_simulation) return false;
    if (q->require_verified_identity && !c->identity_verified) return false;
    return c->ratings_complete;
}

DesignResolveStatus design_resolve_part(Arena *arena,
                                        const DesignCatalogueView *views,
                                        size_t view_count,
                                        const DesignPartQuery *q,
                                        DesignResolveResult *out)
{
    if (!arena || !views || !view_count || !q || !out ||
        (q->rating_count && !q->ratings)) return DESIGN_RESOLVE_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));

    size_t total = 0;
    for (size_t i = 0; i < view_count; ++i) {
        if (!views[i].catalogue) continue;
        if (views[i].catalogue->part_count > SIZE_MAX - total)
            return DESIGN_RESOLVE_ERR_MEMORY;
        total += views[i].catalogue->part_count;
    }
    if (!total) {
        out->status = DESIGN_RESOLVE_NOT_FOUND;
        return out->status;
    }

    DesignResolveCandidate *items =
        (DesignResolveCandidate *)arena_alloc_array(arena, sizeof(*items), total);
    if (!items) return DESIGN_RESOLVE_ERR_MEMORY;

    size_t count = 0;
    for (size_t vi = 0; vi < view_count; ++vi) {
        const PartCatalogue *cat = views[vi].catalogue;
        if (!cat) continue;
        for (size_t pi = 0; pi < cat->part_count; ++pi) {
            const PartRecord *p = &cat->parts[pi];
            bool ratings_complete = true;
            if (!query_match(p, q, &ratings_complete)) continue;
            DesignResolveCandidate c;
            memset(&c, 0, sizeof(c));
            c.part = p;
            c.catalogue_name = views[vi].name ? views[vi].name :
                               design_catalogue_source_name(views[vi].source);
            c.source = views[vi].source;
            c.priority = views[vi].priority;
            c.score = score_candidate(p, q);
            c.missing_fields = missing_fields(p);
            c.identity_verified =
                p->identity_source.status == PART_SOURCE_VERIFIED;
            c.ratings_complete = ratings_complete;
            c.usable_for_symbol = p->symbol_id && p->symbol_id[0];
            c.usable_for_footprint = p->footprint_id && p->footprint_id[0] &&
                                     p->pin_count > 0;
            c.usable_for_simulation =
                ((p->model_id && p->model_id[0]) ||
                 (p->kind == PART_KIND_RESISTOR &&
                  has_nominal_rating(p, PART_RATING_RESISTANCE)) ||
                 (p->kind == PART_KIND_CAPACITOR &&
                  has_nominal_rating(p, PART_RATING_CAPACITANCE)) ||
                 (p->kind == PART_KIND_INDUCTOR &&
                  has_nominal_rating(p, PART_RATING_INDUCTANCE))) &&
                ratings_complete;
            c.usable_for_manufacturing = c.identity_verified &&
                p->manufacturer && p->mpn && c.usable_for_footprint;
            items[count++] = c;
        }
    }

    if (count > 1u) {
        /* First group by canonical identity and retain the best source for
           each ID, then rank the unique candidates by score/priority. */
        qsort(items, count, sizeof(*items), candidate_id_compare);
        size_t write_index = 0;
        for (size_t read_index = 0; read_index < count;) {
            size_t group_end = read_index + 1u;
            size_t best_index = read_index;
            while (group_end < count &&
                   items[read_index].part->id && items[group_end].part->id &&
                   strcmp(items[read_index].part->id,
                          items[group_end].part->id) == 0) {
                if (candidate_compare(&items[group_end], &items[best_index]) < 0)
                    best_index = group_end;
                ++group_end;
            }
            items[write_index++] = items[best_index];
            read_index = group_end;
        }
        count = write_index;
        qsort(items, count, sizeof(*items), candidate_compare);
    }

    out->candidates = items;
    out->candidate_count = count;
    if (!count) {
        out->status = DESIGN_RESOLVE_NOT_FOUND;
        return out->status;
    }
    out->best = &items[0];
    out->ambiguous_count = 1;
    while (out->ambiguous_count < count &&
           items[out->ambiguous_count].score == items[0].score &&
           items[out->ambiguous_count].priority == items[0].priority)
        ++out->ambiguous_count;

    if (out->ambiguous_count > 1) out->status = DESIGN_RESOLVE_AMBIGUOUS;
    else if (!design_candidate_meets_requirements(&items[0], q))
        out->status = DESIGN_RESOLVE_INCOMPLETE;
    else out->status = DESIGN_RESOLVE_OK;
    return out->status;
}
