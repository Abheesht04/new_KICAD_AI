#include "topology_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct TBuf { char *p; size_t n, cap; bool bad; } TBuf;
static bool reserve(TBuf *b, size_t x) {
    if (x > SIZE_MAX - b->n - 1u) return false;
    size_t need=b->n+x+1u; if (need<=b->cap) return true;
    size_t c=b->cap?b->cap:2048u; while(c<need) { if(c>SIZE_MAX/2u){c=need;break;} c*=2u; }
    char *p=(char*)realloc(b->p,c); if(!p)return false; b->p=p;b->cap=c;return true;
}
static void raw(TBuf*b,const char*s){if(!s)return;size_t n=strlen(s);if(!reserve(b,n)){b->bad=true;return;}memcpy(b->p+b->n,s,n);b->n+=n;b->p[b->n]=0;}
static void fmt(TBuf*b,const char*f,...){va_list a,c;va_start(a,f);va_copy(c,a);int n=vsnprintf(NULL,0,f,c);va_end(c);if(n<0||!reserve(b,(size_t)n)){b->bad=true;va_end(a);return;}vsnprintf(b->p+b->n,b->cap-b->n,f,a);va_end(a);b->n+=(size_t)n;}
static void str(TBuf*b,const char*s){if(!s){raw(b,"null");return;}raw(b,"\"");for(const unsigned char*p=(const unsigned char*)s;*p;++p){char q[7];switch(*p){case '"':raw(b,"\\\"");break;case '\\':raw(b,"\\\\");break;case '\n':raw(b,"\\n");break;case '\r':raw(b,"\\r");break;case '\t':raw(b,"\\t");break;default:if(*p<32){snprintf(q,sizeof(q),"\\u%04x",(unsigned)*p);raw(b,q);}else{if(!reserve(b,1)){b->bad=true;return;}b->p[b->n++]=(char)*p;b->p[b->n]=0;}break;}}raw(b,"\"");}
char *copper_topology_json_to_string(const CopperTopology*t,size_t*outlen){
 if (outlen) *outlen = 0;
 if (!t) return NULL;
 TBuf b = {0};
 raw(&b,"{\"schema\":\"copper.topology\",\"schema_version\":1,\"status\":");str(&b,t->complete?"complete":"incomplete");
 size_t unconnected = 0;
 for (size_t i = 0; i < t->terminal_count; ++i)
     if (t->terminals[i].net_index == COPPER_TOPOLOGY_NO_NET) ++unconnected;
 fmt(&b,",\"summary\":{\"components\":%zu,\"terminals\":%zu,\"unconnected_terminals\":%zu,\"nets\":%zu,\"connections\":%zu,\"issues\":%zu},",t->component_count,t->terminal_count,unconnected,t->net_count,t->connection_count,t->issue_count);
 raw(&b,"\"components\":[");
 for(size_t i=0;i<t->component_count;i++){const CopperTopologyComponent*c=&t->components[i];if(i)raw(&b,",");raw(&b,"{\"uuid\":");str(&b,c->uuid);raw(&b,",\"reference\":");str(&b,c->reference);raw(&b,",\"value\":");str(&b,c->value);raw(&b,",\"part_id\":");str(&b,c->part_id);raw(&b,",\"lib_id\":");str(&b,c->lib_id);raw(&b,",\"footprint\":");str(&b,c->footprint);raw(&b,",\"pins\":[");for(size_t j=0;j<c->terminal_count;j++){const CopperTopologyTerminal*p=&t->terminals[c->first_terminal+j];if(j)raw(&b,",");raw(&b,"{\"number\":");str(&b,p->number);raw(&b,",\"name\":");str(&b,p->name);raw(&b,",\"electrical_type\":");str(&b,p->electrical_type);raw(&b,",\"footprint_pad\":");str(&b,p->footprint_pad);raw(&b,",\"net\":");if(p->net_index==COPPER_TOPOLOGY_NO_NET)raw(&b,"null");else str(&b,t->nets[p->net_index].name);raw(&b,",\"state\":");str(&b,p->net_index==COPPER_TOPOLOGY_NO_NET?"unconnected_or_nc":"connected");raw(&b,"}");}raw(&b,"]}");}
 raw(&b,"],\"nets\":[");
 for(size_t i=0;i<t->net_count;i++){const CopperTopologyNet*n=&t->nets[i];if(i)raw(&b,",");raw(&b,"{\"code\":");str(&b,n->code);raw(&b,",\"name\":");str(&b,n->name);raw(&b,",\"class\":");str(&b,n->net_class);fmt(&b,",\"connection_count\":%u}",n->connection_count);}
 raw(&b,"],\"issues\":[");
 for(size_t i=0;i<t->issue_count;i++){const CopperTopologyIssue*x=&t->issues[i];if(i)raw(&b,",");raw(&b,"{\"code\":");str(&b,x->code);raw(&b,",\"reference\":");str(&b,x->reference);raw(&b,",\"pin\":");str(&b,x->pin);raw(&b,",\"net\":");str(&b,x->net);raw(&b,",\"message\":");str(&b,x->message);raw(&b,"}");}
 raw(&b,"]}");
 if(b.bad){free(b.p);return NULL;}if(outlen)*outlen=b.n;return b.p;
}
FsStatus copper_topology_json_write_file(const CopperTopology*t,const char*path){size_t n=0;char*s=copper_topology_json_to_string(t,&n);if(!s)return FS_ERR_OUT_OF_MEMORY;FsStatus st=fs_write_file_atomic_replace(path,s,n);free(s);return st;}
