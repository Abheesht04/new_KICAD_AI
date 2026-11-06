#include "v2/project.h"
#include "v2/transformer.h"
#include "e_series.h"
#include "copper_project.h"
#include "excellon.h"
#include "board.h"
#include "dfm.h"
#include "kicad_cli.h"
#include "gerber.h"
#include "finding.h"
#include "tool_server.h"
#include "agent_loop.h"
#include "filesystem.h"
#include "json.h"
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
/* report BOARD PROFILE [FILE...]: one findings report (src/v2_src/finding.c, "copper.findings/1") for a board's DFM
 * against a fab profile, its model issues, and any drill (.drl) and Gerber files given after it. Inputs are labelled
 * by the paths as given. Exit 0 when the verdict is PASS or UNVERIFIED, 2 for FAIL or INVALID_INPUT, 1 when the board
 * or profile cannot be read. */
static int report_json(int argc,char **argv) {
    Arena arena; arena_init(&arena,1<<20); Board b; BoardError be={0}; DfmProfile p; DfmError de={0}; DfmReport r; FindingList l;
    if(board_load(&arena,argv[2],&b,&be)!=BOARD_OK) { fprintf(stderr,"report: cannot read the board %s: %s\n",argv[2],be.message?be.message:""); arena_destroy(&arena); return 1; }
    if(dfm_profile_load(&arena,argv[3],&p,&de)!=DFM_OK || dfm_check(&arena,&b,&p,&r,&de)!=DFM_OK) {
        fprintf(stderr,"report: profile %s: %s %s\n",argv[3],de.key?de.key:"",de.message?de.message:""); arena_destroy(&arena); return 1; }
    finding_list_init(&l,&arena);
    finding_from_dfm(&l,argv[2],&b,&p,&r);
    finding_from_board(&l,argv[2],&b);
    for(int i=4;i<argc;i++) {
        const char *dot=strrchr(argv[i],'.');
        if(dot && (!strcmp(dot,".drl") || !strcmp(dot,".DRL"))) {
            ExcFile *x=arena_alloc(&arena,sizeof *x);
            if(!x || exc_parse_path(&arena,argv[i],x)==EXC_ERR_IO) { fprintf(stderr,"report: cannot read %s\n",argv[i]); arena_destroy(&arena); return 1; }
            finding_from_excellon(&l,argv[i],x);
        } else {
            GerberFile g;
            GerberStatus gs=gerber_parse_path(&arena,argv[i],&g);
            if(gs==GERBER_IO_ERROR||gs==GERBER_OUT_OF_MEMORY) { fprintf(stderr,"report: cannot read %s\n",argv[i]); arena_destroy(&arena); return 1; }
            finding_from_gerber(&l,argv[i],&g);
        }
    }
    size_t n; const char *text=finding_report_json(&l,&arena,&n);
    if(!text) { fputs("report: out of memory\n",stderr); arena_destroy(&arena); return 1; }
    fwrite(text,1,n,stdout);
    FindingVerdict v=finding_verdict(&l);
    arena_destroy(&arena);
    return v==FINDING_VERDICT_FAIL||v==FINDING_VERDICT_INVALID_INPUT?2:0;
}
/* serve [--root DIR] [--kicad-cli PATH] [--log FILE]: the JSON-RPC 2.0 tool server (src/v2_src/tool_server.c) on
 * stdin/stdout, one JSON message per line. The session log (JSONL, every message in and out) is appended to FILE.
 * Ends at end of input or after "exit". */
static void serve_log(void *ctx,const char *line,size_t len) { FILE *f=ctx; fwrite(line,1,len,f); fputc('\n',f); fflush(f); }
static int serve(int argc,char **argv) {
    ToolServerConfig cfg; memset(&cfg,0,sizeof cfg); cfg.version="2.0.0-dev"; FILE *log=NULL;
    for(int i=2;i<argc;i++) {
        if(i+1<argc && !strcmp(argv[i],"--root")) cfg.root=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--kicad-cli")) cfg.kicad_cli=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--log")) { log=fopen(argv[++i],"ab"); if(!log) { fprintf(stderr,"serve: cannot open the log %s\n",argv[i]); return 64; } }
        else { fprintf(stderr,"copper-core serve [--root DIR] [--kicad-cli PATH] [--log FILE]\n"); return 64; }
    }
    if(log) { cfg.log=serve_log; cfg.log_ctx=log; }
    ToolServer s; tool_server_init(&s,&cfg);
    size_t cap=1<<16,n=0; char *buf=malloc(cap); int c;
    if(!buf) return 1;
    for(;;) {
        c=fgetc(stdin);
        if(c==EOF||c=='\n') {
            if(n||c=='\n') {
                size_t rl; const char *resp=n?tool_server_handle(&s,buf,n,&rl):NULL;
                if(resp) { fwrite(resp,1,rl,stdout); fputc('\n',stdout); fflush(stdout); }
            }
            n=0;
            if(c==EOF||s.exited) break;
            continue;
        }
        if(n+1>=cap) { char *more=realloc(buf,cap*2); if(!more) { free(buf); return 1; } buf=more; cap*=2; }
        buf[n++]=(char)c;
    }
    free(buf); tool_server_destroy(&s); if(log) fclose(log);
    return 0;
}
/* agent-eval PROMPTS OUT [--backend mock|ollama] [--model M] [--url U] [--root DIR] [--kicad-cli PATH]: run every
 * prompt of PROMPTS (eval/prompts.jsonl) through the agent loop (src/v2_src/agent_loop.c) against a fresh tool server,
 * score it against its "expect", and write OUT/results.jsonl (one line per prompt), OUT/summary.txt and, per prompt,
 * OUT/logs/<id>.agent.jsonl (turns) and OUT/logs/<id>.tools.jsonl (tool-server session). OUT must not exist.
 * The default backend is the mock: no model is needed. Exit 0 when every prompt PASSes, 2 otherwise. */
static void file_log(void *ctx,const char *line,size_t len) { FILE *f=ctx; if(f) { fwrite(line,1,len,f); fputc('\n',f); } }
static int agent_eval(int argc,char **argv) {
    const char *prompts=argv[2],*out=argv[3],*backend="mock",*model=NULL,*url=NULL,*root=NULL,*kcli=NULL;
    for(int i=4;i<argc;i++) {
        if(i+1<argc && !strcmp(argv[i],"--backend")) backend=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--model")) model=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--url")) url=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--root")) root=argv[++i];
        else if(i+1<argc && !strcmp(argv[i],"--kicad-cli")) kcli=argv[++i];
        else { fprintf(stderr,"agent-eval PROMPTS OUT [--backend mock|ollama] [--model M] [--url U] [--root DIR] [--kicad-cli PATH]\n"); return 64; }
    }
    if(strcmp(backend,"mock")&&strcmp(backend,"ollama")) { fprintf(stderr,"agent-eval: --backend is mock or ollama\n"); return 64; }
    if(!strcmp(backend,"ollama")&&!model) { fprintf(stderr,"agent-eval: --backend ollama needs --model (e.g. qwen3:8b)\n"); return 64; }
    if(fs_file_exists(out)||fs_directory_exists(out)) { fprintf(stderr,"agent-eval: %s already exists: nothing is overwritten\n",out); return 64; }
    Arena arena; arena_init(&arena,1<<20);
    JsonValue *dummy; JsonError je;
    FILE *pf=fopen(prompts,"rb"); if(!pf) { fprintf(stderr,"agent-eval: cannot read %s\n",prompts); return 1; }
    char logs[4096]; snprintf(logs,sizeof logs,"%s/logs",out);
    if(fs_create_directories(logs)!=FS_OK) { fprintf(stderr,"agent-eval: cannot create %s\n",logs); fclose(pf); return 1; }
    char path[4200]; snprintf(path,sizeof path,"%s/results.jsonl",out);
    FILE *rf=fopen(path,"wb"); if(!rf) { fclose(pf); return 1; }
    size_t total=0,pass=0,fail=0,unverified=0; char line[65536]; unsigned long long ms=0,ptok=0,ctok=0;
    char summary_rows[16384]=""; size_t sl=0;
    while(fgets(line,sizeof line,pf)) {
        size_t n=strlen(line); while(n&&(line[n-1]=='\n'||line[n-1]=='\r')) line[--n]=0;
        if(!n) continue;
        Arena run; arena_init(&run,1<<20);
        JsonValue *v;
        if(json_parse(&run,line,n,&v,&je)!=JSON_OK||!json_get(v,"id")||!json_get(v,"prompt")) { fprintf(stderr,"agent-eval: line %zu of %s is not a prompt object\n",total+1,prompts); arena_destroy(&run); fclose(pf); fclose(rf); return 1; }
        const char *id=json_get(v,"id")->text,*prompt=json_get(v,"prompt")->text;
        char ap[4400],tp[4400]; snprintf(ap,sizeof ap,"%s/%s.agent.jsonl",logs,id); snprintf(tp,sizeof tp,"%s/%s.tools.jsonl",logs,id);
        FILE *af=fopen(ap,"wb"),*tf=fopen(tp,"wb");
        ToolServerConfig tc; memset(&tc,0,sizeof tc); tc.root=root; tc.kicad_cli=kcli; tc.version="eval"; tc.log=file_log; tc.log_ctx=tf;
        ToolServer ts; tool_server_init(&ts,&tc);
        LlmBackend be; LlmMockState ms_; LlmOllamaState os; LlmOllamaConfig oc={url,model,0};
        if(!strcmp(backend,"mock")) llm_mock_init_rules(&be,&ms_); else llm_ollama_init(&be,&os,&oc);
        AgentConfig ac; memset(&ac,0,sizeof ac); ac.log=file_log; ac.log_ctx=af;
        AgentResult r; agent_run(&run,&be,&ts,&ac,prompt,&r);
        char reasons[2048]; AgentVerdict verdict=agent_score(&r,json_get(v,"expect"),reasons,sizeof reasons);
        total++; pass+=verdict==AGENT_PASS; fail+=verdict==AGENT_FAIL; unverified+=verdict==AGENT_UNVERIFIED;
        ms+=r.model_ms; ptok+=r.prompt_tokens; ctok+=r.completion_tokens;
        JsonWriter w; json_writer_init_compact(&w,&run);
        json_write_begin_object(&w);
        json_write_key(&w,"id"); json_write_string(&w,id);
        json_write_key(&w,"backend"); json_write_string(&w,backend);
        json_write_key(&w,"model"); json_write_string(&w,model?model:"mock-rules");
        json_write_key(&w,"verdict"); json_write_string(&w,agent_verdict_name(verdict));
        json_write_key(&w,"status"); json_write_string(&w,agent_status_name(r.status));
        json_write_key(&w,"turns"); json_write_uint(&w,r.turns);
        json_write_key(&w,"tools"); json_write_begin_array(&w); for(size_t i=0;i<r.tool_count;i++) json_write_string(&w,r.tools[i]); json_write_end_array(&w);
        json_write_key(&w,"tool_errors"); json_write_uint(&w,r.tool_errors);
        json_write_key(&w,"invalid_replies"); json_write_uint(&w,r.invalid_replies);
        json_write_key(&w,"answer"); json_write_string(&w,r.answer);
        json_write_key(&w,"reasons"); json_write_string(&w,reasons);
        if(r.backend_error) { json_write_key(&w,"backend_error"); json_write_string(&w,r.backend_error); }
        json_write_key(&w,"model_ms"); json_write_uint(&w,r.model_ms);
        json_write_key(&w,"prompt_tokens"); json_write_uint(&w,r.prompt_tokens);
        json_write_key(&w,"completion_tokens"); json_write_uint(&w,r.completion_tokens);
        json_write_end_object(&w);
        size_t rl; const char *rec=json_writer_finish(&w,&rl);
        if(rec) { fwrite(rec,1,rl,rf); fputc('\n',rf); }
        if(sl<sizeof summary_rows-256) sl+=(size_t)snprintf(summary_rows+sl,sizeof summary_rows-sl,"  %-10s %-18s %-16s turns %zu, tools %zu%s%s\n",agent_verdict_name(verdict),id,
                                                              agent_status_name(r.status),r.turns,r.tool_count,reasons[0]?": ":"",reasons);
        tool_server_destroy(&ts); if(af) fclose(af); if(tf) fclose(tf); arena_destroy(&run);
    }
    fclose(pf); fclose(rf);
    snprintf(path,sizeof path,"%s/summary.txt",out);
    FILE *sf=fopen(path,"wb");
    if(sf) {
        fprintf(sf,"Copper LLM eval: %s\nbackend %s, model %s\n%zu prompts: %zu PASS, %zu FAIL, %zu UNVERIFIED\nmodel time %llu ms, %llu prompt tokens, %llu completion tokens\n\n%s",
                prompts,backend,model?model:"mock-rules (deterministic, no model)",total,pass,fail,unverified,ms,ptok,ctok,summary_rows);
        if(!strcmp(backend,"mock")) fputs("\nThe mock backend measures the pipeline (prompts, tools, scoring), not a model.\n",sf);
        fclose(sf);
    }
    printf("%zu prompts: %zu PASS, %zu FAIL, %zu UNVERIFIED (results in %s)\n",total,pass,fail,unverified,out);
    (void)dummy; arena_destroy(&arena);
    return pass==total?0:2;
}
int main(int argc,char **argv) {
    if(argc>=4 && !strcmp(argv[1],"agent-eval")) return agent_eval(argc,argv);
    if(argc>=2 && !strcmp(argv[1],"serve")) return serve(argc,argv);
    if(argc>=4 && !strcmp(argv[1],"report")) return report_json(argc,argv);
    if(argc==2 && !strcmp(argv[1],"models")) return cp_models_json(stdout);
    if(argc==3 && !strcmp(argv[1],"board")) return board_json(argv[2]);
    if(argc==3 && !strcmp(argv[1],"gerber")) return gerber_json(argv[2]);
    if(argc==4 && !strcmp(argv[1],"dfm")) return dfm_json(argv[2],argv[3]);
    if(argc==5 && !strcmp(argv[1],"kicad")) return kicad_job(argv[2],argv[3],argv[4]);
    if(argc<3) {fprintf(stderr,"copper-core analyze PROJECT | transient PROJECT DT STEPS [be|trap] | ac PROJECT START STOP POINTS [log|lin] | classify TEXT | models | catalogue FILE | eseries E24|E96 VALUE | drill FILE | board FILE | gerber FILE | report BOARD PROFILE [DRILL|GERBER...] | serve [--root DIR] [--kicad-cli PATH] [--log FILE] | agent-eval PROMPTS OUT [--backend mock|ollama] [--model M] | dfm BOARD PROFILE | kicad drc|gerbers|drill|pos BOARD OUT | kicad-new PARENT NAME | kicad-open DIR\n");return 64;}
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
