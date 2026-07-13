#include "code/native/object/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal AArch64 encoder and ELF64 packager for the proven scalar AAPCS64
 * NYIR slice. It deliberately rejects floating/aggregate shapes until their
 * ABI classification is represented; object success never invokes an
 * assembler or another compiler. */

typedef struct {
  int64_t label;
  size_t off;
} ny_a64_label_t;

typedef struct {
  int64_t label;
  size_t off;
  bool conditional;
} ny_a64_patch_t;

typedef struct {
  char symbol[256];
  size_t off;
} ny_a64_reloc_t;

typedef struct {
  char name[256];
  size_t off;
  size_t size;
} ny_a64_def_t;

typedef struct {
  ny_obj_buf_t code;
  const ny_nir_func_t *nir;
  const ny_native_target_info_t *target;
  int value_slots;
  int local_slots;
  int local_base;
  int frame_bytes;
  ny_nir_type_map_t types;
  ny_a64_label_t labels[1024];
  size_t label_count;
  ny_a64_patch_t patches[1024];
  size_t patch_count;
  size_t returns[1024];
  size_t return_count;
  ny_a64_reloc_t relocs[256];
  size_t reloc_count;
  char *err;
  size_t err_len;
} ny_a64_obj_ctx_t;

static int ny_a64_align(int value, int align) {
  return (value + align - 1) & ~(align - 1);
}

static bool ny_a64_u32(ny_a64_obj_ctx_t *c, uint32_t word) {
  if (!ny_obj_u32(&c->code, word)) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: out of memory");
    return false;
  }
  return true;
}

static bool ny_a64_reg_mem(ny_a64_obj_ctx_t *c, bool load, unsigned reg,
                           int off) {
  if (reg > 30 || off < 0 || off > 32760 || (off & 7) != 0) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: invalid stack access reg=%u off=%d",
                      reg, off);
    return false;
  }
  uint32_t op = load ? 0xf94003e0u : 0xf90003e0u;
  return ny_a64_u32(c, op | ((uint32_t)(off / 8) << 10) | reg);
}

static int ny_a64_value_off(const ny_a64_obj_ctx_t *c, int value) {
  (void)c;
  return value * 8;
}

static int ny_a64_local_off(const ny_a64_obj_ctx_t *c, int local) {
  return c->local_base + local * 8;
}

static bool ny_a64_check_value(ny_a64_obj_ctx_t *c, int value,
                               const char *role) {
  if (value >= 0 && value < c->value_slots)
    return true;
  ny_native_set_err(c->err, c->err_len,
                    "AArch64 object writer: invalid %s value v%d", role,
                    value);
  return false;
}

static bool ny_a64_load_value(ny_a64_obj_ctx_t *c, unsigned reg, int value) {
  return ny_a64_check_value(c, value, "source") &&
         ny_a64_reg_mem(c, true, reg, ny_a64_value_off(c, value));
}

static bool ny_a64_store_value(ny_a64_obj_ctx_t *c, int value, unsigned reg) {
  return ny_a64_check_value(c, value, "destination") &&
         ny_a64_reg_mem(c, false, reg, ny_a64_value_off(c, value));
}

static bool ny_a64_load_local(ny_a64_obj_ctx_t *c, unsigned reg, int local) {
  if (local < 0 || local >= c->local_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: invalid local slot %d", local);
    return false;
  }
  return ny_a64_reg_mem(c, true, reg, ny_a64_local_off(c, local));
}

static bool ny_a64_store_local(ny_a64_obj_ctx_t *c, int local, unsigned reg) {
  if (local < 0 || local >= c->local_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: invalid local slot %d", local);
    return false;
  }
  return ny_a64_reg_mem(c, false, reg, ny_a64_local_off(c, local));
}

static bool ny_a64_fp_mem(ny_a64_obj_ctx_t *c, bool load, bool f32,
                          unsigned reg, int off) {
  unsigned scale = f32 ? 4u : 8u;
  if (reg > 31 || off < 0 || (off % (int)scale) != 0 ||
      off / (int)scale > 4095) return false;
  uint32_t op = f32 ? (load ? 0xbd4003e0u : 0xbd0003e0u)
                    : (load ? 0xfd4003e0u : 0xfd0003e0u);
  return ny_a64_u32(c, op | ((uint32_t)(off / (int)scale) << 10) | reg);
}

static bool ny_a64_load_fp_value(ny_a64_obj_ctx_t *c, unsigned reg, int value,
                                 bool f32) {
  return ny_a64_check_value(c, value, "floating source") &&
         ny_a64_fp_mem(c, true, f32, reg, ny_a64_value_off(c, value));
}

static bool ny_a64_store_fp_value(ny_a64_obj_ctx_t *c, int value,
                                  unsigned reg, bool f32) {
  return ny_a64_check_value(c, value, "floating destination") &&
         ny_a64_fp_mem(c, false, f32, reg, ny_a64_value_off(c, value));
}

static bool ny_a64_fp_binop(ny_a64_obj_ctx_t *c, const ny_nir_inst_t *in,
                            uint32_t op, bool f32) {
  return ny_a64_load_fp_value(c, 0, in->a, f32) &&
         ny_a64_load_fp_value(c, 1, in->b, f32) &&
         ny_a64_u32(c, op | (1u << 16)) &&
         ny_a64_store_fp_value(c, in->dst, 0, f32);
}

static bool ny_a64_mov_imm(ny_a64_obj_ctx_t *c, unsigned reg, int64_t value) {
  uint64_t bits = (uint64_t)value;
  if (!ny_a64_u32(c, 0xd2800000u | ((uint32_t)(bits & 0xffffu) << 5) | reg))
    return false;
  for (unsigned shift = 16; shift < 64; shift += 16) {
    unsigned part = (unsigned)((bits >> shift) & 0xffffu);
    if (part &&
        !ny_a64_u32(c, 0xf2800000u | ((shift / 16u) << 21) |
                           ((uint32_t)part << 5) | reg))
      return false;
  }
  return true;
}

static bool ny_a64_binop(ny_a64_obj_ctx_t *c, const ny_nir_inst_t *in,
                         uint32_t op) {
  return ny_a64_load_value(c, 0, in->a) &&
         ny_a64_load_value(c, 1, in->b) &&
         ny_a64_u32(c, op | (1u << 16)) &&
         ny_a64_store_value(c, in->dst, 0);
}

static unsigned ny_a64_cond(ny_nir_cmp_t cmp) {
  static const unsigned conds[] = {0, 1, 11, 13, 12, 10};
  return (unsigned)cmp < sizeof(conds) / sizeof(conds[0]) ? conds[cmp] : 0;
}

static bool ny_a64_add_label(ny_a64_obj_ctx_t *c, int64_t label) {
  if (c->label_count >= sizeof(c->labels) / sizeof(c->labels[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: too many labels");
    return false;
  }
  c->labels[c->label_count++] =
      (ny_a64_label_t){.label = label, .off = c->code.len};
  return true;
}

static bool ny_a64_add_patch(ny_a64_obj_ctx_t *c, int64_t label,
                             bool conditional) {
  if (c->patch_count >= sizeof(c->patches) / sizeof(c->patches[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: too many branch patches");
    return false;
  }
  size_t off = c->code.len;
  c->patches[c->patch_count++] = (ny_a64_patch_t){
      .label = label, .off = off, .conditional = conditional};
  return ny_a64_u32(c, conditional ? 0x54000001u : 0x14000000u);
}

static bool ny_a64_patch_branch(ny_a64_obj_ctx_t *c, size_t off,
                                size_t target, bool conditional) {
  int64_t delta = (int64_t)target - (int64_t)off;
  if ((delta & 3) != 0) return false;
  int64_t words = delta / 4;
  uint32_t insn = 0;
  memcpy(&insn, c->code.data + off, sizeof(insn));
  if (conditional) {
    if (words < -(1 << 18) || words >= (1 << 18)) return false;
    insn = (insn & ~0x00ffffe0u) | (((uint32_t)words & 0x7ffffu) << 5);
  } else {
    if (words < -(1 << 25) || words >= (1 << 25)) return false;
    insn = (insn & 0xfc000000u) | ((uint32_t)words & 0x03ffffffu);
  }
  ny_obj_patch_u32(&c->code, off, insn);
  return true;
}

static bool ny_a64_add_reloc(ny_a64_obj_ctx_t *c, const char *symbol) {
  if (!symbol || !*symbol ||
      c->reloc_count >= sizeof(c->relocs) / sizeof(c->relocs[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: invalid or excessive relocation");
    return false;
  }
  ny_a64_reloc_t *r = &c->relocs[c->reloc_count++];
  snprintf(r->symbol, sizeof(r->symbol), "%s", symbol);
  r->off = c->code.len;
  return ny_a64_u32(c, 0x94000000u);
}

static bool ny_a64_emit_inst(ny_a64_obj_ctx_t *c,
                             const ny_nir_inst_t *in) {
  switch (in->op) {
  case NY_NIR_NOP: return true;
  case NY_NIR_CONST_I64:
    return ny_a64_mov_imm(c, 0, in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NY_NIR_COPY:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_store_value(c, in->dst, 0);
  case NY_NIR_ADD_I64: return ny_a64_binop(c, in, 0x8b000000u);
  case NY_NIR_SUB_I64: return ny_a64_binop(c, in, 0xcb000000u);
  case NY_NIR_MUL_I64: return ny_a64_binop(c, in, 0x9b007c00u);
  case NY_NIR_AND_I64: return ny_a64_binop(c, in, 0x8a000000u);
  case NY_NIR_OR_I64: return ny_a64_binop(c, in, 0xaa000000u);
  case NY_NIR_XOR_I64: return ny_a64_binop(c, in, 0xca000000u);
  case NY_NIR_SHL_I64: return ny_a64_binop(c, in, 0x9ac02000u);
  case NY_NIR_SAR_I64: return ny_a64_binop(c, in, 0x9ac02800u);
  case NY_NIR_DIV_I64: return ny_a64_binop(c, in, 0x9ac00c00u);
  case NY_NIR_MOD_I64:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_load_value(c, 1, in->b) &&
           ny_a64_u32(c, 0x9ac10c02u) && /* sdiv x2,x0,x1 */
           ny_a64_u32(c, 0x9b018040u) && /* msub x0,x2,x1,x0 */
           ny_a64_store_value(c, in->dst, 0);
  case NY_NIR_CMP_I64: {
    unsigned inverse = ny_a64_cond(in->cmp) ^ 1u;
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_load_value(c, 1, in->b) &&
           ny_a64_u32(c, 0xeb01001fu) &&
           ny_a64_u32(c, 0x9a9f07e0u | (inverse << 12)) &&
           ny_a64_store_value(c, in->dst, 0);
  }
  case NY_NIR_LABEL: return ny_a64_add_label(c, in->imm);
  case NY_NIR_LOAD_LOCAL:
    return ny_a64_load_local(c, 0, (int)in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NY_NIR_STORE_LOCAL:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_store_local(c, (int)in->imm, 0);
  case NYIR_ADDR_LOCAL: {
    if (in->imm < 0 || in->imm >= c->local_slots) return false;
    int off = ny_a64_local_off(c, (int)in->imm);
    if (off > 4095) return false;
    return ny_a64_u32(c, 0x910003e0u | ((uint32_t)off << 10)) &&
           ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_LOAD_I64:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0xf9400000u) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_STORE_I64:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_load_value(c, 1, in->c) &&
           ny_a64_u32(c, 0xf9000001u);
  case NYIR_CONST_F64:
    return ny_a64_mov_imm(c, 0, in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_CONST_F32:
    return ny_a64_mov_imm(c, 0, (uint32_t)in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_ADD_F64: return ny_a64_fp_binop(c, in, 0x1e602800u, false);
  case NYIR_SUB_F64: return ny_a64_fp_binop(c, in, 0x1e603800u, false);
  case NYIR_MUL_F64: return ny_a64_fp_binop(c, in, 0x1e600800u, false);
  case NYIR_DIV_F64: return ny_a64_fp_binop(c, in, 0x1e601800u, false);
  case NYIR_ADD_F32: return ny_a64_fp_binop(c, in, 0x1e202800u, true);
  case NYIR_SUB_F32: return ny_a64_fp_binop(c, in, 0x1e203800u, true);
  case NYIR_MUL_F32: return ny_a64_fp_binop(c, in, 0x1e200800u, true);
  case NYIR_DIV_F32: return ny_a64_fp_binop(c, in, 0x1e201800u, true);
  case NYIR_I64_TO_F64:
    return ny_a64_load_value(c, 0, in->a) && ny_a64_u32(c, 0x9e620000u) &&
           ny_a64_store_fp_value(c, in->dst, 0, false);
  case NYIR_I64_TO_F32:
    return ny_a64_load_value(c, 0, in->a) && ny_a64_u32(c, 0x9e220000u) &&
           ny_a64_store_fp_value(c, in->dst, 0, true);
  case NYIR_F64_TO_F32:
    return ny_a64_load_fp_value(c, 0, in->a, false) &&
           ny_a64_u32(c, 0x1e624000u) &&
           ny_a64_store_fp_value(c, in->dst, 0, true);
  case NYIR_F32_TO_F64:
    return ny_a64_load_fp_value(c, 0, in->a, true) &&
           ny_a64_u32(c, 0x1e22c000u) &&
           ny_a64_store_fp_value(c, in->dst, 0, false);
  case NYIR_CMP_F64:
  case NYIR_CMP_F32: {
    bool f32 = in->op == NYIR_CMP_F32;
    unsigned inverse = ny_a64_cond(in->cmp) ^ 1u;
    return ny_a64_load_fp_value(c, 0, in->a, f32) &&
           ny_a64_load_fp_value(c, 1, in->b, f32) &&
           ny_a64_u32(c, (f32 ? 0x1e212000u : 0x1e612000u)) &&
           ny_a64_u32(c, 0x9a9f07e0u | (inverse << 12)) &&
           ny_a64_store_value(c, in->dst, 0);
  }
  case NY_NIR_BR: return ny_a64_add_patch(c, in->imm, false);
  case NY_NIR_BR_IF:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0xf100001fu) &&
           ny_a64_add_patch(c, in->imm, true);
  case NY_NIR_CALL: {
    int args[NY_NIR_CALL_MAX_ARGS];
    int argc = 0;
    if (in->arg_sizes) {
      for (int i = 0; i < (int)in->imm; ++i)
        if (in->arg_sizes[i]) {
          ny_native_set_err(c->err, c->err_len,
                            "AArch64 object writer: aggregate call ABI is not represented");
          return false;
        }
    }
    if (!ny_nir_call_args(in, c->value_slots, args, NY_NIR_CALL_MAX_ARGS,
                          &argc, c->err, c->err_len))
      return false;
    if (argc > 8) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 object writer: %d scalar arguments exceed AAPCS64 registers",
                        argc);
      return false;
    }
    int gp = 0, fp = 0;
    for (int i = 0; i < argc; ++i) {
      bool f64 = c->types.value_f64[args[i]];
      bool f32 = c->types.value_f32[args[i]];
      if (f64 || f32) {
        if (fp >= 8 || !ny_a64_load_fp_value(c, (unsigned)fp++, args[i], f32))
          return false;
      } else if (gp >= 8 || !ny_a64_load_value(c, (unsigned)gp++, args[i])) {
        return false;
      }
    }
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%s%s%s",
             c->target->symbol_prefix ? c->target->symbol_prefix : "",
             (in->flags & NY_NIR_INST_F_EXTERN) ? "" : "ny_fn_",
             in->symbol ? in->symbol : "");
    if (!ny_a64_add_reloc(c, symbol)) return false;
    if (in->dst < 0) return true;
    if (in->flags & NY_NIR_INST_F_RET_F64)
      return ny_a64_store_fp_value(c, in->dst, 0, false);
    if (in->flags & NY_NIR_INST_F_RET_F32)
      return ny_a64_store_fp_value(c, in->dst, 0, true);
    return ny_a64_store_value(c, in->dst, 0);
  }
  case NY_NIR_RET:
    if (in->a >= 0) {
      bool f64 = c->types.value_f64[in->a];
      bool f32 = c->types.value_f32[in->a];
      if ((f64 || f32) ? !ny_a64_load_fp_value(c, 0, in->a, f32)
                       : !ny_a64_load_value(c, 0, in->a))
        return false;
    }
    if (c->return_count >= sizeof(c->returns) / sizeof(c->returns[0]))
      return false;
    c->returns[c->return_count++] = c->code.len;
    return ny_a64_u32(c, 0x14000000u);
  case NYIR_ADDR_SYMBOL: case NYIR_ALLOCA:
  case NYIR_COPY_STRUCT: case NYIR_CAPTURE_RET: case NYIR_OP_COUNT:
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: unsupported op %s",
                      ny_nir_op_name(in->op));
    return false;
  }
  return false;
}

static bool ny_a64_emit_code(ny_a64_obj_ctx_t *c, bool user_function,
                             bool tag_return) {
  c->value_slots = c->nir->next_value;
  c->local_slots = (int)ny_native_nir_local_count(c->nir);
  c->local_base = c->value_slots * 8;
  c->frame_bytes = ny_a64_align((c->value_slots + c->local_slots) * 8, 16);
  if (!ny_nir_type_map_init(&c->types, c->nir, (size_t)c->local_slots)) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: type classification allocation failed");
    return false;
  }
  if (c->frame_bytes > 4095) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: frame %d exceeds immediate slice",
                      c->frame_bytes);
    return false;
  }
  if (!ny_a64_u32(c, 0xa9bf7bfdu) || !ny_a64_u32(c, 0x910003fdu) ||
      (c->frame_bytes &&
       !ny_a64_u32(c, 0xd10003ffu | ((uint32_t)c->frame_bytes << 10))))
    return false;
  if (user_function) {
    int gp = 0, fp = 0;
    int limit = c->local_slots < 8 ? c->local_slots : 8;
    for (int i = 0; i < limit; ++i) {
      bool f64 = c->types.local_f64[i], f32 = c->types.local_f32[i];
      if (f64 || f32) {
        if (fp >= 8 || !ny_a64_fp_mem(c, false, f32, (unsigned)fp++,
                                      ny_a64_local_off(c, i))) return false;
      } else {
        if (gp >= 8 || !ny_a64_store_local(c, i, (unsigned)gp++)) return false;
      }
    }
  }
  for (size_t i = 0; i < c->nir->len; ++i)
    if (!ny_a64_emit_inst(c, &c->nir->data[i])) return false;
  size_t epilogue = c->code.len;
  if (tag_return &&
      (!ny_a64_u32(c, 0xd37ff800u) || !ny_a64_u32(c, 0x91000400u)))
    return false;
  if ((c->frame_bytes &&
       !ny_a64_u32(c, 0x910003ffu | ((uint32_t)c->frame_bytes << 10))) ||
      !ny_a64_u32(c, 0xa8c17bfdu) || !ny_a64_u32(c, 0xd65f03c0u))
    return false;
  for (size_t i = 0; i < c->patch_count; ++i) {
    size_t target = SIZE_MAX;
    for (size_t j = 0; j < c->label_count; ++j)
      if (c->labels[j].label == c->patches[i].label) {
        target = c->labels[j].off;
        break;
      }
    if (target == SIZE_MAX ||
        !ny_a64_patch_branch(c, c->patches[i].off, target,
                             c->patches[i].conditional)) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 object writer: unresolved/out-of-range label %lld",
                        (long long)c->patches[i].label);
      return false;
    }
  }
  for (size_t i = 0; i < c->return_count; ++i)
    if (!ny_a64_patch_branch(c, c->returns[i], epilogue, false)) return false;
  return true;
}

static int ny_a64_def_index(const ny_a64_def_t *defs, size_t count,
                            const char *name) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(defs[i].name, name) == 0) return (int)i;
  return -1;
}

static int ny_a64_name_index(char names[][256], size_t count,
                             const char *name) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(names[i], name) == 0) return (int)i;
  return -1;
}

static bool ny_a64_append(ny_obj_buf_t *code, ny_a64_def_t *defs,
                          size_t *def_count, ny_a64_reloc_t *relocs,
                          size_t *reloc_count, const ny_nir_func_t *nir,
                          const ny_native_target_info_t *target,
                          const char *symbol, bool user_function,
                          bool tag_return, char *err, size_t err_len) {
  if (*def_count >= 256 || ny_a64_def_index(defs, *def_count, symbol) >= 0)
    return false;
  if (!ny_obj_pad_to(code, 16)) return false;
  size_t start = code->len;
  ny_a64_obj_ctx_t c = {.nir = nir, .target = target, .err = err,
                        .err_len = err_len};
  if (!ny_a64_emit_code(&c, user_function, tag_return)) {
    ny_nir_type_map_free(&c.types);
    ny_obj_free(&c.code);
    return false;
  }
  if (*reloc_count + c.reloc_count > 256 ||
      !ny_obj_emit(code, c.code.data, c.code.len)) {
    ny_obj_free(&c.code);
    return false;
  }
  ny_a64_def_t *def = &defs[(*def_count)++];
  snprintf(def->name, sizeof(def->name), "%s", symbol);
  def->off = start;
  def->size = c.code.len;
  for (size_t i = 0; i < c.reloc_count; ++i) {
    relocs[*reloc_count] = c.relocs[i];
    relocs[*reloc_count].off += start;
    (*reloc_count)++;
  }
  ny_obj_free(&c.code);
  ny_nir_type_map_free(&c.types);
  return true;
}

static bool ny_a64_elf_sym(ny_obj_buf_t *b, uint32_t name, unsigned info,
                           uint16_t shndx, uint64_t value, uint64_t size) {
  return ny_obj_u32(b, name) && ny_obj_u8(b, info) && ny_obj_u8(b, 0) &&
         ny_obj_u16(b, shndx) && ny_obj_u64(b, value) && ny_obj_u64(b, size);
}

static bool ny_a64_elf_sh(ny_obj_buf_t *b, uint32_t name, uint32_t type,
                          uint64_t flags, uint64_t off, uint64_t size,
                          uint32_t link, uint32_t info, uint64_t align,
                          uint64_t entsize) {
  return ny_obj_u32(b, name) && ny_obj_u32(b, type) && ny_obj_u64(b, flags) &&
         ny_obj_u64(b, 0) && ny_obj_u64(b, off) && ny_obj_u64(b, size) &&
         ny_obj_u32(b, link) && ny_obj_u32(b, info) && ny_obj_u64(b, align) &&
         ny_obj_u64(b, entsize);
}

static bool ny_a64_write_file(const char *path, const ny_obj_buf_t *file,
                              char *err, size_t err_len) {
  FILE *out = fopen(path, "wb");
  if (!out) {
    ny_native_set_err(err, err_len, "AArch64 ELF writer: cannot open %s: %s",
                      path, strerror(errno));
    return false;
  }
  bool ok = fwrite(file->data, 1, file->len, out) == file->len;
  if (fclose(out) != 0) ok = false;
  if (!ok)
    ny_native_set_err(err, err_len, "AArch64 ELF writer: failed writing %s",
                      path);
  return ok;
}

bool ny_a64_obj_build_bundle(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *out_defs,
    size_t *out_def_count, ny_x64_obj_reloc_t *out_relocs,
    size_t *out_reloc_count, char *err, size_t err_len) {
  if (!rt_main || !target || !entry_symbol || !*entry_symbol || !code ||
      !out_defs || !out_def_count || !out_relocs || !out_reloc_count)
    return false;
  ny_a64_def_t defs[256];
  ny_a64_reloc_t relocs[256];
  size_t def_count = 0, reloc_count = 0;
  for (size_t i = 0; i < func_count; ++i) {
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%sny_fn_%s", target->symbol_prefix,
             func_names && func_names[i] ? func_names[i] : "unknown_fn");
    if (!ny_a64_append(code, defs, &def_count, relocs, &reloc_count,
                       &funcs[i], target, symbol, true, false, err, err_len))
      return false;
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix, entry_symbol);
  if (!ny_a64_append(code, defs, &def_count, relocs, &reloc_count, rt_main,
                     target, entry, false, tag_return, err, err_len))
    return false;
  for (size_t i = 0; i < def_count; ++i) {
    snprintf(out_defs[i].name, sizeof(out_defs[i].name), "%s", defs[i].name);
    out_defs[i].off = defs[i].off;
    out_defs[i].size = defs[i].size;
  }
  for (size_t i = 0; i < reloc_count; ++i) {
    snprintf(out_relocs[i].symbol, sizeof(out_relocs[i].symbol), "%s",
             relocs[i].symbol);
    out_relocs[i].disp_off = relocs[i].off;
    out_relocs[i].type = NY_RELOC_AARCH64_CALL26;
  }
  *out_def_count = def_count;
  *out_reloc_count = reloc_count;
  return true;
}

bool ny_native_emit_elf64_aarch64_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  if (!rt_main || !target || !path || !entry_symbol || !*entry_symbol) return false;
  ny_obj_buf_t code = {0}, file = {0}, strtab = {0};
  ny_a64_def_t defs[256];
  ny_a64_reloc_t relocs[256];
  size_t def_count = 0, reloc_count = 0;
  bool ok = false;
  for (size_t i = 0; i < func_count; ++i) {
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%sny_fn_%s", target->symbol_prefix,
             func_names && func_names[i] ? func_names[i] : "unknown_fn");
    if (!ny_a64_append(&code, defs, &def_count, relocs, &reloc_count,
                       &funcs[i], target, symbol, true, false, err, err_len))
      goto done;
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix, entry_symbol);
  if (!ny_a64_append(&code, defs, &def_count, relocs, &reloc_count, rt_main,
                     target, entry, false, tag_return, err, err_len))
    goto done;

  char externs[256][256];
  size_t extern_count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (ny_a64_def_index(defs, def_count, relocs[i].symbol) >= 0 ||
        ny_a64_name_index(externs, extern_count, relocs[i].symbol) >= 0)
      continue;
    if (extern_count >= 256) goto done;
    snprintf(externs[extern_count++], 256, "%s", relocs[i].symbol);
  }
  uint32_t def_names[256] = {0}, ext_names[256] = {0};
  if (!ny_obj_u8(&strtab, 0)) goto done;
  for (size_t i = 0; i < def_count; ++i) {
    def_names[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, defs[i].name, strlen(defs[i].name) + 1)) goto done;
  }
  for (size_t i = 0; i < extern_count; ++i) {
    ext_names[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, externs[i], strlen(externs[i]) + 1)) goto done;
  }
  const char shstr[] = "\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0";
  if (!ny_obj_zero(&file, 64) || !ny_obj_pad_to(&file, 16)) goto done;
  size_t text_off = file.len;
  if (!ny_obj_emit(&file, code.data, code.len) || !ny_obj_pad_to(&file, 8)) goto done;
  size_t rela_off = file.len;
  for (size_t i = 0; i < reloc_count; ++i) {
    int di = ny_a64_def_index(defs, def_count, relocs[i].symbol);
    int ei = di < 0 ? ny_a64_name_index(externs, extern_count, relocs[i].symbol) : -1;
    if (di < 0 && ei < 0) goto done;
    uint32_t sym = di >= 0 ? (uint32_t)(1 + di)
                           : (uint32_t)(1 + def_count + (size_t)ei);
    uint64_t info = ((uint64_t)sym << 32) | 283u; /* R_AARCH64_CALL26 */
    if (!ny_obj_u64(&file, relocs[i].off) || !ny_obj_u64(&file, info) ||
        !ny_obj_u64(&file, 0)) goto done;
  }
  size_t rela_size = file.len - rela_off;
  size_t symtab_off = file.len;
  if (!ny_a64_elf_sym(&file, 0, 0, 0, 0, 0)) goto done;
  for (size_t i = 0; i < def_count; ++i)
    if (!ny_a64_elf_sym(&file, def_names[i], 0x12, 1, defs[i].off,
                        defs[i].size)) goto done;
  for (size_t i = 0; i < extern_count; ++i)
    if (!ny_a64_elf_sym(&file, ext_names[i], 0x12, 0, 0, 0)) goto done;
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!ny_obj_emit(&file, strtab.data, strtab.len)) goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstr_off = file.len;
  if (!ny_obj_emit(&file, shstr, sizeof(shstr)) || !ny_obj_pad_to(&file, 8)) goto done;
  size_t shoff = file.len;
  if (!ny_a64_elf_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !ny_a64_elf_sh(&file, 1, 1, 0x6, text_off, code.len, 0, 0, 16, 0) ||
      !ny_a64_elf_sh(&file, 7, 4, 0, rela_off, rela_size, 3, 1, 8, 24) ||
      !ny_a64_elf_sh(&file, 18, 2, 0, symtab_off, symtab_size, 4, 1, 8, 24) ||
      !ny_a64_elf_sh(&file, 26, 3, 0, strtab_off, strtab_size, 0, 0, 1, 0) ||
      !ny_a64_elf_sh(&file, 34, 3, 0, shstr_off, sizeof(shstr), 0, 0, 1, 0))
    goto done;
  file.data[0] = 0x7f; file.data[1] = 'E'; file.data[2] = 'L'; file.data[3] = 'F';
  file.data[4] = 2; file.data[5] = 1; file.data[6] = 1;
  ny_obj_patch_u16(&file, 16, 1);   /* ET_REL */
  ny_obj_patch_u16(&file, 18, 183); /* EM_AARCH64 */
  ny_obj_patch_u32(&file, 20, 1);
  ny_obj_patch_u64(&file, 40, shoff);
  ny_obj_patch_u16(&file, 52, 64);
  ny_obj_patch_u16(&file, 58, 64);
  ny_obj_patch_u16(&file, 60, 6);
  ny_obj_patch_u16(&file, 62, 5);
  ok = ny_a64_write_file(path, &file, err, err_len);
done:
  ny_obj_free(&strtab); ny_obj_free(&file); ny_obj_free(&code);
  if (!ok && err && err_len && !err[0])
    ny_native_set_err(err, err_len, "AArch64 ELF object writer failed");
  return ok;
}
