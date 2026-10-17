#include "kicad_pcb.h"

#include <stddef.h>
#include <string.h>

#include "vec.h"

static const char *layer_name(uint8_t layer)
{
    static const char *const inner_names[30] = {
        "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "In5.Cu",
        "In6.Cu", "In7.Cu", "In8.Cu", "In9.Cu", "In10.Cu",
        "In11.Cu", "In12.Cu", "In13.Cu", "In14.Cu", "In15.Cu",
        "In16.Cu", "In17.Cu", "In18.Cu", "In19.Cu", "In20.Cu",
        "In21.Cu", "In22.Cu", "In23.Cu", "In24.Cu", "In25.Cu",
        "In26.Cu", "In27.Cu", "In28.Cu", "In29.Cu", "In30.Cu"
    };

    if (layer == PCB_LAYER_F_CU) return "F.Cu";
    if (layer == PCB_LAYER_B_CU) return "B.Cu";
    if (layer >= PCB_LAYER_IN1_CU && layer <= PCB_LAYER_IN30_CU &&
        (layer % 2u) == 0u) {
        return inner_names[(unsigned)layer / 2u - 2u];
    }
    return NULL;
}

static const char *component_reference(
    const Design *design,
    uint32_t component_id
)
{
    const Component *component =
        design_get_component_const(design, component_id);

    return component ? component->reference : NULL;
}

static const char *net_name(
    const Design *design,
    uint32_t net_id
)
{
    const Net *net = design_get_net_const(design, net_id);

    if (net && net->name && net->name[0] != '\0') {
        return net->name;
    }

    return "N";
}

static void write_escaped_string(FILE *out, const char *text)
{
    fputc('"', out);

    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', out);
            }

            fputc((int)*p, out);
        }
    }

    fputc('"', out);
}

static int has_net_id(
    const PcbBoard *board,
    uint32_t net_id
)
{
    for (size_t i = 0; i < vec_len(board->traces); ++i) {
        if (board->traces[i].net_id == net_id) {
            return 1;
        }
    }

    for (size_t i = 0; i < vec_len(board->vias); ++i) {
        if (board->vias[i].net_id == net_id) {
            return 1;
        }
    }

    for (size_t i = 0; i < vec_len(board->pads); ++i) {
        if (board->pads[i].net_id == net_id) {
            return 1;
        }
    }

    return 0;
}

static void write_net_table(
    const PcbBoard *board,
    const Design *design,
    FILE *out
)
{
    fprintf(out, "  (net 0 \"\")\n");

    /*
     * The physical IR currently uses design net IDs directly. This is
     * deliberately simple and keeps KiCad as a visualization/export target.
     */
    uint32_t max_net_id = 0;

    for (size_t i = 0; i < vec_len(board->traces); ++i) {
        if (board->traces[i].net_id > max_net_id) {
            max_net_id = board->traces[i].net_id;
        }
    }

    for (size_t i = 0; i < vec_len(board->vias); ++i) {
        if (board->vias[i].net_id > max_net_id) {
            max_net_id = board->vias[i].net_id;
        }
    }

    for (size_t i = 0; i < vec_len(board->pads); ++i) {
        if (board->pads[i].net_id > max_net_id) {
            max_net_id = board->pads[i].net_id;
        }
    }

    for (uint32_t net_id = 1; net_id <= max_net_id; ++net_id) {
        if (!has_net_id(board, net_id)) {
            continue;
        }

        fprintf(out, "  (net %u ", net_id);
        write_escaped_string(out, net_name(design, net_id));
        fprintf(out, ")\n");

        if (net_id == UINT32_MAX) {
            break;
        }
    }
}

int kicad_pcb_write(
    const PcbBoard *board,
    const Design *design,
    FILE *out
)
{
    if (!board || !design || !out || board->spec.layer_count < 1) {
        return -1;
    }

    /*
     * Minimal deterministic KiCad PCB S-expression.
     * KiCad is the viewer/interop boundary; this file is not the
     * authoritative engineering representation.
     */
    if (board->spec.layer_count > 32) {
        return -1; /* KiCad layer IDs support at most 30 inner copper layers. */
    }

    fprintf(
        out,
        "(kicad_pcb (version 20241229) (generator \"hardware_compiler\")\n"
        "  (general (thickness %.4f))\n"
        "  (layers\n"
        "    (0 \"F.Cu\" signal)\n",
        board->spec.board_thickness_mm
    );

    if (board->spec.layer_count >= 2) {
        fprintf(out, "    (2 \"B.Cu\" signal)\n");
    }
    for (uint8_t layer_index = 1;
         layer_index + 1u < board->spec.layer_count;
         ++layer_index) {
        const unsigned kicad_id = 2u * (unsigned)layer_index + 2u;
        fprintf(
            out,
            "    (%u \"In%u.Cu\" signal)\n",
            kicad_id,
            (unsigned)layer_index
        );
    }
    fprintf(
        out,
        "    (7 \"B.SilkS\" user \"B.Silkscreen\")\n"
        "    (5 \"F.SilkS\" user \"F.Silkscreen\")\n"
        "    (25 \"Edge.Cuts\" user)\n"
        "  )\n"
        "  (setup (pad_to_mask_clearance 0))\n"
    );

    write_net_table(board, design, out);

    fprintf(
        out,
        "  (gr_rect (start 0 0) (end %.4f %.4f)\n"
        "    (stroke (width 0.05) (type default))\n"
        "    (fill none) (layer \"Edge.Cuts\"))\n",
        board->spec.width_mm,
        board->spec.height_mm
    );

    for (size_t i = 0; i < vec_len(board->placements); ++i) {
        const PcbPlacement *placement = &board->placements[i];
        const char *reference =
            component_reference(design, placement->component_id);

        fprintf(
            out,
            "  (footprint \"Generated:Component\"\n"
            "    (layer \"F.Cu\")\n"
            "    (at %.4f %.4f %.4f)\n",
            placement->x_mm,
            placement->y_mm,
            placement->rotation_deg
        );

        fprintf(
            out,
            "    (fp_rect (start %.4f %.4f) (end %.4f %.4f)\n"
            "      (stroke (width 0.05) (type default))\n"
            "      (fill none) (layer \"F.SilkS\"))\n",
            -placement->width_mm * 0.5,
            -placement->height_mm * 0.5,
            placement->width_mm * 0.5,
            placement->height_mm * 0.5
        );

        fprintf(out, "    (fp_text reference ");
        write_escaped_string(out, reference ? reference : "?");
        fprintf(
            out,
            " (at 0 %.4f %s) (layer \"F.SilkS\")\n"
            "      (effects (font (size 0.8 0.8) (thickness 0.12))))\n",
            placement->height_mm * 0.5 + 0.8,
            (placement->rotation_deg == 180.0 ||
             placement->rotation_deg == 270.0) ? "180" : "0"
        );

        fprintf(out, "  )\n");
    }

    for (size_t i = 0; i < vec_len(board->pads); ++i) {
        const PcbPad *pad = &board->pads[i];
        const char *layer = layer_name(pad->layer);

        if (!layer) {
            return -1;
        }

        fprintf(
            out,
            "  (footprint \"Generated:Pad\"\n"
            "    (layer \"%s\")\n"
            "    (at %.4f %.4f)\n"
            "    (pad \"%u\" %s circle\n"
            "      (at 0 0)\n"
            "      (size %.4f %.4f)\n",
            layer,
            pad->x_mm,
            pad->y_mm,
            (unsigned)pad->pin_number,
            pad->plated ? "thru_hole" : "smd",
            pad->pad_diameter_mm,
            pad->pad_diameter_mm
        );

        if (pad->drill_mm > 0.0) {
            fprintf(out, "      (drill %.4f)\n", pad->drill_mm);
        }

        if (pad->plated) {
            fprintf(out, "      (layers \"*.Cu\" \"*.Mask\")\n");
        } else {
            fprintf(out, "      (layers \"%s\" \"%s.Mask\")\n", layer, layer);
        }

        if (pad->net_id != 0) {
            fprintf(out, "      (net %u ", pad->net_id);
            write_escaped_string(out, net_name(design, pad->net_id));
            fprintf(out, ")\n");
        }

        fprintf(out, "    )\n  )\n");
    }

    for (size_t i = 0; i < vec_len(board->vias); ++i) {
        const PcbVia *via = &board->vias[i];
        const char *start_layer = layer_name(via->layer_start);
        const char *end_layer = layer_name(via->layer_end);

        if (!start_layer || !end_layer) {
            return -1;
        }

        fprintf(
            out,
            "  (via (at %.4f %.4f) (size %.4f) (drill %.4f) "
            "(layers \"%s\" \"%s\")",
            via->x_mm,
            via->y_mm,
            via->diameter_mm,
            via->drill_mm,
            start_layer,
            end_layer
        );

        if (via->net_id != 0) {
            fprintf(out, " (net %u)", via->net_id);
        }

        fprintf(out, ")\n");
    }

    for (size_t i = 0; i < vec_len(board->traces); ++i) {
        const PcbTrace *trace = &board->traces[i];
        const char *layer = layer_name(trace->layer);

        if (!layer) {
            return -1;
        }

        fprintf(
            out,
            "  (segment (start %.4f %.4f) (end %.4f %.4f) "
            "(width %.4f) (layer \"%s\")",
            trace->x1_mm,
            trace->y1_mm,
            trace->x2_mm,
            trace->y2_mm,
            trace->width_mm,
            layer
        );

        if (trace->net_id != 0) {
            fprintf(out, " (net %u)", trace->net_id);
        }

        fprintf(out, ")\n");
    }

    fprintf(out, ")\n");

    return ferror(out) ? -1 : 0;
}

int kicad_pcb_write_file(
    const char *path,
    const PcbBoard *board,
    const Design *design
)
{
    if (!path) {
        return -1;
    }

    FILE *out = fopen(path, "wb");

    if (!out) {
        return -1;
    }

    int result = kicad_pcb_write(board, design, out);

    if (fclose(out) != 0) {
        return -1;
    }

    return result;
}
