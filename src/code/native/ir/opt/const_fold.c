/*
 * Constant folding: evaluates compile-time-known arithmetic and
 * bitwise expressions in NYIR to replace ops with constant results.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include "base/parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
  nyir_func_t *f;
  const bool *known;
  const int64_t *value;
  const bool *fknown;
  const double *fvalue;
} nir_constfold_parallel_ctx_t;

static bool nir_constfold_parallel_task(size_t i, void *opaque) {
  nir_constfold_parallel_ctx_t *ctx = (nir_constfold_parallel_ctx_t *)opaque;
  nyir_inst_t *in = &ctx->f->data[i];
  if (in->dst < 0)
    return true;
  if (in->a >= 0 && in->b >= 0 && ctx->known[in->a] && ctx->known[in->b]) {
    int64_t folded = 0;
    if (nyir_analyze_binary_fold(in->op, ctx->value[in->a],
                                   ctx->value[in->b], &folded) ||
        (in->op == NYIR_CMP_I64 &&
         nyir_analyze_cmp_fold(in->cmp, ctx->value[in->a],
                                 ctx->value[in->b], &folded))) {
      in->op = NYIR_CONST_I64;
      in->imm = folded;
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      return true;
    }
  }
  if (in->a >= 0 && in->b >= 0 && ctx->fknown[in->a] &&
      ctx->fknown[in->b]) {
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
      if (nyir_float_fold_binary(in->op, ab, bb, kind, &out, &out_kind)) {
        in->op = out_kind == 3 ? NYIR_CONST_F32 : NYIR_CONST_F64;
        in->imm = out;
        in->a = -1;
        in->b = -1;
        in->symbol = NULL;
      }
    }
  }
  return true;
}

static bool nir_constfold_parallel_pure(nyir_func_t *f) {
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
    if (n > 256) { free(known); free(value); free(fknown); free(fvalue); }
    return false;
  }
  if (!nir_collect_consts(f, known, value)) {
    if (n > 256) { free(known); free(value); free(fknown); free(fvalue); }
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
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
  bool ok = ny_parallel_for(f->len, f->len, nir_constfold_parallel_task, &ctx);
  if (n > 256) { free(known); free(value); free(fknown); free(fvalue); }
  return ok;
}

bool nyir_const_fold(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  if (!nir_constfold_parallel_pure(f))
    return false;
  size_t nv = (size_t)f->next_value;
  bool stk_known[256] = {0}, stk_fknown[256] = {0};
  int64_t stk_value[256] = {0};
  double stk_fvalue[256] = {0};
  bool *known = nv <= 256 ? stk_known : (bool *)calloc(nv, sizeof(bool));
  int64_t *value = nv <= 256 ? stk_value : (int64_t *)calloc(nv, sizeof(int64_t));
  bool *fknown = nv <= 256 ? stk_fknown : (bool *)calloc(nv, sizeof(bool));
  double *fvalue = nv <= 256 ? stk_fvalue : (double *)calloc(nv, sizeof(double));
  size_t max_local = nyir_max_local(f);
  bool stk_lknown[64] = {0}, stk_laddr[64] = {0};
  int64_t stk_lval[64] = {0};
  bool *local_known = NULL;
  bool *local_addr_taken = NULL;
  int64_t *local_value = NULL;
  if (max_local > 0) {
    local_known = max_local <= 64 ? stk_lknown : (bool *)calloc(max_local, sizeof(bool));
    local_addr_taken = max_local <= 64 ? stk_laddr : (bool *)calloc(max_local, sizeof(bool));
    local_value = max_local <= 64 ? stk_lval : (int64_t *)calloc(max_local, sizeof(int64_t));
  }
  if (!known || !value || !fknown || !fvalue ||
      (max_local > 0 && (!local_known || !local_addr_taken || !local_value))) {
    if (nv > 256) { free(known); free(value); free(fknown); free(fvalue); }
    if (max_local > 64) { free(local_known); free(local_addr_taken); free(local_value); }
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
      continue;
    }
    if (in->op == NYIR_CONST_F64 && in->dst >= 0) {
      double dv;
      memcpy(&dv, &in->imm, sizeof(dv));
      fknown[in->dst] = true;
      fvalue[in->dst] = dv;
      continue;
    }
    if (in->op == NYIR_CONST_F32 && in->dst >= 0) {
      float fv;
      uint32_t u = (uint32_t)(unsigned)in->imm;
      memcpy(&fv, &u, sizeof(fv));
      fknown[in->dst] = true;
      fvalue[in->dst] = (double)fv;
      continue;
    }
    if (in->op == NYIR_I64_TO_F64 && in->dst >= 0 && in->a >= 0 && known[in->a]) {
      double dv = (double)value[in->a];
      in->op = NYIR_CONST_F64;
      memcpy(&in->imm, &dv, sizeof(dv));
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      fknown[in->dst] = true;
      fvalue[in->dst] = dv;
      known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_I64_TO_F32 && in->dst >= 0 && in->a >= 0 && known[in->a]) {
      float fv = (float)value[in->a];
      uint32_t u;
      memcpy(&u, &fv, sizeof(u));
      in->op = NYIR_CONST_F32;
      in->imm = (int64_t)u;
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      fknown[in->dst] = true;
      fvalue[in->dst] = (double)fv;
      known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_F64_TO_F32 && in->dst >= 0 && in->a >= 0 && fknown[in->a]) {
      float fv = (float)fvalue[in->a];
      uint32_t u;
      memcpy(&u, &fv, sizeof(u));
      in->op = NYIR_CONST_F32;
      in->imm = (int64_t)u;
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      fknown[in->dst] = true;
      fvalue[in->dst] = (double)fv;
      known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_F32_TO_F64 && in->dst >= 0 && in->a >= 0 && fknown[in->a]) {
      double dv = fvalue[in->a];
      in->op = NYIR_CONST_F64;
      memcpy(&in->imm, &dv, sizeof(dv));
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      fknown[in->dst] = true;
      fvalue[in->dst] = dv;
      known[in->dst] = false;
      continue;
    }
    /*
     * Float binary folding via shared helper.
     */
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && fknown[in->a] && fknown[in->b]) {
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
      if (nyir_float_fold_binary(in->op, a_bits, b_bits, kind, &out_imm,
                                   &out_kind)) {
        if (out_kind == 3) {
          in->op = NYIR_CONST_F32;
          in->imm = out_imm;
          uint32_t u = (uint32_t)(unsigned)out_imm;
          float rf;
          memcpy(&rf, &u, sizeof(rf));
          fknown[in->dst] = true;
          fvalue[in->dst] = (double)rf;
        } else {
          in->op = NYIR_CONST_F64;
          in->imm = out_imm;
          double rd;
          memcpy(&rd, &out_imm, sizeof(rd));
          fknown[in->dst] = true;
          fvalue[in->dst] = rd;
        }
        in->a = -1;
        in->b = -1;
        in->symbol = NULL;
        known[in->dst] = false;
        continue;
      }
    }
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && fknown[in->a] &&
        fknown[in->b] &&
        (in->op == NYIR_CMP_F64 || in->op == NYIR_CMP_F32)) {
      double av = fvalue[in->a];
      double bv = fvalue[in->b];
      if (in->op == NYIR_CMP_F32) {
        av = (double)(float)av;
        bv = (double)(float)bv;
      }
      int64_t result = 0;
      switch (in->cmp) {
      case NYIR_CMP_EQ: result = av == bv; break;
      case NYIR_CMP_NE: result = av != bv; break;
      case NYIR_CMP_LT: result = av < bv; break;
      case NYIR_CMP_LE: result = av <= bv; break;
      case NYIR_CMP_GT: result = av > bv; break;
      case NYIR_CMP_GE: result = av >= bv; break;
      }
      in->op = NYIR_CONST_I64;
      in->imm = result;
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      known[in->dst] = true;
      value[in->dst] = result;
      fknown[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_COPY && in->dst >= 0) {
      if (in->a >= 0 && known[in->a]) {
        in->op = NYIR_CONST_I64;
        in->imm = value[in->a];
        in->a = -1;
        in->b = -1;
        in->symbol = NULL;
        known[in->dst] = true;
        value[in->dst] = in->imm;
      } else {
        known[in->dst] = false;
      }
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < max_local) {
      if (in->a >= 0 && known[in->a]) {
        local_known[in->imm] = true;
        local_value[in->imm] = value[in->a];
      } else {
        local_known[in->imm] = false;
      }
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
        (size_t)in->imm < max_local && local_known[in->imm]) {
      in->op = NYIR_CONST_I64;
      in->a = -1;
      in->b = -1;
      in->imm = local_value[in->imm];
      in->symbol = NULL;
      known[in->dst] = true;
      value[in->dst] = in->imm;
      continue;
    }
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 && (size_t)in->imm < max_local) {
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
    if (in->op == NYIR_BR_IF && in->a >= 0 && known[in->a]) {
      if (value[in->a]) {
        /*
         * Taken branch: make unconditional, then kill fallthrough until a
         * label (e.g. the else `br L_else` immediately after br_if).
         */
        in->op = NYIR_BR;
        in->a = -1;
        in->b = -1;
        for (size_t j = i + 1; j < f->len && f->data[j].op != NYIR_LABEL;
             ++j) {
          if (f->data[j].op != NYIR_NOP)
            (void)nyir_erase_instruction(f, j);
        }
      } else {
        /*
         * Not taken: drop the conditional; fallthrough `br` stays live.
         */
        in->op = NYIR_NOP;
        in->a = -1;
        in->b = -1;
        in->imm = 0;
      }
    }
    if (in->op == NYIR_LABEL || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF || in->op == NYIR_CALL) {
      if (local_known)
        memset(local_known, 0, max_local * sizeof(bool));
    }
    int64_t folded = 0;
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && known[in->a] &&
        known[in->b] &&
        (nyir_analyze_binary_fold(in->op, value[in->a], value[in->b], &folded) ||
         (in->op == NYIR_CMP_I64 &&
          nyir_analyze_cmp_fold(in->cmp, value[in->a], value[in->b], &folded)))) {
      in->op = NYIR_CONST_I64;
      in->imm = folded;
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      known[in->dst] = true;
      value[in->dst] = folded;
      continue;
    }
    if (in->dst >= 0)
      known[in->dst] = false;
  }
  if (nv > 256) { free(known); free(value); free(fknown); free(fvalue); }
  if (max_local > 64) { free(local_known); free(local_addr_taken); free(local_value); }
  return true;
}

static bool nyir_sccp_fold_float(nyir_op_t op, int64_t a_imm,
                                   int64_t b_imm, unsigned char a_kind,
                                   unsigned char b_kind, int64_t *out_imm,
                                   unsigned char *out_kind) {
  if (a_kind != b_kind)
    return false;
  return nyir_float_fold_binary(op, a_imm, b_imm, a_kind, out_imm, out_kind);
}

static bool nyir_sccp_fold_float_cmp(nyir_op_t op, nyir_cmp_t cmp,
                                       int64_t a_imm, int64_t b_imm,
                                       unsigned char a_kind,
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
  case NYIR_CMP_EQ: res = av == bv; break;
  case NYIR_CMP_NE: res = av != bv; break;
  case NYIR_CMP_LT: res = av < bv; break;
  case NYIR_CMP_LE: res = av <= bv; break;
  case NYIR_CMP_GT: res = av > bv; break;
  case NYIR_CMP_GE: res = av >= bv; break;
  }
  *out = res ? 1 : 0;
  return true;
}

bool nyir_sccp(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build_topology(f, &cfg))
    return false;
  size_t values = (size_t)f->next_value;
  bool *known = calloc(values, sizeof(*known));
  int64_t *constant = calloc(values, sizeof(*constant));
  unsigned char *constant_kind = calloc(values, sizeof(*constant_kind));
  bool *executable = calloc(cfg.block_count, sizeof(*executable));
  size_t edge_count = cfg.succ_offsets[cfg.block_count];
  bool *executable_edge = calloc(edge_count, sizeof(*executable_edge));
  if (!known || !constant || !constant_kind || !executable || (edge_count && !executable_edge)) {
    free(known); free(constant); free(constant_kind); free(executable); free(executable_edge); nyir_cfg_free(&cfg);
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
        const nyir_inst_t *in = &f->data[i];
        if (in->dst < 0 || (size_t)in->dst >= values)
          continue;
        bool have = false;
        int64_t value = 0;
        unsigned char kind = 0;
        if (in->op == NYIR_CONST_I64) {
          have = true; value = in->imm; kind = 1;
        } else if (in->op == NYIR_CONST_F64 || in->op == NYIR_CONST_F32) {
          have = true; value = in->imm;
          kind = in->op == NYIR_CONST_F64 ? 2 : 3;
        } else if (in->op == NYIR_COPY && in->a >= 0 &&
                   (size_t)in->a < values && known[in->a]) {
          have = true; value = constant[in->a]; kind = constant_kind[in->a];
        } else if (in->op == NYIR_PHI) {
          bool loop_phi_ready = true;
          for (size_t pe = cfg.pred_offsets[block];
               pe < cfg.pred_offsets[block + 1]; ++pe) {
            size_t pred = cfg.pred_blocks[pe];
            if (pred < block)
              continue;
            bool live = false;
            for (size_t se = cfg.succ_offsets[pred];
                 se < cfg.succ_offsets[pred + 1]; ++se) {
              if (cfg.succ_blocks[se] == block && executable_edge[se]) {
                live = true;
                break;
              }
            }
            if (!live) {
              loop_phi_ready = false;
              break;
            }
          }
          bool first = true;
          bool overdefined = false;
          if (!loop_phi_ready)
            continue;
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
            if (source < 0 ||
                (size_t)source >= values || !known[source]) {
              overdefined = true;
              have = false;
              break;
            }
            if (first) { value = constant[source]; kind = constant_kind[source]; first = false; have = true; }
            else if (value != constant[source] || kind != constant_kind[source]) {
              overdefined = true;
              have = false;
              break;
            }
          }
          if (overdefined && known[in->dst]) {
            known[in->dst] = false;
            changed = true;
          }
        } else if (in->a >= 0 && in->b >= 0 && (size_t)in->a < values &&
                   (size_t)in->b < values && known[in->a] && known[in->b] &&
                   constant_kind[in->a] == 1 && constant_kind[in->b] == 1) {
          have = nyir_analyze_binary_fold(in->op, constant[in->a],
                                             constant[in->b], &value) ||
                 (in->op == NYIR_CMP_I64 &&
                  nyir_analyze_cmp_fold(in->cmp, constant[in->a],
                                          constant[in->b], &value));
          kind = 1;
        } else if (in->a >= 0 && in->b >= 0 && (size_t)in->a < values &&
                   (size_t)in->b < values && known[in->a] && known[in->b] &&
                   constant_kind[in->a] != 1 &&
                   constant_kind[in->a] == constant_kind[in->b]) {
          unsigned char fkind = 0;
          if (nyir_sccp_fold_float(in->op, constant[in->a], constant[in->b],
                                     constant_kind[in->a],
                                     constant_kind[in->b], &value, &fkind)) {
            have = true;
            kind = fkind;
          } else if (nyir_sccp_fold_float_cmp(in->op, in->cmp, constant[in->a],
                                                constant[in->b],
                                                constant_kind[in->a],
                                                constant_kind[in->b], &value)) {
            have = true;
            kind = 1;
          }
        }
        if (have && (!known[in->dst] || constant[in->dst] != value ||
                     constant_kind[in->dst] != kind)) {
          known[in->dst] = true;
          constant[in->dst] = value;
          constant_kind[in->dst] = kind;
          changed = true;
        }
      }
      size_t end = cfg.block_end[block];
      const nyir_inst_t *term = end > cfg.block_start[block] ?
          &f->data[end - 1] : NULL;
      for (size_t edge = cfg.succ_offsets[block];
           edge < cfg.succ_offsets[block + 1]; ++edge) {
        size_t target = cfg.succ_blocks[edge];
        bool take = true;
        if (term && term->op == NYIR_BR_IF && term->a >= 0 &&
            (size_t)term->a < values && known[term->a] &&
            constant_kind[term->a] == 1 &&
            cfg.succ_offsets[block + 1] - cfg.succ_offsets[block] > 1)
          take = target == cfg.succ_blocks[cfg.succ_offsets[block]]
                     ? constant[term->a] != 0 : constant[term->a] == 0;
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
    nyir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && (size_t)in->dst < values && known[in->dst] &&
        (in->op == NYIR_COPY ||
         in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64 ||
         in->op == NYIR_MUL_I64 || in->op == NYIR_DIV_I64 ||
         in->op == NYIR_MOD_I64 || in->op == NYIR_AND_I64 ||
         in->op == NYIR_OR_I64 || in->op == NYIR_XOR_I64 ||
         in->op == NYIR_SHL_I64 || in->op == NYIR_SAR_I64 ||
         in->op == NYIR_CMP_I64 ||
         in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
         in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
         in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
         in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32 ||
         in->op == NYIR_CMP_F64 || in->op == NYIR_CMP_F32)) {
      in->op = constant_kind[in->dst] == 2 ? NYIR_CONST_F64 :
               constant_kind[in->dst] == 3 ? NYIR_CONST_F32 : NYIR_CONST_I64;
      in->imm = constant[in->dst];
      in->a = in->b = -1; in->symbol = NULL;
    }
    if (in->op == NYIR_BR_IF && in->a >= 0 && (size_t)in->a < values &&
        known[in->a] && constant_kind[in->a] == 1) {
      if (constant[in->a]) { in->op = NYIR_BR; in->a = -1; }
      else { nyir_inst_discard(in); }
    }
  }
  free(known); free(constant); free(constant_kind); free(executable); free(executable_edge); nyir_cfg_free(&cfg);
  return true;
}
