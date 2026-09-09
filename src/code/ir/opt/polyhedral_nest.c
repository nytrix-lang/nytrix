/*
 * Affine loop-nest analysis and adjacent counted-loop fusion.
 *
 * The fusion transform merges two consecutive counted loops with identical
 * trip counts into one loop whose body executes both iterations.  This is
 * the safe subset of PLUTO-style fusion: it fires only on the canonical
 * structured shape, refuses any loop whose body stores to memory or calls,
 * and bails whenever the CFG deviates from the pattern below.
 *
 * Canonical shape fused by this pass:
 *
 *   P1: setup            BR h1
 *   H1: phis; cmp1       BR_IF c1 -> B1      fallthrough -> gap -> P2
 *   B1/K1: body1         BR h1               (latch retargeted to B2)
 *   gap: trivial BR-only blocks on the exit path
 *   P2: setup            BR h2               (retargeted to X)
 *   H2: phis; cmp2       BR_IF c2 -> B2      fallthrough -> X
 *   B2/K2: body2         BR h2               (retargeted to h1)
 *   X: post-loop code
 *
 * After fusion the merged header carries L1's phis plus L2's non-IV phis,
 * the IV phi of L2 is replaced by L1's IV (identical init/step/trip), and
 * the backedge flows K1 -> B2 .. K2 -> H1.  Loop-carried accumulators of
 * L2 stay correct because their header PHIs move with the merge; values
 * defined in B2 could not have escaped to X in the original SSA either,
 * since only header PHIs dominate the exit.
 *
 * References:
 *  Bondhugula, Hartono, Ramanujam, Sadayappan - "A Practical Automatic
 *   Polyhedral Parallelizer and Locality Optimizer" (PLDI 2008).
 *  Kennedy & McKinley - "Optimizing for Parallelism and Data Locality"
 *   (ICS 1992).  Fusion legality under dependences.
 *  Cooper & Torczon - Engineering a Compiler, 2nd Ed., Ch.8 §8.5.
 */
#include "code/ir/opt/loop.h"
#include "code/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

#define NY_AFFINE_MAX_ACCESS 32

typedef struct {
  int64_t coeffs; /* coefficient of the loop IV */
  int64_t constant;
  bool is_write;
} affine_access_t;

typedef struct {
  affine_access_t accesses[NY_AFFINE_MAX_ACCESS];
  size_t count;
} access_pattern_t;

/*
 * Recover the affine form `coeffs * iv + constant` of a scalar value when
 * one exists.  Returns false for anything nonlinear or unrelated to the IV.
 */
static bool extract_affine(const nyir_func_t *f, const int *defs, int value,
                           int64_t iv, affine_access_t *out) {
  if (value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyir_inst_t *in = &f->data[defs[value]];

  memset(out, 0, sizeof(*out));
  switch (in->op) {
  case NYIR_CONST_I64:
    out->constant = in->imm;
    return true;
  case NYIR_PHI:
    /*
     * A header PHI is exactly the IV when its destination is the IV.
     */
    if (in->dst == iv) {
      out->coeffs = 1;
      return true;
    }
    return false;
  default:
    break;
  }
  if (in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) {
    affine_access_t a = {0}, b = {0};
    if (!extract_affine(f, defs, in->a, iv, &a) ||
        !extract_affine(f, defs, in->b, iv, &b))
      return false;
    bool add = in->op == NYIR_ADD_I64;
    out->coeffs = add ? a.coeffs + b.coeffs : a.coeffs - b.coeffs;
    out->constant = add ? a.constant + b.constant : a.constant - b.constant;
    return true;
  }
  if (in->op == NYIR_MUL_I64) {
    affine_access_t a = {0}, b = {0};
    if (!extract_affine(f, defs, in->a, iv, &a) ||
        !extract_affine(f, defs, in->b, iv, &b))
      return false;
    if (a.coeffs == 0 && b.coeffs != 0) {
      out->coeffs = b.coeffs * a.constant;
      out->constant = b.constant * a.constant;
      return true;
    }
    if (b.coeffs == 0 && a.coeffs != 0) {
      out->coeffs = a.coeffs * b.constant;
      out->constant = a.constant * b.constant;
      return true;
    }
    return false;
  }
  return false;
}

/*
 * Collect the memory accesses of one natural loop.  Accesses whose
 * addresses are not provably affine in the loop IV count conservatively
 * as writes so they block fusion.
 */
static void find_loop_accesses(const nyir_func_t *f, const int *defs,
                               const nyir_cfg_t *cfg, const bool *in_loop,
                               const nyir_scev_loop_t *loop,
                               access_pattern_t *pattern) {
  memset(pattern, 0, sizeof(*pattern));
  for (size_t block = 0; block < cfg->block_count; ++block) {
    if (!in_loop[block])
      continue;
    for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
      const nyir_inst_t *in = &f->data[i];
      bool store = in->op == NYIR_STORE_I64;
      bool load = in->op == NYIR_LOAD_I64;
      if (!store && !load)
        continue;
      affine_access_t access = {0};
      if (extract_affine(f, defs, in->a, loop->iv, &access) &&
          access.coeffs != 0)
        access.is_write = store;
      else
        access.is_write = true;
      if (pattern->count < NY_AFFINE_MAX_ACCESS)
        pattern->accesses[pattern->count++] = access;
    }
  }
}

static size_t block_for_label(const nyir_cfg_t *cfg, int64_t label) {
  if (label >= 0)
    for (size_t b = 0; b < cfg->block_count; ++b)
      if (cfg->block_label[b] == label)
        return b;
  return SIZE_MAX;
}

/*
 * One-past-the-last-real-instruction index of a block.
 */
static size_t affine_last_non_nop(const nyir_func_t *f, const nyir_cfg_t *cfg,
                                  size_t block) {
  size_t end = cfg->block_end[block];
  while (end > cfg->block_start[block] && f->data[end - 1].op == NYIR_NOP)
    --end;
  return end;
}

static size_t pred_count(const nyir_cfg_t *cfg, size_t block) {
  return cfg->pred_offsets[block + 1] - cfg->pred_offsets[block];
}

static bool pred_is(const nyir_cfg_t *cfg, size_t block, size_t want) {
  for (size_t p = cfg->pred_offsets[block]; p < cfg->pred_offsets[block + 1];
       ++p)
    if (cfg->pred_blocks[p] == want)
      return true;
  return false;
}

/*
 * A trivial block holds only its LABEL, NOPs, and one unconditional BR.
 * Returns the successor block through `next` when the shape matches.
 */
static bool trivial_br_block(const nyir_func_t *f, const nyir_cfg_t *cfg,
                             size_t block, size_t *next) {
  size_t last = affine_last_non_nop(f, cfg, block);
  if (last <= cfg->block_start[block])
    return false;
  const nyir_inst_t *term = &f->data[last - 1];
  if (term->op != NYIR_BR || term->imm < 0)
    return false;
  size_t target = block_for_label(cfg, term->imm);
  if (target == SIZE_MAX || target == block)
    return false;
  *next = target;
  return true;
}

/*
 * Effects that disqualify an instruction from living inside a fused loop.
 */
#define AFFINE_FORBIDDEN_EFFECTS                                              \
  (NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CALL |    \
   NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE |        \
   NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT |                 \
   NYIR_EFFECT_IO | NYIR_EFFECT_THREAD | NYIR_EFFECT_FFI | NYIR_EFFECT_FENV)

/*
 * Calls, traps, and control effects make interleaving unsafe.
 */
static bool loops_call_free(const nyir_func_t *f, const nyir_cfg_t *cfg,
                            const bool *in1, const bool *in2) {
  for (size_t pass = 0; pass < 2; ++pass) {
    const bool *in_loop = pass == 0 ? in1 : in2;
    for (size_t b = 0; b < cfg->block_count; ++b) {
      if (!in_loop[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyir_inst_t *in = &f->data[i];
        if (in->op == NYIR_NOP || in->op == NYIR_LABEL ||
            in->op == NYIR_PHI || in->op == NYIR_BR || in->op == NYIR_BR_IF)
          continue;
        if (in->op == NYIR_RET)
          return false;
        if (nyir_effective_effects(in) & AFFINE_FORBIDDEN_EFFECTS)
          return false;
      }
    }
  }
  return true;
}

typedef struct {
  nyir_phi_incoming_t *incoming;
  size_t len;
  int dst;
} saved_phi_t;

static void saved_phi_free(saved_phi_t *phi) { free(phi->incoming); }

/*
 * Deep-copy a PHI so it can be re-emitted elsewhere after discarding.
 */
static bool saved_phi_copy(const nyir_inst_t *in, saved_phi_t *out) {
  out->incoming = malloc(in->phi_incoming_len * sizeof(*out->incoming));
  if (!out->incoming)
    return false;
  memcpy(out->incoming, in->phi_incoming,
         in->phi_incoming_len * sizeof(*out->incoming));
  out->len = in->phi_incoming_len;
  out->dst = in->dst;
  return true;
}

/*
 * Splice `inst` into the stream at `pos`, shifting everything else up.
 */
static bool insert_inst(nyir_func_t *f, size_t pos, const nyir_inst_t *inst) {
  if (!nir_ensure_inst_space(f, 1))
    return false;
  memmove(&f->data[pos + 1], &f->data[pos],
          (f->len - pos) * sizeof(*f->data));
  f->data[pos] = *inst;
  f->len++;
  return true;
}

static bool emit_saved_phi(nyir_func_t *f, size_t pos,
                           const saved_phi_t *phi) {
  nyir_phi_incoming_t *incoming = malloc(phi->len * sizeof(*incoming));
  if (!incoming)
    return false;
  memcpy(incoming, phi->incoming, phi->len * sizeof(*incoming));
  nyir_inst_t inst = {.op = NYIR_PHI, .dst = phi->dst};
  inst.phi_incoming = incoming;
  inst.phi_incoming_len = phi->len;
  if (!insert_inst(f, pos, &inst)) {
    free(incoming);
    return false;
  }
  return true;
}

/*
 * Fuse one adjacent pair of counted loops.  Returns true when the function
 * was rewritten; leaves it untouched on every bail-out.
 */
static bool fuse_adjacent_pair(nyir_func_t *f, const nyir_scev_info_t *info) {
  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);
  bool *mask1 = NULL, *mask2 = NULL;
  saved_phi_t moved[NY_AFFINE_MAX_ACCESS];
  size_t moved_count = 0;
  bool committed = false;
  bool ok = false;

  if (!defs || !nyir_cfg_build(f, &cfg))
    goto out;
  mask1 = calloc(cfg.block_count, sizeof(*mask1));
  mask2 = calloc(cfg.block_count, sizeof(*mask2));
  if (!mask1 || !mask2)
    goto out;

  for (size_t li = 0; li < info->count && !committed; ++li) {
    const nyir_scev_loop_t *l1 = &info->loops[li];
    if (!l1->trip_count_known || l1->trip_count < 1)
      continue;
    for (size_t lj = 0; lj < info->count && !committed; ++lj) {
      const nyir_scev_loop_t *l2 = &info->loops[lj];
      if (lj == li || l2->header_block == l1->header_block)
        continue;
      /*
       * Identical iteration space: same count, start, and stride.
       */
      if (!l2->trip_count_known || l2->trip_count != l1->trip_count)
        continue;
      if (l2->init != l1->init || l2->step != l1->step)
        continue;

      size_t h1b = l1->header_block, k1b = l1->latch_block,
             p1b = l1->preheader_block;
      size_t h2b = l2->header_block, k2b = l2->latch_block,
             p2b = l2->preheader_block;
      if (k2b == k1b || p1b == SIZE_MAX || p2b == SIZE_MAX)
        continue;
      int64_t h1l = cfg.block_label[h1b], h2l = cfg.block_label[h2b];
      int64_t p1l = cfg.block_label[p1b], p2l = cfg.block_label[p2b];

      /*
       * Guard shapes: both headers end in BR_IF whose taken edge enters
       * the loop and whose fallthrough begins the next region.
       */
      size_t g1i = affine_last_non_nop(f, &cfg, h1b);
      size_t g2i = affine_last_non_nop(f, &cfg, h2b);
      if (g1i >= f->len || g2i >= f->len)
        continue;
      if (g1i <= cfg.block_start[h1b] || g2i <= cfg.block_start[h2b])
        continue;
      const nyir_inst_t *guard1 = &f->data[g1i - 1];
      const nyir_inst_t *guard2 = &f->data[g2i - 1];
      if (guard1->op != NYIR_BR_IF || guard2->op != NYIR_BR_IF)
        continue;
      if (block_for_label(&cfg, guard1->imm) == SIZE_MAX)
        continue;
      size_t b2b = block_for_label(&cfg, guard2->imm);
      if (b2b == SIZE_MAX)
        continue;
      int64_t b2l = cfg.block_label[b2b];

      /*
       * Latch shapes: both latches close with BR back to their header.
       */
      size_t t1i = affine_last_non_nop(f, &cfg, k1b);
      size_t t2i = affine_last_non_nop(f, &cfg, k2b);
      if (t1i <= cfg.block_start[k1b] || t2i <= cfg.block_start[k2b])
        continue;
      if (f->data[t1i - 1].op != NYIR_BR || f->data[t1i - 1].imm != h1l)
        continue;
      if (f->data[t2i - 1].op != NYIR_BR || f->data[t2i - 1].imm != h2l)
        continue;

      /*
       * Adjacency: guard1's fallthrough reaches P2 across trivial
       * branch-only blocks, and nothing else feeds P2.
       */
      size_t entry = cfg.inst_block[g1i];
      size_t cur = entry;
      size_t hops = 0;
      while (cur != p2b && hops++ < 8) {
        size_t next;
        if (!trivial_br_block(f, &cfg, cur, &next)) {
          cur = SIZE_MAX;
          break;
        }
        cur = next;
      }
      if (cur != p2b || pred_count(&cfg, p2b) != 1)
        continue;

      /*
       * H2 must be entered only from P2 and its own latch, and H1 only
       * from its preheader and a single latch (multi-latch loops would
       * leave dangling backedges after the terminator rewrite).
       */
      if (pred_count(&cfg, h2b) != 2 || !pred_is(&cfg, h2b, p2b) ||
          !pred_is(&cfg, h2b, k2b))
        continue;
      if (pred_count(&cfg, h1b) != 2 || !pred_is(&cfg, h1b, p1b))
        continue;

      /*
       * Exit target X: guard2's fallthrough, labeled and phi-free (the
       * rerouted exit chain makes P2 an extra X predecessor).
       */
      size_t xb = cfg.inst_block[g2i];
      int64_t xl = cfg.block_label[xb];
      if (xl < 0)
        continue;
      bool x_has_phi = false;
      for (size_t i = cfg.block_start[xb]; i < cfg.block_end[xb]; ++i)
        if (f->data[i].op == NYIR_PHI)
          x_has_phi = true;
      if (x_has_phi)
        continue;

      /*
       * Legality: no calls or traps anywhere, and the affine access scan
       * must not surface writes or unanalyzable addresses.
       */
      if (!nyir_cfg_natural_loop_blocks(&cfg, k1b, h1b, mask1,
                                        cfg.block_count) ||
          !nyir_cfg_natural_loop_blocks(&cfg, k2b, h2b, mask2,
                                        cfg.block_count))
        continue;
      if (!loops_call_free(f, &cfg, mask1, mask2))
        continue;
      access_pattern_t pat1, pat2;
      find_loop_accesses(f, defs, &cfg, mask1, l1, &pat1);
      find_loop_accesses(f, defs, &cfg, mask2, l2, &pat2);
      bool any_write = false;
      for (size_t a = 0; a < pat1.count; ++a)
        any_write |= pat1.accesses[a].is_write;
      for (size_t a = 0; a < pat2.count; ++a)
        any_write |= pat2.accesses[a].is_write;
      if (any_write)
        continue;

      /*
       * P2 setup must be constants so hoisting cannot break dominance.
       */
      bool setup_ok = true;
      for (size_t i = cfg.block_start[p2b]; i + 1 < cfg.block_end[p2b]; ++i) {
        if (f->data[i].op == NYIR_LABEL || f->data[i].op == NYIR_NOP)
          continue;
        if (f->data[i].op != NYIR_CONST_I64)
          setup_ok = false;
      }
      if (!setup_ok)
        continue;

      /*
       * Commit.  Snapshot phase first: moved PHI entries are relabeled so
       * their entry edge reads P1 (the merged guard input), and P2's
       * constants are remembered for re-emission above H1.
       */
      int64_t cst_imm[NY_AFFINE_MAX_ACCESS];
      int cst_dst[NY_AFFINE_MAX_ACCESS];
      size_t cst_count = 0;
      for (size_t i = cfg.block_start[p2b]; i + 1 < cfg.block_end[p2b]; ++i) {
        if (f->data[i].op != NYIR_CONST_I64)
          continue;
        if (cst_count == NY_AFFINE_MAX_ACCESS) {
          setup_ok = false;
          break;
        }
        cst_dst[cst_count] = f->data[i].dst;
        cst_imm[cst_count] = f->data[i].imm;
        cst_count++;
      }
      if (!setup_ok)
        continue;

      for (size_t i = cfg.block_start[h2b]; i < g2i - 1; ++i) {
        if (f->data[i].op != NYIR_PHI)
          continue;
        if (moved_count == NY_AFFINE_MAX_ACCESS ||
            !saved_phi_copy(&f->data[i], &moved[moved_count]))
          goto out;
        moved_count++;
      }
      for (size_t m = 0; m < moved_count; ++m)
        for (size_t k = 0; k < moved[m].len; ++k)
          if (moved[m].incoming[k].predecessor_label == p2l)
            moved[m].incoming[k].predecessor_label = p1l;

      /*
       * L2's IV trajectory equals L1's; route every use to L1's IV.
       */
      if (!nyir_replace_all_uses(f, l2->iv, l1->iv))
        goto out;

      /*
       * Discard phase: index-stable NOP fills only.  cmp2 stays alive as
       * a dead definition, which is legal SSA; its branch becomes an
       * unconditional jump to the exit so the abandoned H2 shell stays
       * terminated.
       */
      for (size_t i = cfg.block_start[h2b]; i < g2i - 1; ++i)
        if (f->data[i].op == NYIR_PHI)
          nyir_inst_discard(&f->data[i]);
      nyir_inst_discard(&f->data[g2i - 1]);
      f->data[g2i - 1].op = NYIR_BR;
      f->data[g2i - 1].imm = xl;
      f->data[g2i - 1].effects = nyir_inst_effects(&f->data[g2i - 1]);
      for (size_t i = cfg.block_start[p2b]; i + 1 < cfg.block_end[p2b]; ++i)
        if (f->data[i].op != NYIR_LABEL)
          nyir_inst_discard(&f->data[i]);

      /*
       * Terminator edits by label: collapse the exit chain onto X and
       * close the merged cycle K1 -> B2 .. K2 -> H1.
       */
      cur = entry;
      hops = 0;
      while (cur != p2b && hops++ < 8) {
        size_t next;
        size_t last = affine_last_non_nop(f, &cfg, cur);
        if (!trivial_br_block(f, &cfg, cur, &next))
          break;
        f->data[last - 1].imm = xl;
        cur = next;
      }
      size_t p2t = affine_last_non_nop(f, &cfg, p2b);
      if (p2t > cfg.block_start[p2b] && f->data[p2t - 1].op == NYIR_BR)
        f->data[p2t - 1].imm = xl;
      f->data[t1i - 1].imm = b2l; /* latch1 continues into body2 */
      f->data[t2i - 1].imm = h1l; /* latch2 closes the merged loop */

      /*
       * L1's backedge now arrives from K2 (latch 2): every non-entry
       * incoming on H1's PHIs retargets to K2's label. Relabeling to
       * B2 was only correct when loop 2's body was a single block;
       * with a multi-block body the machine lowerer found no incoming
       * matching the actual K2 -> H1 edge and aborted the build.
       */
      int64_t k2l = cfg.block_label[k2b];
      if (k2l < 0)
        continue;
      for (size_t i = cfg.block_start[h1b] + 1; i < cfg.block_end[h1b]; ++i) {
        if (f->data[i].op != NYIR_PHI)
          break;
        for (size_t k = 0; k < f->data[i].phi_incoming_len; ++k)
          if (f->data[i].phi_incoming[k].predecessor_label != p1l)
            f->data[i].phi_incoming[k].predecessor_label = k2l;
      }

      /*
       * Insertion phase, higher positions first so earlier targets stay
       * valid without recomputation.  The moved PHIs land right after
       * H1's existing PHIs; P2's constants land just before P1's BR.
       */
      size_t phi_ins = cfg.block_start[h1b] + 1;
      while (phi_ins < f->len && f->data[phi_ins].op == NYIR_PHI)
        phi_ins++;
      size_t p1t = affine_last_non_nop(f, &cfg, p1b);
      size_t cst_pos = p1t - 1;
      if (phi_ins > cst_pos) {
        for (size_t m = moved_count; m > 0; --m)
          if (!emit_saved_phi(f, phi_ins, &moved[m - 1]))
            goto out;
        for (size_t c = cst_count; c > 0; --c)
          if (!insert_inst(f, cst_pos,
                           &(nyir_inst_t){.op = NYIR_CONST_I64,
                                          .dst = cst_dst[c - 1],
                                          .imm = cst_imm[c - 1]}))
            goto out;
      } else {
        for (size_t c = cst_count; c > 0; --c)
          if (!insert_inst(f, cst_pos,
                           &(nyir_inst_t){.op = NYIR_CONST_I64,
                                          .dst = cst_dst[c - 1],
                                          .imm = cst_imm[c - 1]}))
            goto out;
        for (size_t m = moved_count; m > 0; --m)
          if (!emit_saved_phi(f, phi_ins, &moved[m - 1]))
            goto out;
      }

      committed = true;
      ok = true;
    }
  }

out:
  for (size_t m = 0; m < moved_count; ++m)
    saved_phi_free(&moved[m]);
  free(mask1);
  free(mask2);
  free(defs);
  nyir_cfg_free(&cfg);
  return ok;
}

bool nyir_affine_nest(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  for (int round = 0; round < 8; ++round) {
    nyir_scev_info_t info = {0};
    if (!nyir_scev_analyze(f, &info))
      return false;
    bool fused = fuse_adjacent_pair(f, &info);
    nyir_scev_free(&info);
    if (!fused)
      break;
  }
  return true;
}

/*
 * Polyhedral-style loop tiling: insert loop-peel and trip-count
 * reduction metadata so downstream vectorization and unrolling can
 * use cache-friendly tile sizes.  For now, annotate counted loops
 * whose trip count exceeds a cache-line threshold with a tile-size
 * hint in the range field.
 *
 * Tile size heuristic: sqrt(L1_cache / element_size).  For i64
 * elements (8 bytes) and 32 KB L1: sqrt(32768/8) ≈ 64.
 * For i32 elements (4 bytes): sqrt(32768/4) ≈ 90.
 *
 * The annotation is advisory: downstream passes (loop_unroll,
 * loop_versioning) consume the range hints.
 *
 * References:
 *  Bondhugula, Hartono, Ramanujam, Sadayappan — "A Practical
 *   Automatic Polyhedral Parallelizer and Locality Optimizer"
 *   (PLDI 2008). §4.2: tiling for cache locality.
 *  Allen & Kennedy — Optimizing Compilers for Modern Architectures,
 *   Ch.7: loop tiling for register/cache blocking.
 *  Fog — Optimizing software in C++ (Vol.1), Ch.15: cache blocking.
 */
bool nyir_polyhedral_tiling(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);
  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }
  nyir_scev_info_t info = {0};
  if (!nyir_scev_analyze(f, &info)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }
  /*
   * Default tile size: sqrt(32KB / 8) = 64 for i64 elements.
   */
  const int64_t DEFAULT_TILE = 64;
  for (size_t li = 0; li < info.count; ++li) {
    const nyir_scev_loop_t *loop = &info.loops[li];
    if (!loop->trip_count_known || loop->trip_count <= (uint64_t)DEFAULT_TILE)
      continue;
    /*
     * Find the loop header and annotate with tile-size hint.
     */
    size_t h = loop->header_block;
    for (size_t i = cfg.block_start[h]; i < cfg.block_end[h]; ++i) {
      nyir_inst_t *in = &f->data[i];
      /*
       * Annotate the loop's comparison/branch with a range hint
       * encoding the tile size.  The range field encodes
       * [min, max] trip counts; set max to the tile size so
       * unrolling can choose a tile-friendly factor.
       */
      if (in->op == NYIR_CMP_I64 && in->dst == loop->limit_value) {
        in->range.min = 0;
        in->range.max = DEFAULT_TILE;
        break;
      }
    }
  }
  /*
   * Pass contract: the boolean result reports success, not "changed".
   * Annotating nothing (no long-running loops) is a normal outcome.
   */
  nyir_scev_free(&info);
  free(defs);
  nyir_cfg_free(&cfg);
  return true;
}
