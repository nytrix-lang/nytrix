/*
 * Portable text-assembly backends for non-primary targets.
 *
 * These emit assembly text from high-level NYIR (not x86-64/AArch64 machine
 * form) for targets that do not yet have a full object/JIT pipeline:
 *
 *   bpf      — eBPF / Linux BPF-style stack machine (r10 frame)
 *   mips     — MIPS-like GPR + stack frame
 *   powerpc  — PowerPC-like GPR + stack frame
 *   avr      — 8-bit AVR-ish register/stack model
 *   wasm     — WebAssembly text (local slots, no classic stack frame)
 *
 * Primary production backends remain x86-64 and AArch64 under
 * code/backend/machine and code/backend/object. Portable emitters
 * are for bring-up, cross-checks, and future object writers — they must not be
 * presented as full native-only substitutes for host x86-64/AArch64.
 *
 * Entry points: nyr_bpf_emit_ir etc. (see internal.h).
 */
#include "code/backend/abi.h"
#include "code/backend/internal.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef enum {
  NY_PORT_BPF,
  NY_PORT_MIPS,
  NY_PORT_POWERPC,
  NY_PORT_AVR,
  NY_PORT_WASM,
} nyr_port_kind_t;

typedef struct {
  nyr_writer_t *w;
  const ny_native_target_info_t *target;
  const nyr_func_t *nyir;
  nyr_port_kind_t kind;
  const char *name;
  const char *pretty;
  const char *ret_reg;
  const char *tmp0;
  const char *tmp1;
  int word_bytes;
  int frame_bytes;
  int local_base;
  int max_local_slot;
  size_t current_index;
  char epilogue_label[160];
  char dispatch_label[160];
  char *err;
  size_t err_len;
} nyr_port_ctx_t;

static int nyr_port_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static const char *nyr_port_reg_name(const nyr_port_ctx_t *c, const char *reg) {
  if (c && c->kind == NY_PORT_POWERPC && reg && reg[0] == 'r' &&
      reg[1] >= '0' && reg[1] <= '9')
    return reg + 1;
  return reg;
}

static void nyr_port_compute_frame(nyr_port_ctx_t *c) {
  c->max_local_slot = 0;
  for (size_t i = 0; c->nyir && i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  if (c->nyir && c->nyir->param_count > (size_t)c->max_local_slot)
    c->max_local_slot = (int)c->nyir->param_count;
  int value_slots =
      c->nyir && c->nyir->next_value > 0 ? c->nyir->next_value : 0;
  c->local_base = value_slots * c->word_bytes;
  int spill = (value_slots + c->max_local_slot) * c->word_bytes;
  if (c->kind == NY_PORT_BPF)
    c->frame_bytes = nyr_port_align(spill, 8);
  else if (c->kind == NY_PORT_WASM)
    c->frame_bytes = value_slots + c->max_local_slot;
  else if (c->kind == NY_PORT_AVR)
    c->frame_bytes = nyr_port_align(spill, 2);
  else
    c->frame_bytes = nyr_port_align(spill + 32, 16);
}

static int nyr_port_value_off(nyr_port_ctx_t *c, int value) {
  return value * c->word_bytes;
}

static int nyr_port_local_off(nyr_port_ctx_t *c, int local) {
  return c->local_base + local * c->word_bytes;
}

static bool nyr_port_load_slot(nyr_port_ctx_t *c, const char *reg, int off);
static bool nyr_port_store_slot(nyr_port_ctx_t *c, int off, const char *reg);
static bool nyr_port_load_value(nyr_port_ctx_t *c, const char *reg, int value);
static bool nyr_port_store_value(nyr_port_ctx_t *c, int value, const char *reg);

static bool nyr_port_check_value(nyr_port_ctx_t *c, int value,
                                 const char *what) {
  if (value < 0 || !c || !c->nyir || value >= c->nyir->next_value) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "%s NYIR emit: invalid %s v%d", c ? c->pretty : "portable",
                  what ? what : "value", value);
    return false;
  }
  return true;
}

static bool nyr_port_avr_reg_range(const char *reg, unsigned *first) {
  unsigned lo = 0;
  unsigned hi = 0;
  char trailing = '\0';
  if (!reg || sscanf(reg, "r%u:r%u%c", &lo, &hi, &trailing) != 2 ||
      hi != lo + 7 || hi > 31)
    return false;
  if (first)
    *first = lo;
  return true;
}

static bool nyr_port_avr_slot_addr(nyr_port_ctx_t *c, int off) {
  if (off < 0 || !nyr_put(c->w, "\tmovw\tr30, r28\n"))
    return false;
  while (off > 0) {
    unsigned step = off > 63 ? 63u : (unsigned)off;
    if (!nyr_printf(c->w, "\tadiw\tr30, %u\n", step))
      return false;
    off -= (int)step;
  }
  return true;
}

static bool nyr_port_avr_advance_y(nyr_port_ctx_t *c, int off) {
  if (off < 0)
    return false;
  while (off > 0) {
    unsigned step = off > 63 ? 63u : (unsigned)off;
    if (!nyr_printf(c->w, "\tadiw\tr28, %u\n", step))
      return false;
    off -= (int)step;
  }
  return true;
}

static bool nyr_port_avr_mul_i64(nyr_port_ctx_t *c, const nyr_inst_t *in) {
  if (in->dst == in->a || in->dst == in->b) {
    nyr_set_error(c->err, c->err_len,
                  "%s NYIR emit: AVR multiply requires distinct SSA slots",
                  c->pretty);
    return false;
  }
  for (unsigned byte = 0; byte < 8; ++byte)
    if (!nyr_printf(c->w, "\tclr\tr%u\n", 18 + byte))
      return false;
  if (!nyr_port_store_value(c, in->dst, "r18:r25") ||
      !nyr_port_load_value(c, "r18:r25", in->a) ||
      !nyr_port_load_value(c, "r10:r17", in->b) ||
      !nyr_printf(c->w,
                  "\tldi\tr27, 64\n"
                  ".Lny_mul_loop_%s_%zu:\n",
                  c->name, c->current_index))
    return false;

  const int result_off = nyr_port_value_off(c, in->dst);
  if (!nyr_put(c->w, "\tsbrs\tr10, 0\n") ||
      !nyr_printf(c->w,
                  "\trjmp\t.Lny_mul_skip_%s_%zu\n"
                  "\tmovw\tr30, r28\n",
                  c->name, c->current_index) ||
      !nyr_port_avr_advance_y(c, result_off))
    return false;
  for (unsigned byte = 0; byte < 8; ++byte) {
    if (!nyr_printf(c->w,
                    "\tldd\tr26, Y+%u\n"
                    "\t%s\tr26, r%u\n"
                    "\tstd\tY+%u, r26\n",
                    byte, byte == 0 ? "add" : "adc", 18 + byte, byte))
      return false;
  }
  if (!nyr_printf(c->w,
                  "\tmovw\tr28, r30\n"
                  ".Lny_mul_skip_%s_%zu:\n"
                  "\tlsl\tr18\n"
                  "\trol\tr19\n"
                  "\trol\tr20\n"
                  "\trol\tr21\n"
                  "\trol\tr22\n"
                  "\trol\tr23\n"
                  "\trol\tr24\n"
                  "\trol\tr25\n"
                  "\tlsr\tr17\n"
                  "\tror\tr16\n"
                  "\tror\tr15\n"
                  "\tror\tr14\n"
                  "\tror\tr13\n"
                  "\tror\tr12\n"
                  "\tror\tr11\n"
                  "\tror\tr10\n"
                  "\tdec\tr27\n"
                  "\tbrne\t.Lny_mul_loop_%s_%zu\n",
                  c->name, c->current_index, c->name, c->current_index))
    return false;
  return nyr_port_load_value(c, "r18:r25", in->dst);
}

static bool nyr_port_load_slot(nyr_port_ctx_t *c, const char *reg, int off) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return nyr_printf(c->w, "\t%s = *(u64 *)(r10 - %d)\n", reg, off + 8);
  case NY_PORT_MIPS:
    return nyr_printf(c->w, "\tld\t%s, %d($fp)\n", reg, off);
  case NY_PORT_POWERPC:
    return nyr_printf(c->w, "\tld\t%s, %d(31)\n", nyr_port_reg_name(c, reg),
                      off);
  case NY_PORT_AVR: {
    unsigned first = 0;
    if (off < 0 || !nyr_port_avr_reg_range(reg, &first)) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: AVR spill exceeds the supported Y frame",
                    c->pretty);
      return false;
    }
    if (off <= 56) {
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w, "\tldd\tr%u, Y+%d\n", first + byte,
                        off + (int)byte))
          return false;
    } else {
      if (!nyr_port_avr_slot_addr(c, off))
        return false;
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w, "\tld\tr%u, Z+\n", first + byte))
          return false;
    }
    return true;
  }
  case NY_PORT_WASM:
    return nyr_printf(c->w, "\tlocal.get $s%d ;; -> %s\n", off, reg);
  }
  return false;
}

static bool nyr_port_store_slot(nyr_port_ctx_t *c, int off, const char *reg) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return nyr_printf(c->w, "\t*(u64 *)(r10 - %d) = %s\n", off + 8, reg);
  case NY_PORT_MIPS:
    return nyr_printf(c->w, "\tsd\t%s, %d($fp)\n", reg, off);
  case NY_PORT_POWERPC:
    return nyr_printf(c->w, "\tstd\t%s, %d(31)\n", nyr_port_reg_name(c, reg),
                      off);
  case NY_PORT_AVR: {
    unsigned first = 0;
    if (off < 0 || !nyr_port_avr_reg_range(reg, &first)) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: AVR spill exceeds the supported Y frame",
                    c->pretty);
      return false;
    }
    if (off <= 56) {
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w, "\tstd\tY+%d, r%u\n", off + (int)byte,
                        first + byte))
          return false;
    } else {
      if (!nyr_port_avr_slot_addr(c, off))
        return false;
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w, "\tst\tZ+, r%u\n", first + byte))
          return false;
    }
    return true;
  }
  case NY_PORT_WASM:
    return nyr_printf(c->w, "\tlocal.set $s%d ;; <- %s\n", off, reg);
  }
  return false;
}

static bool nyr_port_load_value(nyr_port_ctx_t *c, const char *reg, int value) {
  return nyr_port_check_value(c, value, "value") &&
         nyr_port_load_slot(c, reg, nyr_port_value_off(c, value));
}

static bool nyr_port_store_value(nyr_port_ctx_t *c, int value,
                                 const char *reg) {
  return nyr_port_check_value(c, value, "destination") &&
         nyr_port_store_slot(c, nyr_port_value_off(c, value), reg);
}

static bool nyr_port_load_local(nyr_port_ctx_t *c, const char *reg, int local) {
  if (local < 0 || local >= c->max_local_slot) {
    nyr_set_error(c->err, c->err_len, "%s NYIR emit: invalid local slot %d",
                  c->pretty, local);
    return false;
  }
  return nyr_port_load_slot(c, reg, nyr_port_local_off(c, local));
}

static bool nyr_port_powerpc_mov_imm(nyr_port_ctx_t *c, const char *reg,
                                     int64_t value) {
  const char *ppc_reg = nyr_port_reg_name(c, reg);
  if (value >= INT16_MIN && value <= INT16_MAX)
    return nyr_printf(c->w, "\tli\t%s, %" PRId64 "\n", ppc_reg, value);

  const uint64_t bits = (uint64_t)value;
  const unsigned top = (unsigned)((bits >> 48) & UINT64_C(0xffff));
  const unsigned high = (unsigned)((bits >> 32) & UINT64_C(0xffff));
  const unsigned mid = (unsigned)((bits >> 16) & UINT64_C(0xffff));
  const unsigned low = (unsigned)(bits & UINT64_C(0xffff));
  return nyr_printf(c->w,
                    "\tlis\t%s, %u\n"
                    "\tori\t%s, %s, %u\n"
                    "\tsldi\t%s, %s, 32\n"
                    "\toris\t%s, %s, %u\n"
                    "\tori\t%s, %s, %u\n",
                    ppc_reg, top, ppc_reg, ppc_reg, high, ppc_reg, ppc_reg,
                    ppc_reg, ppc_reg, mid, ppc_reg, ppc_reg, low);
}

static bool nyr_port_store_local(nyr_port_ctx_t *c, int local,
                                 const char *reg) {
  if (local < 0 || local >= c->max_local_slot) {
    nyr_set_error(c->err, c->err_len, "%s NYIR emit: invalid local slot %d",
                  c->pretty, local);
    return false;
  }
  return nyr_port_store_slot(c, nyr_port_local_off(c, local), reg);
}

static bool nyr_port_avr_push_stack_arg(nyr_port_ctx_t *c, int value,
                                        unsigned size) {
  int off = nyr_port_value_off(c, value);
  if ((size != 4 && size != 8) || off < 0 || off > INT_MAX - (int)size ||
      !nyr_port_avr_slot_addr(c, off + (int)size))
    return false;
  /*
   * AVR PUSH stores at the current SP before decrementing it. Read backward
   * so the low byte is nearest the return address without clobbering the
   * callee-saved r2:r9 register group.
   */
  for (unsigned byte = 0; byte < size; ++byte)
    if (!nyr_put(c->w, "\tld\tr0, -Z\n\tpush\tr0\n"))
      return false;
  return true;
}

static bool nyr_port_avr_softfloat_helper(const char *symbol) {
  static const char *const helpers[] = {
      "__addsf3",     "__subsf3",      "__mulsf3",    "__divsf3",
      "__floatdisf",  "__fixsfdi",     "__adddf3",    "__subdf3",
      "__muldf3",     "__divdf3",      "__floatdidf", "__fixdfdi",
      "__truncdfsf2", "__extendsfdf2",
  };
  for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); ++i)
    if (symbol && strcmp(symbol, helpers[i]) == 0)
      return true;
  return false;
}

static unsigned nyr_port_avr_external_arg_size(const nyr_inst_t *call,
                                               nyr_scalar_class_t cls) {
  if (cls == NYIR_SCALAR_I64_RAW)
    return 8u;
  if (cls == NYIR_SCALAR_F32)
    return 4u;
  if (cls == NYIR_SCALAR_F64 && call &&
      (nyr_port_avr_softfloat_helper(call->symbol) ||
       strcmp(call->symbol, "rt_f64_to_i64") == 0))
    return 8u;
  return 0u;
}

static bool nyr_port_avr_external_args_supported(const nyr_inst_t *call) {
  if (!call || call->op != NYIR_CALL || !call->symbol)
    return false;
  bool runtime_f64_to_i64 = strcmp(call->symbol, "rt_f64_to_i64") == 0 &&
                            ny_native_runtime_abi_decl(call->symbol) != NULL;
  bool softfloat = nyr_port_avr_softfloat_helper(call->symbol);
  if ((!softfloat && !runtime_f64_to_i64 &&
       (!(call->flags & NYIR_INST_F_EXTERN) ||
        ny_native_runtime_abi_decl(call->symbol))) ||
      (softfloat && !(call->flags & NYIR_INST_F_EXTERN)))
    return false;
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!nyr_call_args(call, INT_MAX, args, NYIR_CALL_MAX_ARGS, &argc, NULL, 0))
    return false;
  if (call->dst >= 0 && call->scalar_class != NYIR_SCALAR_I64_RAW &&
      call->scalar_class != NYIR_SCALAR_F32 &&
      !((softfloat || runtime_f64_to_i64) &&
        call->scalar_class == NYIR_SCALAR_F64))
    return false;
  for (size_t arg = 0; arg < (size_t)argc; ++arg)
    if (!nyr_port_avr_external_arg_size(call,
                                        nyr_call_arg_class_get(call, arg)))
      return false;
  return true;
}

static bool nyr_port_avr_marshal_external_args(nyr_port_ctx_t *c,
                                               const nyr_inst_t *call,
                                               const int *args, int argc,
                                               unsigned *stack_bytes) {
  if (!c || !call || !args || argc < 0 || !stack_bytes)
    return false;
  size_t stack_start = (size_t)argc;
  unsigned reg_bytes = 0;
  *stack_bytes = 0;
  for (size_t i = 0; i < (size_t)argc; ++i) {
    unsigned size =
        nyr_port_avr_external_arg_size(call, nyr_call_arg_class_get(call, i));
    if (stack_start == (size_t)argc && reg_bytes + size <= 18u) {
      reg_bytes += size;
    } else {
      if (stack_start == (size_t)argc)
        stack_start = i;
      if (*stack_bytes > UINT_MAX - size)
        return false;
      *stack_bytes += size;
    }
  }
  for (size_t i = (size_t)argc; i > stack_start; --i) {
    unsigned size = nyr_port_avr_external_arg_size(
        call, nyr_call_arg_class_get(call, i - 1));
    if (!nyr_port_avr_push_stack_arg(c, args[i - 1], size))
      return false;
  }
  reg_bytes = 0;
  for (size_t i = 0; i < stack_start; ++i) {
    unsigned size =
        nyr_port_avr_external_arg_size(call, nyr_call_arg_class_get(call, i));
    int off = nyr_port_value_off(c, args[i]);
    if (off < 0 || !nyr_port_avr_slot_addr(c, off))
      return false;
    unsigned first_reg = 25u - reg_bytes - (size - 1u);
    for (unsigned byte = 0; byte < size; ++byte)
      if (!nyr_printf(c->w, "\tld\tr0, Z+\n\tmov\tr%u, r0\n", first_reg + byte))
        return false;
    reg_bytes += size;
  }
  return true;
}

static bool nyr_port_avr_advance_x(nyr_port_ctx_t *c, int off) {
  if (off < 0 || !nyr_put(c->w, "\tmovw\tr26, r28\n"))
    return false;
  while (off > 0) {
    unsigned step = off > 63 ? 63u : (unsigned)off;
    if (!nyr_printf(c->w, "\tadiw\tr26, %u\n", step))
      return false;
    off -= (int)step;
  }
  return true;
}

static bool nyr_port_avr_load_stack_param(nyr_port_ctx_t *c, size_t param) {
  size_t register_params = c->target->gp_arg_reg_count;
  size_t stack_index = param - register_params;
  if (stack_index > (size_t)(INT_MAX / 8) ||
      c->frame_bytes > INT_MAX - 13 - (int)(stack_index * 8)) {
    nyr_set_error(c->err, c->err_len,
                  "%s NYIR emit: AVR stack parameter offset overflows",
                  c->pretty);
    return false;
  }
  /*
   * The incoming low byte is 13 bytes beyond the frame: ten saved registers,
   * the two-byte return PC, and AVR's pre-decrement PUSH layout.
   */
  int offset = c->frame_bytes + 13 + (int)(stack_index * 8);
  if (!nyr_port_avr_slot_addr(c, offset))
    return false;
  int local_offset = nyr_port_local_off(c, (int)param);
  if (!nyr_port_avr_advance_x(c, local_offset))
    return false;
  for (unsigned byte = 0; byte < 8; ++byte)
    if (!nyr_put(c->w, "\tld\tr0, Z+\n\tst\tX+, r0\n"))
      return false;
  return true;
}

static bool nyr_port_mov_imm(nyr_port_ctx_t *c, const char *reg,
                             int64_t value) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return nyr_printf(c->w, "\t%s = %" PRId64 "\n", reg, value);
  case NY_PORT_MIPS:
    return nyr_printf(c->w, "\tdli\t%s, %" PRId64 "\n", reg, value);
  case NY_PORT_POWERPC:
    return nyr_port_powerpc_mov_imm(c, reg, value);
  case NY_PORT_AVR: {
    unsigned first = 0;
    if (!nyr_port_avr_reg_range(reg, &first)) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: invalid AVR i64 register group", c->pretty);
      return false;
    }
    uint64_t bits = (uint64_t)value;
    if (first >= 16) {
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w, "\tldi\tr%u, %u\n", first + byte,
                        (unsigned)((bits >> (byte * 8)) & UINT64_C(0xff))))
          return false;
      return true;
    }
    if (first != 10) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: unsupported AVR immediate register group",
                    c->pretty);
      return false;
    }
    for (unsigned byte = 0; byte < 6; ++byte) {
      unsigned value_byte = (unsigned)((bits >> (byte * 8)) & UINT64_C(0xff));
      if (value_byte == 0) {
        if (!nyr_printf(c->w, "\tclr\tr%u\n", first + byte))
          return false;
      } else if (!nyr_printf(c->w, "\tldi\tr16, %u\n\tmov\tr%u, r16\n",
                             value_byte, first + byte)) {
        return false;
      }
    }
    unsigned high_byte = (unsigned)(bits >> 56);
    if (high_byte == 0) {
      if (!nyr_put(c->w, "\tclr\tr17\n"))
        return false;
    } else if (!nyr_printf(c->w, "\tldi\tr16, %u\n\tmov\tr17, r16\n",
                           high_byte)) {
      return false;
    }
    unsigned middle_byte = (unsigned)((bits >> 48) & UINT64_C(0xff));
    if (middle_byte == 0)
      return nyr_put(c->w, "\tclr\tr16\n");
    return nyr_printf(c->w, "\tldi\tr16, %u\n", middle_byte);
  }
  case NY_PORT_WASM:
    return nyr_printf(c->w, "\ti64.const %" PRId64 " ;; -> %s\n", value, reg);
  }
  return false;
}

static bool nyr_port_avr_negate_i64(nyr_port_ctx_t *c, unsigned first_reg) {
  for (unsigned byte = 0; byte < 8; ++byte)
    if (!nyr_printf(c->w, "\tcom\tr%u\n", first_reg + byte))
      return false;
  if (!nyr_printf(c->w, "\tldi\tr27, 1\n\tadd\tr%u, r27\n\tldi\tr27, 0\n",
                  first_reg))
    return false;
  for (unsigned byte = 1; byte < 8; ++byte)
    if (!nyr_printf(c->w, "\tadc\tr%u, r27\n", first_reg + byte))
      return false;
  return true;
}

static bool nyr_port_avr_divmod_i64(nyr_port_ctx_t *c, const nyr_inst_t *in,
                                    bool remainder) {
  size_t id = c->current_index;
  if (!nyr_port_load_value(c, c->tmp0, in->a) ||
      !nyr_port_load_value(c, c->tmp1, in->b))
    return false;
  for (unsigned reg = 2; reg <= 9; ++reg)
    if (!nyr_printf(c->w, "\tpush\tr%u\n", reg))
      return false;
  if (!nyr_put(c->w, "\tclr\tr26\n"
                     "\tsbrs\tr25, 7\n") ||
      !nyr_printf(c->w, "\trjmp\t.Lny_div_lhs_pos_%s_%zu\n", c->name, id) ||
      !nyr_put(c->w, "\tldi\tr26, 1\n") || !nyr_port_avr_negate_i64(c, 18) ||
      !nyr_printf(c->w,
                  ".Lny_div_lhs_pos_%s_%zu:\n"
                  "\tsbrs\tr17, 7\n"
                  "\trjmp\t.Lny_div_rhs_pos_%s_%zu\n",
                  c->name, id, c->name, id))
    return false;
  if (!remainder && !nyr_put(c->w, "\tldi\tr27, 1\n\teor\tr26, r27\n"))
    return false;
  if (!nyr_port_avr_negate_i64(c, 10))
    return false;
  if (!nyr_printf(c->w,
                  ".Lny_div_rhs_pos_%s_%zu:\n"
                  "\tclr\tr2\n\tclr\tr3\n\tclr\tr4\n\tclr\tr5\n"
                  "\tclr\tr6\n\tclr\tr7\n\tclr\tr8\n\tclr\tr9\n"
                  "\tldi\tr27, 64\n"
                  ".Lny_div_loop_%s_%zu:\n"
                  "\tlsl\tr18\n\trol\tr19\n\trol\tr20\n\trol\tr21\n"
                  "\trol\tr22\n\trol\tr23\n\trol\tr24\n\trol\tr25\n"
                  "\trol\tr2\n\trol\tr3\n\trol\tr4\n\trol\tr5\n"
                  "\trol\tr6\n\trol\tr7\n\trol\tr8\n\trol\tr9\n",
                  c->name, id, c->name, id))
    return false;
  for (int byte = 7; byte >= 0; --byte)
    if (!nyr_printf(c->w,
                    "\tcp\tr%d, r%d\n"
                    "\tbrlo\t.Lny_div_skip_%s_%zu\n"
                    "\tbrne\t.Lny_div_sub_%s_%zu\n",
                    2 + byte, 10 + byte, c->name, id, c->name, id))
      return false;
  if (!nyr_printf(c->w,
                  ".Lny_div_sub_%s_%zu:\n"
                  "\tsub\tr2, r10\n\tsbc\tr3, r11\n\tsbc\tr4, r12\n"
                  "\tsbc\tr5, r13\n\tsbc\tr6, r14\n\tsbc\tr7, r15\n"
                  "\tsbc\tr8, r16\n\tsbc\tr9, r17\n"
                  "\tori\tr18, 1\n"
                  ".Lny_div_skip_%s_%zu:\n"
                  "\tdec\tr27\n\tbrne\t.Lny_div_loop_%s_%zu\n",
                  c->name, id, c->name, id, c->name, id))
    return false;
  if (remainder) {
    if (!nyr_printf(c->w,
                    "\ttst\tr26\n"
                    "\tbreq\t.Lny_div_result_pos_%s_%zu\n",
                    c->name, id) ||
        !nyr_port_avr_negate_i64(c, 2) ||
        !nyr_printf(c->w,
                    ".Lny_div_result_pos_%s_%zu:\n"
                    "\tmov\tr18, r2\n\tmov\tr19, r3\n"
                    "\tmov\tr20, r4\n\tmov\tr21, r5\n"
                    "\tmov\tr22, r6\n\tmov\tr23, r7\n"
                    "\tmov\tr24, r8\n\tmov\tr25, r9\n",
                    c->name, id))
      return false;
  } else if (!nyr_printf(c->w,
                         "\ttst\tr26\n"
                         "\tbreq\t.Lny_div_result_pos_%s_%zu\n",
                         c->name, id) ||
             !nyr_port_avr_negate_i64(c, 18) ||
             !nyr_printf(c->w, ".Lny_div_result_pos_%s_%zu:\n", c->name, id)) {
    return false;
  }
  for (unsigned reg = 10; reg > 2; --reg)
    if (!nyr_printf(c->w, "\tpop\tr%u\n", reg - 1))
      return false;
  return nyr_port_store_value(c, in->dst, c->tmp0);
}

static bool nyr_port_binop(nyr_port_ctx_t *c, const nyr_inst_t *in,
                           const char *bpf, const char *mips, const char *ppc,
                           const char *avr_helper, const char *wasm) {
  (void)avr_helper;
  if (c->kind == NY_PORT_AVR && strcmp(bpf, "s/") == 0)
    return nyr_port_avr_divmod_i64(c, in, false);
  if (c->kind == NY_PORT_AVR && strcmp(bpf, "s%") == 0)
    return nyr_port_avr_divmod_i64(c, in, true);
  if (!nyr_port_load_value(c, c->tmp0, in->a) ||
      !nyr_port_load_value(c, c->tmp1, in->b))
    return false;
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!nyr_printf(c->w, "\t%s %s= %s\n", c->tmp0, bpf, c->tmp1))
      return false;
    break;
  case NY_PORT_MIPS:
    if (!nyr_printf(c->w, "\t%s\t%s, %s, %s\n", mips, c->tmp0, c->tmp0,
                    c->tmp1))
      return false;
    break;
  case NY_PORT_POWERPC:
    if (strcmp(ppc, "subf") == 0) {
      if (!nyr_printf(
              c->w, "\tsubf\t%s, %s, %s\n", nyr_port_reg_name(c, c->tmp0),
              nyr_port_reg_name(c, c->tmp1), nyr_port_reg_name(c, c->tmp0)))
        return false;
    } else if (!nyr_printf(c->w, "\t%s\t%s, %s, %s\n", ppc,
                           nyr_port_reg_name(c, c->tmp0),
                           nyr_port_reg_name(c, c->tmp0),
                           nyr_port_reg_name(c, c->tmp1)))
      return false;
    break;
  case NY_PORT_AVR:
    if (strcmp(bpf, "*") == 0)
      return nyr_port_avr_mul_i64(c, in);
    if (strcmp(bpf, "<<") == 0 || strcmp(bpf, "s>>") == 0) {
      if (!nyr_printf(c->w,
                      "\tmov\tr27, r10\n"
                      "\tandi\tr27, 63\n"
                      "\ttst\tr27\n"
                      "\tbreq\t.Lny_shift_done_%s_%zu\n"
                      ".Lny_shift_loop_%s_%zu:\n",
                      c->name, c->current_index, c->name, c->current_index))
        return false;
      if (strcmp(bpf, "<<") == 0) {
        if (!nyr_put(c->w, "\tlsl\tr18\n"
                           "\trol\tr19\n"
                           "\trol\tr20\n"
                           "\trol\tr21\n"
                           "\trol\tr22\n"
                           "\trol\tr23\n"
                           "\trol\tr24\n"
                           "\trol\tr25\n"))
          return false;
      } else if (!nyr_put(c->w, "\tasr\tr25\n"
                                "\tror\tr24\n"
                                "\tror\tr23\n"
                                "\tror\tr22\n"
                                "\tror\tr21\n"
                                "\tror\tr20\n"
                                "\tror\tr19\n"
                                "\tror\tr18\n")) {
        return false;
      }
      return nyr_printf(c->w,
                        "\tdec\tr27\n"
                        "\tbrne\t.Lny_shift_loop_%s_%zu\n"
                        ".Lny_shift_done_%s_%zu:\n",
                        c->name, c->current_index, c->name, c->current_index) &&
             nyr_port_store_value(c, in->dst, c->tmp0);
    }
    if (strcmp(bpf, "+") == 0 || strcmp(bpf, "-") == 0) {
      for (unsigned byte = 0; byte < 8; ++byte) {
        const char *op = strcmp(bpf, "+") == 0 ? (byte == 0 ? "add" : "adc")
                                               : (byte == 0 ? "sub" : "sbc");
        if (!nyr_printf(c->w, "\t%s\tr%u, r%u\n", op, 18 + byte, 10 + byte))
          return false;
      }
    } else {
      const char *op = strcmp(bpf, "&") == 0   ? "and"
                       : strcmp(bpf, "|") == 0 ? "or"
                       : strcmp(bpf, "^") == 0 ? "eor"
                                               : NULL;
      if (!op) {
        nyr_set_error(c->err, c->err_len,
                      "%s NYIR emit: unsupported AVR i64 binary operation",
                      c->pretty);
        return false;
      }
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w, "\t%s\tr%u, r%u\n", op, 18 + byte, 10 + byte))
          return false;
    }
    break;
  case NY_PORT_WASM:
    if (!nyr_printf(c->w, "\t%s\n", wasm))
      return false;
    break;
  }
  return nyr_port_store_value(c, in->dst, c->tmp0);
}

static bool nyr_port_ror_i64(nyr_port_ctx_t *c, const nyr_inst_t *in) {
  if (c->kind == NY_PORT_AVR) {
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->b) ||
        !nyr_printf(c->w,
                    "\tmov\tr27, r10\n"
                    "\tandi\tr27, 63\n"
                    "\ttst\tr27\n"
                    "\tbreq\t.Lny_ror_done_%s_%zu\n"
                    ".Lny_ror_loop_%s_%zu:\n"
                    "\tbst\tr18, 0\n"
                    "\tlsr\tr25\n"
                    "\tror\tr24\n"
                    "\tror\tr23\n"
                    "\tror\tr22\n"
                    "\tror\tr21\n"
                    "\tror\tr20\n"
                    "\tror\tr19\n"
                    "\tror\tr18\n"
                    "\tbld\tr25, 7\n"
                    "\tdec\tr27\n"
                    "\tbrne\t.Lny_ror_loop_%s_%zu\n"
                    ".Lny_ror_done_%s_%zu:\n",
                    c->name, c->current_index, c->name, c->current_index,
                    c->name, c->current_index, c->name, c->current_index))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  if (c->kind == NY_PORT_BPF) {
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->b) ||
        !nyr_put(c->w, "\tr7 &= 63\n"
                       "\tr0 = r6\n"
                       "\tr0 >>= r7\n"
                       "\tr1 = 0\n"
                       "\tr1 -= r7\n"
                       "\tr1 &= 63\n"
                       "\tr6 <<= r1\n"
                       "\tr6 |= r0\n"))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  if (c->kind == NY_PORT_MIPS || c->kind == NY_PORT_POWERPC) {
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->b))
      return false;
    if (c->kind == NY_PORT_MIPS) {
      if (!nyr_printf(c->w, "\tdrotrv\t%s, %s, %s\n", c->tmp0, c->tmp0,
                      c->tmp1))
        return false;
    } else if (!nyr_printf(
                   c->w, "\tneg\t%s, %s\n\trotld\t%s, %s, %s\n",
                   nyr_port_reg_name(c, c->tmp1), nyr_port_reg_name(c, c->tmp1),
                   nyr_port_reg_name(c, c->tmp0), nyr_port_reg_name(c, c->tmp0),
                   nyr_port_reg_name(c, c->tmp1))) {
      return false;
    }
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  return nyr_port_binop(c, in, ">>", "dsrav", "srad", "__ny_avr_i64_ror",
                        "i64.rotr");
}

static bool nyr_port_ror32_i64(nyr_port_ctx_t *c, const nyr_inst_t *in) {
  if (c->kind == NY_PORT_BPF) {
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->b) ||
        !nyr_put(c->w, "\tr6 &= 0xffffffff\n"
                       "\tr7 &= 31\n"
                       "\tr0 = r6\n"
                       "\tr0 >>= r7\n"
                       "\tr1 = 32\n"
                       "\tr1 -= r7\n"
                       "\tr6 <<= r1\n"
                       "\tr6 |= r0\n"
                       "\tr6 &= 0xffffffff\n"))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  if (c->kind == NY_PORT_AVR) {
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->b) ||
        !nyr_printf(c->w,
                    "\tclr\tr22\n"
                    "\tclr\tr23\n"
                    "\tclr\tr24\n"
                    "\tclr\tr25\n"
                    "\tmov\tr27, r10\n"
                    "\tandi\tr27, 31\n"
                    "\ttst\tr27\n"
                    "\tbreq\t.Lny_ror32_done_%s_%zu\n"
                    ".Lny_ror32_loop_%s_%zu:\n"
                    "\tbst\tr18, 0\n"
                    "\tlsr\tr21\n"
                    "\tror\tr20\n"
                    "\tror\tr19\n"
                    "\tror\tr18\n"
                    "\tbld\tr21, 7\n"
                    "\tdec\tr27\n"
                    "\tbrne\t.Lny_ror32_loop_%s_%zu\n"
                    ".Lny_ror32_done_%s_%zu:\n",
                    c->name, c->current_index, c->name, c->current_index,
                    c->name, c->current_index, c->name, c->current_index))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  if (c->kind == NY_PORT_MIPS) {
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->b) ||
        !nyr_printf(c->w,
                    "\tdsll32\t$t2, %s, 0\n"
                    "\tdsll32\t%s, %s, 0\n"
                    "\tdsrl32\t%s, %s, 0\n"
                    "\tor\t%s, %s, $t2\n"
                    "\tandi\t%s, %s, 31\n"
                    "\tdrotrv\t%s, %s, %s\n"
                    "\tdsll32\t%s, %s, 0\n"
                    "\tdsrl32\t%s, %s, 0\n",
                    c->tmp0, c->tmp0, c->tmp0, c->tmp0, c->tmp0, c->tmp0,
                    c->tmp0, c->tmp1, c->tmp1, c->tmp0, c->tmp0, c->tmp1,
                    c->tmp0, c->tmp0, c->tmp0, c->tmp0))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  if (c->kind == NY_PORT_POWERPC) {
    const char *value = nyr_port_reg_name(c, c->tmp0);
    const char *count = nyr_port_reg_name(c, c->tmp1);
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->b) ||
        !nyr_printf(c->w,
                    "\trldicl\t0, %s, 0, 32\n"
                    "\trldicl\t%s, %s, 0, 32\n"
                    "\tsldi\t0, 0, 32\n"
                    "\tor\t%s, %s, 0\n"
                    "\tandi.\t%s, %s, 31\n"
                    "\tneg\t%s, %s\n"
                    "\trotld\t%s, %s, %s\n"
                    "\trldicl\t%s, %s, 0, 32\n",
                    value, value, value, value, value, count, count, count,
                    count, value, value, count, value, value))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  return false;
}

static bool nyr_port_select_i64(nyr_port_ctx_t *c, const nyr_inst_t *in) {
  if (c->kind == NY_PORT_WASM) {
    return nyr_port_load_value(c, "$t0", in->b) &&
           nyr_port_load_value(c, "$t0", in->c) &&
           nyr_port_load_value(c, "$t0", in->a) &&
           nyr_put(c->w, "\ti64.const 0\n\ti64.ne\n\tselect\n") &&
           nyr_port_store_value(c, in->dst, "$t0");
  }
  if (c->kind == NY_PORT_BPF) {
    size_t select_id =
        c->nyir && c->nyir->data ? (size_t)(in - c->nyir->data) : 0;
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_printf(c->w, "\tif %s != 0 goto .Lny_select_true_%s_%zu\n",
                    c->tmp0, c->name, select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->c) ||
        !nyr_printf(c->w,
                    "\tgoto .Lny_select_done_%s_%zu\n"
                    ".Lny_select_true_%s_%zu:\n",
                    c->name, select_id, c->name, select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->b) ||
        !nyr_printf(c->w, ".Lny_select_done_%s_%zu:\n", c->name, select_id))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  if (c->kind == NY_PORT_AVR) {
    size_t select_id =
        c->nyir && c->nyir->data ? (size_t)(in - c->nyir->data) : 0;
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_put(c->w, "\tclr\tr0\n"))
      return false;
    for (unsigned byte = 0; byte < 8; ++byte)
      if (!nyr_printf(c->w, "\tor\tr0, r%u\n", 18 + byte))
        return false;
    if (!nyr_printf(c->w,
                    "\tbreq\t.Lny_select_false_%s_%zu\n"
                    "\trjmp\t.Lny_select_true_%s_%zu\n"
                    ".Lny_select_false_%s_%zu:\n",
                    c->name, select_id, c->name, select_id, c->name,
                    select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->c) ||
        !nyr_printf(c->w,
                    "\trjmp\t.Lny_select_done_%s_%zu\n"
                    ".Lny_select_true_%s_%zu:\n",
                    c->name, select_id, c->name, select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->b) ||
        !nyr_printf(c->w, ".Lny_select_done_%s_%zu:\n", c->name, select_id))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  if (c->kind != NY_PORT_MIPS && c->kind != NY_PORT_POWERPC) {
    nyr_set_error(c->err, c->err_len, "%s NYIR emit: select.i64 is unsupported",
                  c->pretty);
    return false;
  }
  size_t select_id =
      c->nyir && c->nyir->data ? (size_t)(in - c->nyir->data) : 0;
  if (!nyr_port_load_value(c, c->tmp0, in->a))
    return false;
  if (c->kind == NY_PORT_MIPS) {
    if (!nyr_printf(c->w,
                    "\tbnez\t%s, .Lny_select_true_%s_%zu\n"
                    "\tnop\n",
                    c->tmp0, c->name, select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->c) ||
        !nyr_printf(c->w,
                    "\tb\t.Lny_select_done_%s_%zu\n"
                    "\tnop\n"
                    ".Lny_select_true_%s_%zu:\n",
                    c->name, select_id, c->name, select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->b) ||
        !nyr_printf(c->w, ".Lny_select_done_%s_%zu:\n", c->name, select_id))
      return false;
  } else {
    const char *cond = nyr_port_reg_name(c, c->tmp0);
    if (!nyr_printf(c->w,
                    "\tcmpdi\t%s, 0\n"
                    "\tbeq\t.Lny_select_false_%s_%zu\n",
                    cond, c->name, select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->b) ||
        !nyr_printf(c->w,
                    "\tb\t.Lny_select_done_%s_%zu\n"
                    ".Lny_select_false_%s_%zu:\n",
                    c->name, select_id, c->name, select_id) ||
        !nyr_port_load_value(c, c->tmp0, in->c) ||
        !nyr_printf(c->w, ".Lny_select_done_%s_%zu:\n", c->name, select_id))
      return false;
  }
  return nyr_port_store_value(c, in->dst, c->tmp0);
}

static const char *nyr_port_cmp_name(nyr_cmp_t cmp) {
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

static bool nyr_port_bswap(nyr_port_ctx_t *c, const nyr_inst_t *in,
                           bool is32) {
  if (c->kind != NY_PORT_WASM && !nyr_port_load_value(c, c->tmp0, in->a))
    return false;
  if (c->kind == NY_PORT_WASM) {
    for (int byte = 0; byte < (is32 ? 4 : 8); ++byte) {
      if (!nyr_port_load_value(c, c->tmp0, in->a))
        return false;
      if (is32) {
        int shift = byte * 8;
        if (!nyr_put(c->w, "\ti32.wrap_i64\n\ti32.const 255\n") ||
            (shift &&
             !nyr_printf(c->w, "\ti32.const %d\n\ti32.shl\n", shift)) ||
            !nyr_put(c->w, "\ti32.and\n"))
          return false;
        int result_shift = (3 - 2 * byte) * 8;
        if (result_shift > 0) {
          if (!nyr_printf(c->w, "\ti32.const %d\n\ti32.shl\n", result_shift))
            return false;
        } else if (result_shift < 0 &&
                   !nyr_printf(c->w, "\ti32.const %d\n\ti32.shr_u\n",
                               -result_shift)) {
          return false;
        }
        if (byte > 0 && !nyr_put(c->w, "\ti32.or\n"))
          return false;
      } else {
        int shift = byte * 8;
        if (!nyr_put(c->w, "\ti64.const 255\n") ||
            (shift &&
             !nyr_printf(c->w, "\ti64.const %d\n\ti64.shl\n", shift)) ||
            !nyr_put(c->w, "\ti64.and\n"))
          return false;
        int result_shift = (7 - 2 * byte) * 8;
        if (result_shift > 0) {
          if (!nyr_printf(c->w, "\ti64.const %d\n\ti64.shl\n", result_shift))
            return false;
        } else if (result_shift < 0 &&
                   !nyr_printf(c->w, "\ti64.const %d\n\ti64.shr_u\n",
                               -result_shift)) {
          return false;
        }
        if (byte > 0 && !nyr_put(c->w, "\ti64.or\n"))
          return false;
      }
    }
    if (is32 && !nyr_put(c->w, "\ti64.extend_i32_u\n"))
      return false;
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!nyr_printf(c->w, "\t%s = be%d %s\n", c->tmp0, is32 ? 32 : 64, c->tmp0))
      return false;
    break;
  case NY_PORT_MIPS:
    if (is32) {
      if (!nyr_printf(c->w, "\twsbh\t%s, %s\n\trotr\t%s, %s, 16\n", c->tmp0,
                      c->tmp0, c->tmp0, c->tmp0))
        return false;
    } else if (!nyr_printf(c->w, "\tdsbh\t%s, %s\n\tdshd\t%s, %s\n", c->tmp0,
                           c->tmp0, c->tmp0, c->tmp0))
      return false;
    break;
  case NY_PORT_POWERPC: {
    const char *reg = nyr_port_reg_name(c, c->tmp0);
    const char *source = nyr_port_reg_name(c, c->tmp1);
    const char *scratch = "6";
    if (is32) {
      if (!nyr_printf(c->w,
                      "\tmr\t%s, %s\n"
                      "\trlwinm\t%s, %s, 24, 0, 7\n"
                      "\trlwimi\t%s, %s, 8, 8, 15\n"
                      "\trlwimi\t%s, %s, 24, 16, 23\n"
                      "\trlwimi\t%s, %s, 8, 24, 31\n"
                      "\tclrldi\t%s, %s, 32\n",
                      source, reg, reg, source, reg, source, reg, source, reg,
                      source, reg, reg))
        return false;
    } else {
      if (!nyr_printf(c->w, "\tli\t%s, 0\n", source))
        return false;
      for (int byte = 0; byte < 8; ++byte) {
        int shift_in = byte * 8;
        int shift_out = (7 - byte) * 8;
        if (shift_in == 0) {
          if (!nyr_printf(c->w, "\tmr\t%s, %s\n", scratch, reg))
            return false;
        } else if (!nyr_printf(c->w, "\tsrdi\t%s, %s, %d\n", scratch, reg,
                               shift_in)) {
          return false;
        }
        if (!nyr_printf(c->w, "\tandi.\t%s, %s, 255\n", scratch, scratch))
          return false;
        if (shift_out && !nyr_printf(c->w, "\tsldi\t%s, %s, %d\n", scratch,
                                     scratch, shift_out))
          return false;
        if (!nyr_printf(c->w, "\tor\t%s, %s, %s\n", source, source, scratch))
          return false;
      }
      if (!nyr_printf(c->w, "\tmr\t%s, %s\n", reg, source))
        return false;
    }
    break;
  }
  case NY_PORT_AVR:
    if (is32) {
      if (!nyr_put(c->w, "\tmov\tr0, r18\n"
                         "\tmov\tr18, r21\n"
                         "\tmov\tr21, r0\n"
                         "\tmov\tr0, r19\n"
                         "\tmov\tr19, r20\n"
                         "\tmov\tr20, r0\n"
                         "\tclr\tr22\n"
                         "\tclr\tr23\n"
                         "\tclr\tr24\n"
                         "\tclr\tr25\n"))
        return false;
    } else if (!nyr_put(c->w, "\tmov\tr0, r18\n"
                              "\tmov\tr18, r25\n"
                              "\tmov\tr25, r0\n"
                              "\tmov\tr0, r19\n"
                              "\tmov\tr19, r24\n"
                              "\tmov\tr24, r0\n"
                              "\tmov\tr0, r20\n"
                              "\tmov\tr20, r23\n"
                              "\tmov\tr23, r0\n"
                              "\tmov\tr0, r21\n"
                              "\tmov\tr21, r22\n"
                              "\tmov\tr22, r0\n"))
      return false;
    break;
  case NY_PORT_WASM:
    /*
     * The portable WAT backend uses a helper until scalar bswap is in core
     * wasm.
     */
    if (!nyr_printf(c->w, "\tcall $__ny_wasm_bswap%d\n", is32 ? 32 : 64))
      return false;
    break;
  }
  return nyr_port_store_value(c, in->dst, c->tmp0);
}

static bool nyr_port_wasm_float_operand(nyr_port_ctx_t *c, int value,
                                        bool is_f32) {
  return nyr_port_load_value(c, c->tmp0, value) &&
         nyr_put(c->w, is_f32 ? "\ti32.wrap_i64\n\tf32.reinterpret_i32\n"
                              : "\tf64.reinterpret_i64\n");
}

static bool nyr_port_wasm_float_result(nyr_port_ctx_t *c, int dst,
                                       bool is_f32) {
  if (!nyr_put(c->w, is_f32 ? "\ti32.reinterpret_f32\n\ti64.extend_i32_u\n"
                            : "\ti64.reinterpret_f64\n"))
    return false;
  return nyr_port_store_value(c, dst, c->tmp0);
}

static bool nyr_port_wasm_float_binary(nyr_port_ctx_t *c, const nyr_inst_t *in,
                                       bool is_f32) {
  const char *op = NULL;
  switch (in->op) {
  case NYIR_ADD_F64:
  case NYIR_ADD_F32:
    op = "add";
    break;
  case NYIR_SUB_F64:
  case NYIR_SUB_F32:
    op = "sub";
    break;
  case NYIR_MUL_F64:
  case NYIR_MUL_F32:
    op = "mul";
    break;
  case NYIR_DIV_F64:
  case NYIR_DIV_F32:
    op = "div";
    break;
  default:
    return false;
  }
  return nyr_port_wasm_float_operand(c, in->a, is_f32) &&
         nyr_port_wasm_float_operand(c, in->b, is_f32) &&
         nyr_printf(c->w, "\t%s.%s\n", is_f32 ? "f32" : "f64", op) &&
         nyr_port_wasm_float_result(c, in->dst, is_f32);
}

static bool nyr_port_wasm_float_cmp(nyr_port_ctx_t *c, const nyr_inst_t *in,
                                    bool is_f32) {
  const char *pred = nyr_port_cmp_name(in->cmp);
  return nyr_port_wasm_float_operand(c, in->a, is_f32) &&
         nyr_port_wasm_float_operand(c, in->b, is_f32) &&
         nyr_printf(c->w, "\t%s.%s\n\ti64.extend_i32_u\n",
                    is_f32 ? "f32" : "f64", pred) &&
         nyr_port_store_value(c, in->dst, c->tmp0);
}

static bool nyr_port_avr_float_cmp(nyr_port_ctx_t *c, const nyr_inst_t *in,
                                   bool is_f32) {
  const size_t id = c->nyir && c->nyir->data ? (size_t)(in - c->nyir->data) : 0;
  if (!nyr_port_load_value(c, c->tmp0, in->a) ||
      !nyr_port_load_value(c, c->tmp1, in->b))
    return false;
  if (is_f32) {
    if (!nyr_printf(c->w,
                    "\tmov\tr26, r21\n"
                    "\tandi\tr26, 0x7f\n"
                    "\tcpi\tr26, 0x7f\n"
                    "\tbrne\t.Lny_cmp_finite_l_%s_%zu\n"
                    "\tsbrs\tr20, 7\n"
                    "\trjmp\t.Lny_cmp_finite_l_%s_%zu\n"
                    "\tmov\tr26, r20\n"
                    "\tandi\tr26, 0x7f\n"
                    "\tor\tr26, r19\n"
                    "\tor\tr26, r18\n"
                    "\tbreq\t.Lny_cmp_finite_l_%s_%zu\n"
                    "\trjmp\t.Lny_cmp_unordered_%s_%zu\n"
                    ".Lny_cmp_finite_l_%s_%zu:\n"
                    "\tmov\tr26, r13\n"
                    "\tandi\tr26, 0x7f\n"
                    "\tcpi\tr26, 0x7f\n"
                    "\tbrne\t.Lny_cmp_finite_r_%s_%zu\n"
                    "\tsbrs\tr12, 7\n"
                    "\trjmp\t.Lny_cmp_finite_r_%s_%zu\n"
                    "\tmov\tr26, r12\n"
                    "\tandi\tr26, 0x7f\n"
                    "\tor\tr26, r11\n"
                    "\tor\tr26, r10\n"
                    "\tbreq\t.Lny_cmp_finite_r_%s_%zu\n"
                    "\trjmp\t.Lny_cmp_unordered_%s_%zu\n"
                    ".Lny_cmp_finite_r_%s_%zu:\n",
                    c->name, id, c->name, id, c->name, id, c->name, id, c->name,
                    id, c->name, id, c->name, id, c->name, id, c->name, id,
                    c->name, id))
      return false;
    if (!nyr_printf(c->w,
                    "\tclr\tr22\n\tclr\tr23\n\tclr\tr24\n\tclr\tr25\n"
                    "\tclr\tr14\n\tclr\tr15\n\tclr\tr16\n\tclr\tr17\n"
                    "\tmov\tr26, r21\n\tandi\tr26, 0x7f\n"
                    "\tor\tr26, r20\n\tor\tr26, r19\n\tor\tr26, r18\n"
                    "\tbreq\t.Lny_cmp_zero_l_%s_%zu\n"
                    "\tsbrc\tr21, 7\n\trjmp\t.Lny_cmp_neg_l_%s_%zu\n"
                    "\tldi\tr26, 0x80\n\teor\tr21, r26\n"
                    "\trjmp\t.Lny_cmp_rhs_%s_%zu\n"
                    ".Lny_cmp_neg_l_%s_%zu:\n"
                    "\tcom\tr18\n\tcom\tr19\n\tcom\tr20\n\tcom\tr21\n"
                    ".Lny_cmp_rhs_%s_%zu:\n"
                    "\tmov\tr26, r13\n\tandi\tr26, 0x7f\n"
                    "\tor\tr26, r12\n\tor\tr26, r11\n\tor\tr26, r10\n"
                    "\tbreq\t.Lny_cmp_zero_b_%s_%zu\n"
                    "\tsbrc\tr13, 7\n\trjmp\t.Lny_cmp_neg_r_%s_%zu\n"
                    "\tldi\tr26, 0x80\n\teor\tr13, r26\n"
                    "\trjmp\t.Lny_cmp_compare_%s_%zu\n"
                    ".Lny_cmp_neg_r_%s_%zu:\n"
                    "\tcom\tr10\n\tcom\tr11\n\tcom\tr12\n\tcom\tr13\n"
                    "\trjmp\t.Lny_cmp_compare_%s_%zu\n"
                    ".Lny_cmp_zero_l_%s_%zu:\n"
                    "\tldi\tr26, 0x80\n\tmov\tr21, r26\n"
                    "\trjmp\t.Lny_cmp_rhs_%s_%zu\n"
                    ".Lny_cmp_zero_b_%s_%zu:\n"
                    "\tldi\tr26, 0x80\n\tmov\tr13, r26\n"
                    ".Lny_cmp_compare_%s_%zu:\n",
                    c->name, id, c->name, id, c->name, id, c->name, id, c->name,
                    id, c->name, id, c->name, id, c->name, id, c->name, id,
                    c->name, id, c->name, id, c->name, id, c->name, id, c->name,
                    id))
      return false;
  } else {
    if (!nyr_printf(c->w,
                    "\tmov\tr26, r25\n"
                    "\tandi\tr26, 0x7f\n"
                    "\tcpi\tr26, 0x7f\n"
                    "\tbrne\t.Lny_cmp_finite_l_%s_%zu\n"
                    "\tmov\tr26, r24\n"
                    "\tandi\tr26, 0x0f\n"
                    "\tor\tr26, r23\n"
                    "\tor\tr26, r22\n"
                    "\tor\tr26, r21\n"
                    "\tor\tr26, r20\n"
                    "\tor\tr26, r19\n"
                    "\tor\tr26, r18\n"
                    "\tbreq\t.Lny_cmp_finite_l_%s_%zu\n"
                    "\trjmp\t.Lny_cmp_unordered_%s_%zu\n"
                    ".Lny_cmp_finite_l_%s_%zu:\n"
                    "\tmov\tr26, r17\n"
                    "\tandi\tr26, 0x7f\n"
                    "\tcpi\tr26, 0x7f\n"
                    "\tbrne\t.Lny_cmp_finite_r_%s_%zu\n"
                    "\tmov\tr26, r16\n"
                    "\tandi\tr26, 0x0f\n"
                    "\tor\tr26, r15\n"
                    "\tor\tr26, r14\n"
                    "\tor\tr26, r13\n"
                    "\tor\tr26, r12\n"
                    "\tor\tr26, r11\n"
                    "\tor\tr26, r10\n"
                    "\tbreq\t.Lny_cmp_finite_r_%s_%zu\n"
                    "\trjmp\t.Lny_cmp_unordered_%s_%zu\n"
                    ".Lny_cmp_finite_r_%s_%zu:\n",
                    c->name, id, c->name, id, c->name, id, c->name, id, c->name,
                    id, c->name, id, c->name, id, c->name, id))
      return false;
    if (!nyr_printf(c->w,
                    "\tmov\tr26, r25\n\tandi\tr26, 0x7f\n"
                    "\tor\tr26, r24\n\tor\tr26, r23\n\tor\tr26, r22\n"
                    "\tor\tr26, r21\n\tor\tr26, r20\n\tor\tr26, r19\n"
                    "\tor\tr26, r18\n\tbreq\t.Lny_cmp_zero_l_%s_%zu\n"
                    "\tsbrc\tr25, 7\n\trjmp\t.Lny_cmp_neg_l_%s_%zu\n"
                    "\tldi\tr26, 0x80\n\teor\tr25, r26\n"
                    "\trjmp\t.Lny_cmp_rhs_%s_%zu\n"
                    ".Lny_cmp_neg_l_%s_%zu:\n"
                    "\tcom\tr18\n\tcom\tr19\n\tcom\tr20\n\tcom\tr21\n"
                    "\tcom\tr22\n\tcom\tr23\n\tcom\tr24\n\tcom\tr25\n"
                    ".Lny_cmp_rhs_%s_%zu:\n"
                    "\tmov\tr26, r17\n\tandi\tr26, 0x7f\n"
                    "\tor\tr26, r16\n\tor\tr26, r15\n\tor\tr26, r14\n"
                    "\tor\tr26, r13\n\tor\tr26, r12\n\tor\tr26, r11\n"
                    "\tor\tr26, r10\n\tbreq\t.Lny_cmp_zero_b_%s_%zu\n"
                    "\tsbrc\tr17, 7\n\trjmp\t.Lny_cmp_neg_r_%s_%zu\n"
                    "\tldi\tr26, 0x80\n\teor\tr17, r26\n"
                    "\trjmp\t.Lny_cmp_compare_%s_%zu\n"
                    ".Lny_cmp_neg_r_%s_%zu:\n"
                    "\tcom\tr10\n\tcom\tr11\n\tcom\tr12\n\tcom\tr13\n"
                    "\tcom\tr14\n\tcom\tr15\n\tcom\tr16\n\tcom\tr17\n"
                    "\trjmp\t.Lny_cmp_compare_%s_%zu\n"
                    ".Lny_cmp_zero_l_%s_%zu:\n"
                    "\tldi\tr26, 0x80\n\tmov\tr25, r26\n"
                    "\trjmp\t.Lny_cmp_rhs_%s_%zu\n"
                    ".Lny_cmp_zero_b_%s_%zu:\n"
                    "\tldi\tr26, 0x80\n\tmov\tr17, r26\n"
                    ".Lny_cmp_compare_%s_%zu:\n",
                    c->name, id, c->name, id, c->name, id, c->name, id, c->name,
                    id, c->name, id, c->name, id, c->name, id, c->name, id,
                    c->name, id, c->name, id, c->name, id, c->name, id, c->name,
                    id))
      return false;
  }
  if (!nyr_printf(c->w,
                  "\tcp\tr18, r10\n"
                  "\tcpc\tr19, r11\n"
                  "\tcpc\tr20, r12\n"
                  "\tcpc\tr21, r13\n"
                  "%s"
                  "\tbrlo\t.Lny_cmp_less_%s_%zu\n"
                  "\tbreq\t.Lny_cmp_equal_%s_%zu\n"
                  "\trjmp\t.Lny_cmp_greater_%s_%zu\n"
                  ".Lny_cmp_less_%s_%zu:\n"
                  "\tldi\tr18, %d\n"
                  "\trjmp\t.Lny_cmp_done_%s_%zu\n"
                  ".Lny_cmp_equal_%s_%zu:\n"
                  "\tldi\tr18, %d\n"
                  "\trjmp\t.Lny_cmp_done_%s_%zu\n"
                  ".Lny_cmp_greater_%s_%zu:\n"
                  "\tldi\tr18, %d\n"
                  "\trjmp\t.Lny_cmp_done_%s_%zu\n"
                  ".Lny_cmp_unordered_%s_%zu:\n"
                  "\tldi\tr18, %d\n"
                  ".Lny_cmp_done_%s_%zu:\n"
                  "\tclr\tr19\n\tclr\tr20\n\tclr\tr21\n"
                  "\tclr\tr22\n\tclr\tr23\n\tclr\tr24\n\tclr\tr25\n",
                  is_f32 ? ""
                         : "\tcpc\tr22, r14\n\tcpc\tr23, r15\n"
                           "\tcpc\tr24, r16\n\tcpc\tr25, r17\n",
                  c->name, id, c->name, id, c->name, id, c->name, id,
                  in->cmp == NYIR_CMP_LT || in->cmp == NYIR_CMP_LE ||
                      in->cmp == NYIR_CMP_NE,
                  c->name, id, c->name, id,
                  in->cmp == NYIR_CMP_EQ || in->cmp == NYIR_CMP_LE ||
                      in->cmp == NYIR_CMP_GE,
                  c->name, id, c->name, id,
                  in->cmp == NYIR_CMP_GT || in->cmp == NYIR_CMP_GE ||
                      in->cmp == NYIR_CMP_NE,
                  c->name, id, c->name, id, in->cmp == NYIR_CMP_NE, c->name,
                  id))
    return false;
  return nyr_port_store_value(c, in->dst, c->tmp0);
}

static bool nyr_port_wasm_float_convert(nyr_port_ctx_t *c,
                                        const nyr_inst_t *in) {
  switch (in->op) {
  case NYIR_I64_TO_F64:
    return nyr_port_load_value(c, c->tmp0, in->a) &&
           nyr_put(c->w, "\tf64.convert_i64_s\n\ti64.reinterpret_f64\n") &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_F64_TO_I64:
    return nyr_port_wasm_float_operand(c, in->a, false) &&
           nyr_put(c->w, "\ti64.trunc_f64_s\n") &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_I64_TO_F32:
    return nyr_port_load_value(c, c->tmp0, in->a) &&
           nyr_put(c->w, "\tf32.convert_i64_s\n\ti32.reinterpret_f32\n"
                         "\ti64.extend_i32_u\n") &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_F32_TO_I64:
    return nyr_port_wasm_float_operand(c, in->a, true) &&
           nyr_put(c->w, "\ti64.trunc_f32_s\n") &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_F64_TO_F32:
    return nyr_port_wasm_float_operand(c, in->a, false) &&
           nyr_put(c->w, "\tf32.demote_f64\n\ti32.reinterpret_f32\n"
                         "\ti64.extend_i32_u\n") &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_F32_TO_F64:
    return nyr_port_wasm_float_operand(c, in->a, true) &&
           nyr_put(c->w, "\tf64.promote_f32\n\ti64.reinterpret_f64\n") &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  default:
    return false;
  }
}

static bool nyr_port_cmp(nyr_port_ctx_t *c, const nyr_inst_t *in) {
  if (!nyr_port_load_value(c, c->tmp0, in->a) ||
      !nyr_port_load_value(c, c->tmp1, in->b))
    return false;
  const char *pred = nyr_port_cmp_name(in->cmp);
  size_t cmp_id = c->nyir && c->nyir->data ? (size_t)(in - c->nyir->data) : 0;
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!nyr_printf(c->w,
                    "\t%s = 0\n"
                    "\tif %s %s %s goto .Lny_cmp_true_%s_%zu\n"
                    "\tgoto .Lny_cmp_done_%s_%zu\n"
                    ".Lny_cmp_true_%s_%zu:\n"
                    "\t%s = 1\n"
                    ".Lny_cmp_done_%s_%zu:\n",
                    c->tmp0, c->tmp0,
                    in->cmp == NYIR_CMP_EQ   ? "=="
                    : in->cmp == NYIR_CMP_NE ? "!="
                    : in->cmp == NYIR_CMP_LT ? "s<"
                    : in->cmp == NYIR_CMP_LE ? "s<="
                    : in->cmp == NYIR_CMP_GT ? "s>"
                                             : "s>=",
                    c->tmp1, c->name, cmp_id, c->name, cmp_id, c->name, cmp_id,
                    c->tmp0, c->name, cmp_id))
      return false;
    break;
  case NY_PORT_MIPS:
    switch (in->cmp) {
    case NYIR_CMP_EQ:
      if (!nyr_printf(c->w, "\txor\t%s, %s, %s\n\tsltiu\t%s, %s, 1\n", c->tmp0,
                      c->tmp0, c->tmp1, c->tmp0, c->tmp0))
        return false;
      break;
    case NYIR_CMP_NE:
      if (!nyr_printf(c->w, "\txor\t%s, %s, %s\n\tsltu\t%s, $zero, %s\n",
                      c->tmp0, c->tmp0, c->tmp1, c->tmp0, c->tmp0))
        return false;
      break;
    case NYIR_CMP_LT:
      if (!nyr_printf(c->w, "\tslt\t%s, %s, %s\n", c->tmp0, c->tmp0, c->tmp1))
        return false;
      break;
    case NYIR_CMP_LE:
      if (!nyr_printf(c->w, "\tslt\t%s, %s, %s\n\txori\t%s, %s, 1\n", c->tmp0,
                      c->tmp1, c->tmp0, c->tmp0, c->tmp0))
        return false;
      break;
    case NYIR_CMP_GT:
      if (!nyr_printf(c->w, "\tslt\t%s, %s, %s\n", c->tmp0, c->tmp1, c->tmp0))
        return false;
      break;
    case NYIR_CMP_GE:
      if (!nyr_printf(c->w, "\tslt\t%s, %s, %s\n\txori\t%s, %s, 1\n", c->tmp0,
                      c->tmp0, c->tmp1, c->tmp0, c->tmp0))
        return false;
      break;
    }
    break;
  case NY_PORT_POWERPC: {
    const char *branch_false = in->cmp == NYIR_CMP_EQ   ? "bne"
                               : in->cmp == NYIR_CMP_NE ? "beq"
                               : in->cmp == NYIR_CMP_LT ? "bge"
                               : in->cmp == NYIR_CMP_LE ? "bgt"
                               : in->cmp == NYIR_CMP_GT ? "ble"
                                                        : "blt";
    const char *lhs = nyr_port_reg_name(c, c->tmp0);
    const char *rhs = nyr_port_reg_name(c, c->tmp1);
    if (!nyr_printf(c->w,
                    "\tcmpd\t%s, %s\n"
                    "\tli\t%s, 0\n"
                    "\t%s\t.Lny_cmp_done_%s_%zu\n"
                    "\tli\t%s, 1\n"
                    ".Lny_cmp_done_%s_%zu:\n",
                    lhs, rhs, lhs, branch_false, c->name, cmp_id, lhs, c->name,
                    cmp_id))
      return false;
  } break;
  case NY_PORT_AVR:
    if (!nyr_put(c->w, "\tcp\tr18, r10\n"
                       "\tcpc\tr19, r11\n"
                       "\tcpc\tr20, r12\n"
                       "\tcpc\tr21, r13\n"
                       "\tcpc\tr22, r14\n"
                       "\tcpc\tr23, r15\n"
                       "\tcpc\tr24, r16\n"
                       "\tcpc\tr25, r17\n"))
      return false;
    if (in->cmp == NYIR_CMP_GT) {
      if (!nyr_printf(c->w,
                      "\tbrlt\t.Lny_cmp_false_%s_%zu\n"
                      "\tbreq\t.Lny_cmp_false_%s_%zu\n"
                      "\tldi\tr18, 1\n"
                      "\trjmp\t.Lny_cmp_done_%s_%zu\n"
                      ".Lny_cmp_false_%s_%zu:\n"
                      "\tclr\tr18\n"
                      "\trjmp\t.Lny_cmp_done_%s_%zu\n",
                      c->name, cmp_id, c->name, cmp_id, c->name, cmp_id,
                      c->name, cmp_id, c->name, cmp_id))
        return false;
    } else {
      const char *branch = in->cmp == NYIR_CMP_EQ   ? "breq"
                           : in->cmp == NYIR_CMP_NE ? "brne"
                           : in->cmp == NYIR_CMP_LT ? "brlt"
                           : in->cmp == NYIR_CMP_LE ? "brlt"
                                                    : "brge";
      if (!nyr_printf(c->w, "\t%s\t.Lny_cmp_true_%s_%zu\n", branch, c->name,
                      cmp_id) ||
          (in->cmp == NYIR_CMP_LE &&
           !nyr_printf(c->w, "\tbreq\t.Lny_cmp_true_%s_%zu\n", c->name,
                       cmp_id)) ||
          !nyr_printf(c->w,
                      "\tclr\tr18\n"
                      "\trjmp\t.Lny_cmp_done_%s_%zu\n"
                      ".Lny_cmp_true_%s_%zu:\n"
                      "\tldi\tr18, 1\n",
                      c->name, cmp_id, c->name, cmp_id))
        return false;
    }
    if (!nyr_printf(c->w,
                    ".Lny_cmp_done_%s_%zu:\n"
                    "\tclr\tr19\n"
                    "\tclr\tr20\n"
                    "\tclr\tr21\n"
                    "\tclr\tr22\n"
                    "\tclr\tr23\n"
                    "\tclr\tr24\n"
                    "\tclr\tr25\n",
                    c->name, cmp_id))
      return false;
    break;
  case NY_PORT_WASM:
    if (!nyr_printf(c->w, "\ti64.%s%s\n\ti64.extend_i32_u\n", pred,
                    (in->cmp == NYIR_CMP_LT || in->cmp == NYIR_CMP_LE ||
                     in->cmp == NYIR_CMP_GT || in->cmp == NYIR_CMP_GE)
                        ? "_s"
                        : ""))
      return false;
    break;
  }
  return nyr_port_store_value(c, in->dst, c->tmp0);
}

static bool nyr_port_label(nyr_port_ctx_t *c, int64_t label) {
  if (c->kind == NY_PORT_WASM)
    return true;
  return nyr_printf(c->w, ".Lnyir_%s_L%" PRId64 ":\n", c->name, label);
}

static bool nyr_port_wasm_label_index(nyr_port_ctx_t *c, int64_t label,
                                      size_t *index) {
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    if (in->op == NYIR_LABEL && in->imm == label) {
      *index = i;
      return true;
    }
  }
  nyr_set_error(c->err, c->err_len,
                "%s NYIR emit: branch target L%" PRId64 " is undefined",
                c->pretty, label);
  return false;
}

static bool nyr_port_wasm_set_pc(nyr_port_ctx_t *c, size_t index) {
  return nyr_printf(c->w, "\ti32.const %zu\n\tlocal.set $pc\n", index);
}

static bool nyr_port_wasm_checked_address(nyr_port_ctx_t *c, int value) {
  return nyr_port_load_value(c, c->tmp0, value) &&
         nyr_put(c->w, "\ti64.const 4294967295\n"
                       "\ti64.gt_u\n"
                       "\tif\n"
                       "\t\tunreachable\n"
                       "\tend\n") &&
         nyr_port_load_value(c, c->tmp0, value) &&
         nyr_put(c->w, "\ti32.wrap_i64\n");
}

static bool nyr_port_wasm_string_offset(nyr_port_ctx_t *c, const char *symbol,
                                        uint32_t *offset) {
  static const char prefix[] = "ny_str_";
  if (!symbol || strncmp(symbol, prefix, sizeof(prefix) - 1) != 0)
    return false;
  const char *digits = symbol + sizeof(prefix) - 1;
  if (!*digits)
    return false;
  size_t wanted = 0;
  for (const char *p = digits; *p; ++p) {
    if (*p < '0' || *p > '9' || wanted > (SIZE_MAX - (size_t)(*p - '0')) / 10)
      return false;
    wanted = wanted * 10 + (size_t)(*p - '0');
  }
  size_t address = 1024;
  for (size_t i = 0; i <= wanted; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "ny_str_%zu", i);
    size_t len = 0;
    if (!nyr_strtab_get(name, &len)) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: unknown WebAssembly data symbol %s",
                    c->pretty, symbol);
      return false;
    }
    if (i == wanted) {
      if (address > UINT32_MAX) {
        nyr_set_error(c->err, c->err_len,
                      "%s NYIR emit: WebAssembly string table exceeds 32-bit "
                      "memory",
                      c->pretty);
        return false;
      }
      *offset = (uint32_t)address;
      return true;
    }
    if (len >= SIZE_MAX - address) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: WebAssembly string table size overflow",
                    c->pretty);
      return false;
    }
    address += len + 1;
  }
  return false;
}

static bool nyr_port_jump(nyr_port_ctx_t *c, int64_t label) {
  switch (c->kind) {
  case NY_PORT_BPF:
    return nyr_printf(c->w, "\tgoto .Lnyir_%s_L%" PRId64 "\n", c->name, label);
  case NY_PORT_MIPS:
    return nyr_printf(c->w, "\tb\t.Lnyir_%s_L%" PRId64 "\n", c->name, label);
  case NY_PORT_POWERPC:
    return nyr_printf(c->w, "\tb\t.Lnyir_%s_L%" PRId64 "\n", c->name, label);
  case NY_PORT_AVR:
    return nyr_printf(c->w, "\trjmp\t.Lnyir_%s_L%" PRId64 "\n", c->name, label);
  case NY_PORT_WASM:
    return nyr_printf(c->w, "\tbr $L%" PRId64 "\n", label);
  }
  return false;
}

static bool nyr_wasm_panic_runtime_pair(const nyr_port_ctx_t *c,
                                        const nyr_inst_t *in, const int *args,
                                        int argc, bool is_alloc) {
  if (!c || !c->nyir || !in || argc != 1 || !args)
    return false;
  if (is_alloc) {
    if (in->dst < 0 || c->current_index + 1 >= c->nyir->len)
      return false;
    const nyr_inst_t *next = &c->nyir->data[c->current_index + 1];
    int panic_args[NYIR_CALL_MAX_ARGS];
    int panic_argc = 0;
    return next->op == NYIR_CALL && next->symbol &&
           strcmp(next->symbol, "rt_panic") == 0 &&
           nyr_call_args(next, c->nyir->next_value, panic_args,
                          NYIR_CALL_MAX_ARGS, &panic_argc, NULL, 0) &&
           panic_argc == 1 && panic_args[0] == in->dst;
  }
  if (c->current_index == 0)
    return false;
  const nyr_inst_t *previous = &c->nyir->data[c->current_index - 1];
  return previous->op == NYIR_CALL && previous->symbol &&
         strcmp(previous->symbol, "rt_alloc_string") == 0 &&
         previous->dst >= 0 && args[0] == previous->dst;
}

static bool nyr_port_avr_raw_pointer_call(const nyr_inst_t *in, int argc) {
  if (!in || !in->symbol || argc < 0)
    return false;
  const ny_native_runtime_abi_decl_t *decl =
      ny_native_runtime_abi_decl(in->symbol);
  if (!decl || !(decl->flags & NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING) ||
      !ny_native_runtime_abi_result_is_pointer(decl))
    return false;
  for (int i = 0; i < argc; ++i)
    if (ny_native_runtime_abi_arg_is_tagged(decl, (size_t)i) ||
        ny_native_runtime_abi_arg_is_f64(decl, (size_t)i))
      return false;
  return true;
}

static bool nyr_port_static_string_value_depth(const nyr_port_ctx_t *c,
                                               int value, size_t before,
                                               unsigned depth) {
  if (!c || !c->nyir || value < 0 || depth > 8)
    return false;
  for (size_t i = before; i > 0; --i) {
    const nyr_inst_t *in = &c->nyir->data[i - 1];
    if (in->dst != value)
      continue;
    if (in->op == NYIR_ADDR_SYMBOL && in->symbol &&
        strncmp(in->symbol, "ny_str_", 7) == 0 &&
        nyr_strtab_get(in->symbol, NULL) != NULL)
      return true;
    if (in->op == NYIR_CALL && in->symbol &&
        strcmp(in->symbol, "rt_cstr_to_str") == 0 &&
        ((in->flags & NYIR_INST_F_EXTERN) ||
         ny_native_runtime_abi_decl(in->symbol) != NULL)) {
      int args[NYIR_CALL_MAX_ARGS];
      int argc = 0;
      return nyr_call_args(in, c->nyir->next_value, args, NYIR_CALL_MAX_ARGS,
                            &argc, NULL, 0) &&
             argc == 1 &&
             nyr_port_static_string_value_depth(c, args[0], i - 1, depth + 1);
    }
    return false;
  }
  return false;
}

static bool nyr_port_static_string_value(const nyr_port_ctx_t *c, int value,
                                         size_t before) {
  return nyr_port_static_string_value_depth(c, value, before, 0);
}

static bool nyr_port_value_unused_after(const nyr_port_ctx_t *c, int value,
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

static bool nyr_port_alloc_panic_pair_at(const nyr_port_ctx_t *c,
                                         const nyr_inst_t *alloc,
                                         size_t index) {
  if (!c || !c->nyir || !alloc || alloc->op != NYIR_CALL || !alloc->symbol ||
      strcmp(alloc->symbol, "rt_alloc_string") != 0 ||
      (!(alloc->flags & NYIR_INST_F_EXTERN) &&
       ny_native_runtime_abi_decl(alloc->symbol) == NULL) ||
      alloc->dst < 0 || index + 1 >= c->nyir->len)
    return false;
  int alloc_args[NYIR_CALL_MAX_ARGS];
  int alloc_argc = 0;
  const nyr_inst_t *panic = &c->nyir->data[index + 1];
  int panic_args[NYIR_CALL_MAX_ARGS];
  int panic_argc = 0;
  return nyr_call_args(alloc, c->nyir->next_value, alloc_args,
                        NYIR_CALL_MAX_ARGS, &alloc_argc, NULL, 0) &&
         alloc_argc == 1 &&
         nyr_port_static_string_value(c, alloc_args[0], index) &&
         panic->op == NYIR_CALL && panic->symbol &&
         strcmp(panic->symbol, "rt_panic") == 0 &&
         ((panic->flags & NYIR_INST_F_EXTERN) ||
          ny_native_runtime_abi_decl(panic->symbol) != NULL) &&
         nyr_call_args(panic, c->nyir->next_value, panic_args,
                        NYIR_CALL_MAX_ARGS, &panic_argc, NULL, 0) &&
         panic_argc == 1 && panic_args[0] == alloc->dst &&
         nyr_port_value_unused_after(c, alloc->dst, index + 2);
}

static bool nyr_port_panic_runtime_pair(const nyr_port_ctx_t *c,
                                        const nyr_inst_t *in, bool is_alloc) {
  if (!c || !c->nyir || !in)
    return false;
  if (is_alloc)
    return nyr_port_alloc_panic_pair_at(c, in, c->current_index);
  if (c->current_index == 0)
    return false;
  const nyr_inst_t *previous = &c->nyir->data[c->current_index - 1];
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  return nyr_call_args(in, c->nyir->next_value, args, NYIR_CALL_MAX_ARGS,
                        &argc, NULL, 0) &&
         argc == 1 && args[0] == previous->dst &&
         nyr_port_alloc_panic_pair_at(c, previous, c->current_index - 1);
}

static bool nyr_port_emit_panic_trap(nyr_port_ctx_t *c) {
  size_t index = c->current_index;
  switch (c->kind) {
  case NY_PORT_AVR:
    return nyr_printf(c->w,
                      "\tbreak\n.Lny_panic_"
                      "spin_%s_%zu:\n"
                      "\trjmp\t.Lny_panic_"
                      "spin_%s_%zu\n",
                      c->name, index, c->name, index);
  case NY_PORT_MIPS:
    return nyr_printf(c->w,
                      "\tbreak\n.Lny_panic_"
                      "spin_%s_%zu:\n"
                      "\tb\t.Lny_panic_spin_%"
                      "s_%zu\n\tnop\n",
                      c->name, index, c->name, index);
  case NY_PORT_POWERPC:
    return nyr_printf(c->w,
                      "\ttrap\n.Lny_panic_"
                      "spin_%s_%zu:\n"
                      "\tb\t.Lny_panic_spin_%"
                      "s_%zu\n",
                      c->name, index, c->name, index);
  case NY_PORT_BPF:
  case NY_PORT_WASM:
    return false;
  }
  return false;
}

static bool nyr_wasm_static_string_value(nyr_port_ctx_t *c, int value) {
  if (!c || !c->nyir || value < 0)
    return false;
  for (size_t i = c->current_index; i > 0; --i) {
    const nyr_inst_t *in = &c->nyir->data[i - 1];
    if (in->dst != value)
      continue;
    uint32_t offset = 0;
    return in->op == NYIR_ADDR_SYMBOL &&
           nyr_port_wasm_string_offset(c, in->symbol, &offset);
  }
  return false;
}

static bool nyr_wasm_host_symbol(const char *symbol) {
  if (!symbol || !symbol[0] ||
      !(symbol[0] == '_' || (symbol[0] >= 'A' && symbol[0] <= 'Z') ||
        (symbol[0] >= 'a' && symbol[0] <= 'z')))
    return false;
  for (const unsigned char *p = (const unsigned char *)symbol + 1; *p; ++p)
    if (!(*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9')))
      return false;
  return true;
}

static nyr_scalar_class_t nyr_wasm_value_class(const nyr_func_t *func,
                                                int value, unsigned depth) {
  if (!func || value < 0 || value >= func->next_value || depth > 32)
    return NYIR_SCALAR_UNKNOWN;
  const nyr_inst_t *def = NULL;
  for (size_t i = func->len; i > 0; --i) {
    if (func->data[i - 1].dst == value) {
      def = &func->data[i - 1];
      break;
    }
  }
  if (!def)
    return NYIR_SCALAR_UNKNOWN;
  if (def->scalar_class != NYIR_SCALAR_UNKNOWN)
    return def->scalar_class;
  if (def->op == NYIR_COPY)
    return nyr_wasm_value_class(func, def->a, depth + 1);
  if (def->op == NYIR_PHI && def->phi_incoming_len) {
    nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
    for (size_t i = 0; i < def->phi_incoming_len; ++i) {
      nyr_scalar_class_t incoming =
          nyr_wasm_value_class(func, def->phi_incoming[i].value, depth + 1);
      if (incoming == NYIR_SCALAR_UNKNOWN ||
          (joined != NYIR_SCALAR_UNKNOWN && joined != incoming))
        return NYIR_SCALAR_UNKNOWN;
      joined = incoming;
    }
    return joined;
  }
  return NYIR_SCALAR_UNKNOWN;
}

static nyr_wasm_import_type_t nyr_wasm_aggregate_import_type(uint32_t packed) {
  uint32_t size = NYIR_ARG_AGG_SIZE(packed);
  unsigned class0 = NYIR_ARG_AGG_CLASS(packed, 0);
  unsigned class1 = NYIR_ARG_AGG_CLASS(packed, 1);
  if (size == 0 || class1 != NYIR_ARG_CLASS_NONE)
    return NYR_WASM_IMPORT_UNSUPPORTED;
  if (class0 == NYIR_ARG_CLASS_MEMORY)
    return NYR_WASM_IMPORT_I32;
  if (class0 == NYIR_ARG_CLASS_INTEGER) {
    if (size == 4)
      return NYR_WASM_IMPORT_I32;
    if (size == 8)
      return NYR_WASM_IMPORT_I64;
  }
  if (class0 == NYIR_ARG_CLASS_SSE) {
    if (size == 4)
      return NYR_WASM_IMPORT_F32;
    if (size == 8)
      return NYR_WASM_IMPORT_F64;
  }
  return NYR_WASM_IMPORT_UNSUPPORTED;
}

static nyr_wasm_import_type_t nyr_wasm_mapped_type(const nyr_type_map_t *types,
                                                   int value,
                                                   nyr_scalar_class_t cls) {
  if (!types || value < 0 || (size_t)value >= types->value_count)
    return NYR_WASM_IMPORT_UNSUPPORTED;
  if (cls == NYIR_SCALAR_POINTER)
    return NYR_WASM_IMPORT_I32;
  if (cls == NYIR_SCALAR_AGGREGATE)
    return NYR_WASM_IMPORT_UNSUPPORTED;
  bool vector = types->value_v128_i64[value] || types->value_v256_i64[value] ||
                types->value_v128_f64[value] || types->value_v128_f32[value] ||
                types->value_v256_f64[value];
  bool f32 = types->value_f32[value];
  bool f64 = types->value_f64[value];
  if (vector || (f32 && f64))
    return NYR_WASM_IMPORT_UNSUPPORTED;
  if (f64)
    return NYR_WASM_IMPORT_F64;
  if (f32)
    return NYR_WASM_IMPORT_F32;
  return NYR_WASM_IMPORT_I64;
}

bool nyr_wasm_call_signature(const nyr_func_t *func, const nyr_inst_t *call,
                             const int *args, int argc,
                             nyr_wasm_import_type_t *arg_types,
                             nyr_wasm_import_type_t *result_type) {
  if (!func || !call || !args || argc < 0 || argc > NYIR_CALL_MAX_ARGS ||
      !result_type)
    return false;
  bool sret = (call->flags & NYIR_INST_F_SRET) != 0;
  if ((sret && (argc == 0 || call->ret_aggregate_size != 0)) ||
      (!sret && call->scalar_class == NYIR_SCALAR_AGGREGATE &&
       call->ret_aggregate_size == 0) ||
      (call->ret_aggregate_size != 0 && (call->dst < 0 || sret)))
    return false;
  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, func, nyr_max_local(func)))
    return false;
  nyr_scalar_class_t sret_class =
      sret ? nyr_call_arg_class_get(call, 0) : NYIR_SCALAR_UNKNOWN;
  if (sret && sret_class == NYIR_SCALAR_UNKNOWN)
    sret_class = nyr_wasm_value_class(func, args[0], 0);
  if (sret && sret_class != NYIR_SCALAR_POINTER) {
    nyr_type_map_free(&types);
    return false;
  }
  if (sret || call->dst < 0) {
    *result_type = NYR_WASM_IMPORT_VOID;
  } else if (call->ret_aggregate_size != 0) {
    uint32_t packed =
        (call->ret_aggregate_size & NYIR_ARG_AGG_SIZE_MASK) |
        ((uint32_t)call->ret_aggregate_classes[0]
         << NYIR_ARG_AGG_CLASS0_SHIFT) |
        ((uint32_t)call->ret_aggregate_classes[1] << NYIR_ARG_AGG_CLASS1_SHIFT);
    *result_type = nyr_wasm_aggregate_import_type(packed);
  } else {
    *result_type = nyr_wasm_mapped_type(&types, call->dst, call->scalar_class);
  }
  bool supported = *result_type != NYR_WASM_IMPORT_UNSUPPORTED;
  for (int i = 0; supported && i < argc; ++i) {
    if (call->arg_sizes && call->arg_sizes[i] != 0) {
      nyr_wasm_import_type_t type =
          nyr_wasm_aggregate_import_type(call->arg_sizes[i]);
      supported = type != NYR_WASM_IMPORT_UNSUPPORTED;
      if (arg_types)
        arg_types[i] = type;
      continue;
    }
    nyr_scalar_class_t arg_class = nyr_call_arg_class_get(call, (size_t)i);
    if (arg_class == NYIR_SCALAR_UNKNOWN)
      arg_class = nyr_wasm_value_class(func, args[i], 0);
    nyr_wasm_import_type_t type = nyr_wasm_mapped_type(
        &types, args[i], sret && i == 0 ? NYIR_SCALAR_POINTER : arg_class);
    supported = type != NYR_WASM_IMPORT_UNSUPPORTED;
    if (arg_types)
      arg_types[i] = type;
  }
  nyr_type_map_free(&types);
  return supported;
}

static bool nyr_wasm_host_import_signature(
    nyr_port_ctx_t *c, const nyr_inst_t *in, const int *args, int argc,
    nyr_wasm_import_type_t *arg_types, nyr_wasm_import_type_t *result_type) {
  const char *symbol = in && in->symbol ? in->symbol : "";
  if (!in || !(in->flags & NYIR_INST_F_EXTERN) ||
      !nyr_wasm_host_symbol(symbol) || strncmp(symbol, "rt_", 3) == 0 ||
      ny_native_runtime_abi_decl(symbol))
    return false;
  return nyr_wasm_call_signature(c->nyir, in, args, argc, arg_types,
                                 result_type);
}

static bool nyr_port_call(nyr_port_ctx_t *c, const nyr_inst_t *in) {
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!nyr_call_args(in, c->nyir->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                      c->err, c->err_len))
    return false;
  bool avr_f64_to_i64 = c->kind == NY_PORT_AVR && in->symbol &&
                        strcmp(in->symbol, "rt_f64_to_i64") == 0;
  const char *sym =
      avr_f64_to_i64 ? "__fixdfdi" : (in->symbol ? in->symbol : "");
  bool is_ext = (in->flags & NYIR_INST_F_EXTERN) != 0 || avr_f64_to_i64;
  bool avr_external_call =
      c->kind == NY_PORT_AVR && nyr_port_avr_external_args_supported(in);
  bool avr_stack_call = c->kind == NY_PORT_AVR &&
                        ((!is_ext && ny_native_runtime_abi_decl(sym) == NULL) ||
                         avr_external_call);
  if (c->kind != NY_PORT_WASM && !avr_stack_call &&
      (size_t)argc > c->target->gp_arg_reg_count) {
    nyr_set_error(c->err, c->err_len,
                  "%s NYIR emit: %d "
                  "scalar arguments "
                  "exceed the "
                  "%zu-register ABI slice",
                  c->pretty, argc, c->target->gp_arg_reg_count);
    return false;
  }
  if ((c->kind == NY_PORT_MIPS || c->kind == NY_PORT_POWERPC ||
       c->kind == NY_PORT_AVR) &&
      (is_ext || ny_native_runtime_abi_decl(sym) != NULL)) {
    /*
     * These zero-argument runtime constructors have fixed IEEE-754 payloads.
     */
    if (c->kind == NY_PORT_AVR &&
        (strcmp(sym, "rt_flt_nan") == 0 || strcmp(sym, "rt_flt_inf") == 0) &&
        argc == 0 && in->dst >= 0 && in->scalar_class == NYIR_SCALAR_F64) {
      uint64_t float_constant = strcmp(sym, "rt_flt_nan") == 0
                                    ? UINT64_C(0x7ff8000000000000)
                                    : UINT64_C(0x7ff0000000000000);
      return nyr_port_mov_imm(c, c->tmp0, float_constant) &&
             nyr_port_store_value(c, in->dst, c->tmp0);
    }
    bool is_alloc = strcmp(sym, "rt_alloc_string") == 0;
    bool is_panic = strcmp(sym, "rt_panic") == 0;
    bool is_cstr_to_str = strcmp(sym, "rt_cstr_to_str") == 0;
    if (is_cstr_to_str && argc == 1 && in->dst >= 0 &&
        nyr_port_static_string_value(c, args[0], c->current_index)) {
      const char *tmp = c->kind == NY_PORT_AVR ? "r18:r25" : c->tmp0;
      return nyr_port_load_value(c, tmp, args[0]) &&
             nyr_port_store_value(c, in->dst, tmp);
    }
    bool panic_pair_call =
        (is_alloc || is_panic) && nyr_port_panic_runtime_pair(c, in, is_alloc);
    if (panic_pair_call) {
      if (is_alloc)
        return true;
      return nyr_port_emit_panic_trap(c);
    }
  }
  nyr_wasm_import_type_t wasm_arg_types[NYIR_CALL_MAX_ARGS];
  nyr_wasm_import_type_t wasm_result_type = NYR_WASM_IMPORT_UNSUPPORTED;
  bool host_import_call =
      c->kind == NY_PORT_WASM &&
      nyr_wasm_host_import_signature(c, in, args, argc, wasm_arg_types,
                                     &wasm_result_type);
  if (host_import_call) {
    for (int i = 0; i < argc; ++i) {
      if (!nyr_port_load_value(c, "$t0", args[i]))
        return false;
      uint32_t packed_arg = in->arg_sizes ? in->arg_sizes[i] : 0;
      if (packed_arg != 0 &&
          NYIR_ARG_AGG_CLASS(packed_arg, 0) != NYIR_ARG_CLASS_MEMORY) {
        if (!nyr_put(c->w, "\ti32.wrap_i64\n"))
          return false;
        if (wasm_arg_types[i] == NYR_WASM_IMPORT_I64) {
          if (!nyr_put(c->w, "\ti64.load\n"))
            return false;
        } else if (wasm_arg_types[i] == NYR_WASM_IMPORT_F64) {
          if (!nyr_put(c->w, "\tf64.load\n"))
            return false;
        } else if (wasm_arg_types[i] == NYR_WASM_IMPORT_I32) {
          if (!nyr_put(c->w, "\ti32.load\n"))
            return false;
        } else if (wasm_arg_types[i] == NYR_WASM_IMPORT_F32) {
          if (!nyr_put(c->w, "\tf32.load\n"))
            return false;
        } else {
          nyr_set_error(c->err, c->err_len,
                        "%s NYIR emit: unsupported Wasm aggregate argument "
                        "representation",
                        c->pretty);
          return false;
        }
        continue;
      }
      if (wasm_arg_types[i] == NYR_WASM_IMPORT_I32) {
        if (!nyr_put(c->w, "\ti32.wrap_i64\n"))
          return false;
      } else if (wasm_arg_types[i] == NYR_WASM_IMPORT_F64) {
        if (!nyr_put(c->w, "\tf64.reinterpret_i64\n"))
          return false;
      } else if (wasm_arg_types[i] == NYR_WASM_IMPORT_F32 &&
                 !nyr_put(c->w, "\ti32.wrap_i64\n\tf32.reinterpret_i32\n")) {
        return false;
      }
    }
    if (!nyr_printf(c->w, "\tcall $%s\n", sym))
      return false;
    if (wasm_result_type == NYR_WASM_IMPORT_VOID)
      return true;
    if (wasm_result_type == NYR_WASM_IMPORT_I32) {
      if (!nyr_put(c->w, "\ti64.extend_i32_u\n"))
        return false;
    } else if (wasm_result_type == NYR_WASM_IMPORT_F64) {
      if (!nyr_put(c->w, "\ti64.reinterpret_f64\n"))
        return false;
    } else if (wasm_result_type == NYR_WASM_IMPORT_F32 &&
               !nyr_put(c->w, "\ti32.reinterpret_f32\n\ti64.extend_i32_u\n")) {
      return false;
    }
    return nyr_port_store_value(c, in->dst, "$ret");
  }
  unsigned avr_external_stack_bytes = 0;
  if (avr_stack_call && is_ext) {
    if (!nyr_port_avr_marshal_external_args(c, in, args, argc,
                                            &avr_external_stack_bytes))
      return false;
  } else if (avr_stack_call) {
    for (int i = argc; i > (int)c->target->gp_arg_reg_count; --i)
      if (!nyr_port_avr_push_stack_arg(c, args[i - 1], 8u))
        return false;
  }
  int register_argc = argc < (int)c->target->gp_arg_reg_count
                          ? argc
                          : (int)c->target->gp_arg_reg_count;
  if (c->kind == NY_PORT_WASM) {
    /*
     * Wasm calls consume every argument from the operand stack; they have no
     * fixed argument-register slice.
     */
    for (int i = 0; i < argc; ++i)
      if (!nyr_port_load_value(c, "$t0", args[i]))
        return false;
  } else if (!(avr_stack_call && is_ext)) {
    for (int i = 0; i < register_argc; ++i)
      if (!nyr_port_load_value(c, c->target->gp_arg_regs[i], args[i]))
        return false;
  }
  if (c->kind == NY_PORT_WASM &&
      (is_ext || ny_native_runtime_abi_decl(sym) != NULL)) {
    bool wasm_allocator_call =
        (strcmp(sym, "rt_malloc") == 0 || strcmp(sym, "rt_malloc_i64") == 0) &&
        argc == 1;
    bool wasm_zero_allocator_call =
        strcmp(sym, "rt_zalloc_raw") == 0 && argc == 1;
    bool is_alloc = strcmp(sym, "rt_alloc_string") == 0;
    bool is_panic = strcmp(sym, "rt_panic") == 0;
    bool is_cstr_to_str = strcmp(sym, "rt_cstr_to_str") == 0;
    bool panic_runtime_call =
        (is_alloc || is_panic) &&
        nyr_wasm_panic_runtime_pair(c, in, args, argc, is_alloc);
    bool wasm_string_call =
        in->dst >= 0 && ((strcmp(sym, "rt_cstr_concat") == 0 && argc == 2) ||
                         ((strcmp(sym, "rt_cstr_len") == 0 ||
                           strcmp(sym, "rt_i64_to_cstr_raw") == 0) &&
                          argc == 1));
    bool static_cstr_call =
        is_cstr_to_str && argc == 1 && nyr_wasm_static_string_value(c, args[0]);
    if (!panic_runtime_call && !static_cstr_call && !wasm_string_call &&
        !wasm_allocator_call && !wasm_zero_allocator_call &&
        !host_import_call) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: "
                    "WebAssembly external call does not match a supported "
                    "runtime helper or scalar i64/Wasm32-pointer/f32/f64 env "
                    "import",
                    c->pretty);
      return false;
    }
  }
  const char *fn_pfx = (is_ext || strncmp(sym, "ny_fn_", 6) == 0 ||
                        ny_native_runtime_abi_decl(sym) != NULL)
                           ? ""
                           : "ny_fn_";
  (void)is_ext;
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!nyr_printf(c->w, "\tcall\t%s%s%s\n", c->target->symbol_prefix, fn_pfx,
                    sym))
      return false;
    return in->dst < 0 || nyr_port_store_value(c, in->dst, "r0");
  case NY_PORT_MIPS:
    if (!nyr_printf(c->w,
                    "\tjal\t%s%s%"
                    "s\n\tnop\n",
                    c->target->symbol_prefix, fn_pfx, sym))
      return false;
    return in->dst < 0 || nyr_port_store_value(c, in->dst, "$v0");
  case NY_PORT_POWERPC:
    if (!nyr_printf(c->w, "\tbl\t%s%s%s\n", c->target->symbol_prefix, fn_pfx,
                    sym))
      return false;
    return in->dst < 0 || nyr_port_store_value(c, in->dst, "r3");
  case NY_PORT_AVR:
    if (!nyr_printf(c->w, "\tcall\t%s%s%s\n", c->target->symbol_prefix, fn_pfx,
                    sym))
      return false;
    if (avr_stack_call && is_ext) {
      for (unsigned byte = 0; byte < avr_external_stack_bytes; ++byte)
        if (!nyr_put(c->w, "\tpop\tr0\n"))
          return false;
    } else if (avr_stack_call) {
      for (int i = register_argc; i < argc; ++i) {
        for (unsigned byte = 0; byte < 8u; ++byte)
          if (!nyr_put(c->w, "\tpop\tr0\n"))
            return false;
      }
    }
    if (in->dst >= 0 && is_ext && in->scalar_class == NYIR_SCALAR_F32) {
      if (!nyr_put(c->w, "\tmov\tr18, r22\n\tmov\tr19, r23\n"
                         "\tmov\tr20, r24\n\tmov\tr21, r25\n"
                         "\tclr\tr22\n\tclr\tr23\n\tclr\tr24\n\tclr\tr25\n"))
        return false;
    }
    return in->dst < 0 || nyr_port_store_value(c, in->dst, "r18:r25");
  case NY_PORT_WASM:
    if (!nyr_printf(c->w, "\tcall $%s%s%s\n", c->target->symbol_prefix, fn_pfx,
                    sym))
      return false;
    if (in->dst < 0)
      return nyr_put(c->w, "\tdrop\n");
    return nyr_port_store_value(c, in->dst, "$ret");
  }
  return false;
}

static bool nyr_port_avr_softfloat_call(nyr_port_ctx_t *c,
                                        const nyr_inst_t *in,
                                        const char *symbol, size_t argc,
                                        nyr_scalar_class_t result_class) {
  if (!c || c->kind != NY_PORT_AVR || !in || !symbol || argc == 0 || argc > 2)
    return false;
  nyr_inst_t call = {
      .op = NYIR_CALL,
      .dst = in->dst,
      .a = in->a,
      .b = in->b,
      .imm = (int64_t)argc,
      .symbol = symbol,
      .flags = NYIR_INST_F_EXTERN,
      .scalar_class = result_class,
  };
  nyr_scalar_class_t arg_class = NYIR_SCALAR_F32;
  switch (in->op) {
  case NYIR_I64_TO_F32:
  case NYIR_I64_TO_F64:
    arg_class = NYIR_SCALAR_I64_RAW;
    break;
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_F64_TO_I64:
  case NYIR_F64_TO_F32:
    arg_class = NYIR_SCALAR_F64;
    break;
  default:
    break;
  }
  for (size_t i = 0; i < argc; ++i)
    nyr_call_arg_class_set(call.arg_scalar_classes, i, arg_class);
  return nyr_port_call(c, &call);
}

static bool nyr_port_emit_inst(nyr_port_ctx_t *c, const nyr_inst_t *in) {
  switch (in->op) {
  case NYIR_NOP:
    return true;
  case NYIR_BOUNDS_CHECK:
    if (c->kind == NY_PORT_AVR) {
      char fail_label[192];
      char done_label[192];
      snprintf(fail_label, sizeof(fail_label),
               ".Lny_bounds_fail_%s_%"
               "zu",
               c->name, c->current_index);
      snprintf(done_label, sizeof(done_label),
               ".Lny_bounds_done_%s_%"
               "zu",
               c->name, c->current_index);
      if (!nyr_port_load_value(c, c->tmp0, in->b) ||
          !nyr_put(c->w, "\tsbrc\tr25,"
                         " 7\n") ||
          !nyr_printf(c->w, "\trjmp\t%s\n", fail_label))
        return false;
      if (in->c >= 0) {
        if (!nyr_port_load_value(c, c->tmp1, in->c))
          return false;
      } else if (!nyr_port_mov_imm(c, c->tmp1, in->imm)) {
        return false;
      }
      if (!nyr_put(c->w, "\tsbrc\tr17,"
                         " 7\n") ||
          !nyr_printf(c->w, "\trjmp\t%s\n", fail_label) ||
          !nyr_put(c->w, "\tcp\tr18, r10\n"
                         "\tcpc\tr19, r11\n"
                         "\tcpc\tr20, r12\n"
                         "\tcpc\tr21, r13\n"
                         "\tcpc\tr22, r14\n"
                         "\tcpc\tr23, r15\n"
                         "\tcpc\tr24, r16\n"
                         "\tcpc\tr25, "
                         "r17\n") ||
          !nyr_printf(c->w,
                      "\tbrlo\t%s\n%s:"
                      "\n",
                      done_label, fail_label) ||
          !nyr_port_emit_panic_trap(c) ||
          !nyr_printf(c->w, "%s:\n", done_label))
        return false;
      return true;
    }
    if (c->kind == NY_PORT_WASM) {
      if (!nyr_port_load_value(c, "$offset", in->b) ||
          !nyr_put(c->w, "\ti64.const 0\n"
                         "\ti64.lt_s\n"
                         "\tif\n"
                         "\t\tunreachable\n"
                         "\tend\n") ||
          !nyr_port_load_value(c, "$offset", in->b) ||
          (in->c >= 0 ? !nyr_port_load_value(c, "$length", in->c)
                      : !nyr_printf(c->w,
                                    "\ti64."
                                    "const "
                                    "%" PRId64 "\n",
                                    in->imm)) ||
          !nyr_put(c->w, "\ti64.ge_s\n"
                         "\tif\n"
                         "\t\tunreachable\n"
                         "\tend\n"))
        return false;
      return true;
    }
    if (c->kind == NY_PORT_MIPS || c->kind == NY_PORT_POWERPC) {
      const size_t check_id = c->nyir && c->nyir->data
                                  ? (size_t)(in - c->nyir->data)
                                  : c->current_index;
      char fail_label[192];
      char done_label[192];
      snprintf(fail_label, sizeof(fail_label),
               ".Lny_bounds_fail_%s_%"
               "zu",
               c->name, check_id);
      snprintf(done_label, sizeof(done_label),
               ".Lny_bounds_done_%s_%"
               "zu",
               c->name, check_id);
      if (!nyr_port_load_value(c, c->tmp0, in->b))
        return false;
      if (c->kind == NY_PORT_MIPS) {
        if (!nyr_printf(c->w,
                        "\tbltz\t%s, "
                        "%s\n\tnop\n",
                        c->tmp0, fail_label))
          return false;
      } else if (!nyr_printf(c->w,
                             "\tcmpdi\t%s, "
                             "0\n\tblt\t%s\n",
                             nyr_port_reg_name(c, c->tmp0), fail_label)) {
        return false;
      }
      if (in->c >= 0) {
        if (!nyr_port_load_value(c, c->tmp1, in->c))
          return false;
      } else if (!nyr_port_mov_imm(c, c->tmp1, in->imm)) {
        return false;
      }
      if (c->kind == NY_PORT_MIPS) {
        if (!nyr_printf(c->w,
                        "\tslt\t$t2, "
                        "%s, %s\n"
                        "\tbeqz\t$t2, "
                        "%s\n\tnop\n"
                        "\tb\t%"
                        "s\n\tnop\n"
                        "%s:"
                        "\n\tbreak\t0\n%"
                        "s:\n",
                        c->tmp0, c->tmp1, fail_label, done_label, fail_label,
                        done_label))
          return false;
      } else if (!nyr_printf(c->w,
                             "\tcmpd\t%s, %s\n"
                             "\tbge\t%s\n"
                             "\tb\t%s\n"
                             "%s:\n\ttw\t31, "
                             "0, 0\n"
                             "%s:\n",
                             nyr_port_reg_name(c, c->tmp0),
                             nyr_port_reg_name(c, c->tmp1), fail_label,
                             done_label, fail_label, done_label)) {
        return false;
      }
      return true;
    }
    /*
     * These bring-up text
     * backends do not yet have a
     * trap sequence. Dropping
     * the check would turn a
     * checked NYIR access into
     * an unchecked one.
     */
    nyr_set_error(c->err, c->err_len,
                  "%s NYIR emit: bounds "
                  "checks are unsupported "
                  "by this "
                  "portable target",
                  c->pretty);
    return false;
  case NYIR_CONST_I64:
    return nyr_port_mov_imm(c, c->tmp0, in->imm) &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_ADDR_SYMBOL: {
    /*
     * Materialize a symbol
     * address for the bring-up
     * text emitters. AVR
     * zero-extends its 16-bit
     * SRAM address into the i64
     * value registers; the other
     * targets use their native
     * address-of-symbol idiom.
     * Only constant-pool/global
     * addresses are produced
     * here, matching the object
     * encoders.
     */
    const char *sym = in->symbol ? in->symbol : "";
    switch (c->kind) {
    case NY_PORT_MIPS:
      if (!nyr_printf(c->w, "\tdla\t%s, %s\n", c->tmp0, sym))
        return false;
      break;
    case NY_PORT_POWERPC:
      if (!nyr_printf(c->w,
                      "\tlis\t%s, "
                      "%s@ha\n\taddi\t%"
                      "s, %s, %s@l\n",
                      nyr_port_reg_name(c, c->tmp0), sym,
                      nyr_port_reg_name(c, c->tmp0),
                      nyr_port_reg_name(c, c->tmp0), sym))
        return false;
      break;
    case NY_PORT_WASM: {
      uint32_t offset;
      if (!nyr_port_wasm_string_offset(c, sym, &offset)) {
        if (c->err && c->err_len && !c->err[0])
          nyr_set_error(c->err, c->err_len,
                        "%s NYIR emit: "
                        "unsupported "
                        "WebAssembly data "
                        "symbol "
                        "%s",
                        c->pretty, sym);
        return false;
      }
      if (!nyr_printf(c->w,
                      "\ti64.const %u "
                      ";; addr.symbol "
                      "%s\n",
                      offset, sym))
        return false;
    } break;
    case NY_PORT_BPF:
      if (!nyr_printf(c->w, "\t%s = %s ll\n", c->tmp0, sym))
        return false;
      break;
    case NY_PORT_AVR:
      if (!nyr_printf(c->w,
                      "\tldi\tr18, "
                      "lo8(%s)\n"
                      "\tldi\tr19, "
                      "hi8(%s)\n"
                      "\tclr\tr20\n\tclr"
                      "\tr21\n\tclr\tr22"
                      "\n"
                      "\tclr\tr23\n\tclr"
                      "\tr24\n\tclr\tr25"
                      "\n",
                      sym, sym))
        return false;
      break;
    }
    return in->dst < 0 || nyr_port_store_value(c, in->dst, c->tmp0);
  }
  case NYIR_COPY:
    return nyr_port_load_value(c, c->tmp0, in->a) &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_ADD_I64:
    return nyr_port_binop(c, in, "+", "daddu", "add", "__ny_avr_i64_add",
                          "i64.add");
  case NYIR_SUB_I64:
    return nyr_port_binop(c, in, "-", "dsubu", "subf", "__ny_avr_i64_sub",
                          "i64.sub");
  case NYIR_MUL_I64:
    return nyr_port_binop(c, in, "*", "dmul", "mulld", "__ny_avr_i64_mul",
                          "i64.mul");
  case NYIR_DIV_I64:
    return nyr_port_binop(c, in, "s/", "ddiv", "divd", "__ny_avr_i64_div",
                          "i64.div_s");
  case NYIR_MOD_I64:
    return nyr_port_binop(c, in, "s%", "drem", "modsd", "__ny_avr_i64_mod",
                          "i64.rem_s");
  case NYIR_AND_I64:
    return nyr_port_binop(c, in, "&", "and", "and", "__ny_avr_i64_and",
                          "i64.and");
  case NYIR_OR_I64:
    return nyr_port_binop(c, in, "|", "or", "or", "__ny_avr_i64_or", "i64.or");
  case NYIR_XOR_I64:
    return nyr_port_binop(c, in, "^", "xor", "xor", "__ny_avr_i64_xor",
                          "i64.xor");
  case NYIR_SHL_I64:
    return nyr_port_binop(c, in, "<<", "dsllv", "sld", "__ny_avr_i64_shl",
                          "i64.shl");
  case NYIR_SAR_I64:
    return nyr_port_binop(c, in, "s>>", "dsrav", "srad", "__ny_avr_i64_sar",
                          "i64.shr_s");
  case NYIR_ROR_I64:
    return nyr_port_ror_i64(c, in);
  case NYIR_ROR32_I64:
    if (c->kind == NY_PORT_AVR || c->kind == NY_PORT_BPF ||
        c->kind == NY_PORT_MIPS || c->kind == NY_PORT_POWERPC)
      return nyr_port_ror32_i64(c, in);
    if (c->kind != NY_PORT_WASM)
      break;
    return nyr_port_load_value(c, c->tmp0, in->a) &&
           nyr_put(c->w, "\ti32.wrap_"
                         "i64\n") &&
           nyr_port_load_value(c, c->tmp1, in->b) &&
           nyr_put(c->w, "\ti32.wrap_i64\n"
                         "\ti32.rotr\n"
                         "\ti64.extend_"
                         "i32_u\n") &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_BSWAP_I64:
    return nyr_port_bswap(c, in, false);
  case NYIR_BSWAP32_I64:
    return nyr_port_bswap(c, in, true);
  case NYIR_CONST_F64:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_mov_imm(c, c->tmp0, in->imm) &&
             nyr_port_store_value(c, in->dst, c->tmp0);
    if (c->kind == NY_PORT_WASM)
      return nyr_printf(c->w,
                        "\ti64.const "
                        "%" PRId64 "\n",
                        in->imm) &&
             nyr_port_store_value(c, in->dst, c->tmp0);
    break;
  case NYIR_CONST_F32:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_mov_imm(c, c->tmp0, in->imm) &&
             nyr_port_store_value(c, in->dst, c->tmp0);
    if (c->kind == NY_PORT_WASM)
      return nyr_printf(c->w,
                        "\ti64.const "
                        "%" PRIu32 "\n",
                        (uint32_t)in->imm) &&
             nyr_port_store_value(c, in->dst, c->tmp0);
    break;
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
    if (c->kind == NY_PORT_AVR) {
      const char *helper = in->op == NYIR_ADD_F64   ? "__adddf3"
                           : in->op == NYIR_SUB_F64 ? "__subdf3"
                           : in->op == NYIR_MUL_F64 ? "__muldf3"
                                                    : "__divdf3";
      return nyr_port_avr_softfloat_call(c, in, helper, 2, NYIR_SCALAR_F64);
    }
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_binary(c, in, false);
    break;
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
    if (c->kind == NY_PORT_AVR) {
      const char *helper = in->op == NYIR_ADD_F32   ? "__addsf3"
                           : in->op == NYIR_SUB_F32 ? "__subsf3"
                           : in->op == NYIR_MUL_F32 ? "__mulsf3"
                                                    : "__divsf3";
      return nyr_port_avr_softfloat_call(c, in, helper, 2, NYIR_SCALAR_F32);
    }
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_binary(c, in, true);
    break;
  case NYIR_SQRT_F64:
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_operand(c, in->a, false) &&
             nyr_put(c->w, "\tf64."
                           "sqrt\n") &&
             nyr_port_wasm_float_result(c, in->dst, false);
    break;
  case NYIR_I64_TO_F64:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_softfloat_call(c, in, "__floatdidf", 1,
                                         NYIR_SCALAR_F64);
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_convert(c, in);
    break;
  case NYIR_F64_TO_I64:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_softfloat_call(c, in, "__fixdfdi", 1,
                                         NYIR_SCALAR_I64_RAW);
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_convert(c, in);
    break;
  case NYIR_F32_TO_I64:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_softfloat_call(c, in, "__fixsfdi", 1,
                                         NYIR_SCALAR_I64_RAW);
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_convert(c, in);
    break;
  case NYIR_I64_TO_F32:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_softfloat_call(c, in, "__floatdisf", 1,
                                         NYIR_SCALAR_F32);
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_convert(c, in);
    break;
  case NYIR_F64_TO_F32:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_softfloat_call(c, in, "__truncdfsf2", 1,
                                         NYIR_SCALAR_F32);
  case NYIR_F32_TO_F64:
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_softfloat_call(c, in, "__extendsfdf2", 1,
                                         NYIR_SCALAR_F64);
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_convert(c, in);
    break;
  case NYIR_CMP_F64:
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_cmp(c, in, false);
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_float_cmp(c, in, false);
    break;
  case NYIR_CMP_F32:
    if (c->kind == NY_PORT_WASM)
      return nyr_port_wasm_float_cmp(c, in, true);
    if (c->kind == NY_PORT_AVR)
      return nyr_port_avr_float_cmp(c, in, true);
    break;
  case NYIR_CMP_I64:
    return nyr_port_cmp(c, in);
  case NYIR_LABEL:
    return nyr_port_label(c, in->imm);
  case NYIR_LOAD_LOCAL:
    return nyr_port_load_local(c, c->tmp0, (int)in->imm) &&
           nyr_port_store_value(c, in->dst, c->tmp0);
  case NYIR_STORE_LOCAL:
    return nyr_port_load_value(c, c->tmp0, in->a) &&
           nyr_port_store_local(c, (int)in->imm, c->tmp0);
  case NYIR_ADDR_LOCAL: {
    if (in->imm < 0 || in->imm >= c->max_local_slot) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: bad "
                    "addr.local",
                    c->pretty);
      return false;
    }
    const int offset = nyr_port_local_off(c, (int)in->imm);
    switch (c->kind) {
    case NY_PORT_BPF:
      if (!nyr_printf(c->w,
                      "\t%s = r10\n\t%s "
                      "+= -%d\n",
                      c->tmp0, c->tmp0, offset + 8))
        return false;
      break;
    case NY_PORT_MIPS:
      if (!nyr_port_mov_imm(c, c->tmp1, offset) ||
          !nyr_printf(c->w,
                      "\tdaddu\t%s, "
                      "$fp, %s\n",
                      c->tmp0, c->tmp1))
        return false;
      break;
    case NY_PORT_POWERPC:
      if (!nyr_port_mov_imm(c, c->tmp1, offset) ||
          !nyr_printf(c->w,
                      "\tadd\t%s, 31, "
                      "%s\n",
                      nyr_port_reg_name(c, c->tmp0),
                      nyr_port_reg_name(c, c->tmp1)))
        return false;
      break;
    case NY_PORT_AVR:
      if (!nyr_port_avr_slot_addr(c, offset) ||
          !nyr_put(c->w, "\tmovw\tr18, "
                         "r30\n"
                         "\tclr\tr20\n\tclr"
                         "\tr21\n\tclr\tr22"
                         "\n"
                         "\tclr\tr23\n\tclr"
                         "\tr24\n\tclr\tr25"
                         "\n"))
        return false;
      break;
    case NY_PORT_WASM:
      return false;
    }
    return nyr_port_store_value(c, in->dst, c->tmp0);
  }
  case NYIR_CALL:
    return nyr_port_call(c, in);
  case NYIR_RET:
    if (c->kind == NY_PORT_WASM) {
      if (in->a >= 0) {
        if (!nyr_port_load_value(c, c->tmp0, in->a))
          return false;
      } else if (!nyr_put(c->w, "\ti64.const "
                                "0\n")) {
        return false;
      }
      return nyr_printf(c->w,
                        "\tlocal.set "
                        "$result\n\tbr "
                        "$exit_%s\n",
                        c->name);
    }
    if (in->a >= 0 && !nyr_port_load_value(c, c->ret_reg, in->a))
      return false;
    return nyr_printf(c->w, "\t%s\t%s\n",
                      c->kind == NY_PORT_BPF    ? "goto"
                      : c->kind == NY_PORT_AVR  ? "rjmp"
                      : c->kind == NY_PORT_WASM ? "br"
                                                : "b",
                      c->epilogue_label);
  case NYIR_LOAD_I64:
    if (c->kind == NY_PORT_AVR) {
      if (!nyr_port_load_value(c, c->tmp0, in->a) ||
          !nyr_put(c->w, "\tmovw\tr30,"
                         " r18\n"))
        return false;
      if (in->flags & NYIR_INST_F_MEM_BYTE) {
        if (!nyr_put(c->w, "\tld\tr18, Z\n"
                           "\tclr\tr19\n\tc"
                           "lr\tr20\n\tclr"
                           "\tr21\n"
                           "\tclr\tr22\n\tc"
                           "lr\tr23\n\tclr"
                           "\tr24\n"
                           "\tclr\tr25\n"))
          return false;
      } else {
        for (unsigned byte = 0; byte < 8; ++byte)
          if (!nyr_printf(c->w,
                          "\tld\tr%u, "
                          "Z%s\n",
                          18 + byte, byte == 7 ? "" : "+"))
            return false;
      }
      return nyr_port_store_value(c, in->dst, c->tmp0);
    }
    if (c->kind == NY_PORT_WASM) {
      if (!nyr_port_wasm_checked_address(c, in->a) ||
          !nyr_put(c->w, (in->flags & NYIR_INST_F_MEM_BYTE) ? "\ti64."
                                                              "load8_u\n"
                                                            : "\ti64."
                                                              "load\n"))
        return false;
      return nyr_port_store_value(c, in->dst, c->tmp0);
    }
    if (!nyr_port_load_value(c, c->tmp0, in->a))
      return false;
    switch (c->kind) {
    case NY_PORT_BPF:
      if (!nyr_printf(c->w,
                      "\t%s = *(%s "
                      "*)(%s + 0)\n",
                      c->tmp1,
                      (in->flags & NYIR_INST_F_MEM_BYTE) ? "u8" : "u64",
                      c->tmp0))
        return false;
      break;
    case NY_PORT_MIPS:
      if (!nyr_printf(c->w,
                      "\t%s\t%s, "
                      "0(%s)\n",
                      (in->flags & NYIR_INST_F_MEM_BYTE) ? "lbu" : "ld",
                      c->tmp1, c->tmp0))
        return false;
      break;
    case NY_PORT_POWERPC:
      if (!nyr_printf(c->w,
                      "\t%s\t%s, "
                      "0(%s)\n",
                      (in->flags & NYIR_INST_F_MEM_BYTE) ? "lbz" : "ld",
                      nyr_port_reg_name(c, c->tmp1),
                      nyr_port_reg_name(c, c->tmp0)))
        return false;
      break;
    case NY_PORT_AVR:
    case NY_PORT_WASM:
      return false;
    }
    return nyr_port_store_value(c, in->dst, c->tmp1);
  case NYIR_STORE_I64:
    if (c->kind == NY_PORT_WASM) {
      if (!nyr_port_wasm_checked_address(c, in->a) ||
          !nyr_port_load_value(c, c->tmp1, in->c))
        return false;
      return nyr_put(c->w, (in->flags & NYIR_INST_F_MEM_BYTE)
                               ? "\ti64.store8\n"
                               : "\ti64.store\n");
    }
    if (!nyr_port_load_value(c, c->tmp0, in->a) ||
        !nyr_port_load_value(c, c->tmp1, in->c))
      return false;
    switch (c->kind) {
    case NY_PORT_BPF:
      return nyr_printf(c->w,
                        "\t*(%s *)(%s + 0) = "
                        "%s\n",
                        (in->flags & NYIR_INST_F_MEM_BYTE) ? "u8" : "u64",
                        c->tmp0, c->tmp1);
    case NY_PORT_MIPS:
      return nyr_printf(c->w, "\t%s\t%s, 0(%s)\n",
                        (in->flags & NYIR_INST_F_MEM_BYTE) ? "sb" : "sd",
                        c->tmp1, c->tmp0);
    case NY_PORT_POWERPC:
      return nyr_printf(c->w, "\t%s\t%s, 0(%s)\n",
                        (in->flags & NYIR_INST_F_MEM_BYTE) ? "stb" : "std",
                        nyr_port_reg_name(c, c->tmp1),
                        nyr_port_reg_name(c, c->tmp0));
    case NY_PORT_AVR:
      if (!nyr_put(c->w, "\tmovw\tr30,"
                         " r18\n"))
        return false;
      if (in->flags & NYIR_INST_F_MEM_BYTE)
        return nyr_put(c->w, "\tst\tZ, r10\n");
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w,
                        "\tst\tZ%s, "
                        "r%u\n",
                        byte == 7 ? "" : "+", 10 + byte))
          return false;
      return true;
    case NY_PORT_WASM:
      return false;
    }
    return false;
  case NYIR_BR:
    if (c->kind == NY_PORT_WASM) {
      size_t target;
      return nyr_port_wasm_label_index(c, in->imm, &target) &&
             nyr_port_wasm_set_pc(c, target) &&
             nyr_printf(c->w, "\tbr %s\n", c->dispatch_label);
    }
    return nyr_port_jump(c, in->imm);
  case NYIR_BR_IF:
    if (!nyr_port_load_value(c, c->tmp0, in->a))
      return false;
    switch (c->kind) {
    case NY_PORT_BPF:
      return nyr_printf(c->w,
                        "\tif %s != 0 goto "
                        ".Lnyir_%s_L%" PRId64 "\n",
                        c->tmp0, c->name, in->imm);
    case NY_PORT_MIPS:
      return nyr_printf(c->w,
                        "\tbnez\t%s, "
                        ".Lnyir_%s_L%" PRId64 "\n",
                        c->tmp0, c->name, in->imm);
    case NY_PORT_POWERPC:
      return nyr_printf(c->w,
                        "\tcmpdi\t%s, "
                        "0\n\tbne\t.Lnyir_%s_"
                        "L%" PRId64 "\n",
                        nyr_port_reg_name(c, c->tmp0), c->name, in->imm);
    case NY_PORT_AVR:
      if (!nyr_put(c->w, "\tclr\tr0\n"))
        return false;
      for (unsigned byte = 0; byte < 8; ++byte)
        if (!nyr_printf(c->w,
                        "\tor\tr0, "
                        "r%u\n",
                        18 + byte))
          return false;
      return nyr_printf(c->w,
                        "\tbreq\t1f\n"
                        "\trjmp\t.Lnyir_%s_"
                        "L%" PRId64 "\n"
                        "1:\n",
                        c->name, in->imm);
    case NY_PORT_WASM: {
      size_t target;
      return nyr_port_wasm_label_index(c, in->imm, &target) &&
             nyr_printf(c->w,
                        "\ti64.const "
                        "0\n\ti64.ne\n"
                        "\tif\n"
                        "\t\ti32.const "
                        "%zu\n"
                        "\t\tlocal.set "
                        "$pc\n"
                        "\telse\n"
                        "\t\ti32.const "
                        "%zu\n"
                        "\t\tlocal.set "
                        "$pc\n"
                        "\tend\n"
                        "\tbr %s\n",
                        target, c->current_index + 1, c->dispatch_label);
    }
    }
  case NYIR_COPY_STRUCT:
    if (c->kind == NY_PORT_WASM) {
      if (in->imm < 0 || in->imm > INT32_MAX) {
        nyr_set_error(c->err, c->err_len,
                      "WebAssembly NYIR emit: struct copy size exceeds i32");
        return false;
      }
      if (in->imm == 0)
        return true;
      return nyr_port_wasm_checked_address(c, in->a) &&
             nyr_port_wasm_checked_address(c, in->b) &&
             nyr_printf(c->w, "\ti32.const %" PRId64 "\n\tmemory.copy\n",
                        in->imm);
    }
    if (c->kind == NY_PORT_AVR) {
      if (in->imm < 0 || in->imm > UINT16_MAX) {
        nyr_set_error(c->err, c->err_len,
                      "AVR NYIR emit: struct copy size exceeds 16-bit address "
                      "space");
        return false;
      }
      if (in->imm == 0)
        return true;
      char loop_label[192], fail_label[192], spin_label[192];
      snprintf(loop_label, sizeof(loop_label), ".Lny_copy_struct_loop_%s_%zu",
               c->name, c->current_index);
      snprintf(fail_label, sizeof(fail_label), ".Lny_copy_struct_fail_%s_%zu",
               c->name, c->current_index);
      snprintf(spin_label, sizeof(spin_label), ".Lny_copy_struct_spin_%s_%zu",
               c->name, c->current_index);
      return nyr_port_load_value(c, c->tmp0, in->a) &&
             nyr_port_load_value(c, c->tmp1, in->b) &&
             nyr_put(c->w, "\tmov\tr0, r20\n"
                           "\tor\tr0, r21\n"
                           "\tor\tr0, r22\n"
                           "\tor\tr0, r23\n"
                           "\tor\tr0, r24\n"
                           "\tor\tr0, r25\n"
                           "\tor\tr0, r12\n"
                           "\tor\tr0, r13\n"
                           "\tor\tr0, r14\n"
                           "\tor\tr0, r15\n"
                           "\tor\tr0, r16\n"
                           "\tor\tr0, r17\n") &&
             nyr_printf(c->w,
                        "\tbreq\t%s\n"
                        "\tbreak\n"
                        "%s:\n"
                        "\trjmp\t%s\n"
                        "%s:\n"
                        "\tmovw\tr26, r18\n"
                        "\tmovw\tr30, r10\n"
                        "\tldi\tr16, lo8(%" PRId64 ")\n"
                        "\tldi\tr17, hi8(%" PRId64 ")\n"
                        "%s:\n"
                        "\tld\tr0, Z+\n"
                        "\tst\tX+, r0\n"
                        "\tsubi\tr16, 1\n"
                        "\tsbci\tr17, 0\n"
                        "\tbrne\t%s\n",
                        fail_label, spin_label, spin_label, fail_label, in->imm,
                        in->imm, loop_label, loop_label);
    }
    if (c->kind == NY_PORT_MIPS || c->kind == NY_PORT_POWERPC) {
      if (in->imm < 0) {
        nyr_set_error(c->err, c->err_len,
                      "%s NYIR emit: negative struct copy size", c->pretty);
        return false;
      }
      if (in->imm == 0)
        return true;
      char loop_label[192], done_label[192];
      snprintf(loop_label, sizeof(loop_label), ".Lny_copy_struct_loop_%s_%zu",
               c->name, c->current_index);
      snprintf(done_label, sizeof(done_label), ".Lny_copy_struct_done_%s_%zu",
               c->name, c->current_index);
      if (c->kind == NY_PORT_MIPS)
        return nyr_port_load_value(c, "$t0", in->a) &&
               nyr_port_load_value(c, "$t1", in->b) &&
               nyr_port_mov_imm(c, "$t2", in->imm) &&
               nyr_printf(c->w,
                          "\tbeqz\t$t2, %s\n"
                          "\tnop\n"
                          "%s:\n"
                          "\tlbu\t$t3, 0($t1)\n"
                          "\tsb\t$t3, 0($t0)\n"
                          "\tdaddiu\t$t0, $t0, 1\n"
                          "\tdaddiu\t$t1, $t1, 1\n"
                          "\tdaddiu\t$t2, $t2, -1\n"
                          "\tbnez\t$t2, %s\n"
                          "\tnop\n"
                          "%s:\n",
                          done_label, loop_label, loop_label, done_label);
      return nyr_port_load_value(c, "r11", in->a) &&
             nyr_port_load_value(c, "r12", in->b) &&
             nyr_port_mov_imm(c, "r9", in->imm) &&
             nyr_printf(
                 c->w,
                 "%s:\n"
                 "\tlbz\t%s, 0(%s)\n"
                 "\tstb\t%s, 0(%s)\n"
                 "\taddi\t%s, %s, 1\n"
                 "\taddi\t%s, %s, 1\n"
                 "\taddic.\t%s, %s, -1\n"
                 "\tbne\t%s\n",
                 loop_label, nyr_port_reg_name(c, "r10"),
                 nyr_port_reg_name(c, "r12"), nyr_port_reg_name(c, "r10"),
                 nyr_port_reg_name(c, "r11"), nyr_port_reg_name(c, "r11"),
                 nyr_port_reg_name(c, "r11"), nyr_port_reg_name(c, "r12"),
                 nyr_port_reg_name(c, "r12"), nyr_port_reg_name(c, "r9"),
                 nyr_port_reg_name(c, "r9"), loop_label);
    }
    break;
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
  case NYIR_ALLOCA:
  case NYIR_PHI:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC4_SET1_F64:
  case NYIR_VEC4_SHUFFLE_F64:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC8_LOAD_F32:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC8_ADD_F32:
  case NYIR_VEC8_SUB_F32:
  case NYIR_VEC8_MUL_F32:
  case NYIR_VEC8_DIV_F32:
  case NYIR_VEC8_FMA_F32:
  case NYIR_VEC8_SET1_F32:
  case NYIR_VEC8_SHUFFLE_F32:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC4_SET1_I64:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC4_ADD_I64:
  case NYIR_VEC4_SUB_I64:
  case NYIR_VEC4_AND_I64:
  case NYIR_VEC4_OR_I64:
  case NYIR_VEC4_XOR_I64:
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
  case NYIR_VEC8_LOAD_I64:
  case NYIR_VEC8_STORE_I64:
  case NYIR_VEC8_ADD_I64:
  case NYIR_VEC8_SUB_I64:
  case NYIR_VEC8_AND_I64:
  case NYIR_VEC8_OR_I64:
  case NYIR_VEC8_XOR_I64:
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
    break;
  case NYIR_SELECT_I64:
    return nyr_port_select_i64(c, in);
  case NYIR_CAPTURE_RET:
    if (c->kind == NY_PORT_WASM && c->current_index > 0 && in->dst >= 0) {
      const nyr_inst_t *call = &c->nyir->data[c->current_index - 1];
      if (call->op == NYIR_CALL && call->dst >= 0 &&
          (call->ret_aggregate_size == 4 || call->ret_aggregate_size == 8) &&
          call->ret_aggregate_classes[0] == NYIR_ARG_CLASS_SSE &&
          call->ret_aggregate_classes[1] == NYIR_ARG_CLASS_NONE && in->imm == 2)
        return nyr_port_load_value(c, "$t0", call->dst) &&
               nyr_port_store_value(c, in->dst, "$t0");
    }
    break;
  case NYIR_OP_COUNT:
    break;
  }
  nyr_set_error(c->err, c->err_len,
                "%s NYIR emit: "
                "unsupported op %s",
                c->pretty, nyr_op_name(in->op));
  return false;
}

static bool nyr_port_header(nyr_port_ctx_t *c) {
  if (c->kind == NY_PORT_WASM) {
    if (!nyr_printf(c->w, "(func $%s%s", c->target->symbol_prefix, c->name))
      return false;
    if (strcmp(c->name, "rt_main") == 0 && !nyr_put(c->w, " (export "
                                                          "\"rt_main\")"))
      return false;
    if (c->nyir->param_count > 0 && !c->nyir->param_types) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: "
                    "WebAssembly function "
                    "parameter types are "
                    "missing",
                    c->pretty);
      return false;
    }
    for (size_t i = 0; i < c->nyir->param_count; ++i) {
      if (c->nyir->param_types[i] != NYIR_PARAM_I64) {
        nyr_set_error(c->err, c->err_len,
                      "%s NYIR emit: "
                      "WebAssembly only "
                      "supports i64 "
                      "function "
                      "parameters",
                      c->pretty);
        return false;
      }
      if (!nyr_printf(c->w,
                      " (param $p%zu "
                      "i64)",
                      i))
        return false;
    }
    if (!nyr_put(c->w, " (result i64)\n"))
      return false;
    for (int i = 0; i < c->frame_bytes; ++i) {
      if (!nyr_printf(c->w,
                      "\t(local $s%d "
                      "i64)\n",
                      i * 8))
        return false;
    }
    if (!nyr_put(c->w, "\t(local $pc "
                       "i32)\n\t(local "
                       "$result i64)\n"))
      return false;
    return true;
  }
  if (c->kind == NY_PORT_BPF && strcmp(c->name, "rt_main") == 0) {
    if (!nyr_put(c->w, "\t.section "
                       "socket,\"ax\"\n"))
      return false;
  } else if (!nyr_put(c->w, "\t.text\n")) {
    return false;
  }
  if (strcmp(c->target->object_format, "macho") != 0 &&
      !nyr_printf(c->w,
                  "\t.type\t%s%s, "
                  "@function\n",
                  c->target->symbol_prefix, c->name))
    return false;
  if (!nyr_printf(c->w,
                  "\t.globl\t%s%s\n%s%s:"
                  "\n",
                  c->target->symbol_prefix, c->name, c->target->symbol_prefix,
                  c->name))
    return false;
  switch (c->kind) {
  case NY_PORT_BPF:
    return nyr_printf(c->w, "\t# eBPF uses r10 as "
                            "frame pointer; spills "
                            "live at negative "
                            "offsets\n");
  case NY_PORT_MIPS:
    return nyr_printf(c->w,
                      "\tdaddiu\t$sp, $sp, "
                      "-%d\n"
                      "\tsd\t$ra, %d($sp)\n"
                      "\tsd\t$fp, %d($sp)\n"
                      "\tmove\t$fp, $sp\n",
                      c->frame_bytes, c->frame_bytes - 8, c->frame_bytes - 16);
  case NY_PORT_POWERPC:
    return nyr_printf(c->w,
                      "\tmflr\t0\n"
                      "\tstd\t0, 16(1)\n"
                      "\tstdu\t1, -%d(1)\n"
                      "\tstd\t31, %d(1)\n"
                      "\tmr\t31, 1\n",
                      c->frame_bytes, c->frame_bytes - 8);
  case NY_PORT_AVR: {
    if (c->frame_bytes < 0 || c->frame_bytes > UINT16_MAX) {
      nyr_set_error(c->err, c->err_len,
                    "%s NYIR emit: AVR "
                    "frame is too large",
                    c->pretty);
      return false;
    }
    for (unsigned reg = 10; reg <= 17; ++reg)
      if (!nyr_printf(c->w, "\tpush\tr%u\n", reg))
        return false;
    unsigned frame_lo = (unsigned)c->frame_bytes & 0xffu;
    unsigned frame_hi = ((unsigned)c->frame_bytes >> 8) & 0xffu;
    /* Keep Y at the frame base and leave the call stack below its locals. */
    return nyr_put(c->w, "\tpush\tr28\n\tp"
                         "ush\tr29\n"
                         "\tin\tr28, 61\n"
                         "\tin\tr29, "
                         "62\n") &&
           nyr_printf(c->w,
                      "\tsubi\tr28, "
                      "%u\n"
                      "\tsbci\tr29, "
                      "%u\n"
                      "\tin\tr0, 63\n"
                      "\tcli\n"
                      "\tmov\tr26, r28\n"
                      "\tmov\tr27, r29\n"
                      "\tsubi\tr26, 1\n"
                      "\tsbci\tr27, 0\n"
                      "\tout\t62, r27\n"
                      "\tout\t61, r26\n"
                      "\tout\t63, r0\n",
                      frame_lo, frame_hi);
  }
  case NY_PORT_WASM:
    return true;
  }
  return false;
}

static bool nyr_port_footer(nyr_port_ctx_t *c, bool tag_return) {
  if (c->kind == NY_PORT_WASM) {
    if (tag_return && !nyr_put(c->w, "\ti64.const "
                                     "1\n\ti64."
                                     "shl\n\ti64.const "
                                     "1\n\ti64.add\n"))
      return false;
    return nyr_put(c->w, ")\n");
  }
  if (!nyr_printf(c->w, "%s:\n", c->epilogue_label))
    return false;
  if (tag_return) {
    switch (c->kind) {
    case NY_PORT_BPF:
      if (!nyr_put(c->w, "\tr0 <<= 1\n\tr0 "
                         "+= 1\n"))
        return false;
      break;
    case NY_PORT_MIPS:
      if (!nyr_put(c->w, "\tdsll\t$v0, "
                         "$v0, "
                         "1\n\tdaddiu\t$v0,"
                         " $v0, 1\n"))
        return false;
      break;
    case NY_PORT_POWERPC:
      if (!nyr_put(c->w, "\tsldi\t3, 3, "
                         "1\n\taddi\t3, 3, "
                         "1\n"))
        return false;
      break;
    case NY_PORT_AVR:
      if (!nyr_put(c->w, "\tlsl\tr18\n"
                         "\trol\tr19\n"
                         "\trol\tr20\n"
                         "\trol\tr21\n"
                         "\trol\tr22\n"
                         "\trol\tr23\n"
                         "\trol\tr24\n"
                         "\trol\tr25\n"
                         "\tori\tr18, 1\n"))
        return false;
      break;
    case NY_PORT_WASM:
      if (!nyr_put(c->w, "\ti64.const "
                         "1\n\ti64."
                         "shl\n\ti64.const "
                         "1\n\ti64.add\n"))
        return false;
      break;
    }
  }
  switch (c->kind) {
  case NY_PORT_BPF:
    if (!nyr_put(c->w, "\texit\n"))
      return false;
    if (strcmp(c->name, "rt_main") == 0)
      return nyr_put(c->w, "\t.size\trt_main, "
                           ".-rt_main\n"
                           "\t.section "
                           "license,\"aw\"\n"
                           "\t.asciz \"GPL\"\n"
                           "\t.text\n");
    return true;
  case NY_PORT_MIPS:
    return nyr_printf(c->w,
                      "\tld\t$ra, %d($sp)\n"
                      "\tld\t$fp, %d($sp)\n"
                      "\tdaddiu\t$sp, $sp, "
                      "%d\n"
                      "\tjr\t$ra\n\tnop\n",
                      c->frame_bytes - 8, c->frame_bytes - 16, c->frame_bytes);
  case NY_PORT_POWERPC:
    return nyr_printf(c->w,
                      "\taddi\t1, 31, %d\n"
                      "\tld\t0, 16(1)\n"
                      "\tld\t31, -8(1)\n"
                      "\tmtlr\t0\n"
                      "\tblr\n",
                      c->frame_bytes);
  case NY_PORT_AVR: {
    unsigned frame_delta = (0u - (unsigned)c->frame_bytes) & 0xffffu;
    if (!nyr_printf(c->w,
                    "\tsubi\tr28, %u\n"
                    "\tsbci\tr29, %u\n"
                    "\tin\tr0, 63\n"
                    "\tcli\n"
                    "\tout\t62, r29\n"
                    "\tout\t63, r0\n"
                    "\tout\t61, r28\n"
                    "\tpop\tr29\n"
                    "\tpop\tr28\n",
                    frame_delta & 0xffu, (frame_delta >> 8) & 0xffu))
      return false;
    for (int reg = 17; reg >= 10; --reg)
      if (!nyr_printf(c->w, "\tpop\tr%d\n", reg))
        return false;
    return nyr_put(c->w, "\tret\n");
  }
  case NY_PORT_WASM:
    return nyr_put(c->w, ")\n");
  }
  return false;
}

static bool nyr_port_emit_nir(nyr_writer_t *w,
                              const ny_native_target_info_t *target,
                              const nyr_func_t *nyir, const char *func_name,
                              bool tag_return, char *err, size_t err_len,
                              nyr_port_kind_t kind, const char *pretty,
                              const char *ret_reg, const char *tmp0,
                              const char *tmp1, int word_bytes) {
  if (!w || !target || !nyir)
    return false;
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  nyr_port_ctx_t ctx = {.w = w,
                        .target = target,
                        .nyir = nyir,
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
           kind == NY_PORT_WASM ? "$ny_%s_epilogue_%s"
                                : ".Lny_%s_epilogue_%"
                                  "s",
           target->target_name ? target->target_name : "portable", name);
  snprintf(ctx.dispatch_label, sizeof(ctx.dispatch_label), "$dispatch_%s",
           name);
  nyr_port_compute_frame(&ctx);

  if (!nyr_port_header(&ctx))
    return false;

  if (kind == NY_PORT_WASM) {
    for (size_t i = 0; i < nyir->param_count; ++i) {
      if (!nyr_printf(w,
                      "\tlocal.get "
                      "$p%zu\n",
                      i) ||
          !nyr_port_store_local(&ctx, (int)i, "$p"))
        return false;
    }
    if (!nyr_put(w, "\ti32.const "
                    "0\n\tlocal.set "
                    "$pc\n"
                    "\t(block $exit_") ||
        !nyr_put(w, name) ||
        !nyr_printf(w, "\n\t\t(loop %s\n", ctx.dispatch_label))
      return false;
    for (size_t i = 0; i < nyir->len; ++i) {
      ctx.current_index = i;
      if (!nyr_printf(w,
                      "\t\t\tlocal.get "
                      "$pc\n"
                      "\t\t\ti32.const "
                      "%zu\n"
                      "\t\t\ti32.eq\n"
                      "\t\t\tif\n",
                      i) ||
          !nyr_port_emit_inst(&ctx, &nyir->data[i]))
        return false;
      const nyr_op_t op = nyir->data[i].op;
      if (op != NYIR_BR && op != NYIR_BR_IF && op != NYIR_RET &&
          (!nyr_port_wasm_set_pc(&ctx, i + 1) ||
           !nyr_printf(w, "\t\t\t\tbr %s\n", ctx.dispatch_label)))
        return false;
      if (!nyr_put(w, "\t\t\tend\n"))
        return false;
    }
    if (!nyr_put(w, "\t\t\tunreachabl"
                    "e\n\t\t)\n\t)"
                    "\n\tlocal.get "
                    "$result\n"))
      return false;
    if (tag_return && !nyr_put(w, "\ti64.const "
                                  "1\n\ti64."
                                  "shl\n\ti64.const "
                                  "1\n\ti64.add\n"))
      return false;
    if (!nyr_port_footer(&ctx, false))
      return false;
    return true;
  }

  if (strcmp(name, "rt_main") != 0) {
    size_t max = nyir->param_count < target->gp_arg_reg_count
                     ? nyir->param_count
                     : target->gp_arg_reg_count;
    for (size_t i = 0; i < max; ++i) {
      if (!nyr_port_store_local(&ctx, (int)i, target->gp_arg_regs[i]))
        return false;
    }
    if (kind == NY_PORT_AVR)
      for (size_t i = max; i < nyir->param_count; ++i)
        if (!nyr_port_avr_load_stack_param(&ctx, i))
          return false;
  }

  for (size_t i = 0; i < nyir->len; ++i) {
    ctx.current_index = i;
    if (!nyr_port_emit_inst(&ctx, &nyir->data[i])) {
      fprintf(stderr,
              "native NYIR repro "
              "(%s emit failed):\n",
              pretty);
      nyr_dump_compact(stderr, nyir, name);
      return false;
    }
  }

  if (!nyr_port_footer(&ctx, tag_return))
    return false;
  if (kind != NY_PORT_WASM &&
      !(kind == NY_PORT_BPF && strcmp(name, "rt_main") == 0) &&
      strcmp(target->object_format, "macho") != 0 &&
      !nyr_printf(w,
                  "\t.size\t%s%s, "
                  ".-%s%s\n",
                  target->symbol_prefix, name, target->symbol_prefix, name))
    return false;
  return true;
}

bool nyr_bpf_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                     const nyr_func_t *nyir, const char *func_name,
                     bool tag_return, char *err, size_t err_len) {
  return nyr_port_emit_nir(w, target, nyir, func_name, tag_return, err, err_len,
                           NY_PORT_BPF, "BPF", "r0", "r6", "r7", 8);
}

bool nyr_mips_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                      const nyr_func_t *nyir, const char *func_name,
                      bool tag_return, char *err, size_t err_len) {
  return nyr_port_emit_nir(w, target, nyir, func_name, tag_return, err, err_len,
                           NY_PORT_MIPS, "MIPS", "$v0", "$t0", "$t1", 8);
}

bool nyr_powerpc_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                         const nyr_func_t *nyir, const char *func_name,
                         bool tag_return, char *err, size_t err_len) {
  return nyr_port_emit_nir(w, target, nyir, func_name, tag_return, err, err_len,
                           NY_PORT_POWERPC, "PowerPC", "r3", "r4", "r5", 8);
}

bool nyr_avr_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                     const nyr_func_t *nyir, const char *func_name,
                     bool tag_return, char *err, size_t err_len) {
  if (!w || !target || !nyir || nyir->return_aggregate_size != 0 ||
      nyir->param_count > NYIR_CALL_MAX_ARGS) {
    nyr_set_error(err, err_len,
                  "AVR portable emitter "
                  "supports scalar "
                  "functions with at most "
                  "64 i64 parameters");
    return false;
  }
  for (size_t i = 0; i < nyir->param_count; ++i) {
    bool aggregate_abi = nyir->param_abi && nyir->param_abi[i] != 0;
    bool supported_scalar = !nyir->param_types ||
                            nyir->param_types[i] == NYIR_PARAM_I64 ||
                            nyir->param_types[i] == NYIR_PARAM_F32 ||
                            nyir->param_types[i] == NYIR_PARAM_F64;
    if (aggregate_abi || !supported_scalar) {
      nyr_set_error(err, err_len,
                    "AVR portable emitter "
                    "does not support aggregate parameters");
      return false;
    }
  }
  int args[NYIR_CALL_MAX_ARGS];
  nyr_port_ctx_t validation = {
      .nyir = nyir, .kind = NY_PORT_AVR, .word_bytes = 8};
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
        (uint64_t)in->imm < nyir->param_count &&
        in->scalar_class == NYIR_SCALAR_AGGREGATE) {
      nyr_set_error(err, err_len,
                    "AVR portable emitter "
                    "does not support aggregate parameters");
      return false;
    }
    switch (in->op) {
    case NYIR_NOP:
    case NYIR_LABEL:
    case NYIR_CONST_I64:
    case NYIR_CONST_F32:
    case NYIR_CONST_F64:
    case NYIR_COPY:
    case NYIR_COPY_STRUCT:
    case NYIR_ADDR_SYMBOL:
    case NYIR_ADD_I64:
    case NYIR_MUL_I64:
    case NYIR_SUB_I64:
    case NYIR_DIV_I64:
    case NYIR_MOD_I64:
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
    case NYIR_ROR_I64:
    case NYIR_ROR32_I64:
    case NYIR_AND_I64:
    case NYIR_OR_I64:
    case NYIR_XOR_I64:
    case NYIR_BSWAP_I64:
    case NYIR_BSWAP32_I64:
    case NYIR_CMP_I64:
    case NYIR_CMP_F64:
    case NYIR_CMP_F32:
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
    case NYIR_DIV_F64:
    case NYIR_ADD_F32:
    case NYIR_SUB_F32:
    case NYIR_MUL_F32:
    case NYIR_DIV_F32:
    case NYIR_F32_TO_I64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_I64:
    case NYIR_I64_TO_F64:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
    case NYIR_LOAD_LOCAL:
    case NYIR_STORE_LOCAL:
    case NYIR_ADDR_LOCAL:
    case NYIR_LOAD_I64:
    case NYIR_STORE_I64:
    case NYIR_RET:
    case NYIR_BR:
    case NYIR_BR_IF:
    case NYIR_SELECT_I64:
    case NYIR_BOUNDS_CHECK:
      break;
    case NYIR_CALL: {
      int argc = 0;
      bool call_args_ok = nyr_call_args(
          in, nyir->next_value, args, NYIR_CALL_MAX_ARGS, &argc, err, err_len);
      bool supported_panic_pair = false;
      bool supported_raw_pointer_call = false;
      bool supported_float_constant = false;
      if (call_args_ok && in->symbol) {
        validation.current_index = i;
        supported_float_constant = (strcmp(in->symbol, "rt_flt_nan") == 0 ||
                                    strcmp(in->symbol, "rt_flt_inf") == 0) &&
                                   argc == 0 && in->dst >= 0 &&
                                   in->scalar_class == NYIR_SCALAR_F64;
        bool is_alloc = strcmp(in->symbol, "rt_alloc_"
                                           "string") == 0;
        bool is_panic = strcmp(in->symbol, "rt_panic") == 0;
        bool is_cstr_to_str = strcmp(in->symbol, "rt_cstr_to_"
                                                 "str") == 0;
        supported_raw_pointer_call = nyr_port_avr_raw_pointer_call(in, argc);
        supported_panic_pair =
            (is_alloc || is_panic) &&
            nyr_port_panic_runtime_pair(&validation, in, is_alloc);
        if (is_cstr_to_str && argc == 1)
          supported_panic_pair =
              nyr_port_static_string_value(&validation, args[0], i);
      }
      bool uses_register_abi =
          in->symbol && ((in->flags & NYIR_INST_F_EXTERN) ||
                         ny_native_runtime_abi_decl(in->symbol) != NULL);
      bool external_stack_args_supported =
          call_args_ok && nyr_port_avr_external_args_supported(in);
      bool argument_count_supported =
          argc <= (int)target->gp_arg_reg_count ||
          (!uses_register_abi && argc <= NYIR_CALL_MAX_ARGS) ||
          external_stack_args_supported;
      if (!in->symbol || !call_args_ok || !argument_count_supported ||
          (uses_register_abi && !external_stack_args_supported &&
           !supported_panic_pair && !supported_raw_pointer_call &&
           !supported_float_constant)) {
        nyr_set_error(err, err_len,
                      "AVR portable "
                      "emitter rejects "
                      "call '%s' at NYIR "
                      "instruction %zu "
                      "direct internal "
                      "calls use the two "
                      "register arguments "
                      "then stack-passed "
                      "i64 arguments and "
                      "external C calls use "
                      "that convention for "
                      "stack-passed raw i64; "
                      "runtime calls "
                      "require a supported "
                      "raw-pointer ABI "
                      "entry or a literal "
                      "panic pair)",
                      in->symbol ? in->symbol : "<unnamed>", i);
        return false;
      }
      break;
    }
    default:
      nyr_set_error(err, err_len,
                    "AVR portable emitter "
                    "does not support "
                    "NYIR operation %s",
                    nyr_op_name(in->op));
      return false;
    }
  }

  nyr_port_ctx_t layout = {.nyir = nyir, .kind = NY_PORT_AVR, .word_bytes = 8};
  nyr_port_compute_frame(&layout);
  if (layout.frame_bytes < 0 || layout.frame_bytes > 1024) {
    nyr_set_error(err, err_len,
                  "AVR portable emitter "
                  "supports at most 1024 "
                  "frame bytes");
    return false;
  }
  return nyr_port_emit_nir(w, target, nyir, func_name, tag_return, err, err_len,
                           NY_PORT_AVR, "AVR", "r18:r25", "r18:r25", "r10:r17",
                           8);
}

bool nyr_wasm_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                      const nyr_func_t *nyir, const char *func_name,
                      bool tag_return, char *err, size_t err_len) {
  return nyr_port_emit_nir(w, target, nyir, func_name, tag_return, err, err_len,
                           NY_PORT_WASM, "WebAssembly", "$ret", "$t0", "$t1",
                           8);
}
