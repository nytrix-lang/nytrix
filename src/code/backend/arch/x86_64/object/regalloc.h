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
  size_t interval_len = 0;
  for (size_t v = 0; v < mach->vreg_len; ++v)
    if (mach_vreg_home_colorable(e, v, block_has_call,
                                 &intervals[interval_len]))
      ++interval_len;
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
     * These intervals have exactly one non-spilled segment and no uses
     * outside their defining block. Call-bearing blocks were excluded above;
     * other block-local values can share homes because each is dead when
     * control leaves its block.
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
