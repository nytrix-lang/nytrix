/* Bounded polyhedral model. All dimensions are normalized iteration ordinals.
 * A schedule is a lexicographic order over the same finite integer domain.
 * Unknown effects, provenance, bounds or exceeded budgets reject the region. */
#ifndef NYR_POLY_H
#define NYR_POLY_H
#include <nyr/ir.h>

#define NYR_POLY_MAX_DIMS 3u
#define NYR_POLY_MAX_EXTENT 1048576u
#define NYR_POLY_MAX_POINTS 16384u
#define NYR_POLY_MAX_BYTES 262144u
#define NYR_POLY_MAX_ACCESS 16u

typedef struct {
  int64_t coeff[NYR_POLY_MAX_DIMS];
  int64_t constant;
  int base; /* -1 for integer; otherwise a dominating ALLOCA value */
} nyr_poly_affine_t;

typedef struct {
  nyr_poly_affine_t offset;
  unsigned width;
  bool write;
} nyr_poly_access_t;

typedef struct {
  size_t begin, end, body_begin, body_end;
  int64_t entry, exit;
  unsigned depth; /* zero is the legacy two-dimensional model */
  int iv[NYR_POLY_MAX_DIMS];
  unsigned extent[NYR_POLY_MAX_DIMS]; /* conservative max extent for dependence proof */
  bool symbolic[NYR_POLY_MAX_DIMS];
  int bound_value[NYR_POLY_MAX_DIMS]; /* original dominating SSA bound for symbolic dim */
  unsigned minimum_extent[NYR_POLY_MAX_DIMS]; /* profitability lower bound */
  int64_t initial[NYR_POLY_MAX_DIMS], stride[NYR_POLY_MAX_DIMS]; /* actual IV = initial + ordinal * stride */
  /* 0 = rectangle; 1 = j < i, 2 = j <= i; 3 = i < j, 4 = i <= j. */
  unsigned triangle;
  size_t access_count;
  nyr_poly_access_t access[NYR_POLY_MAX_ACCESS];
  bool inclusive[NYR_POLY_MAX_DIMS]; /* symbolic original comparison */
  bool external; /* conditional C pointer model; requires runtime versioning */
} nyr_poly_region_t;

typedef struct {
  bool interchange; /* 2D axis order, also for wavefronts */
  unsigned permutation; /* 3D: lexicographic index of axis permutation */
  unsigned tile; /* 0 = untiled; outer schedule dimension otherwise */
  unsigned inner_tile; /* 0 = same as tile; otherwise inner dimension */
  bool wavefront; /* explicit 2D schedule (skew*a+b,a), no tiling */
  unsigned skew; /* 0 defaults to 1; positive coefficient <= 8 */
} nyr_poly_schedule_t;

static inline unsigned nyr_poly_skew(nyr_poly_schedule_t s) {
  return s.skew ? s.skew : 1;
}

static inline unsigned nyr_poly_dims(const nyr_poly_region_t *r) {
  return r->depth ? r->depth : 2;
}
static inline unsigned nyr_poly_axis(nyr_poly_schedule_t s, unsigned dims,
                                    unsigned at) {
  static const unsigned orders[6][3]={{0,1,2},{0,2,1},{1,0,2},
                                      {1,2,0},{2,0,1},{2,1,0}};
  return dims==2 ? at^(s.interchange?1u:0u) : orders[s.permutation][at];
}
static inline bool nyr_poly_contains(const nyr_poly_region_t *r,
                                     unsigned i, unsigned j) {
  return !r->triangle || (r->triangle==1 ? j<i : r->triangle==2 ? j<=i :
                          r->triangle==3 ? i<j : i<=j);
}

/* Find the next supported region at/after start. */
bool nyr_poly_detect(const nyr_func_t *f, size_t start, nyr_poly_region_t *out);
/* Lazy scan reuses one analysis snapshot until the IR is modified. */
typedef struct nyr_poly_scan nyr_poly_scan_t;
nyr_poly_scan_t *nyr_poly_scan_open(const nyr_func_t *f,size_t start);
bool nyr_poly_scan_next(nyr_poly_scan_t *scan,nyr_poly_region_t *out);
void nyr_poly_scan_close(nyr_poly_scan_t *scan);
/* Non-consuming lookahead for a constant/branch-adjacent region. */
bool nyr_poly_scan_adjacent(nyr_poly_scan_t *scan,const nyr_poly_region_t *first,
                             nyr_poly_region_t *second);
/* Detect the first nest and, when requested, its immediately adjacent nest
 * using the same analysis snapshot. second->depth is zero if absent. */
bool nyr_poly_detect_pair(const nyr_func_t *f,size_t start,nyr_poly_region_t *out,
                           nyr_poly_region_t *second);
/* Validate an explicit schedule independently of the cost model.
 * External-pointer models are conditional on the emitter's alias guard. */
bool nyr_poly_legal(const nyr_poly_region_t *r, nyr_poly_schedule_t s);
/* Conservative symbolic proof, independent of the finite event engine. */
bool nyr_poly_symbolic_legal(const nyr_poly_region_t *r, nyr_poly_schedule_t s);
/* Prove a directed conflict relation cannot invert the schedule. */
bool nyr_poly_relation_pair(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                            const nyr_poly_access_t *a, const nyr_poly_access_t *b,
                            unsigned *budget);
bool nyr_poly_relation_fusion_pair(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                                   const nyr_poly_access_t *a, const nyr_poly_access_t *b,
                                   unsigned *budget);
/* Prove that every original-order conflict is componentwise nondecreasing
 * in an untiled schedule. Such a band admits arbitrary positive tile sizes
 * and coordinate permutations (for the axis-permutation schedule family).
 * A failed proof is unknown or a counterexample, never permission. */
bool nyr_poly_relation_permutable_pair(const nyr_poly_region_t *r,
                                      nyr_poly_schedule_t s,
                                      const nyr_poly_access_t *a,
                                      const nyr_poly_access_t *b,
                                      unsigned *budget);
bool nyr_poly_permutable(const nyr_poly_region_t *r, nyr_poly_schedule_t s);
/* No conflicting instances can differ in this coordinate with equal prefix.
 * This is a dependence proof, not permission to introduce threads by itself. */
bool nyr_poly_relation_band_pair(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                                 const nyr_poly_access_t *a, const nyr_poly_access_t *b,
                                 unsigned band, unsigned *budget);
bool nyr_poly_parallel_band(const nyr_poly_region_t *r, nyr_poly_schedule_t s,
                             unsigned band);
/* Find a legal weighted wavefront with a dependence-free inner band. */
bool nyr_poly_find_wavefront(const nyr_poly_region_t *r, nyr_poly_schedule_t *out);
/* Two adjacent equal-domain statements, originally all A before all B. */
bool nyr_poly_adjacent(const nyr_func_t *f, const nyr_poly_region_t *a,
                        const nyr_poly_region_t *b);
bool nyr_poly_fusion_legal(const nyr_poly_region_t *a,const nyr_poly_region_t *b,
                           nyr_poly_schedule_t s);
/* Private, same-point producer storage candidate; -1 if observable. This
 * does not prove schedule legality (the fused emitter still does that). */
int nyr_poly_inline_root(const nyr_func_t *f,const nyr_poly_region_t *a,
                         const nyr_poly_region_t *b);
bool nyr_poly_emit_fused(nyr_func_t *f,const nyr_poly_region_t *a,
                         const nyr_poly_region_t *b,nyr_poly_schedule_t s);
/* Unlike ordinary fusion, require that a whole intermediate allocation
 * is eliminated. Failure preserves the original function. */
bool nyr_poly_emit_inlined(nyr_func_t *f,const nyr_poly_region_t *a,
                           const nyr_poly_region_t *b,nyr_poly_schedule_t s);
/* Three private same-point stages, with pairwise dependence verification. */
bool nyr_poly_emit_chain(nyr_func_t *f,const nyr_poly_region_t *a,
                         const nyr_poly_region_t *b,const nyr_poly_region_t *c,
                         nyr_poly_schedule_t s);
/* Compute-inline an effect-free producer at translated consumer points. */
bool nyr_poly_emit_recomputed(nyr_func_t *f,const nyr_poly_region_t *a,
                              const nyr_poly_region_t *b,nyr_poly_schedule_t s);
/* Deterministic bounded search over legal candidate schedules. */
bool nyr_poly_choose_stream3(const nyr_poly_region_t *r,nyr_poly_schedule_t *out);
bool nyr_poly_choose(const nyr_poly_region_t *r, nyr_poly_schedule_t *out);
/* Choose and commit in one operation: no redundant dependence proof between
 * a successful selection and its immediate regeneration. */
bool nyr_poly_apply(nyr_func_t *f,const nyr_poly_region_t *r,bool allow_3d,
                     bool allow_simd,bool auto_horizontal,nyr_poly_schedule_t *selected);
/* Preserve scalar scheduling when vectorization is disabled by the caller. */
bool nyr_polyhedral_tiling_scalar(nyr_func_t *f);
/* Use the existing target capability option for automatic packed reduction. */
bool nyr_polyhedral_tiling_avx2(nyr_func_t *f);
/* Explicit single-location 3D recurrence promotion; fixed, positive
 * domains only, with all other accesses rooted in separate allocations. */
bool nyr_poly_emit_promoted(nyr_func_t *f,const nyr_poly_region_t *r,
                            nyr_poly_schedule_t s);
/* Vectorize an additive FP recurrence along the contiguous reduction axis. */
bool nyr_poly_emit_horizontal(nyr_func_t *f,const nyr_poly_region_t *r,
                              nyr_poly_schedule_t s,unsigned groups);
/* Register-block a proven private recurrence across the middle loop. */
bool nyr_poly_emit_blocked(nyr_func_t *f,const nyr_poly_region_t *r,
                            nyr_poly_schedule_t s,unsigned lanes);
/* Experimental packed four-lane private FP recurrence. */
bool nyr_poly_emit_vector_blocked(nyr_func_t *f,const nyr_poly_region_t *r,
                                   nyr_poly_schedule_t s);
/* Explicit one- or two-vector microtile (4 or 8 f64 lanes). */
bool nyr_poly_emit_vector_blocked_lanes(nyr_func_t *f,const nyr_poly_region_t *r,
                                       nyr_poly_schedule_t s,unsigned lanes);
/* Keep traversal and physical SIMD placement separate in diagnostics. */
typedef enum {
  NYR_POLY_SCALAR, NYR_POLY_VECTOR_BLOCK, NYR_POLY_VECTOR_REDUCTION
} nyr_poly_vector_mode_t;
/* Optional, exact-region schedule replay and model diagnostics. */
typedef struct {
  uint64_t key;
  unsigned order;
  nyr_poly_schedule_t schedule;
} nyr_poly_replay_t;
uint64_t nyr_poly_model_key(const nyr_func_t *f,const nyr_poly_region_t *r);
bool nyr_poly_parse_replay(const char *text,nyr_poly_replay_t *out);
bool nyr_poly_replay_schedule(const nyr_poly_replay_t *profile,
                               const nyr_func_t *f,const nyr_poly_region_t *r,
                               nyr_poly_schedule_t *out);
void nyr_poly_dump_model(const nyr_func_t *f,const nyr_poly_region_t *r,
                         const nyr_poly_region_t *second,nyr_poly_schedule_t s,
                         unsigned lanes,bool vector,bool candidate);
void nyr_poly_dump_model_chain(const nyr_func_t *f,const nyr_poly_region_t *r,
                         const nyr_poly_region_t *second,const nyr_poly_region_t *third,
                         nyr_poly_schedule_t s,unsigned lanes,nyr_poly_vector_mode_t vector_mode,bool candidate);
void nyr_poly_dump_placement(const nyr_func_t *f,const nyr_poly_region_t *a,
                            const nyr_poly_region_t *b,const nyr_poly_region_t *c,
                            int root,int root2,const char *mode);
/* Transactional region regeneration. On failure f is unchanged. */
bool nyr_poly_emit(nyr_func_t *f, const nyr_poly_region_t *r,
                   nyr_poly_schedule_t s);
#endif
