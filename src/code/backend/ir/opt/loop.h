#ifndef NYIR_LOOP_H
#define NYIR_LOOP_H

#include <nyr/ir.h>
typedef struct {
  size_t header_block;
  size_t latch_block;
  size_t preheader_block;
  size_t header_index;
  size_t latch_index;
  int64_t header_label;
  int iv;
  int init_value;
  int next_value;
  int limit_value;
  int64_t init;
  bool init_is_const;
  int64_t step;
  int64_t limit;
  nyr_cmp_t predicate;
  bool limit_is_const;
  bool trip_count_known;
  uint64_t trip_count;
} nyr_scev_loop_t;

/*
 * A bounded affine value over at most two counted-loop induction variables.
 * `range` is valid only when every term is bounded without i64 overflow at
 * the queried program point.
 */
typedef struct {
  int iv[2];
  int64_t coefficient[2];
  int64_t constant;
  nyr_range_t range;
} nyr_scev_affine_t;

typedef struct {
  nyr_scev_loop_t *loops;
  size_t count;
} nyr_scev_info_t;

bool nyr_scev_analyze(const nyr_func_t *f, nyr_scev_info_t *out);
void nyr_scev_free(nyr_scev_info_t *info);
bool nyr_scev_affine_at(const nyr_func_t *f, const nyr_scev_info_t *info,
                         const int *defs, int value, size_t at,
                         nyr_scev_affine_t *out);
bool nyr_scev_affine_at_cfg(const nyr_func_t *f, const nyr_scev_info_t *info,
                             const int *defs, const nyr_cfg_t *cfg, int value,
                             size_t at, nyr_scev_affine_t *out);
bool nyr_scev_tbuf_bounds_check_safe(const nyr_func_t *f,
                                      const nyr_scev_info_t *info,
                                      const int *defs, size_t at,
                                      const nyr_inst_t *check);
bool nyr_scev_tbuf_bounds_check_safe_cfg(const nyr_func_t *f,
                                          const nyr_scev_info_t *info,
                                          const int *defs,
                                          const nyr_cfg_t *cfg, size_t at,
                                          const nyr_inst_t *check);
bool nyr_scev_lite(nyr_func_t *f);
bool nyr_irce(nyr_func_t *f);
bool nyr_loop_idiom(nyr_func_t *f);
bool nyr_loop_memset(nyr_func_t *f);
bool nyr_loop_last_overwrite(nyr_func_t *f);
bool nyr_loop_rotate(nyr_func_t *f);
bool nyr_loop_interchange(nyr_func_t *f);
bool nyr_loop_versioning(nyr_func_t *f);
bool nyr_loop_peel(nyr_func_t *f);
bool nyr_unroll_jam(nyr_func_t *f);
bool nyr_softpipe_rewrite(nyr_func_t *f);
bool nyr_loop_predication(nyr_func_t *f);
bool nyr_loop_unswitch(nyr_func_t *f);
bool nyr_loop_unroll_ssa(nyr_func_t *f);
bool nyr_loop_vectorize(nyr_func_t *f);

/*
 * Design notes for future loop transforms.  These passes were historically
 * advertised as no-ops; the stubs have been removed, but the specifications
 * and references below remain the contract for a real implementation.
 *
 * Loop Alignment: aligns loop bodies to cache line boundaries for better
 * performance. Inserts padding NOPs before loop headers to align them.
 *  References:
 *   Fog — Optimizing subroutines in assembly (Vol.2), Ch.16 §16.8:
 *    loop alignment and cache-line padding.
 *   Allen & Kennedy — Optimizing Compilers for Modern Architectures,
 *    Ch.5 §5.3: peeling for alignment.
 *   Fog Vol.3 Ch.9 §9.1 — Cache alignment effects on loop performance.
 *
 * Loop Distribution (Fission): splits a loop with multiple independent
 * statements into multiple loops. This enables better vectorization,
 * parallelization, and register allocation for each loop fragment.
 *  References:
 *   Kennedy & McKinley — "Maximizing Loop Parallelism and Improving
 *    Data Locality via Loop Fusion and Distribution" (LCPC 1993).
 *   Wolfe — "High Performance Compilers for Parallel Computing",
 *    Ch.5 §5.3: Loop fission for parallelization and register pressure.
 *   Allen & Kennedy — Optimizing Compilers for Modern Architectures,
 *    Ch.5 §5.4: Fission and fusion legality.
 *
 * Loop Fusion: combines adjacent loops with the same iteration space
 * into a single loop. This reduces loop overhead and improves cache locality.
 *  References:
 *   Kennedy & McKinley — "Maximizing Loop Parallelism and Improving
 *    Data Locality via Loop Fusion and Distribution" (LCPC 1993).
 *   Wolfe — "High Performance Compilers for Parallel Computing",
 *    Ch.5 §5.5: Fusion for data locality and register pressure reduction.
 *   Kennedy & McKinley — "Loop Fusion with Data Dependence" (CACM 1993).
 *
 * Loop rerolling: rerolling a pair such as a[i] += x; a[i + 1] += x requires a
 * canonical induction representation plus an alias proof for both stores.
 * NYIR has scalar and vector memory operations, but no representation for that
 * proof (or for the overflow/trap contract of a widened update).  Creating a
 * vector operation without those facts is speculation: calls, unknown trip
 * counts, aliases, and mismatched stores could all change the observable
 * behavior.  A reroll pass must remain a verifier-safe no-op, retaining every
 * input byte-for-byte and reporting success even when the loop is not a reroll
 * candidate, until those facts have a first-class representation.
 */

/*
 * Affine nest analysis and adjacent counted-loop fusion.  Merges
 * consecutive same-trip loops whose bodies are free of writes and calls.
 */
bool nyr_affine_nest(nyr_func_t *f);

#endif
