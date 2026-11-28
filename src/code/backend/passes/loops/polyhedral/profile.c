/* Optional region inspection and empirical schedule replay. The key is
 * a deterministic fingerprint of the current extracted NYIR region, not
 * a semantic identity across compiler revisions. Every replay is checked
 * by the ordinary legality engine and transactional regenerator. */
#include "code/backend/passes/loops/polyhedral/poly.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static uint64_t mix(uint64_t h,uint64_t v) {
  for (unsigned k=0;k<8;++k) {h^=v&255u;h*=UINT64_C(1099511628211);v>>=8;}
  return h;
}
uint64_t nyr_poly_model_key(const nyr_func_t *f,const nyr_poly_region_t *r) {
  unsigned dims=nyr_poly_dims(r);
  uint64_t h=UINT64_C(14695981039346656037);
  h=mix(h,dims);h=mix(h,r->triangle);h=mix(h,r->external);
  h=mix(h,r->entry);h=mix(h,r->begin);h=mix(h,r->access_count);
  for (unsigned d=0;d<dims;++d) {
    h=mix(h,r->extent[d]);h=mix(h,r->initial[d]);h=mix(h,r->stride[d]);
    h=mix(h,r->symbolic[d]);h=mix(h,r->bound_value[d]);h=mix(h,r->inclusive[d]);
    h=mix(h,r->minimum_extent[d]);h=mix(h,r->iv[d]);
  }
  for (size_t a=0;a<r->access_count;++a) {
    const nyr_poly_access_t *x=&r->access[a];
    h=mix(h,x->offset.base);h=mix(h,x->offset.constant);h=mix(h,x->width);h=mix(h,x->is_write);
    for (unsigned d=0;d<dims;++d) h=mix(h,x->offset.coeff[d]);
  }
  for (size_t p=r->body_begin;p<r->body_end&&p<f->len;++p) {
    const nyr_inst_t *in=&f->data[p];
    h=mix(h,in->op);h=mix(h,in->dst);h=mix(h,in->imm);h=mix(h,in->cmp);
    h=mix(h,in->flags);h=mix(h,in->scalar_class);
    h=mix(h,in->a);h=mix(h,in->b);h=mix(h,in->c);
    h=mix(h,in->d);h=mix(h,in->e);h=mix(h,in->f);
  }
  return h;
}

/* Parse exactly key:order:tile:inner_tile:wavefront:skew. No implicit
 * truncation, signs, whitespace or trailing junk. Malformed input is a
 * no-op rather than a compilation failure. */
bool nyr_poly_parse_replay(const char *text,nyr_poly_replay_t *out) {
  if (!text||!out||strlen(text)<16||strlen(text)>48) return false;
  for (unsigned k=0;k<16;++k)
    if (!((text[k]>='0'&&text[k]<='9')||(text[k]>='a'&&text[k]<='f')||
          (text[k]>='A'&&text[k]<='F'))) return false;
  if (text[16]!=':') return false;
  char *end=NULL;errno=0;
  uintmax_t key=strtoumax(text,&end,16);
  if (errno||end!=text+16||key>UINT64_MAX) return false;
  unsigned v[5],max[]={5,256,256,1,8};
  const char *p=end+1;
  for (unsigned k=0;k<5;++k) {
    if (*p<'0'||*p>'9') return false;
    errno=0;unsigned long n=strtoul(p,&end,10);
    if (errno||n>max[k]||(k<4?*end!=':':*end!=0)) return false;
    v[k]=(unsigned)n;p=end+1;
  }
  if ((!v[1]&&v[2])||(!v[3]&&v[4])||(v[3]&&(v[1]||v[2]||v[0]>1))) return false;
  *out=(nyr_poly_replay_t){.key=(uint64_t)key,.order=v[0],
    .schedule={.tile=v[1],.inner_tile=v[2],.wavefront=v[3]!=0,.skew=v[4]}};
  return true;
}

bool nyr_poly_replay_schedule(const nyr_poly_replay_t *profile,
                               const nyr_func_t *f,const nyr_poly_region_t *r,
                               nyr_poly_schedule_t *out) {
  if (!profile||!out||profile->key!=nyr_poly_model_key(f,r)) return false;
  unsigned dims=nyr_poly_dims(r);
  if ((dims==2&&profile->order>1)||(dims==3&&(profile->schedule.wavefront||
      profile->schedule.inner_tile))) return false;
  *out=profile->schedule;
  if (dims==2) out->interchange=profile->order!=0;
  else out->permutation=profile->order;
  return true;
}

/* OpenScop-inspired separation of iteration domains, ordered accesses and
 * scheduling. This is a versioned NYR diagnostic, not the OSL file format.
 * Symbolic dimensions record a conservative bounding domain; the actual
 * runtime comparison and original SSA bound are retained separately. */
static void dump_statement(FILE *out,const nyr_poly_region_t *r) {
  unsigned dims=nyr_poly_dims(r);
  fprintf(out,"{\"triangle\":%u,\"external\":%s,\"domain\":[",r->triangle,
          r->external?"true":"false");
  for (unsigned d=0;d<dims;++d) {
    fprintf(out,"%s{\"extent\":%u,\"minimum_extent\":%u,\"initial\":%" PRId64
            ",\"stride\":%" PRId64 ",\"symbolic\":%s,\"bound_ssa\":%d,\"inclusive\":%s}",
            d?",":"",r->extent[d],r->minimum_extent[d],r->initial[d],r->stride[d],
            r->symbolic[d]?"true":"false",r->bound_value[d],r->inclusive[d]?"true":"false");
  }
  fputs("],\"accesses\":[",out);
  for (size_t a=0;a<r->access_count;++a) {
    const nyr_poly_access_t *x=&r->access[a];
    fprintf(out,"%s{\"base\":%d,\"write\":%s,\"width\":%u,\"constant\":%" PRId64 ",\"coefficients\":[",
            a?",":"",x->offset.base,x->is_write?"true":"false",x->width,x->offset.constant);
    for (unsigned d=0;d<dims;++d) fprintf(out,"%s%" PRId64,d?",":"",x->offset.coeff[d]);
    fputs("]}",out);
  }
  fputs("]}",out);
}
void nyr_poly_dump_model_chain(const nyr_func_t *f,const nyr_poly_region_t *r,
                         const nyr_poly_region_t *second,const nyr_poly_region_t *third,nyr_poly_schedule_t s,
                         unsigned lanes,nyr_poly_vector_mode_t vector_mode,bool candidate) {
  const char *enabled=getenv("NYTRIX_POLY_DUMP");
  if (!enabled||strcmp(enabled,"1")) return;
  unsigned dims=nyr_poly_dims(r);
  fprintf(stderr,"NYR_POLY_%s {\"version\":1,\"key\":\"%016" PRIx64
          "\",\"dimensions\":%u,\"axes\":[",candidate?"CANDIDATE":"MODEL",
          nyr_poly_model_key(f,r),dims);
  for (unsigned d=0;d<dims;++d) fprintf(stderr,"%s%u",d?",":"",nyr_poly_axis(s,dims,d));
  fprintf(stderr,"],\"tile\":%u,\"inner_tile\":%u,\"wavefront\":%s,\"skew\":%u,\"register_block\":%u,\"simd\":%s,\"simd_axis\":\"%s\",\"reduction_vectors\":%u,\"statements\":[",
          s.tile,s.inner_tile,s.wavefront?"true":"false",nyr_poly_skew(s),
          vector_mode==NYR_POLY_VECTOR_REDUCTION?1:lanes,
          vector_mode!=NYR_POLY_SCALAR?"true":"false",
          vector_mode==NYR_POLY_VECTOR_REDUCTION?"reduction":vector_mode==NYR_POLY_VECTOR_BLOCK?"block":"none",
          vector_mode==NYR_POLY_VECTOR_REDUCTION?lanes:0);
  dump_statement(stderr,r);
  if (second) {fputc(',',stderr);dump_statement(stderr,second);}
  if (third) {fputc(',',stderr);dump_statement(stderr,third);}
  fputs("]}\n",stderr);
}

void nyr_poly_dump_model(const nyr_func_t *f,const nyr_poly_region_t *r,
                         const nyr_poly_region_t *second,nyr_poly_schedule_t s,
                         unsigned lanes,bool vector,bool candidate) {
  nyr_poly_dump_model_chain(f,r,second,NULL,s,lanes,vector?NYR_POLY_VECTOR_BLOCK:NYR_POLY_SCALAR,candidate);
}

/* Placement is separate from traversal. Record the original regions and
 * removed roots without claiming that recomputation is a permutation. */
void nyr_poly_dump_placement(const nyr_func_t *f,const nyr_poly_region_t *a,
                            const nyr_poly_region_t *b,const nyr_poly_region_t *c,
                            int root,int root2,const char *mode) {
  const char *enabled=getenv("NYTRIX_POLY_DUMP");
  if (root<0||!b||!enabled||strcmp(enabled,"1")) return;
  fprintf(stderr,"NYR_POLY_PLACEMENT {\"version\":1,\"mode\":\"%s\",\"keys\":[\"%016" PRIx64
          "\",\"%016" PRIx64 "\"",mode,nyr_poly_model_key(f,a),nyr_poly_model_key(f,b));
  if (c) fprintf(stderr,",\"%016" PRIx64 "\"",nyr_poly_model_key(f,c));
  fprintf(stderr,"],\"elided_roots\":[%d",root);
  if (root2>=0) fprintf(stderr,",%d",root2);
  fputs("]}\n",stderr);
}
