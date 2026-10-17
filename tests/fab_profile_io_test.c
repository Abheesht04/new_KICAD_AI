#include "fab_profile_io.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    FabProfile original = fab_profile_conservative_2layer();

    FILE *tmp = tmpfile();
    assert(tmp != NULL);

    assert(fab_profile_write(tmp, &original) == 0);

    rewind(tmp);

    FabProfile loaded;
    assert(fab_profile_read(tmp, &loaded) == 0);

    assert(loaded.name != NULL);
    assert(strcmp(loaded.name, "conservative_2layer") == 0);
    assert(loaded.max_layer_count == original.max_layer_count);
    assert(loaded.min_trace_width_mm == original.min_trace_width_mm);
    assert(loaded.min_trace_clearance_mm == original.min_trace_clearance_mm);
    assert(loaded.min_via_drill_mm == original.min_via_drill_mm);
    assert(loaded.max_component_height_mm ==
           original.max_component_height_mm);

    fclose(tmp);

    printf("Fab profile serialization test PASSED\n");
    return 0;
}
