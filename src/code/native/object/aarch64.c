/*
 * AArch64 object encoder: machine-form -> A64 instruction encoding
 * for the ARM 64-bit native backend with FPR and NEON support.
 * Target-specific object scaffolding stays local to preserve the AArch64
 * relocation and instruction-layout invariants.
 */
#include "code/native/object/internal.h"
#include "base/parallel.h"
#include "code/native/ir/machine.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Internal AArch64 encoder and ELF64 packager for AAPCS64 scalar + small
 * aggregate (≤16B register / pointer) NYIR. Object success never invokes an
 * assembler or another compiler.
 */

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
  const nyir_func_t *nyir;
  const ny_native_target_info_t *target;
  int value_slots;
  int local_slots;
  int local_base;
  int frame_bytes;
  int *value_offs;
  int *local_offs;
  int *alloca_offs;
  nyir_type_map_t types;
  ny_a64_label_t labels[1024];
  size_t label_count;
  ny_a64_patch_t patches[1024];
  size_t patch_count;
  size_t returns[1024];
  size_t return_count;
  ny_a64_reloc_t *relocs;
  size_t reloc_count;
  size_t reloc_cap;
  char *err;
  size_t err_len;
} ny_a64_obj_ctx_t;

/*
 * Relocations are program-owned transport, not a property of a single
 * encoder stack frame.  Keep the bound explicit so a malformed/generated
 * input cannot turn object emission into unbounded allocation.
 */
static bool ny_a64_reserve_relocs(ny_a64_reloc_t **data, size_t *cap,
                                  size_t want, char *err, size_t err_len) {
  if (!data || !cap || want > NY_NATIVE_MAX_RELOCS) {
    ny_native_set_err(err, err_len,
                      "AArch64 ELF object writer: relocation limit is 4096");
    return false;
  }
  if (want <= *cap)
    return true;
  size_t next = *cap ? *cap : 256;
  while (next < want) {
    if (next > NY_NATIVE_MAX_RELOCS / 2) {
      next = NY_NATIVE_MAX_RELOCS;
      break;
    }
    next *= 2;
  }
  ny_a64_reloc_t *grown = realloc(*data, next * sizeof(*grown));
  if (!grown) {
    ny_native_set_err(err, err_len,
                      "AArch64 ELF object writer: relocation allocation failed");
    return false;
  }
  *data = grown;
  *cap = next;
  return true;
}

static int ny_a64_align(int value, int align) {
  return (value + align - 1) & ~(align - 1);
}

static bool ny_a64_mov_imm(ny_a64_obj_ctx_t *c, unsigned reg, int64_t value);

static bool ny_a64_u32(ny_a64_obj_ctx_t *c, uint32_t word) {
  if (!ny_obj_u32(&c->code, word)) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: out of memory");
    return false;
  }
  return true;
}

static bool ny_a64_reg_mem_base(ny_a64_obj_ctx_t *c, bool load,
                                unsigned reg, unsigned base, int off) {
  if (reg > 30 || base > 31 || off < 0 || off > 32760 || (off & 7) != 0) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: invalid memory access reg=%u base=%u off=%d",
                      reg, base, off);
    return false;
  }
  uint32_t op = load ? 0xf9400000u : 0xf9000000u;
  return ny_a64_u32(c, op | ((uint32_t)(off / 8) << 10) |
                           ((base & 31u) << 5) | reg);
}

static bool ny_a64_reg_mem(ny_a64_obj_ctx_t *c, bool load, unsigned reg,
                           int off) {
  return ny_a64_reg_mem_base(c, load, reg, 31, off);
}

static int ny_a64_value_off(const ny_a64_obj_ctx_t *c, int value) {
  return c && c->value_offs && value >= 0 && value < c->value_slots
             ? c->value_offs[value]
             : -1;
}

static int ny_a64_local_off(const ny_a64_obj_ctx_t *c, int local) {
  return c && c->local_offs && local >= 0 && local < c->local_slots
             ? c->local_offs[local]
             : -1;
}

static bool ny_a64_value_is_v128(const ny_a64_obj_ctx_t *c, int value) {
  return c && value >= 0 && value < c->value_slots &&
         ((c->types.value_v128_i64 && c->types.value_v128_i64[value]) ||
          (c->types.value_v128_f64 && c->types.value_v128_f64[value]) ||
          (c->types.value_v128_f32 && c->types.value_v128_f32[value]));
}

static bool ny_a64_local_is_v128(const ny_a64_obj_ctx_t *c, int local) {
  return c && local >= 0 && local < c->local_slots &&
         ((c->types.local_v128_i64 && c->types.local_v128_i64[local]) ||
          (c->types.local_v128_f64 && c->types.local_v128_f64[local]) ||
          (c->types.local_v128_f32 && c->types.local_v128_f32[local]));
}

static bool ny_a64_q_mem_base(ny_a64_obj_ctx_t *c, bool load, unsigned reg,
                              unsigned base, int off) {
  if (reg > 31 || base > 31 || off < 0 || off > 65520 || (off & 15) != 0) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: invalid vector memory access reg=%u base=%u off=%d",
                      reg, base, off);
    return false;
  }
  uint32_t op = load ? 0x3dc00000u : 0x3d800000u;
  return ny_a64_u32(c, op | ((uint32_t)(off / 16) << 10) |
                           ((base & 31u) << 5) | reg);
}

static bool ny_a64_q_mem(ny_a64_obj_ctx_t *c, bool load, unsigned reg,
                         int off) {
  return ny_a64_q_mem_base(c, load, reg, 31, off);
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

static bool ny_a64_fp_mem_base(ny_a64_obj_ctx_t *c, bool load, bool f32,
                               unsigned reg, unsigned base, int off) {
  unsigned scale = f32 ? 4u : 8u;
  if (reg > 31 || base > 31 || off < 0 || (off % (int)scale) != 0 ||
      off / (int)scale > 4095) return false;
  uint32_t op = f32 ? (load ? 0xbd400000u : 0xbd000000u)
                    : (load ? 0xfd400000u : 0xfd000000u);
  return ny_a64_u32(c, op | ((uint32_t)(off / (int)scale) << 10) |
                           ((base & 31u) << 5) | reg);
}

static bool ny_a64_fp_mem(ny_a64_obj_ctx_t *c, bool load, bool f32,
                          unsigned reg, int off) {
  return ny_a64_fp_mem_base(c, load, f32, reg, 31, off);
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


static bool ny_a64_load_vec_value(ny_a64_obj_ctx_t *c, unsigned reg, int value) {
  return ny_a64_check_value(c, value, "vector source") &&
         ny_a64_value_is_v128(c, value) &&
         ny_a64_q_mem(c, true, reg, ny_a64_value_off(c, value));
}

static bool ny_a64_store_vec_value(ny_a64_obj_ctx_t *c, int value,
                                   unsigned reg) {
  return ny_a64_check_value(c, value, "vector destination") &&
         ny_a64_value_is_v128(c, value) &&
         ny_a64_q_mem(c, false, reg, ny_a64_value_off(c, value));
}

static bool ny_a64_vec_binop(ny_a64_obj_ctx_t *c, const nyir_inst_t *in,
                             uint32_t op) {
  return ny_a64_load_vec_value(c, 1, in->a) &&
         ny_a64_load_vec_value(c, 2, in->b) &&
         ny_a64_u32(c, op | (2u << 16) | (1u << 5)) &&
         ny_a64_store_vec_value(c, in->dst, 0);
}

static bool ny_a64_vec_fma(ny_a64_obj_ctx_t *c, const nyir_inst_t *in,
                           uint32_t op) {
  return ny_a64_load_vec_value(c, 0, in->c) &&
         ny_a64_load_vec_value(c, 1, in->a) &&
         ny_a64_load_vec_value(c, 2, in->b) &&
         ny_a64_u32(c, op | (2u << 16) | (1u << 5)) &&
         ny_a64_store_vec_value(c, in->dst, 0);
}

static uint64_t ny_a64_shuffle_index_half(unsigned imm, unsigned lane_bytes,
                                          unsigned first_lane,
                                          unsigned lane_count) {
  uint64_t bits = 0;
  for (unsigned byte = 0; byte < 8; ++byte) {
    unsigned out_lane = first_lane + byte / lane_bytes;
    unsigned src_lane = (imm >> (out_lane * (lane_count == 2 ? 1u : 2u))) &
                        (lane_count - 1u);
    unsigned src_byte = src_lane * lane_bytes + byte % lane_bytes;
    bits |= (uint64_t)src_byte << (byte * 8);
  }
  return bits;
}

static bool ny_a64_vec_shuffle(ny_a64_obj_ctx_t *c, const nyir_inst_t *in,
                               unsigned lane_bytes, unsigned lane_count) {
  unsigned imm = (unsigned)in->imm;
  uint64_t lo = ny_a64_shuffle_index_half(imm, lane_bytes, 0, lane_count);
  uint64_t hi = ny_a64_shuffle_index_half(imm, lane_bytes, 8 / lane_bytes,
                                          lane_count);
  return ny_a64_load_vec_value(c, 1, in->a) &&
         ny_a64_mov_imm(c, 0, (int64_t)lo) &&
         ny_a64_mov_imm(c, 1, (int64_t)hi) &&
         ny_a64_u32(c, 0x9e670002u) && /* fmov d2,x0 */
         ny_a64_u32(c, 0x4e181c22u) && /* ins v2.d[1],x1 */
         ny_a64_u32(c, 0x4e020020u) && /* tbl v0.16b,{v1.16b},v2.16b */
         ny_a64_store_vec_value(c, in->dst, 0);
}

static bool ny_a64_fp_binop(ny_a64_obj_ctx_t *c, const nyir_inst_t *in,
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

static bool ny_a64_binop(ny_a64_obj_ctx_t *c, const nyir_inst_t *in,
                         uint32_t op) {
  return ny_a64_load_value(c, 0, in->a) &&
         ny_a64_load_value(c, 1, in->b) &&
         ny_a64_u32(c, op | (1u << 16)) &&
         ny_a64_store_value(c, in->dst, 0);
}

static unsigned ny_a64_cond(nyir_cmp_t cmp) {
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
      !ny_a64_reserve_relocs(&c->relocs, &c->reloc_cap,
                             c->reloc_count + 1, c->err, c->err_len)) {
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
                             const nyir_inst_t *in) {
  switch (in->op) {
  case NYIR_NOP: return true;
  case NYIR_CONST_I64:
    return ny_a64_mov_imm(c, 0, in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_COPY:
    if (ny_a64_value_is_v128(c, in->dst))
      return ny_a64_load_vec_value(c, 0, in->a) &&
             ny_a64_store_vec_value(c, in->dst, 0);
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_ADD_I64: return ny_a64_binop(c, in, 0x8b000000u);
  case NYIR_SUB_I64: return ny_a64_binop(c, in, 0xcb000000u);
  case NYIR_MUL_I64: return ny_a64_binop(c, in, 0x9b007c00u);
  case NYIR_AND_I64: return ny_a64_binop(c, in, 0x8a000000u);
  case NYIR_OR_I64: return ny_a64_binop(c, in, 0xaa000000u);
  case NYIR_XOR_I64: return ny_a64_binop(c, in, 0xca000000u);
  case NYIR_SHL_I64: return ny_a64_binop(c, in, 0x9ac02000u);
  case NYIR_SAR_I64: return ny_a64_binop(c, in, 0x9ac02800u);
  case NYIR_DIV_I64: return ny_a64_binop(c, in, 0x9ac00c00u);
  case NYIR_MOD_I64:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_load_value(c, 1, in->b) &&
           ny_a64_u32(c, 0x9ac10c02u) && /* sdiv x2,x0,x1 */
           ny_a64_u32(c, 0x9b018040u) && /* msub x0,x2,x1,x0 */
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_CMP_I64: {
    unsigned inverse = ny_a64_cond(in->cmp) ^ 1u;
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_load_value(c, 1, in->b) &&
           ny_a64_u32(c, 0xeb01001fu) &&
           ny_a64_u32(c, 0x9a9f07e0u | (inverse << 12)) &&
           ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_LABEL: return ny_a64_add_label(c, in->imm);
  case NYIR_LOAD_LOCAL:
    if (ny_a64_value_is_v128(c, in->dst) ||
        ny_a64_local_is_v128(c, (int)in->imm))
      return ny_a64_q_mem(c, true, 0, ny_a64_local_off(c, (int)in->imm)) &&
             ny_a64_store_vec_value(c, in->dst, 0);
    return ny_a64_load_local(c, 0, (int)in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_STORE_LOCAL:
    if (ny_a64_value_is_v128(c, in->a) ||
        ny_a64_local_is_v128(c, (int)in->imm))
      return ny_a64_load_vec_value(c, 0, in->a) &&
             ny_a64_q_mem(c, false, 0, ny_a64_local_off(c, (int)in->imm));
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
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC8_LOAD_F32:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0x3dc00000u) &&
           ny_a64_store_vec_value(c, in->dst, 0);
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_STORE_F32:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_load_vec_value(c, 1, in->b >= 0 ? in->b : in->c) &&
           ny_a64_u32(c, 0x3d800001u);
  case NYIR_VEC4_SET1_I64:
    /*
     * dup v0.2d, x0
     */
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0x4e080c00u) &&
           ny_a64_store_vec_value(c, in->dst, 0);
  case NYIR_VEC4_SET1_F64:
    return ny_a64_load_fp_value(c, 0, in->a, false) &&
           ny_a64_u32(c, 0x4e080400u) &&
           ny_a64_store_vec_value(c, in->dst, 0);
  case NYIR_VEC8_SET1_F32:
    return ny_a64_load_fp_value(c, 0, in->a, true) &&
           ny_a64_u32(c, 0x4e040400u) &&
           ny_a64_store_vec_value(c, in->dst, 0);
  case NYIR_VEC4_ADD_I64:
    return ny_a64_vec_binop(c, in, 0x4ee08400u);
  case NYIR_VEC4_SUB_I64:
    return ny_a64_vec_binop(c, in, 0x6ee08400u);
  case NYIR_VEC4_AND_I64:
    return ny_a64_vec_binop(c, in, 0x4e201c00u);
  case NYIR_VEC4_OR_I64:
    return ny_a64_vec_binop(c, in, 0x4ea01c00u);
  case NYIR_VEC4_XOR_I64:
    return ny_a64_vec_binop(c, in, 0x6e201c00u);
  case NYIR_VEC4_ADD_F64:
    return ny_a64_vec_binop(c, in, 0x4e60d400u);
  case NYIR_VEC4_SUB_F64:
    return ny_a64_vec_binop(c, in, 0x4ee0d400u);
  case NYIR_VEC4_MUL_F64:
    return ny_a64_vec_binop(c, in, 0x6e60dc00u);
  case NYIR_VEC4_DIV_F64:
    return ny_a64_vec_binop(c, in, 0x6e60fc00u);
  case NYIR_VEC8_ADD_F32:
    return ny_a64_vec_binop(c, in, 0x4e20d400u);
  case NYIR_VEC8_SUB_F32:
    return ny_a64_vec_binop(c, in, 0x4ea0d400u);
  case NYIR_VEC8_MUL_F32:
    return ny_a64_vec_binop(c, in, 0x6e20dc00u);
  case NYIR_VEC8_DIV_F32:
    return ny_a64_vec_binop(c, in, 0x6e20fc00u);
  case NYIR_VEC4_FMA_F64:
    return ny_a64_vec_fma(c, in, 0x4e60cc00u);
  case NYIR_VEC8_FMA_F32:
    return ny_a64_vec_fma(c, in, 0x4e20cc00u);
  case NYIR_VEC4_SHUFFLE_F64:
    return ny_a64_vec_shuffle(c, in, 8, 2);
  case NYIR_VEC4_REDUCE_ADD_F64:
    return ny_a64_load_vec_value(c, 0, in->b) &&
           ny_a64_u32(c, 0x7e70d800u) && /* faddp d0, v0.2d */
           ny_a64_load_fp_value(c, 1, in->a, false) &&
           ny_a64_u32(c, 0x1e612800u) && /* fadd d0, d0, d1 */
           ny_a64_store_fp_value(c, in->dst, 0, false);
  case NYIR_VEC8_SHUFFLE_F32:
    return ny_a64_vec_shuffle(c, in, 4, 4);
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
    if (!ny_a64_load_vec_value(c, 1, in->a) ||
        !ny_a64_load_vec_value(c, 2, in->b))
      return false;
    if (in->op == NYIR_VEC4_SAR_I64 && !ny_a64_u32(c, 0x6ee0b842u))
      return false;
    return ny_a64_u32(c, 0x4ee24420u) &&
           ny_a64_store_vec_value(c, in->dst, 0);
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
  case NYIR_BR: return ny_a64_add_patch(c, in->imm, false);
  case NYIR_BR_IF:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0xf100001fu) &&
           ny_a64_add_patch(c, in->imm, true);
  case NYIR_CALL: {
    int args[NYIR_CALL_MAX_ARGS];
    int argc = 0;
    if (!nyir_call_args(in, c->value_slots, args, NYIR_CALL_MAX_ARGS,
                          &argc, c->err, c->err_len))
      return false;
    enum {
      A64_ARG_GP,
      A64_ARG_SRET,
      A64_ARG_F32,
      A64_ARG_F64,
      A64_ARG_V128,
      A64_ARG_AGG_GP,
      A64_ARG_AGG_FP,
      A64_ARG_HFA_F32,
      A64_ARG_HFA_F64,
      A64_ARG_HVA_V128,
      A64_ARG_INDIRECT,
      A64_ARG_STACK_GP,
      A64_ARG_STACK_F32,
      A64_ARG_STACK_F64,
      A64_ARG_STACK_V128,
      A64_ARG_STACK_AGG,
      A64_ARG_STACK_PTR,
    };
    unsigned kinds[NYIR_CALL_MAX_ARGS] = {0};
    unsigned regs[NYIR_CALL_MAX_ARGS] = {0};
    unsigned chunks[NYIR_CALL_MAX_ARGS] = {0};
    unsigned stack_offs[NYIR_CALL_MAX_ARGS] = {0};
    unsigned copy_offs[NYIR_CALL_MAX_ARGS] = {0};
    uint32_t sizes[NYIR_CALL_MAX_ARGS] = {0};
    unsigned gp = 0, fp = 0, stack_used = 0;
    for (int i = 0; i < argc; ++i) {
      if (i == 0 && (in->flags & NYIR_INST_F_SRET)) {
        kinds[i] = A64_ARG_SRET;
        regs[i] = 8;
        continue;
      }
      uint32_t packed = in->arg_sizes ? in->arg_sizes[i] : 0;
      if (packed) {
        uint32_t size = NYIR_ARG_AGG_SIZE(packed);
        unsigned c0 = NYIR_ARG_AGG_CLASS(packed, 0);
        unsigned c1 = NYIR_ARG_AGG_CLASS(packed, 1);
        sizes[i] = size;
        if (c0 == NYIR_ARG_CLASS_HFA_F32 ||
            c0 == NYIR_ARG_CLASS_HFA_F64 ||
            c0 == NYIR_ARG_CLASS_HVA_V128) {
          unsigned elem_size = c0 == NYIR_ARG_CLASS_HFA_F32 ? 4u :
                               c0 == NYIR_ARG_CLASS_HFA_F64 ? 8u : 16u;
          unsigned count = elem_size && size % elem_size == 0
                               ? size / elem_size
                               : 0;
          chunks[i] = count;
          if (count >= 1 && count <= 4 && fp + count <= 8) {
            kinds[i] = c0 == NYIR_ARG_CLASS_HFA_F32 ? A64_ARG_HFA_F32 :
                       c0 == NYIR_ARG_CLASS_HFA_F64 ? A64_ARG_HFA_F64 :
                                                       A64_ARG_HVA_V128;
            regs[i] = fp;
            fp += count;
          } else {
            unsigned align = c0 == NYIR_ARG_CLASS_HVA_V128 ? 16u : 8u;
            fp = 8;
            stack_used = (stack_used + align - 1u) & ~(align - 1u);
            kinds[i] = A64_ARG_STACK_AGG;
            stack_offs[i] = stack_used;
            stack_used += (size + 7u) & ~7u;
          }
          continue;
        }
        bool align16 = c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16;
        bool integer = size > 0 && size <= 16 &&
                       (c0 == NYIR_ARG_CLASS_INTEGER ||
                        c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
                        c0 == NYIR_ARG_CLASS_NONE) &&
                       (size <= 8 || c1 == NYIR_ARG_CLASS_INTEGER ||
                        c1 == NYIR_ARG_CLASS_NONE);
        bool floating = size > 0 && size <= 16 &&
                        c0 == NYIR_ARG_CLASS_SSE &&
                        (size <= 8 || c1 == NYIR_ARG_CLASS_SSE ||
                         c1 == NYIR_ARG_CLASS_NONE);
        unsigned need = size > 8 ? 2u : 1u;
        chunks[i] = need;
        if (integer) {
          if (align16 && (gp & 1u))
            ++gp;
          if (gp + need <= 8) {
            kinds[i] = A64_ARG_AGG_GP;
            regs[i] = gp;
            gp += need;
          } else {
            gp = 8;
            unsigned align = align16 ? 16u : 8u;
            stack_used = (stack_used + align - 1u) & ~(align - 1u);
            kinds[i] = A64_ARG_STACK_AGG;
            stack_offs[i] = stack_used;
            stack_used += (size + 7u) & ~7u;
          }
        } else if (floating && fp + need <= 8) {
          kinds[i] = A64_ARG_AGG_FP;
          regs[i] = fp;
          fp += need;
        } else if (size > 16 || c0 == NYIR_ARG_CLASS_MEMORY ||
                   c0 == NYIR_ARG_CLASS_UNSUPPORTED) {
          if (gp < 8) {
            kinds[i] = A64_ARG_INDIRECT;
            regs[i] = gp++;
          } else {
            stack_used = (stack_used + 7u) & ~7u;
            kinds[i] = A64_ARG_STACK_PTR;
            stack_offs[i] = stack_used;
            stack_used += 8;
          }
        } else {
          unsigned align = size > 8 ? 16u : 8u;
          stack_used = (stack_used + align - 1u) & ~(align - 1u);
          kinds[i] = A64_ARG_STACK_AGG;
          stack_offs[i] = stack_used;
          stack_used += (size + 7u) & ~7u;
        }
        continue;
      }
      bool v128 = ny_a64_value_is_v128(c, args[i]);
      bool f64 = c->types.value_f64 && c->types.value_f64[args[i]];
      bool f32 = c->types.value_f32 && c->types.value_f32[args[i]];
      if (v128) {
        if (fp < 8) {
          kinds[i] = A64_ARG_V128;
          regs[i] = fp++;
        } else {
          stack_used = (stack_used + 15u) & ~15u;
          kinds[i] = A64_ARG_STACK_V128;
          stack_offs[i] = stack_used;
          stack_used += 16;
        }
      } else if (f64 || f32) {
        if (fp < 8) {
          kinds[i] = f32 ? A64_ARG_F32 : A64_ARG_F64;
          regs[i] = fp++;
        } else {
          stack_used = (stack_used + 7u) & ~7u;
          kinds[i] = f32 ? A64_ARG_STACK_F32 : A64_ARG_STACK_F64;
          stack_offs[i] = stack_used;
          stack_used += 8;
        }
      } else if (gp < 8) {
        kinds[i] = A64_ARG_GP;
        regs[i] = gp++;
      } else {
        stack_used = (stack_used + 7u) & ~7u;
        kinds[i] = A64_ARG_STACK_GP;
        stack_offs[i] = stack_used;
        stack_used += 8;
      }
    }
    for (int i = 0; i < argc; ++i) {
      if (kinds[i] != A64_ARG_INDIRECT && kinds[i] != A64_ARG_STACK_PTR)
        continue;
      stack_used = (stack_used + 15u) & ~15u;
      copy_offs[i] = stack_used;
      stack_used += (sizes[i] + 7u) & ~7u;
    }
    unsigned stack_size = (stack_used + 15u) & ~15u;
    unsigned frame_base = 31;
    if (stack_size) {
      if (stack_size >= 4096) {
        ny_native_set_err(c->err, c->err_len,
                          "AArch64 object writer: outgoing call frame too large");
        return false;
      }
      if (!ny_a64_u32(c, 0x910003efu) ||
          !ny_a64_u32(c, 0xd10003ffu | (stack_size << 10)))
        return false;
      frame_base = 15;
    }
    for (int i = 0; i < argc; ++i) {
      if (kinds[i] != A64_ARG_INDIRECT && kinds[i] != A64_ARG_STACK_PTR)
        continue;
      int off = ny_a64_value_off(c, args[i]);
      if (!ny_a64_reg_mem_base(c, true, 9, frame_base, off))
        return false;
      for (uint32_t byte = 0; byte < sizes[i]; ++byte) {
        if (!ny_a64_u32(c, 0x3940012au | (byte << 10)) ||
            !ny_a64_u32(c, 0x390003eau |
                               ((copy_offs[i] + byte) << 10)))
          return false;
      }
    }
    for (int i = 0; i < argc; ++i) {
      int off = ny_a64_value_off(c, args[i]);
      switch (kinds[i]) {
      case A64_ARG_STACK_GP:
        if (!ny_a64_reg_mem_base(c, true, 10, frame_base, off) ||
            !ny_a64_reg_mem_base(c, false, 10, 31, (int)stack_offs[i]))
          return false;
        break;
      case A64_ARG_STACK_PTR:
        if (!ny_a64_u32(c, 0x910003eau | (copy_offs[i] << 10)) ||
            !ny_a64_reg_mem_base(c, false, 10, 31, (int)stack_offs[i]))
          return false;
        break;
      case A64_ARG_STACK_F32:
        if (!ny_a64_fp_mem_base(c, true, true, 16, frame_base, off) ||
            !ny_a64_fp_mem_base(c, false, true, 16, 31,
                                (int)stack_offs[i]))
          return false;
        break;
      case A64_ARG_STACK_F64:
        if (!ny_a64_fp_mem_base(c, true, false, 16, frame_base, off) ||
            !ny_a64_fp_mem_base(c, false, false, 16, 31,
                                (int)stack_offs[i]))
          return false;
        break;
      case A64_ARG_STACK_V128:
        if (!ny_a64_q_mem_base(c, true, 16, frame_base, off) ||
            !ny_a64_q_mem(c, false, 16, (int)stack_offs[i]))
          return false;
        break;
      case A64_ARG_STACK_AGG:
        if (!ny_a64_reg_mem_base(c, true, 9, frame_base, off))
          return false;
        for (uint32_t byte = 0; byte < sizes[i]; ++byte) {
          if (!ny_a64_u32(c, 0x3940012au | (byte << 10)) ||
              !ny_a64_u32(c, 0x390003eau |
                                 ((stack_offs[i] + byte) << 10)))
            return false;
        }
        break;
      default:
        break;
      }
    }
    for (int i = 0; i < argc; ++i) {
      int off = ny_a64_value_off(c, args[i]);
      switch (kinds[i]) {
      case A64_ARG_GP:
      case A64_ARG_SRET:
        if (!ny_a64_reg_mem_base(c, true, regs[i], frame_base, off))
          return false;
        break;
      case A64_ARG_INDIRECT:
        if (!ny_a64_u32(c, 0x910003e0u | (copy_offs[i] << 10) | regs[i]))
          return false;
        break;
      case A64_ARG_F32:
        if (!ny_a64_fp_mem_base(c, true, true, regs[i], frame_base, off))
          return false;
        break;
      case A64_ARG_F64:
        if (!ny_a64_fp_mem_base(c, true, false, regs[i], frame_base, off))
          return false;
        break;
      case A64_ARG_V128:
        if (!ny_a64_q_mem_base(c, true, regs[i], frame_base, off))
          return false;
        break;
      case A64_ARG_AGG_GP:
        if (!ny_a64_reg_mem_base(c, true, 9, frame_base, off))
          return false;
        for (unsigned ch = 0; ch < chunks[i]; ++ch)
          if (!ny_a64_reg_mem_base(c, true, regs[i] + ch, 9, (int)(ch * 8)))
            return false;
        break;
      case A64_ARG_AGG_FP:
        if (!ny_a64_reg_mem_base(c, true, 9, frame_base, off))
          return false;
        for (unsigned ch = 0; ch < chunks[i]; ++ch)
          if (!ny_a64_fp_mem_base(c, true, false, regs[i] + ch, 9,
                                  (int)(ch * 8)))
            return false;
        break;
      case A64_ARG_HFA_F32:
      case A64_ARG_HFA_F64:
      case A64_ARG_HVA_V128:
        if (!ny_a64_reg_mem_base(c, true, 9, frame_base, off))
          return false;
        for (unsigned ch = 0; ch < chunks[i]; ++ch) {
          bool ok = kinds[i] == A64_ARG_HVA_V128
                        ? ny_a64_q_mem_base(c, true, regs[i] + ch, 9,
                                            (int)(ch * 16))
                        : ny_a64_fp_mem_base(
                              c, true, kinds[i] == A64_ARG_HFA_F32,
                              regs[i] + ch, 9,
                              (int)(ch * (kinds[i] == A64_ARG_HFA_F32 ? 4 : 8)));
          if (!ok)
            return false;
        }
        break;
      default:
        break;
      }
    }
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%s%s%s",
             c->target->symbol_prefix ? c->target->symbol_prefix : "",
             (in->flags & NYIR_INST_F_EXTERN) ? "" : "ny_fn_",
             in->symbol ? in->symbol : "");
    if (!ny_a64_add_reloc(c, symbol)) return false;
    if (stack_size &&
        !ny_a64_u32(c, 0x910003ffu | (stack_size << 10)))
      return false;
    if (in->dst < 0)
      return true;
    if (ny_a64_value_is_v128(c, in->dst))
      return ny_a64_store_vec_value(c, in->dst, 0);
    if (in->flags & NYIR_INST_F_RET_F64)
      return ny_a64_store_fp_value(c, in->dst, 0, false);
    if (in->flags & NYIR_INST_F_RET_F32)
      return ny_a64_store_fp_value(c, in->dst, 0, true);
    return ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_RET:
    if (in->a >= 0) {
      bool f64 = c->types.value_f64[in->a];
      bool f32 = c->types.value_f32[in->a];
      if (ny_a64_value_is_v128(c, in->a)) {
        if (!ny_a64_load_vec_value(c, 0, in->a))
          return false;
      } else if ((f64 || f32)
                     ? !ny_a64_load_fp_value(c, 0, in->a, f32)
                     : !ny_a64_load_value(c, 0, in->a)) {
        return false;
      }
    }
    if (c->return_count >= sizeof(c->returns) / sizeof(c->returns[0]))
      return false;
    c->returns[c->return_count++] = c->code.len;
    return ny_a64_u32(c, 0x14000000u);
  case NYIR_ALLOCA: {
    if (in->dst < 0)
      return true;
    int off = c->alloca_offs && in->dst < c->value_slots
                  ? c->alloca_offs[in->dst]
                  : -1;
    if (off < 0 || off > 4095) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 object writer: invalid fixed alloca offset");
      return false;
    }
    if (!ny_a64_u32(c, 0x910003e0u | ((uint32_t)off << 10)))
      return false;
    return ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_COPY_STRUCT: {
    /*
     * Simple byte copy: a=dst ptr, b=src ptr, imm=size. Use x0/x1/x2 loop.
     */
    if (in->imm < 0) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 object writer: COPY_STRUCT has negative size");
      return false;
    }
    if (in->imm == 0)
      return true;
    if (!ny_a64_load_value(c, 0, in->a) || /* x0 = dst */
        !ny_a64_load_value(c, 1, in->b))   /* x1 = src */
      return false;
    if (!ny_a64_mov_imm(c, 2, in->imm)) /* x2 = size */
      return false;
    /*
     * Loop: cbz x2, done; ldrb w3,[x1],#1; strb w3,[x0],#1; sub x2,x2,#1; b loop
     * Encode as unrolled for size<=32, else simple byte loop via patches.
     */
    int64_t n = in->imm;
    if (n <= 32) {
      for (int64_t k = 0; k < n; ++k) {
        /*
         * ldrb w3, [x1, #k]
         */
        if (!ny_a64_u32(c, 0x39400023u | ((uint32_t)k << 10)))
          return false;
        /*
         * strb w3, [x0, #k]
         */
        if (!ny_a64_u32(c, 0x39000003u | ((uint32_t)k << 10)))
          return false;
      }
      return true;
    }
    /*
     * Word-wise when size multiple of 8 and ≤256.
     */
    if ((n & 7) == 0 && n <= 256) {
      for (int64_t k = 0; k < n; k += 8) {
        if (!ny_a64_u32(c, 0xF9400023u | ((uint32_t)(k / 8) << 10)) || /* ldr x3,[x1,#k] */
            !ny_a64_u32(c, 0xF9000003u | ((uint32_t)(k / 8) << 10)))   /* str x3,[x0,#k] */
          return false;
      }
      return true;
    }
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: COPY_STRUCT size %" PRId64
                      " not handled",
                      n);
    return false;
  }
  case NYIR_CAPTURE_RET: {
    /*
     * Capture second return register (x1) or FP after multi-reg return.
     */
    if (in->dst < 0)
      return true;
    unsigned reg = 0;
    switch (in->imm) {
    case 0: reg = 1; break; /* x1 */
    case 1: reg = 0; break; /* x0 */
    case 2: /* d0 as bits — fmov x0, d0 */
      return ny_a64_u32(c, 0x9E660000u) && ny_a64_store_value(c, in->dst, 0);
    case 3:
      return ny_a64_u32(c, 0x9E660020u) && /* fmov x0, d1 */
             ny_a64_store_value(c, in->dst, 0);
    case 4: case 5: case 6: case 7: {
      unsigned v = (unsigned)in->imm - 4u;
      return ny_a64_u32(c, 0x1E260000u | (v << 5)) && /* fmov w0, sN */
             ny_a64_store_value(c, in->dst, 0);
    }
    case 8: case 9: {
      unsigned v = (unsigned)in->imm - 6u; /* d2,d3 */
      return ny_a64_u32(c, 0x9E660000u | (v << 5)) &&
             ny_a64_store_value(c, in->dst, 0);
    }
    case 10: case 11: case 12: case 13:
      return ny_a64_store_vec_value(c, in->dst,
                                    (unsigned)in->imm - 10u); /* q0..q3 */
    default:
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 object writer: unsupported return capture selector");
      return false;
    }
    return ny_a64_store_value(c, in->dst, reg);
  }
  case NYIR_ADDR_SYMBOL:
  case NYIR_OP_COUNT:
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: unsupported op %s",
                      nyir_op_name(in->op));
    return false;
  default:
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: unsupported op %s",
                      nyir_op_name(in->op));
    return false;
  }
}

static int ny_a64_param_count(const ny_a64_obj_ctx_t *c) {
  if (!c || !c->nyir || c->local_slots <= 0)
    return 0;
  if (c->nyir->param_count > 0)
    return c->nyir->param_count <= (size_t)c->local_slots
               ? (int)c->nyir->param_count
               : -1;
  bool *stored = calloc((size_t)c->local_slots, sizeof(bool));
  bool *param = calloc((size_t)c->local_slots, sizeof(bool));
  if (!stored || !param) {
    free(stored);
    free(param);
    return -1;
  }
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyir_inst_t *in = &c->nyir->data[i];
    if (in->op != NYIR_LOAD_LOCAL && in->op != NYIR_STORE_LOCAL)
      continue;
    int local = (int)in->imm;
    if (local < 0 || local >= c->local_slots)
      continue;
    if (in->op == NYIR_STORE_LOCAL)
      stored[local] = true;
    else if (!stored[local])
      param[local] = true;
  }
  int count = 0;
  while (count < c->local_slots && param[count])
    count++;
  free(stored);
  free(param);
  return count;
}

static bool ny_a64_emit_param_spills(ny_a64_obj_ctx_t *c) {
  int count = ny_a64_param_count(c);
  if (count < 0) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: parameter classification allocation failed");
    return false;
  }
  unsigned gp = 0, fp = 0, stack_off = 0;
  for (int local = 0; local < count; ++local) {
    bool v128 = ny_a64_local_is_v128(c, local);
    bool f64 = c->types.local_f64 && c->types.local_f64[local];
    bool f32 = c->types.local_f32 && c->types.local_f32[local];
    int dst = ny_a64_local_off(c, local);
    if (v128) {
      if (fp < 8) {
        if (!ny_a64_q_mem(c, false, fp++, dst))
          return false;
      } else {
        stack_off = (stack_off + 15u) & ~15u;
        if (!ny_a64_q_mem_base(c, true, 16, 29, 16 + (int)stack_off) ||
            !ny_a64_q_mem(c, false, 16, dst))
          return false;
        stack_off += 16;
      }
    } else if (f64 || f32) {
      if (fp < 8) {
        if (!ny_a64_fp_mem(c, false, f32, fp++, dst))
          return false;
      } else {
        stack_off = (stack_off + 7u) & ~7u;
        if (!ny_a64_fp_mem_base(c, true, f32, 16, 29,
                                16 + (int)stack_off) ||
            !ny_a64_fp_mem(c, false, f32, 16, dst))
          return false;
        stack_off += 8;
      }
    } else {
      if (gp < 8) {
        if (!ny_a64_store_local(c, local, gp++))
          return false;
      } else {
        stack_off = (stack_off + 7u) & ~7u;
        if (!ny_a64_reg_mem_base(c, true, 16, 29, 16 + (int)stack_off) ||
            !ny_a64_store_local(c, local, 16))
          return false;
        stack_off += 8;
      }
    }
  }
  return true;
}

static bool ny_a64_emit_code(ny_a64_obj_ctx_t *c, bool user_function,
                             bool tag_return) {
  c->value_slots = c->nyir->next_value;
  c->local_slots = (int)ny_native_nir_local_count(c->nyir);
  if (!nyir_type_map_init(&c->types, c->nyir, (size_t)c->local_slots)) {
    ny_native_set_err(c->err, c->err_len,
                      "AArch64 object writer: type classification allocation failed");
    return false;
  }
  if (c->value_slots > 0) {
    c->value_offs = calloc((size_t)c->value_slots, sizeof(*c->value_offs));
    c->alloca_offs = malloc((size_t)c->value_slots * sizeof(*c->alloca_offs));
    if (!c->value_offs || !c->alloca_offs) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 object writer: value layout allocation failed");
      return false;
    }
    for (int i = 0; i < c->value_slots; ++i)
      c->alloca_offs[i] = -1;
  }
  if (c->local_slots > 0) {
    c->local_offs = calloc((size_t)c->local_slots, sizeof(*c->local_offs));
    if (!c->local_offs) {
      ny_native_set_err(c->err, c->err_len,
                        "AArch64 object writer: local layout allocation failed");
      return false;
    }
  }
  int cursor = 0;
  for (int value = 0; value < c->value_slots; ++value) {
    bool vec = ny_a64_value_is_v128(c, value);
    cursor = ny_a64_align(cursor, vec ? 16 : 8);
    c->value_offs[value] = cursor;
    cursor += vec ? 16 : 8;
  }
  c->local_base = cursor;
  for (int local = 0; local < c->local_slots; ++local) {
    bool vec = ny_a64_local_is_v128(c, local);
    cursor = ny_a64_align(cursor, vec ? 16 : 8);
    c->local_offs[local] = cursor;
    cursor += vec ? 16 : 8;
  }
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyir_inst_t *in = &c->nyir->data[i];
    if (in->op != NYIR_ALLOCA || in->dst < 0 || in->dst >= c->value_slots)
      continue;
    int size = in->imm > 0 ? (int)in->imm : 1;
    cursor = ny_a64_align(cursor, 16);
    c->alloca_offs[in->dst] = cursor;
    cursor += ny_a64_align(size, 16);
  }
  c->frame_bytes = ny_a64_align(cursor, 16);
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
  if (user_function && !ny_a64_emit_param_spills(c))
    return false;
  for (size_t i = 0; i < c->nyir->len; ++i)
    if (!ny_a64_emit_inst(c, &c->nyir->data[i])) return false;
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
                          size_t *def_count, ny_a64_reloc_t **relocs,
                          size_t *reloc_count, size_t *reloc_cap,
                          const nyir_func_t *nyir,
                          const ny_native_target_info_t *target,
                          const char *symbol, bool user_function,
                          bool tag_return, char *err, size_t err_len) {
  if (*def_count >= NY_NATIVE_MAX_DEFS ||
      ny_a64_def_index(defs, *def_count, symbol) >= 0)
    return false;
  if (!ny_obj_pad_to(code, 16)) return false;
  size_t start = code->len;
  ny_a64_obj_ctx_t c = {.nyir = nyir, .target = target, .err = err,
                        .err_len = err_len};
  if (!ny_a64_emit_code(&c, user_function, tag_return)) {
    free(c.value_offs);
    free(c.local_offs);
    free(c.alloca_offs);
    free(c.relocs);
    nyir_type_map_free(&c.types);
    ny_obj_free(&c.code);
    return false;
  }
  if (!ny_a64_reserve_relocs(relocs, reloc_cap,
                             *reloc_count + c.reloc_count, err, err_len) ||
      !ny_obj_emit(code, c.code.data, c.code.len)) {
    free(c.value_offs);
    free(c.local_offs);
    free(c.alloca_offs);
    free(c.relocs);
    nyir_type_map_free(&c.types);
    ny_obj_free(&c.code);
    return false;
  }
  ny_a64_def_t *def = &defs[(*def_count)++];
  snprintf(def->name, sizeof(def->name), "%s", symbol);
  def->off = start;
  def->size = c.code.len;
  for (size_t i = 0; i < c.reloc_count; ++i) {
    (*relocs)[*reloc_count] = c.relocs[i];
    (*relocs)[*reloc_count].off += start;
    (*reloc_count)++;
  }
  free(c.value_offs);
  free(c.local_offs);
  free(c.alloca_offs);
  free(c.relocs);
  ny_obj_free(&c.code);
  nyir_type_map_free(&c.types);
  return true;
}


typedef struct {
  ny_obj_buf_t code;
  ny_a64_reloc_t *relocs;
  size_t reloc_count;
  char symbol[256];
  char error[256];
} ny_a64_parallel_result_t;

typedef struct {
  const nyir_func_t *funcs;
  const char *const *names;
  const ny_native_target_info_t *target;
  ny_a64_parallel_result_t *results;
} ny_a64_parallel_ctx_t;

static bool ny_a64_parallel_task(size_t i, void *opaque) {
  ny_a64_parallel_ctx_t *ctx = (ny_a64_parallel_ctx_t *)opaque;
  ny_a64_parallel_result_t *r = &ctx->results[i];
  snprintf(r->symbol, sizeof(r->symbol), NY_FMT_FN,
           ctx->target->symbol_prefix ? ctx->target->symbol_prefix : "",
           ctx->names && ctx->names[i] ? ctx->names[i] : "unknown_fn");
  ny_a64_def_t def[1];
  size_t dc = 0, rc = 0, reloc_cap = 0;
  if (!ny_a64_append(&r->code, def, &dc, &r->relocs, &rc, &reloc_cap,
                     &ctx->funcs[i], ctx->target, r->symbol, true, false,
                     r->error, sizeof(r->error)))
    return false;
  r->reloc_count = rc;
  return true;
}

static bool ny_a64_append_functions_parallel(
    ny_obj_buf_t *code, ny_a64_def_t *defs, size_t *def_count,
    ny_a64_reloc_t **relocs, size_t *reloc_count, size_t *reloc_cap,
    const nyir_func_t *funcs, const char *const *names, size_t func_count,
    const ny_native_target_info_t *target, char *err, size_t err_len) {
  if (!func_count)
    return true;
  ny_a64_parallel_result_t *results = calloc(func_count, sizeof(*results));
  if (!results)
    return false;
  size_t work = 0;
  for (size_t i = 0; i < func_count; ++i)
    work += funcs[i].len;
  ny_a64_parallel_ctx_t ctx = {funcs, names, target, results};
  if (!ny_parallel_for(func_count, work, ny_a64_parallel_task, &ctx)) {
    if (err && err_len)
      for (size_t i = 0; i < func_count; ++i)
        if (results[i].error[0]) {
          snprintf(err, err_len, "%s", results[i].error);
          break;
        }
    for (size_t i = 0; i < func_count; ++i) {
      ny_obj_free(&results[i].code);
      free(results[i].relocs);
    }
    free(results);
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < func_count && ok; ++i) {
    ny_a64_parallel_result_t *r = &results[i];
    if (*def_count >= NY_NATIVE_MAX_DEFS ||
        !ny_a64_reserve_relocs(relocs, reloc_cap,
                               *reloc_count + r->reloc_count, err, err_len) ||
        !ny_obj_pad_to(code, 16)) {
      ok = false;
      break;
    }
    size_t start = code->len;
    if (!ny_obj_emit(code, r->code.data, r->code.len)) {
      ok = false;
      break;
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             r->symbol);
    defs[*def_count].off = start;
    defs[*def_count].size = r->code.len;
    (*def_count)++;
    for (size_t j = 0; j < r->reloc_count; ++j) {
      (*relocs)[*reloc_count] = r->relocs[j];
      (*relocs)[*reloc_count].off += start;
      (*reloc_count)++;
    }
  }
  for (size_t i = 0; i < func_count; ++i) {
    ny_obj_free(&results[i].code);
    free(results[i].relocs);
  }
  free(results);
  return ok;
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
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *out_defs,
    size_t *out_def_count, ny_x64_obj_reloc_t *out_relocs,
    size_t *out_reloc_count, char *err, size_t err_len) {
  if (!rt_main || !target || !entry_symbol || !*entry_symbol || !code ||
      !out_defs || !out_def_count || !out_relocs || !out_reloc_count)
    return false;
  ny_a64_def_t defs[NY_NATIVE_MAX_DEFS];
  ny_a64_reloc_t *relocs = NULL;
  size_t def_count = 0, reloc_count = 0;
  size_t reloc_cap = 0;
  if (!ny_a64_append_functions_parallel(code, defs, &def_count, &relocs,
                                         &reloc_count, &reloc_cap, funcs, func_names,
                                         func_count, target, err, err_len))
    goto fail;
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix, entry_symbol);
  if (!ny_a64_append(code, defs, &def_count, &relocs, &reloc_count, &reloc_cap, rt_main,
                     target, entry, false, tag_return, err, err_len))
    goto fail;
  if (reloc_count > NY_X64_OBJ_MAX_RELOCS) {
    ny_native_set_err(err, err_len,
                      "AArch64 object bundle: relocation transport exceeds %d",
                      NY_X64_OBJ_MAX_RELOCS);
    goto fail;
  }
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
  free(relocs);
  return true;
fail:
  free(relocs);
  return false;
}

typedef struct {
  const nyir_func_t *funcs;
  ny_mach_func_t *out;
  char (*errors)[256];
} ny_a64_mach_lower_ctx_t;

static bool ny_a64_mach_lower_task(size_t i, void *opaque) {
  ny_a64_mach_lower_ctx_t *ctx = (ny_a64_mach_lower_ctx_t *)opaque;
  return ny_mach_lower_nir(&ctx->funcs[i], &ctx->out[i], (1u << 8), ctx->errors[i], 256);
}

bool ny_native_emit_elf64_aarch64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  if (!rt_main || !target || !path || !entry_symbol || !*entry_symbol) return false;
  ny_obj_buf_t code = {0}, file = {0}, strtab = {0};
  ny_a64_def_t defs[NY_NATIVE_MAX_DEFS];
  ny_a64_reloc_t *relocs = NULL;
  size_t def_count = 0, reloc_count = 0;
  size_t reloc_cap = 0;
  bool ok = false;
  bool built = false;
  ny_mach_func_t top_mach = {0};
  ny_mach_func_t *func_mach = NULL;
  char mach_err[256] = {0};
  bool mach_ok = ny_mach_lower_nir(rt_main, &top_mach, (1u << 8), mach_err,
                                   sizeof(mach_err));
  if (mach_ok && func_count) {
    func_mach = calloc(func_count, sizeof(*func_mach));
    char (*lower_errors)[256] = calloc(func_count, sizeof(*lower_errors));
    if (!func_mach || !lower_errors) {
      free(lower_errors);
      mach_ok = false;
    } else {
      size_t work = 0;
      for (size_t i = 0; i < func_count; ++i)
        work += funcs[i].len;
      ny_a64_mach_lower_ctx_t ctx = {funcs, func_mach, lower_errors};
      mach_ok = ny_parallel_for(func_count, work, ny_a64_mach_lower_task, &ctx);
      if (!mach_ok)
        for (size_t i = 0; i < func_count; ++i)
          if (lower_errors[i][0]) {
            snprintf(mach_err, sizeof(mach_err), "%s", lower_errors[i]);
            break;
          }
      free(lower_errors);
    }
  }
  if (mach_ok) {
    ny_x64_obj_symbol_def_t mach_defs[NY_NATIVE_MAX_DEFS];
    ny_x64_obj_reloc_t mach_relocs[NY_X64_OBJ_MAX_RELOCS];
    size_t mach_def_count = 0, mach_reloc_count = 0;
    if (ny_a64_mach_build_bundle(
            &top_mach, func_mach, func_names, func_count, target, entry_symbol,
            tag_return, &code, mach_defs, &mach_def_count, mach_relocs,
            &mach_reloc_count, mach_err, sizeof(mach_err))) {
      if (mach_def_count <= NY_NATIVE_MAX_DEFS &&
          mach_reloc_count <= NY_X64_OBJ_MAX_RELOCS) {
        relocs = malloc((mach_reloc_count ? mach_reloc_count : 1) *
                        sizeof(*relocs));
        if (!relocs)
          goto done;
        reloc_cap = mach_reloc_count ? mach_reloc_count : 1;
        for (size_t i = 0; i < mach_def_count; ++i) {
          snprintf(defs[i].name, sizeof(defs[i].name), "%s", mach_defs[i].name);
          defs[i].off = mach_defs[i].off;
          defs[i].size = mach_defs[i].size;
        }
        for (size_t i = 0; i < mach_reloc_count; ++i) {
          snprintf(relocs[i].symbol, sizeof(relocs[i].symbol), "%s",
                   mach_relocs[i].symbol);
          relocs[i].off = mach_relocs[i].disp_off;
        }
        def_count = mach_def_count;
        reloc_count = mach_reloc_count;
        built = true;
      }
    }
  }
  ny_mach_func_free(&top_mach);
  if (func_mach) {
    for (size_t i = 0; i < func_count; ++i)
      ny_mach_func_free(&func_mach[i]);
    free(func_mach);
  }
  if (!built) {
    ny_obj_free(&code);
    code = (ny_obj_buf_t){0};
    def_count = reloc_count = 0;
    if (err && err_len)
      err[0] = '\0';
    if (!ny_a64_append_functions_parallel(&code, defs, &def_count, &relocs,
                                           &reloc_count, &reloc_cap, funcs, func_names,
                                           func_count, target, err, err_len))
      goto done;
    char entry[256];
    snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix, entry_symbol);
    if (!ny_a64_append(&code, defs, &def_count, &relocs, &reloc_count, &reloc_cap, rt_main,
                       target, entry, false, tag_return, err, err_len))
      goto done;
  }

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
  free(relocs);
  ny_obj_free(&strtab); ny_obj_free(&file); ny_obj_free(&code);
  if (!ok && err && err_len && !err[0])
    ny_native_set_err(err, err_len, "AArch64 ELF object writer failed");
  return ok;
}

/*
 * AArch64 machine form byte encoder. Encodes const-return and simple single-block
 * i64 ALU into raw bytes (no host assembler). General shapes fall back.
 */

static bool a64_u32(ny_obj_buf_t *code, uint32_t w) {
  return ny_obj_emit(code, &w, sizeof(w));
}

/*
 * movz/movk sequence for 64-bit imm into xd (0..30).
 */
static bool a64_mov_imm64(ny_obj_buf_t *code, unsigned xd, uint64_t imm) {
  /*
   * movz xd, imm16, lsl #0
   */
  uint32_t movz = 0xD2800000u | ((uint32_t)(imm & 0xffff) << 5) | (xd & 31);
  if (!a64_u32(code, movz))
    return false;
  for (int shift = 1; shift < 4; ++shift) {
    uint16_t part = (uint16_t)((imm >> (16 * shift)) & 0xffff);
    if (!part)
      continue;
    /*
     * movk xd, part, lsl #(16*shift)
     */
    uint32_t movk = 0xF2800000u | ((uint32_t)shift << 21) |
                    ((uint32_t)part << 5) | (xd & 31);
    if (!a64_u32(code, movk))
      return false;
  }
  return true;
}

static bool a64_ret(ny_obj_buf_t *code) {
  return a64_u32(code, 0xD65F03C0u); /* ret */
}

/*
 * AArch64 keeps the general stack-slot encoder as the compatibility path, but
 * scalar machine form can use caller-saved registers safely for colored
 * ranges. The encoder handles simple CFG branches and relocates them after
 * block layout; calls, floating point, vectors, and true uncolored spills
 * remain explicit fallback cases. x16 and x17 stay available as encoder
 * scratch registers, so the allocator uses x0 through x15 only.
 */
#define A64_MACH_COLOR_N 10
static const unsigned a64_mach_color_reg[A64_MACH_COLOR_N] =
    {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};

static int a64_slot_off(const ny_mach_func_t *mach,
                        const ny_mach_operand_t *op);
static int a64_mach_frame_size(const ny_mach_func_t *mach);
static bool a64_mach_emit_param_spills(const ny_mach_func_t *mach,
                                       ny_obj_buf_t *code);
static bool a64_ldur_x(ny_obj_buf_t *code, unsigned reg, int off);
static bool a64_stur_x(ny_obj_buf_t *code, unsigned reg, int off);
static bool a64_ldur_fp(ny_obj_buf_t *code, bool f32, unsigned reg, int off);
static bool a64_stur_fp(ny_obj_buf_t *code, bool f32, unsigned reg, int off);
static ny_mach_type_t a64_operand_type(const ny_mach_func_t *mach,
                                       const ny_mach_operand_t *op);

static bool a64_mach_scalar_regalloc_prepare(const ny_mach_func_t *mach,
                                             ny_mach_regalloc_t *alloc) {
  /*
   * PHI lowering materializes one edge block per predecessor.  Those blocks
   * are ordinary machine CFG blocks and the allocator already handles an
   * arbitrary block count; rejecting them here silently routes loop PHIs
   * through the legacy stack-only encoder, which does not preserve parallel
   * copies.
   */
  if (!mach || !alloc || !mach->block_len)
    return false;
  if (!ny_mach_regalloc_build(mach, A64_MACH_COLOR_N, alloc))
    return false;
  bool ok = true;
  for (size_t i = 0; ok && i < alloc->segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &alloc->segments[i];
    /*
     * Split ranges are safe when every segment remains colored: the encoder
     * materializes the allocator's canonical stack home at spill/reload
     * boundaries. A segment with no color still requires the general encoder
     * because it needs a true register spill.
     */
    if (seg->vreg >= mach->vreg_len || seg->color < 0 ||
        seg->color >= A64_MACH_COLOR_N)
      ok = false;
  }
  if (!ok) {
    ny_mach_regalloc_free(alloc);
    return false;
  }
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    switch (in->opcode) {
    case NY_MACH_COPY:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          (in->src0.kind != NY_MACH_OPERAND_VREG &&
           in->src0.kind != NY_MACH_OPERAND_IMM))
        ok = false;
      break;
    case NY_MACH_LOAD:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_FRAME)
        ok = false;
      break;
    case NY_MACH_STORE:
      if (in->dst.kind != NY_MACH_OPERAND_FRAME ||
          in->src0.kind != NY_MACH_OPERAND_VREG)
        ok = false;
      break;
    case NY_MACH_ADD:
    case NY_MACH_SUB:
    case NY_MACH_MUL:
    case NY_MACH_AND:
    case NY_MACH_OR:
    case NY_MACH_XOR:
    case NY_MACH_SHL:
    case NY_MACH_SAR:
    case NY_MACH_DIV:
    case NY_MACH_MOD:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG)
        ok = false;
      break;
    case NY_MACH_CMP:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG)
        ok = false;
      break;
    case NY_MACH_BR:
      if (in->src1.kind != NY_MACH_OPERAND_BLOCK ||
          in->src1.as.block_index >= mach->block_len)
        ok = false;
      break;
    case NY_MACH_BR_IF:
      if (in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_BLOCK ||
          in->src1.as.block_index >= mach->block_len)
        ok = false;
      break;
case NY_MACH_RET:
    case NY_MACH_NOP:
    case NY_MACH_TRAP:
      if (in->opcode == NY_MACH_RET &&
          in->src0.kind != NY_MACH_OPERAND_NONE &&
          in->src0.kind != NY_MACH_OPERAND_VREG &&
          in->src0.kind != NY_MACH_OPERAND_IMM)
        ok = false;
      break;
    default:
      ok = false;
      break;
    }
    if (!ok)
      break;
    if (in->dst.kind == NY_MACH_OPERAND_VREG &&
        (in->dst.as.reg >= mach->vreg_len ||
         !mach->vreg_types ||
         (mach->vreg_types[in->dst.as.reg] != NY_MACH_TYPE_I64 &&
          mach->vreg_types[in->dst.as.reg] != NY_MACH_TYPE_PTR)))
      ok = false;
    const ny_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
    for (size_t oi = 0; oi < 3; ++oi) {
      const ny_mach_operand_t *op = ops[oi];
      if (op->kind == NY_MACH_OPERAND_VREG &&
          (op->as.reg >= mach->vreg_len || !mach->vreg_types ||
           (mach->vreg_types[op->as.reg] != NY_MACH_TYPE_I64 &&
            mach->vreg_types[op->as.reg] != NY_MACH_TYPE_PTR)))
        ok = false;
    }
  }
  if (!ok) {
    ny_mach_regalloc_free(alloc);
    return false;
  }
  return true;
}

static int a64_mach_scalar_reg(const ny_mach_regalloc_t *alloc,
                               uint32_t vreg, size_t inst) {
  if (!alloc || vreg >= alloc->vreg_len)
    return -1;
  const ny_mach_live_segment_t *seg =
      ny_mach_regalloc_segment_at(alloc, vreg, inst);
  if (!seg || seg->color < 0 || seg->color >= A64_MACH_COLOR_N)
    return -1;
  return (int)a64_mach_color_reg[seg->color];
}

static bool a64_mach_mov_reg(ny_obj_buf_t *code, unsigned dst, unsigned src) {
  if (dst > 30 || src > 30)
    return false;
  return a64_u32(code, 0xAA0003E0u | ((src & 31u) << 16) | (dst & 31u));
}

static const ny_mach_live_segment_t *
a64_mach_segment(const ny_mach_regalloc_t *alloc, uint32_t vreg, size_t inst) {
  return ny_mach_regalloc_segment_at(alloc, vreg, inst);
}

static bool a64_mach_load_reloads(const ny_mach_func_t *mach,
                                  const ny_mach_regalloc_t *alloc,
                                  const ny_mach_inst_t *in, size_t inst,
                                  ny_obj_buf_t *code) {
  const ny_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
  uint32_t loaded[3];
  size_t loaded_len = 0;
  for (size_t i = 0; i < 3; ++i) {
    const ny_mach_operand_t *op = ops[i];
    if (op->kind != NY_MACH_OPERAND_VREG)
      continue;
    const ny_mach_live_segment_t *seg = a64_mach_segment(alloc, op->as.reg, inst);
    if (!seg || !seg->reload || seg->start != inst)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < loaded_len; ++j)
      duplicate |= loaded[j] == op->as.reg;
    if (duplicate)
      continue;
    int reg = a64_mach_scalar_reg(alloc, op->as.reg, inst);
    ny_mach_operand_t slot = {.kind = NY_MACH_OPERAND_VREG,
                              .as.reg = op->as.reg};
    if (reg < 0 || !a64_ldur_x(code, (unsigned)reg, a64_slot_off(mach, &slot)))
      return false;
    loaded[loaded_len++] = op->as.reg;
  }
  return true;
}

static bool a64_mach_store_spills(const ny_mach_func_t *mach,
                                  const ny_mach_regalloc_t *alloc,
                                  const ny_mach_inst_t *in, size_t block,
                                  size_t inst,
                                  ny_obj_buf_t *code) {
  const ny_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1, &in->src2};
  uint32_t stored[4];
  size_t stored_len = 0;
  for (size_t i = 0; i < 4; ++i) {
    const ny_mach_operand_t *op = ops[i];
    if (op->kind != NY_MACH_OPERAND_VREG)
      continue;
    const ny_mach_live_segment_t *seg = a64_mach_segment(alloc, op->as.reg, inst);
    if (!seg || !seg->spill || seg->end != inst)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < stored_len; ++j)
      duplicate |= stored[j] == op->as.reg;
    if (duplicate)
      continue;
    int reg = a64_mach_scalar_reg(alloc, op->as.reg, inst);
    ny_mach_operand_t slot = {.kind = NY_MACH_OPERAND_VREG,
                              .as.reg = op->as.reg};
    if (reg < 0 || !a64_stur_x(code, (unsigned)reg, a64_slot_off(mach, &slot)))
      return false;
    stored[stored_len++] = op->as.reg;
  }
  /*
   * A value may remain live on an outgoing edge after its defining
   * instruction, without being an operand of the branch itself.
   */
  for (uint32_t v = 0; v < mach->vreg_len; ++v) {
    if (!ny_mach_regalloc_live_out(alloc, block, v))
      continue;
    const ny_mach_live_segment_t *seg = a64_mach_segment(alloc, v, inst);
    if (!seg || !seg->spill || seg->end != inst)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < stored_len; ++j)
      duplicate |= stored[j] == v;
    if (duplicate)
      continue;
    int reg = a64_mach_scalar_reg(alloc, v, inst);
    ny_mach_operand_t slot = {.kind = NY_MACH_OPERAND_VREG, .as.reg = v};
    if (reg < 0 || stored_len >= sizeof(stored) / sizeof(stored[0]) ||
        !a64_stur_x(code, (unsigned)reg, a64_slot_off(mach, &slot)))
      return false;
    stored[stored_len++] = v;
  }
  return true;
}

static bool a64_mach_scalar_regalloc_encode(const ny_mach_func_t *mach,
                                            const ny_mach_regalloc_t *alloc,
                                            ny_obj_buf_t *code, bool user,
                                            char *err, size_t err_len) {
  int frame = a64_mach_frame_size(mach);
  if (!a64_u32(code, 0xA9BF7BFDu) || !a64_u32(code, 0x910003FDu))
    return false;
  if (frame) {
    if (frame >= 4096 ||
        !a64_u32(code, 0xD10003FFu | ((uint32_t)frame << 10)))
      return false;
  }
  if (user && !a64_mach_emit_param_spills(mach, code))
    return false;
  size_t *block_off = calloc(mach->block_len, sizeof(*block_off));
  size_t *patch_at = mach->inst_len ? calloc(mach->inst_len, sizeof(*patch_at)) : NULL;
  uint32_t *patch_blk = mach->inst_len ? calloc(mach->inst_len, sizeof(*patch_blk)) : NULL;
  size_t patch_len = 0;
  if (!block_off || (mach->inst_len && (!patch_at || !patch_blk)))
    goto fail;

  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    const ny_mach_block_t *block = &mach->blocks[bi];
    block_off[bi] = code->len;
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t i = block->first_inst + n;
      const ny_mach_inst_t *in = &mach->insts[i];
      int dst = in->dst.kind == NY_MACH_OPERAND_VREG
                    ? a64_mach_scalar_reg(alloc, in->dst.as.reg, i)
                    : -1;
      int a = in->src0.kind == NY_MACH_OPERAND_VREG
                  ? a64_mach_scalar_reg(alloc, in->src0.as.reg, i)
                  : -1;
      int b = in->src1.kind == NY_MACH_OPERAND_VREG
                  ? a64_mach_scalar_reg(alloc, in->src1.as.reg, i)
                  : -1;
      if (!a64_mach_load_reloads(mach, alloc, in, i, code))
        goto fail;
      switch (in->opcode) {
    case NY_MACH_COPY:
      if (dst < 0)
        goto fail;
      if (in->src0.kind == NY_MACH_OPERAND_IMM) {
        if (!a64_mov_imm64(code, (unsigned)dst, (uint64_t)in->src0.as.imm))
          goto fail;
      } else if (a < 0 || !a64_mach_mov_reg(code, (unsigned)dst, (unsigned)a))
        goto fail;
      break;
    case NY_MACH_LOAD:
      if (dst < 0 || in->src0.kind != NY_MACH_OPERAND_FRAME ||
          !a64_ldur_x(code, (unsigned)dst, a64_slot_off(mach, &in->src0)))
        goto fail;
      break;
    case NY_MACH_STORE:
      if (a < 0 || in->dst.kind != NY_MACH_OPERAND_FRAME ||
          !a64_stur_x(code, (unsigned)a, a64_slot_off(mach, &in->dst)))
        goto fail;
      break;
    case NY_MACH_ADD:
    case NY_MACH_SUB:
    case NY_MACH_MUL:
    case NY_MACH_AND:
    case NY_MACH_OR:
    case NY_MACH_XOR:
    case NY_MACH_SHL:
    case NY_MACH_SAR:
    case NY_MACH_DIV:
    case NY_MACH_MOD: {
      if (dst < 0 || a < 0 || b < 0)
        goto fail;
      uint32_t op = 0;
      switch (in->opcode) {
      case NY_MACH_ADD: op = 0x8B000000u; break;
      case NY_MACH_SUB: op = 0xCB000000u; break;
      case NY_MACH_MUL: op = 0x9B007C00u; break;
      case NY_MACH_AND: op = 0x8A000000u; break;
      case NY_MACH_OR: op = 0xAA000000u; break;
      case NY_MACH_XOR: op = 0xCA000000u; break;
      case NY_MACH_SHL: op = 0x9AC02000u; break;
      case NY_MACH_SAR: op = 0x9AC02800u; break;
      case NY_MACH_DIV: op = 0x9AC00C00u; break;
      case NY_MACH_MOD:
        if (!a64_u32(code, 0x9AC00C00u | ((uint32_t)b << 16) |
                              ((uint32_t)a << 5) | 17u) ||
            !a64_u32(code, 0x9B008000u | ((uint32_t)b << 16) |
                              (17u << 5) | ((uint32_t)a << 10) |
                              (uint32_t)dst))
          goto fail;
        break;
      default: break;
      }
      if (in->opcode == NY_MACH_MOD)
        break;
      if (!a64_u32(code, op | ((uint32_t)b << 16) |
                            ((uint32_t)a << 5) | (uint32_t)dst))
        goto fail;
      break;
    }
    case NY_MACH_CMP: {
      if (dst < 0 || a < 0 || b < 0 ||
          !a64_u32(code, 0xEB00001Fu | ((uint32_t)b << 16) |
                            ((uint32_t)a << 5)))
        goto fail;
      unsigned cond = 0xb; /* lt */
      switch (in->condition) {
      case NY_MACH_COND_EQ: cond = 0x0; break;
      case NY_MACH_COND_NE: cond = 0x1; break;
      case NY_MACH_COND_LT: cond = 0xb; break;
      case NY_MACH_COND_LE: cond = 0xd; break;
      case NY_MACH_COND_GT: cond = 0xc; break;
      case NY_MACH_COND_GE: cond = 0xa; break;
      default: goto fail;
      }
      if (!a64_u32(code, 0x9A9F07E0u | ((cond ^ 1u) << 12) |
                            (uint32_t)dst))
        goto fail;
      break;
    }
    case NY_MACH_BR:
      if (!a64_mach_store_spills(mach, alloc, in, bi, i, code) ||
          patch_len >= mach->inst_len || !a64_u32(code, 0x14000000u))
        goto fail;
      patch_at[patch_len] = code->len - 4;
      patch_blk[patch_len++] = in->src1.as.block_index;
      break;
    case NY_MACH_BR_IF:
      if (a < 0 || !a64_mach_store_spills(mach, alloc, in, bi, i, code) ||
          patch_len >= mach->inst_len ||
          !a64_u32(code, 0xB5000000u | (uint32_t)a))
        goto fail;
      patch_at[patch_len] = code->len - 4;
      patch_blk[patch_len++] = in->src1.as.block_index;
      break;
    case NY_MACH_RET:
      if (in->src0.kind == NY_MACH_OPERAND_IMM) {
        if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
          goto fail;
      } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
        if (a < 0 || !a64_mach_mov_reg(code, 0, (unsigned)a))
          goto fail;
      }
      if (frame && !a64_u32(code, 0x910003FFu | ((uint32_t)frame << 10)))
        goto fail;
      if (!a64_u32(code, 0xA8C17BFDu) || !a64_ret(code))
        goto fail;
      break;
    case NY_MACH_NOP:
      break;
    case NY_MACH_TRAP:
      /*
       * brk #0 — raises a debug / breakpoint exception.
       */
      if (!a64_u32(code, 0xD4200000u))
        goto fail;
      break;
    default:
      goto fail;
      }
      if (in->opcode != NY_MACH_RET && in->opcode != NY_MACH_BR &&
          in->opcode != NY_MACH_BR_IF &&
          !a64_mach_store_spills(mach, alloc, in, bi, i, code))
        goto fail;
    }
  }
  for (size_t i = 0; i < patch_len; ++i) {
    uint32_t target = patch_blk[i];
    if (target >= mach->block_len || patch_at[i] > UINT32_MAX ||
        block_off[target] > UINT32_MAX)
      goto fail;
    int64_t delta = (int64_t)block_off[target] - (int64_t)patch_at[i];
    if ((delta & 3) != 0)
      goto fail;
    int64_t imm = delta / 4;
    uint32_t word = 0;
    uint32_t current = 0;
    memcpy(&current, code->data + patch_at[i], sizeof(current));
    if ((current & 0x7f000000u) == 0x14000000u) {
      if (imm < -(1ll << 25) || imm >= (1ll << 25))
        goto fail;
      word = 0x14000000u | ((uint32_t)imm & 0x03ffffffu);
    } else {
      if (imm < -(1ll << 18) || imm >= (1ll << 18))
        goto fail;
      word = (current & 0xffe0001fu) |
             (((uint32_t)imm & 0x7ffffu) << 5);
    }
    ny_obj_patch_u32(code, patch_at[i], word);
  }
  free(block_off);
  free(patch_at);
  free(patch_blk);
  return true;
fail:
  free(block_off);
  free(patch_at);
  free(patch_blk);
  if (err && err_len)
    snprintf(err, err_len, "a64 scalar register allocation encode failed");
  return false;
}

/*
 * Scalar FP register allocation is kept separate from the integer allocator:
 * AAPCS64 uses the same v-register number for S/D values, but the files have
 * independent liveness and homes.  This gate is deliberately straight-line.
 * A comparison may produce one integer result, which is materialized in its
 * canonical home before the return; mixed CFG/call forms continue through the
 * proven stack encoder until their cross-class edge protocol is added.
 */
#define A64_MACH_FPR_COLOR_N 16

typedef struct {
  ny_mach_regalloc_t alloc;
  bool *seeded;
  size_t colors_len;
} a64_mach_fpr_state_t;

static int a64_mach_fpr_reg(const a64_mach_fpr_state_t *state,
                            uint32_t vreg, size_t inst) {
  if (!state || vreg >= state->colors_len)
    return -1;
  const ny_mach_live_segment_t *seg =
      ny_mach_regalloc_segment_at(&state->alloc, vreg, inst);
  if (!seg || seg->color < 0 || seg->color >= A64_MACH_FPR_COLOR_N)
    return -1;
  return seg->color;
}

static const ny_mach_live_segment_t *a64_mach_fpr_segment(
    const a64_mach_fpr_state_t *state, uint32_t vreg, size_t inst) {
  return state ? ny_mach_regalloc_segment_at(&state->alloc, vreg, inst)
               : NULL;
}

static bool a64_mach_fpr_carries(const a64_mach_fpr_state_t *state,
                                 uint32_t vreg,
                                 const ny_mach_live_segment_t *seg) {
  if (!state || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const ny_mach_live_segment_t *prev =
      a64_mach_fpr_segment(state, vreg, seg->start - 1);
  return prev && prev->end + 1 == seg->start && !prev->spill &&
         prev->color == seg->color;
}

static bool a64_mach_fpr_begin(a64_mach_fpr_state_t *state, size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        a64_mach_fpr_segment(state, v, inst);
    if (seg && seg->start == inst)
      state->seeded[v] = a64_mach_fpr_carries(state, v, seg);
  }
  return true;
}

static bool a64_mach_fpr_end(const ny_mach_func_t *mach,
                             a64_mach_fpr_state_t *state,
                             size_t inst, ny_obj_buf_t *code) {
  if (!mach || !state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        a64_mach_fpr_segment(state, v, inst);
    if (!seg || seg->end != inst)
      continue;
    const ny_mach_live_segment_t *next =
        a64_mach_fpr_segment(state, v, inst + 1);
    bool needs_home = seg->spill || (next && !a64_mach_fpr_carries(state, v, next));
    if (needs_home && seg->color >= 0 && state->seeded[v]) {
      bool f32 = mach->vreg_types[v] == NY_MACH_TYPE_F32;
      ny_mach_operand_t home = {.kind = NY_MACH_OPERAND_VREG,
                                .as.reg = v};
      if (!a64_stur_fp(code, f32, (unsigned)seg->color,
                       a64_slot_off(mach, &home)))
        return false;
    }
    if (seg->spill || !next || !a64_mach_fpr_carries(state, v, next))
      state->seeded[v] = false;
  }
  return true;
}

static bool a64_mach_fpr_seed(const ny_mach_func_t *mach,
                              a64_mach_fpr_state_t *state, uint32_t vreg,
                              size_t inst, ny_obj_buf_t *code) {
  int reg = a64_mach_fpr_reg(state, vreg, inst);
  if (reg < 0 || state->seeded[vreg])
    return true;
  ny_mach_operand_t home = {.kind = NY_MACH_OPERAND_VREG, .as.reg = vreg};
  bool f32 = mach->vreg_types[vreg] == NY_MACH_TYPE_F32;
  if (!a64_ldur_fp(code, f32, (unsigned)reg, a64_slot_off(mach, &home)))
    return false;
  state->seeded[vreg] = true;
  return true;
}

static bool a64_mach_fpr_load(const ny_mach_func_t *mach,
                              a64_mach_fpr_state_t *state,
                              const ny_mach_operand_t *op, size_t inst,
                              unsigned scratch, ny_obj_buf_t *code,
                              unsigned *out) {
  if (!op || !out)
    return false;
  bool f32 = a64_operand_type(mach, op) == NY_MACH_TYPE_F32;
  if (op->kind == NY_MACH_OPERAND_VREG) {
    int reg = a64_mach_fpr_reg(state, op->as.reg, inst);
    if (reg >= 0) {
      if (!a64_mach_fpr_seed(mach, state, op->as.reg, inst, code))
        return false;
      *out = (unsigned)reg;
      return true;
    }
    *out = scratch;
    return a64_ldur_fp(code, f32, scratch, a64_slot_off(mach, op));
  }
  if (op->kind == NY_MACH_OPERAND_FRAME) {
    *out = scratch;
    return a64_ldur_fp(code, f32, scratch, a64_slot_off(mach, op));
  }
  return false;
}

static bool a64_mach_fpr_prepare(const ny_mach_func_t *mach,
                                 a64_mach_fpr_state_t *state) {
  if (!mach || !state || mach->block_len != 1 || !mach->vreg_len)
    return false;
  for (size_t i = 0; i < mach->vreg_len; ++i)
    if (mach->vreg_types[i] != NY_MACH_TYPE_F32 &&
        mach->vreg_types[i] != NY_MACH_TYPE_F64 &&
        mach->vreg_types[i] != NY_MACH_TYPE_I64)
      return false;
  for (size_t i = 0; i < mach->frame_slot_len; ++i)
    if (mach->frame_slots[i].type != NY_MACH_TYPE_F32 &&
        mach->frame_slots[i].type != NY_MACH_TYPE_F64)
      return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NY_MACH_CALL || in->opcode == NY_MACH_BR ||
        in->opcode == NY_MACH_BR_IF)
      return false;
    switch (in->opcode) {
    case NY_MACH_COPY:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          (in->src0.kind != NY_MACH_OPERAND_VREG &&
           in->src0.kind != NY_MACH_OPERAND_IMM))
        return false;
      break;
    case NY_MACH_LOAD:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_FRAME)
        return false;
      break;
    case NY_MACH_STORE:
      if (in->dst.kind != NY_MACH_OPERAND_FRAME ||
          in->src0.kind != NY_MACH_OPERAND_VREG)
        return false;
      break;
    case NY_MACH_ADD:
    case NY_MACH_SUB:
    case NY_MACH_MUL:
    case NY_MACH_DIV:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG)
        return false;
      break;
    case NY_MACH_RET:
      if (in->src0.kind != NY_MACH_OPERAND_NONE &&
          in->src0.kind != NY_MACH_OPERAND_VREG)
        return false;
      break;
    case NY_MACH_CMP:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->dst.as.reg >= mach->vreg_len ||
          mach->vreg_types[in->dst.as.reg] != NY_MACH_TYPE_I64 ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG ||
          (a64_operand_type(mach, &in->src0) != NY_MACH_TYPE_F32 &&
           a64_operand_type(mach, &in->src0) != NY_MACH_TYPE_F64) ||
          a64_operand_type(mach, &in->src0) !=
              a64_operand_type(mach, &in->src1))
        return false;
      break;
    case NY_MACH_NOP:
      break;
    default:
      return false;
    }
  }
  /*
   * Integer values are permitted only as comparison results consumed by the
   * final return. This keeps the fast path free of an implicit second-class
   * GPR allocator and prevents mixed-class values from crossing an edge.
   */
  for (uint32_t v = 0; v < mach->vreg_len; ++v) {
    if (mach->vreg_types[v] == NY_MACH_TYPE_F32 ||
        mach->vreg_types[v] == NY_MACH_TYPE_F64)
      continue;
    bool defined = false;
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const ny_mach_inst_t *in = &mach->insts[i];
      if (in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg == v) {
        if (in->opcode != NY_MACH_CMP || defined)
          return false;
        defined = true;
      }
      if (in->src0.kind == NY_MACH_OPERAND_VREG && in->src0.as.reg == v &&
          in->opcode != NY_MACH_RET)
        return false;
      if (in->src1.kind == NY_MACH_OPERAND_VREG && in->src1.as.reg == v)
        return false;
    }
    if (!defined)
      return false;
  }
  if (!ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_FPR,
                                    A64_MACH_FPR_COLOR_N, &state->alloc))
    return false;
  for (size_t i = 0; i < state->alloc.segment_len; ++i)
    if (state->alloc.segments[i].color < 0 ||
        state->alloc.segments[i].color >= A64_MACH_FPR_COLOR_N) {
      ny_mach_regalloc_free(&state->alloc);
      return false;
    }
  state->colors_len = mach->vreg_len;
  state->seeded = calloc(state->colors_len, sizeof(*state->seeded));
  if (!state->seeded) {
    ny_mach_regalloc_free(&state->alloc);
    return false;
  }
  return true;
}

static bool a64_mach_fpr_encode(const ny_mach_func_t *mach,
                                a64_mach_fpr_state_t *state,
                                ny_obj_buf_t *code, bool user, char *err,
                                size_t err_len) {
  (void)err;
  (void)err_len;
  int frame = a64_mach_frame_size(mach);
  if (!a64_u32(code, 0xA9BF7BFDu) || !a64_u32(code, 0x910003FDu) ||
      (frame && (frame >= 4096 ||
                 !a64_u32(code, 0xD10003FFu | ((uint32_t)frame << 10)))) )
    return false;
  if (user && !a64_mach_emit_param_spills(mach, code))
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    if (!a64_mach_fpr_begin(state, i))
      return false;
    int dst = in->dst.kind == NY_MACH_OPERAND_VREG
                  ? a64_mach_fpr_reg(state, in->dst.as.reg, i) : -1;
    bool f32 = a64_operand_type(mach, &in->dst) == NY_MACH_TYPE_F32;
    switch (in->opcode) {
    case NY_MACH_COPY: {
      if (dst < 0)
        return false;
      if (in->src0.kind == NY_MACH_OPERAND_IMM) {
        if (!a64_mov_imm64(code, 16, (uint64_t)in->src0.as.imm) ||
            !a64_u32(code, (f32 ? 0x1E270000u : 0x9E670000u) |
                            (16u << 5) | (uint32_t)dst))
          return false;
      } else {
        unsigned src = 16;
        if (!a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &src) ||
            (src != (unsigned)dst &&
             !a64_u32(code, (f32 ? 0x1E204000u : 0x1E604000u) |
                              (src << 5) | (uint32_t)dst)))
          return false;
      }
      state->seeded[in->dst.as.reg] = true;
      break;
    }
    case NY_MACH_LOAD: {
      if (dst < 0 || in->src0.kind != NY_MACH_OPERAND_FRAME ||
          !a64_ldur_fp(code, f32, (unsigned)dst,
                       a64_slot_off(mach, &in->src0)))
        return false;
      state->seeded[in->dst.as.reg] = true;
      break;
    }
    case NY_MACH_STORE: {
      unsigned src = 16;
      if (in->dst.kind != NY_MACH_OPERAND_FRAME ||
          !a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &src) ||
          !a64_stur_fp(code, a64_operand_type(mach, &in->src0) == NY_MACH_TYPE_F32,
                       src, a64_slot_off(mach, &in->dst)))
        return false;
      break;
    }
    case NY_MACH_ADD:
    case NY_MACH_SUB:
    case NY_MACH_MUL:
    case NY_MACH_DIV: {
      unsigned a = 16, b = 17;
      if (dst < 0 || !a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &a) ||
          !a64_mach_fpr_load(mach, state, &in->src1, i, 17, code, &b))
        return false;
      uint32_t base = f32 ? 0 : 0;
      if (in->opcode == NY_MACH_ADD) base = f32 ? 0x1E202800u : 0x1E602800u;
      if (in->opcode == NY_MACH_SUB) base = f32 ? 0x1E203800u : 0x1E603800u;
      if (in->opcode == NY_MACH_MUL) base = f32 ? 0x1E200800u : 0x1E600800u;
      if (in->opcode == NY_MACH_DIV) base = f32 ? 0x1E201800u : 0x1E601800u;
      if (!a64_u32(code, base | (b << 16) | (a << 5) | (uint32_t)dst))
        return false;
      state->seeded[in->dst.as.reg] = true;
      break;
    }
    case NY_MACH_CMP: {
      unsigned a = 16, b = 17;
      bool cmp_f32 = a64_operand_type(mach, &in->src0) == NY_MACH_TYPE_F32;
      if (!a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &a) ||
          !a64_mach_fpr_load(mach, state, &in->src1, i, 17, code, &b))
        return false;
      uint32_t fcmp = cmp_f32 ? 0x1E202000u : 0x1E602000u;
      if (!a64_u32(code, fcmp | (b << 16) | (a << 5)))
        return false;
      unsigned cond = 0xb; /* lt */
      switch (in->condition) {
      case NY_MACH_COND_EQ: cond = 0x0; break;
      case NY_MACH_COND_NE: cond = 0x1; break;
      case NY_MACH_COND_LT: cond = 0xb; break;
      case NY_MACH_COND_LE: cond = 0xd; break;
      case NY_MACH_COND_GT: cond = 0xc; break;
      case NY_MACH_COND_GE: cond = 0xa; break;
      default: return false;
      }
      /*
       * cset x0, cond; store the integer result in the vreg home.
       */
      if (!a64_u32(code, 0x9A9F07E0u | ((cond ^ 1u) << 12)) ||
          !a64_stur_x(code, 0, a64_slot_off(mach, &in->dst)))
        return false;
      break;
    }
    case NY_MACH_RET:
      if (in->src0.kind == NY_MACH_OPERAND_VREG) {
        if (mach->vreg_types[in->src0.as.reg] == NY_MACH_TYPE_I64) {
          if (!a64_ldur_x(code, 0, a64_slot_off(mach, &in->src0)))
            return false;
        } else {
          unsigned src = 16;
          if (!a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &src) ||
              (src != 0 && !a64_u32(code, (mach->vreg_types[in->src0.as.reg] == NY_MACH_TYPE_F32
                                               ? 0x1E204000u : 0x1E604000u) |
                                              (src << 5))))
            return false;
        }
      }
      if (frame && !a64_u32(code, 0x910003FFu | ((uint32_t)frame << 10)))
        return false;
      if (!a64_u32(code, 0xA8C17BFDu) || !a64_ret(code))
        return false;
      break;
    case NY_MACH_NOP:
      break;
    default:
      return false;
    }
    if (in->opcode != NY_MACH_RET && !a64_mach_fpr_end(mach, state, i, code))
      return false;
  }
  return true;
}

/*
 * Straight-line vector machine form uses the AAPCS64 Q-register file without
 * changing the general stack encoder.  Q16/Q17 are reserved as scratch
 * registers; Q0..Q15 are available to the allocator.  This is deliberately a
 * small fast path: calls, control flow, mixed scalar/vector values,
 * broadcasts, and uncolored ranges remain on the stack-backed encoder.
 */
#define A64_MACH_VECTOR_COLOR_N 16


/*
 * Target-tier reporting uses the same physical-color budgets as the AArch64
 * encoder.  Unlike the fast-path eligibility checks below, telemetry still
 * summarizes uncolored segments: a spill is exactly the information the
 * report is meant to expose rather than a reason to suppress the row.
 */
static bool a64_mach_profile_hot_loop_block(const ny_mach_func_t *mach,
                                            uint32_t block) {
  if (!mach || block >= mach->block_len)
    return false;
  uint32_t source_pc = mach->blocks[block].source_pc;
  return source_pc != UINT32_MAX && ny_native_profile_loop_hot(source_pc) >= 16;
}

static size_t a64_mach_regalloc_hot_peak_live(const ny_mach_func_t *mach,
                                              const ny_mach_regalloc_t *a,
                                              size_t inst_len) {
  if (!mach || !a || !inst_len)
    return 0;
  int *delta = calloc(inst_len + 1, sizeof(*delta));
  if (!delta)
    return 0;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &a->segments[i];
    if (!a64_mach_profile_hot_loop_block(mach, seg->block) ||
        seg->start >= inst_len)
      continue;
    size_t end = seg->end < inst_len ? seg->end : inst_len - 1;
    ++delta[seg->start];
    if (end + 1 < inst_len)
      --delta[end + 1];
  }
  size_t peak = 0;
  int live = 0;
  for (size_t pc = 0; pc < inst_len; ++pc) {
    live += delta[pc];
    if (live > 0 && (size_t)live > peak)
      peak = (size_t)live;
  }
  free(delta);
  return peak;
}

static void a64_mach_regalloc_summarize(const ny_mach_func_t *mach,
                                        const ny_mach_regalloc_t *a,
                                        size_t inst_len,
                                        ny_native_regalloc_metrics_t *out) {
  if (!out)
    return;
  *out = (ny_native_regalloc_metrics_t){0};
  if (!a)
    return;
  out->segments = a->segment_len;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &a->segments[i];
    if (seg->color >= 0)
      ++out->colored;
    else
      ++out->spilled;
    if (seg->reload)
      ++out->reloads;
    if (!a64_mach_profile_hot_loop_block(mach, seg->block))
      continue;
    ++out->hot_loop_segments;
    if (seg->color < 0)
      ++out->hot_loop_spilled;
    if (seg->reload)
      ++out->hot_loop_reloads;
  }
  out->peak_live = ny_mach_regalloc_peak_live(a, inst_len);
  out->hot_loop_peak_live = a64_mach_regalloc_hot_peak_live(mach, a, inst_len);
}

bool ny_native_a64_regalloc_metrics(const ny_mach_func_t *mach,
                                    ny_native_regalloc_metrics_t *gpr,
                                    ny_native_regalloc_metrics_t *fpr,
                                    ny_native_regalloc_metrics_t *vector) {
  if (gpr)
    *gpr = (ny_native_regalloc_metrics_t){0};
  if (fpr)
    *fpr = (ny_native_regalloc_metrics_t){0};
  if (vector)
    *vector = (ny_native_regalloc_metrics_t){0};
  if (!mach)
    return false;

  ny_mach_regalloc_t alloc = {0};
  if (mach->vreg_len &&
      !ny_mach_regalloc_build(mach, A64_MACH_COLOR_N, &alloc))
    return false;
  a64_mach_regalloc_summarize(mach, &alloc, mach->inst_len, gpr);
  ny_mach_regalloc_free(&alloc);

  if (mach->vreg_len &&
      !ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_FPR,
                                    A64_MACH_FPR_COLOR_N, &alloc))
    return false;
  a64_mach_regalloc_summarize(mach, &alloc, mach->inst_len, fpr);
  ny_mach_regalloc_free(&alloc);

  if (mach->vreg_len &&
      !ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_VECTOR,
                                    A64_MACH_VECTOR_COLOR_N, &alloc))
    return false;
  a64_mach_regalloc_summarize(mach, &alloc, mach->inst_len, vector);
  ny_mach_regalloc_free(&alloc);
  return true;
}

typedef struct {
  ny_mach_regalloc_t alloc;
  bool *seeded;
  size_t colors_len;
} a64_mach_vector_state_t;

static int a64_mach_vector_reg(const a64_mach_vector_state_t *state,
                               uint32_t vreg, size_t inst) {
  if (!state || vreg >= state->alloc.vreg_len)
    return -1;
  const ny_mach_live_segment_t *seg =
      ny_mach_regalloc_segment_at(&state->alloc, vreg, inst);
  if (!seg || seg->color < 0 || seg->color >= A64_MACH_VECTOR_COLOR_N)
    return -1;
  return seg->color;
}

static const ny_mach_live_segment_t *a64_mach_vector_segment(
    const a64_mach_vector_state_t *state, uint32_t vreg, size_t inst) {
  return state ? ny_mach_regalloc_segment_at(&state->alloc, vreg, inst) : NULL;
}

static bool a64_mach_is_v128(const ny_mach_func_t *m,
                             const ny_mach_operand_t *op) {
  if (!op || op->kind != NY_MACH_OPERAND_VREG || op->as.reg >= m->vreg_len ||
      !m->vreg_types)
    return false;
  ny_mach_type_t t = m->vreg_types[op->as.reg];
  return t == NY_MACH_TYPE_V128_I64 || t == NY_MACH_TYPE_V128_F64 ||
         t == NY_MACH_TYPE_V128_F32;
}

static bool a64_mach_vector_carries(const a64_mach_vector_state_t *state,
                                    uint32_t vreg,
                                    const ny_mach_live_segment_t *seg) {
  if (!state || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const ny_mach_live_segment_t *prev =
      a64_mach_vector_segment(state, vreg, seg->start - 1);
  /*
   * Every CFG edge communicates through the vreg home. Reusing a color on
   * both sides of an edge must not be mistaken for register residency.
   */
  return prev && prev->block == seg->block && prev->end + 1 == seg->start &&
         !prev->spill &&
         prev->color == seg->color;
}

static bool a64_mach_vector_begin(a64_mach_vector_state_t *state,
                                  size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        a64_mach_vector_segment(state, v, inst);
    if (seg && seg->start == inst)
      state->seeded[v] = a64_mach_vector_carries(state, v, seg);
  }
  return true;
}

/*
 * Try to fold single-block machine form to a const i64 return.
 */
static bool a64_try_const_ret(const ny_mach_func_t *mach, int64_t *out) {
  if (!mach || !out || mach->block_len > 1)
    return false;
  int64_t *val = NULL;
  bool *known = NULL;
  if (mach->vreg_len) {
    val = calloc(mach->vreg_len, sizeof(int64_t));
    known = calloc(mach->vreg_len, sizeof(bool));
    if (!val || !known) {
      free(val);
      free(known);
      return false;
    }
  }
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    switch (in->opcode) {
    case NY_MACH_COPY:
      if (in->dst.kind == NY_MACH_OPERAND_VREG &&
          in->src0.kind == NY_MACH_OPERAND_IMM && known) {
        known[in->dst.as.reg] = true;
        val[in->dst.as.reg] = in->src0.as.imm;
      } else if (in->dst.kind == NY_MACH_OPERAND_VREG &&
                 in->src0.kind == NY_MACH_OPERAND_VREG && known &&
                 in->src0.as.reg < mach->vreg_len && known[in->src0.as.reg]) {
        known[in->dst.as.reg] = true;
        val[in->dst.as.reg] = val[in->src0.as.reg];
      } else {
        free(val);
        free(known);
        return false;
      }
      break;
    case NY_MACH_ADD:
    case NY_MACH_SUB:
    case NY_MACH_MUL:
    case NY_MACH_AND:
    case NY_MACH_OR:
    case NY_MACH_XOR:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG || !known ||
          in->src0.as.reg >= mach->vreg_len ||
          in->src1.as.reg >= mach->vreg_len || !known[in->src0.as.reg] ||
          !known[in->src1.as.reg]) {
        free(val);
        free(known);
        return false;
      }
      {
        int64_t a = val[in->src0.as.reg], b = val[in->src1.as.reg], r = 0;
        switch (in->opcode) {
        case NY_MACH_ADD: r = a + b; break;
        case NY_MACH_SUB: r = a - b; break;
        case NY_MACH_MUL: r = a * b; break;
        case NY_MACH_AND: r = a & b; break;
        case NY_MACH_OR: r = a | b; break;
        case NY_MACH_XOR: r = a ^ b; break;
        default: break;
        }
        known[in->dst.as.reg] = true;
        val[in->dst.as.reg] = r;
      }
      break;
    case NY_MACH_RET:
      if (in->src0.kind == NY_MACH_OPERAND_IMM) {
        *out = in->src0.as.imm;
        free(val);
        free(known);
        return true;
      }
      if (in->src0.kind == NY_MACH_OPERAND_VREG && known &&
          in->src0.as.reg < mach->vreg_len && known[in->src0.as.reg]) {
        *out = val[in->src0.as.reg];
        free(val);
        free(known);
        return true;
      }
      free(val);
      free(known);
      return false;
    case NY_MACH_NOP:
      break;
    default:
      free(val);
      free(known);
      return false;
    }
  }
  free(val);
  free(known);
  return false;
}

/*
 * Stack-slot AArch64 machine-form encoding.
 */
static size_t a64_mach_align(size_t value, size_t align) {
  if (align < 1)
    align = 1;
  return (value + align - 1) & ~(align - 1);
}

static bool a64_mach_type_v128(ny_mach_type_t type) {
  return type == NY_MACH_TYPE_V128_I64 ||
         type == NY_MACH_TYPE_V128_F64 ||
         type == NY_MACH_TYPE_V128_F32;
}

static ny_mach_type_t a64_operand_type(const ny_mach_func_t *mach,
                                       const ny_mach_operand_t *op) {
  if (!mach || !op)
    return NY_MACH_TYPE_NONE;
  if (op->kind == NY_MACH_OPERAND_VREG && op->as.reg < mach->vreg_len &&
      mach->vreg_types)
    return mach->vreg_types[op->as.reg];
  if (op->kind == NY_MACH_OPERAND_FRAME &&
      op->as.frame_index < mach->frame_slot_len && mach->frame_slots)
    return mach->frame_slots[op->as.frame_index].type;
  return NY_MACH_TYPE_NONE;
}

static size_t a64_frame_slot_size(const ny_mach_frame_slot_t *slot) {
  size_t size = slot && slot->size ? slot->size : 8;
  if (slot && a64_mach_type_v128(slot->type) && size < 16)
    size = 16;
  return size < 8 ? 8 : size;
}

static size_t a64_frame_slot_align(const ny_mach_frame_slot_t *slot) {
  size_t align = slot && slot->align ? slot->align : 8;
  if (slot && a64_mach_type_v128(slot->type) && align < 16)
    align = 16;
  if (align < 8)
    align = 8;
  return align;
}

static size_t a64_frame_slots_end(const ny_mach_func_t *mach) {
  size_t cursor = 0;
  if (!mach)
    return 0;
  for (size_t i = 0; i < mach->frame_slot_len; ++i) {
    cursor = a64_mach_align(cursor, a64_frame_slot_align(&mach->frame_slots[i]));
    cursor += a64_frame_slot_size(&mach->frame_slots[i]);
  }
  return cursor;
}

static int a64_slot_off(const ny_mach_func_t *mach, const ny_mach_operand_t *op) {
  if (!mach || !op)
    return 0;
  size_t cursor = 0;
  if (op->kind == NY_MACH_OPERAND_FRAME) {
    if (op->as.frame_index >= mach->frame_slot_len)
      return 0;
    for (size_t i = 0; i <= op->as.frame_index; ++i) {
      cursor = a64_mach_align(cursor,
                              a64_frame_slot_align(&mach->frame_slots[i]));
      cursor += a64_frame_slot_size(&mach->frame_slots[i]);
    }
    return -(int)cursor;
  }
  if (op->kind == NY_MACH_OPERAND_VREG) {
    if (op->as.reg >= mach->vreg_len)
      return 0;
    cursor = a64_frame_slots_end(mach);
    for (size_t i = 0; i <= op->as.reg; ++i) {
      bool vec = mach->vreg_types && a64_mach_type_v128(mach->vreg_types[i]);
      cursor = a64_mach_align(cursor, vec ? 16 : 8);
      cursor += vec ? 16 : 8;
    }
    return -(int)cursor;
  }
  return 0;
}

static int a64_mach_frame_size(const ny_mach_func_t *mach) {
  if (!mach)
    return 0;
  size_t cursor = a64_frame_slots_end(mach);
  for (size_t i = 0; i < mach->vreg_len; ++i) {
    bool vec = mach->vreg_types && a64_mach_type_v128(mach->vreg_types[i]);
    cursor = a64_mach_align(cursor, vec ? 16 : 8);
    cursor += vec ? 16 : 8;
  }
  return (int)a64_mach_align(cursor, 16);
}

static bool a64_is_v128(const ny_mach_func_t *mach,
                        const ny_mach_operand_t *op) {
  return a64_mach_type_v128(a64_operand_type(mach, op));
}

static bool a64_is_f32(const ny_mach_func_t *mach,
                       const ny_mach_operand_t *op) {
  return a64_operand_type(mach, op) == NY_MACH_TYPE_F32;
}

static bool a64_is_float(const ny_mach_func_t *mach,
                         const ny_mach_operand_t *op) {
  ny_mach_type_t type = a64_operand_type(mach, op);
  return type == NY_MACH_TYPE_F32 || type == NY_MACH_TYPE_F64;
}

static bool a64_frame_addr(ny_obj_buf_t *code, unsigned reg, int off) {
  if (!code || reg > 30 || off < -4095 || off > 4095)
    return false;
  uint32_t imm = (uint32_t)(off < 0 ? -off : off);
  uint32_t op = off < 0 ? 0xD1000000u : 0x91000000u;
  return a64_u32(code, op | (imm << 10) | (29u << 5) | reg);
}

static bool a64_q_unscaled(ny_obj_buf_t *code, bool load, unsigned reg,
                           unsigned base, int off) {
  if (!code || reg > 31 || base > 31 || off < -256 || off > 255)
    return false;
  uint32_t op = load ? 0x3CC00000u : 0x3C800000u;
  return a64_u32(code, op | ((uint32_t)(off & 0x1ff) << 12) |
                           ((base & 31u) << 5) | reg);
}

static bool a64_load_q(ny_obj_buf_t *code, unsigned reg, int off) {
  if (off >= -256 && off <= 255)
    return a64_q_unscaled(code, true, reg, 29, off);
  return a64_frame_addr(code, 16, off) &&
         a64_q_unscaled(code, true, reg, 16, 0);
}

static bool a64_store_q(ny_obj_buf_t *code, unsigned reg, int off) {
  if (off >= -256 && off <= 255)
    return a64_q_unscaled(code, false, reg, 29, off);
  return a64_frame_addr(code, 16, off) &&
         a64_q_unscaled(code, false, reg, 16, 0);
}

static bool a64_mach_vector_end(a64_mach_vector_state_t *state,
                                const ny_mach_func_t *mach, ny_obj_buf_t *code,
                                size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const ny_mach_live_segment_t *seg =
        a64_mach_vector_segment(state, v, inst);
    if (!seg || seg->end != inst)
      continue;
    const ny_mach_live_segment_t *next =
        a64_mach_vector_segment(state, v, inst + 1);
    bool needs_home = seg->spill ||
                      (next && !a64_mach_vector_carries(state, v, next));
    if (needs_home && seg->color >= 0 && state->seeded[v]) {
      ny_mach_operand_t home = {.kind = NY_MACH_OPERAND_VREG,
                                 .as.reg = v};
      if (!a64_store_q(code, (unsigned)seg->color,
                       a64_slot_off(mach, &home)))
        return false;
    }
    if (seg->spill || !next || !a64_mach_vector_carries(state, v, next))
      state->seeded[v] = false;
  }
  return true;
}

static bool a64_mach_vector_flush(a64_mach_vector_state_t *state,
                                  const ny_mach_func_t *mach,
                                  ny_obj_buf_t *code, size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    if (!state->seeded[v])
      continue;
    const ny_mach_live_segment_t *seg =
        a64_mach_vector_segment(state, v, inst);
    if (!seg || seg->color < 0)
      continue;
    ny_mach_operand_t home = {.kind = NY_MACH_OPERAND_VREG, .as.reg = v};
    if (!a64_store_q(code, (unsigned)seg->color, a64_slot_off(mach, &home)))
      return false;
    state->seeded[v] = false;
  }
  return true;
}

static bool a64_mach_vector_prepare(const ny_mach_func_t *mach,
                                     a64_mach_vector_state_t *state) {
  if (!mach || !state || !mach->vreg_types)
    return false;
  bool has_vector = false;
  for (size_t i = 0; i < mach->vreg_len; ++i) {
    ny_mach_type_t type = mach->vreg_types[i];
    if (type == NY_MACH_TYPE_F32 || type == NY_MACH_TYPE_F64 ||
        type == NY_MACH_TYPE_I64 || type == NY_MACH_TYPE_PTR)
      return false;
    if (a64_mach_type_v128(type))
      has_vector = true;
  }
  if (!has_vector)
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    bool vector = false;
    const ny_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                      &in->src2};
    for (size_t k = 0; k < sizeof(ops) / sizeof(*ops); ++k)
      if (ops[k]->kind == NY_MACH_OPERAND_VREG &&
          ops[k]->as.reg < mach->vreg_len &&
          a64_mach_type_v128(mach->vreg_types[ops[k]->as.reg]))
        vector = true;
    if (in->opcode == NY_MACH_CALL) {
      if (a64_mach_is_v128(mach, &in->dst))
        return false;
      for (size_t ai = 0; ai < in->args_len; ++ai)
        if (a64_mach_is_v128(mach, &in->args[ai]))
          return false;
      continue;
    }
    if (in->opcode == NY_MACH_BR_IF) {
      if (in->src0.kind != NY_MACH_OPERAND_VREG)
        return false;
      continue;
    }
    if (in->opcode == NY_MACH_RET) {
      if (in->src0.kind == NY_MACH_OPERAND_VREG)
        return false;
      continue;
    }
    if (in->opcode == NY_MACH_NOP)
      continue;
    if (!vector)
      return false;
    switch (in->opcode) {
    case NY_MACH_COPY:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          (in->src0.kind != NY_MACH_OPERAND_VREG &&
           in->src0.kind != NY_MACH_OPERAND_FRAME))
        return false;
      break;
    case NY_MACH_LOAD:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_FRAME)
        return false;
      break;
    case NY_MACH_STORE:
      if (in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->dst.kind != NY_MACH_OPERAND_FRAME)
        return false;
      break;
    case NY_MACH_ADD:
    case NY_MACH_SUB:
    case NY_MACH_MUL:
    case NY_MACH_AND:
    case NY_MACH_OR:
    case NY_MACH_XOR:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG)
        return false;
      if ((in->opcode == NY_MACH_AND || in->opcode == NY_MACH_OR ||
           in->opcode == NY_MACH_XOR) &&
          a64_operand_type(mach, &in->dst) != NY_MACH_TYPE_V128_I64)
        return false;
      break;
    case NY_MACH_DIV:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG ||
          a64_operand_type(mach, &in->dst) == NY_MACH_TYPE_V128_I64)
        return false;
      break;
    case NY_MACH_SHL:
    case NY_MACH_SAR:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG ||
          a64_operand_type(mach, &in->dst) != NY_MACH_TYPE_V128_I64)
        return false;
      break;
    case NY_MACH_FMA:
      if (in->dst.kind != NY_MACH_OPERAND_VREG ||
          in->src0.kind != NY_MACH_OPERAND_VREG ||
          in->src1.kind != NY_MACH_OPERAND_VREG ||
          in->src2.kind != NY_MACH_OPERAND_VREG ||
          a64_operand_type(mach, &in->dst) == NY_MACH_TYPE_V128_I64)
        return false;
      break;
    case NY_MACH_NOP:
      break;
    default:
      return false;
    }
  }
  if (!ny_mach_regalloc_build_class(mach, NY_MACH_REGCLASS_VECTOR,
                                    A64_MACH_VECTOR_COLOR_N, &state->alloc))
    return false;
  state->colors_len = mach->vreg_len;
  state->seeded = calloc(state->colors_len ? state->colors_len : 1,
                         sizeof(*state->seeded));
  if (!state->seeded) {
    ny_mach_regalloc_free(&state->alloc);
    return false;
  }
  for (size_t i = 0; i < state->alloc.segment_len; ++i) {
    const ny_mach_live_segment_t *seg = &state->alloc.segments[i];
    if (seg->color < 0 || seg->color >= A64_MACH_VECTOR_COLOR_N) {
      free(state->seeded);
      state->seeded = NULL;
      ny_mach_regalloc_free(&state->alloc);
      return false;
    }
  }
  return true;
}

static bool a64_mach_vector_load_operand(const ny_mach_func_t *mach,
                                          a64_mach_vector_state_t *state,
                                          const ny_mach_operand_t *op,
                                          size_t inst, unsigned scratch,
                                          unsigned *out, ny_obj_buf_t *code) {
  if (!op || !out || !code)
    return false;
  if (op->kind == NY_MACH_OPERAND_VREG) {
    int reg = a64_mach_vector_reg(state, op->as.reg, inst);
    if (reg >= 0) {
      if (!state->seeded[op->as.reg]) {
        if (!a64_load_q(code, (unsigned)reg, a64_slot_off(mach, op)))
          return false;
      }
      if (state->seeded)
        state->seeded[op->as.reg] = true;
      *out = (unsigned)reg;
      return true;
    }
    if (!a64_load_q(code, scratch, a64_slot_off(mach, op)))
      return false;
    *out = scratch;
    return true;
  }
  if (op->kind != NY_MACH_OPERAND_FRAME ||
      !a64_load_q(code, scratch, a64_slot_off(mach, op)))
    return false;
  *out = scratch;
  return true;
}

static bool a64_mach_vector_copy(const ny_mach_func_t *mach,
                                 a64_mach_vector_state_t *state,
                                 const ny_mach_inst_t *in, size_t inst,
                                 ny_obj_buf_t *code) {
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned src = 16;
  if (dst < 0 || !a64_mach_vector_load_operand(mach, state, &in->src0, inst,
                                               16, &src, code))
    return false;
  if (src != (unsigned)dst &&
      !a64_u32(code, 0x4EA01C00u | (src << 16) |
                          ((unsigned)src << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_load(const ny_mach_func_t *mach,
                                 a64_mach_vector_state_t *state,
                                 const ny_mach_inst_t *in, size_t inst,
                                 ny_obj_buf_t *code) {
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  if (dst < 0 || in->src0.kind != NY_MACH_OPERAND_FRAME ||
      !a64_load_q(code, (unsigned)dst, a64_slot_off(mach, &in->src0)))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_store(const ny_mach_func_t *mach,
                                  a64_mach_vector_state_t *state,
                                  const ny_mach_inst_t *in, size_t inst,
                                  ny_obj_buf_t *code) {
  unsigned src = 16;
  return in->dst.kind == NY_MACH_OPERAND_FRAME &&
         a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &src,
                                      code) &&
         a64_store_q(code, src, a64_slot_off(mach, &in->dst));
}

static bool a64_mach_vector_binop(const ny_mach_func_t *mach,
                                  a64_mach_vector_state_t *state,
                                  const ny_mach_inst_t *in, size_t inst,
                                  ny_obj_buf_t *code) {
  ny_mach_type_t type = a64_operand_type(mach, &in->dst);
  uint32_t op = 0;
  if (type == NY_MACH_TYPE_V128_I64) {
    if (in->opcode == NY_MACH_ADD) op = 0x4EE08400u;
    else if (in->opcode == NY_MACH_SUB) op = 0x6EE08400u;
    else if (in->opcode == NY_MACH_MUL) return false;
    else if (in->opcode == NY_MACH_AND) op = 0x4E201C00u;
    else if (in->opcode == NY_MACH_OR) op = 0x4EA01C00u;
    else if (in->opcode == NY_MACH_XOR) op = 0x6E201C00u;
  } else if (type == NY_MACH_TYPE_V128_F64) {
    if (in->opcode == NY_MACH_ADD) op = 0x4E60D400u;
    else if (in->opcode == NY_MACH_SUB) op = 0x4EE0D400u;
    else if (in->opcode == NY_MACH_MUL) op = 0x6E60DC00u;
  } else if (type == NY_MACH_TYPE_V128_F32) {
    if (in->opcode == NY_MACH_ADD) op = 0x4E20D400u;
    else if (in->opcode == NY_MACH_SUB) op = 0x4EA0D400u;
    else if (in->opcode == NY_MACH_MUL) op = 0x6E20DC00u;
  }
  if (!op)
    return false;
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned a = 16, b = 17;
  if (dst < 0 || !a64_mach_vector_load_operand(mach, state, &in->src0, inst,
                                                16, &a, code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src1, inst, 17, &b,
                                     code))
    return false;
  if (!a64_u32(code, op | (b << 16) | (a << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_div(const ny_mach_func_t *mach,
                                a64_mach_vector_state_t *state,
                                const ny_mach_inst_t *in, size_t inst,
                                ny_obj_buf_t *code) {
  ny_mach_type_t type = a64_operand_type(mach, &in->dst);
  uint32_t op = type == NY_MACH_TYPE_V128_F64 ? 0x6E60FC00u :
                type == NY_MACH_TYPE_V128_F32 ? 0x6E20FC00u : 0;
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned a = 16, b = 17;
  if (!op || dst < 0 ||
      !a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &a,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src1, inst, 17, &b,
                                    code) ||
      !a64_u32(code, op | (b << 16) | (a << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_shift(const ny_mach_func_t *mach,
                                  a64_mach_vector_state_t *state,
                                  const ny_mach_inst_t *in, size_t inst,
                                  ny_obj_buf_t *code) {
  if (a64_operand_type(mach, &in->dst) != NY_MACH_TYPE_V128_I64)
    return false;
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned a = 16, b = 17;
  if (dst < 0 ||
      !a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &a,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src1, inst, 17, &b,
                                    code))
    return false;
  uint32_t op = in->opcode == NY_MACH_SAR ? 0x6EE0B842u : 0x4EE24420u;
  if (!a64_u32(code, op) || !a64_u32(code, op | (b << 16) | (a << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_fma(const ny_mach_func_t *mach,
                                a64_mach_vector_state_t *state,
                                const ny_mach_inst_t *in, size_t inst,
                                ny_obj_buf_t *code) {
  ny_mach_type_t type = a64_operand_type(mach, &in->dst);
  uint32_t op = type == NY_MACH_TYPE_V128_F64 ? 0x4E60CC00u :
                type == NY_MACH_TYPE_V128_F32 ? 0x4E20CC00u : 0;
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned a = 16, b = 17, c = 18;
  if (!op || dst < 0 ||
      !a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &a,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src1, inst, 17, &b,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src2, inst, 18, &c,
                                    code))
    return false;
  /*
   * FMLA is destructive: Vd = Vd + Vn * Vm.  Seed Vd with the addend
   * before applying the multiply-add.
   */
  if (c != (unsigned)dst &&
      !a64_u32(code, 0x4EA01C00u | (c << 16) | (c << 5) |
                          (unsigned)dst))
    return false;
  if (!a64_u32(code, op | (b << 16) | (a << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_encode_call(const ny_mach_func_t *mach,
                                        a64_mach_vector_state_t *state,
                                        const ny_mach_inst_t *in, size_t inst,
                                        ny_obj_buf_t *code,
                                        ny_x64_obj_reloc_t *relocs,
                                        size_t *reloc_count,
                                        size_t reloc_cap,
                                        size_t code_base,
                                        const ny_native_target_info_t *target) {
  (void)mach;
  (void)code_base;
  if (in->call_is_extern && in->src0.kind == NY_MACH_OPERAND_SYMBOL) {
    const char *raw = in->src0.as.symbol;
    if (!raw)
      return false;
    char call_sym[256];
    const char *pref = target && target->symbol_prefix ? target->symbol_prefix : "";
    if (in->call_is_extern || strncmp(raw, "rt_", 3) == 0 ||
        strncmp(raw, "ny_fn_", 6) == 0 || raw[0] == '_')
      snprintf(call_sym, sizeof(call_sym), "%s%s", pref, raw);
    else
      snprintf(call_sym, sizeof(call_sym), NY_FMT_FN, pref, raw);
    if (!a64_u32(code, 0x94000000u))
      return false;
    if (relocs && reloc_count && *reloc_count < reloc_cap) {
      snprintf(relocs[*reloc_count].symbol,
               sizeof(relocs[*reloc_count].symbol), "%s", call_sym);
      relocs[*reloc_count].disp_off = code->len - 4;
      relocs[*reloc_count].type = NY_RELOC_AARCH64_CALL26;
      (*reloc_count)++;
    }
    return true;
  } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
    int reg = a64_mach_vector_reg(state, in->src0.as.reg, inst);
    if (reg < 0)
      return false;
    if (!a64_u32(code, 0xD63F0000u | ((uint32_t)reg << 5)))
      return false;
    return true;
  } else {
    return false;
  }
}

static bool a64_mach_vector_encode(const ny_mach_func_t *mach,
                                    a64_mach_vector_state_t *state,
                                    ny_obj_buf_t *code, bool user,
                                    ny_x64_obj_reloc_t *relocs,
                                    size_t *reloc_count,
                                    size_t reloc_cap,
                                    size_t code_base,
                                    const ny_native_target_info_t *target,
                                    char *err, size_t err_len) {
  (void)code_base;
  int frame = a64_mach_frame_size(mach);
  if (!a64_u32(code, 0xA9BF7BFDu) || !a64_u32(code, 0x910003FDu))
    return false;
  if (frame && (frame >= 4096 ||
                !a64_u32(code, 0xD10003FFu | ((uint32_t)frame << 10))))
    return false;
  if (user && !a64_mach_emit_param_spills(mach, code))
    return false;
  size_t *block_off = calloc(mach->block_len ? mach->block_len : 1,
                             sizeof(*block_off));
  size_t *patch_at = calloc(mach->inst_len ? mach->inst_len : 1,
                            sizeof(*patch_at));
  size_t *patch_blk = calloc(mach->inst_len ? mach->inst_len : 1,
                             sizeof(*patch_blk));
  size_t patch_len = 0;
  if (!block_off || !patch_at || !patch_blk)
    goto fail;
  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    const ny_mach_block_t *block = &mach->blocks[bi];
    block_off[bi] = code->len;
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t inst = block->first_inst + n;
      const ny_mach_inst_t *in = &mach->insts[inst];
      if (!a64_mach_vector_begin(state, inst))
        goto fail;
      bool ok = false;
      switch (in->opcode) {
      case NY_MACH_COPY:
        ok = a64_mach_vector_copy(mach, state, in, inst, code);
        break;
      case NY_MACH_LOAD:
        ok = a64_mach_vector_load(mach, state, in, inst, code);
        break;
      case NY_MACH_STORE:
        ok = a64_mach_vector_store(mach, state, in, inst, code);
        break;
      case NY_MACH_ADD:
      case NY_MACH_SUB:
      case NY_MACH_MUL:
      case NY_MACH_AND:
      case NY_MACH_OR:
      case NY_MACH_XOR:
        ok = a64_mach_vector_binop(mach, state, in, inst, code);
        break;
      case NY_MACH_DIV:
        ok = a64_mach_vector_div(mach, state, in, inst, code);
        break;
      case NY_MACH_SHL:
      case NY_MACH_SAR:
        ok = a64_mach_vector_shift(mach, state, in, inst, code);
        break;
      case NY_MACH_FMA:
        ok = a64_mach_vector_fma(mach, state, in, inst, code);
        break;
      case NY_MACH_BR:
        if (patch_len >= mach->inst_len ||
            !a64_mach_vector_flush(state, mach, code, inst) ||
            !a64_u32(code, 0x14000000u))
          ok = false;
        else {
          patch_at[patch_len] = code->len - 4;
          patch_blk[patch_len++] = in->src1.as.block_index;
          ok = true;
        }
        break;
      case NY_MACH_BR_IF:
        if (patch_len >= mach->inst_len ||
            !a64_mach_vector_flush(state, mach, code, inst) ||
            !a64_u32(code, 0xB5000000u))
          ok = false;
        else {
          patch_at[patch_len] = code->len - 4;
          patch_blk[patch_len++] = in->src1.as.block_index;
          ok = true;
        }
        break;
      case NY_MACH_CALL:
        if (!a64_mach_vector_flush(state, mach, code, inst))
          ok = false;
        else
          ok = a64_mach_vector_encode_call(mach, state, in, inst, code,
                                           relocs, reloc_count, reloc_cap,
                                           0, target);
        break;
      case NY_MACH_RET:
        if (frame && !a64_u32(code, 0x910003FFu | ((uint32_t)frame << 10)))
          ok = false;
        else
          ok = a64_u32(code, 0xA8C17BFDu) && a64_ret(code);
        break;
      case NY_MACH_NOP:
        ok = true;
        break;
      default:
        ok = false;
        break;
      }
      if (!ok || !a64_mach_vector_end(state, mach, code, inst)) {
        if (err && err_len)
          snprintf(err, err_len,
                   "a64 vector register allocation encode failed at instruction %zu",
                   inst);
        goto fail;
      }
    }
  }
  for (size_t i = 0; i < patch_len; ++i) {
    if (patch_blk[i] >= mach->block_len || patch_at[i] > UINT32_MAX ||
        block_off[patch_blk[i]] > UINT32_MAX)
      goto fail;
    int64_t delta = (int64_t)block_off[patch_blk[i]] -
                    (int64_t)patch_at[i];
    if ((delta & 3) != 0 || delta / 4 < -(1ll << 25) ||
        delta / 4 >= (1ll << 25))
      goto fail;
    ny_obj_patch_u32(code, patch_at[i],
                     0x14000000u | ((uint32_t)(delta / 4) & 0x03ffffffu));
  }
  free(block_off);
  free(patch_at);
  free(patch_blk);
  return true;
fail:
  free(block_off);
  free(patch_at);
  free(patch_blk);
  return false;
}

static bool a64_q_ptr(ny_obj_buf_t *code, bool load, unsigned reg,
                      unsigned base) {
  if (!code || reg > 31 || base > 30)
    return false;
  uint32_t op = load ? 0x3DC00000u : 0x3D800000u;
  return a64_u32(code, op | (base << 5) | reg);
}

static bool a64_ldur_x(ny_obj_buf_t *code, unsigned reg, int off) {
  if (reg > 30)
    return false;
  if (off >= -256 && off <= 255)
    return a64_u32(code, 0xF8400000u | ((uint32_t)(off & 0x1ff) << 12) |
                             (29u << 5) | reg);
  return a64_frame_addr(code, 16, off) &&
         a64_u32(code, 0xF9400000u | (16u << 5) | reg);
}

static bool a64_stur_x(ny_obj_buf_t *code, unsigned reg, int off) {
  if (reg > 30)
    return false;
  if (off >= -256 && off <= 255)
    return a64_u32(code, 0xF8000000u | ((uint32_t)(off & 0x1ff) << 12) |
                             (29u << 5) | reg);
  unsigned scratch = reg == 16 ? 17u : 16u;
  return a64_frame_addr(code, scratch, off) &&
         a64_u32(code, 0xF9000000u | (scratch << 5) | reg);
}

static bool a64_ldur_fp(ny_obj_buf_t *code, bool f32, unsigned reg, int off) {
  if (reg > 31)
    return false;
  if (off >= -256 && off <= 255) {
    uint32_t op = f32 ? 0xBC400000u : 0xFC400000u;
    return a64_u32(code, op | ((uint32_t)(off & 0x1ff) << 12) |
                             (29u << 5) | reg);
  }
  uint32_t op = f32 ? 0xBD400000u : 0xFD400000u;
  return a64_frame_addr(code, 16, off) &&
         a64_u32(code, op | (16u << 5) | reg);
}

static bool a64_stur_fp(ny_obj_buf_t *code, bool f32, unsigned reg, int off) {
  if (reg > 31)
    return false;
  if (off >= -256 && off <= 255) {
    uint32_t op = f32 ? 0xBC000000u : 0xFC000000u;
    return a64_u32(code, op | ((uint32_t)(off & 0x1ff) << 12) |
                             (29u << 5) | reg);
  }
  uint32_t op = f32 ? 0xBD000000u : 0xFD000000u;
  return a64_frame_addr(code, 16, off) &&
         a64_u32(code, op | (16u << 5) | reg);
}

static uint64_t a64_shuffle_half(unsigned imm, unsigned lane_bytes,
                                 unsigned first_lane, unsigned lane_count) {
  uint64_t bits = 0;
  unsigned shift = lane_count == 2 ? 1u : 2u;
  for (unsigned byte = 0; byte < 8; ++byte) {
    unsigned lane = first_lane + byte / lane_bytes;
    unsigned src_lane = (imm >> (lane * shift)) & (lane_count - 1u);
    unsigned src_byte = src_lane * lane_bytes + byte % lane_bytes;
    bits |= (uint64_t)src_byte << (byte * 8);
  }
  return bits;
}

static bool a64_ldr_x_base(ny_obj_buf_t *code, unsigned reg, unsigned base,
                              int off) {
  if (reg > 30 || base > 31 || off < 0 || (off & 7) || off > 32760)
    return false;
  return a64_u32(code, 0xF9400000u | ((uint32_t)(off / 8) << 10) |
                           (base << 5) | reg);
}

static bool a64_ldr_fp_base(ny_obj_buf_t *code, bool f32, unsigned reg,
                            unsigned base, int off) {
  unsigned scale = f32 ? 4u : 8u;
  if (reg > 31 || base > 31 || off < 0 || off % (int)scale ||
      off / (int)scale > 4095)
    return false;
  uint32_t op = f32 ? 0xBD400000u : 0xFD400000u;
  return a64_u32(code, op | ((uint32_t)(off / (int)scale) << 10) |
                           (base << 5) | reg);
}

static bool a64_str_fp_base(ny_obj_buf_t *code, bool f32, unsigned reg,
                            unsigned base, int off) {
  unsigned scale = f32 ? 4u : 8u;
  if (reg > 31 || base > 31 || off < 0 || off % (int)scale ||
      off / (int)scale > 4095)
    return false;
  uint32_t op = f32 ? 0xBD000000u : 0xFD000000u;
  return a64_u32(code, op | ((uint32_t)(off / (int)scale) << 10) |
                           (base << 5) | reg);
}

static bool a64_ldr_q_base(ny_obj_buf_t *code, unsigned reg, unsigned base,
                           int off) {
  if (reg > 31 || base > 31 || off < 0 || (off & 15) || off > 65520)
    return false;
  return a64_u32(code, 0x3DC00000u | ((uint32_t)(off / 16) << 10) |
                           (base << 5) | reg);
}

static int a64_mach_param_count(const ny_mach_func_t *mach) {
  if (!mach || !mach->frame_slot_len || !mach->block_len)
    return 0;
  if (mach->param_count > 0)
    return mach->param_count <= mach->frame_slot_len
               ? (int)mach->param_count
               : -1;
  bool *written = calloc(mach->frame_slot_len, sizeof(bool));
  bool *param = calloc(mach->frame_slot_len, sizeof(bool));
  if (!written || !param) {
    free(written);
    free(param);
    return -1;
  }
  const ny_mach_block_t *entry = &mach->blocks[0];
  for (size_t n = 0; n < entry->inst_count; ++n) {
    const ny_mach_inst_t *in = &mach->insts[entry->first_inst + n];
    if (in->opcode == NY_MACH_LOAD &&
        in->src0.kind == NY_MACH_OPERAND_FRAME &&
        in->src0.as.frame_index < mach->frame_slot_len &&
        !written[in->src0.as.frame_index])
      param[in->src0.as.frame_index] = true;
    if (in->opcode == NY_MACH_STORE &&
        in->dst.kind == NY_MACH_OPERAND_FRAME &&
        in->dst.as.frame_index < mach->frame_slot_len)
      written[in->dst.as.frame_index] = true;
  }
  int count = 0;
  while ((size_t)count < mach->frame_slot_len && param[count])
    count++;
  free(written);
  free(param);
  return count;
}

static bool a64_mach_emit_param_spills(const ny_mach_func_t *mach,
                                       ny_obj_buf_t *code) {
  int count = a64_mach_param_count(mach);
  if (count < 0)
    return false;
  unsigned gp = 0, fp = 0, stack_off = 0;
  for (int i = 0; i < count; ++i) {
    ny_mach_operand_t slot = {.kind = NY_MACH_OPERAND_FRAME,
                              .as.frame_index = (uint32_t)i};
    int off = a64_slot_off(mach, &slot);
    ny_mach_type_t type = mach->frame_slots[i].type;
    if (a64_mach_type_v128(type)) {
      if (fp < 8) {
        if (!a64_store_q(code, fp++, off))
          return false;
      } else {
        stack_off = (stack_off + 15u) & ~15u;
        if (!a64_ldr_q_base(code, 16, 29, 16 + (int)stack_off) ||
            !a64_store_q(code, 16, off))
          return false;
        stack_off += 16;
      }
    } else if (type == NY_MACH_TYPE_F32 || type == NY_MACH_TYPE_F64) {
      bool f32 = type == NY_MACH_TYPE_F32;
      if (fp < 8) {
        if (!a64_stur_fp(code, f32, fp++, off))
          return false;
      } else {
        stack_off = (stack_off + 7u) & ~7u;
        if (!a64_ldr_fp_base(code, f32, 16, 29, 16 + (int)stack_off) ||
            !a64_stur_fp(code, f32, 16, off))
          return false;
        stack_off += 8;
      }
    } else {
      if (gp < 8) {
        if (!a64_stur_x(code, gp++, off))
          return false;
      } else {
        stack_off = (stack_off + 7u) & ~7u;
        if (!a64_ldr_x_base(code, 16, 29, 16 + (int)stack_off) ||
            !a64_stur_x(code, 16, off))
          return false;
        stack_off += 8;
      }
    }
  }
  return true;
}

static bool a64_encode_func(const ny_mach_func_t *mach, ny_obj_buf_t *code,
                            size_t **block_off_out, bool user_function,
                            ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
                            size_t reloc_cap, size_t code_base,
                            const ny_native_target_info_t *target, char *err,
                            size_t err_len) {
  if (!mach || !code) return false;
  if (err && err_len)
    err[0] = '\0';
  a64_mach_fpr_state_t fpr_state = {0};
  if (a64_mach_fpr_prepare(mach, &fpr_state)) {
    bool fpr_ok = a64_mach_fpr_encode(mach, &fpr_state, code,
                                      user_function, err, err_len);
    size_t colored = 0;
    size_t spilled = 0;
    size_t reloads = 0;
    for (size_t i = 0; i < fpr_state.alloc.segment_len; ++i) {
      if (fpr_state.alloc.segments[i].color >= 0)
        ++colored;
      else
        ++spilled;
      if (fpr_state.alloc.segments[i].reload)
        ++reloads;
    }
    ny_native_mach_fpr_record(
        fpr_state.alloc.segment_len, colored, spilled, reloads,
        ny_mach_regalloc_peak_live(&fpr_state.alloc, mach->inst_len));
    free(fpr_state.seeded);
    ny_mach_regalloc_free(&fpr_state.alloc);
    return fpr_ok;
  }
  a64_mach_vector_state_t vector_state = {0};
  if (a64_mach_vector_prepare(mach, &vector_state)) {
    bool vector_ok = a64_mach_vector_encode(mach, &vector_state, code,
                                            user_function, relocs, reloc_count,
                                            reloc_cap, code_base, target,
                                            err, err_len);
    size_t colored = 0, spilled = 0, reloads = 0;
    for (size_t i = 0; i < vector_state.alloc.segment_len; ++i) {
      if (vector_state.alloc.segments[i].color >= 0)
        ++colored;
      else
        ++spilled;
      if (vector_state.alloc.segments[i].reload)
        ++reloads;
    }
    ny_native_mach_vector_record(
        vector_state.alloc.segment_len, colored, spilled, reloads,
        ny_mach_regalloc_peak_live(&vector_state.alloc, mach->inst_len));
    free(vector_state.seeded);
    ny_mach_regalloc_free(&vector_state.alloc);
    return vector_ok;
  }
  ny_mach_regalloc_t scalar_alloc = {0};
  if (a64_mach_scalar_regalloc_prepare(mach, &scalar_alloc)) {
    bool scalar_ok = a64_mach_scalar_regalloc_encode(
        mach, &scalar_alloc, code, user_function, err, err_len);
    size_t colored = 0, spilled = 0, reloads = 0;
    for (size_t i = 0; i < scalar_alloc.segment_len; ++i) {
      if (scalar_alloc.segments[i].color >= 0)
        ++colored;
      else
        ++spilled;
      if (scalar_alloc.segments[i].reload)
        ++reloads;
    }
    ny_native_mach_regalloc_record(
        scalar_alloc.segment_len, colored, spilled, reloads,
        ny_mach_regalloc_peak_live(&scalar_alloc, mach->inst_len));
    ny_mach_regalloc_free(&scalar_alloc);
    return scalar_ok;
  }
  size_t current_inst = SIZE_MAX;
  unsigned current_opcode = 0;
  int frame = a64_mach_frame_size(mach);
  /*
   * stp x29,x30,[sp,#-16]!; mov x29,sp; sub sp,#frame
   */
  if (!a64_u32(code, 0xA9BF7BFD) || !a64_u32(code, 0x910003FD))
    return false;
  if (frame) {
    /*
     * sub sp, sp, #frame (imm12 must fit)
     */
    if (frame >= 4096) {
      if (err && err_len) snprintf(err, err_len, "a64 machine form: frame too large");
      return false;
    }
    uint32_t sub = 0xD10003FF | ((uint32_t)frame << 10);
    if (!a64_u32(code, sub)) return false;
  }
  if (user_function && !a64_mach_emit_param_spills(mach, code)) {
    if (err && err_len)
      snprintf(err, err_len, "parameter spill failed");
    return false;
  }
  size_t *block_off = calloc(mach->block_len ? mach->block_len : 1, sizeof(size_t));
  if (!block_off && mach->block_len) return false;
  size_t *patch_at = NULL, *patch_blk = NULL, npatch = 0, pcap = 0;

  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    block_off[bi] = code->len;
    const ny_mach_block_t *blk = &mach->blocks[bi];
    for (size_t n = 0; n < blk->inst_count; ++n) {
      current_inst = blk->first_inst + n;
      const ny_mach_inst_t *in = &mach->insts[current_inst];
      current_opcode = (unsigned)in->opcode;
      int dst = a64_slot_off(mach, &in->dst);
      int a = a64_slot_off(mach, &in->src0);
      int b = a64_slot_off(mach, &in->src1);
      switch (in->opcode) {
      case NY_MACH_COPY: {
        if (a64_is_v128(mach, &in->dst)) {
          ny_mach_type_t dt = a64_operand_type(mach, &in->dst);
          if (a64_is_v128(mach, &in->src0)) {
            if (!a64_load_q(code, 0, a) || !a64_store_q(code, 0, dst))
              goto fail;
          } else if (dt == NY_MACH_TYPE_V128_I64) {
            if (in->src0.kind == NY_MACH_OPERAND_IMM) {
              if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
                goto fail;
            } else if (!a64_ldur_x(code, 0, a)) {
              goto fail;
            }
            if (!a64_u32(code, 0x4E080C00u) ||
                !a64_store_q(code, 0, dst))
              goto fail;
          } else if (dt == NY_MACH_TYPE_V128_F64) {
            if (in->src0.kind == NY_MACH_OPERAND_IMM ||
                !a64_ldur_fp(code, false, 0, a) ||
                !a64_u32(code, 0x4E080400u) ||
                !a64_store_q(code, 0, dst))
              goto fail;
          } else if (dt == NY_MACH_TYPE_V128_F32) {
            if (in->src0.kind == NY_MACH_OPERAND_IMM ||
                !a64_ldur_fp(code, true, 0, a) ||
                !a64_u32(code, 0x4E040400u) ||
                !a64_store_q(code, 0, dst))
              goto fail;
          } else {
            goto fail;
          }
          break;
        }
        bool is_f64 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F64;
        bool is_f32 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F32;
        if (is_f64 || is_f32) {
          if (in->src0.kind == NY_MACH_OPERAND_IMM) {
            if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm) ||
                !a64_u32(code, is_f32 ? 0x1E270000u : 0x9E670000u))
              goto fail;
          } else if (!a64_ldur_fp(code, is_f32, 0, a)) {
            goto fail;
          }
          if (
              !a64_stur_fp(code, is_f32, 0, dst))
            goto fail;
          break;
        }
        if (in->src0.kind == NY_MACH_OPERAND_IMM) {
          if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
            goto fail;
        } else if (!a64_ldur_x(code, 0, a)) {
          goto fail;
        }
        if (!a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NY_MACH_LEA: {
        /*
         * add x0, x29, #imm (imm must be non-neg; frame offs are neg — use sub)
         */
        int imm9 = a;
        if (imm9 >= 0 && imm9 <= 4095) {
          if (!a64_u32(code, 0x910003A0u | ((uint32_t)imm9 << 10)))
            goto fail;
        } else if (imm9 < 0 && imm9 >= -4095) {
          if (!a64_u32(code, 0xD10003A0u | ((uint32_t)(-imm9) << 10)))
            goto fail;
        } else
          goto fail;
        if (!a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NY_MACH_CONVERT: {
        bool dst_f64 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F64;
        bool dst_f32 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F32;
        bool src_f64 =
            in->src0.kind == NY_MACH_OPERAND_VREG && in->src0.as.reg < mach->vreg_len &&
            mach->vreg_types[in->src0.as.reg] == NY_MACH_TYPE_F64;
        bool src_f32 =
            in->src0.kind == NY_MACH_OPERAND_VREG && in->src0.as.reg < mach->vreg_len &&
            mach->vreg_types[in->src0.as.reg] == NY_MACH_TYPE_F32;
        if (dst_f64 && !src_f64 && !src_f32) {
          if (!a64_ldur_x(code, 0, a) || !a64_u32(code, 0x9E620000u) ||
              !a64_stur_fp(code, false, 0, dst))
            goto fail;
        } else if (dst_f32 && !src_f64 && !src_f32) {
          if (!a64_ldur_x(code, 0, a) || !a64_u32(code, 0x9E220000u) ||
              !a64_stur_fp(code, true, 0, dst))
            goto fail;
        } else if (!dst_f64 && !dst_f32 && src_f64) {
          if (!a64_ldur_fp(code, false, 0, a) ||
              !a64_u32(code, 0x9E780000u) || !a64_stur_x(code, 0, dst))
            goto fail;
        } else if (dst_f64 && src_f32) {
          if (!a64_ldur_fp(code, true, 0, a) ||
              !a64_u32(code, 0x1E22C000u) ||
              !a64_stur_fp(code, false, 0, dst))
            goto fail;
        } else if (dst_f32 && src_f64) {
          if (!a64_ldur_fp(code, false, 0, a) ||
              !a64_u32(code, 0x1E624000u) ||
              !a64_stur_fp(code, true, 0, dst))
            goto fail;
        } else {
          goto fail;
        }
        break;
      }
      case NY_MACH_LOAD:
        if (a64_is_v128(mach, &in->dst)) {
          if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
            if (!a64_load_q(code, 0, a))
              goto fail;
          } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
            if (!a64_ldur_x(code, 0, a) || !a64_q_ptr(code, true, 0, 0))
              goto fail;
          } else {
            goto fail;
          }
          if (!a64_store_q(code, 0, dst))
            goto fail;
          break;
        }
        if (a64_is_float(mach, &in->dst)) {
          bool f32 = a64_is_f32(mach, &in->dst);
          if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
            if (!a64_ldur_fp(code, f32, 0, a) || !a64_stur_fp(code, f32, 0, dst))
              goto fail;
          } else if (in->src0.kind == NY_MACH_OPERAND_VREG) {
            if (!a64_ldur_x(code, 0, a) || !a64_ldr_fp_base(code, f32, 0, 0, 0) ||
                !a64_stur_fp(code, f32, 0, dst))
              goto fail;
          } else {
            goto fail;
          }
        } else {
          if (in->src0.kind != NY_MACH_OPERAND_FRAME) goto fail;
          if (!a64_ldur_x(code, 0, a) || !a64_stur_x(code, 0, dst))
            goto fail;
        }
        break;
      case NY_MACH_STORE:
        if (a64_is_v128(mach, &in->src0)) {
          if (!a64_load_q(code, 1, a))
            goto fail;
          if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
            if (!a64_store_q(code, 1, dst))
              goto fail;
          } else if (in->dst.kind == NY_MACH_OPERAND_VREG) {
            if (!a64_ldur_x(code, 0, dst) || !a64_q_ptr(code, false, 1, 0))
              goto fail;
          } else {
            goto fail;
          }
          break;
        }
        if (a64_is_float(mach, &in->src0)) {
          bool f32 = a64_is_f32(mach, &in->src0);
          if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
            if (!a64_ldur_fp(code, f32, 0, a) || !a64_stur_fp(code, f32, 0, dst))
              goto fail;
          } else if (in->dst.kind == NY_MACH_OPERAND_VREG) {
            if (!a64_ldur_x(code, 1, dst) || !a64_ldur_fp(code, f32, 0, a) ||
                !a64_str_fp_base(code, f32, 0, 1, 0))
              goto fail;
          } else {
            goto fail;
          }
        } else {
          if (in->dst.kind != NY_MACH_OPERAND_FRAME) goto fail;
          if (!a64_ldur_x(code, 0, a) || !a64_stur_x(code, 0, dst))
            goto fail;
        }
        break;
      case NY_MACH_ADD:
      case NY_MACH_SUB:
      case NY_MACH_AND:
      case NY_MACH_OR:
      case NY_MACH_XOR:
      case NY_MACH_MUL: {
        if (a64_is_v128(mach, &in->dst)) {
          ny_mach_type_t vt = a64_operand_type(mach, &in->dst);
          uint32_t op = 0;
          if (vt == NY_MACH_TYPE_V128_I64) {
            if (in->opcode == NY_MACH_ADD) op = 0x4EE08400u;
            else if (in->opcode == NY_MACH_SUB) op = 0x6EE08400u;
            else if (in->opcode == NY_MACH_AND) op = 0x4E201C00u;
            else if (in->opcode == NY_MACH_OR) op = 0x4EA01C00u;
            else if (in->opcode == NY_MACH_XOR) op = 0x6E201C00u;
            else goto fail;
          } else if (vt == NY_MACH_TYPE_V128_F64) {
            if (in->opcode == NY_MACH_ADD) op = 0x4E60D400u;
            else if (in->opcode == NY_MACH_SUB) op = 0x4EE0D400u;
            else if (in->opcode == NY_MACH_MUL) op = 0x6E60DC00u;
            else goto fail;
          } else if (vt == NY_MACH_TYPE_V128_F32) {
            if (in->opcode == NY_MACH_ADD) op = 0x4E20D400u;
            else if (in->opcode == NY_MACH_SUB) op = 0x4EA0D400u;
            else if (in->opcode == NY_MACH_MUL) op = 0x6E20DC00u;
            else goto fail;
          } else {
            goto fail;
          }
          if (!a64_load_q(code, 1, a) || !a64_load_q(code, 2, b) ||
              !a64_u32(code, op | (2u << 16) | (1u << 5)) ||
              !a64_store_q(code, 0, dst))
            goto fail;
          break;
        }
        bool is_f64 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F64;
        bool is_f32 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F32;
        if (is_f64 || is_f32) {
          uint32_t falu = 0;
          unsigned top = is_f32 ? 0x1E20u : 0x1E60u;
          if (in->opcode == NY_MACH_ADD)
            falu = (top << 16) | 0x2800u;
          else if (in->opcode == NY_MACH_SUB)
            falu = (top << 16) | 0x3800u;
          else if (in->opcode == NY_MACH_MUL)
            falu = (top << 16) | 0x0800u;
          else if (in->opcode == NY_MACH_DIV)
            falu = (top << 16) | 0x1800u;
          else
            goto fail;
          if (!a64_ldur_fp(code, is_f32, 0, a) ||
              !a64_ldur_fp(code, is_f32, 1, b) ||
              !a64_u32(code, falu | (1u << 16)) ||
              !a64_stur_fp(code, is_f32, 0, dst))
            goto fail;
          break;
        }
        if (!a64_ldur_x(code, 0, a) || !a64_ldur_x(code, 1, b))
          goto fail;
        uint32_t alu = 0;
        if (in->opcode == NY_MACH_ADD)
          alu = 0x8B010000; /* add x0,x0,x1 */
        else if (in->opcode == NY_MACH_SUB)
          alu = 0xCB010000;
        else if (in->opcode == NY_MACH_AND)
          alu = 0x8A010000;
        else if (in->opcode == NY_MACH_OR)
          alu = 0xAA010000;
        else if (in->opcode == NY_MACH_XOR)
          alu = 0xCA010000;
        else
          alu = 0x9B017C00; /* mul x0,x0,x1 */
        if (!a64_u32(code, alu) || !a64_stur_x(code, 0, dst)) goto fail;
        break;
      }
      case NY_MACH_SHL:
      case NY_MACH_SAR: {
        if (a64_is_v128(mach, &in->dst)) {
          if (a64_operand_type(mach, &in->dst) != NY_MACH_TYPE_V128_I64 ||
              !a64_load_q(code, 1, a) || !a64_load_q(code, 2, b))
            goto fail;
          if (in->opcode == NY_MACH_SAR &&
              !a64_u32(code, 0x6EE0B842u))
            goto fail;
          if (!a64_u32(code, 0x4EE24420u) ||
              !a64_store_q(code, 0, dst))
            goto fail;
          break;
        }
        if (!a64_ldur_x(code, 0, a) || !a64_ldur_x(code, 1, b))
          goto fail;
        /*
         * lsl/asr x0, x0, x1
         */
        uint32_t sh = in->opcode == NY_MACH_SHL ? 0x9AC12000u : 0x9AC12800u;
        if (!a64_u32(code, sh))
          goto fail;
        if (!a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NY_MACH_DIV:
      case NY_MACH_MOD: {
        if (a64_is_v128(mach, &in->dst)) {
          ny_mach_type_t vt = a64_operand_type(mach, &in->dst);
          uint32_t op = vt == NY_MACH_TYPE_V128_F64 ? 0x6E60FC00u :
                        vt == NY_MACH_TYPE_V128_F32 ? 0x6E20FC00u : 0;
          if (in->opcode != NY_MACH_DIV || !op ||
              !a64_load_q(code, 1, a) || !a64_load_q(code, 2, b) ||
              !a64_u32(code, op | (2u << 16) | (1u << 5)) ||
              !a64_store_q(code, 0, dst))
            goto fail;
          break;
        }
        bool is_f64 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F64;
        bool is_f32 =
            in->dst.kind == NY_MACH_OPERAND_VREG && in->dst.as.reg < mach->vreg_len &&
            mach->vreg_types[in->dst.as.reg] == NY_MACH_TYPE_F32;
        if (is_f64 || is_f32) {
          if (in->opcode != NY_MACH_DIV ||
              !a64_ldur_fp(code, is_f32, 0, a) ||
              !a64_ldur_fp(code, is_f32, 1, b) ||
              !a64_u32(code, (is_f32 ? 0x1E211800u : 0x1E611800u) |
                                (1u << 16)) ||
              !a64_stur_fp(code, is_f32, 0, dst))
            goto fail;
          break;
        }
        if (!a64_ldur_x(code, 0, a) || !a64_ldur_x(code, 1, b))
          goto fail;
        if (in->opcode == NY_MACH_DIV) {
          /*
           * sdiv x0, x0, x1
           */
          if (!a64_u32(code, 0x9AC10C00u))
            goto fail;
        } else {
          /*
           * sdiv x2, x0, x1; msub x0, x2, x1, x0  (mod)
           */
          if (!a64_u32(code, 0x9AC10C02u) || !a64_u32(code, 0x9B018040u))
            goto fail;
        }
        if (!a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NY_MACH_FMA: {
        if (!a64_is_v128(mach, &in->dst))
          goto fail;
        ny_mach_type_t vt = a64_operand_type(mach, &in->dst);
        uint32_t op = vt == NY_MACH_TYPE_V128_F64 ? 0x4E60CC00u :
                      vt == NY_MACH_TYPE_V128_F32 ? 0x4E20CC00u : 0;
        int c_off = a64_slot_off(mach, &in->src2);
        if (!op || !a64_load_q(code, 0, c_off) ||
            !a64_load_q(code, 1, a) || !a64_load_q(code, 2, b) ||
            !a64_u32(code, op | (2u << 16) | (1u << 5)) ||
            !a64_store_q(code, 0, dst))
          goto fail;
        break;
      }
      case NY_MACH_SHUFFLE: {
        if (!a64_is_v128(mach, &in->dst) ||
            in->src1.kind != NY_MACH_OPERAND_IMM ||
            !a64_load_q(code, 1, a))
          goto fail;
        ny_mach_type_t vt = a64_operand_type(mach, &in->dst);
        unsigned lane_bytes = vt == NY_MACH_TYPE_V128_F64 ? 8u :
                              vt == NY_MACH_TYPE_V128_F32 ? 4u : 0u;
        unsigned lane_count = lane_bytes == 8 ? 2u : lane_bytes == 4 ? 4u : 0u;
        if (!lane_count)
          goto fail;
        uint64_t lo = a64_shuffle_half((unsigned)in->src1.as.imm, lane_bytes,
                                       0, lane_count);
        uint64_t hi = a64_shuffle_half((unsigned)in->src1.as.imm, lane_bytes,
                                       8 / lane_bytes, lane_count);
        if (!a64_mov_imm64(code, 0, lo) || !a64_mov_imm64(code, 1, hi) ||
            !a64_u32(code, 0x9E670002u) ||
            !a64_u32(code, 0x4E181C22u) ||
            !a64_u32(code, 0x4E020020u) ||
            !a64_store_q(code, 0, dst))
          goto fail;
        break;
      }
      case NY_MACH_CMP: {
        if (!a64_ldur_x(code, 0, a) || !a64_ldur_x(code, 1, b)) goto fail;
        if (!a64_u32(code, 0xEB01001F)) /* cmp x0,x1 */
          goto fail;
        /*
         * cset x0, cond
         */
        unsigned cond = 0xb; /* lt */
        switch (in->condition) {
        case NY_MACH_COND_EQ: cond = 0x0; break;
        case NY_MACH_COND_NE: cond = 0x1; break;
        case NY_MACH_COND_LT: cond = 0xb; break;
        case NY_MACH_COND_LE: cond = 0xd; break;
        case NY_MACH_COND_GT: cond = 0xc; break;
        case NY_MACH_COND_GE: cond = 0xa; break;
        default: goto fail;
        }
        uint32_t cset = 0x9A9F07E0u | ((cond ^ 1u) << 12);
        if (!a64_u32(code, cset)) goto fail;
        if (!a64_stur_x(code, 0, dst)) goto fail;
        break;
      }
      case NY_MACH_BR:
      case NY_MACH_BR_IF: {
        if (in->opcode == NY_MACH_BR_IF) {
          if (!a64_ldur_x(code, 0, a)) goto fail;
          if (!a64_u32(code, 0xB4000000)) /* cbz x0, +0 placeholder - use b.ne */
            goto fail;
          /*
           * Use cbnz x0, label: 0xB5000000 | imm19 | Rt
           * We'll emit b.ne with patch: actually emit unconditional b with
           * separate path — simplify: cbnz x0, target
           */
          code->len -= 4; /* undo */
          if (!a64_u32(code, 0xB5000000)) /* cbnz x0, #0 */
            goto fail;
        } else {
          if (!a64_u32(code, 0x14000000)) /* b #0 */
            goto fail;
        }
        if (npatch == pcap) {
          pcap = pcap ? pcap * 2 : 8;
          size_t *na = realloc(patch_at, pcap * sizeof(size_t));
          size_t *nb = realloc(patch_blk, pcap * sizeof(size_t));
          if (!na || !nb) { free(na); free(nb); goto fail; }
          patch_at = na; patch_blk = nb;
        }
        patch_at[npatch] = code->len - 4;
        patch_blk[npatch] = in->src1.as.block_index;
        npatch++;
        break;
      }
      case NY_MACH_CALL: {
        enum {
          MARG_GP,
          MARG_SRET,
          MARG_F32,
          MARG_F64,
          MARG_V128,
          MARG_AGG_GP,
          MARG_AGG_FP,
          MARG_HFA_F32,
          MARG_HFA_F64,
          MARG_HVA_V128,
          MARG_INDIRECT,
          MARG_STACK_GP,
          MARG_STACK_F32,
          MARG_STACK_F64,
          MARG_STACK_V128,
          MARG_STACK_AGG,
          MARG_STACK_PTR,
        };
        if (in->args_len > NYIR_CALL_MAX_ARGS)
          goto fail;
        unsigned kinds[NYIR_CALL_MAX_ARGS] = {0};
        unsigned regs_arg[NYIR_CALL_MAX_ARGS] = {0};
        unsigned chunks_arg[NYIR_CALL_MAX_ARGS] = {0};
        unsigned stack_arg[NYIR_CALL_MAX_ARGS] = {0};
        unsigned copy_arg[NYIR_CALL_MAX_ARGS] = {0};
        uint32_t sizes_arg[NYIR_CALL_MAX_ARGS] = {0};
        unsigned gp = 0, fp = 0, stack_used = 0;
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          if (ai == 0 && in->call_has_sret) {
            kinds[ai] = MARG_SRET;
            regs_arg[ai] = 8;
            continue;
          }
          uint32_t packed = in->arg_sizes ? in->arg_sizes[ai] : 0;
          if (packed) {
            uint32_t size = NYIR_ARG_AGG_SIZE(packed);
            unsigned c0 = NYIR_ARG_AGG_CLASS(packed, 0);
            unsigned c1 = NYIR_ARG_AGG_CLASS(packed, 1);
            unsigned need = size > 8 ? 2u : 1u;
            bool align16 = c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16;
            bool integer = size > 0 && size <= 16 &&
                           (c0 == NYIR_ARG_CLASS_INTEGER ||
                            c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
                            c0 == NYIR_ARG_CLASS_NONE) &&
                           (size <= 8 || c1 == NYIR_ARG_CLASS_INTEGER ||
                            c1 == NYIR_ARG_CLASS_NONE);
            bool floating = size > 0 && size <= 16 &&
                            c0 == NYIR_ARG_CLASS_SSE &&
                            (size <= 8 || c1 == NYIR_ARG_CLASS_SSE ||
                             c1 == NYIR_ARG_CLASS_NONE);
            sizes_arg[ai] = size;
            if (c0 == NYIR_ARG_CLASS_HFA_F32 ||
                c0 == NYIR_ARG_CLASS_HFA_F64 ||
                c0 == NYIR_ARG_CLASS_HVA_V128) {
              unsigned elem_size = c0 == NYIR_ARG_CLASS_HFA_F32 ? 4u :
                                   c0 == NYIR_ARG_CLASS_HFA_F64 ? 8u : 16u;
              unsigned count = elem_size && size % elem_size == 0
                                   ? size / elem_size
                                   : 0;
              chunks_arg[ai] = count;
              if (count >= 1 && count <= 4 && fp + count <= 8) {
                kinds[ai] = c0 == NYIR_ARG_CLASS_HFA_F32 ? MARG_HFA_F32 :
                            c0 == NYIR_ARG_CLASS_HFA_F64 ? MARG_HFA_F64 :
                                                            MARG_HVA_V128;
                regs_arg[ai] = fp;
                fp += count;
              } else {
                unsigned align = c0 == NYIR_ARG_CLASS_HVA_V128 ? 16u : 8u;
                fp = 8;
                stack_used = (stack_used + align - 1u) & ~(align - 1u);
                kinds[ai] = MARG_STACK_AGG;
                stack_arg[ai] = stack_used;
                stack_used += (size + 7u) & ~7u;
              }
              continue;
            }
            chunks_arg[ai] = need;
            if (integer) {
              if (align16 && (gp & 1u))
                ++gp;
              if (gp + need <= 8) {
                kinds[ai] = MARG_AGG_GP;
                regs_arg[ai] = gp;
                gp += need;
              } else {
                gp = 8;
                unsigned align = align16 ? 16u : 8u;
                stack_used = (stack_used + align - 1u) & ~(align - 1u);
                kinds[ai] = MARG_STACK_AGG;
                stack_arg[ai] = stack_used;
                stack_used += (size + 7u) & ~7u;
              }
            } else if (floating && fp + need <= 8) {
              kinds[ai] = MARG_AGG_FP;
              regs_arg[ai] = fp;
              fp += need;
            } else if (size > 16 || c0 == NYIR_ARG_CLASS_MEMORY ||
                       c0 == NYIR_ARG_CLASS_UNSUPPORTED) {
              if (gp < 8) {
                kinds[ai] = MARG_INDIRECT;
                regs_arg[ai] = gp++;
              } else {
                stack_used = (stack_used + 7u) & ~7u;
                kinds[ai] = MARG_STACK_PTR;
                stack_arg[ai] = stack_used;
                stack_used += 8;
              }
            } else {
              unsigned align = size > 8 ? 16u : 8u;
              stack_used = (stack_used + align - 1u) & ~(align - 1u);
              kinds[ai] = MARG_STACK_AGG;
              stack_arg[ai] = stack_used;
              stack_used += (size + 7u) & ~7u;
            }
            continue;
          }
          ny_mach_type_t type = a64_operand_type(mach, &in->args[ai]);
          if (a64_mach_type_v128(type)) {
            if (fp < 8) {
              kinds[ai] = MARG_V128;
              regs_arg[ai] = fp++;
            } else {
              stack_used = (stack_used + 15u) & ~15u;
              kinds[ai] = MARG_STACK_V128;
              stack_arg[ai] = stack_used;
              stack_used += 16;
            }
          } else if (type == NY_MACH_TYPE_F32 || type == NY_MACH_TYPE_F64) {
            if (fp < 8) {
              kinds[ai] = type == NY_MACH_TYPE_F32 ? MARG_F32 : MARG_F64;
              regs_arg[ai] = fp++;
            } else {
              stack_used = (stack_used + 7u) & ~7u;
              kinds[ai] = type == NY_MACH_TYPE_F32 ? MARG_STACK_F32
                                                   : MARG_STACK_F64;
              stack_arg[ai] = stack_used;
              stack_used += 8;
            }
          } else if (gp < 8) {
            kinds[ai] = MARG_GP;
            regs_arg[ai] = gp++;
          } else {
            stack_used = (stack_used + 7u) & ~7u;
            kinds[ai] = MARG_STACK_GP;
            stack_arg[ai] = stack_used;
            stack_used += 8;
          }
        }
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          if (kinds[ai] != MARG_INDIRECT && kinds[ai] != MARG_STACK_PTR)
            continue;
          stack_used = (stack_used + 15u) & ~15u;
          copy_arg[ai] = stack_used;
          stack_used += (sizes_arg[ai] + 7u) & ~7u;
        }
        unsigned stack_size = (stack_used + 15u) & ~15u;
        if (stack_size) {
          if (stack_size >= 4096 ||
              !a64_u32(code, 0xD10003FFu | (stack_size << 10)))
            goto fail;
        }
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          if (kinds[ai] != MARG_INDIRECT && kinds[ai] != MARG_STACK_PTR)
            continue;
          int off = a64_slot_off(mach, &in->args[ai]);
          if (!a64_ldur_x(code, 9, off))
            goto fail;
          for (uint32_t byte = 0; byte < sizes_arg[ai]; ++byte) {
            if (!a64_u32(code, 0x3940012Au | (byte << 10)) ||
                !a64_u32(code, 0x390003EAu |
                                  ((copy_arg[ai] + byte) << 10)))
              goto fail;
          }
        }
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          int off = a64_slot_off(mach, &in->args[ai]);
          switch (kinds[ai]) {
          case MARG_STACK_GP:
            if (!a64_ldur_x(code, 10, off) ||
                !a64_u32(code, 0xF90003EAu |
                                  ((uint32_t)(stack_arg[ai] / 8) << 10)))
              goto fail;
            break;
          case MARG_STACK_PTR:
            if (!a64_u32(code, 0x910003EAu | (copy_arg[ai] << 10)) ||
                !a64_u32(code, 0xF90003EAu |
                                  ((uint32_t)(stack_arg[ai] / 8) << 10)))
              goto fail;
            break;
          case MARG_STACK_F32:
            if (!a64_ldur_fp(code, true, 16, off) ||
                !a64_u32(code, 0xBD0003F0u |
                                  ((uint32_t)(stack_arg[ai] / 4) << 10)))
              goto fail;
            break;
          case MARG_STACK_F64:
            if (!a64_ldur_fp(code, false, 16, off) ||
                !a64_u32(code, 0xFD0003F0u |
                                  ((uint32_t)(stack_arg[ai] / 8) << 10)))
              goto fail;
            break;
          case MARG_STACK_V128:
            if (!a64_load_q(code, 16, off) ||
                !a64_u32(code, 0x3D8003F0u |
                                  ((uint32_t)(stack_arg[ai] / 16) << 10)))
              goto fail;
            break;
          case MARG_STACK_AGG:
            if (!a64_ldur_x(code, 9, off))
              goto fail;
            for (uint32_t byte = 0; byte < sizes_arg[ai]; ++byte) {
              if (!a64_u32(code, 0x3940012Au | (byte << 10)) ||
                  !a64_u32(code, 0x390003EAu |
                                    ((stack_arg[ai] + byte) << 10)))
                goto fail;
            }
            break;
          default:
            break;
          }
        }
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          int off = a64_slot_off(mach, &in->args[ai]);
          switch (kinds[ai]) {
          case MARG_GP:
          case MARG_SRET:
            if (!a64_ldur_x(code, regs_arg[ai], off))
              goto fail;
            break;
          case MARG_INDIRECT:
            if (!a64_u32(code, 0x910003E0u | (copy_arg[ai] << 10) |
                                  regs_arg[ai]))
              goto fail;
            break;
          case MARG_F32:
            if (!a64_ldur_fp(code, true, regs_arg[ai], off))
              goto fail;
            break;
          case MARG_F64:
            if (!a64_ldur_fp(code, false, regs_arg[ai], off))
              goto fail;
            break;
          case MARG_V128:
            if (!a64_load_q(code, regs_arg[ai], off))
              goto fail;
            break;
          case MARG_AGG_GP:
            if (!a64_ldur_x(code, 9, off))
              goto fail;
            for (unsigned ch = 0; ch < chunks_arg[ai]; ++ch)
              if (!a64_ldr_x_base(code, regs_arg[ai] + ch, 9,
                                  (int)(ch * 8)))
                goto fail;
            break;
          case MARG_AGG_FP:
            if (!a64_ldur_x(code, 9, off))
              goto fail;
            for (unsigned ch = 0; ch < chunks_arg[ai]; ++ch)
              if (!a64_ldr_fp_base(code, false, regs_arg[ai] + ch, 9,
                                   (int)(ch * 8)))
                goto fail;
            break;
          case MARG_HFA_F32:
          case MARG_HFA_F64:
          case MARG_HVA_V128:
            if (!a64_ldur_x(code, 9, off))
              goto fail;
            for (unsigned ch = 0; ch < chunks_arg[ai]; ++ch) {
              bool ok = kinds[ai] == MARG_HVA_V128
                            ? a64_ldr_q_base(code, regs_arg[ai] + ch, 9,
                                           (int)(ch * 16))
                            : a64_ldr_fp_base(
                                  code, kinds[ai] == MARG_HFA_F32,
                                  regs_arg[ai] + ch, 9,
                                  (int)(ch * (kinds[ai] == MARG_HFA_F32 ? 4 : 8)));
              if (!ok)
                goto fail;
            }
            break;
          default:
            break;
          }
        }
        const char *raw = in->src0.kind == NY_MACH_OPERAND_SYMBOL
                              ? in->src0.as.symbol
                              : NULL;
        if (!raw)
          goto fail;
        char call_sym[256];
        const char *pref =
            target && target->symbol_prefix ? target->symbol_prefix : "";
        if (in->call_is_extern || strncmp(raw, "rt_", 3) == 0 ||
            strncmp(raw, "ny_fn_", 6) == 0 || raw[0] == '_')
          snprintf(call_sym, sizeof(call_sym), "%s%s", pref, raw);
        else
          snprintf(call_sym, sizeof(call_sym), NY_FMT_FN, pref, raw);
        if (!a64_u32(code, 0x94000000u))
          goto fail;
        if (relocs && reloc_count && *reloc_count < reloc_cap) {
          snprintf(relocs[*reloc_count].symbol,
                   sizeof(relocs[*reloc_count].symbol), "%s", call_sym);
          relocs[*reloc_count].disp_off = code->len - 4;
          relocs[*reloc_count].type = NY_RELOC_AARCH64_CALL26;
          (*reloc_count)++;
        }
        if (stack_size &&
            !a64_u32(code, 0x910003FFu | (stack_size << 10)))
          goto fail;
        if (in->dst.kind == NY_MACH_OPERAND_VREG) {
          ny_mach_type_t type = a64_operand_type(mach, &in->dst);
          if (a64_mach_type_v128(type)) {
            if (!a64_store_q(code, 0, dst))
              goto fail;
          } else if (type == NY_MACH_TYPE_F32 || type == NY_MACH_TYPE_F64) {
            if (!a64_stur_fp(code, type == NY_MACH_TYPE_F32, 0, dst))
              goto fail;
          } else if (!a64_stur_x(code, 0, dst)) {
            goto fail;
          }
        }
        break;
      }
      case NY_MACH_RET:
        if (in->src0.kind != NY_MACH_OPERAND_NONE) {
          if (in->src0.kind == NY_MACH_OPERAND_IMM) {
            if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
              goto fail;
          } else {
            ny_mach_type_t type = a64_operand_type(mach, &in->src0);
            if (a64_mach_type_v128(type)) {
              if (!a64_load_q(code, 0, a))
                goto fail;
            } else if (type == NY_MACH_TYPE_F32 || type == NY_MACH_TYPE_F64) {
              if (!a64_ldur_fp(code, type == NY_MACH_TYPE_F32, 0, a))
                goto fail;
            } else if (!a64_ldur_x(code, 0, a)) {
              goto fail;
            }
          }
        }
        if (frame) {
          uint32_t add = 0x910003FF | ((uint32_t)frame << 10);
          if (!a64_u32(code, add)) goto fail;
        }
        if (!a64_u32(code, 0xA8C17BFD) || !a64_ret(code)) /* ldp x29,x30,[sp],#16; ret */
          goto fail;
        break;
      case NY_MACH_NOP:
        break;
      case NY_MACH_INLINE_ASM: {
        /*
         * Encode only templates with a defined AArch64 lowering.
         */
        if (in->src0.kind != NY_MACH_OPERAND_SYMBOL || !in->src0.as.symbol) {
          if (err && err_len)
            snprintf(err, err_len, "inline asm needs template");
          goto fail;
        }
        const char *t = in->src0.as.symbol;
        if (strcmp(t, "yield") == 0) {
          if (!a64_u32(code, 0xD503203Fu)) goto fail;
        } else if (strcmp(t, "wfe") == 0) {
          if (!a64_u32(code, 0xD503205Fu)) goto fail;
        } else if (strcmp(t, "wfi") == 0) {
          if (!a64_u32(code, 0xD503207Fu)) goto fail;
        } else if (strcmp(t, "sev") == 0) {
          if (!a64_u32(code, 0xD503209Fu)) goto fail;
        } else if (strcmp(t, "isb") == 0 || strcmp(t, "isb sy") == 0) {
          if (!a64_u32(code, 0xD5033FDFu)) goto fail;
        } else if (strcmp(t, "dmb") == 0 || strcmp(t, "dmb sy") == 0) {
          if (!a64_u32(code, 0xD5033FBFu)) goto fail;
        } else if (strcmp(t, "dmb ish") == 0) {
          if (!a64_u32(code, 0xD5033BBFu)) goto fail;
        } else if (strcmp(t, "dsb") == 0 || strcmp(t, "dsb sy") == 0) {
          if (!a64_u32(code, 0xD5033F9Fu)) goto fail;
        } else if (strcmp(t, "dsb ish") == 0) {
          if (!a64_u32(code, 0xD5033B9Fu)) goto fail;
        } else if (strncmp(t, "nop", 3) == 0) {
          bool emitted = false;
          for (const char *p = t; *p; ++p) {
            if ((p == t || p[-1] == ';') &&
                (p[0] == 'n' || p[0] == 'N')) {
              if (!a64_u32(code, 0xD503201Fu)) goto fail;
              emitted = true;
            }
          }
          if (!emitted) {
            if (err && err_len)
              snprintf(err, err_len, "unsupported inline asm template");
            goto fail;
          }
        } else {
          if (err && err_len)
            snprintf(err, err_len, "unsupported inline asm template '%s'", t);
          goto fail;
        }
        break;
      }
      case NY_MACH_INTRINSIC: {
        /*
         * CAPTURE_RET selectors: 0=x1, 1=x0, 2=d0, 3=d1,
         * 4..7=s0..s3, 8=d2, 9=d3, 10..13=q0..q3.
         */
        if (in->src0.kind == NY_MACH_OPERAND_IMM) {
          unsigned sel = (unsigned)(in->src0.as.imm & 0xff);
          if (in->dst.kind == NY_MACH_OPERAND_VREG) {
            int doff = a64_slot_off(mach, &in->dst);
            unsigned src_reg = 0;
            if (sel == 0) src_reg = 1;
            else if (sel == 1) src_reg = 0;
            else if (sel == 2 || sel == 3) {
              unsigned v = sel - 2;
              if (!a64_u32(code, 0x9E660000u | (v << 5))) goto fail;
            } else if (sel >= 4 && sel <= 7) {
              unsigned v = sel - 4;
              if (!a64_u32(code, 0x1E260000u | (v << 5))) goto fail;
            } else if (sel == 8 || sel == 9) {
              unsigned v = sel - 6;
              if (!a64_u32(code, 0x9E660000u | (v << 5))) goto fail;
            } else if (sel >= 10 && sel <= 13) {
              if (!a64_store_q(code, sel - 10, doff)) goto fail;
              break;
            } else {
              if (err && err_len)
                snprintf(err, err_len, "unsupported capture.ret selector %u", sel);
              goto fail;
            }
            if (!a64_stur_x(code, src_reg, doff)) goto fail;
          }
          break;
        }
        if (err && err_len)
          snprintf(err, err_len, "unsupported machine intrinsic shape");
        goto fail;
      }
      default:
        goto fail;
      }
    }
  }
  for (size_t i = 0; i < npatch; ++i) {
    size_t at = patch_at[i];
    size_t blk = patch_blk[i];
    if (blk >= mach->block_len) goto fail;
    int64_t rel = (int64_t)block_off[blk] - (int64_t)at;
    int64_t imm = rel / 4;
    uint32_t *w = (uint32_t *)(code->data + at);
    uint32_t op = *w;
    if ((op & 0xFF000000u) == 0x14000000u) {
      /*
       * b imm26
       */
      *w = 0x14000000u | ((uint32_t)imm & 0x03ffffffu);
    } else if ((op & 0xFF000000u) == 0xB5000000u) {
      /*
       * cbnz Rt, imm19
       */
      *w = (op & 0xFF00001Fu) | (((uint32_t)imm & 0x7ffffu) << 5);
    } else
      goto fail;
  }
  free(patch_at);
  free(patch_blk);
  if (block_off_out)
    *block_off_out = block_off;
  else
    free(block_off);
  return true;
fail:
  if (err && err_len && !err[0])
    snprintf(err, err_len, "instruction %zu opcode=%u encode failed",
             current_inst, current_opcode);
  free(patch_at);
  free(patch_blk);
  free(block_off);
  return false;
}

bool ny_a64_mach_build_bundle(
    const ny_mach_func_t *rt_main_mir, const ny_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len) {
  (void)tag_return;
  if (!rt_main_mir || !target || !code || !defs || !def_count || !reloc_count ||
      !entry_symbol) {
    if (err && err_len)
      snprintf(err, err_len, "a64 machine form: missing input");
    return false;
  }
  *def_count = 0;
  *reloc_count = 0;
  code->len = 0;
  size_t reloc_cap = NY_X64_OBJ_MAX_RELOCS;

  /*
   * Fast path: pure const return.
   */
  int64_t cval = 0;
  if (a64_try_const_ret(rt_main_mir, &cval) && func_count == 0) {
    size_t start = code->len;
    if (!a64_mov_imm64(code, 0, (uint64_t)cval) || !a64_ret(code))
      return false;
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             entry_symbol);
    defs[*def_count].off = start;
    defs[*def_count].size = code->len - start;
    (*def_count)++;
    return true;
  }

  for (size_t i = 0; i < func_count; ++i) {
    const char *nm = func_names && func_names[i] ? func_names[i] : "fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", nm);
    size_t start = code->len;
    char ebuf[128];
    if (!a64_encode_func(&func_mirs[i], code, NULL, true, relocs, reloc_count,
                         reloc_cap, start, target, ebuf, sizeof(ebuf))) {
      if (err && err_len)
        snprintf(err, err_len, "a64 machine form: %s", ebuf[0] ? ebuf : "encode fail");
      return false;
    }
    if (*def_count >= NY_NATIVE_MAX_DEFS) return false;
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", symbol);
    defs[*def_count].off = start;
    defs[*def_count].size = code->len - start;
    (*def_count)++;
  }
  {
    size_t start = code->len;
    char ebuf[128] = {0};
    if (!a64_encode_func(rt_main_mir, code, NULL, false, relocs, reloc_count,
                         reloc_cap, start, target, ebuf, sizeof(ebuf))) {
      if (err && err_len)
        snprintf(err, err_len, "a64 machine form: %s", ebuf[0] ? ebuf : "encode fail");
      return false;
    }
    if (*def_count >= NY_NATIVE_MAX_DEFS) return false;
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             entry_symbol);
    defs[*def_count].off = start;
    defs[*def_count].size = code->len - start;
    (*def_count)++;
  }
  return true;
}
