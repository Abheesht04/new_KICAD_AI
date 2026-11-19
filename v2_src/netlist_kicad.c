#include "netlist_kicad.h"
#include "filesystem.h"

#include <stdio.h>
#include <string.h>

static const IRNode *nl_head(const IRNode *n) { return ir_list_head(n); }
static bool nl_list(const IRNode *n, const char *name) {
    const IRNode *h = nl_head(n);
    return n && n->kind == IR_NODE_LIST && h && h->kind == IR_NODE_ATOM && strcmp(h->text, name) == 0;
}
static const char *nl_text(const IRNode *n, size_t i) {
    const IRNode *c = ir_child_at(n, i);
    return c && (c->kind == IR_NODE_ATOM || c->kind == IR_NODE_STRING) ? c->text : NULL;
}
static const IRNode *nl_field(const IRNode *n, const char *name) { return ir_list_find(n, name); }
static const char *nl_field_text(const IRNode *n, const char *name) { return nl_text(nl_field(n, name), 1); }
static void nl_diag(KicadNetlistDiag *d, KicadNetlistStatus s, const char *m) {
    if (!d) return;
    d->status = s;
    snprintf(d->message, sizeof(d->message), "%s", m ? m : "");
}
static size_t count_children(const IRNode *n, const char *name) {
    size_t count = 0;
    if (!n) return 0;
    for (const IRNode *c = n->first_child ? n->first_child->next : NULL; c; c = c->next)
        if (nl_list(c, name)) ++count;
    return count;
}
KicadNetlistStatus kicad_netlist_parse(Arena *arena, const char *source,
                                       size_t length, KicadNetlist *out,
                                       KicadNetlistDiag *diag) {
    IRError error; IRNode *root;
    if (!arena || !source || !out) { nl_diag(diag, KICAD_NETLIST_ERR_ARGUMENT, "arena, source and output required"); return KICAD_NETLIST_ERR_ARGUMENT; }
    memset(out, 0, sizeof(*out)); out->arena = arena;
    if (diag) memset(diag, 0, sizeof(*diag));
    ir_error_clear(&error); root = ir_parse(arena, source, length, &error);
    if (!root) { nl_diag(diag, KICAD_NETLIST_ERR_PARSE, error.message); return KICAD_NETLIST_ERR_PARSE; }
    if (!nl_list(root, "export")) { nl_diag(diag, KICAD_NETLIST_ERR_ROOT, "netlist root is not export"); return KICAD_NETLIST_ERR_ROOT; }
    out->source_root = root;
    const IRNode *components = nl_field(root, "components");
    const IRNode *nets = nl_field(root, "nets");
    out->component_count = count_children(components, "comp");
    out->net_count = count_children(nets, "net");
    size_t total_nodes = 0;
    if (nets) for (const IRNode *net = nets->first_child ? nets->first_child->next : NULL; net; net = net->next)
        if (nl_list(net, "net")) total_nodes += count_children(net, "node");
    out->node_count = total_nodes;
    out->components = out->component_count ? (KicadNetlistComponent *)arena_alloc_array(arena, sizeof(*out->components), out->component_count) : NULL;
    out->nets = out->net_count ? (KicadNetlistNet *)arena_alloc_array(arena, sizeof(*out->nets), out->net_count) : NULL;
    out->nodes = out->node_count ? (KicadNetlistNode *)arena_alloc_array(arena, sizeof(*out->nodes), out->node_count) : NULL;
    if ((out->component_count && !out->components) || (out->net_count && !out->nets) || (out->node_count && !out->nodes)) {
        nl_diag(diag, KICAD_NETLIST_ERR_MEMORY, "netlist arrays allocation failed"); return KICAD_NETLIST_ERR_MEMORY;
    }
    size_t ci = 0;
    if (components) for (const IRNode *c = components->first_child ? components->first_child->next : NULL; c; c = c->next)
        if (nl_list(c, "comp")) {
            KicadNetlistComponent *dst = &out->components[ci++];
            dst->reference = nl_field_text(c, "ref"); dst->value = nl_field_text(c, "value");
            dst->footprint = nl_field_text(c, "footprint"); dst->uuid = nl_field_text(c, "tstamps");
            const IRNode *libsource = nl_field(c, "libsource");
            dst->library = nl_field_text(libsource, "lib"); dst->part = nl_field_text(libsource, "part");
            if (!dst->reference || !dst->value) { nl_diag(diag, KICAD_NETLIST_ERR_PARSE, "netlist component missing ref/value"); return KICAD_NETLIST_ERR_PARSE; }
        }
    size_t ni = 0, nodei = 0;
    if (nets) for (const IRNode *n = nets->first_child ? nets->first_child->next : NULL; n; n = n->next)
        if (nl_list(n, "net")) {
            KicadNetlistNet *dst = &out->nets[ni];
            dst->code = nl_field_text(n, "code"); dst->name = nl_field_text(n, "name");
            dst->net_class = nl_field_text(n, "class"); dst->first_node = (uint32_t)nodei;
            dst->node_count = (uint32_t)count_children(n, "node");
            for (const IRNode *node = n->first_child ? n->first_child->next : NULL; node; node = node->next)
                if (nl_list(node, "node")) {
                    KicadNetlistNode *nd = &out->nodes[nodei++];
                    nd->reference = nl_field_text(node, "ref"); nd->pin = nl_field_text(node, "pin");
                    nd->pin_function = nl_field_text(node, "pinfunction"); nd->pin_type = nl_field_text(node, "pintype");
                    nd->net_index = (uint32_t)ni;
                    if (!nd->reference || !nd->pin) { nl_diag(diag, KICAD_NETLIST_ERR_PARSE, "netlist node missing ref/pin"); return KICAD_NETLIST_ERR_PARSE; }
                }
            if (!dst->code || !dst->name) { nl_diag(diag, KICAD_NETLIST_ERR_PARSE, "net record missing code/name"); return KICAD_NETLIST_ERR_PARSE; }
            ++ni;
        }
    nl_diag(diag, KICAD_NETLIST_OK, "KiCad S-expression netlist parsed");
    return KICAD_NETLIST_OK;
}
KicadNetlistStatus kicad_netlist_load(const char *path, Arena *arena,
                                     KicadNetlist *out, KicadNetlistDiag *diag) {
    const unsigned char *data = NULL; size_t size = 0;
    if (!path || !arena || !out) return KICAD_NETLIST_ERR_ARGUMENT;
    FsStatus fs = fs_read_file_arena(arena, path, &data, &size);
    if (fs != FS_OK) { nl_diag(diag, KICAD_NETLIST_ERR_IO, fs_status_name(fs)); return KICAD_NETLIST_ERR_IO; }
    return kicad_netlist_parse(arena, (const char *)data, size, out, diag);
}
const KicadNetlistNode *kicad_netlist_find_node(const KicadNetlist *n,
                                                const char *ref,
                                                const char *pin) {
    if (!n || !ref || !pin) return NULL;
    for (size_t i = 0; i < n->node_count; ++i)
        if (strcmp(n->nodes[i].reference, ref) == 0 && strcmp(n->nodes[i].pin, pin) == 0)
            return &n->nodes[i];
    return NULL;
}
const char *kicad_netlist_status_name(KicadNetlistStatus s) {
    static const char *const names[] = {"OK", "ARGUMENT", "IO", "PARSE", "ROOT", "MEMORY", "RANGE"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "UNKNOWN";
}
