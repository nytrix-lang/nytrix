#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "code/native/ir/machine.h"
#include "base/common.h"

#include <stdlib.h>
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
  size_t n = a->segment_len + 1;
  ny_mach_live_segment_t *p =
      realloc(a->segments, n * sizeof(*a->segments));
  if (!p)
    return false;
  a->segments = p;
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

static bool color_segments(ny_mach_regalloc_t *a) {
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
    int color = -1;
    for (size_t c = 0; c < a->color_count; ++c)
      if (!used[c]) {
        color = (int)c;
        break;
      }
    if (color < 0) {
      size_t victim = SIZE_MAX;
      size_t farthest = s->end;
      for (size_t j = 0; j < active_n; ++j) {
        if (active[j].end > farthest) {
          farthest = active[j].end;
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
  /* Scan only segments in target->block.  by_block is an index keyed by block
   * id; segment block membership is immutable during coalescing, so the index
   * stays valid even though colors are updated in place. */
  size_t count = a->segment_len;
  const size_t *indices = NULL;
  if (by_block && target->block < a->block_count) {
    indices = by_block[target->block].data;
    count = by_block[target->block].len;
  }
  for (size_t k = 0; k < count; ++k) {
    const ny_mach_live_segment_t *s = &a->segments[indices ? indices[k] : k];
    if (s == target)
      continue;
    if (indices == NULL && (s->block != target->block))
      continue;
    if (s->color != color)
      continue;
    if (!(s->end < target->start || target->end < s->start))
      return true;
  }
  return false;
}

static void coalesce_segments(const ny_mach_func_t *mach,
                              ny_mach_regalloc_t *a,
                              const size_vec_t *succ,
                              const size_vec_t *pred) {
  /* Index segments by block so the per-step conflict check scans only segments
   * in the relevant block instead of every segment.  Block membership is fixed
   * after coloring, so this index is built once and reused. */
  size_vec_t *by_block = NULL;
  if (mach->block_len > 0) {
    by_block = calloc(mach->block_len, sizeof(*by_block));
    if (by_block) {
      for (size_t i = 0; i < a->segment_len; ++i) {
        uint32_t b = a->segments[i].block;
        if (b >= mach->block_len)
          continue;
        if (!size_vec_push(&by_block[b], i)) {
          for (size_t k = 0; k < mach->block_len; ++k)
            free(by_block[k].data);
          free(by_block);
          by_block = NULL;
          break;
        }
      }
    }
  }

  for (size_t i = 0; i < a->segment_len; ++i) {
    ny_mach_live_segment_t *x = &a->segments[i];
    if (x->color < 0)
      continue;
    for (size_t j = 0; j < a->segment_len; ++j) {
      ny_mach_live_segment_t *y = &a->segments[j];
      if (y->vreg != x->vreg || y->color < 0 || y->start != x->end + 1)
        continue;
      if (x->block == y->block) {
        if (!block_has_color_conflict(a, y, x->color, by_block)) {
          y->color = x->color;
          x->spill = false;
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
        y->reload = false;
      }
    }
  }

  if (by_block) {
    for (size_t b = 0; b < mach->block_len; ++b)
      free(by_block[b].data);
    free(by_block);
  }
}

bool ny_mach_regalloc_build_class(const ny_mach_func_t *mach,
                                   ny_mach_reg_class_t reg_class,
                                   size_t color_count,
                                   ny_mach_regalloc_t *out);
bool ny_mach_regalloc_build_class(const ny_mach_func_t *mach,
                                   ny_mach_reg_class_t reg_class,
                                   size_t color_count,
                                   ny_mach_regalloc_t *out) __attribute__((used)) {
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
      if (!is_alloc_type(mach->vreg_types[v], reg_class) || first[v] == SIZE_MAX)
        continue;
      bool lin = bit_get(a.live_in + bi * words, v);
      bool lout = bit_get(a.live_out + bi * words, v);
      size_t start = first[v];
      size_t end = last[v];
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

  if (!color_segments(&a))
    goto fail;
  coalesce_segments(mach, &a, succ, pred);
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
