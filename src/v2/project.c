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
}
void cp_free(CpProject *p) { pcb_board_free(&p->board); arena_destroy(&p->arena); memset(p,0,sizeof *p); }
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
        const IRNode *part=ir_child_at(parts,i);
        if(strcmp(txt(part,0),"part") || !id_ok(txt(part,1)) || !fields(part,2,"|kind||symbol||footprint||power_w||voltage_v||tolerance||is_a||ideality||height_mm||manufacturer||mpn||datasheet|",e,cap)) return errorf(e,cap,"Invalid part definition %s",txt(part,1));
        for(size_t j=1;j<i;j++) if(!strcmp(txt(part,1),txt(ir_child_at(parts,j),1))) return errorf(e,cap,"Duplicate part %s",txt(part,1));
    }
    for(size_t i=1;i<ir_list_length(cs);i++) {
        const IRNode *n=ir_child_at(cs,i);
        if(p->component_count==CP_MAX_COMPONENTS || strcmp(txt(n,0),"component") || !id_ok(txt(n,1)) || comp_index(p,txt(n,1))!=SIZE_MAX) return errorf(e,cap,"Invalid/duplicate component %s",txt(n,1));
        if(!fields(n,2,"|part||value||pins||at|",e,cap)) return 0;
        const IRNode *part=ir_list_find_named(parts,"part",field(n,"part"));
        if(!part) return errorf(e,cap,"%s: part '%s' is not in catalogue",txt(n,1),field(n,"part"));
        CpComponent *c=&p->components[p->component_count++];
        c->ref=txt(n,1); c->part=txt(part,1); c->symbol=field(part,"symbol"); c->footprint=field(part,"footprint");
        c->manufacturer=field(part,"manufacturer"); c->mpn=field(part,"mpn"); c->datasheet=field(part,"datasheet");
        const char *kind=field(part,"kind");
        if(strlen(kind)!=1 || !strchr("RCLDVIJU",*kind)) return errorf(e,cap,"%s: unknown kind",c->ref);
        c->kind=*kind;
        if(c->kind!='V' && c->kind!='I' && (!*c->symbol || !*c->footprint)) return errorf(e,cap,"%s: physical part needs symbol and footprint",c->ref);
        /* Model-bearing kinds must state their value; an omitted value is never defaulted. */
        if(strchr("RCLVI",c->kind) && !ir_list_find(n,"value")) return errorf(e,cap,"%s: kind %c requires an explicit (value ...)",c->ref,c->kind);
        if(!scalar(n,"value",0,-1e12,1e12,&c->value) || (strchr("RCL",c->kind) && c->value<=0)) return errorf(e,cap,"%s: invalid SI value",c->ref);
#define PVAL(k,d,lo,hi,f) if(!scalar(part,k,d,lo,hi,&c->f)) return errorf(e,cap,"%s: invalid %s",c->ref,k)
        PVAL("power_w",0,0,1e9,power_w); PVAL("voltage_v",0,0,1e9,voltage_v);
        PVAL("height_mm",0,0,100,height_mm); PVAL("tolerance",0,0,.99,tolerance); PVAL("is_a",1e-14,1e-30,1,is_a); PVAL("ideality",1,.1,10,ideality);
#undef PVAL
        const IRNode *pins=ir_list_find(n,"pins");
        for(size_t j=1;j<ir_list_length(pins);j++) {
            const IRNode *pin=ir_child_at(pins,j); size_t ni=net_index(p,txt(pin,1));
            if(ir_list_length(pin)!=2 || !id_ok(txt(pin,0)) || ni==SIZE_MAX || c->pin_count==CP_MAX_PINS) return errorf(e,cap,"%s: invalid pin/net mapping",c->ref);
            for(size_t k=0;k<c->pin_count;k++) if(!strcmp(c->pins[k],txt(pin,0))) return errorf(e,cap,"%s: duplicate pin",c->ref);
            c->pins[c->pin_count]=txt(pin,0); c->nodes[c->pin_count++]=ni;
        }
        if(c->pin_count==0 || (strchr("RCLDVI",c->kind) && (c->pin_count!=2 || strcmp(c->pins[0],"1") || strcmp(c->pins[1],"2") || c->nodes[0]==c->nodes[1]))) return errorf(e,cap,"%s: model requires ordered distinct pins 1 and 2",c->ref);
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
        else rc=pcb_add_placement(&p->board,(PcbPlacement){(uint32_t)ci,v[0],v[1],0,v[2],v[3],v[4]});
        if(rc) return errorf(e,cap,"Invalid %s geometry",type);
    }
    return 1;
}
/* Independent constant-current device for the existing Newton/MNA engine. */
typedef struct { size_t a,b; double i; } Current;
static size_t no_unknown(const void *d) { (void)d; return 0; }
static int current_load(void *d,const MnaDevice *dev,MnaAssembly *a) {
    (void)dev; Current *c=d;
    return (!c->a || mna_assembly_add_residual(a,c->a-1,c->i)) && (!c->b || mna_assembly_add_residual(a,c->b-1,-c->i));
}
static const MnaDeviceOps current_ops={MNA_DEVICE_LINEAR,no_unknown,current_load,free,NULL};
static int add_current(MnaCircuit *m,size_t a,size_t b,double i) {
    Current *c=malloc(sizeof *c); if(!c) return 0; *c=(Current){a,b,i};
    if(!mna_add_device(m,&current_ops,c)) { free(c); return 0; } return 1;
}
static int assemble(const CpProject *p,MnaCircuit *m,double dt,const double *state) {
    mna_circuit_init(m,p->net_count-1);
    for(size_t i=0;i<p->component_count;i++) {
        const CpComponent *c=&p->components[i]; size_t a=c->nodes[0],b=c->nodes[1]; int ok=1;
        switch(c->kind) {
        case 'R': ok=mna_add_resistor(m,a,b,c->value); break;
        case 'V': ok=mna_add_voltage_source(m,a,b,c->value); break;
        case 'I': ok=add_current(m,a,b,c->value); break;
        case 'D': ok=mna_add_diode(m,b,a,c->is_a,c->ideality,300); break;
        case 'C': if(dt>0) ok=mna_add_resistor(m,a,b,dt/c->value) && add_current(m,a,b,-c->value/dt*state[i]); break;
        case 'L': if(dt>0) ok=mna_add_resistor(m,a,b,c->value/dt) && add_current(m,a,b,state[i]);
                  else ok=mna_add_voltage_source(m,a,b,0);
                  break;
        case 'J': break; /* Explicit ideal connector boundary; no device stamp. */
        default: return 0; /* Never silently omit an active unmodelled device. */
        }
        if(!ok) return 0;
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
        for(size_t j=0;j<count;j++){size_t i=indices[j];copy->components[i].value=p->components[i].value*(1+((mask&(1u<<j))?1:-1)*p->components[i].tolerance);}
        MnaCircuit m={0};MnaResult r={0};MnaOptions o;MnaValidation v={0};mna_options_default(&o);
        int ok=assemble(copy,&m,0,NULL);if(ok){mna_solve(&m,&o,&r);ok=r.status==MNA_STATUS_SUCCESS&&mna_validate(&m,&r,&o,&v)&&v.valid;}
        if(!ok){issue(report,2,"corner_solve","circuit",mask,0,"Tolerance corner did not solve.");mna_result_free(&r);mna_circuit_free(&m);break;}
        done++;
        for(size_t i=0;i<p->net_count;i++){double value=mna_node_voltage(&r,i);lo[i]=fmin(lo[i],value);hi[i]=fmax(hi[i],value);}
        for(size_t i=0;i<p->component_count;i++)if(copy->components[i].kind=='R') {
            const CpComponent *c=&copy->components[i];double vv=mna_node_voltage(&r,c->nodes[0])-mna_node_voltage(&r,c->nodes[1]);
            power[i]=fmax(power[i],vv*vv/c->value);voltage[i]=fmax(voltage[i],fabs(vv));
        }
        mna_result_free(&r);mna_circuit_free(&m);
    }
    for(size_t i=0;i<p->component_count;i++){const CpComponent*c=&p->components[i];
        if(c->power_w>0&&power[i]>c->power_w*p->power_derating)issue(report,2,"corner_power",c->ref,power[i],c->power_w*p->power_derating,"Worst enumerated resistor tolerance corner exceeds power derating.");
        if(c->voltage_v>0&&voltage[i]>c->voltage_v*p->voltage_derating)issue(report,2,"corner_voltage",c->ref,voltage[i],c->voltage_v*p->voltage_derating,"Worst enumerated resistor corner exceeds voltage derating.");
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
        if(c->height_mm==0) issue(r,1,"height_unknown",c->ref,0,0,"Assembly height is not supplied by the part catalogue.");
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
int cp_analyze(const CpProject *p,FILE *f) {
    MnaCircuit m={0}; MnaResult result={0}; MnaOptions opt; MnaValidation valid={0}; mna_options_default(&opt);
    int supported=assemble(p,&m,0,NULL); if(supported) { mna_solve(&m,&opt,&result); if(result.status==MNA_STATUS_SUCCESS) mna_validate(&m,&result,&opt,&valid); }
    int solved=supported && result.status==MNA_STATUS_SUCCESS && valid.valid;
    fprintf(f,"{\"schema\":2,\"name\":"); js(f,p->name); fprintf(f,",\"dc_solved\":%s,\"physical_checked\":%s,\"issues\":[",solved?"true":"false",p->has_geometry?"true":"false");
    Report r={.f=f};
    if(!solved) issue(&r,2,supported?"mna_failure":"unsupported_model","circuit",supported?result.status:0,0,supported?"DC solve failed: check floating nodes, ideal-source loops and nonlinear convergence.":"An active device lacks a supported model; analysis is blocked.");
    size_t degree[CP_MAX_NETS]={0};
    for(size_t i=0;i<p->component_count;i++) for(size_t j=0;j<p->components[i].pin_count;j++) degree[p->components[i].nodes[j]]++;
    for(size_t i=0;i<p->net_count;i++) if(degree[i]<2) issue(&r,2,"open_node",p->nets[i],(double)degree[i],2,"Net has fewer than two terminals.");
    if(solved) {
        for(size_t i=0;i<p->component_count;i++) {
            const CpComponent *c=&p->components[i]; if(c->kind=='V'||c->kind=='I'||c->kind=='J') continue;
            double v=mna_node_voltage(&result,c->nodes[0])-mna_node_voltage(&result,c->nodes[1]);
            if(c->voltage_v==0) issue(&r,1,"rating_unknown",c->ref,0,0,"Voltage rating not supplied by catalogue; not verified.");
            else if(fabs(v)>c->voltage_v*p->voltage_derating) issue(&r,2,"voltage_derating",c->ref,fabs(v),c->voltage_v*p->voltage_derating,"Operating voltage exceeds derated rating.");
            if(c->kind=='R') {
                double watts=v*v/c->value;
                if(c->power_w==0) issue(&r,1,"rating_unknown",c->ref,watts,0,"Resistor power rating is unknown.");
                else if(watts>c->power_w*p->power_derating) issue(&r,2,"power_derating",c->ref,watts,c->power_w*p->power_derating,"V^2/R exceeds derated resistor power.");
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
        double v=mna_node_voltage(&result,c->nodes[0])-mna_node_voltage(&result,c->nodes[1]);
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
    fprintf(f,"],\"mna_residual\":%.12g,\"kcl_residual_a\":%.12g,\"errors\":%d,\"warnings\":%d,\"coverage\":\"DC R/C-open/L-short/D/V/I and ideal connectors; exact supported copper; rectangular assembly envelopes. No AC, thermal field, SI, EMC, zones or fabrication certification.\"}\n",solved?result.maximum_residual:0,solved?result.maximum_kcl_residual_a:0,r.errors,r.warnings);
    mna_result_free(&result);mna_circuit_free(&m);return ferror(f)?3:(r.errors?2:0);
}
int cp_transient(const CpProject *p,double dt,size_t steps,FILE *f) {
    if(!isfinite(dt)||dt<=0||dt>1000||steps==0||steps>100000) return 2;
    double state[CP_MAX_COMPONENTS]={0};
    fputs("time_s",f); for(size_t i=0;i<p->net_count;i++) fprintf(f,",%s_V",p->nets[i]); fputc('\n',f);
    /* zero stored energy at t=0; ideal DC sources step on for first solve. */
    for(size_t step=1;step<=steps;step++) {
        MnaCircuit m={0};MnaResult r={0};MnaOptions o;MnaValidation v={0};mna_options_default(&o);
        if(!assemble(p,&m,dt,state)) {mna_circuit_free(&m);return 2;}
        mna_solve(&m,&o,&r);
        if(r.status!=MNA_STATUS_SUCCESS || !mna_validate(&m,&r,&o,&v) || !v.valid) {mna_result_free(&r);mna_circuit_free(&m);return 2;}
        fprintf(f,"%.12g",step*dt); for(size_t i=0;i<p->net_count;i++)fprintf(f,",%.12g",mna_node_voltage(&r,i));fputc('\n',f);
        for(size_t i=0;i<p->component_count;i++) {const CpComponent *c=&p->components[i]; double voltage=mna_node_voltage(&r,c->nodes[0])-mna_node_voltage(&r,c->nodes[1]);
            if(c->kind=='C')state[i]=voltage;
            if(c->kind=='L')state[i]+=dt/c->value*voltage;
        }
        mna_result_free(&r);mna_circuit_free(&m);
    }
    return ferror(f)?3:0;
}
