#ifndef COPPER_TOPOLOGY_JSON_H
#define COPPER_TOPOLOGY_JSON_H
#include "topology.h"
#include "filesystem.h"
#include <stddef.h>
char *copper_topology_json_to_string(const CopperTopology *topology, size_t *out_length);
FsStatus copper_topology_json_write_file(const CopperTopology *topology, const char *path);
#endif
