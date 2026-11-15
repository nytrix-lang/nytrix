/* Private optional-class register allocation helpers, included by object.c. */
static void mach_try_build_fast_regalloc(
    const nyr_mach_func_t *mach, bool *enabled, bool **seeded,
    bool seeded_stack[MACH_SEEDED_STACK_MAX], size_t *colors_len,
    size_t color_count, nyr_mach_regalloc_t *regalloc, bool vector_class) {
  *colors_len = mach->vreg_len;
  if (!*enabled || !mach->vreg_len)
    return;
  if (mach->vreg_len <= MACH_SEEDED_STACK_MAX) {
    *seeded = seeded_stack;
    memset(*seeded, 0, mach->vreg_len * sizeof(**seeded));
  } else {
    *seeded = calloc(mach->vreg_len, sizeof(**seeded));
  }
  bool ok = *seeded &&
            (vector_class
                 ? nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_VECTOR,
                                                 color_count, regalloc)
                 : nyr_mach_regalloc_build_x64_class(
                       mach, NYR_MACH_REGCLASS_FPR, color_count, regalloc));
  if (ok)
    return;
  if (*seeded && *seeded != seeded_stack)
    free(*seeded);
  *seeded = NULL;
  nyr_mach_regalloc_free(regalloc);
  *enabled = false;
}

static size_t mach_vreg_home_size(const nyr_mach_func_t *mach, size_t vreg) {
  if (!mach || !mach->vreg_types || vreg >= mach->vreg_len)
    return 8;
  switch (mach->vreg_types[vreg]) {
  case NYR_MACH_TYPE_V256_I64:
  case NYR_MACH_TYPE_V256_F64:
    return 32;
  case NYR_MACH_TYPE_V128_I64:
  case NYR_MACH_TYPE_V128_F64:
  case NYR_MACH_TYPE_V128_F32:
    return 16;
  default:
    return 8;
  }
}

typedef struct {
  size_t start;
  size_t end;
  uint32_t vreg;
  uint32_t block;
} mach_home_interval_t;

static int mach_home_interval_cmp(const void *lhs, const void *rhs) {
  const mach_home_interval_t *a = lhs;
  const mach_home_interval_t *b = rhs;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  if (a->end != b->end)
    return a->end < b->end ? -1 : 1;
  return a->vreg < b->vreg ? -1 : a->vreg != b->vreg;
}

static bool mach_home_color_boundary(const nyr_mach_inst_t *in) {
  if (!in)
    return true;
  if (in->opcode == NYR_MACH_CALL || in->opcode == NYR_MACH_DIV ||
      in->opcode == NYR_MACH_MOD || in->opcode == NYR_MACH_INTRINSIC ||
      ((in->opcode == NYR_MACH_SHL || in->opcode == NYR_MACH_SAR ||
        in->opcode == NYR_MACH_SHR || in->opcode == NYR_MACH_ROR ||
        in->opcode == NYR_MACH_ROR32) &&
       in->src1.kind != NYR_MACH_OPERAND_IMM))
    return true;
  return in->dst.kind == NYR_MACH_OPERAND_PREG ||
         in->src0.kind == NYR_MACH_OPERAND_PREG ||
         in->src1.kind == NYR_MACH_OPERAND_PREG ||
         in->src2.kind == NYR_MACH_OPERAND_PREG;
}

static bool mach_vreg_home_colorable(const nyr_x64_mach_enc_t *e, size_t vreg,
                                     const bool *block_has_call,
                                     mach_home_interval_t *out) {
  if (!e || !e->mach || !out || !e->mach->vreg_types ||
      !e->mach->vreg_classes || vreg >= e->mach->vreg_len ||
      e->mach->vreg_classes[vreg] != NYR_MACH_REGCLASS_GPR ||
      (e->mach->vreg_types[vreg] != NYR_MACH_TYPE_I64 &&
       e->mach->vreg_types[vreg] != NYR_MACH_TYPE_PTR) ||
      !e->regalloc.vreg_offsets || e->regalloc.vreg_len != e->mach->vreg_len)
    return false;
  size_t first = e->regalloc.vreg_offsets[vreg];
  size_t last = e->regalloc.vreg_offsets[vreg + 1];
  if (last != first + 1 || last > e->regalloc.segment_len)
    return false;
  const nyr_mach_live_segment_t *seg = &e->regalloc.segments[first];
  if (seg->vreg != vreg || seg->reload || seg->spill || seg->carried ||
      seg->block >= e->mach->block_len || seg->start > seg->end ||
      seg->end >= e->mach->inst_len)
    return false;
  /*
   * Calls expose aliasing that linear block-local intervals do not model.
   * Keep homes unique in call-bearing blocks.
   */
  if (block_has_call[seg->block])
    return false;
  const nyr_mach_block_t *block = &e->mach->blocks[seg->block];
  if (!block->inst_count || seg->start < block->first_inst ||
      seg->end >= block->first_inst + block->inst_count)
    return false;
  for (size_t pc = seg->start; pc <= seg->end; ++pc) {
    const nyr_mach_inst_t *in = &e->mach->insts[pc];
    if (mach_home_color_boundary(in))
      return false;
    /*
     * Floating predicates are materialized through the GPR result path.
     * Preserve their canonical homes: the block-local interval can otherwise
     * assign the compare result a reused slot and branch on stale state.
     */
    if (in->opcode == NYR_MACH_CMP && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == vreg && mach_is_float(e->mach, &in->src0))
      return false;
    /*
     * A frame-loaded value's home identifies its incoming argument home.
     * Never share it with an unrelated interval.
     */
    if (in->opcode == NYR_MACH_LOAD && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == vreg && in->src0.kind == NYR_MACH_OPERAND_FRAME)
      return false;
  }
  *out = (mach_home_interval_t){.start = seg->start,
                                .end = seg->end,
                                .vreg = (uint32_t)vreg,
                                .block = seg->block};
  return true;
}

static bool mach_home_end_before(const size_t *home_ends, size_t lhs,
                                 size_t rhs) {
  return home_ends[lhs] < home_ends[rhs] ||
         (home_ends[lhs] == home_ends[rhs] && lhs < rhs);
}

static void mach_home_active_push(size_t *active, size_t *active_len,
                                  const size_t *home_ends, size_t home) {
  size_t at = (*active_len)++;
  while (at > 0) {
    size_t parent = (at - 1u) / 2u;
    if (!mach_home_end_before(home_ends, home, active[parent]))
      break;
    active[at] = active[parent];
    at = parent;
  }
  active[at] = home;
}

static size_t mach_home_active_pop(size_t *active, size_t *active_len,
                                   const size_t *home_ends) {
  size_t result = active[0];
  size_t tail = active[--(*active_len)];
  if (*active_len == 0)
    return result;
  size_t at = 0;
  while (at < *active_len / 2u) {
    size_t child = at * 2u + 1u;
    if (child + 1u < *active_len &&
        mach_home_end_before(home_ends, active[child + 1u], active[child]))
      ++child;
    if (!mach_home_end_before(home_ends, active[child], tail))
      break;
    active[at] = active[child];
    at = child;
  }
  active[at] = tail;
  return result;
}

/*
 * Spill-slot coloring. A home only has to be unique while its value is live,
 * so two values whose live ranges do not overlap can share one slot. The
 * single-block path above only covers block-local residents; the frame is
 * dominated by spill slots, which stay live across blocks and calls. Take the
 * union of every segment of a spilled value as its range, which is a superset
 * of the true live set and therefore keeps overlapping temp reuse honest.
 */
static bool mach_vreg_home_spill_interval(const nyr_x64_mach_enc_t *e,
                                          size_t vreg,
                                          mach_home_interval_t *out) {
  if (!e || !e->mach || !out || !e->mach->vreg_types ||
      !e->mach->vreg_classes || vreg >= e->mach->vreg_len ||
      e->mach->vreg_classes[vreg] != NYR_MACH_REGCLASS_GPR ||
      !e->regalloc.vreg_offsets || e->regalloc.vreg_len != e->mach->vreg_len)
    return false;
  if (mach_vreg_home_size(e->mach, vreg) != 8)
    return false;
  size_t first = e->regalloc.vreg_offsets[vreg];
  size_t last = e->regalloc.vreg_offsets[vreg + 1];
  if (last <= first || last > e->regalloc.segment_len)
    return false;
  size_t lo = SIZE_MAX, hi = 0;
  bool uses_home = false;
  for (size_t i = first; i < last; ++i) {
    const nyr_mach_live_segment_t *s = &e->regalloc.segments[i];
    if (s->vreg != vreg || s->block >= e->mach->block_len ||
        s->start > s->end || s->end >= e->mach->inst_len)
      return false;
    const nyr_mach_block_t *b = &e->mach->blocks[s->block];
    if (!b->inst_count || s->start < b->first_inst ||
        s->end >= b->first_inst + b->inst_count)
      return false;
    if (s->spill || s->reload || s->color < 0)
      uses_home = true;
    if (s->start < lo)
      lo = s->start;
    if (s->end > hi)
      hi = s->end;
  }
  if (!uses_home || lo == SIZE_MAX || lo > hi)
    return false;
  for (size_t pc = lo; pc <= hi; ++pc) {
    const nyr_mach_inst_t *in = &e->mach->insts[pc];
    /*
     * A frame-loaded value's home identifies its incoming argument home, and
     * floating predicates materialize through the GPR result path; both keep
     * their canonical, unshared slots.
     */
    if (in->opcode == NYR_MACH_LOAD && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == vreg && in->src0.kind == NYR_MACH_OPERAND_FRAME)
      return false;
    if (in->opcode == NYR_MACH_CMP && in->dst.kind == NYR_MACH_OPERAND_VREG &&
        in->dst.as.reg == vreg && mach_is_float(e->mach, &in->src0))
      return false;
  }
  *out = (mach_home_interval_t){.start = lo,
                                .end = hi,
                                .vreg = (uint32_t)vreg,
                                .block = 0};
  return true;
}

static bool mach_append_vreg_home(nyr_x64_mach_enc_t *e,
                                  size_t frame_slots_bytes, size_t width,
                                  size_t *used, int *out) {
  if (!e || !used || !out || *used > SIZE_MAX - width ||
      frame_slots_bytes > SIZE_MAX - *used - width)
    return false;
  *used += width;
  size_t below_rbp = (size_t)e->cs_slots * 8u + frame_slots_bytes + *used;
  if (below_rbp > (size_t)INT_MAX)
    return false;
  *out = -(int)below_rbp;
  return true;
}

typedef struct {
  int off;
  int new_off;
  size_t width;
  size_t weight;
} mach_home_slot_t;

static int mach_home_slot_hot_cmp(const void *lhs, const void *rhs) {
  const mach_home_slot_t *a = lhs;
  const mach_home_slot_t *b = rhs;
  if (a->weight != b->weight)
    return a->weight > b->weight ? -1 : 1;
  return a->off < b->off ? -1 : a->off > b->off;
}

static int mach_home_offset_low_cmp(const void *lhs, const void *rhs) {
  int a = *(const int *)lhs;
  int b = *(const int *)rhs;
  /* Largest offset is closest to %rbp, hence first inside disp8 reach. */
  return a < b ? 1 : (a > b ? -1 : 0);
}

/*
 * Hot-slot ordering. disp8 frame addressing reaches only the 128 bytes below
 * %rbp, so which value owns which slot decides three bytes per access. The
 * coloring pass allocates in live-range order, which says nothing about how
 * often a slot is touched: a value defined once and spilled everywhere ends up
 * beside a hot accumulator. Count MACH operand references per vreg, group the
 * vregs that share a slot, then permute the offsets of equal-width slots so
 * the most-used homes sit closest to %rbp.
 *
 * Only offsets move. Every slot keeps its byte range, its width, and the
 * disjointness the coloring pass proved, so the frame stays the same size and
 * no two live values can alias; width-16/32 vector homes are left in place.
 * NYTRIX_NO_HOT_HOMES=1 keeps allocation order for A/B runs.
 */
static bool mach_order_hot_homes(nyr_x64_mach_enc_t *e, int *offs,
                                 size_t vreg_len) {
  if (!e || !e->mach || !offs || !vreg_len)
    return true;
  const char *skip = getenv("NYTRIX_NO_HOT_HOMES");
  if (skip && strcmp(skip, "0") != 0 && strcmp(skip, "off") != 0)
    return true;
  const nyr_mach_func_t *mach = e->mach;
  size_t *weights = calloc(vreg_len, sizeof(*weights));
  mach_home_slot_t *slots = calloc(vreg_len, sizeof(*slots));
  int *orig = calloc(vreg_len, sizeof(*orig));
  int *low = calloc(vreg_len, sizeof(*low));
  bool *home_used = calloc(vreg_len, sizeof(*home_used));
  if (!weights || !slots || !orig || !low || !home_used) {
    /* Ordering is an optimization; never fail the compile over it. */
    free(weights);
    free(slots);
    free(orig);
    free(low);
    free(home_used);
    return true;
  }
  /*
   * Only a value that reaches its home competes for a shallow slot. A fully
   * resident value still appears in many operands, and counting those would
   * let it push a hot spilled value out of disp8 reach.
   */
  bool have_segments =
      e->regalloc.vreg_offsets && e->regalloc.vreg_len == vreg_len;
  for (size_t v = 0; v < vreg_len; ++v) {
    home_used[v] = !have_segments;
    if (!have_segments)
      continue;
    size_t first = e->regalloc.vreg_offsets[v];
    size_t last = e->regalloc.vreg_offsets[v + 1];
    if (last > e->regalloc.segment_len)
      last = e->regalloc.segment_len;
    for (size_t i = first; i < last; ++i) {
      const nyr_mach_live_segment_t *seg = &e->regalloc.segments[i];
      if (seg->spill || seg->reload || seg->color < 0) {
        home_used[v] = true;
        break;
      }
    }
  }
  for (size_t pc = 0; pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k)
      if (ops[k]->kind == NYR_MACH_OPERAND_VREG &&
          ops[k]->as.reg < vreg_len && home_used[ops[k]->as.reg])
        ++weights[ops[k]->as.reg];
    for (size_t k = 0; k < in->args_len; ++k)
      if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
          in->args[k].as.reg < vreg_len && home_used[in->args[k].as.reg])
        ++weights[in->args[k].as.reg];
  }
  size_t slot_len = 0;
  for (size_t v = 0; v < vreg_len; ++v) {
    orig[v] = offs[v];
    if (!offs[v])
      continue;
    size_t at = 0;
    while (at < slot_len && slots[at].off != offs[v])
      ++at;
    if (at == slot_len) {
      slots[at].off = offs[v];
      slots[at].new_off = offs[v];
      slots[at].width = mach_vreg_home_size(mach, v);
      ++slot_len;
    }
    slots[at].weight += weights[v];
  }
  size_t low_len = 0;
  for (size_t i = 0; i < slot_len; ++i)
    if (slots[i].width == 8)
      low[low_len++] = slots[i].off;
  qsort(low, low_len, sizeof(*low), mach_home_offset_low_cmp);
  qsort(slots, slot_len, sizeof(*slots), mach_home_slot_hot_cmp);
  size_t at = 0;
  for (size_t i = 0; i < slot_len; ++i)
    if (slots[i].width == 8 && at < low_len)
      slots[i].new_off = low[at++];
  for (size_t v = 0; v < vreg_len; ++v) {
    if (!orig[v])
      continue;
    for (size_t i = 0; i < slot_len; ++i)
      if (slots[i].off == orig[v]) {
        offs[v] = slots[i].new_off;
        break;
      }
  }
  free(weights);
  free(slots);
  free(orig);
  free(low);
  free(home_used);
  return true;
}

/*
 * Frame-slot ordering by access frequency.
 *
 * disp8 frame addressing reaches only the 128 bytes below %%rbp, so which value
 * owns which slot decides three bytes per access. The coloring pass allocates in
 * live-range order, which says nothing about how often a slot is touched: a value
 * defined once and spilled everywhere ends up beside a hot accumulator. Count MACH
 * operand references per vreg that actually reaches a home, group equal-width slots,
 * and permute the offsets of 8-byte slots so the most-used homes sit closest to
 * %%rbp. Only offsets move — every slot keeps its byte range, its width, and the
 * disjointness the coloring pass proved; the frame stays the same size and no two
 * live values can alias; width-16/32 vector homes keep their offsets.  
 * NYTRIX_NO_FRAME_SLOT_ORDER=1 keeps allocation order for A/B runs.
 */
static bool mach_order_frame_slots_by_access(nyr_x64_mach_enc_t *e, int *offs,
                                              size_t frame_slot_len) {
  if (!e || !e->mach || !offs || !frame_slot_len)
    return true;
  const char *skip = getenv("NYTRIX_NO_FRAME_SLOT_ORDER");
  if (skip && strcmp(skip, "0") != 0 && strcmp(skip, "off") != 0)
    return true;
  const nyr_mach_func_t *mach = e->mach;
  /*
   * Count references per vreg that reaches a home. A fully resident value
   * still appears in many operands and counting those would push a hot spilled
   * value out of disp8 reach. Only count vregs that have a home (are spill/reload).
   */
  size_t *weights = calloc(frame_slot_len, sizeof(*weights));
  bool *home_reachable = calloc(frame_slot_len, sizeof(*home_reachable));
  if (!weights || !home_reachable) {
    free(weights);
    free(home_reachable);
    return true;
  }
  for (size_t pc = 0; pc < mach->inst_len; ++pc) {
    const nyr_mach_inst_t *in = &mach->insts[pc];
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); ++k) {
      if (ops[k]->kind == NYR_MACH_OPERAND_VREG &&
          ops[k]->as.reg < frame_slot_len && home_reachable[ops[k]->as.reg])
        ++weights[ops[k]->as.reg];
      if (ops[k]->kind == NYR_MACH_OPERAND_FRAME) {
        int slot = ops[k]->as.frame_index;
        if (slot >= 0 && (size_t)slot < frame_slot_len)
          home_reachable[slot] = true;
      }
    }
    for (size_t k = 0; k < in->args_len; ++k)
      if (in->args[k].kind == NYR_MACH_OPERAND_VREG &&
          in->args[k].as.reg < frame_slot_len && home_reachable[in->args[k].as.reg])
        ++weights[in->args[k].as.reg];
  }
  /*
   * Group slots by width (8 = GPR, 16/32 = vector). Vector homes keep their
   * original offsets; only 8-byte slots are reordered.
   */
  int *slot_idx = malloc(frame_slot_len * sizeof(*slot_idx));
  int *slot_weight = malloc(frame_slot_len * sizeof(*slot_weight));
  int *slot_off = malloc(frame_slot_len * sizeof(*slot_off));
  if (!slot_idx || !slot_weight || !slot_off) {
    free(weights); free(home_reachable); free(slot_idx); free(slot_weight); free(slot_off);
    return true;
  }
  for (size_t slot = 0; slot < frame_slot_len; ++slot) {
    slot_idx[slot] = (int)slot;
    slot_weight[slot] = (int)weights[slot];
    slot_off[slot] = offs[slot];
  }
  /* Sort 8-byte slots by weight descending; keep non-8-byte in place. */
  { size_t write = 0;
    for (size_t read = 0; read < frame_slot_len; ++read) {
      if (slot_off[read] != 0 && mach_vreg_home_size(mach, read) == 8) {
        slot_idx[write] = slot_idx[read];
        slot_weight[write] = slot_weight[read];
        ++write;
      }
    }
    size_t n_8 = write;
    /* Bubble sort the 8-byte slots by weight (typically small n_8). */
    for (size_t i = 0; i < n_8; ++i)
      for (size_t j = i + 1; j < n_8; ++j)
        if (slot_weight[i] < slot_weight[j]) {
          int w = slot_weight[i]; slot_weight[i] = slot_weight[j]; slot_weight[j] = w;
          int idx = slot_idx[i]; slot_idx[i] = slot_idx[j]; slot_idx[j] = idx;
        }
    /* Assign new offsets to 8-byte slots starting from the lowest (closest
     * to %%rbp). The first n_8 offsets below %%rbp + cs_slots*8 are the
     * disp8 reach.  We place them sequentially, keeping the existing
     * displacement-range invariant.
     */
    int base_off = (int)((size_t)e->cs_slots * 8u);
    for (size_t i = 0; i < n_8; ++i) {
      int slot = slot_idx[i];
      offs[slot] = base_off + (int)(i * 8u);
    }
    /* Restore non-8-byte slots to their original offsets. */
    for (size_t slot = 0; slot < frame_slot_len; ++slot)
      if (slot_off[slot] == 0 || mach_vreg_home_size(mach, slot) != 8)
        offs[slot] = slot_off[slot];
  }
  free(weights); free(slot_idx); free(slot_weight); free(slot_off);
  return true;
}

static bool mach_build_vreg_homes(nyr_x64_mach_enc_t *e,
                                  size_t frame_slots_bytes) {
  if (!e || !e->mach)
    return false;
  const nyr_mach_func_t *mach = e->mach;
  if (!mach->vreg_len) {
    e->vreg_home_bytes = 0;
    return true;
  }
  int *offs = calloc(mach->vreg_len, sizeof(*offs));
  mach_home_interval_t *intervals = calloc(mach->vreg_len, sizeof(*intervals));
  bool *block_has_call = calloc(mach->block_len, sizeof(*block_has_call));
  size_t *home_ends = calloc(mach->vreg_len, sizeof(*home_ends));
  int *home_offs = calloc(mach->vreg_len, sizeof(*home_offs));
  if (!offs || !intervals || !block_has_call || !home_ends || !home_offs) {
    free(offs);
    free(intervals);
    free(block_has_call);
    free(home_ends);
    free(home_offs);
    return mach_err(e, "x64 machine form encode: OOM (vreg homes)");
  }
  for (size_t b = 0; b < mach->block_len; ++b) {
    const nyr_mach_block_t *block = &mach->blocks[b];
    size_t end = (size_t)block->first_inst + block->inst_count;
    if (end > mach->inst_len)
      end = mach->inst_len;
    for (size_t pc = block->first_inst; pc < end; ++pc)
      if (mach->insts[pc].opcode == NYR_MACH_CALL) {
        block_has_call[b] = true;
        break;
      }
  }
  /*
   * Spill-slot reuse is opt-out so both layouts can be measured from a single
   * binary: NYTRIX_NO_SPILL_HOME_COLOR=1 restores one slot per value.
   */
  bool spill_color = true;
  {
    const char *off_env = getenv("NYTRIX_NO_SPILL_HOME_COLOR");
    spill_color = !(off_env && strcmp(off_env, "0") != 0 &&
                    strcmp(off_env, "off") != 0);
  }
  size_t interval_len = 0;
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    if (mach_vreg_home_colorable(e, v, block_has_call,
                                 &intervals[interval_len])) {
      ++interval_len;
      continue;
    }
    if (spill_color &&
        mach_vreg_home_spill_interval(e, v, &intervals[interval_len]))
      ++interval_len;
  }
  qsort(intervals, interval_len, sizeof(*intervals), mach_home_interval_cmp);
  size_t *active_homes = calloc(mach->vreg_len, sizeof(*active_homes));
  size_t *free_homes = calloc(mach->vreg_len, sizeof(*free_homes));
  if (!active_homes || !free_homes) {
    free(active_homes);
    free(free_homes);
    free(offs);
    free(intervals);
    free(block_has_call);
    free(home_ends);
    free(home_offs);
    return mach_err(e, "x64 machine form encode: OOM (home interval map)");
  }
  size_t used = 0, home_len = 0, active_len = 0, free_len = 0;
  for (size_t i = 0; i < interval_len; ++i) {
    const mach_home_interval_t *interval = &intervals[i];
    /*
     * Each interval covers every instruction at which its value can be live:
     * block-local residents use their single segment, spilled values use the
     * union of their segments. Two values whose ranges are disjoint are never
     * live at the same time, so they can share one slot.
     */
    while (active_len && home_ends[active_homes[0]] < interval->start)
      free_homes[free_len++] =
          mach_home_active_pop(active_homes, &active_len, home_ends);
    size_t home = free_len ? free_homes[--free_len] : SIZE_MAX;
    if (home == SIZE_MAX) {
      home = home_len;
      if (!mach_append_vreg_home(e, frame_slots_bytes, 8, &used,
                                 &home_offs[home])) {
        free(active_homes);
        free(free_homes);
        free(offs);
        free(intervals);
        free(block_has_call);
        free(home_ends);
        free(home_offs);
        return mach_err(e, "x64 machine form encode: vreg home overflow");
      }
      ++home_len;
    }
    offs[interval->vreg] = home_offs[home];
    home_ends[home] = interval->end;
    mach_home_active_push(active_homes, &active_len, home_ends, home);
  }
  size_t colored_homes = home_len;
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    if (offs[v])
      continue;
    if (!mach_append_vreg_home(e, frame_slots_bytes,
                               mach_vreg_home_size(mach, v), &used, &offs[v])) {
      free(offs);
      free(intervals);
      free(block_has_call);
      free(home_ends);
      free(home_offs);
      free(active_homes);
      free(free_homes);
      return mach_err(e, "x64 machine form encode: vreg home overflow");
    }
  }
  if (!mach_order_frame_slots_by_access(e, offs, mach->vreg_len)) {
    /* fall through: hot-home ordering is a separate optimization */
  }
  if (!mach_order_hot_homes(e, offs, mach->vreg_len)) {
    free(offs);
    free(intervals);
    free(block_has_call);
    free(home_ends);
    free(home_offs);
    free(active_homes);
    free(free_homes);
    return mach_err(e, "x64 machine form encode: vreg home ordering");
  }
  ny_dbg_heuristic_event(
      "regalloc",
      "home-layout vregs=%zu single-block=%zu unique=%zu reused=%zu "
      "bytes=%zu",
      mach->vreg_len, interval_len, colored_homes,
      interval_len > colored_homes ? interval_len - colored_homes : 0, used);
  free(intervals);
  free(block_has_call);
  free(home_ends);
  free(home_offs);
  free(active_homes);
  free(free_homes);
  e->vreg_home_offs = offs;
  e->vreg_home_bytes = used;
  return true;
}
