#include "design_parity.h"

#include <string.h>

static void add_diag(CopperDiagnosticReport *r, DiagnosticSeverity severity,
                     const char *code, const char *file, const char *object,
                     const char *message) {
    if (!r) return;
    CopperDiagnostic d;
    memset(&d, 0, sizeof(d));
    d.severity = severity; d.code = code; d.file = file;
    d.object = object; d.message = message;
    (void)diagnostic_report_add(r, &d);
}
static const char *path_uuid(const char *path) {
    if (!path || !path[0]) return NULL;
    const char *last = strrchr(path, '/');
    if (last && last[1]) return last + 1;
    return path;
}
static const BoardFootprint *find_fp_for_symbol(const Board *board,
                                                 const SchematicSymbol *sym) {
    if (!board || !sym) return NULL;
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i];
        const char *uuid = path_uuid(fp->path);
        if (uuid && sym->uuid && strcmp(uuid, sym->uuid) == 0) return fp;
    }
    /* A present KiCad instance path is authoritative. Never silently fall back
       to refdes when a path UUID exists but does not match. */
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i];
        if (fp->path && fp->path[0]) continue;
        if (fp->reference && sym->reference && strcmp(fp->reference, sym->reference) == 0) return fp;
    }
    return NULL;
}
static const SchematicSymbol *find_symbol_for_fp(const SchematicIR *sch,
                                                  const BoardFootprint *fp) {
    const char *uuid = path_uuid(fp ? fp->path : NULL);
    if (!sch || !fp) return NULL;
    if (uuid) return schematic_find_symbol_uuid(sch, uuid);
    for (size_t i = 0; i < sch->symbol_count; ++i)
        if (fp->reference && sch->symbols[i].reference &&
            strcmp(fp->reference, sch->symbols[i].reference) == 0) return &sch->symbols[i];
    return NULL;
}
static const KicadNetlistNet *node_net(const KicadNetlist *nl,
                                      const KicadNetlistNode *node) {
    if (!nl || !node || node->net_index >= nl->net_count) return NULL;
    return &nl->nets[node->net_index];
}

bool design_parity_check(const SchematicIR *sch, const Board *board,
                         const KicadNetlist *netlist,
                         CopperDiagnosticReport *report,
                         DesignParitySummary *summary) {
    DesignParitySummary local;
    if (!summary) summary = &local;
    memset(summary, 0, sizeof(*summary));
    if (!sch || !board) {
        add_diag(report, DIAG_UNVERIFIED, "PARITY_INPUT_MISSING", NULL, NULL,
                 "Schematic or PCB model is not available; cross-document parity was not checked.");
        return false;
    }
    summary->netlist_available = netlist != NULL;
    for (size_t i = 0; i < sch->symbol_count; ++i) {
        const SchematicSymbol *sym = &sch->symbols[i];
        if (!sym->on_board || sym->dnp) continue;
        if (!sym->footprint || sym->footprint[0] == '\0') {
            ++summary->symbols_without_footprint_assignment;
            add_diag(report, DIAG_UNVERIFIED, "SYMBOL_FOOTPRINT_UNASSIGNED",
                     "*.kicad_sch", sym->reference,
                     "Placed schematic symbol has no footprint assignment. Confirm that it is intentionally virtual or assign a PCB footprint before physical parity can be complete.");
            continue;
        }
        const BoardFootprint *fp = find_fp_for_symbol(board, sym);
        if (!fp) {
            ++summary->missing_footprints;
            add_diag(report, DIAG_WARNING, "PCB_FOOTPRINT_MISSING", "*.kicad_pcb",
                     sym->reference,
                     "Schematic symbol has a non-empty footprint assignment and is on-board, but no matching PCB footprint was found.");
            continue;
        }
        ++summary->matched_components;
        if (fp->reference && strcmp(fp->reference, sym->reference) != 0) {
            ++summary->reference_mismatches;
            add_diag(report, DIAG_ERROR, "COMPONENT_REFERENCE_MISMATCH", "*.kicad_pcb",
                     sym->reference, "PCB footprint reference does not match the schematic component reference.");
        }
        if (fp->lib_id && strcmp(fp->lib_id, sym->footprint) != 0) {
            ++summary->footprint_mismatches;
            add_diag(report, DIAG_WARNING, "FOOTPRINT_ASSIGNMENT_MISMATCH", "*.kicad_pcb",
                     sym->reference, "PCB footprint library identifier differs from the schematic Footprint property.");
        }
    }
    for (size_t i = 0; i < board->footprint_count; ++i) {
        const BoardFootprint *fp = &board->footprints[i];
        if (!find_symbol_for_fp(sch, fp)) {
            ++summary->extra_footprints;
            add_diag(report, DIAG_WARNING, "PCB_FOOTPRINT_WITHOUT_SYMBOL", "*.kicad_pcb",
                     fp->reference, "PCB footprint has no matching placed schematic symbol by KiCad path UUID or reference.");
        }
    }
    if (!netlist) {
        add_diag(report, DIAG_UNVERIFIED, "NETLIST_NOT_SUPPLIED", "*.kicad_net", NULL,
                 "Schematic-to-PCB pad/net connectivity parity is UNVERIFIED because no current KiCad netlist was supplied. Generate a fresh netlist from this schematic revision.");
    } else {
        for (size_t i = 0; i < board->pad_count; ++i) {
            const BoardPad *pad = &board->pads[i];
            if (pad->footprint >= board->footprint_count) continue;
            const BoardFootprint *fp = &board->footprints[pad->footprint];
            if (!fp->reference || !pad->number) continue;
            const KicadNetlistNode *node = kicad_netlist_find_node(netlist, fp->reference, pad->number);
            if (!node) {
                if (pad->net != 0) {
                    ++summary->pad_net_mismatches;
                    add_diag(report, DIAG_ERROR, "PCB_PAD_NOT_IN_SCHEMATIC_NETLIST", "*.kicad_net",
                             fp->reference, "PCB pad has a copper net assignment but no corresponding component/pin node exists in the supplied schematic netlist.");
                }
                continue;
            }
            ++summary->checked_pad_nets;
            if (pad->net == 0) {
                ++summary->pad_net_mismatches;
                add_diag(report, DIAG_ERROR, "PCB_PAD_NET_MISSING", "*.kicad_pcb",
                         fp->reference, "Schematic netlist assigns this component pin to a net, but the corresponding PCB pad has no net assignment.");
                continue;
            }
            const BoardNet *pcb_net = board_net_by_id(board, pad->net);
            const KicadNetlistNet *logical_net = node_net(netlist, node);
            if (!pcb_net || !logical_net || !logical_net->name ||
                !pcb_net->name || strcmp(logical_net->name, pcb_net->name) != 0) {
                ++summary->pad_net_mismatches;
                add_diag(report, DIAG_ERROR, "PCB_PAD_NET_MISMATCH", "*.kicad_pcb",
                         fp->reference, "PCB pad net differs from the corresponding schematic netlist connection.");
            }
        }
    }
    if (summary->missing_footprints == 0 && summary->extra_footprints == 0 &&
        summary->symbols_without_footprint_assignment == 0 &&
        summary->reference_mismatches == 0 && summary->footprint_mismatches == 0 &&
        summary->pad_net_mismatches == 0 && netlist) {
        add_diag(report, DIAG_INFO, "DESIGN_PARITY_MATCHED", NULL, NULL,
                 "Schematic component/footprint identities and checked pad/net assignments agree.");
    }
    return summary->reference_mismatches == 0 && summary->pad_net_mismatches == 0;
}
