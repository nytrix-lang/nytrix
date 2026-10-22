/*
 * Common-subexpression elimination: detects and deduplicates
 * repeated computations with identical operands within a basic block.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "base/parallel.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * CSE: Common Subexpression Elimination (single linear scan)
 *
 * In SSA form, two pure scalar instructions computing the same operation on
 * the same SSA inputs produce the same result. The key includes all six
 * inline operands, predicate, immediate, ABI flags, and symbol, so audited
 * pure calls can participate without conflating helpers or return classes.
 * Unknown calls and memory barriers split the scan because they may observe
 * or invalidate state.
 */
typedef struct {
  nyr_op_t op;
  int a, b, c, d, e, f;
  nyr_cmp_t cmp;
  int64_t imm;
  unsigned flags;
  unsigned effects;
  nyr_scalar_class_t scalar_class;
  uint8_t semantic_ownership;
  bool semantic_mutable;
  uint32_t alias_class;
  nyr_range_t range;
  uint64_t arg_scalar_classes[NYIR_CALL_MAX_ARGS / 16];
  uint64_t arg_ownership[NYIR_CALL_MAX_ARGS / 32];
  uint64_t arg_contracts[NYIR_CALL_MAX_ARGS / 32];
  const char *symbol;
  const int *extra_args;
  size_t extra_args_len;
  const uint32_t *arg_sizes;
  uint32_t ret_aggregate_size;
  uint8_t ret_aggregate_classes[2];
  int dst;
} cse_entry_t;

#define CSE_STACK_TABLE_SIZE 256

static uint64_t cse_hash_symbol(const char *symbol) {
  uint64_t h = UINT64_C(1469598103934665603);
  if (!symbol)
    return h;
  for (const unsigned char *p = (const unsigned char *)symbol; *p; ++p) {
    h ^= *p;
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static bool cse_is_commutative(nyr_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_MUL_I64 || op == NYIR_AND_I64 ||
         op == NYIR_OR_I64 || op == NYIR_XOR_I64 || op == NYIR_ADD_F64 ||
         op == NYIR_MUL_F64 || op == NYIR_ADD_F32 || op == NYIR_MUL_F32;
}

static size_t cse_hash(const nyr_inst_t *in, size_t cap) {
  uint64_t h = (uint64_t)(unsigned)in->op * UINT64_C(2654435761);
  int a = in->a, b = in->b;
  if (cse_is_commutative(in->op) && a > b) {
    int tmp = a;
    a = b;
    b = tmp;
  }
#define CSE_MIX(v, k) h ^= (uint64_t)(unsigned)((v) + 1) * UINT64_C(k)
  CSE_MIX(a, 1610612741);
  CSE_MIX(b, 805306457);
  CSE_MIX(in->c, 122949829);
  CSE_MIX(in->d, 433494437);
  CSE_MIX(in->e, 2971215073);
  CSE_MIX(in->f, 2777789003);
#undef CSE_MIX
  h ^= (uint64_t)in->cmp * UINT64_C(402653189);
  h ^= (uint64_t)(uint32_t)(in->imm >> 32) * UINT64_C(2246822519);
  h ^= (uint64_t)(uint32_t)in->imm * UINT64_C(3266489917);
  h ^= (uint64_t)in->flags * UINT64_C(668265263);
  h ^= (uint64_t)in->effects * UINT64_C(374761393);
  h ^= (uint64_t)in->scalar_class * UINT64_C(2246822519);
  h ^= (uint64_t)in->semantic_ownership * UINT64_C(3266489917);
  h ^= (uint64_t)in->semantic_mutable * UINT64_C(668265263);
  h ^= (uint64_t)in->alias_class * UINT64_C(2971215073);
  h ^= (uint64_t)in->range.has_min * UINT64_C(2777789003);
  h ^= (uint64_t)in->range.has_max * UINT64_C(2246822519);
  h ^= (uint64_t)in->range.min * UINT64_C(3266489917);
  h ^= (uint64_t)in->range.max * UINT64_C(668265263);
  for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 16; ++k)
    h ^= in->arg_scalar_classes[k] *
         (UINT64_C(374761393) + (uint64_t)k * UINT64_C(2971215073));
  for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
    h ^= (uint64_t)in->arg_ownership[k] *
         (UINT64_C(3266489917) + (uint64_t)k * UINT64_C(668265263));
  for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
    h ^= in->arg_contracts[k] *
         (UINT64_C(2246822519) + (uint64_t)k * UINT64_C(374761393));
  h ^= (uint64_t)in->ret_aggregate_size * UINT64_C(374761393);
  h ^= (uint64_t)in->ret_aggregate_classes[0] * UINT64_C(2246822519);
  h ^= (uint64_t)in->ret_aggregate_classes[1] * UINT64_C(3266489917);
  h ^= cse_hash_symbol(in->symbol);
  h ^= (uint64_t)in->extra_args_len * UINT64_C(374761393);
  for (size_t k = 0; k < in->extra_args_len; ++k)
    h ^= (uint64_t)(unsigned)(in->extra_args[k] + 1) *
         (UINT64_C(668265263) + (uint64_t)k * UINT64_C(2246822519));
  if (in->op == NYIR_CALL && in->imm > 0) {
    for (int64_t k = 0; k < in->imm; ++k) {
      uint32_t arg_size = in->arg_sizes ? in->arg_sizes[k] : 0u;
      h ^= (uint64_t)arg_size *
           (UINT64_C(374761393) + (uint64_t)k * UINT64_C(668265263));
    }
  }
  return (size_t)(h % cap);
}

static bool cse_symbol_equal(const char *a, const char *b) {
  if (a == b)
    return true;
  return a && b && strcmp(a, b) == 0;
}

static bool cse_same_key(const cse_entry_t *e, const nyr_inst_t *in) {
  if (!e || !in || e->op != in->op || e->c != in->c || e->d != in->d ||
      e->e != in->e || e->f != in->f || e->cmp != in->cmp ||
      e->imm != in->imm || e->flags != in->flags || e->effects != in->effects ||
      e->scalar_class != in->scalar_class ||
      e->semantic_ownership != in->semantic_ownership ||
      e->semantic_mutable != in->semantic_mutable ||
      e->alias_class != in->alias_class ||
      e->range.has_min != in->range.has_min ||
      e->range.has_max != in->range.has_max || e->range.min != in->range.min ||
      e->range.max != in->range.max ||
      e->ret_aggregate_size != in->ret_aggregate_size ||
      e->ret_aggregate_classes[0] != in->ret_aggregate_classes[0] ||
      e->ret_aggregate_classes[1] != in->ret_aggregate_classes[1] ||
      !cse_symbol_equal(e->symbol, in->symbol) ||
      e->extra_args_len != in->extra_args_len)
    return false;
  for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 16; ++k)
    if (e->arg_scalar_classes[k] != in->arg_scalar_classes[k])
      return false;
  if (memcmp(e->arg_ownership, in->arg_ownership, sizeof(e->arg_ownership)) !=
      0)
    return false;
  if (memcmp(e->arg_contracts, in->arg_contracts, sizeof(e->arg_contracts)) !=
      0)
    return false;

  bool op_match =
      (e->a == in->a && e->b == in->b) ||
      (cse_is_commutative(in->op) && e->a == in->b && e->b == in->a);
  if (!op_match)
    return false;
  for (size_t k = 0; k < in->extra_args_len; ++k)
    if (!e->extra_args || e->extra_args[k] != in->extra_args[k])
      return false;
  if (in->op == NYIR_CALL && in->imm > 0) {
    for (int64_t k = 0; k < in->imm; ++k) {
      uint32_t left = e->arg_sizes ? e->arg_sizes[k] : 0u;
      uint32_t right = in->arg_sizes ? in->arg_sizes[k] : 0u;
      if (left != right)
        return false;
    }
  }
  return true;
}

static bool cse_lookup(const cse_entry_t *table, const nyr_inst_t *in,
                       size_t cap, int *out_dst) {
  size_t idx = cse_hash(in, cap);
  for (size_t probe = 0; probe < cap; ++probe) {
    const cse_entry_t *e = &table[idx];
    if (e->dst < 0)
      return false;
    if (cse_same_key(e, in)) {
      *out_dst = e->dst;
      return true;
    }
    idx = (idx + 1) % cap;
  }
  return false;
}

static void cse_insert(cse_entry_t *table, const nyr_inst_t *in, size_t cap,
                       int dst) {
  size_t idx = cse_hash(in, cap);
  for (size_t probe = 0; probe < cap; ++probe) {
    cse_entry_t *e = &table[idx];
    if (e->dst < 0) {
      e->op = in->op;
      e->a = in->a;
      e->b = in->b;
      e->c = in->c;
      e->d = in->d;
      e->e = in->e;
      e->f = in->f;
      e->cmp = in->cmp;
      e->imm = in->imm;
      e->flags = in->flags;
      e->effects = in->effects;
      e->scalar_class = in->scalar_class;
      e->semantic_ownership = in->semantic_ownership;
      e->semantic_mutable = in->semantic_mutable;
      e->alias_class = in->alias_class;
      e->range = in->range;
      memcpy(e->arg_scalar_classes, in->arg_scalar_classes,
             sizeof(e->arg_scalar_classes));
      memcpy(e->arg_ownership, in->arg_ownership, sizeof(e->arg_ownership));
      memcpy(e->arg_contracts, in->arg_contracts, sizeof(e->arg_contracts));
      e->symbol = in->symbol;
      e->extra_args = in->extra_args;
      e->extra_args_len = in->extra_args_len;
      e->arg_sizes = in->arg_sizes;
      e->ret_aggregate_size = in->ret_aggregate_size;
      e->ret_aggregate_classes[0] = in->ret_aggregate_classes[0];
      e->ret_aggregate_classes[1] = in->ret_aggregate_classes[1];
      e->dst = dst;
      return;
    }
    if (cse_same_key(e, in)) {
      e->dst = dst;
      return;
    }
    idx = (idx + 1) % cap;
  }
}

static void cse_clear(cse_entry_t *table, size_t cap) {
  for (size_t i = 0; i < cap; ++i)
    table[i].dst = -1;
}

typedef struct {
  size_t begin;
  size_t end;
} cse_range_t;
static bool cse_is_foldable(const nyr_inst_t *in) {
  if (!in)
    return false;
  if (in->op == NYIR_CALL) {
    /*
     * Only CSE calls whose trusted summaries prove they cannot observe or
     * change ambient state.  In particular, READ_MEMORY is excluded because
     * summaries do not identify argument-reachable locations.
     */
    const unsigned safe_effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_LOCAL |
                                  NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
    return (in->flags & NYIR_INST_F_EFFECTS_KNOWN) != 0 &&
           (nyr_effective_effects(in) & ~safe_effects) == 0;
  }
  switch (in->op) {
  case NYIR_LOAD_LOCAL:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_CMP_I64:
  case NYIR_I64_TO_F64:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_CMP_F64:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_CMP_F32:
  case NYIR_SQRT_F64:
    return true;
  default:
    return false;
  }
}

typedef struct {
  nyr_func_t *f;
  const cse_range_t *ranges;
  const int *defs;
  int *representatives;
  size_t value_len;
} cse_parallel_ctx_t;

/*
 * Resolve values rewritten by CSE in this basic block. Each SSA definition
 * belongs to exactly one range; checking its definition index before reading
 * the shared representative table keeps parallel block scans independent.
 */
static int cse_resolve_value(const int *defs, const int *representatives,
                             const nyr_func_t *f, size_t value_len,
                             size_t begin, size_t end, int value) {
  /*
   * Mem2reg can leave a representation-preserving copy between equivalent
   * scalar expressions. Follow only same-block raw-i64 copies: tagged,
   * pointer, aggregate, and ownership-changing copies retain their distinct
   * contracts and are never canonicalized here.
   */
  for (size_t hops = 0; hops < end - begin; ++hops) {
    if (value < 0 || (size_t)value >= value_len)
      return value;
    int def = defs[value];
    if (def < 0 || (size_t)def < begin || (size_t)def >= end)
      return value;
    int representative = representatives[value];
    if (representative != value) {
      value = representative;
      continue;
    }
    const nyr_inst_t *copy = &f->data[def];
    if (copy->op != NYIR_COPY || copy->a < 0 ||
        copy->scalar_class != NYIR_SCALAR_I64_RAW ||
        (size_t)copy->a >= value_len)
      return value;
    int source_def = defs[copy->a];
    if (source_def < 0 || (size_t)source_def < begin ||
        (size_t)source_def >= end)
      return value;
    const nyr_inst_t *source = &f->data[source_def];
    if (source->scalar_class != copy->scalar_class ||
        source->semantic_ownership != copy->semantic_ownership ||
        source->semantic_mutable != copy->semantic_mutable ||
        source->alias_class != copy->alias_class)
      return value;
    value = copy->a;
  }
  return value;
}

static void cse_rewrite_operand(const int *defs, const int *representatives,
                                const nyr_func_t *f, size_t value_len,
                                size_t begin, size_t end, int *operand) {
  if (operand && *operand >= 0)
    *operand = cse_resolve_value(defs, representatives, f, value_len, begin,
                                 end, *operand);
}

static bool cse_run_range(nyr_func_t *f, size_t begin, size_t end,
                          const int *defs, int *representatives,
                          size_t value_len) {
  size_t len = end > begin ? end - begin : 0;
  if (!len)
    return true;
  cse_entry_t stack_table[CSE_STACK_TABLE_SIZE];
  cse_entry_t *table = stack_table;
  size_t cap = CSE_STACK_TABLE_SIZE;
  while (cap < len && cap <= SIZE_MAX / 2)
    cap *= 2;
  if (cap < len || cap > SIZE_MAX / sizeof(*table))
    return true;
  if (cap > CSE_STACK_TABLE_SIZE) {
    table = calloc(cap, sizeof(*table));
    if (!table)
      return true;
  }
  cse_clear(table, cap);
  for (size_t i = begin; i < end; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0)
      continue;
    if (!cse_is_foldable(in)) {
      continue;
    }
    cse_rewrite_operand(defs, representatives, f, value_len, begin, end,
                        &in->a);
    cse_rewrite_operand(defs, representatives, f, value_len, begin, end,
                        &in->b);
    cse_rewrite_operand(defs, representatives, f, value_len, begin, end,
                        &in->c);
    cse_rewrite_operand(defs, representatives, f, value_len, begin, end,
                        &in->d);
    cse_rewrite_operand(defs, representatives, f, value_len, begin, end,
                        &in->e);
    cse_rewrite_operand(defs, representatives, f, value_len, begin, end,
                        &in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      cse_rewrite_operand(defs, representatives, f, value_len, begin, end,
                          &in->extra_args[k]);
    int existing = -1;
    if (cse_lookup(table, in, cap, &existing)) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("cse",
                               "eliminate inst=%zu op=%s dst=v%d reuse=v%d "
                               "reason=identical-key-in-effect-stable-block",
                               i, nyr_op_name(in->op), in->dst, existing);
      nir_make_copy(in, existing);
      representatives[in->dst] = existing;
    } else {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("cse",
                               "retain inst=%zu op=%s dst=v%d "
                               "reason=first-key-in-effect-stable-block",
                               i, nyr_op_name(in->op), in->dst);
      cse_insert(table, in, cap, in->dst);
    }
  }
  if (table != stack_table)
    free(table);
  return true;
}

static bool cse_parallel_task(size_t index, void *opaque) {
  cse_parallel_ctx_t *ctx = (cse_parallel_ctx_t *)opaque;
  return cse_run_range(ctx->f, ctx->ranges[index].begin, ctx->ranges[index].end,
                       ctx->defs, ctx->representatives, ctx->value_len);
}

bool nyr_cse(nyr_func_t *f) {
  if (!f || f->next_value <= 0 || f->len == 0)
    return true;
  size_t value_len = (size_t)f->next_value;
  if (value_len > SIZE_MAX / (2 * sizeof(int)))
    return false;
  int value_maps_stack[CSE_STACK_TABLE_SIZE * 2];
  int *value_maps = value_maps_stack;
  if (value_len > CSE_STACK_TABLE_SIZE) {
    value_maps = malloc(value_len * 2 * sizeof(*value_maps));
    if (!value_maps)
      return false;
  }
  int *defs = value_maps;
  int *representatives = value_maps + value_len;
  for (size_t i = 0; i < value_len; ++i) {
    defs[i] = -1;
    representatives[i] = (int)i;
  }
  for (size_t i = 0; i < f->len; ++i) {
    int dst = f->data[i].dst;
    if (dst >= 0 && (size_t)dst < value_len)
      defs[dst] = (int)i;
  }
  cse_range_t *ranges = calloc(f->len + 1, sizeof(*ranges));
  if (!ranges) {
    if (value_maps != value_maps_stack)
      free(value_maps);
    return false;
  }
  size_t count = 0;
  size_t begin = 0;
  for (size_t i = 0; i <= f->len; ++i) {
    bool boundary = i == f->len;
    if (!boundary) {
      nyr_op_t op = f->data[i].op;
      boundary = op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF;
      if (!boundary) {
        unsigned effects = nyr_effective_effects(&f->data[i]);
        const unsigned invalidates =
            NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_WRITE_MEMORY |
            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
            NYIR_EFFECT_FFI | NYIR_EFFECT_FENV |
            NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
        boundary = (effects & invalidates) != 0;
      }
    }
    if (!boundary)
      continue;
    if (i > begin)
      ranges[count++] = (cse_range_t){begin, i};
    begin = i + 1;
  }
  cse_parallel_ctx_t ctx = {f, ranges, defs, representatives, value_len};
  bool ok = ny_parallel_for(count, f->len, cse_parallel_task, &ctx);
  free(ranges);
  if (value_maps != value_maps_stack)
    free(value_maps);
  return ok;
}
