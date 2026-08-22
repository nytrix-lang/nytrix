/*
 * Register allocator: graph-coloring and linear-scan register
 * assignment for XMM, FPR, GPR, and NEON register classes on all targets.
 */
#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "code/native/ir/machine.h"
#include "base/common.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define NY_MACH_SPLIT_SPAN 16

typedef struct {
  size_t *data;
  size_t len;
  size_t cap;
} size_vec_t;

typedef struct {
  size_t start;
  size_t end;
  size_t seg;
  int color;
} active_t;

static size_t bit_words(size_t n) {
  size_t bits = sizeof(size_t) * 8;
  return (n + bits - 1) / bits;
}

static void bit_set(size_t *b, size_t i) {
  b[i / (sizeof(size_t) * 8)] |=
      (size_t)1 << (i % (sizeof(size_t) * 8));
}

static bool bit_get(const size_t *b, size_t i) {
  return (b[i / (sizeof(size_t) * 8)] >>
          (i % (sizeof(size_t) * 8))) & 1;
}

static bool size_vec_push(size_vec_t *v, size_t x) {
  if (v->len == v->cap) {
    size_t cap = v->cap ? v->cap * 2 : 4;
    size_t *p = realloc(v->data, cap * sizeof(*p));
    if (!p)
      return false;
    v->data = p;
    v->cap = cap;
  }
  v->data[v->len++] = x;
  return true;
}
static size_vec_t *build_segment_by_block(const ny_mach_func_t *mach,
                                          const ny_mach_regalloc_t *a) {
  if (!mach || !a || mach->block_len == 0)
    return NULL;
  size_vec_t *by_block = calloc(mach->block_len, sizeof(*by_block));
  if (!by_block)
    return NULL;
  for (size_t i = 0; i < a->segment_len; ++i) {
    uint32_t block = a->segments[i].block;
    if (block >= mach->block_len || !size_vec_push(&by_block[block], i)) {
      for (size_t b = 0; b < mach->block_len; ++b)
        free(by_block[b].data);
      free(by_block);
      return NULL;
    }
  }
  return by_block;
}

static void free_segment_by_block(const ny_mach_func_t *mach,
                                  size_vec_t *by_block) {
  if (!mach || !by_block)
    return;
  for (size_t b = 0; b < mach->block_len; ++b)
    free(by_block[b].data);
  free(by_block);
}


static bool is_alloc_type(ny_mach_type_t t, ny_mach_reg_class_t reg_class) {
  if (reg_class == NY_MACH_REGCLASS_GPR)
    return t == NY_MACH_TYPE_I64 || t == NY_MACH_TYPE_PTR;
  if (reg_class == NY_MACH_REGCLASS_FPR)
    return t == NY_MACH_TYPE_F32 || t == NY_MACH_TYPE_F64;
  if (reg_class == NY_MACH_REGCLASS_VECTOR)
    return t == NY_MACH_TYPE_V128_I64 || t == NY_MACH_TYPE_V128_F64 ||
           t == NY_MACH_TYPE_V128_F32;
  return false;
}

static bool dst_is_def(const ny_mach_inst_t *in) {
  if (!in || in->dst.kind != NY_MACH_OPERAND_VREG)
    return false;
  return in->opcode != NY_MACH_STORE;
}

static void add_use(const ny_mach_operand_t *op, uint32_t *out, size_t *len,
                    size_t cap) {
  if (op && op->kind == NY_MACH_OPERAND_VREG && *len < cap)
    out[(*len)++] = op->as.reg;
}

static size_t inst_uses(const ny_mach_inst_t *in, uint32_t *out, size_t cap) {
  size_t n = 0;
  if (!in)
    return 0;
  add_use(&in->src0, out, &n, cap);
  add_use(&in->src1, out, &n, cap);
  add_use(&in->src2, out, &n, cap);
  if (in->opcode == NY_MACH_STORE)
    add_use(&in->dst, out, &n, cap);
  for (size_t i = 0; i < in->args_len; ++i)
    add_use(&in->args[i], out, &n, cap);
  return n;
}

static bool append_segment(ny_mach_regalloc_t *a, uint32_t vreg,
                           uint32_t block, size_t start, size_t end,
                           bool reload, bool spill) {
  if (start > end)
    return true;
  if (a->segment_len == a->segment_cap) {
    size_t new_cap = a->segment_cap ? a->segment_cap * 2 : 64;
    ny_mach_live_segment_t *p =
        realloc(a->segments, new_cap * sizeof(*a->segments));
    if (!p)
      return false;
    a->segments = p;
    a->segment_cap = new_cap;
  }
  a->segments[a->segment_len++] = (ny_mach_live_segment_t){
      .vreg = vreg,
      .block = block,
      .start = start,
      .end = end,
      .color = -1,
      .reload = reload,
      .spill = spill,
  };
  return true;
}

static int cmp_seg_start(const void *pa, const void *pb) {
  const ny_mach_live_segment_t *a = pa, *b = pb;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  if (a->end != b->end)
    return a->end < b->end ? -1 : 1;
  return a->vreg < b->vreg ? -1 : a->vreg > b->vreg;
}

/*
 * Return the segment carrying vreg at an instruction position.  Segments are
 * split at block boundaries and spills, so a copy hint must use the segment
 * active at the copy rather than the first segment for the vreg.
 */
static const ny_mach_live_segment_t *
segment_at(const ny_mach_regalloc_t *a, uint32_t vreg, size_t pc) {
  for (size_t i = 0; i < a->segment_len; ++i) {
    const ny_mach_live_segment_t *s = &a->segments[i];
    if (s->vreg == vreg && s->start <= pc && pc <= s->end)
      return s;
  }
  return NULL;
}

/*
 * Copies are the allocator's strongest local affinity signal.  Prefer the
 * source color when it is already assigned and the source/destination
 * segments overlap the copy.  This is deliberately a bounded scan over
 * machine copies; unknown or cross-class copies simply provide no hint.
 */
static int copy_hint_color(const ny_mach_func_t *mach,
                           const ny_mach_regalloc_t *a,
                           uint32_t dst_vreg, size_t start, size_t end) {
  if (!mach)
    return -1;
  for (size_t pc = start; pc <= end && pc < mach->inst_len; ++pc) {
    const ny_mach_inst_t *in = &mach->insts[pc];
    if (in->opcode != NY_MACH_COPY ||
        in->dst.kind != NY_MACH_OPERAND_VREG ||
        in->src0.kind != NY_MACH_OPERAND_VREG ||
        in->dst.as.reg != dst_vreg)
      continue;
    const ny_mach_live_segment_t *src =
        segment_at(a, in->src0.as.reg, pc);
    if (src && src->color >= 0)
      return src->color;
  }
  return -1;
}

static int cmp_seg_vreg(const void *pa, const void *pb) {
  const ny_mach_live_segment_t *a = pa, *b = pb;
  if (a->vreg != b->vreg)
    return a->vreg < b->vreg ? -1 : 1;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  return a->end < b->end ? -1 : a->end > b->end;
}

static void active_remove(active_t *a, size_t *n, size_t i) {
  if (i + 1 < *n)
    memmove(&a[i], &a[i + 1], (*n - i - 1) * sizeof(*a));
  --*n;
}

/*
 * Per-block loop nesting depth, approximated from backward edges in the
 * block layout (target index <= source index), consistent with the
 * layout assumption coalesce_loop_edges already relies on ("backedge =
 * srcseg->block > h"). Every backward edge b -> t contributes one level
 * of nesting to every block in [t, b]; well-structured (reducible) nested
 * loops accumulate depth correctly since inner ranges sit inside outer
 * ones. Returns NULL (treated as all-zero depth) on allocation failure or
 * when there are no blocks, so callers can pass the result straight
 * through without a separate null check.
 */
static size_t *compute_loop_depth(size_t nb, const size_vec_t *succ) {
  if (!nb || !succ)
    return NULL;
  size_t *depth = calloc(nb, sizeof(*depth));
  if (!depth)
    return NULL;
  for (size_t b = 0; b < nb; ++b) {
    for (size_t i = 0; i < succ[b].len; ++i) {
      size_t t = succ[b].data[i];
      if (t <= b)
        for (size_t k = t; k <= b; ++k)
          depth[k]++;
    }
  }
  return depth;
}

/*
 * Eviction quality is a tuple, not one ad-hoc scalar.  Live ranges are split
 * into per-block segments, so a value defined outside a loop but consumed on
 * every loop iteration gets the loop block's depth/heat on its live-in
 * segment.  Within that block we additionally protect values with a nearby
 * next use and high use density.  Loaded NyP profile heat is consumed through
 * the machine block's explicit source_pc bridge when available.
 *
 * Larger `no_future_use`/next-use distance and lower depth/heat/use-count make
 * a segment a better eviction candidate.  The final end/vreg tie-breakers keep
 * allocation deterministic.  Segment spans are capped at NY_MACH_SPLIT_SPAN,
 * so the bounded use scans below do not turn allocation into a whole-function
 * quadratic walk.
 */
typedef struct {
  bool no_future_use;
  size_t next_use_distance;
  size_t use_count;
  size_t loop_depth;
  uint64_t profile_heat;
  size_t end;
  uint32_t vreg;
} eviction_score_t;

static void segment_use_metrics(const ny_mach_func_t *mach,
                                const ny_mach_live_segment_t *seg,
                                size_t at, size_t *next_distance,
                                size_t *use_count) {
  *next_distance = SIZE_MAX;
  *use_count = 0;
  if (!mach || !seg || seg->start > seg->end || at > seg->end)
    return;
  size_t begin = at > seg->start ? at : seg->start;
  for (size_t pc = begin; pc <= seg->end && pc < mach->inst_len; ++pc) {
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t n = inst_uses(&mach->insts[pc], uses,
                         sizeof(uses) / sizeof(*uses));
    bool used_here = false;
    for (size_t i = 0; i < n; ++i) {
      if (uses[i] != seg->vreg)
        continue;
      used_here = true;
      ++*use_count;
    }
    if (used_here && *next_distance == SIZE_MAX)
      *next_distance = pc - at;
  }
}

static uint64_t segment_profile_heat(const ny_mach_func_t *mach,
                                     uint32_t block) {
  if (!mach || block >= mach->block_len)
    return 0;
  uint32_t pc = mach->blocks[block].source_pc;
  if (pc == UINT32_MAX)
    return 0;
  uint64_t block_heat = ny_native_profile_block_hot(pc);
  uint64_t loop_heat = ny_native_profile_loop_hot(pc);
  if (UINT64_MAX - block_heat < loop_heat)
    return UINT64_MAX;
  return block_heat + loop_heat;
}

static eviction_score_t eviction_score(const ny_mach_func_t *mach,
                                        const ny_mach_live_segment_t *seg,
                                        size_t at,
                                        const size_t *loop_depth) {
  eviction_score_t score = {0};
  if (!seg)
    return score;
  score.end = seg->end;
  score.vreg = seg->vreg;
  score.loop_depth = loop_depth && seg->block != UINT32_MAX
                         ? loop_depth[seg->block]
                         : 0;
  score.profile_heat = segment_profile_heat(mach, seg->block);
  segment_use_metrics(mach, seg, at, &score.next_use_distance,
                      &score.use_count);
  score.no_future_use = score.next_use_distance == SIZE_MAX;
  return score;
}

static bool eviction_score_better(eviction_score_t a, eviction_score_t b) {
  if (a.no_future_use != b.no_future_use)
    return a.no_future_use;
  if (a.profile_heat != b.profile_heat)
    return a.profile_heat < b.profile_heat;
  if (a.loop_depth != b.loop_depth)
    return a.loop_depth < b.loop_depth;
  if (a.next_use_distance != b.next_use_distance)
    return a.next_use_distance > b.next_use_distance;
  if (a.use_count != b.use_count)
    return a.use_count < b.use_count;
  if (a.end != b.end)
    return a.end > b.end;
  return a.vreg > b.vreg;
}

static bool color_segments(const ny_mach_func_t *mach,
                           ny_mach_regalloc_t *a,
                           const size_t *loop_depth) {
  if (!a->segment_len || !a->color_count)
    return true;
  qsort(a->segments, a->segment_len, sizeof(*a->segments), cmp_seg_start);
  active_t *active = calloc(a->color_count, sizeof(*active));
  bool *used = calloc(a->color_count, sizeof(*used));
  if (!active || !used) {
    free(active);
    free(used);
    return false;
  }
  size_t active_n = 0;
  for (size_t i = 0; i < a->segment_len; ++i) {
    ny_mach_live_segment_t *s = &a->segments[i];
    memset(used, 0, a->color_count * sizeof(*used));
    for (size_t j = 0; j < active_n;) {
      if (active[j].end < s->start)
        active_remove(active, &active_n, j);
      else
        ++j;
    }
    for (size_t j = 0; j < active_n; ++j)
      if (active[j].color >= 0 && (size_t)active[j].color < a->color_count)
        used[active[j].color] = true;
    /*
     * Try copy affinity before the ordinary lowest-numbered color.  A hint
     * is advisory only: overlapping live ranges and spills retain priority.
     */
    int color = copy_hint_color(mach, a, s->vreg, s->start, s->end);
    if (color < 0 || (size_t)color >= a->color_count || used[color])
      color = -1;
    if (color < 0) {
      for (size_t c = 0; c < a->color_count; ++c)
        if (!used[c]) {
          color = (int)c;
          break;
        }
    }
    if (color < 0) {
      size_t victim = SIZE_MAX;
      eviction_score_t best = eviction_score(mach, s, s->start, loop_depth);
      for (size_t j = 0; j < active_n; ++j) {
        ny_mach_live_segment_t *candidate = &a->segments[active[j].seg];
        eviction_score_t score =
            eviction_score(mach, candidate, s->start, loop_depth);
        if (eviction_score_better(score, best)) {
          best = score;
          victim = j;
        }
      }
      if (victim != SIZE_MAX) {
        ny_mach_live_segment_t *old = &a->segments[active[victim].seg];
        color = active[victim].color;
        old->color = -1;
        old->reload = true;
        old->spill = true;
        active_remove(active, &active_n, victim);
      }
    }
    s->color = color;
    if (color >= 0) {
      active[active_n++] =
          (active_t){.start = s->start, .end = s->end, .seg = i, .color = color};
    }
  }
  free(active);
  free(used);
  return true;
}
static bool block_has_color_conflict(const ny_mach_regalloc_t *a,
                                     const ny_mach_live_segment_t *target,
                                     int color,
                                     const size_vec_t *by_block) {
  /*
   * Coalescing is an optional optimization.  Without the block index, reject
   * the candidate rather than turning one allocation failure into O(n²)
   * whole-function scans.
   */
  if (!by_block || target->block >= a->block_count)
    return true;
  const size_t *indices = by_block[target->block].data;
  size_t count = by_block[target->block].len;
  for (size_t k = 0; k < count; ++k) {
    const ny_mach_live_segment_t *s = &a->segments[indices[k]];
    if (s == target)
      continue;
    if (s->color != color)
      continue;
    if (!(s->end < target->start || target->end < s->start))
      return true;
  }
  return false;
}

/*
 * Same as block_has_color_conflict, but ignores segments whose vreg is in
 * the exclusion list.  The phi-edge copy pair (dst vreg, src vreg) is the
 * same value moving between pregs, so the pair may share a color without
 * being a real overlap.
 */

/*
 * Carry is allowed to evict a non-carried competing segment to its existing
 * stack home.  A previously carried segment has deliberately suppressed that
 * home store, so it is never an eviction candidate.
 */
static bool block_color_conflicts_evictable_excl(
    const ny_mach_regalloc_t *a, const ny_mach_live_segment_t *target,
    int color, const size_vec_t *by_block, const uint32_t *excl,
    size_t excl_len) {
  if (!by_block || target->block >= a->block_count)
    return false;
  const size_t *indices = by_block[target->block].data;
  size_t count = by_block[target->block].len;
  for (size_t k = 0; k < count; ++k) {
    const ny_mach_live_segment_t *s = &a->segments[indices[k]];
    if (s == target || s->color != color ||
        s->end < target->start || target->end < s->start)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded && s->carried)
      return false;
  }
  return true;
}

static void block_evict_color_conflicts_excl(
    ny_mach_regalloc_t *a, const ny_mach_live_segment_t *target, int color,
    const size_vec_t *by_block, const uint32_t *excl, size_t excl_len) {
  const size_t *indices = by_block[target->block].data;
  size_t count = by_block[target->block].len;
  for (size_t k = 0; k < count; ++k) {
    ny_mach_live_segment_t *s = &a->segments[indices[k]];
    if (s == target || s->color != color ||
        s->end < target->start || target->end < s->start)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded) {
      s->color = -1;
      s->reload = true;
      s->spill = true;
      s->carried = false;
    }
  }
}

static bool block_color_all_conflicts_evictable_excl(
    const ny_mach_regalloc_t *a, size_t block, int color,
    const size_vec_t *by_block, const uint32_t *excl, size_t excl_len) {
  if (!a || !by_block || block >= a->block_count)
    return false;
  for (size_t k = 0; k < by_block[block].len; ++k) {
    const ny_mach_live_segment_t *s =
        &a->segments[by_block[block].data[k]];
    if (s->color != color)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded && s->carried)
      return false;
  }
  return true;
}

static void block_evict_color_all_excl(
    ny_mach_regalloc_t *a, size_t block, int color,
    const size_vec_t *by_block, const uint32_t *excl, size_t excl_len) {
  for (size_t k = 0; k < by_block[block].len; ++k) {
    ny_mach_live_segment_t *s = &a->segments[by_block[block].data[k]];
    if (s->color != color)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded) {
      s->color = -1;
      s->reload = true;
      s->spill = true;
      s->carried = false;
    }
  }
}

static bool segment_unique_in_block_vreg(const ny_mach_regalloc_t *a,
                                          uint32_t vreg, size_t block,
                                          const size_vec_t *by_vreg) {
  if (!a || !by_vreg || vreg >= a->vreg_len)
    return false;
  size_t count = 0;
  for (size_t k = 0; k < by_vreg[vreg].len; ++k)
    if (a->segments[by_vreg[vreg].data[k]].block == block)
      ++count;
  return count == 1;
}

/*
 * Copy-coalesce phi-edge segments when a conflict-free color exists across
 * the header, edge copy, and predecessor value. Spill/reload homes remain
 * authoritative: nested CFG paths may reload or spill independently, so this
 * pass only makes the redundant edge copy register-to-register.
 */
typedef struct {
  size_t edge; /* edge block index */
  uint32_t src;
  ny_mach_live_segment_t *ev;     /* phi dst segment in the edge block */
  ny_mach_live_segment_t *eseg;   /* src segment in the edge block */
  ny_mach_live_segment_t *srcseg; /* src segment in the pred block (end) */
} loop_copy_t;

static void coalesce_loop_edges(const ny_mach_func_t *mach,
                                ny_mach_regalloc_t *a,
                                const size_vec_t *pred,
                                const size_vec_t *by_block) {
  if (!mach || !a || !pred || !by_block || mach->block_len < 2)
    return;
  /*
   * Carry coalescing is part of the normal GPR allocation path.  It only
   * applies when its structural checks below prove the loop chain safe.
   */
  size_t nb = mach->block_len;
  /*
   * Process each loop header independently to carry registers across backedges.
   */
  size_t nv = a->vreg_len;
  size_vec_t *by_vreg = calloc(nv, sizeof(*by_vreg));
  if (!by_vreg)
    return;
  for (size_t i = 0; i < a->segment_len; ++i) {
    uint32_t v = a->segments[i].vreg;
    if (v >= nv || !size_vec_push(&by_vreg[v], i)) {
      for (size_t k = 0; k < nv; ++k)
        free(by_vreg[k].data);
      free(by_vreg);
      return;
    }
  }
  for (size_t h = 0; h < nb; ++h) {
    if (pred[h].len < 2)
      continue;
    const ny_mach_block_t *hb = &mach->blocks[h];
    if (!hb->inst_count)
      continue;

    /*
     * Edge blocks of the header: blocks ending in a branch to h.
     */
    size_t edge_count = 0;
    size_t edge_blocks[256];
    for (size_t e = 0; e < nb && edge_count < 256; ++e) {
      if (e == h)
        continue;
      const ny_mach_block_t *eb = &mach->blocks[e];
      if (!eb->inst_count)
        continue;
      const ny_mach_inst_t *last =
          &mach->insts[eb->first_inst + eb->inst_count - 1];
      if ((last->opcode == NY_MACH_BR || last->opcode == NY_MACH_BR_IF) &&
          last->src1.kind == NY_MACH_OPERAND_BLOCK &&
          last->src1.as.block_index == h)
        edge_blocks[edge_count++] = e;
    }
    if (getenv("NY_CARRY_DEBUG"))
      fprintf(stderr, "carry: header=%zu preds=%zu edges=%zu\n", h,
              pred[h].len, edge_count);
    if (getenv("NY_CARRY_DEBUG"))
      for (size_t si = 0; si < by_block[h].len; ++si) {
        const ny_mach_live_segment_t *seg =
            &a->segments[by_block[h].data[si]];
        fprintf(stderr,
                "carry: header=%zu v=%u span=%zu-%zu color=%d reload=%d\n",
                h, seg->vreg, seg->start, seg->end, seg->color, seg->reload);
      }
    if (edge_count != pred[h].len)
      continue;

    /*
     * Discover the predecessor region before choosing a color. Every segment
     * of the phi destination that reaches the header must remain conflict-free
     * in its own block even though its reload/spill home stays intact.
     */
    bool *edge_block = calloc(nb, sizeof(*edge_block));
    bool *reaches_h = calloc(nb, sizeof(*reaches_h));
    size_t *work = malloc(nb * sizeof(*work));
    if (!edge_block || !reaches_h || !work) {
      free(edge_block);
      free(reaches_h);
      free(work);
      continue;
    }
    for (size_t ei = 0; ei < edge_count; ++ei)
      edge_block[edge_blocks[ei]] = true;
    size_t work_len = 1;
    reaches_h[h] = true;
    work[0] = h;
    for (size_t wi = 0; wi < work_len; ++wi) {
      size_t block = work[wi];
      for (size_t pi = 0; pi < pred[block].len; ++pi) {
        size_t predecessor = pred[block].data[pi];
        if (predecessor < nb && !reaches_h[predecessor]) {
          reaches_h[predecessor] = true;
          work[work_len++] = predecessor;
        }
      }
    }

    for (size_t i = 0; i < a->segment_len; ++i) {
      ny_mach_live_segment_t *hv = &a->segments[i];
      if (!hv->reload || hv->block >= nb || edge_block[hv->block] ||
          !reaches_h[hv->block])
        continue;
      uint32_t v = hv->vreg;
      if (!segment_unique_in_block_vreg(a, v, hv->block, by_vreg))
        continue;

      loop_copy_t copies[256];
      size_t copy_len = 0;
      bool ok = true;
      for (size_t ei = 0; ei < edge_count; ++ei) {
        size_t e = edge_blocks[ei];
        const ny_mach_block_t *eb = &mach->blocks[e];
        uint32_t src = 0;
        bool found_copy = false;
        for (size_t n = 0; n + 1 < eb->inst_count; ++n) { /* skip branch */
          const ny_mach_inst_t *in = &mach->insts[eb->first_inst + n];
          if (in->opcode == NY_MACH_COPY &&
              in->dst.kind == NY_MACH_OPERAND_VREG &&
              in->dst.as.reg == v &&
              in->src0.kind == NY_MACH_OPERAND_VREG) {
            src = in->src0.as.reg;
            found_copy = true;
            break;
          }
        }
        if (!found_copy || src >= a->vreg_len) {
          if (getenv("NY_CARRY_DEBUG"))
            fprintf(stderr, "carry: header=%zu v=%u edge=%zu no phi copy\n",
                    h, v, e);
          ok = false;
          break;
        }
        ny_mach_live_segment_t *ev = NULL, *eseg = NULL;
        for (size_t si = 0; si < by_vreg[v].len; ++si) {
          ny_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
          if (s->block == e) { ev = s; break; }
        }
        for (size_t si = 0; si < by_vreg[src].len; ++si) {
          ny_mach_live_segment_t *s = &a->segments[by_vreg[src].data[si]];
          if (s->block == e) { eseg = s; break; }
        }
        if (!ev || !eseg ||
            !segment_unique_in_block_vreg(a, v, e, by_vreg) ||
            !segment_unique_in_block_vreg(a, src, e, by_vreg)) {
          ok = false;
          break;
        }
        if (pred[e].len != 1) {
          ok = false;
          break;
        }
        size_t p = pred[e].data[0];
        size_t bend =
            mach->blocks[p].first_inst + mach->blocks[p].inst_count - 1;
        ny_mach_live_segment_t *srcseg = NULL;
        for (size_t si = 0; si < by_vreg[src].len; ++si) {
          ny_mach_live_segment_t *s = &a->segments[by_vreg[src].data[si]];
          if (s->block == p && s->end == bend) { srcseg = s; break; }
        }
        if (!srcseg ||
            !segment_unique_in_block_vreg(a, src, p, by_vreg)) {
          ok = false;
          break;
        }
        copies[copy_len++] = (loop_copy_t){
            .edge = e, .src = src, .ev = ev, .eseg = eseg, .srcseg = srcseg};
      }
      if (!ok || copy_len != edge_count)
        continue;

      /*
       * Candidate colors: distinct srcseg colors, backedges (pred after the
       * header in the base layout) first so the hot path wins conflicts.
       */
      int cands[64];
      size_t ncand = 0;
      for (int pass = 0; pass < 2; ++pass) {
        for (size_t ci = 0; ci < copy_len; ++ci) {
          bool back = copies[ci].srcseg->block > h;
          if ((pass == 0) != back)
            continue;
          int c = copies[ci].srcseg->color;
          if (c < 0)
            continue;
          bool seen = false;
          for (size_t k = 0; k < ncand; ++k)
            if (cands[k] == c) {
              seen = true;
              break;
            }
          if (!seen && ncand < 64)
            cands[ncand++] = c;
        }
      }
      for (size_t color = 0; color < a->color_count && ncand < 64; ++color) {
        bool seen = false;
        for (size_t k = 0; k < ncand; ++k)
          if (cands[k] == (int)color) {
            seen = true;
            break;
          }
        if (!seen)
          cands[ncand++] = (int)color;
      }
      int chosen = -1;
      for (size_t ci = 0; ci < ncand; ++ci) {
        int C = cands[ci];
        uint32_t excl_v[1] = {v};
        if (!block_color_conflicts_evictable_excl(a, hv, C, by_block,
                                                  excl_v, 1) ||
            !block_color_all_conflicts_evictable_excl(
                a, h, C, by_block, excl_v, 1))
          continue;
        bool all_ok = true;
        for (size_t k = 0; k < copy_len; ++k) {
          loop_copy_t *c = &copies[k];
          uint32_t excl[2] = {v, c->src};
          if (!block_color_conflicts_evictable_excl(a, c->ev, C, by_block,
                                                    excl, 2) ||
              !block_color_conflicts_evictable_excl(a, c->eseg, C, by_block,
                                                    excl, 2) ||
              !block_color_conflicts_evictable_excl(a, c->srcseg, C, by_block,
                                                    excl + 1, 1)) {
            all_ok = false;
            break;
          }
        }
        if (!all_ok)
          continue;
        for (size_t si = 0; si < by_vreg[v].len; ++si) {
          ny_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
          if (s == hv || s->block >= nb ||
              edge_block[s->block] || !reaches_h[s->block])
            continue;
          if (!block_color_conflicts_evictable_excl(a, s, C, by_block,
                                                    excl_v, 1)) {
            all_ok = false;
            break;
          }
        }
        if (all_ok) {
          chosen = C;
          break;
        }
      }
      if (chosen < 0)
        continue;

      uint32_t excl_v[1] = {v};
      block_evict_color_conflicts_excl(a, hv, chosen, by_block, excl_v, 1);
      block_evict_color_all_excl(a, h, chosen, by_block, excl_v, 1);
      for (size_t si = 0; si < by_vreg[v].len; ++si) {
        ny_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
        if (s != hv && s->block < nb && !edge_block[s->block] &&
            reaches_h[s->block])
          block_evict_color_conflicts_excl(a, s, chosen, by_block,
                                           excl_v, 1);
      }
      for (size_t k = 0; k < copy_len; ++k) {
        loop_copy_t *c = &copies[k];
        uint32_t excl[2] = {v, c->src};
        block_evict_color_conflicts_excl(a, c->ev, chosen, by_block,
                                         excl, 2);
        block_evict_color_conflicts_excl(a, c->eseg, chosen, by_block,
                                         excl, 2);
        block_evict_color_conflicts_excl(a, c->srcseg, chosen, by_block,
                                         excl + 1, 1);
      }
      hv->color = chosen;
      for (size_t si = 0; si < by_vreg[v].len; ++si) {
        ny_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
        if (s == hv || s->block >= nb ||
            edge_block[s->block] || !reaches_h[s->block])
          continue;
        s->color = chosen;
      }
      if (getenv("NY_CARRY_DEBUG")) {
        fprintf(stderr, "carry: h=%zu v=%u C=%d", h, v, chosen);
        for (size_t k = 0; k < copy_len; ++k)
          fprintf(stderr, " edge=%zu src=%u evc=%d esc=%d srcsc=%d",
                  copies[k].edge, copies[k].src, copies[k].ev->color,
                  copies[k].eseg->color, copies[k].srcseg->color);
        fprintf(stderr, "\n");
      }
    }

    /*
     * Retain every spill/reload boundary. Color equality can still eliminate
     * the edge copy, while homes remain authoritative across nested CFG.
     */
    free(edge_block);
    free(reaches_h);
    free(work);
  }

  for (size_t v = 0; v < nv; ++v)
    free(by_vreg[v].data);
  free(by_vreg);
}

static void coalesce_segments(const ny_mach_func_t *mach,
                              ny_mach_regalloc_t *a,
                              const size_vec_t *succ,
                              const size_vec_t *pred,
                              const size_vec_t *by_block) {
  /*
   * Block membership is fixed after coloring, so the caller builds this
   * index once and shares it with the loop-edge coalescer.
   */

  /*
   * Sort by vreg so all segments of the same vreg are contiguous.  Then a
   * single linear pass finds adjacent same-vreg pairs: cmp_seg_vreg orders
   * by (vreg, start, end), so x.end+1 == y.start and x.vreg == y.vreg can
   * only hold for consecutive elements.  This replaces the O(n^2) pairwise
   * scan with O(n log n) sort + O(n) pass.  The final qsort in the caller
   * re-sorts by vreg anyway, so we leave the array vreg-ordered.
   */
  for (size_t i = 0; i + 1 < a->segment_len; ++i) {
    ny_mach_live_segment_t *x = &a->segments[i];
    ny_mach_live_segment_t *y = &a->segments[i + 1];
    if (x->vreg != y->vreg || x->color < 0 || y->color < 0)
      continue;
    if (y->start != x->end + 1)
      continue;
    if (x->block == y->block) {
      if (!block_has_color_conflict(a, y, x->color, by_block)) {
        y->color = x->color;
        x->spill = false;
        y->spill = false;
        y->reload = false;
      }
      continue;
    }
    if (x->block >= mach->block_len || y->block >= mach->block_len ||
        y->block <= x->block || succ[x->block].len != 1 ||
        succ[x->block].data[0] != y->block || pred[y->block].len != 1 ||
        pred[y->block].data[0] != x->block)
      continue;
    const ny_mach_block_t *xb = &mach->blocks[x->block];
    const ny_mach_block_t *yb = &mach->blocks[y->block];
    if (x->end + 1 != xb->first_inst + xb->inst_count ||
        y->start != yb->first_inst)
      continue;
    if (!block_has_color_conflict(a, y, x->color, by_block)) {
      y->color = x->color;
      x->spill = false;
      y->spill = false;
      y->reload = false;
    }
  }
}

bool ny_mach_regalloc_build_class(const ny_mach_func_t *mach,
                                   ny_mach_reg_class_t reg_class,
                                   size_t color_count,
                                   ny_mach_regalloc_t *out);
__attribute__((used)) bool ny_mach_regalloc_build_class(const ny_mach_func_t *mach,
                                   ny_mach_reg_class_t reg_class,
                                   size_t color_count,
                                   ny_mach_regalloc_t *out) {
  if (!mach || !out)
    return false;
  ny_mach_regalloc_t a = {.vreg_len = mach->vreg_len,
                          .color_count = color_count,
                          .block_count = mach->block_len};
  size_t nb = mach->block_len, nv = mach->vreg_len;
  size_t words = bit_words(nv ? nv : 1);
  a.bit_words = words;
  size_t *use = calloc((nb ? nb : 1) * words, sizeof(size_t));
  size_t *def = calloc((nb ? nb : 1) * words, sizeof(size_t));
  size_t *first = NULL;
  size_t *last = NULL;
  bool *first_use = NULL;
  bool *seen_def = NULL;
  a.live_in = calloc((nb ? nb : 1) * words, sizeof(size_t));
  a.live_out = calloc((nb ? nb : 1) * words, sizeof(size_t));
  size_vec_t *succ = calloc(nb ? nb : 1, sizeof(*succ));
  size_vec_t *pred = calloc(nb ? nb : 1, sizeof(*pred));
  if (!use || !def || !a.live_in || !a.live_out || !succ || !pred)
    goto fail;

  for (size_t bi = 0; bi < nb; ++bi) {
    const ny_mach_block_t *b = &mach->blocks[bi];
    bool falls = true;
    for (size_t n = 0; n < b->inst_count; ++n) {
      const ny_mach_inst_t *in = &mach->insts[b->first_inst + n];
      uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
      size_t un = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
      for (size_t k = 0; k < un; ++k) {
        uint32_t v = uses[k];
        if (v < nv && !bit_get(def + bi * words, v))
          bit_set(use + bi * words, v);
      }
      if (dst_is_def(in) && in->dst.as.reg < nv)
        bit_set(def + bi * words, in->dst.as.reg);
      if (in->opcode == NY_MACH_BR || in->opcode == NY_MACH_BR_IF) {
        uint32_t t = in->src1.as.block_index;
        if (t < nb && !size_vec_push(&succ[bi], t))
          goto fail;
        if (in->opcode == NY_MACH_BR)
          falls = false;
      } else if (in->opcode == NY_MACH_RET) {
        falls = false;
      }
    }
    if (falls && bi + 1 < nb && !size_vec_push(&succ[bi], bi + 1))
      goto fail;
  }
  for (size_t b = 0; b < nb; ++b)
    for (size_t i = 0; i < succ[b].len; ++i)
      if (!size_vec_push(&pred[succ[b].data[i]], b))
        goto fail;

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t bi = nb; bi-- > 0;) {
      for (size_t w = 0; w < words; ++w) {
        size_t old_in = a.live_in[bi * words + w];
        size_t old_out = a.live_out[bi * words + w];
        size_t outw = 0;
        for (size_t s = 0; s < succ[bi].len; ++s)
          outw |= a.live_in[succ[bi].data[s] * words + w];
        size_t inw = use[bi * words + w] |
                     (outw & ~def[bi * words + w]);
        a.live_out[bi * words + w] = outw;
        a.live_in[bi * words + w] = inw;
        changed |= old_in != inw || old_out != outw;
      }
    }
  }

  first = malloc((nv ? nv : 1) * sizeof(*first));
  last = malloc((nv ? nv : 1) * sizeof(*last));
  first_use = calloc(nv ? nv : 1, sizeof(*first_use));
  seen_def = calloc(nv ? nv : 1, sizeof(*seen_def));
  if (!first || !last || !first_use || !seen_def)
    goto fail;
  for (size_t bi = 0; bi < nb; ++bi) {
    const ny_mach_block_t *b = &mach->blocks[bi];
    size_t bend = b->inst_count ? b->first_inst + b->inst_count - 1
                               : b->first_inst;
    for (size_t v = 0; v < nv; ++v) {
      first[v] = SIZE_MAX;
      last[v] = 0;
      first_use[v] = false;
      seen_def[v] = false;
    }
    for (size_t n = 0; n < b->inst_count; ++n) {
      size_t pos = b->first_inst + n;
      const ny_mach_inst_t *in = &mach->insts[pos];
      uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
      size_t un = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
      for (size_t k = 0; k < un; ++k) {
        uint32_t v = uses[k];
        if (v >= nv || !is_alloc_type(mach->vreg_types[v], reg_class))
          continue;
        if (first[v] == SIZE_MAX) {
          first[v] = pos;
          first_use[v] = !seen_def[v];
        }
        last[v] = pos;
      }
      if (dst_is_def(in)) {
        uint32_t v = in->dst.as.reg;
        if (v < nv && is_alloc_type(mach->vreg_types[v], reg_class)) {
          if (first[v] == SIZE_MAX)
            first[v] = pos;
          last[v] = pos;
          seen_def[v] = true;
        }
      }
    }
    for (uint32_t v = 0; v < nv; ++v) {
      bool lin = bit_get(a.live_in + bi * words, v);
      bool lout = bit_get(a.live_out + bi * words, v);
      if (!is_alloc_type(mach->vreg_types[v], reg_class) ||
          (first[v] == SIZE_MAX && !lin && !lout))
        continue;
      size_t start = first[v] == SIZE_MAX ? b->first_inst : first[v];
      size_t end = first[v] == SIZE_MAX ? bend : last[v];
      if (lin)
        start = b->first_inst;
      if (lout)
        end = bend;
      bool reload = lin || first_use[v];
      for (size_t s = start; s <= end;) {
        size_t e = s + NY_MACH_SPLIT_SPAN - 1;
        if (e > end)
          e = end;
        bool more = e < end;
        if (!append_segment(&a, v, (uint32_t)bi, s, e, reload,
                            more || lout))
          goto fail;
        reload = true;
        if (e == SIZE_MAX)
          break;
        s = e + 1;
      }
    }
  }
  free(first);
  free(last);
  free(first_use);
  free(seen_def);
  first = last = NULL;
  first_use = seen_def = NULL;

  {
    size_t *loop_depth = compute_loop_depth(nb, succ);
    bool colored = color_segments(mach, &a, loop_depth);
    free(loop_depth);
    if (!colored)
      goto fail;
  }
  /*
   * The block index stores segment-array indices.  Establish the final
   * vreg ordering before building it; sorting afterward would make every
   * conflict lookup address a different segment and could coalesce
   * overlapping live ranges onto the same physical register.
   */
  qsort(a.segments, a.segment_len, sizeof(*a.segments), cmp_seg_vreg);
  size_vec_t *by_block = build_segment_by_block(mach, &a);
  coalesce_segments(mach, &a, succ, pred, by_block);
  coalesce_loop_edges(mach, &a, pred, by_block);
  free_segment_by_block(mach, by_block);
  by_block = NULL;
  qsort(a.segments, a.segment_len, sizeof(*a.segments), cmp_seg_vreg);
  a.vreg_offsets = calloc(nv + 1, sizeof(*a.vreg_offsets));
  if (!a.vreg_offsets)
    goto fail;
  size_t si = 0;
  for (size_t v = 0; v < nv; ++v) {
    a.vreg_offsets[v] = si;
    while (si < a.segment_len && a.segments[si].vreg == v)
      ++si;
  }
  a.vreg_offsets[nv] = si;

  for (size_t b = 0; b < nb; ++b) {
    free(succ[b].data);
    free(pred[b].data);
  }
  free(succ);
  free(pred);
  free(use);
  free(def);
  *out = a;
  return true;

fail:
  if (succ)
    for (size_t b = 0; b < nb; ++b)
      free(succ[b].data);
  if (pred)
    for (size_t b = 0; b < nb; ++b)
      free(pred[b].data);
  free(succ);
  free(pred);
  free(use);
  free(def);
  free(first);
  free(last);
  free(first_use);
  free(seen_def);
  ny_mach_regalloc_free(&a);
  return false;
}

bool ny_mach_regalloc_build(const ny_mach_func_t *mach, size_t color_count,
                            ny_mach_regalloc_t *out) {
  return ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_GPR,
                                      color_count, out);
}

void ny_mach_regalloc_free(ny_mach_regalloc_t *a) {
  if (!a)
    return;
  free(a->segments);
  free(a->vreg_offsets);
  free(a->live_in);
  free(a->live_out);
  *a = (ny_mach_regalloc_t){0};
}

const ny_mach_live_segment_t *
ny_mach_regalloc_segment_at(const ny_mach_regalloc_t *a, uint32_t vreg,
                            size_t inst) {
  if (!a || !a->vreg_offsets || vreg >= a->vreg_len)
    return NULL;
  size_t lo = a->vreg_offsets[vreg], hi = a->vreg_offsets[vreg + 1];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const ny_mach_live_segment_t *s = &a->segments[mid];
    if (inst < s->start)
      hi = mid;
    else if (inst > s->end)
      lo = mid + 1;
    else
      return s;
  }
  return NULL;
}

size_t ny_mach_regalloc_peak_live(const ny_mach_regalloc_t *a,
                                  size_t inst_len) {
  if (!a || a->segment_len == 0 || inst_len == 0)
    return 0;
  int *delta = calloc(inst_len + 1, sizeof(*delta));
  if (!delta)
    return 0;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &a->segments[i];
    if (seg->start >= inst_len)
      continue;
    size_t end = seg->end < inst_len ? seg->end : inst_len - 1;
    ++delta[seg->start];
    if (end + 1 < inst_len)
      --delta[end + 1];
  }
  size_t peak = 0;
  int live = 0;
  for (size_t pc = 0; pc < inst_len; ++pc) {
    live += delta[pc];
    if (live > 0 && (size_t)live > peak)
      peak = (size_t)live;
  }
  free(delta);
  return peak;
}

bool ny_mach_regalloc_live_in(const ny_mach_regalloc_t *a, size_t block,
                              uint32_t vreg) {
  return a && a->live_in && block < a->block_count && vreg < a->vreg_len &&
         bit_get(a->live_in + block * a->bit_words, vreg);
}

bool ny_mach_regalloc_live_out(const ny_mach_regalloc_t *a, size_t block,
                               uint32_t vreg) {
  return a && a->live_out && block < a->block_count && vreg < a->vreg_len &&
         bit_get(a->live_out + block * a->bit_words, vreg);
}

bool ny_mach_linear_scan_ranges(ny_mach_func_t *mach, int *colors_out,
                                size_t *live_start, size_t *live_end,
                                size_t colors_cap) {
  if (!mach || !colors_out || colors_cap < mach->vreg_len)
    return false;
  ny_mach_regalloc_t a = {0};
  if (!ny_mach_regalloc_build(mach, 8, &a))
    return false;
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    colors_out[v] = -1;
    if (live_start)
      live_start[v] = 0;
    if (live_end)
      live_end[v] = 0;
    size_t best = 0;
    for (size_t i = a.vreg_offsets[v]; i < a.vreg_offsets[v + 1]; ++i) {
      const ny_mach_live_segment_t *s = &a.segments[i];
      size_t span = s->end - s->start + 1;
      if (s->color >= 0 && span > best) {
        best = span;
        colors_out[v] = s->color;
        if (live_start)
          live_start[v] = s->start;
        if (live_end)
          live_end[v] = s->end;
      }
    }
  }
  ny_mach_regalloc_free(&a);
  return true;
}

bool ny_mach_linear_scan(ny_mach_func_t *mach, int *colors_out,
                         size_t colors_cap) {
  return ny_mach_linear_scan_ranges(mach, colors_out, NULL, NULL, colors_cap);
}
