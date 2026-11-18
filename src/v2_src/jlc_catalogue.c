#include "jlc_catalogue.h"
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const IRNode *find_node(const IRNode *n, const char *key) { return ir_list_find(n, key); }
static const IRNode *val_node(const IRNode *n, const char *key) { return ir_child_at(find_node(n, key), 1); }
static const char *val_text(const IRNode *n, const char *key) {
  const IRNode *v = val_node(n, key);
  return v && (v->kind == IR_NODE_STRING || v->kind == IR_NODE_ATOM) ? v->text : NULL;
}
static bool val_i64(const IRNode *n, const char *key, int64_t *out) {
  const IRNode *v = val_node(n, key); return v && ir_atom_to_i64(v, out) == IR_STATUS_OK;
}
static bool truth(const IRNode *n, bool *out) {
  if (!n || !out || (n->kind != IR_NODE_ATOM && n->kind != IR_NODE_STRING)) return false;
  if (!strcmp(n->text, "true") || !strcmp(n->text, "yes") || !strcmp(n->text, "1")) { *out = true; return true; }
  if (!strcmp(n->text, "false") || !strcmp(n->text, "no") || !strcmp(n->text, "0")) { *out = false; return true; }
  return false;
}
static bool date_days(const char *s, int64_t *out) {
  if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i=0; i<10; ++i) if (i!=4 && i!=7 && (s[i]<'0'||s[i]>'9')) return false;
  int y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
  unsigned m=(unsigned)((s[5]-'0')*10+s[6]-'0'), d=(unsigned)((s[8]-'0')*10+s[9]-'0');
  static const unsigned md[12]={31,28,31,30,31,30,31,31,30,31,30,31};
  if (y<1970||m<1||m>12) return false;
  unsigned lim=md[m-1]; if (m==2 && (y%4==0&&(y%100!=0||y%400==0))) lim=29;
  if (d<1||d>lim) return false;
  y-=m<=2; int era=(y>=0?y:y-399)/400; unsigned yoe=(unsigned)(y-era*400);
  unsigned mp=m>2?m-3:m+9; unsigned doy=(153u*mp+2u)/5u+d-1u;
  unsigned doe=yoe*365u+yoe/4u-yoe/100u+doy;
  *out=(int64_t)era*146097+(int64_t)doe-719468; return true;
}
static bool is_part(const IRNode *n) {
  const IRNode *h=ir_list_head(n); return n&&n->kind==IR_NODE_LIST&&h&&h->kind==IR_NODE_ATOM&&!strcmp(h->text,"part");
}
static int cmp_part_id_ptr(const void *a, const void *b) {
  const JlcPart *pa = *(JlcPart *const *)a, *pb = *(JlcPart *const *)b;
  return strcmp(pa->part_id, pb->part_id);
}
static int cmp_lcsc_id_ptr(const void *a, const void *b) {
  const JlcPart *pa = *(JlcPart *const *)a, *pb = *(JlcPart *const *)b;
  return strcmp(pa->lcsc_id, pb->lcsc_id);
}
static const JlcPart *find_index(JlcPart *const *index, size_t count,
                                 const char *key, bool part_id) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const char *candidate = part_id ? index[mid]->part_id : index[mid]->lcsc_id;
    int c = strcmp(candidate, key);
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  if (lo < count) {
    const char *candidate = part_id ? index[lo]->part_id : index[lo]->lcsc_id;
    if (strcmp(candidate, key) == 0) return index[lo];
  }
  return NULL;
}
static JlcCatalogueStatus parse_part(const IRNode *n, JlcPart *p) {
  memset(p,0,sizeof(*p));
  p->lcsc_id=val_text(n,"lcsc_id"); p->part_id=val_text(n,"part_id");
  p->mpn=val_text(n,"mpn"); p->manufacturer=val_text(n,"manufacturer");
  p->jlc_package=val_text(n,"jlc_package"); p->footprint_id=val_text(n,"footprint");
  p->source=val_text(n,"source"); p->observed_date=val_text(n,"observed_date");
  const char *cl=val_text(n,"class");
  if (cl&&!strcmp(cl,"basic")) p->part_class=JLC_CLASS_BASIC;
  else if (cl&&!strcmp(cl,"preferred")) p->part_class=JLC_CLASS_PREFERRED;
  else if (cl&&!strcmp(cl,"extended")) p->part_class=JLC_CLASS_EXTENDED;
  else if (!cl||!strcmp(cl,"unknown")) p->part_class=JLC_CLASS_UNKNOWN;
  else return JLC_CATALOGUE_ERR_RANGE;
  int64_t v;
  if (find_node(n,"cpl_rot_offset_mdeg")) { if(!val_i64(n,"cpl_rot_offset_mdeg",&v)||v<INT32_MIN||v>INT32_MAX)return JLC_CATALOGUE_ERR_RANGE; p->cpl_rot_offset_mdeg=(int32_t)v; }
  if (find_node(n,"cpl_flip_bottom") && !truth(val_node(n,"cpl_flip_bottom"),&p->cpl_flip_bottom)) return JLC_CATALOGUE_ERR_RANGE;
  if (find_node(n,"unit_price_micro_usd")) { if(!val_i64(n,"unit_price_micro_usd",&v)||v<0)return JLC_CATALOGUE_ERR_RANGE; p->unit_price_micro_usd=v; p->has_price=true; }
  if (find_node(n,"stock")) { if(!val_i64(n,"stock",&v)||v<0||v>UINT32_MAX)return JLC_CATALOGUE_ERR_RANGE; p->stock=(uint32_t)v; p->has_stock=true; }
  if (!p->lcsc_id||!p->part_id||!p->mpn||!p->manufacturer||!p->jlc_package||!p->footprint_id||!p->source)
    return JLC_CATALOGUE_ERR_SCHEMA;
  if ((p->has_price||p->has_stock) && !p->observed_date) return JLC_CATALOGUE_ERR_SCHEMA;
  if (p->observed_date) { int64_t day; if(!date_days(p->observed_date,&day))return JLC_CATALOGUE_ERR_RANGE; }
  return JLC_CATALOGUE_OK;
}
const char *jlc_catalogue_status_name(JlcCatalogueStatus s) {
  switch(s) {
    case JLC_CATALOGUE_OK:return "OK"; case JLC_CATALOGUE_ERR_ARGUMENT:return "ARGUMENT";
    case JLC_CATALOGUE_ERR_IO:return "IO"; case JLC_CATALOGUE_ERR_PARSE:return "PARSE";
    case JLC_CATALOGUE_ERR_SCHEMA:return "SCHEMA"; case JLC_CATALOGUE_ERR_DUPLICATE:return "DUPLICATE";
    case JLC_CATALOGUE_ERR_RANGE:return "RANGE"; case JLC_CATALOGUE_ERR_STALE:return "STALE";
    case JLC_CATALOGUE_ERR_MEMORY:return "MEMORY"; default:return "UNKNOWN";
  }
}
JlcCatalogueStatus jlc_catalogue_load(const char *path, Arena *arena,
                                      const char *today_iso, uint32_t max_age,
                                      JlcCatalogue *out) {
  if(!path||!arena||!today_iso||!out)return JLC_CATALOGUE_ERR_ARGUMENT;
  int64_t today; if(!date_days(today_iso,&today))return JLC_CATALOGUE_ERR_ARGUMENT;
  IRError error; ir_error_clear(&error); IRNode *root=ir_parse_file(arena,path,&error);
  if(!root)return JLC_CATALOGUE_ERR_PARSE;
  const IRNode *head=ir_list_head(root);
  if(root->kind!=IR_NODE_LIST||!head||strcmp(head->text,"copper_jlc_catalogue"))return JLC_CATALOGUE_ERR_PARSE;
  int64_t schema=0; if(!val_i64(root,"schema",&schema)||schema!=1)return JLC_CATALOGUE_ERR_SCHEMA;
  const IRNode *list=find_node(root,"parts"); if(!list)return JLC_CATALOGUE_ERR_SCHEMA;
  size_t count=0; for(const IRNode *it=list->first_child;it;it=it->next)if(is_part(it))++count;
  JlcCatalogue parsed={0}; parsed.schema_version=(uint32_t)schema; parsed.source=val_text(root,"source");
  parsed.parts=count?arena_alloc_array(arena,sizeof(*parsed.parts),count):NULL;
  parsed.by_part_id=count?arena_alloc_array(arena,sizeof(*parsed.by_part_id),count):NULL;
  parsed.by_lcsc_id=count?arena_alloc_array(arena,sizeof(*parsed.by_lcsc_id),count):NULL;
  if(count&&(!parsed.parts||!parsed.by_part_id||!parsed.by_lcsc_id))return JLC_CATALOGUE_ERR_MEMORY;
  size_t i=0;
  for(const IRNode *it=list->first_child;it;it=it->next)if(is_part(it)) {
    JlcCatalogueStatus st=parse_part(it,&parsed.parts[i]); if(st!=JLC_CATALOGUE_OK)return st;
    JlcPart *p=&parsed.parts[i];
    if(p->observed_date&&(p->has_price||p->has_stock)) { int64_t observed; if(!date_days(p->observed_date,&observed)||observed>today)return JLC_CATALOGUE_ERR_RANGE; if((uint64_t)(today-observed)>max_age)p->availability_stale=true; }
    ++i;
  }
  parsed.part_count=i;
  for(size_t k=0;k<i;++k) {
    parsed.by_part_id[k]=&parsed.parts[k];
    parsed.by_lcsc_id[k]=&parsed.parts[k];
  }
  if (i > 1) {
    qsort(parsed.by_part_id, i, sizeof(*parsed.by_part_id), cmp_part_id_ptr);
    qsort(parsed.by_lcsc_id, i, sizeof(*parsed.by_lcsc_id), cmp_lcsc_id_ptr);
  }
  for(size_t k=1;k<i;++k) {
    if(!strcmp(parsed.by_part_id[k-1]->part_id, parsed.by_part_id[k]->part_id) ||
       !strcmp(parsed.by_lcsc_id[k-1]->lcsc_id, parsed.by_lcsc_id[k]->lcsc_id))
      return JLC_CATALOGUE_ERR_DUPLICATE;
  }
  *out=parsed; return JLC_CATALOGUE_OK;
}
const JlcPart *jlc_catalogue_find_part_id(const JlcCatalogue *c, const char *id) {
  if (!c || !id) return NULL;
  if (c->by_part_id) return find_index(c->by_part_id, c->part_count, id, true);
  for (size_t i = 0; i < c->part_count; ++i)
    if (!strcmp(c->parts[i].part_id, id)) return &c->parts[i];
  return NULL;
}
const JlcPart *jlc_catalogue_find_lcsc_id(const JlcCatalogue *c, const char *id) {
  if (!c || !id) return NULL;
  if (c->by_lcsc_id) return find_index(c->by_lcsc_id, c->part_count, id, false);
  for (size_t i = 0; i < c->part_count; ++i)
    if (!strcmp(c->parts[i].lcsc_id, id)) return &c->parts[i];
  return NULL;
}
