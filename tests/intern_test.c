#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "intern.h"

int main(void)
{
    Interner interner;
    assert(interner_init(&interner, 8) == 0);

    const char *a = intern_string(&interner, "VIN_24V");
    const char *b = intern_string(&interner, "VIN_24V");

    assert(a != NULL);
    assert(a == b);
    assert(interner_count(&interner) == 1);

    const char *gnd = intern_string(&interner, "GND");
    const char *vcc = intern_string(&interner, "VCC_3V3");

    assert(gnd != NULL);
    assert(vcc != NULL);
    assert(interner_count(&interner) == 3);

    const char raw[] = "SENSE_NODEX";
    const char *sense = intern_string_n(&interner, raw, 10);
    assert(sense != NULL);
    assert(strcmp(sense, "SENSE_NODE") == 0);

    for (int i = 0; i < 1000; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "NET_%d", i);

        const char *interned = intern_string(&interner, name);
        assert(interned != NULL);
        assert(strcmp(interned, name) == 0);
    }

    assert(intern_string(&interner, "VIN_24V") == a);

    interner_destroy(&interner);

    printf("Interner test PASSED\n");
    return 0;
}
