#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nir_value_valid(const ny_nir_func_t *f, int v) {
  return v >= 0 && v < f->next_value;
}

static bool nir_value_defined(const ny_nir_func_t *f, const bool *defined,
                              int v) {
  return nir_value_valid(f, v) && defined && defined[v];
}

static bool nir_label_exists(const ny_nir_func_t *f, int64_t label) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_LABEL && in->imm == label)
      return true;
  }
  return false;
}

bool ny_nir_label_referenced(const ny_nir_func_t *f, int64_t label) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if ((in->op == NY_NIR_BR || in->op == NY_NIR_BR_IF) && in->imm == label)
      return true;
  }
  return false;
}

static unsigned nir_known_effect_mask(void) {
  return NY_NIR_EFFECT_READ_LOCAL | NY_NIR_EFFECT_WRITE_LOCAL |
         NY_NIR_EFFECT_CALL | NY_NIR_EFFECT_CONTROL;
}

bool ny_nir_verify(const ny_nir_func_t *f, char *err, size_t err_len) {
  if (!f)
    return ny_nir_err(err, err_len, "native NYIR verify: missing function");
  if (f->next_value < 0)
    return ny_nir_err(err, err_len, "native NYIR verify: invalid value count");
  bool *defined = NULL;
  if (f->next_value > 0) {
    defined = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    if (!defined)
      return ny_nir_err(err, err_len, "native NYIR verify: out of memory");
  }
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op != NY_NIR_CALL && in->arg_sizes) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i,
                             "non-call instruction has aggregate argument metadata");
    }
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i, "unknown opcode");
    }
    if ((in->op == NY_NIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
         in->op == NYIR_CMP_F32) &&
        in->cmp > NY_NIR_CMP_GE) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i, "unknown comparison predicate");
    }
    unsigned required_effects = ny_nir_inst_effects(in);
    unsigned known_effects = nir_known_effect_mask();
    if ((in->effects & ~known_effects) != 0) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i, "invalid effect mask");
    }
    if (in->effects != required_effects) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i,
                          "effect mask does not match opcode effects");
    }
    if ((in->range.has_min && !in->range.has_max) ||
        (!in->range.has_min && in->range.has_max)) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i, "incomplete range fact");
    }
    if (in->range.has_min && in->range.has_max && in->range.min > in->range.max) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i, "invalid range fact");
    }
    if ((!in->debug.line && in->debug.column) ||
        (!in->debug.line && in->debug.file && in->debug.file[0])) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i, "invalid debug location");
    }
    if (in->dst >= f->next_value) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i, "invalid destination value");
    }
    if (in->dst >= 0 && defined && defined[in->dst]) {
      free(defined);
      return ny_nir_inst_err(err, err_len, in, i,
                          "destination value is already defined");
    }
    switch (in->op) {
    case NY_NIR_NOP:
      break;
    case NY_NIR_LABEL:
      for (size_t j = 0; j < i; ++j) {
        if (f->data[j].op == NY_NIR_LABEL && f->data[j].imm == in->imm) {
          free(defined);
          return ny_nir_inst_err(err, err_len, in, i, "duplicate label");
        }
      }
      break;
    case NY_NIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
      if (in->dst < 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "constant has no destination");
      }
      break;
    case NY_NIR_COPY:
    case NYIR_I64_TO_F64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
    case NYIR_LOAD_I64:
      if (in->dst < 0 || !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "invalid unary value operand");
      }
      break;
    case NY_NIR_LOAD_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "invalid local load");
      }
      break;
    case NYIR_ADDR_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "invalid local address");
      }
      break;
    case NYIR_ADDR_SYMBOL:
      if (in->dst < 0 || !in->symbol || !in->symbol[0]) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "addr.symbol requires a non-empty symbol");
      }
      break;
    case NYIR_ALLOCA:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "alloca requires a valid destination and positive size");
      }
      break;
    case NYIR_COPY_STRUCT:
      if (in->a < 0 || in->b < 0 || in->imm < 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "copy.struct requires valid src, dst, and size");
      }
      break;
    case NYIR_CAPTURE_RET:
      if (in->dst < 0 || in->imm < 0 || in->imm > 3 || i == 0 ||
          (f->data[i - 1].op != NY_NIR_CALL &&
           f->data[i - 1].op != NYIR_CAPTURE_RET)) {
        free(defined);
        return ny_nir_inst_err(
            err, err_len, in, i,
            "capture.ret requires a call/capture chain immediately before it and selector 0..3");
      }
      break;
    case NY_NIR_STORE_LOCAL:
      if (in->imm < 0 || !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "invalid local store");
      }
      break;
    case NYIR_STORE_I64:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "invalid memory store");
      }
      break;
    case NY_NIR_RET:
      if (in->a >= 0 && !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "invalid return value");
      }
      break;
    case NY_NIR_BR:
      if (!nir_label_exists(f, in->imm)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "missing branch target label");
      }
      break;
    case NY_NIR_BR_IF:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_label_exists(f, in->imm)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "invalid conditional branch operand or target");
      }
      break;
    case NY_NIR_CALL:
      if (!in->symbol || !in->symbol[0]) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "call has no symbol");
      }
      if (in->imm < 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "negative call arg count");
      }
      if (in->imm == 0 &&
          (in->a >= 0 || in->b >= 0 || in->c >= 0 || in->d >= 0 ||
           in->e >= 0 || in->f >= 0)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "zero-argument call has value operands");
      }
      if (in->imm == 1 &&
          (in->b >= 0 || in->c >= 0 || in->d >= 0 || in->e >= 0 ||
           in->f >= 0)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "one-argument call has extra value operand");
      }
      if (in->imm == 2 &&
          (in->c >= 0 || in->d >= 0 || in->e >= 0 || in->f >= 0)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "two-argument call has extra value operand");
      }
      if (in->imm == 3 && (in->d >= 0 || in->e >= 0 || in->f >= 0)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "three-argument call has extra value operand");
      }
      if (in->imm == 4 && (in->e >= 0 || in->f >= 0)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "four-argument call has extra value operand");
      }
      if (in->imm == 5 && in->f >= 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "five-argument call has extra value operand");
      }
      if (in->imm > 0 && !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "call arg0 is invalid");
      }
      if (in->imm > 1 && !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "call arg1 is invalid");
      }
      if (in->imm > 2 && !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "call arg2 is invalid");
      }
      if (in->imm > 3 && !nir_value_defined(f, defined, in->d)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "call arg3 is invalid");
      }
      if (in->imm > 4 && !nir_value_defined(f, defined, in->e)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "call arg4 is invalid");
      }
      if (in->imm > 5 && !nir_value_defined(f, defined, in->f)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "call arg5 is invalid");
      }
      if (in->imm > NY_NIR_CALL_MAX_ARGS) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                            "call exceeds the maximum supported argument count");
      }
      if (in->arg_sizes && in->imm <= 0) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i,
                               "zero-argument call has aggregate argument metadata");
      }
      if (in->arg_sizes) {
        for (int64_t arg = 0; arg < in->imm; ++arg) {
          uint32_t packed = in->arg_sizes[arg];
          unsigned c0 = NY_NIR_ARG_AGG_CLASS(packed, 0);
          unsigned c1 = NY_NIR_ARG_AGG_CLASS(packed, 1);
          if (packed != 0 &&
              (NY_NIR_ARG_AGG_SIZE(packed) == 0 ||
               c0 > NY_NIR_ARG_CLASS_UNSUPPORTED ||
               c1 > NY_NIR_ARG_CLASS_UNSUPPORTED)) {
            free(defined);
            return ny_nir_inst_err(err, err_len, in, i,
                                   "invalid aggregate argument metadata");
          }
        }
      }
      if (in->imm <= 6) {
        if (in->extra_args || in->extra_args_len != 0) {
          free(defined);
          return ny_nir_inst_err(err, err_len, in, i,
                              "call has stray stack-args for a register-only arity");
        }
      } else {
        size_t want = (size_t)(in->imm - 6);
        if (!in->extra_args || in->extra_args_len != want) {
          free(defined);
          return ny_nir_inst_err(err, err_len, in, i,
                              "call stack-arg count does not match arity");
        }
        for (size_t k = 0; k < want; ++k) {
          if (!nir_value_defined(f, defined, in->extra_args[k])) {
            free(defined);
            return ny_nir_inst_err(err, err_len, in, i, "call stack-arg is invalid");
          }
        }
      }
      break;
    default:
      if (in->op == NYIR_OP_COUNT) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "unknown opcode");
      }
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return ny_nir_inst_err(err, err_len, in, i, "invalid value operands");
      }
      break;
    }
    if (in->dst >= 0 && defined)
      defined[in->dst] = true;
  }
  free(defined);
  if (!ny_nir_validate_constraints(f, err, err_len))
    return false;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}


bool ny_nir_analyze_binary_fold(ny_nir_op_t op, int64_t a, int64_t b,
                                       int64_t *out) {
  switch (op) {
  case NY_NIR_ADD_I64:
    *out = a + b;
    return true;
  case NY_NIR_SUB_I64:
    *out = a - b;
    return true;
  case NY_NIR_MUL_I64:
    *out = a * b;
    return true;
  case NY_NIR_DIV_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a / b;
    return true;
  case NY_NIR_MOD_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a % b;
    return true;
  case NY_NIR_AND_I64:
    *out = a & b;
    return true;
  case NY_NIR_OR_I64:
    *out = a | b;
    return true;
  case NY_NIR_XOR_I64:
    *out = a ^ b;
    return true;
  case NY_NIR_SHL_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = (int64_t)((uint64_t)a << (unsigned)b);
    return true;
  case NY_NIR_SAR_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = a >> (unsigned)b;
    return true;
  default:
    return false;
  }
}

bool ny_nir_analyze_cmp_fold(ny_nir_cmp_t cmp, int64_t a, int64_t b,
                                    int64_t *out) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    *out = a == b;
    return true;
  case NY_NIR_CMP_NE:
    *out = a != b;
    return true;
  case NY_NIR_CMP_LT:
    *out = a < b;
    return true;
  case NY_NIR_CMP_LE:
    *out = a <= b;
    return true;
  case NY_NIR_CMP_GT:
    *out = a > b;
    return true;
  case NY_NIR_CMP_GE:
    *out = a >= b;
    return true;
  }
  return false;
}

static void ny_nir_fact_set_const(ny_nir_value_fact_t *fact, int64_t value) {
  if (!fact)
    return;
  fact->known_const = true;
  fact->const_value = value;
  fact->range.has_min = true;
  fact->range.has_max = true;
  fact->range.min = value;
  fact->range.max = value;
}

static bool ny_nir_i64_add_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out)
    return false;
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
    return false;
  *out = a + b;
  return true;
}

static bool ny_nir_i64_sub_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out)
    return false;
  if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
    return false;
  *out = a - b;
  return true;
}

static bool ny_nir_i64_div_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out || b == 0 || (a == INT64_MIN && b == -1))
    return false;
  *out = a / b;
  return true;
}

static bool ny_nir_i64_mul_checked(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
  if (!out)
    return false;
  return !__builtin_mul_overflow(a, b, out);
#else
  if (!out)
    return false;
  if (a == 0 || b == 0) {
    *out = 0;
    return true;
  }
  if (a == -1) {
    if (b == INT64_MIN)
      return false;
    *out = -b;
    return true;
  }
  if (b == -1) {
    if (a == INT64_MIN)
      return false;
    *out = -a;
    return true;
  }
  if (a > 0) {
    if (b > 0) {
      if (a > INT64_MAX / b)
        return false;
    } else if (b < INT64_MIN / a) {
      return false;
    }
  } else {
    if (b > 0) {
      if (a < INT64_MIN / b)
        return false;
    } else if (a != 0 && b < INT64_MAX / a) {
      return false;
    }
  }
  *out = a * b;
  return true;
#endif
}

static bool ny_nir_range_from_corners(const int64_t *v, size_t n,
                                      ny_nir_range_t *out) {
  if (!v || n == 0 || !out)
    return false;
  int64_t lo = v[0];
  int64_t hi = v[0];
  for (size_t i = 1; i < n; ++i) {
    if (v[i] < lo)
      lo = v[i];
    if (v[i] > hi)
      hi = v[i];
  }
  *out = (ny_nir_range_t){.has_min = true, .has_max = true, .min = lo, .max = hi};
  return true;
}

static bool ny_nir_fact_binary_range(ny_nir_op_t op,
                                     const ny_nir_value_fact_t *a,
                                     const ny_nir_value_fact_t *b,
                                     ny_nir_range_t *out) {
  if (!a || !b || !out)
    return false;
  if (op == NY_NIR_AND_I64) {
    if (b->known_const && b->const_value >= 0) {
      *out = (ny_nir_range_t){.has_min = true,
                              .has_max = true,
                              .min = 0,
                              .max = b->const_value};
      return true;
    }
    if (a->known_const && a->const_value >= 0) {
      *out = (ny_nir_range_t){.has_min = true,
                              .has_max = true,
                              .min = 0,
                              .max = a->const_value};
      return true;
    }
  }
  if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
      !b->range.has_max)
    return false;
  int64_t lo = 0;
  int64_t hi = 0;
  switch (op) {
  case NY_NIR_ADD_I64:
    if (!ny_nir_i64_add_checked(a->range.min, b->range.min, &lo) ||
        !ny_nir_i64_add_checked(a->range.max, b->range.max, &hi))
      return false;
    break;
  case NY_NIR_SUB_I64:
    if (!ny_nir_i64_sub_checked(a->range.min, b->range.max, &lo) ||
        !ny_nir_i64_sub_checked(a->range.max, b->range.min, &hi))
      return false;
    break;
  case NY_NIR_MUL_I64: {
    int64_t corners[4];
    if (!ny_nir_i64_mul_checked(a->range.min, b->range.min, &corners[0]) ||
        !ny_nir_i64_mul_checked(a->range.min, b->range.max, &corners[1]) ||
        !ny_nir_i64_mul_checked(a->range.max, b->range.min, &corners[2]) ||
        !ny_nir_i64_mul_checked(a->range.max, b->range.max, &corners[3]) ||
        !ny_nir_range_from_corners(corners, 4, out))
      return false;
    return true;
  }
  case NY_NIR_DIV_I64: {
    if (b->range.min <= 0 && b->range.max >= 0)
      return false;
    int64_t corners[4];
    if (!ny_nir_i64_div_checked(a->range.min, b->range.min, &corners[0]) ||
        !ny_nir_i64_div_checked(a->range.min, b->range.max, &corners[1]) ||
        !ny_nir_i64_div_checked(a->range.max, b->range.min, &corners[2]) ||
        !ny_nir_i64_div_checked(a->range.max, b->range.max, &corners[3]) ||
        !ny_nir_range_from_corners(corners, 4, out))
      return false;
    return true;
  }
  case NY_NIR_MOD_I64:
    if (!b->known_const || b->const_value == 0 || b->const_value == INT64_MIN)
      return false;
    hi = b->const_value < 0 ? -b->const_value - 1 : b->const_value - 1;
    lo = a->range.min < 0 ? -hi : 0;
    if (a->range.max <= 0)
      hi = 0;
    break;
  case NY_NIR_SHL_I64:
    if (!b->known_const || b->const_value < 0 || b->const_value >= 63 ||
        a->range.min < 0)
      return false;
    if (!ny_nir_i64_mul_checked(a->range.min, (int64_t)1 << b->const_value,
                                &lo) ||
        !ny_nir_i64_mul_checked(a->range.max, (int64_t)1 << b->const_value,
                                &hi))
      return false;
    break;
  case NY_NIR_SAR_I64:
    if (!b->known_const || b->const_value < 0 || b->const_value >= 64)
      return false;
    lo = a->range.min >> (unsigned)b->const_value;
    hi = a->range.max >> (unsigned)b->const_value;
    break;
  case NY_NIR_AND_I64:
  case NY_NIR_OR_I64:
  case NY_NIR_XOR_I64:
    if (a->range.min < 0 || b->range.min < 0)
      return false;
    lo = 0;
    hi = a->range.max | b->range.max;
    break;
  default:
    return false;
  }
  if (lo > hi)
    return false;
  *out = (ny_nir_range_t){.has_min = true, .has_max = true, .min = lo, .max = hi};
  return true;
}


bool ny_nir_metadata_summary(const ny_nir_func_t *f,
                             ny_nir_metadata_summary_t *summary, char *err,
                             size_t err_len) {
  if (!f)
    return ny_nir_err(err, err_len, "native NYIR metadata: missing function");
  if (!summary)
    return ny_nir_err(err, err_len, "native NYIR metadata: missing summary output");
  char verify_err[256] = {0};
  if (!ny_nir_verify(f, verify_err, sizeof(verify_err)))
    return ny_nir_err(err, err_len, "native NYIR metadata: verifier rejected input: %s",
                   verify_err);
  memset(summary, 0, sizeof(*summary));
  summary->instructions = f->len;
  summary->values = f->next_value > 0 ? (size_t)f->next_value : 0;
  int64_t max_local = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op >= 0 && in->op < NYIR_OP_COUNT)
      summary->ops[in->op]++;
    summary->effect_mask |= in->effects | ny_nir_inst_effects(in);
    if (in->range.has_min || in->range.has_max)
      summary->range_facts++;
    if (in->debug.line || (in->debug.file && in->debug.file[0]))
      summary->debug_locs++;
    switch (in->op) {
    case NY_NIR_LABEL:
      summary->labels++;
      break;
    case NY_NIR_BR:
      summary->branches++;
      break;
    case NY_NIR_BR_IF:
      summary->branches++;
      summary->conditional_branches++;
      break;
    case NY_NIR_CALL:
      summary->calls++;
      break;
    case NY_NIR_RET:
      summary->returns++;
      break;
    case NY_NIR_LOAD_LOCAL:
    case NYIR_ADDR_LOCAL:
    case NY_NIR_STORE_LOCAL:
      if (in->imm > max_local)
        max_local = in->imm;
      break;
    case NYIR_ADDR_SYMBOL:
      break;
    default:
      break;
    }
  }
  summary->locals = max_local >= 0 ? (size_t)max_local + 1 : 0;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

void ny_nir_metadata_summary_dump(FILE *out, const char *name,
                                  const ny_nir_metadata_summary_t *summary) {
  if (!out || !summary)
    return;
  fprintf(out,
          "nyir metadata function=%s insts=%zu values=%zu locals=%zu labels=%zu branches=%zu br_if=%zu calls=%zu returns=%zu ranges=%zu debug=%zu effects=0x%x\n",
          name && name[0] ? name : "<anon>", summary->instructions,
          summary->values, summary->locals, summary->labels,
          summary->branches, summary->conditional_branches, summary->calls,
          summary->returns, summary->range_facts, summary->debug_locs,
          summary->effect_mask);
  for (int op = 0; op < NYIR_OP_COUNT; ++op) {
    if (summary->ops[op])
      fprintf(out, "  op %-14s %zu\n", ny_nir_op_name((ny_nir_op_t)op),
              summary->ops[op]);
  }
}

bool ny_nir_analyze_values(const ny_nir_func_t *f, ny_nir_value_fact_t *facts,
                           size_t fact_count, char *err, size_t err_len) {
  if (!f)
    return ny_nir_err(err, err_len, "native NYIR analysis: missing function");
  if (f->next_value < 0)
    return ny_nir_err(err, err_len, "native NYIR analysis: invalid value count");
  if ((size_t)f->next_value > fact_count)
    return ny_nir_err(err, err_len,
                   "native NYIR analysis: fact table too small (%zu < %d)",
                   fact_count, f->next_value);
  if (facts && fact_count > 0)
    memset(facts, 0, fact_count * sizeof(*facts));

  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT)
      return ny_nir_inst_err(err, err_len, in, i, "unknown opcode");
    if (in->a >= 0 && facts && (size_t)in->a < fact_count)
      facts[in->a].use_count++;
    if (in->b >= 0 && facts && (size_t)in->b < fact_count)
      facts[in->b].use_count++;
    if (in->op == NY_NIR_CALL && in->extra_args && facts) {
      for (size_t k = 0; k < in->extra_args_len; ++k) {
        int v = in->extra_args[k];
        if (v >= 0 && (size_t)v < fact_count)
          facts[v].use_count++;
      }
    }
    if (in->dst < 0 || !facts || (size_t)in->dst >= fact_count)
      continue;
    ny_nir_value_fact_t *dst = &facts[in->dst];
    dst->effects |= in->effects | ny_nir_inst_effects(in);
    if (in->range.has_min || in->range.has_max)
      dst->range = in->range;

    switch (in->op) {
    case NY_NIR_CONST_I64:
      ny_nir_fact_set_const(dst, in->imm);
      break;
    case NY_NIR_COPY:
      if (in->a >= 0 && (size_t)in->a < fact_count) {
        dst->known_const = facts[in->a].known_const;
        dst->const_value = facts[in->a].const_value;
        dst->range = facts[in->a].range;
      }
      break;
    case NY_NIR_CMP_I64:
      dst->range.has_min = true;
      dst->range.has_max = true;
      dst->range.min = 0;
      dst->range.max = 1;
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count && facts[in->a].known_const &&
          facts[in->b].known_const) {
        int64_t folded = 0;
        if (ny_nir_analyze_cmp_fold(in->cmp, facts[in->a].const_value,
                                    facts[in->b].const_value, &folded))
          ny_nir_fact_set_const(dst, folded);
      }
      break;
    default:
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count && facts[in->a].known_const &&
          facts[in->b].known_const) {
        int64_t folded = 0;
        if (ny_nir_analyze_binary_fold(in->op, facts[in->a].const_value,
                                       facts[in->b].const_value, &folded)) {
          ny_nir_fact_set_const(dst, folded);
          break;
        }
      }
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count) {
        ny_nir_range_t r = {0};
        if (ny_nir_fact_binary_range(in->op, &facts[in->a], &facts[in->b], &r))
          dst->range = r;
      }
      break;
    }
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

bool ny_nir_validate_constraints(const ny_nir_func_t *f, char *err,
                                 size_t err_len) {
  if (!f)
    return ny_nir_err(err, err_len, "native NYIR constraints: missing function");
  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return ny_nir_err(err, err_len, "native NYIR constraints: out of memory");
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(known);
      free(value);
      return ny_nir_inst_err(err, err_len, in, i, "unknown opcode");
    }
    if (in->range.has_min && in->range.has_max && in->range.min > in->range.max) {
      free(known);
      free(value);
      return ny_nir_inst_err(err, err_len, in, i, "range minimum exceeds maximum");
    }
    if ((in->op == NY_NIR_SHL_I64 || in->op == NY_NIR_SAR_I64) &&
        in->b >= 0 && known && known[in->b] &&
        (value[in->b] < 0 || value[in->b] >= 64)) {
      free(known);
      free(value);
      return ny_nir_inst_err(err, err_len, in, i, "constant shift amount out of range");
    }
    if ((in->op == NY_NIR_DIV_I64 || in->op == NY_NIR_MOD_I64) &&
        in->b >= 0 && known && known[in->b] && value[in->b] == 0) {
      free(known);
      free(value);
      return ny_nir_inst_err(err, err_len, in, i, "constant divide/modulo by zero");
    }
    if ((in->op == NY_NIR_DIV_I64 || in->op == NY_NIR_MOD_I64) &&
        in->a >= 0 && in->b >= 0 && known && known[in->a] && known[in->b] &&
        value[in->a] == INT64_MIN && value[in->b] == -1) {
      free(known);
      free(value);
      return ny_nir_inst_err(err, err_len, in, i,
                          "constant signed divide/modulo overflow");
    }
    if (in->dst >= 0 && known) {
      if (in->op == NY_NIR_CONST_I64) {
        known[in->dst] = true;
        value[in->dst] = in->imm;
      } else if (in->op == NY_NIR_COPY && in->a >= 0 && known[in->a]) {
        known[in->dst] = true;
        value[in->dst] = value[in->a];
      } else {
        known[in->dst] = false;
      }
    }
  }
  free(known);
  free(value);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
