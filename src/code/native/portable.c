#include "code/native/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef enum {
  NY_PORT_BPF,
  NY_PORT_MIPS,
  NY_PORT_POWERPC,
  NY_PORT_AVR,
  NY_PORT_WASM,
} ny_port_kind_t;

typedef struct {
  ny_native_writer_t *w;
  const ny_native_target_info_t *target;
  const ny_nir_func_t *nir;
  ny_port_kind_t kind;
  const char *name;
  const char *pretty;
  const char *ret_reg;
  const char *tmp0;
  const char *tmp1;
  int word_bytes;
  int frame_bytes;
  int local_base;
  int max_local_slot;
  char epilogue_label[160];
  char *err;
  size_t err_len;
} ny_port_ctx_t;

static int ny_port_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static void ny_port_compute_frame(ny_port_ctx_t *c) {
  c->max_local_slot = 0;
  for (size_t i = 0; c->nir && i < c->nir->len; ++i) {
    const ny_nir_inst_t *in = &c->nir->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  int value_slots = c->nir && c->nir->next_value > 0 ? c->nir->next_value : 0;
  c->local_base = value_slots * c->word_bytes;
  int spill = (value_slots + c->max_local_slot) * c->word_bytes;
  if (c->kind == NY_PORT_BPF)
    c->frame_bytes = ny_port_align(spill, 8);
  else if (c->kind == NY_PORT_WASM)
    c->frame_bytes = value_slots + c->max_local_slot;
  else
    c->frame_bytes = ny_port_align(spill + 32, 16);
}

static int ny_port_value_off(ny_port_ctx_t *c, int value) {
  return value * c->word_bytes;
}

static int ny_port_local_off(ny_port_ctx_t *c, int local) {
  return c->local_base + local * c->word_bytes;
}

static bool ny_port_check_value(ny_port_ctx_t *c, int value, const char *what) {
  if (value < 0 || !c || !c->nir || value >= c->nir->next_value) {
    ny_native_set_err(c ? c->err : NULL, c ? c->err_len : 0,
                      "%s NYIR emit: invalid %s v%d",
                      c ? c->pretty : "portable", what ? what : "value",
                      value);
    return false;
  }
  return true;
}

static bool ny_port_load_slot(ny_port_ctx_t *c, const char *reg, int off) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return ny_native_printf(c->w, "\t%s = *(u64 *)(r10 - %d)\n", reg,
                            off + 8);
  case NY_PORT_MIPS:
    return ny_native_printf(c->w, "\tld\t%s, %d($fp)\n", reg, off);
  case NY_PORT_POWERPC:
    return ny_native_printf(c->w, "\tld\t%s, %d(r31)\n", reg, off);
  case NY_PORT_AVR:
    return ny_native_printf(c->w,
                            "\t; load i64 spill %d into %s via AVR helper ABI\n",
                            off, reg);
  case NY_PORT_WASM:
    return ny_native_printf(c->w, "\tlocal.get $s%d ;; -> %s\n", off, reg);
  }
  return false;
}

static bool ny_port_store_slot(ny_port_ctx_t *c, int off, const char *reg) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return ny_native_printf(c->w, "\t*(u64 *)(r10 - %d) = %s\n", off + 8,
                            reg);
  case NY_PORT_MIPS:
    return ny_native_printf(c->w, "\tsd\t%s, %d($fp)\n", reg, off);
  case NY_PORT_POWERPC:
    return ny_native_printf(c->w, "\tstd\t%s, %d(r31)\n", reg, off);
  case NY_PORT_AVR:
    return ny_native_printf(c->w,
                            "\t; store %s into i64 spill %d via AVR helper ABI\n",
                            reg, off);
  case NY_PORT_WASM:
    return ny_native_printf(c->w, "\tlocal.set $s%d ;; <- %s\n", off, reg);
  }
  return false;
}

static bool ny_port_load_value(ny_port_ctx_t *c, const char *reg, int value) {
  return ny_port_check_value(c, value, "value") &&
         ny_port_load_slot(c, reg, ny_port_value_off(c, value));
}

static bool ny_port_store_value(ny_port_ctx_t *c, int value, const char *reg) {
  return ny_port_check_value(c, value, "destination") &&
         ny_port_store_slot(c, ny_port_value_off(c, value), reg);
}

static bool ny_port_load_local(ny_port_ctx_t *c, const char *reg, int local) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "%s NYIR emit: invalid local slot %d", c->pretty,
                      local);
    return false;
  }
  return ny_port_load_slot(c, reg, ny_port_local_off(c, local));
}

static bool ny_port_store_local(ny_port_ctx_t *c, int local, const char *reg) {
  if (local < 0 || local >= c->max_local_slot) {
    ny_native_set_err(c->err, c->err_len,
                      "%s NYIR emit: invalid local slot %d", c->pretty,
                      local);
    return false;
  }
  return ny_port_store_slot(c, ny_port_local_off(c, local), reg);
}

static bool ny_port_mov_imm(ny_port_ctx_t *c, const char *reg, int64_t value) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return ny_native_printf(c->w, "\t%s = %" PRId64 "\n", reg, value);
  case NY_PORT_MIPS:
    return ny_native_printf(c->w, "\tdli\t%s, %" PRId64 "\n", reg, value);
  case NY_PORT_POWERPC:
    return ny_native_printf(c->w, "\tli\t%s, %" PRId64 "\n", reg, value);
  case NY_PORT_AVR:
    return ny_native_printf(c->w,
                            "\t; %s = i64.const %" PRId64 " via AVR helper ABI\n",
                            reg, value);
  case NY_PORT_WASM:
    return ny_native_printf(c->w, "\ti64.const %" PRId64 " ;; -> %s\n", value,
                            reg);
  }
  return false;
}

static bool ny_port_binop(ny_port_ctx_t *c, const ny_nir_inst_t *in,
                          const char *bpf, const char *mips,
                          const char *ppc, const char *avr_helper,
                          const char *wasm) {
  if (!ny_port_load_value(c, c->tmp0, in->a) ||
      !ny_port_load_value(c, c->tmp1, in->b))
    return false;
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!ny_native_printf(c->w, "\t%s %s= %s\n", c->tmp0, bpf, c->tmp1))
      return false;
    break;
  case NY_PORT_MIPS:
    if (!ny_native_printf(c->w, "\t%s\t%s, %s, %s\n", mips, c->tmp0,
                          c->tmp0, c->tmp1))
      return false;
    break;
  case NY_PORT_POWERPC:
    if (strcmp(ppc, "subf") == 0) {
      if (!ny_native_printf(c->w, "\tsubf\t%s, %s, %s\n", c->tmp0,
                            c->tmp1, c->tmp0))
        return false;
    } else if (!ny_native_printf(c->w, "\t%s\t%s, %s, %s\n", ppc, c->tmp0,
                                 c->tmp0, c->tmp1))
      return false;
    break;
  case NY_PORT_AVR:
    if (!ny_native_printf(c->w, "\tcall\t%s\n", avr_helper))
      return false;
    break;
  case NY_PORT_WASM:
    if (!ny_native_printf(c->w, "\t%s\n", wasm))
      return false;
    break;
  }
  return ny_port_store_value(c, in->dst, c->tmp0);
}

static const char *ny_port_cmp_name(ny_nir_cmp_t cmp) {
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

static bool ny_port_cmp(ny_port_ctx_t *c, const ny_nir_inst_t *in) {
  if (!ny_port_load_value(c, c->tmp0, in->a) ||
      !ny_port_load_value(c, c->tmp1, in->b))
    return false;
  const char *pred = ny_port_cmp_name(in->cmp);
  size_t cmp_id = c->nir && c->nir->data ? (size_t)(in - c->nir->data) : 0;
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!ny_native_printf(c->w,
                          "\t%s = 0\n"
                          "\tif %s %s %s goto .Lny_cmp_true_%zu\n"
                          "\tgoto .Lny_cmp_done_%zu\n"
                          ".Lny_cmp_true_%zu:\n"
                          "\t%s = 1\n"
                          ".Lny_cmp_done_%zu:\n",
                          c->tmp0, c->tmp0,
                          in->cmp == NY_NIR_CMP_EQ ? "==" :
                          in->cmp == NY_NIR_CMP_NE ? "!=" :
                          in->cmp == NY_NIR_CMP_LT ? "<" :
                          in->cmp == NY_NIR_CMP_LE ? "<=" :
                          in->cmp == NY_NIR_CMP_GT ? ">" : ">=",
                          c->tmp1, cmp_id, cmp_id, cmp_id, c->tmp0,
                          cmp_id))
      return false;
    break;
  case NY_PORT_MIPS:
    if (!ny_native_printf(c->w, "\t; %s = (%s %s %s)\n", c->tmp0, c->tmp0,
                          pred, c->tmp1))
      return false;
    break;
  case NY_PORT_POWERPC:
    if (!ny_native_printf(c->w, "\t; %s = (%s %s %s)\n", c->tmp0, c->tmp0,
                          pred, c->tmp1))
      return false;
    break;
  case NY_PORT_AVR:
    if (!ny_native_printf(c->w, "\tcall\t__ny_avr_i64_cmp_%s\n", pred))
      return false;
    break;
  case NY_PORT_WASM:
    if (!ny_native_printf(c->w, "\ti64.%s_s\n", pred))
      return false;
    break;
  }
  return ny_port_store_value(c, in->dst, c->tmp0);
}

static bool ny_port_label(ny_port_ctx_t *c, int64_t label) {
  if (c->kind == NY_PORT_WASM)
    return ny_native_printf(c->w, "\t;; label L%" PRId64 "\n", label);
  return ny_native_printf(c->w, ".Lny_nir_L%" PRId64 ":\n", label);
}

static bool ny_port_jump(ny_port_ctx_t *c, int64_t label) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return ny_native_printf(c->w, "\tgoto .Lny_nir_L%" PRId64 "\n", label);
  case NY_PORT_MIPS:
    return ny_native_printf(c->w, "\tb\t.Lny_nir_L%" PRId64 "\n", label);
  case NY_PORT_POWERPC:
    return ny_native_printf(c->w, "\tb\t.Lny_nir_L%" PRId64 "\n", label);
  case NY_PORT_AVR:
    return ny_native_printf(c->w, "\trjmp\t.Lny_nir_L%" PRId64 "\n", label);
  case NY_PORT_WASM:
    return ny_native_printf(c->w, "\tbr $L%" PRId64 "\n", label);
  }
  return false;
}

static bool ny_port_call(ny_port_ctx_t *c, const ny_nir_inst_t *in) {
  int args[NY_NIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!ny_nir_call_args(in, c->nir->next_value, args,
                        NY_NIR_CALL_MAX_ARGS, &argc, c->err, c->err_len))
    return false;
  if ((size_t)argc > c->target->gp_arg_reg_count) {
    ny_native_set_err(c->err, c->err_len,
                      "%s NYIR emit: %d scalar arguments exceed the %zu-register ABI slice",
                      c->pretty, argc, c->target->gp_arg_reg_count);
    return false;
  }
  for (int i = 0; i < argc; ++i)
    if (!ny_port_load_value(c, c->target->gp_arg_regs[i], args[i]))
      return false;
  const char *sym = in->symbol ? in->symbol : "";
  bool is_ext = (in->flags & NY_NIR_INST_F_EXTERN) != 0;
  (void)is_ext;
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!ny_native_printf(c->w, "\tcall\t%s%s%s\n", c->target->symbol_prefix,
                          is_ext ? "" : "ny_fn_", sym))
      return false;
    return in->dst < 0 || ny_port_store_value(c, in->dst, "r0");
  case NY_PORT_MIPS:
    if (!ny_native_printf(c->w, "\tjal\t%s%s%s\n\tnop\n",
                          c->target->symbol_prefix, is_ext ? "" : "ny_fn_", sym))
      return false;
    return in->dst < 0 || ny_port_store_value(c, in->dst, "$v0");
  case NY_PORT_POWERPC:
    if (!ny_native_printf(c->w, "\tbl\t%s%s%s\n", c->target->symbol_prefix,
                          is_ext ? "" : "ny_fn_", sym))
      return false;
    return in->dst < 0 || ny_port_store_value(c, in->dst, "r3");
  case NY_PORT_AVR:
    if (!ny_native_printf(c->w, "\tcall\t%s%s%s\n", c->target->symbol_prefix,
                          is_ext ? "" : "ny_fn_", sym))
      return false;
    return in->dst < 0 || ny_port_store_value(c, in->dst, "r24:r31");
  case NY_PORT_WASM:
    if (!ny_native_printf(c->w, "\tcall $%s%s%s\n", c->target->symbol_prefix,
                          is_ext ? "" : "ny_fn_", sym))
      return false;
    return in->dst < 0 || ny_port_store_value(c, in->dst, "$ret");
  }
  return false;
}

static bool ny_port_emit_inst(ny_port_ctx_t *c, const ny_nir_inst_t *in) {
  switch (in->op) {
  case NY_NIR_NOP:
    return true;
  case NY_NIR_CONST_I64:
    return ny_port_mov_imm(c, c->tmp0, in->imm) &&
           ny_port_store_value(c, in->dst, c->tmp0);
  case NY_NIR_COPY:
    return ny_port_load_value(c, c->tmp0, in->a) &&
           ny_port_store_value(c, in->dst, c->tmp0);
  case NY_NIR_ADD_I64:
    return ny_port_binop(c, in, "+", "daddu", "add", "__ny_avr_i64_add",
                         "i64.add");
  case NY_NIR_SUB_I64:
    return ny_port_binop(c, in, "-", "dsubu", "subf", "__ny_avr_i64_sub",
                         "i64.sub");
  case NY_NIR_MUL_I64:
    return ny_port_binop(c, in, "*", "dmul", "mulld", "__ny_avr_i64_mul",
                         "i64.mul");
  case NY_NIR_DIV_I64:
    return ny_port_binop(c, in, "/", "ddiv", "divd", "__ny_avr_i64_div",
                         "i64.div_s");
  case NY_NIR_MOD_I64:
    return ny_port_binop(c, in, "%", "drem", "modsd", "__ny_avr_i64_mod",
                         "i64.rem_s");
  case NY_NIR_AND_I64:
    return ny_port_binop(c, in, "&", "and", "and", "__ny_avr_i64_and",
                         "i64.and");
  case NY_NIR_OR_I64:
    return ny_port_binop(c, in, "|", "or", "or", "__ny_avr_i64_or",
                         "i64.or");
  case NY_NIR_XOR_I64:
    return ny_port_binop(c, in, "^", "xor", "xor", "__ny_avr_i64_xor",
                         "i64.xor");
  case NY_NIR_SHL_I64:
    return ny_port_binop(c, in, "<<", "dsllv", "sld", "__ny_avr_i64_shl",
                         "i64.shl");
  case NY_NIR_SAR_I64:
    return ny_port_binop(c, in, ">>", "dsrav", "srad", "__ny_avr_i64_sar",
                         "i64.shr_s");
  case NY_NIR_CMP_I64:
    return ny_port_cmp(c, in);
  case NY_NIR_LABEL:
    return ny_port_label(c, in->imm);
  case NY_NIR_LOAD_LOCAL:
    return ny_port_load_local(c, c->tmp0, (int)in->imm) &&
           ny_port_store_value(c, in->dst, c->tmp0);
  case NY_NIR_STORE_LOCAL:
    return ny_port_load_value(c, c->tmp0, in->a) &&
           ny_port_store_local(c, (int)in->imm, c->tmp0);
  case NY_NIR_CALL:
    return ny_port_call(c, in);
  case NY_NIR_RET:
    if (in->a >= 0 && !ny_port_load_value(c, c->ret_reg, in->a))
      return false;
    return ny_native_printf(c->w, "\t%s\t%s\n",
                            c->kind == NY_PORT_BPF ? "goto" :
                            c->kind == NY_PORT_AVR ? "rjmp" :
                            c->kind == NY_PORT_WASM ? "br" : "b",
                            c->epilogue_label);
  case NY_NIR_BR:
    return ny_port_jump(c, in->imm);
  case NY_NIR_BR_IF:
    if (!ny_port_load_value(c, c->tmp0, in->a))
      return false;
    switch (c->kind) {
    case NY_PORT_BPF:
      return ny_native_printf(c->w, "\tif %s != 0 goto .Lny_nir_L%" PRId64 "\n",
                              c->tmp0, in->imm);
    case NY_PORT_MIPS:
      return ny_native_printf(c->w, "\tbnez\t%s, .Lny_nir_L%" PRId64 "\n",
                              c->tmp0, in->imm);
    case NY_PORT_POWERPC:
      return ny_native_printf(c->w,
                              "\tcmpdi\t%s, 0\n\tbne\t.Lny_nir_L%" PRId64 "\n",
                              c->tmp0, in->imm);
    case NY_PORT_AVR:
      return ny_native_printf(c->w,
                              "\t; branch if %s != 0\n\tbrne\t.Lny_nir_L%" PRId64 "\n",
                              c->tmp0, in->imm);
    case NY_PORT_WASM:
      return ny_native_printf(c->w, "\tbr_if $L%" PRId64 "\n", in->imm);
    }
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
  case NYIR_ADDR_SYMBOL:
  case NYIR_ALLOCA:
  case NYIR_COPY_STRUCT:
  case NYIR_CAPTURE_RET:
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
  case NYIR_OP_COUNT:
    break;
  }
  ny_native_set_err(c->err, c->err_len, "%s NYIR emit: unsupported op %s",
                    c->pretty, ny_nir_op_name(in->op));
  return false;
}

static bool ny_port_header(ny_port_ctx_t *c) {
  if (c->kind == NY_PORT_WASM) {
    if (!ny_native_printf(c->w, ";; Nytrix WebAssembly text backend\n"))
      return false;
    if (!ny_native_printf(c->w, "(func $%s%s (result i64)\n",
                          c->target->symbol_prefix, c->name))
      return false;
    for (int i = 0; i < c->frame_bytes; ++i) {
      if (!ny_native_printf(c->w, "\t(local $s%d i64)\n", i * 8))
        return false;
    }
    return true;
  }
  if (!ny_native_put(c->w, "\t.text\n"))
    return false;
  if (strcmp(c->target->object_format, "macho") != 0 &&
      !ny_native_printf(c->w, "\t.type\t%s%s, @function\n",
                        c->target->symbol_prefix, c->name))
    return false;
  if (!ny_native_printf(c->w, "\t.globl\t%s%s\n%s%s:\n",
                        c->target->symbol_prefix, c->name,
                        c->target->symbol_prefix, c->name))
    return false;
  switch (c->kind) {
  case NY_PORT_BPF:
    return ny_native_printf(c->w,
                            "\t; eBPF uses r10 as frame pointer; spills live at negative offsets\n");
  case NY_PORT_MIPS:
    return ny_native_printf(c->w,
                            "\tdaddiu\t$sp, $sp, -%d\n"
                            "\tsd\t$ra, %d($sp)\n"
                            "\tsd\t$fp, %d($sp)\n"
                            "\tmove\t$fp, $sp\n",
                            c->frame_bytes, c->frame_bytes - 8,
                            c->frame_bytes - 16);
  case NY_PORT_POWERPC:
    return ny_native_printf(c->w,
                            "\tmflr\tr0\n"
                            "\tstd\tr0, 16(r1)\n"
                            "\tstdu\tr1, -%d(r1)\n"
                            "\tmr\tr31, r1\n",
                            c->frame_bytes);
  case NY_PORT_AVR:
    return ny_native_printf(c->w,
                            "\t; AVR raw-int NYIR path uses helper-lowered 64-bit slots\n"
                            "\t; logical frame bytes: %d\n",
                            c->frame_bytes);
  case NY_PORT_WASM:
    return true;
  }
  return false;
}

static bool ny_port_footer(ny_port_ctx_t *c, bool tag_return) {
  if (!ny_native_printf(c->w, "%s:\n", c->epilogue_label))
    return false;
  if (tag_return) {
    switch (c->kind) {
    case NY_PORT_BPF:
      if (!ny_native_put(c->w, "\tr0 <<= 1\n\tr0 += 1\n"))
        return false;
      break;
    case NY_PORT_MIPS:
      if (!ny_native_put(c->w, "\tdsll\t$v0, $v0, 1\n\tdaddiu\t$v0, $v0, 1\n"))
        return false;
      break;
    case NY_PORT_POWERPC:
      if (!ny_native_put(c->w, "\tsldi\tr3, r3, 1\n\taddi\tr3, r3, 1\n"))
        return false;
      break;
    case NY_PORT_AVR:
      if (!ny_native_put(c->w, "\tcall\t__ny_avr_tag_i64\n"))
        return false;
      break;
    case NY_PORT_WASM:
      if (!ny_native_put(c->w, "\ti64.const 1\n\ti64.shl\n\ti64.const 1\n\ti64.add\n"))
        return false;
      break;
    }
  }
  switch (c->kind) {
  case NY_PORT_BPF:
    return ny_native_put(c->w, "\texit\n");
  case NY_PORT_MIPS:
    return ny_native_printf(c->w,
                            "\tld\t$ra, %d($sp)\n"
                            "\tld\t$fp, %d($sp)\n"
                            "\tdaddiu\t$sp, $sp, %d\n"
                            "\tjr\t$ra\n\tnop\n",
                            c->frame_bytes - 8, c->frame_bytes - 16,
                            c->frame_bytes);
  case NY_PORT_POWERPC:
    return ny_native_printf(c->w,
                            "\taddi\tr1, r31, %d\n"
                            "\tld\tr0, 16(r1)\n"
                            "\tmtlr\tr0\n"
                            "\tblr\n",
                            c->frame_bytes);
  case NY_PORT_AVR:
    return ny_native_put(c->w, "\tret\n");
  case NY_PORT_WASM:
    return ny_native_put(c->w, ")\n");
  }
  return false;
}

static bool ny_port_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const ny_nir_func_t *nir, const char *func_name,
                             bool tag_return, char *err, size_t err_len,
                             ny_port_kind_t kind, const char *pretty,
                             const char *ret_reg, const char *tmp0,
                             const char *tmp1, int word_bytes) {
  if (!w || !target || !nir)
    return false;
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  ny_port_ctx_t ctx = {.w = w,
                       .target = target,
                       .nir = nir,
                       .kind = kind,
                       .name = name,
                       .pretty = pretty,
                       .ret_reg = ret_reg,
                       .tmp0 = tmp0,
                       .tmp1 = tmp1,
                       .word_bytes = word_bytes,
                       .err = err,
                       .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label),
           kind == NY_PORT_WASM ? "$ny_%s_epilogue_%s" : ".Lny_%s_epilogue_%s",
           target->target_name ? target->target_name : "portable", name);
  ny_port_compute_frame(&ctx);

  if (!ny_port_header(&ctx))
    return false;

  if (strcmp(name, "rt_main") != 0) {
    int max = ctx.max_local_slot < (int)target->gp_arg_reg_count
                  ? ctx.max_local_slot
                  : (int)target->gp_arg_reg_count;
    for (int i = 0; i < max; ++i) {
      if (!ny_port_store_local(&ctx, i, target->gp_arg_regs[i]))
        return false;
    }
  }

  for (size_t i = 0; i < nir->len; ++i) {
    if (!ny_port_emit_inst(&ctx, &nir->data[i])) {
      fprintf(stderr, "native NYIR repro (%s emit failed):\n", pretty);
      ny_nir_dump(stderr, nir, name);
      return false;
    }
  }

  if (!ny_port_footer(&ctx, tag_return))
    return false;
  if (kind != NY_PORT_WASM && strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", target->symbol_prefix,
                        name, target->symbol_prefix, name))
    return false;
  return true;
}

bool ny_native_bpf_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir, const char *func_name,
                            bool tag_return, char *err, size_t err_len) {
  return ny_port_emit_nir(w, target, nir, func_name, tag_return, err, err_len,
                          NY_PORT_BPF, "BPF", "r0", "r6", "r7", 8);
}

bool ny_native_mips_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const ny_nir_func_t *nir, const char *func_name,
                             bool tag_return, char *err, size_t err_len) {
  return ny_port_emit_nir(w, target, nir, func_name, tag_return, err, err_len,
                          NY_PORT_MIPS, "MIPS", "$v0", "$t0", "$t1", 8);
}

bool ny_native_powerpc_emit_nir(ny_native_writer_t *w,
                                const ny_native_target_info_t *target,
                                const ny_nir_func_t *nir,
                                const char *func_name, bool tag_return,
                                char *err, size_t err_len) {
  return ny_port_emit_nir(w, target, nir, func_name, tag_return, err, err_len,
                          NY_PORT_POWERPC, "PowerPC", "r3", "r4", "r5", 8);
}

bool ny_native_avr_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir, const char *func_name,
                            bool tag_return, char *err, size_t err_len) {
  return ny_port_emit_nir(w, target, nir, func_name, tag_return, err, err_len,
                          NY_PORT_AVR, "AVR", "r24:r31", "r24:r31",
                          "r16:r23", 8);
}

bool ny_native_wasm_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const ny_nir_func_t *nir, const char *func_name,
                             bool tag_return, char *err, size_t err_len) {
  return ny_port_emit_nir(w, target, nir, func_name, tag_return, err, err_len,
                          NY_PORT_WASM, "WebAssembly", "$ret", "$t0", "$t1",
                          8);
}
