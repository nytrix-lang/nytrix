#include "code/native/internal.h"

#include <inttypes.h>
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
} ny_a64_nir_ctx_t;

static int ny_a64_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static void ny_a64_compute_frame(ny_a64_nir_ctx_t *c) {
  c->max_local_slot = 0;
  for (size_t i = 0; c->nir && i < c->nir->len; ++i) {
    const ny_nir_inst_t *in = &c->nir->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  int value_slots = c->nir && c->nir->next_value > 0 ? c->nir->next_value : 0;
  c->local_base = value_slots * 8;
  c->frame_bytes = ny_a64_align((value_slots + c->max_local_slot) * 8, 16);
}

static bool ny_a64_mem(ny_a64_nir_ctx_t *c, const char *op, const char *reg,
                       int off) {
  if (off < 0 || off > 32760 || (off & 7) != 0) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 NYIR emit: stack offset %d is out of encodable range",
                      off);
    return false;
  }
  return ny_native_printf(c->w, "\t%s\t%s, [sp, #%d]\n", op, reg, off);
}

static bool ny_a64_load_value(ny_a64_nir_ctx_t *c, const char *reg, int value) {
  if (value < 0 || !c || !c->nir || value >= c->nir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "AArch64 NYIR emit: invalid value v%d", value);
    return false;
  }
  return ny_a64_mem(c, "ldr", reg, value * 8);
}

static bool ny_a64_store_value(ny_a64_nir_ctx_t *c, int value,
                               const char *reg) {
  if (value < 0 || !c || !c->nir || value >= c->nir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "AArch64 NYIR emit: invalid destination v%d", value);
    return false;
  }
  return ny_a64_mem(c, "str", reg, value * 8);
}

static bool ny_a64_load_local(ny_a64_nir_ctx_t *c, const char *reg,
                              int local) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 NYIR emit: invalid local slot %d", local);
    return false;
  }
  return ny_a64_mem(c, "ldr", reg, c->local_base + local * 8);
}

static bool ny_a64_store_local(ny_a64_nir_ctx_t *c, int local,
                               const char *reg) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 NYIR emit: invalid local slot %d", local);
    return false;
  }
  return ny_a64_mem(c, "str", reg, c->local_base + local * 8);
}

static bool ny_a64_mov_imm(ny_a64_nir_ctx_t *c, const char *reg,
                           int64_t value) {
  uint64_t u = (uint64_t)value;
  if (!ny_native_printf(c->w, "\tmovz\t%s, #%u\n", reg,
                        (unsigned)(u & 0xffff)))
    return false;
  for (int shift = 16; shift < 64; shift += 16) {
    unsigned part = (unsigned)((u >> shift) & 0xffff);
    if (part != 0 && !ny_native_printf(c->w, "\tmovk\t%s, #%u, lsl #%d\n",
                                       reg, part, shift))
      return false;
  }
  return true;
}

static const char *ny_a64_cond(ny_nir_cmp_t cmp) {
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

static bool ny_a64_emit_binop(ny_a64_nir_ctx_t *c, const ny_nir_inst_t *in,
                              const char *op) {
  return ny_a64_load_value(c, "x0", in->a) &&
         ny_a64_load_value(c, "x1", in->b) &&
         ny_native_printf(c->w, "\t%s\tx0, x0, x1\n", op) &&
         ny_a64_store_value(c, in->dst, "x0");
}

static bool ny_a64_emit_inst(ny_a64_nir_ctx_t *c, const ny_nir_inst_t *in) {
  switch (in->op) {
  case NY_NIR_NOP:
    return true;
  case NY_NIR_CONST_I64:
    return ny_a64_mov_imm(c, "x0", in->imm) &&
           ny_a64_store_value(c, in->dst, "x0");
  case NY_NIR_COPY:
    return ny_a64_load_value(c, "x0", in->a) &&
           ny_a64_store_value(c, in->dst, "x0");
  case NY_NIR_ADD_I64:
    return ny_a64_emit_binop(c, in, "add");
  case NY_NIR_SUB_I64:
    return ny_a64_emit_binop(c, in, "sub");
  case NY_NIR_MUL_I64:
    return ny_a64_emit_binop(c, in, "mul");
  case NY_NIR_AND_I64:
    return ny_a64_emit_binop(c, in, "and");
  case NY_NIR_OR_I64:
    return ny_a64_emit_binop(c, in, "orr");
  case NY_NIR_XOR_I64:
    return ny_a64_emit_binop(c, in, "eor");
  case NY_NIR_SHL_I64:
    return ny_a64_emit_binop(c, in, "lsl");
  case NY_NIR_SAR_I64:
    return ny_a64_emit_binop(c, in, "asr");
  case NY_NIR_DIV_I64:
    return ny_a64_load_value(c, "x0", in->a) &&
           ny_a64_load_value(c, "x1", in->b) &&
           ny_native_put(c->w, "\tsdiv\tx0, x0, x1\n") &&
           ny_a64_store_value(c, in->dst, "x0");
  case NY_NIR_MOD_I64:
    return ny_a64_load_value(c, "x0", in->a) &&
           ny_a64_load_value(c, "x1", in->b) &&
           ny_native_put(c->w,
                         "\tsdiv\tx2, x0, x1\n"
                         "\tmsub\tx0, x2, x1, x0\n") &&
           ny_a64_store_value(c, in->dst, "x0");
  case NY_NIR_CMP_I64:
    return ny_a64_load_value(c, "x0", in->a) &&
           ny_a64_load_value(c, "x1", in->b) &&
           ny_native_put(c->w, "\tcmp\tx0, x1\n") &&
           ny_native_printf(c->w, "\tcset\tx0, %s\n", ny_a64_cond(in->cmp)) &&
           ny_a64_store_value(c, in->dst, "x0");
  case NY_NIR_LABEL:
    return ny_native_printf(c->w, ".Lny_nir_L%" PRId64 ":\n", in->imm);
  case NY_NIR_LOAD_LOCAL:
    return ny_a64_load_local(c, "x0", (int)in->imm) &&
           ny_a64_store_value(c, in->dst, "x0");
  case NY_NIR_STORE_LOCAL:
    return ny_a64_load_value(c, "x0", in->a) &&
           ny_a64_store_local(c, (int)in->imm, "x0");
  case NY_NIR_CALL:
    if (in->imm > 2) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 NYIR emit: calls support at most 2 arguments");
      return false;
    }
    if (in->imm > 0 && !ny_a64_load_value(c, "x0", in->a))
      return false;
    if (in->imm > 1 && !ny_a64_load_value(c, "x1", in->b))
      return false;
    const char *a64_sym = in->symbol ? in->symbol : "";
    int a64_is_ext = (in->flags & NY_NIR_INST_F_EXTERN) ? 1 : 0;
    if (!ny_native_printf(c->w, "\tbl\t%s%s%s\n", c->target->symbol_prefix,
                          a64_is_ext ? "" : "ny_fn_", a64_sym))
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, "x0");
  case NY_NIR_RET:
    if (in->a >= 0 && !ny_a64_load_value(c, "x0", in->a))
      return false;
    return ny_native_printf(c->w, "\tb\t%s\n", c->epilogue_label);
  case NY_NIR_BR:
    return ny_native_printf(c->w, "\tb\t.Lny_nir_L%" PRId64 "\n", in->imm);
  case NY_NIR_BR_IF:
    return ny_a64_load_value(c, "x0", in->a) &&
           ny_native_put(c->w, "\tcmp\tx0, #0\n") &&
           ny_native_printf(c->w, "\tb.ne\t.Lny_nir_L%" PRId64 "\n", in->imm);
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
  ny_native_set_err(c->err, c->err_len, "AArch64 NYIR emit: unsupported op %s",
                    ny_nir_op_name(in->op));
  return false;
}

bool ny_native_aarch64_emit_nir(ny_native_writer_t *w,
                                const ny_native_target_info_t *target,
                                const ny_nir_func_t *nir,
                                const char *func_name, bool tag_return,
                                char *err, size_t err_len) {
  if (!w || !target || !nir)
    return false;
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  ny_a64_nir_ctx_t ctx = {
      .w = w, .target = target, .nir = nir, .err = err, .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label),
           ".Lny_aarch64_epilogue_%s", name);
  ny_a64_compute_frame(&ctx);

  if (!ny_native_put(w, "\t.text\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.type\t%s%s, %%function\n", target->symbol_prefix,
                        name))
    return false;
  if (!ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n", target->symbol_prefix,
                        name, target->symbol_prefix, name))
    return false;
  if (!ny_native_put(w, "\tstp\tx29, x30, [sp, #-16]!\n\tmov\tx29, sp\n"))
    return false;
  if (ctx.frame_bytes > 0 &&
      !ny_native_printf(w, "\tsub\tsp, sp, #%d\n", ctx.frame_bytes))
    return false;

  if (strcmp(name, "rt_main") != 0) {
    int max = ctx.max_local_slot < 2 ? ctx.max_local_slot : 2;
    for (int i = 0; i < max; ++i) {
      if (!ny_a64_store_local(&ctx, i, target->gp_arg_regs[i]))
        return false;
    }
  }

  for (size_t i = 0; i < nir->len; ++i) {
    if (!ny_a64_emit_inst(&ctx, &nir->data[i])) {
      fprintf(stderr, "native NYIR repro (AArch64 emit failed):\n");
      ny_nir_dump(stderr, nir, name);
      return false;
    }
  }

  if (!ny_native_printf(w, "%s:\n", ctx.epilogue_label))
    return false;
  if (tag_return && !ny_native_put(w, "\tlsl\tx0, x0, #1\n\tadd\tx0, x0, #1\n"))
    return false;
  if (ctx.frame_bytes > 0 &&
      !ny_native_printf(w, "\tadd\tsp, sp, #%d\n", ctx.frame_bytes))
    return false;
  if (!ny_native_put(w, "\tldp\tx29, x30, [sp], #16\n\tret\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", target->symbol_prefix,
                        name, target->symbol_prefix, name))
    return false;
  return true;
}
