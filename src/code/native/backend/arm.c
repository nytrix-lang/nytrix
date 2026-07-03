#include "code/native/internal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  ny_native_writer_t *w;
  const ny_native_target_info_t *target;
  const ny_nir_func_t *nir;
  int frame_bytes;
  int local_base;
  int max_local_slot;
  char epilogue_label[128];
  char *err;
  size_t err_len;
} ny_arm_nir_ctx_t;

static int ny_arm_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static void ny_arm_nir_compute_frame(ny_arm_nir_ctx_t *c) {
  c->max_local_slot = 0;
  for (size_t i = 0; c->nir && i < c->nir->len; ++i) {
    const ny_nir_inst_t *in = &c->nir->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  int value_slots = c->nir && c->nir->next_value > 0 ? c->nir->next_value : 0;
  c->local_base = value_slots * 4;
  c->frame_bytes = ny_arm_align((value_slots + c->max_local_slot) * 4, 8);
}

static bool ny_arm_load_value(ny_arm_nir_ctx_t *c, const char *reg, int value) {
  if (value < 0 || !c || !c->nir || value >= c->nir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "ARM NYIR emit: invalid value v%d", value);
    return false;
  }
  return ny_native_printf(c->w, "\tldr\t%s, [sp, #%d]\n", reg, value * 4);
}

static bool ny_arm_store_value(ny_arm_nir_ctx_t *c, int value, const char *reg) {
  if (value < 0 || !c || !c->nir || value >= c->nir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "ARM NYIR emit: invalid destination v%d", value);
    return false;
  }
  return ny_native_printf(c->w, "\tstr\t%s, [sp, #%d]\n", reg, value * 4);
}

static bool ny_arm_load_local(ny_arm_nir_ctx_t *c, const char *reg, int local) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "ARM NYIR emit: invalid local slot %d", local);
    return false;
  }
  return ny_native_printf(c->w, "\tldr\t%s, [sp, #%d]\n", reg,
                          c->local_base + local * 4);
}

static bool ny_arm_store_local(ny_arm_nir_ctx_t *c, int local, const char *reg) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "ARM NYIR emit: invalid local slot %d", local);
    return false;
  }
  return ny_native_printf(c->w, "\tstr\t%s, [sp, #%d]\n", reg,
                          c->local_base + local * 4);
}

static bool ny_arm_mov_imm(ny_arm_nir_ctx_t *c, const char *reg, int64_t value) {
  if (value < INT32_MIN || value > INT32_MAX) {
    ny_native_set_err(c->err, c->err_len,
                      "ARM NYIR emit: constant out of i32 range");
    return false;
  }
  return ny_native_printf(c->w, "\tmovw\t%s, #%u\n", reg,
                          (unsigned)((uint32_t)value & 0xffff)) &&
         ny_native_printf(c->w, "\tmovt\t%s, #%u\n", reg,
                          (unsigned)(((uint32_t)value >> 16) & 0xffff));
}

static const char *ny_arm_cmp_cond(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    return "eq";
  case NY_NIR_CMP_NE:
    return "ne";
  case NY_NIR_CMP_LT:
    return "lt";
  case NY_NIR_CMP_LE:
    return "le";
  case NY_NIR_CMP_GT:
    return "gt";
  case NY_NIR_CMP_GE:
    return "ge";
  }
  return "eq";
}

static bool ny_arm_emit_binop(ny_arm_nir_ctx_t *c, const ny_nir_inst_t *in,
                              const char *op) {
  return ny_arm_load_value(c, "r0", in->a) &&
         ny_arm_load_value(c, "r1", in->b) &&
         ny_native_printf(c->w, "\t%s\tr0, r0, r1\n", op) &&
         ny_arm_store_value(c, in->dst, "r0");
}

static bool ny_arm_emit_inst(ny_arm_nir_ctx_t *c, const ny_nir_inst_t *in) {
  switch (in->op) {
  case NY_NIR_NOP:
    return true;
  case NY_NIR_CONST_I64:
    return ny_arm_mov_imm(c, "r0", in->imm) &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_COPY:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_ADD_I64:
    return ny_arm_emit_binop(c, in, "add");
  case NY_NIR_SUB_I64:
    return ny_arm_emit_binop(c, in, "sub");
  case NY_NIR_MUL_I64:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_load_value(c, "r1", in->b) &&
           ny_native_put(c->w, "\tmul\tr0, r0, r1\n") &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_DIV_I64:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_load_value(c, "r1", in->b) &&
           ny_native_put(c->w, "\tsdiv\tr0, r0, r1\n") &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_MOD_I64:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_load_value(c, "r1", in->b) &&
           ny_native_put(c->w,
                         "\tsdiv\tr2, r0, r1\n"
                         "\tmls\tr0, r2, r1, r0\n") &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_AND_I64:
    return ny_arm_emit_binop(c, in, "and");
  case NY_NIR_OR_I64:
    return ny_arm_emit_binop(c, in, "orr");
  case NY_NIR_XOR_I64:
    return ny_arm_emit_binop(c, in, "eor");
  case NY_NIR_SHL_I64:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_load_value(c, "r1", in->b) &&
           ny_native_put(c->w, "\tlsl\tr0, r0, r1\n") &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_SAR_I64:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_load_value(c, "r1", in->b) &&
           ny_native_put(c->w, "\tasr\tr0, r0, r1\n") &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_CMP_I64: {
    const char *cond = ny_arm_cmp_cond(in->cmp);
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_load_value(c, "r1", in->b) &&
           ny_native_put(c->w, "\tcmp\tr0, r1\n\tmov\tr0, #0\n") &&
           ny_native_printf(c->w, "\tmov%s\tr0, #1\n", cond) &&
           ny_arm_store_value(c, in->dst, "r0");
  }
  case NY_NIR_LABEL:
    return ny_native_printf(c->w, ".Lny_nir_L%lld:\n", (long long)in->imm);
  case NY_NIR_LOAD_LOCAL:
    return ny_arm_load_local(c, "r0", (int)in->imm) &&
           ny_arm_store_value(c, in->dst, "r0");
  case NY_NIR_STORE_LOCAL:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_arm_store_local(c, (int)in->imm, "r0");
  case NY_NIR_CALL: {
    if (in->imm > 2) {
      ny_native_set_err(c->err, c->err_len,
                        "ARM NYIR emit: calls support at most 2 NYIR arguments");
      return false;
    }
    if (in->imm > 0 && !ny_arm_load_value(c, "r0", in->a))
      return false;
    if (in->imm > 1 && !ny_arm_load_value(c, "r1", in->b))
      return false;
    if (!ny_native_printf(c->w, "\tbl\t%s%s\n", c->target->symbol_prefix,
                          in->symbol ? in->symbol : ""))
      return false;
    return in->dst < 0 || ny_arm_store_value(c, in->dst, "r0");
  }
  case NY_NIR_RET:
    if (in->a >= 0 && !ny_arm_load_value(c, "r0", in->a))
      return false;
    return ny_native_printf(c->w, "\tb\t%s\n", c->epilogue_label);
  case NY_NIR_BR:
    return ny_native_printf(c->w, "\tb\t.Lny_nir_L%lld\n",
                            (long long)in->imm);
  case NY_NIR_BR_IF:
    return ny_arm_load_value(c, "r0", in->a) &&
           ny_native_put(c->w, "\tcmp\tr0, #0\n") &&
           ny_native_printf(c->w, "\tbne\t.Lny_nir_L%lld\n",
                            (long long)in->imm);
  case NYIR_CONST_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_I64_TO_F64:
  case NYIR_CMP_F64:
  case NYIR_CONST_F32:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_CMP_F32:
  case NYIR_ADDR_LOCAL:
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
  case NYIR_OP_COUNT:
    break;
  }
  ny_native_set_err(c->err, c->err_len, "ARM NYIR emit: unsupported op %s",
                    ny_nir_op_name(in->op));
  return false;
}

bool ny_native_arm_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len) {
  if (!w || !target || !nir)
    return false;
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  ny_arm_nir_ctx_t ctx = {
      .w = w, .target = target, .nir = nir, .err = err, .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label),
           ".Lny_arm_epilogue_%s", name);
  ny_arm_nir_compute_frame(&ctx);

  if (!ny_native_put(w, "\t.syntax unified\n\t.arm\n\t.text\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.type\t%s%s, %%function\n", target->symbol_prefix,
                        name))
    return false;
  if (!ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n", target->symbol_prefix,
                        name, target->symbol_prefix, name))
    return false;
  if (!ny_native_put(w, "\tpush\t{r4, r5, r6, r7, lr}\n"))
    return false;
  if (ctx.frame_bytes > 0 &&
      !ny_native_printf(w, "\tsub\tsp, sp, #%d\n", ctx.frame_bytes))
    return false;

  if (strcmp(name, "rt_main") != 0) {
    int max = ctx.max_local_slot < 4 ? ctx.max_local_slot : 4;
    for (int i = 0; i < max; ++i) {
      if (!ny_arm_store_local(&ctx, i, target->gp_arg_regs[i]))
        return false;
    }
  }

  for (size_t i = 0; i < nir->len; ++i) {
    if (!ny_arm_emit_inst(&ctx, &nir->data[i])) {
      fprintf(stderr, "native NYIR repro (ARM emit failed):\n");
      ny_nir_dump(stderr, nir, name);
      return false;
    }
  }

  if (!ny_native_printf(w, "%s:\n", ctx.epilogue_label))
    return false;
  if (tag_return &&
      !ny_native_put(w, "\tlsl\tr0, r0, #1\n\tadd\tr0, r0, #1\n"))
    return false;
  if (ctx.frame_bytes > 0 &&
      !ny_native_printf(w, "\tadd\tsp, sp, #%d\n", ctx.frame_bytes))
    return false;
  if (!ny_native_put(w, "\tpop\t{r4, r5, r6, r7, pc}\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", target->symbol_prefix,
                        name, target->symbol_prefix, name))
    return false;
  return true;
}
