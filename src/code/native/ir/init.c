#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t ny_nir_f64_to_bits(double v) {
  int64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

double ny_nir_bits_to_f64(int64_t bits) {
  double v = 0;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

int64_t ny_nir_f32_to_bits(float v) {
  int32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)(uint32_t)bits;
}

float ny_nir_bits_to_f32(int64_t bits) {
  int32_t b32 = (int32_t)(uint32_t)bits;
  float v = 0;
  memcpy(&v, &b32, sizeof(v));
  return v;
}

void ny_nir_func_free(ny_nir_func_t *f) {
  if (!f)
    return;
  for (size_t i = 0; i < f->owned_symbols_len; ++i)
    free(f->owned_symbols[i]);
  free(f->owned_symbols);
  for (size_t i = 0; i < f->len; ++i) {
    free(f->data[i].extra_args);
    free(f->data[i].arg_sizes);
  }
  free(f->data);
  memset(f, 0, sizeof(*f));
}

void ny_nir_inst_discard(ny_nir_inst_t *in) {
  if (!in)
    return;
  free(in->extra_args);
  free(in->arg_sizes);
  *in = (ny_nir_inst_t){.op = NY_NIR_NOP,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1};
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
  case NYIR_ADDR_SYMBOL:
    return "addr.symbol";
  case NYIR_ALLOCA:
    return "alloca";
  case NYIR_COPY_STRUCT:
    return "copy.struct";
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
  case NYIR_ADDR_SYMBOL:
  case NYIR_ALLOCA:
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
  case NYIR_COPY_STRUCT:
    inst->dst = -1;
    inst->d = -1;
    inst->e = -1;
    inst->f = -1;
    if (inst->op == NYIR_STORE_I64)
      inst->b = -1;
    else
      inst->c = -1;
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

bool ny_nir_err(char *err, size_t err_len, const char *fmt, ...) {
  if (err && err_len > 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
  }
  return false;
}

bool ny_nir_inst_err(char *err, size_t err_len, const ny_nir_inst_t *in,
                     size_t index, const char *reason) {
  return ny_nir_err(err, err_len,
                 "native NYIR verify: inst %zu opcode=%s dst=v%d a=v%d b=v%d "
                 "imm=%" PRId64 ": %s",
                 index, in ? ny_nir_op_name(in->op) : "<null>",
                 in ? in->dst : -1, in ? in->a : -1, in ? in->b : -1,
                 in ? in->imm : 0, reason ? reason : "invalid instruction");
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
    } else if (in->op == NYIR_ADDR_SYMBOL) {
      fprintf(out, " %s", in->symbol ? in->symbol : "<null>");
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

void ny_nir_collect_stats(const ny_nir_func_t *f, size_t *insts,
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
