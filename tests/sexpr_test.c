#include "arena.h"
#include "sexpr.h"
#include "xform.h"
#include <stdio.h>
#include <stdlib.h>

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(size);
  fread(buf, 1, size, f);
  fclose(f);
  *out_len = (size_t)size;
  return buf;
}

/* Builds one placed resistor instance, with real hardware data taken from
   the lib_symbols block we already embedded, and placement-specific
   fields (uuid, position, reference, value) supplied by the caller.
   The property offsets (reference above, value below) are copied from
   your golden file -- real decisions for a real layout algorithm later,
   hardcoded here because we're proving the TREE, not placement yet. */
static SxNode *
build_resistor_instance(Arena *a, const char *ref, const char *value_str,
                        double x, double y, int rotation, const char *root_uuid,
                        const char *inst_uuid, const char *pin1_uuid,
                        const char *pin2_uuid) {
  SxNode *sym = sx_list(a, "symbol");
  SxNode *lib_id = sx_list(a, "lib_id");
  sx_push(lib_id, sx_string(a, "Device:R"));
  sx_push(sym, lib_id);

  SxNode *at = sx_list(a, "at");
  sx_push(at, sx_atom_double(a, x));
  sx_push(at, sx_atom_double(a, y));
  sx_push(at, sx_atom_int(a, rotation));
  sx_push(sym, at);

  SxNode *unit = sx_list(a, "unit");
  sx_push(unit, sx_atom_int(a, 1));
  sx_push(sym, unit);

  sx_push(sym, sx_list(a, "exclude_from_sim"));
  sx_push(sym, sx_list(a, "in_bom"));
  sx_push(sym, sx_list(a, "on_board"));
  sx_push(sym, sx_list(a, "dnp"));

  SxNode *uuid = sx_list(a, "uuid");
  sx_push(uuid, sx_string(a, inst_uuid));
  sx_push(sym, uuid);

  SxNode *ref_prop = sx_list(a, "property");
  sx_push(ref_prop, sx_string(a, "Reference"));
  sx_push(ref_prop, sx_string(a, ref));
  sx_push(sym, ref_prop);

  SxNode *val_prop = sx_list(a, "property");
  sx_push(val_prop, sx_string(a, "Value"));
  sx_push(val_prop, sx_string(a, value_str));
  sx_push(sym, val_prop);

  SxNode *fp_prop = sx_list(a, "property");
  sx_push(fp_prop, sx_string(a, "Footprint"));
  sx_push(fp_prop, sx_string(a, ""));
  sx_push(sym, fp_prop);

  SxNode *pin1 = sx_list(a, "pin");
  sx_push(pin1, sx_string(a, "1"));
  SxNode *p1u = sx_list(a, "uuid");
  sx_push(p1u, sx_string(a, pin1_uuid));
  sx_push(pin1, p1u);
  sx_push(sym, pin1);

  SxNode *pin2 = sx_list(a, "pin");
  sx_push(pin2, sx_string(a, "2"));
  SxNode *p2u = sx_list(a, "uuid");
  sx_push(p2u, sx_string(a, pin2_uuid));
  sx_push(pin2, p2u);
  sx_push(sym, pin2);

  SxNode *instances = sx_list(a, "instances");
  SxNode *project = sx_list(a, "project");
  sx_push(project, sx_string(a, ""));
  char path_buf[128];
  snprintf(path_buf, sizeof path_buf, "/%s", root_uuid);
  SxNode *path = sx_list(a, "path");
  sx_push(path, sx_string(a, path_buf));
  SxNode *inst_ref = sx_list(a, "reference");
  sx_push(inst_ref, sx_string(a, ref));
  SxNode *inst_unit = sx_list(a, "unit");
  sx_push(inst_unit, sx_atom_int(a, 1));
  sx_push(path, inst_ref);
  sx_push(path, inst_unit);
  sx_push(project, path);
  sx_push(instances, project);
  sx_push(sym, instances);

  return sym;
}

static SxNode *build_label(Arena *a, const char *text, double x, double y,
                           const char *uuid) {
  SxNode *label = sx_list(a, "label");
  sx_push(label, sx_string(a, text));
  SxNode *at = sx_list(a, "at");
  sx_push(at, sx_atom_double(a, x));
  sx_push(at, sx_atom_double(a, y));
  sx_push(at, sx_atom_int(a, 0));
  sx_push(label, at);
  SxNode *u = sx_list(a, "uuid");
  sx_push(u, sx_string(a, uuid));
  sx_push(label, u);
  return label;
}

int main(void) {
  Arena a;
  arena_init(&a, 1024 * 1024);

  /* --- load the real Device:R symbol from your KiCad install --- */
  size_t lib_len;
  char *lib_src = read_file(
      "C:\\Program Files\\KiCad\\9.0\\share\\kicad\\symbols\\Device.kicad_sym",
      &lib_len);

  const char *err = NULL;
  SxNode *lib_root = sx_parse(&a, lib_src, lib_len, &err);
  if (!lib_root) {
    fprintf(stderr, "parse error: %s\n", err);
    return 1;
  }

  const SxNode *device_r = sx_find_named(lib_root, "symbol", "Device:R");
  if (!device_r) {
    fprintf(stderr, "Device:R not found\n");
    return 1;
  }

  /* --- build the schematic --- */
  KicadDocHeader h = kicad_sch_header_default();
  h.uuid = "00000000-0000-0000-0000-000000000001";
  SxNode *doc = kicad_doc_header_build(&a, &h);

  SxNode *lib_symbols = sx_list(&a, "lib_symbols");
  sx_push(lib_symbols,
          (SxNode *)device_r); /* same arena -- safe to link directly */
  sx_push(doc, lib_symbols);

  /* Positions confirmed against your first golden file's transform:
     R at (135.89, 82.55) rot 0 -> pin1 top (135.89,78.74), pin2 bottom
     (135.89,86.36) */
  sx_push(doc,
          build_resistor_instance(&a, "R1", "10k", 135.89, 82.55, 0, h.uuid,
                                  "00000000-0000-0000-0000-000000000010",
                                  "00000000-0000-0000-0000-000000000011",
                                  "00000000-0000-0000-0000-000000000012"));

  /* R2 placed so its pin1 coincides exactly with R1's pin2 -- pin-to-pin
     coincidence connects with no wire needed, same as the power-symbol
     case in your golden file. */
  sx_push(doc,
          build_resistor_instance(&a, "R2", "10k", 135.89, 90.17, 0, h.uuid,
                                  "00000000-0000-0000-0000-000000000020",
                                  "00000000-0000-0000-0000-000000000021",
                                  "00000000-0000-0000-0000-000000000022"));

  sx_push(doc, build_label(&a, "VIN", 135.89, 78.74,
                           "00000000-0000-0000-0000-000000000030"));
  sx_push(doc, build_label(&a, "GND", 135.89, 93.98,
                           "00000000-0000-0000-0000-000000000031"));

  SxNode *sheet_instances = sx_list(&a, "sheet_instances");
  SxNode *sipath = sx_list(&a, "path");
  sx_push(sipath, sx_string(&a, "/"));
  SxNode *page = sx_list(&a, "page");
  sx_push(page, sx_string(&a, "1"));
  sx_push(sipath, page);
  sx_push(sheet_instances, sipath);
  sx_push(doc, sheet_instances);

  FILE *out = fopen("divider.kicad_sch", "w");
  sx_write(doc, out, 0);
  fputc('\n', out);
  fclose(out);

  arena_destroy(&a);
  free(lib_src);
  printf("wrote divider.kicad_sch\n");
  return 0;
}
