#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include "base/parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CSE: Common Subexpression Elimination (single linear scan)         */
/*                                                                    */
/* In SSA form, two pure scalar instructions computing the same       */
/* operation on the same SSA inputs produce the same result. We hash  */
/* (op,a,b,cmp,imm)                                                   */
/* and replace duplicates with COPYs.  The hash table is reset at     */
/* control-flow boundaries (CALL, LABEL, BR, BR_IF) and at STORE_I64  */
/* (which can alias memory).                                          */
/* ------------------------------------------------------------------ */

typedef struct {
  nyir_op_t op;
  int a, b;
  nyir_cmp_t cmp;
  int64_t imm;
  int dst;
} cse_entry_t;

#define CSE_STACK_TABLE_SIZE 256

static size_t cse_hash(nyir_op_t op, int a, int b, nyir_cmp_t cmp,
                       int64_t imm, size_t cap) {
  uint64_t h = (uint64_t)(unsigned)op * UINT64_C(2654435761);
  h ^= (uint64_t)(unsigned)(a + 1) * UINT64_C(1610612741);
  h ^= (uint64_t)(unsigned)(b + 1) * UINT64_C(805306457);
  h ^= (uint64_t)(unsigned)cmp * UINT64_C(402653189);
  h ^= (uint64_t)(uint32_t)(imm >> 32) * UINT64_C(2246822519);
  h ^= (uint64_t)(uint32_t)imm * UINT64_C(3266489917);
  return (size_t)(h % cap);
}
static bool cse_lookup(const cse_entry_t *table, nyir_op_t op, int a, int b,
                       nyir_cmp_t cmp, int64_t imm, size_t cap,
                       int *out_dst) {
  size_t idx = cse_hash(op, a, b, cmp, imm, cap);
  for (size_t probe = 0; probe < cap; ++probe) {
    const cse_entry_t *e = &table[idx];
    if (e->dst < 0)
      return false;
    if (e->op == op && e->a == a && e->b == b && e->cmp == cmp &&
        e->imm == imm) {
      *out_dst = e->dst;
      return true;
    }
    idx = (idx + 1) % cap;
  }
  return false;
}

static void cse_insert(cse_entry_t *table, nyir_op_t op, int a, int b,
                       nyir_cmp_t cmp, int64_t imm, size_t cap, int dst) {
  size_t idx = cse_hash(op, a, b, cmp, imm, cap);
  for (size_t probe = 0; probe < cap; ++probe) {
    cse_entry_t *e = &table[idx];
    if (e->dst < 0) {
      e->op = op;
      e->a = a;
      e->b = b;
      e->cmp = cmp;
      e->imm = imm;
      e->dst = dst;
      return;
    }
    /* Overwrite if same key (latest definition wins). */
    if (e->op == op && e->a == a && e->b == b && e->cmp == cmp &&
        e->imm == imm) {
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

typedef struct {
  nyir_func_t *f;
  const cse_range_t *ranges;
} cse_parallel_ctx_t;

static bool cse_is_foldable(nyir_op_t op) {
  switch (op) {
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
    return true;
  default:
    return false;
  }
}

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
    if (in->dst < 0 || !cse_is_foldable(in->op))
      continue;
    int existing = -1;
    if (cse_lookup(table, in->op, in->a, in->b, in->cmp, in->imm, cap,
                   &existing)) {
      in->op = NYIR_COPY;
      in->a = existing;
      in->b = -1;
      in->cmp = NYIR_CMP_EQ;
      in->imm = 0;
      in->symbol = NULL;
      in->effects = NYIR_EFFECT_NONE;
    } else {
      cse_insert(table, in->op, in->a, in->b, in->cmp, in->imm, cap,
                 in->dst);
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
      if (cse_lookup(table, in->op, -1, -1, NYIR_CMP_EQ, in->imm,
                     cap, &existing)) {
        int duplicate = in->dst;
        if (!nyir_replace_all_uses(f, duplicate, existing) ||
            !nyir_erase_instruction(f, i)) {
          if (table != stack_table)
            free(table);
          nyir_cfg_free(&cfg);
          return false;
        }
      } else {
        cse_insert(table, in->op, -1, -1, NYIR_CMP_EQ, in->imm, cap,
                   in->dst);
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
      boundary = op == NYIR_CALL || op == NYIR_LABEL ||
                 op == NYIR_BR || op == NYIR_BR_IF ||
                 op == NYIR_STORE_I64;
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
