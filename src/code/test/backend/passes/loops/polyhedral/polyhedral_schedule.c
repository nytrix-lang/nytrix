/* Independent all-pairs oracle for finite schedule legality, followed by
 * end-to-end NYIR regeneration checks. No external solver is required. */
#include "opt.h"
#include "code/backend/passes/loops/polyhedral/domain.h"
#include <inttypes.h>
#include "code/backend/passes/verify/loop.h"
#include <nyr/nyr.h>

static unsigned random_state = 0x9172a5u;
static unsigned next_random(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}

static int order(nyr_poly_schedule_t s, unsigned i, unsigned j,
                 unsigned x, unsigned y) {
  if (s.wavefront) { unsigned k=s.skew?s.skew:1; unsigned a=s.interchange?j:i,b=s.interchange?i:j,c=s.interchange?y:x,d=s.interchange?x:y; if (k*a+b!=k*c+d) return k*a+b<k*c+d?-1:1; return (a>c)-(a<c); }
  unsigned a = s.interchange ? j : i, b = s.interchange ? i : j;
  unsigned c = s.interchange ? y : x, d = s.interchange ? x : y;
  if (s.tile) {
    unsigned t = s.inner_tile ? s.inner_tile : s.tile;
    if (a / s.tile != c / s.tile) return a / s.tile < c / s.tile ? -1 : 1;
    if (b / t != d / t) return b / t < d / t ? -1 : 1;
  }
  if (a != c) return a < c ? -1 : 1;
  return (b > d) - (b < d);
}

static int64_t byte_at(nyr_poly_access_t a, unsigned i, unsigned j) {
  return a.offset.constant + a.offset.coeff[0] * i + a.offset.coeff[1] * j;
}

static bool brute(const nyr_poly_region_t *r, nyr_poly_schedule_t s) {
  for (unsigned i = 0; i < r->extent[0]; ++i)
    for (unsigned j = 0; j < r->extent[1]; ++j)
      if (nyr_poly_contains(r, i, j))
        for (unsigned x = i; x < r->extent[0]; ++x)
          for (unsigned y = x == i ? j : 0; y < r->extent[1]; ++y) {
            if (!nyr_poly_contains(r, x, y) || order(s, i, j, x, y) <= 0)
              continue;
            for (size_t a = 0; a < r->access_count; ++a)
              for (size_t b = 0; b < r->access_count; ++b) {
                nyr_poly_access_t p = r->access[a], q = r->access[b];
                if (p.offset.base != q.offset.base || (!p.is_write && !q.is_write))
                  continue;
                int64_t u = byte_at(p, i, j), v = byte_at(q, x, y);
                if (u < v + q.width && v < u + p.width) return false;
              }
          }
  return true;
}

static bool brute_band(const nyr_poly_region_t *r,nyr_poly_schedule_t s,unsigned band) {
  for (unsigned i=0;i<r->extent[0];++i) for (unsigned j=0;j<r->extent[1];++j)
    for (unsigned x=0;x<r->extent[0];++x) for (unsigned y=0;y<r->extent[1];++y) {
      if (!nyr_poly_contains(r,i,j)||!nyr_poly_contains(r,x,y)) continue;
      unsigned k=s.skew?s.skew:1;
      unsigned u0=s.interchange?j:i,v0=s.interchange?y:x;
      unsigned a=k*u0+(s.interchange?i:j),b=k*v0+(s.interchange?x:y);
      if (band ? (a!=b||u0==v0) : a==b) continue;
      for (size_t u=0;u<r->access_count;++u) for (size_t v=0;v<r->access_count;++v) {
        const nyr_poly_access_t *p=&r->access[u],*q=&r->access[v];
        if (p->offset.base!=q->offset.base||(!p->is_write&&!q->is_write)) continue;
        int64_t l=byte_at(*p,i,j),h=byte_at(*q,x,y);
        if (l<h+q->width&&h<l+p->width) return false;
      }
    }
  return true;
}

/* Independent all-pairs proof of componentwise schedule monotonicity. */
static bool brute_permutable(const nyr_poly_region_t *r,nyr_poly_schedule_t s) {
  unsigned dims=nyr_poly_dims(r),nz=dims==3?r->extent[2]:1;
  for (unsigned i=0;i<r->extent[0];++i) for (unsigned j=0;j<r->extent[1];++j)
    for (unsigned k=0;k<nz;++k) for (unsigned x=i;x<r->extent[0];++x)
      for (unsigned y=x==i?j:0;y<r->extent[1];++y)
        for (unsigned z=x==i&&y==j?k+1:0;z<nz;++z) {
          if (!nyr_poly_contains(r,i,j)||!nyr_poly_contains(r,x,y)) continue;
          bool inversion=s.wavefront ? ((s.interchange?j>y:i>x)||
            (uint64_t)nyr_poly_skew(s)*(s.interchange?j:i)+(s.interchange?i:j)>
            (uint64_t)nyr_poly_skew(s)*(s.interchange?y:x)+(s.interchange?x:y)) : j>y||k>z;
          if (!inversion) continue;
          for (size_t a=0;a<r->access_count;++a) for (size_t b=0;b<r->access_count;++b) {
            const nyr_poly_access_t *u=&r->access[a],*v=&r->access[b];
            if (u->offset.base!=v->offset.base||(!u->is_write&&!v->is_write)) continue;
            int64_t l=byte_at(*u,i,j)+u->offset.coeff[2]*k;
            int64_t h=byte_at(*v,x,y)+v->offset.coeff[2]*z;
            if (l<h+v->width&&h<l+u->width) return false;
          }
        }
  return true;
}

static bool random_legality(void) {
  const nyr_poly_schedule_t schedules[] = {
    {0}, {.interchange = true}, {.tile = 2}, {.tile = 3, .inner_tile = 2},
    {.tile = 2, .inner_tile = 3, .interchange = true},
    {.tile = 4, .interchange = true}, {.tile=3,.inner_tile=1},
    {.tile=3,.inner_tile=1,.interchange=true},{.wavefront=true},
    {.wavefront=true,.skew=2},{.wavefront=true,.skew=3},{.wavefront=true,.skew=8},
    {.wavefront=true,.interchange=true},{.wavefront=true,.interchange=true,.skew=2},
    {.wavefront=true,.interchange=true,.skew=3},{.wavefront=true,.interchange=true,.skew=8}};
  unsigned count = 0, symbolic_accept = 0, oracle_accept = 0;
  for (unsigned k = 0; k < 3000; ++k) {
    nyr_poly_region_t r = {.extent = {2 + next_random() % 6,
                                      2 + next_random() % 6},
                            .triangle = next_random() % 5,
                            .access_count = 1 + next_random() % 4};
    if (r.triangle) r.extent[1] = r.extent[0];
    for (size_t a = 0; a < r.access_count; ++a) {
      r.access[a] = (nyr_poly_access_t){
        .offset = {.coeff = {(int)(next_random() % 17) - 8,
                             (int)(next_random() % 17) - 8},
                   .constant = 128 + next_random() % 24,
                   .base = (int)(next_random() % 2)},
        .width = 1u << (next_random() % 4), .is_write = next_random() % 2};
    }
    for (unsigned q=0;q<5;++q) {
      nyr_poly_schedule_t band={.wavefront=q!=0,.skew=q>2?q-2:q,.interchange=q>2};
      bool got=nyr_poly_permutable(&r,band),expected=brute_permutable(&r,band);
      if (got&&!expected) {fprintf(stderr,"false permutable band %u/%u\n",k,q);return false;}
    }
    for (size_t s = 0; s < sizeof(schedules) / sizeof(schedules[0]); ++s) {
      bool expected = brute(&r, schedules[s]);
      bool symbolic = nyr_poly_symbolic_legal(&r, schedules[s]);
      oracle_accept += expected; symbolic_accept += symbolic;
      if ((symbolic && !expected) || nyr_poly_legal(&r, schedules[s]) != expected) {
        fprintf(stderr, "legality mismatch trial=%u schedule=%zu\n", k, s);
        return false;
      }
      if (schedules[s].wavefront && k<500) for (unsigned band=0;band<2;++band)
        if (nyr_poly_parallel_band(&r,schedules[s],band)&&!brute_band(&r,schedules[s],band)) {
          fprintf(stderr,"false coincidence trial=%u schedule=%zu band=%u\n",k,s,band);return false;
        }
      ++count;
    }
  }
  /* Larger than the enumeration budget: a row-local recurrence admits
   * interchange; a cross-row, backwards-column recurrence does not. */
  nyr_poly_region_t r = {.extent = {256, 256}, .access_count = 2,
    .access = {{{.coeff = {4096, 8}, .constant = 8, .base = 1}, 8, true},
               {{.coeff = {4096, 8}, .constant = 0, .base = 1}, 8, false}}};
  if (!nyr_poly_legal(&r, (nyr_poly_schedule_t){.interchange = true}) ||
      !nyr_poly_legal(&r, (nyr_poly_schedule_t){.tile = 16, .inner_tile = 32}))
    return false;
  r.access[0].offset.constant = 4096;
  r.access[1].offset.constant = 8;
  if (nyr_poly_legal(&r, (nyr_poly_schedule_t){.interchange = true}) ||
      nyr_poly_legal(&r, (nyr_poly_schedule_t){.tile = 16}))
    return false;
  /* Large different-layout slices: columns scaled differently, but no
   * dependence crosses a row; interchange and tiling are both legal. */
  r = (nyr_poly_region_t){.extent={256,256},.access_count=2,
    .access={{{.coeff={8192,16},.constant=0,.base=1},8,true},
             {{.coeff={8192,8},.constant=8,.base=1},8,false}}};
  if (!nyr_poly_legal(&r,(nyr_poly_schedule_t){.interchange=true}) ||
      !nyr_poly_legal(&r,(nyr_poly_schedule_t){.tile=16})) return false;
  printf("polyhedral: %u exact + %u symbolic oracle comparisons; symbolic accepted %u/%u legal schedules\n",
         count,count,symbolic_accept,oracle_accept);
  return true;
}

static int address(nyr_func_t *f, int a, int i, int j, int m, int eight) {
  int p = nyr_opt_binop(f, NYIR_MUL_I64, i, m);
  p = nyr_opt_binop(f, NYIR_ADD_I64, p, j);
  p = nyr_opt_binop(f, NYIR_MUL_I64, p, eight);
  return nyr_opt_binop(f, NYIR_ADD_I64, a, p);
}
static void store(nyr_func_t *f, int a, int v) {
  nyr_opt_emit_abc(f, NYIR_STORE_I64, a, -1, v);
}
static int load(nyr_func_t *f, int a) {
  return nyr_opt_emit_abc(f, NYIR_LOAD_I64, a, -1, -1);
}
static void build_with_bounds(nyr_func_t *f, int n, int m, unsigned triangle, bool alias,
                  int si, int sj, int di, int dj, bool symbolic, unsigned variant) {
  nyr_opt_begin(f);
  int zero = nyr_opt_const(f, 0), one = nyr_opt_const(f, 1);
  int ni = nyr_opt_const(f, n), mi = nyr_opt_const(f, m);
  int eight = nyr_opt_const(f, 8);
  int bound_i=ni,bound_j=mi;
  if (symbolic) {
    int input=nyr_opt_emit_imm(f,NYIR_LOAD_LOCAL,0);
    int mask=nyr_opt_const(f,7),shift=nyr_opt_const(f,3);
    bound_i=nyr_opt_binop(f,NYIR_AND_I64,input,mask);
    bound_j=nyr_opt_binop(f,NYIR_AND_I64,
      nyr_opt_binop(f,NYIR_SAR_I64,input,shift),mask);
  }
  int start_i=nyr_opt_const(f,si), start_j=nyr_opt_const(f,sj);
  int step_i=nyr_opt_const(f,di), step_j=nyr_opt_const(f,dj);
  int low_i=nyr_opt_const(f,0), low_j=nyr_opt_const(f,0);
  int a = nyr_opt_emit_imm(f, NYIR_ALLOCA, n * m * 8);
  int b = nyr_opt_emit_imm(f, NYIR_ALLOCA, n * m * 8);
  for (int k = 0; k < n * m; ++k) {
    int off = nyr_opt_const(f, k * 8), v = nyr_opt_const(f, k + 3);
    store(f, nyr_opt_binop(f, NYIR_ADD_I64, a, off), v);
    store(f, nyr_opt_binop(f, NYIR_ADD_I64, b, off), zero);
  }
  nyr_opt_label(f, 9); nyr_opt_br(f, 10); nyr_opt_label(f, 10);
  size_t oi = f->len; int i = f->next_value;
  nyr_opt_phi(f, 9, start_i, 20, zero);
  nyr_opt_br_if(f, nyr_opt_cmp(f, i, di>0||symbolic?bound_i:low_i, di>0?NYIR_CMP_LT:NYIR_CMP_GE), 11);
  nyr_opt_br(f, 30); nyr_opt_label(f, 11);
  int body_one=one;
  if (triangle==3) {
    /* NYC may share an inner-preheader constant with the retained body. */
    body_one=nyr_opt_const(f,1);
    start_j=nyr_opt_binop(f,NYIR_ADD_I64,i,body_one);
  } else if (triangle==4) start_j=i;
  nyr_opt_br(f, 12);
  nyr_opt_label(f, 12); size_t ij = f->len; int j = f->next_value;
  nyr_opt_phi(f, 11, start_j, 13, zero);
  int upper=triangle>2?bound_i:triangle?i:dj>0||symbolic?bound_j:low_j;
  nyr_opt_br_if(f, nyr_opt_cmp(f,j,upper,dj<0?NYIR_CMP_GE:triangle==2?NYIR_CMP_LE:NYIR_CMP_LT),13);
  nyr_opt_br(f, 20); nyr_opt_label(f, 13);
  int source=address(f,a,i,j,mi,eight);
  if (variant==1) {
    int diff=nyr_opt_binop(f,NYIR_SUB_I64,triangle>2?j:i,triangle>2?i:j);
    source=nyr_opt_binop(f,NYIR_ADD_I64,a,nyr_opt_binop(f,NYIR_MUL_I64,diff,eight));
  }
  int v=load(f,source);
  int dst=address(f,alias?a:b,j,i,ni,eight);
  if (variant==2) {
    int x=nyr_opt_const_f64(f,1.25),y=nyr_opt_const_f64(f,2.5);
    int z=nyr_opt_binop(f,NYIR_MUL_F64,x,y);
    store(f,dst,z);f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;
  } else store(f,dst,nyr_opt_binop(f,NYIR_ADD_I64,v,body_one));
  int jn = nyr_opt_binop(f, NYIR_ADD_I64, j, step_j); nyr_opt_br(f, 12);
  nyr_opt_label(f, 20); int in = nyr_opt_binop(f, NYIR_ADD_I64, i, step_i);
  nyr_opt_br(f, 10); nyr_opt_label(f, 30);
  int result = zero;
  for (int k = 0; k < n * m; ++k) {
    int off = nyr_opt_const(f, k * 8);
    int val = load(f, nyr_opt_binop(f, NYIR_ADD_I64, alias ? a : b, off));
    if (variant==2) {
      f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;
      val=nyr_opt_emit_abc(f,NYIR_F64_TO_I64,val,-1,-1);
    }
    int factor = nyr_opt_const(f, k + 1);
    result = nyr_opt_binop(f, NYIR_ADD_I64, result,
                           nyr_opt_binop(f, NYIR_MUL_I64, val, factor));
  }
  nyr_opt_ret(f, result);
  f->data[oi].phi_incoming[1].value = in;
  f->data[ij].phi_incoming[1].value = jn;
}

static void build(nyr_func_t *f, int n, int m, unsigned triangle, bool alias,
                  int si, int sj, int di, int dj) {
  build_with_bounds(f,n,m,triangle,alias,si,sj,di,dj,false,0);
}

static bool evaluate_seed(nyr_func_t *f, int64_t seed, int64_t *out) {
  char err[256];
  nyr_eval_result_t result = {0};
  int64_t locals[1] = {seed};
  if (!nyr_ir_eval(f, locals, 1, 200000, &result, err, sizeof(err))) {
    fprintf(stderr, "polyhedral evaluation: %s\n", err);
    return false;
  }
  *out = result.result;
  nyr_eval_result_free(&result);
  return true;
}

static bool evaluate(nyr_func_t *f, int64_t *out) {
  return evaluate_seed(f,0,out);
}

static bool regenerated(void) {
  unsigned count = 0;
  for (unsigned shape = 0; shape < 3; ++shape)
    for (unsigned swap = 0; swap < 2; ++swap)
      for (unsigned tile = 0; tile < 5; ++tile) {
        nyr_func_t f;
        build(&f, 5, shape ? 5 : 7, shape, false,0,0,1,1);
        nyr_poly_region_t r;
        nyr_poly_schedule_t s = {.interchange = swap != 0, .tile = tile,
                                 .inner_tile = tile==4?1:tile ? tile + 1 : 0};
        int64_t before = 0, after = 0;
        bool ok = nyr_opt_verify(&f, "poly-input") &&
          nyr_poly_detect(&f, 0, &r) && r.triangle == shape &&
          evaluate(&f, &before) && nyr_poly_legal(&r, s) &&
          nyr_poly_emit(&f, &r, s) && nyr_opt_verify(&f, "poly-output") &&
          evaluate(&f, &after) && before == after;
        if (ok && tile) {
          nyr_scev_info_t loops = {0};
          ok = nyr_scev_analyze(&f, &loops) && loops.count >= (s.inner_tile==1?3u:4u);
          nyr_scev_free(&loops);
        }
        nyr_ir_func_free(&f);
        if (!ok) {
          fprintf(stderr, "regeneration mismatch shape=%u swap=%u tile=%u %lld != %lld\n",
                  shape, swap, tile, (long long)before, (long long)after);
          return false;
        }
        ++count;
      }
  const int recurrences[][4]={{1,2,2,2},{4,6,-1,-2},{0,6,2,-3},
                                {4,1,-2,3},{1,1,1,1}};
  for (unsigned k=0;k<sizeof(recurrences)/sizeof(recurrences[0]);++k)
    for (unsigned swap=0;swap<2;++swap)
      for (unsigned tile=0;tile<5;++tile) {
        nyr_func_t f; nyr_poly_region_t r;
        const int *v=recurrences[k];
        build(&f,5,7,0,false,v[0],v[1],v[2],v[3]);
        nyr_poly_schedule_t s={.interchange=swap!=0,.tile=tile,
                               .inner_tile=tile==4?1:tile?tile+1:0};
        int64_t before=0,after=0;
        bool ok=nyr_opt_verify(&f,"strided-input") &&
          nyr_poly_detect(&f,0,&r) && r.initial[0]==v[0] &&
          r.initial[1]==v[1] && r.stride[0]==v[2] && r.stride[1]==v[3] &&
          evaluate(&f,&before) && nyr_poly_emit(&f,&r,s) &&
          nyr_opt_verify(&f,"strided-output") && evaluate(&f,&after) && before==after;
        nyr_ir_func_free(&f);
        if (!ok) {fprintf(stderr,"strided mismatch %u/%u/%u: %lld != %lld\n",k,swap,tile,(long long)before,(long long)after);return false;}
        ++count;
      }
  nyr_func_t f;
  build(&f, 5, 5, 0, true,0,0,1,1);
  nyr_poly_region_t r;
  /* In-place transpose has opposing read/write dependences. */
  bool ok = nyr_poly_detect(&f, 0, &r) &&
    !nyr_poly_legal(&r, (nyr_poly_schedule_t){.interchange = true}) &&
    !nyr_poly_emit(&f, &r, (nyr_poly_schedule_t){.interchange = true});
  nyr_ir_func_free(&f);
  printf("polyhedral: %u regenerated regions; alias rejection %s\n", count,
         ok ? "passed" : "failed");
  return ok;
}

/* Exact tile boundaries do not need min() and the old recurrence must not
 * survive as a dead body instruction. This checks the actual emitted shape,
 * not just the result of the interpreter. */
static bool exact_tiles(void) {
  nyr_func_t f; nyr_poly_region_t r;
  build(&f, 8, 8, 0, false, 0, 0, 1, 1);
  int64_t before=0, after=0;
  nyr_poly_schedule_t s={.tile=4,.inner_tile=4};
  bool ok=nyr_poly_detect(&f,0,&r) && evaluate(&f,&before) &&
    nyr_poly_emit(&f,&r,s) && nyr_opt_verify(&f,"exact-tiles") &&
    evaluate(&f,&after) && before==after;
  for (size_t i=0;ok && i<f.len;++i)
    if (f.data[i].op==NYIR_SELECT_I64) ok=false;
  nyr_ir_func_free(&f);
  if (!ok) fprintf(stderr,"exact tile boundary mismatch\n");
  return ok;
}

/* Unit-stride streams should not pay for schedule search or regeneration. */
static bool profitability(void) {
  nyr_poly_region_t r={.extent={512,512},.access_count=2};
  r.access[0]=(nyr_poly_access_t){.offset={.coeff={4096,8},.base=0},.width=8};
  r.access[1]=(nyr_poly_access_t){.offset={.coeff={4096,8},.base=1},.width=8,.is_write=true};
  nyr_poly_schedule_t s;
  if (nyr_poly_choose(&r,&s)) return false;
  r.access[1].offset.coeff[0]=8;
  r.access[1].offset.coeff[1]=4096;
  if (!nyr_poly_choose(&r,&s) || !nyr_poly_legal(&r,s)) return false;
  r.access[0].offset=r.access[1].offset; r.access[0].offset.base=0;
  return nyr_poly_choose(&r,&s) && s.interchange && !s.tile &&
         nyr_poly_legal(&r,s);
}

/* Wavefront schedules preserve forward north/west dependences, but not
 * arbitrary backwards-column distances. Exercise clipped rectangles,
 * triangles, descending IVs and every pair of bounded runtime extents. */
static bool wavefront_tests(void) {
  unsigned count=0;
  nyr_poly_schedule_t s={.wavefront=true};
  for (unsigned axis=0;axis<2;++axis) for (unsigned skew=1;skew<=8;++skew) {
  s.skew=skew;s.interchange=axis!=0;
  for (unsigned shape=0;shape<3;++shape) {
    for (unsigned variant=0;variant<3;++variant) {
      if (shape&&variant) continue;
      nyr_func_t f;nyr_poly_region_t r;
      build(&f,5,shape?5:7,shape,false,variant==1?4:0,variant==1?6:0,
            variant==1?-1:variant==2?2:1,variant==1?-1:1);
      int64_t before=0,after=0;
      bool ok=nyr_opt_verify(&f,"wavefront/input")&&nyr_poly_detect(&f,0,&r)&&
        evaluate(&f,&before)&&nyr_poly_emit(&f,&r,s)&&
        nyr_opt_verify(&f,"wavefront/output")&&evaluate(&f,&after)&&before==after;
      nyr_ir_func_free(&f);if (!ok) return false;++count;
    }
  }
  nyr_func_t f,original={0};nyr_poly_region_t r;
  build_with_bounds(&f,8,8,0,false,0,0,1,1,true,0);
  bool ok=nyr_ir_func_clone(&f,&original)&&nyr_poly_detect(&f,0,&r)&&nyr_poly_emit(&f,&r,s);
  for (int seed=0;ok&&seed<64;++seed) {
    int64_t a=0,b=0;
    ok=evaluate_seed(&original,seed,&a)&&evaluate_seed(&f,seed,&b)&&a==b;++count;
  }
  nyr_ir_func_free(&f);nyr_ir_func_free(&original);
  if (!ok) return false;
  }
  s.skew=1;s.interchange=false;
  /* A large north/west stencil has a legal diagonal schedule. A
   * previous-row, two-columns-forward dependence reverses that order. */
  nyr_poly_region_t r={.extent={512,512},.access_count=3,
    .access={{{.coeff={8192,8},.constant=8192,.base=1},8,true},
             {{.coeff={8192,8},.constant=0,.base=1},8,false},
             {{.coeff={8192,8},.constant=8184,.base=1},8,false}}};
  if (!nyr_poly_legal(&r,s)) return false;
  r.access[1].offset.constant=16;
  if (nyr_poly_legal(&r,s)) return false;
  if (!nyr_poly_find_wavefront(&r,&s)||s.skew!=3||
      !nyr_poly_parallel_band(&r,s,1)||nyr_poly_parallel_band(&r,s,0)) return false;
  printf("polyhedral: %u wavefront execution comparisons; stencil dependence checks passed\n",count);
  return true;
}

/* Check every pair of runtime extents in [0,7] under all candidate forms,
 * including zero-trip rectangles and partial tiles. Dependences are proved
 * for the maximum rectangle, while codegen must respect each actual bound. */
static bool symbolic_bounds(void) {
  unsigned count=0;
  for (unsigned swap=0;swap<2;++swap)
    for (unsigned tile=0;tile<5;++tile) {
      nyr_func_t f,original={0}; nyr_poly_region_t r;
      build_with_bounds(&f,8,8,0,false,0,0,1,1,true,0);
      nyr_poly_schedule_t s={.interchange=swap!=0,.tile=tile,
        .inner_tile=tile==4?1:tile?tile+1:0};
      bool ok=nyr_opt_verify(&f,"symbolic-input") &&
        nyr_ir_func_clone(&f,&original) && nyr_poly_detect(&f,0,&r) &&
        r.symbolic[0] && r.symbolic[1] && r.extent[0]==7 && r.extent[1]==7 &&
        nyr_poly_emit(&f,&r,s) && nyr_opt_verify(&f,"symbolic-output");
      for (int seed=0;ok && seed<64;++seed) {
        int64_t before=0,after=0;
        ok=evaluate_seed(&original,seed,&before) &&
           evaluate_seed(&f,seed,&after) && before==after;
        ++count;
      }
      nyr_ir_func_free(&original);nyr_ir_func_free(&f);
      if (!ok) {fprintf(stderr,"symbolic bound mismatch %u/%u\n",swap,tile);return false;}
    }
  printf("polyhedral: %u symbolic-bound execution comparisons\n",count);
  return true;
}

static bool symbolic_shapes(void) {
  unsigned count=0;
  const nyr_poly_schedule_t schedules[]={{0},{.interchange=true},{.tile=3},
    {.tile=2,.inner_tile=1,.interchange=true},{.wavefront=true,.skew=3}};
  for (unsigned variant=0;variant<6;++variant) for (unsigned k=0;k<5;++k) {
    unsigned triangle=variant<2?variant+1:0;
    int step=variant==2?2:variant==3?-1:variant==4?-2:1;
    int start=step<0?7:variant==2?1:variant==5?3:0;
    nyr_func_t f,original={0};nyr_poly_region_t r;
    build_with_bounds(&f,8,8,triangle,false,start,start,step,step,true,0);
    bool ok=nyr_opt_verify(&f,"symbolic-shape/input")&&nyr_ir_func_clone(&f,&original)&&
      nyr_poly_detect(&f,0,&r)&&r.symbolic[0]&&r.symbolic[1]&&r.triangle==triangle&&
      nyr_poly_emit(&f,&r,schedules[k])&&nyr_opt_verify(&f,"symbolic-shape/output");
    for (int seed=0;ok&&seed<64;++seed) {
      int64_t a,b;ok=evaluate_seed(&original,seed,&a)&&evaluate_seed(&f,seed,&b)&&a==b;++count;
    }
    nyr_ir_func_free(&f);nyr_ir_func_free(&original);
    if (!ok) {fprintf(stderr,"symbolic shape mismatch %u/%u\n",variant,k);return false;}
  }
  printf("polyhedral: %u dynamic triangular/strided execution comparisons\n",count);
  return true;
}

/* Independent 3D oracle. Compare schedule tuples directly, without the
 * engine's padded integer-rank implementation or its affine slab proof. */
static bool inverted3(nyr_poly_schedule_t s,const unsigned *a,const unsigned *b) {
  static const unsigned perm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
  if (s.tile)
    for (unsigned q=0;q<3;++q) {
      unsigned d=perm[s.permutation][q];
      if (a[d]/s.tile!=b[d]/s.tile) return a[d]/s.tile>b[d]/s.tile;
    }
  for (unsigned q=0;q<3;++q) {
    unsigned d=perm[s.permutation][q];
    if (a[d]!=b[d]) return a[d]>b[d];
  }
  return false;
}
static int64_t byte3(nyr_poly_access_t a,const unsigned *p) {
  int64_t at=a.offset.constant;
  for (unsigned d=0;d<3;++d) at+=a.offset.coeff[d]*p[d];
  return at;
}
static bool brute3(const nyr_poly_region_t *r,nyr_poly_schedule_t s) {
  unsigned p[64][3],count=0;
  for (unsigned i=0;i<r->extent[0];++i)
    for (unsigned j=0;j<r->extent[1];++j)
      for (unsigned k=0;k<r->extent[2];++k) {
        p[count][0]=i;p[count][1]=j;p[count++][2]=k;
      }
  for (unsigned i=0;i<count;++i)
    for (unsigned j=i+1;j<count;++j) {
      if (!inverted3(s,p[i],p[j])) continue;
      for (size_t x=0;x<r->access_count;++x)
        for (size_t y=0;y<r->access_count;++y) {
          nyr_poly_access_t a=r->access[x],b=r->access[y];
          if (a.offset.base!=b.offset.base||(!a.is_write&&!b.is_write)) continue;
          int64_t u=byte3(a,p[i]),v=byte3(b,p[j]);
          if (u<v+b.width&&v<u+a.width) return false;
        }
    }
  return true;
}
static bool legality3(void) {
  unsigned count=0;
  for (unsigned trial=0;trial<600;++trial) {
    nyr_poly_region_t r={.depth=3,.extent={2+next_random()%3,2+next_random()%3,2+next_random()%3},
      .access_count=1+next_random()%4};
    for (size_t a=0;a<r.access_count;++a) {
      r.access[a]=(nyr_poly_access_t){.offset={.base=(int)(next_random()%2),.constant=128+next_random()%16,
        .coeff={(int)(next_random()%17)-8,(int)(next_random()%17)-8,(int)(next_random()%17)-8}},
        .width=1u<<(next_random()%4),.is_write=next_random()%2};
      if (trial%3==0) {
        r.access[a].offset.constant=0;
        r.access[a].offset.coeff[0]=128;r.access[a].offset.coeff[1]=8;r.access[a].offset.coeff[2]=0;
      }
    }
    if (nyr_poly_permutable(&r,(nyr_poly_schedule_t){0})&&
        !brute_permutable(&r,(nyr_poly_schedule_t){0})) return false;
    for (unsigned perm=0;perm<6;++perm)
      for (unsigned tile=0;tile<=2;tile+=2) {
        nyr_poly_schedule_t s={.permutation=perm,.tile=tile};
        bool expected=brute3(&r,s),symbolic=nyr_poly_symbolic_legal(&r,s);
        if ((symbolic&&!expected)||nyr_poly_legal(&r,s)!=expected) {
          fprintf(stderr,"3D legality mismatch %u/%u/%u\n",trial,perm,tile);return false;
        }
        ++count;
      }
  }
  printf("polyhedral: %u 3D exact and symbolic oracle comparisons each\n",count);
  return true;
}

static void build3_shape(nyr_func_t *f,int ni,int nj,int nk,bool fp,unsigned variant) {
  nyr_opt_begin(f);
  int zero=nyr_opt_const(f,0),one=nyr_opt_const(f,1),eight=nyr_opt_const(f,8);
  int dims[]={nyr_opt_const(f,ni),nyr_opt_const(f,nj),nyr_opt_const(f,nk)};
  int a=nyr_opt_emit_imm(f,NYIR_ALLOCA,ni*nk*8),b=nyr_opt_emit_imm(f,NYIR_ALLOCA,nj*nk*8),
      c=nyr_opt_emit_imm(f,NYIR_ALLOCA,ni*nj*8);
  const int arrays[]={a,b,c}, lengths[]={ni*nk,nj*nk,ni*nj};
  for (unsigned q=0;q<3;++q)
    for (int t=0;t<lengths[q];++t) {
      int off=nyr_opt_const(f,t*8),v=fp?nyr_opt_const_f64(f,(double)(t+q+1)):nyr_opt_const(f,t+q+1);
      store(f,nyr_opt_binop(f,NYIR_ADD_I64,arrays[q],off),v);
      if (fp) f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;
    }
  int64_t h[]={10,12,14},body[]={11,13,15},exit[]={30,20,21};
  int64_t entry[]={9,11,13},back[]={20,21,15};
  int iv[3],next[3];size_t phis[3];
  nyr_opt_label(f,9);
  for (unsigned d=0;d<3;++d) {
    nyr_opt_br(f,h[d]);nyr_opt_label(f,h[d]);
    phis[d]=f->len;iv[d]=f->next_value;
    nyr_opt_phi(f,entry[d],zero,back[d],zero);
    nyr_opt_br_if(f,nyr_opt_cmp(f,iv[d],dims[d],NYIR_CMP_LT),body[d]);
    nyr_opt_br(f,exit[d]);nyr_opt_label(f,body[d]);
  }
  int av=load(f,address(f,a,iv[0],iv[2],dims[2],eight));
  if (fp) f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;
  int bv=load(f,variant==1?address(f,b,iv[1],iv[2],dims[2],eight):
                            address(f,b,iv[2],iv[1],dims[1],eight));
  if (fp) f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;
  int ca=variant==2?c:address(f,c,iv[0],iv[1],dims[1],eight),cv=load(f,ca);
  if (fp) f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;
  int product=nyr_opt_binop(f,fp?NYIR_MUL_F64:NYIR_MUL_I64,av,bv);
  store(f,ca,nyr_opt_binop(f,fp?NYIR_ADD_F64:NYIR_ADD_I64,cv,product));
  if (fp) f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;
  for (unsigned d=3;d-->0;) {
    next[d]=nyr_opt_binop(f,NYIR_ADD_I64,iv[d],one);nyr_opt_br(f,h[d]);
    nyr_opt_label(f,exit[d]);f->data[phis[d]].phi_incoming[1].value=next[d];
  }
  int sum=zero;
  for (int t=0;t<ni*nj;++t) {
    int v=load(f,nyr_opt_binop(f,NYIR_ADD_I64,c,nyr_opt_const(f,t*8)));
    if (fp) {f->data[f->len-1].scalar_class=NYIR_SCALAR_F64;v=nyr_opt_emit_abc(f,NYIR_F64_TO_I64,v,-1,-1);}
    sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,nyr_opt_binop(f,NYIR_MUL_I64,v,nyr_opt_const(f,t+1)));
  }
  nyr_opt_ret(f,sum);
}
static void build3(nyr_func_t *f) {build3_shape(f,3,4,5,false,0);}
static bool regenerated3(void) {
  unsigned count=0;
  for (unsigned perm=0;perm<6;++perm)
    for (unsigned tile=0;tile<4;++tile) {
      nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
      build3(&f);nyr_poly_schedule_t s={.permutation=perm,.tile=tile};
      bool ok=nyr_opt_verify(&f,"3D-before")&&nyr_poly_detect(&f,0,&r)&&r.depth==3&&
        evaluate(&f,&before)&&nyr_poly_legal(&r,s)&&nyr_poly_emit(&f,&r,s)&&
        nyr_opt_verify(&f,"3D-after")&&evaluate(&f,&after)&&before==after;
      nyr_ir_func_free(&f);
      if (!ok) {fprintf(stderr,"3D regeneration mismatch %u/%u %lld!=%lld\n",perm,tile,(long long)before,(long long)after);return false;}
      ++count;
    }
  printf("polyhedral: %u 3D memory-reduction regeneration cases\n",count);
  return true;
}

/* Explicit promotion must introduce a scalar loop-carried PHI and move
 * exactly the recurrence load/store outside the inner loop. */
static bool promotion_tests(void) {
  unsigned count=0;
  for (unsigned perm=0;perm<6;++perm) {
    nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
    build3(&f);
    nyr_poly_schedule_t s={.permutation=perm};
    bool expected=nyr_poly_axis(s,3,2)==2;
    bool ok=nyr_opt_verify(&f,"promote/input")&&nyr_poly_detect(&f,0,&r)&&
      evaluate(&f,&before);
    size_t phis=0;
    for (size_t i=0;i<f.len;++i) phis+=f.data[i].op==NYIR_PHI;
    size_t len=f.len;
    if (ok) ok=nyr_poly_emit_promoted(&f,&r,s)==expected;
    if (expected) {
      size_t after_phis=0;
      for (size_t i=0;i<f.len;++i) after_phis+=f.data[i].op==NYIR_PHI;
      ok=ok&&after_phis==phis+1;
    } else ok=ok&&f.len==len;
    ok=ok&&nyr_opt_verify(&f,"promote/output")&&evaluate(&f,&after)&&before==after;
    nyr_ir_func_free(&f);if (!ok) {fprintf(stderr,"promotion failure %u\n",perm);return false;}++count;
  }
  printf("polyhedral: %u private recurrence promotion acceptance/rejection cases\n",count);
  return true;
}

static bool blocked_tests(void) {
  unsigned count=0,accepted=0,tails=0;
  bool old=nyr_fast_math();
  for (unsigned fp=0;fp<2;++fp) {
    nyr_set_fast_math(fp!=0);
    for (unsigned variant=0;variant<3;++variant)
      for (unsigned n=2;n<=9;++n) for (unsigned perm=0;perm<6;++perm)
        for (unsigned lanes=1;lanes<=8;++lanes) {
          nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
          build3_shape(&f,n,n+1,5,fp!=0,variant);
          nyr_poly_schedule_t s={.permutation=perm};
          bool ok=nyr_opt_verify(&f,"block/input")&&nyr_poly_detect(&f,0,&r)&&evaluate(&f,&before);
          unsigned mid=nyr_poly_axis(s,3,1),inner=nyr_poly_axis(s,3,2);
          bool expected=ok&&r.extent[mid]>=lanes&&
            (variant==2?lanes==1:inner==2)&&nyr_poly_legal(&r,s);
          size_t len=f.len,phis=0;
          for (size_t i=0;i<f.len;++i) phis+=f.data[i].op==NYIR_PHI;
          bool got=ok&&nyr_poly_emit_blocked(&f,&r,s,lanes);
          ok=ok&&got==expected;
          if (got) {
            size_t after_phis=0;
            for (size_t i=0;i<f.len;++i) after_phis+=f.data[i].op==NYIR_PHI;
            ok=ok&&after_phis==phis+lanes+(r.extent[mid]%lanes?3:0);++accepted;
            tails+=r.extent[mid]%lanes!=0;
          } else ok=ok&&f.len==len;
          ok=ok&&nyr_opt_verify(&f,"block/output")&&evaluate(&f,&after)&&before==after;
          nyr_ir_func_free(&f);++count;
          if (!ok) {fprintf(stderr,"blocked recurrence mismatch %u/%u/%u/%u/%u got=%d want=%d\n",fp,variant,n,perm,lanes,got,expected);nyr_set_fast_math(old);return false;}
        }
  }
  nyr_set_fast_math(old);
  printf("polyhedral: %u register blocking acceptance/rejection cases (%u applied, %u with scalar remainder)\n",count,accepted,tails);
  return true;
}

/* SIMD must really widen the recurrence and retain exact scalar tails.
 * A strided input, shared output, integer recurrence or short block is
 * deliberately rejected; the original IR must remain executable. */
static bool vector_block_tests(void) {
  unsigned count=0,accepted=0;
  bool old=nyr_fast_math();nyr_set_fast_math(true);
  for (unsigned fp=0;fp<2;++fp) for (unsigned variant=0;variant<3;++variant)
    for (unsigned n=2;n<=11;++n) for (unsigned lanes=4;lanes<=8;lanes+=4) {
      nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
      build3_shape(&f,3,n,5,fp!=0,variant);
      bool ok=nyr_opt_verify(&f,"vector-block/input")&&nyr_poly_detect(&f,0,&r)&&evaluate(&f,&before);
      bool expected=fp&&variant==0&&n>=lanes;
      size_t len=f.len;
      bool got=ok&&nyr_poly_emit_vector_blocked_lanes(&f,&r,(nyr_poly_schedule_t){0},lanes);
      ok=ok&&got==expected;
      if (got) {
        unsigned loads=0,stores=0,muls=0,adds=0;
        for (size_t p=0;p<f.len;++p) {
          loads+=f.data[p].op==NYIR_VEC4_LOAD_F64;
          stores+=f.data[p].op==NYIR_VEC4_STORE_F64;
          muls+=f.data[p].op==NYIR_VEC4_MUL_F64;
          adds+=f.data[p].op==NYIR_VEC4_ADD_F64;
        }
        ok=ok&&loads==lanes/2&&stores==lanes/4&&muls==lanes/4&&adds==lanes/4;++accepted;
      } else ok=ok&&f.len==len;
      ok=ok&&nyr_opt_verify(&f,"vector-block/output")&&evaluate(&f,&after)&&before==after;
      nyr_ir_func_free(&f);++count;
      if (!ok) {fprintf(stderr,"vector block mismatch %u/%u/%u got=%d want=%d before=%lld after=%lld\n",fp,variant,n,got,expected,(long long)before,(long long)after);nyr_set_fast_math(old);return false;}
    }
  nyr_set_fast_math(old);
  printf("polyhedral: %u SIMD block acceptance/rejection cases (%u applied)\n",count,accepted);
  return true;
}

/* Four-wide reduction lanes are legal only for an additive accumulator
 * with a unit-stride or invariant input. Exercise each tail size, all axis
 * orders, strict FP, and non-additive updates. The evaluator also checks
 * the actual packed reduction with a nonzero initial accumulator. */
static bool horizontal_tests(void) {
  bool old=nyr_fast_math();unsigned count=0,accepted=0;
  for (unsigned fast=0;fast<2;++fast) for (unsigned variant=0;variant<3;++variant)
    for (unsigned nk=3;nk<=19;++nk) for (unsigned perm=0;perm<6;++perm)
      for (unsigned groups=1;groups<=4;groups*=2) {
        nyr_set_fast_math(true);
        nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
        build3_shape(&f,3,5,nk,true,variant);
        bool ok=nyr_opt_verify(&f,"horizontal/input")&&nyr_poly_detect(&f,0,&r)&&evaluate(&f,&before);
        nyr_poly_schedule_t s={.permutation=perm};
        unsigned inner=nyr_poly_axis(s,3,2);
        bool expected=ok&&fast&&variant==1&&inner==2&&groups<=2&&nk>=4*groups;
        size_t len=f.len;
        nyr_set_fast_math(fast!=0);
        bool got=ok&&nyr_poly_emit_horizontal(&f,&r,s,groups);
        ok=ok&&got==expected;
        if (got) {
          unsigned loads=0,reduces=0,stores=0;
          for (size_t p=0;p<f.len;++p) {
            loads+=f.data[p].op==NYIR_VEC4_LOAD_F64;
            reduces+=f.data[p].op==NYIR_VEC4_REDUCE_ADD_F64;
            stores+=f.data[p].op==NYIR_VEC4_STORE_F64;
          }
          ok=ok&&loads==2*(groups+(nk%(4*groups))/4)&&reduces==1&&!stores;++accepted;
        } else ok=ok&&f.len==len;
        ok=ok&&nyr_opt_verify(&f,"horizontal/output")&&evaluate(&f,&after)&&before==after;
        nyr_ir_func_free(&f);++count;
        if (!ok) {fprintf(stderr,"horizontal mismatch %u/%u/%u/%u/%u got=%d want=%d before=%lld after=%lld\n",fast,variant,nk,perm,groups,got,expected,(long long)before,(long long)after);nyr_set_fast_math(old);return false;}
      }
  /* Non-additive recurrences and a recurrence using its old value twice
   * cannot be combined by horizontal addition. A rejected candidate
   * must leave the original function executable and unmodified. */
  nyr_set_fast_math(true);
  for (unsigned mode=0;mode<3;++mode) {
    nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
    build3_shape(&f,3,5,9,true,1);
    bool ok=nyr_poly_detect(&f,0,&r);
    if (ok) for (size_t p=r.body_begin;p<r.body_end;++p)
      if (f.data[p].op==NYIR_ADD_F64) {
        if (mode==0) f.data[p].op=NYIR_SUB_F64;
        else if (mode==1) f.data[p].op=NYIR_MUL_F64;
        else f.data[p].b=f.data[p].a;
      }
    size_t len=f.len;
    ok=ok&&nyr_opt_verify(&f,"horizontal/negative")&&evaluate(&f,&before)&&
       !nyr_poly_emit_horizontal(&f,&r,(nyr_poly_schedule_t){0},1)&&f.len==len&&
       nyr_opt_verify(&f,"horizontal/negative-unchanged")&&evaluate(&f,&after)&&before==after;
    nyr_ir_func_free(&f);++count;
    if (!ok) {nyr_set_fast_math(old);return false;}
  }
  nyr_set_fast_math(old);
  printf("polyhedral: %u horizontal SIMD reduction cases (%u applied)\n",count,accepted);
  return true;
}

/* Cross-statement oracle: original A completes before B starts, even
 * when their iteration coordinates are equal or reversed. */
static bool fusion_brute(const nyr_poly_region_t *a,const nyr_poly_region_t *b,
                         nyr_poly_schedule_t s) {
  if (!brute(a,s)||!brute(b,s)) return false;
  for (unsigned i=0;i<a->extent[0];++i) for (unsigned j=0;j<a->extent[1];++j)
    if (nyr_poly_contains(a,i,j))
      for (unsigned x=0;x<b->extent[0];++x) for (unsigned y=0;y<b->extent[1];++y) {
        if (!nyr_poly_contains(b,x,y)||order(s,i,j,x,y)<=0) continue;
        for (size_t p=0;p<a->access_count;++p) for (size_t q=0;q<b->access_count;++q) {
          nyr_poly_access_t u=a->access[p],v=b->access[q];
          if (u.offset.base!=v.offset.base||(!u.is_write&&!v.is_write)) continue;
          int64_t l=byte_at(u,i,j),h=byte_at(v,x,y);
          if (l<h+v.width&&h<l+u.width) return false;
        }
      }
  return true;
}
static bool fusion_legality(void) {
  unsigned count=0,proved=0,possible=0;
  const nyr_poly_schedule_t schedules[]={{0},{.interchange=true},{.tile=2},
     {.tile=3,.inner_tile=1},{.wavefront=true}};
  for (unsigned trial=0;trial<1000;++trial) {
    nyr_poly_region_t a={.extent={2+next_random()%5,2+next_random()%5},
      .triangle=next_random()%3,.access_count=2},b;
    if (a.triangle) a.extent[1]=a.extent[0];b=a;
    nyr_poly_region_t *r[]={&a,&b};
    for (unsigned k=0;k<2;++k) for (unsigned j=0;j<2;++j) {
      r[k]->access[j]=(nyr_poly_access_t){.offset={.base=next_random()%2,
        .constant=128+next_random()%24,.coeff={(int)(next_random()%17)-8,(int)(next_random()%17)-8}},
        .width=1u<<(next_random()%4),.is_write=next_random()%2};
    }
    for (unsigned k=0;k<sizeof(schedules)/sizeof(schedules[0]);++k) {
      bool expected=fusion_brute(&a,&b,schedules[k]);
      bool got=nyr_poly_fusion_legal(&a,&b,schedules[k]);
      if (got&&!expected) {fprintf(stderr,"fusion legality mismatch %u/%u\n",trial,k);return false;}
      possible+=expected;proved+=got;++count;
    }
  }
  printf("polyhedral: %u multi-statement fusion oracle comparisons; proved %u/%u legal schedules\n",count,proved,possible);
  return true;
}
static void build_fusion(nyr_func_t *f,bool shifted,bool side_entry,unsigned triangle,bool narrow_clobber) {
  nyr_opt_begin(f);
  int zero=nyr_opt_const(f,0),one=nyr_opt_const(f,1),eight=nyr_opt_const(f,8),n=nyr_opt_const(f,5);
  int a=nyr_opt_emit_imm(f,NYIR_ALLOCA,25*8),b=nyr_opt_emit_imm(f,NYIR_ALLOCA,25*8);
  for (int t=0;t<25;++t) {
    int off=nyr_opt_const(f,t*8),v=nyr_opt_const(f,t+1);
    store(f,nyr_opt_binop(f,NYIR_ADD_I64,a,off),v);
    store(f,nyr_opt_binop(f,NYIR_ADD_I64,b,off),zero);
  }
  int64_t entry=9;
  nyr_opt_label(f,entry);
  if (side_entry) { /* extra predecessor of first exit, even if never taken */
    nyr_opt_br_if(f,zero,30);nyr_opt_label(f,8);entry=8;
  }
  for (unsigned k=0;k<2;++k) {
    int64_t h=10+40*k,inner=h+2,body=h+3,latch=h+10,exit=h+20;
    nyr_opt_br(f,h);nyr_opt_label(f,h);
    size_t oi=f->len;int i=f->next_value;
    nyr_opt_phi(f,entry,zero,latch,zero);
    nyr_opt_br_if(f,nyr_opt_cmp(f,i,n,NYIR_CMP_LT),h+1);nyr_opt_br(f,exit);
    nyr_opt_label(f,h+1);nyr_opt_br(f,inner);nyr_opt_label(f,inner);
    size_t ij=f->len;int j=f->next_value;
    nyr_opt_phi(f,h+1,zero,body,zero);
    nyr_opt_br_if(f,nyr_opt_cmp(f,j,triangle?i:n,triangle==2?NYIR_CMP_LE:NYIR_CMP_LT),body);nyr_opt_br(f,latch);
    nyr_opt_label(f,body);
    int x=address(f,a,i,j,n,eight);
    if (!k) store(f,x,nyr_opt_binop(f,NYIR_ADD_I64,load(f,x),one));
    else {
      /* Transpose the read in the negative case: some B instances need
       * values from later A instances, so fusion must reject it. */
      if (narrow_clobber) {
        store(f,x,nyr_opt_const(f,255));
        f->data[f->len-1].flags|=NYIR_INST_F_MEM_BYTE;
      }
      int value=load(f,shifted?address(f,a,j,i,n,eight):x);
      store(f,address(f,b,i,j,n,eight),nyr_opt_binop(f,NYIR_MUL_I64,value,eight));
    }
    int next=nyr_opt_binop(f,NYIR_ADD_I64,j,one);nyr_opt_br(f,inner);
    nyr_opt_label(f,latch);int ni=nyr_opt_binop(f,NYIR_ADD_I64,i,one);nyr_opt_br(f,h);
    nyr_opt_label(f,exit);f->data[oi].phi_incoming[1].value=ni;f->data[ij].phi_incoming[1].value=next;
    entry=exit;
  }
  int sum=zero;
  for (int t=0;t<25;++t) {
    int off=nyr_opt_const(f,t*8),v=load(f,nyr_opt_binop(f,NYIR_ADD_I64,b,off));
    sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,nyr_opt_binop(f,NYIR_MUL_I64,v,nyr_opt_const(f,t+1)));
  }
  nyr_opt_ret(f,sum);
}
static bool fusion_regeneration(void) {
  const nyr_poly_schedule_t schedules[]={{0},{.interchange=true},{.tile=2},
    {.tile=3,.inner_tile=1},{.wavefront=true}};
  unsigned count=0;
  for (unsigned shape=0;shape<3;++shape) for (unsigned k=0;k<sizeof(schedules)/sizeof(schedules[0]);++k) {
    nyr_func_t f;nyr_poly_region_t a,b;int64_t before=0,after=0;
    build_fusion(&f,false,false,shape,false);
    bool ok=nyr_opt_verify(&f,"fusion-input")&&nyr_poly_detect(&f,0,&a)&&
      nyr_poly_detect(&f,a.end,&b)&&nyr_poly_adjacent(&f,&a,&b)&&evaluate(&f,&before)&&
      nyr_poly_emit_fused(&f,&a,&b,schedules[k])&&nyr_opt_verify(&f,"fusion-output")&&
      evaluate(&f,&after)&&before==after;
    if (ok) {
      unsigned loads=0;
      for (size_t p=0;p<f.len;++p) loads+=f.data[p].op==NYIR_LOAD_I64;
      /* A's load plus the 25 checksum loads; B's reload is gone. */
      ok=loads==26;
    }
    nyr_scev_info_t loops={0};
    if (ok) ok=nyr_scev_analyze(&f,&loops)&&loops.count==(schedules[k].tile?(schedules[k].inner_tile==1?3u:4u):2u);
    nyr_scev_free(&loops);nyr_ir_func_free(&f);if (!ok) {fprintf(stderr,"fusion regeneration mismatch %u/%u\n",shape,k);return false;}++count;
  }
  for (unsigned k=0;k<2;++k) {
    nyr_func_t f;nyr_poly_region_t a,b;build_fusion(&f,k==0,k==1,0,false);
    bool ok=nyr_opt_verify(&f,"fusion-reject")&&nyr_poly_detect(&f,0,&a)&&nyr_poly_detect(&f,a.end,&b)&&
      !nyr_poly_emit_fused(&f,&a,&b,(nyr_poly_schedule_t){0})&&nyr_opt_verify(&f,"fusion-reject-unchanged");
    nyr_ir_func_free(&f);if (!ok) return false;
  }
  /* A partial store must invalidate the whole-word forwarding group. */
  for (unsigned k=0;k<sizeof(schedules)/sizeof(schedules[0]);++k) {
    nyr_func_t f;nyr_poly_region_t a,b;int64_t before=0,after=0;
    build_fusion(&f,false,false,0,true);
    bool ok=nyr_opt_verify(&f,"partial-before")&&nyr_poly_detect(&f,0,&a)&&
      nyr_poly_detect(&f,a.end,&b)&&evaluate(&f,&before)&&
      nyr_poly_emit_fused(&f,&a,&b,schedules[k])&&nyr_opt_verify(&f,"partial-after")&&
      evaluate(&f,&after)&&before==after;
    unsigned loads=0;for (size_t p=0;p<f.len;++p) loads+=f.data[p].op==NYIR_LOAD_I64;
    nyr_ir_func_free(&f);if (!ok||loads!=27) return false;
  }
  puts("polyhedral: permutable-band oracle checks (including transposed wavefronts), 15 forwarded loads, 5 narrow clobber cases");
  printf("polyhedral: %u fused region execution comparisons; dependence and side-entry rejection passed\n",count);
  return true;
}

/* A distinct private producer array has no uses before/after the two
 * stages. Negative cases keep it live after the pair, read a different
 * element, or use the pointer as a scalar value. */
static void build_inline_stage(nyr_func_t *f,unsigned n,unsigned shape,unsigned variant) {
  nyr_opt_begin(f);f->untagged_values=true;
  int zero=nyr_opt_const(f,0),one=nyr_opt_const(f,1),eight=nyr_opt_const(f,8);
  int bound=nyr_opt_const(f,n);
  int temp=nyr_opt_emit_imm(f,NYIR_ALLOCA,n*n*8);
  int middle=variant>=6?nyr_opt_emit_imm(f,NYIR_ALLOCA,n*n*8):-1;
  int out=nyr_opt_emit_imm(f,NYIR_ALLOCA,n*n*8);
  for (unsigned t=0;t<n*n;++t)
    store(f,nyr_opt_binop(f,NYIR_ADD_I64,out,nyr_opt_const(f,t*8)),zero);
  int64_t entry=9;nyr_opt_label(f,entry);
  for (unsigned k=0;k<(variant>=6?3u:2u);++k) {
    int64_t h=10+40*k,inner=h+2,body=h+3,latch=h+10,exit=h+20;
    nyr_opt_br(f,h);nyr_opt_label(f,h);
    size_t oi=f->len;int i=f->next_value;nyr_opt_phi(f,entry,zero,latch,zero);
    nyr_opt_br_if(f,nyr_opt_cmp(f,i,bound,NYIR_CMP_LT),h+1);nyr_opt_br(f,exit);
    nyr_opt_label(f,h+1);nyr_opt_br(f,inner);nyr_opt_label(f,inner);
    size_t ij=f->len;int j=f->next_value;nyr_opt_phi(f,h+1,zero,body,zero);
    nyr_opt_br_if(f,nyr_opt_cmp(f,j,shape?i:bound,shape==2?NYIR_CMP_LE:NYIR_CMP_LT),body);nyr_opt_br(f,latch);
    nyr_opt_label(f,body);
    int x=address(f,k==2?middle:temp,i,j,bound,eight);
    if (!k) {
      int value=nyr_opt_binop(f,NYIR_ADD_I64,nyr_opt_binop(f,NYIR_MUL_I64,i,eight),j);
      store(f,x,variant==3?temp:value);
      if (variant==4) f->data[f->len-1].flags|=NYIR_INST_F_MEM_BYTE;
    } else {
      int v=load(f,variant==2?address(f,temp,j,i,bound,eight):x);
      if (variant==5) v=nyr_opt_binop(f,NYIR_ADD_I64,v,temp);
      else v=nyr_opt_binop(f,NYIR_ADD_I64,v,load(f,x));
      store(f,address(f,k==1&&middle>=0?middle:out,i,j,bound,eight),v);
    }
    int next=nyr_opt_binop(f,NYIR_ADD_I64,j,one);nyr_opt_br(f,inner);
    nyr_opt_label(f,latch);int ni=nyr_opt_binop(f,NYIR_ADD_I64,i,one);nyr_opt_br(f,h);
    nyr_opt_label(f,exit);f->data[oi].phi_incoming[1].value=ni;f->data[ij].phi_incoming[1].value=next;entry=exit;
  }
  int sum=zero;
  for (unsigned t=0;t<n*n;++t) {
    int off=nyr_opt_const(f,t*8),v=load(f,nyr_opt_binop(f,NYIR_ADD_I64,out,off));
    sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,nyr_opt_binop(f,NYIR_MUL_I64,v,nyr_opt_const(f,t+1)));
  }
  if (variant==1) sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,load(f,temp));
  if (variant==7) sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,load(f,middle));
  if (variant==8) sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,load(f,temp));
  nyr_opt_ret(f,sum);
}
static bool inline_stage_tests(void) {
  const nyr_poly_schedule_t schedules[]={{0},{.interchange=true},{.tile=2},
    {.tile=3,.inner_tile=1},{.wavefront=true,.skew=2},
    {.wavefront=true,.interchange=true,.skew=3}};
  unsigned count=0;
  for (unsigned n=2;n<=9;++n) for (unsigned shape=0;shape<3;++shape)
    for (unsigned k=0;k<sizeof(schedules)/sizeof(*schedules);++k) {
      nyr_func_t f;nyr_poly_region_t a,b;int64_t before=0,after=0;
      build_inline_stage(&f,n,shape,0);
      bool ok=nyr_opt_verify(&f,"inline-stage-before")&&nyr_poly_detect(&f,0,&a)&&
        nyr_poly_detect(&f,a.end,&b)&&nyr_poly_inline_root(&f,&a,&b)>=0&&
        evaluate(&f,&before)&&nyr_poly_emit_fused(&f,&a,&b,schedules[k])&&
        nyr_opt_verify(&f,"inline-stage-after")&&evaluate(&f,&after)&&before==after;
      unsigned allocs=0;for (size_t p=0;p<f.len;++p) allocs+=f.data[p].op==NYIR_ALLOCA;
      nyr_ir_func_free(&f);
      if (!ok||allocs!=1) {fprintf(stderr,"inline stage mismatch n=%u shape=%u schedule=%u allocations=%u\n",n,shape,k,allocs);return false;}
      ++count;
    }
  for (unsigned v=1;v<=5;++v) {
    nyr_func_t f;nyr_poly_region_t a,b;build_inline_stage(&f,5,0,v);
    bool ok=nyr_opt_verify(&f,"inline-stage-reject")&&nyr_poly_detect(&f,0,&a)&&
      nyr_poly_detect(&f,a.end,&b)&&nyr_poly_inline_root(&f,&a,&b)<0;
    nyr_ir_func_free(&f);if (!ok) {fprintf(stderr,"inline rejection %u\n",v);return false;}
  }
  printf("polyhedral: %u private producer elimination cases; 5 observable/shifted/narrow/escaped cases rejected\n",count);
  return true;
}

static bool chain_tests(void) {
  const nyr_poly_schedule_t schedules[]={{0},{.interchange=true},{.tile=2},
    {.tile=3,.inner_tile=1},{.wavefront=true,.skew=2},
    {.wavefront=true,.interchange=true,.skew=3}};
  unsigned count=0;
  for (unsigned n=2;n<=9;++n) for (unsigned shape=0;shape<3;++shape)
    for (unsigned k=0;k<sizeof(schedules)/sizeof(*schedules);++k) {
      nyr_func_t f;nyr_poly_region_t a,b,c;int64_t before=0,after=0;
      build_inline_stage(&f,n,shape,6);
      bool ok=nyr_opt_verify(&f,"chain-before")&&nyr_poly_detect(&f,0,&a)&&
        nyr_poly_detect(&f,a.end,&b)&&nyr_poly_detect(&f,b.end,&c)&&
        evaluate(&f,&before)&&nyr_poly_emit_chain(&f,&a,&b,&c,schedules[k])&&
        nyr_opt_verify(&f,"chain-after")&&evaluate(&f,&after)&&before==after;
      unsigned allocs=0;for (size_t p=0;p<f.len;++p) allocs+=f.data[p].op==NYIR_ALLOCA;
      nyr_ir_func_free(&f);
      if (!ok||allocs!=1) {fprintf(stderr,"chain mismatch n=%u shape=%u schedule=%u allocations=%u\n",n,shape,k,allocs);return false;}
      ++count;
    }
  for (unsigned v=7;v<=8;++v) {
    nyr_func_t f;nyr_poly_region_t a,b,c;build_inline_stage(&f,5,0,v);
    size_t len=f.len;
    bool ok=nyr_opt_verify(&f,"chain-reject")&&nyr_poly_detect(&f,0,&a)&&
      nyr_poly_detect(&f,a.end,&b)&&nyr_poly_detect(&f,b.end,&c)&&
      !nyr_poly_emit_chain(&f,&a,&b,&c,(nyr_poly_schedule_t){0})&&f.len==len&&
      nyr_opt_verify(&f,"chain-unchanged");
    nyr_ir_func_free(&f);if (!ok) return false;
  }
  printf("polyhedral: %u three-stage storage elimination comparisons; two observable intermediates rejected\n",count);
  return true;
}

/* Producer and consumer domains are deliberately different. The producer
 * covers a dense rectangle; the consumer uses forward/backward translated
 * coordinates, including a nonzero original IV start. */
static void build_recompute(nyr_func_t *f,unsigned n,bool transpose,unsigned variant) {
  nyr_opt_begin(f);f->untagged_values=true;
  int zero=nyr_opt_const(f,0),one=nyr_opt_const(f,1),eight=nyr_opt_const(f,8);
  int bound=nyr_opt_const(f,n),short_bound=nyr_opt_const(f,n-1);
  int temp=nyr_opt_emit_imm(f,NYIR_ALLOCA,(n*n+1)*8);
  int out=nyr_opt_emit_imm(f,NYIR_ALLOCA,n*n*8);
  int input=variant==7||variant==8?nyr_opt_emit_imm(f,NYIR_ALLOCA,n*n*8):-1;
  if (input>=0) for (unsigned t=0;t<n*n;++t)
    store(f,nyr_opt_binop(f,NYIR_ADD_I64,input,nyr_opt_const(f,t*8)),nyr_opt_const(f,t*13+5));
  for (unsigned t=0;t<n*n;++t)
    store(f,nyr_opt_binop(f,NYIR_ADD_I64,out,nyr_opt_const(f,t*8)),zero);
  int64_t entry=9;nyr_opt_label(f,entry);
  for (unsigned k=0;k<2;++k) {
    int64_t h=10+40*k,inner=h+2,body=h+3,latch=h+10,exit=h+20;
    int ilim=k&&variant==2?short_bound:bound;
    int jlim=k&&(variant==0||variant==2||variant==3||variant==4||variant==6||variant==7||variant==8)?short_bound:bound;
    int istart=k&&variant==1?one:zero;
    nyr_opt_br(f,h);nyr_opt_label(f,h);
    size_t oi=f->len;int i=f->next_value;nyr_opt_phi(f,entry,istart,latch,zero);
    nyr_opt_br_if(f,nyr_opt_cmp(f,i,ilim,NYIR_CMP_LT),h+1);nyr_opt_br(f,exit);
    nyr_opt_label(f,h+1);nyr_opt_br(f,inner);nyr_opt_label(f,inner);
    size_t ij=f->len;int j=f->next_value;nyr_opt_phi(f,h+1,zero,body,zero);
    /* Test CLooG-style header literal extraction as well. */
    if (k&&variant==3) jlim=nyr_opt_const(f,n-1);
    nyr_opt_br_if(f,nyr_opt_cmp(f,j,jlim,NYIR_CMP_LT),body);nyr_opt_br(f,latch);
    nyr_opt_label(f,body);
    int x=address(f,temp,transpose?j:i,transpose?i:j,bound,eight);
    if (!k) {
      int value=nyr_opt_binop(f,NYIR_ADD_I64,nyr_opt_binop(f,NYIR_MUL_I64,i,eight),j);
      if (variant==6) value=nyr_opt_binop(f,NYIR_ADD_I64,value,load(f,address(f,out,i,j,bound,eight)));
      if (input>=0) value=nyr_opt_binop(f,NYIR_ADD_I64,value,load(f,address(f,input,i,j,bound,eight)));
      store(f,x,value);
    } else {
      int u=i,v=j;
      if (variant==1) u=nyr_opt_binop(f,NYIR_SUB_I64,i,one);
      else {
        v=nyr_opt_binop(f,NYIR_ADD_I64,j,one);
        if (variant==2) u=nyr_opt_binop(f,NYIR_ADD_I64,i,one);
      }
      int y=address(f,temp,transpose?v:u,transpose?u:v,bound,eight);
      int a=load(f,x),b=load(f,y);
      int value=nyr_opt_binop(f,NYIR_ADD_I64,a,b);
      if (variant==3) value=nyr_opt_binop(f,NYIR_ADD_I64,value,load(f,y));
      store(f,address(f,out,i,j,bound,eight),value);
      if (variant==8) store(f,address(f,input,i,j,bound,eight),value);
    }
    int next=nyr_opt_binop(f,NYIR_ADD_I64,j,one);nyr_opt_br(f,inner);
    nyr_opt_label(f,latch);int ni=nyr_opt_binop(f,NYIR_ADD_I64,i,one);nyr_opt_br(f,h);
    nyr_opt_label(f,exit);f->data[oi].phi_incoming[1].value=ni;f->data[ij].phi_incoming[1].value=next;entry=exit;
  }
  int sum=zero;
  for (unsigned t=0;t<n*n;++t) {
    int off=nyr_opt_const(f,t*8),v=load(f,nyr_opt_binop(f,NYIR_ADD_I64,out,off));
    sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,nyr_opt_binop(f,NYIR_MUL_I64,v,nyr_opt_const(f,t+1)));
  }
  if (variant==4) sum=nyr_opt_binop(f,NYIR_ADD_I64,sum,load(f,temp));
  nyr_opt_ret(f,sum);
}
static bool recompute_tests(void) {
  const nyr_poly_schedule_t schedules[]={{0},{.interchange=true},{.tile=2},
    {.tile=3,.inner_tile=1},{.wavefront=true,.skew=2},
    {.wavefront=true,.interchange=true,.skew=3}};
  unsigned count=0;
  for (unsigned n=3;n<=8;++n) for (unsigned t=0;t<2;++t) for (unsigned case_index=0;case_index<5;++case_index)
    for (unsigned k=0;k<sizeof(schedules)/sizeof(*schedules);++k) {
      unsigned v=case_index==4?7:case_index;
      nyr_func_t f;nyr_poly_region_t a,b;int64_t before=0,after=0;
      build_recompute(&f,n,t,v);
      bool ok=nyr_opt_verify(&f,"recompute-before")&&nyr_poly_detect_pair(&f,0,&a,&b)&&b.depth&&
        evaluate(&f,&before)&&nyr_poly_emit_recomputed(&f,&a,&b,schedules[k])&&
        nyr_opt_verify(&f,"recompute-after")&&evaluate(&f,&after)&&before==after;
      unsigned allocs=0;for (size_t p=0;p<f.len;++p) allocs+=f.data[p].op==NYIR_ALLOCA;
      nyr_ir_func_free(&f);
      if (!ok||allocs!=(v==7?2u:1u)) {fprintf(stderr,"recompute mismatch n=%u transpose=%u case=%u schedule=%u allocations=%u\n",n,t,v,k,allocs);return false;}
      ++count;
    }
  for (unsigned v=4;v<=8;++v) {
    if (v==7) continue;
    nyr_func_t f;nyr_poly_region_t a,b;build_recompute(&f,5,false,v);
    bool ok=nyr_opt_verify(&f,"recompute-reject")&&nyr_poly_detect_pair(&f,0,&a,&b)&&b.depth&&
      !nyr_poly_emit_recomputed(&f,&a,&b,(nyr_poly_schedule_t){0})&&nyr_opt_verify(&f,"recompute-reject-unchanged");
    nyr_ir_func_free(&f);if (!ok) {fprintf(stderr,"recompute rejection %u\n",v);return false;}
  }
  printf("polyhedral: %u shifted producer recomputation cases; live-out, unproduced and clobbered-input producers rejected\n",count);
  return true;
}

static bool domain_extrema(void) {
  unsigned count=0,regenerated_count=0;
  for (unsigned shape=1;shape<=4;++shape)
    for (unsigned n=2;n<=20;++n)
      for (int a=-9;a<=9;++a) for (int b=-9;b<=9;++b) {
        nyr_poly_region_t r={.extent={n,n},.triangle=shape};
        ny_i128 c[3]={a,b,0},lo,hi;
        int64_t l=INT64_MAX,h=INT64_MIN;
        for (unsigned i=0;i<n;++i) for (unsigned j=0;j<n;++j) {
          if (!nyr_poly_contains(&r,i,j)) continue;
          int64_t v=(int64_t)a*i+(int64_t)b*j+3;
          if (v<l) l=v;
          if (v>h) h=v;
        }
        if (!nyr_poly_extrema(&r,c,3,&lo,&hi)||lo!=l||hi!=h) return false;
        ++count;
      }
  for (unsigned shape=1;shape<=4;++shape)
    for (unsigned dynamic=0;dynamic<=1;++dynamic)
      for (unsigned swap=0;swap<=1;++swap) for (unsigned tile=0;tile<=7;++tile) {
        nyr_func_t f,original={0};nyr_poly_region_t r;
        build_with_bounds(&f,8,8,shape,false,0,0,1,1,dynamic!=0,1);
        nyr_poly_schedule_t s=tile>4?(nyr_poly_schedule_t){.wavefront=true,.skew=tile-4,.interchange=swap!=0}:
          (nyr_poly_schedule_t){.interchange=swap!=0,.tile=tile};
        bool ok=nyr_opt_verify(&f,"triangle-diff/input")&&nyr_ir_func_clone(&f,&original)&&
          nyr_poly_detect(&f,0,&r)&&r.triangle==shape&&nyr_poly_emit(&f,&r,s)&&
          nyr_opt_verify(&f,"triangle-diff/output");
        for (unsigned seed=0;ok&&seed<(dynamic?8u:1u);++seed) {
          int64_t x,y;ok=evaluate_seed(&original,seed,&x)&&evaluate_seed(&f,seed,&y)&&x==y;
          ++regenerated_count;
        }
        nyr_ir_func_free(&original);nyr_ir_func_free(&f);
        if (!ok) {fprintf(stderr,"triangle difference mismatch %u/%u/%u/%u\n",shape,dynamic,swap,tile);return false;}
      }
  printf("polyhedral: %u exact triangular extrema and %u difference-address execution comparisons\n",count,regenerated_count);
  return true;
}

/* Check projected tile prefixes across unequal tile widths, both axis
 * orders and all strict/non-strict triangle orientations. */
static bool projected_tiles(void) {
  unsigned count=0;
  for (unsigned shape=1;shape<=4;++shape)
    for (unsigned n=2;n<=7;++n) for (unsigned swap=0;swap<2;++swap)
      for (unsigned ta=1;ta<=4;++ta) for (unsigned tb=1;tb<=4;++tb) {
        nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
        build_with_bounds(&f,n,n,shape,false,0,0,1,1,false,1);
        nyr_poly_schedule_t s={.interchange=swap!=0,.tile=ta,.inner_tile=tb};
        bool ok=nyr_opt_verify(&f,"projected/input")&&nyr_poly_detect(&f,0,&r)&&
          evaluate(&f,&before)&&nyr_poly_emit(&f,&r,s)&&
          nyr_opt_verify(&f,"projected/output")&&evaluate(&f,&after)&&before==after;
        nyr_ir_func_free(&f);++count;
        if (!ok) {fprintf(stderr,"projected tile mismatch %u/%u/%u/%u/%u\n",shape,n,swap,ta,tb);return false;}
      }
  printf("polyhedral: %u projected rectangular/strip triangle execution comparisons\n",count);
  return true;
}

static bool replay_tests(void) {
  const char *bad[]={"", "0123456789abcdef:1:0:1:0:0",
    "0123456789abcdef:0:0:0:0:1", "0123456789abcdef:1:257:0:0:0",
    "0123456789abcdef:1:0:0:0:0x", "0123456789abcde:1:0:0:0:0",
    "0123456789abcdef:1:99999999999999999999:0:0:0", "0123456789abcdef:-1:0:0:0:0"};
  nyr_poly_replay_t profile;
  for (size_t k=0;k<sizeof(bad)/sizeof(bad[0]);++k)
    if (nyr_poly_parse_replay(bad[k],&profile)) return false;
  const char *prior=getenv("NYTRIX_POLY_REPLAY");
  char *saved=prior?strdup(prior):NULL;
  if (prior&&!saved) return false;
  bool ok=true;
  for (unsigned k=0;k<4&&ok;++k) {
    nyr_func_t f;nyr_poly_region_t r;int64_t before=0,after=0;
    build(&f,5,5,0,k==3,0,0,1,1);
    ok=nyr_poly_detect(&f,0,&r)&&evaluate(&f,&before);
    uint64_t key=ok?nyr_poly_model_key(&f,&r):0;
    char text[80];
    snprintf(text,sizeof(text),"%016" PRIx64 ":%u:0:0:0:0",k==2?key^1:key,k!=1);
    nyr_poly_schedule_t selected={0};
    ok=ok&&nyr_poly_parse_replay(text,&profile)&&
       (nyr_poly_replay_schedule(&profile,&f,&r,&selected)==(k!=2));
    if (k==3) ok=ok&&!nyr_poly_legal(&r,selected);
    size_t len=f.len;
    ok=ok&&nyr_opt_setenv("NYTRIX_POLY_REPLAY",text)==0&&
       nyr_polyhedral_tiling(&f)&&nyr_opt_verify(&f,"replay/output")&&
       evaluate(&f,&after)&&before==after;
    if (k==0) ok=ok&&f.len!=len;
    else ok=ok&&f.len==len;
    nyr_ir_func_free(&f);
  }
  if (saved) nyr_opt_setenv("NYTRIX_POLY_REPLAY",saved);
  else nyr_opt_unsetenv("NYTRIX_POLY_REPLAY");
  free(saved);
  if (ok) puts("polyhedral: 8 malformed replay profiles rejected; valid, stale, identity and illegal schedules checked");
  return ok;
}

static bool fp_policy(void) {
  nyr_func_t f;nyr_poly_region_t r;
  bool old=nyr_fast_math();
  build_with_bounds(&f,5,7,0,false,0,0,1,1,false,2);
  nyr_set_fast_math(false);
  bool ok=nyr_opt_verify(&f,"fp/input")&&!nyr_poly_detect(&f,0,&r);
  nyr_set_fast_math(true);
  int64_t a,b;
  ok=ok&&nyr_poly_detect(&f,0,&r)&&evaluate(&f,&a)&&
     nyr_poly_emit(&f,&r,(nyr_poly_schedule_t){.interchange=true,.tile=2})&&
     nyr_opt_verify(&f,"fp/output")&&evaluate(&f,&b)&&a==b;
  nyr_set_fast_math(old);nyr_ir_func_free(&f);
  if (ok) puts("polyhedral: fast-math FP schedule verified; strict FP rejected");
  return ok;
}

/* Complete and partial strips use disjoint SSA namespaces. Compare
 * every runtime residue including empty/short/exact-multiple bounds. */
static bool isolated_tiles(void) {
  const char *env=getenv("NYTRIX_POLY_EXPERIMENTAL_ISOLATE");
  char *saved=env?strdup(env):NULL;
  nyr_opt_setenv("NYTRIX_POLY_EXPERIMENTAL_ISOLATE","1");
  bool ok=true;unsigned count=0;
  for (unsigned symbolic=0;ok&&symbolic<2;++symbolic)
    for (unsigned swap=0;ok&&swap<2;++swap)
      for (unsigned width=2;ok&&width<=5;++width) {
        nyr_func_t f,original={0};nyr_poly_region_t r;
        build_with_bounds(&f,symbolic?8:7,symbolic?8:9,0,false,0,0,1,1,symbolic,0);
        nyr_poly_schedule_t schedule={.interchange=swap,.tile=width,.inner_tile=1};
        ok=nyr_ir_func_clone(&f,&original)&&nyr_poly_detect(&f,0,&r)&&
          nyr_poly_emit(&f,&r,schedule)&&nyr_opt_verify(&f,"isolate-output");
        for (unsigned seed=0;ok&&seed<(symbolic?64u:1u);++seed) {
          int64_t a,b;ok=evaluate_seed(&original,seed,&a)&&evaluate_seed(&f,seed,&b)&&a==b;
          ++count;
        }
        /* The split duplicates the point-loop nest, not just the condition. */
        if (ok&&(symbolic||r.extent[swap?1:0]%width)) {
          nyr_scev_info_t loops={0};
          ok=nyr_scev_analyze(&f,&loops)&&loops.count>=6;
          nyr_scev_free(&loops);
        }
        nyr_ir_func_free(&original);nyr_ir_func_free(&f);
        if (!ok) fprintf(stderr,"isolated tiles mismatch symbolic=%u axis=%u width=%u\n",symbolic,swap,width);
      }
  if (saved) nyr_opt_setenv("NYTRIX_POLY_EXPERIMENTAL_ISOLATE",saved);
  else nyr_opt_unsetenv("NYTRIX_POLY_EXPERIMENTAL_ISOLATE");
  free(saved);
  if (ok) printf("polyhedral: %u full/partial isolated strip execution comparisons\n",count);
  return ok;
}

int main(void) {
  return isolated_tiles() && domain_extrema() && projected_tiles() && replay_tests() && fp_policy() && random_legality() && wavefront_tests() && regenerated() && exact_tiles() && profitability() && symbolic_bounds() && symbolic_shapes() && legality3() && regenerated3() && promotion_tests() && blocked_tests() && vector_block_tests() && horizontal_tests() && fusion_legality() && fusion_regeneration() && inline_stage_tests() && chain_tests() && recompute_tests() ? 0 : 1;
}
