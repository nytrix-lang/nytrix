/*
 * Block-local dead-store elimination for heap and vector stores.
 *
 * A store S1 is dead when a later store S2 in the same block writes the same
 * address with the same opcode (same width) and nothing between them can
 * observe S1:
 *
 *   - no call or instruction with unknown, volatile, I/O, thread or FFI
 *     effects (they may read any memory);
 *   - every memory read in between provably addresses a different object.
 *
 * Object identity comes from provenance: an address that reaches, through
 * COPY and base-pointer ADD/SUB, the result of a typed-buffer constructor
 * call or an ALLOCA names that fresh allocation; anything else is unknown
 * and blocks the proof. Addresses are compared structurally through copies,
 * constants, and pure integer arithmetic.
 *
 * The motivating shape is an unrolled `C[i] = C[i] + a_k * B[k][i]` update,
 * which stores the running sum after every term although only the last
 * store is observable when `B` and `C` are distinct buffers.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

static const nyr_inst_t *md_def(const nyr_func_t *f, const int *defs, int v) {
  if (v < 0 || v >= f->next_value || defs[v] < 0)
    return NULL;
  return &f->data[defs[v]];
}

static int md_strip(const nyr_func_t *f, const int *defs, int v) {
  for (int depth = 0; depth < 32; ++depth) {
    const nyr_inst_t *d = md_def(f, defs, v);
    if (!d || d->op != NYIR_COPY || d->a < 0)
      return v;
    v = d->a;
  }
  return v;
}

/*
 * Allocation root of an address, or -1 when unknown.
 */
static int md_root(const nyr_func_t *f, const int *defs, int v) {
  for (int depth = 0; depth < 64; ++depth) {
    v = md_strip(f, defs, v);
    const nyr_inst_t *d = md_def(f, defs, v);
    if (!d)
      return -1;
    if (d->op == NYIR_ALLOCA)
      return v;
    if (d->op == NYIR_CALL && d->symbol &&
        strcmp(d->symbol, "rt_tbuf_new_typed_raw") == 0)
      return v;
    if ((d->op == NYIR_ADD_I64 || d->op == NYIR_SUB_I64) && d->a >= 0) {
      v = d->a;
      continue;
    }
    return -1;
  }
  return -1;
}

static bool md_same(const nyr_func_t *f, const int *defs, int x, int y,
                    int depth) {
  if (depth > 16)
    return false;
  x = md_strip(f, defs, x);
  y = md_strip(f, defs, y);
  if (x == y)
    return x >= 0;
  const nyr_inst_t *a = md_def(f, defs, x), *b = md_def(f, defs, y);
  if (!a || !b || a->op != b->op)
    return false;
  switch (a->op) {
  case NYIR_CONST_I64:
    return a->imm == b->imm;
  case NYIR_ADD_I64:
  case NYIR_MUL_I64:
    return (md_same(f, defs, a->a, b->a, depth + 1) &&
            md_same(f, defs, a->b, b->b, depth + 1)) ||
           (md_same(f, defs, a->a, b->b, depth + 1) &&
            md_same(f, defs, a->b, b->a, depth + 1));
  case NYIR_SUB_I64:
  case NYIR_SHL_I64:
    return md_same(f, defs, a->a, b->a, depth + 1) &&
           md_same(f, defs, a->b, b->b, depth + 1);
  default:
    return false;
  }
}

static bool md_is_store(nyr_op_t op) {
  return op == NYIR_STORE_I64 || op == NYIR_VEC4_STORE_F64 ||
         op == NYIR_VEC8_STORE_F32;
}

static bool md_is_load(nyr_op_t op) {
  return op == NYIR_LOAD_I64 || op == NYIR_VEC4_LOAD_F64 ||
         op == NYIR_VEC8_LOAD_F32 || op == NYIR_VEC4_LOAD_I64 ||
         op == NYIR_VEC8_LOAD_I64;
}

static const unsigned MD_BARRIER =
    NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO |
    NYIR_EFFECT_THREAD | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT |
    NYIR_EFFECT_ALLOCATION;

bool nyr_mem_dse(nyr_func_t *f) {
  if (!f || !f->len || getenv("NYTRIX_NO_MEM_DSE"))
    return true;
  int *defs = nyr_build_defs(f);
  if (!defs)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *s1 = &f->data[i];
    if (!md_is_store(s1->op) || s1->a < 0 || s1->flags != 0 ||
        (nyr_effective_effects(s1) & MD_BARRIER))
      continue;
    int root = md_root(f, defs, s1->a);
    if (root < 0)
      continue;
    for (size_t j = i + 1; j < f->len; ++j) {
      const nyr_inst_t *in = &f->data[j];
      if (in->op == NYIR_NOP)
        continue;
      if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
          in->op == NYIR_RET || in->op == NYIR_PHI)
        break; /* block boundary */
      if (in->op == s1->op && in->flags == 0 &&
          md_same(f, defs, in->a, s1->a, 0)) {
        nyr_inst_discard(&f->data[i]);
        break;
      }
      unsigned eff = nyr_effective_effects(in);
      if ((eff & MD_BARRIER) || in->op == NYIR_CALL)
        break;
      bool reads = md_is_load(in->op) || (eff & NYIR_EFFECT_READ_MEMORY);
      if (reads) {
        if (!md_is_load(in->op) || in->a < 0)
          break;
        int other = md_root(f, defs, in->a);
        if (other < 0 || other == root)
          break;
      }
    }
  }
  free(defs);
  return true;
}
