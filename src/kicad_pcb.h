#ifndef KICAD_PCB_H
#define KICAD_PCB_H

#include <stdio.h>

#include "design.h"
#include "pcb.h"

int kicad_pcb_write(
    const PcbBoard *board,
    const Design *design,
    FILE *out
);

int kicad_pcb_write_file(
    const char *path,
    const PcbBoard *board,
    const Design *design
);

#endif
