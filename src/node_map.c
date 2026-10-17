#include "node_map.h"

#include <stdlib.h>

bool node_map_build(const Design *design, uint32_t ground_net_id, NodeMap *map)
{
    if (!design || !map) {
        return false;
    }

    map->net_to_row = NULL;
    map->net_count = 0;
    map->unknown_count = 0;

    size_t n = vec_len(design->nets);

    /*
     * Defend the net_id == position invariant every call. This is the
     * cheapest possible correctness check (O(n), no extra allocation) and
     * it's the whole reason this map is safe to build by direct indexing
     * instead of a real net_id -> position lookup. If this ever fires, the
     * fix is to replace the indexing below with an actual lookup -- see
     * node_map.h.
     */
    for (size_t i = 0; i < n; i++) {
        if (design->nets[i].id != (uint32_t)i) {
            return false;
        }
    }

    size_t *net_to_row = malloc(n * sizeof(size_t));
    if (n > 0 && !net_to_row) {
        return false;
    }

    size_t next_row = 0;
    for (size_t i = 0; i < n; i++) {
        if (design->nets[i].id == ground_net_id) {
            net_to_row[i] = SIZE_MAX; /* sentinel: this net has no row */
        } else {
            net_to_row[i] = next_row++;
        }
    }

    map->net_to_row = net_to_row;
    map->net_count = n;
    map->unknown_count = next_row;
    return true;
}

void node_map_free(NodeMap *map)
{
    if (!map) {
        return;
    }
    free(map->net_to_row);
    map->net_to_row = NULL;
    map->net_count = 0;
    map->unknown_count = 0;
}

size_t node_map_row(const NodeMap *map, uint32_t net_id)
{
    if (!map || net_id >= map->net_count) {
        return SIZE_MAX;
    }
    return map->net_to_row[net_id];
}
