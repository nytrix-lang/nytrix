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
} ny_i386_nir_ctx_t;

static int ny_i386_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static void ny_i386_nir_compute_frame(ny_i386_nir_ctx_t *c) {
  c->max_local_slot = 0;
  for (size_t i = 0; c->nir && i < c->nir->len; ++i) {
    const ny_nir_inst_t *in = &c->nir->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  int value_slots = c->nir && c->nir->next_value > 0 ? c->nir->next_value : 0;
  c->local_base = value_slots * 4;
  c->frame_bytes = ny_i386_align((value_slots + c->max_local_slot) * 4, 16);
}

static int ny_i386_value_off(int value) {
  return -4 * (value + 2);
}

static int ny_i386_local_off(ny_i386_nir_ctx_t *c, int local) {
  return -4 * ((c->local_base / 4) + local + 2);
}

static bool ny_i386_load_value(ny_i386_nir_ctx_t *c, const char *reg, int value) {
  if (value < 0 || !c || !c->nir || value >= c->nir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "i386 NYIR emit: invalid value v%d", value);
    return false;
  }
  return ny_native_printf(c->w, "\tmovl\t%d(%%ebp), %s\n",
                          ny_i386_value_off(value), reg);
}

static bool ny_i386_store_value(ny_i386_nir_ctx_t *c, int value,
                                const char *reg) {
  if (value < 0 || !c || !c->nir || value >= c->nir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "i386 NYIR emit: invalid destination v%d", value);
    return false;
  }
  return ny_native_printf(c->w, "\tmovl\t%s, %d(%%ebp)\n", reg,
                          ny_i386_value_off(value));
}

static bool ny_i386_load_local(ny_i386_nir_ctx_t *c, const char *reg,
                               int local) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "i386 NYIR emit: invalid local slot %d", local);
    return false;
  }
  return ny_native_printf(c->w, "\tmovl\t%d(%%ebp), %s\n",
                          ny_i386_local_off(c, local), reg);
}

static bool ny_i386_store_local(ny_i386_nir_ctx_t *c, int local,
                                const char *reg) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "i386 NYIR emit: invalid local slot %d", local);
    return false;
  }
  return ny_native_printf(c->w, "\tmovl\t%s, %d(%%ebp)\n", reg,
                          ny_i386_local_off(c, local));
}

static bool ny_i386_mov_imm(ny_i386_nir_ctx_t *c, const char *reg,
                           int64_t value) {
  if (value < INT32_MIN || value > UINT32_MAX) {
    ny_native_set_err(c->err, c->err_len,
                      "i386 NYIR emit: constant out of i32 range");
    return false;
  }
  return ny_native_printf(c->w, "\tmovl\t$0x%08x, %s\n", (uint32_t)value, reg);
}

static const char *ny_i386_setcc(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    return "sete";
  case NY_NIR_CMP_NE:
    return "setne";
  case NY_NIR_CMP_LT:
    return "setl";
  case NY_NIR_CMP_LE:
    return "setle";
  case NY_NIR_CMP_GT:
    return "setg";
  case NY_NIR_CMP_GE:
    return "setge";
  }
  return "sete";
}

static bool ny_i386_emit_binop(ny_i386_nir_ctx_t *c, const ny_nir_inst_t *in,
                              const char *op) {
  return ny_i386_load_value(c, "%eax", in->a) &&
         ny_i386_load_value(c, "%ebx", in->b) &&
         ny_native_printf(c->w, "\t%s\t%%ebx, %%eax\n", op) &&
         ny_i386_store_value(c, in->dst, "%eax");
}

static bool ny_i386_emit_inst(ny_i386_nir_ctx_t *c, const ny_nir_inst_t *in) {
  switch (in->op) {
  case NY_NIR_NOP:
    return true;
  case NY_NIR_CONST_I64:
    return ny_i386_mov_imm(c, "%eax", in->imm) &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NY_NIR_COPY:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NY_NIR_ADD_I64:
    return ny_i386_emit_binop(c, in, "addl");
  case NY_NIR_SUB_I64:
    return ny_i386_emit_binop(c, in, "subl");
  case NY_NIR_MUL_I64:
    return ny_i386_emit_binop(c, in, "imull");
  case NY_NIR_DIV_I64:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_load_value(c, "%ebx", in->b) &&
           ny_native_put(c->w, "\tcltd\n\tidivl\t%ebx\n") &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NY_NIR_MOD_I64:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_load_value(c, "%ebx", in->b) &&
           ny_native_put(c->w, "\tcltd\n\tidivl\t%ebx\n") &&
           ny_i386_store_value(c, in->dst, "%edx");
  case NY_NIR_AND_I64:
    return ny_i386_emit_binop(c, in, "andl");
  case NY_NIR_OR_I64:
    return ny_i386_emit_binop(c, in, "orl");
  case NY_NIR_XOR_I64:
    return ny_i386_emit_binop(c, in, "xorl");
  case NY_NIR_SHL_I64:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_load_value(c, "%ecx", in->b) &&
           ny_native_put(c->w, "\tshll\t%cl, %eax\n") &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NY_NIR_SAR_I64:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_load_value(c, "%ecx", in->b) &&
           ny_native_put(c->w, "\tsarl\t%cl, %eax\n") &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NY_NIR_CMP_I64:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_load_value(c, "%ebx", in->b) &&
           ny_native_put(c->w, "\tcmpl\t%ebx, %eax\n\txorl\t%eax, %eax\n") &&
           ny_native_printf(c->w, "\t%s\t%%al\n", ny_i386_setcc(in->cmp)) &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NY_NIR_LABEL:
    return ny_native_printf(c->w, ".Lny_nir_L%lld:\n", (long long)in->imm);
  case NY_NIR_LOAD_LOCAL:
    return ny_i386_load_local(c, "%eax", (int)in->imm) &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NYIR_ADDR_LOCAL:
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      ny_native_set_err(c->err, c->err_len,
                        "i386 NYIR emit: addr.local invalid slot %" PRId64,
                        in->imm);
      return false;
    }
    return ny_native_printf(c->w, "\tleal\t%d(%%ebp), %%eax\n",
                            ny_i386_local_off(c, (int)in->imm)) &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NY_NIR_STORE_LOCAL:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_store_local(c, (int)in->imm, "%eax");
  case NY_NIR_CALL: {
    int args[NY_NIR_CALL_MAX_ARGS];
    int argc = 0;
    if (!ny_nir_call_args(in, c->nir->next_value, args,
                          NY_NIR_CALL_MAX_ARGS, &argc, c->err, c->err_len))
      return false;
    for (int i = argc - 1; i >= 0; --i) {
      if (!ny_i386_load_value(c, "%eax", args[i]) ||
          !ny_native_put(c->w, "\tpushl\t%eax\n"))
        return false;
    }
    char fn_label[256];
    const char *sym = in->symbol ? in->symbol : "";
    if (in->flags & NY_NIR_INST_F_EXTERN) {
      snprintf(fn_label, sizeof(fn_label), "%s%s", c->target->symbol_prefix,
               sym);
    } else {
      snprintf(fn_label, sizeof(fn_label), "%s%sny_fn_%s",
               c->target->symbol_prefix,
               c->target->symbol_prefix[0] ? "" : "", sym);
    }
    if (!ny_native_printf(c->w, "\tcall\t%s\n", fn_label))
      return false;
    if (argc > 0 &&
        !ny_native_printf(c->w, "\taddl\t$%d, %%esp\n", argc * 4))
      return false;
    return in->dst < 0 || ny_i386_store_value(c, in->dst, "%eax");
  }
  case NY_NIR_RET:
    if (in->a >= 0 && !ny_i386_load_value(c, "%eax", in->a))
      return false;
    return ny_native_printf(c->w, "\tjmp\t%s\n", c->epilogue_label);
  case NY_NIR_BR:
    return ny_native_printf(c->w, "\tjmp\t.Lny_nir_L%lld\n",
                            (long long)in->imm);
  case NY_NIR_BR_IF:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_native_put(c->w, "\ttestl\t%eax, %eax\n") &&
           ny_native_printf(c->w, "\tjne\t.Lny_nir_L%lld\n",
                            (long long)in->imm);
  case NYIR_LOAD_I64:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_native_put(c->w, "\tmovl\t(%eax), %eax\n") &&
           ny_i386_store_value(c, in->dst, "%eax");
  case NYIR_STORE_I64:
    return ny_i386_load_value(c, "%eax", in->a) &&
           ny_i386_load_value(c, "%ebx", in->c) &&
           ny_native_put(c->w, "\tmovl\t%ebx, (%eax)\n");
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
  case NYIR_ADDR_SYMBOL:
  case NYIR_ALLOCA:
  case NYIR_COPY_STRUCT:
  case NYIR_CAPTURE_RET:
  case NYIR_OP_COUNT:
    break;
  }
  ny_native_set_err(c->err, c->err_len, "i386 NYIR emit: unsupported op %s",
                    ny_nir_op_name(in->op));
  return false;
}

bool ny_native_i386_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len) {
  if (!w || !target || !nir)
    return false;
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  ny_i386_nir_ctx_t ctx = {
      .w = w, .target = target, .nir = nir, .err = err, .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label),
           ".Lny_i386_epilogue_%s", name);
  ny_i386_nir_compute_frame(&ctx);

  if (!ny_native_put(w, "\t.text\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.type\t%s%s, @function\n", target->symbol_prefix,
                        name))
    return false;
  if (!ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n", target->symbol_prefix,
                        name, target->symbol_prefix, name))
    return false;
  if (!ny_native_put(w, "\tpushl\t%ebp\n\tmovl\t%esp, %ebp\n\tpushl\t%ebx\n"))
    return false;
  if (ctx.frame_bytes > 0 &&
      !ny_native_printf(w, "\tsubl\t$%d, %%esp\n", ctx.frame_bytes))
    return false;

  if (strcmp(name, "rt_main") != 0) {
    for (int i = 0; i < ctx.max_local_slot; ++i) {
      if (!ny_native_printf(w, "\tmovl\t%d(%%ebp), %%eax\n", 8 + i * 4) ||
          !ny_i386_store_local(&ctx, i, "%eax"))
        return false;
    }
  }

  for (size_t i = 0; i < nir->len; ++i) {
    if (!ny_i386_emit_inst(&ctx, &nir->data[i])) {
      fprintf(stderr, "native NYIR repro (i386 emit failed):\n");
      ny_nir_dump(stderr, nir, name);
      return false;
    }
  }

  if (!ny_native_printf(w, "%s:\n", ctx.epilogue_label))
    return false;
  if (tag_return && !ny_native_put(w, "\tleal\t1(,%eax,2), %eax\n"))
    return false;
  if (!ny_native_put(w, "\tmovl\t-4(%ebp), %ebx\n\tleave\n\tret\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", target->symbol_prefix,
                        name, target->symbol_prefix, name))
    return false;
  return true;
}
