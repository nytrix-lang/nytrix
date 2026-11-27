/* Rebuild scheduled loops in scratch IR. The original statement is emitted
 * once syntactically and its SSA IDs are retained. Original IV IDs become
 * point-loop PHIs; all new tile/control values use fresh IDs. */
#include "code/backend/passes/loops/polyhedral/domain.h"
#include "code/backend/passes/util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define POLY_MAX_BLOCK 8u

typedef struct {
  nyr_func_t tmp;
  const nyr_func_t *src;
  const nyr_poly_region_t *r, *second, *third;
  nyr_poly_schedule_t schedule;
  int64_t label;
  int zero, one, wave, skew, skew_plus_one, tile[3], bound[3], origin[3], initial[3], stride[3];
  bool ok;
  /* Optional fixed-trip, single-object recurrence carried in an SSA PHI. */
  size_t promote_load, promote_store;
  int promote_phi[POLY_MAX_BLOCK], promote_value[POLY_MAX_BLOCK], block_iv[POLY_MAX_BLOCK];
  int vector_shared[NYR_POLY_MAX_ACCESS];
  unsigned block_count;
  bool promote, vector, horizontal, horizontal_epilogue;
  int inline_root, inline_root2; /* private storage removed by placement */
  int inline_values[512];size_t inline_count;
  /* Separate pure producer algorithm from the consumer traversal. Each
   * read has a proven translation from consumer to producer coordinates. */
  const nyr_poly_region_t *producer;
  int64_t shift[NYR_POLY_MAX_ACCESS][2];
  bool sliding;
  size_t slide_low,slide_high;
  int slide_phi,slide_next;
  bool tail;
  int tail_begin, tail_iv[3];
  /* Optional full/partial strip separation, distinct from register tails. */
  bool isolate, isolate_tail;
  int full_end;
} emitter_t;

static nyr_inst_t inst(nyr_op_t op) {
  return (nyr_inst_t){.op=op,.dst=-1,.a=-1,.b=-1,.c=-1,.d=-1,.e=-1,.f=-1};
}
static int emit(emitter_t *e, nyr_inst_t in) {
  if (!e->ok) return -1;
  size_t len=e->tmp.len;
  int id=nyr_ir_emit(&e->tmp,in);
  if (e->tmp.len!=len+1) e->ok=false;
  return id;
}
static int constant(emitter_t *e, int64_t n) {
  nyr_inst_t in=inst(NYIR_CONST_I64);in.imm=n;return emit(e,in);
}
static void mark(emitter_t *e, int64_t l) {
  nyr_inst_t in=inst(NYIR_LABEL);in.imm=l;emit(e,in);
}
static void branch(emitter_t *e, int64_t l) {
  nyr_inst_t in=inst(NYIR_BR);in.imm=l;emit(e,in);
}
static int bin(emitter_t *e, nyr_op_t op, int a, int b) {
  nyr_inst_t in=inst(op);in.a=a;in.b=b;in.cmp=NYIR_CMP_LT;return emit(e,in);
}
static int select_bound(emitter_t *e, int a, int b, bool min) {
  int cond=bin(e,NYIR_CMP_I64,a,b);
  nyr_inst_t in=inst(NYIR_SELECT_I64);in.a=cond;in.b=min?a:b;in.c=min?b:a;return emit(e,in);
}
/* Same-point reference groups, inspired by PPCG gpu_group.c's private
 * storage selection. Keep the latest full-width SSA value for an exact
 * affine reference. Any possibly overlapping write kills the entry.
 * This is local promotion only: stores stay observable, no memory is
 * allocated or moved, and no value crosses an iteration boundary. */
typedef struct {
  nyr_poly_access_t access;
  int value;
  nyr_scalar_class_t type;
} point_value_t;

static bool same_ref(const nyr_poly_access_t *a,const nyr_poly_access_t *b) {
  return a->offset.base==b->offset.base&&a->width==b->width&&
         a->offset.constant==b->offset.constant&&
         !memcmp(a->offset.coeff,b->offset.coeff,sizeof(a->offset.coeff));
}

static bool point_overlap(const nyr_poly_region_t *r,
                          const nyr_poly_access_t *a,const nyr_poly_access_t *b) {
  if (a->offset.base!=b->offset.base) return false;
  ny_i128 low,high,coeff[NYR_POLY_MAX_DIMS];
  for (unsigned d=0;d<NYR_POLY_MAX_DIMS;++d)
    coeff[d]=(ny_i128)a->offset.coeff[d]-b->offset.coeff[d];
  if (!nyr_poly_extrema(r,coeff,(ny_i128)a->offset.constant-b->offset.constant,&low,&high))
    return true;
  return low<(ny_i128)b->width&&high>-(ny_i128)a->width;
}

static bool plain_word(const nyr_inst_t *in,const nyr_poly_access_t *a) {
  return a->width==8&&!in->flags&&!in->semantic_ownership&&
         (in->scalar_class==NYIR_SCALAR_UNKNOWN||in->scalar_class==NYIR_SCALAR_I64_RAW);
}

/* Same-width FP load/store forwarding is valid under the existing
 * fast-math region policy; no arithmetic is reassociated here. */
static bool point_word(const nyr_inst_t *in,const nyr_poly_access_t *a) {
  return plain_word(in,a)||(a->width==8&&!in->flags&&!in->semantic_ownership&&
      in->scalar_class==NYIR_SCALAR_F64&&nyr_fast_math());
}

/* Halide-style compute-at for a restricted two-stage pipeline. The producer
 * writes exactly one plain word per point; every consumer reference to this
 * object is an exact same-point read of that word. The existing fusion
 * legality proof is still mandatory. The entire SSA use graph of the
 * allocation must end exclusively in these memory addresses. In particular,
 * a pointer stored as data, passed to a call, used outside either body, or
 * converted to an integer result prevents storage elimination. */
static bool body_has(const nyr_poly_region_t *r,size_t p) {
  return p>=r->body_begin&&p<r->body_end;
}

static bool private_address_uses(const nyr_func_t *f,const nyr_poly_region_t *a,
                                 const nyr_poly_region_t *b,int root,
                                 int *values,size_t *value_count) {
  nyr_use_def_t uses={0};
  if (!nyr_build_use_def(f,&uses)) return false;
  /* A bounded graph walk; excessive address complexity conservatively fails. */
  int pending[256]={root};size_t count=1;
  bool ok=root>=0&&(size_t)root<uses.value_count;
  for (size_t n=0;ok&&n<count;++n) {
    int v=pending[n];
    for (size_t k=uses.offsets[v];k<uses.offsets[v+1];++k) {
      size_t p=uses.users[k];
      const nyr_inst_t *in=&f->data[p];
      if ((!body_has(a,p)&&!body_has(b,p))||in->symbol||in->extra_args_len||
          in->phi_incoming_len||in->c==v||in->d==v||in->e==v||in->f==v) {
        ok=false;break;
      }
      if ((in->op==NYIR_LOAD_I64||in->op==NYIR_STORE_I64)&&in->a==v&&in->b!=v)
        continue;
      if (in->flags||in->semantic_ownership||nyr_effective_effects(in)||
          !((in->op==NYIR_COPY&&in->a==v&&in->b!=v)||
            (in->op==NYIR_ADD_I64&&((in->a==v)!=(in->b==v)))||
            (in->op==NYIR_SUB_I64&&in->a==v&&in->b!=v))||
          in->dst<0||(size_t)in->dst>=uses.value_count) {ok=false;break;}
      bool seen=false;
      for (size_t j=0;j<count;++j) seen|=pending[j]==in->dst;
      if (!seen) {
        if (count==sizeof(pending)/sizeof(*pending)) {ok=false;break;}
        pending[count++]=in->dst;
      }
    }
  }
  nyr_use_def_free(&uses);
  if (ok&&values&&value_count) {memcpy(values,pending,count*sizeof(*values));*value_count=count;}
  return ok;
}

static const nyr_inst_t *access_inst(const nyr_func_t *f,
                                    const nyr_poly_region_t *r,size_t index) {
  for (size_t p=r->body_begin;p<r->body_end;++p) {
    const nyr_inst_t *in=&f->data[p];
    if (in->op!=NYIR_LOAD_I64&&in->op!=NYIR_STORE_I64) continue;
    if (!index--) return in;
  }
  return NULL;
}

static int inline_plan(const nyr_func_t *f,const nyr_poly_region_t *a,
                        const nyr_poly_region_t *b,int *values,size_t *value_count) {
  if (!f||!a||!b||a->external||b->external||!f->untagged_values||
      a->body_end>f->len||b->body_end>f->len||a->access_count>NYR_POLY_MAX_ACCESS||
      b->access_count>NYR_POLY_MAX_ACCESS) return -1;
  for (size_t k=0;k<a->access_count;++k) {
    const nyr_poly_access_t *w=&a->access[k];
    if (!w->write||w->offset.base<0) continue;
    const nyr_inst_t *wi=access_inst(f,a,k);
    if (!wi||!point_word(wi,w)) continue;
    bool ok=true,read=false;
    for (size_t j=0;j<a->access_count;++j)
      if (j!=k&&a->access[j].offset.base==w->offset.base) ok=false;
    for (size_t j=0;ok&&j<b->access_count;++j) {
      const nyr_poly_access_t *x=&b->access[j];
      if (x->offset.base!=w->offset.base) continue;
      const nyr_inst_t *ri=access_inst(f,b,j);
      if (x->write||!same_ref(w,x)||!ri||!point_word(ri,x)||
          ri->scalar_class!=wi->scalar_class) ok=false;
      else read=true;
    }
    if (!ok||!read) continue;
    bool alloca=false;
    for (size_t p=0;p<a->begin;++p)
      if (f->data[p].dst==w->offset.base&&f->data[p].op==NYIR_ALLOCA&&
          !(f->data[p].flags&NYIR_INST_F_SCOPE_LOCAL)) {alloca=true;break;}
    if (alloca&&private_address_uses(f,a,b,w->offset.base,values,value_count)) return w->offset.base;
  }
  return -1;
}

int nyr_poly_inline_root(const nyr_func_t *f,const nyr_poly_region_t *a,
                         const nyr_poly_region_t *b) {
  return inline_plan(f,a,b,NULL,NULL);
}

static bool inline_storage(const emitter_t *e,int v) {
  return v>=0&&(v==e->inline_root||v==e->inline_root2);
}

static bool inline_address_value(const emitter_t *e,int v) {
  if (v<0) return false;
  for (size_t k=0;k<e->inline_count;++k)
    if (v==e->inline_values[k]) return true;
  return false;
}

/* Compute-inline instead of compute-root: a cheap, effect-free producer
 * writing an injective dense 2D array may be evaluated at the translated
 * coordinates of each consumer read. Every read must have a producer point
 * in the actual domain, not just an in-bounds byte address. The consumer
 * schedule is checked independently; the eliminated object has no other
 * observers. Read-only producer inputs may be replayed only if every
 * access is proven in bounds and the consumer cannot modify their roots. */
static bool plan_recompute(emitter_t *e,const nyr_poly_region_t *a,
                           const nyr_poly_region_t *b) {
  const nyr_func_t *f=e->src;
  if (!f->untagged_values||a->external||b->external||a->depth!=2||b->depth!=2||
      a->triangle||b->triangle||!a->access_count||a->access_count>3||
      a->body_end-a->body_begin>40||b->body_end-b->body_begin>80) return false;
  for (unsigned d=0;d<2;++d)
    if (a->symbolic[d]||b->symbolic[d]||a->initial[d]||a->stride[d]!=1||
        b->stride[d]!=1||!a->extent[d]||!b->extent[d]) return false;
  size_t writer=SIZE_MAX;
  for (size_t k=0;k<a->access_count;++k) if (a->access[k].write) {
    if (writer!=SIZE_MAX) return false;
    writer=k;
  }
  if (writer==SIZE_MAX) return false;
  const nyr_poly_access_t *w=&a->access[writer];
  const nyr_inst_t *wi=access_inst(f,a,writer);
  if (!wi||!point_word(wi,w)||w->offset.base<0) return false;
  /* Recomputing a producer read after an intervening consumer write can
   * change the observed value. Separate stack allocations are disjoint;
   * same-root writes are conservatively rejected even if offsets differ. */
  for (size_t k=0;k<a->access_count;++k) if (k!=writer) {
    const nyr_poly_access_t *x=&a->access[k];
    const nyr_inst_t *ri=access_inst(f,a,k);
    if (x->offset.base==w->offset.base||!ri||!plain_word(ri,x)) return false;
    for (size_t j=0;j<b->access_count;++j)
      if (b->access[j].write&&b->access[j].offset.base==x->offset.base) return false;
  }
  unsigned fast=w->offset.coeff[1]==8?1:0,slow=fast^1u;
  int64_t pitch=(int64_t)a->extent[fast]*8;
  if (w->offset.coeff[fast]!=8||w->offset.coeff[slow]!=pitch) return false;
  bool alloca=false;
  for (size_t p=0;p<a->begin;++p)
    if (f->data[p].dst==w->offset.base&&f->data[p].op==NYIR_ALLOCA&&
        !(f->data[p].flags&NYIR_INST_F_SCOPE_LOCAL)) {alloca=true;break;}
  if (!alloca) return false;
  unsigned reads=0;bool shifted=false;
  for (size_t k=0;k<b->access_count;++k) {
    const nyr_poly_access_t *x=&b->access[k];
    if (x->offset.base!=w->offset.base) continue;
    const nyr_inst_t *ri=access_inst(f,b,k);
    if (x->write||!ri||!point_word(ri,x)||ri->scalar_class!=wi->scalar_class||
        x->offset.coeff[0]!=w->offset.coeff[0]||x->offset.coeff[1]!=w->offset.coeff[1])
      return false;
    ny_i128 delta=(ny_i128)x->offset.constant-w->offset.constant;
    if (delta<0||delta>INT64_MAX||delta%8) return false;
    int64_t d[2];d[slow]=(int64_t)delta/pitch;d[fast]=((int64_t)delta%pitch)/8;
    for (unsigned k2=0;k2<2;++k2) {
      if ((ny_i128)d[k2]+b->extent[k2]>a->extent[k2]) return false;
      ny_i128 offset=(ny_i128)d[k2]-b->initial[k2];
      if (offset<INT64_MIN||offset>INT64_MAX) return false;
      e->shift[k][k2]=(int64_t)offset;
      shifted|=d[k2]!=0||a->extent[k2]!=b->extent[k2];
    }
    if (++reads>3) return false;
  }
  if (!reads||!shifted||!private_address_uses(f,a,b,w->offset.base,e->inline_values,&e->inline_count)) return false;
  /* Limit recomputation to cheap integer arithmetic and constants. The
   * extractor already excludes effects, but keep an explicit whitelist
   * because this transformation may duplicate the computation. */
  unsigned work=0;
  for (size_t p=a->body_begin;p<a->body_end;++p) {
    const nyr_inst_t *in=&f->data[p];
    if (in==wi||in->op==NYIR_NOP||inline_address_value(e,in->dst)) continue;
    if (in->op==NYIR_LOAD_I64) {
      /* Every such input has a read-only, in-bounds affine model above.
       * Charge more than an ALU operation for duplicated input loads. */
      work+=3;continue;
    }
    if (in->flags||in->semantic_ownership||nyr_effective_effects(in)||in->symbol||
        in->extra_args_len||in->phi_incoming_len||in->arg_sizes||in->dst<0) return false;
    switch (in->op) {
    case NYIR_CONST_I64:case NYIR_COPY:break;
    case NYIR_ADD_I64:case NYIR_SUB_I64:case NYIR_MUL_I64:case NYIR_SHL_I64:
    case NYIR_SAR_I64:case NYIR_AND_I64:case NYIR_OR_I64:case NYIR_XOR_I64:
      ++work;break;
    default:return false;
    }
  }
  if (work*reads>(a->access_count>1?32u:24u)) return false;
  e->inline_root=w->offset.base;e->producer=a;
  /* Halide-style sliding window: two adjacent producer coordinates in
   * the innermost unit-step consumer loop. Warm up the first coordinate
   * outside that loop, then carry the next value in one SSA PHI. A tile,
   * skew, or unrelated third reference falls back to recomputation. */
  const char *sliding=getenv("NYTRIX_POLY_SLIDING");
  if ((!sliding||strcmp(sliding,"0"))&&!e->schedule.tile&&!e->schedule.wavefront) {
    unsigned inner=nyr_poly_axis(e->schedule,2,1),outer=inner^1u;
    size_t lo=SIZE_MAX,hi=SIZE_MAX;
    for (size_t k=0;k<b->access_count;++k) if (b->access[k].offset.base==e->inline_root) {
      if (lo==SIZE_MAX||e->shift[k][inner]<e->shift[lo][inner]) lo=k;
      if (hi==SIZE_MAX||e->shift[k][inner]>e->shift[hi][inner]) hi=k;
    }
    if (lo!=SIZE_MAX&&hi!=SIZE_MAX&&lo!=hi&&
        (ny_i128)e->shift[hi][inner]-e->shift[lo][inner]==1&&
        e->shift[lo][outer]==e->shift[hi][outer]) {
      e->sliding=true;
      for (size_t k=0;k<b->access_count;++k) if (b->access[k].offset.base==e->inline_root)
        if (e->shift[k][outer]!=e->shift[lo][outer]||
            (e->shift[k][inner]!=e->shift[lo][inner]&&e->shift[k][inner]!=e->shift[hi][inner]))
          e->sliding=false;
      e->slide_low=lo;e->slide_high=hi;
    }
  }
  return true;
}

static int remapped(const int *old,const int *fresh,size_t count,int v) {
  for (size_t k=0;k<count;++k) if (v==old[k]) return fresh[k];
  return v;
}

static int recompute_value(emitter_t *e,size_t access,int axis,int at) {
  const nyr_poly_region_t *a=e->producer;
  int old[48],fresh[48];size_t count=2;
  for (unsigned d=0;d<2;++d) {
    old[d]=a->iv[d];fresh[d]=(int)d==axis?at:e->r->iv[d];
    if (e->shift[access][d]) fresh[d]=bin(e,NYIR_ADD_I64,fresh[d],constant(e,e->shift[access][d]));
  }
  for (size_t p=a->body_begin;p<a->body_end;++p) {
    nyr_inst_t in=e->src->data[p];
    if (in.op==NYIR_STORE_I64) return remapped(old,fresh,count,in.c);
    if (in.op==NYIR_NOP||inline_address_value(e,in.dst)) continue;
    if (count==sizeof(old)/sizeof(*old)||in.dst<0) break;
    nyr_clear_derived_facts(&in);
    int *ops[]={&in.a,&in.b,&in.c,&in.d,&in.e,&in.f};
    for (size_t k=0;k<6;++k) *ops[k]=remapped(old,fresh,count,*ops[k]);
    old[count]=in.dst;in.dst=-1;fresh[count++]=emit(e,in);
  }
  e->ok=false;return -1;
}

static void emit_point_body(emitter_t *e,const nyr_poly_region_t *r,
                            point_value_t *values,size_t *count) {
  size_t access=0;
  for (size_t p=r->body_begin;p<r->body_end;++p) {
    nyr_inst_t in=e->src->data[p];
    nyr_clear_derived_facts(&in);
    bool mem=in.op==NYIR_LOAD_I64||in.op==NYIR_STORE_I64;
    if (!mem||r->external) {
      if (!inline_address_value(e,in.dst)) emit(e,in);
      continue;
    }
    if (access>=r->access_count) {e->ok=false;return;}
    const nyr_poly_access_t *a=&r->access[access++];
    if (a->write!=(in.op==NYIR_STORE_I64)) {e->ok=false;return;}
    bool word=point_word(&in,a);
    if (e->producer&&a->offset.base==e->inline_root) {
      if (a->write||!word) {e->ok=false;return;}
      /* Reuse repeated identical translated reads in the same consumer. */
      int value=-1;
      for (size_t k=0;k<*count;++k)
        if (same_ref(a,&values[k].access)&&values[k].type==in.scalar_class) value=values[k].value;
      if (value<0&&e->sliding&&same_ref(a,&r->access[e->slide_low])) value=e->slide_phi;
      if (value<0) value=recompute_value(e,access-1,-1,-1);
      nyr_inst_t copy=inst(NYIR_COPY);copy.dst=in.dst;copy.a=value;
      copy.scalar_class=in.scalar_class;copy.debug=in.debug;emit(e,copy);
      if (e->sliding&&same_ref(a,&r->access[e->slide_high])) e->slide_next=in.dst;
      if (*count<3*NYR_POLY_MAX_ACCESS) values[(*count)++]=(point_value_t){*a,in.dst,in.scalar_class};
      continue;
    }
    if (a->write) {
      for (size_t k=0;k<*count;) {
        if (point_overlap(r,a,&values[k].access)) values[k]=values[--*count];
        else ++k;
      }
    } else if (word) {
      for (size_t k=0;k<*count;++k) {
        if (!same_ref(a,&values[k].access)||values[k].type!=in.scalar_class) continue;
        nyr_inst_t copy=inst(NYIR_COPY);
        copy.dst=in.dst;copy.a=values[k].value;copy.scalar_class=in.scalar_class;
        copy.debug=in.debug;emit(e,copy);
        mem=false;break;
      }
      if (!mem) continue;
    }
    /* A deleted producer store still defines the same-point SSA value.
     * Every consumer load must actually forward, otherwise abort the
     * transaction instead of accidentally reading uninitialized storage. */
    if (inline_storage(e,a->offset.base)) {
      if (!a->write||!word||*count>=3*NYR_POLY_MAX_ACCESS) {e->ok=false;return;}
    } else emit(e,in);
    if (word&&*count<3*NYR_POLY_MAX_ACCESS)
      values[(*count)++]=(point_value_t){*a,a->write?in.c:in.dst,in.scalar_class};
  }
  if (!r->external&&access!=r->access_count) e->ok=false;
}

/* Promote one full-width location invariant along the innermost loop.
 * All other references must use different roots. For external roots the
 * versioned fast path proves write/read envelopes disjoint, and the
 * original recurrence remains in the fallback. Trip counts are fixed
 * and positive; no speculation across zero-trip loops. */
static bool promotable_word(const nyr_inst_t *in,const nyr_poly_access_t *a) {
  return point_word(in,a);
}

static bool plan_promotion(emitter_t *e) {
  const nyr_poly_region_t *r=e->r;
  if (e->second||nyr_poly_dims(r)!=3||e->schedule.tile||
      e->schedule.wavefront) return false;
  unsigned inner=nyr_poly_axis(e->schedule,3,2),mid=nyr_poly_axis(e->schedule,3,1);
  if (r->extent[inner]<2||!e->block_count||e->block_count>POLY_MAX_BLOCK||
      (!e->horizontal&&r->extent[mid]<e->block_count)||r->body_end-r->body_begin>128) return false;
  for (unsigned d=0;d<3;++d)
    if (r->symbolic[d]||r->initial[d]||r->stride[d]!=1) return false;
  size_t wr=SIZE_MAX,rd=SIZE_MAX;
  for (size_t k=0;k<r->access_count;++k) {
    const nyr_poly_access_t *a=&r->access[k];
    if (!a->write) continue;
    if (wr!=SIZE_MAX||a->width!=8||a->offset.coeff[inner]) return false;
    wr=k;
  }
  if (wr==SIZE_MAX) return false;
  const nyr_poly_access_t *w=&r->access[wr];
  /* The blocked schedule interleaves lanes on the middle axis. Output
   * byte intervals in those lanes must be disjoint; all inputs are read
   * only and rooted separately from the output. The outer axis retains
   * its order. This is the legality proof for this restricted microtile. */
  if (!e->horizontal&&e->block_count>1&&w->offset.coeff[mid]>-8&&w->offset.coeff[mid]<8) return false;
  for (size_t k=0;k<r->access_count;++k) {
    const nyr_poly_access_t *a=&r->access[k];
    if (k==wr||a->offset.base!=w->offset.base) continue;
    if (rd!=SIZE_MAX||k>wr||!same_ref(a,w)) return false;
    rd=k;
  }
  if (rd==SIZE_MAX) return false;
  size_t k=0,load=SIZE_MAX,store=SIZE_MAX;
  for (size_t p=r->body_begin;p<r->body_end;++p) {
    const nyr_inst_t *in=&e->src->data[p];
    if (in->op!=NYIR_LOAD_I64&&in->op!=NYIR_STORE_I64) continue;
    if (k>=r->access_count) return false;
    if (k==rd||k==wr) {
      if (!promotable_word(in,&r->access[k])) return false;
      if (k==rd) load=p;else store=p;
    }
    ++k;
  }
  if (k!=r->access_count||load==SIZE_MAX||store==SIZE_MAX||
      e->src->data[load].scalar_class!=e->src->data[store].scalar_class) return false;
  e->promote_load=load;e->promote_store=store;e->promote=true;
  return true;
}

/* Horizontal vectorization changes the association of an additive FP
 * recurrence, not its memory order. Only the final store's direct add may
 * consume the accumulator. Other lane-varying inputs must be unit-stride
 * (checked by vector_body). Fast-math is required by the caller. */
static bool plan_horizontal(const emitter_t *e) {
  const nyr_poly_region_t *r=e->r;
  unsigned inner=nyr_poly_axis(e->schedule,3,2);
  const nyr_inst_t *load=&e->src->data[e->promote_load];
  const nyr_inst_t *store=&e->src->data[e->promote_store];
  if (r->extent[inner]<4*e->block_count||load->scalar_class!=NYIR_SCALAR_F64||
      (e->block_count!=1&&e->block_count!=2)||!nyr_fast_math()) return false;
  size_t add=SIZE_MAX;
  for (size_t p=r->body_begin;p<r->body_end;++p) {
    const nyr_inst_t *in=&e->src->data[p];
    if (in->dst==store->c&&in->op==NYIR_ADD_F64&&
        ((in->a==load->dst)!=(in->b==load->dst))) add=p;
  }
  if (add==SIZE_MAX||add>=e->promote_store) return false;
  for (size_t p=r->body_begin;p<r->body_end;++p) {
    const nyr_inst_t *in=&e->src->data[p];
    if (in->op==NYIR_NOP) continue;
    int ops[]={in->a,in->b,in->c,in->d,in->e,in->f};
    for (size_t j=0;j<sizeof(ops)/sizeof(*ops);++j) {
      if (ops[j]==load->dst&&p!=add) return false;
      if (ops[j]==store->c&&p!=e->promote_store) return false;
    }
  }
  return true;
}

/* Reconstruct the proven affine byte address before the inner loop.
 * Ordinals equal original IVs by the promotion precondition. */
static int promotion_address(emitter_t *e,unsigned inner,unsigned lane) {
  const nyr_poly_region_t *r=e->r;
  const nyr_poly_affine_t *a=NULL;
  for (size_t k=0;k<r->access_count;++k)
    if (r->access[k].write) {a=&r->access[k].offset;break;}
  if (!a) {e->ok=false;return -1;}
  int addr=a->base;
  if (a->constant) addr=bin(e,NYIR_ADD_I64,addr,constant(e,a->constant));
  for (unsigned d=0;d<3;++d) if (d!=inner&&a->coeff[d]) {
    int off=d==nyr_poly_axis(e->schedule,3,1)?e->block_iv[lane]:r->iv[d];
    if (a->coeff[d]!=1) off=bin(e,NYIR_MUL_I64,off,constant(e,a->coeff[d]));
    addr=bin(e,NYIR_ADD_I64,addr,off);
  }
  return addr;
}

/* A four-wide microtile may use a packed recurrence instead of scalar
 * lanes when all input addresses are invariant or consecutive along the
 * blocked axis. The preflight walks exactly the same SSA graph as emission;
 * unsupported scalar consumers of vector values reject before cloning. */
static int vector_value(const int *old,const int *mapped,size_t n,int value) {
  for (size_t k=0;k<n;++k) if (old[k]==value) return mapped[k];
  return -1;
}

static bool vector_body(emitter_t *e,bool generate,unsigned group) {
  const nyr_poly_region_t *r=e->r;
  unsigned mid=nyr_poly_axis(e->schedule,3,e->horizontal?2:1);
  int old[128],mapped[128],scalar_old[128],scalar_new[128];
  size_t count=0,access=0,scalar_count=0;
  for (size_t p=r->body_begin;p<r->body_end;++p) {
    nyr_inst_t in=e->src->data[p];nyr_clear_derived_facts(&in);
    int value=-1;
    int a=vector_value(old,mapped,count,in.a);
    int b=vector_value(old,mapped,count,in.b);
    int c=vector_value(old,mapped,count,in.c);
    if (in.op==NYIR_NOP) continue;
    int original=in.dst;
    if (generate&&(group||e->horizontal_epilogue)) {
      int *ops[]={&in.a,&in.b,&in.c,&in.d,&in.e,&in.f};
      for (size_t k=0;k<sizeof(ops)/sizeof(*ops);++k) {
        if (*ops[k]==r->iv[mid]) {*ops[k]=e->block_iv[group];continue;}
        int replacement=vector_value(scalar_old,scalar_new,scalar_count,*ops[k]);
        if (replacement>=0) *ops[k]=replacement;
      }
      if (original>=0) {
        if (scalar_count==128||e->tmp.next_value==INT_MAX) return false;
        in.dst=e->tmp.next_value++;
        scalar_old[scalar_count]=original;scalar_new[scalar_count++]=in.dst;
      }
    }
    if (in.op==NYIR_LOAD_I64||in.op==NYIR_STORE_I64) {
      if (access>=r->access_count) return false;
      const nyr_poly_access_t *x=&r->access[access++];
      if (a>=0||!promotable_word(&in,x)||in.scalar_class!=NYIR_SCALAR_F64) return false;
      if (p==e->promote_store) {
        if (c<0||x->offset.coeff[mid]!=(e->horizontal?0:8)) return false;
        if (generate) e->promote_value[group]=c;
        continue;
      }
      if (x->write||(x->offset.coeff[mid]!=0&&x->offset.coeff[mid]!=8)) return false;
      if (p==e->promote_load) value=generate?e->promote_phi[group]:in.dst;
      else if (x->offset.coeff[mid]==8) {
        value=in.dst;
        if (generate) {in.op=NYIR_VEC4_LOAD_F64;in.effects=0;emit(e,in);}
      } else {
        value=in.dst;
        if (generate) {
          /* Share the invariant input broadcast across vector groups. */
          if (group) value=e->vector_shared[access-1];
          else {
            emit(e,in);
            nyr_inst_t v=inst(NYIR_VEC4_SET1_F64);v.a=in.dst;v.scalar_class=NYIR_SCALAR_F64;
            value=emit(e,v);e->vector_shared[access-1]=value;
          }
        }
      }
    } else if (in.op==NYIR_CONST_F64) {
      value=in.dst;
      if (generate) {
        emit(e,in);nyr_inst_t v=inst(NYIR_VEC4_SET1_F64);
        v.a=in.dst;v.scalar_class=NYIR_SCALAR_F64;value=emit(e,v);
      }
    } else if (in.op==NYIR_ADD_F64||in.op==NYIR_SUB_F64||in.op==NYIR_MUL_F64) {
      if (a<0||b<0) return false;
      value=in.dst;
      if (generate) {
        in.op=in.op==NYIR_ADD_F64?NYIR_VEC4_ADD_F64:
              in.op==NYIR_SUB_F64?NYIR_VEC4_SUB_F64:NYIR_VEC4_MUL_F64;
        in.a=a;in.b=b;in.scalar_class=NYIR_SCALAR_F64;in.effects=0;emit(e,in);
      }
    } else if (in.op==NYIR_COPY&&a>=0) value=a;
    else {
      switch (in.op) {
      case NYIR_CONST_I64:case NYIR_COPY:case NYIR_ADD_I64:case NYIR_SUB_I64:
      case NYIR_MUL_I64:case NYIR_SHL_I64:case NYIR_SAR_I64:
      case NYIR_AND_I64:case NYIR_OR_I64:case NYIR_XOR_I64:break;
      default:return false;
      }
      if (a>=0||b>=0||c>=0) return false;
      if (generate) emit(e,in);
    }
    if (value>=0) {
      if (original<0||count==128) return false;
      old[count]=original;mapped[count++]=value;
    }
    if (generate&&!e->ok) return false;
  }
  return access==r->access_count;
}

/* Emit the original statement once per register lane. Each copy gets a
 * disjoint SSA namespace, apart from values defined outside the body.
 * An input load invariant along the blocked axis can reuse lane zero's
 * exact value: the only write targets a different allocation. */
static void emit_promoted_body(emitter_t *e) {
  const nyr_poly_region_t *r=e->r;
  unsigned mid=nyr_poly_axis(e->schedule,3,1);
  for (unsigned lane=0;lane<e->block_count;++lane) {
    int old[128],fresh[128];size_t count=0,access=0;
    for (size_t p=r->body_begin;p<r->body_end;++p) {
      nyr_inst_t in=e->src->data[p];nyr_clear_derived_facts(&in);
      bool mem=in.op==NYIR_LOAD_I64||in.op==NYIR_STORE_I64;
      const nyr_poly_access_t *a=NULL;
      if (mem) {
        if (access>=r->access_count) {e->ok=false;return;}
        a=&r->access[access++];
      }
      int original=in.dst;
      if (lane||e->tail) {
        int *ops[]={&in.a,&in.b,&in.c,&in.d,&in.e,&in.f};
        for (size_t k=0;k<sizeof(ops)/sizeof(*ops);++k) {
          if (*ops[k]==r->iv[mid]) {*ops[k]=e->block_iv[lane];continue;}
          if (e->tail&&*ops[k]==r->iv[nyr_poly_axis(e->schedule,3,2)]) {
            *ops[k]=e->tail_iv[nyr_poly_axis(e->schedule,3,2)];continue;
          }
          for (size_t v=0;v<count;++v)
            if (*ops[k]==old[v]) {*ops[k]=fresh[v];break;}
        }
        if (original>=0) {
          if (count==128||e->tmp.next_value==INT_MAX) {e->ok=false;return;}
          in.dst=e->tmp.next_value++;old[count]=original;fresh[count++]=in.dst;
        }
      }
      if (p==e->promote_store) {e->promote_value[lane]=in.c;continue;}
      if (p==e->promote_load||(lane&&a&&!a->write&&!a->offset.coeff[mid])) {
        nyr_inst_t copy=inst(NYIR_COPY);copy.dst=in.dst;
        copy.a=p==e->promote_load?e->promote_phi[lane]:original;
        copy.scalar_class=in.scalar_class;copy.debug=in.debug;emit(e,copy);
      } else emit(e,in);
    }
    if (access!=r->access_count) {e->ok=false;return;}
  }
}

static void loop(emitter_t *e, unsigned level, int64_t pre, int64_t exit) {
  if (!e->ok) return;
  unsigned dims=nyr_poly_dims(e->r);
  bool strip=dims==2 && e->schedule.tile && e->schedule.inner_tile==1;
  unsigned levels=strip?3:e->schedule.tile?dims*2:dims;
  if (level==levels) {
    if (e->promote) {
      if ((e->vector||e->horizontal)&&!e->tail) {
        for (unsigned g=0;g<(e->horizontal?e->block_count:e->block_count/4)&&e->ok;++g) {
          if (e->horizontal) {
            int iv=e->r->iv[nyr_poly_axis(e->schedule,3,2)];
            e->block_iv[g]=g?bin(e,NYIR_ADD_I64,iv,constant(e,4*g)):iv;
          }
          if (!vector_body(e,true,g)) e->ok=false;
        }
      }
      else emit_promoted_body(e);
      branch(e,exit);return;
    }
    point_value_t values[3*NYR_POLY_MAX_ACCESS];size_t count=0;
    emit_point_body(e,e->r,values,&count);
    if (e->second) {
      for (unsigned d=0;d<dims;++d) {
        nyr_inst_t copy=inst(NYIR_COPY);copy.dst=e->second->iv[d];copy.a=e->r->iv[d];emit(e,copy);
      }
      emit_point_body(e,e->second,values,&count);
    }
    if (e->third) {
      for (unsigned d=0;d<dims;++d) {
        nyr_inst_t copy=inst(NYIR_COPY);copy.dst=e->third->iv[d];copy.a=e->r->iv[d];emit(e,copy);
      }
      emit_point_body(e,e->third,values,&count);
    }
    branch(e,exit);return;
  }
  bool wave=e->schedule.wavefront;
  unsigned d=wave?(e->schedule.interchange?1:0):nyr_poly_axis(e->schedule,dims,level%dims);
  bool tile=e->schedule.tile && level<(strip?1u:dims);
  int low=e->zero, high=e->bound[d], step=tile?e->tile[d]:e->one;
  if (e->isolate&&!level) {
    if (e->isolate_tail) low=e->full_end;
    else high=e->full_end;
  }
  /* Separate full register microtiles from the scalar remainder.
   * The hot loop has no lane predicates; the disjoint tail covers
   * precisely the remaining ordinals (CLooG-style domain splitting). */
  bool split_tail=e->promote&&!e->horizontal&&!e->tail&&level==1&&e->block_count>1&&
                  e->r->extent[d]%e->block_count;
  if (e->promote&&!e->horizontal&&level==1&&e->block_count>1) step=constant(e,e->block_count);
  if (split_tail) high=constant(e,e->r->extent[d]/e->block_count*e->block_count);
  if (e->tail&&level==1) low=e->tail_begin;

  if (wave) {
    unsigned k=nyr_poly_skew(e->schedule);
    if (!level) {
      /* q = k*i+j, 0 <= q < k*(N-1)+M. Empty N or M may
       * produce empty inner bands, never a spurious statement. */
      int last=bin(e,NYIR_SUB_I64,e->bound[d],e->one);
      if (k!=1) last=bin(e,NYIR_MUL_I64,last,e->skew);
      high=bin(e,NYIR_ADD_I64,last,e->bound[1-d]);
    } else {
      int after=bin(e,NYIR_ADD_I64,e->wave,e->one);
      int delta=bin(e,NYIR_SUB_I64,after,e->bound[1-d]);
      low=select_bound(e,e->zero,delta,false);
      int last=e->wave;
      if (k!=1) {
        /* ceil(max(0,q-M+1)/k); all divisions have nonnegative
         * dividends and positive constant divisors. */
        low=bin(e,NYIR_DIV_I64,bin(e,NYIR_ADD_I64,low,constant(e,k-1)),e->skew);
        last=bin(e,NYIR_DIV_I64,last,e->skew);
      }
      high=select_bound(e,e->bound[d],bin(e,NYIR_ADD_I64,last,e->one),true);
      if (e->r->triangle) {
        unsigned triangle=e->r->triangle;
        if (d) triangle=triangle>2?triangle-2:triangle+2;
        int num=e->wave;
        if (triangle>2) {
          /* j=q-k*i >= i+cut: i < floor((q+k+1-cut)/(k+1)). */
          int add=triangle==3?e->skew:constant(e,k+1);
          num=bin(e,NYIR_ADD_I64,num,add);
          int bound=k==1?bin(e,NYIR_SAR_I64,num,e->one):
                          bin(e,NYIR_DIV_I64,num,e->skew_plus_one);
          high=select_bound(e,high,bound,true);
        } else {
          /* j=q-k*i <= i-cut. */
          if (triangle==2) num=bin(e,NYIR_ADD_I64,num,e->skew);
          int bound=k==1?bin(e,NYIR_SAR_I64,num,e->one):
                          bin(e,NYIR_DIV_I64,num,e->skew_plus_one);
          if (triangle==1) bound=bin(e,NYIR_ADD_I64,bound,e->one);
          low=select_bound(e,low,bound,false);
        }
      }
    }
  }
  if (e->schedule.tile && !tile && (!strip || level==2)) {
    low=e->origin[d];
    int tile_end=bin(e,NYIR_ADD_I64,low,e->tile[d]);
    unsigned width=dims==3 || d==(e->schedule.interchange?1u:0u)?e->schedule.tile:
                   e->schedule.inner_tile?e->schedule.inner_tile:e->schedule.tile;
    /* Full tiles need no dynamic min; triangle clipping is still separate. */
    high=e->isolate?(e->isolate_tail?e->bound[d]:tile_end):
      e->r->symbolic[d] || e->r->extent[d]%width ?
      select_bound(e,tile_end,high,true):tile_end;
  }
  /* CLooG-style projection of the triangle onto the scheduled prefix.
   * Restrict the outer band to nonempty rows, then restrict the second
   * tile/strip band to tiles intersecting those rows. A tile origin is
   * rounded DOWN on a lower bound, since the point loop clips its tail.
   * Final point clipping below still defines the exact integer domain. */
  if (!wave&&e->r->triangle) {
    unsigned greater=e->r->triangle>2?1u:0u;
    bool strict=(e->r->triangle&1u)!=0;
    if (!level&&strict) {
      if (d!=greater) high=bin(e,NYIR_SUB_I64,high,e->one);
      else if (!tile) low=e->one;
    }
    if (e->schedule.tile&&level==1) {
      unsigned first=1-d;
      int origin=e->origin[first];
      if (d==greater) {
        int cut=strict?bin(e,NYIR_ADD_I64,origin,e->one):origin;
        if (tile) cut=bin(e,NYIR_MUL_I64,
                          bin(e,NYIR_DIV_I64,cut,e->tile[d]),e->tile[d]);
        low=cut;
      } else {
        int end=bin(e,NYIR_ADD_I64,origin,e->tile[first]);
        end=select_bound(e,end,e->bound[first],true);
        if (strict) end=bin(e,NYIR_SUB_I64,end,e->one);
        high=select_bound(e,high,end,true);
      }
    }
  }
  /* The second point dimension is constrained by the first. Generate
   * clipped affine bounds, not a rectangular loop with a per-point guard. */
  if (!wave && e->r->triangle && level == levels - 1) {
    int other=e->r->iv[1-d];
    bool strict=(e->r->triangle&1u)!=0;
    if (d==(e->r->triangle>2?0u:1u)) {
      if (!strict) other=bin(e,NYIR_ADD_I64,other,e->one);
      high=select_bound(e,high,other,true);
    } else {
      if (strict) other=bin(e,NYIR_ADD_I64,other,e->one);
      low=select_bound(e,low,other,false);
    }
  }
  bool slide=e->sliding&&level==levels-1;
  int slide_init=-1;size_t slide_at=0;
  if (slide) {
    slide_init=recompute_value(e,e->slide_low,(int)d,e->initial[d]);
    e->slide_next=-1;
  }
  bool promote=e->promote&&level==levels-1;
  bool vector=promote&&e->vector&&!e->tail;
  bool horizontal=promote&&e->horizontal;
  unsigned accumulators=vector?e->block_count/4:e->block_count;
  int scalar_init=-1;
  if (horizontal) {
    unsigned width=4*e->block_count;
    high=constant(e,e->r->extent[d]/width*width);
    step=constant(e,width);
  }
  int addr[POLY_MAX_BLOCK],init[POLY_MAX_BLOCK];
  int64_t finish=split_tail?++e->label:exit;
  if (promote) {
    unsigned mid=nyr_poly_axis(e->schedule,3,1);
    int middle_iv=e->tail?e->tail_iv[mid]:e->r->iv[mid];
    for (unsigned lane=0;lane<accumulators;++lane) {
      if (horizontal&&lane) {init[lane]=init[0];continue;}
      e->block_iv[lane]=lane?bin(e,NYIR_ADD_I64,middle_iv,constant(e,vector?4*lane:lane)):middle_iv;
      addr[lane]=promotion_address(e,d,lane);
      nyr_inst_t load=e->src->data[e->promote_load];
      nyr_clear_derived_facts(&load);load.dst=-1;load.a=addr[lane];
      if (vector) {load.op=NYIR_VEC4_LOAD_F64;load.effects=0;}
      init[lane]=emit(e,load);
      if (horizontal) {
        scalar_init=init[lane];
        nyr_inst_t zero=inst(NYIR_CONST_F64);zero.scalar_class=NYIR_SCALAR_F64;
        nyr_inst_t broadcast=inst(NYIR_VEC4_SET1_F64);
        broadcast.a=emit(e,zero);broadcast.scalar_class=NYIR_SCALAR_F64;
        init[lane]=emit(e,broadcast);
      }
    }
    finish=++e->label;
  }
  int64_t h=++e->label,b=++e->label,l=++e->label;
  branch(e,h);mark(e,h);
  nyr_phi_incoming_t *incoming=malloc(2*sizeof(*incoming));
  if (!incoming) {e->ok=false;return;}
  incoming[0]=(nyr_phi_incoming_t){.predecessor_label=pre,.value=low};
  incoming[1]=(nyr_phi_incoming_t){.predecessor_label=l,.value=low};
  bool normalize = !tile && (!wave || level) && (e->r->initial[d] || e->r->stride[d] != 1);
  nyr_inst_t phi=inst(NYIR_PHI);phi.dst=(tile || normalize || (wave&&!level))?-1:e->r->iv[d];
  if (e->tail) phi.dst=-1;
  phi.phi_incoming=incoming;phi.phi_incoming_len=2;
  size_t at=e->tmp.len;
  int iv=emit(e,phi);
  if (e->tail) e->tail_iv[d]=iv;
  if (!e->ok) {free(incoming);return;}
  if (slide) {
    nyr_phi_incoming_t *window=malloc(2*sizeof(*window));
    if (!window) {e->ok=false;return;}
    window[0]=(nyr_phi_incoming_t){.predecessor_label=pre,.value=slide_init};
    window[1]=(nyr_phi_incoming_t){.predecessor_label=l,.value=slide_init};
    nyr_inst_t value=inst(NYIR_PHI);value.phi_incoming=window;value.phi_incoming_len=2;
    const nyr_inst_t *read=access_inst(e->src,e->r,e->slide_low);
    if (!read) {free(window);e->ok=false;return;}
    value.scalar_class=read->scalar_class;
    slide_at=e->tmp.len;e->slide_phi=emit(e,value);
    if (!e->ok) {free(window);return;}
  }
  size_t acc_at[POLY_MAX_BLOCK];
  if (promote) for (unsigned lane=0;lane<accumulators;++lane) {
    nyr_phi_incoming_t *acc=malloc(2*sizeof(*acc));
    if (!acc) {e->ok=false;return;}
    acc[0]=(nyr_phi_incoming_t){.predecessor_label=pre,.value=init[lane]};
    acc[1]=(nyr_phi_incoming_t){.predecessor_label=l,.value=init[lane]};
    nyr_inst_t acc_phi=inst(NYIR_PHI);acc_phi.phi_incoming=acc;acc_phi.phi_incoming_len=2;
    acc_phi.scalar_class=e->src->data[e->promote_load].scalar_class;
    acc_at[lane]=e->tmp.len;e->promote_phi[lane]=emit(e,acc_phi);
    if (!e->ok) {free(acc);return;}
  }
  if (tile) e->origin[d]=iv;
  if (wave&&!level) e->wave=iv;
  int cond=bin(e,NYIR_CMP_I64,iv,high);
  nyr_inst_t test=inst(NYIR_BR_IF);test.a=cond;test.imm=b;emit(e,test);
  branch(e,finish);mark(e,b);
  if (normalize) {
    int value=iv;
    if (e->r->stride[d] != 1) value=bin(e,NYIR_MUL_I64,value,e->stride[d]);
    nyr_inst_t actual=inst(e->r->initial[d] ? NYIR_ADD_I64 : NYIR_COPY);
    actual.dst=e->r->iv[d]; actual.a=value;
    if (e->r->initial[d]) actual.b=e->initial[d];
    emit(e,actual);
  }
  if (wave&&level) {
    int scaled=nyr_poly_skew(e->schedule)==1?iv:bin(e,NYIR_MUL_I64,iv,e->skew);
    int value=bin(e,NYIR_SUB_I64,e->wave,scaled);
    if (e->r->stride[1-d]!=1) value=bin(e,NYIR_MUL_I64,value,e->stride[1-d]);
    nyr_inst_t actual=inst(e->r->initial[1-d]?NYIR_ADD_I64:NYIR_COPY);
    actual.dst=e->r->iv[1-d];actual.a=value;
    if (e->r->initial[1-d]) actual.b=e->initial[1-d];
    emit(e,actual);
  }
  loop(e,level+1,b,l);
  mark(e,l);int next=bin(e,NYIR_ADD_I64,iv,step);branch(e,h);
  if (e->ok) e->tmp.data[at].phi_incoming[1].value=next;
  if (slide&&e->ok) {
    if (e->slide_next<0) {e->ok=false;return;}
    e->tmp.data[slide_at].phi_incoming[1].value=e->slide_next;
  }
  if (promote&&e->ok) {
    mark(e,finish);
    for (unsigned lane=0;lane<accumulators;++lane)
      e->tmp.data[acc_at[lane]].phi_incoming[1].value=e->promote_value[lane];
    for (unsigned lane=0;lane<(horizontal?1:accumulators);++lane) {
      int result=e->promote_phi[lane];
      if (horizontal) {
        for (unsigned g=1;g<accumulators;++g) {
          nyr_inst_t add=inst(NYIR_VEC4_ADD_F64);
          add.a=result;add.b=e->promote_phi[g];add.scalar_class=NYIR_SCALAR_F64;
          result=emit(e,add);
        }
        /* Finish complete vector groups before horizontal reduction. This
         * bounds the scalar epilogue to three iterations regardless of the
         * main microtile width. The epilogue gets fresh SSA definitions;
         * no values defined solely inside the hot loop are reused here. */
        unsigned groups=e->block_count,width=4*groups,n=e->r->extent[d];
        e->horizontal_epilogue=true;
        for (unsigned k=n/width*width;k<n/4*4&&e->ok;k+=4) {
          e->block_iv[0]=constant(e,k);e->promote_phi[0]=result;
          if (!vector_body(e,true,0)) e->ok=false;
          result=e->promote_value[0];
        }
        e->horizontal_epilogue=false;
        nyr_inst_t reduce=inst(NYIR_VEC4_REDUCE_ADD_F64);
        reduce.a=scalar_init;reduce.b=result;reduce.scalar_class=NYIR_SCALAR_F64;
        result=emit(e,reduce);
        e->tail=true;e->block_count=1;
        e->block_iv[0]=e->r->iv[nyr_poly_axis(e->schedule,3,1)];
        for (unsigned k=n/4*4;k<n&&e->ok;++k) {
          e->tail_iv[d]=constant(e,k);e->promote_phi[0]=result;
          emit_promoted_body(e);result=e->promote_value[0];
        }
        e->tail=false;e->block_count=groups;
      }
      nyr_inst_t store=e->src->data[e->promote_store];
      nyr_clear_derived_facts(&store);store.a=addr[lane];store.c=result;
      if (vector) {store.op=NYIR_VEC4_STORE_F64;store.effects=0;store.b=store.c;store.c=-1;}
      emit(e,store);
    }
    branch(e,exit);
  }
  if (split_tail&&e->ok) {
    unsigned lanes=e->block_count;
    mark(e,finish);
    e->tail=true;e->tail_begin=high;e->block_count=1;
    loop(e,level,finish,exit);
    e->block_count=lanes;e->tail=false;
  }
}

/* The second copy of a scheduled region uses a distinct SSA namespace.
 * Fresh temporaries already have distinct IDs. Remap only reused original
 * definitions in this suffix, including incoming values of generated PHIs.
 * Original preheader constants and roots are shared, never duplicated. */
static bool rename_isolated_tail(emitter_t *e,size_t begin) {
  int old[128],fresh[128];size_t count=0;
  for (size_t p=begin;p<e->tmp.len;++p) {
    int v=e->tmp.data[p].dst;
    if (v<0||v>=e->src->next_value) continue;
    if (count==128||e->tmp.next_value==INT_MAX||
        remapped(old,fresh,count,v)!=v) return false;
    old[count]=v;fresh[count++]=e->tmp.next_value++;
  }
  for (size_t p=begin;p<e->tmp.len;++p) {
    nyr_inst_t *in=&e->tmp.data[p];
    int *ops[]={&in->dst,&in->a,&in->b,&in->c,&in->d,&in->e,&in->f};
    for (size_t k=0;k<sizeof(ops)/sizeof(*ops);++k)
      *ops[k]=remapped(old,fresh,count,*ops[k]);
    for (size_t k=0;k<in->phi_incoming_len;++k)
      in->phi_incoming[k].value=remapped(old,fresh,count,in->phi_incoming[k].value);
  }
  return true;
}

/* Unsigned address comparisons in the signed NYIR comparison vocabulary.
 * An overflowed exclusive end fails the guard before any reordered access. */
static int unsigned_le(emitter_t *e,int a,int b,int sign) {
  nyr_inst_t in=inst(NYIR_CMP_I64);
  in.a=bin(e,NYIR_XOR_I64,a,sign);in.b=bin(e,NYIR_XOR_I64,b,sign);
  in.cmp=NYIR_CMP_LE;
  return emit(e,in);
}

typedef struct { int base, low, high; int64_t lo, hi; bool write; } envelope_t;
/* Dynamic rectangles: exact affine min/max over the actual ordinal box.
 * For an empty dimension use ordinal zero; the resulting nonempty envelope
 * is conservative (the scheduled loop still executes zero times).
 * For any nonempty max rectangle all individual positive and negative
 * stride contributions, and the intermediate sums, fit i64: its minimum
 * is nonnegative and its maximum byte is < INT64_MAX, as checked below.
 * A symbolic triangle uses its already proven maximum domain instead. */
static bool access_envelope(emitter_t *e,const nyr_poly_access_t *a,
                            const int last[3],int *low,int *high) {
  const nyr_poly_region_t *r=e->r;
  ny_i128 lo=a->offset.constant,hi=lo+a->width;
  for (unsigned d=0;d<nyr_poly_dims(r);++d) if (!r->symbolic[d]) {
    ny_i128 delta=(ny_i128)a->offset.coeff[d]*(r->extent[d]-1);
    if (delta<0) lo+=delta;else hi+=delta;
  }
  if (lo<0||hi>INT64_MAX) return false;
  *low=constant(e,(int64_t)lo);*high=constant(e,(int64_t)hi);
  for (unsigned d=0;d<nyr_poly_dims(r);++d) if (r->symbolic[d]) {
    int64_t c=a->offset.coeff[d];if (!c) continue;
    int delta=c==1?last[d]:bin(e,NYIR_MUL_I64,last[d],constant(e,c));
    int *bound=c<0?low:high;
    *bound=bin(e,NYIR_ADD_I64,*bound,delta);
  }
  return e->ok;
}
static bool alias_guard(emitter_t *e,int64_t fast,int64_t slow) {
  envelope_t roots[4]={0};unsigned count=0;
  const nyr_poly_region_t *r=e->r;
  bool dynamic=false;int last[3]={-1,-1,-1};
  if (!r->triangle) for (unsigned d=0;d<nyr_poly_dims(r);++d)
    if (r->symbolic[d]) {
      dynamic=true;
      last[d]=select_bound(e,e->zero,bin(e,NYIR_SUB_I64,e->bound[d],e->one),false);
    }
  for (size_t k=0;k<r->access_count;++k) {
    const nyr_poly_access_t *a=&r->access[k];
    ny_i128 lo,hi;
    if (!nyr_poly_access_extrema(r,a,&lo,&hi)||lo<0||hi>=INT64_MAX) return false;
    ++hi;
    unsigned i=0;while (i<count&&roots[i].base!=a->offset.base) ++i;
    bool first=i==count;
    if (first) {
      if (count==4) return false;
      roots[count++]=(envelope_t){.base=a->offset.base,.lo=(int64_t)lo,.hi=(int64_t)hi};
    }
    envelope_t *x=&roots[i];x->write|=a->write;
    if (!dynamic) {
      if (lo<x->lo) x->lo=(int64_t)lo;
      if (hi>x->hi) x->hi=(int64_t)hi;
      continue;
    }
    /* A load/store of the same reference has the same envelope. */
    bool seen=false;
    for (size_t j=0;j<k;++j) seen|=same_ref(a,&r->access[j]);
    if (seen) continue;
    int low,high;
    if (!access_envelope(e,a,last,&low,&high)) return false;
    x->low=first?low:select_bound(e,x->low,low,true);
    x->high=first?high:select_bound(e,x->high,high,false);
  }
  int sign=constant(e,INT64_MIN),ok=e->one;
  for (unsigned i=0;i<count;++i) {
    envelope_t *x=&roots[i];
    if (!dynamic) {
      x->low=x->lo?bin(e,NYIR_ADD_I64,x->base,constant(e,x->lo)):x->base;
      x->high=bin(e,NYIR_ADD_I64,x->base,constant(e,x->hi));
    } else {
      x->low=bin(e,NYIR_ADD_I64,x->base,x->low);
      x->high=bin(e,NYIR_ADD_I64,x->base,x->high);
    }
    ok=bin(e,NYIR_AND_I64,ok,unsigned_le(e,x->base,x->high,sign));
  }
  for (unsigned i=0;i<count;++i) for (unsigned j=i+1;j<count;++j) {
    envelope_t *a=&roots[i],*b=&roots[j];
    if (!a->write&&!b->write) continue;
    int separate=bin(e,NYIR_OR_I64,unsigned_le(e,a->high,b->low,sign),
                         unsigned_le(e,b->high,a->low,sign));
    ok=bin(e,NYIR_AND_I64,ok,separate);
  }
  /* Convert boolean algebra to an explicit compare before BR_IF. */
  nyr_inst_t cmp=inst(NYIR_CMP_I64);cmp.a=ok;cmp.b=e->one;cmp.cmp=NYIR_CMP_EQ;
  nyr_inst_t test=inst(NYIR_BR_IF);test.a=emit(e,cmp);test.imm=fast;emit(e,test);
  branch(e,slow);
  return e->ok;
}

/* Clone the original region into a disjoint SSA/label namespace. The model
 * rejects all live-outs and side entrances, so only its exit is shared. */
static bool emit_fallback(emitter_t *e,int64_t pre) {
  const nyr_poly_region_t *r=e->r;
  const nyr_func_t *f=e->src;
  int old[128],fresh[128];int64_t labels[32],new_labels[32];
  size_t nv=0,nl=0;
  for (size_t p=r->begin;p<r->end;++p) {
    const nyr_inst_t *in=&f->data[p];
    if (in->dst>=0) {
      if (nv==128||e->tmp.next_value==INT_MAX) return false;
      old[nv]=in->dst;fresh[nv++]=e->tmp.next_value++;
    }
    if (in->op==NYIR_LABEL) {
      if (nl==32||e->label==INT64_MAX) return false;
      labels[nl]=in->imm;new_labels[nl++]=++e->label;
    }
  }
  if (!nl||labels[0]!=r->entry) return false;
  mark(e,pre);branch(e,new_labels[0]);
  for (size_t p=r->begin;p<r->end;++p) {
    nyr_inst_t in=f->data[p];nyr_clear_derived_facts(&in);
    if (in.symbol||in.extra_args||in.arg_sizes) return false;
    int *ops[]={&in.dst,&in.a,&in.b,&in.c,&in.d,&in.e,&in.f};
    for (size_t k=0;k<sizeof(ops)/sizeof(ops[0]);++k)
      for (size_t v=0;v<nv;++v) if (*ops[k]==old[v]) {*ops[k]=fresh[v];break;}
    if (in.op==NYIR_LABEL||in.op==NYIR_BR||in.op==NYIR_BR_IF) {
      bool found=false;
      for (size_t l=0;l<nl;++l) if (in.imm==labels[l]) {in.imm=new_labels[l];found=true;break;}
      if (!found&&in.imm!=r->exit) return false;
    }
    if (in.phi_incoming) {
      nyr_phi_incoming_t *phi=malloc(in.phi_incoming_len*sizeof(*phi));
      if (!phi) return false;
      memcpy(phi,in.phi_incoming,in.phi_incoming_len*sizeof(*phi));in.phi_incoming=phi;
      for (size_t k=0;k<in.phi_incoming_len;++k) {
        for (size_t v=0;v<nv;++v) if (phi[k].value==old[v]) {phi[k].value=fresh[v];break;}
        bool found=false;
        for (size_t l=0;l<nl;++l) if (phi[k].predecessor_label==labels[l]) {
          phi[k].predecessor_label=new_labels[l];found=true;break;
        }
        if (!found) phi[k].predecessor_label=pre;
      }
    }
    emit(e,in);
    if (!e->ok) {free(in.phi_incoming);return false;}
  }
  return true;
}

/* The detector has proved every intermediate and the exit recurrence fit
 * in i64 for the original bound's full range. Work in ordinal space. */
static int symbolic_extent(emitter_t *e,unsigned d) {
  const nyr_poly_region_t *r=e->r;
  int limit=r->bound_value[d];
  int64_t step=r->stride[d];
  if (!r->initial[d]&&step==1&&!r->inclusive[d])
    return r->minimum_extent[d]?limit:select_bound(e,e->zero,limit,false);
  int delta=step>0?bin(e,NYIR_SUB_I64,limit,e->initial[d]):
                   bin(e,NYIR_SUB_I64,e->initial[d],limit);
  int64_t mag=step>0?step:-step;
  int64_t adjustment=mag-(r->inclusive[d]?0:1);
  if (adjustment) delta=bin(e,NYIR_ADD_I64,delta,constant(e,adjustment));
  if (!r->minimum_extent[d]) delta=select_bound(e,e->zero,delta,false);
  return mag==1?delta:bin(e,NYIR_DIV_I64,delta,constant(e,mag));
}

/* A preheader/latch constant may also be consumed by the retained body.
 * Recreate only those constants, at the new region entry; never speculate
 * an arbitrary computation or carry stale range metadata. */
static void emit_body_constants(emitter_t *e,const nyr_poly_region_t *r,size_t end) {
  for (size_t p=r->begin;p<end;++p) {
    if (p>=r->body_begin&&p<r->body_end) continue;
    nyr_inst_t in=e->src->data[p];
    if (in.op!=NYIR_CONST_I64||in.dst<0) continue;
    bool needed=false;
    for (size_t q=r->body_begin;q<r->body_end&&!needed;++q) {
      const nyr_inst_t *u=&e->src->data[q];
      needed=u->a==in.dst||u->b==in.dst||u->c==in.dst||
             u->d==in.dst||u->e==in.dst||u->f==in.dst;
    }
    if (needed) {nyr_clear_derived_facts(&in);emit(e,in);}
  }
}

static bool emit_region(nyr_func_t *f,const nyr_poly_region_t *r,
                         const nyr_poly_region_t *second,const nyr_poly_region_t *third,
                         nyr_poly_schedule_t s,bool proven,unsigned block_count,nyr_poly_vector_mode_t vector_mode,bool require_inline,bool recompute) {
  bool vector=vector_mode==NYR_POLY_VECTOR_BLOCK,
       horizontal=vector_mode==NYR_POLY_VECTOR_REDUCTION;
  if (!f || !r || r->end>f->len || r->begin>=r->end ||
      r->body_begin<r->begin || r->body_end>r->end ||
      r->body_begin>r->body_end ||
      (second && (second->end>f->len || second->begin>=second->end ||
                  second->body_begin<second->begin ||
                  second->body_begin>second->body_end ||
                  second->body_end>second->end)) ||
      (third&&(!second||recompute||block_count||third->end>f->len||
               third->begin>=third->end||third->body_begin<third->begin||
               third->body_begin>third->body_end||third->body_end>third->end)) ||
      f->next_value>INT_MAX-((block_count||recompute||third)?4096:256) ||
      (recompute&&!second)||r->access_count>NYR_POLY_MAX_ACCESS||
      (second&&second->access_count>NYR_POLY_MAX_ACCESS)||
      (third&&third->access_count>NYR_POLY_MAX_ACCESS)) return false;
  if (r->external&&(!f->untagged_values||second||r->end-r->begin>128||
      f->next_value>INT_MAX-512)) return false;
  const nyr_poly_region_t *first=r;
  nyr_poly_region_t merged=recompute?*second:*r;
  size_t bridge=r->end;
  if (second) {
    merged.begin=r->begin;merged.entry=r->entry;merged.end=third?third->end:second->end;merged.exit=third?third->exit:second->exit;
    r=&merged;
  }
  /* The model only accepts payload-free statements. Do not share owned
   * pointers with the source even if a caller supplies an invalid model. */
  for (size_t p=first->body_begin;p<first->body_end;++p) {
    const nyr_inst_t *in=&f->data[p];
    if (in->symbol || in->extra_args || in->arg_sizes || in->phi_incoming)
      return false;
  }
  if (second) for (size_t p=second->body_begin;p<second->body_end;++p) {
    const nyr_inst_t *in=&f->data[p];
    if (in->symbol||in->extra_args||in->arg_sizes||in->phi_incoming) return false;
  }
  if (third) for (size_t p=third->body_begin;p<third->body_end;++p) {
    const nyr_inst_t *in=&f->data[p];
    if (in->symbol||in->extra_args||in->arg_sizes||in->phi_incoming) return false;
  }
  /* Clone once before emitting, so body debug locations borrow strings
   * already owned by the candidate. Discarding old instructions does not
   * release the candidate's string pool. */
  emitter_t e={.src=f,.r=r,.second=recompute?NULL:second,.third=third,.schedule=s,.ok=true,.block_count=block_count,.vector=vector,.horizontal=horizontal,.inline_root=-1,.inline_root2=-1};
  const char *inline_stage=getenv("NYTRIX_POLY_INLINE_STAGE");
  if (recompute) {
    if (!plan_recompute(&e,first,second)) return false;
  } else if (second&&(!inline_stage||strcmp(inline_stage,"0")))
    e.inline_root=inline_plan(f,first,second,e.inline_values,&e.inline_count);
  /* Reject an unsupported recurrence before cloning the whole function. */
  if (require_inline&&e.inline_root<0) return false;
  if (third) {
    size_t count=0;
    e.inline_root2=inline_plan(f,second,third,e.inline_values+e.inline_count,&count);
    if (e.inline_root2<0||e.inline_root2==e.inline_root) return false;
    e.inline_count+=count;
  }
  /* Test private placement before expensive dependence/CFG proofs. A
   * rejected storage candidate is a normal no-op, not a fusion request. */
  if (!proven&&(recompute?!nyr_poly_legal(second,s):
                second?!nyr_poly_fusion_legal(first,second,s):!nyr_poly_legal(first,s))) return false;
  if (second&&!nyr_poly_adjacent(f,first,second)) return false;
  /* Preserve every cross-stage dependence, not just neighboring pairs. */
  if (third&&(!nyr_poly_adjacent(f,second,third)||
      !nyr_poly_fusion_legal(first,third,s)||!nyr_poly_fusion_legal(second,third,s))) return false;
  if (block_count&&!plan_promotion(&e)) return false;
  if (vector&&((block_count!=4&&block_count!=8)||!nyr_fast_math()||!vector_body(&e,false,0))) return false;
  if (horizontal&&(!plan_horizontal(&e)||!vector_body(&e,false,0))) return false;
  nyr_func_t copy={0};
  if (!nyr_ir_func_clone(f,&copy)) return false;
  e.src=&copy;
  e.tmp.next_value=f->next_value;
  for (size_t p=0;p<f->len;++p)
    if (f->data[p].op==NYIR_LABEL && f->data[p].imm>e.label)
      e.label=f->data[p].imm;
  if (e.label>INT64_MAX-80) goto fail;
  mark(&e,r->entry);
  if (second) for (size_t p=bridge+1;p<second->begin;++p) {
    if (f->data[p].op!=NYIR_CONST_I64) continue;
    nyr_inst_t in=copy.data[p];nyr_clear_derived_facts(&in);emit(&e,in);
  }
  if (third) for (size_t p=second->end+1;p<third->begin;++p) {
    if (f->data[p].op!=NYIR_CONST_I64) continue;
    nyr_inst_t in=copy.data[p];nyr_clear_derived_facts(&in);emit(&e,in);
  }
  emit_body_constants(&e,first,bridge);
  if (second) emit_body_constants(&e,second,second->end);
  if (third) emit_body_constants(&e,third,third->end);
  e.zero=constant(&e,0);e.one=constant(&e,1);
  if (s.wavefront) {
    e.skew=nyr_poly_skew(s)==1?e.one:constant(&e,nyr_poly_skew(s));
    if (r->triangle&&nyr_poly_skew(s)!=1) e.skew_plus_one=constant(&e,nyr_poly_skew(s)+1);
  }
  unsigned dims=nyr_poly_dims(r);
  for (unsigned d=0;d<dims;++d) {
    unsigned width=dims==2 && d!=nyr_poly_axis(s,dims,0) && s.inner_tile ? s.inner_tile:s.tile;
    e.tile[d]=s.tile?constant(&e,width):-1;
    if (!r->stride[d]) goto fail;
    if (r->stride[d]==INT64_MIN) goto fail;
    e.initial[d]=r->initial[d]?constant(&e,r->initial[d]):e.zero;
    e.stride[d]=r->stride[d]!=1?constant(&e,r->stride[d]):e.one;
    e.bound[d]=d==1&&r->triangle&&r->symbolic[0]?e.bound[0]:
       r->symbolic[d]?symbolic_extent(&e,d):constant(&e,r->extent[d]);
  }
  int64_t fast=r->entry,slow=-1;
  if (r->external) {
    fast=++e.label;slow=++e.label;
    if (!alias_guard(&e,fast,slow)) goto fail;
    mark(&e,fast);
  }
  /* Polly's full/partial tile isolation trades code size for a regular
   * vectorizable point loop. Start with one strip axis and bounded body
   * size. The flag is opt-in until the downstream cost model is calibrated. */
  const char *isolate=getenv("NYTRIX_POLY_EXPERIMENTAL_ISOLATE");
  unsigned axis=nyr_poly_axis(s,dims,0);
  if (isolate&&!strcmp(isolate,"1")&&!second&&!third&&!block_count&&!r->external&&
      dims==2&&!r->triangle&&s.tile>1&&s.inner_tile==1&&
      r->extent[axis]>s.tile&&r->body_end-r->body_begin<=32&&
      (r->symbolic[axis]||r->extent[axis]%s.tile)&&f->next_value<INT_MAX-1024) {
    e.isolate=true;
    e.full_end=r->symbolic[axis]?bin(&e,NYIR_MUL_I64,
      bin(&e,NYIR_DIV_I64,e.bound[axis],e.tile[axis]),e.tile[axis]):
      constant(&e,r->extent[axis]/s.tile*s.tile);
    int64_t partial=++e.label;
    loop(&e,0,fast,partial);
    mark(&e,partial);
    size_t begin=e.tmp.len;
    e.isolate_tail=true;
    loop(&e,0,partial,r->exit);
    if (!e.ok||!rename_isolated_tail(&e,begin)) goto fail;
  } else loop(&e,0,fast,r->exit);
  if (!e.ok||(r->external&&!emit_fallback(&e,slow))) goto fail;
  size_t removed=r->end-r->begin, len=copy.len-removed+e.tmp.len;
  if (len>SIZE_MAX/sizeof(*copy.data)) goto fail;
  nyr_inst_t *data=malloc(len*sizeof(*data));
  if (!data) goto fail;
  for (size_t p=r->begin;p<r->end;++p) nyr_inst_discard(&copy.data[p]);
  memcpy(data,copy.data,r->begin*sizeof(*data));
  memcpy(data+r->begin,e.tmp.data,e.tmp.len*sizeof(*data));
  memcpy(data+r->begin+e.tmp.len,copy.data+r->end,(copy.len-r->end)*sizeof(*data));
  free(copy.data);copy.data=data;copy.len=copy.cap=len;
  copy.next_value=e.tmp.next_value;
  /* Move generated PHI payloads; the copied source owns debug strings. */
  free(e.tmp.data);e.tmp.data=NULL;e.tmp.len=e.tmp.cap=0;
  char err[256];
  if (e.inline_root>=0) {
    /* The escape proof identified every derived pointer. All its body
     * definitions were omitted and every matching load forwarded. Remove
     * only the proven allocation (no whole-function DCE or compaction). */
    for (size_t p=0;p<r->begin;++p)
      if (copy.data[p].op==NYIR_ALLOCA&&inline_storage(&e,copy.data[p].dst))
        nyr_inst_discard(&copy.data[p]);
  }
  nyr_refresh_metadata(&copy);
  if (!nyr_ir_verify(&copy,err,sizeof(err))) goto fail;
  nyr_poly_dump_model_chain(f,recompute?second:first,recompute?NULL:second,third,s,block_count,vector_mode,false);
  nyr_poly_dump_placement(f,first,second,third,e.inline_root,e.inline_root2,
                         recompute?(e.sliding?"sliding":"recompute"):"point");
  nyr_ir_func_free(f);*f=copy;
  nyr_ir_func_free(&e.tmp);return true;
fail:
  nyr_ir_func_free(&copy);nyr_ir_func_free(&e.tmp);return false;
}

bool nyr_poly_emit(nyr_func_t *f,const nyr_poly_region_t *r,nyr_poly_schedule_t s) {
  return emit_region(f,r,NULL,NULL,s,false,0,NYR_POLY_SCALAR,false,false);
}
bool nyr_poly_emit_promoted(nyr_func_t *f,const nyr_poly_region_t *r,nyr_poly_schedule_t s) {
  return emit_region(f,r,NULL,NULL,s,false,1,NYR_POLY_SCALAR,false,false);
}
bool nyr_poly_emit_horizontal(nyr_func_t *f,const nyr_poly_region_t *r,
                              nyr_poly_schedule_t s,unsigned groups) {
  return (groups==1||groups==2)&&
         emit_region(f,r,NULL,NULL,s,false,groups,NYR_POLY_VECTOR_REDUCTION,false,false);
}
bool nyr_poly_emit_blocked(nyr_func_t *f,const nyr_poly_region_t *r,nyr_poly_schedule_t s,unsigned lanes) {
  return lanes>0&&lanes<=POLY_MAX_BLOCK&&emit_region(f,r,NULL,NULL,s,false,lanes,NYR_POLY_SCALAR,false,false);
}
bool nyr_poly_emit_vector_blocked(nyr_func_t *f,const nyr_poly_region_t *r,
                                   nyr_poly_schedule_t s) {
  return emit_region(f,r,NULL,NULL,s,false,4,NYR_POLY_VECTOR_BLOCK,false,false);
}
bool nyr_poly_emit_vector_blocked_lanes(nyr_func_t *f,const nyr_poly_region_t *r,
                                       nyr_poly_schedule_t s,unsigned lanes) {
  return (lanes==4||lanes==8)&&emit_region(f,r,NULL,NULL,s,false,lanes,NYR_POLY_VECTOR_BLOCK,false,false);
}
bool nyr_poly_emit_fused(nyr_func_t *f,const nyr_poly_region_t *a,
                         const nyr_poly_region_t *b,nyr_poly_schedule_t s) {
  return b&&emit_region(f,a,b,NULL,s,false,0,NYR_POLY_SCALAR,false,false);
}

bool nyr_poly_emit_inlined(nyr_func_t *f,const nyr_poly_region_t *a,
                           const nyr_poly_region_t *b,nyr_poly_schedule_t s) {
  return b&&emit_region(f,a,b,NULL,s,false,0,NYR_POLY_SCALAR,true,false);
}

bool nyr_poly_emit_chain(nyr_func_t *f,const nyr_poly_region_t *a,
                         const nyr_poly_region_t *b,const nyr_poly_region_t *c,
                         nyr_poly_schedule_t s) {
  return b&&c&&emit_region(f,a,b,c,s,false,0,NYR_POLY_SCALAR,true,false);
}

bool nyr_poly_emit_recomputed(nyr_func_t *f,const nyr_poly_region_t *a,
                              const nyr_poly_region_t *b,nyr_poly_schedule_t s) {
  return b&&emit_region(f,a,b,NULL,s,false,0,NYR_POLY_SCALAR,true,true);
}

/* A deliberately narrow default: a private dense matrix recurrence with
 * two read-only input streams, one invariant across the middle axis.
 * Avoid blocking unit-stride integer dot products: their single PHI is
 * useful to the downstream vectorizer. FP and strided integer kernels
 * benefit from shared input loads and multiple scalar accumulators. */
static unsigned profitable_promotion(const nyr_func_t *f,const nyr_poly_region_t *r) {
  if (!f->untagged_values||r->access_count!=4||
      r->body_end-r->body_begin>64||r->extent[0]<16||
      r->extent[1]<32||r->extent[2]<32) return 0;
  size_t wr=SIZE_MAX,readers=0;int bases[2]={-1,-1};
  bool contiguous=true,shared=false;
  for (size_t k=0;k<r->access_count;++k) if (r->access[k].write) {
    if (wr!=SIZE_MAX) return 0;
    wr=k;
  }
  if (wr==SIZE_MAX) return 0;
  const nyr_poly_access_t *w=&r->access[wr];
  if (w->offset.coeff[1]!=8||w->offset.coeff[2]||
      w->offset.coeff[0]<(int64_t)r->extent[1]*8) return 0;
  for (size_t k=0;k<r->access_count;++k) {
    const nyr_poly_access_t *a=&r->access[k];
    if (a->offset.base==w->offset.base) continue;
    if (readers==2||a->width!=8||a->offset.coeff[2]<8) return 0;
    bases[readers++]=a->offset.base;
    contiguous&=a->offset.coeff[2]==8;
    shared|=a->offset.coeff[1]==0;
  }
  if (readers!=2||bases[0]==bases[1]||!shared) return 0;
  bool fp=false;
  for (size_t p=r->body_begin;p<r->body_end;++p)
    if (f->data[p].op==NYIR_STORE_I64)
      fp=f->data[p].scalar_class==NYIR_SCALAR_F64;
  if (fp) {
    if (!nyr_fast_math()) return 0;
    if (r->extent[1]>=64) return 4;
    return r->extent[1]%2==0?2:1;
  }
  if (contiguous) return 1;
  return r->extent[1]>=64||(r->extent[1]>=48&&r->extent[2]>=48)?2:0;
}

/* For dense dot products, vectorizing the reduction axis avoids the
 * strided gather of a middle-axis microtile. A second vector accumulator
 * hides the dependency latency if the remainder is cheap. Keep other
 * layouts and very small/variable domains on the previous path. */
static unsigned profitable_horizontal(const nyr_func_t *f,const nyr_poly_region_t *r) {
  if (!f->untagged_values||!nyr_fast_math()||r->access_count!=4||
      r->body_end-r->body_begin>64||r->extent[0]<16||r->extent[1]<16||
      r->extent[2]<16) return 0;
  for (unsigned d=0;d<3;++d)
    if (r->symbolic[d]||r->initial[d]||r->stride[d]!=1) return 0;
  const nyr_poly_access_t *w=NULL;
  for (size_t k=0;k<r->access_count;++k) if (r->access[k].write) {
    if (w) return 0;
    w=&r->access[k];
  }
  if (!w||w->width!=8||w->offset.coeff[2]||w->offset.coeff[1]!=8||
      w->offset.coeff[0]<(int64_t)r->extent[1]*8) return 0;
  int roots[2]={-1,-1};unsigned reads=0;bool row=false,col=false;
  for (size_t k=0;k<r->access_count;++k) {
    const nyr_poly_access_t *a=&r->access[k];
    if (a->offset.base==w->offset.base) continue;
    if (reads==2||a->write||a->width!=8||a->offset.coeff[2]!=8) return 0;
    roots[reads++]=a->offset.base;
    row|=!a->offset.coeff[1]&&a->offset.coeff[0]>=(int64_t)r->extent[2]*8;
    col|=!a->offset.coeff[0]&&a->offset.coeff[1]>=(int64_t)r->extent[2]*8;
  }
  if (reads!=2||roots[0]==roots[1]||!row||!col) return 0;
  /* Check the type before invoking the recurrence planner. */
  for (size_t p=r->body_begin;p<r->body_end;++p)
    if (f->data[p].op==NYIR_STORE_I64&&f->data[p].scalar_class!=NYIR_SCALAR_F64)
      return 0;
  unsigned n=r->extent[2];
  return n>=32&&(n%8<=3||n>=128)?2:1;
}

/* Explicit block-width overrides are a tuning control, not a legality
 * override. The recurrence planner still checks every lane. */
static unsigned requested_block_width(void) {
  const char *value=getenv("NYTRIX_POLY_EXPERIMENTAL_REGISTER_BLOCK");
  if (!value||value[0]<'1'||value[0]>'8'||value[1]) return 1;
  return (unsigned)(value[0]-'0');
}

bool nyr_poly_apply(nyr_func_t *f,const nyr_poly_region_t *r,bool allow_3d,
                     bool allow_simd,bool auto_horizontal,nyr_poly_schedule_t *selected) {
  if (!f||!r||!selected) return false;
  const char *promotion=getenv("NYTRIX_POLY_EXPERIMENTAL_PROMOTION");
  bool forced=promotion&&!strcmp(promotion,"1");
  if (nyr_poly_dims(r)==3&&(!promotion||forced)) {
    unsigned lanes=forced?1:profitable_promotion(f,r);
    if (forced) lanes=requested_block_width();
    /* Source order is legal by construction. The promotion planner proves
     * the stronger local recurrence and disjoint-lane conditions. */
    nyr_poly_schedule_t identity={0};
    const char *simd=getenv("NYTRIX_POLY_EXPERIMENTAL_SIMD");
    const char *horizontal=getenv("NYTRIX_POLY_HORIZONTAL");
    unsigned groups=0;
    if (horizontal) {
      if (!strcmp(horizontal,"1")||!strcmp(horizontal,"2"))
        groups=(unsigned)(horizontal[0]-'0');
    } else if (!forced&&auto_horizontal) groups=profitable_horizontal(f,r);
    if (allow_simd&&auto_horizontal&&groups&&
        emit_region(f,r,NULL,NULL,identity,true,groups,NYR_POLY_VECTOR_REDUCTION,false,false)) {
      *selected=identity;return true;
    }
    allow_simd=allow_simd&&simd&&!strcmp(simd,"1");
    if (lanes&&(((lanes==4||lanes==8)&&allow_simd&&emit_region(f,r,NULL,NULL,identity,true,lanes,NYR_POLY_VECTOR_BLOCK,false,false))||
        emit_region(f,r,NULL,NULL,identity,true,lanes,NYR_POLY_SCALAR,false,false)||
        (lanes>1&&emit_region(f,r,NULL,NULL,identity,true,1,NYR_POLY_SCALAR,false,false)))) {
      *selected=identity;return true;
    }
  }
  bool chosen=nyr_poly_dims(r)==3&&!allow_3d?nyr_poly_choose_stream3(r,selected):
                nyr_poly_choose(r,selected);
  if (chosen&&forced&&nyr_poly_dims(r)==3) {
    unsigned lanes=requested_block_width();
    if (emit_region(f,r,NULL,NULL,*selected,true,lanes,NYR_POLY_SCALAR,false,false)) return true;
  }
  return chosen&&emit_region(f,r,NULL,NULL,*selected,true,0,NYR_POLY_SCALAR,false,false);
}
