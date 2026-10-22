/*
 * Native backend stack manager: explicit stack-frame layout tracking
 * for register spills, local variables, and ABI-mandated slots.
 */
#include "base/debug.h"
#include "code/backend/abi.h"
#include "code/backend/internal.h"

#include <inttypes.h>
#include <limits.h>
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
  nyr_writer_t *w;
  const ny_native_target_info_t *target;
  const nyr_func_t *nyir;
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
  size_t bounds_serial;
  size_t copy_serial;
  size_t divmod_serial;
  int max_divmod_slots;
  bool is_leaf;
  char epilogue[160];
  char *err;
  size_t err_len;
} ny_sm_ctx_t;

static int sm_align(int n, int a) { return (n + a - 1) & ~(a - 1); }

static void sm_frame(ny_sm_ctx_t *c) {
  c->max_local_slot = 0;
  c->max_divmod_slots = 0;
  c->is_leaf = true;
  for (size_t i = 0; c->nyir && i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    if (in->op == NYIR_CALL)
      c->is_leaf = false;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
    if (in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64)
      ++c->max_divmod_slots;
  }
  if ((c->kind == NY_SM_A64 || c->kind == NY_SM_ARM) && c->nyir &&
      c->nyir->param_count > (size_t)c->max_local_slot)
    c->max_local_slot = (int)c->nyir->param_count;
  int vs = c->nyir && c->nyir->next_value > 0 ? c->nyir->next_value : 0;
  int slot = c->kind == NY_SM_I386 ? 8 : c->word;
  c->local_base = vs * slot;
  int spill = (vs + c->max_local_slot) * slot;
  if (c->kind == NY_SM_ARM)
    spill += (vs + c->max_local_slot + c->max_divmod_slots * 10) * c->word;
  if (c->kind == NY_SM_RISCV)
    c->frame_bytes = sm_align(spill + 16, c->frame_align);
  else
    c->frame_bytes = sm_align(spill, c->frame_align);
  ny_dbg_mem_layout(c->pretty ? c->pretty : "NYIR", 0, (size_t)c->frame_bytes,
                    c->frame_align,
                    "frame: value slots followed by source-local slots");
  ny_dbg_mem_layout("NYIR values", 0, (size_t)c->local_base, slot,
                    "spill/home slots");
  ny_dbg_mem_layout("NYIR locals", (size_t)c->local_base,
                    (size_t)c->max_local_slot * (size_t)slot *
                        (c->kind == NY_SM_ARM ? 2u : 1u),
                    c->kind == NY_SM_ARM ? 2 * slot : slot,
                    "source local slots");
  if (c->kind == NY_SM_ARM)
    ny_dbg_mem_layout("NYIR i64 high words",
                      (size_t)(vs + c->max_local_slot) * (size_t)c->word,
                      (size_t)(vs + c->max_local_slot) * (size_t)c->word,
                      c->word,
                      "secondary 32-bit halves for values and local slots");
}

static bool sm_err_val(ny_sm_ctx_t *c, int v, const char *what) {
  if (v < 0 || !c || !c->nyir || v >= c->nyir->next_value) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "%s NYIR emit: invalid %s v%d", c ? c->pretty : "stack",
                  what ? what : "value", v);
    return false;
  }
  return true;
}

static int sm_voff(ny_sm_ctx_t *c, int v) {
  if (c->kind == NY_SM_I386)
    return -4 - 8 * (v + 1);
  return v * c->word;
}
static int sm_vhi_off(ny_sm_ctx_t *c, int v) {
  if (c->kind == NY_SM_ARM) {
    const int values =
        c->nyir && c->nyir->next_value > 0 ? c->nyir->next_value : 0;
    return (values + 2 * c->max_local_slot + v) * c->word;
  }
  return sm_voff(c, v) + 4;
}
static int sm_loff(ny_sm_ctx_t *c, int local) {
  if (c->kind == NY_SM_I386)
    return -4 - c->local_base - 8 * (local + 1);
  if (c->kind == NY_SM_ARM)
    return c->local_base + 2 * local * c->word;
  return c->local_base + local * c->word;
}
static int sm_lhi_off(ny_sm_ctx_t *c, int local) {
  return sm_loff(c, local) + c->word;
}

static bool sm_movi(ny_sm_ctx_t *c, const char *reg, int64_t v);

static bool sm_ld(ny_sm_ctx_t *c, const char *reg, int off) {
  switch (c->kind) {
  case NY_SM_ARM:
    return nyr_printf(c->w, "\tldr\t%s, [sp, #%d]\n", reg, off);
  case NY_SM_A64:
    if (off < 0 || (off & 7)) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 NYIR emit: stack offset %d out of range", off);
      return false;
    }
    if (off <= 32760)
      return nyr_printf(c->w, "\tldr\t%s, [sp, #%d]\n", reg, off);
    return sm_movi(c, "x16", off) && nyr_put(c->w, "\tadd\tx16, sp, x16\n") &&
           nyr_printf(c->w, "\tldr\t%s, [x16]\n", reg);
  case NY_SM_I386:
    return nyr_printf(c->w, "\tmovl\t%d(%%ebp), %s\n", off, reg);
  case NY_SM_RISCV:
    if (off < 0 || off > 2047) {
      nyr_set_error(c->err, c->err_len,
                    "RISC-V NYIR emit: frame offset %d out of range", off);
      return false;
    }
    return nyr_printf(c->w, "\tld\t%s, %d(sp)\n", reg, off);
  }
  return false;
}

static bool sm_st(ny_sm_ctx_t *c, int off, const char *reg) {
  switch (c->kind) {
  case NY_SM_ARM:
    return nyr_printf(c->w, "\tstr\t%s, [sp, #%d]\n", reg, off);
  case NY_SM_A64:
    if (off < 0 || (off & 7)) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 NYIR emit: stack offset %d out of range", off);
      return false;
    }
    if (off <= 32760)
      return nyr_printf(c->w, "\tstr\t%s, [sp, #%d]\n", reg, off);
    return sm_movi(c, "x16", off) && nyr_put(c->w, "\tadd\tx16, sp, x16\n") &&
           nyr_printf(c->w, "\tstr\t%s, [x16]\n", reg);
  case NY_SM_I386:
    return nyr_printf(c->w, "\tmovl\t%s, %d(%%ebp)\n", reg, off);
  case NY_SM_RISCV:
    if (off < 0 || off > 2047) {
      nyr_set_error(c->err, c->err_len,
                    "RISC-V NYIR emit: frame offset %d out of range", off);
      return false;
    }
    return nyr_printf(c->w, "\tsd\t%s, %d(sp)\n", reg, off);
  }
  return false;
}

static bool sm_ldv(ny_sm_ctx_t *c, const char *reg, int v) {
  return sm_err_val(c, v, "value") && sm_ld(c, reg, sm_voff(c, v));
}
static bool sm_stv(ny_sm_ctx_t *c, int v, const char *reg) {
  return sm_err_val(c, v, "destination") && sm_st(c, sm_voff(c, v), reg);
}

static bool sm_i386_store_u32(ny_sm_ctx_t *c, int off, uint32_t bits) {
  return nyr_printf(c->w, "\tmovl\t$0x%08" PRIx32 ", %%eax\n", bits) &&
         sm_st(c, off, "%eax");
}

static bool sm_i386_store_f64(ny_sm_ctx_t *c, int dst, uint64_t bits) {
  if (!sm_err_val(c, dst, "destination"))
    return false;
  int off = sm_voff(c, dst);
  return sm_i386_store_u32(c, off, (uint32_t)bits) &&
         sm_i386_store_u32(c, off + 4, (uint32_t)(bits >> 32));
}

static bool sm_i386_copy_value(ny_sm_ctx_t *c, int dst, int src) {
  if (!sm_ldv(c, c->t0, src) || !sm_stv(c, dst, c->t0))
    return false;
  if (c->kind != NY_SM_I386)
    return true;
  return sm_ld(c, c->t0, sm_voff(c, src) + 4) &&
         sm_st(c, sm_voff(c, dst) + 4, c->t0);
}

static bool sm_i386_emit_fbinop(ny_sm_ctx_t *c, const nyr_inst_t *in,
                                bool is_f32, const char *op) {
  if (c->kind != NY_SM_I386 || !sm_err_val(c, in->a, "value") ||
      !sm_err_val(c, in->b, "value") || !sm_err_val(c, in->dst, "destination"))
    return false;
  const char *load = is_f32 ? "flds" : "fldl";
  const char *store = is_f32 ? "fstps" : "fstpl";
  return nyr_printf(c->w, "\t%s\t%d(%%ebp)\n\t%s\t%d(%%ebp)\n", load,
                    sm_voff(c, in->a), load, sm_voff(c, in->b)) &&
         nyr_printf(c->w, "\t%s\t%%st, %%st(1)\n", op) &&
         nyr_printf(c->w, "\t%s\t%d(%%ebp)\n", store, sm_voff(c, in->dst));
}

static bool sm_i386_cmp_f(ny_sm_ctx_t *c, const nyr_inst_t *in, bool is_f32) {
  if (c->kind != NY_SM_I386 || !sm_err_val(c, in->a, "value") ||
      !sm_err_val(c, in->b, "value") || !sm_err_val(c, in->dst, "destination"))
    return false;
  const char *load = is_f32 ? "flds" : "fldl";
  const char *set = in->cmp == NYIR_CMP_EQ   ? "e"
                    : in->cmp == NYIR_CMP_NE ? "ne"
                    : in->cmp == NYIR_CMP_LT ? "b"
                    : in->cmp == NYIR_CMP_LE ? "be"
                    : in->cmp == NYIR_CMP_GT ? "a"
                                             : "ae";
  return nyr_printf(c->w, "\t%s\t%d(%%ebp)\n\t%s\t%d(%%ebp)\n", load,
                    sm_voff(c, in->b), load, sm_voff(c, in->a)) &&
         nyr_put(c->w, "\tfcomip\t%st(1), %st\n\tfstp\t%st(0)\n"
                       "\tmovl\t$0, %eax\n") &&
         nyr_printf(c->w, "\tset%s\t%%al\n\tmovzbl\t%%al, %%eax\n", set) &&
         sm_stv(c, in->dst, c->t0);
}

static bool sm_a64_fbinop(ny_sm_ctx_t *c, const nyr_inst_t *in, bool is_f32,
                          const char *op) {
  if (c->kind != NY_SM_A64 || !sm_err_val(c, in->a, "value") ||
      !sm_err_val(c, in->b, "value") || !sm_err_val(c, in->dst, "destination"))
    return false;
  const char *reg = is_f32 ? "s" : "d";
  return nyr_printf(c->w,
                    "\tldr\t%s0, [sp, #%d]\n"
                    "\tldr\t%s1, [sp, #%d]\n"
                    "\t%s\t%s0, %s0, %s1\n"
                    "\tstr\t%s0, [sp, #%d]\n",
                    reg, sm_voff(c, in->a), reg, sm_voff(c, in->b), op, reg,
                    reg, reg, reg, sm_voff(c, in->dst));
}

static bool sm_a64_convert(ny_sm_ctx_t *c, const nyr_inst_t *in,
                           const char *load_reg, const char *convert,
                           const char *store_reg) {
  if (c->kind != NY_SM_A64 || !sm_err_val(c, in->a, "value") ||
      !sm_err_val(c, in->dst, "destination"))
    return false;
  return nyr_printf(c->w, "\tldr\t%s, [sp, #%d]\n", load_reg,
                    sm_voff(c, in->a)) &&
         nyr_printf(c->w, "\t%s\n", convert) &&
         nyr_printf(c->w, "\tstr\t%s, [sp, #%d]\n", store_reg,
                    sm_voff(c, in->dst));
}

static bool sm_a64_float_to_i64(ny_sm_ctx_t *c, const nyr_inst_t *in,
                                bool is_f32) {
  if (c->kind != NY_SM_A64 || !sm_err_val(c, in->a, "value") ||
      !sm_err_val(c, in->dst, "destination"))
    return false;
  return nyr_printf(c->w,
                    "\tldr\t%s0, [sp, #%d]\n"
                    "\tfcvtzs\tx0, %s0\n",
                    is_f32 ? "s" : "d", sm_voff(c, in->a),
                    is_f32 ? "s" : "d") &&
         sm_stv(c, in->dst, "x0");
}

static bool sm_a64_cmp_float(ny_sm_ctx_t *c, const nyr_inst_t *in,
                             bool is_f32) {
  if (c->kind != NY_SM_A64 || !sm_err_val(c, in->a, "value") ||
      !sm_err_val(c, in->b, "value") || !sm_err_val(c, in->dst, "destination"))
    return false;
  const char *condition = NULL;
  switch (in->cmp) {
  case NYIR_CMP_EQ:
    condition = "eq";
    break;
  case NYIR_CMP_NE:
    condition = "ne";
    break;
  case NYIR_CMP_LT:
    condition = "mi";
    break;
  case NYIR_CMP_LE:
    condition = "ls";
    break;
  case NYIR_CMP_GT:
    condition = "gt";
    break;
  case NYIR_CMP_GE:
    condition = "ge";
    break;
  default:
    nyr_set_error(c->err, c->err_len,
                  "AArch64 NYIR emit: invalid float comparison predicate");
    return false;
  }
  const char *reg = is_f32 ? "s" : "d";
  return nyr_printf(c->w,
                    "\tldr\t%s0, [sp, #%d]\n"
                    "\tldr\t%s1, [sp, #%d]\n"
                    "\tfcmp\t%s0, %s1\n"
                    "\tcset\tx0, %s\n",
                    reg, sm_voff(c, in->a), reg, sm_voff(c, in->b), reg, reg,
                    condition) &&
         sm_stv(c, in->dst, "x0");
}

static bool sm_a64_store_parameters(ny_sm_ctx_t *c) {
  size_t gp = 0;
  size_t fp = 0;
  size_t stack_offset = 0;
  for (size_t i = 0; i < c->nyir->param_count; ++i) {
    nyr_param_type_t type =
        c->nyir->param_types ? c->nyir->param_types[i] : NYIR_PARAM_I64;
    bool is_f32 = type == NYIR_PARAM_F32;
    bool is_f64 = type == NYIR_PARAM_F64;
    size_t reg = is_f32 || is_f64 ? fp++ : gp++;
    if (type == NYIR_PARAM_AGGREGATE) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 NYIR emit: aggregate parameters are unsupported "
                    "by the portable stack emitter");
      return false;
    }
    if (i >= (size_t)c->max_local_slot)
      continue;
    int offset = sm_loff(c, (int)i);
    if (reg >= 8) {
      size_t incoming_offset =
          (size_t)c->frame_bytes + (c->is_leaf ? 0 : 16) + stack_offset;
      const char *base = c->is_leaf ? "sp" : "x29";
      size_t base_offset = c->is_leaf ? incoming_offset : 16 + stack_offset;
      if (!nyr_printf(c->w,
                      "\tldr\tx0, [%s, #%zu]\n"
                      "\tstr\tx0, [sp, #%d]\n",
                      base, base_offset, offset))
        return false;
      stack_offset += 8;
    } else if (is_f32 || is_f64) {
      if (!nyr_printf(c->w, "\tstr\t%s%zu, [sp, #%d]\n", is_f32 ? "s" : "d",
                      reg, offset))
        return false;
    } else if (!nyr_printf(c->w, "\tstr\tx%zu, [sp, #%d]\n", reg, offset)) {
      return false;
    }
  }
  return true;
}

typedef struct {
  size_t next_reg;
  size_t stack_bytes;
  bool stack_started;
} sm_arm_arg_state_t;

typedef struct {
  size_t reg;
  size_t stack_offset;
  unsigned words;
} sm_arm_arg_location_t;

static void sm_arm_assign_arg(sm_arm_arg_state_t *state, unsigned words,
                              sm_arm_arg_location_t *location) {
  if (words == 2 && (state->next_reg & 1u))
    ++state->next_reg;
  if (!state->stack_started && state->next_reg + words <= 4) {
    location->reg = state->next_reg;
    location->stack_offset = SIZE_MAX;
    location->words = words;
    state->next_reg += words;
    return;
  }
  state->stack_started = true;
  location->reg = SIZE_MAX;
  if (words == 2)
    state->stack_bytes = (state->stack_bytes + 7u) & ~(size_t)7u;
  else
    state->stack_bytes = (state->stack_bytes + 3u) & ~(size_t)3u;
  location->stack_offset = state->stack_bytes;
  location->words = words;
  state->stack_bytes += (size_t)words * 4u;
}

static bool sm_arm_store_parameters(ny_sm_ctx_t *c) {
  sm_arm_arg_state_t state = {0};
  for (size_t i = 0; i < c->nyir->param_count; ++i) {
    nyr_param_type_t type =
        c->nyir->param_types ? c->nyir->param_types[i] : NYIR_PARAM_I64;
    unsigned words = type == NYIR_PARAM_F64 ? 2u : 1u;
    sm_arm_arg_location_t location;
    sm_arm_assign_arg(&state, words, &location);
    int low_off = sm_loff(c, (int)i);
    int high_off = sm_lhi_off(c, (int)i);
    if (location.reg != SIZE_MAX) {
      if (!nyr_printf(c->w, "\tstr\tr%zu, [sp, #%d]\n", location.reg, low_off))
        return false;
      if (type == NYIR_PARAM_F64) {
        if (!nyr_printf(c->w, "\tstr\tr%zu, [sp, #%d]\n", location.reg + 1u,
                        high_off))
          return false;
      } else if (type == NYIR_PARAM_I64) {
        if (!nyr_printf(c->w, "\tasr\tr12, r%zu, #31\n", location.reg) ||
            !sm_st(c, high_off, "r12"))
          return false;
      } else if (!nyr_put(c->w, "\tmov\tr12, #0\n") ||
                 !sm_st(c, high_off, "r12")) {
        return false;
      }
      continue;
    }
    size_t incoming_offset = (size_t)c->frame_bytes + (c->is_leaf ? 0u : 24u) +
                             location.stack_offset;
    if (incoming_offset > INT_MAX || !sm_ld(c, "r12", (int)incoming_offset) ||
        !sm_st(c, low_off, "r12"))
      return false;
    if (type == NYIR_PARAM_F64) {
      if (incoming_offset + 4u > INT_MAX ||
          !sm_ld(c, "r12", (int)(incoming_offset + 4u)) ||
          !sm_st(c, high_off, "r12"))
        return false;
    } else if (type == NYIR_PARAM_I64) {
      if (!nyr_put(c->w, "\tasr\tr12, r12, #31\n") ||
          !sm_st(c, high_off, "r12"))
        return false;
    } else if (!nyr_put(c->w, "\tmov\tr12, #0\n") ||
               !sm_st(c, high_off, "r12")) {
      return false;
    }
  }
  return true;
}

static bool sm_ldl(ny_sm_ctx_t *c, const char *reg, int local) {
  if (local < 0 || local >= c->max_local_slot) {
    nyr_set_error(c->err, c->err_len, "%s NYIR emit: bad local %d", c->pretty,
                  local);
    return false;
  }
  return sm_ld(c, reg, sm_loff(c, local));
}
static bool sm_stl(ny_sm_ctx_t *c, int local, const char *reg) {
  if (local < 0 || local >= c->max_local_slot) {
    nyr_set_error(c->err, c->err_len, "%s NYIR emit: bad local %d", c->pretty,
                  local);
    return false;
  }
  return sm_st(c, sm_loff(c, local), reg);
}

static bool sm_movi(ny_sm_ctx_t *c, const char *reg, int64_t v) {
  switch (c->kind) {
  case NY_SM_ARM:
    if (v < INT32_MIN || v > INT32_MAX) {
      nyr_set_error(c->err, c->err_len, "ARM NYIR emit: imm out of i32");
      return false;
    }
    return nyr_printf(c->w, "\tmovw\t%s, #%u\n\tmovt\t%s, #%u\n", reg,
                      (unsigned)((uint32_t)v & 0xffff), reg,
                      (unsigned)(((uint32_t)v >> 16) & 0xffff));
  case NY_SM_A64: {
    uint64_t bits = (uint64_t)v;
    bool emitted = false;
    for (unsigned shift = 0; shift < 64; shift += 16) {
      unsigned chunk = (unsigned)((bits >> shift) & UINT64_C(0xffff));
      if (!chunk && (emitted || bits != 0))
        continue;
      if (!nyr_printf(c->w,
                      emitted ? "\tmovk\t%s, #%u, lsl #%u\n"
                              : "\tmovz\t%s, #%u, lsl #%u\n",
                      reg, chunk, shift))
        return false;
      emitted = true;
    }
    return emitted;
  }
  case NY_SM_I386:
    return nyr_printf(c->w, "\tmovl\t$%" PRId64 ", %s\n", v, reg);
  case NY_SM_RISCV:
    return nyr_printf(c->w, "\tli\t%s, %" PRId64 "\n", reg, v);
  }
  return false;
}

static bool sm_a64_adjust_sp(ny_sm_ctx_t *c, bool subtract) {
  const char *op = subtract ? "sub" : "add";
  if (c->frame_bytes <= 4095)
    return nyr_printf(c->w, "\t%s\tsp, sp, #%d\n", op, c->frame_bytes);
  return sm_movi(c, "x16", c->frame_bytes) &&
         nyr_printf(c->w, "\t%s\tsp, sp, x16\n", op);
}

static bool sm_binop3(ny_sm_ctx_t *c, const char *op) {
  /*
   * t0 = t0 op t1
   */
  switch (c->kind) {
  case NY_SM_ARM:
  case NY_SM_A64:
    return nyr_printf(c->w, "\t%s\t%s, %s, %s\n", op, c->t0, c->t0, c->t1);
  case NY_SM_I386:
    /*
     * op %t1, %t0  (AT&T)
     */
    return nyr_printf(c->w, "\t%sl\t%s, %s\n", op, c->t1, c->t0);
  case NY_SM_RISCV:
    return nyr_printf(c->w, "\t%s\t%s, %s, %s\n", op, c->t0, c->t0, c->t1);
  }
  return false;
}

static bool sm_emit_binop(ny_sm_ctx_t *c, const nyr_inst_t *in,
                          const char *arm, const char *a64, const char *i386,
                          const char *rv) {
  if (!sm_ldv(c, c->t0, in->a) || !sm_ldv(c, c->t1, in->b))
    return false;
  const char *op = c->kind == NY_SM_ARM    ? arm
                   : c->kind == NY_SM_A64  ? a64
                   : c->kind == NY_SM_I386 ? i386
                                           : rv;
  return sm_binop3(c, op) && sm_stv(c, in->dst, c->t0);
}

static bool sm_i386_bitwise64(ny_sm_ctx_t *c, const nyr_inst_t *in,
                              const char *op) {
  if (!nyr_put(c->w, "\tpushl\t%ebx\n") || !sm_ldv(c, "%eax", in->a) ||
      !sm_ldv(c, "%ecx", in->b) ||
      !nyr_printf(c->w, "\t%sl\t%%ecx, %%eax\n", op) ||
      !sm_stv(c, in->dst, "%eax") || !sm_ld(c, "%edx", sm_voff(c, in->a) + 4) ||
      !sm_ld(c, "%ebx", sm_voff(c, in->b) + 4) ||
      !nyr_printf(c->w, "\t%sl\t%%ebx, %%edx\n", op) ||
      !sm_st(c, sm_voff(c, in->dst) + 4, "%edx"))
    return false;
  return nyr_put(c->w, "\tpopl\t%ebx\n");
}

static bool sm_i386_shift64(ny_sm_ctx_t *c, const nyr_inst_t *in,
                            bool arithmetic_right) {
  if (!sm_ldv(c, "%eax", in->a) || !sm_ld(c, "%edx", sm_voff(c, in->a) + 4) ||
      !sm_ldv(c, "%ecx", in->b))
    return false;
  size_t serial = c->bounds_serial++;
  char large[192];
  char done[192];
  snprintf(large, sizeof(large), "%s_i64_shift_large_%zu", c->epilogue, serial);
  snprintf(done, sizeof(done), "%s_i64_shift_done_%zu", c->epilogue, serial);
  if (!nyr_put(c->w, "\tandl\t$63, %ecx\n\tcmpl\t$32, %ecx\n") ||
      !nyr_printf(c->w, "\tjae\t%s\n", large))
    return false;
  if (arithmetic_right) {
    if (!nyr_put(c->w, "\tshrdl\t%cl, %edx, %eax\n\tsarl\t%cl, %edx\n"))
      return false;
  } else if (!nyr_put(c->w, "\tshldl\t%cl, %eax, %edx\n"
                            "\tshll\t%cl, %eax\n")) {
    return false;
  }
  if (!nyr_printf(c->w, "\tjmp\t%s\n%s:\n\tsubl\t$32, %%ecx\n", done, large))
    return false;
  if (arithmetic_right) {
    if (!nyr_put(c->w, "\tmovl\t%edx, %eax\n\tsarl\t%cl, %eax\n"
                       "\tsarl\t$31, %edx\n"))
      return false;
  } else if (!nyr_put(c->w, "\tmovl\t%eax, %edx\n\tshll\t%cl, %edx\n"
                            "\txorl\t%eax, %eax\n")) {
    return false;
  }
  return nyr_printf(c->w, "%s:\n", done) && sm_stv(c, in->dst, "%eax") &&
         sm_st(c, sm_voff(c, in->dst) + 4, "%edx");
}

static bool sm_i386_addsub64(ny_sm_ctx_t *c, const nyr_inst_t *in,
                             bool subtract) {
  if (!sm_ldv(c, "%eax", in->a) || !sm_ldv(c, "%ecx", in->b) ||
      !nyr_printf(c->w, "\t%sl\t%%ecx, %%eax\n", subtract ? "sub" : "add") ||
      !sm_stv(c, in->dst, "%eax") || !sm_ld(c, "%edx", sm_voff(c, in->a) + 4) ||
      !sm_ld(c, "%ecx", sm_voff(c, in->b) + 4) ||
      !nyr_printf(c->w, "\t%sl\t%%ecx, %%edx\n", subtract ? "sbb" : "adc") ||
      !sm_st(c, sm_voff(c, in->dst) + 4, "%edx"))
    return false;
  return true;
}

static bool sm_i386_mul64(ny_sm_ctx_t *c, const nyr_inst_t *in) {
  if (!nyr_put(c->w, "\tpushl\t%ebx\n") || !sm_ldv(c, "%eax", in->a) ||
      !sm_ldv(c, "%ecx", in->b) || !nyr_put(c->w, "\tmull\t%ecx\n") ||
      !sm_stv(c, in->dst, "%eax") || !sm_ld(c, "%ecx", sm_voff(c, in->a) + 4) ||
      !sm_ldv(c, "%ebx", in->b) ||
      !nyr_put(c->w, "\timull\t%ebx, %ecx\n\taddl\t%ecx, %edx\n") ||
      !sm_ld(c, "%ecx", sm_voff(c, in->b) + 4) ||
      !sm_ld(c, "%ebx", sm_voff(c, in->a)) ||
      !nyr_put(c->w, "\timull\t%ebx, %ecx\n\taddl\t%ecx, %edx\n") ||
      !sm_st(c, sm_voff(c, in->dst) + 4, "%edx") ||
      !nyr_put(c->w, "\tpopl\t%ebx\n"))
    return false;
  return true;
}

static bool sm_bounds_check(ny_sm_ctx_t *c, const nyr_inst_t *in) {
  if (!sm_err_val(c, in->b, "bounds offset") ||
      (in->c >= 0 && !sm_err_val(c, in->c, "bounds length")))
    return false;

  char fail[192];
  char ok[192];
  size_t serial = c->bounds_serial++;
  snprintf(fail, sizeof(fail), "%s_bounds_fail_%zu", c->epilogue, serial);
  snprintf(ok, sizeof(ok), "%s_bounds_ok_%zu", c->epilogue, serial);
  bool skip_upper_bound = false;

  if (c->kind == NY_SM_I386) {
    if (!sm_ldv(c, c->t0, in->b) || !nyr_put(c->w, "\tcmpl\t$0, %eax\n") ||
        !nyr_printf(c->w, "\tjl\t%s\n", fail))
      return false;
    if (in->c >= 0) {
      if (!sm_ldv(c, c->t1, in->c) || !nyr_put(c->w, "\tcmpl\t%ecx, %eax\n") ||
          !nyr_printf(c->w, "\tjae\t%s\n", fail))
        return false;
    } else if (in->imm < 0) {
      if (!nyr_printf(c->w, "\tjmp\t%s\n", fail))
        return false;
    } else if ((uint64_t)in->imm <= UINT32_MAX) {
      if (!sm_movi(c, c->t1, in->imm) ||
          !nyr_put(c->w, "\tcmpl\t%ecx, %eax\n") ||
          !nyr_printf(c->w, "\tjae\t%s\n", fail))
        return false;
    }
    return nyr_printf(c->w, "\tjmp\t%s\n%s:\n\tud2\n%s:\n", ok, fail, ok);
  }

  if (!sm_ldv(c, c->t0, in->b))
    return false;
  if (in->c >= 0) {
    if (!sm_ldv(c, c->t1, in->c))
      return false;
  } else if (in->imm < 0) {
    const char *branch = c->kind == NY_SM_RISCV ? "j" : "b";
    if (!nyr_printf(c->w, "\t%s\t%s\n", branch, fail))
      return false;
    goto trap_and_continue;
  } else if (c->kind == NY_SM_ARM && in->imm > INT32_MAX) {
    /*
     * A nonnegative 32-bit offset is always below this bound.
     */
    skip_upper_bound = true;
  } else if (!sm_movi(c, c->t1, in->imm)) {
    return false;
  }

  switch (c->kind) {
  case NY_SM_ARM:
    if (!nyr_put(c->w, "\tcmp\tr0, #0\n") ||
        !nyr_printf(c->w, "\tblt\t%s\n", fail) ||
        (!skip_upper_bound && (!nyr_put(c->w, "\tcmp\tr0, r1\n") ||
                               !nyr_printf(c->w, "\tbhs\t%s\n", fail))))
      return false;
    break;
  case NY_SM_A64:
    if (!nyr_put(c->w, "\tcmp\tx0, #0\n") ||
        !nyr_printf(c->w, "\tb.lt\t%s\n", fail) ||
        (!skip_upper_bound && (!nyr_put(c->w, "\tcmp\tx0, x1\n") ||
                               !nyr_printf(c->w, "\tb.hs\t%s\n", fail))))
      return false;
    break;
  case NY_SM_RISCV:
    if (!nyr_printf(c->w, "\tblt\ta0, zero, %s\n", fail) ||
        (!skip_upper_bound && !nyr_printf(c->w, "\tbgeu\ta0, a1, %s\n", fail)))
      return false;
    break;
  case NY_SM_I386:
    return false;
  }
  const char *branch = c->kind == NY_SM_RISCV  ? "j"
                       : c->kind == NY_SM_I386 ? "jmp"
                                               : "b";
  if (!nyr_printf(c->w, "\t%s\t%s\n", branch, ok))
    return false;

trap_and_continue:
  if (!nyr_printf(c->w, "%s:\n", fail))
    return false;
  switch (c->kind) {
  case NY_SM_ARM:
    if (!nyr_put(c->w, "\tudf\t#0\n"))
      return false;
    break;
  case NY_SM_A64:
    if (!nyr_put(c->w, "\tbrk\t#0\n"))
      return false;
    break;
  case NY_SM_I386:
    if (!nyr_put(c->w, "\tud2\n"))
      return false;
    break;
  case NY_SM_RISCV:
    if (!nyr_put(c->w, "\tebreak\n"))
      return false;
    break;
  }
  return nyr_printf(c->w, "%s:\n", ok);
}

static const char *sm_cond(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "eq";
  case NYIR_CMP_NE:
    return "ne";
  case NYIR_CMP_LT:
    return "lt";
  case NYIR_CMP_LE:
    return "le";
  case NYIR_CMP_GT:
    return "gt";
  case NYIR_CMP_GE:
    return "ge";
  }
  return "eq";
}

static bool sm_arm_cmp64(ny_sm_ctx_t *c, const nyr_inst_t *in) {
  if (!sm_err_val(c, in->a, "left operand") ||
      !sm_err_val(c, in->b, "right operand") ||
      !sm_err_val(c, in->dst, "destination"))
    return false;
  size_t serial = c->bounds_serial++;
  char low[192], done[192];
  snprintf(low, sizeof(low), "%s_i64_cmp_low_%zu", c->epilogue, serial);
  snprintf(done, sizeof(done), "%s_i64_cmp_done_%zu", c->epilogue, serial);
  if (!sm_ld(c, "r2", sm_vhi_off(c, in->a)) ||
      !sm_ld(c, "r3", sm_vhi_off(c, in->b)) ||
      !nyr_put(c->w, "\tcmp\tr2, r3\n"))
    return false;
  if (in->cmp == NYIR_CMP_EQ || in->cmp == NYIR_CMP_NE) {
    const char *set = in->cmp == NYIR_CMP_EQ ? "eq" : "ne";
    if (!nyr_printf(c->w,
                    "\tbeq\t%s\n\tmov\tr0, #%d\n\tb\t%s\n"
                    "%s:\n",
                    low, in->cmp == NYIR_CMP_NE, done, low) ||
        !sm_ld(c, "r2", sm_voff(c, in->a)) ||
        !sm_ld(c, "r3", sm_voff(c, in->b)) ||
        !nyr_put(c->w, "\tcmp\tr2, r3\n\tmov\tr0, #0\n") ||
        !nyr_printf(c->w, "\tmov%s\tr0, #1\n", set))
      return false;
  } else {
    const char *high_cond =
        in->cmp == NYIR_CMP_LT || in->cmp == NYIR_CMP_LE ? "lt" : "gt";
    const char *low_cond = in->cmp == NYIR_CMP_LT   ? "lo"
                           : in->cmp == NYIR_CMP_LE ? "ls"
                           : in->cmp == NYIR_CMP_GT ? "hi"
                                                    : "hs";
    if (!nyr_printf(c->w,
                    "\tbeq\t%s\n\tmov\tr0, #0\n\tmov%s\tr0, #1\n"
                    "\tb\t%s\n%s:\n",
                    low, high_cond, done, low) ||
        !sm_ld(c, "r2", sm_voff(c, in->a)) ||
        !sm_ld(c, "r3", sm_voff(c, in->b)) ||
        !nyr_put(c->w, "\tcmp\tr2, r3\n\tmov\tr0, #0\n") ||
        !nyr_printf(c->w, "\tmov%s\tr0, #1\n", low_cond))
      return false;
  }
  return nyr_printf(c->w, "%s:\n\tmov\tr1, #0\n", done) &&
         sm_st(c, sm_voff(c, in->dst), "r0") &&
         sm_st(c, sm_vhi_off(c, in->dst), "r1");
}

static bool sm_arm_bin64(ny_sm_ctx_t *c, const nyr_inst_t *in,
                         const char *low_op, const char *high_op) {
  if (!sm_err_val(c, in->a, "left operand") ||
      !sm_err_val(c, in->b, "right operand") ||
      !sm_err_val(c, in->dst, "destination") ||
      !sm_ld(c, "r0", sm_voff(c, in->a)) ||
      !sm_ld(c, "r1", sm_vhi_off(c, in->a)) ||
      !sm_ld(c, "r2", sm_voff(c, in->b)) ||
      !sm_ld(c, "r3", sm_vhi_off(c, in->b)))
    return false;
  if (strcmp(low_op, "adds") == 0) {
    if (!nyr_put(c->w, "\tadds\tr0, r0, r2\n\tadc\tr1, r1, r3\n"))
      return false;
  } else if (strcmp(low_op, "subs") == 0) {
    if (!nyr_put(c->w, "\tsubs\tr0, r0, r2\n\tsbc\tr1, r1, r3\n"))
      return false;
  } else if (!nyr_printf(c->w, "\t%s\tr0, r0, r2\n\t%s\tr1, r1, r3\n", low_op,
                         high_op)) {
    return false;
  }
  return sm_st(c, sm_voff(c, in->dst), "r0") &&
         sm_st(c, sm_vhi_off(c, in->dst), "r1");
}

static bool sm_arm_mul64(ny_sm_ctx_t *c, const nyr_inst_t *in) {
  if (!sm_err_val(c, in->a, "left operand") ||
      !sm_err_val(c, in->b, "right operand") ||
      !sm_err_val(c, in->dst, "destination") ||
      !sm_ld(c, "r0", sm_voff(c, in->a)) ||
      !sm_ld(c, "r1", sm_vhi_off(c, in->a)) ||
      !sm_ld(c, "r2", sm_voff(c, in->b)) ||
      !sm_ld(c, "r3", sm_vhi_off(c, in->b)) ||
      !nyr_put(c->w, "\tmul\tr12, r1, r2\n"
                     "\tmul\tr1, r0, r3\n"
                     "\tadd\tr12, r12, r1\n"
                     "\tumull\tr0, r1, r0, r2\n"
                     "\tadd\tr1, r1, r12\n") ||
      !sm_st(c, sm_voff(c, in->dst), "r0"))
    return false;
  return sm_st(c, sm_vhi_off(c, in->dst), "r1");
}

static bool sm_arm_shift64(ny_sm_ctx_t *c, const nyr_inst_t *in,
                           bool arithmetic_right) {
  if (!sm_err_val(c, in->a, "shift value") ||
      !sm_err_val(c, in->b, "shift count") ||
      !sm_err_val(c, in->dst, "shift destination"))
    return false;
  size_t serial = c->bounds_serial++;
  char large[192], done[192];
  snprintf(large, sizeof(large), "%s_i64_shift_large_%zu", c->epilogue, serial);
  snprintf(done, sizeof(done), "%s_i64_shift_done_%zu", c->epilogue, serial);
  if (!sm_ld(c, "r0", sm_voff(c, in->a)) ||
      !sm_ld(c, "r1", sm_vhi_off(c, in->a)) ||
      !sm_ld(c, "r2", sm_voff(c, in->b)) ||
      !nyr_put(c->w, "\tand\tr2, r2, #63\n\tcmp\tr2, #32\n") ||
      !nyr_printf(c->w, "\tbhs\t%s\n\trsb\tr3, r2, #32\n", large))
    return false;
  if (arithmetic_right) {
    if (!nyr_put(c->w, "\tmov\tr12, r1, asr r2\n"
                       "\tmov\tr0, r0, lsr r2\n"
                       "\torr\tr0, r0, r1, lsl r3\n"
                       "\tmov\tr1, r12\n"))
      return false;
  } else if (!nyr_put(c->w, "\tmov\tr12, r1, lsl r2\n"
                            "\torr\tr12, r12, r0, lsr r3\n"
                            "\tmov\tr0, r0, lsl r2\n"
                            "\tmov\tr1, r12\n")) {
    return false;
  }
  if (!nyr_printf(c->w, "\tb\t%s\n%s:\n\tsub\tr2, r2, #32\n", done, large))
    return false;
  if (arithmetic_right) {
    if (!nyr_put(c->w, "\tasr\tr0, r1, r2\n\tasr\tr1, r1, #31\n"))
      return false;
  } else if (!nyr_put(c->w, "\tlsl\tr1, r0, r2\n\tmov\tr0, #0\n")) {
    return false;
  }
  return nyr_printf(c->w, "%s:\n", done) &&
         sm_st(c, sm_voff(c, in->dst), "r0") &&
         sm_st(c, sm_vhi_off(c, in->dst), "r1");
}

static bool sm_arm_divmod64(ny_sm_ctx_t *c, const nyr_inst_t *in,
                            bool remainder) {
  if (!sm_err_val(c, in->a, "dividend") || !sm_err_val(c, in->b, "divisor") ||
      !sm_err_val(c, in->dst, "divmod destination"))
    return false;
  const int values =
      c->nyir && c->nyir->next_value > 0 ? c->nyir->next_value : 0;
  size_t serial = c->divmod_serial++;
  const int base =
      (2 * values + 2 * c->max_local_slot + (int)(serial * 10)) * c->word;
  const int qlo = base;
  const int qhi = base + c->word;
  const int dlo = base + 2 * c->word;
  const int dhi = base + 3 * c->word;
  const int rlo = base + 4 * c->word;
  const int rhi = base + 5 * c->word;
  const int sign_a = base + 6 * c->word;
  const int sign_b = base + 7 * c->word;
  const int count = base + 8 * c->word;
  const int carry = base + 9 * c->word;
  char loop[192], subtract[192], skip_subtract[192], done[192];
  char sign_a_done[192], sign_b_done[192], sign_done[192], zero[192];
  char nonzero[192], exit[192];
  snprintf(loop, sizeof(loop), "%s_i64_div_loop_%zu", c->epilogue, serial);
  snprintf(subtract, sizeof(subtract), "%s_i64_div_sub_%zu", c->epilogue,
           serial);
  snprintf(skip_subtract, sizeof(skip_subtract), "%s_i64_div_skip_%zu",
           c->epilogue, serial);
  snprintf(done, sizeof(done), "%s_i64_div_done_%zu", c->epilogue, serial);
  snprintf(sign_a_done, sizeof(sign_a_done), "%s_i64_div_sign_a_%zu",
           c->epilogue, serial);
  snprintf(sign_b_done, sizeof(sign_b_done), "%s_i64_div_sign_b_%zu",
           c->epilogue, serial);
  snprintf(sign_done, sizeof(sign_done), "%s_i64_div_sign_done_%zu",
           c->epilogue, serial);
  snprintf(zero, sizeof(zero), "%s_i64_div_zero_%zu", c->epilogue, serial);
  snprintf(nonzero, sizeof(nonzero), "%s_i64_div_nonzero_%zu", c->epilogue,
           serial);
  snprintf(exit, sizeof(exit), "%s_i64_div_exit_%zu", c->epilogue, serial);

  if (!sm_ld(c, "r0", sm_voff(c, in->a)) ||
      !sm_ld(c, "r1", sm_vhi_off(c, in->a)) ||
      !nyr_put(c->w, "\tcmp\tr1, #0\n\tmovlt\tr2, #1\n\tmovge\tr2, #0\n") ||
      !sm_st(c, sign_a, "r2") ||
      !nyr_printf(c->w,
                  "\tbge\t%s\n\trsbs\tr0, r0, #0\n\trsc\tr1, r1, #0\n"
                  "%s:\n",
                  sign_a_done, sign_a_done) ||
      !sm_st(c, qlo, "r0") || !sm_st(c, qhi, "r1") ||
      !sm_ld(c, "r0", sm_voff(c, in->b)) ||
      !sm_ld(c, "r1", sm_vhi_off(c, in->b)) ||
      !nyr_put(c->w, "\tcmp\tr1, #0\n\tmovlt\tr2, #1\n\tmovge\tr2, #0\n") ||
      !sm_st(c, sign_b, "r2") ||
      !nyr_printf(c->w,
                  "\tbge\t%s\n\trsbs\tr0, r0, #0\n\trsc\tr1, r1, #0\n"
                  "%s:\n",
                  sign_b_done, sign_b_done) ||
      !sm_st(c, dlo, "r0") || !sm_st(c, dhi, "r1") ||
      !nyr_put(c->w, "\tcmp\tr1, #0\n") ||
      !nyr_printf(c->w, "\tbne\t%s\n", nonzero) || !sm_ld(c, "r0", dlo) ||
      !nyr_put(c->w, "\tcmp\tr0, #0\n") ||
      !nyr_printf(c->w, "\tbeq\t%s\n%s:\n", zero, nonzero) ||
      !sm_movi(c, "r0", 0) || !sm_st(c, rlo, "r0") || !sm_st(c, rhi, "r0") ||
      !sm_movi(c, "r0", 64) || !sm_st(c, count, "r0") ||
      !nyr_printf(c->w, "%s:\n", loop))
    return false;

  /*
   * Shift the next dividend bit into a 65-bit restoring remainder.
   */
  if (!sm_ld(c, "r0", qhi) || !sm_ld(c, "r1", qlo) ||
      !nyr_put(c->w, "\tlsr\tr12, r0, #31\n") || !sm_st(c, carry, "r12") ||
      !nyr_put(c->w, "\tlsr\tr2, r1, #31\n\tlsl\tr0, r0, #1\n"
                     "\torr\tr0, r0, r2\n\tlsl\tr1, r1, #1\n") ||
      !sm_st(c, qlo, "r1") || !sm_st(c, qhi, "r0") || !sm_ld(c, "r0", rhi) ||
      !sm_ld(c, "r1", rlo) ||
      !nyr_put(c->w, "\tlsr\tr12, r0, #31\n\tlsl\tr0, r0, #1\n"
                     "\tlsl\tr1, r1, #1\n") ||
      !sm_ld(c, "r2", carry) ||
      !nyr_put(c->w, "\torr\tr1, r1, r2\n\torr\tr0, r0, r12\n") ||
      !sm_st(c, rlo, "r1") || !sm_st(c, rhi, "r0") || !sm_st(c, carry, "r12") ||
      !sm_ld(c, "r0", carry) || !nyr_put(c->w, "\tcmp\tr0, #0\n") ||
      !nyr_printf(c->w, "\tbne\t%s\n", subtract) || !sm_ld(c, "r0", rhi) ||
      !sm_ld(c, "r1", dhi) || !nyr_put(c->w, "\tcmp\tr0, r1\n") ||
      !nyr_printf(c->w, "\tbhi\t%s\n\tblo\t%s\n", subtract, skip_subtract) ||
      !sm_ld(c, "r0", rlo) || !sm_ld(c, "r1", dlo) ||
      !nyr_put(c->w, "\tcmp\tr0, r1\n") ||
      !nyr_printf(c->w, "\tbhs\t%s\n\tb\t%s\n%s:\n", subtract, skip_subtract,
                  subtract) ||
      !sm_ld(c, "r0", rlo) || !sm_ld(c, "r1", dlo) ||
      !nyr_put(c->w, "\tsubs\tr0, r0, r1\n") || !sm_ld(c, "r2", rhi) ||
      !sm_ld(c, "r3", dhi) || !nyr_put(c->w, "\tsbc\tr2, r2, r3\n") ||
      !sm_st(c, rlo, "r0") || !sm_st(c, rhi, "r2") || !sm_ld(c, "r0", qlo) ||
      !nyr_put(c->w, "\torr\tr0, r0, #1\n") || !sm_st(c, qlo, "r0") ||
      !nyr_printf(c->w, "%s:\n", skip_subtract) || !sm_ld(c, "r0", count) ||
      !nyr_put(c->w, "\tsubs\tr0, r0, #1\n") || !sm_st(c, count, "r0") ||
      !nyr_printf(c->w, "\tbne\t%s\n", loop))
    return false;

  if (!nyr_printf(c->w, "%s:\n", done))
    return false;
  if (remainder) {
    if (!sm_ld(c, "r0", sign_a))
      return false;
  } else if (!sm_ld(c, "r0", sign_a) || !sm_ld(c, "r1", sign_b) ||
             !nyr_put(c->w, "\teor\tr0, r0, r1\n")) {
    return false;
  }
  if (!nyr_put(c->w, "\tcmp\tr0, #0\n") ||
      !nyr_printf(c->w, "\tbeq\t%s\n", sign_done) ||
      !sm_ld(c, "r0", remainder ? rlo : qlo) ||
      !sm_ld(c, "r1", remainder ? rhi : qhi) ||
      !nyr_put(c->w, "\trsbs\tr0, r0, #0\n\trsc\tr1, r1, #0\n") ||
      !sm_st(c, remainder ? rlo : qlo, "r0") ||
      !sm_st(c, remainder ? rhi : qhi, "r1") ||
      !nyr_printf(c->w, "%s:\n", sign_done) ||
      !sm_ld(c, "r0", remainder ? rlo : qlo) ||
      !sm_ld(c, "r1", remainder ? rhi : qhi) ||
      !sm_st(c, sm_voff(c, in->dst), "r0") ||
      !sm_st(c, sm_vhi_off(c, in->dst), "r1") ||
      !nyr_printf(c->w, "\tb\t%s\n%s:\n\tudf\t#0\n%s:\n", exit, zero, exit))
    return false;
  return true;
}

static bool sm_cmp(ny_sm_ctx_t *c, const nyr_inst_t *in) {
  if (c->kind == NY_SM_ARM)
    return sm_arm_cmp64(c, in);
  if (!sm_ldv(c, c->t0, in->a) || !sm_ldv(c, c->t1, in->b))
    return false;
  const char *cond = sm_cond(in->cmp);
  switch (c->kind) {
  case NY_SM_ARM:
    return false;
  case NY_SM_A64:
    return nyr_put(c->w, "\tcmp\tx0, x1\n\tmov\tx0, #0\n") &&
           nyr_printf(c->w, "\tcset\tx0, %s\n", cond) &&
           sm_stv(c, in->dst, c->t0);
  case NY_SM_I386: {
    if (in->cmp == NYIR_CMP_EQ || in->cmp == NYIR_CMP_NE) {
      const char *set = in->cmp == NYIR_CMP_EQ ? "e" : "ne";
      const char *combine = in->cmp == NYIR_CMP_EQ ? "andb" : "orb";
      return nyr_put(c->w, "\tpushl\t%ebx\n") &&
             sm_ld(c, "%edx", sm_voff(c, in->a) + 4) &&
             sm_ld(c, "%ebx", sm_voff(c, in->b) + 4) &&
             nyr_put(c->w, "\tcmpl\t%ebx, %edx\n") &&
             nyr_printf(c->w, "\tset%s\t%%dl\n", set) &&
             nyr_put(c->w, "\tcmpl\t%ecx, %eax\n") &&
             nyr_printf(c->w, "\tset%s\t%%al\n\t%s\t%%dl, %%al\n", set,
                        combine) &&
             nyr_put(c->w, "\tmovzbl\t%al, %eax\n\tpopl\t%ebx\n") &&
             sm_stv(c, in->dst, c->t0) &&
             sm_i386_store_u32(c, sm_voff(c, in->dst) + 4, 0);
    }
    const char *high_set =
        in->cmp == NYIR_CMP_LT || in->cmp == NYIR_CMP_LE ? "l" : "g";
    const char *low_set = in->cmp == NYIR_CMP_LT   ? "b"
                          : in->cmp == NYIR_CMP_LE ? "be"
                          : in->cmp == NYIR_CMP_GT ? "a"
                                                   : "ae";
    return nyr_put(c->w, "\tpushl\t%ebx\n") &&
           sm_ld(c, "%edx", sm_voff(c, in->a) + 4) &&
           sm_ld(c, "%ebx", sm_voff(c, in->b) + 4) &&
           nyr_put(c->w, "\tcmpl\t%ebx, %edx\n") &&
           nyr_printf(c->w, "\tset%s\t%%dl\n\tsete\t%%bl\n", high_set) &&
           nyr_put(c->w, "\tcmpl\t%ecx, %eax\n") &&
           nyr_printf(c->w,
                      "\tset%s\t%%al\n\tandb\t%%bl, %%al\n"
                      "\torb\t%%dl, %%al\n\tmovzbl\t%%al, %%eax\n",
                      low_set) &&
           nyr_put(c->w, "\tpopl\t%ebx\n") && sm_stv(c, in->dst, c->t0) &&
           sm_i386_store_u32(c, sm_voff(c, in->dst) + 4, 0);
  }
  case NY_SM_RISCV: {
    /*
     * slt-based materialize
     */
    if (in->cmp == NYIR_CMP_LT)
      return nyr_put(c->w, "\tslt\ta0, a0, a1\n") && sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_GT)
      return nyr_put(c->w, "\tslt\ta0, a1, a0\n") && sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_EQ)
      return nyr_put(c->w, "\txor\ta0, a0, a1\n\tseqz\ta0, a0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_NE)
      return nyr_put(c->w, "\txor\ta0, a0, a1\n\tsnez\ta0, a0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (in->cmp == NYIR_CMP_LE)
      return nyr_put(c->w, "\tslt\ta0, a1, a0\n\txori\ta0, a0, 1\n") &&
             sm_stv(c, in->dst, c->t0);
    return nyr_put(c->w, "\tslt\ta0, a0, a1\n\txori\ta0, a0, 1\n") &&
           sm_stv(c, in->dst, c->t0);
  }
  }
  return false;
}

static bool sm_value_unused_from(const ny_sm_ctx_t *c, int value,
                                 size_t start) {
  for (size_t i = start; i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t op = 0; op < sizeof(operands) / sizeof(operands[0]); ++op)
      if (operands[op] == value)
        return false;
    for (size_t arg = 0; arg < in->extra_args_len; ++arg)
      if (in->extra_args[arg] == value)
        return false;
    for (size_t phi = 0; phi < in->phi_incoming_len; ++phi)
      if (in->phi_incoming[phi].value == value)
        return false;
  }
  return true;
}

static bool sm_value_is_static_string_depth(const ny_sm_ctx_t *c, int value,
                                            size_t before, unsigned depth) {
  if (depth > 8)
    return false;
  for (size_t i = before; i > 0; --i) {
    const nyr_inst_t *in = &c->nyir->data[i - 1];
    if (in->dst != value)
      continue;
    if (in->op == NYIR_ADDR_SYMBOL && in->symbol &&
        strncmp(in->symbol, "ny_str_", 7) == 0)
      return nyr_strtab_get(in->symbol, NULL) != NULL;
    if (in->op == NYIR_CALL && in->symbol &&
        strcmp(in->symbol, "rt_cstr_to_str") == 0 &&
        ((in->flags & NYIR_INST_F_EXTERN) ||
         ny_native_runtime_abi_decl(in->symbol) != NULL)) {
      int args[NYIR_CALL_MAX_ARGS];
      int argc = 0;
      return nyr_call_args(in, c->nyir->next_value, args, NYIR_CALL_MAX_ARGS,
                            &argc, NULL, 0) &&
             argc == 1 &&
             sm_value_is_static_string_depth(c, args[0], i - 1, depth + 1);
    }
    return false;
  }
  return false;
}

static bool sm_value_is_static_string(const ny_sm_ctx_t *c, int value,
                                      size_t before) {
  return sm_value_is_static_string_depth(c, value, before, 0);
}

static bool sm_native_panic_alloc_pair(const ny_sm_ctx_t *c,
                                       const nyr_inst_t *in) {
  if (!c ||
      (c->kind != NY_SM_RISCV && c->kind != NY_SM_ARM && c->kind != NY_SM_A64 &&
       c->kind != NY_SM_I386) ||
      !c->nyir || !in || in->op != NYIR_CALL || !in->symbol ||
      strcmp(in->symbol, "rt_alloc_string") != 0 ||
      (!(in->flags & NYIR_INST_F_EXTERN) &&
       ny_native_runtime_abi_decl(in->symbol) == NULL) ||
      in->dst < 0 || in < c->nyir->data ||
      (size_t)(in - c->nyir->data) + 1 >= c->nyir->len)
    return false;
  size_t index = (size_t)(in - c->nyir->data);
  int alloc_args[NYIR_CALL_MAX_ARGS];
  int alloc_argc = 0;
  const nyr_inst_t *panic = &c->nyir->data[index + 1];
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  return nyr_call_args(in, c->nyir->next_value, alloc_args, NYIR_CALL_MAX_ARGS,
                        &alloc_argc, NULL, 0) &&
         alloc_argc == 1 &&
         sm_value_is_static_string(c, alloc_args[0], index) &&
         panic->op == NYIR_CALL && panic->symbol &&
         strcmp(panic->symbol, "rt_panic") == 0 &&
         ((panic->flags & NYIR_INST_F_EXTERN) ||
          ny_native_runtime_abi_decl(panic->symbol) != NULL) &&
         nyr_call_args(panic, c->nyir->next_value, args, NYIR_CALL_MAX_ARGS,
                        &argc, NULL, 0) &&
         argc == 1 && args[0] == in->dst &&
         sm_value_unused_from(c, in->dst, index + 2);
}

static bool sm_call(ny_sm_ctx_t *c, const nyr_inst_t *in) {
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  int a64_stack_bytes = 0;
  if (!nyr_call_args(in, c->nyir->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                      c->err, c->err_len))
    return false;
  if (c->kind != NY_SM_I386 && c->kind != NY_SM_A64 && c->kind != NY_SM_ARM &&
      (size_t)argc > c->target->gp_arg_reg_count) {
    nyr_set_error(c->err, c->err_len, "%s NYIR emit: %d args exceed %zu regs",
                  c->pretty, argc, c->target->gp_arg_reg_count);
    return false;
  }
  const char *sym = in->symbol ? in->symbol : "";
  if (strcmp(sym, "rt_cstr_to_str") == 0 && in->dst >= 0 &&
      ((in->flags & NYIR_INST_F_EXTERN) ||
       ny_native_runtime_abi_decl(sym) != NULL) &&
      argc == 1 &&
      sm_value_is_static_string(c, args[0], (size_t)(in - c->nyir->data)))
    return sm_ldv(c, c->t0, args[0]) && sm_stv(c, in->dst, c->t0);
  if (sm_native_panic_alloc_pair(c, in))
    return true;
  if (strcmp(sym, "rt_panic") == 0 && c->nyir && in >= c->nyir->data + 1 &&
      sm_native_panic_alloc_pair(c, in - 1)) {
    size_t index = (size_t)(in - c->nyir->data);
    switch (c->kind) {
    case NY_SM_ARM:
      return nyr_printf(c->w,
                        "\tudf\t#0\n%s_ny_panic_spin_%zu:\n"
                        "\tb\t%s_ny_panic_spin_%zu\n",
                        c->epilogue, index, c->epilogue, index);
    case NY_SM_A64:
      return nyr_printf(c->w,
                        "\tbrk\t#0\n%s_ny_panic_spin_%zu:\n"
                        "\tb\t%s_ny_panic_spin_%zu\n",
                        c->epilogue, index, c->epilogue, index);
    case NY_SM_I386:
      return nyr_printf(c->w,
                        "\tud2\n%s_ny_panic_spin_%zu:\n"
                        "\tjmp\t%s_ny_panic_spin_%zu\n",
                        c->epilogue, index, c->epilogue, index);
    case NY_SM_RISCV:
      return nyr_printf(c->w,
                        "\tebreak\n%s_ny_panic_spin_%zu:\n"
                        "\tj\t%s_ny_panic_spin_%zu\n",
                        c->epilogue, index, c->epilogue, index);
    }
    return false;
  }
  if (c->kind == NY_SM_I386) {
    /*
     * push right-to-left, call, pop
     */
    for (int i = argc - 1; i >= 0; --i) {
      if (!sm_ldv(c, c->t0, args[i]) || !nyr_put(c->w, "\tpushl\t%eax\n"))
        return false;
    }
  } else if (c->kind == NY_SM_A64) {
    size_t gp = 0;
    size_t fp = 0;
    size_t stack_offsets[NYIR_CALL_MAX_ARGS];
    size_t stack_count = 0;
    for (int i = 0; i < argc; ++i)
      stack_offsets[i] = SIZE_MAX;
    for (int i = 0; i < argc; ++i) {
      nyr_scalar_class_t cls = nyr_call_arg_class_get(in, (size_t)i);
      bool is_f32 = cls == NYIR_SCALAR_F32;
      bool is_f64 = cls == NYIR_SCALAR_F64;
      if (cls == NYIR_SCALAR_AGGREGATE) {
        nyr_set_error(c->err, c->err_len,
                      "AArch64 NYIR emit: aggregate call arguments are "
                      "unsupported by the portable stack emitter");
        return false;
      }
      size_t reg = is_f32 || is_f64 ? fp++ : gp++;
      if (reg >= 8)
        stack_offsets[i] = stack_count++ * 8;
    }
    a64_stack_bytes = (int)((stack_count * 8 + 15) & ~(size_t)15);
    if (a64_stack_bytes &&
        !nyr_printf(c->w, "\tsub\tsp, sp, #%d\n", a64_stack_bytes))
      return false;
    gp = 0;
    fp = 0;
    for (int i = 0; i < argc; ++i) {
      nyr_scalar_class_t cls = nyr_call_arg_class_get(in, (size_t)i);
      bool is_f32 = cls == NYIR_SCALAR_F32;
      bool is_f64 = cls == NYIR_SCALAR_F64;
      size_t reg = is_f32 || is_f64 ? fp++ : gp++;
      int src = sm_voff(c, args[i]) + a64_stack_bytes;
      if (stack_offsets[i] != SIZE_MAX) {
        if (!nyr_printf(c->w,
                        "\tldr\tx9, [sp, #%d]\n"
                        "\tstr\tx9, [sp, #%zu]\n",
                        src, stack_offsets[i]))
          return false;
      } else if (is_f32 || is_f64) {
        if (!nyr_printf(c->w, "\tldr\t%s%zu, [sp, #%d]\n", is_f32 ? "s" : "d",
                        reg, src))
          return false;
      } else if (!nyr_printf(c->w, "\tldr\tx%zu, [sp, #%d]\n", reg, src)) {
        return false;
      }
    }
  } else if (c->kind == NY_SM_ARM) {
    sm_arm_arg_state_t state = {0};
    sm_arm_arg_location_t locations[NYIR_CALL_MAX_ARGS];
    for (int i = 0; i < argc; ++i) {
      nyr_scalar_class_t cls = nyr_call_arg_class_get(in, (size_t)i);
      sm_arm_assign_arg(&state, cls == NYIR_SCALAR_F64 ? 2u : 1u,
                        &locations[i]);
    }
    a64_stack_bytes = (int)((state.stack_bytes + 7u) & ~(size_t)7u);
    if (a64_stack_bytes &&
        !nyr_printf(c->w, "\tsub\tsp, sp, #%d\n", a64_stack_bytes))
      return false;
    for (int i = 0; i < argc; ++i) {
      const sm_arm_arg_location_t *location = &locations[i];
      int source_low = sm_voff(c, args[i]) + a64_stack_bytes;
      if (location->reg != SIZE_MAX) {
        if (!sm_ld(c, "r12", source_low) ||
            !nyr_printf(c->w, "\tmov\tr%zu, r12\n", location->reg))
          return false;
        if (location->words == 2 &&
            (!sm_ld(c, "r12", sm_vhi_off(c, args[i]) + a64_stack_bytes) ||
             !nyr_printf(c->w, "\tmov\tr%zu, r12\n", location->reg + 1u)))
          return false;
      } else {
        if (location->stack_offset > INT_MAX || !sm_ld(c, "r12", source_low) ||
            !sm_st(c, (int)location->stack_offset, "r12"))
          return false;
        if (location->words == 2 &&
            (location->stack_offset + 4u > INT_MAX ||
             !sm_ld(c, "r12", sm_vhi_off(c, args[i]) + a64_stack_bytes) ||
             !sm_st(c, (int)(location->stack_offset + 4u), "r12")))
          return false;
      }
    }
    if (ny_dbg_enabled & NY_DBG_ABI) {
      for (int i = 0; i < argc && (size_t)i < c->target->gp_arg_reg_count;
           ++i) {
        char detail[128];
        const sm_arm_arg_location_t *location = &locations[i];
        if (location->reg == SIZE_MAX)
          continue;
        snprintf(detail, sizeof(detail), "%s arg %d class=%d",
                 in->symbol ? in->symbol : "<indirect>", i,
                 (int)nyr_call_arg_class_get(in, (size_t)i));
        ny_dbg_abi_classify("call-arg", c->target->gp_arg_regs[location->reg],
                            detail);
      }
    }
  } else {
    for (int i = 0; i < argc; ++i)
      if (!sm_ldv(c, c->target->gp_arg_regs[i], args[i]))
        return false;
    /*
     * Call-site ABI placement (-vvvv ABI channel): physical register per
     * argument at the machine-form call.
     */
    if (ny_dbg_enabled & NY_DBG_ABI) {
      for (int i = 0; i < argc && i < 6; ++i) {
        char detail[128];
        snprintf(detail, sizeof(detail), "%s arg %d class=%d",
                 in->symbol ? in->symbol : "<indirect>", i,
                 (int)nyr_call_arg_class_get(in, (size_t)i));
        ny_dbg_abi_classify("call-arg", c->target->gp_arg_regs[i], detail);
      }
    }
  }
  bool ext = (in->flags & NYIR_INST_F_EXTERN) != 0;
  const char *pfx = c->target->symbol_prefix ? c->target->symbol_prefix : "";
  const char *fn = (ext || strncmp(sym, "ny_fn_", 6) == 0 ||
                    ny_native_runtime_abi_decl(sym) != NULL)
                       ? ""
                       : "ny_fn_";
  switch (c->kind) {
  case NY_SM_ARM:
  case NY_SM_A64:
    if (!nyr_printf(c->w, "\tbl\t%s%s%s\n", pfx, fn, sym))
      return false;
    if (a64_stack_bytes &&
        !nyr_printf(c->w, "\tadd\tsp, sp, #%d\n", a64_stack_bytes))
      return false;
    break;
  case NY_SM_I386:
    if (!nyr_printf(c->w, "\tcall\t%s%s%s\n", pfx, fn, sym))
      return false;
    if (argc && !nyr_printf(c->w, "\taddl\t$%d, %%esp\n", argc * 4))
      return false;
    break;
  case NY_SM_RISCV:
    if (!nyr_printf(c->w, "\tcall\t%s%s%s\n", pfx, fn, sym))
      return false;
    break;
  }
  if (in->dst < 0)
    return true;
  if (c->kind == NY_SM_ARM && in->scalar_class == NYIR_SCALAR_F64)
    return sm_st(c, sm_voff(c, in->dst), "r0") &&
           sm_st(c, sm_vhi_off(c, in->dst), "r1");
  if (c->kind == NY_SM_ARM && in->scalar_class == NYIR_SCALAR_F32)
    return sm_st(c, sm_voff(c, in->dst), "r0") &&
           nyr_put(c->w, "\tmov\tr12, #0\n") &&
           sm_st(c, sm_vhi_off(c, in->dst), "r12");
  if (c->kind == NY_SM_A64 && in->scalar_class == NYIR_SCALAR_F64)
    return sm_st(c, sm_voff(c, in->dst), "d0");
  if (c->kind == NY_SM_A64 && in->scalar_class == NYIR_SCALAR_F32)
    return sm_st(c, sm_voff(c, in->dst), "s0");
  return sm_stv(c, in->dst, c->ret);
}

static bool sm_inst(ny_sm_ctx_t *c, const nyr_inst_t *in) {
  switch (in->op) {
  case NYIR_NOP:
    return true;
  case NYIR_COPY_STRUCT:
    if (c->kind != NY_SM_ARM && c->kind != NY_SM_A64 && c->kind != NY_SM_I386 &&
        c->kind != NY_SM_RISCV)
      break;
    if (in->imm < 0) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: negative struct copy size", c->pretty);
      return false;
    }
    if (in->imm == 0)
      return true;
    {
      const size_t serial = c->copy_serial++;
      char loop[192], done[192];
      snprintf(loop, sizeof(loop), "%s_copy_loop_%zu", c->epilogue, serial);
      snprintf(done, sizeof(done), "%s_copy_done_%zu", c->epilogue, serial);
      if (c->kind == NY_SM_ARM || c->kind == NY_SM_I386) {
        if (in->imm > INT32_MAX) {
          nyr_set_error(c->err, c->err_len,
                        "%s NYIR emit: struct copy size exceeds 32-bit "
                        "address space",
                        c->pretty);
          return false;
        }
        if (c->kind == NY_SM_ARM)
          return sm_ldv(c, "r0", in->a) && sm_ldv(c, "r1", in->b) &&
                 sm_movi(c, "r2", in->imm) &&
                 nyr_printf(c->w,
                            "%s:\n"
                            "\tldrb\tr3, [r1], #1\n"
                            "\tstrb\tr3, [r0], #1\n"
                            "\tsubs\tr2, r2, #1\n"
                            "\tbne\t%s\n"
                            "%s:\n",
                            loop, loop, done);
        return sm_ldv(c, "%eax", in->a) && sm_ldv(c, "%ecx", in->b) &&
               sm_movi(c, "%edx", in->imm) &&
               nyr_printf(c->w,
                          "\tpushl\t%%ebx\n"
                          "%s:\n"
                          "\tmovb\t(%%ecx), %%bl\n"
                          "\tmovb\t%%bl, (%%eax)\n"
                          "\tincl\t%%eax\n"
                          "\tincl\t%%ecx\n"
                          "\tdecl\t%%edx\n"
                          "\tjnz\t%s\n"
                          "\tpopl\t%%ebx\n"
                          "%s:\n",
                          loop, loop, done);
      }
      if (c->kind == NY_SM_RISCV)
        return sm_ldv(c, "a0", in->a) && sm_ldv(c, "a1", in->b) &&
               sm_movi(c, "a2", in->imm) &&
               nyr_printf(c->w,
                          "%s:\n"
                          "\tlbu\ta3, 0(a1)\n"
                          "\tsb\ta3, 0(a0)\n"
                          "\taddi\ta0, a0, 1\n"
                          "\taddi\ta1, a1, 1\n"
                          "\taddi\ta2, a2, -1\n"
                          "\tbnez\ta2, %s\n"
                          "%s:\n",
                          loop, loop, done);
      return sm_ldv(c, "x2", in->a) && sm_ldv(c, "x3", in->b) &&
             sm_movi(c, "x4", in->imm) &&
             nyr_printf(c->w,
                        "%s:\n"
                        "\tldrb\tw5, [x3], #1\n"
                        "\tstrb\tw5, [x2], #1\n"
                        "\tsubs\tx4, x4, #1\n"
                        "\tb.ne\t%s\n"
                        "%s:\n",
                        loop, loop, done);
    }
  case NYIR_BOUNDS_CHECK:
    return sm_bounds_check(c, in);
  case NYIR_CONST_I64:
    if (c->kind == NY_SM_I386)
      return sm_err_val(c, in->dst, "destination") &&
             sm_i386_store_u32(c, sm_voff(c, in->dst), (uint32_t)in->imm) &&
             sm_i386_store_u32(c, sm_voff(c, in->dst) + 4,
                               (uint32_t)((uint64_t)in->imm >> 32));
    if (c->kind == NY_SM_ARM)
      return sm_err_val(c, in->dst, "destination") &&
             sm_movi(c, "r0", (int32_t)(uint32_t)in->imm) &&
             sm_st(c, sm_voff(c, in->dst), "r0") &&
             sm_movi(c, "r0", (int32_t)((uint64_t)in->imm >> 32)) &&
             sm_st(c, sm_vhi_off(c, in->dst), "r0");
    return sm_movi(c, c->t0, in->imm) && sm_stv(c, in->dst, c->t0);
  case NYIR_CONST_F64:
    if (c->kind == NY_SM_I386)
      return sm_i386_store_f64(c, in->dst, (uint64_t)in->imm);
    if (c->kind == NY_SM_ARM)
      return sm_err_val(c, in->dst, "destination") &&
             sm_movi(c, "r0", (int32_t)(uint32_t)in->imm) &&
             sm_st(c, sm_voff(c, in->dst), "r0") &&
             sm_movi(c, "r0", (int32_t)((uint64_t)in->imm >> 32)) &&
             sm_st(c, sm_vhi_off(c, in->dst), "r0");
    if (c->kind == NY_SM_A64)
      return sm_movi(c, "x0", in->imm) && sm_stv(c, in->dst, "x0");
    return false;
  case NYIR_CONST_F32:
    if (c->kind == NY_SM_I386)
      return sm_err_val(c, in->dst, "destination") &&
             sm_i386_store_u32(c, sm_voff(c, in->dst), (uint32_t)in->imm);
    if (c->kind == NY_SM_ARM)
      return sm_err_val(c, in->dst, "destination") &&
             sm_movi(c, "r0", (int32_t)(uint32_t)in->imm) &&
             sm_st(c, sm_voff(c, in->dst), "r0") && sm_movi(c, "r0", 0) &&
             sm_st(c, sm_vhi_off(c, in->dst), "r0");
    if (c->kind == NY_SM_A64)
      return sm_movi(c, "x0", (uint32_t)in->imm) && sm_stv(c, in->dst, "x0");
    return false;
  case NYIR_COPY:
    if (c->kind == NY_SM_I386)
      return sm_i386_copy_value(c, in->dst, in->a);
    return sm_ldv(c, c->t0, in->a) && sm_stv(c, in->dst, c->t0);
  case NYIR_ADD_F64:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, false, "fadd");
    return sm_i386_emit_fbinop(c, in, false, "faddp");
  case NYIR_SUB_F64:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, false, "fsub");
    return sm_i386_emit_fbinop(c, in, false, "fsubp");
  case NYIR_MUL_F64:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, false, "fmul");
    return sm_i386_emit_fbinop(c, in, false, "fmulp");
  case NYIR_DIV_F64:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, false, "fdiv");
    return sm_i386_emit_fbinop(c, in, false, "fdivp");
  case NYIR_ADD_F32:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, true, "fadd");
    return sm_i386_emit_fbinop(c, in, true, "faddp");
  case NYIR_SUB_F32:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, true, "fsub");
    return sm_i386_emit_fbinop(c, in, true, "fsubp");
  case NYIR_MUL_F32:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, true, "fmul");
    return sm_i386_emit_fbinop(c, in, true, "fmulp");
  case NYIR_DIV_F32:
    if (c->kind == NY_SM_A64)
      return sm_a64_fbinop(c, in, true, "fdiv");
    return sm_i386_emit_fbinop(c, in, true, "fdivp");
  case NYIR_I64_TO_F64:
  case NYIR_I64_TO_F32:
    if (c->kind == NY_SM_A64)
      return sm_a64_convert(c, in, "x0",
                            in->op == NYIR_I64_TO_F64 ? "scvtf d0, x0"
                                                      : "scvtf s0, x0",
                            in->op == NYIR_I64_TO_F64 ? "d0" : "s0");
    if (c->kind != NY_SM_I386 || !sm_err_val(c, in->a, "value") ||
        !sm_err_val(c, in->dst, "destination"))
      return false;
    return nyr_printf(c->w, "\tfildl\t%d(%%ebp)\n", sm_voff(c, in->a)) &&
           nyr_printf(c->w,
                      in->op == NYIR_I64_TO_F64 ? "\tfstpl\t%d(%%ebp)\n"
                                                : "\tfstps\t%d(%%ebp)\n",
                      sm_voff(c, in->dst));
  case NYIR_F32_TO_F64:
  case NYIR_F64_TO_F32:
    if (c->kind == NY_SM_A64)
      return sm_a64_convert(c, in, in->op == NYIR_F32_TO_F64 ? "s0" : "d0",
                            in->op == NYIR_F32_TO_F64 ? "fcvt d0, s0"
                                                      : "fcvt s0, d0",
                            in->op == NYIR_F32_TO_F64 ? "d0" : "s0");
    if (c->kind != NY_SM_I386 || !sm_err_val(c, in->a, "value") ||
        !sm_err_val(c, in->dst, "destination"))
      return false;
    return nyr_printf(c->w,
                      in->op == NYIR_F32_TO_F64 ? "\tflds\t%d(%%ebp)\n"
                                                : "\tfldl\t%d(%%ebp)\n",
                      sm_voff(c, in->a)) &&
           nyr_printf(c->w,
                      in->op == NYIR_F32_TO_F64 ? "\tfstpl\t%d(%%ebp)\n"
                                                : "\tfstps\t%d(%%ebp)\n",
                      sm_voff(c, in->dst));
  case NYIR_F64_TO_I64:
  case NYIR_F32_TO_I64:
    if (c->kind == NY_SM_A64)
      return sm_a64_float_to_i64(c, in, in->op == NYIR_F32_TO_I64);
    if (c->kind != NY_SM_I386 || !sm_err_val(c, in->a, "value") ||
        !sm_err_val(c, in->dst, "destination"))
      return false;
    return nyr_printf(c->w,
                      in->op == NYIR_F64_TO_I64 ? "\tfldl\t%d(%%ebp)\n"
                                                : "\tflds\t%d(%%ebp)\n",
                      sm_voff(c, in->a)) &&
           nyr_printf(c->w, "\tfistpl\t%d(%%ebp)\n", sm_voff(c, in->dst));
  case NYIR_CMP_F64:
    if (c->kind == NY_SM_A64)
      return sm_a64_cmp_float(c, in, false);
    return sm_i386_cmp_f(c, in, false);
  case NYIR_CMP_F32:
    if (c->kind == NY_SM_A64)
      return sm_a64_cmp_float(c, in, true);
    return sm_i386_cmp_f(c, in, true);
  case NYIR_SQRT_F64:
    if (c->kind == NY_SM_A64) {
      if (!sm_err_val(c, in->a, "value") ||
          !sm_err_val(c, in->dst, "destination"))
        return false;
      return nyr_printf(c->w,
                        "\tldr\td0, [sp, #%d]\n\tfsqrt\td0, d0\n"
                        "\tstr\td0, [sp, #%d]\n",
                        sm_voff(c, in->a), sm_voff(c, in->dst));
    }
    if (c->kind != NY_SM_I386 || !sm_err_val(c, in->a, "value") ||
        !sm_err_val(c, in->dst, "destination"))
      return false;
    return nyr_printf(c->w,
                      "\tfldl\t%d(%%ebp)\n\tfsqrt\n"
                      "\tfstpl\t%d(%%ebp)\n",
                      sm_voff(c, in->a), sm_voff(c, in->dst));
  case NYIR_ADD_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_bin64(c, in, "adds", "adc");
    if (c->kind == NY_SM_I386)
      return sm_i386_addsub64(c, in, false);
    return sm_emit_binop(c, in, "add", "add", "add", "add");
  case NYIR_SUB_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_bin64(c, in, "subs", "sbc");
    if (c->kind == NY_SM_I386)
      return sm_i386_addsub64(c, in, true);
    return sm_emit_binop(c, in, "sub", "sub", "sub", "sub");
  case NYIR_MUL_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_mul64(c, in);
    if (c->kind == NY_SM_I386)
      return sm_i386_mul64(c, in);
    return sm_emit_binop(c, in, "mul", "mul", "imul", "mul");
  case NYIR_DIV_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_divmod64(c, in, false);
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tcltd\n\tidivl\t%ecx\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_RISCV)
      return sm_emit_binop(c, in, "sdiv", "sdiv", "idiv", "div");
    return sm_emit_binop(c, in, "sdiv", "sdiv", "idiv", "div");
  case NYIR_MOD_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_divmod64(c, in, true);
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tcltd\n\tidivl\t%ecx\n\tmovl\t%edx, %eax\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_A64)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tsdiv\tx2, x0, x1\n\tmsub\tx0, x2, x1, x0\n") &&
             sm_stv(c, in->dst, c->t0);
    return sm_emit_binop(c, in, "sdiv", "sdiv", "idiv", "rem");
  case NYIR_AND_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_bin64(c, in, "and", "and");
    if (c->kind == NY_SM_I386)
      return sm_i386_bitwise64(c, in, "and");
    return sm_emit_binop(c, in, "and", "and", "and", "and");
  case NYIR_OR_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_bin64(c, in, "orr", "orr");
    if (c->kind == NY_SM_I386)
      return sm_i386_bitwise64(c, in, "or");
    return sm_emit_binop(c, in, "orr", "orr", "or", "or");
  case NYIR_XOR_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_bin64(c, in, "eor", "eor");
    if (c->kind == NY_SM_I386)
      return sm_i386_bitwise64(c, in, "xor");
    return sm_emit_binop(c, in, "eor", "eor", "xor", "xor");
  case NYIR_BSWAP_I64:
    if (c->kind == NY_SM_A64)
      return sm_ldv(c, c->t0, in->a) && nyr_put(c->w, "\trev\tx0, x0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_ARM)
      return sm_err_val(c, in->a, "byte-swap value") &&
             sm_err_val(c, in->dst, "byte-swap destination") &&
             sm_ld(c, "r0", sm_voff(c, in->a)) &&
             sm_ld(c, "r1", sm_vhi_off(c, in->a)) &&
             nyr_put(c->w, "\trev\tr2, r1\n\trev\tr3, r0\n") &&
             sm_st(c, sm_voff(c, in->dst), "r2") &&
             sm_st(c, sm_vhi_off(c, in->dst), "r3");
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) &&
             sm_ld(c, "%edx", sm_voff(c, in->a) + 4) &&
             nyr_put(c->w,
                     "\tbswap\t%eax\n\tbswap\t%edx\n\txchgl\t%eax, %edx\n") &&
             sm_stv(c, in->dst, c->t0) &&
             sm_st(c, sm_voff(c, in->dst) + 4, "%edx");
    if (c->kind == NY_SM_RISCV)
      return sm_ldv(c, c->t0, in->a) &&
             nyr_put(c->w, "\tslli\ta2, a0, 56\n\tsrli\ta3, a0, 56\n"
                           "\tor\ta2, a2, a3\n\tsrli\ta3, a0, 8\n"
                           "\tandi\ta3, a3, 255\n\tslli\ta3, a3, 48\n"
                           "\tor\ta2, a2, a3\n\tsrli\ta3, a0, 16\n"
                           "\tandi\ta3, a3, 255\n\tslli\ta3, a3, 40\n"
                           "\tor\ta2, a2, a3\n\tsrli\ta3, a0, 24\n"
                           "\tandi\ta3, a3, 255\n\tslli\ta3, a3, 32\n"
                           "\tor\ta2, a2, a3\n\tsrli\ta3, a0, 32\n"
                           "\tandi\ta3, a3, 255\n\tslli\ta3, a3, 24\n"
                           "\tor\ta2, a2, a3\n\tsrli\ta3, a0, 40\n"
                           "\tandi\ta3, a3, 255\n\tslli\ta3, a3, 16\n"
                           "\tor\ta2, a2, a3\n\tsrli\ta3, a0, 48\n"
                           "\tandi\ta3, a3, 255\n\tslli\ta3, a3, 8\n"
                           "\tor\ta2, a2, a3\n\tmv\ta0, a2\n") &&
             sm_stv(c, in->dst, c->t0);
  case NYIR_BSWAP32_I64:
    if (c->kind == NY_SM_A64)
      return sm_ldv(c, c->t0, in->a) && nyr_put(c->w, "\trev\tw0, w0\n") &&
             sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_ARM)
      return sm_ldv(c, c->t0, in->a) &&
             nyr_put(c->w, "\trev\tr0, r0\n\tmov\tr2, #0\n") &&
             sm_stv(c, in->dst, c->t0) &&
             sm_st(c, sm_voff(c, in->dst) + 4, "r2");
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) &&
             nyr_put(c->w, "\tbswap\t%eax\n\txorl\t%edx, %edx\n") &&
             sm_stv(c, in->dst, c->t0) &&
             sm_st(c, sm_voff(c, in->dst) + 4, "%edx");
    if (c->kind == NY_SM_RISCV)
      return sm_ldv(c, c->t0, in->a) &&
             nyr_put(c->w, "\tslli\ta0, a0, 32\n\tsrli\ta0, a0, 32\n"
                           "\tandi\ta2, a0, 255\n\tslli\ta2, a2, 24\n"
                           "\tsrli\ta3, a0, 8\n\tandi\ta3, a3, 255\n"
                           "\tslli\ta3, a3, 16\n\tsrli\ta4, a0, 16\n"
                           "\tandi\ta4, a4, 255\n\tslli\ta4, a4, 8\n"
                           "\tsrli\ta5, a0, 24\n\tor\ta0, a2, a3\n"
                           "\tor\ta0, a0, a4\n\tor\ta0, a0, a5\n") &&
             sm_stv(c, in->dst, c->t0);
  case NYIR_SHL_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_shift64(c, in, false);
    if (c->kind == NY_SM_I386)
      return sm_i386_shift64(c, in, false);
    return sm_emit_binop(c, in, "lsl", "lsl", "shl", "sll");
  case NYIR_SAR_I64:
    if (c->kind == NY_SM_ARM)
      return sm_arm_shift64(c, in, true);
    if (c->kind == NY_SM_I386)
      return sm_i386_shift64(c, in, true);
    return sm_emit_binop(c, in, "asr", "asr", "sar", "sra");
  case NYIR_ROR_I64:
    if (c->kind == NY_SM_A64)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tror\tx0, x0, x1\n") && sm_stv(c, in->dst, c->t0);
    if (c->kind != NY_SM_RISCV)
      break;
    if (!sm_ldv(c, c->t0, in->a) || !sm_ldv(c, c->t1, in->b) ||
        !nyr_put(c->w, "\tandi\ta1, a1, 63\n\tneg\ta2, a1\n"
                       "\tsrl\ta3, a0, a1\n\tsll\ta0, a0, a2\n"
                       "\tor\ta0, a0, a3\n") ||
        !sm_stv(c, in->dst, c->t0))
      return false;
    return true;
  case NYIR_ROR32_I64:
    if (c->kind == NY_SM_A64)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tror\tw0, w0, w1\n") && sm_stv(c, in->dst, c->t0);
    if (c->kind == NY_SM_ARM)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tror\tr0, r0, r1\n\tmov\tr2, #0\n") &&
             sm_stv(c, in->dst, c->t0) &&
             sm_st(c, sm_voff(c, in->dst) + 4, "r2");
    if (c->kind == NY_SM_I386)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\trorl\t%cl, %eax\n\txorl\t%edx, %edx\n") &&
             sm_stv(c, in->dst, c->t0) &&
             sm_st(c, sm_voff(c, in->dst) + 4, "%edx");
    if (c->kind == NY_SM_RISCV)
      return sm_ldv(c, c->t0, in->a) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tslli\ta0, a0, 32\n\tsrli\ta0, a0, 32\n"
                           "\tandi\ta1, a1, 31\n\tneg\ta2, a1\n"
                           "\tandi\ta2, a2, 31\n\tsrl\ta3, a0, a1\n"
                           "\tsll\ta0, a0, a2\n\tor\ta0, a0, a3\n"
                           "\tslli\ta0, a0, 32\n\tsrli\ta0, a0, 32\n") &&
             sm_stv(c, in->dst, c->t0);
    break;
  case NYIR_CMP_I64:
    return sm_cmp(c, in);
  case NYIR_SELECT_I64:
    switch (c->kind) {
    case NY_SM_ARM:
      return sm_ldv(c, c->t0, in->a) && nyr_put(c->w, "\tcmp\tr0, #0\n") &&
             sm_ldv(c, c->t0, in->c) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tmovne\tr0, r1\n") && sm_stv(c, in->dst, c->t0);
    case NY_SM_A64:
      return sm_ldv(c, c->t0, in->a) && nyr_put(c->w, "\tcmp\tx0, #0\n") &&
             sm_ldv(c, c->t0, in->c) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tcsel\tx0, x1, x0, ne\n") &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_I386:
      return sm_ldv(c, c->t0, in->a) &&
             nyr_put(c->w, "\ttestl\t%eax, %eax\n") &&
             sm_ldv(c, c->t0, in->c) && sm_ldv(c, c->t1, in->b) &&
             nyr_put(c->w, "\tcmovnel\t%ecx, %eax\n") &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_RISCV:
      if (!sm_ldv(c, c->t0, in->a) || !nyr_put(c->w, "\tbnez\ta0, 1f\n") ||
          !sm_ldv(c, c->t0, in->c) || !nyr_put(c->w, "\tj\t2f\n1:\n") ||
          !sm_ldv(c, c->t0, in->b) || !nyr_put(c->w, "2:\n") ||
          !sm_stv(c, in->dst, c->t0))
        return false;
      return true;
    }
    break;
  case NYIR_LABEL:
    return nyr_printf(c->w, "%s_nyir_L%" PRId64 ":\n", c->epilogue, in->imm);
  case NYIR_ADDR_SYMBOL:
    if (!in->symbol || !in->symbol[0]) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: addr.symbol missing symbol name", c->pretty);
      return false;
    }
    switch (c->kind) {
    case NY_SM_ARM:
      return nyr_printf(c->w, "\tldr\tr0, =%s\n", in->symbol) &&
             (in->dst < 0 || sm_stv(c, in->dst, c->t0));
    case NY_SM_A64:
      return nyr_printf(c->w, "\tadrp\tx0, %s\n\tadd\tx0, x0, :lo12:%s\n",
                        in->symbol, in->symbol) &&
             (in->dst < 0 || sm_stv(c, in->dst, c->t0));
    case NY_SM_I386:
      return nyr_printf(c->w, "\tmovl\t$%s, %%eax\n", in->symbol) &&
             (in->dst < 0 || sm_stv(c, in->dst, c->t0));
    case NY_SM_RISCV:
      return nyr_printf(c->w, "\tla\ta0, %s\n", in->symbol) &&
             (in->dst < 0 || sm_stv(c, in->dst, c->t0));
    }
    return false;
  case NYIR_LOAD_LOCAL:
    if (!sm_ldl(c, c->t0, (int)in->imm) || !sm_stv(c, in->dst, c->t0))
      return false;
    return (c->kind != NY_SM_ARM && c->kind != NY_SM_I386) ||
           (sm_ld(c, c->kind == NY_SM_I386 ? "%edx" : "r0",
                  sm_lhi_off(c, (int)in->imm)) &&
            sm_st(c, sm_vhi_off(c, in->dst),
                  c->kind == NY_SM_I386 ? "%edx" : "r0"));
  case NYIR_STORE_LOCAL:
    if (!sm_ldv(c, c->t0, in->a) || !sm_stl(c, (int)in->imm, c->t0))
      return false;
    return (c->kind != NY_SM_ARM && c->kind != NY_SM_I386) ||
           (sm_ld(c, c->kind == NY_SM_I386 ? "%edx" : "r0",
                  sm_vhi_off(c, in->a)) &&
            sm_st(c, sm_lhi_off(c, (int)in->imm),
                  c->kind == NY_SM_I386 ? "%edx" : "r0"));
  case NYIR_ADDR_LOCAL: {
    int off = sm_loff(c, (int)in->imm);
    if (in->imm < 0 || in->imm >= c->max_local_slot) {
      nyr_set_error(c->err, c->err_len, "%s NYIR emit: bad addr.local",
                    c->pretty);
      return false;
    }
    switch (c->kind) {
    case NY_SM_ARM:
      return nyr_printf(c->w, "\tadd\tr0, sp, #%d\n", off) &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_A64:
      return nyr_printf(c->w, "\tadd\tx0, sp, #%d\n", off) &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_I386:
      return nyr_printf(c->w, "\tleal\t%d(%%ebp), %%eax\n", off) &&
             sm_stv(c, in->dst, c->t0);
    case NY_SM_RISCV:
      return nyr_printf(c->w, "\taddi\ta0, sp, %d\n", off) &&
             sm_stv(c, in->dst, c->t0);
    }
    return false;
  }
  case NYIR_LOAD_I64:
    if (!sm_ldv(c, c->t0, in->a))
      return false;
    switch (c->kind) {
    case NY_SM_ARM:
      return nyr_put(c->w, "\tldr\tr2, [r0]\n\tldr\tr3, [r0, #4]\n") &&
             sm_st(c, sm_voff(c, in->dst), "r2") &&
             sm_st(c, sm_vhi_off(c, in->dst), "r3");
    case NY_SM_A64:
      return nyr_put(c->w, "\tldr\tx0, [x0]\n") && sm_stv(c, in->dst, c->t0);
    case NY_SM_I386:
      return nyr_put(c->w, "\tmovl\t(%eax), %ecx\n\tmovl\t4(%eax), %edx\n") &&
             sm_stv(c, in->dst, "%ecx") &&
             sm_st(c, sm_voff(c, in->dst) + 4, "%edx");
    case NY_SM_RISCV:
      return nyr_put(c->w, "\tld\ta0, 0(a0)\n") && sm_stv(c, in->dst, c->t0);
    }
    return false;
  case NYIR_STORE_I64:
    if (!sm_ldv(c, c->t0, in->a) || !sm_ldv(c, c->t1, in->c))
      return false;
    switch (c->kind) {
    case NY_SM_ARM:
      return sm_ld(c, "r2", sm_vhi_off(c, in->c)) &&
             nyr_put(c->w, "\tstr\tr1, [r0]\n\tstr\tr2, [r0, #4]\n");
    case NY_SM_A64:
      return nyr_put(c->w, "\tstr\tx1, [x0]\n");
    case NY_SM_I386:
      return sm_ld(c, "%edx", sm_voff(c, in->c) + 4) &&
             nyr_put(c->w, "\tmovl\t%ecx, (%eax)\n\tmovl\t%edx, 4(%eax)\n");
    case NY_SM_RISCV:
      return nyr_put(c->w, "\tsd\ta1, 0(a0)\n");
    }
    return false;
  case NYIR_CALL:
    return sm_call(c, in);
  case NYIR_RET:
    if (in->a >= 0) {
      if (c->kind == NY_SM_A64 && in->scalar_class == NYIR_SCALAR_F64) {
        if (!sm_ld(c, "d0", sm_voff(c, in->a)))
          return false;
      } else if (c->kind == NY_SM_A64 && in->scalar_class == NYIR_SCALAR_F32) {
        if (!sm_ld(c, "s0", sm_voff(c, in->a)))
          return false;
      } else if (c->kind == NY_SM_ARM && in->scalar_class == NYIR_SCALAR_F64) {
        if (!sm_err_val(c, in->a, "return value") ||
            !sm_ld(c, "r0", sm_voff(c, in->a)) ||
            !sm_ld(c, "r1", sm_vhi_off(c, in->a)))
          return false;
      } else if (c->kind == NY_SM_ARM &&
                 in->scalar_class == NYIR_SCALAR_I64_RAW) {
        if (!sm_err_val(c, in->a, "return value") ||
            !sm_ld(c, "r0", sm_voff(c, in->a)) ||
            !sm_ld(c, "r1", sm_vhi_off(c, in->a)))
          return false;
      } else if (c->kind == NY_SM_I386 &&
                 in->scalar_class == NYIR_SCALAR_I64_RAW) {
        if (!sm_ldv(c, c->ret, in->a) ||
            !sm_ld(c, "%edx", sm_voff(c, in->a) + 4))
          return false;
      } else if (!sm_ldv(c, c->ret, in->a)) {
        return false;
      }
    }
    switch (c->kind) {
    case NY_SM_ARM:
    case NY_SM_A64:
      return nyr_printf(c->w, "\tb\t%s\n", c->epilogue);
    case NY_SM_I386:
      return nyr_printf(c->w, "\tjmp\t%s\n", c->epilogue);
    case NY_SM_RISCV:
      return nyr_printf(c->w, "\tj\t%s\n", c->epilogue);
    }
    return false;
  case NYIR_BR:
    switch (c->kind) {
    case NY_SM_ARM:
    case NY_SM_A64:
      return nyr_printf(c->w, "\tb\t%s_nyir_L%" PRId64 "\n", c->epilogue,
                        in->imm);
    case NY_SM_I386:
      return nyr_printf(c->w, "\tjmp\t%s_nyir_L%" PRId64 "\n", c->epilogue,
                        in->imm);
    case NY_SM_RISCV:
      return nyr_printf(c->w, "\tj\t%s_nyir_L%" PRId64 "\n", c->epilogue,
                        in->imm);
    }
    return false;
  case NYIR_BR_IF:
    if (!sm_ldv(c, c->t0, in->a))
      return false;
    switch (c->kind) {
    case NY_SM_ARM:
      return nyr_put(c->w, "\tcmp\tr0, #0\n") &&
             nyr_printf(c->w, "\tbne\t%s_nyir_L%" PRId64 "\n", c->epilogue,
                        in->imm);
    case NY_SM_A64:
      return nyr_printf(c->w, "\tcbnz\tx0, %s_nyir_L%" PRId64 "\n", c->epilogue,
                        in->imm);
    case NY_SM_I386:
      return nyr_put(c->w, "\ttestl\t%eax, %eax\n") &&
             nyr_printf(c->w, "\tjnz\t%s_nyir_L%" PRId64 "\n", c->epilogue,
                        in->imm);
    case NY_SM_RISCV:
      return nyr_printf(c->w, "\tbnez\ta0, %s_nyir_L%" PRId64 "\n", c->epilogue,
                        in->imm);
    }
    return false;
  default:
    break;
  }
  nyr_set_error(c->err, c->err_len, "%s NYIR emit: unsupported op %s",
                c->pretty, nyr_op_name(in->op));
  return false;
}

static bool sm_emit(nyr_writer_t *w, const ny_native_target_info_t *target,
                    const nyr_func_t *nyir, const char *func_name,
                    bool tag_return, char *err, size_t err_len,
                    ny_sm_kind_t kind, const char *pretty, const char *ret,
                    const char *t0, const char *t1, int word, int frame_align) {
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

  if (c.kind == NY_SM_ARM && !nyr_put(w, "\t.syntax unified\n\t.arm\n"))
    return false;
  if (!nyr_put(w, "\t.text\n"))
    return false;
  const char *pfx = target->symbol_prefix ? target->symbol_prefix : "";
  if (strcmp(target->object_format, "macho") != 0) {
    const char *ty = (c.kind == NY_SM_ARM || c.kind == NY_SM_A64) ? "%function"
                                                                  : "@function";
    if (!nyr_printf(w, "\t.type\t%s%s, %s\n", pfx, name, ty))
      return false;
  }
  if (!nyr_printf(w, "\t.globl\t%s%s\n%s%s:\n", pfx, name, pfx, name))
    return false;

  /*
   * prologue
   */
  switch (c.kind) {
  case NY_SM_ARM:
    if (!c.is_leaf && !nyr_put(w, "\tpush\t{r4, r5, r6, r7, r8, lr}\n"))
      return false;
    if (c.frame_bytes > 0 &&
        !nyr_printf(w, "\tsub\tsp, sp, #%d\n", c.frame_bytes))
      return false;
    break;
  case NY_SM_A64:
    if (!c.is_leaf &&
        !nyr_put(w, "\tstp\tx29, x30, [sp, #-16]!\n\tmov\tx29, sp\n"))
      return false;
    if (c.frame_bytes > 0 && !sm_a64_adjust_sp(&c, true))
      return false;
    break;
  case NY_SM_I386:
    if (!nyr_put(w, "\tpushl\t%ebp\n\tmovl\t%esp, %ebp\n"))
      return false;
    if (c.frame_bytes > 0 &&
        !nyr_printf(w, "\tsubl\t$%d, %%esp\n", c.frame_bytes))
      return false;
    break;
  case NY_SM_RISCV:
    if (!nyr_printf(w, "\taddi\tsp, sp, -%d\n", c.frame_bytes))
      return false;
    if (!c.is_leaf &&
        !nyr_printf(w,
                    "\tsd\tra, %d(sp)\n\tsd\ts0, %d(sp)\n\taddi\ts0, sp, %d\n",
                    c.frame_bytes - 8, c.frame_bytes - 16, c.frame_bytes))
      return false;
    break;
  }

  if (strcmp(name, "rt_main") != 0) {
    if (c.kind == NY_SM_A64) {
      if (!sm_a64_store_parameters(&c))
        return false;
    } else if (c.kind == NY_SM_I386) {
      int incoming_offset = 8;
      for (size_t i = 0; i < nyir->param_count; ++i) {
        nyr_param_type_t type =
            nyir->param_types ? nyir->param_types[i] : NYIR_PARAM_I64;
        if (i >= (size_t)c.max_local_slot ||
            !sm_ld(&c, "%eax", incoming_offset) ||
            !sm_st(&c, sm_loff(&c, (int)i), "%eax"))
          return false;
        if (type == NYIR_PARAM_F64) {
          if (!sm_ld(&c, "%eax", incoming_offset + 4) ||
              !sm_st(&c, sm_lhi_off(&c, (int)i), "%eax"))
            return false;
          incoming_offset += 8;
        } else {
          if (type == NYIR_PARAM_I64) {
            if (!nyr_put(w, "\tcltd\n") ||
                !sm_st(&c, sm_lhi_off(&c, (int)i), "%edx"))
              return false;
          } else if (!sm_i386_store_u32(&c, sm_lhi_off(&c, (int)i), 0)) {
            return false;
          }
          incoming_offset += 4;
        }
      }
    } else if (c.kind == NY_SM_ARM) {
      if (!sm_arm_store_parameters(&c))
        return false;
    } else {
      int max = c.max_local_slot < (int)target->gp_arg_reg_count
                    ? c.max_local_slot
                    : (int)target->gp_arg_reg_count;
      for (int i = 0; i < max; ++i) {
        if (!sm_stl(&c, i, target->gp_arg_regs[i]))
          return false;
      }
    }
  }

  for (size_t i = 0; i < nyir->len; ++i) {
    if (!sm_inst(&c, &nyir->data[i])) {
      fprintf(stderr, "native NYIR repro (%s emit failed):\n", pretty);
      nyr_dump_compact(stderr, nyir, name);
      return false;
    }
  }

  if (!nyr_printf(w, "%s:\n", c.epilogue))
    return false;
  if (tag_return) {
    switch (c.kind) {
    case NY_SM_ARM:
      if (!nyr_put(w, "\tlsl\tr0, r0, #1\n\tadd\tr0, r0, #1\n"))
        return false;
      break;
    case NY_SM_A64:
      if (!nyr_put(w, "\tlsl\tx0, x0, #1\n\tadd\tx0, x0, #1\n"))
        return false;
      break;
    case NY_SM_I386:
      if (!nyr_put(w, "\tleal\t1(,%eax,2), %eax\n"))
        return false;
      break;
    case NY_SM_RISCV:
      if (!nyr_put(w, "\tslli\ta0, a0, 1\n\taddi\ta0, a0, 1\n"))
        return false;
      break;
    }
  }
  switch (c.kind) {
  case NY_SM_ARM:
    if (c.frame_bytes > 0 &&
        !nyr_printf(w, "\tadd\tsp, sp, #%d\n", c.frame_bytes))
      return false;
    if (!nyr_put(w, c.is_leaf ? "\tbx\tlr\n"
                              : "\tpop\t{r4, r5, r6, r7, r8, pc}\n"))
      return false;
    break;
  case NY_SM_A64:
    if (c.frame_bytes > 0 && !sm_a64_adjust_sp(&c, false))
      return false;
    if (!nyr_put(w,
                 c.is_leaf ? "\tret\n" : "\tldp\tx29, x30, [sp], #16\n\tret\n"))
      return false;
    break;
  case NY_SM_I386:
    if (!nyr_put(w, "\tleave\n\tret\n"))
      return false;
    break;
  case NY_SM_RISCV:
    if (!c.is_leaf && !nyr_printf(w, "\tld\tra, %d(sp)\n\tld\ts0, %d(sp)\n",
                                  c.frame_bytes - 8, c.frame_bytes - 16))
      return false;
    if (!nyr_printf(w, "\taddi\tsp, sp, %d\n\tret\n", c.frame_bytes))
      return false;
    break;
  }
  if (strcmp(target->object_format, "macho") != 0 &&
      !nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", pfx, name, pfx, name))
    return false;
  return true;
}

bool nyr_arm_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                     const nyr_func_t *nyir, const char *func_name,
                     bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len,
                 NY_SM_ARM, "ARM", "r0", "r0", "r1", 4, 8);
}

bool nyr_aarch64_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                         const nyr_func_t *nyir, const char *func_name,
                         bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len,
                 NY_SM_A64, "AArch64", "x0", "x0", "x1", 8, 16);
}

bool nyr_i386_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                      const nyr_func_t *nyir, const char *func_name,
                      bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len,
                 NY_SM_I386, "i386", "%eax", "%eax", "%ecx", 4, 16);
}

bool nyr_riscv_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                       const nyr_func_t *nyir, const char *func_name,
                       bool tag_return, char *err, size_t err_len) {
  return sm_emit(w, target, nyir, func_name, tag_return, err, err_len,
                 NY_SM_RISCV, "RISC-V", "a0", "a0", "a1", 8, 16);
}
