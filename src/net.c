#include "net.h"

#include <string.h>

void net_init(Net *net)
{
    if (!net) {
        return;
    }

    memset(net, 0, sizeof(*net));
    net->connections = NULL;
}

void net_free(Net *net)
{
    if (!net) {
        return;
    }

    vec_free(net->connections);
}

int net_add_connection(
    Net *net,
    uint32_t component_id,
    uint16_t pin_number
)
{
    if (!net) {
        return -1;
    }

    for (size_t i = 0; i < vec_len(net->connections); ++i) {
        NetConnection *connection = &net->connections[i];

        if (connection->component_id == component_id &&
            connection->pin_number == pin_number) {
            return -1;
        }
    }

    NetConnection connection;
    connection.component_id = component_id;
    connection.pin_number = pin_number;

    return vec_push(net->connections, connection);
}
