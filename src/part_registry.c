#include "part_registry.h"

#include <string.h>

#include "vec.h"

int part_registry_init(PartRegistry *registry) {
  if (registry == NULL)
    return -1;

  memset(registry, 0, sizeof(*registry));

  return 0;
}

void part_registry_free(PartRegistry *registry) {
  if (registry == NULL)
    return;

  vec_free(registry->providers);
}

int part_registry_register(PartRegistry *registry, PartProvider provider) {
  if (registry == NULL)
    return -1;

  return vec_push(registry->providers, provider);
}

bool part_registry_find(const PartRegistry *registry,
                        const PartRequest *request, PartResult *out) {
  size_t i;

  if (registry == NULL || request == NULL || out == NULL) {
    return false;
  }

  /*
   * ----------------------------------------------------------
   * PHASE 1: exact identity
   *
   * Only do this if the caller actually supplied an identity.
   * ----------------------------------------------------------
   */

  if (request->exact != NULL) {
    for (i = 0; i < vec_len(registry->providers); ++i) {
      const PartProvider *provider = &registry->providers[i];

      int result = part_provider_resolve(provider, request, out);

      if (result > 0)
        return true;
    }
  }

  /*
   * ----------------------------------------------------------
   * PHASE 2: capability search
   *
   * Copy the request so that we do NOT modify the caller's
   * object.
   * ----------------------------------------------------------
   */

  {
    PartRequest capability_request = *request;

    capability_request.exact = NULL;

    for (i = 0; i < vec_len(registry->providers); ++i) {
      const PartProvider *provider = &registry->providers[i];

      int result = part_provider_resolve(provider, &capability_request, out);

      if (result > 0)
        return true;
    }
  }

  return false;
}
