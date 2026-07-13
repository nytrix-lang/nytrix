#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ny_nir_eval_find_label(const ny_nir_func_t *f, int64_t label,
                                   size_t *pc_out) {
  if (!f || !pc_out)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NY_NIR_LABEL && f->data[i].imm == label) {
      *pc_out = i + 1;
      return true;
    }
  }
  return false;
}

static bool ny_nir_eval_read_value(const int64_t *values, const bool *known,
                                   int value, int64_t *out) {
  if (!values || !known || !out || value < 0 || !known[value])
    return false;
  *out = values[value];
  return true;
}

static void ny_nir_eval_note_value(ny_nir_eval_result_t *result, int value) {
  if (result && value >= 0 && (size_t)value > result->max_value_index)
    result->max_value_index = (size_t)value;
}

static void ny_nir_eval_note_local(ny_nir_eval_result_t *result, int64_t local) {
  if (result && local >= 0 && (size_t)local > result->max_local_index)
    result->max_local_index = (size_t)local;
}

bool ny_nir_eval_with_calls(const ny_nir_func_t *f, int64_t *locals,
                            size_t local_count, size_t max_steps,
                            ny_nir_eval_result_t *result,
                            ny_nir_call_resolver_t resolver, void *resolver_ctx,
                            char *err, size_t err_len) {
  if (!f)
    return ny_nir_err(err, err_len, "native NYIR VM: missing function");
  char verify_err[256] = {0};
  if (!ny_nir_verify(f, verify_err, sizeof(verify_err)))
    return ny_nir_err(err, err_len, "native NYIR VM: verifier rejected input: %s",
                   verify_err);
  if (f->next_value < 0)
    return ny_nir_err(err, err_len, "native NYIR VM: invalid value count");
  size_t value_count = (size_t)f->next_value;
  int64_t *values = value_count ? (int64_t *)calloc(value_count, sizeof(*values)) : NULL;
  bool *known = value_count ? (bool *)calloc(value_count, sizeof(*known)) : NULL;
  if (value_count && (!values || !known)) {
    free(values);
    free(known);
    return ny_nir_err(err, err_len, "native NYIR VM: out of memory");
  }
  if (result)
    memset(result, 0, sizeof(*result));
  if (max_steps == 0)
    max_steps = 1000000;

  /* Precompute label→PC table once. Previously every BR/BR_IF did an O(n)
   * linear scan of all instructions, making loop back-edges catastrophically
   * expensive. This is the single biggest VM dispatch bottleneck. */
  size_t label_count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NY_NIR_LABEL && f->data[i].imm >= 0 &&
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
    return ny_nir_err(err, err_len, "native NYIR VM: out of memory");
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NY_NIR_LABEL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < label_count) {
      label_pc[(size_t)f->data[i].imm] = i + 1;
      label_found[(size_t)f->data[i].imm] = true;
    }
  }

  size_t pc = 0;
  size_t steps = 0;
  const ny_nir_inst_t *in = NULL;
  size_t inst_index = 0;
  const bool profiling = (result != NULL);
  while (pc < f->len) {
    if (++steps > max_steps) {
      free(values);
      free(known);
      free(label_pc);
      free(label_found);
      return ny_nir_err(err, err_len, "native NYIR VM: step limit exceeded");
    }
    inst_index = pc;
    if (profiling && inst_index > result->max_pc)
      result->max_pc = inst_index;
    in = &f->data[pc++];
    if (profiling && in->op >= 0 && in->op < NYIR_OP_COUNT)
      result->op_counts[in->op]++;
    int64_t a = 0;
    int64_t b = 0;
    int64_t out = 0;
    switch (in->op) {
    case NY_NIR_NOP:
    case NY_NIR_LABEL:
      break;
    case NY_NIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
      if (in->dst >= 0) {
        ny_nir_eval_note_value(result, in->dst);
        values[in->dst] = in->imm;
        known[in->dst] = true;
      }
      break;
    case NY_NIR_COPY:
    case NYIR_I64_TO_F64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
      if (!ny_nir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      ny_nir_eval_note_value(result, in->dst);
      if (in->op == NYIR_I64_TO_F64)
        values[in->dst] = ny_nir_f64_to_bits((double)a);
      else if (in->op == NYIR_I64_TO_F32)
        values[in->dst] = ny_nir_f32_to_bits((float)a);
      else if (in->op == NYIR_F64_TO_F32)
        values[in->dst] = ny_nir_f32_to_bits((float)ny_nir_bits_to_f64(a));
      else if (in->op == NYIR_F32_TO_F64)
        values[in->dst] = ny_nir_f64_to_bits((double)ny_nir_bits_to_f32(a));
      else
        values[in->dst] = a;
      known[in->dst] = true;
      break;
    case NYIR_LOAD_I64: {
      if (!ny_nir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      int64_t *ptr = (int64_t *)(uintptr_t)a;
      if (!ptr)
        goto unsupported;
      ny_nir_eval_note_value(result, in->dst);
      values[in->dst] = *ptr;
      known[in->dst] = true;
      break;
    }
    case NYIR_ADDR_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count || !locals)
        goto bad_local;
      ny_nir_eval_note_local(result, in->imm);
      ny_nir_eval_note_value(result, in->dst);
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
      if (!ny_nir_eval_read_value(values, known, in->a, &a) ||
          !ny_nir_eval_read_value(values, known, in->c, &val))
        goto missing_value;
      int64_t *ptr = (int64_t *)(uintptr_t)a;
      if (!ptr)
        goto unsupported;
      *ptr = val;
      break;
    }
    case NY_NIR_LOAD_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count)
        goto bad_local;
      ny_nir_eval_note_local(result, in->imm);
      ny_nir_eval_note_value(result, in->dst);
      values[in->dst] = locals ? locals[in->imm] : 0;
      known[in->dst] = true;
      break;
    case NY_NIR_STORE_LOCAL:
      if (in->imm < 0 || (size_t)in->imm >= local_count)
        goto bad_local;
      if (!ny_nir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      ny_nir_eval_note_local(result, in->imm);
      if (locals)
        locals[in->imm] = a;
      break;
    case NY_NIR_CMP_I64:
    case NYIR_CMP_F64:
    case NYIR_CMP_F32:
      if (!ny_nir_eval_read_value(values, known, in->a, &a) ||
          !ny_nir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      if (in->op == NYIR_CMP_F64) {
        double da = ny_nir_bits_to_f64(a);
        double db = ny_nir_bits_to_f64(b);
        switch (in->cmp) {
        case NY_NIR_CMP_EQ: out = da == db; break;
        case NY_NIR_CMP_NE: out = da != db; break;
        case NY_NIR_CMP_LT: out = da < db; break;
        case NY_NIR_CMP_LE: out = da <= db; break;
        case NY_NIR_CMP_GT: out = da > db; break;
        case NY_NIR_CMP_GE: out = da >= db; break;
        default: goto unsupported;
        }
      } else if (in->op == NYIR_CMP_F32) {
        float fa = ny_nir_bits_to_f32(a);
        float fb = ny_nir_bits_to_f32(b);
        switch (in->cmp) {
        case NY_NIR_CMP_EQ: out = fa == fb; break;
        case NY_NIR_CMP_NE: out = fa != fb; break;
        case NY_NIR_CMP_LT: out = fa < fb; break;
        case NY_NIR_CMP_LE: out = fa <= fb; break;
        case NY_NIR_CMP_GT: out = fa > fb; break;
        case NY_NIR_CMP_GE: out = fa >= fb; break;
        default: goto unsupported;
        }
      } else if (!ny_nir_analyze_cmp_fold(in->cmp, a, b, &out)) {
        goto unsupported;
      }
      ny_nir_eval_note_value(result, in->dst);
      values[in->dst] = out;
      known[in->dst] = true;
      break;
    case NY_NIR_ADD_I64:
    case NY_NIR_SUB_I64:
    case NY_NIR_MUL_I64:
    case NY_NIR_DIV_I64:
    case NY_NIR_MOD_I64:
    case NY_NIR_AND_I64:
    case NY_NIR_OR_I64:
    case NY_NIR_XOR_I64:
    case NY_NIR_SHL_I64:
    case NY_NIR_SAR_I64:
      if (!ny_nir_eval_read_value(values, known, in->a, &a) ||
          !ny_nir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      if (!ny_nir_analyze_binary_fold(in->op, a, b, &out))
        goto unsupported;
      ny_nir_eval_note_value(result, in->dst);
      values[in->dst] = out;
      known[in->dst] = true;
      break;
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
    case NYIR_DIV_F64: {
      if (!ny_nir_eval_read_value(values, known, in->a, &a) ||
          !ny_nir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      double da = ny_nir_bits_to_f64(a);
      double db = ny_nir_bits_to_f64(b);
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
      ny_nir_eval_note_value(result, in->dst);
      values[in->dst] = ny_nir_f64_to_bits(dout);
      known[in->dst] = true;
      break;
    }
    case NYIR_ADD_F32:
    case NYIR_SUB_F32:
    case NYIR_MUL_F32:
    case NYIR_DIV_F32: {
      if (!ny_nir_eval_read_value(values, known, in->a, &a) ||
          !ny_nir_eval_read_value(values, known, in->b, &b))
        goto missing_value;
      float fa = ny_nir_bits_to_f32(a);
      float fb = ny_nir_bits_to_f32(b);
      float fout = 0;
      switch (in->op) {
      case NYIR_ADD_F32: fout = fa + fb; break;
      case NYIR_SUB_F32: fout = fa - fb; break;
      case NYIR_MUL_F32: fout = fa * fb; break;
      case NYIR_DIV_F32: fout = fa / fb; break;
      default: goto unsupported;
      }
      ny_nir_eval_note_value(result, in->dst);
      values[in->dst] = ny_nir_f32_to_bits(fout);
      known[in->dst] = true;
      break;
    }
    case NY_NIR_BR:
      if (in->imm < 0 || (size_t)in->imm >= label_count || !label_found[in->imm])
        goto missing_label;
      pc = label_pc[in->imm];
      break;
    case NY_NIR_BR_IF:
      if (!ny_nir_eval_read_value(values, known, in->a, &a))
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
    case NY_NIR_RET:
      if (result) {
        result->returned = true;
        result->steps = steps;
      }
      if (in->a >= 0) {
        if (!ny_nir_eval_read_value(values, known, in->a, &a))
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
    case NY_NIR_CALL: {
      if (result)
        result->call_count++;
      if (!resolver) {
        free(values);
        free(known);
        free(label_pc);
        free(label_found);
        return ny_nir_inst_err(err, err_len, in, inst_index,
                            "NYIR VM does not execute external calls yet");
      }
      if (in->imm < 0 || in->imm > NY_NIR_CALL_MAX_ARGS) {
        free(values);
        free(known);
        free(label_pc);
        free(label_found);
        return ny_nir_inst_err(err, err_len, in, inst_index,
                            "NYIR VM supports a bounded number of call args");
      }
      int64_t args[NY_NIR_CALL_MAX_ARGS];
      if (in->imm > 0 && !ny_nir_eval_read_value(values, known, in->a, &args[0]))
        goto missing_value;
      if (in->imm > 1 && !ny_nir_eval_read_value(values, known, in->b, &args[1]))
        goto missing_value;
      if (in->imm > 2 && !ny_nir_eval_read_value(values, known, in->c, &args[2]))
        goto missing_value;
      if (in->imm > 3 && !ny_nir_eval_read_value(values, known, in->d, &args[3]))
        goto missing_value;
      if (in->imm > 4 && !ny_nir_eval_read_value(values, known, in->e, &args[4]))
        goto missing_value;
      if (in->imm > 5 && !ny_nir_eval_read_value(values, known, in->f, &args[5]))
        goto missing_value;
      for (int64_t k = 6; k < in->imm; ++k) {
        int src = (in->extra_args && (size_t)(k - 6) < in->extra_args_len)
                      ? in->extra_args[k - 6]
                      : -1;
        if (!ny_nir_eval_read_value(values, known, src, &args[k]))
          goto missing_value;
      }
      if (!resolver(resolver_ctx, in->symbol, args, (size_t)in->imm, &out, err,
                    err_len)) {
        free(values);
        free(known);
        free(label_pc);
        free(label_found);
        return false;
      }
      if (in->dst >= 0) {
        ny_nir_eval_note_value(result, in->dst);
        values[in->dst] = out;
        known[in->dst] = true;
      }
      break;
    }
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

missing_value:
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return ny_nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM read an unavailable value");
bad_local:
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return ny_nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM local slot is out of range");
missing_label:
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return ny_nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM branch target is missing");
unsupported:
  free(values);
  free(known);
  free(label_pc);
  free(label_found);
  return ny_nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM operation is unsupported for these operands");
}

bool ny_nir_eval(const ny_nir_func_t *f, int64_t *locals, size_t local_count,
                 size_t max_steps, ny_nir_eval_result_t *result, char *err,
                 size_t err_len) {
  return ny_nir_eval_with_calls(f, locals, local_count, max_steps, result, NULL,
                                NULL, err, err_len);
}
