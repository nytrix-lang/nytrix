/*
 * Register allocator: graph-coloring and linear-scan register
 * assignment for XMM, FPR, GPR, and NEON register classes on all targets.
 */
#include "base/common.h"
#include "base/debug.h"
#include "code/backend/core/nyr.h"
#include "code/backend/lower/machine.h"
#include <nyr/ir.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  size_t use_cursor;
  size_t use_end;
  size_t loop_depth;
  uint64_t profile_heat;
  bool eviction_metadata_ready;
} active_t;

typedef struct {
  size_t *offsets;
  size_t *positions;
  size_t vreg_len;
} regalloc_use_index_t;

static const char *reg_class_name(nyr_mach_reg_class_t reg_class) {
  switch (reg_class) {
  case NYR_MACH_REGCLASS_GPR:
    return "GPR";
  case NYR_MACH_REGCLASS_FPR:
    return "FPR";
  case NYR_MACH_REGCLASS_VECTOR:
    return "VECTOR";
  case NYR_MACH_REGCLASS_FLAGS:
    return "FLAGS";
  default:
    return "NONE";
  }
}

static size_t bit_words(size_t n) {
  size_t bits = sizeof(size_t) * 8;
  return (n + bits - 1) / bits;
}

static void bit_set(size_t *b, size_t i) {
  b[i / (sizeof(size_t) * 8)] |= (size_t)1 << (i % (sizeof(size_t) * 8));
}

static bool bit_get(const size_t *b, size_t i) {
  return (b[i / (sizeof(size_t) * 8)] >> (i % (sizeof(size_t) * 8))) & 1;
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

static bool regalloc_touch_vreg(size_vec_t *block_values, bool *in_block,
                                size_t *first, size_t *last, bool *first_use,
                                bool *seen_def, uint32_t v) {
  if (in_block[v])
    return true;
  in_block[v] = true;
  first[v] = SIZE_MAX;
  last[v] = 0;
  first_use[v] = false;
  seen_def[v] = false;
  return size_vec_push(block_values, v);
}
static size_vec_t *build_segment_by_block(const nyr_mach_func_t *mach,
                                          const nyr_mach_regalloc_t *a) {
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

static void free_segment_by_block(const nyr_mach_func_t *mach,
                                  size_vec_t *by_block) {
  if (!mach || !by_block)
    return;
  for (size_t b = 0; b < mach->block_len; ++b)
    free(by_block[b].data);
  free(by_block);
}

static bool is_alloc_type(nyr_mach_type_t t, nyr_mach_reg_class_t reg_class) {
  if (reg_class == NYR_MACH_REGCLASS_GPR)
    return t == NYR_MACH_TYPE_I64 || t == NYR_MACH_TYPE_PTR;
  if (reg_class == NYR_MACH_REGCLASS_FPR)
    return t == NYR_MACH_TYPE_F32 || t == NYR_MACH_TYPE_F64;
  if (reg_class == NYR_MACH_REGCLASS_VECTOR)
    return t == NYR_MACH_TYPE_V128_I64 || t == NYR_MACH_TYPE_V128_F64 ||
           t == NYR_MACH_TYPE_V128_F32 || t == NYR_MACH_TYPE_V256_I64 ||
           t == NYR_MACH_TYPE_V256_F64;
  return false;
}

static bool dst_is_def(const nyr_mach_inst_t *in) {
  if (!in || in->dst.kind != NYR_MACH_OPERAND_VREG)
    return false;
  return in->opcode != NYR_MACH_STORE;
}

static void add_use(const nyr_mach_operand_t *op, uint32_t *out, size_t *len,
                    size_t cap) {
  if (op && op->kind == NYR_MACH_OPERAND_VREG && *len < cap)
    out[(*len)++] = op->as.reg;
}

static size_t inst_uses(const nyr_mach_inst_t *in, uint32_t *out, size_t cap) {
  size_t n = 0;
  if (!in)
    return 0;
  add_use(&in->src0, out, &n, cap);
  add_use(&in->src1, out, &n, cap);
  add_use(&in->src2, out, &n, cap);
  if (in->opcode == NYR_MACH_STORE)
    add_use(&in->dst, out, &n, cap);
  for (size_t i = 0; i < in->args_len; ++i)
    add_use(&in->args[i], out, &n, cap);
  return n;
}

/*
 * Fixed-register instructions are home barriers only for colors that map to
 * the physical registers they clobber. The x64 GPR color map omits RAX and
 * assigns R8/R9/RCX/RDX to colors 8..11; variable magic DIV/MOD can clobber
 * all four. The verified constant positive power-of-two DIV sequence only
 * needs R11 (RAX is not allocatable); the corresponding MOD sequence needs
 * both RCX and RDX. x64 integer vector reductions clobber RDX and variable
 * shifts clobber RCX. Splitting
 * unaffected values needlessly stores and reloads loop scalars around every
 * divide. Calls use their explicit clobber masks, with zero remaining the
 * conservative all-colors contract for hand-built machine IR.
 * Match the encoder's conservative, one-definition immediate propagation.
 */
static bool mach_constant_i64(const nyr_mach_func_t *mach, uint32_t vreg,
                              size_t depth, int64_t *out) {
  /*
   * This is an optional allocation optimization; keep adversarial copy
   * chains bounded and fall back to conservative clobbers beyond the limit.
   */
  if (!mach || !out || vreg >= mach->vreg_len || depth >= 64 ||
      !mach->vreg_types || !mach->vreg_classes ||
      mach->vreg_types[vreg] != NYR_MACH_TYPE_I64 ||
      mach->vreg_classes[vreg] != NYR_MACH_REGCLASS_GPR)
    return false;
  const nyr_mach_inst_t *def = NULL;
  for (size_t pc = 0; pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (!dst_is_def(in) || in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->dst.as.reg != vreg)
      continue;
    if (def)
      return false;
    def = in;
  }
  if (!def || def->opcode != NYR_MACH_COPY)
    return false;
  if (def->src0.kind == NYR_MACH_OPERAND_IMM) {
    *out = def->src0.as.imm;
    return true;
  }
  if (def->src0.kind != NYR_MACH_OPERAND_VREG ||
      def->src0.as.reg >= mach->vreg_len ||
      mach->vreg_types[def->src0.as.reg] != mach->vreg_types[vreg] ||
      mach->vreg_classes[def->src0.as.reg] != mach->vreg_classes[vreg])
    return false;
  return mach_constant_i64(mach, def->src0.as.reg, depth + 1, out);
}

static uint64_t inst_fixed_clobbers(const nyr_mach_func_t *mach,
                                    const nyr_mach_inst_t *in,
                                    nyr_mach_reg_class_t reg_class,
                                    bool preserve_gpr_calls) {
  if (!in)
    return 0;
  if (in->opcode == NYR_MACH_CALL) {
    if (preserve_gpr_calls && reg_class == NYR_MACH_REGCLASS_GPR)
      return in->call_clobbers | ~((1ULL << 5) - 1);
    if (in->call_clobbers)
      return in->call_clobbers;
    return UINT64_MAX;
  }
  if (reg_class != NYR_MACH_REGCLASS_GPR)
    return 0;
  /*
   * The x64 integer vector reduction lowering uses RDX to hold its scalar
   * accumulator while it reduces the packed lanes. Keep that color out of the
   * live range at the reduction; RAX is already outside the allocatable map.
   */
  if (in->opcode == NYR_MACH_REDUCE_ADD &&
      in->src0.reg_class == NYR_MACH_REGCLASS_GPR)
    return UINT64_C(1) << 11;
  if (in->opcode == NYR_MACH_DIV || in->opcode == NYR_MACH_MOD) {
    /*
     * The x64 encoder selects its signed power-of-two sequence for immediate
     * divisors in 2..INT32_MAX. Keep this classifier aligned with that
     * encoder contract: DIV uses R11 (color 7) as its sign-correction
     * temporary, while MOD uses RCX and RDX to form the remainder. RAX is not
     * allocatable.
     * Other forms use the general magic/idiv sequence.
     */
    int64_t divisor = 0;
    bool known_divisor =
        in->src1.kind == NYR_MACH_OPERAND_IMM
            ? (divisor = in->src1.as.imm, true)
            : in->src1.kind == NYR_MACH_OPERAND_VREG &&
                  mach_constant_i64(mach, in->src1.as.reg, 0, &divisor);
    if (!in->unsigned_arith && known_divisor && divisor > 1 &&
        divisor <= INT32_MAX &&
        (((uint64_t)divisor & ((uint64_t)divisor - 1)) == 0))
      return in->opcode == NYR_MACH_DIV
                 ? UINT64_C(1) << 7 /* R11 truncation-bias scratch */
                 : (UINT64_C(1) << 10) | (UINT64_C(1) << 11);
    /*
     * A known signed divisor other than 0/+-1/powers of two takes the magic
     * reciprocal sequence, which needs only RAX, RDX and the reserved R10
     * scratch: RDX is the one allocatable color it clobbers. MOD by 2^31-1
     * has its own sequence that also uses R8/R9.
     */
    if (!in->unsigned_arith && known_divisor && divisor != 0 && divisor != 1 &&
        divisor != -1 &&
        !(in->opcode == NYR_MACH_MOD && divisor == INT64_C(2147483647))) {
      uint64_t ad = divisor > 0 ? (uint64_t)divisor
                                : (uint64_t)(-(divisor + 1)) + 1;
      if ((ad & (ad - 1)) != 0)
        return UINT64_C(1) << 11; /* RDX */
    }
    return UINT64_C(0xf) << 8; /* General magic division uses R8/R9/RCX/RDX. */
  }
  if ((in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_SAR ||
       in->opcode == NYR_MACH_SHR || in->opcode == NYR_MACH_ROR ||
       in->opcode == NYR_MACH_ROR32) &&
      in->src1.kind != NYR_MACH_OPERAND_IMM)
    return UINT64_C(1) << 10; /* x64 RCX, used for variable shift counts. */
  return 0;
}

uint64_t nyr_mach_inst_fixed_clobbers(const nyr_mach_func_t *mach,
                                      const nyr_mach_inst_t *in,
                                      nyr_mach_reg_class_t reg_class) {
  return inst_fixed_clobbers(mach, in, reg_class, false);
}

bool nyr_mach_regalloc_fixed_clobbers(const nyr_mach_func_t *mach, size_t pc,
                                      nyr_mach_reg_class_t reg_class,
                                      bool preserve_gpr_calls, uint64_t *out) {
  if (!mach || !out || pc >= mach->inst_len ||
      !nyr_mach_verify(mach, 0, NULL, 0))
    return false;
  *out = inst_fixed_clobbers(mach, &mach->insts[pc], reg_class,
                             preserve_gpr_calls);
  return true;
}

/*
 * next_clobber[pc] is the first fixed-register barrier at or after pc, or
 * SIZE_MAX. Building this once avoids rescanning every long live range for
 * barriers that are rare in large straight-line blocks.
 */
static size_t *build_next_clobber(const nyr_mach_func_t *mach,
                                  nyr_mach_reg_class_t reg_class,
                                  bool preserve_gpr_calls, size_t color_count,
                                  size_vec_t **clobber_sites_out,
                                  size_t *clobber_color_count_out) {
  if (clobber_sites_out)
    *clobber_sites_out = NULL;
  if (clobber_color_count_out)
    *clobber_color_count_out = 0;
  if (!mach || mach->inst_len == SIZE_MAX ||
      mach->inst_len + 1 > SIZE_MAX / sizeof(size_t))
    return NULL;
  size_t *next = malloc((mach->inst_len + 1) * sizeof(*next));
  if (!next)
    return NULL;
  size_t color_count_indexed = color_count < 64 ? color_count : 64;
  size_vec_t *clobber_sites = calloc(
      color_count_indexed ? color_count_indexed : 1, sizeof(*clobber_sites));
  if (!clobber_sites) {
    free(next);
    return NULL;
  }
  next[mach->inst_len] = SIZE_MAX;
  uint64_t alloc_colors =
      color_count >= 64 ? UINT64_MAX
                        : (color_count ? (UINT64_C(1) << color_count) - 1 : 0);
  for (size_t pc = mach->inst_len; pc-- > 0;) {
    uint64_t clobbers = inst_fixed_clobbers(mach, &mach->insts[pc], reg_class,
                                            preserve_gpr_calls);
    for (size_t c = 0; c < color_count_indexed; ++c)
      if ((clobbers & (UINT64_C(1) << c)) &&
          !size_vec_push(&clobber_sites[c], pc)) {
        for (size_t i = 0; i < color_count_indexed; ++i)
          free(clobber_sites[i].data);
        free(clobber_sites);
        free(next);
        return NULL;
      }
    next[pc] = alloc_colors && (clobbers & alloc_colors) == alloc_colors
                   ? pc
                   : next[pc + 1];
  }
  for (size_t c = 0; c < color_count_indexed; ++c) {
    size_t lo = 0, hi = clobber_sites[c].len;
    while (lo < hi) {
      --hi;
      if (lo >= hi)
        break;
      size_t tmp = clobber_sites[c].data[lo];
      clobber_sites[c].data[lo] = clobber_sites[c].data[hi];
      clobber_sites[c].data[hi] = tmp;
      ++lo;
    }
  }
  if (clobber_sites_out)
    *clobber_sites_out = clobber_sites;
  else {
    for (size_t c = 0; c < color_count_indexed; ++c)
      free(clobber_sites[c].data);
    free(clobber_sites);
  }
  if (clobber_color_count_out)
    *clobber_color_count_out = color_count_indexed;
  return next;
}

static void free_clobber_sites(size_vec_t *clobber_sites, size_t color_count) {
  if (!clobber_sites)
    return;
  for (size_t c = 0; c < color_count; ++c)
    free(clobber_sites[c].data);
  free(clobber_sites);
}

static bool append_segment(nyr_mach_regalloc_t *a, uint32_t vreg,
                           uint32_t block, size_t start, size_t end,
                           bool reload, bool spill) {
  if (start > end)
    return true;
  if (a->segment_len == a->segment_cap) {
    size_t new_cap = a->segment_cap ? a->segment_cap * 2 : 64;
    nyr_mach_live_segment_t *p =
        realloc(a->segments, new_cap * sizeof(*a->segments));
    if (!p)
      return false;
    a->segments = p;
    a->segment_cap = new_cap;
  }
  size_t segment_index = a->segment_len;
  a->segments[segment_index] = (nyr_mach_live_segment_t){
      .seg = segment_index,
      .vreg = vreg,
      .block = block,
      .start = start,
      .end = end,
      .color = -1,
      .reload = reload,
      .spill = spill,
      .spill_reason =
          spill ? NYR_MACH_SPILL_FIXED_CLOBBER : NYR_MACH_SPILL_NONE,
  };
  a->segment_len = segment_index + 1;
  return true;
}

/*
 * Fixed-register availability is represented per segment by its clobber
 * mask. Do not constrain every value in a call-containing block to callee-
 * saved colors: most values are not live across the call and may safely use
 * the full register set.
 */
static size_t segment_color_limit(const nyr_mach_func_t *mach,
                                  const nyr_mach_live_segment_t *segment,
                                  size_t color_count) {
  (void)mach;
  (void)segment;
  return color_count;
}

static bool segment_accepts_color(const nyr_mach_func_t *mach,
                                  const nyr_mach_live_segment_t *segment,
                                  size_t color_count, int color) {
  return color >= 0 &&
         (size_t)color < segment_color_limit(mach, segment, color_count);
}

typedef struct {
  uint32_t block;
  uint32_t vreg;
  size_t start;
  size_t end;
  int color;
  bool carried;
  bool reload;
} regalloc_verify_span_t;

typedef struct {
  uint32_t vreg;
  size_t start;
  size_t end;
  bool occupied;
} regalloc_verify_color_t;

static int cmp_verify_span(const void *pa, const void *pb) {
  const regalloc_verify_span_t *a = pa;
  const regalloc_verify_span_t *b = pb;
  if (a->block != b->block)
    return a->block < b->block ? -1 : 1;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  if (a->end != b->end)
    return a->end < b->end ? -1 : 1;
  return a->vreg < b->vreg ? -1 : a->vreg != b->vreg;
}

static int cmp_verify_span_vreg(const void *pa, const void *pb) {
  const regalloc_verify_span_t *a = pa;
  const regalloc_verify_span_t *b = pb;
  if (a->vreg != b->vreg)
    return a->vreg < b->vreg ? -1 : 1;
  if (a->block != b->block)
    return a->block < b->block ? -1 : 1;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  return a->end < b->end ? -1 : a->end != b->end;
}

static bool verify_use_has_segment(const regalloc_verify_span_t *spans,
                                   size_t span_count, uint32_t vreg,
                                   uint32_t block, size_t pc) {
  size_t lo = 0, hi = span_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const regalloc_verify_span_t *span = &spans[mid];
    bool before =
        span->vreg < vreg || (span->vreg == vreg && span->block < block) ||
        (span->vreg == vreg && span->block == block && span->start <= pc);
    if (before)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (!lo)
    return false;
  const regalloc_verify_span_t *span = &spans[lo - 1];
  return span->vreg == vreg && span->block == block && span->start <= pc &&
         pc <= span->end;
}

static const regalloc_verify_span_t *
verify_segment_at(const regalloc_verify_span_t *spans, size_t span_count,
                  uint32_t vreg, uint32_t block, size_t pc) {
  size_t lo = 0, hi = span_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const regalloc_verify_span_t *span = &spans[mid];
    bool before =
        span->vreg < vreg || (span->vreg == vreg && span->block < block) ||
        (span->vreg == vreg && span->block == block && span->start <= pc);
    if (before)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (!lo)
    return NULL;
  const regalloc_verify_span_t *span = &spans[lo - 1];
  return span->vreg == vreg && span->block == block && span->start <= pc &&
                 pc <= span->end
             ? span
             : NULL;
}

static bool verify_build_predecessors(const nyr_mach_func_t *mach,
                                      size_t **offsets_out,
                                      uint32_t **preds_out) {
  if (!mach || !offsets_out || !preds_out || mach->block_len == SIZE_MAX ||
      mach->block_len > UINT32_MAX ||
      mach->inst_len > SIZE_MAX - mach->block_len ||
      mach->inst_len + mach->block_len > SIZE_MAX / sizeof(uint32_t))
    return false;
  size_t *offsets = calloc(mach->block_len + 1, sizeof(*offsets));
  size_t *counts =
      calloc(mach->block_len ? mach->block_len : 1, sizeof(*counts));
  size_t *cursor = NULL;
  uint32_t *preds = NULL;
  if (!offsets || !counts)
    goto fail;
  size_t edge_count = 0;
  for (size_t from = 0; from < mach->block_len; ++from) {
    const nyr_mach_block_t *block = &mach->blocks[from];
    if ((size_t)block->first_inst > mach->inst_len ||
        block->inst_count > mach->inst_len - (size_t)block->first_inst)
      goto fail;
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[block->first_inst + n];
      if (in->opcode != NYR_MACH_BR && in->opcode != NYR_MACH_BR_IF)
        continue;
      if (in->src1.kind != NYR_MACH_OPERAND_BLOCK ||
          in->src1.as.block_index >= mach->block_len)
        goto fail;
      ++counts[in->src1.as.block_index];
      ++edge_count;
    }
    bool falls_through = true;
    if (block->inst_count) {
      const nyr_mach_inst_t *last =
          &mach->insts[block->first_inst + block->inst_count - 1];
      falls_through =
          last->opcode != NYR_MACH_BR && last->opcode != NYR_MACH_RET;
    }
    if (falls_through && from + 1 < mach->block_len) {
      ++counts[from + 1];
      ++edge_count;
    }
  }
  if (edge_count > mach->inst_len + mach->block_len)
    goto fail;
  size_t total = 0;
  for (size_t block = 0; block < mach->block_len; ++block) {
    offsets[block] = total;
    if (counts[block] > SIZE_MAX - total)
      goto fail;
    total += counts[block];
  }
  offsets[mach->block_len] = total;
  preds = total ? malloc(total * sizeof(*preds)) : NULL;
  cursor = mach->block_len ? malloc(mach->block_len * sizeof(*cursor)) : NULL;
  if ((total && !preds) || (mach->block_len && !cursor))
    goto fail;
  if (mach->block_len)
    memcpy(cursor, offsets, mach->block_len * sizeof(*cursor));
  for (size_t from = 0; from < mach->block_len; ++from) {
    const nyr_mach_block_t *block = &mach->blocks[from];
    for (size_t n = 0; n < block->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[block->first_inst + n];
      if (in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF)
        preds[cursor[in->src1.as.block_index]++] = (uint32_t)from;
    }
    bool falls_through = true;
    if (block->inst_count) {
      const nyr_mach_inst_t *last =
          &mach->insts[block->first_inst + block->inst_count - 1];
      falls_through =
          last->opcode != NYR_MACH_BR && last->opcode != NYR_MACH_RET;
    }
    if (falls_through && from + 1 < mach->block_len)
      preds[cursor[from + 1]++] = (uint32_t)from;
  }
  free(counts);
  free(cursor);
  *offsets_out = offsets;
  *preds_out = preds;
  return true;

fail:
  free(offsets);
  free(counts);
  free(cursor);
  free(preds);
  return false;
}

static bool rhs_tie_supported(nyr_mach_reg_class_t reg_class,
                              nyr_mach_opcode_t opcode) {
  if (reg_class == NYR_MACH_REGCLASS_GPR)
    return opcode == NYR_MACH_ADD || opcode == NYR_MACH_MUL ||
           opcode == NYR_MACH_AND || opcode == NYR_MACH_OR ||
           opcode == NYR_MACH_XOR;
  if (reg_class == NYR_MACH_REGCLASS_FPR)
    return opcode == NYR_MACH_ADD || opcode == NYR_MACH_MUL;
  return false;
}

static bool verify_tied_input(const nyr_mach_inst_t *in, uint32_t value,
                              nyr_mach_reg_class_t reg_class,
                              bool allow_rhs_tie) {
  if (!in || in->dst.kind != NYR_MACH_OPERAND_VREG)
    return false;
  switch (in->opcode) {
  case NYR_MACH_COPY:
  case NYR_MACH_CONVERT:
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_MUL:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
  case NYR_MACH_SHL:
  case NYR_MACH_SAR:
  case NYR_MACH_SHR:
  case NYR_MACH_ROR:
  case NYR_MACH_ROR32:
  case NYR_MACH_VSHR:
  case NYR_MACH_BSWAP64:
  case NYR_MACH_BSWAP32:
  case NYR_MACH_POPCOUNT64:
  case NYR_MACH_FMA:
  case NYR_MACH_SQRT:
  case NYR_MACH_SHUFFLE:
  case NYR_MACH_SELECT:
  case NYR_MACH_REDUCE_ADD:
    break;
  default:
    return false;
  }
  /*
   * The generic allocator permits only src0 reuse. The x64 entry point may
   * additionally permit selected commutative RHS ties; those encoders have
   * explicit preservation logic for a dying RHS. SELECT's value arms are
   * selected before the result is written.
   */
  if (in->opcode == NYR_MACH_SELECT)
    return (in->src1.kind == NYR_MACH_OPERAND_VREG &&
            in->src1.as.reg == value) ||
           (in->src2.kind == NYR_MACH_OPERAND_VREG && in->src2.as.reg == value);
  return (in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg == value) ||
         (allow_rhs_tie && rhs_tie_supported(reg_class, in->opcode) &&
          in->src1.kind == NYR_MACH_OPERAND_VREG && in->src1.as.reg == value);
}

static bool regalloc_affinity_input(const nyr_mach_inst_t *in, uint32_t value,
                                    nyr_mach_reg_class_t reg_class,
                                    bool allow_rhs_tie) {
  if (!verify_tied_input(in, value, reg_class, allow_rhs_tie))
    return false;
  /*
   * SELECT's predicate chooses a value; it is not a result-value affinity.
   */
  if (in->opcode == NYR_MACH_SELECT)
    return (in->src1.kind == NYR_MACH_OPERAND_VREG &&
            in->src1.as.reg == value) ||
           (in->src2.kind == NYR_MACH_OPERAND_VREG && in->src2.as.reg == value);
  return true;
}

static bool verify_tied_boundary(const nyr_mach_func_t *mach,
                                 const regalloc_verify_color_t *active,
                                 const regalloc_verify_span_t *span,
                                 nyr_mach_reg_class_t reg_class,
                                 bool allow_rhs_tie) {
  if (!active || !span || active->end != span->start ||
      span->start >= mach->inst_len)
    return false;
  const nyr_mach_inst_t *in = &mach->insts[span->start];
  if (!dst_is_def(in) || in->dst.kind != NYR_MACH_OPERAND_VREG)
    return false;
  uint32_t dst = in->dst.as.reg;
  uint32_t source = dst == span->vreg ? active->vreg : span->vreg;
  if ((dst != span->vreg && dst != active->vreg) ||
      (dst == active->vreg && active->start != span->start) ||
      !verify_tied_input(in, source, reg_class, allow_rhs_tie))
    return false;
  if (mach->vreg_classes)
    return mach->vreg_classes[dst] == mach->vreg_classes[source];
  for (nyr_mach_reg_class_t cls = NYR_MACH_REGCLASS_GPR;
       cls <= NYR_MACH_REGCLASS_VECTOR; cls++)
    if (is_alloc_type(mach->vreg_types[dst], cls) &&
        is_alloc_type(mach->vreg_types[source], cls))
      return true;
  return false;
}

/*
 * Latch recurrence sharing (vector class): a loop PHI `phi` and the latch
 * value `src` that its edge copy `phi = copy src` installs may share one
 * register although the PHI's single per-block segment spans the whole
 * latch. The overlap is only nominal when
 *   - `src` is defined at its segment start by an instruction that reads
 *     `phi` at most as the src0 of a single-instruction lowering
 *     (vector_latch_def_reads_phi_ok; multi-instruction vector lowerings may
 *     write the destination before their last operand read),
 *   - `src` dies exactly at the edge copy `phi = copy src`, and
 *   - between that definition and the copy, nothing reads or redefines
 *     `phi`.
 * The register then holds the old PHI value until its last read, the new
 * value from the definition on, and the copy is a no-op. Mirrors
 * vector_phi_reads_precede_source(), which proposes the coloring.
 */
static bool vector_latch_def_reads_phi_ok(const nyr_mach_func_t *mach,
                                          const nyr_mach_inst_t *def,
                                          uint32_t phi);

static bool verify_vector_latch_recurrence(
    const nyr_mach_func_t *mach, const regalloc_verify_color_t *active,
    const regalloc_verify_span_t *span, nyr_mach_reg_class_t reg_class) {
  if (!mach || !active || !span || reg_class != NYR_MACH_REGCLASS_VECTOR ||
      active->start > span->start || span->end > active->end ||
      span->end >= mach->inst_len || span->start >= span->end)
    return false;
  const uint32_t phi = active->vreg, src = span->vreg;
  if (phi == src)
    return false;
  const nyr_mach_inst_t *def = &mach->insts[span->start];
  if (!dst_is_def(def) || def->dst.kind != NYR_MACH_OPERAND_VREG ||
      def->dst.as.reg != src || !vector_latch_def_reads_phi_ok(mach, def, phi))
    return false;
  const nyr_mach_inst_t *copy = &mach->insts[span->end];
  if (copy->opcode != NYR_MACH_COPY ||
      copy->dst.kind != NYR_MACH_OPERAND_VREG || copy->dst.as.reg != phi ||
      copy->src0.kind != NYR_MACH_OPERAND_VREG || copy->src0.as.reg != src)
    return false;
  for (size_t pc = span->start + 1; pc < span->end; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (dst_is_def(in) && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        (in->dst.as.reg == phi || in->dst.as.reg == src))
      return false;
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t use_count = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < use_count; ++i)
      if (uses[i] == phi)
        return false;
  }
  return true;
}

/*
 * Check the final split ranges after coalescing, before encoders consume them.
 *
 * Post-allocation fixed-register check: a colored segment may not stay live
 * across a non-call instruction whose target constraint clobbers that color
 * (variable shift/rotate count in RCX, division scratch, reduction
 * accumulator). Calls are checked by their own split/preservation contract.
 * Returns the offending pc, or SIZE_MAX when the segment is clean.
 */
static size_t verify_fixed_clobber_pc(const nyr_mach_func_t *mach,
                                      const nyr_mach_live_segment_t *seg,
                                      nyr_mach_reg_class_t reg_class) {
  if (seg->color < 0 || seg->color >= 64)
    return SIZE_MAX;
  uint64_t mask = UINT64_C(1) << seg->color;
  for (size_t pc = seg->start; pc <= seg->end && pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (in->opcode == NYR_MACH_CALL)
      continue;
    if (!(inst_fixed_clobbers(mach, in, reg_class, false) & mask))
      continue;
    bool live_after =
        pc < seg->end || seg->spill_reason == NYR_MACH_SPILL_LIVE_OUT;
    if (!live_after)
      continue;
    if (dst_is_def(in) && in->dst.as.reg == seg->vreg)
      continue;
    return pc;
  }
  return SIZE_MAX;
}

static bool regalloc_verify_mode(const nyr_mach_func_t *mach,
                                 const nyr_mach_regalloc_t *alloc,
                                 nyr_mach_reg_class_t reg_class,
                                 bool allow_rhs_tie) {
  const bool debug = getenv("NY_REGALLOC_VERIFY_DEBUG") != NULL;
  if (!mach || !alloc)
    return false;
  if ((mach->block_len && !mach->blocks) || (mach->inst_len && !mach->insts) ||
      (mach->vreg_len && !mach->vreg_types))
    return false;
  if ((alloc->segment_len && !alloc->segments) ||
      alloc->segment_len > SIZE_MAX / sizeof(regalloc_verify_span_t))
    return false;
  regalloc_verify_span_t *spans =
      alloc->segment_len ? malloc(alloc->segment_len * sizeof(*spans)) : NULL;
  if (alloc->segment_len && !spans)
    return false;
  for (size_t i = 0; i < alloc->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &alloc->segments[i];
    if (seg->vreg >= mach->vreg_len || seg->block >= mach->block_len ||
        seg->start > seg->end || seg->color < -1 ||
        (seg->carried && seg->color < 0) || !mach->vreg_types ||
        !is_alloc_type(mach->vreg_types[seg->vreg], reg_class)) {
      if (debug)
        fprintf(stderr, "[regalloc-verify] malformed segment %zu\n", i);
      free(spans);
      return false;
    }
    const nyr_mach_block_t *block = &mach->blocks[seg->block];
    if (block->inst_count) {
      if (block->inst_count > SIZE_MAX - (size_t)block->first_inst) {
        if (debug)
          fprintf(stderr, "[regalloc-verify] overflowing block %u\n",
                  seg->block);
        free(spans);
        return false;
      }
      size_t block_end = (size_t)block->first_inst + block->inst_count;
      if (seg->start < block->first_inst || seg->end >= block_end) {
        if (debug)
          fprintf(stderr, "[regalloc-verify] segment %zu outside block %u\n", i,
                  seg->block);
        free(spans);
        return false;
      }
    } else if (seg->start != block->first_inst ||
               seg->end != block->first_inst) {
      if (debug)
        fprintf(stderr,
                "[regalloc-verify] segment %zu outside empty block %u\n", i,
                seg->block);
      free(spans);
      return false;
    }
    if (seg->color >= 0 &&
        !segment_accepts_color(mach, seg, alloc->color_count, seg->color)) {
      if (debug)
        fprintf(stderr,
                "[regalloc-verify] illegal color segment %zu v%u class=%d "
                "color=%d colors=%zu\n",
                i, seg->vreg, reg_class, seg->color, alloc->color_count);
      free(spans);
      return false;
    }
    size_t clobber_pc = verify_fixed_clobber_pc(mach, seg, reg_class);
    if (clobber_pc != SIZE_MAX) {
      if (debug)
        fprintf(stderr,
                "[regalloc-verify] segment %zu v%u color=%d span=%zu-%zu "
                "reload=%d spill=%d carried=%d acyclic=%d reason=%d live "
                "across fixed clobber %s at pc=%zu\n",
                i, seg->vreg, seg->color, seg->start, seg->end, seg->reload,
                seg->spill, seg->carried, seg->acyclic_carried,
                (int)seg->spill_reason,
                nyr_mach_opcode_name(mach->insts[clobber_pc].opcode),
                clobber_pc);
      free(spans);
      return false;
    }
    spans[i] = (regalloc_verify_span_t){.block = seg->block,
                                        .vreg = seg->vreg,
                                        .start = seg->start,
                                        .end = seg->end,
                                        .color = seg->color,
                                        .carried = seg->carried,
                                        .reload = seg->reload};
  }
  if (alloc->segment_len > 1)
    qsort(spans, alloc->segment_len, sizeof(*spans), cmp_verify_span);
  regalloc_verify_color_t *active =
      alloc->color_count ? calloc(alloc->color_count, sizeof(*active)) : NULL;
  if (alloc->color_count && !active) {
    free(spans);
    return false;
  }
  uint32_t active_block = UINT32_MAX;
  bool valid = true;
  for (size_t i = 0; i < alloc->segment_len && valid; ++i) {
    const regalloc_verify_span_t *span = &spans[i];
    if (span->block != active_block) {
      if (active)
        memset(active, 0, alloc->color_count * sizeof(*active));
      active_block = span->block;
    }
    if (span->color < 0)
      continue;
    if (!active) {
      /*
       * A colored segment with no color table means the allocation metadata
       * is inconsistent; report it as an invalid allocation instead of
       * indexing a NULL table.
       */
      valid = false;
      break;
    }
    size_t color = (size_t)span->color;
    if (active[color].occupied && active[color].end >= span->start) {
      if (verify_vector_latch_recurrence(mach, &active[color], span,
                                         reg_class)) {
        /*
         * The PHI segment outlives the latch value; keep it active so later
         * segments in this color are still checked against it.
         */
        continue;
      }
      if (!verify_tied_boundary(mach, &active[color], span, reg_class,
                                allow_rhs_tie)) {
        if (debug)
          fprintf(stderr,
                  "[regalloc-verify] overlapping color %zu block=%u "
                  "v%u[%zu,%zu] v%u[%zu,%zu]\n",
                  color, span->block, active[color].vreg, active[color].start,
                  active[color].end, span->vreg, span->start, span->end);
        if (debug && span->start < mach->inst_len) {
          const nyr_mach_inst_t *qi = &mach->insts[span->start];
          fprintf(
              stderr, "  at pc=%zu %s dst=%d s0=%d s1=%d s2=%d (rhs-tie=%d)\n",
              span->start, nyr_mach_opcode_name(qi->opcode),
              qi->dst.kind == NYR_MACH_OPERAND_VREG ? (int)qi->dst.as.reg : -1,
              qi->src0.kind == NYR_MACH_OPERAND_VREG ? (int)qi->src0.as.reg
                                                     : -1,
              qi->src1.kind == NYR_MACH_OPERAND_VREG ? (int)qi->src1.as.reg
                                                     : -1,
              qi->src2.kind == NYR_MACH_OPERAND_VREG ? (int)qi->src2.as.reg
                                                     : -1,
              (int)allow_rhs_tie);
        }
        valid = false;
        break;
      }
    }
    active[color] = (regalloc_verify_color_t){.vreg = span->vreg,
                                              .start = span->start,
                                              .end = span->end,
                                              .occupied = true};
  }
  free(active);
  /*
   * Every allocated operand read must have exactly one home/register segment
   * covering its instruction. Bounds and non-overlap alone can accept a
   * malformed split that silently leaves a use between two ranges.
   */
  if (valid && alloc->segment_len > 1)
    qsort(spans, alloc->segment_len, sizeof(*spans), cmp_verify_span_vreg);
  if (valid) {
    for (size_t i = 0; i < alloc->segment_len;) {
      size_t end = i + 1;
      while (end < alloc->segment_len && spans[end].vreg == spans[i].vreg &&
             spans[end].block == spans[i].block)
        ++end;
      const nyr_mach_block_t *block = &mach->blocks[spans[i].block];
      size_t block_end = (size_t)block->first_inst + block->inst_count;
      if (alloc->block_count == mach->block_len && alloc->live_in &&
          nyr_mach_regalloc_live_in(alloc, spans[i].block, spans[i].vreg) &&
          spans[i].start != block->first_inst) {
        if (debug)
          fprintf(stderr,
                  "[regalloc-verify] live-in v%u starts after block entry\n",
                  spans[i].vreg);
        valid = false;
      }
      if (alloc->block_count == mach->block_len && alloc->live_out &&
          nyr_mach_regalloc_live_out(alloc, spans[i].block, spans[i].vreg) &&
          block->inst_count && spans[end - 1].end != block_end - 1) {
        if (debug)
          fprintf(stderr,
                  "[regalloc-verify] live-out v%u ends before block exit\n",
                  spans[i].vreg);
        valid = false;
      }
      for (size_t j = i + 1; valid && j < end; ++j) {
        if (spans[j - 1].end >= spans[j].start) {
          if (debug)
            fprintf(
                stderr,
                "[regalloc-verify] overlapping ranges for v%u in block %u\n",
                spans[j].vreg, spans[j].block);
          valid = false;
        }
      }
      i = end;
    }
  }
  /*
   * A carried register is a proof obligation: at a live-in boundary, every
   * predecessor that supplies this value must end with the same color. A
   * carried edge-copy destination must be defined by a tied copy whose source
   * is allocated at that point; the copy itself may move between colors.
   */
  size_t *pred_offsets = NULL;
  uint32_t *preds = NULL;
  bool has_carried = false;
  for (size_t i = 0; i < alloc->segment_len; ++i)
    has_carried |= spans[i].carried;
  if (valid && has_carried &&
      !verify_build_predecessors(mach, &pred_offsets, &preds)) {
    if (debug)
      fprintf(stderr, "[regalloc-verify] could not build CFG edge proof\n");
    valid = false;
  }
  if (valid) {
    for (size_t i = 0; valid && i < alloc->segment_len; ++i) {
      const regalloc_verify_span_t *span = &spans[i];
      if (!span->carried)
        continue;
      if (span->color < 0 || span->reload ||
          alloc->block_count != mach->block_len || !alloc->live_in ||
          !alloc->live_out) {
        if (debug)
          fprintf(stderr,
                  "[regalloc-verify] carried v%u lacks a resident boundary\n",
                  span->vreg);
        valid = false;
        break;
      }
      if (nyr_mach_regalloc_live_in(alloc, span->block, span->vreg)) {
        const nyr_mach_block_t *target = &mach->blocks[span->block];
        bool incoming = false;
        for (size_t edge = pred_offsets[span->block];
             edge < pred_offsets[span->block + 1]; ++edge) {
          size_t pred = preds[edge];
          if (!nyr_mach_regalloc_live_out(alloc, pred, span->vreg))
            continue;
          const nyr_mach_block_t *source = &mach->blocks[pred];
          if (!source->inst_count) {
            valid = false;
            break;
          }
          size_t source_pc =
              (size_t)source->first_inst + source->inst_count - 1;
          size_t target_pc = target->first_inst;
          const regalloc_verify_span_t *from = verify_segment_at(
              spans, alloc->segment_len, span->vreg, (uint32_t)pred, source_pc);
          const regalloc_verify_span_t *to = verify_segment_at(
              spans, alloc->segment_len, span->vreg, span->block, target_pc);
          if (!from || !to || from->color != span->color ||
              to->color != span->color) {
            if (debug)
              fprintf(
                  stderr,
                  "[regalloc-verify] carried v%u color mismatch on B%zu->B%u\n",
                  span->vreg, pred, span->block);
            valid = false;
            break;
          }
          incoming = true;
        }
        if (valid && !incoming) {
          if (debug)
            fprintf(
                stderr,
                "[regalloc-verify] carried live-in v%u has no supplying edge\n",
                span->vreg);
          valid = false;
        }
      } else {
        if (span->start >= mach->inst_len) {
          valid = false;
          break;
        }
        const nyr_mach_inst_t *in = &mach->insts[span->start];
        uint32_t source_vreg = in->src0.kind == NYR_MACH_OPERAND_VREG
                                   ? in->src0.as.reg
                                   : UINT32_MAX;
        if (in->opcode != NYR_MACH_COPY || !dst_is_def(in) ||
            in->dst.kind != NYR_MACH_OPERAND_VREG ||
            in->dst.as.reg != span->vreg ||
            !verify_tied_input(in, source_vreg, reg_class, allow_rhs_tie)) {
          if (debug)
            fprintf(stderr,
                    "[regalloc-verify] carried v%u has no tied edge copy\n",
                    span->vreg);
          valid = false;
          break;
        }
        const regalloc_verify_span_t *from = verify_segment_at(
            spans, alloc->segment_len, source_vreg, span->block, span->start);
        if (!from) {
          if (debug)
            fprintf(
                stderr,
                "[regalloc-verify] carried v%u copy source has no segment\n",
                span->vreg);
          valid = false;
        }
      }
    }
  }
  free(pred_offsets);
  free(preds);
  for (size_t bi = 0; valid && bi < mach->block_len; ++bi) {
    const nyr_mach_block_t *block = &mach->blocks[bi];
    if ((size_t)block->first_inst > mach->inst_len ||
        block->inst_count > mach->inst_len - (size_t)block->first_inst) {
      if (debug)
        fprintf(stderr,
                "[regalloc-verify] block %zu exceeds instruction list\n", bi);
      valid = false;
      break;
    }
    size_t end = (size_t)block->first_inst + block->inst_count;
    for (size_t pc = block->first_inst; valid && pc < end; ++pc) {
      uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
      size_t use_count =
          inst_uses(&mach->insts[pc], uses, sizeof(uses) / sizeof(*uses));
      for (size_t ui = 0; ui < use_count; ++ui) {
        uint32_t vreg = uses[ui];
        if (vreg >= mach->vreg_len || !mach->vreg_types) {
          valid = false;
          break;
        }
        if (is_alloc_type(mach->vreg_types[vreg], reg_class) &&
            !verify_use_has_segment(spans, alloc->segment_len, vreg,
                                    (uint32_t)bi, pc)) {
          if (debug)
            fprintf(
                stderr,
                "[regalloc-verify] use of v%u at pc=%zu has no %s segment\n",
                vreg, pc, reg_class_name(reg_class));
          valid = false;
          break;
        }
      }
    }
  }
  free(spans);
  return valid;
}

bool nyr_mach_regalloc_verify(const nyr_mach_func_t *mach,
                              const nyr_mach_regalloc_t *alloc,
                              nyr_mach_reg_class_t reg_class) {
  return regalloc_verify_mode(mach, alloc, reg_class,
                              alloc && alloc->x64_rhs_tie);
}

/*
 * Return true when this live range is an operand of a call.  Such a value is
 * consumed at the ABI boundary; placing it in the callee-saved bank lets the
 * encoder move it directly into its argument register instead of staging it
 * through a canonical home and reloading it immediately.
 */
static bool segment_uses_call_arg(const nyr_mach_func_t *mach,
                                  const nyr_mach_live_segment_t *seg) {
  if (!mach || !seg || seg->start > seg->end || seg->start >= mach->inst_len)
    return false;
  size_t end = seg->end < mach->inst_len ? seg->end : mach->inst_len - 1;
  for (size_t pc = seg->start; pc <= end; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (in->opcode != NYR_MACH_CALL)
      continue;
    if (in->src0.kind == NYR_MACH_OPERAND_VREG &&
        in->src0.as.reg == seg->vreg)
      return true;
    for (size_t ai = 0; ai < in->args_len; ++ai)
      if (in->args[ai].kind == NYR_MACH_OPERAND_VREG &&
          in->args[ai].as.reg == seg->vreg)
        return true;
  }
  return false;
}

/*
 * The x64 allocator keeps colors 0..4 in callee-saved registers and later
 * reserves some of them for loop homes. Prefer caller-saved colors for local
 * GPR segments: clobber masks already exclude them across calls, and this
 * leaves the callee-saved bank available for values that must survive calls
 * or for the loop-home allocator. Call arguments are the exception: they are
 * consumed at the call site, so use the callee-saved bank first and let the
 * encoder place them directly. Without this ordering, ordinary local segments
 * take r12-r15/rbx first and the loop-home pass has to evict them.
 */
static size_t segment_color_at_rank(size_t rank, size_t color_limit,
                                    nyr_mach_reg_class_t reg_class,
                                    bool preserve_gpr_calls,
                                    bool prefer_callee) {
  if (!prefer_callee && preserve_gpr_calls &&
      reg_class == NYR_MACH_REGCLASS_GPR && color_limit > 5) {
    size_t caller_count = color_limit - 5;
    return rank < caller_count ? 5 + rank : rank - caller_count;
  }
  return rank;
}

/*
 * A spilled phi destination in an edge-copy block has no preg to preserve:
 * the coalescer may assign it the incoming value's preg once block conflicts
 * and predecessor materialization have both been proven. Fixed-clobber
 * segments remain pinned to their canonical home.
 */
static bool loop_edge_dest_accepts_color(const nyr_mach_func_t *mach,
                                         const nyr_mach_live_segment_t *segment,
                                         size_t color_count, int color) {
  if (!mach || !segment || color < 0 ||
      (size_t)color >= segment_color_limit(mach, segment, color_count))
    return false;
  return segment->color >= 0 ||
         (segment->spill &&
          segment->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER);
}

static int cmp_seg_start(const void *pa, const void *pb) {
  const nyr_mach_live_segment_t *a = pa, *b = pb;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  if (a->end != b->end)
    return a->end < b->end ? -1 : 1;
  return a->vreg < b->vreg ? -1 : a->vreg > b->vreg;
}

typedef struct {
  uint32_t dst;
  uint32_t src;
  size_t pc;
} regalloc_affinity_ref_t;

typedef struct {
  uint32_t vreg;
  size_t start;
  size_t end;
  size_t segment;
} regalloc_segment_ref_t;

typedef struct {
  size_t *offsets;
  regalloc_affinity_ref_t *refs;
  size_t ref_count;
  regalloc_segment_ref_t *segments;
  size_t segment_count;
} regalloc_affinity_index_t;

static int cmp_affinity_ref(const void *pa, const void *pb) {
  const regalloc_affinity_ref_t *a = pa, *b = pb;
  if (a->dst != b->dst)
    return a->dst < b->dst ? -1 : 1;
  if (a->pc != b->pc)
    return a->pc < b->pc ? -1 : 1;
  return a->src < b->src ? -1 : a->src > b->src;
}

static int cmp_segment_ref(const void *pa, const void *pb) {
  const regalloc_segment_ref_t *a = pa, *b = pb;
  if (a->vreg != b->vreg)
    return a->vreg < b->vreg ? -1 : 1;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  if (a->end != b->end)
    return a->end < b->end ? -1 : 1;
  return a->segment < b->segment ? -1 : a->segment > b->segment;
}

/*
 * Build sorted indexes for advisory tied-input affinity. The allocator's
 * canonical segments remain ordered by start for linear scan; these
 * references let a hint query visit only instructions targeting this vreg
 * and binary-search the source segment active at that instruction.
 */
static bool affinity_index_build(const nyr_mach_func_t *mach,
                                 const nyr_mach_regalloc_t *a,
                                 nyr_mach_reg_class_t reg_class,
                                 bool allow_rhs_tie,
                                 regalloc_affinity_index_t *out) {
  if (!mach || !a || !out || a->vreg_len == SIZE_MAX ||
      a->vreg_len + 1 > SIZE_MAX / sizeof(size_t) ||
      mach->inst_len > SIZE_MAX / sizeof(regalloc_affinity_ref_t) ||
      a->segment_len > SIZE_MAX / sizeof(regalloc_segment_ref_t))
    return false;
  size_t ref_count = 0;
  for (size_t pc = 0; pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (in->dst.kind != NYR_MACH_OPERAND_VREG || in->dst.as.reg >= a->vreg_len)
      continue;
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t use_count = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < use_count; ++i) {
      if (uses[i] >= a->vreg_len ||
          !regalloc_affinity_input(in, uses[i], reg_class, allow_rhs_tie))
        continue;
      bool duplicate = false;
      for (size_t j = 0; j < i; ++j)
        if (uses[j] == uses[i]) {
          duplicate = true;
          break;
        }
      if (!duplicate) {
        if (ref_count == SIZE_MAX)
          return false;
        ++ref_count;
      }
    }
  }
  if (!ref_count)
    return true;
  if (ref_count > SIZE_MAX / sizeof(regalloc_affinity_ref_t))
    return false;
  regalloc_affinity_ref_t *refs = malloc(ref_count * sizeof(*refs));
  regalloc_segment_ref_t *segments =
      a->segment_len ? malloc(a->segment_len * sizeof(*segments)) : NULL;
  size_t *offsets = calloc(a->vreg_len + 1, sizeof(*offsets));
  if (!refs || (a->segment_len && !segments) || !offsets) {
    free(refs);
    free(segments);
    free(offsets);
    return false;
  }
  size_t ref_at = 0;
  for (size_t pc = 0; pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (in->dst.kind != NYR_MACH_OPERAND_VREG || in->dst.as.reg >= a->vreg_len)
      continue;
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t use_count = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < use_count; ++i) {
      if (uses[i] >= a->vreg_len ||
          !regalloc_affinity_input(in, uses[i], reg_class, allow_rhs_tie))
        continue;
      bool duplicate = false;
      for (size_t j = 0; j < i; ++j)
        if (uses[j] == uses[i]) {
          duplicate = true;
          break;
        }
      if (!duplicate)
        refs[ref_at++] = (regalloc_affinity_ref_t){
            .dst = in->dst.as.reg, .src = uses[i], .pc = pc};
    }
  }
  if (ref_count > 1)
    qsort(refs, ref_count, sizeof(*refs), cmp_affinity_ref);
  for (size_t i = 0; i < ref_count; ++i)
    ++offsets[(size_t)refs[i].dst + 1];
  for (size_t v = 1; v <= a->vreg_len; ++v)
    offsets[v] += offsets[v - 1];
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &a->segments[i];
    segments[i] = (regalloc_segment_ref_t){
        .vreg = seg->vreg, .start = seg->start, .end = seg->end, .segment = i};
  }
  if (a->segment_len > 1)
    qsort(segments, a->segment_len, sizeof(*segments), cmp_segment_ref);
  *out = (regalloc_affinity_index_t){.offsets = offsets,
                                     .refs = refs,
                                     .ref_count = ref_count,
                                     .segments = segments,
                                     .segment_count = a->segment_len};
  return true;
}

static void affinity_index_free(regalloc_affinity_index_t *index) {
  if (!index)
    return;
  free(index->offsets);
  free(index->refs);
  free(index->segments);
  *index = (regalloc_affinity_index_t){0};
}

static const nyr_mach_live_segment_t *
affinity_index_segment_at(const nyr_mach_regalloc_t *a,
                          const regalloc_affinity_index_t *index, uint32_t vreg,
                          size_t pc) {
  size_t lo = 0, hi = index->segment_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const regalloc_segment_ref_t *ref = &index->segments[mid];
    if (ref->vreg < vreg || (ref->vreg == vreg && ref->start <= pc))
      lo = mid + 1;
    else
      hi = mid;
  }
  if (!lo)
    return NULL;
  const regalloc_segment_ref_t *ref = &index->segments[lo - 1];
  return ref->vreg == vreg && ref->start <= pc && pc <= ref->end
             ? &a->segments[ref->segment]
             : NULL;
}

/*
 * Return the segment carrying vreg at an instruction position.  Segments are
 * split at block boundaries and spills, so a copy hint must use the segment
 * active at the copy rather than the first segment for the vreg.
 */
static const nyr_mach_live_segment_t *segment_at(const nyr_mach_regalloc_t *a,
                                                 uint32_t vreg, size_t pc) {
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *s = &a->segments[i];
    if (s->vreg == vreg && s->start <= pc && pc <= s->end)
      return s;
  }
  return NULL;
}

/*
 * Tied inputs provide local affinity only when their live range ends at the
 * defining instruction. Reusing a still-live input's color would overwrite
 * a value the consumer still needs. Sorted indexes keep the query local to
 * instructions targeting this vreg. If index allocation fails, retain the
 * conservative linear fallback.
 */
static int affinity_hint_color(const nyr_mach_func_t *mach,
                               const nyr_mach_regalloc_t *a,
                               const regalloc_affinity_index_t *index,
                               nyr_mach_reg_class_t reg_class,
                               bool allow_rhs_tie, uint32_t dst_vreg,
                               size_t start, size_t end,
                               uint32_t *source_vreg) {
  if (source_vreg)
    *source_vreg = UINT32_MAX;
  if (!mach)
    return -1;
  if (index && index->offsets && dst_vreg < a->vreg_len) {
    size_t lo = index->offsets[dst_vreg];
    size_t hi = index->offsets[dst_vreg + 1];
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (index->refs[mid].pc < start)
        lo = mid + 1;
      else
        hi = mid;
    }
    for (size_t i = lo;
         i < index->offsets[dst_vreg + 1] && index->refs[i].pc <= end; ++i) {
      const regalloc_affinity_ref_t *ref = &index->refs[i];
      if (ref->pc != start)
        continue;
      if (ref->src == dst_vreg)
        continue;
      const nyr_mach_live_segment_t *src =
          affinity_index_segment_at(a, index, ref->src, ref->pc);
      if (!src || src->color < 0 || src->end != ref->pc)
        continue;
      if (source_vreg)
        *source_vreg = ref->src;
      if (src->color >= 0)
        return src->color;
    }
    return -1;
  }
  for (size_t pc = start; pc <= end && pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (in->dst.kind != NYR_MACH_OPERAND_VREG || in->dst.as.reg != dst_vreg)
      continue;
    uint32_t candidates[3 + NYIR_CALL_MAX_ARGS];
    size_t candidate_count =
        inst_uses(in, candidates, sizeof(candidates) / sizeof(*candidates));
    for (size_t i = 0; i < candidate_count; ++i) {
      uint32_t src_vreg = candidates[i];
      if (src_vreg == dst_vreg)
        continue;
      if (!regalloc_affinity_input(in, src_vreg, reg_class, allow_rhs_tie))
        continue;
      const nyr_mach_live_segment_t *src = segment_at(a, src_vreg, pc);
      if (!src || src->color < 0 || src->end != pc)
        continue;
      if (source_vreg)
        *source_vreg = src_vreg;
      return src->color;
    }
  }
  return -1;
}

static int cmp_seg_vreg(const void *pa, const void *pb) {
  const nyr_mach_live_segment_t *a = pa, *b = pb;
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
 * allocation deterministic. Artificial segment boundaries multiply work
 * for long live ranges, so segments should end only at real barriers.
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

static bool regalloc_use_index_build(const nyr_mach_func_t *mach,
                                     size_t vreg_len,
                                     regalloc_use_index_t *out) {
  if (!mach || !out)
    return false;
  if (vreg_len == SIZE_MAX || vreg_len + 1 > SIZE_MAX / sizeof(size_t) ||
      vreg_len > SIZE_MAX / sizeof(size_t))
    return false;
  size_t *counts = calloc(vreg_len ? vreg_len : 1, sizeof(*counts));
  size_t *offsets = calloc(vreg_len + 1, sizeof(*offsets));
  if (!counts || !offsets) {
    free(counts);
    free(offsets);
    return false;
  }
  size_t total = 0;
  for (size_t pc = 0; pc < mach->inst_len; ++pc) {
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t n = inst_uses(&mach->insts[pc], uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < n; ++i) {
      uint32_t v = uses[i];
      if (v >= vreg_len)
        continue;
      bool duplicate = false;
      for (size_t j = 0; j < i; ++j)
        if (uses[j] == v) {
          duplicate = true;
          break;
        }
      if (duplicate)
        continue;
      if (counts[v] == SIZE_MAX || total == SIZE_MAX) {
        free(counts);
        free(offsets);
        return false;
      }
      counts[v]++;
      total++;
    }
  }
  for (size_t v = 0; v < vreg_len; ++v) {
    if (offsets[v] > SIZE_MAX - counts[v]) {
      free(counts);
      free(offsets);
      return false;
    }
    offsets[v + 1] = offsets[v] + counts[v];
  }
  if (total > SIZE_MAX / sizeof(size_t)) {
    free(counts);
    free(offsets);
    return false;
  }
  size_t *positions = total ? malloc(total * sizeof(*positions)) : NULL;
  size_t *cursor = vreg_len ? malloc(vreg_len * sizeof(*cursor)) : NULL;
  if ((total && !positions) || (vreg_len && !cursor)) {
    free(counts);
    free(offsets);
    free(positions);
    free(cursor);
    return false;
  }
  if (vreg_len)
    memcpy(cursor, offsets, vreg_len * sizeof(*cursor));
  for (size_t pc = 0; pc < mach->inst_len; ++pc) {
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t n = inst_uses(&mach->insts[pc], uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < n; ++i) {
      uint32_t v = uses[i];
      bool duplicate = false;
      for (size_t j = 0; j < i; ++j)
        if (uses[j] == v) {
          duplicate = true;
          break;
        }
      if (v < vreg_len && !duplicate)
        positions[cursor[v]++] = pc;
    }
  }
  free(counts);
  free(cursor);
  *out = (regalloc_use_index_t){
      .offsets = offsets, .positions = positions, .vreg_len = vreg_len};
  return true;
}

static void regalloc_use_index_free(regalloc_use_index_t *index) {
  if (!index)
    return;
  free(index->offsets);
  free(index->positions);
  *index = (regalloc_use_index_t){0};
}

static void segment_use_metrics(const regalloc_use_index_t *index,
                                const nyr_mach_live_segment_t *seg, size_t at,
                                size_t *next_distance, size_t *use_count) {
  *next_distance = SIZE_MAX;
  *use_count = 0;
  if (!index || !index->offsets || !seg || seg->vreg >= index->vreg_len ||
      seg->start > seg->end || at > seg->end)
    return;
  size_t begin = at > seg->start ? at : seg->start;
  size_t lo = index->offsets[seg->vreg];
  size_t hi = index->offsets[seg->vreg + 1];
  size_t first = lo;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (index->positions[mid] < begin)
      lo = mid + 1;
    else
      hi = mid;
  }
  first = lo;
  if (first == index->offsets[seg->vreg + 1] ||
      index->positions[first] > seg->end)
    return;
  *next_distance = index->positions[first] - at;
  lo = first;
  hi = index->offsets[seg->vreg + 1];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (index->positions[mid] <= seg->end)
      lo = mid + 1;
    else
      hi = mid;
  }
  *use_count = lo - first;
}

static void segment_use_bounds(const regalloc_use_index_t *index,
                               const nyr_mach_live_segment_t *seg,
                               size_t *first_out, size_t *end_out) {
  *first_out = 0;
  *end_out = 0;
  if (!index || !index->offsets || !seg || seg->vreg >= index->vreg_len ||
      seg->start > seg->end)
    return;
  size_t lo = index->offsets[seg->vreg];
  size_t hi = index->offsets[seg->vreg + 1];
  size_t begin = lo;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (index->positions[mid] < seg->start)
      lo = mid + 1;
    else
      hi = mid;
  }
  begin = lo;
  hi = index->offsets[seg->vreg + 1];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (index->positions[mid] <= seg->end)
      lo = mid + 1;
    else
      hi = mid;
  }
  *first_out = begin;
  *end_out = lo;
}

static uint64_t segment_profile_heat(const nyr_mach_func_t *mach,
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

static eviction_score_t eviction_score(const nyr_mach_func_t *mach,
                                       const regalloc_use_index_t *use_index,
                                       const nyr_mach_live_segment_t *seg,
                                       size_t at, const size_t *loop_depth) {
  eviction_score_t score = {0};
  if (!seg)
    return score;
  score.end = seg->end;
  score.vreg = seg->vreg;
  score.loop_depth =
      loop_depth && seg->block != UINT32_MAX ? loop_depth[seg->block] : 0;
  score.profile_heat = segment_profile_heat(mach, seg->block);
  segment_use_metrics(use_index, seg, at, &score.next_use_distance,
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

static bool segment_blocks_color(const nyr_mach_func_t *mach,
                                 const nyr_mach_live_segment_t *segment,
                                 const size_vec_t *clobber_sites,
                                 size_t *cursor) {
  if (!mach || !segment || !clobber_sites || !clobber_sites->len)
    return false;
  size_t first = cursor ? *cursor : 0;
  while (first < clobber_sites->len &&
         clobber_sites->data[first] < segment->start)
    ++first;
  if (cursor)
    *cursor = first;
  for (size_t i = first; i < clobber_sites->len; ++i) {
    size_t pc = clobber_sites->data[i];
    if (pc > segment->end)
      break;
    bool live_after =
        pc < segment->end || (pc == segment->end &&
                              segment->spill_reason == NYR_MACH_SPILL_LIVE_OUT);
    if (!live_after)
      break;
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (dst_is_def(in) && in->dst.as.reg == segment->vreg)
      continue;
    return true;
  }
  return false;
}

static bool color_segments(const nyr_mach_func_t *mach, nyr_mach_regalloc_t *a,
                           const size_t *loop_depth,
                           nyr_mach_reg_class_t reg_class,
                           bool preserve_gpr_calls, bool allow_rhs_tie,
                           const size_vec_t *clobber_sites,
                           size_t clobber_color_count) {
  if (!a->segment_len || !a->color_count)
    return true;
  regalloc_use_index_t use_index = {0};
  if (!regalloc_use_index_build(mach, a->vreg_len, &use_index))
    return false;
  regalloc_affinity_index_t affinity_index = {0};
  qsort(a->segments, a->segment_len, sizeof(*a->segments), cmp_seg_start);
  (void)affinity_index_build(mach, a, reg_class, allow_rhs_tie,
                             &affinity_index);
  bool use_color_mask = a->color_count <= 64;
  active_t *active = calloc(a->color_count * 2 + 2, sizeof(*active));
  bool *used = use_color_mask ? NULL : calloc(a->color_count, sizeof(*used));
  bool *blocked =
      use_color_mask ? NULL : calloc(a->color_count, sizeof(*blocked));
  if (!active || (!use_color_mask && (!used || !blocked))) {
    free(active);
    free(used);
    free(blocked);
    regalloc_use_index_free(&use_index);
    affinity_index_free(&affinity_index);
    return false;
  }
  size_t active_n = 0;
  /*
   * Segments are sorted by start, so each color's clobber cursor only moves
   * forward. This makes the fixed-register query amortized linear in the
   * number of clobber sites instead of binary-searching for every segment.
   */
  size_t clobber_cursor[64] = {0};
  for (size_t i = 0; i < a->segment_len; ++i) {
    nyr_mach_live_segment_t *s = &a->segments[i];
    size_t color_limit = segment_color_limit(mach, s, a->color_count);
    uint64_t used_mask = 0;
    uint64_t blocked_mask = 0;
    if (!use_color_mask) {
      memset(used, 0, a->color_count * sizeof(*used));
      memset(blocked, 0, a->color_count * sizeof(*blocked));
    }
    /*
     * Partial fixed-register clobbers do not require a stack-home split.
     * Keep values resident in unaffected registers and exclude only colors
     * clobbered by instructions covered by this live segment. A definition
     * at the clobber instruction is produced after the fixed-register work.
     */
    for (size_t c = 0; c < color_limit && c < clobber_color_count; ++c)
      if (segment_blocks_color(mach, s, &clobber_sites[c],
                               &clobber_cursor[c])) {
        if (use_color_mask)
          blocked_mask |= UINT64_C(1) << c;
        else
          blocked[c] = true;
      }
    for (size_t j = 0; j < active_n;) {
      if (active[j].end < s->start)
        active_remove(active, &active_n, j);
      else
        ++j;
    }
    for (size_t j = 0; j < active_n; ++j) {
      if (active[j].color >= 0 && (size_t)active[j].color < a->color_count) {
        if (use_color_mask)
          used_mask |= UINT64_C(1) << active[j].color;
        else
          used[active[j].color] = true;
      }
    }
    if (use_color_mask)
      used_mask |= blocked_mask;
    else
      for (size_t c = 0; c < color_limit; ++c)
        used[c] = used[c] || blocked[c];
    /*
     * Try copy affinity before the ordinary lowest-numbered color.  A hint
     * is advisory only: overlapping live ranges and spills retain priority.
     */
    bool prefer_callee =
        preserve_gpr_calls && reg_class == NYR_MACH_REGCLASS_GPR &&
        segment_uses_call_arg(mach, s);
    uint32_t affinity_source = UINT32_MAX;
    int color =
        affinity_hint_color(mach, a, &affinity_index, reg_class, allow_rhs_tie,
                            s->vreg, s->start, s->end, &affinity_source);
    /*
     * A copy neighbor may suggest a caller-saved color.  Call arguments are
     * consumed at the ABI boundary, so keep the callee-saved preference
     * instead of staging the value through its home only to reload it into an
     * argument register.
     */
    if (prefer_callee && color >= 5)
      color = -1;
    const char *choice = "lowest-free";
    bool hinted_color_used =
        color >= 0 && (size_t)color < a->color_count &&
        (use_color_mask ? (used_mask & (UINT64_C(1) << color)) != 0
                        : used[color]);
    bool coalesced_dead_input = false;
    if (hinted_color_used && color >= 0 && (size_t)color < color_limit &&
        affinity_source < a->vreg_len && s->start < mach->inst_len) {
      const nyr_mach_inst_t *in = &mach->insts[s->start];
      /*
       * The dying input may donate its register only to a pure definition.
       * A destination that is also read needs both values at once.
       */
      uint32_t inputs[3 + NYIR_CALL_MAX_ARGS];
      size_t input_count =
          inst_uses(in, inputs, sizeof(inputs) / sizeof(*inputs));
      bool reads_own_dst = false;
      for (size_t k = 0; k < input_count; ++k)
        reads_own_dst |= inputs[k] == s->vreg;
      bool is_input = !reads_own_dst && in->dst.kind == NYR_MACH_OPERAND_VREG &&
                      in->dst.as.reg == s->vreg &&
                      regalloc_affinity_input(in, affinity_source, reg_class,
                                              allow_rhs_tie);
      const nyr_mach_live_segment_t *source =
          is_input ? affinity_index_segment_at(a, &affinity_index,
                                               affinity_source, s->start)
                   : NULL;
      if (source && source->end == s->start && source->color == color) {
        size_t source_index = (size_t)(source - a->segments);
        size_t active_source = SIZE_MAX;
        bool color_has_other_live_value = false;
        for (size_t j = 0; j < active_n; ++j) {
          if (active[j].color != color)
            continue;
          if (active[j].seg == source_index && active[j].end == s->start &&
              active_source == SIZE_MAX)
            active_source = j;
          else
            color_has_other_live_value = true;
        }
        bool color_blocked = use_color_mask
                                 ? (blocked_mask & (UINT64_C(1) << color)) != 0
                                 : blocked[color];
        if (active_source != SIZE_MAX && !color_has_other_live_value &&
            !color_blocked) {
          /*
           * Keep the dying input active: it shares this color until its
           * last use, and a later eviction must release it too.
           */
          (void)active_source;
          if (use_color_mask)
            used_mask &= ~(UINT64_C(1) << color);
          else
            used[color] = false;
          hinted_color_used = false;
          coalesced_dead_input = true;
        }
      }
    }
    if (color < 0 || (size_t)color >= color_limit || hinted_color_used)
      color = -1;
    else
      choice = coalesced_dead_input ? "dead-input-coalescing"
                                    : "tied-input-affinity";
    if (color < 0) {
      for (size_t rank = 0; rank < color_limit; ++rank) {
        size_t c = segment_color_at_rank(rank, color_limit, reg_class,
                                         preserve_gpr_calls || mach->c_abi,
                                         prefer_callee);
        if (use_color_mask ? !(used_mask & (UINT64_C(1) << c)) : !used[c]) {
          color = (int)c;
          break;
        }
      }
    }
    if (color < 0) {
      size_t victim = SIZE_MAX;
      eviction_score_t best =
          eviction_score(mach, &use_index, s, s->start, loop_depth);
      for (size_t j = 0; j < active_n; ++j) {
        if ((size_t)active[j].color >= color_limit ||
            (use_color_mask
                 ? (blocked_mask & (UINT64_C(1) << active[j].color)) != 0
                 : blocked[active[j].color]))
          continue;
        nyr_mach_live_segment_t *candidate = &a->segments[active[j].seg];
        if (!active[j].eviction_metadata_ready) {
          segment_use_bounds(&use_index, candidate, &active[j].use_cursor,
                             &active[j].use_end);
          active[j].loop_depth = loop_depth && candidate->block != UINT32_MAX
                                     ? loop_depth[candidate->block]
                                     : 0;
          active[j].profile_heat = segment_profile_heat(mach, candidate->block);
          active[j].eviction_metadata_ready = true;
        }
        while (active[j].use_cursor < active[j].use_end &&
               use_index.positions[active[j].use_cursor] < s->start)
          ++active[j].use_cursor;
        size_t next_use = active[j].use_cursor;
        eviction_score_t score = {
            .no_future_use = next_use == active[j].use_end,
            .next_use_distance = next_use == active[j].use_end
                                     ? SIZE_MAX
                                     : use_index.positions[next_use] - s->start,
            .use_count = active[j].use_end - next_use,
            .loop_depth = active[j].loop_depth,
            .profile_heat = active[j].profile_heat,
            .end = candidate->end,
            .vreg = candidate->vreg,
        };
        if (eviction_score_better(score, best)) {
          best = score;
          victim = j;
        }
      }
      if (victim != SIZE_MAX) {
        nyr_mach_live_segment_t *old = &a->segments[active[victim].seg];
        color = active[victim].color;
        ny_dbg_heuristic_event(
            "regalloc",
            "class=%s v%u spills v%u at pc=%zu; victim score heat=%llu "
            "loop=%zu next=%zu uses=%zu end=%zu",
            reg_class_name(reg_class), s->vreg, old->vreg, s->start,
            (unsigned long long)best.profile_heat, best.loop_depth,
            best.next_use_distance, best.use_count, best.end);
        old->color = -1;
        old->reload = true;
        old->spill = true;
        old->spill_reason = NYR_MACH_SPILL_REGISTER_PRESSURE;
        active_remove(active, &active_n, victim);
        /*
         * A tied boundary (dying input and its result) lets two live
         * segments share one color. Evicting only one of them would leave
         * the other beside the new owner, so release the whole color.
         */
        for (size_t j = 0; j < active_n;) {
          if (active[j].color != color) {
            ++j;
            continue;
          }
          nyr_mach_live_segment_t *tied = &a->segments[active[j].seg];
          tied->color = -1;
          tied->reload = true;
          tied->spill = true;
          tied->spill_reason = NYR_MACH_SPILL_REGISTER_PRESSURE;
          active_remove(active, &active_n, j);
        }
        choice = "evict-lower-priority-live-range";
      } else {
        ny_dbg_heuristic_event("regalloc",
                               "class=%s spills v%u at pc=%zu; no active "
                               "segment is a legal eviction candidate",
                               reg_class_name(reg_class), s->vreg, s->start);
        s->reload = true;
        s->spill = true;
        s->spill_reason = NYR_MACH_SPILL_REGISTER_PRESSURE;
        choice = "spill-current-no-evictable-victim";
      }
    }
    s->color = color;
    char reason[96];
    snprintf(reason, sizeof(reason), "class=%s %s", reg_class_name(reg_class),
             choice);
    ny_dbg_regalloc_decision((int)s->vreg, color, reason);
    if (color >= 0) {
      active[active_n++] = (active_t){
          .start = s->start, .end = s->end, .seg = i, .color = color};
    }
  }
  free(active);
  free(used);
  free(blocked);
  regalloc_use_index_free(&use_index);
  affinity_index_free(&affinity_index);
  return true;
}

static bool block_has_color_conflict(const nyr_mach_regalloc_t *a,
                                     const nyr_mach_live_segment_t *target,
                                     int color, const size_vec_t *by_block) {
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
    const nyr_mach_live_segment_t *s = &a->segments[indices[k]];
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
    const nyr_mach_regalloc_t *a, const nyr_mach_live_segment_t *target,
    int color, const size_vec_t *by_block, const uint8_t *expensive_vreg,
    const uint32_t *excl, size_t excl_len) {
  if (!by_block || target->block >= a->block_count)
    return false;
  const size_t *indices = by_block[target->block].data;
  size_t count = by_block[target->block].len;
  for (size_t k = 0; k < count; ++k) {
    const nyr_mach_live_segment_t *s = &a->segments[indices[k]];
    if (s == target || s->color != color || s->end < target->start ||
        target->end < s->start)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded && (s->carried || (expensive_vreg && s->vreg < a->vreg_len &&
                                     expensive_vreg[s->vreg])))
      return false;
  }
  return true;
}

static void block_evict_color_conflicts_excl(
    nyr_mach_regalloc_t *a, const nyr_mach_live_segment_t *target, int color,
    const size_vec_t *by_block, const uint32_t *excl, size_t excl_len) {
  const size_t *indices = by_block[target->block].data;
  size_t count = by_block[target->block].len;
  for (size_t k = 0; k < count; ++k) {
    nyr_mach_live_segment_t *s = &a->segments[indices[k]];
    if (s == target || s->color != color || s->end < target->start ||
        target->end < s->start)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded) {
      if (getenv("NY_DUMP_REGALLOC_CONFLICTS"))
        fprintf(stderr,
                "[regalloc-conflict] B%u color=%d keeper=v%u [%zu,%zu] "
                "evicts=v%u [%zu,%zu] carried=%d\n",
                target->block, color, target->vreg, target->start, target->end,
                s->vreg, s->start, s->end, (int)s->carried);
      s->color = -1;
      s->reload = true;
      s->spill = true;
      s->spill_reason = NYR_MACH_SPILL_BLOCK_CONFLICT;
      s->carried = false;
    }
  }
}

static bool block_color_all_conflicts_evictable_excl(
    const nyr_mach_regalloc_t *a, size_t block, int color,
    const size_vec_t *by_block, const uint8_t *expensive_vreg,
    const uint32_t *excl, size_t excl_len) {
  if (!a || !by_block || block >= a->block_count)
    return false;
  for (size_t k = 0; k < by_block[block].len; ++k) {
    const nyr_mach_live_segment_t *s = &a->segments[by_block[block].data[k]];
    if (s->color != color)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded && (s->carried || (expensive_vreg && s->vreg < a->vreg_len &&
                                     expensive_vreg[s->vreg])))
      return false;
  }
  return true;
}

static void block_evict_color_all_excl(nyr_mach_regalloc_t *a, size_t block,
                                       int color, const size_vec_t *by_block,
                                       const uint32_t *excl, size_t excl_len) {
  for (size_t k = 0; k < by_block[block].len; ++k) {
    nyr_mach_live_segment_t *s = &a->segments[by_block[block].data[k]];
    if (s->color != color)
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded) {
      if (getenv("NY_DUMP_REGALLOC_CONFLICTS"))
        fprintf(stderr,
                "[regalloc-conflict] B%zu color=%d edge-owner=v%u evicts=v%u "
                "[%zu,%zu] carried=%d\n",
                block, color, excl_len ? excl[0] : UINT32_MAX, s->vreg,
                s->start, s->end, (int)s->carried);
      s->color = -1;
      s->reload = true;
      s->spill = true;
      s->spill_reason = NYR_MACH_SPILL_BLOCK_CONFLICT;
      s->carried = false;
    }
  }
}

/*
 * Cost of the evictions block_evict_color_conflicts_excl() (target != NULL)
 * or block_evict_color_all_excl() (target == NULL) would perform. Every
 * evicted segment costs 1; each read or write of its value inside its span
 * adds 8, since that access then goes through the stack home. A live-through
 * segment with no access in its block is the cheapest victim.
 */
static size_t segment_access_count(const nyr_mach_func_t *mach,
                                   const nyr_mach_live_segment_t *s) {
  size_t n = 0;
  for (size_t pc = s->start; pc <= s->end && pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (dst_is_def(in) && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == s->vreg)
      ++n;
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t use_count = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < use_count; ++i)
      if (uses[i] == s->vreg)
        ++n;
  }
  return n;
}

static size_t block_color_eviction_count_excl(
    const nyr_mach_func_t *mach, const nyr_mach_regalloc_t *a,
    const nyr_mach_live_segment_t *target, size_t block, int color,
    const size_vec_t *by_block, const uint32_t *excl, size_t excl_len) {
  if (!a || !by_block)
    return SIZE_MAX;
  if (target)
    block = target->block;
  if (block >= a->block_count)
    return SIZE_MAX;
  size_t n = 0;
  for (size_t k = 0; k < by_block[block].len; ++k) {
    const nyr_mach_live_segment_t *s = &a->segments[by_block[block].data[k]];
    if (s == target || s->color != color)
      continue;
    if (target && (s->end < target->start || target->end < s->start))
      continue;
    bool excluded = false;
    for (size_t x = 0; x < excl_len; ++x)
      if (s->vreg == excl[x]) {
        excluded = true;
        break;
      }
    if (!excluded)
      n += 1 + 8 * segment_access_count(mach, s);
  }
  return n;
}

static nyr_mach_live_segment_t *
segment_in_block_vreg(nyr_mach_regalloc_t *a, uint32_t vreg, size_t block,
                      const size_vec_t *by_block, bool *unique) {
  if (unique)
    *unique = false;
  if (!a || !by_block || block >= a->block_count || vreg >= a->vreg_len)
    return NULL;
  nyr_mach_live_segment_t *found = NULL;
  for (size_t k = 0; k < by_block[block].len; ++k) {
    nyr_mach_live_segment_t *segment = &a->segments[by_block[block].data[k]];
    if (segment->vreg != vreg)
      continue;
    if (found)
      return NULL;
    found = segment;
  }
  if (unique)
    *unique = found != NULL;
  return found;
}

/*
 * (vreg, block) -> unique segment index. Long-lived values can have thousands
 * of segments, so even scanning one value's own list per candidate is
 * quadratic. One open-addressed table makes every lookup O(1); duplicate
 * entries are marked so the caller sees the same "not unique" answer as a
 * full scan.
 */
typedef struct {
  uint64_t key;
  uint32_t seg; /* 1-based; 0 empty, UINT32_MAX duplicate */
} seg_block_slot_t;

typedef struct {
  seg_block_slot_t *slots;
  size_t cap;
} seg_block_index_t;

static uint64_t seg_block_hash(uint64_t key) {
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53ULL;
  key ^= key >> 33;
  return key;
}

static bool seg_block_index_build(seg_block_index_t *idx,
                                  const nyr_mach_regalloc_t *a) {
  if (!idx)
    return false;
  *idx = (seg_block_index_t){0};
  size_t n = a ? a->segment_len : 0;
  if (n > UINT32_MAX - 1)
    return false;
  size_t want = n ? n * 2 : 1;
  if (n && want / 2 != n)
    return false;
  size_t cap = 8;
  while (cap < want) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap <<= 1;
  }
  seg_block_slot_t *slots = calloc(cap, sizeof(*slots));
  if (!slots)
    return false;
  for (size_t i = 0; i < n; ++i) {
    const nyr_mach_live_segment_t *s = &a->segments[i];
    uint64_t key = ((uint64_t)s->vreg << 32) | (uint32_t)s->block;
    size_t h = (size_t)seg_block_hash(key) & (cap - 1);
    for (;;) {
      seg_block_slot_t *slot = &slots[h];
      if (!slot->seg) {
        slot->key = key;
        slot->seg = (uint32_t)i + 1;
        break;
      }
      if (slot->key == key) {
        slot->seg = UINT32_MAX;
        break;
      }
      h = (h + 1) & (cap - 1);
    }
  }
  idx->slots = slots;
  idx->cap = cap;
  return true;
}

static nyr_mach_live_segment_t *
seg_block_index_find(nyr_mach_regalloc_t *a, const seg_block_index_t *idx,
                     uint32_t vreg, size_t block, bool *unique) {
  if (unique)
    *unique = false;
  if (!a || !idx || !idx->slots || vreg >= a->vreg_len ||
      block > UINT32_MAX)
    return NULL;
  uint64_t key = ((uint64_t)vreg << 32) | (uint32_t)block;
  size_t h = (size_t)seg_block_hash(key) & (idx->cap - 1);
  for (;;) {
    const seg_block_slot_t *slot = &idx->slots[h];
    if (!slot->seg)
      return NULL;
    if (slot->key == key) {
      if (slot->seg == UINT32_MAX)
        return NULL;
      if (unique)
        *unique = true;
      return &a->segments[slot->seg - 1];
    }
    h = (h + 1) & (idx->cap - 1);
  }
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
  size_t copy_pc;        /* phi copy instruction in the edge block */
  bool src_dies_at_copy; /* src segment ends at the copy: dst may reuse it */
  nyr_mach_live_segment_t *ev;     /* phi dst segment in the edge block */
  nyr_mach_live_segment_t *eseg;   /* src segment in the edge block */
  nyr_mach_live_segment_t *srcseg; /* src segment in the pred block (end) */
} loop_copy_t;

static bool coalesce_preg_reaches_edge(const nyr_mach_func_t *mach,
                                       const nyr_mach_live_segment_t *x,
                                       bool preserve_gpr_calls);
static bool segment_color_survives_clobbers(const nyr_mach_func_t *mach,
                                            const nyr_mach_live_segment_t *x,
                                            nyr_mach_reg_class_t reg_class,
                                            int color, bool preserve_gpr_calls);

static bool loop_phi_carry_enabled(nyr_mach_reg_class_t reg_class) {
  /*
   * Vector carry is enabled by default: its executable regression checks
   * every lane across loop backedges and the loop exit home. Keep an explicit
   * opt-out for diagnosis. Scalar GPR/FPR carry remains opt-in.
   */
  const bool vector = reg_class == NYR_MACH_REGCLASS_VECTOR;
  const char *value =
      getenv(vector ? "NYTRIX_VECTOR_LOOP_CARRY" : "NYTRIX_GPR_LOOP_CARRY");
  if (vector && !value)
    return true;
  return (reg_class == NYR_MACH_REGCLASS_GPR ||
          reg_class == NYR_MACH_REGCLASS_FPR ||
          reg_class == NYR_MACH_REGCLASS_VECTOR) &&
         value && strcmp(value, "0") != 0 && strcmp(value, "off") != 0 &&
         strcmp(value, "false") != 0;
}

/*
 * Call arguments are observable home-backed values in the x86 encoder. Keep
 * loop-carried values that cross a call on their canonical home until the
 * call boundary itself has a complete register-preservation proof.
 */
static bool loop_phi_has_call_use(const nyr_mach_func_t *mach, uint32_t vreg) {
  if (!mach)
    return true;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode != NYR_MACH_CALL)
      continue;
    for (size_t a = 0; in->args && a < in->args_len; ++a)
      if (in->args[a].kind == NYR_MACH_OPERAND_VREG &&
          in->args[a].as.reg == vreg)
        return true;
  }
  return false;
}

/*
 * A carried loop value can stay resident only if calls inside the loop region
 * preserve the encoder's carry contract. Calls may home and invalidate
 * residency even when the loop value is not itself an argument. Restrict the
 * carry optimization to call-free loop bodies; ordinary segment allocation
 * already models call clobbers.
 */
static bool loop_phi_region_has_call(const nyr_mach_func_t *mach,
                                     const size_vec_t *succ,
                                     const bool *can_reach_header,
                                     size_t block_count, size_t header,
                                     size_t *work) {
  if (!mach || !succ || !can_reach_header || !work || header >= block_count)
    return true;
  uint8_t *in_loop = calloc(block_count, sizeof(*in_loop));
  if (!in_loop)
    return true;
  size_t work_len = 1;
  in_loop[header] = 1;
  work[0] = header;
  for (size_t wi = 0; wi < work_len; ++wi) {
    size_t block = work[wi];
    for (size_t si = 0; si < succ[block].len; ++si) {
      size_t next = succ[block].data[si];
      if (next >= block_count || !can_reach_header[next] || in_loop[next])
        continue;
      in_loop[next] = 1;
      work[work_len++] = next;
    }
  }
  bool has_call = false;
  for (size_t bi = 0; bi < work_len && !has_call; ++bi) {
    const nyr_mach_block_t *block = &mach->blocks[work[bi]];
    for (size_t ii = 0; ii < block->inst_count; ++ii) {
      const nyr_mach_inst_t *in = &mach->insts[block->first_inst + ii];
      if (in->opcode == NYR_MACH_CALL) {
        has_call = true;
        break;
      }
    }
  }
  free(in_loop);
  return has_call;
}

static bool block_reads_vreg_before(const nyr_mach_func_t *mach, size_t block,
                                    size_t before, uint32_t vreg) {
  if (!mach || block >= mach->block_len)
    return true;
  const nyr_mach_block_t *b = &mach->blocks[block];
  if (before < b->first_inst || before > (size_t)b->first_inst + b->inst_count)
    return true;
  for (size_t pc = b->first_inst; pc < before; ++pc) {
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t use_count =
        inst_uses(&mach->insts[pc], uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < use_count; ++i)
      if (uses[i] == vreg)
        return true;
  }
  return false;
}

/*
 * A latch definition may read the PHI it replaces as src0 when its lowering is
 * one instruction (or a copy of src0 into the destination followed by one
 * instruction) whose other operand is not the PHI: reading src0 into the
 * shared register is then a no-op. Multi-instruction lowerings (vector shifts,
 * compares, selects) may write the destination before their last read.
 */
static bool vector_latch_def_reads_phi_ok(const nyr_mach_func_t *mach,
                                          const nyr_mach_inst_t *def,
                                          uint32_t phi) {
  if (def->args_len)
    return false;
  /*
   * VBLENDVPD reads all three vector operands before writing its destination.
   */
  if (def->opcode == NYR_MACH_SELECT &&
      def->dst.kind == NYR_MACH_OPERAND_VREG &&
      def->dst.as.reg < mach->vreg_len &&
      mach->vreg_types[def->dst.as.reg] == NYR_MACH_TYPE_V256_I64)
    return true;
  bool src0_phi =
      def->src0.kind == NYR_MACH_OPERAND_VREG && def->src0.as.reg == phi;
  bool other_phi =
      (def->src1.kind == NYR_MACH_OPERAND_VREG && def->src1.as.reg == phi) ||
      (def->src2.kind == NYR_MACH_OPERAND_VREG && def->src2.as.reg == phi);
  if (other_phi)
    return false;
  if (!src0_phi)
    return true;
  switch (def->opcode) {
  case NYR_MACH_ADD:
  case NYR_MACH_SUB:
  case NYR_MACH_AND:
  case NYR_MACH_OR:
  case NYR_MACH_XOR:
    return def->src1.kind == NYR_MACH_OPERAND_VREG ||
           def->src1.kind == NYR_MACH_OPERAND_FRAME;
  default:
    return false;
  }
}

/*
 * A loop recurrence computes the next value from the old one in the latch
 * itself (x' = f(x)). Sharing one register between the PHI and the copy
 * source is still interference-free when the source is defined in this edge
 * block and every read of the old PHI value lies strictly before that
 * definition, and nothing after the copy reads it. Used for the vector
 * class only, whose carry path has executable lane-by-lane regressions.
 */
static bool vector_phi_reads_precede_source(const nyr_mach_func_t *mach,
                                            size_t block, size_t copy_pc,
                                            uint32_t phi, uint32_t src) {
  if (!mach || block >= mach->block_len)
    return false;
  const nyr_mach_block_t *b = &mach->blocks[block];
  size_t end = (size_t)b->first_inst + b->inst_count;
  if (copy_pc < b->first_inst || copy_pc >= end)
    return false;
  size_t def_pc = SIZE_MAX;
  for (size_t pc = b->first_inst; pc < copy_pc; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    if (in->opcode != NYR_MACH_STORE && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == src)
      def_pc = pc;
  }
  if (def_pc == SIZE_MAX)
    return false;
  /*
   * Check that the definition consumes the old value before writing the
   * shared destination register.
   */
  const nyr_mach_inst_t *def = &mach->insts[def_pc];
  if (!vector_latch_def_reads_phi_ok(mach, def, phi))
    return false;
  for (size_t pc = def_pc + 1; pc < end; ++pc) {
    if (pc == copy_pc)
      continue;
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t use_count =
        inst_uses(&mach->insts[pc], uses, sizeof(uses) / sizeof(*uses));
    for (size_t i = 0; i < use_count; ++i)
      if (uses[i] == phi)
        return false;
  }
  return true;
}

static void
coalesce_loop_edges(const nyr_mach_func_t *mach, nyr_mach_regalloc_t *a,
                    const size_vec_t *succ, const size_vec_t *pred,
                    const size_vec_t *by_block, const size_t *loop_depth,
                    bool preserve_gpr_calls, nyr_mach_reg_class_t reg_class) {
  if (!mach || !a || !pred || !by_block || !loop_depth || mach->block_len < 2)
    return;
  /*
   * Read this process-wide switch once, not once per candidate PHI.
   */
  const bool carry_enabled = loop_phi_carry_enabled(reg_class);
  const bool carry_debug = getenv("NY_CARRY_DEBUG") != NULL;
  /*
   * Structural checks below restrict carries to loop chains whose incoming
   * edges materialize one consistent physical register.
   */
  size_t nb = mach->block_len;
  /*
   * Process each loop header independently to carry registers across backedges.
   */
  size_t nv = a->vreg_len;
  size_vec_t *by_vreg = calloc(nv, sizeof(*by_vreg));
  uint8_t *expensive_vreg = calloc(nv, sizeof(*expensive_vreg));
  if (!by_vreg || !expensive_vreg) {
    free(by_vreg);
    free(expensive_vreg);
    return;
  }
  /*
   * Keep loop-edge coalescing from evicting the result of an expensive
   * operation such as signed division. A single home store/reload can erase
   * the benefit of removing a cheap register copy on every loop iteration.
   */
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->dst.kind != NYR_MACH_OPERAND_VREG || in->dst.as.reg >= nv)
      continue;
    if (in->opcode == NYR_MACH_DIV || in->opcode == NYR_MACH_MOD)
      expensive_vreg[in->dst.as.reg] = 1;
  }
  for (size_t i = 0; i < a->segment_len; ++i) {
    uint32_t v = a->segments[i].vreg;
    if (v >= nv || !size_vec_push(&by_vreg[v], i)) {
      for (size_t k = 0; k < nv; ++k)
        free(by_vreg[k].data);
      free(by_vreg);
      free(expensive_vreg);
      return;
    }
  }
  seg_block_index_t block_index = {0};
  if (!seg_block_index_build(&block_index, a)) {
    for (size_t k = 0; k < nv; ++k)
      free(by_vreg[k].data);
    free(by_vreg);
    free(expensive_vreg);
    return;
  }
  for (size_t h = 0; h < nb; ++h) {
    if (pred[h].len < 2)
      continue;
    /*
     * The loop-edge coalescer uses block layout order to identify backedges
     * (see the candidate-color ordering below). A join with no predecessor
     * after its header cannot carry a loop value, so avoid scanning every
     * machine block and rebuilding reachability for ordinary diamonds.
     */
    bool has_layout_backedge = false;
    for (size_t pi = 0; pi < pred[h].len; ++pi) {
      if (pred[h].data[pi] > h) {
        has_layout_backedge = true;
        break;
      }
    }
    if (!has_layout_backedge)
      continue;
    const nyr_mach_block_t *hb = &mach->blocks[h];
    if (!hb->inst_count)
      continue;

    /*
     * Edge blocks of the header: blocks ending in a branch to h.
     */
    size_t edge_count = 0;
    size_t edge_blocks[256];
    /*
     * `pred[h]` was built by scanning source blocks in layout order, so it
     * preserves the old ascending order while avoiding a full machine-CFG
     * scan for every loop header.  Keep duplicate edges from one predecessor
     * unique, as the former scan did, and retain the same 256-edge bailout.
     */
    if (pred[h].len > sizeof(edge_blocks) / sizeof(edge_blocks[0]))
      continue;
    for (size_t pi = 0; pi < pred[h].len; ++pi) {
      size_t e = pred[h].data[pi];
      if (e == h)
        continue;
      const nyr_mach_block_t *eb = &mach->blocks[e];
      if (!eb->inst_count)
        continue;
      const nyr_mach_inst_t *last =
          &mach->insts[eb->first_inst + eb->inst_count - 1];
      if ((last->opcode == NYR_MACH_BR || last->opcode == NYR_MACH_BR_IF) &&
          last->src1.kind == NYR_MACH_OPERAND_BLOCK &&
          last->src1.as.block_index == h) {
        bool duplicate = false;
        for (size_t i = 0; i < edge_count; ++i)
          if (edge_blocks[i] == e) {
            duplicate = true;
            break;
          }
        if (duplicate)
          continue;
        edge_blocks[edge_count++] = e;
      }
    }
    if (carry_debug)
      fprintf(stderr, "carry: header=%zu preds=%zu edges=%zu class=%s\n", h,
              pred[h].len, edge_count, reg_class_name(reg_class));
    if (carry_debug)
      for (size_t si = 0; si < by_block[h].len; ++si) {
        const nyr_mach_live_segment_t *seg = &a->segments[by_block[h].data[si]];
        fprintf(stderr,
                "carry: header=%zu v=%u span=%zu-%zu color=%d reload=%d\n", h,
                seg->vreg, seg->start, seg->end, seg->color, seg->reload);
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

    /*
     * One PHI is coalesced at most once per header. Each of its region
     * segments may serve as `hv`, and recoalescing the same PHI into another
     * color would repeat evictions without carrying anything new.
     */
    uint8_t *coalesced_here = calloc(nv ? nv : 1, sizeof(*coalesced_here));
    for (size_t i = 0; coalesced_here && i < a->segment_len; ++i) {
      nyr_mach_live_segment_t *hv = &a->segments[i];
      if (hv->vreg < nv && coalesced_here[hv->vreg])
        continue;
      if (hv->block >= nb ||
          (!hv->reload && !nyr_mach_regalloc_live_in(a, hv->block, hv->vreg)) ||
          edge_block[hv->block] || !reaches_h[hv->block])
        continue;
      uint32_t v = hv->vreg;
      bool unique = false;
      if (seg_block_index_find(a, &block_index, v, hv->block, &unique) != hv ||
          !unique) {
        if (carry_debug)
          fprintf(stderr,
                  "carry: header=%zu v=%u block=%u not a unique block "
                  "segment\n",
                  h, v, hv->block);
        continue;
      }

      loop_copy_t copies[256];
      size_t copy_len = 0;
      bool ok = true;
      for (size_t ei = 0; ei < edge_count; ++ei) {
        size_t e = edge_blocks[ei];
        const nyr_mach_block_t *eb = &mach->blocks[e];
        uint32_t src = 0;
        size_t copy_pc = SIZE_MAX;
        bool found_copy = false;
        for (size_t n = 0; n + 1 < eb->inst_count; ++n) { /* skip branch */
          const nyr_mach_inst_t *in = &mach->insts[eb->first_inst + n];
          if (in->opcode == NYR_MACH_COPY &&
              in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == v &&
              in->src0.kind == NYR_MACH_OPERAND_VREG) {
            src = in->src0.as.reg;
            copy_pc = eb->first_inst + n;
            found_copy = true;
            break;
          }
        }
        if (!found_copy || src >= a->vreg_len) {
          if (carry_debug)
            fprintf(stderr, "carry: header=%zu v=%u edge=%zu no phi copy\n", h,
                    v, e);
          ok = false;
          break;
        }
        /*
         * The old PHI value must remain available until the incoming copy if
         * the edge block reads it first. Sharing the copy source's register
         * across that earlier read would create overlapping live ranges even
         * though the copy itself is a valid dying-input tie.
         * Vector latch recurrences (x' = f(x) computed in the latch, with
         * every read of x before the definition of x') are the exception:
         * vector_phi_reads_precede_source() proves the overlap nominal and
         * verify_vector_latch_recurrence() re-checks the final coloring.
         */
        if (block_reads_vreg_before(mach, e, copy_pc, v) &&
            !(reg_class == NYR_MACH_REGCLASS_VECTOR && e > h &&
              vector_phi_reads_precede_source(mach, e, copy_pc, v, src))) {
          if (carry_debug) {
            fprintf(stderr,
                    "carry: header=%zu v=%u edge=%zu reads old phi before "
                    "copy at %zu; keep homes (src=v%u class=%s vregs=%zu)\n",
                    h, v, e, copy_pc, src, reg_class_name(reg_class),
                    (size_t)mach->vreg_len);
            if (getenv("NY_CARRY_DEBUG_BLOCK") &&
                reg_class == NYR_MACH_REGCLASS_VECTOR) {
              const nyr_mach_block_t *bb = &mach->blocks[e];
              for (size_t q = bb->first_inst;
                   q < (size_t)bb->first_inst + bb->inst_count; ++q) {
                const nyr_mach_inst_t *qi = &mach->insts[q];
                fprintf(stderr, "   pc=%zu %s dst=%d s0=%d s1=%d\n", q,
                        nyr_mach_opcode_name(qi->opcode),
                        qi->dst.kind == NYR_MACH_OPERAND_VREG
                            ? (int)qi->dst.as.reg
                            : -1,
                        qi->src0.kind == NYR_MACH_OPERAND_VREG
                            ? (int)qi->src0.as.reg
                            : -1,
                        qi->src1.kind == NYR_MACH_OPERAND_VREG
                            ? (int)qi->src1.as.reg
                            : -1);
              }
            }
          }
          ok = false;
          break;
        }
        bool ev_unique = false, eseg_unique = false;
        nyr_mach_live_segment_t *ev =
            seg_block_index_find(a, &block_index, v, e, &ev_unique);
        nyr_mach_live_segment_t *eseg =
            seg_block_index_find(a, &block_index, src, e, &eseg_unique);
        if (!ev || !eseg || !ev_unique || !eseg_unique) {
          if (carry_debug)
            fprintf(stderr,
                    "carry: header=%zu v=%u edge=%zu missing segment "
                    "ev=%d eseg=%d evu=%d esegu=%d src=%u\n",
                    h, v, e, ev != NULL, eseg != NULL, (int)ev_unique,
                    (int)eseg_unique, src);
          ok = false;
          break;
        }
        bool entry_preheader = e == 0 && pred[e].len == 0 && e < h;
        /*
         * A source defined in the edge block itself reaches the copy
         * regardless of how many predecessors the edge block has. Only a
         * source that must arrive from the predecessor needs its terminator
         * value to be unique and live at the branch.
         */
        bool src_defined_on_edge = false;
        for (size_t pc = eb->first_inst; pc < copy_pc; ++pc) {
          const nyr_mach_inst_t *in = &mach->insts[pc];
          if (dst_is_def(in) && in->dst.kind == NYR_MACH_OPERAND_VREG &&
              in->dst.as.reg == src) {
            src_defined_on_edge = true;
            break;
          }
        }
        if (!src_defined_on_edge && !entry_preheader && pred[e].len != 1) {
          if (carry_debug)
            fprintf(stderr,
                    "carry: header=%zu v=%u edge=%zu preds=%zu not single\n", h,
                    v, e, pred[e].len);
          ok = false;
          break;
        }
        bool srcseg_unique = false;
        nyr_mach_live_segment_t *srcseg = NULL;
        if (src_defined_on_edge && pred[e].len != 1) {
          srcseg = eseg;
        } else {
          size_t p = entry_preheader ? e : pred[e].data[0];
          size_t bend =
              mach->blocks[p].first_inst + mach->blocks[p].inst_count - 1;
          srcseg = seg_block_index_find(a, &block_index, src, p,
                                        &srcseg_unique);
          if (!srcseg || srcseg->end != bend || !srcseg_unique) {
            if (!src_defined_on_edge) {
              if (carry_debug)
                fprintf(stderr,
                        "carry: header=%zu v=%u edge=%zu src=%u has no "
                        "predecessor segment and no edge definition\n",
                        h, v, e, src);
              ok = false;
              break;
            }
            srcseg = eseg;
          }
        }
        copies[copy_len++] = (loop_copy_t){
            .edge = e,
            .src = src,
            .copy_pc = copy_pc,
            /*
             * A source whose edge-block segment ends exactly at the copy has
             * no later reader, so the destination may take over its preg and
             * the copy becomes a no-op. This is what lets every incoming edge
             * of a loop phi agree on one physical register.
             */
            .src_dies_at_copy = eseg->end == copy_pc,
            .ev = ev,
            .eseg = eseg,
            .srcseg = srcseg};
      }
      if (!ok || copy_len != edge_count)
        continue;

      /*
       * The encoder's carried bit is per machine vreg, while this coalescing
       * proof is for one loop header. A machine vreg reused by a second PHI
       * (common after loop lowering) can have extra definitions outside this
       * header's incoming edges. Marking all of its segments carried then
       * seeds unrelated blocks from a preg that only this loop proved.
       * Require this PHI destination's complete definition set to belong to
       * the edges just inspected before publishing a vreg-wide carry.
       */
      for (size_t pc = 0; pc < mach->inst_len && ok; ++pc) {
        const nyr_mach_inst_t *in = &mach->insts[pc];
        if (!dst_is_def(in) || in->dst.kind != NYR_MACH_OPERAND_VREG ||
            in->dst.as.reg != v)
          continue;
        size_t def_block = SIZE_MAX;
        for (size_t b = 0; b < nb; ++b) {
          const nyr_mach_block_t *db = &mach->blocks[b];
          if (pc >= db->first_inst && pc < db->first_inst + db->inst_count) {
            def_block = b;
            break;
          }
        }
        if (def_block >= nb || !edge_block[def_block]) {
          if (carry_debug)
            fprintf(stderr,
                    "carry: header=%zu v=%u def pc=%zu block=%zu outside "
                    "edge set\n",
                    h, v, pc, def_block);
          ok = false;
        }
      }
      if (!ok)
        continue;

      /*
       * Candidate colors: distinct srcseg colors, backedges (pred after the
       * header in the base layout) first so the hot path wins conflicts.
       */
      int cands[64];
      size_t ncand = 0;
      /*
       * The PHI's own header color needs no header eviction; try it first.
       */
      if (hv->color >= 0 && (size_t)hv->color < a->color_count)
        cands[ncand++] = hv->color;
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
      /*
       * Choose the legal color whose carry evicts least (see
       * block_color_eviction_count_excl): an evicted loop value is reloaded
       * from its home on every access, which can cost more than the copy
       * the carry removes. Candidate order breaks ties.
       */
      size_t chosen_cost = SIZE_MAX;
      for (size_t ci = 0; ci < ncand && chosen_cost != 0; ++ci) {
        int C = cands[ci];
        if (!segment_accepts_color(mach, hv, a->color_count, C)) {
          if (carry_debug)
            fprintf(stderr, "carry: h=%zu v=%u C=%d reject header color\n", h,
                    v, C);
          continue;
        }
        uint32_t excl_v[1] = {v};
        if (!block_color_conflicts_evictable_excl(a, hv, C, by_block,
                                                  expensive_vreg, excl_v, 1) ||
            !block_color_all_conflicts_evictable_excl(
                a, h, C, by_block, expensive_vreg, excl_v, 1)) {
          if (carry_debug)
            fprintf(stderr, "carry: h=%zu v=%u C=%d reject header conflict\n",
                    h, v, C);
          continue;
        }
        bool all_ok = true;
        for (size_t k = 0; k < copy_len; ++k) {
          loop_copy_t *c = &copies[k];
          bool preheader_copy = c->edge < h;
          bool recolor_source = !preheader_copy || c->src_dies_at_copy;
          if (!loop_edge_dest_accepts_color(mach, c->ev, a->color_count, C) ||
              (recolor_source &&
               (!segment_accepts_color(mach, c->eseg, a->color_count, C) ||
                !segment_accepts_color(mach, c->srcseg, a->color_count, C) ||
                !segment_color_survives_clobbers(mach, c->eseg, reg_class, C,
                                                 preserve_gpr_calls) ||
                !segment_color_survives_clobbers(mach, c->srcseg, reg_class, C,
                                                 preserve_gpr_calls))) ||
              !segment_color_survives_clobbers(mach, c->ev, reg_class, C,
                                               preserve_gpr_calls) ||
              (preheader_copy && c->eseg->color == C && !c->src_dies_at_copy)) {
            if (carry_debug)
              fprintf(stderr,
                      "carry: h=%zu v=%u C=%d reject copy edge=%zu "
                      "ev=%d/%d eseg=%d srcseg=%d\n",
                      h, v, C, c->edge, c->ev->color, c->ev->spill_reason,
                      c->eseg->color, c->srcseg->color);
            all_ok = false;
            break;
          }
          uint32_t excl[2] = {v, c->src};
          uint32_t dest_excl[1] = {v};
          const bool dest_shares_src = preheader_copy && c->src_dies_at_copy;
          if (!block_color_conflicts_evictable_excl(
                  a, c->ev, C, by_block, expensive_vreg,
                  (preheader_copy && !dest_shares_src) ? dest_excl : excl,
                  (preheader_copy && !dest_shares_src) ? 1 : 2) ||
              (recolor_source &&
               (!block_color_conflicts_evictable_excl(
                    a, c->eseg, C, by_block, expensive_vreg, excl, 2) ||
                !block_color_conflicts_evictable_excl(
                    a, c->srcseg, C, by_block, expensive_vreg, excl + 1, 1)))) {
            if (carry_debug)
              fprintf(stderr,
                      "carry: h=%zu v=%u C=%d reject copy conflict edge=%zu\n",
                      h, v, C, c->edge);
            all_ok = false;
            break;
          }
          /*
           * The edge copy may be removed only if its source reaches the
           * predecessor terminator in the candidate color. Check the color
           * being considered, since the coalescer recolors this segment.
           */
          nyr_mach_live_segment_t source_in_candidate_color = *c->srcseg;
          source_in_candidate_color.color = C;
          if (!preheader_copy && !c->src_dies_at_copy &&
              !coalesce_preg_reaches_edge(mach, &source_in_candidate_color,
                                          preserve_gpr_calls)) {
            if (carry_debug)
              fprintf(stderr, "carry: h=%zu v=%u C=%d reject source edge=%zu\n",
                      h, v, C, c->edge);
            all_ok = false;
            break;
          }
        }
        if (!all_ok)
          continue;
        for (size_t si = 0; si < by_vreg[v].len; ++si) {
          nyr_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
          if (s == hv || s->block >= nb || edge_block[s->block] ||
              !reaches_h[s->block])
            continue;
          if (!segment_accepts_color(mach, s, a->color_count, C) ||
              !segment_color_survives_clobbers(mach, s, reg_class, C,
                                               preserve_gpr_calls) ||
              !block_color_conflicts_evictable_excl(
                  a, s, C, by_block, expensive_vreg, excl_v, 1)) {
            if (carry_debug)
              fprintf(stderr,
                      "carry: h=%zu v=%u C=%d reject chain segment B%u "
                      "preg=%d spill=%d reason=%d\n",
                      h, v, C, s->block, s->color, s->spill, s->spill_reason);
            all_ok = false;
            break;
          }
        }
        if (all_ok) {
          size_t evicted = block_color_eviction_count_excl(
                               mach, a, hv, 0, C, by_block, excl_v, 1) +
                           block_color_eviction_count_excl(mach, a, NULL, h, C,
                                                           by_block, excl_v, 1);
          for (size_t si = 0; si < by_vreg[v].len; ++si) {
            const nyr_mach_live_segment_t *s =
                &a->segments[by_vreg[v].data[si]];
            if (s != hv && s->block < nb && !edge_block[s->block] &&
                reaches_h[s->block])
              evicted += block_color_eviction_count_excl(mach, a, s, 0, C,
                                                         by_block, excl_v, 1);
          }
          for (size_t k = 0; k < copy_len; ++k) {
            const loop_copy_t *c = &copies[k];
            uint32_t excl[2] = {v, c->src};
            evicted += block_color_eviction_count_excl(mach, a, c->ev, 0, C,
                                                       by_block, excl, 2);
            if (c->edge > h || c->src_dies_at_copy)
              evicted += block_color_eviction_count_excl(mach, a, c->eseg, 0, C,
                                                         by_block, excl, 2) +
                         block_color_eviction_count_excl(
                             mach, a, c->srcseg, 0, C, by_block, excl + 1, 1);
          }
          if (carry_debug && evicted)
            fprintf(stderr, "carry: h=%zu v=%u C=%d eviction cost %zu\n", h, v,
                    C, evicted);
          if (evicted < chosen_cost) {
            chosen_cost = evicted;
            chosen = C;
          }
        }
      }
      if (chosen < 0)
        continue;
      coalesced_here[v] = 1;
      uint32_t excl_v[1] = {v};
      block_evict_color_conflicts_excl(a, hv, chosen, by_block, excl_v, 1);
      block_evict_color_all_excl(a, h, chosen, by_block, excl_v, 1);
      for (size_t si = 0; si < by_vreg[v].len; ++si) {
        nyr_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
        if (s != hv && s->block < nb && !edge_block[s->block] &&
            reaches_h[s->block])
          block_evict_color_conflicts_excl(a, s, chosen, by_block, excl_v, 1);
      }
      for (size_t k = 0; k < copy_len; ++k) {
        loop_copy_t *c = &copies[k];
        bool preheader_copy = c->edge < h;
        uint32_t excl[2] = {v, c->src};
        block_evict_color_conflicts_excl(a, c->ev, chosen, by_block, excl, 2);
        bool recolor_source = !preheader_copy || c->src_dies_at_copy;
        if (recolor_source) {
          block_evict_color_conflicts_excl(a, c->eseg, chosen, by_block, excl,
                                           2);
          block_evict_color_conflicts_excl(a, c->srcseg, chosen, by_block,
                                           excl + 1, 1);
        }
        c->ev->color = chosen;
        if (recolor_source) {
          c->eseg->color = chosen;
          c->srcseg->color = chosen;
        }
      }
      hv->color = chosen;
      /*
       * The PHI and its edge sources now share `chosen`. Later loop-edge
       * coalescing must not evict them, even when the carry chain below
       * does not qualify: a split-off segment would leave the shared
       * register assumption of the edge copies without a resident value.
       */
      expensive_vreg[v] = 1;
      for (size_t k = 0; k < copy_len; ++k)
        if (copies[k].src < nv)
          expensive_vreg[copies[k].src] = 1;
      for (size_t si = 0; si < by_vreg[v].len; ++si) {
        nyr_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
        if (s == hv || s->block >= nb || edge_block[s->block] ||
            !reaches_h[s->block])
          continue;
        s->color = chosen;
      }
      /*
       * Complete the carry contract.  Every phi edge now shares one physical
       * color; require each edge copy source to actually materialize that
       * preg at its block end, then mark the loop-region chain carried so the
       * encoder keeps the value resident across the backedge instead of
       * homing at every boundary.  A carried segment whose block can hand the
       * value to a consumer outside the chain keeps spill=true: the encoder
       * homes it at the block-exit store, so outside consumers reload a
       * current home rather than a stale one.  Segments split around a fixed
       * clobber (division, variable shifts, calls) keep their spill/reload
       * pair — the canonical home stays authoritative across the barrier.
       * Very large generated functions can have thousands of dynamic
       * temporaries sharing the same GPR pool. Keep their canonical homes;
       * loop carries remain enabled for bounded functions where the edge
       * proof and allocator pressure oracle cover the full region.
       *
       * Nesting is a property of the natural loop body: the header plus the
       * blocks that reach one of its backedges without passing through it.
       * `reaches_h` also contains every block before the loop (and the rest
       * of an enclosing loop), so a sibling nest elsewhere in the outer loop
       * must not disable this carry.
       */
      bool nested_loop = false;
      {
        uint8_t *body = calloc(nb, sizeof(*body));
        size_t *body_work = body ? malloc(nb * sizeof(*body_work)) : NULL;
        if (!body || !body_work) {
          nested_loop = true;
        } else {
          size_t body_len = 0;
          body[h] = 1;
          for (size_t ei = 0; ei < edge_count; ++ei)
            if (edge_blocks[ei] > h && !body[edge_blocks[ei]]) {
              body[edge_blocks[ei]] = 1;
              body_work[body_len++] = edge_blocks[ei];
            }
          for (size_t wi = 0; wi < body_len; ++wi) {
            size_t block = body_work[wi];
            for (size_t pi = 0; pi < pred[block].len; ++pi) {
              size_t pb = pred[block].data[pi];
              if (pb < nb && !body[pb]) {
                body[pb] = 1;
                body_work[body_len++] = pb;
              }
            }
          }
          for (size_t b = 0; b < nb && !nested_loop; ++b)
            nested_loop = body[b] && loop_depth[b] > loop_depth[h];
        }
        free(body);
        free(body_work);
      }
      bool carry_chain_ok =
          copy_len > 0 && carry_enabled &&
          (reg_class == NYR_MACH_REGCLASS_VECTOR || mach->vreg_len <= 1024) &&
          !nested_loop && !loop_phi_has_call_use(mach, v) &&
          !loop_phi_region_has_call(mach, succ, reaches_h, nb, h, work);
      for (size_t k = 0; carry_chain_ok && k < copy_len; ++k)
        if (copies[k].edge > h && !copies[k].src_dies_at_copy &&
            !coalesce_preg_reaches_edge(mach, copies[k].srcseg,
                                        preserve_gpr_calls)) {
          carry_chain_ok = false;
        }
      for (size_t n = 0; carry_chain_ok && n < hb->inst_count; ++n) {
        const nyr_mach_inst_t *in = &mach->insts[hb->first_inst + n];
        if (in->opcode == NYR_MACH_RET &&
            in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg == v)
          carry_chain_ok = false; /* returns keep the canonical home */
      }
      if (carry_chain_ok) {
        uint8_t *carried_block = calloc(nb, sizeof(*carried_block));
        if (carried_block) {
          carried_block[h] = 1;
          for (size_t ei = 0; ei < edge_count; ++ei)
            carried_block[edge_blocks[ei]] = 1;
          hv->carried = true;
          hv->reload = false;
          hv->spill = false;
          hv->spill_reason = NYR_MACH_SPILL_NONE;
          for (size_t si = 0; si < by_vreg[v].len; ++si) {
            nyr_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
            if (s == hv || s->block >= nb || edge_block[s->block] ||
                !reaches_h[s->block])
              continue;
            if (s->spill_reason == NYR_MACH_SPILL_FIXED_CLOBBER)
              continue;
            s->carried = true;
            s->reload = false;
            s->spill = false;
            s->spill_reason = NYR_MACH_SPILL_NONE;
            carried_block[s->block] = 1;
          }
          for (size_t k = 0; k < copy_len; ++k) {
            loop_copy_t *c = &copies[k];
            /*
             * Only the phi destination flows around the loop; the copy
             * source keeps its own home-backed segments.  Carrying the
             * source's edge-block segment would claim its preg at latch
             * entry, where it still holds the destination's old value.
             */
            nyr_mach_live_segment_t *s = c->ev;
            if (s && s->block < nb &&
                s->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER) {
              s->carried = true;
              s->reload = false;
              s->spill = false;
              s->spill_reason = NYR_MACH_SPILL_NONE;
              carried_block[s->block] = 1;
            }
          }
          /*
           * Exit homing: a carried segment that ends its block keeps spill
           * when some successor's arrival segment of v is not carried — the
           * encoder then writes the preg home at the block-exit store and
           * the outside consumer reloads a current value.
           */
          for (size_t si = 0; si < by_vreg[v].len; ++si) {
            nyr_mach_live_segment_t *s = &a->segments[by_vreg[v].data[si]];
            if (!s->carried || s->block >= nb)
              continue;
            const nyr_mach_block_t *sb = &mach->blocks[s->block];
            if (!sb->inst_count)
              continue;
            size_t bend = sb->first_inst + sb->inst_count - 1;
            if (s->end != bend)
              continue;
            bool all_succ_carried = succ[s->block].len > 0;
            for (size_t qi = 0; all_succ_carried && qi < succ[s->block].len;
                 ++qi) {
              size_t q = succ[s->block].data[qi];
              if (q >= nb) {
                all_succ_carried = false;
                break;
              }
              /*
               * A successor where v is dead reads no home.
               */
              if (!nyr_mach_regalloc_live_in(a, q, v))
                continue;
              bool q_arrives_carried = false;
              for (size_t sj = 0; sj < by_vreg[v].len; ++sj) {
                const nyr_mach_live_segment_t *qs =
                    &a->segments[by_vreg[v].data[sj]];
                if (qs->block == q && qs->start == mach->blocks[q].first_inst &&
                    qs->carried) {
                  q_arrives_carried = true;
                  break;
                }
              }
              if (!q_arrives_carried)
                all_succ_carried = false;
            }
            if (!all_succ_carried)
              s->spill = true;
          }
          free(carried_block);
        } else {
          carry_chain_ok = false;
        }
      }
      if (carry_debug) {
        fprintf(stderr, "carry: h=%zu v=%u C=%d chain=%d nested=%d", h, v,
                chosen, (int)carry_chain_ok, (int)nested_loop);
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
    free(coalesced_here);
    free(edge_block);
    free(reaches_h);
    free(work);
  }

  for (size_t v = 0; v < nv; ++v)
    free(by_vreg[v].data);
  free(by_vreg);
  free(expensive_vreg);
  free(block_index.slots);
}

/*
 * The cross-block carry contract claims x's preg survives the CFG edge into
 * y. That holds only when the predecessor actually materialized the value in
 * that preg: a local def (commit), a local use (seed/reload), or a previously
 * proven carried arrival. A live-through segment with no local mention never
 * loads its preg at runtime — reloads materialize on first use — so carrying
 * its nominal color would make the successor read a stale register. The color
 * must also survive every fixed-register clobber crossed while the value
 * remains live; checking only the segment terminator misses calls earlier in
 * the block.
 */
static bool coalesce_preg_reaches_edge(const nyr_mach_func_t *mach,
                                       const nyr_mach_live_segment_t *x,
                                       bool preserve_gpr_calls) {
  if (!mach || !x || x->block >= mach->block_len)
    return false;
  const nyr_mach_block_t *b = &mach->blocks[x->block];
  bool mentioned = false;
  for (size_t n = 0; n < b->inst_count && !mentioned; ++n) {
    size_t pos = b->first_inst + n;
    /*
     * Only a mention inside this segment materializes ITS color. A def in
     * an earlier segment of the same block published a different preg, so
     * a live-through tail segment here must never carry.
     */
    if (pos < x->start)
      continue;
    if (pos > x->end)
      break;
    const nyr_mach_inst_t *in = &mach->insts[pos];
    uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
    size_t un = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
    for (size_t k = 0; k < un; ++k) {
      if (uses[k] == x->vreg) {
        mentioned = true;
        break;
      }
    }
    if (!mentioned && dst_is_def(in) && in->dst.as.reg == x->vreg)
      mentioned = true;
  }
  if (!mentioned)
    return false;
  if (x->color < 0 || x->color >= 64)
    return false;
  return segment_color_survives_clobbers(mach, x, NYR_MACH_REGCLASS_GPR,
                                         x->color, preserve_gpr_calls);
}

/*
 * True when no fixed-register barrier inside x's live interval clobbers
 * color. This is the single target-constraint check shared by the edge
 * reachability proof and the loop-carry recoloring candidates: a segment
 * recolored onto RCX must not span a variable shift, onto RDX a general
 * division, and so on (see inst_fixed_clobbers).
 */
static bool segment_color_survives_clobbers(const nyr_mach_func_t *mach,
                                            const nyr_mach_live_segment_t *x,
                                            nyr_mach_reg_class_t reg_class,
                                            int color,
                                            bool preserve_gpr_calls) {
  if (!mach || !x || color < 0 || color >= 64)
    return false;
  uint64_t color_mask = UINT64_C(1) << color;
  for (size_t pc = x->start; pc <= x->end && pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    uint64_t clobber =
        inst_fixed_clobbers(mach, in, reg_class, preserve_gpr_calls);
    if (!(clobber & color_mask))
      continue;
    bool live_after =
        pc < x->end ||
        (pc == x->end && x->spill_reason == NYR_MACH_SPILL_LIVE_OUT);
    if (!live_after)
      continue;
    /*
     * A destination defined by the clobbering instruction is produced after
     * the fixed-register operation and is not live through that barrier.
     */
    if (dst_is_def(in) && in->dst.as.reg == x->vreg)
      continue;
    return false;
  }
  return true;
}

static void coalesce_segments(const nyr_mach_func_t *mach,
                              nyr_mach_regalloc_t *a, const size_vec_t *succ,
                              const size_vec_t *pred,
                              const size_vec_t *by_block,
                              bool preserve_gpr_calls) {
  /*
   * Block membership is fixed after coloring, so the caller builds this
   * index once and shares it with the loop-edge coalescer.
   */
  const bool edge_debug = getenv("NY_CARRY_DEBUG") != NULL;
  uint8_t *expensive_vreg =
      calloc(a->vreg_len ? a->vreg_len : 1, sizeof(*expensive_vreg));
  if (expensive_vreg)
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const nyr_mach_inst_t *in = &mach->insts[i];
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < a->vreg_len &&
          (in->opcode == NYR_MACH_DIV || in->opcode == NYR_MACH_MOD))
        expensive_vreg[in->dst.as.reg] = 1;
    }

  /*
   * Sort by vreg so all segments of the same vreg are contiguous. The pass
   * considers neighboring segments for each value; CFG checks below decide
   * whether a physical-register carry across their blocks is legal. This
   * replaces the O(n^2) pairwise scan with O(n log n) sort + O(n) pass.
   */
  for (size_t i = 0; i + 1 < a->segment_len; ++i) {
    nyr_mach_live_segment_t *x = &a->segments[i];
    nyr_mach_live_segment_t *y = &a->segments[i + 1];
    if (x->vreg != y->vreg || x->color < 0 || y->color < 0)
      continue;
    if (x->block == y->block) {
      if (y->start != x->end + 1)
        continue;
      /*
       * A fixed-register instruction is a semantic home barrier.  Segment
       * construction deliberately ends the old range on the barrier with
       * spill=true and starts the successor with reload=true.  Coalescing
       * those adjacent chunks merely because they received the same color
       * would erase that contract; calls then reload stale stack homes after
       * the barrier.  Preserve the split even when the physical color is
       * identical.
       */
      nyr_mach_reg_class_t cls =
          (mach->vreg_classes && x->vreg < mach->vreg_len)
              ? mach->vreg_classes[x->vreg]
              : NYR_MACH_REGCLASS_GPR;
      if (x->end < mach->inst_len &&
          inst_fixed_clobbers(mach, &mach->insts[x->end], cls,
                              preserve_gpr_calls) != 0)
        continue;
      if (segment_accepts_color(mach, y, a->color_count, x->color) &&
          !block_has_color_conflict(a, y, x->color, by_block)) {
        y->color = x->color;
        x->spill = false;
        x->spill_reason = NYR_MACH_SPILL_NONE;
        y->spill = false;
        y->spill_reason = NYR_MACH_SPILL_NONE;
        y->reload = false;
      }
      continue;
    }
    if (x->block >= mach->block_len || y->block >= mach->block_len ||
        y->block <= x->block || succ[x->block].len != 1 ||
        succ[x->block].data[0] != y->block || pred[y->block].len != 1 ||
        pred[y->block].data[0] != x->block)
      continue;
    const nyr_mach_block_t *xb = &mach->blocks[x->block];
    const nyr_mach_block_t *yb = &mach->blocks[y->block];
    /*
     * The CFG edge, not instruction-array adjacency, proves that x's preg
     * reaches y. Machine blocks are emitted in layout order, so unrelated
     * blocks can sit between a unique predecessor and successor in the
     * instruction array even though they do not execute on this edge.
     */
    if (x->end + 1 != xb->first_inst + xb->inst_count ||
        y->start != yb->first_inst)
      continue;
    bool class_legal = segment_accepts_color(mach, y, a->color_count, x->color);
    bool conflicts =
        class_legal && block_has_color_conflict(a, y, x->color, by_block);
    uint32_t excluded_vreg = x->vreg;
    bool evictable =
        conflicts && expensive_vreg &&
        block_color_conflicts_evictable_excl(a, y, x->color, by_block,
                                             expensive_vreg, &excluded_vreg, 1);
    uint32_t conflict_vreg = UINT32_MAX;
    if (conflicts && edge_debug && by_block && y->block < a->block_count) {
      const size_t *indices = by_block[y->block].data;
      size_t count = by_block[y->block].len;
      for (size_t k = 0; k < count; ++k) {
        const nyr_mach_live_segment_t *other = &a->segments[indices[k]];
        if (other != y && other->color == x->color &&
            !(other->end < y->start || y->end < other->start)) {
          conflict_vreg = other->vreg;
          break;
        }
      }
    }
    if (edge_debug) {
      char conflict_name[16];
      if (conflict_vreg == UINT32_MAX)
        snprintf(conflict_name, sizeof(conflict_name), "-");
      else
        snprintf(conflict_name, sizeof(conflict_name), "%u", conflict_vreg);
      fprintf(stderr,
              "carry-edge: v=%u B%u->B%u preg=%d class-legal=%s "
              "conflict-v=%s "
              "target-conflict=%s evictable=%s result=%s\n",
              x->vreg, x->block, y->block, x->color, class_legal ? "yes" : "no",
              conflict_name, conflicts ? "yes" : "no", evictable ? "yes" : "no",
              class_legal && (!conflicts || evictable) ? "carry" : "home");
    }
    if (class_legal && (!conflicts || evictable) &&
        coalesce_preg_reaches_edge(mach, x, preserve_gpr_calls)) {
      if (conflicts)
        block_evict_color_conflicts_excl(a, y, x->color, by_block,
                                         &excluded_vreg, 1);
      y->color = x->color;
      x->spill = false;
      x->spill_reason = NYR_MACH_SPILL_NONE;
      y->spill = false;
      y->spill_reason = NYR_MACH_SPILL_NONE;
      y->reload = false;
      /*
       * This unique CFG edge supplies the successor's only runtime entry
       * value. The same-color preg is the incoming contract; if a later
       * segment cannot carry it, the encoder synchronizes at that consumer.
       */
      y->carried = true;
    }
  }
  free(expensive_vreg);
}

/*
 * Keep a single-definition scalar in one register through acyclic CFG regions.
 * Unlike phi coalescing, this is an unchanged SSA value: every live incoming
 * edge already carries the same value. The fixed point starts at its sole
 * definition, so loop backedges cannot bootstrap an unproved carry.
 */
static void coalesce_unchanged_dag_values(
    const nyr_mach_func_t *mach, nyr_mach_regalloc_t *a, const size_vec_t *succ,
    const size_vec_t *pred, const size_vec_t *by_block,
    nyr_mach_reg_class_t reg_class, const size_t *loop_depth) {
  if (!mach || !a || !succ || !pred || !by_block || mach->block_len < 2 ||
      !a->segment_len || !loop_depth || reg_class != NYR_MACH_REGCLASS_GPR)
    return;
  /*
   * This pass is GPR-only; class is encoded per segment's type.
   */
  size_t nb = mach->block_len, nv = a->vreg_len;
  size_t *def_count = calloc(nv ? nv : 1, sizeof(*def_count));
  size_t *def_block = malloc((nv ? nv : 1) * sizeof(*def_block));
  uint8_t *reachable = calloc(nb ? nb : 1, sizeof(*reachable));
  uint8_t *carried = calloc(nb ? nb : 1, sizeof(*carried));
  size_t *work = malloc((nb ? nb : 1) * sizeof(*work));
  if (!def_count || !def_block || !reachable || !carried || !work) {
    free(def_count);
    free(def_block);
    free(reachable);
    free(carried);
    free(work);
    return;
  }
  for (size_t v = 0; v < nv; ++v)
    def_block[v] = SIZE_MAX;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (dst_is_def(in) && in->dst.as.reg < nv) {
      uint32_t v = in->dst.as.reg;
      ++def_count[v];
      /*
       * The block index is recovered below from the ordered block ranges.
       */
      for (size_t b = 0; b < nb; ++b)
        if (i >= mach->blocks[b].first_inst &&
            i < mach->blocks[b].first_inst + mach->blocks[b].inst_count) {
          def_block[v] = b;
          break;
        }
    }
  }

  size_t words = a->bit_words;
  for (uint32_t v = 0; v < nv; ++v) {
    if (def_count[v] != 1 || def_block[v] >= nb ||
        !is_alloc_type(mach->vreg_types[v], NYR_MACH_REGCLASS_GPR) ||
        loop_depth[def_block[v]] != 0 ||
        bit_get(a->live_in + def_block[v] * words, v))
      continue;
    memset(reachable, 0, nb * sizeof(*reachable));
    memset(carried, 0, nb * sizeof(*carried));

    /*
     * Collect only blocks where this definition remains live.
     */
    size_t work_len = 1;
    reachable[def_block[v]] = true;
    work[0] = def_block[v];
    bool valid = true;
    /*
     * Block-entry carry is only valid when the encoder can preserve this
     * preg across every instruction on the live path. Calls use the
     * canonical homes as their ABI boundary and invalidate scalar residency;
     * the ordinary allocator has the complete call-clobber handling. Keep
     * this optimization on call-free live regions until that boundary has
     * an explicit per-edge register-preservation contract.
     */
    for (size_t wi = 0; wi < work_len && valid; ++wi) {
      size_t b = work[wi];
      /*
       * A value defined outside loops can still be live into and through a
       * loop. This pass only proves acyclic edge residency; loop edges need
       * the dedicated PHI carry analysis below.
       */
      if (loop_depth[b] != 0) {
        valid = false;
        break;
      }
      bool unique = false;
      nyr_mach_live_segment_t *seg =
          segment_in_block_vreg(a, v, b, by_block, &unique);
      if (!seg || !unique ||
          seg->spill_reason == NYR_MACH_SPILL_FIXED_CLOBBER) {
        valid = false;
        break;
      }
      const nyr_mach_block_t *block = &mach->blocks[b];
      for (size_t ii = 0; ii < block->inst_count; ++ii) {
        const nyr_mach_inst_t *in = &mach->insts[block->first_inst + ii];
        if (in->opcode == NYR_MACH_CALL) {
          valid = false;
          break;
        }
      }
      if (!valid)
        break;
      for (size_t si = 0; si < succ[b].len; ++si) {
        size_t q = succ[b].data[si];
        if (q >= nb || !bit_get(a->live_in + q * words, v))
          continue;
        if (!reachable[q]) {
          reachable[q] = true;
          work[work_len++] = q;
        }
      }
    }
    if (!valid)
      continue;

    /*
     * A join is carryable only after every live incoming edge is proven.
     */
    carried[def_block[v]] = true;
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = 0; b < nb; ++b) {
        if (!reachable[b] || carried[b] || !bit_get(a->live_in + b * words, v))
          continue;
        bool all_incoming = pred[b].len != 0;
        for (size_t pi = 0; all_incoming && pi < pred[b].len; ++pi) {
          size_t p = pred[b].data[pi];
          if (p >= nb || !bit_get(a->live_out + p * words, v) || !carried[p])
            all_incoming = false;
        }
        if (!all_incoming)
          continue;
        bool unique = false;
        nyr_mach_live_segment_t *seg =
            segment_in_block_vreg(a, v, b, by_block, &unique);
        if (seg && unique && seg->start == mach->blocks[b].first_inst &&
            seg->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER) {
          carried[b] = true;
          changed = true;
        }
      }
    }

    /*
     * Require every reachable value segment to accept the same register.
     */
    int candidates[64];
    size_t candidate_len = 0;
    bool unique = false;
    nyr_mach_live_segment_t *origin =
        segment_in_block_vreg(a, v, def_block[v], by_block, &unique);
    if (!origin || !unique)
      continue;
    if (origin->color >= 0)
      candidates[candidate_len++] = origin->color;
    for (size_t c = 0; c < a->color_count && candidate_len < 64; ++c) {
      bool seen = false;
      for (size_t k = 0; k < candidate_len; ++k)
        seen |= candidates[k] == (int)c;
      if (!seen)
        candidates[candidate_len++] = (int)c;
    }
    int chosen = -1;
    for (size_t ci = 0; ci < candidate_len && chosen < 0; ++ci) {
      int color = candidates[ci];
      bool safe = true;
      for (size_t b = 0; safe && b < nb; ++b) {
        if (!reachable[b])
          continue;
        bool seg_unique = false;
        nyr_mach_live_segment_t *seg =
            segment_in_block_vreg(a, v, b, by_block, &seg_unique);
        safe = seg && seg_unique &&
               seg->spill_reason != NYR_MACH_SPILL_FIXED_CLOBBER &&
               segment_accepts_color(mach, seg, a->color_count, color) &&
               segment_color_survives_clobbers(mach, seg, reg_class, color,
                                               false) &&
               !block_has_color_conflict(a, seg, color, by_block);
      }
      if (safe)
        chosen = color;
    }
    if (chosen < 0)
      continue;

    for (size_t b = 0; b < nb; ++b) {
      if (!reachable[b])
        continue;
      bool seg_unique = false;
      nyr_mach_live_segment_t *seg =
          segment_in_block_vreg(a, v, b, by_block, &seg_unique);
      if (!seg || !seg_unique)
        continue;
      seg->color = chosen;
      if (carried[b] && b != def_block[v]) {
        seg->carried = true;
        seg->acyclic_carried = true;
        seg->reload = false;
        seg->spill = false;
        seg->spill_reason = NYR_MACH_SPILL_NONE;
      }
    }
    /*
     * Publish a home only where some live successor cannot accept the carry.
     */
    for (size_t b = 0; b < nb; ++b) {
      if (!reachable[b])
        continue;
      bool seg_unique = false;
      nyr_mach_live_segment_t *seg =
          segment_in_block_vreg(a, v, b, by_block, &seg_unique);
      if (!seg || !seg_unique || !bit_get(a->live_out + b * words, v))
        continue;
      bool all_successors_carried = true;
      for (size_t si = 0; si < succ[b].len; ++si) {
        size_t q = succ[b].data[si];
        if (q < nb && bit_get(a->live_in + q * words, v) && !carried[q]) {
          all_successors_carried = false;
          break;
        }
      }
      if (!all_successors_carried)
        seg->spill = true;
      else
        seg->spill = false;
    }
  }
  free(def_count);
  free(def_block);
  free(reachable);
  free(carried);
  free(work);
}

/*
 * Loop-invariant vector values (broadcasts, constants) live through a loop
 * without a definition inside it. Their per-block segments otherwise reload
 * the canonical home at every block entry, i.e. once per iteration. When
 * every segment of such a value in the natural loop body is its block's only
 * segment of the value, spans the whole block in one common color C, and
 * every loop entry edge leaves a preheader whose segment materializes the
 * value in C, the register itself is valid on every loop edge: nothing in the
 * body defines the value, and no other body segment can hold C while the
 * value is live across the entire body. Mark the body segments carried so the
 * encoder seeds C at block entry. Call-free bodies only.
 */
static bool preheader_align_vector_color(
    const nyr_mach_func_t *mach, nyr_mach_regalloc_t *a, size_t block,
    nyr_mach_live_segment_t *ps, int color, const size_vec_t *by_block,
    const uint8_t *locked_vreg, size_t nv, bool preserve_gpr_calls) {
  if (!ps || ps->carried || ps->color == color)
    return ps && ps->color == color &&
           segment_color_survives_clobbers(mach, ps, NYR_MACH_REGCLASS_VECTOR,
                                           color, preserve_gpr_calls);
  if (!segment_accepts_color(mach, ps, a->color_count, color) ||
      !segment_color_survives_clobbers(mach, ps, NYR_MACH_REGCLASS_VECTOR,
                                       color, preserve_gpr_calls))
    return false;
  int old_color = ps->color;
  nyr_mach_live_segment_t *conflict = NULL;
  for (size_t k = 0; k < by_block[block].len; ++k) {
    nyr_mach_live_segment_t *s = &a->segments[by_block[block].data[k]];
    if (s == ps || s->color != color)
      continue;
    if (s->end < ps->start || ps->end < s->start)
      continue;
    if (s->carried || (s->vreg < nv && locked_vreg[s->vreg]))
      return false;
    if (conflict)
      return false;
    conflict = s;
  }
  if (!conflict) {
    ps->color = color;
    if (old_color < 0) {
      ps->spill_reason = NYR_MACH_SPILL_NONE;
      const nyr_mach_block_t *pb = &mach->blocks[block];
      ps->reload =
          ps->start == pb->first_inst && nyr_mach_regalloc_live_in(a, block, ps->vreg);
    }
    return true;
  }
  if (old_color < 0 ||
      !segment_accepts_color(mach, conflict, a->color_count, old_color) ||
      !segment_color_survives_clobbers(mach, conflict, NYR_MACH_REGCLASS_VECTOR,
                                       old_color, preserve_gpr_calls))
    return false;
  for (size_t k = 0; k < by_block[block].len; ++k) {
    const nyr_mach_live_segment_t *s = &a->segments[by_block[block].data[k]];
    if (s == ps || s == conflict || s->color != old_color)
      continue;
    if (!(s->end < conflict->start || conflict->end < s->start))
      return false;
  }
  conflict->color = old_color;
  ps->color = color;
  return true;
}

static void carry_loop_invariant_vectors(const nyr_mach_func_t *mach,
                                         nyr_mach_regalloc_t *a,
                                         const size_vec_t *succ,
                                         const size_vec_t *pred,
                                         const size_vec_t *by_block,
                                         bool preserve_gpr_calls) {
  const size_t nb = mach->block_len, nv = a->vreg_len;
  if (nb < 2 || !nv || a->block_count != nb)
    return;
  uint8_t *body = calloc(nb, 1);
  uint8_t *defined = calloc(nv, 1);
  uint8_t *used = calloc(nv, 1);
  uint32_t *def_count = calloc(nv, sizeof(*def_count));
  size_t *def_block = malloc(nv * sizeof(*def_block));
  size_t *work = malloc(nb * sizeof(*work));
  if (!body || !defined || !used || !def_count || !def_block || !work)
    goto done;
  for (uint32_t v = 0; v < nv; ++v)
    def_block[v] = SIZE_MAX;
  for (size_t b = 0; b < nb; ++b) {
    const nyr_mach_block_t *blk = &mach->blocks[b];
    for (size_t n = 0; n < blk->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[blk->first_inst + n];
      if (dst_is_def(in) && in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < nv) {
        def_count[in->dst.as.reg]++;
        def_block[in->dst.as.reg] = b;
      }
    }
  }
  for (size_t h = 0; h < nb; ++h) {
    bool backedge = false;
    for (size_t pi = 0; pi < pred[h].len; ++pi)
      backedge |= pred[h].data[pi] >= h;
    if (!backedge)
      continue;
    memset(body, 0, nb);
    size_t len = 0;
    body[h] = 1;
    for (size_t pi = 0; pi < pred[h].len; ++pi) {
      size_t p = pred[h].data[pi];
      if (p >= h && p < nb && !body[p]) {
        body[p] = 1;
        work[len++] = p;
      }
    }
    for (size_t wi = 0; wi < len; ++wi)
      for (size_t pi = 0; pi < pred[work[wi]].len; ++pi) {
        size_t p = pred[work[wi]].data[pi];
        if (p < nb && !body[p]) {
          body[p] = 1;
          work[len++] = p;
        }
      }
    /*
     * A body block reached from outside except through h is not natural.
     */
    bool natural = true, has_call = false;
    memset(defined, 0, nv);
    memset(used, 0, nv);
    for (size_t b = 0; b < nb && natural; ++b) {
      if (!body[b])
        continue;
      if (b != h)
        for (size_t pi = 0; pi < pred[b].len; ++pi)
          if (!body[pred[b].data[pi]])
            natural = false;
      const nyr_mach_block_t *blk = &mach->blocks[b];
      for (size_t n = 0; n < blk->inst_count; ++n) {
        const nyr_mach_inst_t *in = &mach->insts[blk->first_inst + n];
        if (in->opcode == NYR_MACH_CALL)
          has_call = true;
        if (dst_is_def(in) && in->dst.kind == NYR_MACH_OPERAND_VREG &&
            in->dst.as.reg < nv)
          defined[in->dst.as.reg] = 1;
        uint32_t u[3 + NYIR_CALL_MAX_ARGS];
        size_t un = inst_uses(in, u, sizeof(u) / sizeof(*u));
        for (size_t ui = 0; ui < un; ++ui)
          if (u[ui] < nv)
            used[u[ui]] = 1;
      }
    }
    if (!natural || has_call)
      continue;
    for (uint32_t v = 0; v < nv; ++v) {
      if (defined[v] || !used[v] || !nyr_mach_regalloc_live_in(a, h, v))
        continue;
      int color = -1;
      bool ok = true;
      for (size_t b = 0; b < nb && ok; ++b) {
        if (!body[b])
          continue;
        bool unique = false;
        nyr_mach_live_segment_t *seg =
            segment_in_block_vreg(a, v, b, by_block, &unique);
        const nyr_mach_block_t *blk = &mach->blocks[b];
        size_t last = blk->inst_count ? blk->first_inst + blk->inst_count - 1
                                      : blk->first_inst;
        if (!seg || !unique || seg->color < 0 ||
            seg->start != blk->first_inst || seg->end != last ||
            !nyr_mach_regalloc_live_in(a, b, v) ||
            seg->spill_reason == NYR_MACH_SPILL_FIXED_CLOBBER ||
            (color >= 0 && seg->color != color) ||
            !segment_color_survives_clobbers(mach, seg,
                                             NYR_MACH_REGCLASS_VECTOR,
                                             seg->color, preserve_gpr_calls)) {
          ok = false;
          break;
        }
        color = seg->color;
      }
      if (!ok || color < 0)
        continue;
      bool entry = false;
      for (size_t pi = 0; pi < pred[h].len && ok; ++pi) {
        size_t p = pred[h].data[pi];
        if (body[p])
          continue;
        entry = true;
        const nyr_mach_block_t *pb = &mach->blocks[p];
        if (!pb->inst_count) {
          ok = false;
          break;
        }
        size_t last = pb->first_inst + pb->inst_count - 1;
        nyr_mach_live_segment_t *ps = NULL;
        for (size_t k = 0; k < by_block[p].len; ++k) {
          nyr_mach_live_segment_t *cand =
              &a->segments[by_block[p].data[k]];
          if (cand->vreg == v && cand->end == last)
            ps = cand;
        }
        if (!ps || !segment_accepts_color(mach, ps, a->color_count, color) ||
            !segment_color_survives_clobbers(mach, ps, NYR_MACH_REGCLASS_VECTOR,
                                             color, preserve_gpr_calls))
          ok = false;
        else if (ps->color != color) {
          if (ps->carried ||
              !preheader_align_vector_color(mach, a, p, ps, color, by_block,
                                            used, nv, preserve_gpr_calls))
            ok = false;
        }
      }
      if (!ok || !entry)
        continue;
      bool def_outside =
          def_count[v] == 1 && def_block[v] < nb && !body[def_block[v]];
      if (def_outside) {
        nyr_mach_live_segment_t *dseg =
            segment_in_block_vreg(a, v, def_block[v], by_block, NULL);
        if (dseg && dseg->color >= 0)
          dseg->spill = true;
      }
      bool has_unpub_exit = false;
      if (!def_outside) {
        for (size_t b = 0; b < nb && !has_unpub_exit; ++b) {
          if (!body[b] || !nyr_mach_regalloc_live_out(a, b, v))
            continue;
          for (size_t si = 0; si < succ[b].len; ++si) {
            size_t q = succ[b].data[si];
            if (q < nb && !body[q] && nyr_mach_regalloc_live_in(a, q, v)) {
              has_unpub_exit = true;
              break;
            }
          }
        }
      }
      for (size_t b = 0; b < nb; ++b) {
        if (!body[b])
          continue;
        nyr_mach_live_segment_t *seg =
            segment_in_block_vreg(a, v, b, by_block, NULL);
        seg->carried = true;
        seg->reload = false;
        if (!has_unpub_exit)
          seg->spill = false;
      }
    }
  }
done:
  free(body);
  free(defined);
  free(used);
  free(def_count);
  free(def_block);
  free(work);
}

/*
 * Coalescing (loop carries above all) may evict a block-local temporary from
 * the color it received during linear scan, even when another color stays
 * free across its whole span. An evicted temporary costs a home store and a
 * reload on its use, often on a loop recurrence. Give such segments a second
 * chance: a segment that was register-only before coalescing (no reload, no
 * spill, not carried) takes any color that is legal, survives the fixed
 * clobbers it spans, and overlaps no segment of that color in its block.
 * Its pre-coalescing flags are restored, so its home is never touched.
 */
typedef struct {
  int color;
  bool local;
} regalloc_local_snapshot_t;

static regalloc_local_snapshot_t *
regalloc_local_snapshot(const nyr_mach_regalloc_t *a) {
  if (!a || !a->segment_len)
    return NULL;
  regalloc_local_snapshot_t *snap = malloc(a->segment_len * sizeof(*snap));
  if (!snap)
    return NULL;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *s = &a->segments[i];
    snap[i].color = s->color;
    snap[i].local = s->color >= 0 && !s->reload && !s->spill && !s->carried &&
                    !s->acyclic_carried &&
                    s->spill_reason == NYR_MACH_SPILL_NONE;
  }
  return snap;
}

static void recolor_evicted_locals(const nyr_mach_func_t *mach,
                                   nyr_mach_regalloc_t *a,
                                   const size_vec_t *by_block,
                                   const regalloc_local_snapshot_t *snap,
                                   nyr_mach_reg_class_t reg_class,
                                   bool preserve_gpr_calls) {
  for (size_t i = 0; i < a->segment_len; ++i) {
    nyr_mach_live_segment_t *s = &a->segments[i];
    if (!snap[i].local || s->color >= 0 ||
        s->spill_reason != NYR_MACH_SPILL_BLOCK_CONFLICT || s->carried ||
        s->acyclic_carried || s->block >= a->block_count)
      continue;
    int chosen = -1;
    for (size_t k = 0; k <= a->color_count && chosen < 0; ++k) {
      /*
       * Try the linear-scan color first, then the lowest free one.
       */
      int c = k == 0 ? snap[i].color : (int)(k - 1);
      if (c < 0 || (size_t)c >= a->color_count || (k > 0 && c == snap[i].color))
        continue;
      if (!segment_accepts_color(mach, s, a->color_count, c) ||
          !segment_color_survives_clobbers(mach, s, reg_class, c,
                                           preserve_gpr_calls) ||
          block_has_color_conflict(a, s, c, by_block))
        continue;
      chosen = c;
    }
    if (chosen < 0)
      continue;
    s->color = chosen;
    s->reload = false;
    s->spill = false;
    s->spill_reason = NYR_MACH_SPILL_NONE;
  }
}

/*
 * Set while rebuilding an allocation whose coalesced (carried) form failed
 * verification. The plain form keeps edge copies and reloads, so it is always
 * valid; losing it would send the whole register class to stack homes.
 */
static _Thread_local bool regalloc_retry_without_coalescing;

static bool nyr_mach_regalloc_build_class_mode_once(
    const nyr_mach_func_t *mach, nyr_mach_reg_class_t reg_class,
    size_t color_count, bool preserve_gpr_calls, bool allow_rhs_tie,
    nyr_mach_regalloc_t *out) {
  if (!mach || !out)
    return false;
  /*
   * Fixed-register descriptions below depend on well-formed operands and
   * unique, typed COPY definitions. Do not derive allocation barriers from
   * unchecked machine IR.
   */
  {
    char verr[256] = {0};
    if (!nyr_mach_verify(mach, 0, verr, sizeof(verr))) {
      if (getenv("NYTRIX_TRACE_REGALLOC_FAIL")) {
        fprintf(stderr, "regalloc: machine verify failed: %s\n", verr);
        if (getenv("NYTRIX_TRACE_REGALLOC_DUMP"))
          nyr_mach_dump(mach, stderr);
      }
      return false;
    }
  }
  ny_dbg_regalloc_batch_begin();
  nyr_mach_regalloc_t a = {.vreg_len = mach->vreg_len,
                           .color_count = color_count,
                           .block_count = mach->block_len,
                           .x64_rhs_tie = allow_rhs_tie};
  size_t nb = mach->block_len, nv = mach->vreg_len;
  size_t words = bit_words(nv ? nv : 1);
  size_t *next_clobber = NULL;
  size_t *loop_depth = NULL;
  size_vec_t *clobber_sites = NULL;
  size_t clobber_color_count = 0;
  a.bit_words = words;
  size_t *use = calloc((nb ? nb : 1) * words, sizeof(size_t));
  size_t *def = calloc((nb ? nb : 1) * words, sizeof(size_t));
  size_t *first = NULL;
  size_t *last = NULL;
  bool *first_use = NULL;
  bool *seen_def = NULL;
  bool *in_block = NULL;
  size_vec_t *block_values = NULL;
  a.live_in = calloc((nb ? nb : 1) * words, sizeof(size_t));
  a.live_out = calloc((nb ? nb : 1) * words, sizeof(size_t));
  size_vec_t *succ = calloc(nb ? nb : 1, sizeof(*succ));
  size_vec_t *pred = calloc(nb ? nb : 1, sizeof(*pred));
  if (!use || !def || !a.live_in || !a.live_out || !succ || !pred)
    goto fail;

  for (size_t bi = 0; bi < nb; ++bi) {
    const nyr_mach_block_t *b = &mach->blocks[bi];
    bool falls = true;
    for (size_t n = 0; n < b->inst_count; ++n) {
      const nyr_mach_inst_t *in = &mach->insts[b->first_inst + n];
      uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
      size_t un = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
      for (size_t k = 0; k < un; ++k) {
        uint32_t v = uses[k];
        if (v < nv && !bit_get(def + bi * words, v))
          bit_set(use + bi * words, v);
      }
      if (dst_is_def(in) && in->dst.as.reg < nv)
        bit_set(def + bi * words, in->dst.as.reg);
      if (in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) {
        uint32_t t = in->src1.as.block_index;
        if (t < nb && !size_vec_push(&succ[bi], t))
          goto fail;
        if (in->opcode == NYR_MACH_BR)
          falls = false;
      } else if (in->opcode == NYR_MACH_RET) {
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
        size_t inw = use[bi * words + w] | (outw & ~def[bi * words + w]);
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
  in_block = calloc(nv ? nv : 1, sizeof(*in_block));
  block_values = calloc(nb ? nb : 1, sizeof(*block_values));
  if (!first || !last || !first_use || !seen_def || !in_block || !block_values)
    goto fail;
  next_clobber =
      build_next_clobber(mach, reg_class, preserve_gpr_calls, color_count,
                         &clobber_sites, &clobber_color_count);
  if (!next_clobber)
    goto fail;
  for (size_t bi = 0; bi < nb; ++bi) {
    const nyr_mach_block_t *b = &mach->blocks[bi];
    size_t bend =
        b->inst_count ? b->first_inst + b->inst_count - 1 : b->first_inst;
    size_vec_t *values = &block_values[bi];
    for (size_t n = 0; n < b->inst_count; ++n) {
      size_t pos = b->first_inst + n;
      const nyr_mach_inst_t *in = &mach->insts[pos];
      uint32_t uses[3 + NYIR_CALL_MAX_ARGS];
      size_t un = inst_uses(in, uses, sizeof(uses) / sizeof(*uses));
      for (size_t k = 0; k < un; ++k) {
        uint32_t v = uses[k];
        if (v >= nv || !is_alloc_type(mach->vreg_types[v], reg_class))
          continue;
        if (!regalloc_touch_vreg(values, in_block, first, last, first_use,
                                 seen_def, v))
          goto fail;
        if (first[v] == SIZE_MAX) {
          first[v] = pos;
          first_use[v] = !seen_def[v];
        }
        last[v] = pos;
      }
      if (dst_is_def(in)) {
        uint32_t v = in->dst.as.reg;
        if (v < nv && is_alloc_type(mach->vreg_types[v], reg_class)) {
          if (!regalloc_touch_vreg(values, in_block, first, last, first_use,
                                   seen_def, v))
            goto fail;
          if (first[v] == SIZE_MAX)
            first[v] = pos;
          last[v] = pos;
          seen_def[v] = true;
        }
      }
    }
    /*
     * Values may be live through a block without a local use/def. Add those
     * sparse live-set members to the block's candidate list, then build
     * intervals only for values that can actually receive a segment here.
     * The previous full-vreg scans per block made large straight-line
     * functions with assertion branches quadratic in blocks × vregs.
     */
    const size_t word_bits = sizeof(size_t) * 8;
    for (size_t w = 0; w < words; ++w) {
      size_t bits = a.live_in[bi * words + w] | a.live_out[bi * words + w];
      while (bits) {
#if SIZE_MAX == UINT64_MAX
        size_t bit = (size_t)__builtin_ctzll((unsigned long long)bits);
#else
        size_t bit = (size_t)__builtin_ctzl((unsigned long)bits);
#endif
        size_t v = w * word_bits + bit;
        if (v < nv && is_alloc_type(mach->vreg_types[v], reg_class) &&
            !regalloc_touch_vreg(values, in_block, first, last, first_use,
                                 seen_def, (uint32_t)v))
          goto fail;
        bits &= bits - 1;
      }
    }
    for (size_t vi = 0; vi < values->len; ++vi) {
      uint32_t v = (uint32_t)values->data[vi];
      bool lin = bit_get(a.live_in + bi * words, v);
      bool lout = bit_get(a.live_out + bi * words, v);
      if (first[v] == SIZE_MAX && !lin && !lout)
        continue;
      size_t start = first[v] == SIZE_MAX ? b->first_inst : first[v];
      size_t end = first[v] == SIZE_MAX ? bend : last[v];
      if (lin)
        start = b->first_inst;
      if (lout)
        end = bend;
      bool reload = lin || first_use[v];
      for (size_t s = start; s <= end;) {
        size_t e = end;
        bool clobber = false;
        size_t pc = s < mach->inst_len ? next_clobber[s] : SIZE_MAX;
        if (pc <= end) {
          const nyr_mach_inst_t *in = &mach->insts[pc];
          /*
           * A fixed-register instruction clobbers values live through it,
           * but not its own result. For example, DIV produces its quotient
           * in RAX after consuming RAX/RDX; the encoder can then move that
           * result directly into an allocated callee-saved color. Marking
           * this definition as a clobber boundary forces an unnecessary
           * stack home and reload on every execution of the following loop
           * body. A value first defined here has no pre-instruction value
           * that needs to survive the clobber.
           */
          bool defined_at_clobber = dst_is_def(in) && in->dst.as.reg == v &&
                                    first[v] == pc && !first_use[v];
          if (defined_at_clobber)
            pc = pc + 1 < mach->inst_len ? next_clobber[pc + 1] : SIZE_MAX;
          if (pc <= end) {
            e = pc;
            clobber = true;
          }
        }
        bool more = e < end;
        /*
         * A clobber is itself part of the old segment: its inputs must be
         * available before the fixed-register sequence runs.  The following
         * segment starts with reload=true, making the canonical home the
         * only legal source after the sequence.
         */
        if (!append_segment(&a, v, (uint32_t)bi, s, e, reload,
                            more || lout || clobber))
          goto fail;
        nyr_mach_live_segment_t *segment = &a.segments[a.segment_len - 1];
        if (clobber)
          segment->spill_reason = NYR_MACH_SPILL_FIXED_CLOBBER;
        else if (lout)
          segment->spill_reason = NYR_MACH_SPILL_LIVE_OUT;
        else if (more)
          segment->spill_reason = NYR_MACH_SPILL_SEGMENT_BOUND;
        reload = true;
        if (e == SIZE_MAX)
          break;
        s = e + 1;
      }
      in_block[v] = false;
    }
    values->len = 0;
  }
  free(first);
  free(last);
  free(first_use);
  free(seen_def);
  first = last = NULL;
  first_use = seen_def = NULL;
  free(in_block);
  in_block = NULL;
  for (size_t bi = 0; bi < nb; ++bi)
    free(block_values[bi].data);
  free(block_values);
  block_values = NULL;
  free(next_clobber);
  next_clobber = NULL;

  loop_depth = compute_loop_depth(nb, succ);
  bool colored =
      color_segments(mach, &a, loop_depth, reg_class, preserve_gpr_calls,
                     allow_rhs_tie, clobber_sites, clobber_color_count);
  free_clobber_sites(clobber_sites, clobber_color_count);
  clobber_sites = NULL;
  clobber_color_count = 0;
  if (!colored) {
    free(loop_depth);
    loop_depth = NULL;
    goto fail;
  }
  /*
   * The block index stores segment-array indices.  Establish the final
   * vreg ordering before building it; sorting afterward would make every
   * conflict lookup address a different segment and could coalesce
   * overlapping live ranges onto the same physical register.
   */
  if (a.segment_len > 1)
    qsort(a.segments, a.segment_len, sizeof(*a.segments), cmp_seg_vreg);
  size_vec_t *by_block = build_segment_by_block(mach, &a);
  regalloc_local_snapshot_t *local_snapshot =
      reg_class == NYR_MACH_REGCLASS_VECTOR ? regalloc_local_snapshot(&a)
                                            : NULL;
  if (!regalloc_retry_without_coalescing) {
    coalesce_segments(mach, &a, succ, pred, by_block, preserve_gpr_calls);
    coalesce_unchanged_dag_values(mach, &a, succ, pred, by_block, reg_class,
                                  loop_depth);
    coalesce_loop_edges(mach, &a, succ, pred, by_block, loop_depth,
                        preserve_gpr_calls, reg_class);
  }
  if (local_snapshot && by_block)
    recolor_evicted_locals(mach, &a, by_block, local_snapshot, reg_class,
                           preserve_gpr_calls);
  if (reg_class == NYR_MACH_REGCLASS_VECTOR && by_block &&
      !regalloc_retry_without_coalescing && loop_phi_carry_enabled(reg_class))
    carry_loop_invariant_vectors(mach, &a, succ, pred, by_block,
                                 preserve_gpr_calls);
  free(local_snapshot);
  free_segment_by_block(mach, by_block);
  by_block = NULL;
  free(loop_depth);
  if (a.segment_len > 1)
    qsort(a.segments, a.segment_len, sizeof(*a.segments), cmp_seg_vreg);
  if (!regalloc_verify_mode(mach, &a, reg_class, allow_rhs_tie))
    goto fail;
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

  if (ny_dbg_is_enabled(NY_DBG_REGALLOC)) {
    size_t home_total = 0, home_reported = 0;
    for (size_t bi = 0; bi < nb; ++bi) {
      size_t live_in = 0, live_out = 0, spills = 0, reloads = 0, peak = 0;
      for (size_t v = 0; v < nv; ++v) {
        live_in += bit_get(a.live_in + bi * words, v) ? 1u : 0u;
        live_out += bit_get(a.live_out + bi * words, v) ? 1u : 0u;
      }
      const nyr_mach_block_t *block = &mach->blocks[bi];
      for (size_t pc = block->first_inst;
           pc < block->first_inst + block->inst_count; ++pc) {
        size_t at_pc = 0;
        for (size_t si2 = 0; si2 < a.segment_len; ++si2) {
          const nyr_mach_live_segment_t *seg = &a.segments[si2];
          if (seg->block != bi)
            continue;
          if (seg->spill && seg->start == pc)
            ++spills;
          if (seg->reload && seg->start == pc)
            ++reloads;
          if (seg->start <= pc && pc <= seg->end)
            ++at_pc;
        }
        if (at_pc > peak)
          peak = at_pc;
      }
      ny_dbg_regalloc_block(bi, peak, live_in, live_out, spills, reloads);
      for (size_t si2 = 0; si2 < a.segment_len; ++si2) {
        const nyr_mach_live_segment_t *seg = &a.segments[si2];
        if (seg->block != bi || !seg->spill)
          continue;
        const char *reason = "unknown";
        switch (seg->spill_reason) {
        case NYR_MACH_SPILL_FIXED_CLOBBER:
          reason = "fixed-register clobber";
          break;
        case NYR_MACH_SPILL_LIVE_OUT:
          reason = "live across block boundary";
          break;
        case NYR_MACH_SPILL_SEGMENT_BOUND:
          reason = "bounded live segment";
          break;
        case NYR_MACH_SPILL_REGISTER_PRESSURE:
          reason = "register pressure";
          break;
        case NYR_MACH_SPILL_BLOCK_CONFLICT:
          reason = "block allocation conflict";
          break;
        case NYR_MACH_SPILL_NONE:
          break;
        }
        const char *barrier = "-";
        bool call_barrier = false;
        if (seg->spill_reason == NYR_MACH_SPILL_FIXED_CLOBBER &&
            seg->end < mach->inst_len) {
          const nyr_mach_inst_t *at = &mach->insts[seg->end];
          if (at->opcode == NYR_MACH_CALL &&
              at->src0.kind == NYR_MACH_OPERAND_SYMBOL && at->src0.as.symbol) {
            barrier = at->src0.as.symbol;
            call_barrier = true;
          } else if (at->opcode == NYR_MACH_CALL) {
            barrier = "indirect call";
            call_barrier = true;
          } else {
            barrier = "fixed-register instruction";
          }
        }
        const char *cause = reason;
        if (call_barrier)
          cause = "call-clobber boundary";
        /*
         * Home-store records are per-vreg-per-segment and dominate -vvvv
         * output (6k+ lines for a small program). Keep the full record only
         * for call-clobber boundaries (the interesting spill reasons);
         * everything else aggregates into the per-block count below.
         */
        home_total++;
        /*
         * Call-clobber home stores are the flood: one per vreg per call in
         * range. They are routine (the ABI forces the spill); report only
         * genuine pressure/conflict spills individually and fold the rest
         * into the per-function count.
         */
        bool routine_call_spill =
            call_barrier || seg->spill_reason == NYR_MACH_SPILL_FIXED_CLOBBER;
        if (routine_call_spill || seg->spill_reason == NYR_MACH_SPILL_NONE ||
            seg->spill_reason == NYR_MACH_SPILL_LIVE_OUT ||
            seg->spill_reason == NYR_MACH_SPILL_SEGMENT_BOUND)
          continue;
        home_reported++;
        ny_dbg_heuristic_event("REGALLOC_HOME",
                               "store v%u B%zu i%zu..%zu preg=%d barrier=%s "
                               "reload_after=%s cause=%s",
                               seg->vreg, bi, seg->start, seg->end, seg->color,
                               barrier, seg->reload ? "yes" : "no", cause);
      }
    }
    if (home_total > home_reported)
      ny_dbg_heuristic_event("REGALLOC_HOME",
                             "%zu home stores withheld (routine; "
                             "call-clobber/spill records shown)",
                             home_total - home_reported);
  }
  if (ny_dbg_is_enabled(NY_DBG_MEM)) {
    size_t frame_slots = mach->frame_slot_len;
    size_t frame_bytes = (frame_slots + mach->vreg_len) * 8u;
    frame_bytes = (frame_bytes + 15u) & ~(size_t)15u;
    ny_dbg_mem_layout("machine frame", 0, frame_bytes, 16,
                      "estimated bytes: source frame slots plus vreg homes");
    ny_dbg_mem_layout("source slots", 0, frame_slots * 8u, 8,
                      "frame-slot region");
    ny_dbg_mem_layout(
        "vreg home area", frame_slots * 8u, mach->vreg_len * 8u, 8,
        "reserved canonical homes; stores and reloads are listed per block");
    /*
     * Per-vreg home map: one row per colored vreg with its canonical home
     * offset (frame_slots*8 + vreg*8, 8 bytes) and machine type, so the
     * machine-code frame is as inspectable as the NYIR frame map.
     */
    for (size_t v = 0; v < mach->vreg_len; ++v) {
      if (!mach->vreg_types || mach->vreg_types[v] == NYR_MACH_TYPE_NONE)
        continue;
      char row[64], detail[128];
      snprintf(row, sizeof(row), "v%u home", (unsigned)v);
      const char *ty = mach->vreg_types[v] == NYR_MACH_TYPE_F64        ? "f64"
                       : mach->vreg_types[v] == NYR_MACH_TYPE_V128_I64 ? "v128"
                       : mach->vreg_types[v] == NYR_MACH_TYPE_V256_I64 ? "v256"
                                                                       : "i64";
      snprintf(detail, sizeof(detail), "rbp-relative=-%zu 8-byte word type=%s",
               (frame_slots + (size_t)v) * 8u + 8u, ty);
      ny_dbg_mem_layout(row, (frame_slots + (size_t)v) * 8u + 8u, 8, 8, detail);
    }
  }

  for (size_t b = 0; b < nb; ++b) {
    free(succ[b].data);
    free(pred[b].data);
  }
  free(succ);
  free(pred);
  free(use);
  free(def);
  ny_dbg_regalloc_batch_end();
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
  free(next_clobber);
  free_clobber_sites(clobber_sites, clobber_color_count);
  free(in_block);
  if (block_values)
    for (size_t bi = 0; bi < nb; ++bi)
      free(block_values[bi].data);
  free(block_values);
  ny_dbg_regalloc_batch_end();
  nyr_mach_regalloc_free(&a);
  return false;
}

static bool nyr_mach_regalloc_build_class_mode(const nyr_mach_func_t *mach,
                                               nyr_mach_reg_class_t reg_class,
                                               size_t color_count,
                                               bool preserve_gpr_calls,
                                               bool allow_rhs_tie,
                                               nyr_mach_regalloc_t *out) {
  if (nyr_mach_regalloc_build_class_mode_once(mach, reg_class, color_count,
                                              preserve_gpr_calls,
                                              allow_rhs_tie, out))
    return true;
  if (!mach || !out || regalloc_retry_without_coalescing)
    return false;
  regalloc_retry_without_coalescing = true;
  bool ok = nyr_mach_regalloc_build_class_mode_once(
      mach, reg_class, color_count, preserve_gpr_calls, allow_rhs_tie, out);
  regalloc_retry_without_coalescing = false;
  return ok;
}

bool nyr_mach_regalloc_build_class(const nyr_mach_func_t *mach,
                                   nyr_mach_reg_class_t reg_class,
                                   size_t color_count,
                                   nyr_mach_regalloc_t *out) {
  return nyr_mach_regalloc_build_class_mode(mach, reg_class, color_count, false,
                                            false, out);
}

/*
 * A carried segment promises that its register already holds the value at
 * block entry. The allocator can emit one whose predecessor ends in a
 * stack-resident segment (spilled under register pressure), so the register
 * holds some other value there. Turn such a segment into an ordinary reload
 * from the home the predecessor wrote; later segments carried from it stay
 * valid. Returns the number of segments repaired.
 */
size_t nyr_mach_regalloc_repair_carried(const nyr_mach_func_t *mach,
                                        nyr_mach_regalloc_t *a) {
  if (!mach || !a || !a->segment_len || a->block_count != mach->block_len ||
      !a->live_in || !a->live_out)
    return 0;
  size_t *pred_offsets = NULL;
  uint32_t *preds = NULL;
  if (!verify_build_predecessors(mach, &pred_offsets, &preds))
    return 0;
  size_t repaired = 0;
  bool *lazy = calloc(a->segment_len, sizeof(*lazy));
  if (!lazy) {
    free(pred_offsets);
    free(preds);
    return 0;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < a->segment_len; ++i) {
      nyr_mach_live_segment_t *s = &a->segments[i];
      if (!s->carried || s->color < 0 || s->block >= mach->block_len ||
          !nyr_mach_regalloc_live_in(a, s->block, s->vreg))
        continue;
      bool stack_pred = false;
      for (size_t edge = pred_offsets[s->block];
           edge < pred_offsets[s->block + 1]; ++edge) {
        size_t pred = preds[edge];
        if (!nyr_mach_regalloc_live_out(a, pred, s->vreg))
          continue;
        const nyr_mach_block_t *source = &mach->blocks[pred];
        if (!source->inst_count)
          continue;
        const nyr_mach_live_segment_t *from = nyr_mach_regalloc_segment_at(
            a, s->vreg, (size_t)source->first_inst + source->inst_count - 1);
        /*
         * A repaired segment loads its home only at its first use, so a
         * segment carried from it would see an unloaded register.
         */
        if (from && (from->color < 0 || lazy[from - a->segments]))
          stack_pred = true;
      }
      if (!stack_pred)
        continue;
      s->carried = false;
      s->acyclic_carried = false;
      s->reload = true;
      lazy[i] = true;
      ++repaired;
      changed = true;
    }
  }
  free(lazy);
  free(pred_offsets);
  free(preds);
  return repaired;
}

bool nyr_mach_regalloc_build_x64_class(const nyr_mach_func_t *mach,
                                       nyr_mach_reg_class_t reg_class,
                                       size_t color_count,
                                       nyr_mach_regalloc_t *out) {
  /*
   * x64 colors 0..4 are the ABI callee-saved bank (rbx, r12-r15); the
   * encoder pushes every used color from that bank in the prologue. Calls
   * therefore split only the caller-saved colors, not the whole GPR file.
   * A clobber-less MIR CALL otherwise falls back to UINT64_MAX and homes
   * every live value at the call, which is especially costly in recursive
   * functions like moves() and solve(). Explicit call_clobbers are ORed in
   * so a target-specific barrier can still widen the mask.
   */
  bool preserve_gpr_calls = reg_class == NYR_MACH_REGCLASS_GPR;
  return nyr_mach_regalloc_build_class_mode(mach, reg_class, color_count,
                                            preserve_gpr_calls, true, out);
}

/*
 * An encoder that reserves colors after allocation (x86-64 loop registers)
 * evicts the ordinary segments that held them. A block-local segment, one
 * defined and dying inside its block with no home traffic, has no cross-block
 * contract, so it can take any other color that is free over its interval.
 * `candidate[i]` marks such evicted segments; `color_blocked` reports the
 * encoder's own reservations. Returns the number of segments recolored.
 */
size_t nyr_mach_regalloc_recolor_locals(
    const nyr_mach_func_t *mach, nyr_mach_regalloc_t *a,
    nyr_mach_reg_class_t reg_class, const unsigned char *candidate,
    bool (*color_blocked)(void *ctx, int color, size_t start, size_t end),
    void *ctx) {
  if (!mach || !a || !candidate || !a->segment_len || !a->color_count)
    return 0;
  size_vec_t *by_block = build_segment_by_block(mach, a);
  if (!by_block)
    return 0;
  size_t nc = a->color_count;
  size_vec_t *color_segs = calloc(nc, sizeof(*color_segs));
  if (!color_segs) {
    free_segment_by_block(mach, by_block);
    return 0;
  }
  size_t recolored = 0;
  for (size_t b = 0; b < a->block_count; ++b) {
    const size_t *indices = by_block[b].data;
    size_t count = by_block[b].len;
    if (!count)
      continue;
    bool has_cand = false;
    for (size_t k = 0; k < count; ++k) {
      size_t idx = indices[k];
      const nyr_mach_live_segment_t *s = &a->segments[idx];
      if (candidate[idx] && s->color < 0 && s->start <= s->end &&
          s->end < mach->inst_len) {
        has_cand = true;
        break;
      }
    }
    if (!has_cand)
      continue;
    for (size_t c = 0; c < nc; ++c)
      color_segs[c].len = 0;
    for (size_t k = 0; k < count; ++k) {
      size_t idx = indices[k];
      int sc = a->segments[idx].color;
      if (sc >= 0 && (size_t)sc < nc)
        (void)size_vec_push(&color_segs[sc], idx);
    }
    for (size_t k = 0; k < count; ++k) {
      size_t idx = indices[k];
      nyr_mach_live_segment_t *s = &a->segments[idx];
      if (!candidate[idx] || s->color >= 0 || s->start > s->end ||
          s->end >= mach->inst_len)
        continue;
      for (int c = 0; (size_t)c < nc; ++c) {
        if (!segment_accepts_color(mach, s, nc, c) ||
            !segment_color_survives_clobbers(mach, s, reg_class, c, false))
          continue;
        bool conflict = false;
        const size_t *cidxs = color_segs[c].data;
        size_t clen = color_segs[c].len;
        for (size_t j = 0; j < clen; ++j) {
          const nyr_mach_live_segment_t *other = &a->segments[cidxs[j]];
          if (!(other->end < s->start || s->end < other->start)) {
            conflict = true;
            break;
          }
        }
        if (conflict ||
            (color_blocked && color_blocked(ctx, c, s->start, s->end)))
          continue;
        s->color = c;
        s->reload = false;
        s->spill = false;
        s->carried = s->acyclic_carried = false;
        s->spill_reason = NYR_MACH_SPILL_NONE;
        (void)size_vec_push(&color_segs[c], idx);
        ++recolored;
        break;
      }
    }
  }
  for (size_t c = 0; c < nc; ++c)
    free(color_segs[c].data);
  free(color_segs);
  free_segment_by_block(mach, by_block);
  return recolored;
}

bool nyr_mach_regalloc_build(const nyr_mach_func_t *mach, size_t color_count,
                             nyr_mach_regalloc_t *out) {
  return nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_GPR, color_count,
                                       out);
}

bool nyr_mach_regalloc_build_callee_saved_gpr(const nyr_mach_func_t *mach,
                                              size_t color_count,
                                              nyr_mach_regalloc_t *out) {
  /*
   * Target encoders may opt into this only when every allocated GPR color
   * maps to an ABI callee-saved physical register.  Calls then cease to be
   * artificial split points; DIV/MOD and variable shifts remain barriers.
   */
  return nyr_mach_regalloc_build_class_mode(mach, NYR_MACH_REGCLASS_GPR,
                                            color_count, true, false, out);
}

void nyr_mach_regalloc_free(nyr_mach_regalloc_t *a) {
  if (!a)
    return;
  free(a->segments);
  free(a->vreg_offsets);
  free(a->live_in);
  free(a->live_out);
  *a = (nyr_mach_regalloc_t){0};
}

const nyr_mach_live_segment_t *
nyr_mach_regalloc_segment_at(const nyr_mach_regalloc_t *a, uint32_t vreg,
                             size_t inst) {
  if (!a || !a->vreg_offsets || vreg >= a->vreg_len)
    return NULL;
  size_t lo = a->vreg_offsets[vreg], hi = a->vreg_offsets[vreg + 1];
  if (hi - lo == 1) {
    const nyr_mach_live_segment_t *s = &a->segments[lo];
    return inst >= s->start && inst <= s->end ? s : NULL;
  }
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const nyr_mach_live_segment_t *s = &a->segments[mid];
    if (inst < s->start)
      hi = mid;
    else if (inst > s->end)
      lo = mid + 1;
    else
      return s;
  }
  return NULL;
}

size_t nyr_mach_regalloc_peak_live(const nyr_mach_regalloc_t *a,
                                   size_t inst_len) {
  if (!a || a->segment_len == 0 || inst_len == 0)
    return 0;
  /*
   * Most emitted functions are small; keep their peak-live sweep allocation
   * free. Large functions retain the same O(insts + segments) difference scan.
   */
  int local_delta[257] = {0};
  int *delta = local_delta;
  bool heap_delta = false;
  if (inst_len >= sizeof(local_delta) / sizeof(local_delta[0])) {
    if (inst_len == SIZE_MAX)
      return 0;
    delta = calloc(inst_len + 1, sizeof(*delta));
    if (!delta)
      return 0;
    heap_delta = true;
  }
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &a->segments[i];
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
  if (heap_delta)
    free(delta);
  return peak;
}

bool nyr_mach_regalloc_live_in(const nyr_mach_regalloc_t *a, size_t block,
                               uint32_t vreg) {
  return a && a->live_in && block < a->block_count && vreg < a->vreg_len &&
         bit_get(a->live_in + block * a->bit_words, vreg);
}

bool nyr_mach_regalloc_live_out(const nyr_mach_regalloc_t *a, size_t block,
                                uint32_t vreg) {
  return a && a->live_out && block < a->block_count && vreg < a->vreg_len &&
         bit_get(a->live_out + block * a->bit_words, vreg);
}

bool nyr_mach_linear_scan_ranges(nyr_mach_func_t *mach, int *colors_out,
                                 size_t *live_start, size_t *live_end,
                                 size_t colors_cap) {
  if (!mach || !colors_out || colors_cap < mach->vreg_len)
    return false;
  nyr_mach_regalloc_t a = {0};
  if (!nyr_mach_regalloc_build(mach, 8, &a))
    return false;
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    colors_out[v] = -1;
    if (live_start)
      live_start[v] = 0;
    if (live_end)
      live_end[v] = 0;
    size_t best = 0;
    for (size_t i = a.vreg_offsets[v]; i < a.vreg_offsets[v + 1]; ++i) {
      const nyr_mach_live_segment_t *s = &a.segments[i];
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
  nyr_mach_regalloc_free(&a);
  return true;
}

bool nyr_mach_linear_scan(nyr_mach_func_t *mach, int *colors_out,
                          size_t colors_cap) {
  return nyr_mach_linear_scan_ranges(mach, colors_out, NULL, NULL, colors_cap);
}
