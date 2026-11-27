/* Finite integer-set dependence engine. Enumerate the bounded domain once,
 * group accesses by object/byte, then prove lexicographic order preservation
 * for RAW, WAR and WAW. Keeping the last write and maximum scheduled read
 * since it is sufficient: all other conflict orders follow transitively.
 * No pairwise quadratic search, and no approximate alias test. */
#include "code/backend/passes/loops/polyhedral/domain.h"
#include "base/compat.h"
#include <stdlib.h>
#include <limits.h>

typedef struct {
  uint64_t byte;
  unsigned base, instance;
  bool write;
} event_t;

typedef struct { event_t *data; size_t count; } events_t;

static int cmp_event(const void *pa, const void *pb) {
  const event_t *a=pa,*b=pb;
  if (a->base != b->base) return a->base < b->base ? -1 : 1;
  if (a->byte != b->byte) return a->byte < b->byte ? -1 : 1;
  return (a->instance > b->instance) - (a->instance < b->instance);
}

static uint64_t offset3(const nyr_poly_access_t *a, unsigned i, unsigned j,
                        unsigned k) {
  return (uint64_t)((ny_i128)a->offset.constant +
         (ny_i128)a->offset.coeff[0]*i + (ny_i128)a->offset.coeff[1]*j +
         (ny_i128)a->offset.coeff[2]*k);
}
static unsigned extent3(const nyr_poly_region_t *r) {
  return nyr_poly_dims(r)==3 ? r->extent[2] : 1;
}
static uint64_t points(const nyr_poly_region_t *r) {
  return (uint64_t)r->extent[0]*r->extent[1]*extent3(r);
}

static bool events_build(const nyr_poly_region_t *r, events_t *out) {
  *out=(events_t){0};
  if (!r || !r->extent[0] || !r->extent[1] || !r->access_count ||
      r->access_count > NYR_POLY_MAX_ACCESS) return false;
  uint64_t count=points(r);
  if (count > NYR_POLY_MAX_POINTS) return false;
  unsigned bytes=0;
  for (size_t a=0;a<r->access_count;++a) {
    if (!r->access[a].width || r->access[a].width>8 || r->access[a].offset.base<0)
      return false;
    bytes+=r->access[a].width;
  }
  if (count*bytes > NYR_POLY_MAX_BYTES) return false;
  out->data=malloc((size_t)(count*bytes)*sizeof(*out->data));
  if (!out->data) return false;
  unsigned instance=0;
  for (unsigned i=0;i<r->extent[0];++i)
    for (unsigned j=0;j<r->extent[1];++j)
      for (unsigned z=0;z<extent3(r);++z)
      for (size_t k=0;k<r->access_count;++k,++instance) {
        if (!nyr_poly_contains(r, i, j)) continue;
        const nyr_poly_access_t *a=&r->access[k];
        uint64_t at=offset3(a,i,j,z);
        for (unsigned b=0;b<a->width;++b)
          out->data[out->count++]=(event_t){at+b,(unsigned)a->offset.base,
                                           instance,a->write};
      }
  qsort(out->data,out->count,sizeof(*out->data),cmp_event);
  return true;
}

/* Ranking into a padded rectangular schedule is injective even for partial
 * boundary tiles. Unused ranks are harmless. */
static uint64_t rank_point(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                           unsigned i, unsigned j) {
  if (s.wavefront) {
    unsigned a=s.interchange?j:i,b=s.interchange?i:j;
    return ((uint64_t)nyr_poly_skew(s)*a+b)*r->extent[s.interchange?1:0]+a;
  }
  unsigned a=s.interchange?j:i, b=s.interchange?i:j;
  unsigned nb=r->extent[s.interchange?0:1];
  if (!s.tile) return (uint64_t)a*nb+b;
  unsigned ta=s.tile, tb=s.inner_tile?s.inner_tile:ta;
  unsigned tiles=(nb+tb-1)/tb;
  return (((uint64_t)(a/ta)*tiles+b/tb)*ta+a%ta)*tb+b%tb;
}

static uint64_t rank3(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                      unsigned i,unsigned j,unsigned k) {
  unsigned p[3]={i,j,k}; uint64_t rank=0;
  for (unsigned t=0;t<3;++t) {
    unsigned d=nyr_poly_axis(s,3,t);
    rank=rank*(s.tile?(r->extent[d]+s.tile-1)/s.tile:r->extent[d])+
         (s.tile?p[d]/s.tile:p[d]);
  }
  if (s.tile)
    for (unsigned t=0;t<3;++t)
      rank=rank*s.tile+p[nyr_poly_axis(s,3,t)]%s.tile;
  return rank;
}

static bool valid_schedule(nyr_poly_schedule_t s) { return s.tile<=256 && s.inner_tile<=256 && (s.tile || !s.inner_tile); }
static bool valid_schedule_for(const nyr_poly_region_t *r, nyr_poly_schedule_t s) {
  if (s.wavefront) return s.skew<=8 && nyr_poly_dims(r)==2 && !s.permutation && !s.tile && !s.inner_tile;
  return !s.skew && valid_schedule(s) && (nyr_poly_dims(r)==3 ?
    !s.interchange && !s.inner_tile && s.permutation<6 : !s.permutation);
}

static bool legal(const nyr_poly_region_t *r, const events_t *ev,
                  nyr_poly_schedule_t s) {
  size_t n=(size_t)points(r)*r->access_count;
  uint64_t *rank=malloc(n*sizeof(*rank));
  if (!rank) return false;
  for (unsigned i=0;i<r->extent[0];++i)
    for (unsigned j=0;j<r->extent[1];++j)
      for (unsigned k=0;k<extent3(r);++k) {
      uint64_t p=(nyr_poly_dims(r)==3?rank3(r,s,i,j,k):rank_point(r,s,i,j))*r->access_count;
      size_t old=(((size_t)i*r->extent[1]+j)*extent3(r)+k)*r->access_count;
      for (size_t a=0;a<r->access_count;++a) rank[old+a]=p+a+1;
    }
  bool ok=true; uint64_t write=0, read=0;
  for (size_t k=0;k<ev->count;++k) {
    const event_t *e=&ev->data[k];
    if (!k || e->base!=ev->data[k-1].base || e->byte!=ev->data[k-1].byte)
      write=read=0;
    uint64_t now=rank[e->instance];
    if (now<=write || (e->write && now<=read)) {ok=false;break;}
    if (e->write) {write=now;read=0;}
    else if (now>read) read=now;
  }
  free(rank);
  return ok;
}

/* Cheap sufficient proof for large rectangular domains. If every potentially
 * conflicting pair either occupies separate objects/ranges or has the same
 * injective affine address at each iteration, no inter-iteration dependence
 * exists. The proof is conservative; all unresolved pairs go to the exact
 * finite-domain engine. For triangular sets use their actual affine extrema. */
static bool bounds(const nyr_poly_region_t *r, const nyr_poly_access_t *a,
                   ny_i128 *lo, ny_i128 *hi) {
  if (a->offset.base < 0 || !a->width || a->width > 8) return false;
  return nyr_poly_access_extrema(r,a,lo,hi)&&*lo>=0&&*hi<=INT64_MAX;
}

static bool valid_region(const nyr_poly_region_t *r) {
  if (!r || nyr_poly_dims(r)<2 || nyr_poly_dims(r)>3 || r->triangle>4 ||
      (r->triangle && (nyr_poly_dims(r)!=2 || r->extent[0]!=r->extent[1])) ||
      !r->access_count || r->access_count>NYR_POLY_MAX_ACCESS) return false;
  for (unsigned d=0;d<nyr_poly_dims(r);++d)
    if (!r->extent[d] || r->extent[d]>NYR_POLY_MAX_EXTENT) return false;
  for (size_t k=0;k<r->access_count;++k) {
    ny_i128 lo,hi;
    if (!bounds(r,&r->access[k],&lo,&hi)) return false;
  }
  return true;
}

static bool injective(const nyr_poly_region_t *r,
                      const nyr_poly_access_t *a, unsigned width) {
  unsigned dims=nyr_poly_dims(r),axis[NYR_POLY_MAX_DIMS]={0,1,2};
  ny_i128 stride[NYR_POLY_MAX_DIMS];
  for (unsigned d=0;d<dims;++d) {
    stride[d]=a->offset.coeff[d];
    if (stride[d]<0) stride[d]=-stride[d];
  }
  for (unsigned i=0;i<dims;++i) for (unsigned j=i+1;j<dims;++j)
    if (stride[axis[j]]<stride[axis[i]]) {
      unsigned t=axis[i];axis[i]=axis[j];axis[j]=t;
    }
  ny_i128 span=width;
  for (unsigned i=0;i<dims;++i) {
    unsigned d=axis[i];
    if (stride[d]<span) return false;
    span+=stride[d]*(r->extent[d]-1);
  }
  return true;
}

static bool independent(const nyr_poly_region_t *r) {
  for (size_t i = 0; i < r->access_count; ++i)
    for (size_t j = i; j < r->access_count; ++j) {
      const nyr_poly_access_t *a = &r->access[i], *b = &r->access[j];
      if ((!a->write && !b->write) || a->offset.base != b->offset.base)
        continue;
      ny_i128 al, ah, bl, bh;
      if (!bounds(r,a,&al,&ah) || !bounds(r,b,&bl,&bh)) return false;
      if (ah < bl || bh < al) continue;
      if (a->offset.constant != b->offset.constant ||
          memcmp(a->offset.coeff,b->offset.coeff,nyr_poly_dims(r)*sizeof(a->offset.coeff[0])) ||
          !injective(r,a,a->width > b->width ? a->width : b->width))
        return false;
    }
  return true;
}

/* A bounded symbolic conflict proof for large domains. A common affine
 * layout gives a Diophantine equation in the iteration distance, independent
 * of the number of statement instances. For different layouts we can still
 * prove disjoint byte residues using a GCD test. Any unanswered query is
 * conservative: use the finite engine when possible, otherwise reject. */
static uint64_t magnitude(int64_t x) {
  return x < 0 ? UINT64_C(0) - (uint64_t)x : (uint64_t)x;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
  while (b) { uint64_t t=a%b; a=b; b=t; }
  return a;
}

static bool ordered_distance(nyr_poly_schedule_t s, int64_t di, int64_t dj) {
  if (!di && !dj) return true;
  /* Orient in the original lexicographic order. */
  if (di < 0 || (!di && dj < 0)) { di=-di; dj=-dj; }
  int64_t a=s.interchange?dj:di, b=s.interchange?di:dj;
  if (a < 0 || (!a && b < 0)) return false;
  /* A mixed-sign distance can cross a reversed inner tile boundary. If
   * the leading dimension crosses at least one tile, it always dominates. */
  return !s.tile || b >= 0 || a >= (int64_t)s.tile;
}

/* If accesses use the same row (or column) stride and their combined
 * footprint within that slice is narrower than the stride, every conflict
 * has identical coordinates along that axis. Every supported schedule is
 * monotone along the remaining axis, even for different affine layouts. */
static bool slice_ordered(const nyr_poly_region_t *r,
                          const nyr_poly_access_t *a,
                          const nyr_poly_access_t *b) {
  for (unsigned d=0;d<2;++d) {
    int64_t stride=a->offset.coeff[d];
    if (!stride || stride!=b->offset.coeff[d]) continue;
    ny_i128 lo[2], hi[2];
    const nyr_poly_access_t *v[2]={a,b};
    for (unsigned k=0;k<2;++k) {
      lo[k]=v[k]->offset.constant; hi[k]=lo[k]+v[k]->width-1;
      ny_i128 delta=(ny_i128)v[k]->offset.coeff[1-d]*(r->extent[1-d]-1);
      if (delta<0) lo[k]+=delta; else hi[k]+=delta;
    }
    ny_i128 low=lo[0]<lo[1]?lo[0]:lo[1];
    ny_i128 high=hi[0]>hi[1]?hi[0]:hi[1];
    if (high-low < (ny_i128)magnitude(stride)) return true;
  }
  return false;
}

/* Search for a counterexample in the four-dimensional product domain.
 * A box is discarded only when its address interval, domain constraints or
 * schedule intervals prove it cannot contain an inverted conflict. The
 * budget is shared across all pairs; exhausted search never proves legality. */
typedef struct { unsigned lo[4], hi[4]; } dep_box_t;
typedef struct {
  const nyr_poly_region_t *r;
  nyr_poly_schedule_t s;
  const nyr_poly_access_t *a,*b;
  unsigned *budget;
} dep_query_t;

static bool no_inversion(const dep_query_t *q, dep_box_t box) {
  if (!*q->budget) return false;
  --*q->budget;
  const nyr_poly_region_t *r=q->r;
  if (r->triangle) {
    for (unsigned d=0;d<4;d+=2) {
      unsigned strict=r->triangle&1u;
      unsigned x=d+(r->triangle>2),y=d+1-(r->triangle>2);
      if (box.lo[y]+strict>box.hi[x]) return true;
      if (box.lo[x]<box.lo[y]+strict) box.lo[x]=box.lo[y]+strict;
      if (box.hi[y]>box.hi[x]-strict) box.hi[y]=box.hi[x]-strict;
    }
  }
  nyr_poly_schedule_t original={0};
  uint64_t old_lo[2],old_hi[2],new_lo[2],new_hi[2];
  for (unsigned k=0;k<2;++k) {
    unsigned d=2*k;
    old_lo[k]=rank_point(r,original,box.lo[d],box.lo[d+1]);
    old_hi[k]=rank_point(r,original,box.hi[d],box.hi[d+1]);
    new_lo[k]=rank_point(r,q->s,box.lo[d],box.lo[d+1]);
    new_hi[k]=rank_point(r,q->s,box.hi[d],box.hi[d+1]);
  }
  bool forward=old_lo[0]<old_hi[1] && new_hi[0]>new_lo[1];
  bool backward=old_lo[1]<old_hi[0] && new_hi[1]>new_lo[0];
  if (!forward && !backward) return true;
  ny_i128 lo=(ny_i128)q->a->offset.constant-q->b->offset.constant,hi=lo;
  unsigned split=0, span=0;
  for (unsigned d=0;d<4;++d) {
    ny_i128 c=d<2?(ny_i128)q->a->offset.coeff[d]:-(ny_i128)q->b->offset.coeff[d-2];
    ny_i128 l=c*box.lo[d], h=c*box.hi[d];
    lo+=l<h?l:h; hi+=l<h?h:l;
    unsigned n=box.hi[d]-box.lo[d];
    if (n>span) {span=n;split=d;}
  }
  if (lo>(ny_i128)q->b->width-1 || hi<1-(ny_i128)q->a->width) return true;
  if (!span) return false; /* exact overlapping, inverted instances */
  unsigned middle=box.lo[split]+span/2;
  dep_box_t left=box; left.hi[split]=middle; box.lo[split]=middle+1;
  return no_inversion(q,left) && no_inversion(q,box);
}

static bool box_pair(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                         const nyr_poly_access_t *a, const nyr_poly_access_t *b,
                         unsigned *budget) {
  dep_query_t q={r,s,a,b,budget};
  dep_box_t box={.hi={r->extent[0]-1,r->extent[1]-1,
                     r->extent[0]-1,r->extent[1]-1}};
  return no_inversion(&q,box);
}

static bool general_pair(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                         const nyr_poly_access_t *a, const nyr_poly_access_t *b,
                         unsigned *budget) {
  if (points(r)<=64 && box_pair(r,s,a,b,budget)) return true;
  if (!*budget) return false;
  unsigned allowance=*budget<2048?*budget:2048, left=allowance;
  bool ok=nyr_poly_relation_pair(r,s,a,b,&left) &&
          (a==b || nyr_poly_relation_pair(r,s,b,a,&left));
  *budget-=allowance-left;
  return ok;
}

static bool symbolic_legal(const nyr_poly_region_t *r, nyr_poly_schedule_t s) {
  unsigned budget=32768, box_budget=65536;
  for (size_t i=0;i<r->access_count;++i)
    for (size_t j=i;j<r->access_count;++j) {
      const nyr_poly_access_t *a=&r->access[i], *b=&r->access[j];
      if ((!a->write && !b->write) || a->offset.base!=b->offset.base) continue;
      ny_i128 al,ah,bl,bh;
      if (!bounds(r,a,&al,&ah) || !bounds(r,b,&bl,&bh)) return false;
      if (ah<bl || bh<al || slice_ordered(r,a,b)) continue;
      if (a->offset.constant==b->offset.constant &&
          a->offset.coeff[0]==b->offset.coeff[0] &&
          a->offset.coeff[1]==b->offset.coeff[1] &&
          injective(r,a,a->width>b->width?a->width:b->width)) continue;
      uint64_t g=0;
      for (unsigned d=0;d<2;++d) {
        g=gcd_u64(g,magnitude(a->offset.coeff[d]));
        g=gcd_u64(g,magnitude(b->offset.coeff[d]));
      }
      bool same=a->offset.coeff[0]==b->offset.coeff[0] &&
                a->offset.coeff[1]==b->offset.coeff[1];
      for (int byte=1-(int)b->width;byte<(int)a->width;++byte) {
        ny_i128 diff=(ny_i128)a->offset.constant-b->offset.constant+byte;
        if (g && diff%(ny_i128)g) continue;
        if (!g && diff) continue;
        if (!same || !g) {
          if (!general_pair(r,s,a,b,&box_budget)) return false;
          break;
        }
        /* Enumerate the shorter distance dimension, solve the other exactly.
         * A rectangular superset of a triangular domain is conservative. */
        unsigned d=r->extent[0]<=r->extent[1]?0:1;
        if (!a->offset.coeff[1-d]) d=1-d;
        int64_t c=a->offset.coeff[d], q=a->offset.coeff[1-d];
        if (!q) {
          if (!general_pair(r,s,a,b,&box_budget)) return false;
          break;
        }
        int64_t bound=(int64_t)r->extent[d]-1;
        int64_t other=(int64_t)r->extent[1-d]-1;
        if ((uint64_t)(2*bound+1)>budget) {
          if (!general_pair(r,s,a,b,&box_budget)) return false;
          break;
        }
        budget-=(unsigned)(2*bound+1);
        for (int64_t x=-bound;x<=bound;++x) {
          ny_i128 rem=diff-(ny_i128)c*x;
          if (rem%q) continue;
          ny_i128 y=rem/q;
          if (y < -other || y > other) continue;
          if (!ordered_distance(s,d? (int64_t)y:x,d?x:(int64_t)y)) {
            if (!general_pair(r,s,a,b,&box_budget)) return false;
            goto pair_proven;
          }
        }
      }
    pair_proven: ;
    }
  return true;
}

/* Three-dimensional common-layout proof. Sort nonzero strides by magnitude
 * and require disjoint slabs. Conflicts can then vary only along zero-stride
 * dimensions. A single such dimension is a memory-carried reduction axis;
 * every supported permutation/tile schedule preserves its scalar order. */
static bool layout3(const nyr_poly_region_t *r,const nyr_poly_access_t *a,
                    const nyr_poly_access_t *b) {
  if (a->offset.constant!=b->offset.constant) return false;
  unsigned axes[3]={0,1,2}, zero=0;
  uint64_t stride[3];
  for (unsigned d=0;d<3;++d) {
    if (a->offset.coeff[d]!=b->offset.coeff[d]) return false;
    stride[d]=magnitude(a->offset.coeff[d]);
    zero+=!stride[d];
  }
  if (zero>1) return false;
  for (unsigned i=0;i<3;++i)
    for (unsigned j=i+1;j<3;++j)
      if (stride[axes[j]]<stride[axes[i]]) {
        unsigned t=axes[i];axes[i]=axes[j];axes[j]=t;
      }
  ny_i128 span=a->width>b->width?a->width:b->width;
  for (unsigned i=zero;i<3;++i) {
    unsigned d=axes[i];
    if ((ny_i128)stride[d]<span) return false;
    span+=(ny_i128)stride[d]*(r->extent[d]-1);
  }
  return true;
}
static bool symbolic_relation_legal(const nyr_poly_region_t *r, nyr_poly_schedule_t s) {
  unsigned budget=8192;
  for (size_t i=0;i<r->access_count;++i)
    for (size_t j=i;j<r->access_count;++j) {
      const nyr_poly_access_t *a=&r->access[i],*b=&r->access[j];
      if ((!a->write&&!b->write)||a->offset.base!=b->offset.base) continue;
      ny_i128 al,ah,bl,bh;
      if (!bounds(r,a,&al,&ah)||!bounds(r,b,&bl,&bh)) return false;
      if (ah<bl||bh<al||(nyr_poly_dims(r)==3&&layout3(r,a,b)) ||
          (nyr_poly_dims(r)==2&&slice_ordered(r,a,b))) continue;
      if (!nyr_poly_relation_pair(r,s,a,b,&budget) ||
          (a!=b&&!nyr_poly_relation_pair(r,s,b,a,&budget))) return false;
    }
  return true;
}

bool nyr_poly_symbolic_legal(const nyr_poly_region_t *r, nyr_poly_schedule_t s) {
  if (!valid_region(r)||!valid_schedule_for(r,s)) return false;
  if (!s.interchange&&!s.permutation&&!s.tile&&!s.wavefront) return true;
  return independent(r) || (nyr_poly_dims(r)==3 || s.wavefront ?
         symbolic_relation_legal(r,s) : symbolic_legal(r,s));
}

bool nyr_poly_legal(const nyr_poly_region_t *r, nyr_poly_schedule_t s) {
  events_t ev;
  if (!valid_region(r)||!valid_schedule_for(r,s)) return false;
  if (!s.interchange&&!s.permutation&&!s.tile&&!s.wavefront) return true;
  if (independent(r) || ((nyr_poly_dims(r)==3 || s.wavefront) &&
      symbolic_relation_legal(r,s))) return true;
  if (!events_build(r,&ev)) return nyr_poly_dims(r)==2 && !s.wavefront && symbolic_legal(r,s);
  bool ok=legal(r,&ev,s); free(ev.data); return ok;
}

bool nyr_poly_fusion_legal(const nyr_poly_region_t *a,const nyr_poly_region_t *b,
                           nyr_poly_schedule_t s) {
  if (!valid_region(a)||!valid_region(b)||a->external||b->external||!valid_schedule_for(a,s)||
      nyr_poly_dims(a)!=nyr_poly_dims(b)||a->triangle!=b->triangle) return false;
  unsigned dims=nyr_poly_dims(a);
  for (unsigned d=0;d<dims;++d)
    if (a->extent[d]!=b->extent[d]||a->symbolic[d]!=b->symbolic[d]||
        (a->symbolic[d]&&(a->bound_value[d]!=b->bound_value[d]||
                         a->inclusive[d]!=b->inclusive[d]))||
        a->initial[d]!=b->initial[d]||a->stride[d]!=b->stride[d]) return false;
  if (!nyr_poly_legal(a,s)||!nyr_poly_legal(b,s)) return false;
  unsigned budget=4096;
  for (size_t i=0;i<a->access_count;++i)
    for (size_t j=0;j<b->access_count;++j) {
      const nyr_poly_access_t *x=&a->access[i],*y=&b->access[j];
      if ((!x->write&&!y->write)||x->offset.base!=y->offset.base) continue;
      ny_i128 xl,xh,yl,yh;
      if (!bounds(a,x,&xl,&xh)||!bounds(b,y,&yl,&yh)) return false;
      if (xh<yl||yh<xl) continue;
      if (dims==2&&x->offset.constant==y->offset.constant&&
          x->offset.coeff[0]==y->offset.coeff[0]&&x->offset.coeff[1]==y->offset.coeff[1]&&
          injective(a,x,x->width>y->width?x->width:y->width)) continue;
      /* A same-layout reduction is NOT enough: fusion can interleave
       * accesses on its carried axis. Use the directed relation instead. */
      if (!nyr_poly_relation_fusion_pair(a,s,x,y,&budget)) return false;
    }
  return true;
}

/* A band is coincident iff no conflicting instances with identical outer
 * coordinates differ along that band. All ordered pairs are checked because
 * the access orientation is independent of the original statement order.
 * Like ISL coincidence constraints, this is separate from legality. */
bool nyr_poly_parallel_band(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                             unsigned band) {
  if (!valid_region(r)||!valid_schedule_for(r,s)) return false;
  unsigned levels=nyr_poly_dims(r)*(s.tile?2u:1u);
  if (band>=levels) return false;
  if (independent(r)) return true;
  unsigned budget=8192;
  for (size_t i=0;i<r->access_count;++i)
    for (size_t j=0;j<r->access_count;++j) {
      const nyr_poly_access_t *a=&r->access[i],*b=&r->access[j];
      if ((!a->write&&!b->write)||a->offset.base!=b->offset.base) continue;
      ny_i128 al,ah,bl,bh;
      if (!bounds(r,a,&al,&ah)||!bounds(r,b,&bl,&bh)) return false;
      if (ah<bl||bh<al) continue;
      if (nyr_poly_dims(r)==2&&a->offset.constant==b->offset.constant&&
          a->offset.coeff[0]==b->offset.coeff[0]&&a->offset.coeff[1]==b->offset.coeff[1]&&
          injective(r,a,a->width>b->width?a->width:b->width)) continue;
      if (!nyr_poly_relation_band_pair(r,s,a,b,band,&budget)) return false;
    }
  return true;
}

/* PPCG separates schedule-band properties from tile sizes. A componentwise
 * nonnegative dependence stays ordered after any positive strip-mining or
 * permutation. Cache the stronger certificate during candidate search.
 * Wavefront certificates apply to their own affine coordinate system only. */
bool nyr_poly_permutable(const nyr_poly_region_t *r, nyr_poly_schedule_t s) {
  if (!valid_region(r)||!valid_schedule_for(r,s)||s.tile) return false;
  if (independent(r)) return true;
  unsigned dims=nyr_poly_dims(r),budget=2048;
  for (size_t i=0;i<r->access_count;++i)
    for (size_t j=i;j<r->access_count;++j) {
      const nyr_poly_access_t *a=&r->access[i],*b=&r->access[j];
      if ((!a->write&&!b->write)||a->offset.base!=b->offset.base) continue;
      ny_i128 al,ah,bl,bh;
      if (!bounds(r,a,&al,&ah)||!bounds(r,b,&bl,&bh)) return false;
      if (ah<bl||bh<al) continue;
      if ((dims==2&&slice_ordered(r,a,b))||
          (dims==3&&layout3(r,a,b))) continue;
      if (!nyr_poly_relation_permutable_pair(r,s,a,b,&budget)||
          (a!=b&&!nyr_poly_relation_permutable_pair(r,s,b,a,&budget))) return false;
    }
  return true;
}

bool nyr_poly_find_wavefront(const nyr_poly_region_t *r, nyr_poly_schedule_t *out) {
  if (!out||!valid_region(r)||nyr_poly_dims(r)!=2||independent(r)) return false;
  /* Prefer the smallest positive affine coefficient: less wave overhead.
   * An inner coincident band is a prerequisite, not a profitability claim. */
  for (unsigned k=1;k<=8;++k) for (unsigned axis=0;axis<2;++axis) {
    nyr_poly_schedule_t s={.wavefront=true,.skew=k,.interchange=axis!=0};
    if (nyr_poly_symbolic_legal(r,s)&&nyr_poly_parallel_band(r,s,1)) {
      *out=s;return true;
    }
  }
  return false;
}

/* A deliberately small cost model: 32KiB L1, 64-byte lines, eight ways,
 * per-access stream line transitions and additional loop-control work.
 * Model object bases separately (no assumed absolute addresses). This is a
 * profitability heuristic, never a legality assumption. */
#define SETS 64u
#define WAYS 8u
/* All candidates visit the same bounded sample. Compute the expensive
 * affine addresses once rather than multiplying 128-bit offsets on every
 * candidate. The score and tie-breaking order are unchanged. */
/* Cache tags pack the line's high bits, a dense object id and the set
 * index. Given the object id, the set recovers the low six line bits.
 * The top bit distinguishes valid tags from zero-initialized cache slots.
 * Valid byte offsets are <= INT64_MAX; even the enclosing sample's unused
 * points fit the unsigned line representation. */
typedef struct { uint64_t *line;unsigned ni,nj,nk; } sample_t;
typedef struct {
  uint64_t tags[SETS][WAYS], prior[NYR_POLY_MAX_ACCESS];
  uint32_t ages[SETS][WAYS],tick;
  unsigned prior_way[NYR_POLY_MAX_ACCESS];
  uint64_t cost;
} sample_cost_t;

static inline void sample_score_line(sample_cost_t *c,size_t k,uint64_t tag) {
  if (c->prior[k]!=tag) {c->cost+=2;c->prior[k]=tag;}
  unsigned set=(unsigned)tag&(SETS-1),recent=c->prior_way[k];
  uint64_t *tags=c->tags[set];uint32_t *ages=c->ages[set];
  ++c->tick;
  if (tags[recent]==tag) {ages[recent]=c->tick;return;}
  unsigned old=0;
  for (unsigned w=0;w<WAYS;++w) {
    if (tags[w]==tag) {ages[w]=c->tick;c->prior_way[k]=w;return;}
    if (ages[w]<ages[old]) old=w;
  }
  tags[old]=tag;ages[old]=c->tick;c->cost+=12;c->prior_way[k]=old;
}
static bool sample_build(const nyr_poly_region_t *r,sample_t *sample) {
  unsigned dims=nyr_poly_dims(r),limit=dims==3?16:64;
  *sample=(sample_t){.ni=r->extent[0]<limit?r->extent[0]:limit,
                     .nj=r->extent[1]<limit?r->extent[1]:limit,
                     .nk=dims==3?(r->extent[2]<limit?r->extent[2]:limit):1};
  size_t n=(size_t)sample->ni*sample->nj*sample->nk*r->access_count;
  sample->line=malloc(n*sizeof(*sample->line));if (!sample->line) return false;
  unsigned root[NYR_POLY_MAX_ACCESS],bias[NYR_POLY_MAX_ACCESS];
  for (size_t a=0;a<r->access_count;++a) {
    int base=r->access[a].offset.base;
    root[a]=(unsigned)a;
    for (size_t b=0;b<a;++b)
      if (r->access[b].offset.base==base) {root[a]=root[b];break;}
    bias[a]=((unsigned)base*17u)&(SETS-1);
  }
  uint64_t *p=sample->line;
  for (unsigned i=0;i<sample->ni;++i) for (unsigned j=0;j<sample->nj;++j)
    for (unsigned z=0;z<sample->nk;++z) for (size_t a=0;a<r->access_count;++a) {
      uint64_t line=offset3(&r->access[a],i,j,z)/64;
      *p++=(UINT64_C(1)<<63)|((line>>6)<<10)|((uint64_t)root[a]<<6)|((line+bias[a])&63u);
    }
  return true;
}
static uint64_t score(const nyr_poly_region_t *r,const sample_t *sample,
                      nyr_poly_schedule_t s,uint64_t cutoff) {
  sample_cost_t c={0};
  unsigned na=s.interchange?sample->nj:sample->ni,nb=s.interchange?sample->ni:sample->nj;
  unsigned ta=s.tile?s.tile:na,tb=s.tile?(s.inner_tile?s.inner_tile:s.tile):nb;
  /* Project each triangular row before visiting its points. Preserve the
   * original control-cost accounting and cache access order exactly. */
  bool outer_greater=(r->triangle>2)==s.interchange;
  unsigned strict=r->triangle&1u;
  size_t row_stride=(s.interchange?sample->nj:1)*r->access_count;
  for (unsigned aa=0;aa<na;aa+=ta)
    for (unsigned bb=0;bb<nb;bb+=tb) {
      unsigned ae=aa+ta<na?aa+ta:na,be=bb+tb<nb?bb+tb:nb;
      if (s.tile) c.cost+=s.inner_tile==1?4:12;
      for (unsigned a=aa;a<ae;++a) {
        if (s.inner_tile!=1) c.cost+=4;
        unsigned low=bb,high=be;
        if (r->triangle) {
          if (outer_greater) {
            unsigned end=a+1-strict;
            if (end<high) high=end;
          } else if (a+strict>low) low=a+strict;
        }
        size_t index=(s.interchange?(size_t)low*sample->nj+a:(size_t)a*sample->nj+low)*r->access_count;
        for (unsigned b=low;b<high;++b,index+=row_stride) {
          const uint64_t *line=sample->line+index;
          for (size_t k=0;k<r->access_count;++k) sample_score_line(&c,k,line[k]);
        }
        /* Nonnegative cost terms: a losing prefix cannot recover. */
        if (c.cost>=cutoff) return c.cost;
      }
    }
  return c.cost;
}

typedef struct {
  const nyr_poly_region_t *r;
  events_t *ev;
  const sample_t *sample;
  bool free_order, have_events, attempted_events, band_checked;
  nyr_poly_schedule_t best;
  uint64_t best_cost;
  nyr_poly_schedule_t seen[48];
  size_t seen_count;
} search_t;

static void consider(search_t *search, nyr_poly_schedule_t cand) {
  const nyr_poly_region_t *r=search->r;
  unsigned a=r->extent[cand.interchange?1:0];
  unsigned b=r->extent[cand.interchange?0:1];
  unsigned tb=cand.inner_tile?cand.inner_tile:cand.tile;
  if (!valid_schedule(cand) || (cand.tile && (cand.tile>=a || tb>=b))) return;
  /* Rectangular hill climbing often revisits the same candidate. Normalize
   * the implicit square tile and cache visits, not legality assumptions. */
  if (cand.tile&&cand.inner_tile==cand.tile) cand.inner_tile=0;
  for (size_t i=0;i<search->seen_count;++i) {
    nyr_poly_schedule_t prior=search->seen[i];
    if (prior.interchange==cand.interchange&&prior.tile==cand.tile&&
        prior.inner_tile==cand.inner_tile) return;
  }
  if (search->seen_count<sizeof(search->seen)/sizeof(search->seen[0]))
    search->seen[search->seen_count++]=cand;
  uint64_t cost=score(r,search->sample,cand,search->best_cost);
  if (cost>=search->best_cost) return;
  if (!search->free_order && !search->band_checked) {
    search->band_checked=true;
    /* A single certificate covers all remaining axis/tile candidates.
     * Bounded failure falls back to the existing per-schedule checks. */
    search->free_order=nyr_poly_permutable(r,(nyr_poly_schedule_t){0});
  }
  if (!search->free_order && !symbolic_legal(r,cand)) {
    /* Most common affine layouts prove cheaply. Materialize the exact event
     * relation only when a profitable candidate remains unresolved. */
    if (!search->attempted_events) {
      search->attempted_events=true;
      search->have_events=events_build(r,search->ev);
    }
    if (!search->have_events || !legal(r,search->ev,cand)) return;
  }
  search->best=cand; search->best_cost=cost;
}

/* Bound the three-dimensional search to a few sampled permutations and
 * cube tiles. Large-domain legality is decided once by the slab proof. */
static uint64_t score3(const nyr_poly_region_t *r,const sample_t *sample,
                       nyr_poly_schedule_t s,uint64_t cutoff) {
  sample_cost_t c={0};unsigned n[3],t[3],axis[3];
  const unsigned ext[]={sample->ni,sample->nj,sample->nk};
  const size_t stride[]={ (size_t)sample->nj*sample->nk*r->access_count,
                           (size_t)sample->nk*r->access_count,r->access_count };
  for (unsigned q=0;q<3;++q) {
    axis[q]=nyr_poly_axis(s,3,q);
    n[q]=ext[axis[q]];
    t[q]=s.tile?s.tile:n[q];
  }
  for (unsigned aa=0;aa<n[0];aa+=t[0])
    for (unsigned bb=0;bb<n[1];bb+=t[1])
      for (unsigned cc=0;cc<n[2];cc+=t[2]) {
        if (s.tile)c.cost+=24;
        for (unsigned a=aa;a<n[0]&&a<aa+t[0];++a)
          for (unsigned b=bb;b<n[1]&&b<bb+t[1];++b) {
            c.cost+=4;
            size_t index=(size_t)a*stride[axis[0]]+(size_t)b*stride[axis[1]]+
                         (size_t)cc*stride[axis[2]];
            for (unsigned z=cc;z<n[2]&&z<cc+t[2];++z,index+=stride[axis[2]])
              for (size_t k=0;k<r->access_count;++k)
                sample_score_line(&c,k,sample->line[index+k]);
            if (c.cost>=cutoff) return c.cost;
          }
      }
  return c.cost;
}
static bool choose3(const nyr_poly_region_t *r,nyr_poly_schedule_t *out) {
  bool strided=false;
  for (size_t i=0;i<r->access_count;++i)
    strided|=magnitude(r->access[i].offset.coeff[2])>r->access[i].width;
  if (!strided) return false;
  events_t ev={0}; bool attempted=false, have=false;
  sample_t sample;
  if (!sample_build(r,&sample)) return false;
  uint64_t baseline=score3(r,&sample,(nyr_poly_schedule_t){0},UINT64_MAX),best=baseline;
  nyr_poly_schedule_t selected={0};
  /* Cube tiling is available through the explicit schedule API, but its
   * automatic cost model is not yet competitive on reduction kernels. */
  for (unsigned perm=1;perm<6;++perm) {
    nyr_poly_schedule_t s={.permutation=perm};
    uint64_t cost=score3(r,&sample,s,best);
    if (cost>=best) continue;
    bool proven=symbolic_relation_legal(r,s);
    if (!proven&&!attempted) {attempted=true;have=events_build(r,&ev);}
    if (proven||(have&&legal(r,&ev,s))) {selected=s;best=cost;}
  }
  free(ev.data);free(sample.line);
  if (best*10>baseline*8||(!selected.permutation&&!selected.tile)) return false;
  *out=selected;return true;
}

/* Conservative automatic 3D fast path: only independent streams, a
 * genuinely strided write in source order, and a new unit-stride inner
 * dimension for every stream. Reduction axes and mixed layouts stay opt-in.
 * Choose the best middle-axis locality among the eligible permutations. */
bool nyr_poly_choose_stream3(const nyr_poly_region_t *r,nyr_poly_schedule_t *out) {
  if (!out||!valid_region(r)||nyr_poly_dims(r)!=3||points(r)<1024) return false;
  bool strided_write=false;
  for (unsigned d=0;d<3;++d)
    if (r->symbolic[d]) return false;
  for (size_t i=0;i<r->access_count;++i)
    strided_write|=r->access[i].write&&magnitude(r->access[i].offset.coeff[2])>=64;
  if (!strided_write||!independent(r)) return false;
  bool found=false;
  ny_i128 best=0;
  for (unsigned k=1;k<6;++k) {
    nyr_poly_schedule_t s={.permutation=k};
    unsigned inner=nyr_poly_axis(s,3,2),middle=nyr_poly_axis(s,3,1);
    if (inner==2||r->extent[inner]<16||r->extent[inner]*UINT64_C(4)<r->extent[2]) continue;
    ny_i128 score=0;bool contiguous=true;
    for (size_t i=0;i<r->access_count;++i) {
      const nyr_poly_access_t *a=&r->access[i];
      contiguous&=magnitude(a->offset.coeff[inner])<=a->width;
      score+=(ny_i128)magnitude(a->offset.coeff[middle])*(a->write?2:1);
    }
    if (contiguous&&(!found||score<best)) {found=true;best=score;*out=s;}
  }
  return found;
}

bool nyr_poly_choose(const nyr_poly_region_t *r, nyr_poly_schedule_t *out) {
  if (!out || !valid_region(r) ||
      points(r)<128) return false;
  /* A large maximum alone is not evidence of profitable runtime blocking.
   * Keep highly variable or potentially tiny symbolic nests in source order. */
  for (unsigned d=0;d<nyr_poly_dims(r);++d)
    if (r->symbolic[d] && (r->minimum_extent[d]<16 ||
        (uint64_t)r->minimum_extent[d]*4<r->extent[d])) return false;
  if (nyr_poly_dims(r)==3) return choose3(r,out);
  /* All streams already walk consecutive bytes (or are loop-invariant).
   * Avoid an expensive search that can only disturb this good inner order.
   * This is a profitability shortcut, not a legality decision. */
  bool contiguous=true, swapped_contiguous=true, strided_write=false;
  for (size_t k=0;k<r->access_count;++k) {
    const nyr_poly_access_t *a=&r->access[k];
    uint64_t outer=magnitude(a->offset.coeff[0]);
    uint64_t inner=magnitude(a->offset.coeff[1]);
    if (inner>a->width) contiguous=false;
    if (outer>a->width) swapped_contiguous=false;
    if (a->write && inner>=64 && outer==a->width) strided_write=true;
  }
  if (contiguous) return false;
  bool free_order=independent(r);
  /* If a rectangular nest simply walks every stream in the wrong order,
   * avoid scoring tiled schedules at all. Require independent accesses,
   * a long enough new inner loop, and a substantial strided write; do not
   * apply this shortcut to narrow or extremely asymmetric nests. */
  if (free_order && !r->triangle && swapped_contiguous && strided_write &&
      r->extent[0]>=16 && (uint64_t)r->extent[0]*4>=r->extent[1]) {
    *out=(nyr_poly_schedule_t){.interchange=true};
    return true;
  }
  events_t ev={0};
  sample_t sample;
  if (!sample_build(r,&sample)) return false;
  uint64_t baseline=score(r,&sample,(nyr_poly_schedule_t){0},UINT64_MAX);
  search_t search={.r=r,.ev=&ev,.sample=&sample,.free_order=free_order,
                   .best_cost=baseline};
  /* First explore order and square tiles, then hill-climb nearby rectangular
   * tiles. This keeps scoring work bounded rather than a Cartesian sweep. */
  static const unsigned tiles[]={0,4,8,16,32};
  for (unsigned order=0;order<2;++order)
    for (size_t t=0;t<sizeof(tiles)/sizeof(tiles[0]);++t) {
      if (!order && !tiles[t]) continue;
      consider(&search,(nyr_poly_schedule_t){.interchange=order!=0,.tile=tiles[t]});
    }
  /* Strip schedules avoid an unnecessary fourth loop: (a/T,b,a%T).
   * They often retain most of the locality benefit with cheaper codegen. */
  for (unsigned order=0;order<2;++order)
    for (size_t t=1;t<sizeof(tiles)/sizeof(tiles[0]);++t)
      consider(&search,(nyr_poly_schedule_t){.interchange=order!=0,
        .tile=tiles[t],.inner_tile=1});
  for (unsigned round=0;round<2 && search.best.tile;++round) {
    nyr_poly_schedule_t center=search.best;
    unsigned a=center.tile,b=center.inner_tile?center.inner_tile:a;
    for (unsigned dim=0;dim<2;++dim) {
      for (unsigned up=0;up<2;++up) {
        nyr_poly_schedule_t cand=center;
        cand.tile=a; cand.inner_tile=b;
        unsigned *v=dim?&cand.inner_tile:&cand.tile;
        *v=up?*v*2:*v/2;
        if (*v>=4 && *v<=64) consider(&search,cand);
      }
    }
    if (center.tile==search.best.tile && center.inner_tile==search.best.inner_tile)
      break;
  }
  free(ev.data);free(sample.line);
  if (search.best_cost*10>baseline*9 || (!search.best.tile && !search.best.interchange))
    return false;
  *out=search.best;
  return true;
}
