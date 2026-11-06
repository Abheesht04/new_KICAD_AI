#include "v2/project.h"
#include "v2/transformer.h"
#include "e_series.h"
#include "copper_project.h"
#include "excellon.h"
#include "board.h"
#include "dfm.h"
#include "kicad_cli.h"
#include "gerber.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
static void json_string(FILE *f,const char *s) {
    fputc('"',f);
    for(;s && *s;s++) { unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\') fprintf(f,"\\%c",c); else if(c<32) fprintf(f,"\\u%04x",c); else fputc(c,f); }
    fputc('"',f);
}
static const char *plating_name(ExcPlating p) {
    return p==EXC_PLATING_PLATED?"plated":p==EXC_PLATING_NONPLATED?"non_plated":p==EXC_PLATING_MIXED?"mixed":"unknown";
}
static const char *function_name(ExcDrillFunction d) {
    return d==EXC_DRILL_COMPONENT?"component":d==EXC_DRILL_VIA?"via":d==EXC_DRILL_MECHANICAL?"mechanical":d==EXC_DRILL_OTHER?"other":"unknown";
}
/* drill FILE: an Excellon drill file read independently of KiCad (src/v2_src/excellon.c), in mm. Exit 0 when it parsed with
 * no error issue, 2 when the file has errors (the content is still reported), 3 when it could not be read. */
static int drill_json(const char *path) {
    Arena arena; arena_init(&arena,1<<16); ExcFile *f=malloc(sizeof *f); if(!f) return 3;
    ExcStatus st=exc_parse_path(&arena,path,f);
    if(st==EXC_ERR_IO||st==EXC_ERR_ARGUMENT||st==EXC_ERR_MEMORY) { fprintf(stderr,"DRILL %s: %s\n",exc_status_name(st),path); free(f); arena_destroy(&arena); return 3; }
    printf("{\"status\":\"%s\",\"units\":\"%s\",\"decimal\":%s,\"absolute\":%s,\"file_plating\":\"%s\",\"layers\":[%d,%d],\"tools\":[",
           exc_status_name(st),f->units==EXC_UNITS_METRIC?"mm":f->units==EXC_UNITS_INCH?"inch":"unknown",f->zeros==EXC_ZEROS_DECIMAL?"true":"false",
           f->absolute?"true":"false",plating_name(f->file_plating),f->layer_from,f->layer_to);
    for(size_t i=0;i<f->tool_count;i++) { const ExcTool *t=&f->tools[i];
        printf("%s{\"number\":%u,\"diameter_mm\":%.6f,\"plating\":\"%s\",\"function\":\"%s\",\"hits\":%zu,\"slots\":%zu}",i?",":"",
               (unsigned)t->number,(double)t->diameter/1e6,plating_name(t->plating),function_name(t->function),t->hit_count,t->slot_count); }
    fputs("],\"hits\":[",stdout);
    for(size_t i=0;i<f->hit_count;i++) printf("%s[%.6f,%.6f,%u]",i?",":"",(double)f->hits[i].pos.x/1e6,(double)f->hits[i].pos.y/1e6,(unsigned)f->tools[f->hits[i].tool].number);
    fputs("],\"issues\":[",stdout);
    for(size_t i=0;i<f->issue_count;i++) { const ExcIssue *x=&f->issues[i];
        printf("%s{\"severity\":\"%s\",\"line\":%u,\"tool\":%u,\"message\":",i?",":"",x->severity==EXC_SEV_ERROR?"error":x->severity==EXC_SEV_WARNING?"warning":"info",(unsigned)x->line,(unsigned)x->tool);
        json_string(stdout,exc_issue_text(x->code)); fputc('}',stdout); }
    printf("],\"issues_dropped\":%zu,\"errors\":%zu,\"warnings\":%zu}\n",f->issues_dropped,f->error_count,f->warning_count);
    int rc=st==EXC_OK?0:2; free(f); arena_destroy(&arena); return rc;
}
static void project_json(const CopperProject *p) {
    printf("{\"name\":"); json_string(stdout,p->name);
    const char *const keys[]={"root_dir","project_file","schematic_file","board_file","copper_dir","kicad_cli"};
    const char *const vals[]={p->root_dir,p->project_file,p->schematic_file,p->board_file,p->copper_dir,p->kicad_cli_path};
    for(size_t i=0;i<6;i++) { printf(",\"%s\":",keys[i]); if(vals[i]) json_string(stdout,vals[i]); else fputs("null",stdout); }
    printf(",\"has_schematic\":%s,\"has_board\":%s,\"has_symbol_table\":%s,\"has_footprint_table\":%s}\n",p->has_schematic?"true":"false",
           p->has_board?"true":"false",p->has_symbol_table?"true":"false",p->has_footprint_table?"true":"false");
}
/* kicad-new PARENT NAME: an empty native KiCad 9 project in PARENT/NAME, created atomically, never over anything.
 * kicad-open DIR: what an existing KiCad project folder holds. Exit 2 with the reason on stderr otherwise. */
static int kicad_project(int create,const char *a,const char *b) {
    Arena arena; arena_init(&arena,1<<16); CopperProject p; CopperProjectError err={0};
    CopperProjectCreateOptions o={a,b,true,NULL};
    CopperProjectStatus st=create?copper_project_create(&arena,&o,&p,&err):copper_project_open(&arena,a,&p,&err);
    if(st==COPPER_PROJECT_OK) project_json(&p);
    else fprintf(stderr,"PROJECT ERROR: %s: %s\n",copper_project_status_name(st),copper_project_status_message(st));
    arena_destroy(&arena); return st==COPPER_PROJECT_OK?0:2;
}
static const char *const PAD_TYPES[]={"smd","thru_hole","np_thru_hole","connect"};
static const char *const PAD_SHAPES[]={"rect","roundrect","circle","oval","trapezoid","custom","other"};
static void net_json(const Board *b,int32_t net) { if(net<0) fputs("null",stdout); else json_string(stdout,b->nets[net].name); }
/* board FILE: a .kicad_pcb read by the C board model (src/v2_src/board.c), lengths as exact integer nm and angles in
 * micro-degrees. Exit 0; 2 with {"status","line","column","item","context","message"} when it cannot be read. */
static int board_json(const char *path) {
    Arena arena; arena_init(&arena,1<<20); Board b; BoardError e={0};
    BoardStatus st=board_load(&arena,path,&b,&e);
    if(st!=BOARD_OK) {
        printf("{\"status\":\"%s\",\"line\":%zu,\"column\":%zu,\"item\":%zu,\"context\":",board_status_name(st),e.line,e.column,e.item);
        json_string(stdout,e.context?e.context:""); fputs(",\"message\":",stdout); json_string(stdout,e.message?e.message:""); puts("}");
        arena_destroy(&arena); return 2;
    }
    printf("{\"status\":\"OK\",\"version\":%lld,\"generator\":",(long long)b.version); json_string(stdout,b.generator?b.generator:"");
    fputs(",\"generator_version\":",stdout); json_string(stdout,b.generator_version?b.generator_version:"");
    printf(",\"thickness_nm\":%lld,\"copper_layers\":%zu,\"nets\":[",(long long)b.thickness,b.copper_layers);
    for(size_t i=0;i<b.net_count;i++) { if(i) fputc(',',stdout); json_string(stdout,b.nets[i].name); }
    fputs("],\"footprints\":[",stdout);
    for(size_t i=0;i<b.footprint_count;i++) { const BoardFootprint *f=&b.footprints[i];
        if(i) fputc(',',stdout);
        fputs("{\"ref\":",stdout); json_string(stdout,f->reference); fputs(",\"lib_id\":",stdout); json_string(stdout,f->lib_id);
        printf(",\"x_nm\":%lld,\"y_nm\":%lld,\"angle_udeg\":%lld,\"back\":%s,\"pads\":[",(long long)f->at.x,(long long)f->at.y,(long long)f->angle_udeg,f->back?"true":"false");
        for(size_t k=0;k<f->pad_count;k++) { const BoardPad *p=&b.pads[f->first_pad+k];
            if(k) fputc(',',stdout);
            fputs("{\"number\":",stdout); json_string(stdout,p->number);
            printf(",\"type\":\"%s\",\"shape\":\"%s\",\"x_nm\":%lld,\"y_nm\":%lld,\"exact\":%s,\"angle_udeg\":%lld,\"size_nm\":[%lld,%lld]",
                   PAD_TYPES[p->type],PAD_SHAPES[p->shape],(long long)p->at.x,(long long)p->at.y,p->exact?"true":"false",(long long)p->angle_udeg,(long long)p->size_x,(long long)p->size_y);
            if(p->has_drill) printf(",\"drill_nm\":[%lld,%lld],\"drill_oval\":%s",(long long)p->drill_x,(long long)p->drill_y,p->drill_oval?"true":"false");
            printf(",\"layers\":%llu",(unsigned long long)p->layers);
            fputs(",\"net\":",stdout); net_json(&b,p->net); fputc('}',stdout); }
        fputs("]}",stdout); }
    printf("],\"tracks\":%zu,\"vias\":[",b.track_count);
    for(size_t i=0;i<b.via_count;i++) { const BoardVia *v=&b.vias[i];
        printf("%s{\"x_nm\":%lld,\"y_nm\":%lld,\"size_nm\":%lld,\"drill_nm\":%lld,\"type\":\"%s\",\"net\":",i?",":"",(long long)v->at.x,(long long)v->at.y,
               (long long)v->size,(long long)v->drill,v->type==BOARD_VIA_BLIND?"blind":v->type==BOARD_VIA_MICRO?"micro":"through");
        net_json(&b,v->net); fputc('}',stdout); }
    size_t keepouts=0; for(size_t i=0;i<b.zone_count;i++) keepouts+=b.zones[i].keepout;
    printf("],\"zones\":%zu,\"keepout_zones\":%zu,\"outline\":{\"edges\":%zu,\"loops\":%zu,\"open_loops\":%zu,\"closed\":%s,\"dangling_ends\":%zu,\"max_gap_nm\":%lld",
           b.zone_count,keepouts,b.edge_count,b.outline.loop_count,b.outline.open_loops,b.outline.closed?"true":"false",b.outline.dangling_ends,(long long)b.outline.max_gap);
    if(b.outline.has_bounds) printf(",\"bounds_nm\":[%lld,%lld,%lld,%lld]",(long long)b.outline.bounds.min.x,(long long)b.outline.bounds.min.y,
                                    (long long)b.outline.bounds.max.x,(long long)b.outline.bounds.max.y);
    fputs(",\"items\":[",stdout);   /* [kind, start x, y, mid x, y, end x, y, loop, from_footprint] in nm */
    for(size_t i=0;i<b.edge_count;i++) { const BoardEdge *g=&b.edges[i];
        printf("%s[\"%s\",%lld,%lld,%lld,%lld,%lld,%lld,%lld,%s]",i?",":"",g->kind==BOARD_EDGE_ARC?"arc":g->kind==BOARD_EDGE_CIRCLE?"circle":"line",
               (long long)g->start.x,(long long)g->start.y,(long long)g->mid.x,(long long)g->mid.y,(long long)g->end.x,(long long)g->end.y,
               g->loop==SIZE_MAX?-1LL:(long long)g->loop,g->from_footprint?"true":"false"); }
    fputc(']',stdout);
    fputs("},\"issues\":[",stdout);
    for(size_t i=0;i<b.issue_count;i++) { if(i) fputc(',',stdout);
        printf("{\"code\":\"%s\",\"item\":%zu,\"context\":",board_issue_name(b.issues[i].code),b.issues[i].item);
        json_string(stdout,b.issues[i].context?b.issues[i].context:""); fputc('}',stdout); }
    puts("]}");
    arena_destroy(&arena); return 0;
}
static const char *const ITEM_KINDS[]={"none","track","via","pad","edge","board"};
/* dfm BOARD PROFILE: V2 DFM (src/v2_src/dfm.c) of a .kicad_pcb against a fab profile (fab_profiles/NAME.json), as JSON:
 * the profile and its source, a verdict per rule, and every violation (lengths in nm). Exit 0 when nothing FAILs
 * (PASS or UNVERIFIED: see "verdict"), 2 on FAIL, 1 when the board or the profile cannot be read. */
static int dfm_json(const char *board_path,const char *profile_path) {
    Arena arena; arena_init(&arena,1<<20); Board b; BoardError be={0}; DfmProfile p; DfmError de={0}; DfmReport r;
    BoardStatus bs=board_load(&arena,board_path,&b,&be);
    if(bs!=BOARD_OK) { printf("{\"status\":\"BOARD_%s\",\"line\":%zu,\"item\":%zu,\"message\":",board_status_name(bs),be.line,be.item);
        json_string(stdout,be.message?be.message:""); puts("}"); arena_destroy(&arena); return 1; }
    DfmStatus ds=dfm_profile_load(&arena,profile_path,&p,&de);
    if(ds==DFM_OK) ds=dfm_check(&arena,&b,&p,&r,&de);
    if(ds!=DFM_OK) { printf("{\"status\":\"PROFILE_%s\",\"line\":%zu,\"column\":%zu,\"key\":",dfm_status_name(ds),de.line,de.column);
        json_string(stdout,de.key?de.key:""); fputs(",\"message\":",stdout); json_string(stdout,de.message?de.message:""); puts("}");
        arena_destroy(&arena); return 1; }
    printf("{\"status\":\"OK\",\"verdict\":\"%s\",\"profile\":{\"name\":",dfm_verdict_name(r.verdict)); json_string(stdout,p.name);
    fputs(",\"fabricator\":",stdout); json_string(stdout,p.fabricator); fputs(",\"source\":",stdout); json_string(stdout,p.source_url);
    fputs(",\"retrieved\":",stdout); json_string(stdout,p.retrieved); fputs("},\"rules\":[",stdout);
    for(int k=0;k<DFM_RULE_COUNT;k++) { const DfmRuleResult *x=&r.rules[k];
        printf("%s{\"rule\":\"%s\",\"verdict\":\"%s\",\"checked\":%zu,\"violations\":%zu,\"unverified\":%zu,\"note\":",k?",":"",
               dfm_rule_name(x->rule),dfm_verdict_name(x->verdict),x->checked,x->violations,x->unverified);
        if(x->note) json_string(stdout,x->note); else fputs("null",stdout); fputc('}',stdout); }
    fputs("],\"violations\":[",stdout);
    for(size_t i=0;i<r.violation_count;i++) { const DfmViolation *v=&r.violations[i]; const DfmLimitValue *l=&p.limits[v->limit];
        printf("%s{\"rule\":\"%s\",\"limit\":\"%s\",\"limit_nm\":%lld,\"measured_nm\":%lld,\"a\":[\"%s\",%zu],\"b\":[\"%s\",%zu],\"at_nm\":[%lld,%lld],\"source\":",
               i?",":"",dfm_rule_name(v->rule),dfm_limit_key(v->limit),(long long)v->limit_nm,(long long)v->measured_nm,ITEM_KINDS[v->a.kind],v->a.index,
               ITEM_KINDS[v->b.kind],v->b.index,(long long)v->at.x,(long long)v->at.y);
        json_string(stdout,l->quote); fputc('}',stdout); }
    puts("]}");
    int rc=r.verdict==DFM_FAIL?2:0; arena_destroy(&arena); return rc;
}
/* kicad JOB BOARD OUT: run kicad-cli (src/v2_src/kicad_cli.c; found via KICAD_CLI_PATH, PATH or the KiCad 9 install
 * folders) for JOB = drc | gerbers | drill | pos, into OUT, which must not exist. JSON: status, the CLI version and
 * whether it is 9.x, argv, exit code, elapsed time, files, stderr. COPPER_KICAD_TIMEOUT_MS sets the timeout (default
 * 600000). Exit 0 on OK (DRC violations included: see drc_violations), 2 otherwise. */
static int kicad_job(const char *job,const char *board,const char *out) {
    Arena arena; arena_init(&arena,1<<16); KicadCli cli; KicadCliResult r;
    const char *t=getenv("COPPER_KICAD_TIMEOUT_MS"); unsigned long timeout=t&&*t?strtoul(t,NULL,10):600000ul;
    KicadCliStatus s=kicad_cli_open(&arena,NULL,(uint32_t)timeout,false,&cli,&r);
    if(s==KCLI_OK) {
        if(!strcmp(job,"drc")) { KicadDrcOptions o={false,true}; s=kicad_cli_drc(&arena,&cli,board,out,&o,&r); }
        else if(!strcmp(job,"gerbers")) s=kicad_cli_gerbers(&arena,&cli,board,out,&r);
        else if(!strcmp(job,"drill")) s=kicad_cli_drill(&arena,&cli,board,out,&r);
        else if(!strcmp(job,"pos")) s=kicad_cli_pos(&arena,&cli,board,out,&r);
        else { fprintf(stderr,"kicad: JOB is drc, gerbers, drill or pos\n"); arena_destroy(&arena); return 64; }
    }
    printf("{\"status\":\"%s\",\"job\":\"%s\",\"kicad_cli\":",kicad_cli_status_name(s),kicad_cli_job_name(r.job));
    if(cli.path) json_string(stdout,cli.path); else fputs("null",stdout);
    fputs(",\"version\":",stdout); if(r.cli_version) json_string(stdout,r.cli_version); else fputs("null",stdout);
    printf(",\"kicad9\":%s,\"exit_code\":%d,\"timed_out\":%s,\"elapsed_ms\":%llu,\"drc_violations\":%s,\"argv\":[",
           r.cli_is_9x?"true":"false",r.exit_code,r.timed_out?"true":"false",(unsigned long long)r.elapsed_ms,r.drc_violations?"true":"false");
    for(size_t i=0;i<r.argc;i++) { if(i) fputc(',',stdout); json_string(stdout,r.argv[i]); }
    fputs("],\"output\":",stdout); if(r.output) json_string(stdout,r.output); else fputs("null",stdout);
    fputs(",\"files\":[",stdout);
    for(size_t i=0;i<r.file_count;i++) { if(i) fputc(',',stdout); json_string(stdout,r.files[i]); }
    fputs("],\"message\":",stdout); if(r.message) json_string(stdout,r.message); else fputs("null",stdout);
    fputs(",\"stderr\":",stdout); json_string(stdout,r.stderr_text?r.stderr_text:""); puts("}");
    arena_destroy(&arena); return s==KCLI_OK?0:2;
}
static const char *const GERBER_KINDS[]={"flash","draw","region","arc"};
/* gerber FILE: a Gerber RS-274X/X2 file read by src/v2_src/gerber.c, lengths in nm (file coordinates, y up). JSON:
 * status, units, format, file attributes, apertures (D-code, type, bounds, exact), object counts, outline (draws
 * chained into loops), flashes with their attributes, and issues with line numbers. Exit 0 when OK, 2 on content
 * errors, 1 when unreadable. */
static int gerber_json(const char *path) {
    Arena arena; arena_init(&arena,1<<20); GerberFile g; GerberOutline o;
    GerberStatus st=gerber_parse_path(&arena,path,&g);
    if(st==GERBER_IO_ERROR||st==GERBER_OUT_OF_MEMORY||st==GERBER_INVALID_ARGUMENT) { printf("{\"status\":\"%s\"}\n",gerber_status_name(st)); arena_destroy(&arena); return 1; }
    printf("{\"status\":\"%s\",\"units\":\"%s\",\"format\":[%d,%d],\"attributes\":{",gerber_status_name(st),
           g.units==GERBER_UNITS_MM?"mm":g.units==GERBER_UNITS_INCH?"inch":"unknown",g.int_digits,g.dec_digits);
    for(size_t i=0;i<g.attribute_count;i++) { if(i) fputc(',',stdout); json_string(stdout,g.attributes[i].name); fputs(":[",stdout);
        for(size_t k=0;k<g.attributes[i].value_count;k++) { if(k) fputc(',',stdout); json_string(stdout,g.attributes[i].values[k]); } fputc(']',stdout); }
    fputs("},\"apertures\":[",stdout);
    static const char *const AP[]={"C","R","O","P","macro"};
    for(size_t i=0;i<g.aperture_count;i++) { const GerberAperture *a=&g.apertures[i];
        printf("%s{\"d\":%d,\"type\":\"%s\",\"shapes\":%zu,\"exact\":%s,\"bounds_nm\":[%lld,%lld,%lld,%lld],\"function\":",i?",":"",a->number,AP[a->type],a->shape_count,
               a->exact?"true":"false",(long long)a->bounds.min.x,(long long)a->bounds.min.y,(long long)a->bounds.max.x,(long long)a->bounds.max.y);
        const char *fn=NULL; for(size_t k=0;k<a->attribute_count;k++) if(!strcmp(a->attributes[k].name,".AperFunction")&&a->attributes[k].value_count) fn=a->attributes[k].values[0];
        if(fn) json_string(stdout,fn); else fputs("null",stdout); fputc('}',stdout); }
    printf("],\"counts\":{\"flashes\":%zu,\"draws\":%zu,\"regions\":%zu,\"arcs\":%zu}",g.flash_count,g.draw_count,g.region_count,g.arc_count);
    if(gerber_outline(&arena,&g,&o)==GERBER_OK)
        printf(",\"outline\":{\"segments\":%zu,\"loops\":%zu,\"open_loops\":%zu,\"closed\":%s,\"bounds_nm\":[%lld,%lld,%lld,%lld]}",o.segments,o.loops,o.open_loops,
               o.closed?"true":"false",(long long)o.bounds.min.x,(long long)o.bounds.min.y,(long long)o.bounds.max.x,(long long)o.bounds.max.y);
    fputs(",\"flashes\":[",stdout);
    for(size_t i=0,first=1;i<g.object_count;i++) { const GerberObject *x=&g.objects[i]; if(x->kind!=GERBER_FLASH) continue;
        if(!first) fputc(',',stdout); first=0;
        printf("{\"d\":%d,\"x_nm\":%lld,\"y_nm\":%lld,\"dark\":%s,\"line\":%zu",g.apertures[x->aperture].number,(long long)x->at.x,(long long)x->at.y,x->dark?"true":"false",x->line);
        if(x->net) { fputs(",\"net\":",stdout); json_string(stdout,x->net); }
        if(x->pin_ref) { fputs(",\"pin\":[",stdout); json_string(stdout,x->pin_ref); fputc(',',stdout); json_string(stdout,x->pin_number); fputc(']',stdout); }
        fputc('}',stdout); }
    fputs("],\"issues\":[",stdout);
    for(size_t i=0;i<g.issue_count;i++) { const GerberIssue *x=&g.issues[i];
        printf("%s{\"code\":\"%s\",\"severity\":\"%s\",\"line\":%zu,\"detail\":",i?",":"",gerber_issue_name(x->code),x->severity==GERBER_ERROR?"error":"warning",x->line);
        json_string(stdout,x->detail?x->detail:""); fputc('}',stdout); }
    (void)GERBER_KINDS;
    puts("]}");
    int rc=st==GERBER_OK?0:2; arena_destroy(&arena); return rc;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"models")) return cp_models_json(stdout);
    if(argc==3 && !strcmp(argv[1],"board")) return board_json(argv[2]);
    if(argc==3 && !strcmp(argv[1],"gerber")) return gerber_json(argv[2]);
    if(argc==4 && !strcmp(argv[1],"dfm")) return dfm_json(argv[2],argv[3]);
    if(argc==5 && !strcmp(argv[1],"kicad")) return kicad_job(argv[2],argv[3],argv[4]);
    if(argc<3) {fprintf(stderr,"copper-core analyze PROJECT | transient PROJECT DT STEPS [be|trap] | ac PROJECT START STOP POINTS [log|lin] | classify TEXT | models | catalogue FILE | eseries E24|E96 VALUE | drill FILE | board FILE | gerber FILE | dfm BOARD PROFILE | kicad drc|gerbers|drill|pos BOARD OUT | kicad-new PARENT NAME | kicad-open DIR\n");return 64;}
    if(argc==3 && !strcmp(argv[1],"drill")) return drill_json(argv[2]);
    if(argc==4 && !strcmp(argv[1],"kicad-new")) return kicad_project(1,argv[2],argv[3]);
    if(argc==3 && !strcmp(argv[1],"kicad-open")) return kicad_project(0,argv[2],NULL);
    if(argc==3 && !strcmp(argv[1],"catalogue")) {
        char error[512];
        if(cp_check_catalogue(argv[2],stdout,error,sizeof error)) return 0;
        fprintf(stderr,"PROJECT ERROR: %s\n",error);return 2;
    }
    if(argc==4 && !strcmp(argv[1],"eseries")) {
        /* Standard values around VALUE: the nearest, and the smallest not below it. */
        ESeries series; char *end; double v=strtod(argv[3],&end);
        if(!strcmp(argv[2],"E24")) series=E_SERIES_E24; else if(!strcmp(argv[2],"E96")) series=E_SERIES_E96;
        else {fprintf(stderr,"ESERIES invalid_argument: series must be E24 or E96\n");return 2;}
        if(*end || !(v>0) || v>1e12) {fprintf(stderr,"ESERIES invalid_argument: value must be a finite number > 0\n");return 2;}
        printf("{\"series\":\"%s\",\"value\":%.12g,\"nearest\":%.12g,\"at_least\":%.12g,\"at_most\":%.12g}\n",argv[2],v,e_series_nearest(series,v),e_series_at_least(series,v),e_series_at_most(series,v));
        return 0;
    }
    if(!strcmp(argv[1],"classify")) { CtResult r=ct_classify(argv[2]);printf("{\"category\":\"%s\",\"confidence\":%.6g,\"accepted\":%s,\"authority\":\"proposal_only\"}\n",ct_label(r.label),r.confidence,r.accepted?"true":"false"); return r.accepted?0:2; }
    CpProject *p=malloc(sizeof *p);if(!p)return 3;cp_init(p);char error[512];int rc=2;
    if(!cp_load(p,argv[2],error,sizeof error)) {fprintf(stderr,"PROJECT ERROR: %s\n",error);goto done;}
    if(!strcmp(argv[1],"analyze") && argc==3) rc=cp_analyze(p,stdout);
    else if(!strcmp(argv[1],"transient") && (argc==5 || argc==6)) {
        char *end;errno=0;double dt=strtod(argv[3],&end);if(*end||errno||end==argv[3])goto done;
        errno=0;unsigned long steps=strtoul(argv[4],&end,10);if(*end||errno||end==argv[4])goto done;
        int method=DEVICE_INTEGRATION_BACKWARD_EULER;
        if(argc==6){if(!strcmp(argv[5],"trap"))method=DEVICE_INTEGRATION_TRAPEZOIDAL;else if(strcmp(argv[5],"be"))goto done;}
        rc=cp_transient(p,dt,steps,method,stdout);
    } else if(!strcmp(argv[1],"ac") && (argc==6 || argc==7)) {
        char *end;double start,stop;unsigned long points;int log_sweep=1;
        errno=0;start=strtod(argv[3],&end);if(*end||errno||end==argv[3])goto done;
        errno=0;stop=strtod(argv[4],&end);if(*end||errno||end==argv[4])goto done;
        errno=0;points=strtoul(argv[5],&end,10);if(*end||errno||end==argv[5])goto done;
        if(argc==7){if(!strcmp(argv[6],"lin"))log_sweep=0;else if(strcmp(argv[6],"log"))goto done;}
        rc=cp_ac(p,start,stop,points,log_sweep,stdout);
    } else {fprintf(stderr,"Unknown command/argument count\n");rc=64;}
 done: cp_free(p);free(p);return rc;
}
