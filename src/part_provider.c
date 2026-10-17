#include "part_provider.h"

#include <string.h>

int part_provider_resolve(
    const PartProvider *provider,
    const PartRequest *request,
    PartResult *result
)
{
    if (!provider || !request || !result || !provider->resolve) {
        return -1;
    }

    memset(result, 0, sizeof(*result));

    return provider->resolve(
        provider->context,
        request,
        result
    );
}
