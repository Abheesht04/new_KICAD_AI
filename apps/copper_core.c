#include "v2/project.h"
#include "v2/transformer.h"
#include "e_series.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"models")) return cp_models_json(stdout);
    if(argc<3) {fprintf(stderr,"copper-core analyze PROJECT | transient PROJECT DT STEPS [be|trap] | ac PROJECT START STOP POINTS [log|lin] | classify TEXT | models | catalogue FILE | eseries E24|E96 VALUE\n");return 64;}
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
