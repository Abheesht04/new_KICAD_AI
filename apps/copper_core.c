#include "v2/project.h"
#include "v2/transformer.h"
#include "e_series.h"
#include "copper_project.h"
#include "excellon.h"
#include "placement.h"
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
/* nm as decimal mm, exact (at most 6 decimals, trailing zeros dropped). */
static void mm_text(FILE *f,int64_t nm) {
    uint64_t a=(uint64_t)(nm<0?-nm:nm); unsigned long long w=(unsigned long long)(a/1000000u),r=(unsigned long long)(a%1000000u);
    if(nm<0) fputc('-',f);
    if(!r) { fprintf(f,"%llu",w); return; }
    char d[8]; snprintf(d,sizeof d,"%06llu",r); for(int i=5;i>0&&d[i]=='0';i--) d[i]=0;
    fprintf(f,"%llu.%s",w,d);
}
static void placement_json(const PlProblem *P,const PlPlacement *pl) {
    fputc('{',stdout);
    for(size_t i=0,first=1;i<P->part_count;i++) {
        if(!pl->placed[i]) continue;
        if(!first) fputc(',',stdout); first=0;
        json_string(stdout,P->parts[i].ref); fputs(":[",stdout); mm_text(stdout,pl->x[i]); fputc(',',stdout);
        mm_text(stdout,pl->y[i]); printf(",%d]",pl->rot[i]*90);
    }
    fputc('}',stdout);
}
/* place PROBLEM ITERATIONS SEEDS: the C placer (src/v2_src/placement.c) on a placement problem file. Constructive
 * start, then one annealed placement per comma-separated seed, as JSON. Exit 0; 2 when a part has no legal
 * position (listed under "failures"); 1 for an unreadable problem (status, line, item, message). */
static int place(const char *path,const char *iters,const char *seeds) {
    Arena arena; arena_init(&arena,1<<20); PlProblem P; PlError e={0}; PlPlacement start; int rc=0;
    char *end=NULL; unsigned long n=strtoul(iters,&end,10);
    if(!*iters||*end||n>10000000ul) { fprintf(stderr,"place: ITERATIONS must be an integer 0..10000000\n"); arena_destroy(&arena); return 64; }
    PlStatus st=pl_problem_load(&arena,path,&P,&e);
    if(st!=PL_OK) {
        printf("{\"status\":\"%s\",\"line\":%zu,\"item\":%zu,\"message\":",pl_status_name(st),e.line,e.item);
        json_string(stdout,e.message?e.message:""); puts("}"); arena_destroy(&arena); return 1;
    }
    if((st=pl_placement_alloc(&arena,&P,&start))==PL_OK) st=pl_constructive(&arena,&P,&start);
    printf("{\"status\":\"%s\",\"engine\":\"copper_v2 placement\",\"constructive\":{\"placement\":",pl_status_name(st));
    placement_json(&P,&start); fputs(",\"failures\":[",stdout);
    for(size_t i=0,first=1;i<P.part_count;i++) if(!start.placed[i]) { if(!first) fputc(',',stdout); first=0; json_string(stdout,P.parts[i].ref); }
    fputc(']',stdout);
    if(st==PL_OK) printf(",\"cost_nm\":%lld",(long long)pl_cost(&P,&start));
    fputs("},\"annealed\":[",stdout);
    if(st==PL_OK) {
        const char *s=seeds; int first=1;
        while(*s) {
            unsigned long long seed=strtoull(s,&end,10);
            if(end==s||(*end&&*end!=',')) { rc=64; fprintf(stderr,"place: SEEDS is a comma-separated list of integers\n"); break; }
            PlPlacement out; PlStats stats; PlStatus a=pl_placement_alloc(&arena,&P,&out);
            if(a==PL_OK) a=pl_anneal(&arena,&P,&start,seed,(uint32_t)n,&out,&stats);
            if(!first) fputc(',',stdout); first=0;
            printf("{\"seed\":%llu,\"status\":\"%s\"",seed,pl_status_name(a));
            if(a==PL_OK) {
                fputs(",\"placement\":",stdout); placement_json(&P,&out);
                printf(",\"cost_nm\":%lld,\"start_cost_nm\":%lld,\"moves\":%llu,\"legal_moves\":%llu,\"accepted\":%llu,\"improved\":%llu,\"refine_moves\":%llu",
                       (long long)stats.best_cost,(long long)stats.start_cost,(unsigned long long)stats.moves,(unsigned long long)stats.legal_moves,
                       (unsigned long long)stats.accepted,(unsigned long long)stats.improved,(unsigned long long)stats.refine_moves);
            } else rc=1;
            fputc('}',stdout);
            s=*end?end+1:end;
        }
    } else rc=st==PL_NO_LEGAL_POSITION?2:1;
    puts("]}"); arena_destroy(&arena); return rc;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"models")) return cp_models_json(stdout);
    if(argc<3) {fprintf(stderr,"copper-core analyze PROJECT | transient PROJECT DT STEPS [be|trap] | ac PROJECT START STOP POINTS [log|lin] | classify TEXT | models | catalogue FILE | eseries E24|E96 VALUE | drill FILE | kicad-new PARENT NAME | kicad-open DIR | place PROBLEM ITERATIONS SEEDS\n");return 64;}
    if(argc==5 && !strcmp(argv[1],"place")) return place(argv[2],argv[3],argv[4]);
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
