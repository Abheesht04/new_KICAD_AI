#include "project.h"
#include "pcb_copper.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

static const char *txt(const IRNode *n, size_t i) {
    const IRNode *v=ir_child_at(n,i); return v && v->text ? v->text : "";
}
static const char *field(const IRNode *n,const char *key) { return txt(ir_list_find(n,key),1); }
static int num(const IRNode *n,size_t i,double *v) {
    return ir_atom_to_double(ir_child_at(n,i),v)==IR_STATUS_OK && isfinite(*v);
}
static int errorf(char *e,size_t cap,const char *fmt,...) {
    va_list a; va_start(a,fmt); vsnprintf(e,cap,fmt,a); va_end(a); return 0;
}
static int id_ok(const char *s) {
    if(!s || !*s || strlen(s)>127) return 0;
    for(;*s;s++) if(!isalnum((unsigned char)*s) && !strchr("_:+-./",*s)) return 0;
    return 1;
}
static size_t net_index(const CpProject *p,const char *s) {
    for(size_t i=0;i<p->net_count;i++) if(!strcmp(s,p->nets[i])) return i;
    return SIZE_MAX;
}
static size_t comp_index(const CpProject *p,const char *s) {
    for(size_t i=0;i<p->component_count;i++) if(!strcmp(s,p->components[i].ref)) return i;
    return SIZE_MAX;
}
static int scalar(const IRNode *n,const char *key,double def,double min,double max,double *out) {
    const IRNode *v=ir_list_find(n,key); *out=def;
    return (!v || (ir_list_length(v)==2 && num(v,1,out))) && *out>=min && *out<=max;
}
/* Unknown and duplicate fields fail closed: a typo must not weaken a constraint. */
static int fields(const IRNode *n,size_t skip,const char *allowed,char *e,size_t cap) {
    for(size_t i=skip;i<ir_list_length(n);i++) {
        const IRNode *a=ir_child_at(n,i); const char *key=txt(a,0);
        char wrapped[160]; snprintf(wrapped,sizeof wrapped,"|%s|",key);
        if(!*key || !strstr(allowed,wrapped)) return errorf(e,cap,"Unknown field '%s'",key);
        for(size_t j=skip;j<i;j++) if(!strcmp(key,txt(ir_child_at(n,j),0)))
            return errorf(e,cap,"Duplicate field '%s'",key);
    }
    return 1;
}
void cp_init(CpProject *p) {
    memset(p,0,sizeof *p); arena_init(&p->arena,65536);
    pcb_board_init(&p->board,(PcbBoardSpec){40,30,1.6,0.035,2});
    p->fab=fab_profile_conservative_2layer();
    char ignored[128]; model_registry_init(&p->models);
    model_registry_register_builtin_devices(&p->models,ignored,sizeof ignored); /* a missing model fails cp_load */
}
void cp_free(CpProject *p) { model_registry_free(&p->models); pcb_board_free(&p->board); arena_destroy(&p->arena); memset(p,0,sizeof *p); }
static const char *keep(CpProject *p,const char *s) {
    size_t n=strlen(s)+1; char *copy=arena_alloc(&p->arena,n); if(copy) memcpy(copy,s,n); return copy?copy:"";
}
/* Device family per part kind. J (ideal boundary) and U (unmodelled) never bind a device model. */
static const char *kind_family(char kind) {
    switch(kind) { case 'R': return "resistor"; case 'C': return "capacitor"; case 'L': return "inductor";
    case 'D': return "diode"; case 'V': return "voltage_source"; case 'I': return "current_source"; default: return NULL; }
}
static const char *kind_default_model(char kind) {
    switch(kind) { case 'R': return "ideal.resistor"; case 'C': return "ideal.capacitor"; case 'L': return "ideal.inductor";
    case 'D': return "shockley.diode"; case 'V': return "ideal.voltage_source"; case 'I': return "ideal.current_source"; default: return NULL; }
}
/* A rating or dimension: absent leaves *q unchanged (UNKNOWN unless an earlier layer set it); 'unknown' makes it
 * UNKNOWN; otherwise it must be finite and in (0, max]. Zero is rejected: it is not a rating. */
static int quantity(const IRNode *n,const char *key,double max,const char *source,CpQuantity *q,const char *who,char *e,size_t cap) {
    const IRNode *v=ir_list_find(n,key); double d=0;
    if(!v) return 1;
    if(ir_list_length(v)==2 && !strcmp(txt(v,1),"unknown")) { *q=(CpQuantity){0,0,source}; return 1; }
    if(ir_list_length(v)!=2 || !num(v,1,&d) || d<=0 || d>max)
        return errorf(e,cap,"%s: %s must be a positive finite value up to %g, or 'unknown'%s",who,key,max,
                      ir_list_length(v)==2 && num(v,1,&d) && d==0 ? " (zero is not a rating; omit it or write unknown)" : "");
    *q=(CpQuantity){d,1,source}; return 1;
}
/* A catalogue part definition, validated against its bound device model. */
typedef struct {
    char kind;
    const DeviceModelSpec *model;
    const char *model_source;
    const char *terminal_pin[DEVICE_MODEL_MAX_TERMINALS];
    double param_values[DEVICE_MODEL_MAX_PARAMS];
    int param_present[DEVICE_MODEL_MAX_PARAMS];
    double tolerance;
    CpQuantity power_w, voltage_v, height_mm;
} PartInfo;
static int read_part(CpProject *p,const IRNode *part,PartInfo *out,char *e,size_t cap) {
    static const char *const default_pins[DEVICE_MODEL_MAX_TERMINALS]={"1","2","3","4","5","6","7","8"};
    const char *id=txt(part,1); char why[256];
    memset(out,0,sizeof *out);
    if(strcmp(txt(part,0),"part") || !id_ok(id)) return errorf(e,cap,"Invalid part definition %s",id);
    const char *kind=field(part,"kind");
    if(strlen(kind)!=1 || !strchr("RCLDVIJU",*kind)) return errorf(e,cap,"part %s: unknown kind '%s'",id,kind);
    out->kind=*kind;
    const IRNode *model=ir_list_find(part,"model");
    if(model) {
        if(!kind_family(out->kind)) return errorf(e,cap,"part %s: kind %c has no device model; remove (model ...)",id,out->kind);
        out->model=model_registry_find_device(&p->models,txt(model,1)); out->model_source="explicit";
        if(ir_list_length(model)!=2 || !out->model) return errorf(e,cap,"part %s: unknown device model '%s'",id,txt(model,1));
        if(strcmp(out->model->family,kind_family(out->kind))) return errorf(e,cap,"part %s: model %s is a %s, not kind %c",id,out->model->id,out->model->family,out->kind);
    } else if(kind_default_model(out->kind)) {
        out->model=model_registry_find_device(&p->models,kind_default_model(out->kind)); out->model_source="kind_default";
        if(!out->model) return errorf(e,cap,"built-in device model %s is not registered",kind_default_model(out->kind));
    } else out->model_source="none";
    /* Identity and data fields, plus the bound model's non-primary parameters; anything else is rejected. */
    char allowed[640]="|kind||symbol||footprint||power_w||voltage_v||tolerance||height_mm||manufacturer||mpn||datasheet||model||pinmap||provenance||catalogue|";
    for(size_t k=0;out->model && k<out->model->param_count;k++) if((int)k!=out->model->primary_param) {
        size_t used=strlen(allowed); snprintf(allowed+used,sizeof allowed-used,"|%s|",out->model->params[k].name);
    }
    if(!fields(part,2,allowed,why,sizeof why)) return errorf(e,cap,"part %s: %s",id,why);
    if(out->kind!='V' && out->kind!='I' && (!*field(part,"symbol") || !*field(part,"footprint"))) return errorf(e,cap,"part %s: physical part needs symbol and footprint",id);
    snprintf(why,sizeof why,"part:%s",id); const char *source=keep(p,why);
    out->power_w=out->voltage_v=out->height_mm=(CpQuantity){0,0,source};
    if(!quantity(part,"power_w",1e9,source,&out->power_w,id,e,cap) || !quantity(part,"voltage_v",1e9,source,&out->voltage_v,id,e,cap) ||
       !quantity(part,"height_mm",100,source,&out->height_mm,id,e,cap)) return 0;
    if(!scalar(part,"tolerance",0,0,.99,&out->tolerance)) return errorf(e,cap,"part %s: invalid tolerance",id);
    for(size_t k=0;out->model && k<out->model->param_count;k++) {
        const IRNode *v=ir_list_find(part,out->model->params[k].name);
        if(!v || (int)k==out->model->primary_param) continue;
        if(ir_list_length(v)!=2 || !num(v,1,&out->param_values[k])) return errorf(e,cap,"part %s: %s must be a finite number",id,out->model->params[k].name);
        out->param_present[k]=1;
    }
    /* Explicit logical terminal -> KiCad symbol pin map; default: terminal i -> pin "i+1". */
    const IRNode *map=ir_list_find(part,"pinmap");
    if(map && !out->model) return errorf(e,cap,"part %s: (pinmap ...) needs a device model",id);
    if(!out->model) return 1;
    if(!map) { for(size_t t=0;t<out->model->terminal_count;t++) out->terminal_pin[t]=default_pins[t]; return 1; }
    if(ir_list_length(map)-1!=out->model->terminal_count)
        return errorf(e,cap,"part %s: pinmap must map each of the %zu terminals of %s exactly once",id,out->model->terminal_count,out->model->id);
    for(size_t j=1;j<ir_list_length(map);j++) {
        const IRNode *entry=ir_child_at(map,j); const char *name=txt(entry,0), *pin=txt(entry,1);
        int t=device_model_terminal_index(out->model,name);
        if(ir_list_length(entry)<2 || ir_list_length(entry)>3) return errorf(e,cap,"part %s: pinmap entries are (terminal symbol_pin [pad])",id);
        if(t<0) return errorf(e,cap,"part %s: model %s has no terminal '%s'",id,out->model->id,name);
        if(out->terminal_pin[t]) return errorf(e,cap,"part %s: terminal '%s' is mapped twice",id,name);
        if(!id_ok(pin)) return errorf(e,cap,"part %s: invalid symbol pin for terminal '%s'",id,name);
        if(ir_list_length(entry)==3 && strcmp(txt(entry,2),pin))
            return errorf(e,cap,"part %s: terminal %s maps symbol pin %s to pad %s; KiCad pairs symbol pin N with footprint pad N, so they must match",id,name,pin,txt(entry,2));
        for(size_t u=0;u<out->model->terminal_count;u++)
            if(out->terminal_pin[u] && !strcmp(out->terminal_pin[u],pin)) return errorf(e,cap,"part %s: symbol pin %s is mapped to two terminals",id,pin);
        out->terminal_pin[t]=pin;
    }
    return 1;
}
static int load_rules(CpProject *p,const IRNode *n,char *e,size_t cap) {
    if(n && !fields(n,1,"|trace_width_mm||clearance_mm||edge_mm||via_diameter_mm||drill_mm||annular_mm||hole_gap_mm||max_aspect||mask_expansion_mm||mask_web_mm||courtyard_gap_mm||max_drop_v||max_current_density_a_mm2||power_derating||voltage_derating||max_height_mm|",e,cap)) return 0;
#define RULE(k,d,lo,hi,f) if(!scalar(n,k,d,lo,hi,&(f))) return errorf(e,cap,"Invalid rule %s",k)
    RULE("trace_width_mm",.2,.05,10,p->fab.min_trace_width_mm);
    RULE("clearance_mm",.2,.01,10,p->fab.min_trace_clearance_mm);
    RULE("edge_mm",.3,.01,20,p->fab.min_copper_to_edge_mm);
    RULE("via_diameter_mm",.6,.1,10,p->fab.min_via_diameter_mm);
    RULE("drill_mm",.3,.05,10,p->fab.min_via_drill_mm);
    RULE("annular_mm",.15,.01,5,p->fab.min_via_annular_ring_mm);
    p->fab.min_pad_annular_ring_mm=p->fab.min_via_annular_ring_mm;
    RULE("hole_gap_mm",.25,.01,10,p->fab.min_hole_clearance_mm);
    RULE("max_aspect",8,1,30,p->max_aspect);
    RULE("mask_expansion_mm",.05,0,1,p->mask_expansion);
    RULE("mask_web_mm",.1,0,2,p->min_mask_web);
    RULE("courtyard_gap_mm",.25,0,10,p->courtyard_gap);
    RULE("max_drop_v",.05,1e-9,100,p->max_drop);
    RULE("max_current_density_a_mm2",20,.01,1e5,p->max_current_density);
    RULE("power_derating",.5,.01,1,p->power_derating);
    RULE("voltage_derating",.8,.01,1,p->voltage_derating);
    RULE("max_height_mm",10,.01,100,p->fab.max_component_height_mm);
#undef RULE
    return 1;
}
int cp_load(CpProject *p,const char *path,char *e,size_t cap) {
    IRError ie; p->root=ir_parse_file(&p->arena,path,&ie);
    if(!p->root) return errorf(e,cap,"Parse at %zu:%zu: %s",ie.line,ie.column,ie.message?ie.message:"I/O failure");
    const IRNode *root=p->root;
    if(strcmp(txt(root,0),"copper_project") || strcmp(field(root,"version"),"2")) return errorf(e,cap,"Expected (copper_project (version 2) ...)");
    if(!fields(root,1,"|version||name||nets||parts||components||board||rules||geometry||targets|",e,cap)) return 0;
    p->name=field(root,"name");
    const IRNode *nets=ir_list_find(root,"nets");
    if(ir_list_length(nets)<2 || strcmp(txt(nets,1),"GND")) return errorf(e,cap,"nets must begin with GND");
    for(size_t i=1;i<ir_list_length(nets);i++) {
        const char *s=txt(nets,i);
        if(!id_ok(s) || net_index(p,s)!=SIZE_MAX || p->net_count==CP_MAX_NETS) return errorf(e,cap,"Invalid/duplicate net: %s",s);
        p->nets[p->net_count++]=s;
    }
    const IRNode *b=ir_list_find(root,"board"); double dims[5]={40,30,1.6,.035,2};
    if(b) { if(ir_list_length(b)!=6) return errorf(e,cap,"board requires width height thickness copper layers");
        for(size_t i=0;i<5;i++) if(!num(b,i+1,&dims[i]) || dims[i]<=0) return errorf(e,cap,"Invalid board dimensions"); }
    if(dims[0]>500 || dims[1]>500 || dims[2]>10 || dims[3]>1 || dims[4]!=2) return errorf(e,cap,"V2 supports rectangular two-layer boards up to 500mm");
    p->board.spec=(PcbBoardSpec){dims[0],dims[1],dims[2],dims[3],2};
    p->fab.max_board_width_mm=p->fab.max_board_height_mm=500;
    if(!load_rules(p,ir_list_find(root,"rules"),e,cap)) return 0;
    const IRNode *parts=ir_list_find(root,"parts"), *cs=ir_list_find(root,"components");
    for(size_t i=1;i<ir_list_length(parts);i++) {
        const IRNode *part=ir_child_at(parts,i); PartInfo info;
        if(!read_part(p,part,&info,e,cap)) return 0;
        for(size_t j=1;j<i;j++) if(!strcmp(txt(part,1),txt(ir_child_at(parts,j),1))) return errorf(e,cap,"Duplicate part %s",txt(part,1));
    }
    for(size_t i=1;i<ir_list_length(cs);i++) {
        const IRNode *n=ir_child_at(cs,i); PartInfo info; char why[256];
        if(p->component_count==CP_MAX_COMPONENTS || strcmp(txt(n,0),"component") || !id_ok(txt(n,1)) || comp_index(p,txt(n,1))!=SIZE_MAX) return errorf(e,cap,"Invalid/duplicate component %s",txt(n,1));
        if(!fields(n,2,"|part||value||pins||at||override|",e,cap)) return 0;
        const IRNode *part=ir_list_find_named(parts,"part",field(n,"part"));
        if(!part) return errorf(e,cap,"%s: part '%s' is not in catalogue",txt(n,1),field(n,"part"));
        if(!read_part(p,part,&info,e,cap)) return 0;
        CpComponent *c=&p->components[p->component_count++];
        c->ref=txt(n,1); c->part=txt(part,1); c->symbol=field(part,"symbol"); c->footprint=field(part,"footprint");
        c->manufacturer=field(part,"manufacturer"); c->mpn=field(part,"mpn"); c->datasheet=field(part,"datasheet");
        c->provenance=field(part,"provenance"); c->catalogue=field(part,"catalogue"); c->override_provenance="";
        c->kind=info.kind; c->model=info.model; c->model_source=info.model_source; c->tolerance=info.tolerance;
        c->power_w=info.power_w; c->voltage_v=info.voltage_v; c->height_mm=info.height_mm;
        /* A model's primary parameter comes from (value ...); an omitted value is never defaulted. */
        int primary=c->model ? c->model->primary_param : -1;
        const IRNode *value=ir_list_find(n,"value");
        if(primary>=0 && !value) return errorf(e,cap,"%s: kind %c requires an explicit (value ...)",c->ref,c->kind);
        if(value && (ir_list_length(value)!=2 || !num(value,1,&c->value))) return errorf(e,cap,"%s: invalid SI value",c->ref);
        if(c->model) {
            if(primary>=0) { info.param_values[primary]=c->value; info.param_present[primary]=1; }
            if(!device_model_resolve_params(c->model,info.param_values,info.param_present,c->params,why,sizeof why)) return errorf(e,cap,"%s: invalid SI value: %s",c->ref,why);
        }
        const IRNode *pins=ir_list_find(n,"pins");
        for(size_t j=1;j<ir_list_length(pins);j++) {
            const IRNode *pin=ir_child_at(pins,j); size_t ni=net_index(p,txt(pin,1));
            if(ir_list_length(pin)!=2 || !id_ok(txt(pin,0)) || ni==SIZE_MAX || c->pin_count==CP_MAX_PINS) return errorf(e,cap,"%s: invalid pin/net mapping",c->ref);
            for(size_t k=0;k<c->pin_count;k++) if(!strcmp(c->pins[k],txt(pin,0))) return errorf(e,cap,"%s: duplicate pin",c->ref);
            c->pins[c->pin_count]=txt(pin,0); c->nodes[c->pin_count++]=ni;
        }
        if(c->pin_count==0) return errorf(e,cap,"%s: no pins",c->ref);
        /* Bind every model terminal to exactly one connected pin through the part's pin map. */
        for(size_t t=0;c->model && t<c->model->terminal_count;t++) {
            size_t k=0; while(k<c->pin_count && strcmp(c->pins[k],info.terminal_pin[t])) k++;
            if(k==c->pin_count) return errorf(e,cap,"%s: terminal %s of %s is symbol pin %s, which is missing from (pins ...)",c->ref,c->model->terminals[t].name,c->model->id,info.terminal_pin[t]);
            c->terminal_pin[t]=k;
        }
        if(c->model && c->pin_count!=c->model->terminal_count) return errorf(e,cap,"%s: pins must be exactly the %zu pins mapped to %s terminals",c->ref,c->model->terminal_count,c->model->id);
        if(c->model && c->model->terminal_count==2 && c->nodes[c->terminal_pin[0]]==c->nodes[c->terminal_pin[1]])
            return errorf(e,cap,"%s: both terminals are on net %s; the part would short itself",c->ref,p->nets[c->nodes[c->terminal_pin[0]]]);
        /* Project override: data only (ratings, height, tolerance), with mandatory provenance. Highest precedence. */
        const IRNode *ov=ir_list_find(n,"override");
        if(ov) {
            if(!fields(ov,1,"|power_w||voltage_v||height_mm||tolerance||provenance|",why,sizeof why)) return errorf(e,cap,"%s: override: %s",c->ref,why);
            c->override_provenance=field(ov,"provenance");
            if(!*c->override_provenance) return errorf(e,cap,"%s: (override ...) needs (provenance \"...\") naming its data source",c->ref);
            snprintf(why,sizeof why,"override:%s",c->ref); const char *source=keep(p,why);
            if(!quantity(ov,"power_w",1e9,source,&c->power_w,c->ref,e,cap) || !quantity(ov,"voltage_v",1e9,source,&c->voltage_v,c->ref,e,cap) ||
               !quantity(ov,"height_mm",100,source,&c->height_mm,c->ref,e,cap)) return 0;
            if(!scalar(ov,"tolerance",c->tolerance,0,.99,&c->tolerance)) return errorf(e,cap,"%s: override: invalid tolerance",c->ref);
        }
        const IRNode *at=ir_list_find(n,"at");
        if(at && (ir_list_length(at)!=4 || !num(at,1,&c->x) || !num(at,2,&c->y) || !num(at,3,&c->rotation))) return errorf(e,cap,"%s: invalid placement",c->ref);
        if(fabs(c->x)>500 || fabs(c->y)>500 || fmod(c->rotation,90)!=0) return errorf(e,cap,"%s: placement requires 90-degree rotation",c->ref);
    }
    if(!p->component_count || p->net_count<2) return errorf(e,cap,"Empty circuit");
    const IRNode *targets=ir_list_find(root,"targets");
    for(size_t i=1;i<ir_list_length(targets);i++) {
        const IRNode *n=ir_child_at(targets,i); double lo,hi;
        if(strcmp(txt(n,0),"voltage") || ir_list_length(n)!=4 || net_index(p,txt(n,1))==SIZE_MAX || !num(n,2,&lo) || !num(n,3,&hi) || lo>hi) return errorf(e,cap,"Invalid voltage target");
    }
    const IRNode *geo=ir_list_find(root,"geometry"); p->has_geometry=geo!=NULL;
    for(size_t i=1;i<ir_list_length(geo);i++) {
        const IRNode *g=ir_child_at(geo,i); const char *type=txt(g,0); double v[12]={0};
        size_t start=0,count=0,ci=0,ni=0;
        if(!strcmp(type,"trace")) { ni=net_index(p,txt(g,1)); start=2; count=6; }
        else if(!strcmp(type,"pad")) { ci=comp_index(p,txt(g,1)); ni=net_index(p,txt(g,3)); start=4; count=9; }
        else if(!strcmp(type,"via")) { ni=net_index(p,txt(g,1)); start=2; count=4; }
        else if(!strcmp(type,"body")) { ci=comp_index(p,txt(g,1)); start=2; count=5; }
        else return errorf(e,cap,"Unsupported geometry %s",type);
        if(ni==SIZE_MAX || ci==SIZE_MAX || ir_list_length(g)!=start+count) return errorf(e,cap,"Invalid %s reference/arity",type);
        for(size_t k=0;k<count;k++) if(!num(g,start+k,&v[k]) || fabs(v[k])>500) return errorf(e,cap,"Invalid %s number",type);
        int rc=-1;
        if(!strcmp(type,"trace")) {
            if(v[5]!=0 && v[5]!=2) return errorf(e,cap,"Invalid copper layer");
            rc=pcb_add_trace(&p->board,(PcbTrace){(uint32_t)ni+1,(uint8_t)v[5],v[0],v[1],v[2],v[3],v[4]});
        } else if(!strcmp(type,"pad")) {
            size_t pi=SIZE_MAX; for(size_t k=0;k<p->components[ci].pin_count;k++) if(!strcmp(p->components[ci].pins[k],txt(g,2))) pi=k;
            if(pi==SIZE_MAX || p->components[ci].nodes[pi]!=ni || (v[4]!=0&&v[4]!=2) || (v[7]!=0&&v[7]!=1) || v[2]<=0 || v[3]<0) return errorf(e,cap,"Pad disagrees with electrical IR");
            PcbPad pad={.component_id=(uint32_t)ci,.pin_number=(uint16_t)(pi+1),.net_id=(uint32_t)ni+1,.x_mm=v[0],.y_mm=v[1],.pad_diameter_mm=v[2],.drill_mm=v[3],.layer=(uint8_t)v[4],.plated=v[3]>0,.width_mm=v[5],.height_mm=v[6],.rectangular=(int)v[7],.corner_radius_mm=v[8]};
            rc=pcb_add_pad(&p->board,pad);
        } else if(!strcmp(type,"via")) rc=pcb_add_via(&p->board,(PcbVia){(uint32_t)ni+1,v[0],v[1],v[2],v[3],0,2,true});
        else {
            /* Height comes from part data; geometry may restate it (0 = not stated) but never invent it. */
            const CpComponent *bc=&p->components[ci]; double h=bc->height_mm.known ? bc->height_mm.value : 0;
            if(v[4]!=0 && (!bc->height_mm.known || fabs(v[4]-h)>1e-9)) return errorf(e,cap,"%s: body height %g disagrees with the part data",bc->ref,v[4]);
            rc=pcb_add_placement(&p->board,(PcbPlacement){(uint32_t)ci,v[0],v[1],0,v[2],v[3],h});
        }
        if(rc) return errorf(e,cap,"Invalid %s geometry",type);
    }
    return 1;
}
typedef double CpState[DEVICE_MODEL_MAX_STATE];
static void terminal_nodes(const CpComponent *c,size_t *nodes) {
    for(size_t t=0;t<c->model->terminal_count;t++) nodes[t]=c->nodes[c->terminal_pin[t]];
}
/* Voltage from model terminal 0 to terminal 1 (two-terminal models). */
static double terminal_voltage(const CpComponent *c,const MnaResult *r) {
    return mna_node_voltage(r,c->nodes[c->terminal_pin[0]])-mna_node_voltage(r,c->nodes[c->terminal_pin[1]]);
}
/* Stamps every component through its device model. Returns 1, or 0 with the reporting rule and a reason:
 * unsupported_model (no model, kind U), unsupported_analysis (model lacks the capability), model_stamp. */
static int assemble(const CpProject *p,MnaCircuit *m,DeviceAnalysis analysis,double dt,CpState *state,
                    const char **rule,char *why,size_t cap) {
    mna_circuit_init(m,p->net_count-1);
    for(size_t i=0;i<p->component_count;i++) {
        const CpComponent *c=&p->components[i]; size_t nodes[DEVICE_MODEL_MAX_TERMINALS];
        if(!c->model) {
            if(c->kind=='J') continue; /* Explicit ideal connector boundary; no device stamp. */
            *rule="unsupported_model"; snprintf(why,cap,"%s: part %s (kind %c) has no device model; analysis is blocked.",c->ref,c->part,c->kind);
            return 0; /* Never silently omit an active unmodelled device. */
        }
        if(!device_model_supports(c->model,analysis)) {
            *rule="unsupported_analysis";
            snprintf(why,cap,"%s: model %s v%u does not support %s analysis.",c->ref,c->model->id,c->model->version,device_analysis_name(analysis));
            return 0;
        }
        DeviceStampContext ctx={analysis,dt,state?state[i]:NULL};
        terminal_nodes(c,nodes);
        if(!(analysis==DEVICE_ANALYSIS_DC ? c->model->stamp_dc : c->model->stamp_transient)(m,nodes,c->params,&ctx)) {
            *rule="model_stamp"; snprintf(why,cap,"%s: model %s could not be stamped (allocation failure).",c->ref,c->model->id);
            return 0;
        }
    }
    return 1;
}
static void js(FILE *f,const char *s) {
    fputc('"',f); for(;s&&*s;s++) { unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\') { fputc('\\',f); fputc(c,f); }
        else if(c<32) fprintf(f,"\\u%04x",c); else fputc(c,f);
    } fputc('"',f);
}
typedef struct { FILE *f; int count, errors, warnings; } Report;
static void issue(Report *r,int severity,const char *rule,const char *object,double measured,double limit,const char *message) {
    if(r->count++) fputc(',',r->f);
    fprintf(r->f,"{\"severity\":\"%s\",\"rule\":",severity==2?"error":"warning"); js(r->f,rule);
    fputs(",\"object\":",r->f); js(r->f,object); fprintf(r->f,",\"measured\":%.12g,\"limit\":%.12g,\"message\":",isfinite(measured)?measured:0,isfinite(limit)?limit:0); js(r->f,message); fputc('}',r->f);
    if(severity==2) r->errors++; else r->warnings++;
}
/* Enumerate all resistor tolerance corners for bounded small designs.
 * Nonlinear circuits are re-solved at each corner; this is corner analysis,
 * not a proof of all interior extrema for arbitrary nonlinear networks. */
static unsigned tolerance_corners(const CpProject *p,Report *report,double *lo,double *hi) {
    size_t indices[8],count=0;
    for(size_t i=0;i<p->component_count;i++) if(p->components[i].kind=='R' && p->components[i].tolerance>0) {
        if(count==8) {issue(report,2,"corner_capacity","circuit",9,8,"At most eight uncertain resistors can be exhaustively corner-tested.");return 0;}
        indices[count++]=i;
    }
    if(!count)return 0;
    CpProject *copy=malloc(sizeof *copy);
    if(!copy){issue(report,2,"corner_allocation","circuit",0,0,"Cannot allocate corner state.");return 0;}
    *copy=*p; /* Shared immutable IR/geometry; only copied component values change. */
    double power[CP_MAX_COMPONENTS]={0},voltage[CP_MAX_COMPONENTS]={0};unsigned done=0;
    for(unsigned mask=0;mask<(1u<<count);mask++) {
        for(size_t j=0;j<count;j++){size_t i=indices[j];CpComponent *c=&copy->components[i];
            c->value=p->components[i].value*(1+((mask&(1u<<j))?1:-1)*p->components[i].tolerance);c->params[c->model->primary_param]=c->value;}
        MnaCircuit m={0};MnaResult r={0};MnaOptions o;MnaValidation v={0};mna_options_default(&o);const char *rule;char why[256];
        int ok=assemble(copy,&m,DEVICE_ANALYSIS_DC,0,NULL,&rule,why,sizeof why);if(ok){mna_solve(&m,&o,&r);ok=r.status==MNA_STATUS_SUCCESS&&mna_validate(&m,&r,&o,&v)&&v.valid;}
        if(!ok){issue(report,2,"corner_solve","circuit",mask,0,"Tolerance corner did not solve.");mna_result_free(&r);mna_circuit_free(&m);break;}
        done++;
        for(size_t i=0;i<p->net_count;i++){double value=mna_node_voltage(&r,i);lo[i]=fmin(lo[i],value);hi[i]=fmax(hi[i],value);}
        for(size_t i=0;i<p->component_count;i++)if(copy->components[i].kind=='R') {
            const CpComponent *c=&copy->components[i];double vv=terminal_voltage(c,&r);
            power[i]=fmax(power[i],vv*vv/c->value);voltage[i]=fmax(voltage[i],fabs(vv));
        }
        mna_result_free(&r);mna_circuit_free(&m);
    }
    for(size_t i=0;i<p->component_count;i++){const CpComponent*c=&p->components[i];
        if(c->power_w.known&&power[i]>c->power_w.value*p->power_derating)issue(report,2,"corner_power",c->ref,power[i],c->power_w.value*p->power_derating,"Worst enumerated resistor tolerance corner exceeds power derating.");
        if(c->voltage_v.known&&voltage[i]>c->voltage_v.value*p->voltage_derating)issue(report,2,"corner_voltage",c->ref,voltage[i],c->voltage_v.value*p->voltage_derating,"Worst enumerated resistor corner exceeds voltage derating.");
    }
    const IRNode *ts=ir_list_find(p->root,"targets");
    for(size_t i=1;i<ir_list_length(ts);i++){const IRNode*t=ir_child_at(ts,i);double a=0,b=0;num(t,2,&a);num(t,3,&b);size_t n=net_index(p,txt(t,1));
        if(lo[n]<a||hi[n]>b)issue(report,2,"corner_target",p->nets[n],lo[n]<a?lo[n]:hi[n],lo[n]<a?a:b,"Resistor tolerance corner violates voltage interval.");
    }
    free(copy);return done;
}
static void physical(const CpProject *p,Report *r) {
    if(!p->has_geometry) { issue(r,1,"geometry_missing","board",0,1,"Physical DFM not run; export/import real footprints first."); return; }
    const PcbBoard *b=&p->board; PcbDfmReport dr; pcb_dfm_report_init(&dr);
    if(pcb_dfm_validate(b,&p->fab,&dr)<0) issue(r,2,"dfm_engine","board",0,0,"Physical DFM engine failed.");
    for(size_t i=0;i<pcb_dfm_issue_count(&dr);i++) {
        const PcbDfmIssue *d=pcb_dfm_issue_get(&dr,i); char ref[64]; snprintf(ref,sizeof ref,"%s:%u",pcb_dfm_object_kind_name(d->object_kind),d->object_id);
        issue(r,d->severity==PCB_DFM_ERROR?2:1,pcb_dfm_rule_name(d->rule),ref,d->measured,d->limit,d->message);
    } pcb_dfm_report_free(&dr);
    for(size_t i=0;i<p->component_count;i++) {
        const CpComponent *c=&p->components[i]; if(c->kind=='V'||c->kind=='I') continue;
        if(!c->height_mm.known) issue(r,1,"height_unknown",c->ref,0,0,"Assembly height is not supplied by the part catalogue.");
        for(size_t k=0;k<c->pin_count;k++) { int found=0;
            for(size_t j=0;j<vec_len(b->pads);j++) if(b->pads[j].component_id==i && b->pads[j].pin_number==k+1) found=1;
            if(!found) issue(r,2,"pad_missing",c->ref,(double)(k+1),0,"Electrical pin has no physical pad.");
        }
    }
    for(size_t i=0;i<vec_len(b->vias)+vec_len(b->pads);i++) {
        double drill=i<vec_len(b->vias)?b->vias[i].drill_mm:b->pads[i-vec_len(b->vias)].drill_mm;
        if(drill>0 && b->spec.board_thickness_mm/drill>p->max_aspect) issue(r,2,"drill_aspect","hole",b->spec.board_thickness_mm/drill,p->max_aspect,"Board thickness / finished drill exceeds profile.");
    }
    /* Drill-to-drill includes pad/via combinations; hole rules ignore nets. */
    size_t hole_count=vec_len(b->vias)+vec_len(b->pads);
    for(size_t i=0;i<hole_count;i++)for(size_t j=i+1;j<hole_count;j++) {
        double x[2],y[2],d[2];size_t idx[2]={i,j};
        for(int k=0;k<2;k++)if(idx[k]<vec_len(b->vias)){const PcbVia*v=&b->vias[idx[k]];x[k]=v->x_mm;y[k]=v->y_mm;d[k]=v->drill_mm;}else{const PcbPad*a=&b->pads[idx[k]-vec_len(b->vias)];x[k]=a->x_mm;y[k]=a->y_mm;d[k]=a->drill_mm;}
        if(d[0]<=0||d[1]<=0)continue;
        Nm limit=nm_from_mm_ceil((d[0]+d[1])*.5+p->fab.min_hole_clearance_mm);
        if(geom_cmp_pt_pt((Pt){nm_from_mm(x[0]),nm_from_mm(y[0])},(Pt){nm_from_mm(x[1]),nm_from_mm(y[1])},limit)<0)
            issue(r,2,"all_hole_clearance","hole pair",hypot(x[0]-x[1],y[0]-y[1])-(d[0]+d[1])*.5,p->fab.min_hole_clearance_mm,"Finished drill edges violate separation (pad/via pairs included).");
    }
    for(size_t i=0;i<vec_len(b->pads);i++) for(size_t j=i+1;j<vec_len(b->pads);j++) {
        CopperItem a,c; double gap;
        if(pcb_copper_item(b,COPPER_ITEM_PAD,i,&a)!=PCB_COPPER_OK || pcb_copper_item(b,COPPER_ITEM_PAD,j,&c)!=PCB_COPPER_OK) continue;
        if(!copper_pair_gap_nm(&a,&c,&gap)) continue;
        /* Exact comparator for web threshold; report floats only describe it. */
        Nm req=nm_from_mm_ceil(2*p->mask_expansion+p->min_mask_web);
        a.net=1;c.net=2; CopperResult v=copper_pair_check(&a,&c,req);
        if(v==COPPER_RESULT_TOO_CLOSE || v==COPPER_RESULT_SHORT) issue(r,2,"mask_web","pad pair",gap/1e6-2*p->mask_expansion,p->min_mask_web,"Mask openings leave insufficient web (rectangular pad envelope). ");
    }
    for(size_t i=0;i<vec_len(b->placements);i++) for(size_t j=i+1;j<vec_len(b->placements);j++) {
        const PcbPlacement *a=&b->placements[i],*c=&b->placements[j];
        CopperItem x={.shape=copper_shape_rect((Pt){nm_from_mm(a->x_mm-a->width_mm/2),nm_from_mm(a->y_mm-a->height_mm/2)},(Pt){nm_from_mm(a->x_mm+a->width_mm/2),nm_from_mm(a->y_mm+a->height_mm/2)},0),.layers=1,.net=1};
        CopperItem y={.shape=copper_shape_rect((Pt){nm_from_mm(c->x_mm-c->width_mm/2),nm_from_mm(c->y_mm-c->height_mm/2)},(Pt){nm_from_mm(c->x_mm+c->width_mm/2),nm_from_mm(c->y_mm+c->height_mm/2)},0),.layers=1,.net=2};
        CopperResult v=copper_pair_check(&x,&y,nm_from_mm_ceil(p->courtyard_gap));
        if(v==COPPER_RESULT_TOO_CLOSE || v==COPPER_RESULT_SHORT) issue(r,2,"courtyard_gap","component pair",0,p->courtyard_gap,"Assembly envelopes overlap or violate separation.");
    }
}
static void quantity_json(FILE *f,const char *name,const CpQuantity *q) {
    fprintf(f,"\"%s\":{\"known\":%s",name,q->known?"true":"false");
    if(q->known) { fprintf(f,",\"value\":%.12g,\"source\":",q->value); js(f,q->source); }
    fputc('}',f);
}
static void capabilities_json(FILE *f,const DeviceModelSpec *m) {
    fprintf(f,"{\"dc\":%s,\"transient\":%s,\"ac\":%s}",device_model_supports(m,DEVICE_ANALYSIS_DC)?"true":"false",
            device_model_supports(m,DEVICE_ANALYSIS_TRANSIENT)?"true":"false",device_model_supports(m,DEVICE_ANALYSIS_AC)?"true":"false");
}
/* Per-component binding: model identity and capabilities, terminal -> pin map, parameters, ratings with
 * provenance, and catalogue identity. Lets reports and tools trace every value to its source. */
static void components_json(const CpProject *p,FILE *f) {
    fputs(",\"components\":[",f);
    for(size_t i=0;i<p->component_count;i++) {
        const CpComponent *c=&p->components[i];
        if(i) fputc(',',f);
        fputs("{\"ref\":",f); js(f,c->ref); fputs(",\"part\":",f); js(f,c->part); fprintf(f,",\"kind\":\"%c\",\"model\":",c->kind);
        if(c->model) { fputs("{\"id\":",f); js(f,c->model->id); fprintf(f,",\"version\":%u,\"source\":",c->model->version); js(f,c->model_source); fputc('}',f); }
        else fputs("null",f);
        fputs(",\"capabilities\":",f); capabilities_json(f,c->model);
        fputs(",\"terminals\":{",f);
        for(size_t t=0;c->model && t<c->model->terminal_count;t++) { if(t) fputc(',',f); js(f,c->model->terminals[t].name); fputc(':',f); js(f,c->pins[c->terminal_pin[t]]); }
        fputs("},\"params\":{",f);
        for(size_t k=0;c->model && k<c->model->param_count;k++) { if(k) fputc(',',f); js(f,c->model->params[k].name); fprintf(f,":%.12g",c->params[k]); }
        fputs("},\"ratings\":{",f);
        quantity_json(f,"power_w",&c->power_w); fputc(',',f); quantity_json(f,"voltage_v",&c->voltage_v); fputc(',',f); quantity_json(f,"height_mm",&c->height_mm);
        fprintf(f,"},\"tolerance\":%.12g,\"identity\":{\"symbol\":",c->tolerance); js(f,c->symbol);
        fputs(",\"footprint\":",f); js(f,c->footprint); fputs(",\"manufacturer\":",f); js(f,c->manufacturer);
        fputs(",\"mpn\":",f); js(f,c->mpn); fputs(",\"datasheet\":",f); js(f,c->datasheet);
        fputs("},\"provenance\":",f); js(f,c->provenance); fputs(",\"catalogue\":",f); js(f,c->catalogue);
        fputs(",\"override_provenance\":",f); js(f,c->override_provenance); fputc('}',f);
    }
    fputc(']',f);
}
int cp_check_catalogue(const char *path,FILE *f,char *e,size_t cap) {
    CpProject *p=malloc(sizeof *p); IRError ie; int ok=0;
    if(!p) return errorf(e,cap,"out of memory");
    cp_init(p); p->root=ir_parse_file(&p->arena,path,&ie);
    if(!p->root) { errorf(e,cap,"Parse at %zu:%zu: %s",ie.line,ie.column,ie.message?ie.message:"I/O failure"); goto done; }
    if(strcmp(txt(p->root,0),"catalogue")) { errorf(e,cap,"Expected (catalogue (part ...) ...)"); goto done; }
    for(size_t i=1;i<ir_list_length(p->root);i++) {
        const IRNode *part=ir_child_at(p->root,i); PartInfo info;
        if(!read_part(p,part,&info,e,cap)) goto done;
        for(size_t j=1;j<i;j++) if(!strcmp(txt(part,1),txt(ir_child_at(p->root,j),1))) { errorf(e,cap,"Duplicate part %s",txt(part,1)); goto done; }
    }
    fputs("{\"parts\":[",f);
    for(size_t i=1;i<ir_list_length(p->root);i++) {
        const IRNode *part=ir_child_at(p->root,i); PartInfo info; read_part(p,part,&info,e,cap);
        if(i>1) fputc(',',f);
        fputs("{\"id\":",f); js(f,txt(part,1)); fprintf(f,",\"kind\":\"%c\",\"model\":",info.kind);
        if(info.model) js(f,info.model->id); else fputs("null",f);
        fputs(",\"model_source\":",f); js(f,info.model_source); fputs(",\"terminals\":{",f);
        for(size_t t=0;info.model && t<info.model->terminal_count;t++) { if(t) fputc(',',f); js(f,info.model->terminals[t].name); fputc(':',f); js(f,info.terminal_pin[t]); }
        fputs("},\"ratings\":{",f);
        quantity_json(f,"power_w",&info.power_w); fputc(',',f); quantity_json(f,"voltage_v",&info.voltage_v); fputc(',',f); quantity_json(f,"height_mm",&info.height_mm);
        fputs("}}",f);
    }
    fputs("]}\n",f); ok=!ferror(f);
 done:
    cp_free(p); free(p); return ok;
}
int cp_models_json(FILE *f) {
    ModelRegistry reg; char err[256];
    model_registry_init(&reg);
    if(!model_registry_register_builtin_devices(&reg,err,sizeof err)) { model_registry_free(&reg); fprintf(stderr,"MODEL ERROR: %s\n",err); return 2; }
    fprintf(f,"{\"contract_version\":%u,\"models\":[",DEVICE_MODEL_CONTRACT_VERSION);
    for(size_t i=0;i<model_registry_device_count(&reg);i++) {
        const DeviceModelSpec *m=model_registry_device_at(&reg,i);
        if(i) fputc(',',f);
        fputs("{\"id\":",f); js(f,m->id); fprintf(f,",\"version\":%u,\"family\":",m->version); js(f,m->family);
        fputs(",\"summary\":",f); js(f,m->summary); fputs(",\"assumptions\":",f); js(f,m->assumptions);
        fputs(",\"terminals\":[",f);
        for(size_t t=0;t<m->terminal_count;t++) { if(t) fputc(',',f); fputs("{\"name\":",f); js(f,m->terminals[t].name);
            fputs(",\"role\":",f); js(f,m->terminals[t].role); fputs(",\"pin_names\":",f);
            if(m->terminals[t].pin_names) js(f,m->terminals[t].pin_names); else fputs("null",f); fputc('}',f); }
        fputs("],\"params\":[",f);
        for(size_t k=0;k<m->param_count;k++) { const DeviceParam *q=&m->params[k]; if(k) fputc(',',f);
            fputs("{\"name\":",f); js(f,q->name); fputs(",\"unit\":",f); js(f,device_unit_symbol(q->unit));
            fprintf(f,",\"minimum\":%.12g,\"maximum\":%.12g,\"min_exclusive\":%s,\"default\":",q->minimum,q->maximum,q->min_exclusive?"true":"false");
            if(q->has_default) fprintf(f,"%.12g",q->default_value); else fputs("null",f);
            fprintf(f,",\"bound_to_value\":%s,\"meaning\":",(int)k==m->primary_param?"true":"false"); js(f,q->meaning); fputc('}',f); }
        fputs("],\"capabilities\":",f); capabilities_json(f,m); fprintf(f,",\"state_count\":%zu}",m->state_count);
    }
    fputs("]}\n",f);
    model_registry_free(&reg);
    return ferror(f)?3:0;
}
int cp_analyze(const CpProject *p,FILE *f) {
    MnaCircuit m={0}; MnaResult result={0}; MnaOptions opt; MnaValidation valid={0}; mna_options_default(&opt);
    const char *rule="mna_failure"; char why[256];
    int supported=assemble(p,&m,DEVICE_ANALYSIS_DC,0,NULL,&rule,why,sizeof why); if(supported) { mna_solve(&m,&opt,&result); if(result.status==MNA_STATUS_SUCCESS) mna_validate(&m,&result,&opt,&valid); }
    int solved=supported && result.status==MNA_STATUS_SUCCESS && valid.valid;
    fprintf(f,"{\"schema\":2,\"name\":"); js(f,p->name); fprintf(f,",\"dc_solved\":%s,\"physical_checked\":%s,\"issues\":[",solved?"true":"false",p->has_geometry?"true":"false");
    Report r={.f=f};
    if(!solved) issue(&r,2,rule,"circuit",supported?result.status:0,0,supported?"DC solve failed: check floating nodes, ideal-source loops and nonlinear convergence.":why);
    size_t degree[CP_MAX_NETS]={0};
    for(size_t i=0;i<p->component_count;i++) for(size_t j=0;j<p->components[i].pin_count;j++) degree[p->components[i].nodes[j]]++;
    for(size_t i=0;i<p->net_count;i++) if(degree[i]<2) issue(&r,2,"open_node",p->nets[i],(double)degree[i],2,"Net has fewer than two terminals.");
    if(solved) {
        for(size_t i=0;i<p->component_count;i++) {
            const CpComponent *c=&p->components[i]; if(!c->model||c->kind=='V'||c->kind=='I') continue;
            double v=terminal_voltage(c,&result);
            if(!c->voltage_v.known) issue(&r,1,"rating_unknown",c->ref,0,0,"Voltage rating not supplied by catalogue; not verified.");
            else if(fabs(v)>c->voltage_v.value*p->voltage_derating) issue(&r,2,"voltage_derating",c->ref,fabs(v),c->voltage_v.value*p->voltage_derating,"Operating voltage exceeds derated rating.");
            if(c->kind=='R') {
                double watts=v*v/c->value;
                if(!c->power_w.known) issue(&r,1,"rating_unknown",c->ref,watts,0,"Resistor power rating is unknown.");
                else if(watts>c->power_w.value*p->power_derating) issue(&r,2,"power_derating",c->ref,watts,c->power_w.value*p->power_derating,"V^2/R exceeds derated resistor power.");
            }
        }
        const IRNode *ts=ir_list_find(p->root,"targets");
        for(size_t i=1;i<ir_list_length(ts);i++) { const IRNode *t=ir_child_at(ts,i); double lo=0,hi=0; num(t,2,&lo);num(t,3,&hi);
            double v=mna_node_voltage(&result,net_index(p,txt(t,1)));
            if(v<lo || v>hi) issue(&r,2,"voltage_target",txt(t,1),v,v<lo?lo:hi,"Solved node voltage is outside required interval.");
        }
    }
    /* Conservative whole-circuit supply-current budget, not a routed-network solve.
       Apply it to every segment: pessimistic but cannot understate DC branch demand
       for the supported passive networks. No temperature rise is claimed. */
    double budget=0; size_t source_index=0;
    if(solved) for(size_t i=0;i<p->component_count;i++) {
        const CpComponent *c=&p->components[i];
        if(c->kind=='V' || c->kind=='L') budget+=fabs(mna_voltage_source_current(&m,&result,source_index++));
        if(c->kind=='I') budget+=fabs(c->value);
    }
    if(solved && p->has_geometry) for(size_t i=0;i<vec_len(p->board.traces);i++) {
        const PcbTrace *t=&p->board.traces[i];
        double area=t->width_mm*p->board.spec.copper_thickness_mm;
        double resistance=.00001724*hypot(t->x2_mm-t->x1_mm,t->y2_mm-t->y1_mm)/area;
        char object[64];snprintf(object,sizeof object,"trace:%zu",i);
        if(budget/area>p->max_current_density) issue(&r,2,"current_density_budget",object,budget/area,p->max_current_density,"Conservative DC supply-current / copper area exceeds user heuristic (not IPC thermal certification).");
        if(budget*resistance>p->max_drop) issue(&r,2,"trace_drop_budget",object,budget*resistance,p->max_drop,"I_budget * rho_20C * length / copper area exceeds allowed segment drop.");
    }
    double corner_lo[CP_MAX_NETS]={0},corner_hi[CP_MAX_NETS]={0};
    if(solved)for(size_t i=0;i<p->net_count;i++)corner_lo[i]=corner_hi[i]=mna_node_voltage(&result,i);
    unsigned corner_count=solved?tolerance_corners(p,&r,corner_lo,corner_hi):0;
    physical(p,&r);
    fputs("],\"nodes\":[",f);
    for(size_t i=0;i<p->net_count;i++) { if(i) fputc(',',f); fputs("{\"name\":",f);js(f,p->nets[i]); if(solved) fprintf(f,",\"voltage_v\":%.12g}",mna_node_voltage(&result,i)); else fputs(",\"voltage_v\":null}",f); }
    fprintf(f,"],\"tolerance_corner_count\":%u,\"corner_nodes\":[",corner_count);
    if(corner_count)for(size_t i=0;i<p->net_count;i++){if(i)fputc(',',f);fputs("{\"name\":",f);js(f,p->nets[i]);fprintf(f,",\"min_v\":%.12g,\"max_v\":%.12g}",corner_lo[i],corner_hi[i]);}
    fputs("],\"calculations\":[",f); int count=0;
    if(solved) for(size_t i=0;i<p->component_count;i++) { const CpComponent *c=&p->components[i]; if(c->kind!='R')continue;
        double v=terminal_voltage(c,&result);
        if(count++)fputc(',',f);
        fputs("{\"ref\":",f);js(f,c->ref);
        fprintf(f,",\"formula\":\"I=(Va-Vb)/R; P=I*I*R\",\"voltage_v\":%.12g,\"resistance_ohm\":%.12g,\"current_a\":%.12g,\"power_w\":%.12g}",v,c->value,v/c->value,v*v/c->value);
    }
    fputs("],\"trace_calculations\":[",f);
    if(solved && p->has_geometry) for(size_t i=0;i<vec_len(p->board.traces);i++) {
        const PcbTrace *t=&p->board.traces[i];double area=t->width_mm*p->board.spec.copper_thickness_mm;
        double resistance=.00001724*hypot(t->x2_mm-t->x1_mm,t->y2_mm-t->y1_mm)/area;
        if(i)fputc(',',f);
        fprintf(f,"{\"trace\":%zu,\"current_budget_a\":%.12g,\"resistance_ohm_20c\":%.12g,\"drop_budget_v\":%.12g,\"loss_budget_w\":%.12g}",i,budget,resistance,budget*resistance,budget*budget*resistance);
    }
    fputc(']',f); components_json(p,f);
    fprintf(f,",\"mna_residual\":%.12g,\"kcl_residual_a\":%.12g,\"errors\":%d,\"warnings\":%d,\"coverage\":\"DC R/C-open/L-short/D/V/I and ideal connectors; exact supported copper; rectangular assembly envelopes. No AC, thermal field, SI, EMC, zones or fabrication certification.\"}\n",solved?result.maximum_residual:0,solved?result.maximum_kcl_residual_a:0,r.errors,r.warnings);
    mna_result_free(&result);mna_circuit_free(&m);return ferror(f)?3:(r.errors?2:0);
}
int cp_transient(const CpProject *p,double dt,size_t steps,FILE *f) {
    if(!isfinite(dt)||dt<=0||dt>1000||steps==0||steps>100000) return 2;
    CpState state[CP_MAX_COMPONENTS], next;
    memset(state,0,sizeof state);
    fputs("time_s",f); for(size_t i=0;i<p->net_count;i++) fprintf(f,",%s_V",p->nets[i]); fputc('\n',f);
    /* zero stored energy at t=0; ideal DC sources step on for first solve. */
    for(size_t step=1;step<=steps;step++) {
        MnaCircuit m={0};MnaResult r={0};MnaOptions o;MnaValidation v={0};mna_options_default(&o);const char *rule;char why[256];
        if(!assemble(p,&m,DEVICE_ANALYSIS_TRANSIENT,dt,state,&rule,why,sizeof why)) {mna_circuit_free(&m);fprintf(stderr,"TRANSIENT %s: %s\n",rule,why);return 2;}
        mna_solve(&m,&o,&r);
        if(r.status!=MNA_STATUS_SUCCESS || !mna_validate(&m,&r,&o,&v) || !v.valid) {mna_result_free(&r);mna_circuit_free(&m);return 2;}
        fprintf(f,"%.12g",step*dt); for(size_t i=0;i<p->net_count;i++)fprintf(f,",%.12g",mna_node_voltage(&r,i));fputc('\n',f);
        /* Commit model state only after the step solved and validated. */
        for(size_t i=0;i<p->component_count;i++) {const CpComponent *c=&p->components[i]; size_t nodes[DEVICE_MODEL_MAX_TERMINALS];
            if(!c->model || !c->model->state_count) continue;
            DeviceStampContext ctx={DEVICE_ANALYSIS_TRANSIENT,dt,state[i]};
            terminal_nodes(c,nodes);
            if(!c->model->commit_transient(&r,nodes,c->params,&ctx,next)) {mna_result_free(&r);mna_circuit_free(&m);return 2;}
            memcpy(state[i],next,sizeof next);
        }
        mna_result_free(&r);mna_circuit_free(&m);
    }
    return ferror(f)?3:0;
}
