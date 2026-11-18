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
  /* 0: block-local resident home, 1: spill-colored home (separate pool). */
  uint32_t pool;
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

/*
 * Range facts for home coloring. Both interval builders used to rescan every
 * instruction of a value's live range per value (quadratic on large bodies:
 * 2.5 s to encode one 36k-instruction function). These answer the same
 * questions from one linear pass:
 *  - boundary_prefix[pc]: color boundaries among instructions [0, pc);
 *  - pin_pcs (by vreg, ascending pc): instructions that pin a value's home
 *    (a frame load into it, or a float compare producing it).
 */
typedef struct {
  uint32_t *boundary_prefix;
  uint32_t *pin_start; /* vreg -> first index in pin_pcs; vreg_len + 1 */
  uint32_t *pin_pcs;
} mach_home_ranges_t;

static bool mach_home_inst_pins(const nyr_mach_func_t *mach,
                                const nyr_mach_inst_t *in, size_t *vreg) {
  if (in->dst.kind != NYR_MACH_OPERAND_VREG)
    return false;
  if ((in->opcode == NYR_MACH_LOAD &&
       in->src0.kind == NYR_MACH_OPERAND_FRAME) ||
      (in->opcode == NYR_MACH_CMP && mach_is_float(mach, &in->src0))) {
    *vreg = (size_t)in->dst.as.reg;
    return true;
  }
  return false;
}

static void mach_home_ranges_free(mach_home_ranges_t *r) {
  free(r->boundary_prefix);
  free(r->pin_start);
  free(r->pin_pcs);
  *r = (mach_home_ranges_t){0};
}

static bool mach_home_ranges_build(const nyr_mach_func_t *mach,
                                   mach_home_ranges_t *r) {
  *r = (mach_home_ranges_t){0};
  size_t n = mach->inst_len, vn = mach->vreg_len;
  if (n >= UINT32_MAX || vn >= UINT32_MAX)
    return false;
  r->boundary_prefix = malloc((n + 1) * sizeof(*r->boundary_prefix));
  r->pin_start = calloc(vn + 1, sizeof(*r->pin_start));
  if (!r->boundary_prefix || !r->pin_start) {
    mach_home_ranges_free(r);
    return false;
  }
  uint32_t count = 0, pins = 0;
  for (size_t pc = 0; pc < n; ++pc) {
    r->boundary_prefix[pc] = count;
    if (mach_home_color_boundary(&mach->insts[pc]))
      count++;
    size_t v = 0;
    if (mach_home_inst_pins(mach, &mach->insts[pc], &v) && v < vn) {
      r->pin_start[v + 1]++;
      pins++;
    }
  }
  r->boundary_prefix[n] = count;
  for (size_t v = 0; v < vn; ++v)
    r->pin_start[v + 1] += r->pin_start[v];
  r->pin_pcs = malloc((pins ? pins : 1) * sizeof(*r->pin_pcs));
  uint32_t *fill = malloc((vn ? vn : 1) * sizeof(*fill));
  if (!r->pin_pcs || !fill) {
    free(fill);
    mach_home_ranges_free(r);
    return false;
  }
  memcpy(fill, r->pin_start, vn * sizeof(*fill));
  for (size_t pc = 0; pc < n; ++pc) {
    size_t v = 0;
    if (mach_home_inst_pins(mach, &mach->insts[pc], &v) && v < vn)
      r->pin_pcs[fill[v]++] = (uint32_t)pc;
  }
  free(fill);
  return true;
}

/* True when some instruction in [lo, hi] pins vreg's home. */
static bool mach_home_pinned_in(const mach_home_ranges_t *r, size_t vreg,
                                size_t lo, size_t hi) {
  uint32_t a = r->pin_start[vreg], b = r->pin_start[vreg + 1];
  while (a < b) { /* first pin pc >= lo */
    uint32_t m = a + (b - a) / 2;
    if (r->pin_pcs[m] < lo)
      a = m + 1;
    else
      b = m;
  }
  return a < r->pin_start[vreg + 1] && r->pin_pcs[a] <= hi;
}

static bool mach_vreg_home_colorable(const nyr_x64_mach_enc_t *e, size_t vreg,
                                     const bool *block_has_call,
                                     const mach_home_ranges_t *ranges,
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
  if (ranges) {
    if (ranges->boundary_prefix[seg->end + 1] !=
            ranges->boundary_prefix[seg->start] ||
        mach_home_pinned_in(ranges, vreg, seg->start, seg->end))
      return false;
  }
  for (size_t pc = seg->start; !ranges && pc <= seg->end; ++pc) {
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
                                          const mach_home_ranges_t *ranges,
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
    /*
     * A value that is register-resident anywhere can be defined in its
     * register without writing the home.  The encoder's sticky-RAX tag is
     * keyed by home offset and assumes the slot still names the live value,
     * so such a value must keep a private slot: sharing it let a later read
     * reuse a dead neighbour left in %rax (x64_loop_recolor, plain/8 homes).
     */
    if (s->color >= 0)
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
  if (ranges && mach_home_pinned_in(ranges, vreg, lo, hi))
    return false;
  for (size_t pc = lo; !ranges && pc <= hi; ++pc) {
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
/*
 * Offset -> slot index map for mach_order_hot_homes. Slots have unique
 * offsets; both lookups used to scan every slot per value (quadratic on
 * functions with tens of thousands of homes).
 */
typedef struct {
  int *keys;
  size_t *vals;
  size_t cap;
} mach_off_map_t;

static bool mach_off_map_init(mach_off_map_t *m, size_t n) {
  m->cap = 16;
  while (m->cap < n * 2 + 2)
    m->cap *= 2;
  m->keys = malloc(m->cap * sizeof(*m->keys));
  m->vals = malloc(m->cap * sizeof(*m->vals));
  if (!m->keys || !m->vals) {
    free(m->keys);
    free(m->vals);
    return false;
  }
  for (size_t i = 0; i < m->cap; ++i)
    m->keys[i] = 0; /* offset 0 means "no home" and is never a key */
  return true;
}

static size_t *mach_off_map_slot(mach_off_map_t *m, int key, bool *found) {
  size_t mask = m->cap - 1;
  size_t at = ((uint32_t)key * 2654435761u) & mask;
  while (m->keys[at] != 0 && m->keys[at] != key)
    at = (at + 1) & mask;
  *found = m->keys[at] == key;
  m->keys[at] = key;
  return &m->vals[at];
}

static void mach_off_map_free(mach_off_map_t *m) {
  free(m->keys);
  free(m->vals);
}

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
  mach_off_map_t by_off = {0};
  bool indexed = mach_off_map_init(&by_off, vreg_len);
  for (size_t v = 0; v < vreg_len; ++v) {
    orig[v] = offs[v];
    if (!offs[v])
      continue;
    size_t at = 0;
    if (indexed) {
      bool found = false;
      size_t *slot = mach_off_map_slot(&by_off, offs[v], &found);
      if (!found)
        *slot = slot_len;
      at = *slot;
    } else {
      while (at < slot_len && slots[at].off != offs[v])
        ++at;
    }
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
  if (indexed) {
    /* rebuild for the sorted slot order */
    mach_off_map_free(&by_off);
    indexed = mach_off_map_init(&by_off, slot_len);
    for (size_t i = 0; indexed && i < slot_len; ++i) {
      bool found = false;
      size_t *slot = mach_off_map_slot(&by_off, slots[i].off, &found);
      if (!found)
        *slot = i;
    }
  }
  for (size_t v = 0; v < vreg_len; ++v) {
    if (!orig[v])
      continue;
    if (indexed) {
      bool found = false;
      size_t *slot = mach_off_map_slot(&by_off, orig[v], &found);
      if (found) /* every home offset is some slot's offset */
        offs[v] = slots[*slot].new_off;
      continue;
    }
    for (size_t i = 0; i < slot_len; ++i)
      if (slots[i].off == orig[v]) {
        offs[v] = slots[i].new_off;
        break;
      }
  }
  if (indexed)
    mach_off_map_free(&by_off);
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
  (void)e;
  (void)offs;
  (void)frame_slot_len;
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

/*
 * A scalar parameter slot that the body only reads through one LOAD already
 * holds its value for the whole call (the prologue spills the argument there).
 * Give the loaded vreg that slot as its home: the entry load then needs no
 * reload/store pair, and every later home access reads the argument slot
 * directly. Recursive calls (towers, queens) paid a load and a store per
 * parameter per call for the copy. NYTRIX_NO_PARAM_HOME_ALIAS=1 disables it.
 */
static void mach_alias_param_homes(nyr_x64_mach_enc_t *e, int *offs) {
  const nyr_mach_func_t *mach = e->mach;
  if (!mach || !offs || !mach->param_count || !e->frame_slot_offsets ||
      mach->param_count > mach->frame_slot_len ||
      getenv("NYTRIX_NO_PARAM_HOME_ALIAS"))
    return;
  for (size_t slot = 0; slot < mach->param_count; ++slot) {
    if (mach->param_types && mach->param_types[slot] == NYIR_PARAM_AGGREGATE)
      continue;
    nyr_mach_type_t type = mach->frame_slots[slot].type;
    if (mach->frame_slots[slot].size > 8 || type == NYR_MACH_TYPE_F32 ||
        type == NYR_MACH_TYPE_F64)
      continue;
    size_t refs = 0, load_at = SIZE_MAX;
    for (size_t i = 0; i < mach->inst_len && refs < 2; ++i) {
      const nyr_mach_inst_t *in = &mach->insts[i];
      const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                         &in->src2};
      for (size_t k = 0; k < 4; ++k)
        if (ops[k]->kind == NYR_MACH_OPERAND_FRAME &&
            ops[k]->as.frame_index == slot) {
          ++refs;
          if (k == 1 && in->opcode == NYR_MACH_LOAD)
            load_at = i;
        }
      for (size_t k = 0; k < in->args_len; ++k)
        if (in->args[k].kind == NYR_MACH_OPERAND_FRAME &&
            in->args[k].as.frame_index == slot)
          refs += 2;
    }
    if (refs != 1 || load_at == SIZE_MAX)
      continue;
    const nyr_mach_inst_t *load = &mach->insts[load_at];
    if (load->dst.kind != NYR_MACH_OPERAND_VREG ||
        load->dst.as.reg >= mach->vreg_len ||
        mach_is_float(mach, &load->dst) || mach_is_v128(mach, &load->dst))
      continue;
    uint32_t v = load->dst.as.reg;
    size_t defs = 0;
    for (size_t i = 0; i < mach->inst_len && defs < 2; ++i) {
      const nyr_mach_inst_t *in = &mach->insts[i];
      if (in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == v &&
          in->opcode != NYR_MACH_STORE)
        ++defs;
    }
    if (defs != 1)
      continue;
    offs[v] = e->frame_slot_offsets[slot];
  }
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
  mach_home_ranges_t ranges_storage;
  const mach_home_ranges_t *ranges =
      getenv("NYTRIX_HOME_RANGE_SCAN") ||
              !mach_home_ranges_build(mach, &ranges_storage)
          ? NULL
          : &ranges_storage;
  for (size_t v = 0; v < mach->vreg_len; ++v) {
    if (mach_vreg_home_colorable(e, v, block_has_call, ranges,
                                 &intervals[interval_len])) {
      ++interval_len;
      continue;
    }
    if (spill_color &&
        mach_vreg_home_spill_interval(e, v, ranges,
                                      &intervals[interval_len])) {
      intervals[interval_len].pool = 1;
      ++interval_len;
    }
  }
  if (ranges)
    mach_home_ranges_free(&ranges_storage);
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
  /*
   * Block-local residents and spill-colored values never share a slot.  A
   * resident value can be defined in its register without writing its home,
   * while the encoder's sticky-RAX tag names a home offset; a spilled
   * neighbour tagged on the same slot then reads as the resident value
   * (x64_loop_recolor plain/8: the XOR chain's last step reused a dead
   * partial result from %rax).  Each pool is colored independently.
   */
  size_t used = 0, home_len = 0, active_len = 0, free_len = 0;
  for (uint32_t pool = 0; pool < 2; ++pool) {
  active_len = free_len = 0;
  for (size_t i = 0; i < interval_len; ++i) {
    const mach_home_interval_t *interval = &intervals[i];
    if (interval->pool != pool)
      continue;
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
  mach_alias_param_homes(e, offs);
  e->vreg_home_offs = offs;
  free(e->vreg_home_shared);
  e->vreg_home_shared = NULL;
  e->vreg_home_bytes = used;
  return true;
}
