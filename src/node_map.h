#ifndef NODE_MAP_H
#define NODE_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "design.h"

/*
 * --------------------------------------------------------------------------
 * NodeMap
 * --------------------------------------------------------------------------
 *
 * Bridges Design's nets (identified by net_id, assigned by design_add_net)
 * to compact 0..unknown_count-1 row indices used when building the MNA
 * matrix. The ground net gets no row at all -- it's the reference node,
 * always implicitly 0V.
 *
 * INVARIANT THIS RELIES ON:
 *
 *     net_id == array position in design->nets at the time this map is
 *     built.
 *
 * This holds today because design_add_net() always assigns
 * `stored.id = (uint32_t)vec_len(design->nets)` at push time, and there is
 * currently no way to remove a net from a Design. If that ever changes
 * (net removal, ids sourced elsewhere, nets re-ordered), this map silently
 * indexes wrong -- node_map_build() below defends against that with an
 * assertion pass (NDEBUG builds skip it, so also treat this comment as the
 * contract): it re-derives the invariant every call rather than trusting
 * it blindly. If you add net removal, replace the direct array index in
 * node_map_build with a real net_id -> position lookup (e.g. reuse
 * Interner's hash-table pattern, or a linear scan -- net counts are small)
 * before ids and positions can diverge.
 */
typedef struct {
    size_t *net_to_row;    /* indexed by net_id; SIZE_MAX = "this net IS ground" */
    size_t net_count;      /* length of net_to_row == vec_len(design->nets) at build time */
    size_t unknown_count;  /* number of non-ground nets == matrix size before source rows */
} NodeMap;

/*
 * Assigns every non-ground net a compact row index 0..unknown_count-1, in
 * the order they appear in design->nets, skipping ground_net_id entirely.
 *
 * Returns false (and leaves *map zeroed) if:
 *   - design or map is NULL
 *   - allocation fails
 *   - the net_id == position invariant above is violated (checked always,
 *     not just in debug builds, since silent misindexing here corrupts
 *     every downstream KCL stamp)
 */
bool node_map_build(const Design *design, uint32_t ground_net_id, NodeMap *map);

void node_map_free(NodeMap *map);

/*
 * Convenience lookup: row index for net_id, or SIZE_MAX if net_id is the
 * ground net (or out of range).
 */
size_t node_map_row(const NodeMap *map, uint32_t net_id);

#endif
