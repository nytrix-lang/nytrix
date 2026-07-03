#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Shared native NYIR diagnostics and lightweight optimization.
 *
 * NYIR is the common compiler/backend trace point for the native migration. It
 * currently supports the raw-int debug subset: constants, arithmetic,
 * comparisons, locals, labels/branches, calls, and returns. The verifier and
 * optimizer are intentionally small and deterministic so backend bugs can be
 * reduced from source to NYIR before looking at emitted assembly.
 */

void ny_nir_func_free(ny_nir_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->owned_symbols_len; ++i)
    free(f->owned_symbols[i]);
  free(f->owned_symbols);
  for (size_t i = 0; i < f->len; ++i)
    free(f->data[i].extra_args);
  free(f->data);
  memset(f, 0, sizeof(*f));
}

void ny_nir_inst_discard(ny_nir_inst_t *in) {
  if (!in)
    return;
  free(in->extra_args);
  *in = (ny_nir_inst_t){.op = NY_NIR_NOP,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1};
}

static const char *ny_nir_func_own_symbol(ny_nir_func_t *f, char *s) {
  if (!f || !s)
    return NULL;
  if (f->owned_symbols_len >= f->owned_symbols_cap) {
    size_t cap = f->owned_symbols_cap ? f->owned_symbols_cap * 2 : 16;
    char **data = (char **)realloc(f->owned_symbols, cap * sizeof(*data));
    if (!data) {
      free(s);
      return NULL;
    }
    f->owned_symbols = data;
    f->owned_symbols_cap = cap;
  }
  f->owned_symbols[f->owned_symbols_len++] = s;
  return s;
}

static int64_t ny_nir_f64_to_bits(double v) {
  int64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

static double ny_nir_bits_to_f64(int64_t bits) {
  double v = 0;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

static int64_t ny_nir_f32_to_bits(float v) {
  int32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)(uint32_t)bits;
}

static float ny_nir_bits_to_f32(int64_t bits) {
  int32_t b32 = (int32_t)(uint32_t)bits;
  float v = 0;
  memcpy(&v, &b32, sizeof(v));
  return v;
}

const char *ny_nir_op_name(ny_nir_op_t op) {
  switch (op) {
  case NY_NIR_NOP:
    return "nop";
  case NY_NIR_CONST_I64:
    return "const.i64";
  case NY_NIR_COPY:
    return "copy";
  case NY_NIR_ADD_I64:
    return "add.i64";
  case NY_NIR_SUB_I64:
    return "sub.i64";
  case NY_NIR_MUL_I64:
    return "mul.i64";
  case NY_NIR_DIV_I64:
    return "div.i64";
  case NY_NIR_MOD_I64:
    return "mod.i64";
  case NY_NIR_AND_I64:
    return "and.i64";
  case NY_NIR_OR_I64:
    return "or.i64";
  case NY_NIR_XOR_I64:
    return "xor.i64";
  case NY_NIR_SHL_I64:
    return "shl.i64";
  case NY_NIR_SAR_I64:
    return "sar.i64";
  case NY_NIR_CMP_I64:
    return "cmp.i64";
  case NY_NIR_LABEL:
    return "label";
  case NY_NIR_LOAD_LOCAL:
    return "load.local";
  case NY_NIR_STORE_LOCAL:
    return "store.local";
  case NY_NIR_CALL:
    return "call";
  case NY_NIR_RET:
    return "ret";
  case NY_NIR_BR:
    return "br";
  case NY_NIR_BR_IF:
    return "br.if";
  case NYIR_CONST_F64:
    return "const.f64";
  case NYIR_ADD_F64:
    return "add.f64";
  case NYIR_SUB_F64:
    return "sub.f64";
  case NYIR_MUL_F64:
    return "mul.f64";
  case NYIR_DIV_F64:
    return "div.f64";
  case NYIR_I64_TO_F64:
    return "i64.to.f64";
  case NYIR_CMP_F64:
    return "cmp.f64";
  case NYIR_CONST_F32:
    return "const.f32";
  case NYIR_ADD_F32:
    return "add.f32";
  case NYIR_SUB_F32:
    return "sub.f32";
  case NYIR_MUL_F32:
    return "mul.f32";
  case NYIR_DIV_F32:
    return "div.f32";
  case NYIR_I64_TO_F32:
    return "i64.to.f32";
  case NYIR_F64_TO_F32:
    return "f64.to.f32";
  case NYIR_F32_TO_F64:
    return "f32.to.f64";
  case NYIR_CMP_F32:
    return "cmp.f32";
  case NYIR_ADDR_LOCAL:
    return "addr.local";
  case NYIR_LOAD_I64:
    return "load.i64";
  case NYIR_STORE_I64:
    return "store.i64";
  case NYIR_OP_COUNT:
    break;
  }
  return "unknown";
}

unsigned ny_nir_inst_effects(const ny_nir_inst_t *inst) {
  if (!inst)
    return NY_NIR_EFFECT_NONE;
  switch (inst->op) {
  case NY_NIR_LOAD_LOCAL:
    return NY_NIR_EFFECT_READ_LOCAL;
  case NY_NIR_STORE_LOCAL:
    return NY_NIR_EFFECT_WRITE_LOCAL;
  case NYIR_LOAD_I64:
    return NY_NIR_EFFECT_CALL;
  case NYIR_STORE_I64:
    return NY_NIR_EFFECT_CALL | NY_NIR_EFFECT_WRITE_LOCAL;
  case NY_NIR_CALL:
    return NY_NIR_EFFECT_CALL;
  case NY_NIR_RET:
  case NY_NIR_BR:
  case NY_NIR_BR_IF:
    return NY_NIR_EFFECT_CONTROL;
  default:
    return NY_NIR_EFFECT_NONE;
  }
}

static void ny_nir_init_inst_metadata(ny_nir_inst_t *inst) {
  if (!inst)
    return;
  inst->effects = ny_nir_inst_effects(inst);
  if (inst->op == NY_NIR_CONST_I64 && !inst->range.has_min &&
      !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = inst->imm;
    inst->range.max = inst->imm;
  } else if ((inst->op == NY_NIR_CMP_I64 || inst->op == NYIR_CMP_F64 ||
              inst->op == NYIR_CMP_F32) &&
             !inst->range.has_min &&
             !inst->range.has_max) {
    inst->range.has_min = true;
    inst->range.has_max = true;
    inst->range.min = 0;
    inst->range.max = 1;
  }
}

static void ny_nir_normalize_operands(ny_nir_inst_t *inst) {
  if (!inst)
    return;
  switch (inst->op) {
  case NY_NIR_COPY:
  case NYIR_I64_TO_F64:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_LOAD_I64:
  case NY_NIR_RET:
  case NY_NIR_BR_IF:
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_ADDR_LOCAL:
    inst->a = -1;
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NY_NIR_STORE_LOCAL:
    inst->dst = -1;
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_STORE_I64:
    inst->dst = -1;
    inst->b = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NY_NIR_CALL:
    if (inst->imm <= 0)
      inst->a = -1;
    if (inst->imm <= 1)
      inst->b = -1;
    if (inst->imm <= 2)
      inst->c = -1;
    if (inst->imm <= 3)
      inst->d = -1;
    if (inst->imm <= 4)
      inst->e = -1;
    if (inst->imm <= 5)
      inst->f = -1;
    if (inst->imm <= 6) {
      free(inst->extra_args);
      inst->extra_args = NULL;
      inst->extra_args_len = 0;
    } else {
      inst->extra_args_len = (size_t)(inst->imm - 6);
    }
    break;
  case NY_NIR_ADD_I64:
  case NY_NIR_SUB_I64:
  case NY_NIR_MUL_I64:
  case NY_NIR_DIV_I64:
  case NY_NIR_MOD_I64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NY_NIR_AND_I64:
  case NY_NIR_OR_I64:
  case NY_NIR_XOR_I64:
  case NY_NIR_SHL_I64:
  case NY_NIR_SAR_I64:
  case NY_NIR_CMP_I64:
  case NYIR_CMP_F64:
  case NYIR_CMP_F32:
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NY_NIR_NOP:
  case NY_NIR_CONST_I64:
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
  case NY_NIR_LABEL:
  case NY_NIR_LOAD_LOCAL:
  case NY_NIR_BR:
    inst->a = -1;
    inst->b = -1;
    inst->c = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    break;
  case NYIR_OP_COUNT:
    break;
  }
}

void ny_nir_refresh_metadata(ny_nir_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    ny_nir_normalize_operands(in);
    in->effects = ny_nir_inst_effects(in);
    if (in->op == NY_NIR_CONST_I64) {
      in->range = (ny_nir_range_t){.has_min = true,
                                   .has_max = true,
                                   .min = in->imm,
                                   .max = in->imm};
    } else if (in->op == NY_NIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
               in->op == NYIR_CMP_F32) {
      in->range = (ny_nir_range_t){.has_min = true,
                                   .has_max = true,
                                   .min = 0,
                                   .max = 1};
    }
  }
}

int ny_nir_emit(ny_nir_func_t *f, ny_nir_inst_t inst) {
  if (!f)
    return -1;
  ny_nir_normalize_operands(&inst);
  if (inst.dst < 0 && inst.op != NY_NIR_STORE_LOCAL && inst.op != NY_NIR_RET &&
      inst.op != NY_NIR_BR && inst.op != NY_NIR_BR_IF &&
      inst.op != NY_NIR_LABEL && inst.op != NY_NIR_NOP)
    inst.dst = f->next_value++;
  ny_nir_init_inst_metadata(&inst);
  if (f->len >= f->cap) {
    size_t cap = f->cap ? f->cap * 2 : 64;
    ny_nir_inst_t *data = (ny_nir_inst_t *)realloc(f->data, cap * sizeof(*data));
    if (!data)
      return -1;
    f->data = data;
    f->cap = cap;
  }
  f->data[f->len++] = inst;
  return inst.dst;
}

static bool nir_err(char *err, size_t err_len, const char *fmt, ...) {
  if (err && err_len > 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
  }
  return false;
}

static bool nir_inst_err(char *err, size_t err_len, const ny_nir_inst_t *in,
                         size_t index, const char *reason) {
  return nir_err(err, err_len,
                 "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
                 "imm=%" PRId64 ": %s",
                 index, in ? ny_nir_op_name(in->op) : "<null>",
                 in ? in->dst : -1, in ? in->a : -1, in ? in->b : -1,
                 in ? in->imm : 0, reason ? reason : "invalid instruction");
}

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

static bool nir_label_referenced(const ny_nir_func_t *f, int64_t label) {
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
    return nir_err(err, err_len, "native NYIR verify: missing function");
  if (f->next_value < 0)
    return nir_err(err, err_len, "native NYIR verify: invalid value count");
  bool *defined = NULL;
  if (f->next_value > 0) {
    defined = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    if (!defined)
      return nir_err(err, err_len, "native NYIR verify: out of memory");
  }
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(defined);
      return nir_inst_err(err, err_len, in, i, "unknown opcode");
    }
    if ((in->op == NY_NIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
         in->op == NYIR_CMP_F32) &&
        in->cmp > NY_NIR_CMP_GE) {
      free(defined);
      return nir_inst_err(err, err_len, in, i, "unknown comparison predicate");
    }
    unsigned required_effects = ny_nir_inst_effects(in);
    unsigned known_effects = nir_known_effect_mask();
    if ((in->effects & ~known_effects) != 0) {
      free(defined);
      return nir_inst_err(err, err_len, in, i, "invalid effect mask");
    }
    if (in->effects != required_effects) {
      free(defined);
      return nir_inst_err(err, err_len, in, i,
                          "effect mask does not match opcode effects");
    }
    if ((in->range.has_min && !in->range.has_max) ||
        (!in->range.has_min && in->range.has_max)) {
      free(defined);
      return nir_inst_err(err, err_len, in, i, "incomplete range fact");
    }
    if (in->range.has_min && in->range.has_max && in->range.min > in->range.max) {
      free(defined);
      return nir_inst_err(err, err_len, in, i, "invalid range fact");
    }
    if ((!in->debug.line && in->debug.column) ||
        (!in->debug.line && in->debug.file && in->debug.file[0])) {
      free(defined);
      return nir_inst_err(err, err_len, in, i, "invalid debug location");
    }
    if (in->dst >= f->next_value) {
      free(defined);
      return nir_inst_err(err, err_len, in, i, "invalid destination value");
    }
    if (in->dst >= 0 && defined && defined[in->dst]) {
      free(defined);
      return nir_inst_err(err, err_len, in, i,
                          "destination value is already defined");
    }
    switch (in->op) {
    case NY_NIR_NOP:
      break;
    case NY_NIR_LABEL:
      for (size_t j = 0; j < i; ++j) {
        if (f->data[j].op == NY_NIR_LABEL && f->data[j].imm == in->imm) {
          free(defined);
          return nir_inst_err(err, err_len, in, i, "duplicate label");
        }
      }
      break;
    case NY_NIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
      if (in->dst < 0) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "constant has no destination");
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
        return nir_inst_err(err, err_len, in, i, "invalid unary value operand");
      }
      break;
    case NY_NIR_LOAD_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "invalid local load");
      }
      break;
    case NYIR_ADDR_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "invalid local address");
      }
      break;
    case NY_NIR_STORE_LOCAL:
      if (in->imm < 0 || !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "invalid local store");
      }
      break;
    case NYIR_STORE_I64:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "invalid memory store");
      }
      break;
    case NY_NIR_RET:
      if (in->a >= 0 && !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "invalid return value");
      }
      break;
    case NY_NIR_BR:
      if (!nir_label_exists(f, in->imm)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "missing branch target label");
      }
      break;
    case NY_NIR_BR_IF:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_label_exists(f, in->imm)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "invalid conditional branch operand or target");
      }
      break;
    case NY_NIR_CALL:
      if (!in->symbol || !in->symbol[0]) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "call has no symbol");
      }
      if (in->imm < 0) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "negative call arg count");
      }
      if (in->imm == 0 &&
          (in->a >= 0 || in->b >= 0 || in->c >= 0 || in->d >= 0 ||
           in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "zero-argument call has value operands");
      }
      if (in->imm == 1 &&
          (in->b >= 0 || in->c >= 0 || in->d >= 0 || in->e >= 0 ||
           in->f >= 0)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "one-argument call has extra value operand");
      }
      if (in->imm == 2 &&
          (in->c >= 0 || in->d >= 0 || in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "two-argument call has extra value operand");
      }
      if (in->imm == 3 && (in->d >= 0 || in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "three-argument call has extra value operand");
      }
      if (in->imm == 4 && (in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "four-argument call has extra value operand");
      }
      if (in->imm == 5 && in->f >= 0) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "five-argument call has extra value operand");
      }
      if (in->imm > 0 && !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "call arg0 is invalid");
      }
      if (in->imm > 1 && !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "call arg1 is invalid");
      }
      if (in->imm > 2 && !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "call arg2 is invalid");
      }
      if (in->imm > 3 && !nir_value_defined(f, defined, in->d)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "call arg3 is invalid");
      }
      if (in->imm > 4 && !nir_value_defined(f, defined, in->e)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "call arg4 is invalid");
      }
      if (in->imm > 5 && !nir_value_defined(f, defined, in->f)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "call arg5 is invalid");
      }
      if (in->imm > NY_NIR_CALL_MAX_ARGS) {
        free(defined);
        return nir_inst_err(err, err_len, in, i,
                            "call exceeds the maximum supported argument count");
      }
      if (in->imm <= 6) {
        if (in->extra_args || in->extra_args_len != 0) {
          free(defined);
          return nir_inst_err(err, err_len, in, i,
                              "call has stray stack-args for a register-only arity");
        }
      } else {
        size_t want = (size_t)(in->imm - 6);
        if (!in->extra_args || in->extra_args_len != want) {
          free(defined);
          return nir_inst_err(err, err_len, in, i,
                              "call stack-arg count does not match arity");
        }
        for (size_t k = 0; k < want; ++k) {
          if (!nir_value_defined(f, defined, in->extra_args[k])) {
            free(defined);
            return nir_inst_err(err, err_len, in, i, "call stack-arg is invalid");
          }
        }
      }
      break;
    default:
      if (in->op == NYIR_OP_COUNT) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "unknown opcode");
      }
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return nir_inst_err(err, err_len, in, i, "invalid value operands");
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


static bool ny_nir_analyze_binary_fold(ny_nir_op_t op, int64_t a, int64_t b,
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

static bool ny_nir_analyze_cmp_fold(ny_nir_cmp_t cmp, int64_t a, int64_t b,
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
    return nir_err(err, err_len, "native NYIR metadata: missing function");
  if (!summary)
    return nir_err(err, err_len, "native NYIR metadata: missing summary output");
  char verify_err[256] = {0};
  if (!ny_nir_verify(f, verify_err, sizeof(verify_err)))
    return nir_err(err, err_len, "native NYIR metadata: verifier rejected input: %s",
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
    return nir_err(err, err_len, "native NYIR analysis: missing function");
  if (f->next_value < 0)
    return nir_err(err, err_len, "native NYIR analysis: invalid value count");
  if ((size_t)f->next_value > fact_count)
    return nir_err(err, err_len,
                   "native NYIR analysis: fact table too small (%zu < %d)",
                   fact_count, f->next_value);
  if (facts && fact_count > 0)
    memset(facts, 0, fact_count * sizeof(*facts));

  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT)
      return nir_inst_err(err, err_len, in, i, "unknown opcode");
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
    return nir_err(err, err_len, "native NYIR constraints: missing function");
  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return nir_err(err, err_len, "native NYIR constraints: out of memory");
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(known);
      free(value);
      return nir_inst_err(err, err_len, in, i, "unknown opcode");
    }
    if (in->range.has_min && in->range.has_max && in->range.min > in->range.max) {
      free(known);
      free(value);
      return nir_inst_err(err, err_len, in, i, "range minimum exceeds maximum");
    }
    if ((in->op == NY_NIR_SHL_I64 || in->op == NY_NIR_SAR_I64) &&
        in->b >= 0 && known && known[in->b] &&
        (value[in->b] < 0 || value[in->b] >= 64)) {
      free(known);
      free(value);
      return nir_inst_err(err, err_len, in, i, "constant shift amount out of range");
    }
    if ((in->op == NY_NIR_DIV_I64 || in->op == NY_NIR_MOD_I64) &&
        in->b >= 0 && known && known[in->b] && value[in->b] == 0) {
      free(known);
      free(value);
      return nir_inst_err(err, err_len, in, i, "constant divide/modulo by zero");
    }
    if ((in->op == NY_NIR_DIV_I64 || in->op == NY_NIR_MOD_I64) &&
        in->a >= 0 && in->b >= 0 && known && known[in->a] && known[in->b] &&
        value[in->a] == INT64_MIN && value[in->b] == -1) {
      free(known);
      free(value);
      return nir_inst_err(err, err_len, in, i,
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

void ny_nir_dump(FILE *out, const ny_nir_func_t *f, const char *name) {
  if (!out)
    out = stderr;
  fprintf(out, "nyir function %s values=%d insts=%zu\n",
          name && name[0] ? name : "<anon>", f ? f->next_value : 0,
          f ? f->len : 0);
  if (!f)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    fprintf(out, "  %04zu: ", i);
    if (in->dst >= 0)
      fprintf(out, "v%d = ", in->dst);
    fprintf(out, "%s", ny_nir_op_name(in->op));
    if (in->op == NY_NIR_CONST_I64)
      fprintf(out, " %" PRId64, in->imm);
    else if (in->op == NYIR_CONST_F64)
      fprintf(out, " %.17g", ny_nir_bits_to_f64(in->imm));
    else if (in->op == NYIR_CONST_F32)
      fprintf(out, " %.9g", (double)ny_nir_bits_to_f32(in->imm));
    else if (in->op == NY_NIR_LABEL)
      fprintf(out, " L%" PRId64, in->imm);
    else if (in->op == NY_NIR_COPY || in->op == NYIR_I64_TO_F64 ||
             in->op == NYIR_I64_TO_F32 || in->op == NYIR_F64_TO_F32 ||
             in->op == NYIR_F32_TO_F64) {
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
    } else if (in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL ||
               in->op == NYIR_ADDR_LOCAL) {
      fprintf(out, " local#%" PRId64, in->imm);
      if (in->symbol)
        fprintf(out, "(%s)", in->symbol);
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
    } else if (in->op == NY_NIR_CALL) {
      fprintf(out, " %s argc=%" PRId64, in->symbol ? in->symbol : "<null>",
              in->imm);
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
      if (in->b >= 0)
        fprintf(out, " v%d", in->b);
      if (in->c >= 0)
        fprintf(out, " v%d", in->c);
      if (in->d >= 0)
        fprintf(out, " v%d", in->d);
      if (in->e >= 0)
        fprintf(out, " v%d", in->e);
      if (in->f >= 0)
        fprintf(out, " v%d", in->f);
      for (size_t k = 0; k < in->extra_args_len; ++k)
        fprintf(out, " v%d", in->extra_args[k]);
    } else if (in->op == NY_NIR_BR || in->op == NY_NIR_BR_IF) {
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
      fprintf(out, " L%" PRId64, in->imm);
    } else {
      if (in->a >= 0)
        fprintf(out, " v%d", in->a);
      if (in->b >= 0)
        fprintf(out, " v%d", in->b);
      if (in->imm)
        fprintf(out, " imm=%" PRId64, in->imm);
    }
    if (in->effects)
      fprintf(out, " effects=0x%x", in->effects);
    if (in->range.has_min || in->range.has_max) {
      fprintf(out, " range=");
      if (in->range.has_min)
        fprintf(out, "%" PRId64, in->range.min);
      else
        fputc('*', out);
      fputs("..", out);
      if (in->range.has_max)
        fprintf(out, "%" PRId64, in->range.max);
      else
        fputc('*', out);
    }
    if (in->debug.line) {
      fprintf(out, " loc=");
      if (in->debug.file && in->debug.file[0])
        fprintf(out, "%s:", in->debug.file);
      fprintf(out, "%u:%u", in->debug.line, in->debug.column);
    }
    fprintf(out, "\n");
  }
}

static bool ny_nir_write_u8(FILE *out, uint8_t v) {
  return out && fwrite(&v, 1, 1, out) == 1;
}

static bool ny_nir_write_u16le(FILE *out, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool ny_nir_write_u32le(FILE *out, uint32_t v) {
  uint8_t b[4] = {
      (uint8_t)(v & 0xff),
      (uint8_t)((v >> 8) & 0xff),
      (uint8_t)((v >> 16) & 0xff),
      (uint8_t)((v >> 24) & 0xff),
  };
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool ny_nir_write_i32le(FILE *out, int32_t v) {
  return ny_nir_write_u32le(out, (uint32_t)v);
}

static bool ny_nir_write_i64le(FILE *out, int64_t v) {
  uint64_t u = (uint64_t)v;
  uint8_t b[8] = {
      (uint8_t)(u & 0xff),
      (uint8_t)((u >> 8) & 0xff),
      (uint8_t)((u >> 16) & 0xff),
      (uint8_t)((u >> 24) & 0xff),
      (uint8_t)((u >> 32) & 0xff),
      (uint8_t)((u >> 40) & 0xff),
      (uint8_t)((u >> 48) & 0xff),
      (uint8_t)((u >> 56) & 0xff),
  };
  return out && fwrite(b, 1, sizeof(b), out) == sizeof(b);
}

static bool ny_nir_write_str(FILE *out, const char *s) {
  uint32_t n = s ? (uint32_t)strlen(s) : 0;
  return ny_nir_write_u32le(out, n) &&
         (n == 0 || fwrite(s, 1, n, out) == n);
}

static bool ny_nir_read_exact(FILE *in, void *p, size_t n) {
  return in && (n == 0 || fread(p, 1, n, in) == n);
}

static bool ny_nir_read_u16le(FILE *in, uint16_t *out) {
  uint8_t b[2];
  if (!out || !ny_nir_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool ny_nir_read_u32le(FILE *in, uint32_t *out) {
  uint8_t b[4];
  if (!out || !ny_nir_read_exact(in, b, sizeof(b)))
    return false;
  *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

static bool ny_nir_read_i32le(FILE *in, int32_t *out) {
  uint32_t u = 0;
  if (!out || !ny_nir_read_u32le(in, &u))
    return false;
  *out = (int32_t)u;
  return true;
}

static bool ny_nir_read_i64le(FILE *in, int64_t *out) {
  uint8_t b[8];
  if (!out || !ny_nir_read_exact(in, b, sizeof(b)))
    return false;
  uint64_t u = (uint64_t)b[0] | ((uint64_t)b[1] << 8) |
               ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
               ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
               ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
  *out = (int64_t)u;
  return true;
}

static bool ny_nir_read_str(FILE *in, char **out, uint32_t max_len) {
  uint32_t n = 0;
  if (!out || !ny_nir_read_u32le(in, &n) || n > max_len)
    return false;
  char *s = (char *)malloc((size_t)n + 1);
  if (!s)
    return false;
  if (!ny_nir_read_exact(in, s, n)) {
    free(s);
    return false;
  }
  s[n] = '\0';
  *out = s;
  return true;
}

bool ny_nir_dump_binary(FILE *out, const ny_nir_func_t *f, const char *name) {
  if (!out || !f)
    return false;
  if (fwrite("NYIR", 1, 4, out) != 4)
    return false;
  if (!ny_nir_write_u16le(out, 5) ||              /* format version */
      !ny_nir_write_u16le(out, 0) ||              /* flags */
      !ny_nir_write_str(out, name && name[0] ? name : "<anon>") ||
      !ny_nir_write_i32le(out, f->next_value) ||
      !ny_nir_write_u32le(out, (uint32_t)f->len))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (!ny_nir_write_u16le(out, (uint16_t)in->op) ||
        !ny_nir_write_u16le(out, (uint16_t)in->cmp) ||
        !ny_nir_write_i32le(out, in->dst) ||
        !ny_nir_write_i32le(out, in->a) ||
        !ny_nir_write_i32le(out, in->b) ||
        !ny_nir_write_i32le(out, in->c) ||
        !ny_nir_write_i32le(out, in->d) ||
        !ny_nir_write_i32le(out, in->e) ||
        !ny_nir_write_i32le(out, in->f) ||
        !ny_nir_write_i64le(out, in->imm) ||
        !ny_nir_write_u32le(out, in->flags) ||
        !ny_nir_write_u32le(out, in->effects) ||
        !ny_nir_write_u32le(out, in->debug.line) ||
        !ny_nir_write_u32le(out, in->debug.column) ||
        !ny_nir_write_u8(out, in->range.has_min ? 1 : 0) ||
        !ny_nir_write_u8(out, in->range.has_max ? 1 : 0) ||
        !ny_nir_write_i64le(out, in->range.min) ||
        !ny_nir_write_i64le(out, in->range.max) ||
        !ny_nir_write_str(out, in->debug.file) ||
        !ny_nir_write_str(out, in->symbol) ||
        !ny_nir_write_u32le(out, (uint32_t)in->extra_args_len))
      return false;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (!ny_nir_write_i32le(out, in->extra_args[k]))
        return false;
    }
  }
  return true;
}

bool ny_nir_load_binary(FILE *in, ny_nir_func_t *out, char *name,
                        size_t name_len, char *err, size_t err_len) {
  if (!in || !out)
    return nir_err(err, err_len, "native NYIR load: missing input");
  ny_nir_func_t loaded = {0};
  char magic[4];
  uint16_t version = 0;
  uint16_t flags = 0;
  char *loaded_name = NULL;
  int32_t next_value = 0;
  uint32_t inst_count = 0;

  if (!ny_nir_read_exact(in, magic, sizeof(magic)) ||
      memcmp(magic, "NYIR", 4) != 0)
    goto malformed;
  if (!ny_nir_read_u16le(in, &version) || !ny_nir_read_u16le(in, &flags))
    goto malformed;
  if (version != 1 && version != 2 && version != 3 && version != 4 &&
      version != 5)
    return nir_err(err, err_len, "native NYIR load: unsupported version %u",
                   (unsigned)version);
  if (flags != 0)
    return nir_err(err, err_len, "native NYIR load: unsupported flags 0x%x",
                   (unsigned)flags);
  if (!ny_nir_read_str(in, &loaded_name, 1024) ||
      !ny_nir_read_i32le(in, &next_value) ||
      !ny_nir_read_u32le(in, &inst_count))
    goto malformed;
  if (next_value < 0)
    goto malformed;
  if (inst_count > (uint32_t)(SIZE_MAX / sizeof(*loaded.data)))
    return nir_err(err, err_len, "native NYIR load: instruction count too large");
  if (inst_count > 0) {
    loaded.data = (ny_nir_inst_t *)calloc(inst_count, sizeof(*loaded.data));
    if (!loaded.data) {
      free(loaded_name);
      return nir_err(err, err_len, "native NYIR load: out of memory");
    }
    loaded.cap = inst_count;
  }
  loaded.next_value = next_value;

  for (uint32_t i = 0; i < inst_count; ++i) {
    ny_nir_inst_t inst = {.dst = -1,
                          .a = -1,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1};
    uint16_t op = 0;
    uint16_t cmp = 0;
    int32_t dst = 0;
    int32_t a = 0;
    int32_t b = 0;
    uint32_t flags32 = 0;
    uint32_t effects32 = 0;
    uint32_t debug_line = 0;
    uint32_t debug_column = 0;
    uint8_t has_min = 0;
    uint8_t has_max = 0;
    char *debug_file = NULL;
    char *symbol = NULL;
    if (!ny_nir_read_u16le(in, &op) || !ny_nir_read_u16le(in, &cmp) ||
        !ny_nir_read_i32le(in, &dst) || !ny_nir_read_i32le(in, &a) ||
        !ny_nir_read_i32le(in, &b))
      goto malformed;
    if (version >= 3) {
      if (!ny_nir_read_i32le(in, &inst.c) || !ny_nir_read_i32le(in, &inst.d))
        goto malformed;
    }
    if (version >= 4) {
      if (!ny_nir_read_i32le(in, &inst.e) || !ny_nir_read_i32le(in, &inst.f))
        goto malformed;
    }
    if (!ny_nir_read_i64le(in, &inst.imm) || !ny_nir_read_u32le(in, &flags32))
      goto malformed;
    if (version >= 2) {
      if (!ny_nir_read_u32le(in, &effects32) ||
          !ny_nir_read_u32le(in, &debug_line) ||
          !ny_nir_read_u32le(in, &debug_column) ||
          !ny_nir_read_exact(in, &has_min, 1) ||
          !ny_nir_read_exact(in, &has_max, 1) ||
          !ny_nir_read_i64le(in, &inst.range.min) ||
          !ny_nir_read_i64le(in, &inst.range.max) ||
          !ny_nir_read_str(in, &debug_file, 4096))
        goto malformed;
    }
    if (!ny_nir_read_str(in, &symbol, 4096)) {
      free(debug_file);
      goto malformed;
    }
    uint32_t extra_len = 0;
    int *extra = NULL;
    if (version >= 5) {
      if (!ny_nir_read_u32le(in, &extra_len) ||
          extra_len > NY_NIR_CALL_MAX_ARGS) {
        free(symbol);
        goto malformed;
      }
      if (extra_len > 0) {
        extra = (int *)malloc((size_t)extra_len * sizeof(*extra));
        if (!extra) {
          free(symbol);
          free(loaded_name);
          ny_nir_func_free(&loaded);
          return nir_err(err, err_len, "native NYIR load: out of memory");
        }
        for (uint32_t k = 0; k < extra_len; ++k) {
          int32_t v = 0;
          if (!ny_nir_read_i32le(in, &v)) {
            free(extra);
            free(symbol);
            goto malformed;
          }
          extra[k] = (int)v;
        }
      }
    }
    if (op >= NYIR_OP_COUNT || cmp > NY_NIR_CMP_GE) {
      free(extra);
      free(symbol);
      goto malformed;
    }
    inst.extra_args = extra;
    inst.extra_args_len = extra_len;
    inst.op = (ny_nir_op_t)op;
    inst.cmp = (ny_nir_cmp_t)cmp;
    inst.dst = dst;
    inst.a = a;
    inst.b = b;
    inst.flags = flags32;
    inst.effects = effects32;
    inst.debug.line = debug_line;
    inst.debug.column = debug_column;
    inst.range.has_min = has_min != 0;
    inst.range.has_max = has_max != 0;
    if (debug_file && debug_file[0]) {
      inst.debug.file = ny_nir_func_own_symbol(&loaded, debug_file);
      if (!inst.debug.file) {
        free(inst.extra_args);
        free(symbol);
        free(loaded_name);
        ny_nir_func_free(&loaded);
        return nir_err(err, err_len, "native NYIR load: out of memory");
      }
    } else {
      free(debug_file);
    }
    if (symbol[0]) {
      inst.symbol = ny_nir_func_own_symbol(&loaded, symbol);
      if (!inst.symbol) {
        free(inst.extra_args);
        free(loaded_name);
        ny_nir_func_free(&loaded);
        return nir_err(err, err_len, "native NYIR load: out of memory");
      }
    } else {
      free(symbol);
    }
    loaded.data[loaded.len++] = inst;
  }

  if (!ny_nir_verify(&loaded, err, err_len)) {
    free(loaded_name);
    ny_nir_func_free(&loaded);
    return false;
  }
  ny_nir_refresh_metadata(&loaded);
  if (name && name_len > 0) {
    snprintf(name, name_len, "%s", loaded_name ? loaded_name : "");
  }
  free(loaded_name);
  ny_nir_func_free(out);
  *out = loaded;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;

malformed:
  free(loaded_name);
  ny_nir_func_free(&loaded);
  return nir_err(err, err_len, "native NYIR load: malformed binary dump");
}



void ny_nir_eval_result_dump(FILE *out, const char *name,
                             const ny_nir_eval_result_t *result) {
  if (!out)
    out = stderr;
  fprintf(out,
          "nyir vm profile function=%s returned=%s result=%" PRId64 " steps=%zu branches_taken=%zu branches_not_taken=%zu calls=%zu max_pc=%zu max_value=%zu max_local=%zu\n",
          name && name[0] ? name : "rt_main",
          result && result->returned ? "yes" : "no",
          result ? result->result : 0, result ? result->steps : 0,
          result ? result->branch_taken : 0,
          result ? result->branch_not_taken : 0,
          result ? result->call_count : 0,
          result ? result->max_pc : 0,
          result ? result->max_value_index : 0,
          result ? result->max_local_index : 0);
  if (!result)
    return;
  for (size_t i = 0; i < (size_t)NYIR_OP_COUNT; ++i) {
    if (result->op_counts[i] == 0)
      continue;
    fprintf(out, "  op %-14s %zu\n", ny_nir_op_name((ny_nir_op_t)i),
            result->op_counts[i]);
  }
}

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
    return nir_err(err, err_len, "native NYIR VM: missing function");
  char verify_err[256] = {0};
  if (!ny_nir_verify(f, verify_err, sizeof(verify_err)))
    return nir_err(err, err_len, "native NYIR VM: verifier rejected input: %s",
                   verify_err);
  if (f->next_value < 0)
    return nir_err(err, err_len, "native NYIR VM: invalid value count");
  size_t value_count = (size_t)f->next_value;
  int64_t *values = value_count ? (int64_t *)calloc(value_count, sizeof(*values)) : NULL;
  bool *known = value_count ? (bool *)calloc(value_count, sizeof(*known)) : NULL;
  if (value_count && (!values || !known)) {
    free(values);
    free(known);
    return nir_err(err, err_len, "native NYIR VM: out of memory");
  }
  if (result)
    memset(result, 0, sizeof(*result));
  if (max_steps == 0)
    max_steps = 1000000;

  size_t pc = 0;
  size_t steps = 0;
  const ny_nir_inst_t *in = NULL;
  size_t inst_index = 0;
  while (pc < f->len) {
    if (++steps > max_steps) {
      free(values);
      free(known);
      return nir_err(err, err_len, "native NYIR VM: step limit exceeded");
    }
    inst_index = pc;
    if (result && inst_index > result->max_pc)
      result->max_pc = inst_index;
    in = &f->data[pc++];
    if (result && in->op >= 0 && in->op < NYIR_OP_COUNT)
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
      if (!ny_nir_eval_find_label(f, in->imm, &pc))
        goto missing_label;
      break;
    case NY_NIR_BR_IF:
      if (!ny_nir_eval_read_value(values, known, in->a, &a))
        goto missing_value;
      if (a) {
        if (result)
          result->branch_taken++;
        if (!ny_nir_eval_find_label(f, in->imm, &pc))
          goto missing_label;
      } else if (result) {
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
      if (err && err_len > 0)
        err[0] = '\0';
      return true;
    case NY_NIR_CALL: {
      if (result)
        result->call_count++;
      if (!resolver) {
        free(values);
        free(known);
        return nir_inst_err(err, err_len, in, inst_index,
                            "NYIR VM does not execute external calls yet");
      }
      if (in->imm < 0 || in->imm > NY_NIR_CALL_MAX_ARGS) {
        free(values);
        free(known);
        return nir_inst_err(err, err_len, in, inst_index,
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
  if (err && err_len > 0)
    err[0] = '\0';
  return true;

missing_value:
  free(values);
  free(known);
  return nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM read an unavailable value");
bad_local:
  free(values);
  free(known);
  return nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM local slot is out of range");
missing_label:
  free(values);
  free(known);
  return nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM branch target is missing");
unsupported:
  free(values);
  free(known);
  return nir_inst_err(err, err_len, in, inst_index,
                      "NYIR VM operation is unsupported for these operands");
}

bool ny_nir_eval(const ny_nir_func_t *f, int64_t *locals, size_t local_count,
                 size_t max_steps, ny_nir_eval_result_t *result, char *err,
                 size_t err_len) {
  return ny_nir_eval_with_calls(f, locals, local_count, max_steps, result, NULL,
                                NULL, err, err_len);
}

static void ny_nir_collect_stats(const ny_nir_func_t *f, size_t *insts,
                                 int *values, size_t *ops, size_t op_count) {
  if (insts)
    *insts = f ? f->len : 0;
  if (values)
    *values = f ? f->next_value : 0;
  if (ops && op_count > 0) {
    memset(ops, 0, op_count * sizeof(*ops));
    for (size_t i = 0; f && i < f->len; ++i) {
      if ((size_t)f->data[i].op < op_count)
        ops[f->data[i].op]++;
    }
  }
}

void ny_nir_dump_stats(FILE *out, const ny_nir_opt_stats_t *stats) {
  if (!out)
    out = stderr;
  if (!stats)
    return;
  size_t removed = stats->before_insts > stats->after_insts
                       ? stats->before_insts - stats->after_insts
                       : 0;
  fprintf(out,
          "nyir optimize before_insts=%zu after_insts=%zu removed=%zu "
          "before_values=%d after_values=%d\n",
          stats->before_insts, stats->after_insts, removed,
          stats->before_values, stats->after_values);
  fputs("nyir optimize ops", out);
  for (size_t op = 0; op < (size_t)NYIR_OP_COUNT; ++op) {
    size_t before = stats->before_ops[op];
    size_t after = stats->after_ops[op];
    if (!before && !after)
      continue;
    fprintf(out, " %s:%zu->%zu", ny_nir_op_name((ny_nir_op_t)op), before,
            after);
  }
  fputc('\n', out);
}

static bool nir_binary_fold(ny_nir_op_t op, int64_t a, int64_t b, int64_t *out) {
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

static bool nir_cmp_fold(ny_nir_cmp_t cmp, int64_t a, int64_t b, int64_t *out) {
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

bool ny_nir_const_fold(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
  int64_t max_local = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > max_local)
      max_local = in->imm;
  }
  bool *local_known = NULL;
  bool *local_addr_taken = NULL;
  int64_t *local_value = NULL;
  if (max_local >= 0) {
    local_known = (bool *)calloc((size_t)max_local + 1, sizeof(bool));
    local_addr_taken = (bool *)calloc((size_t)max_local + 1, sizeof(bool));
    local_value = (int64_t *)calloc((size_t)max_local + 1, sizeof(int64_t));
  }
  if (!known || !value ||
      (max_local >= 0 && (!local_known || !local_addr_taken || !local_value))) {
    free(known);
    free(value);
    free(local_known);
    free(local_addr_taken);
    free(local_value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
      continue;
    }
    if (in->op == NY_NIR_COPY && in->dst >= 0) {
      if (in->a >= 0 && known[in->a]) {
        in->op = NY_NIR_CONST_I64;
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
    if (in->op == NY_NIR_STORE_LOCAL && in->imm >= 0 && in->imm <= max_local) {
      if (in->a >= 0 && known[in->a]) {
        local_known[in->imm] = true;
        local_value[in->imm] = value[in->a];
      } else {
        local_known[in->imm] = false;
      }
      continue;
    }
    if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
        in->imm <= max_local && local_known[in->imm]) {
      in->op = NY_NIR_CONST_I64;
      in->a = -1;
      in->b = -1;
      in->imm = local_value[in->imm];
      in->symbol = NULL;
      known[in->dst] = true;
      value[in->dst] = in->imm;
      continue;
    }
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 && in->imm <= max_local) {
      local_addr_taken[in->imm] = true;
      local_known[in->imm] = false;
      if (in->dst >= 0)
        known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_STORE_I64 && local_known && local_addr_taken) {
      for (int64_t local = 0; local <= max_local; ++local) {
        if (local_addr_taken[local])
          local_known[local] = false;
      }
    }
    if (in->op == NY_NIR_BR_IF && in->a >= 0 && known[in->a]) {
      if (value[in->a]) {
        in->op = NY_NIR_BR;
        in->a = -1;
        in->b = -1;
      } else {
        in->op = NY_NIR_NOP;
        in->a = -1;
        in->b = -1;
        in->imm = 0;
      }
    }
    if (in->op == NY_NIR_LABEL || in->op == NY_NIR_BR ||
        in->op == NY_NIR_BR_IF || in->op == NY_NIR_CALL) {
      if (local_known)
        memset(local_known, 0, ((size_t)max_local + 1) * sizeof(bool));
    }
    int64_t folded = 0;
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && known[in->a] &&
        known[in->b] &&
        (nir_binary_fold(in->op, value[in->a], value[in->b], &folded) ||
         (in->op == NY_NIR_CMP_I64 &&
          nir_cmp_fold(in->cmp, value[in->a], value[in->b], &folded)))) {
      in->op = NY_NIR_CONST_I64;
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
  free(known);
  free(value);
  free(local_known);
  free(local_addr_taken);
  free(local_value);
  return true;
}

static int nir_alias_find(const int *alias, int v) {
  if (!alias || v < 0)
    return v;
  int cur = v;
  for (int depth = 0; depth < 64 && alias[cur] >= 0 && alias[cur] != cur; ++depth)
    cur = alias[cur];
  return cur;
}

bool ny_nir_copy_prop(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  int *alias = (int *)malloc((size_t)f->next_value * sizeof(int));
  if (!alias)
    return false;
  for (int i = 0; i < f->next_value; ++i)
    alias[i] = i;
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->a >= 0)
      in->a = nir_alias_find(alias, in->a);
    if (in->b >= 0)
      in->b = nir_alias_find(alias, in->b);
    if (in->c >= 0)
      in->c = nir_alias_find(alias, in->c);
    if (in->d >= 0)
      in->d = nir_alias_find(alias, in->d);
    if (in->e >= 0)
      in->e = nir_alias_find(alias, in->e);
    if (in->f >= 0)
      in->f = nir_alias_find(alias, in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        in->extra_args[k] = nir_alias_find(alias, in->extra_args[k]);
    }
    if (in->dst >= 0) {
      if (in->op == NY_NIR_COPY && in->a >= 0)
        alias[in->dst] = nir_alias_find(alias, in->a);
      else
        alias[in->dst] = in->dst;
    }
  }
  free(alias);
  return true;
}

static bool nir_collect_consts(const ny_nir_func_t *f, bool *known,
                               int64_t *value) {
  if (!f || !known || !value)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    } else if (in->op == NY_NIR_COPY && in->dst >= 0 && in->a >= 0 &&
               known[in->a]) {
      known[in->dst] = true;
      value[in->dst] = value[in->a];
    } else if (in->dst >= 0) {
      known[in->dst] = false;
    }
  }
  return true;
}

static void nir_make_copy(ny_nir_inst_t *in, int src) {
  int dst = in->dst;
  *in = (ny_nir_inst_t){.op = NY_NIR_COPY, .dst = dst, .a = src, .b = -1};
}

static void nir_make_const(ny_nir_inst_t *in, int64_t value) {
  int dst = in->dst;
  *in = (ny_nir_inst_t){.op = NY_NIR_CONST_I64,
                        .dst = dst,
                        .a = -1,
                        .b = -1,
                        .imm = value};
}

static bool nir_cmp_same_value(ny_nir_cmp_t cmp, int64_t *out) {
  if (!out)
    return false;
  switch (cmp) {
  case NY_NIR_CMP_EQ:
  case NY_NIR_CMP_LE:
  case NY_NIR_CMP_GE:
    *out = 1;
    return true;
  case NY_NIR_CMP_NE:
  case NY_NIR_CMP_LT:
  case NY_NIR_CMP_GT:
    *out = 0;
    return true;
  }
  return false;
}

static bool nir_cmp_range_fold(ny_nir_cmp_t cmp, const ny_nir_range_t *a,
                               const ny_nir_range_t *b, int64_t *out) {
  if (!a || !b || !out || !a->has_min || !a->has_max || !b->has_min ||
      !b->has_max)
    return false;
  bool disjoint = a->max < b->min || b->max < a->min;
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    if (disjoint) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_NE:
    if (disjoint) {
      *out = 1;
      return true;
    }
    return false;
  case NY_NIR_CMP_LT:
    if (a->max < b->min) {
      *out = 1;
      return true;
    }
    if (a->min >= b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_LE:
    if (a->max <= b->min) {
      *out = 1;
      return true;
    }
    if (a->min > b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_GT:
    if (a->min > b->max) {
      *out = 1;
      return true;
    }
    if (a->max <= b->min) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_GE:
    if (a->min >= b->max) {
      *out = 1;
      return true;
    }
    if (a->max < b->min) {
      *out = 0;
      return true;
    }
    return false;
  }
  return false;
}

bool ny_nir_peephole(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
  ny_nir_value_fact_t *facts =
      (ny_nir_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  if (!known || !value || !facts) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  if (!nir_collect_consts(f, known, value) ||
      !ny_nir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    bool ak = known[in->a];
    bool bk = known[in->b];
    int64_t av = ak ? value[in->a] : 0;
    int64_t bv = bk ? value[in->b] : 0;
    switch (in->op) {
    case NY_NIR_ADD_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_SUB_I64:
      if (in->a == in->b)
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      break;
    case NY_NIR_MUL_I64:
      if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 1)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_DIV_I64:
      if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      break;
    case NY_NIR_MOD_I64:
      if (bk && (bv == 1 || bv == -1))
        nir_make_const(in, 0);
      else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      break;
    case NY_NIR_AND_I64:
      if (in->a == in->b)
        nir_make_copy(in, in->a);
      else if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == -1)
        nir_make_copy(in, in->a);
      else if (ak && av == -1)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_OR_I64:
      if (in->a == in->b)
        nir_make_copy(in, in->a);
      else if ((bk && bv == -1) || (ak && av == -1))
        nir_make_const(in, -1);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_XOR_I64:
      if (in->a == in->b)
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_SHL_I64:
    case NY_NIR_SAR_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_const(in, 0);
      break;
    case NY_NIR_CMP_I64: {
      int64_t folded = 0;
      if (in->a == in->b) {
        if (nir_cmp_same_value(in->cmp, &folded))
          nir_make_const(in, folded);
      } else if (in->a >= 0 && in->b >= 0 &&
                 nir_cmp_range_fold(in->cmp, &facts[in->a].range,
                                    &facts[in->b].range, &folded)) {
        nir_make_const(in, folded);
      }
      break;
    }
    default:
      break;
    }
  }
  free(known);
  free(value);
  free(facts);
  return true;
}

static size_t nir_next_non_nop(const ny_nir_func_t *f, size_t start) {
  if (!f)
    return 0;
  for (size_t i = start; i < f->len; ++i) {
    if (f->data[i].op != NY_NIR_NOP)
      return i;
  }
  return f->len;
}

bool ny_nir_cfg_simplify(ny_nir_func_t *f) {
  if (!f)
    return true;

  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return false;
    }
    if (!nir_collect_consts(f, known, value)) {
      free(known);
      free(value);
      return false;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_BR_IF && in->a >= 0 && known && known[in->a]) {
      int64_t target = in->imm;
      *in = value[in->a] != 0
                ? (ny_nir_inst_t){.op = NY_NIR_BR,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .imm = target}
                : (ny_nir_inst_t){.op = NY_NIR_NOP, .dst = -1, .a = -1, .b = -1};
    }
    if (in->op != NY_NIR_BR)
      continue;
    size_t next = nir_next_non_nop(f, i + 1);
    if (next < f->len && f->data[next].op == NY_NIR_LABEL &&
        f->data[next].imm == in->imm) {
      *in = (ny_nir_inst_t){.op = NY_NIR_NOP, .dst = -1, .a = -1, .b = -1};
    }
  }
  free(known);
  free(value);
  return true;
}

bool ny_nir_dce(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *used = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  if (!used)
    return false;

  bool reachable = true;
  bool fallthrough = true;
  bool first_inst = true;
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_LABEL) {
      reachable = first_inst || fallthrough || nir_label_referenced(f, in->imm);
      fallthrough = reachable;
    }
    if (!reachable && in->op != NY_NIR_LABEL) {
      free(in->extra_args);
      in->op = NY_NIR_NOP;
      in->dst = -1;
      in->a = -1;
      in->b = -1;
      in->c = -1;
      in->d = -1;
      in->e = -1;
      in->f = -1;
      in->imm = 0;
      in->symbol = NULL;
      in->extra_args = NULL;
      in->extra_args_len = 0;
      first_inst = false;
      continue;
    }
    if (in->op == NY_NIR_RET || in->op == NY_NIR_BR) {
      reachable = false;
      fallthrough = false;
    } else if (in->op != NY_NIR_NOP) {
      fallthrough = reachable;
    }
    first_inst = false;
  }

  for (size_t i = f->len; i > 0; --i) {
    ny_nir_inst_t *in = &f->data[i - 1];
    bool side_effect = in->effects != NY_NIR_EFFECT_NONE ||
                       in->op == NY_NIR_RET || in->op == NY_NIR_BR ||
                       in->op == NY_NIR_BR_IF;
    if (in->op == NY_NIR_LABEL)
      side_effect = nir_label_referenced(f, in->imm);
    bool keep = side_effect || (in->dst >= 0 && used[in->dst]);
    if (!keep) {
      free(in->extra_args);
      in->op = NY_NIR_NOP;
      in->dst = -1;
      in->a = -1;
      in->b = -1;
      in->c = -1;
      in->d = -1;
      in->e = -1;
      in->f = -1;
      in->imm = 0;
      in->symbol = NULL;
      in->extra_args = NULL;
      in->extra_args_len = 0;
      continue;
    }
    if (in->a >= 0)
      used[in->a] = true;
    if (in->b >= 0)
      used[in->b] = true;
    if (in->c >= 0)
      used[in->c] = true;
    if (in->d >= 0)
      used[in->d] = true;
    if (in->e >= 0)
      used[in->e] = true;
    if (in->f >= 0)
      used[in->f] = true;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        used[in->extra_args[k]] = true;
    }
  }
  free(used);
  return true;
}

static bool nir_remap_value(const int *map, int map_len, int value, int *out) {
  if (!out)
    return false;
  if (value < 0) {
    *out = value;
    return true;
  }
  if (!map || value >= map_len || map[value] < 0)
    return false;
  *out = map[value];
  return true;
}

bool ny_nir_compact(ny_nir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  size_t out = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NY_NIR_NOP)
      continue;
    if (out != i)
      f->data[out] = f->data[i];
    out++;
  }
  f->len = out;

  if (f->next_value <= 0)
    return true;

  int *map = (int *)malloc((size_t)f->next_value * sizeof(*map));
  if (!map)
    return false;
  for (int i = 0; i < f->next_value; ++i)
    map[i] = -1;

  int next = 0;
  for (size_t i = 0; i < f->len; ++i) {
    int dst = f->data[i].dst;
    if (dst >= 0) {
      if (dst >= f->next_value) {
        free(map);
        return false;
      }
      map[dst] = next++;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (!nir_remap_value(map, f->next_value, in->dst, &in->dst) ||
        !nir_remap_value(map, f->next_value, in->a, &in->a) ||
        !nir_remap_value(map, f->next_value, in->b, &in->b) ||
        !nir_remap_value(map, f->next_value, in->c, &in->c) ||
        !nir_remap_value(map, f->next_value, in->d, &in->d) ||
        !nir_remap_value(map, f->next_value, in->e, &in->e) ||
        !nir_remap_value(map, f->next_value, in->f, &in->f)) {
      free(map);
      return false;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (!nir_remap_value(map, f->next_value, in->extra_args[k],
                           &in->extra_args[k])) {
        free(map);
        return false;
      }
    }
  }
  f->next_value = next;
  free(map);
  return true;
}

static bool timed_pass(ny_nir_func_t *f, bool (*pass)(ny_nir_func_t *),
                       double *out_ms) {
  ny_tick_t t0 = ny_ticks_now();
  bool ok = pass(f);
  if (ok)
    ny_nir_refresh_metadata(f);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0);
  return ok;
}

static const char *pass_names[9] = {
    "const_fold (1)", "peephole",    "copy_prop", "const_fold (2)",
    "cfg_simplify",   "dce",         "cfg_simplify (2)", "compact",
    "total",
};

const char *ny_nir_opt_pass_name(int pass) {
  if (pass < 0 || pass >= 9)
    return "?";
  return pass_names[pass];
}

bool ny_nir_optimize_with_stats(ny_nir_func_t *f, ny_nir_opt_stats_t *stats) {
  if (stats) {
    memset(stats, 0, sizeof(*stats));
    ny_nir_collect_stats(f, &stats->before_insts, &stats->before_values,
                         stats->before_ops, NYIR_OP_COUNT);
  }
  double *t = stats ? stats->pass_time_ms : NULL;
  bool ok =
      timed_pass(f, ny_nir_const_fold, t ? &t[0] : NULL) &&
      timed_pass(f, ny_nir_peephole, t ? &t[1] : NULL) &&
      timed_pass(f, ny_nir_copy_prop, t ? &t[2] : NULL) &&
      timed_pass(f, ny_nir_const_fold, t ? &t[3] : NULL) &&
      timed_pass(f, ny_nir_cfg_simplify, t ? &t[4] : NULL) &&
      timed_pass(f, ny_nir_dce, t ? &t[5] : NULL) &&
      timed_pass(f, ny_nir_cfg_simplify, t ? &t[6] : NULL) &&
      timed_pass(f, ny_nir_compact, t ? &t[7] : NULL);
  if (stats) {
    ny_nir_collect_stats(f, &stats->after_insts, &stats->after_values,
                         stats->after_ops, NYIR_OP_COUNT);
    stats->pass_time_ms[8] = 0;
    for (int i = 0; i < 8; ++i)
      stats->pass_time_ms[8] += stats->pass_time_ms[i];
  }
  if (ok)
    ny_nir_refresh_metadata(f);
  if (verbose_enabled >= 1 && stats && stats->pass_time_ms[8] > 0.001) {
    size_t removed = stats->before_insts - stats->after_insts;
    double pct = stats->before_insts > 0
                     ? 100.0 * (double)removed / stats->before_insts
                     : 0.0;
    fprintf(stderr, "nyir opt: %zu->%zu insts (-%zu, %.1f%%) in %.2fms",
            stats->before_insts, stats->after_insts, removed, pct,
            stats->pass_time_ms[8]);
    if (verbose_enabled >= 2) {
      for (int i = 0; i < 8; ++i)
        fprintf(stderr, " %s=%.2fms", pass_names[i], stats->pass_time_ms[i]);
    }
    fputc('\n', stderr);
  }
  return ok;
}

bool ny_nir_optimize(ny_nir_func_t *f) {
  return ny_nir_optimize_with_stats(f, NULL);
}

static bool timed_pass_verified(ny_nir_func_t *f, bool (*pass)(ny_nir_func_t *),
                                 double *out_ms, FILE *dump, int pass_idx,
                                 bool *ok) {
  ny_tick_t t0 = ny_ticks_now();
  *ok = pass(f);
  if (*ok)
    ny_nir_refresh_metadata(f);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0);
  if (dump && f->len > 0) {
    char name[64];
    snprintf(name, sizeof(name), "<after-%s>", ny_nir_opt_pass_name(pass_idx));
    ny_nir_dump(dump, f, name);
  }
  if (*ok) {
    char vbuf[256] = {0};
    if (!ny_nir_verify(f, vbuf, sizeof(vbuf))) {
      if (dump)
        fprintf(dump, "nyir verify FAILED after %s: %s\n",
                ny_nir_opt_pass_name(pass_idx), vbuf);
      *ok = false;
    }
  }
  return *ok;
}

bool ny_nir_optimize_debug(ny_nir_func_t *f, FILE *dump,
                           ny_nir_opt_stats_t *stats) {
  if (!dump)
    dump = stderr;

  /* Dump before first pass. */
  if (f->len > 0)
    ny_nir_dump(dump, f, "<before-optimize>");

  /* Run with per-pass timing, dump, and verify checkpoints. */
  double *t = stats ? stats->pass_time_ms : NULL;
  bool ok = true;
  ok = ok && timed_pass_verified(f, ny_nir_const_fold, t ? &t[0] : NULL,
                                  dump, 0, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_peephole, t ? &t[1] : NULL,
                                  dump, 1, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_copy_prop, t ? &t[2] : NULL,
                                  dump, 2, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_const_fold, t ? &t[3] : NULL,
                                  dump, 3, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_cfg_simplify, t ? &t[4] : NULL,
                                  dump, 4, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_dce, t ? &t[5] : NULL,
                                  dump, 5, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_cfg_simplify, t ? &t[6] : NULL,
                                  dump, 6, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_compact, t ? &t[7] : NULL,
                                  dump, 7, &ok);
  if (stats) {
    if (!t) {
      /* timing was not collected during passes due to NULL stats; collect now */
      t = stats->pass_time_ms;
      memset(stats, 0, sizeof(*stats));
      ny_nir_collect_stats(f, &stats->after_insts, &stats->after_values,
                           stats->after_ops, NYIR_OP_COUNT);
    }
    stats->pass_time_ms[8] = 0;
    for (int i = 0; i < 8; ++i)
      stats->pass_time_ms[8] += stats->pass_time_ms[i];
  }

  if (ok)
    ny_nir_refresh_metadata(f);

  /* Print pass timings. */
  if (stats && stats->pass_time_ms[8] > 0.001) {
    fprintf(dump, "nyir pass timing:");
    for (int i = 0; i < 9; ++i) {
      if (i < 8 || stats->pass_time_ms[i] > 0.001)
        fprintf(dump, " %s=%.2fms", ny_nir_opt_pass_name(i),
                stats->pass_time_ms[i]);
    }
    fputc('\n', dump);
  }

  return ok;
}
