/*
 * Native backend stack manager: explicit stack-frame layout tracking
 * for register spills, local variables, and ABI-mandated slots.
 */
#include "code/native/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Shared stack-machine text NYIR emitter for secondary ISAs (ARM / AArch64 /
 * i386 / RISC-V). One implementation, per-ISA string/layout profiles — replaces
 * four near-copy backends.
 */

typedef enum {
  NY_SM_ARM = 0,
  NY_SM_A64,
  NY_SM_I386,
  NY_SM_RISCV,
} ny_sm_kind_t;

typedef struct {
  ny_native_writer_t *w;
  const ny_native_target_info_t *target;
  const nyir_func_t *nyir;
  ny_sm_kind_t kind;
  const char *pretty;
  const char *ret; /* return register name in asm */
  const char *t0;
  const char *t1;
  int word;
  int frame_align;
  int frame_bytes;
  int local_base;
  int max_local_slot;
  bool is_leaf;
  char epilogue[160];
  char *err;
  size_t err_len;
} ny_sm_ctx_t;

static int sm_align(int n, int a) { return (n + a - 1) & ~(a - 1); }

static void sm_frame(ny_sm_ctx_t *c) {
  c->max_local_slot = 0;
  c->is_leaf = true;
  for (size_t i = 0; c->nyir && i < c->nyir->len; ++i) {
    const nyir_inst_t *in = &c->nyir->data[i];
    if (in->op == NYIR_CALL)
      c->is_leaf = false;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  int vs = c->nyir && c->nyir->next_value > 0 ? c->nyir->next_value : 0;
  c->local_base = vs * c->word;
  int spill = (vs + c->max_local_slot) * c->word;
  if (c->kind == NY_SM_RISCV)
    c->frame_bytes = sm_align(spill + 16, c->frame_align);
  else
    c->frame_bytes = sm_align(spill, c->frame_align);
}

static bool sm_err_val(ny_sm_ctx_t *c, int v, const char *what) {
  if (v < 0 || !c || !c->nyir || v >= c->nyir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "%s NYIR emit: invalid %s v%d",
                      c ? c->pretty : "stack", what ? what : "value", v);
    return false;
  }
  return true;
}

static int sm_voff(ny_sm_ctx_t *c, int v) {
  if (c->kind == NY_SM_I386)
    return -c->word * (v + 2);
  return v * c->word;
}
static int sm_loff(ny_sm_ctx_t *c, int local) {
  if (c->kind == NY_SM_I386)
    return -c->word * ((c->local_base / c->word) + local + 2);
  return c->local_base + local * c->word;
}

static bool sm_ld(ny_sm_ctx_t *c, const char *reg, int off) {
  switch (c->kind) {
  case NY_SM_ARM:
    return ny_native_printf(c->w, "\tldr\t%s, [sp, #%d]\n", reg, off);
  case NY_SM_A64:
    if (off < 0 || off > 32760 || (off & 7)) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 NYIR emit: stack offset %d out of range", off);
      return false;
    }
    return ny_native_printf(c->w, "\tldr\t%s, [sp, #%d]\n", reg, off);
  case NY_SM_I386:
    return ny_native_printf(c->w, "\tmovl\t%d(%%ebp), %s\n", off, reg);
  case NY_SM_RISCV:
    if (off < 0 || off > 2047) {
      ny_native_set_err(c->err, c->err_len,
                        "RISC-V NYIR emit: frame offset %d out of range", off);
      return false;
    }
    return ny_native_printf(c->w, "\tld\t%s, %d(sp)\n", reg, off);
  }
  return false;
}

static bool sm_st(ny_sm_ctx_t *c, int off, const char *reg) {
  switch (c->kind) {
  case NY_SM_ARM:
    return ny_native_printf(c->w, "\tstr\t%s, [sp, #%d]\n", reg, off);
  case NY_SM_A64:
    if (off < 0 || off > 32760 || (off & 7)) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 NYIR emit: stack offset %d out of range", off);
      return false;
    }
    return ny_native_printf(c->w, "\tstr\t%s, [sp, #%d]\n", reg, off);
  case NY_SM_I386:
    return ny_native_printf(c->w, "\tmovl\t%s, %d(%%ebp)\n", reg, off);
  case NY_SM_RISCV:
    if (off < 0 || off > 2047) {
      ny_native_set_err(c->err, c->err_len,
                        "RISC-V NYIR emit: frame offset %d out of range", off);
      return false;
    }
    return ny_native_printf(c->w, "\tsd\t%s, %d(sp)\n", reg, off);
  }
  return false;
}

static bool sm_ldv(ny_sm_ctx_t *c, const char *reg, int v) {
  return sm_err_val(c, v, "value") && sm_ld(c, reg, sm_voff(c, v));
}
static bool sm_stv(ny_sm_ctx_t *c, int v, const char *reg) {
  return sm_err_val(c, v, "destination") && sm_st(c, sm_voff(c, v), reg);
}
static bool sm_ldl(ny_sm_ctx_t *c, const char *reg, int local) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len, "%s NYIR emit: bad local %d",
                      c->pretty, local);
    return false;
  }
  return sm_ld(c, reg, sm_loff(c, local));
}
static bool sm_stl(ny_sm_ctx_t *c, int local, const char *reg) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len, "%s NYIR emit: bad local %d",
                      c->pretty, local);
    return false;
  }
  return sm_st(c, sm_loff(c, local), reg);
}

static bool sm_movi(ny_sm_ctx_t *c, const char *reg, int64_t v) {
  switch (c->kind) {
  case NY_SM_ARM:
    if (v < INT32_MIN || v > INT32_MAX) {
      ny_native_set_err(c->err, c->err_len, "ARM NYIR emit: imm out of i32");
      return false;
    }
    return ny_native_printf(c->w, "\tmovw\t%s, #%u\n\tmovt\t%s, #%u\n", reg,
                            (unsigned)((uint32_t)v & 0xffff), reg,
                            (unsigned)(((uint32_t)v >> 16) & 0xffff));
  case NY_SM_A64:
    return ny_native_printf(c->w, "\tmov\t%s, #%" PRId64 "\n", reg, v);
  case NY_SM_I386:
    return ny_native_printf(c->w, "\tmovl\t$%" PRId64 ", %s\n", v, reg);
  case NY_SM_RISCV:
    return ny_native_printf(c->w, "\tli\t%s, %" PRId64 "\n", reg, v);
  }
  return false;
}

static bool sm_binop3(ny_sm_ctx_t *c, const char *op) {
  /*
   * t0 = t0 op t1
   */
  switch (c->kind) {
  case NY_SM_ARM:
  case NY_SM_A64:
    return ny_native_printf(c->w, "\t%s\t%s, %s, %s\n", op, c->t0, c->t0, c->t1);
  case NY_SM_I386:
    /*
     * op %t1, %t0  (AT&T)
     */
    return ny_native_printf(c->w, "\t%sl\t%s, %s\n", op, c->t1, c->t0);
  case NY_SM_RISCV:
    return ny_native_printf(c->w, "\t%s\t%s, %s, %s\n", op, c->t0, c->t0, c->t1);
  }
  return false;
}

static bool sm_emit_binop(ny_sm_ctx_t *c, const nyir_inst_t *in,
                          const char *arm, const char *a64, const char *i386,
                          const char *rv) {
  if (!sm_ldv(c, c->t0, in->a) || !sm_ldv(c, c->t1, in->b))
    return false;
  const char *op = c->kind == NY_SM_ARM     ? arm
                   : c->kind == NY_SM_A64   ? a64
                   : c->kind == NY_SM_I386  ? i386
                                            : rv;
  return sm_binop3(c, op) && sm_stv(c, in->dst, c->t0);
}

static const char *sm_cond(nyir_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ: return "eq";
  case NYIR_CMP_NE: return "ne";
  case NYIR_CMP_LT: return "lt";
  case NYIR_CMP_LE: return "le";
  case NYIR_CMP_GT: return "gt";
  case NYIR_CMP_GE: return "ge";
  }
  return "eq";
}

static bool sm_cmp(ny_sm_ctx_t *c, const nyir_inst_t *in) {
  if (!sm_ldv(c, c->t0, in->a) || !sm_ldv(c, c->t1, in->b))
    return false;
  const char *cond = sm_cond(in->cmp);
  switch (c->kind) {
  case NY_SM_ARM:
    return ny_native_put(c->w, "\tcmp\tr0, r1\n\tmov\tr0, #0\n") &&
           ny_native_printf(c->w, "\tmov%s\tr0, #1\n", cond) &&
           sm_stv(c, in->dst, c->t0);
  case NY_SM_A64:
    return ny_native_put(c->w, "\tcmp\tx0, x1\n\tmov\tx0, #0\n") &&
           ny_native_printf(c->w, "\tcset\tx0, %s\n", cond) &&
           sm_stv(c, in->dst, c->t0);
  case NY_SM_I386: {
    const char *set = in->cmp == NYIR_CMP_EQ   ? "e"
                      : in->cmp == NYIR_CMP_NE ? "ne"
                      : in->cmp == NYIR_CMP_LT ? "l"
                      : in->cmp == NYIR_CMP_LE ? "le"
                      : in->cmp == NYIR_CMP_GT ? "g"
                                                   : "ge";
    return ny_native_put(c->w, "\tcmpl\t%ecx, %eax\n\tmovl\t$0, %eax\n") &&
           ny_native_printf(c->w, "\tset%s\t%%al\n\tmovzbl\t%%al, %%eax\n",
                            set) &&
           sm_stv(c, in->dst, c->t0);
  }
  case NY_SM_RISCV: {
    /*
     * slt-based materialize
     */
    if (in->cmp == NYIR_CMP_LT)
      return ny_native_put(c->w, "\tslt\ta0, a0, a1\n") &&
             sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_GT)
      return ny_native_put(c->w, "\tslt\ta0, a1, a0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_EQ)
      return ny_native_put(c->w,
                           "\txor\ta0, a0, a1\n\tseqz\ta0, a0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_NE)
      return ny_native_put(c->w,
                           "\txor\ta0, a0, a1\n\tsnez\ta0, a0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_LE)
      return ny_native_put(c->w,
                           "\tslt\ta0, a1, a0\n\txori\ta0, a0, 1\n") &&
             sm_stv(c, in->dst, c->t0);
    return ny_native_put(c->w,
                         "\tslt\ta0, a0, a1\n\txori\ta0, a0, 1\n") &&
           sm_stv(c, in->dst, c->t0);
  }
  }
  return false;
}

static bool sm_call(ny_sm_ctx_t *c, const nyir_inst_t *in) {
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!nyir_call_args(in, c->nyir->next_value, args, NYIR_CALL_MAX_ARGS,
                        &argc, c->err, c->err_len))
    return false;
  if ((size_t)argc > c->target->gp_arg_reg_count) {
    ny_native_set_err(c->err, c->err_len,
                      "%s NYIR emit: %d args exceed %zu regs", c->pretty, argc,
                      c->target->gp_arg_reg_count);
    return false;
  }
  if (c->kind == NY_SM_I386) {
    /*
     * push right-to-left, call, pop
     */
    for (int i = argc - 1; i >= 0; --i) {
      if (!sm_ldv(c, c->t0, args[i]) ||
          !ny_native_put(c->w, "\tpushl\t%eax\n"))
        return false;
    }
  } else {
    for (int i = 0; i < argc; ++i)
      if (!sm_ldv(c, c->target->gp_arg_regs[i], args[i]))
        return false;
  }
  const char *sym = in->symbol ? in->symbol : "";
  bool ext = (in->flags & NYIR_INST_F_EXTERN) != 0;
  const char *pfx = c->target->symbol_prefix ? c->target->symbol_prefix : "";
  const char *fn = ext ? "" : "ny_fn_";
  switch (c->kind) {
  case NY_SM_ARM:
  case NY_SM_A64:
    if (!ny_native_printf(c->w, "\tbl\t%s%s%s\n", pfx, fn, sym))
      return false;
    break;
  case NY_SM_I386:
    if (!ny_native_printf(c->w, "\tcall\t%s%s%s\n", pfx, fn, sym))
      return false;
    if (argc && !ny_native_printf(c->w, "\taddl\t$%d, %%esp\n", argc * 4))
      return false;
    break;
  case NY_SM_RISCV:
    if (!ny_native_printf(c->w, "\tcall\t%s%s%s\n", pfx, fn, sym))
      return false;
    break;
  }
  return in->dst < 0 || sm_stv(c, in->dst, c->ret);
}

static bool sm_inst(ny_sm_ctx_t *c, const nyir_inst_t *in) {
  switch (in->op) {
  case NYIR_NOP:
    return true;
  case NYIR_CONST_I64:
    return sm_movi(c, c->t0, in->imm) && sm_stv(c, in->dst, c->t0);
  case NYIR_COPY:
    return sm_ldv(c, c->t0, in->a) && sm_stv(c, in->dst, c->t0);
  case NYIR_ADD_I64:
    return sm_emit_binop(c, in, "add", "add", "add", "add");
  case NYIR_SUB_I64:
    return sm_emit_binop(c, in, "sub", "sub", "sub", "sub");
  case NYIR_MUL_I64:
    if (c->kind == NY_SM_ARM)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tmul\tr0, r0, r1\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\timull\t%ecx, %eax\n") &&
             sm_stv(c, in->dst, c->t0);
    return sm_emit_binop(c, in, "mul", "mul", "imul", "mul");
  case NYIR_DIV_I64:
    if (c->kind == NY_SM_ARM)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tsdiv\tr0, r0, r1\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tcltd\n\tidivl\t%ecx\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_RISCV)
      return sm_emit_binop(c, in, "sdiv", "sdiv", "idiv", "div");
    return sm_emit_binop(c, in, "sdiv", "sdiv", "idiv", "div");
  case NYIR_MOD_I64:
    if (c->kind == NY_SM_ARM)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w,
                           "\tsdiv\tr2, r0, r1\n\tmls\tr0, r2, r1, r0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tcltd\n\tidivl\t%ecx\n\tmovl\t%edx, %eax\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_A64)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w,
                           "\tsdiv\tx2, x0, x1\n\tmsub\tx0, x2, x1, x0\n") &&
             sm_stv(c, in->dst, c->t0);
    return sm_emit_binop(c, in, "sdiv", "sdiv", "idiv", "rem");
  case NYIR_AND_I64:
    return sm_emit_binop(c, in, "and", "and", "and", "and");
  case NYIR_OR_I64:
    return sm_emit_binop(c, in, "orr", "orr", "or", "or");
  case NYIR_XOR_I64:
    return sm_emit_binop(c, in, "eor", "eor", "xor", "xor");
  case NYIR_SHL_I64:
    if (c->kind == NY_SM_ARM)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tlsl\tr0, r0, r1\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tmovl\t%ecx, %ecx\n\tshll\t%cl, %eax\n") &&
             sm_stv(c, in->dst, c->t0);
    return sm_emit_binop(c, in, "lsl", "lsl", "shl", "sll");
  case NYIR_SAR_I64:
    if (c->kind == NY_SM_ARM)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tasr\tr0, r0, r1\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             ny_native_put(c->w, "\tsarl\t%cl, %eax\n") &&
             sm_stv(c, in->dst, c->t0);
    return sm_emit_binop(c, in, "asr", "asr", "sar", "sra");
  case NYIR_CMP_I64:
    return sm_cmp(c, in);
  case NYIR_LABEL:
    return ny_native_printf(c->w, ".Lnyir_L%" PRId64 ":\n", in->imm);
  case NYIR_LOAD_LOCAL:
    return sm_ldl(c, c->t0, (int)in->imm) && sm_stv(c, in->dst, c->t0);
  case NYIR_STORE_LOCAL:
    return sm_ldv(c, c->t0, in->a) && sm_stl(c, (int)in->imm, c->t0);
  case NYIR_ADDR_LOCAL: {
    int off = sm_loff(c, (int)in->imm);
    if (in->imm < 0 || in->imm >= c->max_local_slot) {
      ny_native_set_err(c->err, c->err_len, "%s NYIR emit: bad addr.local",
                        c->pretty);
      return false;
    }
    switch (c->kind) {
    case NY_SM_ARM:
      return ny_native_printf(c->w, "\tadd\tr0, sp, #%d\n", off) &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_A64:
      return ny_native_printf(c->w, "\tadd\tx0, sp, #%d\n", off) &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_I386:
      return ny_native_printf(c->w, "\tleal\t%d(%%ebp), %%eax\n", off) &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_RISCV:
      return ny_native_printf(c->w, "\taddi\ta0, sp, %d\n", off) &&
             sm_stv(c, in->dst, c->t0);
    }
    return false;
  }
  case NYIR_LOAD_I64:
    if (!sm_ldv(c, c->t0, in->a))
      return false;
    switch (c->kind) {
    case NY_SM_ARM:
      return ny_native_put(c->w, "\tldr\tr0, [r0]\n") && sm_stv(c, in->dst, c->t0);
    case NY_SM_A64:
      return ny_native_put(c->w, "\tldr\tx0, [x0]\n") && sm_stv(c, in->dst, c->t0);
    case NY_SM_I386:
      return ny_native_put(c->w, "\tmovl\t(%eax), %eax\n") &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_RISCV:
      return ny_native_put(c->w, "\tld\ta0, 0(a0)\n") && sm_stv(c, in->dst, c->t0);
    }
    return false;
  case NYIR_STORE_I64:
    if (!sm_ldv(c, c->t0, in->a) || !sm_ldv(c, c->t1, in->c))
      return false;
    switch (c->kind) {
    case NY_SM_ARM:
      return ny_native_put(c->w, "\tstr\tr1, [r0]\n");
    case NY_SM_A64:
      return ny_native_put(c->w, "\tstr\tx1, [x0]\n");
    case NY_SM_I386:
      return ny_native_put(c->w, "\tmovl\t%ecx, (%eax)\n");
    case NY_SM_RISCV:
      return ny_native_put(c->w, "\tsd\ta1, 0(a0)\n");
    }
    return false;
  case NYIR_CALL:
    return sm_call(c, in);
  case NYIR_RET:
    if (in->a >= 0 && !sm_ldv(c, c->ret, in->a))
      return false;
    switch (c->kind) {
    case NY_SM_ARM:
    case NY_SM_A64:
      return ny_native_printf(c->w, "\tb\t%s\n", c->epilogue);
    case NY_SM_I386:
      return ny_native_printf(c->w, "\tjmp\t%s\n", c->epilogue);
    case NY_SM_RISCV:
      return ny_native_printf(c->w, "\tj\t%s\n", c->epilogue);
    }
    return false;
  case NYIR_BR:
    switch (c->kind) {
    case NY_SM_ARM:
    case NY_SM_A64:
      return ny_native_printf(c->w, "\tb\t.Lnyir_L%" PRId64 "\n", in->imm);
    case NY_SM_I386:
      return ny_native_printf(c->w, "\tjmp\t.Lnyir_L%" PRId64 "\n", in->imm);
    case NY_SM_RISCV:
      return ny_native_printf(c->w, "\tj\t.Lnyir_L%" PRId64 "\n", in->imm);
    }
    return false;
  case NYIR_BR_IF:
    if (!sm_ldv(c, c->t0, in->a))
      return false;
    switch (c->kind) {
    case NY_SM_ARM:
      return ny_native_put(c->w, "\tcmp\tr0, #0\n") &&
             ny_native_printf(c->w, "\tbne\t.Lnyir_L%" PRId64 "\n", in->imm);
    case NY_SM_A64:
      return ny_native_printf(c->w, "\tcbnz\tx0, .Lnyir_L%" PRId64 "\n",
                              in->imm);
    case NY_SM_I386:
      return ny_native_put(c->w, "\ttestl\t%eax, %eax\n") &&
             ny_native_printf(c->w, "\tjnz\t.Lnyir_L%" PRId64 "\n", in->imm);
    case NY_SM_RISCV:
      return ny_native_printf(c->w, "\tbnez\ta0, .Lnyir_L%" PRId64 "\n",
                              in->imm);
    }
    return false;
  default:
    break;
  }
  ny_native_set_err(c->err, c->err_len, "%s NYIR emit: unsupported op %s",
                    c->pretty, nyir_op_name(in->op));
  return false;
}

static bool sm_emit(ny_native_writer_t *w, const ny_native_target_info_t *target,
                    const nyir_func_t *nyir, const char *func_name,
                    bool tag_return, char *err, size_t err_len, ny_sm_kind_t kind,
                    const char *pretty, const char *ret, const char *t0,
                    const char *t1, int word, int frame_align) {
  if (!w || !target || !nyir)
    return false;
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  ny_sm_ctx_t c = {.w = w,
                   .target = target,
                   .nyir = nyir,
                   .kind = kind,
                   .pretty = pretty,
                   .ret = ret,
                   .t0 = t0,
                   .t1 = t1,
                   .word = word,
                   .frame_align = frame_align,
                   .err = err,
                   .err_len = err_len};
  snprintf(c.epilogue, sizeof(c.epilogue), ".Lny_%s_epilogue_%s",
           target->target_name ? target->target_name : "sm", name);
  sm_frame(&c);

  if (c.kind == NY_SM_ARM && !ny_native_put(w, "\t.syntax unified\n\t.arm\n"))
    return false;
  if (!ny_native_put(w, "\t.text\n"))
    return false;
  const char *pfx = target->symbol_prefix ? target->symbol_prefix : "";
  if (strcmp(target->object_format, "macho") != 0) {
    const char *ty = (c.kind == NY_SM_ARM || c.kind == NY_SM_A64) ? "%function"
                                                                  : "@function";
    if (!ny_native_printf(w, "\t.type\t%s%s, %s\n", pfx, name, ty))
      return false;
  }
  if (!ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n", pfx, name, pfx, name))
    return false;

  /*
   * prologue
   */
  switch (c.kind) {
  case NY_SM_ARM:
    if (!c.is_leaf && !ny_native_put(w, "\tpush\t{r4, r5, r6, r7, lr}\n"))
      return false;
    if (c.frame_bytes > 0 &&
        !ny_native_printf(w, "\tsub\tsp, sp, #%d\n", c.frame_bytes))
      return false;
    break;
  case NY_SM_A64:
    if (!c.is_leaf &&
        !ny_native_put(w, "\tstp\tx29, x30, [sp, #-16]!\n\tmov\tx29, sp\n"))
      return false;
    if (c.frame_bytes > 0 &&
        !ny_native_printf(w, "\tsub\tsp, sp, #%d\n", c.frame_bytes))
      return false;
    break;
  case NY_SM_I386:
    if (!ny_native_put(w, "\tpushl\t%ebp\n\tmovl\t%esp, %ebp\n"))
      return false;
    if (c.frame_bytes > 0 &&
        !ny_native_printf(w, "\tsubl\t$%d, %%esp\n", c.frame_bytes))
      return false;
    break;
  case NY_SM_RISCV:
    if (!ny_native_printf(w, "\taddi\tsp, sp, -%d\n", c.frame_bytes))
      return false;
    if (!c.is_leaf &&
        !ny_native_printf(w, "\tsd\tra, %d(sp)\n\tsd\ts0, %d(sp)\n\taddi\ts0, sp, %d\n",
                          c.frame_bytes - 8, c.frame_bytes - 16, c.frame_bytes))
      return false;
    break;
  }

  if (strcmp(name, "rt_main") != 0) {
    if (c.kind == NY_SM_I386) {
      for (int i = 0; i < c.max_local_slot; ++i)
        if (!ny_native_printf(w, "\tmovl\t%d(%%ebp), %%eax\n", 8 + i * 4) ||
            !sm_stl(&c, i, c.t0))
          return false;
    } else {
      int max = c.max_local_slot < (int)target->gp_arg_reg_count
                    ? c.max_local_slot
                    : (int)target->gp_arg_reg_count;
      for (int i = 0; i < max; ++i)
        if (!sm_stl(&c, i, target->gp_arg_regs[i]))
          return false;
    }
  }

  for (size_t i = 0; i < nyir->len; ++i) {
    if (!sm_inst(&c, &nyir->data[i])) {
      fprintf(stderr, "native NYIR repro (%s emit failed):\n", pretty);
      nyir_dump(stderr, nyir, name);
      return false;
    }
  }

  if (!ny_native_printf(w, "%s:\n", c.epilogue))
    return false;
  if (tag_return) {
    switch (c.kind) {
    case NY_SM_ARM:
      if (!ny_native_put(w, "\tlsl\tr0, r0, #1\n\tadd\tr0, r0, #1\n"))
        return false;
      break;
    case NY_SM_A64:
      if (!ny_native_put(w, "\tlsl\tx0, x0, #1\n\tadd\tx0, x0, #1\n"))
        return false;
      break;
    case NY_SM_I386:
      if (!ny_native_put(w, "\tleal\t1(,%eax,2), %eax\n"))
        return false;
      break;
    case NY_SM_RISCV:
      if (!ny_native_put(w, "\tslli\ta0, a0, 1\n\taddi\ta0, a0, 1\n"))
        return false;
      break;
    }
  }
  switch (c.kind) {
  case NY_SM_ARM:
    if (c.frame_bytes > 0 &&
        !ny_native_printf(w, "\tadd\tsp, sp, #%d\n", c.frame_bytes))
      return false;
    if (!ny_native_put(w, c.is_leaf ? "\tpop\t{pc}\n"
                                    : "\tpop\t{r4, r5, r6, r7, pc}\n"))
      return false;
    break;
  case NY_SM_A64:
    if (c.frame_bytes > 0 &&
        !ny_native_printf(w, "\tadd\tsp, sp, #%d\n", c.frame_bytes))
      return false;
    if (!ny_native_put(w, c.is_leaf ? "\tret\n"
                                    : "\tldp\tx29, x30, [sp], #16\n\tret\n"))
      return false;
    break;
  case NY_SM_I386:
    if (!ny_native_put(w, "\tleave\n\tret\n"))
      return false;
    break;
  case NY_SM_RISCV:
    if (!c.is_leaf &&
        !ny_native_printf(w, "\tld\tra, %d(sp)\n\tld\ts0, %d(sp)\n",
                          c.frame_bytes - 8, c.frame_bytes - 16))
      return false;
    if (!ny_native_printf(w, "\taddi\tsp, sp, %d\n\tret\n", c.frame_bytes))
      return false;
    break;
  }
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", pfx, name, pfx, name))
    return false;
  return true;
}

bool ny_native_arm_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const nyir_func_t *nyir, const char *func_name,
                            bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len, NY_SM_ARM,
                  "ARM", "r0", "r0", "r1", 4, 8);
}

bool ny_native_aarch64_emit_nir(ny_native_writer_t *w,
                                const ny_native_target_info_t *target,
                                const nyir_func_t *nyir, const char *func_name,
                                bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len, NY_SM_A64,
                  "AArch64", "x0", "x0", "x1", 8, 16);
}

bool ny_native_i386_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const nyir_func_t *nyir, const char *func_name,
                             bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len, NY_SM_I386,
                  "i386", "%eax", "%eax", "%ecx", 4, 16);
}

bool ny_native_riscv_emit_nir(ny_native_writer_t *w,
                              const ny_native_target_info_t *target,
                              const nyir_func_t *nyir, const char *func_name,
                              bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len,
                  NY_SM_RISCV, "RISC-V", "a0", "a0", "a1", 8, 16);
}
