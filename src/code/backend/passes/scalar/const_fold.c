/*
 * Constant folding: evaluates compile-time-known arithmetic and
 * bitwise expressions in NYIR to replace ops with constant results.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "base/parallel.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  nyr_func_t *f;
  const bool *known;
  const int64_t *value;
  const bool *fknown;
  const double *fvalue;
} nir_constfold_parallel_ctx_t;

typedef struct {
  int64_t id;
  size_t index;
  size_t incoming;
} nir_constfold_label_t;

static int nir_constfold_label_compare(const void *left, const void *right) {
  const nir_constfold_label_t *a = (const nir_constfold_label_t *)left;
  const nir_constfold_label_t *b = (const nir_constfold_label_t *)right;
  return (a->id > b->id) - (a->id < b->id);
}

static nir_constfold_label_t *
nir_constfold_find_label(nir_constfold_label_t *labels, size_t count,
                         int64_t id) {
  nir_constfold_label_t key = {.id = id};
  return bsearch(&key, labels, count, sizeof(*labels),
                 nir_constfold_label_compare);
}

static bool nir_constfold_binary(const nyr_inst_t *in, int64_t lhs, int64_t rhs,
                                 int64_t *out) {
  if (in->op == NYIR_INLINE_ASM && in->imm == -23 && in->symbol &&
      strcmp(in->symbol, "unsigned-shift-right") == 0) {
    *out = (int64_t)((uint64_t)lhs >> ((uint64_t)rhs & 63u));
    return true;
  }
  return nyr_analyze_binary_fold(in->op, lhs, rhs, out);
}

/*
 * Preserve local constants through a direct unconditional jump to its
 * immediately following label only when that jump is the label's sole
 * predecessor. This keeps simple compiler-generated boolean temporaries
 * constant without carrying path-specific facts across CFG joins.
 */
static bool nir_constfold_local_bridges(const nyr_func_t *f, bool *bridges) {
  size_t label_count = 0;
  for (size_t i = 0; i < f->len; ++i)
    label_count += f->data[i].op == NYIR_LABEL;
  if (!label_count)
    return true;
  nir_constfold_label_t *labels =
      (nir_constfold_label_t *)calloc(label_count, sizeof(*labels));
  if (!labels)
    return false;
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LABEL)
      labels[count++] =
          (nir_constfold_label_t){.id = f->data[i].imm, .index = i};
  }
  qsort(labels, count, sizeof(*labels), nir_constfold_label_compare);
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR && in->op != NYIR_BR_IF)
      continue;
    nir_constfold_label_t *label =
        nir_constfold_find_label(labels, count, in->imm);
    if (label)
      ++label->incoming;
  }
  for (size_t i = 1; i < f->len; ++i) {
    if (f->data[i].op != NYIR_LABEL)
      continue;
    size_t branch = i - 1;
    while (branch > 0 && f->data[branch].op == NYIR_NOP)
      --branch;
    const nyr_inst_t *in = &f->data[branch];
    if (in->op != NYIR_BR || in->imm != f->data[i].imm)
      continue;
    nir_constfold_label_t *label =
        nir_constfold_find_label(labels, count, in->imm);
    if (label && label->index == i && label->incoming == 1) {
      bridges[branch] = true;
      bridges[i] = true;
    }
  }
  free(labels);
  return true;
}

static bool nir_constfold_parallel_task(size_t i, void *opaque) {
  nir_constfold_parallel_ctx_t *ctx = (nir_constfold_parallel_ctx_t *)opaque;
  nyr_inst_t *in = &ctx->f->data[i];
  if (in->dst < 0)
    return true;
  if (in->a >= 0 && in->b >= 0 && ctx->known[in->a] && ctx->known[in->b]) {
    int64_t folded = 0;
    if (nir_constfold_binary(in, ctx->value[in->a], ctx->value[in->b],
                             &folded) ||
        (in->op == NYIR_CMP_I64 &&
         nyr_analyze_cmp_fold(in->cmp, ctx->value[in->a], ctx->value[in->b],
                              &folded))) {
      nyr_inst_t folded_inst = {
          .op = NYIR_CONST_I64, .dst = in->dst, .imm = folded};
      NY_DBG_INST_FMT(in, &folded_inst,
                      "const_fold: %s %" PRId64 " %" PRId64 " -> %" PRId64,
                      nyr_op_name(in->op), ctx->value[in->a], ctx->value[in->b],
                      folded);
      nir_make_const(in, folded);
      return true;
    }
  }
  if (in->a >= 0 && in->b >= 0 && ctx->fknown[in->a] && ctx->fknown[in->b]) {
    double av = ctx->fvalue[in->a], bv = ctx->fvalue[in->b];
    bool f32 = in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
               in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32;
    if (f32 || in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
        in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64) {
      int64_t ab = 0, bb = 0, out = 0;
      unsigned char out_kind = 0;
      unsigned char kind = f32 ? 3 : 2;
      if (f32) {
        float af = (float)av, bf = (float)bv;
        uint32_t au = 0, bu = 0;
        memcpy(&au, &af, sizeof(au));
        memcpy(&bu, &bf, sizeof(bu));
        ab = (int64_t)au;
        bb = (int64_t)bu;
      } else {
        memcpy(&ab, &av, sizeof(av));
        memcpy(&bb, &bv, sizeof(bv));
      }
      if (nyr_float_fold_binary(in->op, ab, bb, kind, &out, &out_kind)) {
        nyr_inst_t float_inst = {.op = out_kind == 3 ? NYIR_CONST_F32
                                                     : NYIR_CONST_F64,
                                 .dst = in->dst,
                                 .imm = out};
        NY_DBG_INST_FMT(in, &float_inst, "const_fold: %s float -> %s",
                        nyr_op_name(in->op),
                        out_kind == 3 ? "CONST_F32" : "CONST_F64");
        if (out_kind == 3)
          nir_make_f32_const(in, out);
        else
          nir_make_f64_const(in, out);
      }
    }
  }
  return true;
}

static bool nir_constfold_parallel_pure(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t n = (size_t)f->next_value;
  bool stk_known[256] = {0}, stk_fknown[256] = {0};
  int64_t stk_value[256] = {0};
  double stk_fvalue[256] = {0};
  bool *known = n <= 256 ? stk_known : calloc(n, sizeof(*known));
  int64_t *value = n <= 256 ? stk_value : calloc(n, sizeof(*value));
  bool *fknown = n <= 256 ? stk_fknown : calloc(n, sizeof(*fknown));
  double *fvalue = n <= 256 ? stk_fvalue : calloc(n, sizeof(*fvalue));
  if (!known || !value || !fknown || !fvalue) {
    if (n > 256) {
      free(known);
      free(value);
      free(fknown);
      free(fvalue);
    }
    return false;
  }
  if (!nir_collect_consts(f, known, value)) {
    if (n > 256) {
      free(known);
      free(value);
      free(fknown);
      free(fvalue);
    }
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst < 0)
      continue;
    if (in->op == NYIR_CONST_F64) {
      fknown[in->dst] = true;
      memcpy(&fvalue[in->dst], &in->imm, sizeof(double));
    } else if (in->op == NYIR_CONST_F32) {
      uint32_t u = (uint32_t)(unsigned)in->imm;
      float v = 0;
      memcpy(&v, &u, sizeof(v));
      fknown[in->dst] = true;
      fvalue[in->dst] = (double)v;
    }
  }
  nir_constfold_parallel_ctx_t ctx = {f, known, value, fknown, fvalue};
  /*
   * Emit candidate explanations serially before the parallel rewrite pass.
   * The shared debug sink performs deduplication and is not a worker-thread
   * API; tracing here keeps diagnostics race-free and reports the exact
   * pre-rewrite operands.
   */
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC)) {
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst < 0 || in->a < 0 || in->b < 0)
        continue;
      if (known[in->a] && known[in->b]) {
        int64_t folded = 0;
        bool can_fold =
            nir_constfold_binary(in, value[in->a], value[in->b], &folded) ||
            (in->op == NYIR_CMP_I64 &&
             nyr_analyze_cmp_fold(in->cmp, value[in->a], value[in->b],
                                  &folded));
        if (can_fold)
          ny_dbg_heuristic_event(
              "CONST_FOLD",
              "decision=fold op=%s lhs=%lld rhs=%lld result=%lld "
              "reason=both-operands-constant-and-operation-is-defined",
              nyr_op_name(in->op), (long long)value[in->a],
              (long long)value[in->b], (long long)folded);
        else
          ny_dbg_heuristic_event("CONST_FOLD",
                                 "decision=keep op=%s lhs=%lld rhs=%lld "
                                 "reason=operation-not-safely-foldable",
                                 nyr_op_name(in->op), (long long)value[in->a],
                                 (long long)value[in->b]);
      } else if (known[in->a] || known[in->b]) {
        ny_dbg_heuristic_event(
            "CONST_FOLD",
            "decision=keep op=%s lhs_constant=%s rhs_constant=%s "
            "reason=needs-two-constant-operands",
            nyr_op_name(in->op), known[in->a] ? "yes" : "no",
            known[in->b] ? "yes" : "no");
      }
    }
  }
  bool ok = ny_parallel_for(f->len, f->len, nir_constfold_parallel_task, &ctx);
  if (n > 256) {
    free(known);
    free(value);
    free(fknown);
    free(fvalue);
  }
  return ok;
}

bool nyr_const_fold(nyr_func_t *f) {
  if (!f || !f->next_value)
    return true;
  if (!nir_constfold_parallel_pure(f))
    return false;
  size_t nv = (size_t)f->next_value;
  bool *local_bridges =
      (bool *)calloc(f->len ? f->len : 1, sizeof(*local_bridges));
  if (!local_bridges || !nir_constfold_local_bridges(f, local_bridges)) {
    free(local_bridges);
    return false;
  }
  bool stk_known[256] = {0}, stk_fknown[256] = {0};
  int64_t stk_value[256] = {0};
  double stk_fvalue[256] = {0};
  /*
   * Constant kind per value: 1 = i64, 2 = f64, 3 = f32.  CONST_F64/F32
   * store their IEEE bit pattern in imm, so a value that is "known" as an
   * i64 word is NOT necessarily an integer constant.  Rewrites below must
   * preserve the kind or downstream consumers (the machine-form encoder
   * types operands from the defining opcode) will numerically convert f64
   * bit patterns and corrupt every float arithmetic the fold touches.
   */
  unsigned char stk_vkind[256] = {0};
  unsigned char *vkind = nv <= 256 ? stk_vkind : (unsigned char *)calloc(nv, 1);
  bool *known = nv <= 256 ? stk_known : (bool *)calloc(nv, sizeof(bool));
  int64_t *value =
      nv <= 256 ? stk_value : (int64_t *)calloc(nv, sizeof(int64_t));
  bool *fknown = nv <= 256 ? stk_fknown : (bool *)calloc(nv, sizeof(bool));
  double *fvalue =
      nv <= 256 ? stk_fvalue : (double *)calloc(nv, sizeof(double));
  size_t max_local = nyr_max_local(f);
  bool stk_lknown[64] = {0}, stk_laddr[64] = {0};
  int64_t stk_lval[64] = {0};
  unsigned char stk_lkind[64] = {0};
  bool *local_known = NULL;
  bool *local_addr_taken = NULL;
  int64_t *local_value = NULL;
  unsigned char *local_kind = NULL;
  if (max_local > 0) {
    local_known =
        max_local <= 64 ? stk_lknown : (bool *)calloc(max_local, sizeof(bool));
    local_addr_taken =
        max_local <= 64 ? stk_laddr : (bool *)calloc(max_local, sizeof(bool));
    local_value = max_local <= 64
                      ? stk_lval
                      : (int64_t *)calloc(max_local, sizeof(int64_t));
    local_kind =
        max_local <= 64 ? stk_lkind : (unsigned char *)calloc(max_local, 1);
  }
  if (!known || !value || !fknown || !fvalue || !vkind ||
      (max_local > 0 &&
       (!local_known || !local_addr_taken || !local_value || !local_kind))) {
    free(local_bridges);
    if (nv > 256) {
      free(known);
      free(value);
      free(fknown);
      free(fvalue);
      free(vkind);
    }
    if (max_local > 64) {
      free(local_known);
      free(local_addr_taken);
      free(local_value);
      free(local_kind);
    }
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
      vkind[in->dst] = 1;
      continue;
    }
    if (in->op == NYIR_CONST_F64 && in->dst >= 0) {
      double dv;
      memcpy(&dv, &in->imm, sizeof(dv));
      fknown[in->dst] = true;
      fvalue[in->dst] = dv;
      vkind[in->dst] = 2;
      continue;
    }
    if (in->op == NYIR_CONST_F32 && in->dst >= 0) {
      float fv;
      uint32_t u = (uint32_t)(unsigned)in->imm;
      memcpy(&fv, &u, sizeof(fv));
      fknown[in->dst] = true;
      fvalue[in->dst] = (double)fv;
      vkind[in->dst] = 3;
      continue;
    }
    if (in->op == NYIR_I64_TO_F64 && in->dst >= 0 && in->a >= 0 &&
        known[in->a]) {
      double dv = (double)value[in->a];
      int64_t bits;
      memcpy(&bits, &dv, sizeof(bits));
      nir_make_f64_const(in, bits);
      fknown[in->dst] = true;
      fvalue[in->dst] = dv;
      vkind[in->dst] = 2;
      known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_I64_TO_F32 && in->dst >= 0 && in->a >= 0 &&
        known[in->a]) {
      float fv = (float)value[in->a];
      uint32_t u;
      memcpy(&u, &fv, sizeof(u));
      nir_make_f32_const(in, (int64_t)u);
      fknown[in->dst] = true;
      fvalue[in->dst] = (double)fv;
      vkind[in->dst] = 3;
      known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_F64_TO_F32 && in->dst >= 0 && in->a >= 0 &&
        fknown[in->a]) {
      float fv = (float)fvalue[in->a];
      uint32_t u;
      memcpy(&u, &fv, sizeof(u));
      nir_make_f32_const(in, (int64_t)u);
      fknown[in->dst] = true;
      fvalue[in->dst] = (double)fv;
      vkind[in->dst] = 3;
      known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_F32_TO_F64 && in->dst >= 0 && in->a >= 0 &&
        fknown[in->a]) {
      double dv = fvalue[in->a];
      int64_t bits;
      memcpy(&bits, &dv, sizeof(bits));
      nir_make_f64_const(in, bits);
      fknown[in->dst] = true;
      fvalue[in->dst] = dv;
      vkind[in->dst] = 2;
      known[in->dst] = false;
      continue;
    }
    /*
     * Float binary folding via shared helper.
     */
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && fknown[in->a] &&
        fknown[in->b]) {
      double av = fvalue[in->a], bv = fvalue[in->b];
      bool is_f32 = (in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
                     in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32);
      unsigned char kind = is_f32 ? 3 : 2;
      int64_t a_bits = 0, b_bits = 0;
      if (is_f32) {
        float af = (float)av, bf = (float)bv;
        uint32_t au = 0, bu = 0;
        memcpy(&au, &af, sizeof(au));
        memcpy(&bu, &bf, sizeof(bu));
        a_bits = (int64_t)au;
        b_bits = (int64_t)bu;
      } else {
        memcpy(&a_bits, &av, sizeof(av));
        memcpy(&b_bits, &bv, sizeof(bv));
      }
      int64_t out_imm = 0;
      unsigned char out_kind = 0;
      if (nyr_float_fold_binary(in->op, a_bits, b_bits, kind, &out_imm,
                                &out_kind)) {
        if (out_kind == 3) {
          nir_make_f32_const(in, out_imm);
          uint32_t u = (uint32_t)(unsigned)out_imm;
          float rf;
          memcpy(&rf, &u, sizeof(rf));
          fknown[in->dst] = true;
          fvalue[in->dst] = (double)rf;
          vkind[in->dst] = 3;
        } else {
          nir_make_f64_const(in, out_imm);
          double rd;
          memcpy(&rd, &out_imm, sizeof(rd));
          fknown[in->dst] = true;
          fvalue[in->dst] = rd;
          vkind[in->dst] = 2;
        }
        known[in->dst] = false;
        continue;
      }
    }
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && fknown[in->a] &&
        fknown[in->b] && (in->op == NYIR_CMP_F64 || in->op == NYIR_CMP_F32)) {
      double av = fvalue[in->a];
      double bv = fvalue[in->b];
      if (in->op == NYIR_CMP_F32) {
        av = (double)(float)av;
        bv = (double)(float)bv;
      }
      int64_t result = 0;
      switch (in->cmp) {
      case NYIR_CMP_EQ:
        result = av == bv;
        break;
      case NYIR_CMP_NE:
        result = av != bv;
        break;
      case NYIR_CMP_LT:
        result = av < bv;
        break;
      case NYIR_CMP_LE:
        result = av <= bv;
        break;
      case NYIR_CMP_GT:
        result = av > bv;
        break;
      case NYIR_CMP_GE:
        result = av >= bv;
        break;
      }
      nir_make_const(in, result);
      known[in->dst] = true;
      value[in->dst] = result;
      fknown[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_COPY && in->dst >= 0) {
      if (in->a >= 0 && known[in->a]) {
        nir_make_const(in, value[in->a]);
        known[in->dst] = true;
        value[in->dst] = in->imm;
      } else {
        known[in->dst] = false;
      }
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < max_local) {
      if (in->a >= 0 && known[in->a]) {
        local_known[in->imm] = true;
        local_value[in->imm] = value[in->a];
        local_kind[in->imm] = 1;
      } else if (in->a >= 0 && fknown[in->a]) {
        /*
         * The stored word is an IEEE bit pattern, not an integer.  Record
         * the constant kind so the load below rewrites to CONST_F64/F32 —
         * rewriting it as CONST_I64 made the machine-form encoder read the
         * pattern as an integer and numerically convert it (1.0 became
         * 4.60718e+18 in every float expression the fold touched).
         */
        local_known[in->imm] = true;
        local_kind[in->imm] = vkind[in->a];
        if (vkind[in->a] == 2) {
          double dv = fvalue[in->a];
          memcpy(&local_value[in->imm], &dv, sizeof(dv));
        } else if (vkind[in->a] == 3) {
          float fv = (float)fvalue[in->a];
          uint32_t u;
          memcpy(&u, &fv, sizeof(u));
          local_value[in->imm] = (int64_t)u;
        } else {
          local_known[in->imm] = false;
          local_kind[in->imm] = 0;
        }
      } else {
        local_known[in->imm] = false;
        local_kind[in->imm] = 0;
      }
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
        (size_t)in->imm < max_local && local_known[in->imm]) {
      if (local_kind[in->imm] == 2) {
        nir_make_f64_const(in, local_value[in->imm]);
        fknown[in->dst] = true;
        double dv;
        memcpy(&dv, &in->imm, sizeof(dv));
        fvalue[in->dst] = dv;
        vkind[in->dst] = 2;
        known[in->dst] = false;
      } else if (local_kind[in->imm] == 3) {
        nir_make_f32_const(in, local_value[in->imm]);
        fknown[in->dst] = true;
        float fv;
        uint32_t u = (uint32_t)(unsigned)in->imm;
        memcpy(&fv, &u, sizeof(fv));
        fvalue[in->dst] = (double)fv;
        vkind[in->dst] = 3;
        known[in->dst] = false;
      } else {
        nir_make_const(in, local_value[in->imm]);
        known[in->dst] = true;
        value[in->dst] = in->imm;
        vkind[in->dst] = 1;
      }
      continue;
    }
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < max_local) {
      local_addr_taken[in->imm] = true;
      local_known[in->imm] = false;
      if (in->dst >= 0)
        known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_STORE_I64 && local_known && local_addr_taken) {
      for (size_t local = 0; local < max_local; ++local) {
        if (local_addr_taken[local])
          local_known[local] = false;
      }
    }
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_CALL) {
      if (local_known && !local_bridges[i])
        memset(local_known, 0, max_local * sizeof(bool));
    }
    int64_t folded = 0;
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && known[in->a] &&
        known[in->b] &&
        (nir_constfold_binary(in, value[in->a], value[in->b], &folded) ||
         (in->op == NYIR_CMP_I64 &&
          nyr_analyze_cmp_fold(in->cmp, value[in->a], value[in->b],
                               &folded)))) {
      nyr_inst_t folded_inst = {
          .op = NYIR_CONST_I64, .dst = in->dst, .imm = folded};
      NY_DBG_INST_FMT(in, &folded_inst,
                      "const_fold: %s %" PRId64 " %" PRId64 " -> %" PRId64,
                      nyr_op_name(in->op), value[in->a], value[in->b], folded);
      nir_make_const(in, folded);
      known[in->dst] = true;
      value[in->dst] = folded;
      continue;
    }
    if (in->dst >= 0)
      known[in->dst] = false;
  }
  if (nv > 256) {
    free(known);
    free(value);
    free(fknown);
    free(fvalue);
    free(vkind);
  }
  if (max_local > 64) {
    free(local_known);
    free(local_addr_taken);
    free(local_value);
    free(local_kind);
  }
  free(local_bridges);
  return true;
}

static bool nyr_sccp_fold_float(nyr_op_t op, int64_t a_imm, int64_t b_imm,
                                unsigned char a_kind, unsigned char b_kind,
                                int64_t *out_imm, unsigned char *out_kind) {
  if (a_kind != b_kind)
    return false;
  return nyr_float_fold_binary(op, a_imm, b_imm, a_kind, out_imm, out_kind);
}

static bool nyr_sccp_fold_float_cmp(nyr_op_t op, nyr_cmp_t cmp, int64_t a_imm,
                                    int64_t b_imm, unsigned char a_kind,
                                    unsigned char b_kind, int64_t *out) {
  if ((op != NYIR_CMP_F64 && op != NYIR_CMP_F32) || a_kind != b_kind ||
      (a_kind != 2 && a_kind != 3))
    return false;
  double av, bv;
  if (a_kind == 3) {
    uint32_t au = (uint32_t)(unsigned)a_imm;
    uint32_t bu = (uint32_t)(unsigned)b_imm;
    float af, bf;
    memcpy(&af, &au, sizeof(af));
    memcpy(&bf, &bu, sizeof(bf));
    av = (double)af;
    bv = (double)bf;
  } else {
    memcpy(&av, &a_imm, sizeof(av));
    memcpy(&bv, &b_imm, sizeof(bv));
  }
  bool res = false;
  switch (cmp) {
  case NYIR_CMP_EQ:
    res = av == bv;
    break;
  case NYIR_CMP_NE:
    res = av != bv;
    break;
  case NYIR_CMP_LT:
    res = av < bv;
    break;
  case NYIR_CMP_LE:
    res = av <= bv;
    break;
  case NYIR_CMP_GT:
    res = av > bv;
    break;
  case NYIR_CMP_GE:
    res = av >= bv;
    break;
  }
  *out = res ? 1 : 0;
  return true;
}

bool nyr_sccp(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build_topology(f, &cfg))
    return false;
  enum { UNSEEN, INTEGER, F64, F32, VARYING };
  size_t values = (size_t)f->next_value;
  int64_t *constant = calloc(values, sizeof(*constant));
  unsigned char *constant_kind = calloc(values, sizeof(*constant_kind));
  bool *executable = calloc(cfg.block_count, sizeof(*executable));
  size_t edge_count = cfg.succ_offsets[cfg.block_count];
  bool *executable_edge = calloc(edge_count, sizeof(*executable_edge));
  if (!constant || !constant_kind || !executable ||
      (edge_count && !executable_edge)) {
    free(constant);
    free(constant_kind);
    free(executable);
    free(executable_edge);
    nyr_cfg_free(&cfg);
    return false;
  }
  executable[0] = true;
  bool changed;
  do {
    changed = false;
    for (size_t block = 0; block < cfg.block_count; ++block) {
      if (!executable[block])
        continue;
      for (size_t i = cfg.block_start[block]; i < cfg.block_end[block]; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->dst < 0 || (size_t)in->dst >= values)
          continue;
        int64_t value = 0;
        unsigned char kind = VARYING;
        if (in->op == NYIR_CONST_I64) {
          value = in->imm;
          kind = INTEGER;
        } else if (in->op == NYIR_CONST_F64 || in->op == NYIR_CONST_F32) {
          value = in->imm;
          kind = in->op == NYIR_CONST_F64 ? F64 : F32;
        } else if (in->op == NYIR_COPY && in->a >= 0 &&
                   (size_t)in->a < values) {
          value = constant[in->a];
          kind = constant_kind[in->a];
        } else if (in->op == NYIR_PHI) {
          kind = UNSEEN;
          for (size_t k = 0; k < in->phi_incoming_len; ++k) {
            size_t pred = cfg.block_count;
            for (size_t edge = cfg.pred_offsets[block];
                 edge < cfg.pred_offsets[block + 1]; ++edge) {
              size_t candidate = cfg.pred_blocks[edge];
              if (cfg.block_label[candidate] ==
                  in->phi_incoming[k].predecessor_label) {
                pred = candidate;
                break;
              }
            }
            int source = in->phi_incoming[k].value;
            bool edge_live = false;
            if (pred < cfg.block_count)
              for (size_t edge = cfg.succ_offsets[pred];
                   edge < cfg.succ_offsets[pred + 1]; ++edge)
                if (cfg.succ_blocks[edge] == block && executable_edge[edge]) {
                  edge_live = true;
                  break;
                }
            if (!edge_live)
              continue;
            unsigned char incoming = source >= 0 && (size_t)source < values
                                         ? constant_kind[source]
                                         : VARYING;
            if (incoming == UNSEEN)
              continue;
            if (incoming == VARYING ||
                (kind != UNSEEN &&
                 (kind != incoming || value != constant[source]))) {
              kind = VARYING;
              break;
            }
            value = constant[source];
            kind = incoming;
          }
        } else if (in->a >= 0 && in->b >= 0 && (size_t)in->a < values &&
                   (size_t)in->b < values) {
          unsigned char a = constant_kind[in->a], b = constant_kind[in->b];
          if (a == UNSEEN || b == UNSEEN) {
            kind = UNSEEN;
          } else if (a == INTEGER && b == INTEGER) {
            if (nir_constfold_binary(in, constant[in->a], constant[in->b],
                                     &value) ||
                (in->op == NYIR_CMP_I64 &&
                 nyr_analyze_cmp_fold(in->cmp, constant[in->a], constant[in->b],
                                      &value)))
              kind = INTEGER;
          } else if (a == b && (a == F32 || a == F64)) {
            if (nyr_sccp_fold_float(in->op, constant[in->a], constant[in->b], a,
                                    b, &value, &kind)) {
              /*
               * kind is set by the fold.
               */
            } else if (nyr_sccp_fold_float_cmp(in->op, in->cmp, constant[in->a],
                                               constant[in->b], a, b, &value))
              kind = INTEGER;
          }
        }
        /*
         * An executable predecessor may be discovered after a PHI and its
         * consumers were visited. Merge facts monotonically so those
         * consumers also become varying instead of retaining stale values.
         */
        unsigned char old = constant_kind[in->dst];
        if (old == VARYING || kind == UNSEEN)
          continue;
        if (old != UNSEEN && (old != kind || constant[in->dst] != value))
          kind = VARYING;
        if (old != kind) {
          constant[in->dst] = value;
          constant_kind[in->dst] = kind;
          changed = true;
        }
      }
      size_t end = cfg.block_end[block];
      const nyr_inst_t *term =
          end > cfg.block_start[block] ? &f->data[end - 1] : NULL;
      for (size_t edge = cfg.succ_offsets[block];
           edge < cfg.succ_offsets[block + 1]; ++edge) {
        size_t target = cfg.succ_blocks[edge];
        bool take = true;
        if (term && term->op == NYIR_BR_IF && term->a >= 0 &&
            (size_t)term->a < values && constant_kind[term->a] == INTEGER &&
            cfg.succ_offsets[block + 1] - cfg.succ_offsets[block] > 1)
          take = target == cfg.succ_blocks[cfg.succ_offsets[block]]
                     ? constant[term->a] != 0
                     : constant[term->a] == 0;
        if (take && !executable[target]) {
          executable[target] = true;
          changed = true;
        }
        if (take && !executable_edge[edge]) {
          executable_edge[edge] = true;
          changed = true;
        }
      }
    }
  } while (changed);
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst >= 0 && (size_t)in->dst < values &&
        constant_kind[in->dst] > UNSEEN && constant_kind[in->dst] < VARYING &&
        (in->op == NYIR_COPY || in->op == NYIR_ADD_I64 ||
         in->op == NYIR_SUB_I64 || in->op == NYIR_MUL_I64 ||
         in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64 ||
         in->op == NYIR_AND_I64 || in->op == NYIR_OR_I64 ||
         in->op == NYIR_XOR_I64 || in->op == NYIR_SHL_I64 ||
         in->op == NYIR_SAR_I64 || in->op == NYIR_CMP_I64 ||
         in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
         in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
         in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
         in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32 ||
         in->op == NYIR_CMP_F64 || in->op == NYIR_CMP_F32)) {
      if (constant_kind[in->dst] == F64)
        nir_make_f64_const(in, constant[in->dst]);
      else if (constant_kind[in->dst] == F32)
        nir_make_f32_const(in, constant[in->dst]);
      else
        nir_make_const(in, constant[in->dst]);
    }
  }
  free(constant);
  free(constant_kind);
  free(executable);
  free(executable_edge);
  nyr_cfg_free(&cfg);
  return true;
}
