/*
 * Common-subexpression elimination: detects and deduplicates
 * repeated computations with identical operands within a basic block.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include "base/parallel.h"
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
  nyir_op_t op;
  int a, b, c, d, e, f;
  nyir_cmp_t cmp;
  int64_t imm;
  unsigned flags;
  const char *symbol;
  const int *extra_args;
  size_t extra_args_len;
  const uint32_t *arg_sizes;
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

static bool cse_is_commutative(nyir_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_MUL_I64 ||
         op == NYIR_AND_I64 || op == NYIR_OR_I64 ||
         op == NYIR_XOR_I64 || op == NYIR_ADD_F64 ||
         op == NYIR_MUL_F64 || op == NYIR_ADD_F32 ||
         op == NYIR_MUL_F32;
}

static size_t cse_hash(const nyir_inst_t *in, size_t cap) {
  uint64_t h = (uint64_t)(unsigned)in->op * UINT64_C(2654435761);
  int a = in->a, b = in->b;
  if (cse_is_commutative(in->op) && a > b) {
    int tmp = a; a = b; b = tmp;
  }
#define CSE_MIX(v, k) h ^= (uint64_t)(unsigned)((v) + 1) * UINT64_C(k)
  CSE_MIX(a, 1610612741); CSE_MIX(b, 805306457);
  CSE_MIX(in->c, 122949829); CSE_MIX(in->d, 433494437);
  CSE_MIX(in->e, 2971215073); CSE_MIX(in->f, 2777789003);
#undef CSE_MIX
  h ^= (uint64_t)in->cmp * UINT64_C(402653189);
  h ^= (uint64_t)(uint32_t)(in->imm >> 32) * UINT64_C(2246822519);
  h ^= (uint64_t)(uint32_t)in->imm * UINT64_C(3266489917);
  h ^= (uint64_t)in->flags * UINT64_C(668265263);
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

static bool cse_same_key(const cse_entry_t *e, const nyir_inst_t *in) {
  if (!e || !in || e->op != in->op ||
      e->c != in->c || e->d != in->d || e->e != in->e || e->f != in->f ||
      e->cmp != in->cmp || e->imm != in->imm || e->flags != in->flags ||
      !cse_symbol_equal(e->symbol, in->symbol) ||
      e->extra_args_len != in->extra_args_len)
    return false;
  
  bool op_match = (e->a == in->a && e->b == in->b) || 
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

static bool cse_lookup(const cse_entry_t *table, const nyir_inst_t *in,
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

static void cse_insert(cse_entry_t *table, const nyir_inst_t *in,
                       size_t cap, int dst) {
  size_t idx = cse_hash(in, cap);
  for (size_t probe = 0; probe < cap; ++probe) {
    cse_entry_t *e = &table[idx];
    if (e->dst < 0) {
      e->op = in->op;
      e->a = in->a; e->b = in->b; e->c = in->c;
      e->d = in->d; e->e = in->e; e->f = in->f;
      e->cmp = in->cmp;
      e->imm = in->imm;
      e->flags = in->flags;
      e->symbol = in->symbol;
      e->extra_args = in->extra_args;
      e->extra_args_len = in->extra_args_len;
      e->arg_sizes = in->arg_sizes;
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
static bool cse_is_foldable(const nyir_inst_t *in) {
  if (!in)
    return false;
  if (in->op == NYIR_CALL) {
    unsigned effects = nyir_call_effect_summary(in);
    /*
     * Deterministic read-only helpers may be CSE'd inside one effect-stable
     * block.  MAY_TRAP is not itself a blocker: the dominating first call
     * would already have trapped on that path.  State-changing, identity-
     * producing, externally observable, FFI/thread, and unknown calls remain
     * ineligible.
     */
    const unsigned blockers = NYIR_EFFECT_WRITE_LOCAL |
                              NYIR_EFFECT_WRITE_MEMORY |
                              NYIR_EFFECT_ALLOCATION |
                              NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
                              NYIR_EFFECT_FFI | NYIR_EFFECT_FENV |
                              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
    return (effects & NYIR_EFFECT_CALL) != 0 && (effects & blockers) == 0;
  }
  switch (in->op) {
  case NYIR_ADD_I64: case NYIR_SUB_I64: case NYIR_MUL_I64:
  case NYIR_DIV_I64: case NYIR_MOD_I64: case NYIR_AND_I64:
  case NYIR_OR_I64: case NYIR_XOR_I64: case NYIR_SHL_I64:
  case NYIR_SAR_I64: case NYIR_CMP_I64:
  case NYIR_I64_TO_F64: case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32: case NYIR_F32_TO_F64:
  case NYIR_ADD_F64: case NYIR_SUB_F64: case NYIR_MUL_F64:
  case NYIR_DIV_F64: case NYIR_CMP_F64:
  case NYIR_ADD_F32: case NYIR_SUB_F32: case NYIR_MUL_F32:
  case NYIR_DIV_F32: case NYIR_CMP_F32:
  case NYIR_SQRT_F64:
    return true;
  default:
    return false;
  }
}


typedef struct {
  nyir_func_t *f;
  const cse_range_t *ranges;
} cse_parallel_ctx_t;

static bool cse_run_range(nyir_func_t *f, size_t begin, size_t end) {
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
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || !cse_is_foldable(in))
      continue;
    int existing = -1;
    if (cse_lookup(table, in, cap, &existing)) {
      nir_make_copy(in, existing);
    } else {
      cse_insert(table, in, cap, in->dst);
    }
  }
  if (table != stack_table)
    free(table);
  return true;
}

static bool cse_parallel_task(size_t index, void *opaque) {
  cse_parallel_ctx_t *ctx = (cse_parallel_ctx_t *)opaque;
  return cse_run_range(ctx->f, ctx->ranges[index].begin,
                       ctx->ranges[index].end);
}

static bool cse_is_constant(nyir_op_t op) {
  return op == NYIR_CONST_I64 || op == NYIR_CONST_F64 ||
         op == NYIR_CONST_F32;
}

static bool cse_canonicalize_constants(nyir_func_t *f) {
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build_topology(f, &cfg))
    return false;
  for (size_t block = 0; block < cfg.block_count; ++block) {
    size_t len = cfg.block_end[block] - cfg.block_start[block];
    size_t cap = CSE_STACK_TABLE_SIZE;
    while (cap < len && cap <= SIZE_MAX / 2)
      cap *= 2;
    cse_entry_t stack_table[CSE_STACK_TABLE_SIZE];
    cse_entry_t *table = stack_table;
    if (cap > CSE_STACK_TABLE_SIZE) {
      table = calloc(cap, sizeof(*table));
      if (!table) {
        nyir_cfg_free(&cfg);
        return false;
      }
    }
    cse_clear(table, cap);
    for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
      nyir_inst_t *in = &f->data[i];
      if (in->dst < 0 || !cse_is_constant(in->op))
        continue;
      int existing = -1;
      if (cse_lookup(table, in, cap, &existing)) {
        int duplicate = in->dst;
        if (!nyir_replace_all_uses(f, duplicate, existing) ||
            !nyir_erase_instruction(f, i)) {
          if (table != stack_table)
            free(table);
          nyir_cfg_free(&cfg);
          return false;
        }
      } else {
        cse_insert(table, in, cap, in->dst);
      }
    }
    if (table != stack_table)
      free(table);
  }
  nyir_cfg_free(&cfg);
  return true;
}

bool nyir_cse(nyir_func_t *f) {
  if (!f || f->next_value <= 0 || f->len == 0)
    return true;
  if (!cse_canonicalize_constants(f))
    return false;
  cse_range_t *ranges = calloc(f->len + 1, sizeof(*ranges));
  if (!ranges)
    return false;
  size_t count = 0;
  size_t begin = 0;
  for (size_t i = 0; i <= f->len; ++i) {
    bool boundary = i == f->len;
    if (!boundary) {
      nyir_op_t op = f->data[i].op;
      boundary = op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF;
      if (!boundary) {
        unsigned effects = nyir_effective_effects(&f->data[i]);
        const unsigned invalidates = NYIR_EFFECT_WRITE_LOCAL |
                                     NYIR_EFFECT_WRITE_MEMORY |
                                     NYIR_EFFECT_ALLOCATION |
                                     NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
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
  cse_parallel_ctx_t ctx = {f, ranges};
  bool ok = ny_parallel_for(count, f->len, cse_parallel_task, &ctx);
  free(ranges);
  return ok;
}
