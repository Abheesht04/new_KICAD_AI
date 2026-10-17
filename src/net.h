#ifndef NET_H
#define NET_H

#include <stdint.h>

#include "vec.h"

typedef struct {
    uint32_t component_id;
    uint16_t pin_number;
} NetConnection;

typedef struct {
    uint32_t id;
    const char *name;
    NetConnection *connections;
} Net;

void net_init(Net *net);
void net_free(Net *net);

int net_add_connection(
    Net *net,
    uint32_t component_id,
    uint16_t pin_number
);

#endif
