#ifndef FAB_PROFILE_IO_H
#define FAB_PROFILE_IO_H

#include <stdio.h>

#include "dfm_pcb.h"

int fab_profile_write(FILE *out, const FabProfile *profile);
int fab_profile_write_file(const char *path, const FabProfile *profile);

int fab_profile_read(FILE *in, FabProfile *profile);
int fab_profile_read_file(const char *path, FabProfile *profile);

#endif
