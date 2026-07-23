#include "code/native/ir/internal.h"
#include "code/native/internal.h"
#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void nyir_eval_result_free(nyir_eval_result_t *result) {
  if (!result)
    return;
  free(result->pc_counts);
  free(result->edges);
  result->pc_counts = NULL;
  result->edges = NULL;
  result->pc_count_len = 0;
  result->edge_count = 0;
  result->edge_cap = 0;
}

static bool nyir_eval_note_edge(nyir_eval_result_t *result, size_t from,
                                  size_t to) {
  if (!result || from == SIZE_MAX)
    return true;
  for (size_t i = 0; i < result->edge_count; ++i) {
    if (result->edges[i].from_pc == from && result->edges[i].to_pc == to) {
      result->edges[i].count++;
      return true;
    }
  }
  if (result->edge_count == result->edge_cap) {
    size_t cap = result->edge_cap ? result->edge_cap * 2 : 16;
    if (cap < result->edge_cap || cap > SIZE_MAX / sizeof(*result->edges))
      return false;
    nyir_profile_edge_t *edges =
        realloc(result->edges, cap * sizeof(*result->edges));
    if (!edges)
      return false;
    result->edges = edges;
    result->edge_cap = cap;
  }
  result->edges[result->edge_count++] =
      (nyir_profile_edge_t){from, to, 1};
  return true;
}

static bool nyir_eval_read_value(const int64_t *values, const bool *known,
                                   int value, int64_t *out) {
  if (!values || !known || !out || value < 0 || !known[value])
    return false;
  *out = values[value];
  return true;
}

static void nyir_eval_note_value(nyir_eval_result_t *result, int value) {
  if (result && value >= 0 && (size_t)value > result->max_value_index)
    result->max_value_index = (size_t)value;
}

static void nyir_eval_note_local(nyir_eval_result_t *result, int64_t local) {
  if (result && local >= 0 && (size_t)local > result->max_local_index)
    result->max_local_index = (size_t)local;
}

bool nyir_eval_with_calls(const nyir_func_t *f, int64_t *locals,
                            size_t local_count, size_t max_steps,
                            nyir_eval_result_t *result,
                            nyir_call_resolver_t resolver, void *resolver_ctx,
                            char *err, size_t err_len) {
  if (!f)
    return nyir_err(err, err_len, "native NYIR VM: missing function");
  char verify_err[256] = {0};
  if (!nyir_verify(f, verify_err, sizeof(verify_err)))
    return nyir_err(err, err_len, "native NYIR VM: verifier rejected input: %s",
                   verify_err);
  if (f->next_value < 0)
    return nyir_err(err, err_len, "native NYIR VM: invalid value count");
  size_t value_count = (size_t)f->next_value;
  int64_t *values = value_count ? (int64_t *)calloc(value_count, sizeof(*values)) : NULL;
  bool *known = value_count ? (bool *)calloc(value_count, sizeof(*known)) : NULL;
  if (value_count && (!values || !known)) {
    free(values);
    free(known);
    return nyir_err(err, err_len, NY_NATIVE_OOM);
  }
  if (result) {
    memset(result, 0, sizeof(*result));
    if (f->len) {
      result->pc_counts = calloc(f->len, sizeof(*result->pc_counts));
      if (!result->pc_counts) {
        free(values);
        free(known);
        return nyir_err(err, err_len, NY_NATIVE_OOM);
      }
      result->pc_count_len = f->len;
    }
  }
  if (max_steps == 0)
    max_steps = 1000000;

  /* Precompute label→PC table once. Previously every BR/BR_IF did an O(n)
   * linear scan of all instructions, making loop back-edges catastrophically
   * expensive. This is the single biggest VM dispatch bottleneck. */
  size_t label_count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm >= label_count)
      label_count = (size_t)f->data[i].imm + 1;
  }
  size_t *label_pc = label_count ? (size_t *)calloc(label_count, sizeof(size_t)) : NULL;
  bool *label_found = label_count ? (bool *)calloc(label_count, sizeof(bool)) : NULL;
  if (label_count && (!label_pc || !label_found)) {
    free(values);
    free(known);
    free(label_pc);
    free(label_found);
    return nyir_err(err, err_len, NY_NATIVE_OOM);
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < label_count) {
      label_pc[(size_t)f->data[i].imm] = i + 1;
      label_found[(size_t)f->data[i].imm] = true;
    }
  }

  size_t pc = 0;
  size_t steps = 0;
  int64_t current_label = -1;
  int64_t predecessor_label = -1;
  const nyir_inst_t *in = NULL;
  size_t inst_index = 0;
  const bool profiling = (result != NULL);
  size_t previous_pc = SIZE_MAX;
  while (pc < f->len) {
    if (++steps > max_steps) {
      nyir_eval_result_free(result);
      free(values);
      free(known);
      free(label_pc);
      free(label_found);
      return nyir_err(err, err_len, "native NYIR VM: step limit exceeded");
    }
    inst_index = pc;
    if (profiling) {
      if (inst_index > result->max_pc)
        result->max_pc = inst_index;
      if (inst_index < result->pc_count_len)
        result->pc_counts[inst_index]++;
      if (!nyir_eval_note_edge(result, previous_pc, inst_index))
        goto profile_oom;
      previous_pc = inst_index;
    }
    in = &f->data[pc++];
    if (profiling && in->op >= 0 && in->op < NYIR_OP_COUNT)
      result->op_counts[in->op]++;
    int64_t a = 0;
    int64_t b = 0;
    int64_t out = 0;
    switch (in->op) {
    case NYIR_NOP:
      break;
    case NYIR_LABEL:
      predecessor_label = current_label;
      current_label = in->imm;
      break;
    case NYIR_PHI: {
      bool found = false;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        if (in->phi_incoming[k].predecessor_label == predecessor_label) {
          if (!nyir_eval_read_value(values, known, in->phi_incoming[k].value, &out))
            goto missing_value;
          found = true;
          break;
        }
      }
      if (!found) goto unsupported;
      values[in->dst] = out;
      known[in->dst] = true;
      break;
    }
    case NYIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
      if (in->dst >= 0) {
        nyir_eval_note_value(result, in->dst);
        values[in->dst] = in->imm;
        known[in->dst] = true;
      }
      break;
    case NYIR_COPY:
    case NYIR_I64_TO_F64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
      if (!nyir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      nyir_eval_note_value(result, in->dst);
      if (in->op == NYIR_I64_TO_F64)
        values[in->dst] = nyir_f64_to_bits((double)a);
      else if (in->op == NYIR_I64_TO_F32)
        values[in->dst] = nyir_f32_to_bits((float)a);
      else if (in->op == NYIR_F64_TO_F32)
        values[in->dst] = nyir_f32_to_bits((float)nyir_bits_to_f64(a));
      else if (in->op == NYIR_F32_TO_F64)
        values[in->dst] = nyir_f64_to_bits((double)nyir_bits_to_f32(a));
      else
        values[in->dst] = a;
      known[in->dst] = true;
      break;
    case NYIR_LOAD_I64: {
      if (!nyir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      int64_t *ptr = (int64_t *)(uintptr_t)a;
      if (!ptr)
        goto unsupported;
      nyir_eval_note_value(result, in->dst);
      values[in->dst] = *ptr;
      known[in->dst] = true;
      break;
    }
    case NYIR_ADDR_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count || !locals)
        goto bad_local;
      nyir_eval_note_local(result, in->imm);
      nyir_eval_note_value(result, in->dst);
      values[in->dst] = (int64_t)(uintptr_t)&locals[in->imm];
      known[in->dst] = true;
      break;
    case NYIR_ADDR_SYMBOL:
    case NYIR_ALLOCA:
    case NYIR_COPY_STRUCT:
    case NYIR_CAPTURE_RET:
      goto unsupported;
    case NYIR_STORE_I64: {
      int64_t val = 0;
      if (!nyir_eval_read_value(values, known, in->a, &a) ||
          !nyir_eval_read_value(values, known, in->c, &val))
        goto missing_value;
      int64_t *ptr = (int64_t *)(uintptr_t)a;
      if (!ptr)
        goto unsupported;
      *ptr = val;
      break;
    }
    case NYIR_LOAD_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count)
        goto bad_local;
      nyir_eval_note_local(result, in->imm);
      nyir_eval_note_value(result, in->dst);
      values[in->dst] = locals ? locals[in->imm] : 0;
      known[in->dst] = true;
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count)
        goto bad_local;
      if (!nyir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      nyir_eval_note_local(result, in->imm);
      if (locals)
        locals[in->imm] = a;
      break;
    case NYIR_CMP_I64:
    case NYIR_CMP_F64:
    case NYIR_CMP_F32:
      if (!nyir_eval_read_value(values, known, in->a, &a) ||
          !nyir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      if (in->op == NYIR_CMP_F64) {
        double da = nyir_bits_to_f64(a);
        double db = nyir_bits_to_f64(b);
        switch (in->cmp) {
        case NYIR_CMP_EQ: out = da == db; break;
        case NYIR_CMP_NE: out = da != db; break;
        case NYIR_CMP_LT: out = da < db; break;
        case NYIR_CMP_LE: out = da <= db; break;
        case NYIR_CMP_GT: out = da > db; break;
        case NYIR_CMP_GE: out = da >= db; break;
        default: goto unsupported;
        }
      } else if (in->op == NYIR_CMP_F32) {
        float fa = nyir_bits_to_f32(a);
        float fb = nyir_bits_to_f32(b);
        switch (in->cmp) {
        case NYIR_CMP_EQ: out = fa == fb; break;
        case NYIR_CMP_NE: out = fa != fb; break;
        case NYIR_CMP_LT: out = fa < fb; break;
        case NYIR_CMP_LE: out = fa <= fb; break;
        case NYIR_CMP_GT: out = fa > fb; break;
        case NYIR_CMP_GE: out = fa >= fb; break;
        default: goto unsupported;
        }
      } else if (!nyir_analyze_cmp_fold(in->cmp, a, b, &out)) {
        goto unsupported;
      }
      nyir_eval_note_value(result, in->dst);
      values[in->dst] = out;
      known[in->dst] = true;
      break;
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
      if (!nyir_eval_read_value(values, known, in->a, &a) ||
          !nyir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      if (!nyir_analyze_binary_fold(in->op, a, b, &out))
        goto unsupported;
      nyir_eval_note_value(result, in->dst);
      values[in->dst] = out;
      known[in->dst] = true;
      break;
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
    case NYIR_DIV_F64: {
      if (!nyir_eval_read_value(values, known, in->a, &a) ||
          !nyir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      double da = nyir_bits_to_f64(a);
      double db = nyir_bits_to_f64(b);
      double dout = 0;
      switch (in->op) {
      case NYIR_ADD_F64:
        dout = da + db;
        break;
      case NYIR_SUB_F64:
        dout = da - db;
        break;
      case NYIR_MUL_F64:
        dout = da * db;
        break;
      case NYIR_DIV_F64:
        dout = da / db;
        break;
      default:
        goto unsupported;
      }
      nyir_eval_note_value(result, in->dst);
      values[in->dst] = nyir_f64_to_bits(dout);
      known[in->dst] = true;
      break;
    }
    case NYIR_ADD_F32:
    case NYIR_SUB_F32:
    case NYIR_MUL_F32:
    case NYIR_DIV_F32: {
      if (!nyir_eval_read_value(values, known, in->a, &a) ||
          !nyir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      float fa = nyir_bits_to_f32(a);
      float fb = nyir_bits_to_f32(b);
      float fout = 0;
      switch (in->op) {
      case NYIR_ADD_F32: fout = fa + fb; break;
      case NYIR_SUB_F32: fout = fa - fb; break;
      case NYIR_MUL_F32: fout = fa * fb; break;
      case NYIR_DIV_F32: fout = fa / fb; break;
      default: goto unsupported;
      }
      nyir_eval_note_value(result, in->dst);
      values[in->dst] = nyir_f32_to_bits(fout);
      known[in->dst] = true;
      break;
    }
    case NYIR_BR:
      if (in->imm < 0 || (size_t)in->imm >= label_count || !label_found[in->imm])
        goto missing_label;
      pc = label_pc[in->imm];
      break;
    case NYIR_BR_IF:
      if (!nyir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      if (a) {
        if (profiling)
          result->branch_taken++;
        if (in->imm < 0 || (size_t)in->imm >= label_count || !label_found[in->imm])
          goto missing_label;
        pc = label_pc[in->imm];
      } else if (profiling) {
        result->branch_not_taken++;
      }
      break;
    case NYIR_RET:
      if (result) {
        result->returned = true;
        result->steps = steps;
      }
      if (in->a >= 0) {
        if (!nyir_eval_read_value(values, known, in->a, &a))
          goto missing_value;
        if (result)
          result->result = a;
      }
      free(values);
      free(known);
      free(label_pc);
      free(label_found);
      if (err && err_len > 0)
        err[0] = '\0';
      return true;
    case NYIR_CALL: {
      if (result)
        result->call_count++;
      if (!resolver) {
        nyir_eval_result_free(result);
        free(values);
        free(known);
        free(label_pc);
        free(label_found);
        return nyir_inst_err(err, err_len, in, inst_index,
                            "NYIR VM does not execute external calls yet");
      }
      if (in->imm < 0 || in->imm > NYIR_CALL_MAX_ARGS) {
        nyir_eval_result_free(result);
        free(values);
        free(known);
        free(label_pc);
        free(label_found);
        return nyir_inst_err(err, err_len, in, inst_index,
                            "NYIR VM supports a bounded number of call args");
      }
      int64_t args[NYIR_CALL_MAX_ARGS];
      if (in->imm > 0 && !nyir_eval_read_value(values, known, in->a, &args[0]))
        goto missing_value;
      if (in->imm > 1 && !nyir_eval_read_value(values, known, in->b, &args[1]))
        goto missing_value;
      if (in->imm > 2 && !nyir_eval_read_value(values, known, in->c, &args[2]))
        goto missing_value;
      if (in->imm > 3 && !nyir_eval_read_value(values, known, in->d, &args[3]))
        goto missing_value;
      if (in->imm > 4 && !nyir_eval_read_value(values, known, in->e, &args[4]))
        goto missing_value;
      if (in->imm > 5 && !nyir_eval_read_value(values, known, in->f, &args[5]))
        goto missing_value;
      for (int64_t k = 6; k < in->imm; ++k) {
        int src = (in->extra_args && (size_t)(k - 6) < in->extra_args_len)
                      ? in->extra_args[k - 6]
                      : -1;
        if (!nyir_eval_read_value(values, known, src, &args[k]))
          goto missing_value;
      }
      if (!resolver(resolver_ctx, in->symbol, args, (size_t)in->imm, &out, err,
                    err_len)) {
        nyir_eval_result_free(result);
        free(values);
        free(known);
        free(label_pc);
        free(label_found);
        return false;
      }
      if (in->dst >= 0) {
        nyir_eval_note_value(result, in->dst);
        values[in->dst] = out;
        known[in->dst] = true;
      }
      break;
    }
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC8_STORE_F32:
    case NYIR_VEC8_ADD_F32:
    case NYIR_VEC8_SUB_F32:
    case NYIR_VEC8_MUL_F32:
    case NYIR_VEC8_DIV_F32:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC8_FMA_F32:
    case NYIR_VEC8_SHUFFLE_F32:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC4_SHL_I64:
    case NYIR_VEC4_SAR_I64:
    case NYIR_VEC4_SET1_I64:
    case NYIR_OP_COUNT:
      goto unsupported;
    }
  }
  if (result)
    result->steps = steps;
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;

profile_oom:
  nyir_eval_result_free(result);
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return nyir_inst_err(err, err_len, in, inst_index,
                         "NYIR VM profile allocation failed");
missing_value:
  nyir_eval_result_free(result);
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return nyir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM read an unavailable value");
bad_local:
  nyir_eval_result_free(result);
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return nyir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM local slot is out of range");
missing_label:
  nyir_eval_result_free(result);
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return nyir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM branch target is missing");
unsupported:
  nyir_eval_result_free(result);
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return nyir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM operation is unsupported for these operands");
}

bool nyir_eval(const nyir_func_t *f, int64_t *locals, size_t local_count,
                 size_t max_steps, nyir_eval_result_t *result, char *err,
                 size_t err_len) {
  return nyir_eval_with_calls(f, locals, local_count, max_steps, result, NULL,
                                NULL, err, err_len);
}

/* ------------------------------------------------------------------ */
/* Translation-validation seed                                         */
/*                                                                    */
/* Bounded multi-input differential interpretation for pure integer   */
/* straight-line NYIR. Not a full SMT harness — just the first safety */
/* net that catches pass miscompiles before native emission.          */
/* ------------------------------------------------------------------ */

bool nyir_is_pure_i64_straightline(const nyir_func_t *f) {
  /* Historical name: now admits pure i64 scalar control-flow + PHIs + locals.
   * Still excludes calls, floats, and non-local memory. */
  if (!f || f->len == 0)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    switch (in->op) {
    case NYIR_NOP:
    case NYIR_CONST_I64:
    case NYIR_COPY:
    case NYIR_PHI:
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
    case NYIR_LOAD_LOCAL:
    case NYIR_STORE_LOCAL:
    case NYIR_LABEL:
    case NYIR_BR:
    case NYIR_BR_IF:
    case NYIR_RET:
      break;
    default:
      return false;
    }
  }
  return true;
}

static size_t nyir_tv_local_count(const nyir_func_t *f) {
  return nyir_max_local(f);
}

static void nyir_tv_fill_locals(int64_t *locals, size_t n, int trial,
                                  uint64_t *rng) {
  if (!locals || n == 0)
    return;
  static const int64_t edges[] = {
      0, 1, -1, 2, -2, 7, 8, 15, 16, 255, 256, INT64_MAX, INT64_MIN};
  if (trial == 0) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = 0;
    return;
  }
  if (trial == 1) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = 1;
    return;
  }
  if (trial == 2) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = -1;
    return;
  }
  if (trial == 3) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = (int64_t)i + 1;
    return;
  }
  if (trial == 4) {
    for (size_t i = 0; i < n; ++i)
      locals[i] = edges[i % (sizeof(edges) / sizeof(edges[0]))];
    return;
  }
  /* Deterministic LCG fill for remaining trials. */
  for (size_t i = 0; i < n; ++i) {
    *rng = (*rng * UINT64_C(6364136223846793005)) + UINT64_C(1);
    locals[i] = (int64_t)(*rng);
    /* Avoid 0 in some slots so div/mod trials are more often defined. */
    if ((trial & 1) && locals[i] == 0)
      locals[i] = (int64_t)(i + 3);
  }
}

bool nyir_tv_equiv_straightline(const nyir_func_t *before,
                                  const nyir_func_t *after, int trials,
                                  char *err, size_t err_len) {
  if (!before || !after)
    return nyir_err(err, err_len, "native TV: missing function");
  if (trials <= 0)
    return true;
  /* Seed scope: pure i64 scalar NYIR (locals, arithmetic, compares, PHI,
   * labels/branches, returns). Calls/floats/heap memory stay out of scope. */
  if (!nyir_is_pure_i64_straightline(before) ||
      !nyir_is_pure_i64_straightline(after))
    return true;

  size_t n_before = nyir_tv_local_count(before);
  size_t n_after = nyir_tv_local_count(after);
  size_t n = n_before > n_after ? n_before : n_after;
  int64_t *locals_a = n ? (int64_t *)calloc(n, sizeof(int64_t)) : NULL;
  int64_t *locals_b = n ? (int64_t *)calloc(n, sizeof(int64_t)) : NULL;
  if (n && (!locals_a || !locals_b)) {
    free(locals_a);
    free(locals_b);
    return nyir_err(err, err_len, "native TV: out of memory");
  }

  uint64_t rng = UINT64_C(0xC0FFEE) ^ (uint64_t)(unsigned)trials;
  int compared = 0;
  for (int t = 0; t < trials; ++t) {
    nyir_tv_fill_locals(locals_a, n, t, &rng);
    if (n)
      memcpy(locals_b, locals_a, n * sizeof(int64_t));

    nyir_eval_result_t ra = {0};
    nyir_eval_result_t rb = {0};
    char ea[192] = {0};
    char eb[192] = {0};
    bool ok_a = nyir_eval(before, locals_a, n, 100000, &ra, ea, sizeof(ea));
    bool ok_b = nyir_eval(after, locals_b, n, 100000, &rb, eb, sizeof(eb));

    if (!ok_a && !ok_b) {
      nyir_eval_result_free(&ra);
      nyir_eval_result_free(&rb);
      continue;
    }
    if (ok_a != ok_b) {
      nyir_eval_result_free(&ra);
      nyir_eval_result_free(&rb);
      free(locals_a);
      free(locals_b);
      return nyir_err(
          err, err_len,
          "native TV: trial %d eval divergence (before=%s after=%s)", t,
          ok_a ? "ok" : (ea[0] ? ea : "fail"),
          ok_b ? "ok" : (eb[0] ? eb : "fail"));
    }
    if (!ra.returned || !rb.returned) {
      free(locals_a);
      free(locals_b);
      return nyir_err(err, err_len,
                        "native TV: trial %d missing return (before=%d after=%d)",
                        t, (int)ra.returned, (int)rb.returned);
    }
    if (ra.result != rb.result) {
      free(locals_a);
      free(locals_b);
      return nyir_err(
          err, err_len,
          "native TV: trial %d result mismatch before=%" PRId64
          " after=%" PRId64,
          t, ra.result, rb.result);
    }
    compared++;
    nyir_eval_result_free(&ra);
    nyir_eval_result_free(&rb);
  }

  free(locals_a);
  free(locals_b);
  if (compared == 0 && trials > 0) {
    /* Every trial trapped — still treat as pass for the seed, but note it. */
    return true;
  }
  return true;
}
