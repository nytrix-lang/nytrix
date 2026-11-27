/* O3 bounded polyhedral scheduling. Unsupported regions are normal no-ops. */
#include "code/backend/passes/loops/polyhedral/poly.h"
#include "base/debug.h"
#include <stdlib.h>
#include <string.h>

/* Restrict automatic fusion to streaming producer/consumer nests. General
 * explicit fusion remains available through the schedule API. A shared
 * same-point read after write is a concrete locality opportunity; mere
 * legality or two unrelated loops is not sufficient. */
static bool fusion_candidate(const nyr_poly_region_t *a,const nyr_poly_region_t *b,
                             nyr_poly_schedule_t *s) {
  if (a->depth!=2||b->depth!=2||a->triangle||b->triangle||
      a->access_count+b->access_count>6||a->body_end-a->body_begin>40||
      b->body_end-b->body_begin>40) return false;
  for (unsigned d=0;d<2;++d)
    if (a->extent[d]!=b->extent[d]||a->symbolic[d]||b->symbolic[d]||
        a->initial[d]!=b->initial[d]||a->stride[d]!=b->stride[d]) return false;
  if ((uint64_t)a->extent[0]*a->extent[1]<1024) return false;
  bool reuse=false,contiguous[2]={true,true};
  const nyr_poly_region_t *regions[]={a,b};
  for (unsigned k=0;k<2;++k)
    for (size_t j=0;j<regions[k]->access_count;++j) {
      const nyr_poly_access_t *x=&regions[k]->access[j];
      for (unsigned d=0;d<2;++d)
        if (x->offset.coeff[d]>(int64_t)x->width||x->offset.coeff[d]<-(int64_t)x->width)
          contiguous[d]=false;
    }
  for (size_t i=0;i<a->access_count;++i)
    for (size_t j=0;j<b->access_count;++j) {
      const nyr_poly_access_t *x=&a->access[i],*y=&b->access[j];
      if (x->write&&!y->write&&x->width==y->width&&x->offset.base==y->offset.base&&
          x->offset.constant==y->offset.constant&&x->offset.coeff[0]==y->offset.coeff[0]&&
          x->offset.coeff[1]==y->offset.coeff[1]) reuse=true;
    }
  if (!reuse||(!contiguous[0]&&!contiguous[1])) return false;
  *s=(nyr_poly_schedule_t){.interchange=!contiguous[1]};
  return true;
}

/* Cheap producer filter: do not look ahead for generic reductions or
 * read/modify/write stages. Default scheduling only tries private buffer
 * elimination, not arbitrary legal fusion. */
static bool possible_inline(const nyr_func_t *f,const nyr_poly_region_t *r) {
  if (!f->untagged_values||r->external||r->depth!=2||r->triangle||
      r->access_count>3||r->body_end-r->body_begin>40||
      r->symbolic[0]||r->symbolic[1]||
      (uint64_t)r->extent[0]*r->extent[1]<1024) return false;
  int root=-1;
  for (size_t k=0;k<r->access_count;++k) if (r->access[k].write) {
    if (root>=0||r->access[k].width!=8) return false;
    root=r->access[k].offset.base;
  }
  if (root<0) return false;
  for (size_t k=0;k<r->access_count;++k)
    if (!r->access[k].write&&r->access[k].offset.base==root) return false;
  return true;
}

static bool run_polyhedral(nyr_func_t *f,bool allow_simd,bool auto_horizontal) {
  if (!f || f->len<16) return true;
  const char *experimental = getenv("NYTRIX_POLY_EXPERIMENTAL_3D");
  bool allow_3d = experimental && strcmp(experimental, "1") == 0;
  const char *fusion=getenv("NYTRIX_POLY_EXPERIMENTAL_FUSION");
  bool allow_fusion=fusion&&strcmp(fusion,"1")==0;
  const char *inline_stage=getenv("NYTRIX_POLY_INLINE_STAGE");
  bool allow_inline=!inline_stage||strcmp(inline_stage,"0");
  const char *recompute=getenv("NYTRIX_POLY_EXPERIMENTAL_RECOMPUTE");
  bool allow_recompute=recompute&&!strcmp(recompute,"1");
  const char *symbolic=getenv("NYTRIX_POLY_EXPERIMENTAL_SYMBOLIC_STRIDE");
  bool allow_symbolic_stride=symbolic&&strcmp(symbolic,"1")==0;
  nyr_poly_replay_t profile;
  bool replay=nyr_poly_parse_replay(getenv("NYTRIX_POLY_REPLAY"),&profile);
  size_t start=0;
  nyr_poly_scan_t *scan=NULL;
  for (unsigned attempt=0;attempt<4;++attempt) {
    nyr_poly_region_t r,b={0};
    if (!scan) scan=nyr_poly_scan_open(f,start);
    if (!nyr_poly_scan_next(scan,&r)) break;
    nyr_poly_schedule_t s;
    size_t old_len=f->len;
    start=r.end;
    nyr_poly_dump_model(f,&r,NULL,(nyr_poly_schedule_t){0},0,false,true);
    if (replay&&nyr_poly_replay_schedule(&profile,f,&r,&s)) {
      /* Explicit original order skips this region. Every transformed replay
       * is validated, even if the profile was generated on this revision. */
      if (!s.interchange&&!s.permutation&&!s.tile&&!s.wavefront) continue;
      if (nyr_poly_emit(f,&r,s)) {
        start=f->len-(old_len-r.end);
        nyr_poly_scan_close(scan);scan=NULL;
        continue;
      }
      NY_DBG(NY_DBG_PASS,"polyhedral replay rejected; using normal selection");
    }
    bool complex_symbolic=false;
    for (unsigned d=0;d<nyr_poly_dims(&r);++d)
      complex_symbolic|=r.symbolic[d]&&(r.initial[d]||r.stride[d]!=1);
    /* These domains are legal, but normalized strides have shown mixed
     * profitability in native code. Keep the new automatic path opt-in. */
    if (complex_symbolic&&!allow_symbolic_stride) continue;
    if ((allow_fusion||((allow_inline||allow_recompute)&&possible_inline(f,&r)))&&
        nyr_poly_scan_adjacent(scan,&r,&b)) {
      nyr_poly_region_t c={0};nyr_poly_schedule_t next={0};
      bool candidate=fusion_candidate(&r,&b,&s);
      if (allow_inline&&candidate&&possible_inline(f,&b)&&
          nyr_poly_scan_adjacent(scan,&b,&c)&&fusion_candidate(&b,&c,&next)&&
          next.interchange==s.interchange&&nyr_poly_emit_chain(f,&r,&b,&c,s)) {
        start=f->len-(old_len-c.end);
        nyr_poly_scan_close(scan);scan=NULL;
        continue;
      }
      bool applied=candidate&&
        (allow_fusion?nyr_poly_emit_fused(f,&r,&b,s):
         allow_inline&&nyr_poly_emit_inlined(f,&r,&b,s));
      if (!applied&&allow_recompute&&r.access_count<=3) {
        /* The consumer's original order is always a candidate; the
         * compute-inline planner proves translated producer coverage. */
        s=(nyr_poly_schedule_t){0};
        applied=nyr_poly_emit_recomputed(f,&r,&b,s);
      }
      if (applied) {
        NY_DBG(NY_DBG_PASS,"polyhedral schedule: producer/consumer nests interchange=%d",s.interchange);
        start=f->len-(old_len-b.end);
        nyr_poly_scan_close(scan);scan=NULL;
        continue;
      }
    }
    if (nyr_poly_apply(f,&r,allow_3d,allow_simd,auto_horizontal,&s)) {
      NY_DBG(NY_DBG_PASS,"polyhedral schedule: depth=%u permutation=%u interchange=%d tile=%ux%u",nyr_poly_dims(&r),s.permutation,s.interchange,s.tile,s.inner_tile?s.inner_tile:s.tile);
      start=f->len-(old_len-r.end);
      /* The old snapshot borrows no IR payload; close it before rescanning. */
      nyr_poly_scan_close(scan);
      scan=NULL;
    }
  }
  nyr_poly_scan_close(scan);
  return true;
}

bool nyr_polyhedral_tiling(nyr_func_t *f) {return run_polyhedral(f,true,false);}
bool nyr_polyhedral_tiling_scalar(nyr_func_t *f) {return run_polyhedral(f,false,false);}
bool nyr_polyhedral_tiling_avx2(nyr_func_t *f) {return run_polyhedral(f,true,true);}
