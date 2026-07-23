#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "base/common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif

/* Real SMT translation validation for pure i64 straight-line NYIR.
 * With Z3: prove before ≡ after for all free local inputs (bitvector).
 * Without Z3 / with branches: multi-input interpreter differential. */

static bool nyir_has_cfg_or_phi(const nyir_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_op_t op = f->data[i].op;
    if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_LABEL ||
        op == NYIR_PHI)
      return true;
  }
  return false;
}

#ifdef NYTRIX_HAS_Z3
static bool nyir_smt_eval_straightline(Z3_context z3, Z3_sort bv,
                                         const nyir_func_t *f,
                                         Z3_ast *locals, size_t nloc,
                                         Z3_ast *out_ret) {
  Z3_ast *vals = NULL;
  Z3_ast *loc = NULL;
  if (f->next_value > 0) {
    vals = (Z3_ast *)calloc((size_t)f->next_value, sizeof(Z3_ast));
    if (!vals)
      return false;
  }
  if (nloc) {
    loc = (Z3_ast *)calloc(nloc, sizeof(Z3_ast));
    if (!loc) {
      free(vals);
      return false;
    }
    for (size_t i = 0; i < nloc; ++i)
      loc[i] = locals[i];
  }
  *out_ret = Z3_mk_int64(z3, 0, bv);
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    Z3_ast a = Z3_mk_int64(z3, 0, bv);
    Z3_ast b = Z3_mk_int64(z3, 0, bv);
    if (in->a >= 0 && vals)
      a = vals[in->a];
    if (in->b >= 0 && vals)
      b = vals[in->b];
    Z3_ast r = a;
    switch (in->op) {
    case NYIR_CONST_I64:
      r = Z3_mk_int64(z3, in->imm, bv);
      break;
    case NYIR_COPY:
      r = a;
      break;
    case NYIR_ADD_I64:
      r = Z3_mk_bvadd(z3, a, b);
      break;
    case NYIR_SUB_I64:
      r = Z3_mk_bvsub(z3, a, b);
      break;
    case NYIR_MUL_I64:
      r = Z3_mk_bvmul(z3, a, b);
      break;
    case NYIR_DIV_I64:
      r = Z3_mk_bvsdiv(z3, a, b);
      break;
    case NYIR_MOD_I64:
      r = Z3_mk_bvsrem(z3, a, b);
      break;
    case NYIR_AND_I64:
      r = Z3_mk_bvand(z3, a, b);
      break;
    case NYIR_OR_I64:
      r = Z3_mk_bvor(z3, a, b);
      break;
    case NYIR_XOR_I64:
      r = Z3_mk_bvxor(z3, a, b);
      break;
    case NYIR_SHL_I64:
      r = Z3_mk_bvshl(z3, a, b);
      break;
    case NYIR_SAR_I64:
      r = Z3_mk_bvashr(z3, a, b);
      break;
    case NYIR_CMP_I64: {
      Z3_ast c = Z3_mk_true(z3);
      switch (in->cmp) {
      case NYIR_CMP_EQ:
        c = Z3_mk_eq(z3, a, b);
        break;
      case NYIR_CMP_NE:
        c = Z3_mk_not(z3, Z3_mk_eq(z3, a, b));
        break;
      case NYIR_CMP_LT:
        c = Z3_mk_bvslt(z3, a, b);
        break;
      case NYIR_CMP_LE:
        c = Z3_mk_bvsle(z3, a, b);
        break;
      case NYIR_CMP_GT:
        c = Z3_mk_bvsgt(z3, a, b);
        break;
      case NYIR_CMP_GE:
        c = Z3_mk_bvsge(z3, a, b);
        break;
      }
      r = Z3_mk_ite(z3, c, Z3_mk_int64(z3, 1, bv), Z3_mk_int64(z3, 0, bv));
      break;
    }
    case NYIR_LOAD_LOCAL:
      if (in->imm >= 0 && (size_t)in->imm < nloc && loc)
        r = loc[in->imm];
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm >= 0 && (size_t)in->imm < nloc && loc && in->a >= 0 && vals)
        loc[in->imm] = vals[in->a];
      continue;
    case NYIR_RET:
      if (in->a >= 0 && vals)
        *out_ret = vals[in->a];
      free(vals);
      free(loc);
      return true;
    case NYIR_NOP:
      continue;
    default:
      free(vals);
      free(loc);
      return false;
    }
    if (in->dst >= 0 && vals)
      vals[in->dst] = r;
  }
  free(vals);
  free(loc);
  return true;
}
#endif

#ifdef NYTRIX_HAS_Z3
typedef struct {
  Z3_ast *values;
  Z3_ast *locals;
  Z3_ast path;
  size_t pc;
  size_t steps;
  unsigned backedges;
  int64_t current_label;
  int64_t predecessor_label;
} ny_smt_path_t;

typedef struct {
  Z3_ast valid;
  Z3_ast value;
  size_t paths;
  bool complete;
} ny_smt_result_t;

static Z3_ast ny_z3_and2(Z3_context z3, Z3_ast a, Z3_ast b) {
  Z3_ast xs[2] = {a, b};
  return Z3_mk_and(z3, 2, xs);
}

static Z3_ast ny_z3_or2(Z3_context z3, Z3_ast a, Z3_ast b) {
  Z3_ast xs[2] = {a, b};
  return Z3_mk_or(z3, 2, xs);
}

static Z3_ast ny_z3_nonzero(Z3_context z3, Z3_sort bv, Z3_ast v) {
  return Z3_mk_not(z3, Z3_mk_eq(z3, v, Z3_mk_int64(z3, 0, bv)));
}

static bool ny_smt_clone_path(const nyir_func_t *f, size_t nloc,
                              const ny_smt_path_t *src, ny_smt_path_t *dst) {
  *dst = *src;
  if (f->next_value > 0) {
    dst->values = malloc((size_t)f->next_value * sizeof(*dst->values));
    if (!dst->values)
      return false;
    memcpy(dst->values, src->values,
           (size_t)f->next_value * sizeof(*dst->values));
  }
  if (nloc) {
    dst->locals = malloc(nloc * sizeof(*dst->locals));
    if (!dst->locals) {
      free(dst->values);
      dst->values = NULL;
      return false;
    }
    memcpy(dst->locals, src->locals, nloc * sizeof(*dst->locals));
  }
  return true;
}

static void ny_smt_free_path(ny_smt_path_t *path) {
  if (!path)
    return;
  free(path->values);
  free(path->locals);
  path->values = NULL;
  path->locals = NULL;
}

static size_t ny_smt_label_pc(const nyir_func_t *f, int64_t label) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == label)
      return i;
  return SIZE_MAX;
}

static bool ny_smt_record_return(Z3_context z3, ny_smt_result_t *out,
                                 Z3_ast path, Z3_ast value) {
  if (!out || out->paths >= 4096)
    return false;
  out->value = Z3_mk_ite(z3, path, value, out->value);
  out->valid = ny_z3_or2(z3, out->valid, path);
  out->paths++;
  return true;
}

static bool nyir_smt_walk_cfg(Z3_context z3, Z3_sort bv,
                                const nyir_func_t *f, size_t nloc,
                                ny_smt_path_t *st, ny_smt_result_t *out) {
  const size_t step_limit = f->len * 12u + 64u;
  while (st->pc < f->len) {
    if (++st->steps > step_limit) {
      out->complete = false;
      return true;
    }
    const nyir_inst_t *in = &f->data[st->pc++];
    Z3_ast zero = Z3_mk_int64(z3, 0, bv);
    Z3_ast one = Z3_mk_int64(z3, 1, bv);
    Z3_ast a = zero, b = zero, r = zero;
    if (in->a >= 0) {
      if (!st->values || !st->values[in->a])
        return false;
      a = st->values[in->a];
    }
    if (in->b >= 0) {
      if (!st->values || !st->values[in->b])
        return false;
      b = st->values[in->b];
    }
    switch (in->op) {
    case NYIR_NOP:
      continue;
    case NYIR_LABEL:
      st->predecessor_label = st->current_label;
      st->current_label = in->imm;
      continue;
    case NYIR_PHI: {
      bool found = false;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        if (in->phi_incoming[k].predecessor_label != st->predecessor_label)
          continue;
        int v = in->phi_incoming[k].value;
        if (v < 0 || !st->values || !st->values[v])
          return false;
        r = st->values[v];
        found = true;
        break;
      }
      if (!found)
        return false;
      break;
    }
    case NYIR_CONST_I64:
      r = Z3_mk_int64(z3, in->imm, bv);
      break;
    case NYIR_COPY:
      r = a;
      break;
    case NYIR_ADD_I64:
      r = Z3_mk_bvadd(z3, a, b);
      break;
    case NYIR_SUB_I64:
      r = Z3_mk_bvsub(z3, a, b);
      break;
    case NYIR_MUL_I64:
      r = Z3_mk_bvmul(z3, a, b);
      break;
    case NYIR_DIV_I64:
    case NYIR_MOD_I64:
      st->path = ny_z3_and2(z3, st->path, ny_z3_nonzero(z3, bv, b));
      r = in->op == NYIR_DIV_I64 ? Z3_mk_bvsdiv(z3, a, b)
                                      : Z3_mk_bvsrem(z3, a, b);
      break;
    case NYIR_AND_I64:
      r = Z3_mk_bvand(z3, a, b);
      break;
    case NYIR_OR_I64:
      r = Z3_mk_bvor(z3, a, b);
      break;
    case NYIR_XOR_I64:
      r = Z3_mk_bvxor(z3, a, b);
      break;
    case NYIR_SHL_I64:
      r = Z3_mk_bvshl(z3, a, b);
      break;
    case NYIR_SAR_I64:
      r = Z3_mk_bvashr(z3, a, b);
      break;
    case NYIR_CMP_I64: {
      Z3_ast c = Z3_mk_true(z3);
      switch (in->cmp) {
      case NYIR_CMP_EQ: c = Z3_mk_eq(z3, a, b); break;
      case NYIR_CMP_NE: c = Z3_mk_not(z3, Z3_mk_eq(z3, a, b)); break;
      case NYIR_CMP_LT: c = Z3_mk_bvslt(z3, a, b); break;
      case NYIR_CMP_LE: c = Z3_mk_bvsle(z3, a, b); break;
      case NYIR_CMP_GT: c = Z3_mk_bvsgt(z3, a, b); break;
      case NYIR_CMP_GE: c = Z3_mk_bvsge(z3, a, b); break;
      }
      r = Z3_mk_ite(z3, c, one, zero);
      break;
    }
    case NYIR_LOAD_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= nloc || !st->locals)
        return false;
      r = st->locals[in->imm];
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= nloc || !st->locals)
        return false;
      st->locals[in->imm] = a;
      continue;
    case NYIR_BR: {
      size_t target = ny_smt_label_pc(f, in->imm);
      if (target == SIZE_MAX)
        return false;
      if (target < st->pc && ++st->backedges > 8) {
        out->complete = false;
        return true;
      }
      st->pc = target;
      continue;
    }
    case NYIR_BR_IF: {
      size_t target = ny_smt_label_pc(f, in->imm);
      if (target == SIZE_MAX)
        return false;
      Z3_ast cond = ny_z3_nonzero(z3, bv, a);
      ny_smt_path_t taken = {0};
      if (!ny_smt_clone_path(f, nloc, st, &taken))
        return false;
      taken.path = ny_z3_and2(z3, taken.path, cond);
      if (target < st->pc && ++taken.backedges > 8)
        out->complete = false;
      else {
        taken.pc = target;
        if (!nyir_smt_walk_cfg(z3, bv, f, nloc, &taken, out)) {
          ny_smt_free_path(&taken);
          return false;
        }
      }
      ny_smt_free_path(&taken);
      st->path = ny_z3_and2(z3, st->path, Z3_mk_not(z3, cond));
      continue;
    }
    case NYIR_RET:
      return ny_smt_record_return(z3, out, st->path,
                                  in->a >= 0 ? a : zero);
    default:
      return false;
    }
    if (in->dst >= 0) {
      if (!st->values)
        return false;
      st->values[in->dst] = r;
    }
  }
  return true;
}

static bool nyir_smt_eval_cfg(Z3_context z3, Z3_sort bv,
                                const nyir_func_t *f, Z3_ast *locals,
                                size_t nloc, ny_smt_result_t *out) {
  *out = (ny_smt_result_t){.valid = Z3_mk_false(z3),
                           .value = Z3_mk_int64(z3, 0, bv),
                           .complete = true};
  ny_smt_path_t st = {.path = Z3_mk_true(z3),
                      .current_label = -1,
                      .predecessor_label = -1};
  if (f->next_value > 0) {
    st.values = calloc((size_t)f->next_value, sizeof(*st.values));
    if (!st.values)
      return false;
  }
  if (nloc) {
    st.locals = malloc(nloc * sizeof(*st.locals));
    if (!st.locals) {
      ny_smt_free_path(&st);
      return false;
    }
    memcpy(st.locals, locals, nloc * sizeof(*st.locals));
  }
  bool ok = nyir_smt_walk_cfg(z3, bv, f, nloc, &st, out);
  ny_smt_free_path(&st);
  return ok && out->paths > 0;
}
#endif


bool nyir_tv_smt_equiv(const nyir_func_t *before, const nyir_func_t *after,
                         char *err, size_t err_len) {
  if (!before || !after)
    return nyir_err(err, err_len, "SMT TV: missing function");
  if (!nyir_is_pure_i64_straightline(before) ||
      !nyir_is_pure_i64_straightline(after))
    return true;

  if (nyir_has_cfg_or_phi(before) || nyir_has_cfg_or_phi(after)) {
#ifndef NYTRIX_HAS_Z3
    return nyir_tv_equiv_straightline(before, after, 2048, err, err_len);
#else
    size_t n_before = nyir_max_local(before);
    size_t n_after = nyir_max_local(after);
    size_t nloc = n_before > n_after ? n_before : n_after;
    Z3_config cfg = Z3_mk_config();
    Z3_context z3 = Z3_mk_context(cfg);
    Z3_del_config(cfg);
    Z3_solver solver = Z3_mk_solver(z3);
    Z3_solver_inc_ref(z3, solver);
    Z3_sort bv = Z3_mk_bv_sort(z3, 64);
    Z3_ast *locals = nloc ? calloc(nloc, sizeof(*locals)) : NULL;
    if (nloc && !locals) {
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyir_err(err, err_len, "SMT CFG TV: OOM");
    }
    for (size_t i = 0; i < nloc; ++i) {
      char name[32];
      snprintf(name, sizeof(name), "L%zu", i);
      locals[i] = Z3_mk_const(z3, Z3_mk_string_symbol(z3, name), bv);
    }
    ny_smt_result_t rb = {0}, ra = {0};
    bool eb = nyir_smt_eval_cfg(z3, bv, before, locals, nloc, &rb);
    bool ea = nyir_smt_eval_cfg(z3, bv, after, locals, nloc, &ra);
    if (!eb || !ea) {
      free(locals);
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyir_tv_equiv_straightline(before, after, 2048, err, err_len);
    }
    Z3_ast valid_mismatch = Z3_mk_xor(z3, rb.valid, ra.valid);
    Z3_ast both_valid = ny_z3_and2(z3, rb.valid, ra.valid);
    Z3_ast value_mismatch = Z3_mk_not(z3, Z3_mk_eq(z3, rb.value, ra.value));
    Z3_ast bad_value = ny_z3_and2(z3, both_valid, value_mismatch);
    Z3_solver_assert(z3, solver, ny_z3_or2(z3, valid_mismatch, bad_value));
    Z3_lbool st = Z3_solver_check(z3, solver);
    bool ok = st == Z3_L_FALSE;
    if (st == Z3_L_TRUE)
      nyir_err(err, err_len, "SMT CFG TV: bounded counterexample");
    else if (st != Z3_L_FALSE)
      nyir_err(err, err_len, "SMT CFG TV: solver unknown");
    free(locals);
    Z3_solver_dec_ref(z3, solver);
    Z3_del_context(z3);
    return ok;
#endif
  }

#ifndef NYTRIX_HAS_Z3
  return nyir_tv_equiv_straightline(before, after, 64, err, err_len);
#else
  size_t n_before = nyir_max_local(before);
  size_t n_after = nyir_max_local(after);
  size_t nloc = n_before > n_after ? n_before : n_after;

  Z3_config cfg = Z3_mk_config();
  Z3_context z3 = Z3_mk_context(cfg);
  Z3_del_config(cfg);
  Z3_solver solver = Z3_mk_solver(z3);
  Z3_solver_inc_ref(z3, solver);
  Z3_sort bv = Z3_mk_bv_sort(z3, 64);

  Z3_ast *locals = NULL;
  if (nloc) {
    locals = (Z3_ast *)calloc(nloc, sizeof(Z3_ast));
    if (!locals) {
      Z3_solver_dec_ref(z3, solver);
      Z3_del_context(z3);
      return nyir_err(err, err_len, "SMT TV: OOM");
    }
    for (size_t i = 0; i < nloc; ++i) {
      char name[32];
      snprintf(name, sizeof(name), "L%zu", i);
      locals[i] = Z3_mk_const(z3, Z3_mk_string_symbol(z3, name), bv);
    }
  }

  Z3_ast ret_b = Z3_mk_int64(z3, 0, bv);
  Z3_ast ret_a = Z3_mk_int64(z3, 0, bv);
  bool ok_b =
      nyir_smt_eval_straightline(z3, bv, before, locals, nloc, &ret_b);
  bool ok_a =
      nyir_smt_eval_straightline(z3, bv, after, locals, nloc, &ret_a);
  if (!ok_b || !ok_a) {
    free(locals);
    Z3_solver_dec_ref(z3, solver);
    Z3_del_context(z3);
    return nyir_tv_equiv_straightline(before, after, 64, err, err_len);
  }

  /* Ask: exists inputs s.t. ret_before != ret_after? */
  Z3_solver_assert(z3, solver, Z3_mk_not(z3, Z3_mk_eq(z3, ret_b, ret_a)));
  Z3_lbool st = Z3_solver_check(z3, solver);
  bool ok = (st == Z3_L_FALSE);
  if (st == Z3_L_TRUE)
    nyir_err(err, err_len, "SMT TV: counterexample (before != after)");
  else if (st != Z3_L_FALSE)
    nyir_err(err, err_len, "SMT TV: solver unknown");

  free(locals);
  Z3_solver_dec_ref(z3, solver);
  Z3_del_context(z3);
  return ok;
#endif
}
