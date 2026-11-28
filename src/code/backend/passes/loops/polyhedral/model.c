/* Extract bounded rectangular 2D/3D or triangular 2D SCoPs. Each statement
 * retains its original instruction order. CFG and SSA uses certify the shape;
 * accesses use byte coordinates of stack objects or conditional raw C pointer
 * roots. External roots require an emitted overflow/disjointness guard. */
#include "code/backend/passes/loops/polyhedral/domain.h"
#include "code/backend/passes/util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool checked(ny_i128 v, int64_t *out) {
  if (v < INT64_MIN || v > INT64_MAX)
    return false;
  *out = (int64_t)v;
  return true;
}

/* Only raw C functions opt into speculative pointer versioning. Valid C
 * accesses do not wrap the address space or trap; guard disjoint ranges
 * before exploiting independence between different pointer SSA roots. */
static bool external_root(const nyr_func_t *f,const nyr_inst_t *in) {
  return f->untagged_values&&in->op==NYIR_LOAD_LOCAL&&in->imm>=0&&
         (uint64_t)in->imm<f->param_count&&f->param_types&&
         f->param_types[in->imm]==NYIR_PARAM_I64;
}

static bool affine(const nyr_func_t *f, const int *defs,
                   const nyr_poly_region_t *r, int v, unsigned depth,
                   nyr_poly_affine_t *out) {
  *out = (nyr_poly_affine_t){.base = -1};
  if (v < 0 || v >= f->next_value || defs[v] < 0 || depth > 24)
    return false;
  for (unsigned d = 0; d < nyr_poly_dims(r); ++d)
    if (v == r->iv[d]) {
      out->coeff[d] = r->stride[d];
      out->constant = r->initial[d];
      return true;
    }
  size_t at = (size_t)defs[v];
  const nyr_inst_t *in = &f->data[at];
  if (in->op == NYIR_CONST_I64) {
    out->constant = in->imm;
    return true;
  }
  if (in->op == NYIR_ALLOCA && at < r->begin && in->imm > 0 &&
      !(in->flags & NYIR_INST_F_SCOPE_LOCAL)) {
    out->base = v;
    return true;
  }
  if (r->external&&at<r->begin&&external_root(f,in)) {
    out->base=v;
    return true;
  }
  if (in->op == NYIR_COPY)
    return affine(f, defs, r, in->a, depth + 1, out);
  if (in->flags & NYIR_INST_F_NARROW32) return false;
  if (in->op != NYIR_ADD_I64 && in->op != NYIR_SUB_I64 &&
      in->op != NYIR_MUL_I64 && in->op != NYIR_SHL_I64)
    return false;
  nyr_poly_affine_t a, b;
  if (!affine(f, defs, r, in->a, depth + 1, &a) ||
      !affine(f, defs, r, in->b, depth + 1, &b))
    return false;
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    int sign = in->op == NYIR_ADD_I64 ? 1 : -1;
    if ((a.base >= 0 && b.base >= 0) || (b.base >= 0 && sign < 0))
      return false;
    out->base = a.base >= 0 ? a.base : b.base;
    for (unsigned d = 0; d < nyr_poly_dims(r); ++d)
      if (!checked((ny_i128)a.coeff[d] + (ny_i128)sign * b.coeff[d],
                   &out->coeff[d]))
        return false;
    return checked((ny_i128)a.constant + (ny_i128)sign * b.constant,
                   &out->constant);
  }
  if (a.base >= 0 || b.base >= 0)
    return false;
  if (in->op == NYIR_MUL_I64 && !a.coeff[0] && !a.coeff[1] && !a.coeff[2]) {
    nyr_poly_affine_t tmp = a; a = b; b = tmp;
  }
  if (b.coeff[0] || b.coeff[1] || b.coeff[2])
    return false;
  int64_t scale = b.constant;
  if (in->op == NYIR_SHL_I64) {
    if (scale < 0 || scale > 62)
      return false;
    scale = INT64_C(1) << scale;
  }
  for (unsigned d = 0; d < nyr_poly_dims(r); ++d)
    if (!checked((ny_i128)a.coeff[d] * scale, &out->coeff[d]))
      return false;
  return checked((ny_i128)a.constant * scale, &out->constant);
}

/* Modular ADD/SUB/MUL/SHL address expressions are equal modulo 2^64 to
 * base + affine offset. The final mathematical offset must fit within the
 * live allocation. Natural alignment also excludes unaligned traps. */
static bool access_model(const nyr_func_t *f, const int *defs,
                         nyr_poly_region_t *r, const nyr_inst_t *in) {
  if (r->access_count == NYR_POLY_MAX_ACCESS ||
      (in->flags & (NYIR_INST_F_MEM_VOLATILE | NYIR_INST_F_MEM_SEG_FS |
                    NYIR_INST_F_MEM_SEG_GS)))
    return false;
  unsigned allowed = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                     NYIR_EFFECT_MAY_TRAP;
  if (nyr_effective_effects(in) & ~allowed)
    return false;
  nyr_poly_access_t a = {.is_write = in->op == NYIR_STORE_I64,
                         .width = (in->flags & NYIR_INST_F_MEM_BYTE) ? 1u :
                          (in->flags & NYIR_INST_F_MEM_U16) ? 2u :
                          (in->flags & NYIR_INST_F_MEM_I32) ? 4u : 8u};
  if (!affine(f, defs, r, in->a, 0, &a.offset) || a.offset.base < 0)
    return false;
  const nyr_inst_t *base = &f->data[defs[a.offset.base]];
  ny_i128 lo,hi;
  if (a.offset.constant % a.width) return false;
  for (unsigned d=0;d<nyr_poly_dims(r);++d)
    if (a.offset.coeff[d] % (int64_t)a.width) return false;
  if (!nyr_poly_access_extrema(r,&a,&lo,&hi)||lo<0||hi>=INT64_MAX) return false;
  if (base->op==NYIR_ALLOCA) {
    if (hi>=base->imm) return false;
  } else if (!r->external||!external_root(f,base)) return false;
  r->access[r->access_count++] = a;
  return true;
}

static bool pure(const nyr_inst_t *in) {
  if (in->symbol || in->extra_args || in->arg_sizes || in->phi_incoming ||
      nyr_effective_effects(in) || (in->flags & NYIR_INST_F_ASSUME))
    return false;
  switch (in->op) {
  case NYIR_NOP: case NYIR_CONST_I64: case NYIR_COPY:
  case NYIR_ADD_I64: case NYIR_SUB_I64: case NYIR_MUL_I64:
  case NYIR_AND_I64: case NYIR_OR_I64: case NYIR_XOR_I64:
  case NYIR_SHL_I64: case NYIR_SAR_I64: case NYIR_CMP_I64:
  case NYIR_SELECT_I64:
    return true;
  case NYIR_CONST_F32: case NYIR_CONST_F64:
    return true;
  case NYIR_I64_TO_F32: case NYIR_I64_TO_F64:
  case NYIR_ADD_F32: case NYIR_SUB_F32: case NYIR_MUL_F32:
  case NYIR_ADD_F64: case NYIR_SUB_F64: case NYIR_MUL_F64:
    /* Reordering independent FP statements requires explicit fast-math. */
    return nyr_fast_math();
  default:
    return false;
  }
}

/* Consume non-NOP instructions in the canonical header/body/latch layout.
 * The CFG checks below additionally reject any hidden incoming or outgoing
 * edge, including edges into the middle of the region. */
static const nyr_inst_t *take(const nyr_func_t *f, size_t *p, nyr_op_t op) {
  while (*p < f->len && f->data[*p].op == NYIR_NOP) ++*p;
  if (*p >= f->len || f->data[*p].op != op) return NULL;
  return &f->data[(*p)++];
}

/* Normalize the original integer recurrence to a zero-based ordinal. The
 * failing exit test must also be representable: a wrapping IV is not affine. */
static bool recurrence(const nyr_func_t *f, const int *defs, int iv, int next,
                       int64_t *step) {
  if (next < 0 || next >= f->next_value || defs[next] < 0) return false;
  const nyr_inst_t *in = &f->data[defs[next]];
  int64_t c;
  if (in->op == NYIR_ADD_I64 && in->a == iv &&
      nyr_const_i64(f,defs,in->b,&c)) {
    *step = c;
  } else if (in->op == NYIR_SUB_I64 && in->a == iv &&
             nyr_const_i64(f,defs,in->b,&c) && c != INT64_MIN) {
    *step = -c;
  } else return false;
  return *step && pure(in) && !(in->flags & NYIR_INST_F_NARROW32);
}

static bool trip_count(int64_t start, int64_t limit, int64_t step,
                       nyr_cmp_t cmp, unsigned *extent) {
  ny_i128 distance, mag;
  bool inclusive;
  if (step > 0 && (cmp == NYIR_CMP_LT || cmp == NYIR_CMP_LE)) {
    distance = (ny_i128)limit - start; mag = step;
    inclusive = cmp == NYIR_CMP_LE;
  } else if (step < 0 && (cmp == NYIR_CMP_GT || cmp == NYIR_CMP_GE)) {
    distance = (ny_i128)start - limit; mag = -(ny_i128)step;
    inclusive = cmp == NYIR_CMP_GE;
  } else return false;
  if (distance < 0 || (!distance && !inclusive)) return false;
  ny_i128 n = inclusive ? distance / mag + 1 : (distance + mag - 1) / mag;
  int64_t exit;
  if (n < 2 || n > NYR_POLY_MAX_EXTENT ||
      !checked((ny_i128)start + n * step, &exit)) return false;
  *extent = (unsigned)n;
  return true;
}

/* A symbolic unit-step bound is usable only when it dominates the entire
 * region and shared range analysis proves a finite nonnegative envelope.
 * Dependence analysis uses that envelope; regeneration uses the original
 * SSA bound, so no runtime iteration is invented. */
static bool symbolic_bound(const nyr_func_t *f, const int *defs,
                           const nyr_cfg_t *cfg, nyr_value_fact_t **facts,
                           nyr_poly_region_t *r, unsigned d, int value, int64_t init,
                           int64_t step, nyr_cmp_t cmp) {
  if (value<0 || value>=f->next_value || defs[value]<0 ||
      (size_t)defs[value]>=r->begin || f->next_value>32768 ||
      !nyr_cfg_dominates(cfg,cfg->inst_block[defs[value]],
                         cfg->inst_block[r->begin])) return false;
  if (!*facts) {
    *facts=calloc((size_t)f->next_value,sizeof(**facts));
    if (!*facts) return false;
    if (!nyr_analyze_values(f,*facts,(size_t)f->next_value,NULL,0)) {
      free(*facts);*facts=NULL;return false;
    }
  }
  nyr_range_t range={0};
  if (!nir_value_range_at_with_cfg(f,*facts,cfg,defs,value,r->begin,&range) ||
      !range.has_min || !range.has_max || range.min>range.max ||
      !step || step==INT64_MIN) return false;
  bool up=step>0, inclusive=cmp==NYIR_CMP_LE||cmp==NYIR_CMP_GE;
  if (up?(cmp!=NYIR_CMP_LT&&cmp!=NYIR_CMP_LE):
         (cmp!=NYIR_CMP_GT&&cmp!=NYIR_CMP_GE)) return false;
  ny_i128 mag=up?step:-step;
  ny_i128 distances[2]={up?(ny_i128)range.min-init:(ny_i128)init-range.max,
                        up?(ny_i128)range.max-init:(ny_i128)init-range.min};
  unsigned count[2];
  for (unsigned i=0;i<2;++i) {
    ny_i128 numerator=distances[i]+mag-(inclusive?0:1);
    int64_t tmp;
    if (!checked(distances[i],&tmp)||!checked(numerator,&tmp)) return false;
    ny_i128 n=numerator>0?numerator/mag:0;
    if (n>NYR_POLY_MAX_EXTENT||!checked((ny_i128)init+n*step,&tmp)) return false;
    count[i]=(unsigned)n;
  }
  if (count[1]<2) return false;
  r->symbolic[d]=true;r->bound_value[d]=value;r->inclusive[d]=inclusive;
  r->extent[d]=count[1];r->minimum_extent[d]=count[0];
  return true;
}

/* Literal loop limits may be materialized immediately after the header
 * PHI rather than in the preheader. They have no effects; the later
 * use-def check rejects escaping constants and regeneration recreates
 * any literal also used by the retained body. */
static void header_constants(const nyr_func_t *f,size_t *p) {
  while (*p<f->len&&(f->data[*p].op==NYIR_NOP||
         (f->data[*p].op==NYIR_CONST_I64&&pure(&f->data[*p])))) ++*p;
}

static bool header(const nyr_func_t *f, const int *defs, const nyr_cfg_t *cfg,
                   nyr_value_fact_t **facts, size_t *p,
                   nyr_poly_region_t *r, unsigned d, int64_t *label,
                   int64_t *body, int64_t *exit, int *next,
                   int64_t *back, int64_t *entry) {
  const nyr_inst_t *h = take(f,p,NYIR_LABEL), *phi = take(f,p,NYIR_PHI);
  header_constants(f,p);
  const nyr_inst_t *cmp = take(f,p,NYIR_CMP_I64), *br = take(f,p,NYIR_BR_IF),
                   *out = take(f,p,NYIR_BR);
  if (!h || !phi || !cmp || !br || !out || phi->phi_incoming_len != 2 ||
      !phi->phi_incoming || cmp->a != phi->dst ||
      (cmp->flags & NYIR_INST_F_ASSUME) || br->a != cmp->dst)
    return false;
  unsigned k,upper=0;
  int64_t init=0, step;
  for (k=0;k<2;++k) {
    if (!recurrence(f,defs,phi->dst,phi->phi_incoming[1-k].value,&step)) continue;
    int v=phi->phi_incoming[k].value;
    if (nyr_const_i64(f,defs,v,&init)) break;
    if (d!=1||r->initial[0]||r->stride[0]!=1||step!=1||
        (cmp->cmp!=NYIR_CMP_LT&&cmp->cmp!=NYIR_CMP_LE)) continue;
    v=nyr_strip_copy_chain(f,defs,v,16);
    if (v==r->iv[0]) upper=4;
    else if (v>=0&&v<f->next_value&&defs[v]>=0) {
      const nyr_inst_t *in=&f->data[defs[v]];
      int64_t c;
      if (in->op==NYIR_ADD_I64&&pure(in)&&!(in->flags&NYIR_INST_F_NARROW32)&&
          ((in->a==r->iv[0]&&nyr_const_i64(f,defs,in->b,&c)&&c==1)||
           (in->b==r->iv[0]&&nyr_const_i64(f,defs,in->a,&c)&&c==1))) upper=3;
    }
    if (upper) {init=0;break;}
  }
  if (k==2) return false;
  int64_t limit;
  if (!upper && d == 1 && cmp->b == r->iv[0] && init == 0 && step == 1 &&
      r->initial[0] == 0 && r->stride[0] == 1 &&
      (cmp->cmp == NYIR_CMP_LT || cmp->cmp == NYIR_CMP_LE)) {
    r->extent[d] = r->extent[0];
    r->triangle = cmp->cmp == NYIR_CMP_LT ? 1u : 2u;
    if (r->symbolic[0]) {
      r->symbolic[d]=true;r->bound_value[d]=r->bound_value[0];
      r->minimum_extent[d]=r->minimum_extent[0];
      r->inclusive[d]=r->inclusive[0];
    }
  } else if (nyr_const_i64(f,defs,cmp->b,&limit)) {
    if (!trip_count(init,limit,step,cmp->cmp,&r->extent[d])) return false;
  } else if (!symbolic_bound(f,defs,cfg,facts,r,d,cmp->b,init,step,cmp->cmp)) return false;
  if (upper) {
    /* The two axes share an identical ordinal envelope at runtime. */
    if (r->extent[d]!=r->extent[0]||r->symbolic[d]!=r->symbolic[0]||
        (r->symbolic[d]&&(r->bound_value[d]!=r->bound_value[0]||
                        r->inclusive[d]!=r->inclusive[0]))) return false;
    r->triangle=upper;
  }
  if (!r->symbolic[d]) r->minimum_extent[d]=r->extent[d];
  r->iv[d]=phi->dst; r->initial[d]=init; r->stride[d]=step;
  *label=h->imm; *body=br->imm; *exit=out->imm;
  *next=phi->phi_incoming[1-k].value;
  *back=phi->phi_incoming[1-k].predecessor_label;
  *entry=phi->phi_incoming[k].predecessor_label;
  return true;
}

static bool increment(const nyr_func_t *f, const int *defs, size_t at,
                      int iv, int next) {
  int64_t step;
  return f->data[at].dst == next && recurrence(f,defs,iv,next,&step);
}

static bool header_shape(const nyr_func_t *f, size_t *p);
/* Small pure preheader for an affine inner start such as i+1. The region's
 * use-def check still forbids erasing any value needed by the retained body. */
static bool nested_header(const nyr_func_t *f,size_t p,size_t *at,
                          const nyr_inst_t **go) {
  for (unsigned k=0;k<8&&p<f->len;++k) {
    const nyr_inst_t *in=&f->data[p];
    if (in->op!=NYIR_NOP&&in->op!=NYIR_CONST_I64&&in->op!=NYIR_COPY&&
        in->op!=NYIR_ADD_I64) break;
    if (!pure(in)) return false;
    ++p;
  }
  *go=take(f,&p,NYIR_BR);
  *at=p;
  return header_shape(f,&p);
}

static bool match(const nyr_func_t *f, const int *defs, const nyr_cfg_t *cfg,
                  const nyr_use_def_t *uses, nyr_value_fact_t **facts, size_t at, nyr_poly_region_t *r) {
  const char *version=getenv("NYTRIX_POLY_EXPERIMENTAL_ALIAS_VERSIONING");
  bool allow_external=f->untagged_values&&version&&!strcmp(version,"1");
  *r = (nyr_poly_region_t){.begin=at,.external=allow_external};
  size_t p=at;
  int64_t h[NYR_POLY_MAX_DIMS], b[NYR_POLY_MAX_DIMS], e[NYR_POLY_MAX_DIMS],
          back[NYR_POLY_MAX_DIMS], entry[NYR_POLY_MAX_DIMS];
  int next[NYR_POLY_MAX_DIMS];
  const nyr_inst_t *go=NULL;
  for (unsigned d=0;d<NYR_POLY_MAX_DIMS;++d) {
    if (!header(f,defs,cfg,facts,&p,r,d,&h[d],&b[d],&e[d],&next[d],&back[d],&entry[d]))
      return false;
    if (d && ((go && go->imm!=h[d]) || entry[d]!=b[d-1] || back[d-1]!=e[d]))
      return false;
    const nyr_inst_t *body=take(f,&p,NYIR_LABEL);
    if (!body || body->imm!=b[d]) return false;
    r->depth=d+1;
    size_t next_header;
    if (d+1==NYR_POLY_MAX_DIMS||!nested_header(f,p,&next_header,&go)) break;
    p=next_header;
  }
  unsigned dims=nyr_poly_dims(r), inner=dims-1;
  if (dims<2 || (dims==3 && r->triangle) || back[inner]!=b[inner]) return false;
  r->body_begin=p;
  while (p < f->len && f->data[p].op != NYIR_BR) {
    const nyr_inst_t *in=&f->data[p++];
    if (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) {
      if (!access_model(f,defs,r,in)) return false;
    } else if (!pure(in)) return false;
  }
  if (p == r->body_begin || !r->access_count) return false;
  size_t last=p;
  while (last > r->body_begin && f->data[last-1].op == NYIR_NOP) --last;
  if (last == r->body_begin || !increment(f,defs,last-1,r->iv[inner],next[inner]))
    return false;
  r->body_end=last-1;
  /* Consume the closing latch of every enclosing loop, innermost first. */
  for (unsigned d=dims;d-- > 0;) {
    const nyr_inst_t *endloop=take(f,&p,NYIR_BR);
    if (!endloop || endloop->imm!=h[d]) return false;
    if (!d) break;
    const nyr_inst_t *latch=take(f,&p,NYIR_LABEL);
    if (!latch || latch->imm!=e[d]) return false;
    while (p < f->len && (f->data[p].op==NYIR_NOP || f->data[p].op==NYIR_CONST_I64)) {
      if (!pure(&f->data[p++])) return false;
    }
    if (p>=f->len || !increment(f,defs,p++,r->iv[d-1],next[d-1])) return false;
  }
  while (p < f->len && f->data[p].op==NYIR_NOP) ++p;
  if (p>=f->len || f->data[p].op!=NYIR_LABEL || f->data[p].imm!=e[0]) return false;
  r->end=p; r->entry=h[0]; r->exit=e[0];
  bool external=false, local=false;
  unsigned roots=0;
  for (size_t k=0;k<r->access_count;++k) {
    int root=r->access[k].offset.base;
    const nyr_inst_t *base=&f->data[defs[root]];
    external|=base->op!=NYIR_ALLOCA;
    local|=base->op==NYIR_ALLOCA;
    if (!nyr_cfg_dominates(cfg,cfg->inst_block[defs[root]],cfg->inst_block[at])) return false;
    bool seen=false;
    for (size_t j=0;j<k;++j) seen|=r->access[j].offset.base==root;
    roots+=!seen;
  }
  /* External regions use the maximum proven domain for address envelopes.
   * This may reject otherwise disjoint runtime footprints but never assumes
   * more independence than the actual domain has. Bound cloned IR size and
   * keep mixed-provenance regions unsupported. */
  r->external=external;
  if (external&&(local||roots>4||r->end-r->begin>128)) return false;
  /* Removed values must not escape, and only the two IVs (not the original
   * guard or latch values) may be consumed by the regenerated body. */
  for (size_t i=at;i<p;++i) {
    const nyr_inst_t *in=&f->data[i];
    if (in->op == NYIR_LABEL && (in->flags & NYIR_INST_F_LABEL_ADDRESS_TAKEN))
      return false;
    int v=in->dst;
    if (v < 0) continue;
    if ((size_t)v >= uses->value_count) return false;
    bool preserved = (i >= r->body_begin && i < r->body_end) ||
                     (in->op==NYIR_CONST_I64&&pure(in));
    for (unsigned d=0;d<dims;++d) preserved |= v==r->iv[d];
    for (size_t u=uses->offsets[v];u<uses->offsets[v+1];++u) {
      size_t user=uses->users[u];
      if (user < at || user >= p || (!preserved && user >= r->body_begin &&
                                   user < r->body_end)) return false;
    }
  }
  /* Single entry and exit; no externally visible PHIs on the exit edge
   * (its predecessor will change). */
  size_t first=cfg->inst_block[at], end=cfg->inst_block[p];
  if (first >= end) return false;
  for (size_t block=first;block<end;++block) {
    for (size_t k=cfg->pred_offsets[block];k<cfg->pred_offsets[block+1];++k) {
      size_t pred=cfg->pred_blocks[k];
      if ((pred<first || pred>=end) &&
          (block!=first || cfg->block_label[pred]!=entry[0])) return false;
    }
    for (size_t k=cfg->succ_offsets[block];k<cfg->succ_offsets[block+1];++k) {
      size_t succ=cfg->succ_blocks[k];
      if ((succ<first || succ>=end) && succ!=end) return false;
    }
  }
  for (size_t i=p;i<cfg->block_end[end];++i)
    if (f->data[i].op==NYIR_PHI) return false;
  return true;
}

/* Cheap structural prefilter: do not allocate CFG/dominance/use-def tables
 * for ordinary single loops or functions with no canonical nested header. */
static bool header_shape(const nyr_func_t *f, size_t *p) {
  if (!take(f,p,NYIR_LABEL)||!take(f,p,NYIR_PHI)) return false;
  header_constants(f,p);
  return take(f,p,NYIR_CMP_I64) && take(f,p,NYIR_BR_IF) &&
         take(f,p,NYIR_BR);
}
static bool candidate_shape(const nyr_func_t *f, size_t p) {
  if (!header_shape(f,&p) || !take(f,&p,NYIR_LABEL)) return false;
  size_t next_header;const nyr_inst_t *go;
  return nested_header(f,p,&next_header,&go);
}

struct nyr_poly_scan {
  const nyr_func_t *f;
  size_t next;
  nyr_cfg_t cfg;
  nyr_use_def_t uses;
  int *defs;
  nyr_value_fact_t *facts;
};

void nyr_poly_scan_close(nyr_poly_scan_t *scan) {
  if (!scan) return;
  free(scan->facts);
  free(scan->defs);
  nyr_cfg_free(&scan->cfg);
  nyr_use_def_free(&scan->uses);
  free(scan);
}

nyr_poly_scan_t *nyr_poly_scan_open(const nyr_func_t *f,size_t start) {
  if (!f||f->len<16||f->len>32768) return NULL;
  while (start<f->len&&(f->data[start].op!=NYIR_LABEL||!candidate_shape(f,start))) ++start;
  if (start>=f->len) return NULL;
  nyr_poly_scan_t *scan=calloc(1,sizeof(*scan));
  if (!scan) return NULL;
  scan->f=f;
  scan->next=start;
  scan->defs=nyr_build_defs(f);
  if (!scan->defs||!nyr_cfg_build(f,&scan->cfg)||!nyr_build_use_def(f,&scan->uses)) {
    nyr_poly_scan_close(scan);
    return NULL;
  }
  return scan;
}

bool nyr_poly_scan_next(nyr_poly_scan_t *scan,nyr_poly_region_t *out) {
  if (!scan||!out) return false;
  const nyr_func_t *f=scan->f;
  while (scan->next<f->len) {
    size_t p=scan->next++;
    if (f->data[p].op==NYIR_LABEL&&candidate_shape(f,p)&&
        match(f,scan->defs,&scan->cfg,&scan->uses,&scan->facts,p,out)) {
      scan->next=out->end;
      return true;
    }
  }
  return false;
}

bool nyr_poly_scan_adjacent(nyr_poly_scan_t *scan,const nyr_poly_region_t *out,
                             nyr_poly_region_t *second) {
  if (!scan||!out||!second||out->end>=scan->f->len) return false;
  const nyr_func_t *f=scan->f;
  /* Look only across the constant/branch bridge. Preserve the scan cursor
   * and reuse the existing CFG, definitions, use-def and value facts. */
  size_t q=out->end+1;
  while (q<f->len&&(f->data[q].op==NYIR_NOP||f->data[q].op==NYIR_CONST_I64)) ++q;
  if (q+1<f->len&&f->data[q].op==NYIR_BR&&f->data[q+1].op==NYIR_LABEL&&
      f->data[q+1].imm==f->data[q].imm) ++q;
  *second=(nyr_poly_region_t){0};
  if (q<f->len&&f->data[q].op==NYIR_LABEL&&candidate_shape(f,q)) {
    nyr_poly_region_t b;
    if (match(f,scan->defs,&scan->cfg,&scan->uses,&scan->facts,q,&b)) {
      *second=b;return true;
    }
  }
  return false;
}

bool nyr_poly_detect_pair(const nyr_func_t *f,size_t start,nyr_poly_region_t *out,
                           nyr_poly_region_t *second) {
  if (second) *second=(nyr_poly_region_t){0};
  nyr_poly_scan_t *scan=nyr_poly_scan_open(f,start);
  if (!scan) return false;
  bool found=nyr_poly_scan_next(scan,out);
  if (found&&second) (void)nyr_poly_scan_adjacent(scan,out,second);
  nyr_poly_scan_close(scan);
  return found;
}

bool nyr_poly_detect(const nyr_func_t *f,size_t start,nyr_poly_region_t *out) {
  return nyr_poly_detect_pair(f,start,out,NULL);
}

/* A bridge may only contain the first exit label, constants and a branch
 * into the second header. Its constants can move before the combined nest.
 * No alternative control path may enter it. */
bool nyr_poly_adjacent(const nyr_func_t *f,const nyr_poly_region_t *a,
                        const nyr_poly_region_t *b) {
  if (!f||!a||!b||a->end>=b->begin||b->begin>=f->len||b->end>f->len||
      f->data[a->end].op!=NYIR_LABEL||f->data[a->end].imm!=a->exit||
      (f->data[a->end].flags&NYIR_INST_F_LABEL_ADDRESS_TAKEN)) return false;
  size_t p=a->end+1;
  while (p<b->begin&&(f->data[p].op==NYIR_NOP||f->data[p].op==NYIR_CONST_I64)) {
    if (!pure(&f->data[p++])) return false;
  }
  if (p!=b->begin && (p+1!=b->begin||f->data[p].op!=NYIR_BR||f->data[p].imm!=b->entry)) return false;
  nyr_cfg_t cfg={0};
  if (!nyr_cfg_build_topology(f,&cfg)) return false;
  size_t bridge=cfg.inst_block[a->end],first=cfg.inst_block[a->begin],second=cfg.inst_block[b->begin];
  bool ok=bridge<cfg.block_count&&first<bridge&&second>bridge&&second<cfg.block_count&&
    cfg.pred_offsets[bridge+1]>cfg.pred_offsets[bridge]&&
    cfg.succ_offsets[bridge+1]-cfg.succ_offsets[bridge]==1&&
    cfg.succ_blocks[cfg.succ_offsets[bridge]]==second;
  for (size_t k=ok?cfg.pred_offsets[bridge]:0;ok&&k<cfg.pred_offsets[bridge+1];++k)
    ok=cfg.pred_blocks[k]>=first&&cfg.pred_blocks[k]<bridge;
  nyr_cfg_free(&cfg);return ok;
}
