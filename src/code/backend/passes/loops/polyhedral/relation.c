/* Integer relation emptiness for schedule inversions. Inspired by MLIR
 * AffineAnalysis's dependence relations and Presburger projection; original
 * NYIR implementation. No external solver. Exhaustion means unknown. */
#include "code/backend/passes/loops/polyhedral/poly.h"
#include "base/compat.h"
#include <limits.h>

#define VARS (4u * NYR_POLY_MAX_DIMS)
#define ROWS 64u
#define CAP INT64_C(1000000000000)
typedef struct { int64_t a[VARS]; ny_i128 b; } row_t; /* a*x <= b */
typedef struct { unsigned n, count; row_t row[ROWS]; } relation_t;
typedef struct { int64_t lo[VARS], hi[VARS]; } box_t;
typedef enum { EMPTY, NONEMPTY, UNKNOWN } answer_t;

static uint64_t mag(int64_t x) {
  return x < 0 ? UINT64_C(0) - (uint64_t)x : (uint64_t)x;
}
static uint64_t gcd(uint64_t a,uint64_t b) {
  while (b) { uint64_t t=a%b; a=b; b=t; } return a;
}
static ny_i128 floor_div(ny_i128 a,ny_i128 b) {
  ny_i128 q=a/b,r=a%b; return q-(r!=0 && ((r<0)!=(b<0)));
}
static ny_i128 ceil_div(ny_i128 a,ny_i128 b) {
  ny_i128 q=a/b,r=a%b; return q+(r!=0 && ((r<0)==(b<0)));
}
/* Integer tightening: a*x is a multiple of gcd(a). */
static void normalize(row_t *r,unsigned n) {
  uint64_t g=0;
  for (unsigned i=0;i<n;++i) g=gcd(g,mag(r->a[i]));
  if (g<=1) return;
  for (unsigned i=0;i<n;++i) r->a[i]=(int64_t)((ny_i128)r->a[i]/g);
  r->b=floor_div(r->b,g);
}
static bool push(relation_t *r,row_t x) {
  normalize(&x,r->n);
  for (unsigned i=0;i<r->count;++i) {
    if (memcmp(r->row[i].a,x.a,sizeof(x.a))) continue;
    if (x.b<r->row[i].b) r->row[i].b=x.b;
    return true;
  }
  if (r->count==ROWS) return false;
  r->row[r->count++]=x; return true;
}
static bool compare(relation_t *r,unsigned x,unsigned y,int64_t rhs) {
  row_t a={.b=rhs}; ++a.a[x]; --a.a[y]; return push(r,a);
}
static bool equal(relation_t *r,unsigned x,unsigned y) {
  return compare(r,x,y,0)&&compare(r,y,x,0);
}
/* Row minima give necessary bounds on each variable. Shrinking a box
 * makes any earlier minima in the current sweep conservative. */
static answer_t propagate_wide(const relation_t *r,box_t *box) {
  for (unsigned sweep=0;sweep<8;++sweep) {
    bool changed=false,all=true;
    for (unsigned k=0;k<r->count;++k) {
      const row_t *a=&r->row[k]; ny_i128 low=0,high=0;
      for (unsigned d=0;d<r->n;++d) {
        ny_i128 l=(ny_i128)a->a[d]*box->lo[d],h=(ny_i128)a->a[d]*box->hi[d];
        low+=l<h?l:h; high+=l<h?h:l;
      }
      if (low>a->b) return EMPTY;
      if (high<=a->b) continue;
      all=false;
      for (unsigned d=0;d<r->n;++d) {
        int64_t c=a->a[d]; if (!c) continue;
        int64_t v=c>0?box->lo[d]:box->hi[d];
        ny_i128 rest=a->b-low+(ny_i128)c*v;
        if (c>0) {
          ny_i128 bound=floor_div(rest,c);
          if (bound<box->lo[d]) return EMPTY;
          if (bound<box->hi[d]) {box->hi[d]=(int64_t)bound;changed=true;}
        } else {
          ny_i128 bound=ceil_div(rest,c);
          if (bound>box->hi[d]) return EMPTY;
          if (bound>box->lo[d]) {box->lo[d]=(int64_t)bound;changed=true;}
        }
      }
    }
    if (all) return NONEMPTY;
    if (!changed) break;
  }
  return UNKNOWN;
}
/* Transprecision propagation (FPL-inspired): establish a conservative
 * absolute bound on EVERY row sum, product and rhs once at the root of a
 * query. Subboxes only shrink, so the certificate survives subdivision.
 * Leave enough headroom for rhs - low + coefficient * endpoint. The
 * wide path remains the authoritative fallback for arbitrary i64 inputs.
 * This avoids compiler-runtime 128-bit division for ordinary small rows. */
static bool narrow_box(const relation_t *r,const box_t *box) {
  const ny_i128 limit=INT64_MAX/4;
  for (unsigned k=0;k<r->count;++k) {
    const row_t *a=&r->row[k];
    if (a->b < -limit || a->b > limit) return false;
    ny_i128 sum=0;
    for (unsigned d=0;d<r->n;++d) {
      uint64_t lo=mag(box->lo[d]),hi=mag(box->hi[d]);
      sum+=(ny_i128)mag(a->a[d])*(lo>hi?lo:hi);
      if (sum>limit) return false;
    }
  }
  return true;
}
static int64_t floor_div64(int64_t a,int64_t b) {
  int64_t q=a/b,r=a%b; return q-(r!=0&&((r<0)!=(b<0)));
}
static int64_t ceil_div64(int64_t a,int64_t b) {
  int64_t q=a/b,r=a%b; return q+(r!=0&&((r<0)==(b<0)));
}
static answer_t propagate_narrow(const relation_t *r,box_t *box) {
  for (unsigned sweep=0;sweep<8;++sweep) {
    bool changed=false,all=true;
    for (unsigned k=0;k<r->count;++k) {
      const row_t *a=&r->row[k]; int64_t low=0,high=0;
      for (unsigned d=0;d<r->n;++d) {
        int64_t l=(int64_t)a->a[d]*box->lo[d],h=(int64_t)a->a[d]*box->hi[d];
        low+=l<h?l:h; high+=l<h?h:l;
      }
      const int64_t rhs=(int64_t)a->b;
      if (low>rhs) return EMPTY;
      if (high<=rhs) continue;
      all=false;
      for (unsigned d=0;d<r->n;++d) {
        int64_t c=a->a[d]; if (!c) continue;
        int64_t v=c>0?box->lo[d]:box->hi[d];
        int64_t rest=rhs-low+(int64_t)c*v;
        if (c>0) {
          int64_t bound=floor_div64(rest,c);
          if (bound<box->lo[d]) return EMPTY;
          if (bound<box->hi[d]) {box->hi[d]=(int64_t)bound;changed=true;}
        } else {
          int64_t bound=ceil_div64(rest,c);
          if (bound>box->hi[d]) return EMPTY;
          if (bound>box->lo[d]) {box->lo[d]=(int64_t)bound;changed=true;}
        }
      }
    }
    if (all) return NONEMPTY;
    if (!changed) break;
  }
  return UNKNOWN;
}
/* Bounded Fourier-Motzkin projection. Integer tightening preserves every
 * integer solution, so proving the projected relaxation empty is sound.
 * No claim of exact integer projection is made. */
static bool project_empty(const relation_t *src,const box_t *box) {
  relation_t cur=*src;
  for (unsigned d=0;d<cur.n;++d) {
    row_t lo={.b=-box->lo[d]},hi={.b=box->hi[d]}; lo.a[d]=-1; hi.a[d]=1;
    if (!push(&cur,lo)||!push(&cur,hi)) return false;
  }
  bool done[VARS]={0};
  for (unsigned step=0;step<cur.n;++step) {
    unsigned best=VARS,work=UINT_MAX;
    for (unsigned d=0;d<cur.n;++d) if (!done[d]) {
      unsigned p=0,n=0;
      for (unsigned k=0;k<cur.count;++k) {p+=cur.row[k].a[d]>0;n+=cur.row[k].a[d]<0;}
      if (p*n<work) {work=p*n;best=d;}
    }
    if (best==VARS||work>ROWS) return false;
    done[best]=true; relation_t next={.n=cur.n};
    for (unsigned k=0;k<cur.count;++k)
      if (!cur.row[k].a[best]&&!push(&next,cur.row[k])) return false;
    for (unsigned p=0;p<cur.count;++p) {
      const row_t *a=&cur.row[p]; if (a->a[best]<=0) continue;
      for (unsigned q=0;q<cur.count;++q) {
        const row_t *b=&cur.row[q]; if (b->a[best]>=0) continue;
        uint64_t g=gcd(mag(a->a[best]),mag(b->a[best]));
        ny_i128 m=(ny_i128)mag(b->a[best])/g,n=(ny_i128)a->a[best]/g;
        if (m>CAP||n>CAP) return false;
        row_t v={0}; const ny_i128 cap=(ny_i128)1<<80;
        if (a->b>cap||a->b< -cap||b->b>cap||b->b< -cap) return false;
        v.b=m*a->b+n*b->b;
        for (unsigned d=0;d<cur.n;++d) {
          ny_i128 c=m*a->a[d]+n*b->a[d];
          if (c>CAP||c< -CAP) return false;
          v.a[d]=(int64_t)c;
        }
        if (!push(&next,v)) return false;
      }
    }
    for (unsigned k=0;k<next.count;++k) {
      bool zero=true; for (unsigned d=0;d<next.n;++d) zero&=!next.row[k].a[d];
      if (zero&&next.row[k].b<0) return true;
    }
    cur=next;
  }
  return false;
}
static answer_t solve_box(const relation_t *r,box_t box,unsigned *budget,bool project,bool narrow) {
  if (!*budget) return UNKNOWN;
  --*budget;
  answer_t a=narrow?propagate_narrow(r,&box):propagate_wide(r,&box);
  if (a!=UNKNOWN) return a;
  if (project&&project_empty(r,&box)) return EMPTY;
  unsigned split=VARS; int64_t span=0;
  for (unsigned d=0;d<r->n;++d) {
    int64_t n=box.hi[d]-box.lo[d];
    if (n>0&&(!span||n<span)) {span=n;split=d;}
  }
  if (split==VARS) return UNKNOWN;
  int64_t mid=box.lo[split]+span/2;
  box_t left=box;left.hi[split]=mid;box.lo[split]=mid+1;
  answer_t l=solve_box(r,left,budget,false,narrow);
  if (l==NONEMPTY) return l;
  answer_t h=solve_box(r,box,budget,false,narrow);
  return h==NONEMPTY?h:l==EMPTY&&h==EMPTY?EMPTY:UNKNOWN;
}

static answer_t solve(const relation_t *r,box_t box,unsigned *budget,bool project) {
#ifdef NYR_POLY_FORCE_WIDE
  bool narrow=false;
#else
  bool narrow=narrow_box(r,&box);
#endif
  return solve_box(r,box,budget,project,narrow);
}

/* A lexicographic inversion is a union of relations, one for each pair
 * of first-differing schedule coordinates. Quotient dimensions encode
 * floor division for tiles exactly. Original access order is unchanged
 * within each iteration. This query is directed: also test b -> a. */
static bool relation_pair(const nyr_poly_region_t *r,nyr_poly_schedule_t s,
                          const nyr_poly_access_t *a,const nyr_poly_access_t *b,
                          unsigned *budget,bool fusion,int band,bool permutable) {
  if (!r||!a||!b||!a->width||!b->width) return false;
  unsigned dims=nyr_poly_dims(r),n=2*dims;
  if (dims<2||dims>3||!budget||!*budget||s.permutation>=6||s.tile>256||s.inner_tile>256||
      r->triangle>4||(r->triangle&&(dims!=2||r->extent[0]!=r->extent[1]))) return false;
  if (s.skew>8 || (!s.wavefront&&s.skew) ||
      (s.wavefront&&(dims!=2||s.tile||s.inner_tile||s.permutation))) return false;
  relation_t base={.n=s.tile?2*n:s.wavefront?n+2:n}; box_t box={0};
  row_t overlap={.b=(ny_i128)b->offset.constant-a->offset.constant+b->width-1};
  row_t other={.b=(ny_i128)a->offset.constant-b->offset.constant+a->width-1};
  for (unsigned d=0;d<dims;++d) {
    if (!r->extent[d]||r->extent[d]>NYR_POLY_MAX_EXTENT||
        a->offset.coeff[d]==INT64_MIN||b->offset.coeff[d]==INT64_MIN) return false;
    box.hi[d]=box.hi[d+dims]=r->extent[d]-1;
    overlap.a[d]=a->offset.coeff[d]; overlap.a[d+dims]=-b->offset.coeff[d];
    other.a[d]=-overlap.a[d]; other.a[d+dims]=-overlap.a[d+dims];
  }
  if (!push(&base,overlap)||!push(&base,other)) return false;
  if (r->triangle) for (unsigned k=0;k<2;++k) {
    unsigned x=k*dims+(r->triangle>2),y=k*dims+1-(r->triangle>2);
    if (!compare(&base,y,x,(r->triangle&1u)?-1:0)) return false;
  }
  unsigned x[2*NYR_POLY_MAX_DIMS],y[2*NYR_POLY_MAX_DIMS],levels=0;
  if (s.wavefront) {
    for (unsigned k=0;k<2;++k) {
      unsigned q=n+k, v=k*dims+(s.interchange?1:0), w=k*dims+(s.interchange?0:1);
      int64_t skew=nyr_poly_skew(s);
      box.hi[q]=skew*box.hi[v]+box.hi[w];
      row_t lo={0},hi={0};lo.a[q]=1;lo.a[v]=-skew;lo.a[w]=-1;
      hi.a[q]=-1;hi.a[v]=skew;hi.a[w]=1;
      if (!push(&base,lo)||!push(&base,hi)) return false;
    }
    x[levels]=n;y[levels++]=n+1;
    x[levels]=s.interchange?1:0;y[levels++]=dims+(s.interchange?1:0);
  } else {
  if (s.tile) for (unsigned t=0;t<dims;++t) {
    unsigned d=nyr_poly_axis(s,dims,t),width=t==1&&s.inner_tile?s.inner_tile:s.tile;
    for (unsigned k=0;k<2;++k) {
      unsigned v=k*dims+d,q=v+n; box.hi[q]=box.hi[v]/width;
      row_t lo={0},hi={.b=width-1};lo.a[q]=width;lo.a[v]=-1;hi.a[q]=-(int64_t)width;hi.a[v]=1;
      if (!push(&base,lo)||!push(&base,hi)) return false;
    }
    x[levels]=n+d;y[levels++]=n+d+dims;
  }
  for (unsigned t=0;t<dims;++t) {
    unsigned d=nyr_poly_axis(s,dims,t);x[levels]=d;y[levels++]=d+dims;
  }
  }
  if (permutable) {
    if (s.tile) return false;
    /* The original schedule orients every conflicting iteration pair.
     * Unlike lexicographic legality, a permutable band requires EVERY
     * component to be nondecreasing. Prove each negative component empty.
     * No equality on earlier NEW coordinates is imposed. */
    relation_t prefix=base;
    for (unsigned i=0;i<dims;++i) {
      relation_t old=prefix;
      if (!compare(&old,i,i+dims,-1)) return false;
      for (unsigned j=0;j<levels;++j) {
        relation_t query=old;
        if (!compare(&query,y[j],x[j],-1)||
            solve(&query,box,budget,true)!=EMPTY) return false;
      }
      if (!equal(&prefix,i,i+dims)) return false;
    }
    return true;
  }
  if (band>=0) {
    if ((unsigned)band>=levels) return false;
    for (unsigned j=0;j<(unsigned)band;++j)
      if (!equal(&base,x[j],y[j])) return false;
    /* Caller tests all ordered access pairs; a conflict at x < y suffices. */
    return compare(&base,x[band],y[band],-1)&&solve(&base,box,budget,true)==EMPTY;
  }
  if (fusion) {
    /* Before fusion every source statement executes before every target
     * statement. Equal new iteration vectors still execute A before B. */
    relation_t prefix=base;
    for (unsigned j=0;j<levels;++j) {
      relation_t query=prefix;
      if (!compare(&query,y[j],x[j],-1)||solve(&query,box,budget,true)!=EMPTY) return false;
      if (!equal(&prefix,x[j],y[j])) return false;
    }
    return true;
  }
  relation_t old_prefix=base;
  for (unsigned i=0;i<dims;++i) {
    relation_t old=old_prefix;
    if (!compare(&old,i,i+dims,-1)) return false;
    relation_t new_prefix=old;
    for (unsigned j=0;j<levels;++j) {
      relation_t query=new_prefix;
      if (!compare(&query,y[j],x[j],-1)||solve(&query,box,budget,true)!=EMPTY) return false;
      if (!equal(&new_prefix,x[j],y[j])) return false;
    }
    if (!equal(&old_prefix,i,i+dims)) return false;
  }
  return true;
}

bool nyr_poly_relation_pair(const nyr_poly_region_t *r,nyr_poly_schedule_t s,
                            const nyr_poly_access_t *a,const nyr_poly_access_t *b,
                            unsigned *budget) {
  return relation_pair(r,s,a,b,budget,false,-1,false);
}
bool nyr_poly_relation_fusion_pair(const nyr_poly_region_t *r,nyr_poly_schedule_t s,
                                   const nyr_poly_access_t *a,const nyr_poly_access_t *b,
                                   unsigned *budget) {
  return relation_pair(r,s,a,b,budget,true,-1,false);
}

bool nyr_poly_relation_band_pair(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                                 const nyr_poly_access_t *a, const nyr_poly_access_t *b,
                                 unsigned band, unsigned *budget) {
  if (band>=2*NYR_POLY_MAX_DIMS) return false;
  return relation_pair(r,s,a,b,budget,false,(int)band,false);
}

/* A permutable band certificate is stronger than any individual tile
 * legality query. Inspired by PPCG's schedule-band tiling interface. */
bool nyr_poly_relation_permutable_pair(const nyr_poly_region_t *r,
                                      nyr_poly_schedule_t s,
                                      const nyr_poly_access_t *a,
                                      const nyr_poly_access_t *b,
                                      unsigned *budget) {
  return relation_pair(r,s,a,b,budget,false,-1,true);
}
