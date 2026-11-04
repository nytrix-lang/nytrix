/*
 * AArch64 object encoder: machine-form -> A64 instruction encoding
 * for the ARM 64-bit native backend with FPR and NEON support.
 * Target-specific object scaffolding stays local to preserve the AArch64
 * relocation and instruction-layout invariants.
 */
#include "base/debug.h"
#include "base/parallel.h"
#include "code/backend/abi.h"
#include "code/backend/bare/payload.h"
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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
  unsigned type;
} ny_a64_reloc_t;

typedef struct {
  char name[256];
  size_t off;
  size_t size;
} ny_a64_def_t;

typedef struct {
  nyr_obj_buf_t code;
  const nyr_func_t *nyir;
  const ny_native_target_info_t *target;
  int value_slots;
  int local_slots;
  int local_base;
  int frame_bytes;
  int runtime_scratch_off;
  size_t bare_payload_entry; /* code offset + 1 */
  int *value_offs;
  int *local_offs;
  int *alloca_offs;
  int *agg_home_offs;
  nyr_type_map_t types;
  ny_a64_label_t inline_labels[1024];
  ny_a64_label_t *labels;
  size_t label_cap;
  size_t label_count;
  ny_a64_patch_t inline_patches[1024];
  ny_a64_patch_t *patches;
  size_t patch_cap;
  size_t patch_count;
  size_t inline_returns[1024];
  size_t *returns;
  size_t return_cap;
  size_t return_count;
  ny_a64_reloc_t *relocs;
  size_t reloc_count;
  size_t reloc_cap;
  char *err;
  size_t err_len;
} ny_a64_obj_ctx_t;

static bool ny_a64_debug_line_push(nyr_native_debug_line_t **lines,
                                   size_t *count, size_t *cap,
                                   nyr_native_debug_line_t line);
static bool ny_a64_mach_build_bundle_debug(
    const nyr_mach_func_t *rt_main_mir, const nyr_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool rt_main_user_function, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_func_t *rt_main_source, const nyr_func_t *func_sources,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len);

/*
 * Relocations use a geometrically growing transport buffer. The caller still
 * owns the total bundle budget and allocation failures remain fatal.
 */
static bool ny_a64_reserve_relocs(ny_a64_reloc_t **data, size_t *cap,
                                  size_t want, char *err, size_t err_len) {
  if (!data || !cap) {
    nyr_set_error(err, err_len,
                  "AArch64 ELF object writer: invalid relocation buffer");
    return false;
  }
  if (want <= *cap)
    return true;
  size_t next = *cap ? *cap : 256;
  while (next < want) {
    if (next > SIZE_MAX / 2) {
      next = want;
      break;
    }
    next *= 2;
  }
  if (next > SIZE_MAX / sizeof(**data)) {
    nyr_set_error(err, err_len,
                  "AArch64 ELF object writer: relocation buffer too large");
    return false;
  }
  ny_a64_reloc_t *grown = realloc(*data, next * sizeof(*grown));
  if (!grown) {
    nyr_set_error(err, err_len,
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
  if (!nyr_obj_u32(&c->code, word)) {
    nyr_set_error(c->err, c->err_len, "AArch64 object writer: out of memory");
    return false;
  }
  return true;
}

/*
 * ADD/SUB (immediate) has a 12-bit immediate. Split large frame adjustments
 * into encodable chunks while keeping all locals anchored at x29.
 */
static bool ny_a64_adjust_sp(ny_a64_obj_ctx_t *c, bool subtract,
                             unsigned amount) {
  const uint32_t base = subtract ? 0xd10003ffu : 0x910003ffu;
  while (amount > 4095u) {
    if (!ny_a64_u32(c, base | (4095u << 10)))
      return false;
    amount -= 4095u;
  }
  return amount == 0 || ny_a64_u32(c, base | (amount << 10));
}

static bool ny_a64_reg_mem_base(ny_a64_obj_ctx_t *c, bool load, unsigned reg,
                                unsigned base, int off) {
  if (reg > 30 || base > 31 || off < 0 || (off & 7) != 0 ||
      (off > 32760 && base != 31)) {
    nyr_set_error(
        c->err, c->err_len,
        "AArch64 object writer: invalid memory access reg=%u base=%u off=%d",
        reg, base, off);
    return false;
  }
  if (off > 32760) {
    unsigned address = reg < 16 ? 16u : reg == 16 ? 17u : 16u;
    if (!ny_a64_u32(c, 0x910003e0u | address))
      return false;
    if (reg < 16) {
      if (!ny_a64_mov_imm(c, 17, off) ||
          !ny_a64_u32(c, 0x8b000000u | (17u << 16) | (16u << 5) | 16u))
        return false;
    } else {
      unsigned remaining = (unsigned)off;
      while (remaining) {
        unsigned chunk = remaining > 4095u ? 4095u : remaining;
        if (!ny_a64_u32(c,
                        0x91000000u | (chunk << 10) | (address << 5) | address))
          return false;
        remaining -= chunk;
      }
    }
    base = address;
    off = 0;
  }
  uint32_t op = load ? 0xf9400000u : 0xf9000000u;
  return ny_a64_u32(c, op | ((uint32_t)(off / 8) << 10) | ((base & 31u) << 5) |
                           reg);
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

/*
 * Four-lane f64 values occupy 32-byte, 16-aligned homes as two Q halves.
 */
static bool ny_a64_value_is_v256(const ny_a64_obj_ctx_t *c, int value) {
  return c && value >= 0 && value < c->value_slots && c->types.value_v256_f64 &&
         c->types.value_v256_f64[value];
}

static bool ny_a64_local_is_v256(const ny_a64_obj_ctx_t *c, int local) {
  return c && local >= 0 && local < c->local_slots && c->types.local_v256_f64 &&
         c->types.local_v256_f64[local];
}

static bool ny_a64_q_mem_base(ny_a64_obj_ctx_t *c, bool load, unsigned reg,
                              unsigned base, int off) {
  if (reg > 31 || base > 31 || off < 0 || off > 65520 || (off & 15) != 0) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: invalid vector memory access "
                  "reg=%u base=%u off=%d",
                  reg, base, off);
    return false;
  }
  uint32_t op = load ? 0x3dc00000u : 0x3d800000u;
  return ny_a64_u32(c, op | ((uint32_t)(off / 16) << 10) | ((base & 31u) << 5) |
                           reg);
}

static bool ny_a64_q_mem(ny_a64_obj_ctx_t *c, bool load, unsigned reg,
                         int off) {
  return ny_a64_q_mem_base(c, load, reg, 31, off);
}

static bool ny_a64_check_value(ny_a64_obj_ctx_t *c, int value,
                               const char *role) {
  if (value >= 0 && value < c->value_slots)
    return true;
  nyr_set_error(c->err, c->err_len,
                "AArch64 object writer: invalid %s value v%d", role, value);
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
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: invalid local slot %d", local);
    return false;
  }
  return ny_a64_reg_mem(c, true, reg, ny_a64_local_off(c, local));
}

static bool ny_a64_store_local(ny_a64_obj_ctx_t *c, int local, unsigned reg) {
  if (local < 0 || local >= c->local_slots) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: invalid local slot %d", local);
    return false;
  }
  return ny_a64_reg_mem(c, false, reg, ny_a64_local_off(c, local));
}

static bool ny_a64_fp_mem_base(ny_a64_obj_ctx_t *c, bool load, bool f32,
                               unsigned reg, unsigned base, int off) {
  unsigned scale = f32 ? 4u : 8u;
  if (reg > 31 || base > 31 || off < 0 || (off % (int)scale) != 0 ||
      off / (int)scale > 4095)
    return false;
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

static bool ny_a64_store_fp_value(ny_a64_obj_ctx_t *c, int value, unsigned reg,
                                  bool f32) {
  return ny_a64_check_value(c, value, "floating destination") &&
         ny_a64_fp_mem(c, false, f32, reg, ny_a64_value_off(c, value));
}

static bool ny_a64_load_vec_value(ny_a64_obj_ctx_t *c, unsigned reg,
                                  int value) {
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

static bool ny_a64_load_vec4_half(ny_a64_obj_ctx_t *c, unsigned reg, int value,
                                  unsigned half) {
  return ny_a64_check_value(c, value, "vector source") &&
         ny_a64_value_is_v256(c, value) &&
         ny_a64_q_mem(c, true, reg,
                      ny_a64_value_off(c, value) + (int)(half * 16u));
}

static bool ny_a64_store_vec4_half(ny_a64_obj_ctx_t *c, int value, unsigned reg,
                                   unsigned half) {
  return ny_a64_check_value(c, value, "vector destination") &&
         ny_a64_value_is_v256(c, value) &&
         ny_a64_q_mem(c, false, reg,
                      ny_a64_value_off(c, value) + (int)(half * 16u));
}

/*
 * Four-lane f64 op: apply one 2d NEON instruction per half. `op` must carry
 * Rn=Rm=Rd=0; the caller's lane arguments select the half registers.
 */
static bool ny_a64_vec4_binop(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                              uint32_t op) {
  return ny_a64_load_vec4_half(c, 0, in->a, 0) &&
         ny_a64_load_vec4_half(c, 1, in->a, 1) &&
         ny_a64_load_vec4_half(c, 2, in->b, 0) &&
         ny_a64_load_vec4_half(c, 3, in->b, 1) &&
         ny_a64_u32(c, op | (2u << 16) | (0u << 5) | 0u) &&
         ny_a64_u32(c, op | (3u << 16) | (1u << 5) | 1u) &&
         ny_a64_store_vec4_half(c, in->dst, 0, 0) &&
         ny_a64_store_vec4_half(c, in->dst, 1, 1);
}

/*
 * Four-lane f64 FMA: c + a*b per lane as two fused fmla operations
 * (NEON fmla is always single-rounding).
 */
static bool ny_a64_vec4_fma(ny_a64_obj_ctx_t *c, const nyr_inst_t *in) {
  return ny_a64_load_vec4_half(c, 0, in->a, 0) &&
         ny_a64_load_vec4_half(c, 1, in->a, 1) &&
         ny_a64_load_vec4_half(c, 2, in->b, 0) &&
         ny_a64_load_vec4_half(c, 3, in->b, 1) &&
         ny_a64_load_vec4_half(c, 4, in->c, 0) &&
         ny_a64_load_vec4_half(c, 5, in->c, 1) &&
         ny_a64_u32(c, 0x4e60cc00u | (4u << 16) | (2u << 5) | 0u) &&
         ny_a64_u32(c, 0x4e60cc00u | (5u << 16) | (3u << 5) | 1u) &&
         ny_a64_store_vec4_half(c, in->dst, 0, 0) &&
         ny_a64_store_vec4_half(c, in->dst, 1, 1);
}

/*
 * Four-lane f64 shuffle: dst lane i takes source lane (imm >> 2i) & 3.
 * The source halves form a 32-byte TBL table; each destination half is
 * looked up through an index vector assembled from four immediate words.
 */
static uint64_t ny_a64_vec4_shuffle_index(unsigned imm, unsigned lane) {
  uint64_t base = (uint64_t)(((imm >> (lane * 2u)) & 3u) * 8u);
  uint64_t bits = 0;
  for (unsigned byte = 0; byte < 8; ++byte)
    bits |= base << (byte * 8u);
  return bits;
}

static bool ny_a64_vec4_shuffle(ny_a64_obj_ctx_t *c, const nyr_inst_t *in) {
  unsigned imm = (unsigned)in->imm;
  if (!ny_a64_load_vec4_half(c, 1, in->a, 0) ||
      !ny_a64_load_vec4_half(c, 2, in->a, 1))
    return false;
  for (unsigned chunk = 0; chunk < 4; ++chunk)
    if (!ny_a64_mov_imm(c, chunk,
                        (int64_t)ny_a64_vec4_shuffle_index(imm, chunk)))
      return false;
  return ny_a64_u32(c, 0x9e670003u) && /* fmov d3, x0 */
         ny_a64_u32(c, 0x4e181c23u) && /* ins v3.d[1], x1 */
         ny_a64_u32(c, 0x9e670004u) && /* fmov d4, x2 */
         ny_a64_u32(c, 0x4e181c24u) && /* ins v4.d[1], x3 */
         ny_a64_u32(c,
                    0x4e034020u) && /* tbl v0.16b, {v1.16b, v2.16b}, v3.16b */
         ny_a64_u32(c,
                    0x4e044025u) && /* tbl v5.16b, {v1.16b, v2.16b}, v4.16b */
         ny_a64_store_vec4_half(c, in->dst, 0, 0) &&
         ny_a64_store_vec4_half(c, in->dst, 5, 1);
}

/*
 * Four-lane f64 reduction: start + lane0 + lane1 + lane2 + lane3 as two
 * pairwise faddp operations and two scalar fadds.
 */
static bool ny_a64_vec4_reduce_add(ny_a64_obj_ctx_t *c, const nyr_inst_t *in) {
  return ny_a64_load_vec4_half(c, 0, in->b, 0) &&
         ny_a64_load_vec4_half(c, 1, in->b, 1) &&
         ny_a64_u32(c, 0x7e70d802u) && /* faddp d2, v0.2d */
         ny_a64_u32(c, 0x7e70d823u) && /* faddp d3, v1.2d */
         ny_a64_u32(c, 0x1e632842u) && /* fadd d2, d2, d3 */
         ny_a64_load_fp_value(c, 1, in->a, false) &&
         ny_a64_u32(c, 0x1e612842u) && /* fadd d2, d2, d1 */
         ny_a64_store_fp_value(c, in->dst, 2, false);
}

static bool ny_a64_vec_binop(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                             uint32_t op) {
  return ny_a64_load_vec_value(c, 1, in->a) &&
         ny_a64_load_vec_value(c, 2, in->b) &&
         ny_a64_u32(c, op | (2u << 16) | (1u << 5)) &&
         ny_a64_store_vec_value(c, in->dst, 0);
}

static bool ny_a64_vec_fma(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
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
    unsigned src_lane =
        (imm >> (out_lane * (lane_count == 2 ? 1u : 2u))) & (lane_count - 1u);
    unsigned src_byte = src_lane * lane_bytes + byte % lane_bytes;
    bits |= (uint64_t)src_byte << (byte * 8);
  }
  return bits;
}

static bool ny_a64_vec_shuffle(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                               unsigned lane_bytes, unsigned lane_count) {
  unsigned imm = (unsigned)in->imm;
  uint64_t lo = ny_a64_shuffle_index_half(imm, lane_bytes, 0, lane_count);
  uint64_t hi =
      ny_a64_shuffle_index_half(imm, lane_bytes, 8 / lane_bytes, lane_count);
  return ny_a64_load_vec_value(c, 1, in->a) &&
         ny_a64_mov_imm(c, 0, (int64_t)lo) &&
         ny_a64_mov_imm(c, 1, (int64_t)hi) &&
         ny_a64_u32(c, 0x9e670002u) && /* fmov d2,x0 */
         ny_a64_u32(c, 0x4e181c22u) && /* ins v2.d[1],x1 */
         ny_a64_u32(c, 0x4e020020u) && /* tbl v0.16b,{v1.16b},v2.16b */
         ny_a64_store_vec_value(c, in->dst, 0);
}

static bool ny_a64_fp_binop(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
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
    if (part && !ny_a64_u32(c, 0xf2800000u | ((shift / 16u) << 21) |
                                   ((uint32_t)part << 5) | reg))
      return false;
  }
  return true;
}

static bool ny_a64_binop(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                         uint32_t op) {
  return ny_a64_load_value(c, 0, in->a) && ny_a64_load_value(c, 1, in->b) &&
         ny_a64_u32(c, op | (1u << 16)) && ny_a64_store_value(c, in->dst, 0);
}

static unsigned ny_a64_cond(nyr_cmp_t cmp) {
  static const unsigned conds[] = {0, 1, 11, 13, 12, 10};
  return (unsigned)cmp < sizeof(conds) / sizeof(conds[0]) ? conds[cmp] : 0;
}

static bool ny_a64_add_label(ny_a64_obj_ctx_t *c, int64_t label) {
  if (c->label_count >= c->label_cap) {
    nyr_set_error(c->err, c->err_len, "AArch64 object writer: too many labels");
    return false;
  }
  c->labels[c->label_count++] =
      (ny_a64_label_t){.label = label, .off = c->code.len};
  return true;
}

static bool ny_a64_add_patch(ny_a64_obj_ctx_t *c, int64_t label,
                             bool conditional) {
  if (c->patch_count >= c->patch_cap) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: too many branch patches");
    return false;
  }
  size_t off = c->code.len;
  c->patches[c->patch_count++] =
      (ny_a64_patch_t){.label = label, .off = off, .conditional = conditional};
  return ny_a64_u32(c, conditional ? 0x54000001u : 0x14000000u);
}

static void ny_a64_control_flow_free(ny_a64_obj_ctx_t *c) {
  if (c->labels != c->inline_labels)
    free(c->labels);
  if (c->patches != c->inline_patches)
    free(c->patches);
  if (c->returns != c->inline_returns)
    free(c->returns);
}

static bool ny_a64_control_flow_init(ny_a64_obj_ctx_t *c) {
  c->labels = c->inline_labels;
  c->patches = c->inline_patches;
  c->returns = c->inline_returns;
  c->label_cap = sizeof(c->inline_labels) / sizeof(c->inline_labels[0]);
  c->patch_cap = sizeof(c->inline_patches) / sizeof(c->inline_patches[0]);
  c->return_cap = sizeof(c->inline_returns) / sizeof(c->inline_returns[0]);
  if (!c->nyir || c->nyir->len <= c->label_cap)
    return true;

  size_t cap = c->nyir->len;
  c->labels = calloc(cap, sizeof(*c->labels));
  c->patches = calloc(cap, sizeof(*c->patches));
  c->returns = calloc(cap, sizeof(*c->returns));
  if (!c->labels || !c->patches || !c->returns) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: control-flow allocation failed");
    ny_a64_control_flow_free(c);
    c->labels = NULL;
    c->patches = NULL;
    c->returns = NULL;
    return false;
  }
  c->label_cap = cap;
  c->patch_cap = cap;
  c->return_cap = cap;
  return true;
}

static bool ny_a64_patch_branch(ny_a64_obj_ctx_t *c, size_t off, size_t target,
                                bool conditional) {
  int64_t delta = (int64_t)target - (int64_t)off;
  if ((delta & 3) != 0)
    return false;
  int64_t words = delta / 4;
  uint32_t insn = 0;
  memcpy(&insn, c->code.data + off, sizeof(insn));
  if (conditional) {
    if (words < -(1 << 18) || words >= (1 << 18))
      return false;
    insn = (insn & ~0x00ffffe0u) | (((uint32_t)words & 0x7ffffu) << 5);
  } else {
    if (words < -(1 << 25) || words >= (1 << 25))
      return false;
    insn = (insn & 0xfc000000u) | ((uint32_t)words & 0x03ffffffu);
  }
  nyr_obj_patch_u32(&c->code, off, insn);
  return true;
}

static bool ny_a64_record_reloc(ny_a64_obj_ctx_t *c, const char *symbol,
                                unsigned type) {
  if (!symbol || !*symbol ||
      !ny_a64_reserve_relocs(&c->relocs, &c->reloc_cap, c->reloc_count + 1,
                             c->err, c->err_len)) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: invalid or excessive relocation");
    return false;
  }
  ny_a64_reloc_t *r = &c->relocs[c->reloc_count++];
  snprintf(r->symbol, sizeof(r->symbol), "%s", symbol);
  r->off = c->code.len;
  r->type = type;
  return true;
}

static bool ny_a64_add_reloc_type(ny_a64_obj_ctx_t *c, const char *symbol,
                                  unsigned type) {
  return ny_a64_record_reloc(c, symbol, type) && ny_a64_u32(c, 0x94000000u);
}

static bool ny_a64_add_reloc(ny_a64_obj_ctx_t *c, const char *symbol) {
  return ny_a64_add_reloc_type(c, symbol, NY_RELOC_AARCH64_CALL26);
}

#include "code/backend/bare/aarch64.inc"

static bool ny_a64_bare_payload(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                                const int *args, int argc) {
  unsigned expected = 0;
  unsigned op =
      ny_bare_payload_op(ny_native_runtime_operation(in->symbol), &expected);
  if (!op || argc != (int)expected) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 bare runtime payload argument mismatch");
    return false;
  }
  if (!c->bare_payload_entry) {
    size_t skip = c->code.len;
    if (!ny_a64_u32(c, 0x14000000u))
      return false;
    size_t base = c->code.len;
    if (!nyr_obj_emit(&c->code, ny_bare_aarch64_code,
                      sizeof(ny_bare_aarch64_code)))
      return false;
    while (c->code.len & 3)
      if (!nyr_obj_emit(&c->code, "\0", 1))
        return false;
    if (!ny_a64_patch_branch(c, skip, c->code.len, false))
      return false;
    c->bare_payload_entry = base + ny_bare_aarch64_entry + 1;
  }
  /*
   * Runtime boundaries use raw word homes, including binary64 bits.
   */
  for (int i = 0; i < argc; ++i)
    if (!ny_a64_load_value(c, (unsigned)i, args[i]))
      return false;
  if (!ny_a64_mov_imm(c, 4, op))
    return false;
  size_t call = c->code.len;
  if (!ny_a64_u32(c, 0x94000000u) ||
      !ny_a64_patch_branch(c, call, c->bare_payload_entry - 1, false))
    return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

static bool ny_a64_inline_bare_cstr_copy_concat(ny_a64_obj_ctx_t *c,
                                                const nyr_inst_t *in, int argc,
                                                bool convert_one) {
  if (!c || !in || (convert_one ? argc != 1 : argc != 2)) {
    nyr_set_error(
        c ? c->err : NULL, c ? c->err_len : 0,
        convert_one
            ? "AArch64 bare C-string conversion expects one argument"
            : "AArch64 bare C-string concatenation expects two arguments");
    return false;
  }
  if (!ny_a64_adjust_sp(c, true, 64))
    return false;
  for (unsigned reg = 19; reg <= 26; ++reg)
    if (!ny_a64_reg_mem_base(c, false, reg, 31, (int)((reg - 19) * 8)))
      return false;

  if (!ny_a64_u32(c, 0xaa0003f3u)) /* mov x19,x0 */
    return false;
  size_t null_conversion = SIZE_MAX;
  if (convert_one) {
    if (!ny_a64_u32(c, 0xb4000000u)) /* cbz x0,failure */
      return false;
    null_conversion = c->code.len - sizeof(uint32_t);
    if (!ny_a64_u32(c, 0xaa1f03f4u)) /* mov x20,xzr */
      return false;
  } else if (!ny_a64_u32(c, 0xaa0103f4u)) { /* mov x20,x1 */
    return false;
  }
  if (!ny_a64_u32(c, 0xaa1f03f5u) || /* mov x21,xzr */
      !ny_a64_u32(c, 0xb4000013u))   /* cbz x19,left_done */
    return false;
  size_t left_empty = c->code.len - sizeof(uint32_t);
  size_t left_loop = c->code.len;
  if (!ny_a64_u32(c, 0x38756a69u) || /* ldrb w9,[x19,x21] */
      !ny_a64_u32(c, 0x34000009u))   /* cbz w9,left_done */
    return false;
  size_t left_end = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0x910006b5u) || /* add x21,x21,#1 */
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t left_back = c->code.len - sizeof(uint32_t);
  size_t left_done = c->code.len;
  if (!ny_a64_patch_branch(c, left_empty, left_done, true) ||
      !ny_a64_patch_branch(c, left_end, left_done, true) ||
      !ny_a64_patch_branch(c, left_back, left_loop, false) ||
      !ny_a64_u32(c, 0xaa1f03f6u) || /* mov x22,xzr */
      !ny_a64_u32(c, 0xb4000014u))   /* cbz x20,right_done */
    return false;
  size_t right_empty = c->code.len - sizeof(uint32_t);
  size_t right_loop = c->code.len;
  if (!ny_a64_u32(c, 0x38766a89u) || /* ldrb w9,[x20,x22] */
      !ny_a64_u32(c, 0x34000009u))   /* cbz w9,right_done */
    return false;
  size_t right_end = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0x910006d6u) || /* add x22,x22,#1 */
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t right_back = c->code.len - sizeof(uint32_t);
  size_t right_done = c->code.len;
  if (!ny_a64_patch_branch(c, right_empty, right_done, true) ||
      !ny_a64_patch_branch(c, right_end, right_done, true) ||
      !ny_a64_patch_branch(c, right_back, right_loop, false) ||
      !ny_a64_u32(c, 0xab1602b7u) || /* adds x23,x21,x22 */
      !ny_a64_u32(c, 0x54000002u))   /* b.cs failure */
    return false;
  size_t total_overflow = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xb14006e9u) || /* adds x9,x23,#1,lsl #12 */
      !ny_a64_u32(c, 0x54000002u))   /* b.cs failure */
    return false;
  size_t page_add_overflow = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xb1008129u) || /* adds x9,x9,#32 */
      !ny_a64_u32(c, 0x54000002u))   /* b.cs failure */
    return false;
  size_t header_add_overflow = c->code.len - sizeof(uint32_t);
  if (!ny_a64_mov_imm(c, 10, -4096) ||
      !ny_a64_u32(c, 0x8a0a0129u) || /* and x9,x9,x10 */
      !ny_a64_u32(c, 0xaa1f03e0u) || /* mov x0,xzr */
      !ny_a64_u32(c, 0xaa0903e1u) || /* mov x1,x9 */
      !ny_a64_mov_imm(c, 2, 3) || !ny_a64_mov_imm(c, 3, 34) ||
      !ny_a64_mov_imm(c, 4, -1) || !ny_a64_mov_imm(c, 5, 0) ||
      !ny_a64_mov_imm(c, 8, c->target->syscalls.mmap_nr) ||
      !ny_a64_u32(c, 0xd4000001u) || !ny_a64_u32(c, 0xf100001fu) ||
      !ny_a64_u32(c, 0x54000004u))
    return false;
  size_t mmap_failure = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xaa0003f8u) || /* mov x24,x0 */
      !ny_a64_mov_imm(c, 11, INT64_C(0x545249584e5954)) ||
      !ny_a64_reg_mem_base(c, false, 11, 24, 0) ||
      !ny_a64_reg_mem_base(c, false, 9, 24, 8) ||
      !ny_a64_u32(c, 0x8b1702f9u) || /* add x25,x23,x23 */
      !ny_a64_u32(c, 0x91000739u) || /* add x25,x25,#1 */
      !ny_a64_reg_mem_base(c, false, 25, 24, 16) ||
      !ny_a64_mov_imm(c, 11, 120) ||
      !ny_a64_reg_mem_base(c, false, 11, 24, 24) ||
      !ny_a64_u32(c, 0x9100830au) || /* add x10,x24,#32 */
      !ny_a64_u32(c, 0xaa1f03fau))   /* mov x26,xzr */
    return false;
  size_t left_copy_loop = c->code.len;
  if (!ny_a64_u32(c, 0xeb15035fu) || /* cmp x26,x21 */
      !ny_a64_u32(c, 0x54000002u))   /* b.hs left_copy_done */
    return false;
  size_t left_copy_done_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0x387a6a69u) || /* ldrb w9,[x19,x26] */
      !ny_a64_u32(c, 0x383a6949u) || /* strb w9,[x10,x26] */
      !ny_a64_u32(c, 0x9100075au) || /* add x26,x26,#1 */
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t left_copy_back = c->code.len - sizeof(uint32_t);
  size_t left_copy_done = c->code.len;
  if (!ny_a64_patch_branch(c, left_copy_done_branch, left_copy_done, true) ||
      !ny_a64_patch_branch(c, left_copy_back, left_copy_loop, false) ||
      !ny_a64_u32(c, 0x8b15014au) || /* add x10,x10,x21 */
      !ny_a64_u32(c, 0xaa1f03fau))   /* mov x26,xzr */
    return false;
  size_t right_copy_loop = c->code.len;
  if (!ny_a64_u32(c, 0xeb16035fu) || /* cmp x26,x22 */
      !ny_a64_u32(c, 0x54000002u))   /* b.hs right_copy_done */
    return false;
  size_t right_copy_done_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0x387a6a89u) || /* ldrb w9,[x20,x26] */
      !ny_a64_u32(c, 0x383a6949u) || /* strb w9,[x10,x26] */
      !ny_a64_u32(c, 0x9100075au) || /* add x26,x26,#1 */
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t right_copy_back = c->code.len - sizeof(uint32_t);
  size_t right_copy_done = c->code.len;
  if (!ny_a64_patch_branch(c, right_copy_done_branch, right_copy_done, true) ||
      !ny_a64_patch_branch(c, right_copy_back, right_copy_loop, false) ||
      !ny_a64_u32(c, 0x3836695fu) || /* strb wzr,[x10,x22] */
      !ny_a64_u32(c, 0x91008300u) || /* add x0,x24,#32 */
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t success_branch = c->code.len - sizeof(uint32_t);
  size_t failure = c->code.len;
  if (!ny_a64_mov_imm(c, 0, 0))
    return false;
  size_t done = c->code.len;
  if (!ny_a64_patch_branch(c, total_overflow, failure, true) ||
      !ny_a64_patch_branch(c, page_add_overflow, failure, true) ||
      !ny_a64_patch_branch(c, header_add_overflow, failure, true) ||
      !ny_a64_patch_branch(c, mmap_failure, failure, true) ||
      (null_conversion != SIZE_MAX &&
       !ny_a64_patch_branch(c, null_conversion, failure, true)) ||
      !ny_a64_patch_branch(c, success_branch, done, false))
    return false;
  for (unsigned reg = 19; reg <= 26; ++reg)
    if (!ny_a64_reg_mem_base(c, true, reg, 31, (int)((reg - 19) * 8)))
      return false;
  if (!ny_a64_adjust_sp(c, false, 64))
    return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

/*
 * Bare strings are NUL-terminated byte strings. Keep Unicode character
 * construction in the runtime-operation layer so the language lowering is
 * target independent; this legalization allocates a page and writes the
 * UTF-8 encoding without libc.
 */
static bool ny_a64_inline_bare_str_chr(ny_a64_obj_ctx_t *c,
                                       const nyr_inst_t *in, int argc) {
  if (!c || !in || argc != 1) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare string character helper expects one argument");
    return false;
  }
  /*
   * Preserve the scalar across mmap; x0..x18 are caller-saved scratch here.
   */
  if (!ny_a64_u32(c, 0xaa0003e9u) || !ny_a64_mov_imm(c, 0, 0) ||
      !ny_a64_mov_imm(c, 1, 4096) || !ny_a64_mov_imm(c, 2, 3) ||
      !ny_a64_mov_imm(c, 3, 34) || !ny_a64_mov_imm(c, 4, -1) ||
      !ny_a64_mov_imm(c, 5, 0) ||
      !ny_a64_mov_imm(c, 8, c->target->syscalls.mmap_nr) ||
      !ny_a64_u32(c, 0xd4000001u) || !ny_a64_u32(c, 0xf100001fu) ||
      !ny_a64_u32(c, 0x54000004u)) /* b.mi allocation_failed */
    return false;
  size_t allocation_failed_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xaa0003eau) || /* mov x10,x0 */
      !ny_a64_u32(c, 0xf100013fu) || /* cmp x9,#0 */
      !ny_a64_u32(c, 0x5400000bu))   /* b.lt empty */
    return false;
  size_t negative_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_mov_imm(c, 11, 0x10ffff) ||
      !ny_a64_u32(c, 0xeb0b013fu) || /* cmp x9,x11 */
      !ny_a64_u32(c, 0x5400000cu))   /* b.gt empty */
    return false;
  size_t range_branch = c->code.len - sizeof(uint32_t);

  size_t length_branches[3];
  const int64_t limits[3] = {127, 2047, 65535};
  for (size_t i = 0; i < 3; ++i) {
    if (!ny_a64_mov_imm(c, 11, limits[i]) ||
        !ny_a64_u32(c, 0xeb0b013fu) || /* cmp x9,x11 */
        !ny_a64_u32(c, 0x5400000du))   /* b.le length_i */
      return false;
    length_branches[i] = c->code.len - sizeof(uint32_t);
  }

  /*
   * Four-byte UTF-8: F0 | (cp >> 18), then three continuation bytes.
   */
  if (!ny_a64_u32(c, 0xd352fd2cu) || /* lsr x12,x9,#18 */
      !ny_a64_mov_imm(c, 13, 0xf0) ||
      !ny_a64_u32(c, 0x2a0d018cu) || /* orr w12,w12,w13 */
      !ny_a64_u32(c, 0x3900014cu) || /* strb w12,[x10,#0] */
      !ny_a64_u32(c, 0x530c7d2cu) || /* lsr w12,w9,#12 */
      !ny_a64_u32(c, 0x1200158cu) || /* and w12,w12,#0x3f */
      !ny_a64_mov_imm(c, 13, 0x80) || !ny_a64_u32(c, 0x2a0d018cu) ||
      !ny_a64_u32(c, 0x3900054cu) || /* strb w12,[x10,#1] */
      !ny_a64_u32(c, 0x53067d2cu) || /* lsr w12,w9,#6 */
      !ny_a64_u32(c, 0x1200158cu) || !ny_a64_u32(c, 0x2a0d018cu) ||
      !ny_a64_u32(c, 0x3900094cu) || /* strb w12,[x10,#2] */
      !ny_a64_u32(c, 0x1200152cu) || /* and w12,w9,#0x3f */
      !ny_a64_u32(c, 0x2a0d018cu) ||
      !ny_a64_u32(c, 0x39000d4cu) || /* strb w12,[x10,#3] */
      !ny_a64_u32(c, 0x3900115fu) || /* strb wzr,[x10,#4] */
      !ny_a64_u32(c, 0x14000000u))   /* b finish */
    return false;
  size_t four_finish = c->code.len - sizeof(uint32_t);

  size_t three_byte = c->code.len;
  if (!ny_a64_u32(c, 0xd34cfd2cu) || /* lsr x12,x9,#12 */
      !ny_a64_mov_imm(c, 13, 0xe0) || !ny_a64_u32(c, 0x2a0d018cu) ||
      !ny_a64_u32(c, 0x3900014cu) || !ny_a64_u32(c, 0x53067d2cu) ||
      !ny_a64_u32(c, 0x1200158cu) || !ny_a64_mov_imm(c, 13, 0x80) ||
      !ny_a64_u32(c, 0x2a0d018cu) || !ny_a64_u32(c, 0x3900054cu) ||
      !ny_a64_u32(c, 0x1200152cu) || !ny_a64_u32(c, 0x2a0d018cu) ||
      !ny_a64_u32(c, 0x3900094cu) || !ny_a64_u32(c, 0x39000d5fu) ||
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t three_finish = c->code.len - sizeof(uint32_t);

  size_t two_byte = c->code.len;
  if (!ny_a64_u32(c, 0xd346fd2cu) || /* lsr x12,x9,#6 */
      !ny_a64_mov_imm(c, 13, 0xc0) || !ny_a64_u32(c, 0x2a0d018cu) ||
      !ny_a64_u32(c, 0x3900014cu) || !ny_a64_u32(c, 0x1200152cu) ||
      !ny_a64_mov_imm(c, 13, 0x80) || !ny_a64_u32(c, 0x2a0d018cu) ||
      !ny_a64_u32(c, 0x3900054cu) || !ny_a64_u32(c, 0x3900095fu) ||
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t two_finish = c->code.len - sizeof(uint32_t);

  size_t one_byte = c->code.len;
  if (!ny_a64_u32(c, 0x2a0903ecu) || /* mov w12,w9 */
      !ny_a64_u32(c, 0x3900014cu) || !ny_a64_u32(c, 0x3900055fu) ||
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t one_finish = c->code.len - sizeof(uint32_t);

  size_t empty = c->code.len;
  if (!ny_a64_u32(c, 0x3900015fu)) /* strb wzr,[x10,#0] */
    return false;
  size_t finish = c->code.len;
  if (!ny_a64_u32(c, 0xaa0a03e0u) || /* mov x0,x10 */
      !ny_a64_u32(c, 0x14000000u))   /* b done */
    return false;
  size_t success_done_branch = c->code.len - sizeof(uint32_t);
  size_t allocation_failed = c->code.len;
  if (!ny_a64_mov_imm(c, 0, 0))
    return false;
  size_t done = c->code.len;

  if (!ny_a64_patch_branch(c, allocation_failed_branch, allocation_failed,
                           true) ||
      !ny_a64_patch_branch(c, negative_branch, empty, true) ||
      !ny_a64_patch_branch(c, range_branch, empty, true) ||
      !ny_a64_patch_branch(c, length_branches[0], one_byte, true) ||
      !ny_a64_patch_branch(c, length_branches[1], two_byte, true) ||
      !ny_a64_patch_branch(c, length_branches[2], three_byte, true) ||
      !ny_a64_patch_branch(c, four_finish, finish, false) ||
      !ny_a64_patch_branch(c, three_finish, finish, false) ||
      !ny_a64_patch_branch(c, two_finish, finish, false) ||
      !ny_a64_patch_branch(c, one_finish, finish, false) ||
      !ny_a64_patch_branch(c, success_done_branch, done, false))
    return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

/*
 * Repeat a bare C string without libc. This is the target legalization of
 * the shared `rt_str_repeat` runtime operation; strings remain plain
 * NUL-terminated byte pointers on the freestanding route.
 */
static bool ny_a64_inline_bare_str_repeat(ny_a64_obj_ctx_t *c,
                                          const nyr_inst_t *in, int argc) {
  if (!c || !in || argc != 2) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare string repeat expects two arguments");
    return false;
  }
  if (!ny_a64_u32(c, 0xaa0003e9u) || /* mov x9,x0 (source) */
      !ny_a64_u32(c, 0xaa0103eau) || /* mov x10,x1 (count) */
      !ny_a64_u32(c, 0xf100015fu) || /* cmp x10,#0 */
      !ny_a64_u32(c, 0x5400000du))   /* b.le empty */
    return false;
  size_t count_empty_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xb4000000u)) /* cbz x9,empty */
    return false;
  size_t null_empty_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xaa1f03ecu)) /* mov x12,xzr (source length) */
    return false;
  size_t length_loop = c->code.len;
  if (!ny_a64_u32(c, 0x386c692du) || /* ldrb w13,[x9,x12] */
      !ny_a64_u32(c, 0x3400000du))   /* cbz w13,length_done */
    return false;
  size_t length_done_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0x9100058cu) || /* add x12,x12,#1 */
      !ny_a64_u32(c, 0x14000000u))   /* b length_loop */
    return false;
  size_t length_back = c->code.len - sizeof(uint32_t);
  size_t length_done = c->code.len;
  if (!ny_a64_patch_branch(c, length_done_branch, length_done, true) ||
      !ny_a64_patch_branch(c, length_back, length_loop, false) ||
      !ny_a64_u32(c, 0xf100019fu) || /* cmp x12,#0 */
      !ny_a64_u32(c, 0x5400000du))   /* b.le empty */
    return false;
  size_t zero_length_branch = c->code.len - sizeof(uint32_t);
  /*
   * Compute total length and reject multiplication/addition overflow.
   */
  if (!ny_a64_u32(c, 0x9b0a7d8eu) || /* mul x14,x12,x10 */
      !ny_a64_u32(c, 0x9b4a7d8fu) || /* umulh x15,x12,x10 */
      !ny_a64_u32(c, 0xf10001ffu) || /* cmp x15,#0 */
      !ny_a64_u32(c, 0x54000001u))   /* b.ne empty */
    return false;
  size_t multiply_overflow_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xb10005c1u) || /* adds x1,x14,#1 */
      !ny_a64_u32(c, 0x54000002u))   /* b.cs empty */
    return false;
  size_t length_overflow_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xb13ffc21u) || /* adds x1,x1,#4095 */
      !ny_a64_u32(c, 0x54000002u))   /* b.cs empty */
    return false;
  size_t page_overflow_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_mov_imm(c, 16, -4096) ||
      !ny_a64_u32(c, 0x8a100021u) || /* and x1,x1,x16 */
      !ny_a64_mov_imm(c, 0, 0) || !ny_a64_mov_imm(c, 2, 3) ||
      !ny_a64_mov_imm(c, 3, 34) || !ny_a64_mov_imm(c, 4, -1) ||
      !ny_a64_mov_imm(c, 5, 0) ||
      !ny_a64_mov_imm(c, 8, c->target->syscalls.mmap_nr) ||
      !ny_a64_u32(c, 0xd4000001u) || !ny_a64_u32(c, 0xf100001fu) ||
      !ny_a64_u32(c, 0x54000004u)) /* b.mi empty */
    return false;
  size_t mmap_failure_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xaa0003f1u) || /* mov x17,x0 (output) */
      !ny_a64_u32(c, 0xaa1f03ebu))   /* mov x11,xzr (source index) */
    return false;
  size_t copy_loop = c->code.len;
  if (!ny_a64_u32(c, 0xeb0e01ffu) || /* cmp x15,x14 */
      !ny_a64_u32(c, 0x54000002u))   /* b.hs copy_done */
    return false;
  size_t copy_done_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0x386b692du) || /* ldrb w13,[x9,x11] */
      !ny_a64_u32(c, 0x382f6a2du) || /* strb w13,[x17,x15] */
      !ny_a64_u32(c, 0x910005efu) || /* add x15,x15,#1 */
      !ny_a64_u32(c, 0x9100056bu) || /* add x11,x11,#1 */
      !ny_a64_u32(c, 0xeb0c017fu) || /* cmp x11,x12 */
      !ny_a64_u32(c, 0x54000001u))   /* b.ne copy_loop */
    return false;
  size_t source_wrap_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_mov_imm(c, 11, 0) || !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t copy_back = c->code.len - sizeof(uint32_t);
  size_t copy_done = c->code.len;
  if (!ny_a64_patch_branch(c, copy_done_branch, copy_done, true) ||
      !ny_a64_patch_branch(c, copy_back, copy_loop, false) ||
      !ny_a64_u32(c, 0x382e6a3fu)) /* strb wzr,[x17,x14] */
    return false;
  if (!ny_a64_u32(c, 0xaa1103e0u) || /* mov x0,x17 */
      !ny_a64_u32(c, 0x14000000u))   /* b finish */
    return false;
  size_t success_branch = c->code.len - sizeof(uint32_t);
  size_t empty = c->code.len;
  if (!ny_a64_mov_imm(c, 0, 0))
    return false;
  size_t finish = c->code.len;
  if (!ny_a64_patch_branch(c, count_empty_branch, empty, true) ||
      !ny_a64_patch_branch(c, null_empty_branch, empty, true) ||
      !ny_a64_patch_branch(c, zero_length_branch, empty, true) ||
      !ny_a64_patch_branch(c, mmap_failure_branch, empty, true) ||
      !ny_a64_patch_branch(c, multiply_overflow_branch, empty, true) ||
      !ny_a64_patch_branch(c, length_overflow_branch, empty, true) ||
      !ny_a64_patch_branch(c, page_overflow_branch, empty, true) ||
      !ny_a64_patch_branch(c, source_wrap_branch, copy_loop, true) ||
      !ny_a64_patch_branch(c, success_branch, finish, false))
    return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

static bool ny_a64_inline_bare_cstr_eq(ny_a64_obj_ctx_t *c,
                                       const nyr_inst_t *in, int argc) {
  if (!c || !in || argc != 2) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare C-string equality expects two arguments");
    return false;
  }
  if (!ny_a64_u32(c, 0xaa0003e9u) || /* mov x9,x0 */
      !ny_a64_u32(c, 0xaa0103eau) || /* mov x10,x1 */
      !ny_a64_u32(c, 0xeb01001fu) || /* cmp x0,x1 */
      !ny_a64_u32(c, 0x54000000u))   /* b.eq equal */
    return false;
  size_t equal_pointer_branch = c->code.len - sizeof(uint32_t);
  size_t x0_null_branch = c->code.len;
  if (!ny_a64_u32(c, 0xb4000000u) || /* cbz x0,unequal */
      !ny_a64_u32(c, 0xb4000001u))   /* cbz x1,unequal */
    return false;
  size_t x1_null_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xaa1f03ebu)) /* mov x11,xzr */
    return false;
  size_t loop = c->code.len;
  if (!ny_a64_u32(c, 0x386b692cu) || /* ldrb w12,[x9,x11] */
      !ny_a64_u32(c, 0x386b694du) || /* ldrb w13,[x10,x11] */
      !ny_a64_u32(c, 0x6b0d019fu) || /* cmp w12,w13 */
      !ny_a64_u32(c, 0x54000001u))   /* b.ne unequal */
    return false;
  size_t unequal_byte_branch = c->code.len - sizeof(uint32_t);
  size_t equal_byte_branch = c->code.len;
  if (!ny_a64_u32(c, 0x3400000cu) || /* cbz w12,equal */
      !ny_a64_u32(c, 0x9100056bu) || /* add x11,x11,#1 */
      !ny_a64_u32(c, 0x14000000u))   /* b loop */
    return false;
  size_t loop_branch = c->code.len - sizeof(uint32_t);
  size_t unequal = c->code.len;
  if (!ny_a64_u32(c, 0xaa1f03e0u) || /* mov x0,xzr */
      !ny_a64_u32(c, 0x14000000u))   /* b finish */
    return false;
  size_t unequal_finish_branch = c->code.len - sizeof(uint32_t);
  size_t equal = c->code.len;
  if (!ny_a64_mov_imm(c, 0, 1))
    return false;
  size_t finish = c->code.len;
  return ny_a64_patch_branch(c, equal_pointer_branch, equal, true) &&
         ny_a64_patch_branch(c, x0_null_branch, unequal, true) &&
         ny_a64_patch_branch(c, x1_null_branch, unequal, true) &&
         ny_a64_patch_branch(c, unequal_byte_branch, unequal, true) &&
         ny_a64_patch_branch(c, equal_byte_branch, equal, true) &&
         ny_a64_patch_branch(c, loop_branch, loop, false) &&
         ny_a64_patch_branch(c, unequal_finish_branch, finish, false) &&
         (in->dst < 0 || ny_a64_store_value(c, in->dst, 0));
}

static bool ny_a64_inline_bare_cstr_cmp(ny_a64_obj_ctx_t *c,
                                        const nyr_inst_t *in, int argc) {
  if (!c || !in || argc != 2) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare C-string comparison expects two arguments");
    return false;
  }
  if (!ny_a64_u32(c, 0xeb01001fu) || /* cmp x0,x1 */
      !ny_a64_u32(c, 0x54000000u))   /* b.eq equal */
    return false;
  size_t equal_pointer_branch = c->code.len - sizeof(uint32_t);
  size_t null_left_branch = c->code.len;
  if (!ny_a64_u32(c, 0xb4000000u) || /* cbz x0,null-left */
      !ny_a64_u32(c, 0xb4000001u))   /* cbz x1,null-right */
    return false;
  size_t null_right_branch = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xaa0003e9u) || /* mov x9,x0 */
      !ny_a64_u32(c, 0xaa0103eau) || /* mov x10,x1 */
      !ny_a64_u32(c, 0xaa1f03ebu))   /* mov x11,xzr */
    return false;
  size_t loop = c->code.len;
  if (!ny_a64_u32(c, 0x386b692cu) || /* ldrb w12,[x9,x11] */
      !ny_a64_u32(c, 0x386b694du) || /* ldrb w13,[x10,x11] */
      !ny_a64_u32(c, 0x6b0d019fu) || /* cmp w12,w13 */
      !ny_a64_u32(c, 0x54000001u))   /* b.ne different */
    return false;
  size_t different_branch = c->code.len - sizeof(uint32_t);
  size_t terminator_branch = c->code.len;
  if (!ny_a64_u32(c, 0x3400000cu) || /* cbz w12,equal */
      !ny_a64_u32(c, 0x9100056bu) || /* add x11,x11,#1 */
      !ny_a64_u32(c, 0x14000000u))   /* b loop */
    return false;
  size_t loop_branch = c->code.len - sizeof(uint32_t);
  size_t different = c->code.len;
  if (!ny_a64_u32(c, 0xcb0d0180u) || /* sub x0,x12,x13 */
      !ny_a64_u32(c, 0x14000000u))   /* b finish */
    return false;
  size_t different_finish_branch = c->code.len - sizeof(uint32_t);
  size_t null_left = c->code.len;
  if (!ny_a64_mov_imm(c, 0, -1) || !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t null_left_finish_branch = c->code.len - sizeof(uint32_t);
  size_t null_right = c->code.len;
  if (!ny_a64_mov_imm(c, 0, 1) || !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t null_right_finish_branch = c->code.len - sizeof(uint32_t);
  size_t equal = c->code.len;
  if (!ny_a64_mov_imm(c, 0, 0))
    return false;
  size_t finish = c->code.len;
  return ny_a64_patch_branch(c, equal_pointer_branch, equal, true) &&
         ny_a64_patch_branch(c, null_left_branch, null_left, true) &&
         ny_a64_patch_branch(c, null_right_branch, null_right, true) &&
         ny_a64_patch_branch(c, different_branch, different, true) &&
         ny_a64_patch_branch(c, terminator_branch, equal, true) &&
         ny_a64_patch_branch(c, loop_branch, loop, false) &&
         ny_a64_patch_branch(c, different_finish_branch, finish, false) &&
         ny_a64_patch_branch(c, null_left_finish_branch, finish, false) &&
         ny_a64_patch_branch(c, null_right_finish_branch, finish, false) &&
         (in->dst < 0 || ny_a64_store_value(c, in->dst, 0));
}

static bool ny_a64_inline_bare_print(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                                     int argc) {
  if (!c || !in || !in->symbol)
    return false;
  ny_native_runtime_op_t operation = ny_native_runtime_operation(in->symbol);
  if (operation == NY_NATIVE_RUNTIME_OP_STR_CHR)
    return ny_a64_inline_bare_str_chr(c, in, argc);
  if (operation == NY_NATIVE_RUNTIME_OP_STR_REPEAT)
    return ny_a64_inline_bare_str_repeat(c, in, argc);
  if (operation == NY_NATIVE_RUNTIME_OP_CSTR_EQUAL)
    return ny_a64_inline_bare_cstr_eq(c, in, argc);
  if (operation == NY_NATIVE_RUNTIME_OP_CSTR_COMPARE)
    return ny_a64_inline_bare_cstr_cmp(c, in, argc);
  if (operation == NY_NATIVE_RUNTIME_OP_CSTR_CONCAT)
    return ny_a64_inline_bare_cstr_copy_concat(c, in, argc, false);
  if (operation == NY_NATIVE_RUNTIME_OP_CSTR_TO_STRING)
    return ny_a64_inline_bare_cstr_copy_concat(c, in, argc, true);
  if (operation == NY_NATIVE_RUNTIME_OP_PRINT_CSTR) {
    if (argc != 1) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 bare print helper expects one argument");
      return false;
    }
    size_t null_branch = c->code.len;
    if (!ny_a64_u32(c, 0xb4000000u) || /* cbz x0, return */
        !ny_a64_u32(c, 0xaa0003e1u) || /* mov x1,x0 */
        !ny_a64_u32(c, 0xaa1f03e2u))   /* mov x2,xzr */
      return false;
    size_t loop = c->code.len;
    if (!ny_a64_u32(c, 0x38626823u)) /* ldrb w3,[x1,x2] */
      return false;
    size_t done_branch = c->code.len;
    if (!ny_a64_u32(c, 0x34000003u) || /* cbz w3, write */
        !ny_a64_u32(c, 0x91000442u))   /* add x2,x2,#1 */
      return false;
    size_t back_branch = c->code.len;
    if (!ny_a64_u32(c, 0x14000000u))
      return false;
    size_t write = c->code.len;
    if (!ny_a64_mov_imm(c, 0, 1) ||
        !ny_a64_mov_imm(c, 8, c->target->syscalls.write_nr) ||
        !ny_a64_u32(c, 0xd4000001u))
      return false;
    size_t ret = c->code.len;
    if (!ny_a64_u32(c, 0xaa1f03e0u) || /* mov x0,xzr */
        !ny_a64_patch_branch(c, null_branch, ret, true) ||
        !ny_a64_patch_branch(c, done_branch, write, true) ||
        !ny_a64_patch_branch(c, back_branch, loop, false))
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
  }
  if (operation == NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE) {
    if (argc != 0) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 bare newline helper takes no arguments");
      return false;
    }
    return ny_a64_u32(c, 0xd10043ffu) && /* sub sp,sp,#16 */
           ny_a64_mov_imm(c, 1, '\n') &&
           ny_a64_u32(c, 0x390003e1u) && /* strb w1,[sp] */
           ny_a64_mov_imm(c, 0, 1) &&
           ny_a64_u32(c, 0x910003e1u) && /* mov x1,sp */
           ny_a64_mov_imm(c, 2, 1) &&
           ny_a64_mov_imm(c, 8, c->target->syscalls.write_nr) &&
           ny_a64_u32(c, 0xd4000001u) && ny_a64_u32(c, 0x910043ffu) &&
           ny_a64_u32(c, 0xaa1f03e0u) &&
           (in->dst < 0 || ny_a64_store_value(c, in->dst, 0));
  }
  if (operation == NY_NATIVE_RUNTIME_OP_PRINT_I64) {
    if (argc != 1) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 bare integer print helper expects one argument");
      return false;
    }
    if (!ny_a64_u32(c, 0xd100c3ffu) || /* sub sp,sp,#48 */
        !ny_a64_u32(c, 0xaa0003edu) || /* mov x13,x0 */
        !ny_a64_u32(c, 0x9100c3eeu) || /* add x14,sp,#48 */
        !ny_a64_u32(c, 0xaa1f03e4u) || /* mov x4,xzr */
        !ny_a64_u32(c, 0xf100001fu))   /* cmp x0,#0 */
      return false;
    size_t positive_branch = c->code.len;
    if (!ny_a64_u32(c, 0x5400000au) || /* b.ge positive */
        !ny_a64_mov_imm(c, 4, 1) || !ny_a64_u32(c, 0xcb0003e9u)) /* neg x9,x0 */
      return false;
    size_t negative_branch = c->code.len;
    if (!ny_a64_u32(c, 0x14000000u))
      return false;
    size_t positive = c->code.len;
    if (!ny_a64_u32(c, 0xaa0003e9u)) /* mov x9,x0 */
      return false;
    size_t loop = c->code.len;
    if (!ny_a64_mov_imm(c, 10, 10) ||
        !ny_a64_u32(c, 0x9aca092bu) || /* udiv x11,x9,x10 */
        !ny_a64_u32(c, 0x9b0aa56cu) || /* msub x12,x11,x10,x9 */
        !ny_a64_u32(c, 0x9100c18cu) || /* add x12,x12,#48 */
        !ny_a64_u32(c, 0xd1000421u) || /* sub x1,x1,#1 */
        !ny_a64_u32(c, 0x3900002cu) || /* strb w12,[x1] */
        !ny_a64_u32(c, 0xaa0b03e9u) || /* mov x9,x11 */
        !ny_a64_u32(c, 0xf100013fu))   /* cmp x9,#0 */
      return false;
    size_t loop_branch = c->code.len;
    if (!ny_a64_u32(c, 0x54000001u) || /* b.ne loop */
        !ny_a64_u32(c, 0xb4000004u))   /* cbz x4,no_sign */
      return false;
    size_t sign_branch = c->code.len - sizeof(uint32_t);
    if (!ny_a64_u32(c, 0xd1000421u) || /* sub x1,x1,#1 */
        !ny_a64_mov_imm(c, 12, '-') ||
        !ny_a64_u32(c, 0x3900002cu)) /* strb w12,[x1] */
      return false;
    size_t no_sign = c->code.len;
    if (!ny_a64_u32(c, 0xaa0e03e2u) || /* mov x2,x14 */
        !ny_a64_u32(c, 0xcb0101c2u) || /* sub x2,x14,x1 */
        !ny_a64_mov_imm(c, 0, 1) ||
        !ny_a64_mov_imm(c, 8, c->target->syscalls.write_nr) ||
        !ny_a64_u32(c, 0xd4000001u) || !ny_a64_u32(c, 0x9100c3ffu) ||
        !ny_a64_u32(c, 0xaa0d03e0u) || /* mov x0,x13 */
        !ny_a64_patch_branch(c, positive_branch, positive, true) ||
        !ny_a64_patch_branch(c, negative_branch, loop, false) ||
        !ny_a64_patch_branch(c, loop_branch, loop, true) ||
        !ny_a64_patch_branch(c, sign_branch, no_sign, true))
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
  }
  if (operation == NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR) {
    if (argc != 1 || c->runtime_scratch_off < 0 ||
        c->runtime_scratch_off > 4095 - 5) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 bare boolean formatting has no frame scratch");
      return false;
    }
    if (!ny_a64_u32(c,
                    0x910003e1u | ((uint32_t)c->runtime_scratch_off << 10)) ||
        !ny_a64_u32(c, 0xf100041fu)) /* cmp x0,#1 */
      return false;
    size_t raw_true_branch = c->code.len;
    if (!ny_a64_u32(c, 0x54000000u) || /* b.eq true */
        !ny_a64_u32(c, 0xf100201fu))   /* cmp x0,#8 */
      return false;
    size_t tagged_true_branch = c->code.len;
    if (!ny_a64_u32(c, 0x54000000u))
      return false;
    static const char false_text[] = "false";
    for (size_t i = 0; i < sizeof(false_text); ++i)
      if (!ny_a64_mov_imm(c, 9, (unsigned char)false_text[i]) ||
          !ny_a64_u32(c, 0x39000029u | ((uint32_t)i << 10)))
        return false;
    size_t false_done_branch = c->code.len;
    if (!ny_a64_u32(c, 0x14000000u))
      return false;
    size_t true_label = c->code.len;
    static const char true_text[] = "true";
    for (size_t i = 0; i < sizeof(true_text); ++i)
      if (!ny_a64_mov_imm(c, 9, (unsigned char)true_text[i]) ||
          !ny_a64_u32(c, 0x39000029u | ((uint32_t)i << 10)))
        return false;
    size_t done = c->code.len;
    if (!ny_a64_u32(c, 0xaa0103e0u) || /* mov x0,x1 */
        !ny_a64_patch_branch(c, raw_true_branch, true_label, true) ||
        !ny_a64_patch_branch(c, tagged_true_branch, true_label, true) ||
        !ny_a64_patch_branch(c, false_done_branch, done, false))
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
  }
  if (operation == NY_NATIVE_RUNTIME_OP_MONOTONIC_NS) {
    if (argc != 0) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 bare monotonic clock helper takes no arguments");
      return false;
    }
    if (!ny_a64_u32(c, 0xd10043ffu) || /* sub sp,sp,#16 */
        !ny_a64_mov_imm(c, 0, 1) ||
        !ny_a64_u32(c, 0x910003e1u) || /* mov x1,sp */
        !ny_a64_mov_imm(c, 8, c->target->syscalls.clock_gettime_nr) ||
        !ny_a64_u32(c, 0xd4000001u) || /* clock_gettime */
        !ny_a64_u32(c, 0xf100001fu))   /* cmp x0,#0 */
      return false;
    size_t error_branch = c->code.len;
    if (!ny_a64_u32(c, 0x5400000bu) || /* b.mi error */
        !ny_a64_u32(c, 0xf94003e9u) || /* ldr x9,[sp] */
        !ny_a64_u32(c, 0xf94007eau) || /* ldr x10,[sp,#8] */
        !ny_a64_mov_imm(c, 11, INT64_C(1000000000)) ||
        !ny_a64_u32(c, 0x9b0b7d29u) || /* mul x9,x9,x11 */
        !ny_a64_u32(c, 0x8b0a0120u))   /* add x0,x9,x10 */
      return false;
    size_t done_branch = c->code.len;
    if (!ny_a64_u32(c, 0x14000000u))
      return false;
    size_t error = c->code.len;
    if (!ny_a64_u32(c, 0xaa1f03e0u)) /* mov x0,xzr */
      return false;
    size_t done = c->code.len;
    if (!ny_a64_u32(c, 0x910043ffu) ||
        !ny_a64_patch_branch(c, error_branch, error, true) ||
        !ny_a64_patch_branch(c, done_branch, done, false))
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
  }
  return false;
}

static bool ny_a64_inline_bare_alloc(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                                     int argc) {
  if (!c || !in || !in->symbol)
    return false;
  ny_native_runtime_op_t operation = ny_native_runtime_operation(in->symbol);
  bool allocate = operation == NY_NATIVE_RUNTIME_OP_BARE_HEAP_ALLOC;
  bool free_memory = operation == NY_NATIVE_RUNTIME_OP_BARE_HEAP_FREE;
  bool untag = operation == NY_NATIVE_RUNTIME_OP_BARE_UNTAG;
  bool typed_buffer = operation == NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_NEW;
  if (!allocate && !free_memory && !untag && !typed_buffer)
    return false;
  if (argc != (typed_buffer ? 2 : 1)) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 bare allocation helper has invalid arguments");
    return false;
  }
  if (typed_buffer) {
    if (!ny_a64_u32(c, 0xaa0003e9u) || /* mov x9,x0 */
        !ny_a64_u32(c, 0xf100001fu))   /* cmp x0,#0 */
      return false;
    size_t count_nonnegative = c->code.len;
    if (!ny_a64_u32(c, 0x5400000au) || /* b.ge keep_count */
        !ny_a64_mov_imm(c, 9, 0))
      return false;
    size_t count_ready = c->code.len;
    if (!ny_a64_u32(c, 0xaa0103eau) || /* mov x10,x1 */
        !ny_a64_u32(c, 0xf100003fu))   /* cmp x1,#0 */
      return false;
    size_t elem_positive = c->code.len;
    if (!ny_a64_u32(c, 0x5400000cu) || /* b.gt keep_elem */
        !ny_a64_mov_imm(c, 10, 1))
      return false;
    size_t elem_ready = c->code.len;
    if (!ny_a64_u32(c, 0xaa0903edu) || /* mov x13,x9 */
        !ny_a64_u32(c, 0xf10021bfu))   /* cmp x13,#8 */
      return false;
    size_t capacity_ready = c->code.len;
    if (!ny_a64_u32(c, 0x5400000au) || /* b.ge keep_capacity */
        !ny_a64_mov_imm(c, 13, 8))
      return false;
    size_t multiply = c->code.len;
    if (!ny_a64_u32(c, 0x9b0a7dac) || /* mul x12,x13,x10 */
        !ny_a64_u32(c, 0x9bca7dae) || /* umulh x14,x13,x10 */
        !ny_a64_u32(c, 0xf10001dfu))  /* cmp x14,#0 */
      return false;
    size_t multiply_overflow = c->code.len;
    if (!ny_a64_u32(c, 0x54000001u) || /* b.ne fail */
        !ny_a64_u32(c, 0xb1008181u))   /* adds x1,x12,#32 */
      return false;
    size_t length_overflow = c->code.len;
    if (!ny_a64_u32(c, 0x54000002u) || /* b.cs fail */
        !ny_a64_mov_imm(c, 0, 0) || !ny_a64_mov_imm(c, 2, 3) ||
        !ny_a64_mov_imm(c, 3, 34) || !ny_a64_mov_imm(c, 4, -1) ||
        !ny_a64_mov_imm(c, 5, 0) ||
        !ny_a64_mov_imm(c, 8, c->target->syscalls.mmap_nr) ||
        !ny_a64_u32(c, 0xd4000001u) || !ny_a64_u32(c, 0xf100001fu))
      return false;
    size_t mmap_failure = c->code.len;
    if (!ny_a64_u32(c, 0x54000004u) || /* b.mi fail */
        !ny_a64_mov_imm(c, 11, INT64_C(0x4e59544255464d47)) ||
        !ny_a64_reg_mem_base(c, false, 11, 0, 0) ||
        !ny_a64_reg_mem_base(c, false, 9, 0, 8) ||
        !ny_a64_reg_mem_base(c, false, 10, 0, 16) ||
        !ny_a64_reg_mem_base(c, false, 13, 0, 24) ||
        !ny_a64_u32(c, 0x91008000u)) /* add x0,x0,#32 */
      return false;
    size_t done_branch = c->code.len;
    if (!ny_a64_u32(c, 0x14000000u) || !ny_a64_u32(c, 0xaa1f03e0u))
      return false;
    size_t fail = c->code.len - sizeof(uint32_t);
    if (!ny_a64_patch_branch(c, multiply_overflow, fail, true) ||
        !ny_a64_patch_branch(c, length_overflow, fail, true) ||
        !ny_a64_patch_branch(c, mmap_failure, fail, true))
      return false;
    size_t done = c->code.len;
    if (!ny_a64_patch_branch(c, done_branch, done, false) ||
        !ny_a64_patch_branch(c, count_nonnegative, count_ready, true) ||
        !ny_a64_patch_branch(c, elem_positive, elem_ready, true) ||
        !ny_a64_patch_branch(c, capacity_ready, multiply, true))
      return false;
  } else if (untag) {
    if (!ny_a64_u32(c, 0x92400009u) || /* and x9,x0,#1 */
        !ny_a64_u32(c, 0xf100013fu))   /* cmp x9,#0 */
      return false;
    size_t already_raw = c->code.len;
    if (!ny_a64_u32(c, 0x54000000u) || /* b.eq done */
        !ny_a64_u32(c, 0x9343fc00u))   /* asr x0,x0,#3 */
      return false;
    size_t done = c->code.len;
    if (!ny_a64_patch_branch(c, already_raw, done, true))
      return false;
  } else if (allocate) {
    size_t failure = 0, overflow = 0;
    if ((strcmp(in->symbol, "rt_malloc") == 0 ||
         strcmp(in->symbol, "rt_malloc_uninit") == 0) &&
        !ny_a64_u32(c, 0x9341fc00u)) /* asr x0,x0,#1 */
      return false;
    if (!ny_a64_u32(c, 0xf100001fu)) /* cmp x0,#0 */
      return false;
    failure = c->code.len;
    if (!ny_a64_u32(c, 0x54000004u) || /* b.mi fail */
        !ny_a64_u32(c, 0xb1003c09u))   /* adds x9,x0,#15 */
      return false;
    overflow = c->code.len;
    if (!ny_a64_u32(c, 0x54000002u) || /* b.cs fail */
        !ny_a64_mov_imm(c, 10, -16) ||
        !ny_a64_u32(c, 0x8a0a0129u) || /* and x9,x9,x10 */
        !ny_a64_u32(c, 0xb100812au))   /* adds x10,x9,#32 */
      return false;
    size_t length_overflow = c->code.len;
    if (!ny_a64_u32(c, 0x54000002u) || /* b.cs fail */
        !ny_a64_mov_imm(c, 0, 0) || !ny_a64_u32(c, 0xaa0a03e1u) ||
        !ny_a64_mov_imm(c, 2, 3) || !ny_a64_mov_imm(c, 3, 34) ||
        !ny_a64_mov_imm(c, 4, -1) || !ny_a64_mov_imm(c, 5, 0) ||
        !ny_a64_mov_imm(c, 8, c->target->syscalls.mmap_nr) ||
        !ny_a64_u32(c, 0xd4000001u) || !ny_a64_u32(c, 0xf100001fu))
      return false;
    size_t mmap_failure = c->code.len;
    if (!ny_a64_u32(c, 0x54000004u) || /* b.mi fail */
        !ny_a64_mov_imm(c, 11, INT64_C(0x545249584e5954)) ||
        !ny_a64_reg_mem_base(c, false, 11, 0, 0) ||
        !ny_a64_u32(c, 0x8b09012cu) || /* add x12,x9,x9 */
        !ny_a64_u32(c, 0x9100058cu) || /* add x12,x12,#1 */
        !ny_a64_reg_mem_base(c, false, 12, 0, 8) ||
        !ny_a64_u32(c, 0xaa1f03ecu) ||
        !ny_a64_reg_mem_base(c, false, 12, 0, 16) ||
        !ny_a64_reg_mem_base(c, false, 12, 0, 24) ||
        !ny_a64_u32(c, 0x91008000u)) /* add x0,x0,#32 */
      return false;
    size_t done_branch = c->code.len;
    if (!ny_a64_u32(c, 0x14000000u) || !ny_a64_u32(c, 0xaa1f03e0u))
      return false;
    size_t fail = c->code.len - sizeof(uint32_t);
    if (!ny_a64_patch_branch(c, failure, fail, true) ||
        !ny_a64_patch_branch(c, overflow, fail, true) ||
        !ny_a64_patch_branch(c, length_overflow, fail, true) ||
        !ny_a64_patch_branch(c, mmap_failure, fail, true))
      return false;
    size_t done = c->code.len;
    if (!ny_a64_patch_branch(c, done_branch, done, false))
      return false;
    /*
     * The successful mmap result flows around the failure value.  Failure
     * branches land at the second instruction emitted above.
     */
  } else {
    if (!ny_a64_u32(c, 0xf100001fu)) /* cmp x0,#0 */
      return false;
    size_t null_branch = c->code.len;
    if (!ny_a64_u32(c, 0x54000000u) || /* b.eq done */
        !ny_a64_u32(c, 0xd1008009u) || /* sub x9,x0,#32 */
        !ny_a64_mov_imm(c, 10, INT64_C(0x545249584e5954)) ||
        !ny_a64_reg_mem_base(c, true, 11, 9, 0) ||
        !ny_a64_u32(c, 0xeb0a017fu)) /* cmp x11,x10 */
      return false;
    size_t invalid_branch = c->code.len;
    if (!ny_a64_u32(c, 0x54000001u) || /* b.ne done */
        !ny_a64_reg_mem_base(c, true, 12, 9, 8) ||
        !ny_a64_u32(c, 0xd341fd8cu) || /* lsr x12,x12,#1 */
        !ny_a64_u32(c, 0xb1003d8cu))   /* adds x12,x12,#15 */
      return false;
    size_t overflow_branch = c->code.len;
    if (!ny_a64_u32(c, 0x54000002u) || /* b.cs done */
        !ny_a64_mov_imm(c, 13, -16) ||
        !ny_a64_u32(c, 0x8a0d018cu) || /* and x12,x12,x13 */
        !ny_a64_u32(c, 0x9100818cu) || /* add x12,x12,#32 */
        !ny_a64_u32(c, 0xaa0903e0u) || /* mov x0,x9 */
        !ny_a64_u32(c, 0xaa0c03e1u) || /* mov x1,x12 */
        !ny_a64_mov_imm(c, 8, c->target->syscalls.munmap_nr) ||
        !ny_a64_u32(c, 0xd4000001u))
      return false;
    size_t done = c->code.len;
    if (!ny_a64_u32(c, 0xaa1f03e0u) ||
        !ny_a64_patch_branch(c, null_branch, done, true) ||
        !ny_a64_patch_branch(c, invalid_branch, done, true) ||
        !ny_a64_patch_branch(c, overflow_branch, done, true))
      return false;
  }
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

static bool ny_a64_inline_bare_memset(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                                      int argc) {
  if (!c || !in || argc != 3 ||
      (strcmp(in->symbol, "rt_memset") != 0 &&
       strcmp(in->symbol, "rt_memset_raw") != 0)) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare memset expects three arguments");
    return false;
  }
  if (strcmp(in->symbol, "rt_memset") == 0) {
    if (!ny_a64_u32(c, 0x92400029u) || /* and x9,x1,#1 */
        !ny_a64_u32(c, 0xf100013fu))   /* cmp x9,#0 */
      return false;
    size_t raw_value = c->code.len;
    if (!ny_a64_u32(c, 0x54000000u) || /* b.eq value_ready */
        !ny_a64_u32(c, 0x9341fc21u))   /* asr x1,x1,#1 */
      return false;
    size_t value_ready = c->code.len;
    if (!ny_a64_patch_branch(c, raw_value, value_ready, true) ||
        !ny_a64_u32(c, 0x92400049u) || /* and x9,x2,#1 */
        !ny_a64_u32(c, 0xf100013fu))
      return false;
    size_t raw_count = c->code.len;
    if (!ny_a64_u32(c, 0x54000000u) || /* b.eq count_ready */
        !ny_a64_u32(c, 0x9341fc42u))   /* asr x2,x2,#1 */
      return false;
    size_t count_ready = c->code.len;
    if (!ny_a64_patch_branch(c, raw_count, count_ready, true))
      return false;
  }
  if (!ny_a64_u32(c, 0xaa0003ecu) || /* mov x12,x0 */
      !ny_a64_u32(c, 0xf100005fu))   /* cmp x2,#1 */
    return false;
  size_t done_branch = c->code.len;
  if (!ny_a64_u32(c, 0x5400000du)) /* b.le done */
    return false;
  size_t loop = c->code.len;
  if (!ny_a64_u32(c, 0x39000181u) || /* strb w1,[x12] */
      !ny_a64_u32(c, 0x9100058cu) || /* add x12,x12,#1 */
      !ny_a64_u32(c, 0xd1000442u) || /* sub x2,x2,#1 */
      !ny_a64_u32(c, 0xf100005fu))
    return false;
  size_t loop_branch = c->code.len;
  if (!ny_a64_u32(c, 0x5400000cu) || /* b.gt loop */
      !ny_a64_u32(c, 0xaa0c03e0u) || /* mov x0,x12 */
      !ny_a64_patch_branch(c, done_branch, c->code.len - 4, true) ||
      !ny_a64_patch_branch(c, loop_branch, loop, true))
    return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

static bool ny_a64_inline_bare_memory(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                                      int argc) {
  if (!c || !in || argc != 3 || !in->symbol ||
      (strcmp(in->symbol, "rt_memcpy_raw") != 0 &&
       strcmp(in->symbol, "rt_memcmp_raw") != 0)) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare memory operation expects three arguments");
    return false;
  }
  if (strcmp(in->symbol, "rt_memcpy_raw") == 0) {
    if (!ny_a64_u32(c, 0xaa0003e3u) || /* mov x3,x0 */
        !ny_a64_u32(c, 0xaa0103e4u) || /* mov x4,x1 */
        !ny_a64_u32(c, 0xf100005fu))   /* cmp x2,#0 */
      return false;
    size_t empty_branch = c->code.len;
    if (!ny_a64_u32(c, 0x5400000du)) /* b.le done */
      return false;
    size_t loop = c->code.len;
    if (!ny_a64_u32(c, 0x39400086u) || /* ldrb w6,[x4] */
        !ny_a64_u32(c, 0x39000066u) || /* strb w6,[x3] */
        !ny_a64_u32(c, 0x91000463u) || /* add x3,x3,#1 */
        !ny_a64_u32(c, 0x91000484u) || /* add x4,x4,#1 */
        !ny_a64_u32(c, 0xf1000442u))   /* subs x2,x2,#1 */
      return false;
    size_t loop_branch = c->code.len;
    if (!ny_a64_u32(c, 0x54000001u)) /* b.ne loop */
      return false;
    size_t done = c->code.len;
    if (!ny_a64_patch_branch(c, empty_branch, done, true) ||
        !ny_a64_patch_branch(c, loop_branch, loop, true))
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
  }
  if (!ny_a64_u32(c, 0xaa0003e4u) || /* mov x4,x0 */
      !ny_a64_u32(c, 0xaa0103e5u) || /* mov x5,x1 */
      !ny_a64_u32(c, 0xf100005fu))   /* cmp x2,#0 */
    return false;
  size_t empty_branch = c->code.len;
  if (!ny_a64_u32(c, 0x5400000du)) /* b.le empty */
    return false;
  size_t loop = c->code.len;
  if (!ny_a64_u32(c, 0x39400086u) || /* ldrb w6,[x4] */
      !ny_a64_u32(c, 0x394000a7u) || /* ldrb w7,[x5] */
      !ny_a64_u32(c, 0x6b0700dfu))   /* cmp w6,w7 */
    return false;
  size_t mismatch_branch = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne mismatch */
      !ny_a64_u32(c, 0x91000484u) || /* add x4,x4,#1 */
      !ny_a64_u32(c, 0x910004a5u) || /* add x5,x5,#1 */
      !ny_a64_u32(c, 0xf1000442u))   /* subs x2,x2,#1 */
    return false;
  size_t loop_branch = c->code.len;
  if (!ny_a64_u32(c, 0x5400000cu)) /* b.gt loop */
    return false;
  size_t equal = c->code.len;
  if (!ny_a64_u32(c, 0xaa1f03e0u) || /* mov x0,xzr */
      !ny_a64_u32(c, 0x14000000u))
    return false;
  size_t equal_done_branch = c->code.len - sizeof(uint32_t);
  size_t mismatch = c->code.len;
  if (!ny_a64_u32(c, 0x4b0700c0u) || /* sub w0,w6,w7 */
      !ny_a64_u32(c, 0x93407c00u))   /* sxtw x0,w0 */
    return false;
  size_t done = c->code.len;
  if (!ny_a64_patch_branch(c, empty_branch, equal, true) ||
      !ny_a64_patch_branch(c, mismatch_branch, mismatch, true) ||
      !ny_a64_patch_branch(c, loop_branch, loop, true) ||
      !ny_a64_patch_branch(c, equal_done_branch, done, false))
    return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

static bool ny_a64_inline_bare_tbuf_clone(ny_a64_obj_ctx_t *c,
                                          const nyr_inst_t *in, int argc) {
  if (!c || !in || argc != 1 || !in->symbol ||
      strcmp(in->symbol, "rt_tbuf_clone_raw") != 0) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare typed-buffer clone expects one argument");
    return false;
  }
  if (!ny_a64_u32(c, 0xaa0003e8u) ||                           /* mov x8,x0 */
      !ny_a64_mov_imm(c, 0, 0) || !ny_a64_u32(c, 0xf100011fu)) /* cmp x8,#0 */
    return false;
  size_t invalid = c->code.len;
  if (!ny_a64_u32(c, 0x54000000u) || /* b.eq invalid */
      !ny_a64_u32(c, 0xd1008109u) || /* sub x9,x8,#32 */
      !ny_a64_reg_mem_base(c, true, 10, 9, 0) ||
      !ny_a64_mov_imm(c, 11, INT64_C(0x4e59544255464d47)) ||
      !ny_a64_u32(c, 0xeb0b015fu)) /* cmp x10,x11 */
    return false;
  size_t bad_magic = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne invalid */
      !ny_a64_reg_mem_base(c, true, 12, 9, 8) ||
      !ny_a64_u32(c, 0xf100019fu)) /* cmp x12,#0 */
    return false;
  size_t bad_count = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt invalid */
      !ny_a64_reg_mem_base(c, true, 13, 9, 16) ||
      !ny_a64_u32(c, 0xf10001bfu)) /* cmp x13,#0 */
    return false;
  size_t bad_elem_size = c->code.len;
  if (!ny_a64_u32(c, 0x5400000du) || /* b.le invalid */
      !ny_a64_reg_mem_base(c, true, 14, 9, 24) ||
      !ny_a64_u32(c, 0xeb0c01dfu)) /* cmp x14,x12 */
    return false;
  size_t bad_capacity = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt invalid */
      !ny_a64_u32(c, 0x9b0d7dcfu) || /* mul x15,x14,x13 */
      !ny_a64_u32(c, 0x9bcd7dd0u) || /* umulh x16,x14,x13 */
      !ny_a64_u32(c, 0xf100021fu))   /* cmp x16,#0 */
    return false;
  size_t multiply_overflow = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne invalid */
      !ny_a64_u32(c, 0xb10081efu))   /* adds x15,x15,#32 */
    return false;
  size_t length_overflow = c->code.len;
  if (!ny_a64_u32(c, 0x54000002u) || /* b.cs invalid */
      !ny_a64_u32(c, 0xd10043ffu) || /* sub sp,sp,#16 */
      !ny_a64_reg_mem_base(c, false, 9, 31, 0) ||
      !ny_a64_reg_mem_base(c, false, 15, 31, 8) || !ny_a64_mov_imm(c, 0, 0) ||
      !ny_a64_u32(c, 0xaa0f03e1u) || /* mov x1,x15 */
      !ny_a64_mov_imm(c, 2, 3) || !ny_a64_mov_imm(c, 3, 34) ||
      !ny_a64_mov_imm(c, 4, -1) || !ny_a64_mov_imm(c, 5, 0) ||
      !ny_a64_mov_imm(c, 8, c->target->syscalls.mmap_nr) ||
      !ny_a64_u32(c, 0xd4000001u) ||
      !ny_a64_u32(c, 0xf100001fu)) /* cmp x0,#0 */
    return false;
  size_t mmap_failure = c->code.len;
  if (!ny_a64_u32(c, 0x54000004u) || /* b.mi mmap_failure */
      !ny_a64_u32(c, 0xaa0003edu) || /* mov x13,x0 */
      !ny_a64_reg_mem_base(c, true, 9, 31, 0) ||
      !ny_a64_reg_mem_base(c, true, 10, 31, 8) || !ny_a64_mov_imm(c, 11, 0))
    return false;
  size_t copy_loop = c->code.len;
  if (!ny_a64_u32(c, 0xeb0a017fu)) /* cmp x11,x10 */
    return false;
  size_t copy_done = c->code.len;
  if (!ny_a64_u32(c, 0x54000002u) || /* b.hs copy_done */
      !ny_a64_u32(c, 0x8b0b012eu) || /* add x14,x9,x11 */
      !ny_a64_u32(c, 0x394001ccu) || /* ldrb w12,[x14] */
      !ny_a64_u32(c, 0x8b0b01afu) || /* add x15,x13,x11 */
      !ny_a64_u32(c, 0x390001ecu) || /* strb w12,[x15] */
      !ny_a64_u32(c, 0x9100056bu))   /* add x11,x11,#1 */
    return false;
  size_t copy_branch = c->code.len;
  if (!ny_a64_u32(c, 0x14000000u))
    return false;
  size_t copy_finish = c->code.len;
  if (!ny_a64_u32(c, 0x910081a0u) || /* add x0,x13,#32 */
      !ny_a64_u32(c, 0x910043ffu))   /* add sp,sp,#16 */
    return false;
  size_t success_done_branch = c->code.len;
  if (!ny_a64_u32(c, 0x14000000u))
    return false;
  size_t map_fail = c->code.len;
  if (!ny_a64_u32(c, 0x910043ffu) || !ny_a64_mov_imm(c, 0, 0))
    return false;
  size_t done = c->code.len;
  if (!ny_a64_patch_branch(c, invalid, done, true) ||
      !ny_a64_patch_branch(c, bad_magic, done, true) ||
      !ny_a64_patch_branch(c, bad_count, done, true) ||
      !ny_a64_patch_branch(c, bad_elem_size, done, true) ||
      !ny_a64_patch_branch(c, bad_capacity, done, true) ||
      !ny_a64_patch_branch(c, multiply_overflow, done, true) ||
      !ny_a64_patch_branch(c, length_overflow, done, true) ||
      !ny_a64_patch_branch(c, mmap_failure, map_fail, true) ||
      !ny_a64_patch_branch(c, copy_done, copy_finish, true) ||
      !ny_a64_patch_branch(c, copy_branch, copy_loop, false) ||
      !ny_a64_patch_branch(c, success_done_branch, done, false))
    return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

static bool ny_a64_inline_bare_tbuf_len(ny_a64_obj_ctx_t *c,
                                        const nyr_inst_t *in, int argc) {
  if (!c || !in || argc != 1 || !in->symbol ||
      strcmp(in->symbol, "rt_tbuf_len_raw") != 0)
    return false;
  if (!ny_a64_u32(c, 0xaa0003e8u) ||                           /* mov x8,x0 */
      !ny_a64_mov_imm(c, 0, 0) || !ny_a64_u32(c, 0xf100011fu)) /* cmp x8,#0 */
    return false;
  size_t invalid = c->code.len;
  if (!ny_a64_u32(c, 0x54000000u) || /* b.eq invalid */
      !ny_a64_u32(c, 0xd1008109u) || /* sub x9,x8,#32 */
      !ny_a64_reg_mem_base(c, true, 10, 9, 0) ||
      !ny_a64_mov_imm(c, 11, INT64_C(0x4e59544255464d47)) ||
      !ny_a64_u32(c, 0xeb0b015fu)) /* cmp x10,x11 */
    return false;
  size_t bad_magic = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne invalid */
      !ny_a64_reg_mem_base(c, true, 10, 9, 8) ||
      !ny_a64_u32(c, 0xf100015fu)) /* cmp x10,#0 */
    return false;
  size_t bad_count = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt invalid */
      !ny_a64_reg_mem_base(c, true, 11, 9, 16) ||
      !ny_a64_u32(c, 0xf100017fu)) /* cmp x11,#0 */
    return false;
  size_t bad_elem_size = c->code.len;
  if (!ny_a64_u32(c, 0x5400000du) || /* b.le invalid */
      !ny_a64_reg_mem_base(c, true, 12, 9, 24) ||
      !ny_a64_u32(c, 0xeb0a019fu)) /* cmp x12,x10 */
    return false;
  size_t bad_capacity = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt invalid */
      !ny_a64_u32(c, 0xaa0a03e0u))
    return false;
  size_t done_branch = c->code.len;
  if (!ny_a64_u32(c, 0x14000000u))
    return false;
  size_t done = c->code.len;
  return ny_a64_patch_branch(c, invalid, done, true) &&
         ny_a64_patch_branch(c, bad_magic, done, true) &&
         ny_a64_patch_branch(c, bad_count, done, true) &&
         ny_a64_patch_branch(c, bad_elem_size, done, true) &&
         ny_a64_patch_branch(c, bad_capacity, done, true) &&
         ny_a64_patch_branch(c, done_branch, done, false) &&
         (in->dst < 0 || ny_a64_store_value(c, in->dst, 0));
}

static bool ny_a64_inline_bare_tbuf_pop(ny_a64_obj_ctx_t *c,
                                        const nyr_inst_t *in, int argc) {
  bool dynamic = in && in->symbol && strcmp(in->symbol, "rt_tbuf_pop_any") == 0;
  if (!c || !in || argc != 1 || !in->symbol ||
      (!dynamic && strcmp(in->symbol, "rt_tbuf_pop_raw") != 0))
    return false;
  /*
   * Bare raw pop supports i64 elements only. Invalid buffers, empty lists,
   * malformed headers, and other element strides return the runtime's zero
   * result without mutating the buffer.
   */
  if (!ny_a64_u32(c, 0xaa0003e8u) || /* mov x8,x0 */
      !ny_a64_mov_imm(c, 0, 0) || !ny_a64_u32(c, 0xf100011fu))
    return false; /* cmp x8,#0 */
  size_t invalid[5];
  if (!ny_a64_u32(c, 0x54000000u)) /* b.eq done */
    return false;
  invalid[0] = c->code.len - sizeof(uint32_t);
  if (!ny_a64_u32(c, 0xd1008109u) || /* sub x9,x8,#32 */
      !ny_a64_reg_mem_base(c, true, 10, 9, 0) ||
      !ny_a64_mov_imm(c, 11, INT64_C(0x4e59544255464d47)) ||
      !ny_a64_u32(c, 0xeb0b015fu)) /* cmp x10,x11 */
    return false;
  invalid[1] = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne done */
      !ny_a64_reg_mem_base(c, true, 12, 9, 8) ||
      !ny_a64_u32(c, 0xf100019fu)) /* cmp x12,#0 */
    return false;
  invalid[2] = c->code.len;
  if (!ny_a64_u32(c, 0x5400000du) || /* b.le done */
      !ny_a64_reg_mem_base(c, true, 13, 9, 16) ||
      !ny_a64_u32(c, 0xd280010eu) || /* mov x14,#8 */
      !ny_a64_u32(c, 0xeb0e01bfu))   /* cmp x13,x14 */
    return false;
  invalid[3] = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne done */
      !ny_a64_reg_mem_base(c, true, 14, 9, 24) ||
      !ny_a64_u32(c, 0xeb0c01dfu)) /* cmp x14,x12 */
    return false;
  invalid[4] = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt done */
      !ny_a64_u32(c, 0xd100058cu) || /* sub x12,x12,#1 */
      !ny_a64_u32(c, 0x8b0c0d0eu) || /* add x14,x8,x12,lsl #3 */
      !ny_a64_reg_mem_base(c, true, 0, 14, 0) ||
      !ny_a64_u32(c, 0xaa1f03efu) || /* mov x15,xzr */
      !ny_a64_reg_mem_base(c, false, 15, 14, 0) ||
      !ny_a64_reg_mem_base(c, false, 12, 9, 8))
    return false;
  if (dynamic && (!ny_a64_u32(c, 0x8b000000u) || /* add x0,x0,x0 */
                  !ny_a64_u32(c, 0xb2400001u)))  /* orr x0,x0,#1 */
    return false;
  size_t done = c->code.len;
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    if (!ny_a64_patch_branch(c, invalid[i], done, true))
      return false;
  return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
}

static bool ny_a64_inline_bare_tbuf_index(ny_a64_obj_ctx_t *c,
                                          const nyr_inst_t *in, int argc) {
  if (!c || !in || argc != 2 || !in->symbol ||
      strcmp(in->symbol, "rt_tbuf_index_read_raw") != 0)
    return false;
  /*
   * Bare raw indexing covers scalar i64 buffers. Invalid headers, non-i64
   * strides, and out-of-range indices terminate instead of becoming unchecked
   * memory accesses or returning a misleading value.
   */
  if (!ny_a64_u32(c, 0xaa0003e8u) || /* mov x8,x0 */
      !ny_a64_u32(c, 0xaa0103e9u) || /* mov x9,x1 */
      !ny_a64_u32(c, 0xf100011fu))   /* cmp x8,#0 */
    return false;
  size_t null_buffer = c->code.len;
  if (!ny_a64_u32(c, 0x54000000u) || /* b.eq error */
      !ny_a64_u32(c, 0xd100810au) || /* sub x10,x8,#32 */
      !ny_a64_reg_mem_base(c, true, 11, 10, 0) ||
      !ny_a64_mov_imm(c, 12, INT64_C(0x4e59544255464d47)) ||
      !ny_a64_u32(c, 0xeb0c017fu)) /* cmp x11,x12 */
    return false;
  size_t bad_magic = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne error */
      !ny_a64_reg_mem_base(c, true, 12, 10, 8) ||
      !ny_a64_u32(c, 0xf100019fu)) /* cmp x12,#0 */
    return false;
  size_t bad_count = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt error */
      !ny_a64_reg_mem_base(c, true, 13, 10, 16) ||
      !ny_a64_u32(c, 0xf10001bfu)) /* cmp x13,#0 */
    return false;
  size_t bad_stride = c->code.len;
  if (!ny_a64_u32(c, 0x5400000du) || /* b.le error */
      !ny_a64_mov_imm(c, 14, 8) ||
      !ny_a64_u32(c, 0xeb0e01bfu)) /* cmp x13,x14 */
    return false;
  size_t non_i64 = c->code.len;
  if (!ny_a64_u32(c, 0x54000001u) || /* b.ne error */
      !ny_a64_reg_mem_base(c, true, 14, 10, 24) ||
      !ny_a64_u32(c, 0xeb0c01dfu)) /* cmp x14,x12 */
    return false;
  size_t bad_capacity = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt error */
      !ny_a64_u32(c, 0xf100013fu))   /* cmp x9,#0 */
    return false;
  size_t nonnegative_index = c->code.len;
  if (!ny_a64_u32(c, 0x5400000au) || /* b.ge normalized */
      !ny_a64_u32(c, 0x8b0c0129u) || /* add x9,x9,x12 */
      !ny_a64_u32(c, 0xf100013fu))   /* cmp x9,#0 */
    return false;
  size_t negative_oob = c->code.len;
  if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt error */
      !ny_a64_u32(c, 0xeb0c013fu))   /* cmp x9,x12 */
    return false;
  size_t normalized = c->code.len;
  if (!ny_a64_u32(c, 0xeb0c013fu)) /* cmp x9,x12 */
    return false;
  size_t high_oob = c->code.len;
  if (!ny_a64_u32(c, 0x5400000au) || /* b.ge error */
      !ny_a64_u32(c, 0x8b090d0eu) || /* add x14,x8,x9,lsl #3 */
      !ny_a64_reg_mem_base(c, true, 0, 14, 0))
    return false;
  size_t success_branch = c->code.len;
  if (!ny_a64_u32(c, 0x14000000u))
    return false;
  size_t error = c->code.len;
  if (!ny_a64_mov_imm(c, 0, 1) ||
      !ny_a64_mov_imm(c, 8, c->target->syscalls.exit_nr) ||
      !ny_a64_u32(c, 0xd4000001u))
    return false;
  size_t finish = c->code.len;
  return ny_a64_patch_branch(c, null_buffer, error, true) &&
         ny_a64_patch_branch(c, bad_magic, error, true) &&
         ny_a64_patch_branch(c, bad_count, error, true) &&
         ny_a64_patch_branch(c, bad_stride, error, true) &&
         ny_a64_patch_branch(c, non_i64, error, true) &&
         ny_a64_patch_branch(c, bad_capacity, error, true) &&
         ny_a64_patch_branch(c, nonnegative_index, normalized, true) &&
         ny_a64_patch_branch(c, negative_oob, error, true) &&
         ny_a64_patch_branch(c, high_oob, error, true) &&
         ny_a64_patch_branch(c, success_branch, finish, false) &&
         (in->dst < 0 || ny_a64_store_value(c, in->dst, 0));
}

static bool ny_a64_inline_bare_io(ny_a64_obj_ctx_t *c, const nyr_inst_t *in,
                                  int argc) {
  if (!c || !in || !in->symbol)
    return false;
  ny_native_runtime_op_t operation = ny_native_runtime_operation(in->symbol);
  if (operation == NY_NATIVE_RUNTIME_OP_CSTR_LEN) {
    if (argc != 1) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 bare C-string length expects one argument");
      return false;
    }
    if (!ny_a64_u32(c, 0xaa1f03e1u))
      return false; /* mov x1,xzr */
    size_t null_branch = c->code.len;
    if (!ny_a64_u32(c, 0xb4000000u))
      return false;
    size_t loop = c->code.len;
    if (!ny_a64_u32(c, 0x38616802u)) /* ldrb w2,[x0,x1] */
      return false;
    size_t end_branch = c->code.len;
    if (!ny_a64_u32(c, 0x34000002u) || /* cbz w2,done */
        !ny_a64_u32(c, 0x91000421u) || /* add x1,x1,#1 */
        !ny_a64_u32(c, 0x14000000u))
      return false;
    size_t loop_branch = c->code.len - sizeof(uint32_t);
    size_t done = c->code.len;
    if (!ny_a64_patch_branch(c, null_branch, done, true) ||
        !ny_a64_patch_branch(c, end_branch, done, true) ||
        !ny_a64_patch_branch(c, loop_branch, loop, false) ||
        !ny_a64_u32(c, 0xaa0103e0u)) /* mov x0,x1 */
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
  }
  if (operation == NY_NATIVE_RUNTIME_OP_WRITE_OFF) {
    if (argc != 4) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 bare write expects four arguments");
      return false;
    }
    size_t invalid_branches[3];
    if (!ny_a64_u32(c, 0xf100001fu)) /* cmp x0,#0 */
      return false;
    invalid_branches[0] = c->code.len;
    if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt invalid */
        !ny_a64_u32(c, 0xf100005fu))   /* cmp x2,#0 */
      return false;
    invalid_branches[1] = c->code.len;
    if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt invalid */
        !ny_a64_u32(c, 0xf100007fu))   /* cmp x3,#0 */
      return false;
    invalid_branches[2] = c->code.len;
    if (!ny_a64_u32(c, 0x5400000bu) || /* b.lt invalid */
        !ny_a64_u32(c, 0x8b030021u) || /* add x1,x1,x3 */
        !ny_a64_mov_imm(c, 8, c->target->syscalls.write_nr) ||
        !ny_a64_u32(c, 0xd4000001u) || !ny_a64_u32(c, 0x14000000u))
      return false;
    size_t done_branch = c->code.len - sizeof(uint32_t);
    size_t invalid = c->code.len;
    if (!ny_a64_mov_imm(c, 0, -1))
      return false;
    size_t done = c->code.len;
    if (!ny_a64_patch_branch(c, invalid_branches[0], invalid, true) ||
        !ny_a64_patch_branch(c, invalid_branches[1], invalid, true) ||
        !ny_a64_patch_branch(c, invalid_branches[2], invalid, true) ||
        !ny_a64_patch_branch(c, done_branch, done, false))
      return false;
    return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
  }
  return false;
}

static bool ny_a64_inline_bare_syscall(ny_a64_obj_ctx_t *c,
                                       const nyr_inst_t *in, int argc) {
  if (!c || !in || !c->target || !c->target->syscalls.available) {
    nyr_set_error(c ? c->err : NULL, c ? c->err_len : 0,
                  "AArch64 bare raw syscall requires a Linux target");
    return false;
  }
  if (argc != 7) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 bare raw syscall expects seven arguments");
    return false;
  }
  /*
   * AAPCS arguments are x0..x6; Linux takes nr in x8 and args in x0..x5.
   */
  if (!ny_a64_u32(c, 0xaa0003e8u)) /* mov x8,x0 */
    return false;
  for (unsigned arg = 1; arg <= 6; ++arg)
    if (!ny_a64_u32(c, 0xaa0003e9u | (arg << 16)) || /* mov x9,xN */
        !ny_a64_u32(c, 0xaa0903e0u | (arg - 1)))     /* mov xN-1,x9 */
      return false;
  return ny_a64_u32(c, 0xd4000001u) && /* svc #0 */
         (in->dst < 0 || ny_a64_store_value(c, in->dst, 0));
}

static bool ny_a64_emit_inst(ny_a64_obj_ctx_t *c, const nyr_inst_t *in) {
  switch (in->op) {
  case NYIR_NOP:
    return true;
  case NYIR_CONST_I64:
    return ny_a64_mov_imm(c, 0, in->imm) && ny_a64_store_value(c, in->dst, 0);
  case NYIR_COPY:
    if (ny_a64_value_is_v128(c, in->dst))
      return ny_a64_load_vec_value(c, 0, in->a) &&
             ny_a64_store_vec_value(c, in->dst, 0);
    return ny_a64_load_value(c, 0, in->a) && ny_a64_store_value(c, in->dst, 0);
  case NYIR_SELECT_I64:
    return ny_a64_load_value(c, 9, in->a) && ny_a64_load_value(c, 10, in->b) &&
           ny_a64_load_value(c, 11, in->c) &&
           ny_a64_u32(c, 0xF100013Fu) && /* cmp x9, #0 */
           ny_a64_u32(c, 0x9A8B1140u) && /* csel x0, x10, x11, ne */
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_ADD_I64:
    return ny_a64_binop(c, in, 0x8b000000u);
  case NYIR_SUB_I64:
    return ny_a64_binop(c, in, 0xcb000000u);
  case NYIR_MUL_I64:
    return ny_a64_binop(c, in, 0x9b007c00u);
  case NYIR_AND_I64:
    return ny_a64_binop(c, in, 0x8a000000u);
  case NYIR_OR_I64:
    return ny_a64_binop(c, in, 0xaa000000u);
  case NYIR_XOR_I64:
    return ny_a64_binop(c, in, 0xca000000u);
  case NYIR_SHL_I64:
    return ny_a64_binop(c, in, 0x9ac02000u);
  case NYIR_SAR_I64:
    return ny_a64_binop(c, in, 0x9ac02800u);
  case NYIR_ROR32_I64:
    return ny_a64_binop(c, in, 0x1ac02c00u);
  case NYIR_ROR_I64:
    return ny_a64_binop(c, in, 0x9ac02c00u);
  case NYIR_BSWAP_I64:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0xdac00c00u) && /* rev x0,x0 */
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_BSWAP32_I64:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0x5ac00800u) && /* rev w0,w0; zero-extends x0 */
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_DIV_I64:
    return ny_a64_binop(c, in, 0x9ac00c00u);
  case NYIR_MOD_I64:
    return ny_a64_load_value(c, 0, in->a) && ny_a64_load_value(c, 1, in->b) &&
           ny_a64_u32(c, 0x9ac10c02u) && /* sdiv x2,x0,x1 */
           ny_a64_u32(c, 0x9b018040u) && /* msub x0,x2,x1,x0 */
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_CMP_I64: {
    unsigned inverse = ny_a64_cond(in->cmp) ^ 1u;
    return ny_a64_load_value(c, 0, in->a) && ny_a64_load_value(c, 1, in->b) &&
           ny_a64_u32(c, 0xeb01001fu) &&
           ny_a64_u32(c, 0x9a9f07e0u | (inverse << 12)) &&
           ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_LABEL:
    return ny_a64_add_label(c, in->imm);
  case NYIR_LOAD_LOCAL:
    if (ny_a64_value_is_v256(c, in->dst) ||
        ny_a64_local_is_v256(c, (int)in->imm))
      return ny_a64_q_mem(c, true, 0, ny_a64_local_off(c, (int)in->imm)) &&
             ny_a64_q_mem(c, true, 1, ny_a64_local_off(c, (int)in->imm) + 16) &&
             ny_a64_store_vec4_half(c, in->dst, 0, 0) &&
             ny_a64_store_vec4_half(c, in->dst, 1, 1);
    if (ny_a64_value_is_v128(c, in->dst) ||
        ny_a64_local_is_v128(c, (int)in->imm))
      return ny_a64_q_mem(c, true, 0, ny_a64_local_off(c, (int)in->imm)) &&
             ny_a64_store_vec_value(c, in->dst, 0);
    return ny_a64_load_local(c, 0, (int)in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_STORE_LOCAL:
    if (ny_a64_value_is_v256(c, in->a) || ny_a64_local_is_v256(c, (int)in->imm))
      return ny_a64_load_vec4_half(c, 0, in->a, 0) &&
             ny_a64_load_vec4_half(c, 1, in->a, 1) &&
             ny_a64_q_mem(c, false, 0, ny_a64_local_off(c, (int)in->imm)) &&
             ny_a64_q_mem(c, false, 1, ny_a64_local_off(c, (int)in->imm) + 16);
    if (ny_a64_value_is_v128(c, in->a) || ny_a64_local_is_v128(c, (int)in->imm))
      return ny_a64_load_vec_value(c, 0, in->a) &&
             ny_a64_q_mem(c, false, 0, ny_a64_local_off(c, (int)in->imm));
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_store_local(c, (int)in->imm, 0);
  case NYIR_ADDR_LOCAL: {
    if (in->imm < 0 || in->imm >= c->local_slots)
      return false;
    int off = ny_a64_local_off(c, (int)in->imm);
    if (off > 4095)
      return false;
    return ny_a64_u32(c, 0x910003e0u | ((uint32_t)off << 10)) &&
           ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_ADDR_SYMBOL: {
    if (!in->symbol || !*in->symbol)
      return false;
    /*
     * adrp x0, symbol@PAGE; add x0, x0, symbol@PAGEOFF
     */
    if (!ny_a64_record_reloc(c, in->symbol,
                             NY_RELOC_AARCH64_ADR_PREL_PG_HI21) ||
        !ny_a64_u32(c, 0x90000000u))
      return false;
    if (!ny_a64_record_reloc(c, in->symbol, NY_RELOC_AARCH64_ADD_ABS_LO12_NC) ||
        !ny_a64_u32(c, 0x91000000u))
      return false;
    return ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_LOAD_I64:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, (in->flags & NYIR_INST_F_MEM_BYTE)
                             ? 0x39400000u /* ldrb w0, [x0] */
                         : (in->flags & NYIR_INST_F_MEM_U16)
                             ? 0x79400000u /* ldrh w0, [x0] */
                         : (in->flags & NYIR_INST_F_MEM_I32)
                             ? 0xb9800000u /* ldrsw x0, [x0] */
                             : 0xf9400000u) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_STORE_I64:
    return ny_a64_load_value(c, 0, in->a) && ny_a64_load_value(c, 1, in->c) &&
           ny_a64_u32(c, (in->flags & NYIR_INST_F_MEM_BYTE)
                             ? 0x39000001u /* strb w1, [x0] */
                         : (in->flags & NYIR_INST_F_MEM_U16)
                             ? 0x79000001u /* strh w1, [x0] */
                         : (in->flags & NYIR_INST_F_MEM_I32)
                             ? 0xb9000001u /* str w1, [x0] */
                             : 0xf9000001u);
  case NYIR_CONST_F64:
    return ny_a64_mov_imm(c, 0, in->imm) && ny_a64_store_value(c, in->dst, 0);
  case NYIR_CONST_F32:
    return ny_a64_mov_imm(c, 0, (uint32_t)in->imm) &&
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_F64_TO_I64:
    return ny_a64_load_fp_value(c, 0, in->a, false) &&
           ny_a64_u32(c, 0x9e780000u) && /* fcvtzs x0, d0 */
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_F32_TO_I64:
    return ny_a64_load_fp_value(c, 0, in->a, true) &&
           ny_a64_u32(c, 0x9e380000u) && /* fcvtzs x0, s0 */
           ny_a64_store_value(c, in->dst, 0);
  case NYIR_ADD_F64:
    return ny_a64_fp_binop(c, in, 0x1e602800u, false);
  case NYIR_SUB_F64:
    return ny_a64_fp_binop(c, in, 0x1e603800u, false);
  case NYIR_MUL_F64:
    return ny_a64_fp_binop(c, in, 0x1e600800u, false);
  case NYIR_DIV_F64:
    return ny_a64_fp_binop(c, in, 0x1e601800u, false);
  case NYIR_ADD_F32:
    return ny_a64_fp_binop(c, in, 0x1e202800u, true);
  case NYIR_SUB_F32:
    return ny_a64_fp_binop(c, in, 0x1e203800u, true);
  case NYIR_MUL_F32:
    return ny_a64_fp_binop(c, in, 0x1e200800u, true);
  case NYIR_DIV_F32:
    return ny_a64_fp_binop(c, in, 0x1e201800u, true);
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
  case NYIR_VEC8_LOAD_F32:
    return ny_a64_load_value(c, 0, in->a) && ny_a64_u32(c, 0x3dc00000u) &&
           ny_a64_store_vec_value(c, in->dst, 0);
  case NYIR_VEC4_LOAD_F64:
    /*
     * Four lanes: two 16-byte loads from [addr] and [addr, #16].
     */
    return ny_a64_load_value(c, 0, in->a) && ny_a64_u32(c, 0x3dc00000u) &&
           ny_a64_u32(c, 0x3dc00401u) &&
           ny_a64_store_vec4_half(c, in->dst, 0, 0) &&
           ny_a64_store_vec4_half(c, in->dst, 1, 1);
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_STORE_F32:
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_load_vec_value(c, 1, in->b >= 0 ? in->b : in->c) &&
           ny_a64_u32(c, 0x3d800001u);
  case NYIR_VEC4_STORE_F64:
    return ny_a64_load_vec4_half(c, 0, in->b >= 0 ? in->b : in->c, 0) &&
           ny_a64_load_vec4_half(c, 1, in->b >= 0 ? in->b : in->c, 1) &&
           ny_a64_load_value(c, 0, in->a) && ny_a64_u32(c, 0x3d800000u) &&
           ny_a64_u32(c, 0x3d800401u);
  case NYIR_VEC4_SET1_I64:
    /*
     * dup v0.2d, x0
     */
    return ny_a64_load_value(c, 0, in->a) && ny_a64_u32(c, 0x4e080c00u) &&
           ny_a64_store_vec_value(c, in->dst, 0);
  case NYIR_VEC8_SET1_I64:
    /*
     * dup v0.2d, x0; copy both 128-bit halves into the 256-bit home.
     */
    return ny_a64_load_value(c, 0, in->a) &&
           ny_a64_u32(c, 0x4e080c00u) &&
           ny_a64_store_vec4_half(c, in->dst, 0, 0) &&
           ny_a64_store_vec4_half(c, in->dst, 1, 0);
  case NYIR_VEC4_SET1_F64:
    /*
     * Broadcast one scalar across both halves: dup v0.2d, v0.d[0], then
     * orr v1.16b, v0.16b, v0.16b.
     */
    return ny_a64_load_fp_value(c, 0, in->a, false) &&
           ny_a64_u32(c, 0x4e080400u) && ny_a64_u32(c, 0x4ea01c01u) &&
           ny_a64_store_vec4_half(c, in->dst, 0, 0) &&
           ny_a64_store_vec4_half(c, in->dst, 1, 1);
  case NYIR_VEC8_SET1_F32:
    return ny_a64_load_fp_value(c, 0, in->a, true) &&
           ny_a64_u32(c, 0x4e040400u) && ny_a64_store_vec_value(c, in->dst, 0);
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
    return ny_a64_vec4_binop(c, in, 0x4e60d400u);
  case NYIR_VEC4_SUB_F64:
    return ny_a64_vec4_binop(c, in, 0x4ee0d400u);
  case NYIR_VEC4_MUL_F64:
    return ny_a64_vec4_binop(c, in, 0x6e60dc00u);
  case NYIR_VEC4_DIV_F64:
    return ny_a64_vec4_binop(c, in, 0x6e60fc00u);
  case NYIR_VEC8_ADD_F32:
    return ny_a64_vec_binop(c, in, 0x4e20d400u);
  case NYIR_VEC8_SUB_F32:
    return ny_a64_vec_binop(c, in, 0x4ea0d400u);
  case NYIR_VEC8_MUL_F32:
    return ny_a64_vec_binop(c, in, 0x6e20dc00u);
  case NYIR_VEC8_DIV_F32:
    return ny_a64_vec_binop(c, in, 0x6e20fc00u);
  case NYIR_VEC4_FMA_F64:
    return ny_a64_vec4_fma(c, in);
  case NYIR_VEC8_FMA_F32:
    return ny_a64_vec_fma(c, in, 0x4e20cc00u);
  case NYIR_VEC4_SHUFFLE_F64:
    return ny_a64_vec4_shuffle(c, in);
  case NYIR_VEC4_REDUCE_ADD_F64:
    return ny_a64_vec4_reduce_add(c, in);
  case NYIR_VEC8_SHUFFLE_F32:
    return ny_a64_vec_shuffle(c, in, 4, 4);
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
    if (!ny_a64_load_vec_value(c, 1, in->a) ||
        !ny_a64_load_vec_value(c, 2, in->b))
      return false;
    if (in->op == NYIR_VEC4_SAR_I64 && !ny_a64_u32(c, 0x6ee0b842u))
      return false;
    return ny_a64_u32(c, 0x4ee24420u) && ny_a64_store_vec_value(c, in->dst, 0);
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
  case NYIR_BR:
    return ny_a64_add_patch(c, in->imm, false);
  case NYIR_BR_IF:
    return ny_a64_load_value(c, 0, in->a) && ny_a64_u32(c, 0xf100001fu) &&
           ny_a64_add_patch(c, in->imm, true);
  case NYIR_CALL: {
    int args[NYIR_CALL_MAX_ARGS];
    int argc = 0;
    if (!nyr_call_args(in, c->value_slots, args, NYIR_CALL_MAX_ARGS, &argc,
                       c->err, c->err_len))
      return false;
    if (c->target && c->target->syscalls.available &&
        ny_native_runtime_operation(in->symbol) ==
            NY_NATIVE_RUNTIME_OP_TBUF_REGISTER_STATIC_HANDLE) {
      if (argc != 1 || !ny_a64_load_value(c, 0, args[0])) {
        nyr_set_error(
            c->err, c->err_len,
            "AArch64 bare static buffer registration expects one argument");
        return false;
      }
      return in->dst < 0 || ny_a64_store_value(c, in->dst, 0);
    }
    if (c->target && c->target->syscalls.available &&
        ny_bare_payload_op(ny_native_runtime_operation(in->symbol), NULL))
      return ny_a64_bare_payload(c, in, args, argc);
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
        if (c0 == NYIR_ARG_CLASS_HFA_F32 || c0 == NYIR_ARG_CLASS_HFA_F64 ||
            c0 == NYIR_ARG_CLASS_HVA_V128) {
          unsigned elem_size = c0 == NYIR_ARG_CLASS_HFA_F32   ? 4u
                               : c0 == NYIR_ARG_CLASS_HFA_F64 ? 8u
                                                              : 16u;
          unsigned count =
              elem_size && size % elem_size == 0 ? size / elem_size : 0;
          chunks[i] = count;
          if (count >= 1 && count <= 4 && fp + count <= 8) {
            kinds[i] = c0 == NYIR_ARG_CLASS_HFA_F32   ? A64_ARG_HFA_F32
                       : c0 == NYIR_ARG_CLASS_HFA_F64 ? A64_ARG_HFA_F64
                                                      : A64_ARG_HVA_V128;
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
        bool floating = size > 0 && size <= 16 && c0 == NYIR_ARG_CLASS_SSE &&
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
      nyr_scalar_class_t arg_class = nyr_call_arg_class_get(in, (size_t)i);
      bool inferred_f64 = c->types.value_f64 && c->types.value_f64[args[i]];
      bool inferred_f32 = c->types.value_f32 && c->types.value_f32[args[i]];
      if ((arg_class == NYIR_SCALAR_F64 && inferred_f32) ||
          (arg_class == NYIR_SCALAR_F32 && inferred_f64) ||
          (arg_class == NYIR_SCALAR_UNKNOWN && (inferred_f64 || inferred_f32)))
        return false;
      bool f64 = arg_class == NYIR_SCALAR_F64;
      bool f32 = arg_class == NYIR_SCALAR_F32;
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
      if (!ny_a64_u32(c, 0x910003efu) || !ny_a64_adjust_sp(c, true, stack_size))
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
            !ny_a64_u32(c, 0x390003eau | ((copy_offs[i] + byte) << 10)))
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
            !ny_a64_fp_mem_base(c, false, true, 16, 31, (int)stack_offs[i]))
          return false;
        break;
      case A64_ARG_STACK_F64:
        if (!ny_a64_fp_mem_base(c, true, false, 16, frame_base, off) ||
            !ny_a64_fp_mem_base(c, false, false, 16, 31, (int)stack_offs[i]))
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
              !ny_a64_u32(c, 0x390003eau | ((stack_offs[i] + byte) << 10)))
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
          bool ok =
              kinds[i] == A64_ARG_HVA_V128
                  ? ny_a64_q_mem_base(c, true, regs[i] + ch, 9, (int)(ch * 16))
                  : ny_a64_fp_mem_base(
                        c, true, kinds[i] == A64_ARG_HFA_F32, regs[i] + ch, 9,
                        (int)(ch * (kinds[i] == A64_ARG_HFA_F32 ? 4 : 8)));
          if (!ok)
            return false;
        }
        break;
      default:
        break;
      }
    }
    if (ny_native_runtime_operation(in->symbol) ==
        NY_NATIVE_RUNTIME_OP_LINUX_SYSCALL)
      return ny_a64_inline_bare_syscall(c, in, argc);
    ny_native_runtime_op_t operation = ny_native_runtime_operation(in->symbol);
    if (operation == NY_NATIVE_RUNTIME_OP_CSTR_EQUAL ||
        operation == NY_NATIVE_RUNTIME_OP_CSTR_COMPARE ||
        operation == NY_NATIVE_RUNTIME_OP_CSTR_CONCAT ||
        operation == NY_NATIVE_RUNTIME_OP_CSTR_TO_STRING ||
        operation == NY_NATIVE_RUNTIME_OP_PRINT_CSTR ||
        operation == NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE ||
        operation == NY_NATIVE_RUNTIME_OP_PRINT_I64 ||
        operation == NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR ||
        operation == NY_NATIVE_RUNTIME_OP_MONOTONIC_NS ||
        operation == NY_NATIVE_RUNTIME_OP_STR_CHR ||
        operation == NY_NATIVE_RUNTIME_OP_STR_REPEAT)
      return ny_a64_inline_bare_print(c, in, argc);
    if (in->symbol && (strcmp(in->symbol, "rt_malloc_i64") == 0 ||
                       strcmp(in->symbol, "rt_malloc_raw_i64") == 0 ||
                       strcmp(in->symbol, "rt_malloc_raw") == 0 ||
                       strcmp(in->symbol, "rt_calloc_raw_i64") == 0 ||
                       strcmp(in->symbol, "rt_calloc_raw") == 0 ||
                       strcmp(in->symbol, "rt_zalloc_raw") == 0 ||
                       strcmp(in->symbol, "rt_malloc") == 0 ||
                       strcmp(in->symbol, "rt_malloc_uninit") == 0 ||
                       strcmp(in->symbol, "rt_free") == 0 ||
                       strcmp(in->symbol, "rt_free_raw") == 0 ||
                       strcmp(in->symbol, "rt_zfree_raw") == 0 ||
                       strcmp(in->symbol, "rt_untag") == 0 ||
                       ny_native_runtime_operation(in->symbol) ==
                           NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_NEW))
      return ny_a64_inline_bare_alloc(c, in, argc);
    if (operation == NY_NATIVE_RUNTIME_OP_MEMSET)
      return ny_a64_inline_bare_memset(c, in, argc);
    if (operation == NY_NATIVE_RUNTIME_OP_MEMCPY ||
        operation == NY_NATIVE_RUNTIME_OP_MEMCMP)
      return ny_a64_inline_bare_memory(c, in, argc);
    if (ny_native_runtime_operation(in->symbol) ==
        NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_CLONE)
      return ny_a64_inline_bare_tbuf_clone(c, in, argc);
    if (ny_native_runtime_operation(in->symbol) ==
        NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_LEN)
      return ny_a64_inline_bare_tbuf_len(c, in, argc);
    if (ny_native_runtime_operation(in->symbol) ==
        NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_POP)
      return ny_a64_inline_bare_tbuf_pop(c, in, argc);
    if (strcmp(in->symbol, "rt_tbuf_pop_any") == 0)
      return ny_a64_inline_bare_tbuf_pop(c, in, argc);
    if (ny_native_runtime_operation(in->symbol) ==
        NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_INDEX_READ)
      return ny_a64_inline_bare_tbuf_index(c, in, argc);
    if (operation == NY_NATIVE_RUNTIME_OP_CSTR_LEN ||
        operation == NY_NATIVE_RUNTIME_OP_WRITE_OFF)
      return ny_a64_inline_bare_io(c, in, argc);
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%s%s%s",
             c->target->symbol_prefix ? c->target->symbol_prefix : "",
             (in->flags & NYIR_INST_F_EXTERN) ? "" : "ny_fn_",
             in->symbol ? in->symbol : "");
    if (!ny_a64_add_reloc(c, symbol))
      return false;
    if (stack_size && !ny_a64_adjust_sp(c, false, stack_size))
      return false;
    if (in->dst < 0)
      return true;
    if (ny_a64_value_is_v128(c, in->dst))
      return ny_a64_store_vec_value(c, in->dst, 0);
    if (in->scalar_class == NYIR_SCALAR_F64)
      return ny_a64_store_fp_value(c, in->dst, 0, false);
    if (in->scalar_class == NYIR_SCALAR_F32)
      return ny_a64_store_fp_value(c, in->dst, 0, true);
    return ny_a64_store_value(c, in->dst, 0);
  }
  case NYIR_RET:
    if (c->nyir->return_aggregate_size &&
        c->nyir->return_aggregate_classes[0] == NYIR_ARG_CLASS_HFA_F64) {
      unsigned count = c->nyir->return_aggregate_size / 8u;
      if (in->a < 0 || count == 0 || count > 4 ||
          !ny_a64_load_value(c, 9, in->a))
        return false;
      for (unsigned piece = 0; piece < count; ++piece)
        if (!ny_a64_fp_mem_base(c, true, false, piece, 9, (int)(piece * 8u)))
          return false;
      if (c->return_count >= c->return_cap)
        return false;
      c->returns[c->return_count++] = c->code.len;
      return ny_a64_u32(c, 0x14000000u);
    }
    if (c->nyir->return_aggregate_size == 16u &&
        (c->nyir->return_aggregate_classes[0] == NYIR_ARG_CLASS_INTEGER ||
         c->nyir->return_aggregate_classes[0] ==
             NYIR_ARG_CLASS_AAPCS_INTEGER_A16) &&
        c->nyir->return_aggregate_classes[1] == NYIR_ARG_CLASS_INTEGER) {
      if (in->a < 0 || in->b < 0 || !ny_a64_load_value(c, 0, in->a) ||
          !ny_a64_load_value(c, 1, in->b))
        return false;
      if (c->return_count >= c->return_cap)
        return false;
      c->returns[c->return_count++] = c->code.len;
      return ny_a64_u32(c, 0x14000000u);
    }
    if (in->a >= 0) {
      bool f64 = c->types.value_f64[in->a];
      bool f32 = c->types.value_f32[in->a];
      if (ny_a64_value_is_v128(c, in->a)) {
        if (!ny_a64_load_vec_value(c, 0, in->a))
          return false;
      } else if ((f64 || f32) ? !ny_a64_load_fp_value(c, 0, in->a, f32)
                              : !ny_a64_load_value(c, 0, in->a)) {
        return false;
      }
    }
    if (c->return_count >= c->return_cap)
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
      nyr_set_error(c->err, c->err_len,
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
      nyr_set_error(c->err, c->err_len,
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
     * Loop: cbz x2, done; ldrb w3,[x1],#1; strb w3,[x0],#1; sub x2,x2,#1; b
     * loop Encode as unrolled for size<=32, else simple byte loop via patches.
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
        if (!ny_a64_u32(c, 0xF9400023u | ((uint32_t)(k / 8)
                                          << 10)) || /* ldr x3,[x1,#k] */
            !ny_a64_u32(c, 0xF9000003u |
                               ((uint32_t)(k / 8) << 10))) /* str x3,[x0,#k] */
          return false;
      }
      return true;
    }
    nyr_set_error(
        c->err, c->err_len,
        "AArch64 object writer: COPY_STRUCT size %" PRId64 " not handled", n);
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
    case 0:
      reg = 1;
      break; /* x1 */
    case 1:
      reg = 0;
      break; /* x0 */
    case 2:  /* d0 as bits — fmov x0, d0 */
      return ny_a64_u32(c, 0x9E660000u) && ny_a64_store_value(c, in->dst, 0);
    case 3:
      return ny_a64_u32(c, 0x9E660020u) && /* fmov x0, d1 */
             ny_a64_store_value(c, in->dst, 0);
    case 4:
    case 5:
    case 6:
    case 7: {
      unsigned v = (unsigned)in->imm - 4u;
      return ny_a64_u32(c, 0x1E260000u | (v << 5)) && /* fmov w0, sN */
             ny_a64_store_value(c, in->dst, 0);
    }
    case 8:
    case 9: {
      unsigned v = (unsigned)in->imm - 6u; /* d2,d3 */
      return ny_a64_u32(c, 0x9E660000u | (v << 5)) &&
             ny_a64_store_value(c, in->dst, 0);
    }
    case 10:
    case 11:
    case 12:
    case 13:
      return ny_a64_store_vec_value(c, in->dst,
                                    (unsigned)in->imm - 10u); /* q0..q3 */
    default:
      nyr_set_error(
          c->err, c->err_len,
          "AArch64 object writer: unsupported return capture selector");
      return false;
    }
    return ny_a64_store_value(c, in->dst, reg);
  }
  case NYIR_BOUNDS_CHECK: {
    /*
     * Inline unsigned trap: cmp x9(value), limit; b.lo ok; exit(1).
     */
    if (!ny_a64_load_value(c, 9, in->b))
      return false;
    if (in->c >= 0) {
      if (!ny_a64_load_value(c, 10, in->c))
        return false;
    } else if (!ny_a64_mov_imm(c, 10, in->imm)) {
      return false;
    }
    /*
     * subs xzr, x9, x10
     */
    if (!ny_a64_u32(c, 0xEB0A013Fu))
      return false;
    /*
     * b.lo +12 (skip the exit sequence): cond=3, imm19=3
     */
    if (!ny_a64_u32(c, 0x54000063u))
      return false;
    /*
     * Linux/AArch64 exit(1). A signal trap leaks emulator diagnostics into
     * captured program output and does not match the bare runtime's checked
     * failure path.
     */
    if (c->target && c->target->syscalls.available)
      return ny_a64_u32(c, 0xd2800020u) && /* mov x0,#1 */
             ny_a64_mov_imm(c, 8, c->target->syscalls.exit_nr) &&
             ny_a64_u32(c, 0xd4000001u); /* svc #0 */
    return ny_a64_u32(c, 0xd4200000u);   /* brk #0 */
  }
  case NYIR_OP_COUNT:
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: unsupported op %s",
                  nyr_op_name(in->op));
    return false;
  default:
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: unsupported op %s",
                  nyr_op_name(in->op));
    return false;
  }
}

static unsigned ny_a64_elf_reloc_type(unsigned type) {
  switch (type) {
  case NY_RELOC_AARCH64_CALL26:
    return 283u; /* R_AARCH64_CALL26 */
  case NY_RELOC_AARCH64_ADR_PREL_PG_HI21:
    return 275u; /* R_AARCH64_ADR_PREL_PG_HI21 */
  case NY_RELOC_AARCH64_ADD_ABS_LO12_NC:
    return 277u; /* R_AARCH64_ADD_ABS_LO12_NC */
  default:
    return type;
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
    const nyr_inst_t *in = &c->nyir->data[i];
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

/*
 * Keep the neutral object writer independent of emit.c, which belongs to the
 * full compiler and is intentionally not linked into the standalone NYR ABI.
 */
static int ny_a64_local_count(const nyr_func_t *f) {
  int64_t max_slot = -1;
  for (size_t i = 0; f && i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > max_slot)
      max_slot = in->imm;
  }
  size_t inferred = max_slot >= 0 ? (size_t)max_slot + 1u : 0u;
  size_t count = f && f->param_count > inferred ? f->param_count : inferred;
  return count <= INT_MAX ? (int)count : -1;
}

static bool ny_a64_spill_aggregate_param(ny_a64_obj_ctx_t *c, int local,
                                         unsigned *gp, unsigned *fp,
                                         unsigned *stack_off) {
  if (!c || !c->nyir || !c->nyir->param_abi || !gp || !fp || !stack_off ||
      local < 0 || (size_t)local >= c->nyir->param_count)
    return false;
  uint32_t packed = c->nyir->param_abi[local];
  unsigned size = NYIR_ARG_AGG_SIZE(packed);
  unsigned c0 = NYIR_ARG_AGG_CLASS(packed, 0);
  int home = c->agg_home_offs ? c->agg_home_offs[local] : -1;
  if (!size)
    return false;
  if (c0 == NYIR_ARG_CLASS_MEMORY) {
    if (*gp < 8) {
      if (!ny_a64_store_local(c, local, (*gp)++))
        return false;
    } else {
      *stack_off = (*stack_off + 7u) & ~7u;
      if (!ny_a64_reg_mem_base(c, true, 16, 29, 16 + (int)*stack_off) ||
          !ny_a64_store_local(c, local, 16))
        return false;
      *stack_off += 8;
    }
    return true;
  }
  if (home < 0)
    return false;
  if (c0 == NYIR_ARG_CLASS_HFA_F64) {
    unsigned count = size / 8u;
    if (count == 0 || count > 4)
      return false;
    if (*fp + count <= 8) {
      for (unsigned piece = 0; piece < count; ++piece)
        if (!ny_a64_fp_mem_base(c, false, false, (*fp)++, 31,
                                home + (int)(piece * 8u)))
          return false;
    } else {
      *fp = 8;
      *stack_off = (*stack_off + 7u) & ~7u;
      for (unsigned piece = 0; piece < count; ++piece)
        if (!ny_a64_fp_mem_base(c, true, false, 16, 29,
                                16 + (int)*stack_off + (int)(piece * 8u)) ||
            !ny_a64_fp_mem_base(c, false, false, 16, 31,
                                home + (int)(piece * 8u)))
          return false;
      *stack_off += (size + 7u) & ~7u;
    }
  } else {
    unsigned chunks = (size + 7u) / 8u;
    if (!chunks || chunks > 2 ||
        (c0 != NYIR_ARG_CLASS_INTEGER &&
         c0 != NYIR_ARG_CLASS_AAPCS_INTEGER_A16 && c0 != NYIR_ARG_CLASS_NONE))
      return false;
    if (c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16 && (*gp & 1u))
      ++*gp;
    if (*gp + chunks <= 8) {
      for (unsigned piece = 0; piece < chunks; ++piece)
        if (!ny_a64_reg_mem_base(c, false, (*gp)++, 31,
                                 home + (int)(piece * 8u)))
          return false;
    } else {
      *gp = 8;
      unsigned align = c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ? 16u : 8u;
      *stack_off = (*stack_off + align - 1u) & ~(align - 1u);
      for (unsigned piece = 0; piece < chunks; ++piece)
        if (!ny_a64_reg_mem_base(c, true, 16, 29,
                                 16 + (int)*stack_off + (int)(piece * 8u)) ||
            !ny_a64_reg_mem_base(c, false, 16, 31, home + (int)(piece * 8u)))
          return false;
      *stack_off += chunks * 8u;
    }
  }
  if (home > 4095 ||
      !ny_a64_u32(c, 0x910003e0u | ((uint32_t)home << 10) | 9u) ||
      !ny_a64_store_local(c, local, 9))
    return false;
  return true;
}

static bool ny_a64_emit_param_spills(ny_a64_obj_ctx_t *c) {
  int count = ny_a64_param_count(c);
  if (count < 0) {
    nyr_set_error(
        c->err, c->err_len,
        "AArch64 object writer: parameter classification allocation failed");
    return false;
  }
  unsigned gp = 0, fp = 0, stack_off = 0;
  for (int local = 0; local < count; ++local) {
    /*
     * AAPCS64 keeps the hidden result pointer in x8; it does not consume
     * x0 or shift the ordinary argument allocation.
     */
    if (local == 0 && c->nyir->return_aggregate_size > 16u &&
        c->nyir->return_aggregate_classes[0] == NYIR_ARG_CLASS_MEMORY) {
      if (!ny_a64_store_local(c, local, 8))
        return false;
      continue;
    }
    if ((size_t)local < c->nyir->param_count && c->nyir->param_types &&
        c->nyir->param_types[local] == NYIR_PARAM_AGGREGATE) {
      if (!ny_a64_spill_aggregate_param(c, local, &gp, &fp, &stack_off))
        return false;
      continue;
    }
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
        if (!ny_a64_fp_mem_base(c, true, f32, 16, 29, 16 + (int)stack_off) ||
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
  c->local_slots = ny_a64_local_count(c->nyir);
  if (c->local_slots < 0) {
    nyr_set_error(c->err, c->err_len,
                  "AArch64 object writer: local slot count exceeds limit");
    return false;
  }
  if (!nyr_type_map_init(&c->types, c->nyir, (size_t)c->local_slots)) {
    nyr_set_error(
        c->err, c->err_len,
        "AArch64 object writer: type classification allocation failed");
    return false;
  }
  if (c->value_slots > 0) {
    c->value_offs = calloc((size_t)c->value_slots, sizeof(*c->value_offs));
    c->alloca_offs = malloc((size_t)c->value_slots * sizeof(*c->alloca_offs));
    if (!c->value_offs || !c->alloca_offs) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 object writer: value layout allocation failed");
      return false;
    }
    for (int i = 0; i < c->value_slots; ++i)
      c->alloca_offs[i] = -1;
  }
  if (c->local_slots > 0) {
    c->local_offs = calloc((size_t)c->local_slots, sizeof(*c->local_offs));
    c->agg_home_offs =
        malloc((size_t)c->local_slots * sizeof(*c->agg_home_offs));
    if (!c->local_offs || !c->agg_home_offs) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 object writer: local layout allocation failed");
      return false;
    }
    for (int local = 0; local < c->local_slots; ++local)
      c->agg_home_offs[local] = -1;
  }
  bool needs_runtime_scratch = false;
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    ny_native_runtime_op_t operation = ny_native_runtime_operation(in->symbol);
    if (in->op == NYIR_CALL && operation == NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR) {
      needs_runtime_scratch = true;
      break;
    }
  }
  int cursor = 0;
  c->runtime_scratch_off = needs_runtime_scratch ? 0 : -1;
  if (needs_runtime_scratch)
    cursor = 32;
  for (int value = 0; value < c->value_slots; ++value) {
    bool v256 = ny_a64_value_is_v256(c, value);
    bool vec = v256 || ny_a64_value_is_v128(c, value);
    cursor = ny_a64_align(cursor, vec ? 16 : 8);
    c->value_offs[value] = cursor;
    cursor += v256 ? 32 : vec ? 16 : 8;
  }
  c->local_base = cursor;
  for (int local = 0; local < c->local_slots; ++local) {
    bool v256 = ny_a64_local_is_v256(c, local);
    bool vec = v256 || ny_a64_local_is_v128(c, local);
    cursor = ny_a64_align(cursor, vec ? 16 : 8);
    c->local_offs[local] = cursor;
    cursor += v256 ? 32 : vec ? 16 : 8;
  }
  for (size_t local = 0;
       local < c->nyir->param_count && local < (size_t)c->local_slots;
       ++local) {
    if (!c->nyir->param_types ||
        c->nyir->param_types[local] != NYIR_PARAM_AGGREGATE)
      continue;
    uint32_t packed = c->nyir->param_abi ? c->nyir->param_abi[local] : 0;
    unsigned size = NYIR_ARG_AGG_SIZE(packed);
    unsigned cls = NYIR_ARG_AGG_CLASS(packed, 0);
    if (!size || cls == NYIR_ARG_CLASS_MEMORY)
      continue;
    unsigned align = cls == NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ? 16u : 8u;
    cursor = ny_a64_align(cursor, (int)align);
    c->agg_home_offs[local] = cursor;
    cursor += (int)ny_a64_align((int)size, 8);
  }
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyr_inst_t *in = &c->nyir->data[i];
    if (in->op != NYIR_ALLOCA || in->dst < 0 || in->dst >= c->value_slots)
      continue;
    int size = in->imm > 0 ? (int)in->imm : 1;
    cursor = ny_a64_align(cursor, 16);
    c->alloca_offs[in->dst] = cursor;
    cursor += ny_a64_align(size, 16);
  }
  c->frame_bytes = ny_a64_align(cursor, 16);
  if (!ny_a64_u32(c, 0xa9bf7bfdu) || !ny_a64_u32(c, 0x910003fdu) ||
      (c->frame_bytes && !ny_a64_adjust_sp(c, true, (unsigned)c->frame_bytes)))
    return false;
  if (user_function && !ny_a64_emit_param_spills(c))
    return false;
  for (size_t i = 0; i < c->nyir->len; ++i)
    if (!ny_a64_emit_inst(c, &c->nyir->data[i]))
      return false;
  size_t epilogue = c->code.len;
  if (tag_return &&
      (!ny_a64_u32(c, 0xd37ff800u) || !ny_a64_u32(c, 0x91000400u)))
    return false;
  if ((c->frame_bytes &&
       !ny_a64_adjust_sp(c, false, (unsigned)c->frame_bytes)) ||
      !ny_a64_u32(c, 0xa8c17bfdu) || !ny_a64_u32(c, 0xd65f03c0u))
    return false;
  for (size_t i = 0; i < c->patch_count; ++i) {
    size_t target = SIZE_MAX;
    for (size_t j = 0; j < c->label_count; ++j)
      if (c->labels[j].label == c->patches[i].label) {
        target = c->labels[j].off;
        break;
      }
    if (target == SIZE_MAX || !ny_a64_patch_branch(c, c->patches[i].off, target,
                                                   c->patches[i].conditional)) {
      nyr_set_error(c->err, c->err_len,
                    "AArch64 object writer: unresolved/out-of-range label %lld",
                    (long long)c->patches[i].label);
      return false;
    }
  }
  for (size_t i = 0; i < c->return_count; ++i)
    if (!ny_a64_patch_branch(c, c->returns[i], epilogue, false))
      return false;
  return true;
}

static int ny_a64_def_index(const ny_a64_def_t *defs, size_t count,
                            const char *name);

static bool ny_a64_append_str_defs(nyr_obj_buf_t *code, ny_a64_def_t *defs,
                                   size_t *def_count,
                                   const ny_a64_reloc_t *relocs,
                                   size_t reloc_count, char *err,
                                   size_t err_len) {
  if (!code || !defs || !def_count)
    return false;
  for (size_t i = 0; i < reloc_count; ++i) {
    const char *name = relocs[i].symbol;
    if (ny_a64_def_index(defs, *def_count, name) >= 0 ||
        strncmp(name, "ny_str_", 7) != 0)
      continue;
    size_t len = 0;
    const char *text = nyr_strtab_get(name, &len);
    if (!text)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS || len > (SIZE_MAX - 1) / 2) {
      nyr_set_error(err, err_len,
                    "AArch64 native string table exceeds object limits");
      return false;
    }
    while (code->len & 7u)
      if (!nyr_obj_u8(code, 0)) {
        nyr_set_error(err, err_len,
                      "AArch64 native string table allocation failed");
        return false;
      }
    if (!nyr_obj_u64(code, (uint64_t)(len * 2u + 1u)) ||
        !nyr_obj_u64(code, 121u)) {
      nyr_set_error(err, err_len,
                    "AArch64 native string table allocation failed");
      return false;
    }
    size_t offset = code->len;
    if (!nyr_obj_emit(code, text, len + 1)) {
      nyr_set_error(err, err_len,
                    "AArch64 native string table allocation failed");
      return false;
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", name);
    defs[*def_count].off = offset;
    defs[*def_count].size = len + 1;
    ++*def_count;
  }
  return true;
}

static bool ny_a64_append_array_defs(nyr_obj_buf_t *code, ny_a64_def_t *defs,
                                     size_t *def_count, char *err,
                                     size_t err_len) {
  if (!code || !defs || !def_count || *def_count > NY_NATIVE_MAX_DEFS)
    return false;
  size_t remaining = NY_NATIVE_MAX_DEFS - *def_count;
  nyr_x64_obj_symbol_def_t *array_defs =
      calloc(remaining ? remaining : 1, sizeof(*array_defs));
  if (!array_defs) {
    nyr_set_error(err, err_len,
                  "AArch64 native array definition allocation failed");
    return false;
  }
  size_t array_count = 0;
  bool ok =
      nyr_arraytab_append_defs(code, array_defs, &array_count, err, err_len);
  if (ok && array_count > remaining) {
    nyr_set_error(err, err_len,
                  "AArch64 native array table exceeds object limits");
    ok = false;
  }
  for (size_t i = 0; ok && i < array_count; ++i) {
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             array_defs[i].name);
    defs[*def_count].off = array_defs[i].off;
    defs[*def_count].size = array_defs[i].size;
    ++*def_count;
  }
  free(array_defs);
  return ok;
}

static int ny_a64_def_index(const ny_a64_def_t *defs, size_t count,
                            const char *name) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(defs[i].name, name) == 0)
      return (int)i;
  return -1;
}

static int ny_a64_name_index(char names[][256], size_t count,
                             const char *name) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(names[i], name) == 0)
      return (int)i;
  return -1;
}

static bool ny_a64_append(nyr_obj_buf_t *code, ny_a64_def_t *defs,
                          size_t *def_count, ny_a64_reloc_t **relocs,
                          size_t *reloc_count, size_t *reloc_cap,
                          const nyr_func_t *nyir,
                          const ny_native_target_info_t *target,
                          const char *symbol, bool user_function,
                          bool tag_return, char *err, size_t err_len) {
  if (*def_count >= NY_NATIVE_MAX_DEFS ||
      ny_a64_def_index(defs, *def_count, symbol) >= 0)
    return false;
  if (!nyr_obj_pad_to(code, 16))
    return false;
  size_t start = code->len;
  ny_a64_obj_ctx_t c = {
      .nyir = nyir, .target = target, .err = err, .err_len = err_len};
  if (!ny_a64_control_flow_init(&c))
    return false;
  if (!ny_a64_emit_code(&c, user_function, tag_return)) {
    free(c.value_offs);
    free(c.local_offs);
    free(c.alloca_offs);
    free(c.agg_home_offs);
    free(c.relocs);
    ny_a64_control_flow_free(&c);
    nyr_type_map_free(&c.types);
    nyr_obj_free(&c.code);
    return false;
  }
  if (!ny_a64_reserve_relocs(relocs, reloc_cap, *reloc_count + c.reloc_count,
                             err, err_len) ||
      !nyr_obj_emit(code, c.code.data, c.code.len)) {
    free(c.value_offs);
    free(c.local_offs);
    free(c.alloca_offs);
    free(c.agg_home_offs);
    free(c.relocs);
    ny_a64_control_flow_free(&c);
    nyr_type_map_free(&c.types);
    nyr_obj_free(&c.code);
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
  free(c.agg_home_offs);
  free(c.relocs);
  ny_a64_control_flow_free(&c);
  nyr_obj_free(&c.code);
  nyr_type_map_free(&c.types);
  return true;
}

typedef struct {
  nyr_obj_buf_t code;
  ny_a64_reloc_t *relocs;
  size_t reloc_count;
  char symbol[256];
  char error[256];
} ny_a64_parallel_result_t;

typedef struct {
  const nyr_func_t *funcs;
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
    nyr_obj_buf_t *code, ny_a64_def_t *defs, size_t *def_count,
    ny_a64_reloc_t **relocs, size_t *reloc_count, size_t *reloc_cap,
    const nyr_func_t *funcs, const char *const *names, size_t func_count,
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
      nyr_obj_free(&results[i].code);
      free(results[i].relocs);
    }
    free(results);
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < func_count && ok; ++i) {
    ny_a64_parallel_result_t *r = &results[i];
    if (*def_count >= NY_NATIVE_MAX_DEFS ||
        !ny_a64_reserve_relocs(relocs, reloc_cap, *reloc_count + r->reloc_count,
                               err, err_len) ||
        !nyr_obj_pad_to(code, 16)) {
      ok = false;
      break;
    }
    size_t start = code->len;
    if (!nyr_obj_emit(code, r->code.data, r->code.len)) {
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
    nyr_obj_free(&results[i].code);
    free(results[i].relocs);
  }
  free(results);
  return ok;
}

static bool ny_a64_elf_sym(nyr_obj_buf_t *b, uint32_t name, unsigned info,
                           uint16_t shndx, uint64_t value, uint64_t size) {
  return nyr_obj_u32(b, name) && nyr_obj_u8(b, info) && nyr_obj_u8(b, 0) &&
         nyr_obj_u16(b, shndx) && nyr_obj_u64(b, value) && nyr_obj_u64(b, size);
}

static bool ny_a64_elf_sh(nyr_obj_buf_t *b, uint32_t name, uint32_t type,
                          uint64_t flags, uint64_t off, uint64_t size,
                          uint32_t link, uint32_t info, uint64_t align,
                          uint64_t entsize) {
  return nyr_obj_u32(b, name) && nyr_obj_u32(b, type) &&
         nyr_obj_u64(b, flags) && nyr_obj_u64(b, 0) && nyr_obj_u64(b, off) &&
         nyr_obj_u64(b, size) && nyr_obj_u32(b, link) && nyr_obj_u32(b, info) &&
         nyr_obj_u64(b, align) && nyr_obj_u64(b, entsize);
}

static bool ny_a64_write_file(const char *path, const nyr_obj_buf_t *file,
                              char *err, size_t err_len) {
  FILE *out = fopen(path, "wb");
  if (!out) {
    nyr_set_error(err, err_len, "AArch64 ELF writer: cannot open %s: %s", path,
                  strerror(errno));
    return false;
  }
  bool ok = fwrite(file->data, 1, file->len, out) == file->len;
  if (fclose(out) != 0)
    ok = false;
  if (!ok)
    nyr_set_error(err, err_len, "AArch64 ELF writer: failed writing %s", path);
  return ok;
}

bool nyr_a64_obj_build_bundle(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *out_defs,
    size_t *out_def_count, nyr_x64_obj_reloc_t *out_relocs,
    size_t *out_reloc_count, char *err, size_t err_len) {
  if (!rt_main || !target || !entry_symbol || !*entry_symbol || !code ||
      !out_defs || !out_def_count || !out_relocs || !out_reloc_count)
    return false;
  char verify_err[512] = {0};
  if (!nyr_ir_verify(rt_main, verify_err, sizeof(verify_err))) {
    nyr_set_error(err, err_len,
                  "AArch64 NYIR ABI verification for entry failed: %s",
                  verify_err);
    return false;
  }
  for (size_t i = 0; i < func_count; ++i) {
    verify_err[0] = '\0';
    if (nyr_ir_verify(&funcs[i], verify_err, sizeof(verify_err)))
      continue;
    nyr_set_error(
        err, err_len, "AArch64 NYIR ABI verification for %s failed: %s",
        func_names && func_names[i] ? func_names[i] : "<anon>", verify_err);
    return false;
  }
  ny_a64_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  ny_a64_reloc_t *relocs = NULL;
  size_t def_count = 0, reloc_count = 0;
  size_t reloc_cap = 0;
  if (!defs) {
    nyr_set_error(err, err_len, "AArch64 object definition allocation failed");
    return false;
  }
  /*
   * The scalar AArch64 NYIR encoder does not encode SSA PHIs directly.
   * Machine lowering can consume preserved PHIs, but this direct NYIR bundle
   * path cannot.  Destroy them before the parallel emit pass, just like the
   * x86-64 NYIR object fallback does.  Doing this here (before workers start)
   * also avoids mutating individual functions concurrently.
   */
  nyr_phi_elim((nyr_func_t *)rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyr_phi_elim((nyr_func_t *)&funcs[i]);
  if (!ny_a64_append_functions_parallel(
          code, defs, &def_count, &relocs, &reloc_count, &reloc_cap, funcs,
          func_names, func_count, target, err, err_len))
    goto fail;
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix, entry_symbol);
  if (!ny_a64_append(code, defs, &def_count, &relocs, &reloc_count, &reloc_cap,
                     rt_main, target, entry,
                     rt_main->param_count != 0 ||
                         rt_main->return_aggregate_size != 0,
                     tag_return, err, err_len))
    goto fail;
  if (reloc_count > NY_X64_OBJ_MAX_RELOCS) {
    nyr_set_error(err, err_len,
                  "AArch64 object bundle: relocation transport exceeds %d",
                  NY_X64_OBJ_MAX_RELOCS);
    goto fail;
  }
  for (size_t i = 0; i < def_count; ++i) {
    snprintf(out_defs[i].name, sizeof(out_defs[i].name), "%s", defs[i].name);
    out_defs[i].off = defs[i].off;
    out_defs[i].size = defs[i].size;
  }
  if (!nyr_strtab_append_defs(code, out_defs, &def_count, err, err_len))
    goto fail;
  if (!nyr_globaltab_append_defs(code, out_defs, &def_count, err, err_len))
    goto fail;
  if (!nyr_arraytab_append_defs(code, out_defs, &def_count, err, err_len))
    goto fail;
  for (size_t i = 0; i < reloc_count; ++i) {
    snprintf(out_relocs[i].symbol, sizeof(out_relocs[i].symbol), "%s",
             relocs[i].symbol);
    out_relocs[i].disp_off = relocs[i].off;
    out_relocs[i].type = relocs[i].type;
  }
  *out_def_count = def_count;
  *out_reloc_count = reloc_count;
  free(relocs);
  free(defs);
  return true;
fail:
  free(relocs);
  free(defs);
  return false;
}

typedef struct {
  const nyr_func_t *funcs;
  nyr_mach_func_t *out;
  char (*errors)[256];
} ny_a64_mach_lower_ctx_t;

static bool ny_a64_mach_lower_task(size_t i, void *opaque) {
  ny_a64_mach_lower_ctx_t *ctx = (ny_a64_mach_lower_ctx_t *)opaque;
  return nyr_mach_lower_nir(&ctx->funcs[i], &ctx->out[i], (1u << 8),
                            ctx->errors[i], 256);
}

static bool ny_a64_nir_has_aggregate_signature(const nyr_func_t *func) {
  if (!func)
    return false;
  if (func->return_aggregate_size)
    return true;
  for (size_t i = 0; i < func->param_count; ++i)
    if (func->param_types && func->param_types[i] == NYIR_PARAM_AGGREGATE)
      return true;
  return false;
}

bool nyr_native_emit_elf64_aarch64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  if (!rt_main || !target || !path || !entry_symbol || !*entry_symbol)
    return false;
  nyr_obj_buf_t code = {0}, file = {0}, strtab = {0};
  ny_a64_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  ny_a64_reloc_t *relocs = NULL;
  size_t def_count = 0, reloc_count = 0;
  size_t reloc_cap = 0;
  bool ok = false;
  bool built = false;
  nyr_native_debug_line_t *debug_lines = NULL;
  size_t debug_line_count = 0;
  size_t debug_line_cap = 0;
  nyr_obj_buf_t debug_line = {0};
  size_t debug_base_reloc = 0;
  size_t debug_base_def = 0;
  nyr_mach_func_t top_mach = {0};
  nyr_mach_func_t *func_mach = NULL;
  uint32_t *def_names = NULL;
  uint32_t *ext_names = NULL;
  char (*externs)[256] = NULL;
  char mach_err[256] = {0};
  nyr_x64_obj_symbol_def_t *mach_defs = NULL;
  nyr_x64_obj_reloc_t *mach_relocs = NULL;
  if (!defs) {
    nyr_set_error(err, err_len,
                  "AArch64 ELF object definition allocation failed");
    goto done;
  }
  bool mach_ok = nyr_mach_lower_nir(rt_main, &top_mach, (1u << 8), mach_err,
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
    for (size_t f = 0; f <= func_count && mach_ok; ++f) {
      const nyr_func_t *func = f == func_count ? rt_main : &funcs[f];
      for (size_t i = 0; i < func->len; ++i) {
        const nyr_inst_t *in = &func->data[i];
        ny_native_runtime_op_t operation =
            ny_native_runtime_operation(in->symbol);
        if (in->op == NYIR_CALL && in->symbol &&
            (operation == NY_NATIVE_RUNTIME_OP_LINUX_SYSCALL ||
             operation == NY_NATIVE_RUNTIME_OP_CSTR_TO_STRING ||
             operation == NY_NATIVE_RUNTIME_OP_BOOL_TO_CSTR ||
             operation == NY_NATIVE_RUNTIME_OP_PRINT_CSTR ||
             operation == NY_NATIVE_RUNTIME_OP_PRINT_I64 ||
             operation == NY_NATIVE_RUNTIME_OP_PRINT_NEWLINE ||
             operation == NY_NATIVE_RUNTIME_OP_MONOTONIC_NS ||
             strcmp(in->symbol, "rt_malloc_i64") == 0 ||
             strcmp(in->symbol, "rt_malloc_raw_i64") == 0 ||
             strcmp(in->symbol, "rt_malloc_raw") == 0 ||
             strcmp(in->symbol, "rt_calloc_raw_i64") == 0 ||
             strcmp(in->symbol, "rt_calloc_raw") == 0 ||
             strcmp(in->symbol, "rt_zalloc_raw") == 0 ||
             strcmp(in->symbol, "rt_malloc") == 0 ||
             strcmp(in->symbol, "rt_malloc_uninit") == 0 ||
             strcmp(in->symbol, "rt_free") == 0 ||
             strcmp(in->symbol, "rt_free_raw") == 0 ||
             strcmp(in->symbol, "rt_zfree_raw") == 0 ||
             strcmp(in->symbol, "rt_untag") == 0 ||
             ny_native_runtime_operation(in->symbol) ==
                 NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_NEW ||
             operation == NY_NATIVE_RUNTIME_OP_MEMSET ||
             operation == NY_NATIVE_RUNTIME_OP_MEMCPY ||
             operation == NY_NATIVE_RUNTIME_OP_MEMCMP ||
             ny_native_runtime_operation(in->symbol) ==
                 NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_CLONE ||
             ny_native_runtime_operation(in->symbol) ==
                 NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_LEN ||
             ny_native_runtime_operation(in->symbol) ==
                 NY_NATIVE_RUNTIME_OP_TYPED_BUFFER_INDEX_READ ||
             operation == NY_NATIVE_RUNTIME_OP_CSTR_LEN ||
             operation == NY_NATIVE_RUNTIME_OP_CSTR_CONCAT ||
             strcmp(in->symbol, "rt_cstr_eq") == 0 ||
             operation == NY_NATIVE_RUNTIME_OP_WRITE_OFF)) {
          mach_ok = false;
          break;
        }
      }
    }
  }
  if (mach_ok) {
    mach_defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*mach_defs));
    mach_relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*mach_relocs));
    size_t mach_def_count = 0, mach_reloc_count = 0;
    if (!mach_defs || !mach_relocs) {
      free(mach_defs);
      free(mach_relocs);
      mach_defs = NULL;
      mach_relocs = NULL;
    }
    if (mach_defs && mach_relocs &&
        ny_a64_mach_build_bundle_debug(
            &top_mach, func_mach, func_names, func_count, target, entry_symbol,
            tag_return, ny_a64_nir_has_aggregate_signature(rt_main), &code,
            mach_defs, &mach_def_count, mach_relocs, &mach_reloc_count, rt_main,
            funcs, &debug_lines, &debug_line_count, &debug_line_cap, mach_err,
            sizeof(mach_err))) {
      if (mach_def_count <= NY_NATIVE_MAX_DEFS &&
          mach_reloc_count <= NY_X64_OBJ_MAX_RELOCS) {
        relocs =
            malloc((mach_reloc_count ? mach_reloc_count : 1) * sizeof(*relocs));
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
          relocs[i].type = mach_relocs[i].type;
        }
        def_count = mach_def_count;
        reloc_count = mach_reloc_count;
        built = true;
      }
    }
  }
  nyr_mach_func_free(&top_mach);
  if (func_mach) {
    for (size_t i = 0; i < func_count; ++i)
      nyr_mach_func_free(&func_mach[i]);
    free(func_mach);
  }
  if (!built) {
    free(debug_lines);
    debug_lines = NULL;
    debug_line_count = 0;
    debug_line_cap = 0;
    nyr_obj_free(&code);
    code = (nyr_obj_buf_t){0};
    def_count = reloc_count = 0;
    if (err && err_len)
      err[0] = '\0';
    /*
     * The machine-form attempt preserves SSA PHIs.  If it falls back to the
     * direct AArch64 NYIR encoder, lower those PHIs to edge copies first; the
     * direct encoder intentionally has no NYIR_PHI instruction case.
     */
    nyr_phi_elim((nyr_func_t *)rt_main);
    for (size_t i = 0; i < func_count; ++i)
      nyr_phi_elim((nyr_func_t *)&funcs[i]);
    if (!ny_a64_append_functions_parallel(
            &code, defs, &def_count, &relocs, &reloc_count, &reloc_cap, funcs,
            func_names, func_count, target, err, err_len))
      goto done;
    char entry[256];
    snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix, entry_symbol);
    if (!ny_a64_append(&code, defs, &def_count, &relocs, &reloc_count,
                       &reloc_cap, rt_main, target, entry,
                       rt_main->param_count != 0 ||
                           rt_main->return_aggregate_size != 0,
                       tag_return, err, err_len))
      goto done;
  }
  if (!ny_a64_append_str_defs(&code, defs, &def_count, relocs, reloc_count, err,
                              err_len))
    goto done;
  if (!ny_a64_append_array_defs(&code, defs, &def_count, err, err_len))
    goto done;

  if (!debug_lines) {
    debug_line_cap = func_count + 1;
    if (debug_line_cap < 32)
      debug_line_cap = 32;
    debug_lines = calloc(debug_line_cap, sizeof(*debug_lines));
    if (!debug_lines)
      goto done;
  }
  for (size_t i = 0; i < func_count; ++i) {
    const char *name =
        func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%sny_fn_%s",
             target->symbol_prefix ? target->symbol_prefix : "", name);
    int def_i = ny_a64_def_index(defs, def_count, symbol);
    if (def_i < 0)
      continue;
    for (size_t j = 0; funcs && j < funcs[i].len; ++j) {
      const nyr_debug_loc_t *loc = &funcs[i].data[j].debug;
      if (!loc->line)
        continue;
      if (!ny_a64_debug_line_push(
              &debug_lines, &debug_line_count, &debug_line_cap,
              (nyr_native_debug_line_t){(size_t)def_i, defs[def_i].off,
                                        loc->file, loc->line, loc->column}))
        goto done;
      break;
    }
  }
  char entry_name[256];
  snprintf(entry_name, sizeof(entry_name), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  int entry_i = ny_a64_def_index(defs, def_count, entry_name);
  if (entry_i >= 0)
    for (size_t j = 0; j < rt_main->len; ++j) {
      const nyr_debug_loc_t *loc = &rt_main->data[j].debug;
      if (!loc->line)
        continue;
      if (!ny_a64_debug_line_push(
              &debug_lines, &debug_line_count, &debug_line_cap,
              (nyr_native_debug_line_t){(size_t)entry_i, defs[entry_i].off,
                                        loc->file, loc->line, loc->column}))
        goto done;
      break;
    }

  if (reloc_count > SIZE_MAX / sizeof(*externs))
    goto done;
  externs = calloc(reloc_count ? reloc_count : 1, sizeof(*externs));
  if (!externs) {
    nyr_set_error(err, err_len, "AArch64 external symbol allocation failed");
    goto done;
  }
  size_t extern_count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (ny_a64_def_index(defs, def_count, relocs[i].symbol) >= 0 ||
        ny_a64_name_index(externs, extern_count, relocs[i].symbol) >= 0)
      continue;
    snprintf(externs[extern_count++], 256, "%s", relocs[i].symbol);
  }
  /*
   * def_count is bounded by NY_NATIVE_MAX_DEFS (4096), not 256 -- the old
   * fixed arrays were a ~15 KB stack smash on any bundle with more than
   * 256 defined symbols. Heap-allocate to the real bounds.
   */
  if (def_count > NY_NATIVE_MAX_DEFS)
    goto done;
  def_names = calloc(def_count ? def_count : 1, sizeof(*def_names));
  ext_names = calloc(extern_count ? extern_count : 1, sizeof(*ext_names));
  if ((def_count && !def_names) || (extern_count && !ext_names)) {
    free(def_names);
    free(ext_names);
    goto done;
  }
  if (!nyr_obj_u8(&strtab, 0)) {
    free(def_names);
    free(ext_names);
    goto done;
  }
  for (size_t i = 0; i < def_count; ++i) {
    def_names[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, defs[i].name, strlen(defs[i].name) + 1))
      goto done;
  }
  for (size_t i = 0; i < extern_count; ++i) {
    ext_names[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, externs[i], strlen(externs[i]) + 1))
      goto done;
  }
  const char shstr[] = "\0.text\0.rela.text\0.debug_line\0.rela.debug_line\0."
                       "symtab\0.strtab\0.shstrtab\0";
  const uint32_t sh_text = 1;
  const uint32_t sh_rela_text = 7;
  const uint32_t sh_debug_line = 18;
  const uint32_t sh_rela_debug_line = 30;
  const uint32_t sh_symtab = 47;
  const uint32_t sh_strtab = 55;
  const uint32_t sh_shstrtab = 63;
  const uint32_t sec_debug = 3;
  if (debug_line_count &&
      !nyr_native_build_debug_line(debug_lines, debug_line_count, code.len,
                                   &debug_line, &debug_base_reloc,
                                   &debug_base_def, err, err_len))
    goto done;
  if (debug_line_count && debug_base_def >= def_count)
    goto done;
  /*
   * The optional debug pair shifts .symtab and following sections by two.
   */
  const uint32_t sec_symtab = debug_line_count ? 5 : 3;
  const uint32_t sec_strtab = sec_symtab + 1;
  const uint32_t sec_shstrtab = sec_symtab + 2;
  if (!nyr_obj_zero(&file, 64) || !nyr_obj_pad_to(&file, 16))
    goto done;
  size_t text_off = file.len;
  if (!nyr_obj_emit(&file, code.data, code.len) || !nyr_obj_pad_to(&file, 8))
    goto done;
  size_t debug_off = file.len;
  if (debug_line_count &&
      (!nyr_obj_emit(&file, debug_line.data, debug_line.len) ||
       !nyr_obj_pad_to(&file, 8)))
    goto done;
  size_t rela_off = file.len;
  for (size_t i = 0; i < reloc_count; ++i) {
    int di = ny_a64_def_index(defs, def_count, relocs[i].symbol);
    int ei = di < 0 ? ny_a64_name_index(externs, extern_count, relocs[i].symbol)
                    : -1;
    if (di < 0 && ei < 0)
      goto done;
    uint32_t sym =
        di >= 0 ? (uint32_t)(1 + di) : (uint32_t)(1 + def_count + (size_t)ei);
    uint64_t info =
        ((uint64_t)sym << 32) | ny_a64_elf_reloc_type(relocs[i].type);
    if (!nyr_obj_u64(&file, relocs[i].off) || !nyr_obj_u64(&file, info) ||
        !nyr_obj_u64(&file, 0))
      goto done;
  }
  size_t rela_size = file.len - rela_off;
  size_t rela_debug_off = file.len;
  if (debug_line_count) {
    uint64_t info = ((uint64_t)(1u + (uint32_t)debug_base_def) << 32) | 257u;
    if (!nyr_obj_u64(&file, debug_base_reloc) || !nyr_obj_u64(&file, info) ||
        !nyr_obj_u64(&file, 0))
      goto done;
  }
  size_t rela_debug_size = file.len - rela_debug_off;
  size_t symtab_off = file.len;
  if (!ny_a64_elf_sym(&file, 0, 0, 0, 0, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i)
    if (!ny_a64_elf_sym(&file, def_names[i], 0x12, 1, defs[i].off,
                        defs[i].size))
      goto done;
  for (size_t i = 0; i < extern_count; ++i)
    if (!ny_a64_elf_sym(&file, ext_names[i], 0x12, 0, 0, 0))
      goto done;
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!nyr_obj_emit(&file, strtab.data, strtab.len))
    goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstr_off = file.len;
  if (!nyr_obj_emit(&file, shstr, sizeof(shstr)) || !nyr_obj_pad_to(&file, 8))
    goto done;
  size_t shoff = file.len;
  if (ny_dbg_is_enabled(NY_DBG_ELF)) {
    ny_dbg_elf_section(".text", text_off, code.len, 0x6);
    ny_dbg_elf_section(".rela.text", rela_off, rela_size, 0);
    if (debug_line_count) {
      ny_dbg_elf_section(".debug_line", debug_off, debug_line.len, 0);
      ny_dbg_elf_section(".rela.debug_line", rela_debug_off, rela_debug_size,
                         0);
    }
    ny_dbg_elf_section(".symtab", symtab_off, symtab_size, 0);
    ny_dbg_elf_section(".strtab", strtab_off, strtab.len, 0);
    ny_dbg_elf_section(".shstrtab", shstr_off, sizeof(shstr), 0);
    for (size_t i = 0; i < reloc_count; ++i)
      ny_dbg_elf_reloc(relocs[i].off, relocs[i].symbol,
                       relocs[i].type == NY_RELOC_AARCH64_CALL26
                           ? "R_AARCH64_CALL26"
                           : "R_AARCH64_ABS64");
    if (debug_line_count && debug_base_def < def_count)
      ny_dbg_elf_reloc(debug_base_reloc, defs[debug_base_def].name,
                       "R_AARCH64_ABS64");
    for (size_t i = 0; i < def_count; ++i)
      ny_dbg_elf_symbol(defs[i].name, ".text", defs[i].off, defs[i].size);
    for (size_t i = 0; i < extern_count; ++i)
      ny_dbg_elf_symbol(externs[i], "UND", 0, 0);
  }
  if (!ny_a64_elf_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !ny_a64_elf_sh(&file, sh_text, 1, 0x6, text_off, code.len, 0, 0, 16, 0) ||
      !ny_a64_elf_sh(&file, sh_rela_text, 4, 0, rela_off, rela_size, sec_symtab,
                     1, 8, 24) ||
      (debug_line_count &&
       (!ny_a64_elf_sh(&file, sh_debug_line, 1, 0, debug_off, debug_line.len, 0,
                       0, 1, 0) ||
        !ny_a64_elf_sh(&file, sh_rela_debug_line, 4, 0, rela_debug_off,
                       rela_debug_size, sec_symtab, sec_debug, 8, 24))) ||
      !ny_a64_elf_sh(&file, sh_symtab, 2, 0, symtab_off, symtab_size,
                     sec_strtab, 1, 8, 24) ||
      !ny_a64_elf_sh(&file, sh_strtab, 3, 0, strtab_off, strtab_size, 0, 0, 1,
                     0) ||
      !ny_a64_elf_sh(&file, sh_shstrtab, 3, 0, shstr_off, sizeof(shstr), 0, 0,
                     1, 0))
    goto done;
  file.data[0] = 0x7f;
  file.data[1] = 'E';
  file.data[2] = 'L';
  file.data[3] = 'F';
  file.data[4] = 2;
  file.data[5] = 1;
  file.data[6] = 1;
  nyr_obj_patch_u16(&file, 16, 1);   /* ET_REL */
  nyr_obj_patch_u16(&file, 18, 183); /* EM_AARCH64 */
  nyr_obj_patch_u32(&file, 20, 1);
  nyr_obj_patch_u64(&file, 40, shoff);
  nyr_obj_patch_u16(&file, 52, 64);
  nyr_obj_patch_u16(&file, 58, 64);
  nyr_obj_patch_u16(&file, 60, sec_shstrtab + 1);
  nyr_obj_patch_u16(&file, 62, sec_shstrtab);
  ok = ny_a64_write_file(path, &file, err, err_len);
done:
  free(externs);
  free(def_names);
  free(ext_names);
  free(mach_defs);
  free(mach_relocs);
  free(defs);
  free(relocs);
  free(debug_lines);
  nyr_obj_free(&debug_line);
  nyr_obj_free(&strtab);
  nyr_obj_free(&file);
  nyr_obj_free(&code);
  if (!ok && err && err_len && !err[0])
    nyr_set_error(err, err_len, "AArch64 ELF object writer failed");
  return ok;
}

/*
 * AArch64 machine form byte encoder. Encodes const-return and simple
 * single-block i64 ALU into raw bytes (no host assembler). General shapes fall
 * back.
 */

static bool a64_u32(nyr_obj_buf_t *code, uint32_t w) {
  return nyr_obj_emit(code, &w, sizeof(w));
}

/*
 * movz/movk sequence for 64-bit imm into xd (0..30).
 */
static bool a64_mov_imm64(nyr_obj_buf_t *code, unsigned xd, uint64_t imm) {
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

static bool a64_ret(nyr_obj_buf_t *code) {
  return a64_u32(code, 0xD65F03C0u); /* ret */
}

/*
 * AArch64 keeps the general stack-slot encoder as the compatibility path, but
 * scalar machine form can use caller-saved registers safely for colored
 * ranges. The encoder handles simple CFG branches and relocates them after
 * block layout; calls, floating point, vectors, and true uncolored spills
 * remain explicit fallback cases. x16 and x17 stay available as encoder
 * scratch registers, so the allocator uses x0 through x15 only.
 *
 * Caller-saved ONLY. The pool previously used x19..x28 (the entire AAPCS64
 * callee-saved GPR set) without any prologue saves -- every host caller
 * holding live values there was silently corrupted. This fast path rejects
 * calls, so caller-saved scratch is both safe and free of save/restore cost.
 * x0/x1 avoided (primary/secondary return), x8 reserved for sret,
 * x16/x17 are encoder scratch, x18 is the platform register.
 */
#define A64_MACH_COLOR_N 10
static const unsigned a64_mach_color_reg[A64_MACH_COLOR_N] = {2, 3, 4,  5,  6,
                                                              7, 9, 10, 11, 12};

static int a64_slot_off(const nyr_mach_func_t *mach,
                        const nyr_mach_operand_t *op);
static int a64_mach_frame_size(const nyr_mach_func_t *mach);
static bool a64_mach_emit_param_spills(const nyr_mach_func_t *mach,
                                       nyr_obj_buf_t *code);
static bool a64_ldur_x(nyr_obj_buf_t *code, unsigned reg, int off);
static bool a64_stur_x(nyr_obj_buf_t *code, unsigned reg, int off);
static bool a64_ldur_fp(nyr_obj_buf_t *code, bool f32, unsigned reg, int off);
static bool a64_stur_fp(nyr_obj_buf_t *code, bool f32, unsigned reg, int off);
static nyr_mach_type_t a64_operand_type(const nyr_mach_func_t *mach,
                                        const nyr_mach_operand_t *op);

static bool a64_mach_scalar_regalloc_prepare(const nyr_mach_func_t *mach,
                                             nyr_mach_regalloc_t *alloc) {
  /*
   * PHI lowering materializes one edge block per predecessor.  Those blocks
   * are ordinary machine CFG blocks and the allocator already handles an
   * arbitrary block count; rejecting them here silently routes loop PHIs
   * through the legacy stack-only encoder, which does not preserve parallel
   * copies.
   */
  if (!mach || !alloc || !mach->block_len)
    return false;
  if (!nyr_mach_regalloc_build(mach, A64_MACH_COLOR_N, alloc))
    return false;
  bool ok = true;
  for (size_t i = 0; ok && i < alloc->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &alloc->segments[i];
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
    nyr_mach_regalloc_free(alloc);
    return false;
  }
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    switch (in->opcode) {
    case NYR_MACH_COPY:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          (in->src0.kind != NYR_MACH_OPERAND_VREG &&
           in->src0.kind != NYR_MACH_OPERAND_IMM))
        ok = false;
      break;
    case NYR_MACH_LOAD:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_FRAME)
        ok = false;
      break;
    case NYR_MACH_STORE:
      if (in->dst.kind != NYR_MACH_OPERAND_FRAME ||
          in->src0.kind != NYR_MACH_OPERAND_VREG)
        ok = false;
      break;
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_AND:
    case NYR_MACH_OR:
    case NYR_MACH_XOR:
    case NYR_MACH_SHL:
    case NYR_MACH_SAR:
    case NYR_MACH_DIV:
    case NYR_MACH_MOD:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG)
        ok = false;
      break;
    case NYR_MACH_CMP:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG)
        ok = false;
      break;
    case NYR_MACH_BR:
      if (in->src1.kind != NYR_MACH_OPERAND_BLOCK ||
          in->src1.as.block_index >= mach->block_len)
        ok = false;
      break;
    case NYR_MACH_BR_IF:
      if (in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_BLOCK ||
          in->src1.as.block_index >= mach->block_len)
        ok = false;
      break;
    case NYR_MACH_RET:
    case NYR_MACH_NOP:
    case NYR_MACH_TRAP:
      if (in->opcode == NYR_MACH_RET &&
          in->src0.kind != NYR_MACH_OPERAND_NONE &&
          in->src0.kind != NYR_MACH_OPERAND_VREG &&
          in->src0.kind != NYR_MACH_OPERAND_IMM)
        ok = false;
      break;
    default:
      ok = false;
      break;
    }
    if (!ok)
      break;
    if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
        (in->dst.as.reg >= mach->vreg_len || !mach->vreg_types ||
         (mach->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_I64 &&
          mach->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_PTR)))
      ok = false;
    const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
    for (size_t oi = 0; oi < 3; ++oi) {
      const nyr_mach_operand_t *op = ops[oi];
      if (op->kind == NYR_MACH_OPERAND_VREG &&
          (op->as.reg >= mach->vreg_len || !mach->vreg_types ||
           (mach->vreg_types[op->as.reg] != NYR_MACH_TYPE_I64 &&
            mach->vreg_types[op->as.reg] != NYR_MACH_TYPE_PTR)))
        ok = false;
    }
  }
  if (!ok) {
    nyr_mach_regalloc_free(alloc);
    return false;
  }
  return true;
}

static int a64_mach_scalar_reg(const nyr_mach_regalloc_t *alloc, uint32_t vreg,
                               size_t inst) {
  if (!alloc || vreg >= alloc->vreg_len)
    return -1;
  const nyr_mach_live_segment_t *seg =
      nyr_mach_regalloc_segment_at(alloc, vreg, inst);
  if (!seg || seg->color < 0 || seg->color >= A64_MACH_COLOR_N)
    return -1;
  return (int)a64_mach_color_reg[seg->color];
}

static bool a64_mach_mov_reg(nyr_obj_buf_t *code, unsigned dst, unsigned src) {
  if (dst > 30 || src > 30)
    return false;
  return a64_u32(code, 0xAA0003E0u | ((src & 31u) << 16) | (dst & 31u));
}

static const nyr_mach_live_segment_t *
a64_mach_segment(const nyr_mach_regalloc_t *alloc, uint32_t vreg, size_t inst) {
  return nyr_mach_regalloc_segment_at(alloc, vreg, inst);
}

static bool a64_mach_load_reloads(const nyr_mach_func_t *mach,
                                  const nyr_mach_regalloc_t *alloc,
                                  const nyr_mach_inst_t *in, size_t inst,
                                  nyr_obj_buf_t *code) {
  const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2};
  uint32_t loaded[3];
  size_t loaded_len = 0;
  for (size_t i = 0; i < 3; ++i) {
    const nyr_mach_operand_t *op = ops[i];
    if (op->kind != NYR_MACH_OPERAND_VREG)
      continue;
    const nyr_mach_live_segment_t *seg =
        a64_mach_segment(alloc, op->as.reg, inst);
    if (!seg || !seg->reload || seg->start != inst)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < loaded_len; ++j)
      duplicate |= loaded[j] == op->as.reg;
    if (duplicate)
      continue;
    int reg = a64_mach_scalar_reg(alloc, op->as.reg, inst);
    nyr_mach_operand_t slot = {.kind = NYR_MACH_OPERAND_VREG,
                               .as.reg = op->as.reg};
    if (reg < 0 || !a64_ldur_x(code, (unsigned)reg, a64_slot_off(mach, &slot)))
      return false;
    loaded[loaded_len++] = op->as.reg;
  }
  return true;
}

static bool a64_mach_store_spills(const nyr_mach_func_t *mach,
                                  const nyr_mach_regalloc_t *alloc,
                                  const nyr_mach_inst_t *in, size_t block,
                                  size_t inst, nyr_obj_buf_t *code) {
  const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1, &in->src2};
  uint32_t stored[4];
  size_t stored_len = 0;
  for (size_t i = 0; i < 4; ++i) {
    const nyr_mach_operand_t *op = ops[i];
    if (op->kind != NYR_MACH_OPERAND_VREG)
      continue;
    const nyr_mach_live_segment_t *seg =
        a64_mach_segment(alloc, op->as.reg, inst);
    if (!seg || !seg->spill || seg->end != inst)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < stored_len; ++j)
      duplicate |= stored[j] == op->as.reg;
    if (duplicate)
      continue;
    int reg = a64_mach_scalar_reg(alloc, op->as.reg, inst);
    nyr_mach_operand_t slot = {.kind = NYR_MACH_OPERAND_VREG,
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
    if (!nyr_mach_regalloc_live_out(alloc, block, v))
      continue;
    const nyr_mach_live_segment_t *seg = a64_mach_segment(alloc, v, inst);
    if (!seg || !seg->spill || seg->end != inst)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < stored_len; ++j)
      duplicate |= stored[j] == v;
    if (duplicate)
      continue;
    int reg = a64_mach_scalar_reg(alloc, v, inst);
    nyr_mach_operand_t slot = {.kind = NYR_MACH_OPERAND_VREG, .as.reg = v};
    if (reg < 0 || stored_len >= sizeof(stored) / sizeof(stored[0]) ||
        !a64_stur_x(code, (unsigned)reg, a64_slot_off(mach, &slot)))
      return false;
    stored[stored_len++] = v;
  }
  return true;
}

static bool a64_mach_scalar_regalloc_encode(const nyr_mach_func_t *mach,
                                            const nyr_mach_regalloc_t *alloc,
                                            nyr_obj_buf_t *code, bool user,
                                            size_t *inst_offsets, char *err,
                                            size_t err_len) {
  int frame = a64_mach_frame_size(mach);
  if (!a64_u32(code, 0xA9BF7BFDu) || !a64_u32(code, 0x910003FDu))
    return false;
  if (frame) {
    if (frame >= 4096 || !a64_u32(code, 0xD10003FFu | ((uint32_t)frame << 10)))
      return false;
  }
  if (user && !a64_mach_emit_param_spills(mach, code))
    return false;
  size_t *block_off = calloc(mach->block_len, sizeof(*block_off));
  size_t *patch_at =
      mach->inst_len ? calloc(mach->inst_len, sizeof(*patch_at)) : NULL;
  uint32_t *patch_blk =
      mach->inst_len ? calloc(mach->inst_len, sizeof(*patch_blk)) : NULL;
  size_t patch_len = 0;
  if (!block_off || (mach->inst_len && (!patch_at || !patch_blk)))
    goto fail;

  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    const nyr_mach_block_t *block = &mach->blocks[bi];
    block_off[bi] = code->len;
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t i = block->first_inst + n;
      if (inst_offsets && i < mach->inst_len)
        inst_offsets[i] = code->len;
      const nyr_mach_inst_t *in = &mach->insts[i];
      int dst = in->dst.kind == NYR_MACH_OPERAND_VREG
                    ? a64_mach_scalar_reg(alloc, in->dst.as.reg, i)
                    : -1;
      int a = in->src0.kind == NYR_MACH_OPERAND_VREG
                  ? a64_mach_scalar_reg(alloc, in->src0.as.reg, i)
                  : -1;
      int b = in->src1.kind == NYR_MACH_OPERAND_VREG
                  ? a64_mach_scalar_reg(alloc, in->src1.as.reg, i)
                  : -1;
      if (!a64_mach_load_reloads(mach, alloc, in, i, code))
        goto fail;
      switch (in->opcode) {
      case NYR_MACH_COPY:
        if (dst < 0)
          goto fail;
        if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
          if (!a64_mov_imm64(code, (unsigned)dst, (uint64_t)in->src0.as.imm))
            goto fail;
        } else if (a < 0 || !a64_mach_mov_reg(code, (unsigned)dst, (unsigned)a))
          goto fail;
        break;
      case NYR_MACH_LOAD:
        if (dst < 0 || in->src0.kind != NYR_MACH_OPERAND_FRAME ||
            !a64_ldur_x(code, (unsigned)dst, a64_slot_off(mach, &in->src0)))
          goto fail;
        break;
      case NYR_MACH_STORE:
        if (a < 0 || in->dst.kind != NYR_MACH_OPERAND_FRAME ||
            !a64_stur_x(code, (unsigned)a, a64_slot_off(mach, &in->dst)))
          goto fail;
        break;
      case NYR_MACH_ADD:
      case NYR_MACH_SUB:
      case NYR_MACH_MUL:
      case NYR_MACH_AND:
      case NYR_MACH_OR:
      case NYR_MACH_XOR:
      case NYR_MACH_SHL:
      case NYR_MACH_SAR:
      case NYR_MACH_DIV:
      case NYR_MACH_MOD: {
        if (dst < 0 || a < 0 || b < 0)
          goto fail;
        uint32_t op = 0;
        switch (in->opcode) {
        case NYR_MACH_ADD:
          op = 0x8B000000u;
          break;
        case NYR_MACH_SUB:
          op = 0xCB000000u;
          break;
        case NYR_MACH_MUL:
          op = 0x9B007C00u;
          break;
        case NYR_MACH_AND:
          op = 0x8A000000u;
          break;
        case NYR_MACH_OR:
          op = 0xAA000000u;
          break;
        case NYR_MACH_XOR:
          op = 0xCA000000u;
          break;
        case NYR_MACH_SHL:
          op = 0x9AC02000u;
          break;
        case NYR_MACH_SAR:
          op = 0x9AC02800u;
          break;
        case NYR_MACH_DIV:
          op = 0x9AC00C00u;
          break;
        case NYR_MACH_MOD:
          if (!a64_u32(code, 0x9AC00C00u | ((uint32_t)b << 16) |
                                 ((uint32_t)a << 5) | 17u) ||
              !a64_u32(code, 0x9B008000u | ((uint32_t)b << 16) | (17u << 5) |
                                 ((uint32_t)a << 10) | (uint32_t)dst))
            goto fail;
          break;
        default:
          break;
        }
        if (in->opcode == NYR_MACH_MOD)
          break;
        if (!a64_u32(code, op | ((uint32_t)b << 16) | ((uint32_t)a << 5) |
                               (uint32_t)dst))
          goto fail;
        break;
      }
      case NYR_MACH_SELECT: {
        int other = in->src2.kind == NYR_MACH_OPERAND_VREG
                        ? a64_mach_scalar_reg(alloc, in->src2.as.reg, i)
                        : -1;
        if (dst < 0 || a < 0 || b < 0 || other < 0 ||
            !a64_u32(code, 0xF100001Fu | ((uint32_t)a << 5)) ||
            !a64_u32(code, 0x9A801000u | ((uint32_t)other << 16) |
                               ((uint32_t)b << 5) | (uint32_t)dst))
          goto fail;
        break;
      }
      case NYR_MACH_CMP: {
        if (dst < 0 || a < 0 || b < 0 ||
            !a64_u32(code,
                     0xEB00001Fu | ((uint32_t)b << 16) | ((uint32_t)a << 5)))
          goto fail;
        unsigned cond = 0xb; /* lt */
        switch (in->condition) {
        case NYR_MACH_COND_EQ:
          cond = 0x0;
          break;
        case NYR_MACH_COND_NE:
          cond = 0x1;
          break;
        case NYR_MACH_COND_LT:
          cond = 0xb;
          break;
        case NYR_MACH_COND_LE:
          cond = 0xd;
          break;
        case NYR_MACH_COND_GT:
          cond = 0xc;
          break;
        case NYR_MACH_COND_GE:
          cond = 0xa;
          break;
        default:
          goto fail;
        }
        if (!a64_u32(code, 0x9A9F07E0u | ((cond ^ 1u) << 12) | (uint32_t)dst))
          goto fail;
        break;
      }
      case NYR_MACH_BR:
        if (!a64_mach_store_spills(mach, alloc, in, bi, i, code) ||
            patch_len >= mach->inst_len || !a64_u32(code, 0x14000000u))
          goto fail;
        patch_at[patch_len] = code->len - 4;
        patch_blk[patch_len++] = in->src1.as.block_index;
        break;
      case NYR_MACH_BR_IF:
        if (a < 0 || !a64_mach_store_spills(mach, alloc, in, bi, i, code) ||
            patch_len >= mach->inst_len ||
            !a64_u32(code, 0xB5000000u | (uint32_t)a))
          goto fail;
        patch_at[patch_len] = code->len - 4;
        patch_blk[patch_len++] = in->src1.as.block_index;
        break;
      case NYR_MACH_RET:
        if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
          if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
            goto fail;
        } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
          if (a < 0 || !a64_mach_mov_reg(code, 0, (unsigned)a))
            goto fail;
        }
        if (frame && !a64_u32(code, 0x910003FFu | ((uint32_t)frame << 10)))
          goto fail;
        if (!a64_u32(code, 0xA8C17BFDu) || !a64_ret(code))
          goto fail;
        break;
      case NYR_MACH_NOP:
        break;
      case NYR_MACH_TRAP:
        /*
         * brk #0 — raises a debug / breakpoint exception.
         */
        if (!a64_u32(code, 0xD4200000u))
          goto fail;
        break;
      default:
        goto fail;
      }
      if (in->opcode != NYR_MACH_RET && in->opcode != NYR_MACH_BR &&
          in->opcode != NYR_MACH_BR_IF &&
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
      word = (current & 0xffe0001fu) | (((uint32_t)imm & 0x7ffffu) << 5);
    }
    nyr_obj_patch_u32(code, patch_at[i], word);
  }
  free(block_off);
  free(patch_at);
  free(patch_blk);
  return true;
fail:
  free(block_off);
  free(patch_at);
  free(patch_blk);
  if (err && err_len && !err[0])
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
 *
 * v0..v7 ONLY. v8..v15 are AAPCS64 callee-saved (low 64 bits) and this
 * path emits no saves; colors previously mapped straight to vN.
 */
#define A64_MACH_FPR_COLOR_N 8

typedef struct {
  nyr_mach_regalloc_t alloc;
  bool *seeded;
  size_t colors_len;
} a64_mach_fpr_state_t;

static int a64_mach_fpr_reg(const a64_mach_fpr_state_t *state, uint32_t vreg,
                            size_t inst) {
  if (!state || vreg >= state->colors_len)
    return -1;
  const nyr_mach_live_segment_t *seg =
      nyr_mach_regalloc_segment_at(&state->alloc, vreg, inst);
  if (!seg || seg->color < 0 || seg->color >= A64_MACH_FPR_COLOR_N)
    return -1;
  return seg->color;
}

static const nyr_mach_live_segment_t *
a64_mach_fpr_segment(const a64_mach_fpr_state_t *state, uint32_t vreg,
                     size_t inst) {
  return state ? nyr_mach_regalloc_segment_at(&state->alloc, vreg, inst) : NULL;
}

static bool a64_mach_fpr_carries(const a64_mach_fpr_state_t *state,
                                 uint32_t vreg,
                                 const nyr_mach_live_segment_t *seg) {
  if (!state || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const nyr_mach_live_segment_t *prev =
      a64_mach_fpr_segment(state, vreg, seg->start - 1);
  return prev && prev->end + 1 == seg->start && !prev->spill &&
         prev->color == seg->color;
}

static bool a64_mach_fpr_begin(a64_mach_fpr_state_t *state, size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const nyr_mach_live_segment_t *seg = a64_mach_fpr_segment(state, v, inst);
    if (seg && seg->start == inst)
      state->seeded[v] = a64_mach_fpr_carries(state, v, seg);
  }
  return true;
}

static bool a64_mach_fpr_end(const nyr_mach_func_t *mach,
                             a64_mach_fpr_state_t *state, size_t inst,
                             nyr_obj_buf_t *code) {
  if (!mach || !state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const nyr_mach_live_segment_t *seg = a64_mach_fpr_segment(state, v, inst);
    if (!seg || seg->end != inst)
      continue;
    const nyr_mach_live_segment_t *next =
        a64_mach_fpr_segment(state, v, inst + 1);
    bool needs_home =
        seg->spill || (next && !a64_mach_fpr_carries(state, v, next));
    if (needs_home && seg->color >= 0 && state->seeded[v]) {
      bool f32 = mach->vreg_types[v] == NYR_MACH_TYPE_F32;
      nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_VREG, .as.reg = v};
      if (!a64_stur_fp(code, f32, (unsigned)seg->color,
                       a64_slot_off(mach, &home)))
        return false;
    }
    if (seg->spill || !next || !a64_mach_fpr_carries(state, v, next))
      state->seeded[v] = false;
  }
  return true;
}

static bool a64_mach_fpr_seed(const nyr_mach_func_t *mach,
                              a64_mach_fpr_state_t *state, uint32_t vreg,
                              size_t inst, nyr_obj_buf_t *code) {
  int reg = a64_mach_fpr_reg(state, vreg, inst);
  if (reg < 0 || state->seeded[vreg])
    return true;
  nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_VREG, .as.reg = vreg};
  bool f32 = mach->vreg_types[vreg] == NYR_MACH_TYPE_F32;
  if (!a64_ldur_fp(code, f32, (unsigned)reg, a64_slot_off(mach, &home)))
    return false;
  state->seeded[vreg] = true;
  return true;
}

static bool a64_mach_fpr_load(const nyr_mach_func_t *mach,
                              a64_mach_fpr_state_t *state,
                              const nyr_mach_operand_t *op, size_t inst,
                              unsigned scratch, nyr_obj_buf_t *code,
                              unsigned *out) {
  if (!op || !out)
    return false;
  bool f32 = a64_operand_type(mach, op) == NYR_MACH_TYPE_F32;
  if (op->kind == NYR_MACH_OPERAND_VREG) {
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
  if (op->kind == NYR_MACH_OPERAND_FRAME) {
    *out = scratch;
    return a64_ldur_fp(code, f32, scratch, a64_slot_off(mach, op));
  }
  return false;
}

static bool a64_mach_fpr_prepare(const nyr_mach_func_t *mach,
                                 a64_mach_fpr_state_t *state) {
  if (!mach || !state || mach->block_len != 1 || !mach->vreg_len)
    return false;
  bool has_float = false;
  for (size_t i = 0; i < mach->vreg_len; ++i)
    if (mach->vreg_types[i] != NYR_MACH_TYPE_F32 &&
        mach->vreg_types[i] != NYR_MACH_TYPE_F64 &&
        mach->vreg_types[i] != NYR_MACH_TYPE_I64)
      return false;
    else if (mach->vreg_types[i] == NYR_MACH_TYPE_F32 ||
             mach->vreg_types[i] == NYR_MACH_TYPE_F64)
      has_float = true;
  /*
   * The FPR fast path cannot improve integer-only functions.
   */
  if (!has_float)
    return false;
  for (size_t i = 0; i < mach->frame_slot_len; ++i)
    if (mach->frame_slots[i].type != NYR_MACH_TYPE_F32 &&
        mach->frame_slots[i].type != NYR_MACH_TYPE_F64)
      return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NYR_MACH_CALL || in->opcode == NYR_MACH_BR ||
        in->opcode == NYR_MACH_BR_IF)
      return false;
    switch (in->opcode) {
    case NYR_MACH_COPY:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          (in->src0.kind != NYR_MACH_OPERAND_VREG &&
           in->src0.kind != NYR_MACH_OPERAND_IMM))
        return false;
      break;
    case NYR_MACH_LOAD:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_FRAME)
        return false;
      break;
    case NYR_MACH_STORE:
      if (in->dst.kind != NYR_MACH_OPERAND_FRAME ||
          in->src0.kind != NYR_MACH_OPERAND_VREG)
        return false;
      break;
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_DIV:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG)
        return false;
      break;
    case NYR_MACH_RET:
      if (in->src0.kind != NYR_MACH_OPERAND_NONE &&
          in->src0.kind != NYR_MACH_OPERAND_VREG)
        return false;
      break;
    case NYR_MACH_CMP:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->dst.as.reg >= mach->vreg_len ||
          mach->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_I64 ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG ||
          (a64_operand_type(mach, &in->src0) != NYR_MACH_TYPE_F32 &&
           a64_operand_type(mach, &in->src0) != NYR_MACH_TYPE_F64) ||
          a64_operand_type(mach, &in->src0) !=
              a64_operand_type(mach, &in->src1))
        return false;
      break;
    case NYR_MACH_NOP:
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
    if (mach->vreg_types[v] == NYR_MACH_TYPE_F32 ||
        mach->vreg_types[v] == NYR_MACH_TYPE_F64)
      continue;
    bool defined = false;
    for (size_t i = 0; i < mach->inst_len; ++i) {
      const nyr_mach_inst_t *in = &mach->insts[i];
      if (in->dst.kind == NYR_MACH_OPERAND_VREG && in->dst.as.reg == v) {
        if (in->opcode != NYR_MACH_CMP || defined)
          return false;
        defined = true;
      }
      if (in->src0.kind == NYR_MACH_OPERAND_VREG && in->src0.as.reg == v &&
          in->opcode != NYR_MACH_RET)
        return false;
      if (in->src1.kind == NYR_MACH_OPERAND_VREG && in->src1.as.reg == v)
        return false;
    }
    if (!defined)
      return false;
  }
  if (!nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_FPR,
                                     A64_MACH_FPR_COLOR_N, &state->alloc))
    return false;
  for (size_t i = 0; i < state->alloc.segment_len; ++i)
    if (state->alloc.segments[i].color < 0 ||
        state->alloc.segments[i].color >= A64_MACH_FPR_COLOR_N) {
      nyr_mach_regalloc_free(&state->alloc);
      return false;
    }
  state->colors_len = mach->vreg_len;
  state->seeded = calloc(state->colors_len, sizeof(*state->seeded));
  if (!state->seeded) {
    nyr_mach_regalloc_free(&state->alloc);
    return false;
  }
  return true;
}

static bool a64_mach_fpr_encode(const nyr_mach_func_t *mach,
                                a64_mach_fpr_state_t *state,
                                nyr_obj_buf_t *code, bool user,
                                size_t *inst_offsets, char *err,
                                size_t err_len) {
  (void)err;
  (void)err_len;
  int frame = a64_mach_frame_size(mach);
  if (!a64_u32(code, 0xA9BF7BFDu) || !a64_u32(code, 0x910003FDu) ||
      (frame && (frame >= 4096 ||
                 !a64_u32(code, 0xD10003FFu | ((uint32_t)frame << 10)))))
    return false;
  if (user && !a64_mach_emit_param_spills(mach, code))
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    if (inst_offsets)
      inst_offsets[i] = code->len;
    const nyr_mach_inst_t *in = &mach->insts[i];
    if (!a64_mach_fpr_begin(state, i))
      return false;
    int dst = in->dst.kind == NYR_MACH_OPERAND_VREG
                  ? a64_mach_fpr_reg(state, in->dst.as.reg, i)
                  : -1;
    bool f32 = a64_operand_type(mach, &in->dst) == NYR_MACH_TYPE_F32;
    switch (in->opcode) {
    case NYR_MACH_COPY: {
      if (dst < 0)
        return false;
      if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
        if (!a64_mov_imm64(code, 16, (uint64_t)in->src0.as.imm) ||
            !a64_u32(code, (f32 ? 0x1E270000u : 0x9E670000u) | (16u << 5) |
                               (uint32_t)dst))
          return false;
      } else {
        unsigned src = 16;
        if (!a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &src) ||
            (src != (unsigned)dst &&
             !a64_u32(code, (f32 ? 0x1E204000u : 0x1E604000u) | (src << 5) |
                                (uint32_t)dst)))
          return false;
      }
      state->seeded[in->dst.as.reg] = true;
      break;
    }
    case NYR_MACH_LOAD: {
      if (dst < 0 || in->src0.kind != NYR_MACH_OPERAND_FRAME ||
          !a64_ldur_fp(code, f32, (unsigned)dst, a64_slot_off(mach, &in->src0)))
        return false;
      state->seeded[in->dst.as.reg] = true;
      break;
    }
    case NYR_MACH_STORE: {
      unsigned src = 16;
      if (in->dst.kind != NYR_MACH_OPERAND_FRAME ||
          !a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &src) ||
          !a64_stur_fp(code,
                       a64_operand_type(mach, &in->src0) == NYR_MACH_TYPE_F32,
                       src, a64_slot_off(mach, &in->dst)))
        return false;
      break;
    }
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_DIV: {
      unsigned a = 16, b = 17;
      if (dst < 0 ||
          !a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &a) ||
          !a64_mach_fpr_load(mach, state, &in->src1, i, 17, code, &b))
        return false;
      uint32_t base = f32 ? 0 : 0;
      if (in->opcode == NYR_MACH_ADD)
        base = f32 ? 0x1E202800u : 0x1E602800u;
      if (in->opcode == NYR_MACH_SUB)
        base = f32 ? 0x1E203800u : 0x1E603800u;
      if (in->opcode == NYR_MACH_MUL)
        base = f32 ? 0x1E200800u : 0x1E600800u;
      if (in->opcode == NYR_MACH_DIV)
        base = f32 ? 0x1E201800u : 0x1E601800u;
      if (!a64_u32(code, base | (b << 16) | (a << 5) | (uint32_t)dst))
        return false;
      state->seeded[in->dst.as.reg] = true;
      break;
    }
    case NYR_MACH_CMP: {
      unsigned a = 16, b = 17;
      bool cmp_f32 = a64_operand_type(mach, &in->src0) == NYR_MACH_TYPE_F32;
      if (!a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &a) ||
          !a64_mach_fpr_load(mach, state, &in->src1, i, 17, code, &b))
        return false;
      uint32_t fcmp = cmp_f32 ? 0x1E202000u : 0x1E602000u;
      if (!a64_u32(code, fcmp | (b << 16) | (a << 5)))
        return false;
      unsigned cond = 0xb; /* lt */
      switch (in->condition) {
      case NYR_MACH_COND_EQ:
        cond = 0x0;
        break;
      case NYR_MACH_COND_NE:
        cond = 0x1;
        break;
      case NYR_MACH_COND_LT:
        cond = 0xb;
        break;
      case NYR_MACH_COND_LE:
        cond = 0xd;
        break;
      case NYR_MACH_COND_GT:
        cond = 0xc;
        break;
      case NYR_MACH_COND_GE:
        cond = 0xa;
        break;
      default:
        return false;
      }
      /*
       * cset x0, cond; store the integer result in the vreg home.
       */
      if (!a64_u32(code, 0x9A9F07E0u | ((cond ^ 1u) << 12)) ||
          !a64_stur_x(code, 0, a64_slot_off(mach, &in->dst)))
        return false;
      break;
    }
    case NYR_MACH_RET:
      if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
        if (mach->vreg_types[in->src0.as.reg] == NYR_MACH_TYPE_I64) {
          if (!a64_ldur_x(code, 0, a64_slot_off(mach, &in->src0)))
            return false;
        } else {
          unsigned src = 16;
          if (!a64_mach_fpr_load(mach, state, &in->src0, i, 16, code, &src) ||
              (src != 0 && !a64_u32(code, (mach->vreg_types[in->src0.as.reg] ==
                                                   NYR_MACH_TYPE_F32
                                               ? 0x1E204000u
                                               : 0x1E604000u) |
                                              (src << 5))))
            return false;
        }
      }
      if (frame && !a64_u32(code, 0x910003FFu | ((uint32_t)frame << 10)))
        return false;
      if (!a64_u32(code, 0xA8C17BFDu) || !a64_ret(code))
        return false;
      break;
    case NYR_MACH_NOP:
      break;
    default:
      return false;
    }
    if (in->opcode != NYR_MACH_RET && !a64_mach_fpr_end(mach, state, i, code))
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
 *
 * Q0..Q7 ONLY. Q16/Q17 stay encoder scratch; v8../Q8..Q15 are AAPCS64
 * callee-saved and this path emits no saves.
 */
#define A64_MACH_VECTOR_COLOR_N 8

/*
 * Target-tier reporting uses the same physical-color budgets as the AArch64
 * encoder.  Unlike the fast-path eligibility checks below, telemetry still
 * summarizes uncolored segments: a spill is exactly the information the
 * report is meant to expose rather than a reason to suppress the row.
 */
static bool a64_mach_profile_hot_loop_block(const nyr_mach_func_t *mach,
                                            uint32_t block) {
  if (!mach || block >= mach->block_len)
    return false;
  uint32_t source_pc = mach->blocks[block].source_pc;
  return source_pc != UINT32_MAX && ny_native_profile_loop_hot(source_pc) >= 16;
}

static size_t a64_mach_regalloc_hot_peak_live(const nyr_mach_func_t *mach,
                                              const nyr_mach_regalloc_t *a,
                                              size_t inst_len) {
  if (!mach || !a || !inst_len)
    return 0;
  int *delta = calloc(inst_len + 1, sizeof(*delta));
  if (!delta)
    return 0;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &a->segments[i];
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

static void a64_mach_regalloc_summarize(const nyr_mach_func_t *mach,
                                        const nyr_mach_regalloc_t *a,
                                        size_t inst_len,
                                        nyr_native_regalloc_metrics_t *out) {
  if (!out)
    return;
  *out = (nyr_native_regalloc_metrics_t){0};
  if (!a)
    return;
  out->segments = a->segment_len;
  for (size_t i = 0; i < a->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &a->segments[i];
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
  out->peak_live = nyr_mach_regalloc_peak_live(a, inst_len);
  out->hot_loop_peak_live = a64_mach_regalloc_hot_peak_live(mach, a, inst_len);
}

bool nyr_native_a64_regalloc_metrics(const nyr_mach_func_t *mach,
                                     nyr_native_regalloc_metrics_t *gpr,
                                     nyr_native_regalloc_metrics_t *fpr,
                                     nyr_native_regalloc_metrics_t *vector) {
  if (gpr)
    *gpr = (nyr_native_regalloc_metrics_t){0};
  if (fpr)
    *fpr = (nyr_native_regalloc_metrics_t){0};
  if (vector)
    *vector = (nyr_native_regalloc_metrics_t){0};
  if (!mach)
    return false;

  nyr_mach_regalloc_t alloc = {0};
  if (mach->vreg_len &&
      !nyr_mach_regalloc_build(mach, A64_MACH_COLOR_N, &alloc))
    return false;
  a64_mach_regalloc_summarize(mach, &alloc, mach->inst_len, gpr);
  nyr_mach_regalloc_free(&alloc);

  if (mach->vreg_len &&
      !nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_FPR,
                                     A64_MACH_FPR_COLOR_N, &alloc))
    return false;
  a64_mach_regalloc_summarize(mach, &alloc, mach->inst_len, fpr);
  nyr_mach_regalloc_free(&alloc);

  if (mach->vreg_len &&
      !nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_VECTOR,
                                     A64_MACH_VECTOR_COLOR_N, &alloc))
    return false;
  a64_mach_regalloc_summarize(mach, &alloc, mach->inst_len, vector);
  nyr_mach_regalloc_free(&alloc);
  return true;
}

typedef struct {
  nyr_mach_regalloc_t alloc;
  bool *seeded;
  size_t colors_len;
} a64_mach_vector_state_t;

static int a64_mach_vector_reg(const a64_mach_vector_state_t *state,
                               uint32_t vreg, size_t inst) {
  if (!state || vreg >= state->alloc.vreg_len)
    return -1;
  const nyr_mach_live_segment_t *seg =
      nyr_mach_regalloc_segment_at(&state->alloc, vreg, inst);
  if (!seg || seg->color < 0 || seg->color >= A64_MACH_VECTOR_COLOR_N)
    return -1;
  return seg->color;
}

static const nyr_mach_live_segment_t *
a64_mach_vector_segment(const a64_mach_vector_state_t *state, uint32_t vreg,
                        size_t inst) {
  return state ? nyr_mach_regalloc_segment_at(&state->alloc, vreg, inst) : NULL;
}

static bool a64_mach_is_v128(const nyr_mach_func_t *m,
                             const nyr_mach_operand_t *op) {
  if (!op || op->kind != NYR_MACH_OPERAND_VREG || op->as.reg >= m->vreg_len ||
      !m->vreg_types)
    return false;
  nyr_mach_type_t t = m->vreg_types[op->as.reg];
  return t == NYR_MACH_TYPE_V128_I64 || t == NYR_MACH_TYPE_V128_F64 ||
         t == NYR_MACH_TYPE_V128_F32;
}

static bool a64_mach_vector_carries(const a64_mach_vector_state_t *state,
                                    uint32_t vreg,
                                    const nyr_mach_live_segment_t *seg) {
  if (!state || !seg || seg->start == 0 || seg->reload || seg->color < 0)
    return false;
  const nyr_mach_live_segment_t *prev =
      a64_mach_vector_segment(state, vreg, seg->start - 1);
  /*
   * Every CFG edge communicates through the vreg home. Reusing a color on
   * both sides of an edge must not be mistaken for register residency.
   */
  return prev && prev->block == seg->block && prev->end + 1 == seg->start &&
         !prev->spill && prev->color == seg->color;
}

static bool a64_mach_vector_begin(a64_mach_vector_state_t *state, size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const nyr_mach_live_segment_t *seg =
        a64_mach_vector_segment(state, v, inst);
    if (seg && seg->start == inst)
      state->seeded[v] = a64_mach_vector_carries(state, v, seg);
  }
  return true;
}

/*
 * Try to fold single-block machine form to a const i64 return.
 */
static bool a64_try_const_ret(const nyr_mach_func_t *mach, int64_t *out) {
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
    const nyr_mach_inst_t *in = &mach->insts[i];
    switch (in->opcode) {
    case NYR_MACH_COPY:
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->src0.kind == NYR_MACH_OPERAND_IMM && known) {
        known[in->dst.as.reg] = true;
        val[in->dst.as.reg] = in->src0.as.imm;
      } else if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
                 in->src0.kind == NYR_MACH_OPERAND_VREG && known &&
                 in->src0.as.reg < mach->vreg_len && known[in->src0.as.reg]) {
        known[in->dst.as.reg] = true;
        val[in->dst.as.reg] = val[in->src0.as.reg];
      } else {
        free(val);
        free(known);
        return false;
      }
      break;
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_AND:
    case NYR_MACH_OR:
    case NYR_MACH_XOR:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG || !known ||
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
        case NYR_MACH_ADD:
          r = a + b;
          break;
        case NYR_MACH_SUB:
          r = a - b;
          break;
        case NYR_MACH_MUL:
          r = a * b;
          break;
        case NYR_MACH_AND:
          r = a & b;
          break;
        case NYR_MACH_OR:
          r = a | b;
          break;
        case NYR_MACH_XOR:
          r = a ^ b;
          break;
        default:
          break;
        }
        known[in->dst.as.reg] = true;
        val[in->dst.as.reg] = r;
      }
      break;
    case NYR_MACH_RET:
      if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
        *out = in->src0.as.imm;
        free(val);
        free(known);
        return true;
      }
      if (in->src0.kind == NYR_MACH_OPERAND_VREG && known &&
          in->src0.as.reg < mach->vreg_len && known[in->src0.as.reg]) {
        *out = val[in->src0.as.reg];
        free(val);
        free(known);
        return true;
      }
      free(val);
      free(known);
      return false;
    case NYR_MACH_NOP:
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

static bool a64_mach_type_v128(nyr_mach_type_t type) {
  return type == NYR_MACH_TYPE_V128_I64 || type == NYR_MACH_TYPE_V128_F64 ||
         type == NYR_MACH_TYPE_V128_F32;
}

static nyr_mach_type_t a64_operand_type(const nyr_mach_func_t *mach,
                                        const nyr_mach_operand_t *op) {
  if (!mach || !op)
    return NYR_MACH_TYPE_NONE;
  if (op->kind == NYR_MACH_OPERAND_VREG && op->as.reg < mach->vreg_len &&
      mach->vreg_types)
    return mach->vreg_types[op->as.reg];
  if (op->kind == NYR_MACH_OPERAND_FRAME &&
      op->as.frame_index < mach->frame_slot_len && mach->frame_slots)
    return mach->frame_slots[op->as.frame_index].type;
  return NYR_MACH_TYPE_NONE;
}

static size_t a64_frame_slot_size(const nyr_mach_frame_slot_t *slot) {
  size_t size = slot && slot->size ? slot->size : 8;
  if (slot && a64_mach_type_v128(slot->type) && size < 16)
    size = 16;
  return size < 8 ? 8 : size;
}

static size_t a64_frame_slot_align(const nyr_mach_frame_slot_t *slot) {
  size_t align = slot && slot->align ? slot->align : 8;
  if (slot && a64_mach_type_v128(slot->type) && align < 16)
    align = 16;
  if (align < 8)
    align = 8;
  return align;
}

static size_t a64_frame_slots_end(const nyr_mach_func_t *mach) {
  size_t cursor = 0;
  if (!mach)
    return 0;
  for (size_t i = 0; i < mach->frame_slot_len; ++i) {
    cursor =
        a64_mach_align(cursor, a64_frame_slot_align(&mach->frame_slots[i]));
    cursor += a64_frame_slot_size(&mach->frame_slots[i]);
  }
  return cursor;
}

static int a64_slot_off(const nyr_mach_func_t *mach,
                        const nyr_mach_operand_t *op) {
  if (!mach || !op)
    return 0;
  size_t cursor = 0;
  if (op->kind == NYR_MACH_OPERAND_FRAME) {
    if (op->as.frame_index >= mach->frame_slot_len)
      return 0;
    for (size_t i = 0; i <= op->as.frame_index; ++i) {
      cursor =
          a64_mach_align(cursor, a64_frame_slot_align(&mach->frame_slots[i]));
      cursor += a64_frame_slot_size(&mach->frame_slots[i]);
    }
    return -(int)cursor;
  }
  if (op->kind == NYR_MACH_OPERAND_VREG) {
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

static int a64_mach_frame_size(const nyr_mach_func_t *mach) {
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

static bool a64_is_v128(const nyr_mach_func_t *mach,
                        const nyr_mach_operand_t *op) {
  return a64_mach_type_v128(a64_operand_type(mach, op));
}

static bool a64_is_f32(const nyr_mach_func_t *mach,
                       const nyr_mach_operand_t *op) {
  return a64_operand_type(mach, op) == NYR_MACH_TYPE_F32;
}

static bool a64_is_float(const nyr_mach_func_t *mach,
                         const nyr_mach_operand_t *op) {
  nyr_mach_type_t type = a64_operand_type(mach, op);
  return type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64;
}

static bool a64_frame_addr(nyr_obj_buf_t *code, unsigned reg, int off) {
  if (!code || reg > 30 || off < -4095 || off > 4095)
    return false;
  uint32_t imm = (uint32_t)(off < 0 ? -off : off);
  uint32_t op = off < 0 ? 0xD1000000u : 0x91000000u;
  return a64_u32(code, op | (imm << 10) | (29u << 5) | reg);
}

static bool a64_q_unscaled(nyr_obj_buf_t *code, bool load, unsigned reg,
                           unsigned base, int off) {
  if (!code || reg > 31 || base > 31 || off < -256 || off > 255)
    return false;
  uint32_t op = load ? 0x3CC00000u : 0x3C800000u;
  return a64_u32(code, op | ((uint32_t)(off & 0x1ff) << 12) |
                           ((base & 31u) << 5) | reg);
}

static bool a64_load_q(nyr_obj_buf_t *code, unsigned reg, int off) {
  if (off >= -256 && off <= 255)
    return a64_q_unscaled(code, true, reg, 29, off);
  return a64_frame_addr(code, 16, off) &&
         a64_q_unscaled(code, true, reg, 16, 0);
}

static bool a64_store_q(nyr_obj_buf_t *code, unsigned reg, int off) {
  if (off >= -256 && off <= 255)
    return a64_q_unscaled(code, false, reg, 29, off);
  return a64_frame_addr(code, 16, off) &&
         a64_q_unscaled(code, false, reg, 16, 0);
}

static bool a64_mach_vector_end(a64_mach_vector_state_t *state,
                                const nyr_mach_func_t *mach,
                                nyr_obj_buf_t *code, size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    const nyr_mach_live_segment_t *seg =
        a64_mach_vector_segment(state, v, inst);
    if (!seg || seg->end != inst)
      continue;
    const nyr_mach_live_segment_t *next =
        a64_mach_vector_segment(state, v, inst + 1);
    bool needs_home =
        seg->spill || (next && !a64_mach_vector_carries(state, v, next));
    if (needs_home && seg->color >= 0 && state->seeded[v]) {
      nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_VREG, .as.reg = v};
      if (!a64_store_q(code, (unsigned)seg->color, a64_slot_off(mach, &home)))
        return false;
    }
    if (seg->spill || !next || !a64_mach_vector_carries(state, v, next))
      state->seeded[v] = false;
  }
  return true;
}

static bool a64_mach_vector_flush(a64_mach_vector_state_t *state,
                                  const nyr_mach_func_t *mach,
                                  nyr_obj_buf_t *code, size_t inst) {
  if (!state || !state->seeded)
    return true;
  for (uint32_t v = 0; v < state->colors_len; ++v) {
    if (!state->seeded[v])
      continue;
    const nyr_mach_live_segment_t *seg =
        a64_mach_vector_segment(state, v, inst);
    if (!seg || seg->color < 0)
      continue;
    nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_VREG, .as.reg = v};
    if (!a64_store_q(code, (unsigned)seg->color, a64_slot_off(mach, &home)))
      return false;
    state->seeded[v] = false;
  }
  return true;
}

static bool a64_mach_vector_prepare(const nyr_mach_func_t *mach,
                                    a64_mach_vector_state_t *state) {
  if (!mach || !state || !mach->vreg_types)
    return false;
  bool has_vector = false;
  for (size_t i = 0; i < mach->vreg_len; ++i) {
    nyr_mach_type_t type = mach->vreg_types[i];
    if (type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64 ||
        type == NYR_MACH_TYPE_I64 || type == NYR_MACH_TYPE_PTR)
      return false;
    if (a64_mach_type_v128(type))
      has_vector = true;
  }
  if (!has_vector)
    return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const nyr_mach_inst_t *in = &mach->insts[i];
    bool vector = false;
    const nyr_mach_operand_t *ops[] = {&in->dst, &in->src0, &in->src1,
                                       &in->src2};
    for (size_t k = 0; k < sizeof(ops) / sizeof(*ops); ++k)
      if (ops[k]->kind == NYR_MACH_OPERAND_VREG &&
          ops[k]->as.reg < mach->vreg_len &&
          a64_mach_type_v128(mach->vreg_types[ops[k]->as.reg]))
        vector = true;
    if (in->opcode == NYR_MACH_CALL) {
      if (a64_mach_is_v128(mach, &in->dst))
        return false;
      for (size_t ai = 0; ai < in->args_len; ++ai)
        if (a64_mach_is_v128(mach, &in->args[ai]))
          return false;
      continue;
    }
    if (in->opcode == NYR_MACH_BR_IF) {
      if (in->src0.kind != NYR_MACH_OPERAND_VREG)
        return false;
      continue;
    }
    if (in->opcode == NYR_MACH_RET) {
      if (in->src0.kind == NYR_MACH_OPERAND_VREG)
        return false;
      continue;
    }
    if (in->opcode == NYR_MACH_NOP)
      continue;
    if (!vector)
      return false;
    switch (in->opcode) {
    case NYR_MACH_COPY:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          (in->src0.kind != NYR_MACH_OPERAND_VREG &&
           in->src0.kind != NYR_MACH_OPERAND_FRAME))
        return false;
      break;
    case NYR_MACH_LOAD:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_FRAME)
        return false;
      break;
    case NYR_MACH_STORE:
      if (in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->dst.kind != NYR_MACH_OPERAND_FRAME)
        return false;
      break;
    case NYR_MACH_ADD:
    case NYR_MACH_SUB:
    case NYR_MACH_MUL:
    case NYR_MACH_AND:
    case NYR_MACH_OR:
    case NYR_MACH_XOR:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG)
        return false;
      if ((in->opcode == NYR_MACH_AND || in->opcode == NYR_MACH_OR ||
           in->opcode == NYR_MACH_XOR) &&
          a64_operand_type(mach, &in->dst) != NYR_MACH_TYPE_V128_I64)
        return false;
      break;
    case NYR_MACH_DIV:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG ||
          a64_operand_type(mach, &in->dst) == NYR_MACH_TYPE_V128_I64)
        return false;
      break;
    case NYR_MACH_SHL:
    case NYR_MACH_SAR:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG ||
          a64_operand_type(mach, &in->dst) != NYR_MACH_TYPE_V128_I64)
        return false;
      break;
    case NYR_MACH_FMA:
      if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
          in->src0.kind != NYR_MACH_OPERAND_VREG ||
          in->src1.kind != NYR_MACH_OPERAND_VREG ||
          in->src2.kind != NYR_MACH_OPERAND_VREG ||
          a64_operand_type(mach, &in->dst) == NYR_MACH_TYPE_V128_I64)
        return false;
      break;
    case NYR_MACH_NOP:
      break;
    default:
      return false;
    }
  }
  if (!nyr_mach_regalloc_build_class(mach, NYR_MACH_REGCLASS_VECTOR,
                                     A64_MACH_VECTOR_COLOR_N, &state->alloc))
    return false;
  state->colors_len = mach->vreg_len;
  state->seeded =
      calloc(state->colors_len ? state->colors_len : 1, sizeof(*state->seeded));
  if (!state->seeded) {
    nyr_mach_regalloc_free(&state->alloc);
    return false;
  }
  for (size_t i = 0; i < state->alloc.segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &state->alloc.segments[i];
    if (seg->color < 0 || seg->color >= A64_MACH_VECTOR_COLOR_N) {
      free(state->seeded);
      state->seeded = NULL;
      nyr_mach_regalloc_free(&state->alloc);
      return false;
    }
  }
  return true;
}

static bool a64_mach_vector_load_operand(const nyr_mach_func_t *mach,
                                         a64_mach_vector_state_t *state,
                                         const nyr_mach_operand_t *op,
                                         size_t inst, unsigned scratch,
                                         unsigned *out, nyr_obj_buf_t *code) {
  if (!op || !out || !code)
    return false;
  if (op->kind == NYR_MACH_OPERAND_VREG) {
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
  if (op->kind != NYR_MACH_OPERAND_FRAME ||
      !a64_load_q(code, scratch, a64_slot_off(mach, op)))
    return false;
  *out = scratch;
  return true;
}

static bool a64_mach_vector_copy(const nyr_mach_func_t *mach,
                                 a64_mach_vector_state_t *state,
                                 const nyr_mach_inst_t *in, size_t inst,
                                 nyr_obj_buf_t *code) {
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned src = 16;
  if (dst < 0 || !a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16,
                                               &src, code))
    return false;
  if (src != (unsigned)dst &&
      !a64_u32(code, 0x4EA01C00u | (src << 16) | ((unsigned)src << 5) |
                         (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_load(const nyr_mach_func_t *mach,
                                 a64_mach_vector_state_t *state,
                                 const nyr_mach_inst_t *in, size_t inst,
                                 nyr_obj_buf_t *code) {
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  if (dst < 0 || in->src0.kind != NYR_MACH_OPERAND_FRAME ||
      !a64_load_q(code, (unsigned)dst, a64_slot_off(mach, &in->src0)))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_store(const nyr_mach_func_t *mach,
                                  a64_mach_vector_state_t *state,
                                  const nyr_mach_inst_t *in, size_t inst,
                                  nyr_obj_buf_t *code) {
  unsigned src = 16;
  return in->dst.kind == NYR_MACH_OPERAND_FRAME &&
         a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &src,
                                      code) &&
         a64_store_q(code, src, a64_slot_off(mach, &in->dst));
}

static bool a64_mach_vector_binop(const nyr_mach_func_t *mach,
                                  a64_mach_vector_state_t *state,
                                  const nyr_mach_inst_t *in, size_t inst,
                                  nyr_obj_buf_t *code) {
  nyr_mach_type_t type = a64_operand_type(mach, &in->dst);
  uint32_t op = 0;
  if (type == NYR_MACH_TYPE_V128_I64) {
    if (in->opcode == NYR_MACH_ADD)
      op = 0x4EE08400u;
    else if (in->opcode == NYR_MACH_SUB)
      op = 0x6EE08400u;
    else if (in->opcode == NYR_MACH_MUL)
      return false;
    else if (in->opcode == NYR_MACH_AND)
      op = 0x4E201C00u;
    else if (in->opcode == NYR_MACH_OR)
      op = 0x4EA01C00u;
    else if (in->opcode == NYR_MACH_XOR)
      op = 0x6E201C00u;
  } else if (type == NYR_MACH_TYPE_V128_F64) {
    if (in->opcode == NYR_MACH_ADD)
      op = 0x4E60D400u;
    else if (in->opcode == NYR_MACH_SUB)
      op = 0x4EE0D400u;
    else if (in->opcode == NYR_MACH_MUL)
      op = 0x6E60DC00u;
  } else if (type == NYR_MACH_TYPE_V128_F32) {
    if (in->opcode == NYR_MACH_ADD)
      op = 0x4E20D400u;
    else if (in->opcode == NYR_MACH_SUB)
      op = 0x4EA0D400u;
    else if (in->opcode == NYR_MACH_MUL)
      op = 0x6E20DC00u;
  }
  if (!op)
    return false;
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned a = 16, b = 17;
  if (dst < 0 ||
      !a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &a,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src1, inst, 17, &b, code))
    return false;
  if (!a64_u32(code, op | (b << 16) | (a << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_div(const nyr_mach_func_t *mach,
                                a64_mach_vector_state_t *state,
                                const nyr_mach_inst_t *in, size_t inst,
                                nyr_obj_buf_t *code) {
  nyr_mach_type_t type = a64_operand_type(mach, &in->dst);
  uint32_t op = type == NYR_MACH_TYPE_V128_F64   ? 0x6E60FC00u
                : type == NYR_MACH_TYPE_V128_F32 ? 0x6E20FC00u
                                                 : 0;
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

static bool a64_mach_vector_shift(const nyr_mach_func_t *mach,
                                  a64_mach_vector_state_t *state,
                                  const nyr_mach_inst_t *in, size_t inst,
                                  nyr_obj_buf_t *code) {
  if (a64_operand_type(mach, &in->dst) != NYR_MACH_TYPE_V128_I64)
    return false;
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned a = 16, b = 17;
  if (dst < 0 ||
      !a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &a,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src1, inst, 17, &b, code))
    return false;
  uint32_t op = in->opcode == NYR_MACH_SAR ? 0x6EE0B842u : 0x4EE24420u;
  if (!a64_u32(code, op) ||
      !a64_u32(code, op | (b << 16) | (a << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_fma(const nyr_mach_func_t *mach,
                                a64_mach_vector_state_t *state,
                                const nyr_mach_inst_t *in, size_t inst,
                                nyr_obj_buf_t *code) {
  nyr_mach_type_t type = a64_operand_type(mach, &in->dst);
  uint32_t op = type == NYR_MACH_TYPE_V128_F64   ? 0x4E60CC00u
                : type == NYR_MACH_TYPE_V128_F32 ? 0x4E20CC00u
                                                 : 0;
  int dst = a64_mach_vector_reg(state, in->dst.as.reg, inst);
  unsigned a = 16, b = 17, c = 18;
  if (!op || dst < 0 ||
      !a64_mach_vector_load_operand(mach, state, &in->src0, inst, 16, &a,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src1, inst, 17, &b,
                                    code) ||
      !a64_mach_vector_load_operand(mach, state, &in->src2, inst, 18, &c, code))
    return false;
  /*
   * FMLA is destructive: Vd = Vd + Vn * Vm.  Seed Vd with the addend
   * before applying the multiply-add.
   */
  if (c != (unsigned)dst &&
      !a64_u32(code, 0x4EA01C00u | (c << 16) | (c << 5) | (unsigned)dst))
    return false;
  if (!a64_u32(code, op | (b << 16) | (a << 5) | (unsigned)dst))
    return false;
  state->seeded[in->dst.as.reg] = true;
  return true;
}

static bool a64_mach_vector_encode_call(
    const nyr_mach_func_t *mach, a64_mach_vector_state_t *state,
    const nyr_mach_inst_t *in, size_t inst, nyr_obj_buf_t *code,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, size_t reloc_cap,
    size_t code_base, const ny_native_target_info_t *target) {
  (void)mach;
  (void)code_base;
  if (in->call_is_extern && in->src0.kind == NYR_MACH_OPERAND_SYMBOL) {
    const char *raw = in->src0.as.symbol;
    if (!raw)
      return false;
    char call_sym[256];
    const char *pref =
        target && target->symbol_prefix ? target->symbol_prefix : "";
    if (in->call_is_extern || ny_native_runtime_abi_decl(raw) != NULL ||
        strncmp(raw, "ny_fn_", 6) == 0 || raw[0] == '_')
      snprintf(call_sym, sizeof(call_sym), "%s%s", pref, raw);
    else
      snprintf(call_sym, sizeof(call_sym), NY_FMT_FN, pref, raw);
    if (!a64_u32(code, 0x94000000u))
      return false;
    /*
     * An unrelocated BL+0 is a self-loop at runtime; overflow is a hard
     * error, not a silent skip.
     */
    if (!relocs || !reloc_count || *reloc_count >= reloc_cap)
      return false;
    snprintf(relocs[*reloc_count].symbol, sizeof(relocs[*reloc_count].symbol),
             "%s", call_sym);
    relocs[*reloc_count].disp_off = code->len - 4;
    relocs[*reloc_count].type = NY_RELOC_AARCH64_CALL26;
    (*reloc_count)++;
    return true;
  } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
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

static bool a64_mach_vector_encode(
    const nyr_mach_func_t *mach, a64_mach_vector_state_t *state,
    nyr_obj_buf_t *code, bool user, size_t *inst_offsets,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, size_t reloc_cap,
    size_t code_base, const ny_native_target_info_t *target, char *err,
    size_t err_len) {
  (void)code_base;
  int frame = a64_mach_frame_size(mach);
  if (!a64_u32(code, 0xA9BF7BFDu) || !a64_u32(code, 0x910003FDu))
    return false;
  if (frame &&
      (frame >= 4096 || !a64_u32(code, 0xD10003FFu | ((uint32_t)frame << 10))))
    return false;
  if (user && !a64_mach_emit_param_spills(mach, code))
    return false;
  size_t *block_off =
      calloc(mach->block_len ? mach->block_len : 1, sizeof(*block_off));
  size_t *patch_at =
      calloc(mach->inst_len ? mach->inst_len : 1, sizeof(*patch_at));
  size_t *patch_blk =
      calloc(mach->inst_len ? mach->inst_len : 1, sizeof(*patch_blk));
  size_t patch_len = 0;
  if (!block_off || !patch_at || !patch_blk)
    goto fail;
  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    const nyr_mach_block_t *block = &mach->blocks[bi];
    block_off[bi] = code->len;
    for (size_t n = 0; n < block->inst_count; ++n) {
      size_t inst = block->first_inst + n;
      if (inst_offsets && inst < mach->inst_len)
        inst_offsets[inst] = code->len;
      const nyr_mach_inst_t *in = &mach->insts[inst];
      if (!a64_mach_vector_begin(state, inst))
        goto fail;
      bool ok = false;
      switch (in->opcode) {
      case NYR_MACH_COPY:
        ok = a64_mach_vector_copy(mach, state, in, inst, code);
        break;
      case NYR_MACH_LOAD:
        ok = a64_mach_vector_load(mach, state, in, inst, code);
        break;
      case NYR_MACH_STORE:
        ok = a64_mach_vector_store(mach, state, in, inst, code);
        break;
      case NYR_MACH_ADD:
      case NYR_MACH_SUB:
      case NYR_MACH_MUL:
      case NYR_MACH_AND:
      case NYR_MACH_OR:
      case NYR_MACH_XOR:
        ok = a64_mach_vector_binop(mach, state, in, inst, code);
        break;
      case NYR_MACH_DIV:
        ok = a64_mach_vector_div(mach, state, in, inst, code);
        break;
      case NYR_MACH_SHL:
      case NYR_MACH_SAR:
        ok = a64_mach_vector_shift(mach, state, in, inst, code);
        break;
      case NYR_MACH_FMA:
        ok = a64_mach_vector_fma(mach, state, in, inst, code);
        break;
      case NYR_MACH_BR:
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
      case NYR_MACH_BR_IF:
        /*
         * The shared patch loop rewrites every site as an UNCONDITIONAL B,
         * so a CBNZ placeholder would lose its condition entirely and
         * become an always-taken branch. The prepare gate already rejects
         * conditional vector forms; keep it that way.
         */
        ok = false;
        if (err && err_len)
          snprintf(err, err_len,
                   "aarch64 vector encode: conditional branch unsupported");
        break;
      case NYR_MACH_CALL:
        if (!a64_mach_vector_flush(state, mach, code, inst))
          ok = false;
        else
          ok = a64_mach_vector_encode_call(mach, state, in, inst, code, relocs,
                                           reloc_count, reloc_cap, 0, target);
        break;
      case NYR_MACH_RET:
        if (frame && !a64_u32(code, 0x910003FFu | ((uint32_t)frame << 10)))
          ok = false;
        else
          ok = a64_u32(code, 0xA8C17BFDu) && a64_ret(code);
        break;
      case NYR_MACH_NOP:
        ok = true;
        break;
      default:
        ok = false;
        break;
      }
      if (!ok || !a64_mach_vector_end(state, mach, code, inst)) {
        if (err && err_len)
          snprintf(
              err, err_len,
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
    int64_t delta = (int64_t)block_off[patch_blk[i]] - (int64_t)patch_at[i];
    if ((delta & 3) != 0 || delta / 4 < -(1ll << 25) ||
        delta / 4 >= (1ll << 25))
      goto fail;
    nyr_obj_patch_u32(code, patch_at[i],
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

static bool a64_q_ptr(nyr_obj_buf_t *code, bool load, unsigned reg,
                      unsigned base) {
  if (!code || reg > 31 || base > 30)
    return false;
  uint32_t op = load ? 0x3DC00000u : 0x3D800000u;
  return a64_u32(code, op | (base << 5) | reg);
}

static bool a64_ldur_x(nyr_obj_buf_t *code, unsigned reg, int off) {
  if (reg > 30)
    return false;
  if (off >= -256 && off <= 255)
    return a64_u32(code, 0xF8400000u | ((uint32_t)(off & 0x1ff) << 12) |
                             (29u << 5) | reg);
  return a64_frame_addr(code, 16, off) &&
         a64_u32(code, 0xF9400000u | (16u << 5) | reg);
}

static bool a64_stur_x(nyr_obj_buf_t *code, unsigned reg, int off) {
  if (reg > 30)
    return false;
  if (off >= -256 && off <= 255)
    return a64_u32(code, 0xF8000000u | ((uint32_t)(off & 0x1ff) << 12) |
                             (29u << 5) | reg);
  unsigned scratch = reg == 16 ? 17u : 16u;
  return a64_frame_addr(code, scratch, off) &&
         a64_u32(code, 0xF9000000u | (scratch << 5) | reg);
}

static bool a64_ldur_fp(nyr_obj_buf_t *code, bool f32, unsigned reg, int off) {
  if (reg > 31)
    return false;
  if (off >= -256 && off <= 255) {
    uint32_t op = f32 ? 0xBC400000u : 0xFC400000u;
    return a64_u32(code,
                   op | ((uint32_t)(off & 0x1ff) << 12) | (29u << 5) | reg);
  }
  uint32_t op = f32 ? 0xBD400000u : 0xFD400000u;
  return a64_frame_addr(code, 16, off) && a64_u32(code, op | (16u << 5) | reg);
}

static bool a64_stur_fp(nyr_obj_buf_t *code, bool f32, unsigned reg, int off) {
  if (reg > 31)
    return false;
  if (off >= -256 && off <= 255) {
    uint32_t op = f32 ? 0xBC000000u : 0xFC000000u;
    return a64_u32(code,
                   op | ((uint32_t)(off & 0x1ff) << 12) | (29u << 5) | reg);
  }
  uint32_t op = f32 ? 0xBD000000u : 0xFD000000u;
  return a64_frame_addr(code, 16, off) && a64_u32(code, op | (16u << 5) | reg);
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

static bool a64_ldr_x_base(nyr_obj_buf_t *code, unsigned reg, unsigned base,
                           int off) {
  if (reg > 30 || base > 31 || off < 0 || (off & 7) || off > 32760)
    return false;
  return a64_u32(code,
                 0xF9400000u | ((uint32_t)(off / 8) << 10) | (base << 5) | reg);
}

static bool a64_ldr_fp_base(nyr_obj_buf_t *code, bool f32, unsigned reg,
                            unsigned base, int off) {
  unsigned scale = f32 ? 4u : 8u;
  if (reg > 31 || base > 31 || off < 0 || off % (int)scale ||
      off / (int)scale > 4095)
    return false;
  uint32_t op = f32 ? 0xBD400000u : 0xFD400000u;
  return a64_u32(code,
                 op | ((uint32_t)(off / (int)scale) << 10) | (base << 5) | reg);
}

static bool a64_str_fp_base(nyr_obj_buf_t *code, bool f32, unsigned reg,
                            unsigned base, int off) {
  unsigned scale = f32 ? 4u : 8u;
  if (reg > 31 || base > 31 || off < 0 || off % (int)scale ||
      off / (int)scale > 4095)
    return false;
  uint32_t op = f32 ? 0xBD000000u : 0xFD000000u;
  return a64_u32(code,
                 op | ((uint32_t)(off / (int)scale) << 10) | (base << 5) | reg);
}

static bool a64_ldr_q_base(nyr_obj_buf_t *code, unsigned reg, unsigned base,
                           int off) {
  if (reg > 31 || base > 31 || off < 0 || (off & 15) || off > 65520)
    return false;
  return a64_u32(code, 0x3DC00000u | ((uint32_t)(off / 16) << 10) |
                           (base << 5) | reg);
}

static int a64_mach_param_count(const nyr_mach_func_t *mach) {
  if (!mach || !mach->frame_slot_len || !mach->block_len)
    return 0;
  if (mach->param_count > 0)
    return mach->param_count <= mach->frame_slot_len ? (int)mach->param_count
                                                     : -1;
  bool *written = calloc(mach->frame_slot_len, sizeof(bool));
  bool *param = calloc(mach->frame_slot_len, sizeof(bool));
  if (!written || !param) {
    free(written);
    free(param);
    return -1;
  }
  const nyr_mach_block_t *entry = &mach->blocks[0];
  for (size_t n = 0; n < entry->inst_count; ++n) {
    const nyr_mach_inst_t *in = &mach->insts[entry->first_inst + n];
    if (in->opcode == NYR_MACH_LOAD &&
        in->src0.kind == NYR_MACH_OPERAND_FRAME &&
        in->src0.as.frame_index < mach->frame_slot_len &&
        !written[in->src0.as.frame_index])
      param[in->src0.as.frame_index] = true;
    if (in->opcode == NYR_MACH_STORE &&
        in->dst.kind == NYR_MACH_OPERAND_FRAME &&
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

static bool a64_mach_has_aggregate_abi(const nyr_mach_func_t *mach) {
  if (!mach || mach->return_aggregate_size)
    return mach != NULL;
  for (size_t i = 0; i < mach->param_count; ++i)
    if (mach->param_types && mach->param_types[i] == NYIR_PARAM_AGGREGATE)
      return true;
  return false;
}

static bool a64_mach_spill_aggregate_param(const nyr_mach_func_t *mach,
                                           nyr_obj_buf_t *code, int local,
                                           unsigned *gp, unsigned *fp,
                                           unsigned *stack_off) {
  if (!mach || !code || !mach->param_abi || !mach->param_aggregate_home_slots ||
      local < 0 || (size_t)local >= mach->param_count || !gp || !fp ||
      !stack_off)
    return false;
  uint32_t packed = mach->param_abi[local];
  unsigned size = NYIR_ARG_AGG_SIZE(packed);
  unsigned c0 = NYIR_ARG_AGG_CLASS(packed, 0);
  unsigned c1 = NYIR_ARG_AGG_CLASS(packed, 1);
  if (!size)
    return false;

  int local_off = a64_slot_off(
      mach, &(nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_FRAME,
                                  .as.frame_index = (uint32_t)local});
  if (c0 == NYIR_ARG_CLASS_MEMORY) {
    if (*gp < 8) {
      if (!a64_stur_x(code, *gp, local_off))
        return false;
      ++*gp;
    } else {
      *stack_off = (*stack_off + 7u) & ~7u;
      if (!a64_ldr_x_base(code, 16, 29, 16 + (int)*stack_off) ||
          !a64_stur_x(code, 16, local_off))
        return false;
      *stack_off += 8;
    }
    return true;
  }

  uint32_t first = mach->param_aggregate_home_slots[local];
  if (first == UINT32_MAX)
    return false;
  if (c0 == NYIR_ARG_CLASS_HFA_F64) {
    unsigned count = size / 8u;
    if (count == 0 || count > 4 || size % 8u != 0)
      return false;
    if (*fp + count <= 8) {
      for (unsigned piece = 0; piece < count; ++piece) {
        nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_FRAME,
                                   .as.frame_index =
                                       first + count - piece - 1u};
        if (!a64_stur_fp(code, false, *fp + piece, a64_slot_off(mach, &home)))
          return false;
      }
      *fp += count;
    } else {
      *fp = 8;
      *stack_off = (*stack_off + 7u) & ~7u;
      for (unsigned piece = 0; piece < count; ++piece) {
        nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_FRAME,
                                   .as.frame_index =
                                       first + count - piece - 1u};
        if (!a64_ldr_fp_base(code, false, 16, 29,
                             16 + (int)*stack_off + (int)(piece * 8u)) ||
            !a64_stur_fp(code, false, 16, a64_slot_off(mach, &home)))
          return false;
      }
      *stack_off += size;
    }
  } else {
    unsigned chunks = (size + 7u) / 8u;
    bool align16 = c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16;
    if (!chunks || chunks > 2 ||
        (c0 != NYIR_ARG_CLASS_INTEGER &&
         c0 != NYIR_ARG_CLASS_AAPCS_INTEGER_A16 && c0 != NYIR_ARG_CLASS_NONE) ||
        (chunks > 1 && c1 != NYIR_ARG_CLASS_INTEGER &&
         c1 != NYIR_ARG_CLASS_NONE))
      return false;
    if (align16 && (*gp & 1u))
      ++*gp;
    if (*gp + chunks <= 8) {
      for (unsigned piece = 0; piece < chunks; ++piece) {
        nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_FRAME,
                                   .as.frame_index =
                                       first + chunks - piece - 1u};
        if (!a64_stur_x(code, *gp + piece, a64_slot_off(mach, &home)))
          return false;
      }
      *gp += chunks;
    } else {
      *gp = 8;
      unsigned align = align16 ? 16u : 8u;
      *stack_off = (*stack_off + align - 1u) & ~(align - 1u);
      for (unsigned piece = 0; piece < chunks; ++piece) {
        nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_FRAME,
                                   .as.frame_index =
                                       first + chunks - piece - 1u};
        if (!a64_ldr_x_base(code, 16, 29,
                            16 + (int)*stack_off + (int)(piece * 8u)) ||
            !a64_stur_x(code, 16, a64_slot_off(mach, &home)))
          return false;
      }
      *stack_off += chunks * 8u;
    }
  }
  unsigned chunks = c0 == NYIR_ARG_CLASS_HFA_F64 ? size / 8u : (size + 7u) / 8u;
  nyr_mach_operand_t home = {.kind = NYR_MACH_OPERAND_FRAME,
                             .as.frame_index = first + chunks - 1u};
  return a64_frame_addr(code, 9, a64_slot_off(mach, &home)) &&
         a64_stur_x(code, 9, local_off);
}

static bool a64_mach_load_return_piece(const nyr_mach_func_t *mach,
                                       nyr_obj_buf_t *code,
                                       const nyr_mach_operand_t *operand,
                                       unsigned reg, bool floating, bool f32) {
  if (!mach || !code || !operand || reg > 31 ||
      operand->kind == NYR_MACH_OPERAND_NONE)
    return false;
  if (floating)
    return operand->kind != NYR_MACH_OPERAND_IMM &&
           a64_ldur_fp(code, f32, reg, a64_slot_off(mach, operand));
  if (operand->kind == NYR_MACH_OPERAND_IMM)
    return a64_mov_imm64(code, reg, (uint64_t)operand->as.imm);
  return a64_ldur_x(code, reg, a64_slot_off(mach, operand));
}

static bool a64_mach_emit_param_spills(const nyr_mach_func_t *mach,
                                       nyr_obj_buf_t *code) {
  int count = a64_mach_param_count(mach);
  if (count < 0)
    return false;
  unsigned gp = 0, fp = 0, stack_off = 0;
  for (int i = 0; i < count; ++i) {
    nyr_mach_operand_t slot = {.kind = NYR_MACH_OPERAND_FRAME,
                               .as.frame_index = (uint32_t)i};
    int off = a64_slot_off(mach, &slot);
    if (i == 0 && mach->return_aggregate_size > 16u &&
        mach->return_aggregate_classes[0] == NYIR_ARG_CLASS_MEMORY) {
      if (!a64_stur_x(code, 8, off))
        return false;
      continue;
    }
    if ((size_t)i < mach->param_count && mach->param_types &&
        mach->param_types[i] == NYIR_PARAM_AGGREGATE) {
      if (!a64_mach_spill_aggregate_param(mach, code, i, &gp, &fp, &stack_off))
        return false;
      continue;
    }
    nyr_mach_type_t type = mach->frame_slots[i].type;
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
    } else if (type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64) {
      bool f32 = type == NYR_MACH_TYPE_F32;
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

static bool a64_encode_func(const nyr_mach_func_t *mach, nyr_obj_buf_t *code,
                            size_t **block_off_out, size_t **inst_off_out,
                            bool user_function, nyr_x64_obj_reloc_t *relocs,
                            size_t *reloc_count, size_t reloc_cap,
                            size_t code_base,
                            const ny_native_target_info_t *target, char *err,
                            size_t err_len) {
  if (!mach || !code)
    return false;
  if (block_off_out)
    *block_off_out = NULL;
  if (inst_off_out)
    *inst_off_out = NULL;
  size_t inst_offset_count = mach->inst_len ? mach->inst_len : 1;
  if (inst_offset_count > SIZE_MAX / sizeof(size_t))
    return false;
  size_t *inst_offsets = malloc(inst_offset_count * sizeof(*inst_offsets));
  if (!inst_offsets && mach->inst_len)
    return false;
  if (inst_offsets)
    memset(inst_offsets, 0xff, inst_offset_count * sizeof(*inst_offsets));
  if (err && err_len)
    err[0] = '\0';
  bool aggregate_abi = a64_mach_has_aggregate_abi(mach);
  a64_mach_fpr_state_t fpr_state = {0};
  if (!aggregate_abi && a64_mach_fpr_prepare(mach, &fpr_state)) {
    bool fpr_ok = a64_mach_fpr_encode(mach, &fpr_state, code, user_function,
                                      inst_offsets, err, err_len);
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
    nyr_native_mach_fpr_record(
        fpr_state.alloc.segment_len, colored, spilled, reloads,
        nyr_mach_regalloc_peak_live(&fpr_state.alloc, mach->inst_len));
    free(fpr_state.seeded);
    nyr_mach_regalloc_free(&fpr_state.alloc);
    if (inst_off_out)
      *inst_off_out = inst_offsets;
    else
      free(inst_offsets);
    return fpr_ok;
  }
  a64_mach_vector_state_t vector_state = {0};
  if (!aggregate_abi && a64_mach_vector_prepare(mach, &vector_state)) {
    bool vector_ok = a64_mach_vector_encode(
        mach, &vector_state, code, user_function, inst_offsets, relocs,
        reloc_count, reloc_cap, code_base, target, err, err_len);
    size_t colored = 0, spilled = 0, reloads = 0;
    for (size_t i = 0; i < vector_state.alloc.segment_len; ++i) {
      if (vector_state.alloc.segments[i].color >= 0)
        ++colored;
      else
        ++spilled;
      if (vector_state.alloc.segments[i].reload)
        ++reloads;
    }
    nyr_native_mach_vector_record(
        vector_state.alloc.segment_len, colored, spilled, reloads,
        nyr_mach_regalloc_peak_live(&vector_state.alloc, mach->inst_len));
    free(vector_state.seeded);
    nyr_mach_regalloc_free(&vector_state.alloc);
    if (inst_off_out)
      *inst_off_out = inst_offsets;
    else
      free(inst_offsets);
    return vector_ok;
  }
  nyr_mach_regalloc_t scalar_alloc = {0};
  if (!aggregate_abi && mach->block_len == 1 &&
      a64_mach_scalar_regalloc_prepare(mach, &scalar_alloc)) {
    bool scalar_ok = a64_mach_scalar_regalloc_encode(
        mach, &scalar_alloc, code, user_function, inst_offsets, err, err_len);
    size_t colored = 0, spilled = 0, reloads = 0;
    for (size_t i = 0; i < scalar_alloc.segment_len; ++i) {
      if (scalar_alloc.segments[i].color >= 0)
        ++colored;
      else
        ++spilled;
      if (scalar_alloc.segments[i].reload)
        ++reloads;
    }
    nyr_native_mach_regalloc_record(
        scalar_alloc.segment_len, colored, spilled, reloads,
        nyr_mach_regalloc_peak_live(&scalar_alloc, mach->inst_len));
    nyr_mach_regalloc_free(&scalar_alloc);
    if (inst_off_out)
      *inst_off_out = inst_offsets;
    else
      free(inst_offsets);
    return scalar_ok;
  }
  size_t current_inst = SIZE_MAX;
  unsigned current_opcode = 0;
  int frame = a64_mach_frame_size(mach);
  /*
   * stp x29,x30,[sp,#-16]!; mov x29,sp; sub sp,#frame
   */
  if (!a64_u32(code, 0xA9BF7BFD) || !a64_u32(code, 0x910003FD)) {
    free(inst_offsets);
    return false;
  }
  if (frame) {
    /*
     * sub sp, sp, #frame (imm12 must fit)
     */
    if (frame >= 4096) {
      if (err && err_len)
        snprintf(err, err_len, "a64 machine form: frame too large");
      free(inst_offsets);
      return false;
    }
    uint32_t sub = 0xD10003FF | ((uint32_t)frame << 10);
    if (!a64_u32(code, sub)) {
      free(inst_offsets);
      return false;
    }
  }
  if (user_function && !a64_mach_emit_param_spills(mach, code)) {
    if (err && err_len)
      snprintf(err, err_len, "parameter spill failed");
    free(inst_offsets);
    return false;
  }
  size_t *block_off =
      calloc(mach->block_len ? mach->block_len : 1, sizeof(size_t));
  if (!block_off && mach->block_len) {
    free(inst_offsets);
    return false;
  }
  size_t *patch_at = NULL, *patch_blk = NULL, npatch = 0, pcap = 0;

  for (size_t bi = 0; bi < mach->block_len; ++bi) {
    block_off[bi] = code->len;
    const nyr_mach_block_t *blk = &mach->blocks[bi];
    for (size_t n = 0; n < blk->inst_count; ++n) {
      current_inst = blk->first_inst + n;
      inst_offsets[current_inst] = code->len;
      const nyr_mach_inst_t *in = &mach->insts[current_inst];
      current_opcode = (unsigned)in->opcode;
      int dst = a64_slot_off(mach, &in->dst);
      int a = a64_slot_off(mach, &in->src0);
      int b = a64_slot_off(mach, &in->src1);
      switch (in->opcode) {
      case NYR_MACH_COPY: {
        if (a64_is_v128(mach, &in->dst)) {
          nyr_mach_type_t dt = a64_operand_type(mach, &in->dst);
          if (a64_is_v128(mach, &in->src0)) {
            if (!a64_load_q(code, 0, a) || !a64_store_q(code, 0, dst))
              goto fail;
          } else if (dt == NYR_MACH_TYPE_V128_I64) {
            if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
              if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
                goto fail;
            } else if (!a64_ldur_x(code, 0, a)) {
              goto fail;
            }
            if (!a64_u32(code, 0x4E080C00u) || !a64_store_q(code, 0, dst))
              goto fail;
          } else if (dt == NYR_MACH_TYPE_V128_F64) {
            if (in->src0.kind == NYR_MACH_OPERAND_IMM ||
                !a64_ldur_fp(code, false, 0, a) ||
                !a64_u32(code, 0x4E080400u) || !a64_store_q(code, 0, dst))
              goto fail;
          } else if (dt == NYR_MACH_TYPE_V128_F32) {
            if (in->src0.kind == NYR_MACH_OPERAND_IMM ||
                !a64_ldur_fp(code, true, 0, a) || !a64_u32(code, 0x4E040400u) ||
                !a64_store_q(code, 0, dst))
              goto fail;
          } else {
            goto fail;
          }
          break;
        }
        bool is_f64 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                      in->dst.as.reg < mach->vreg_len &&
                      mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F64;
        bool is_f32 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                      in->dst.as.reg < mach->vreg_len &&
                      mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F32;
        if (is_f64 || is_f32) {
          if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
            if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm) ||
                !a64_u32(code, is_f32 ? 0x1E270000u : 0x9E670000u))
              goto fail;
          } else if (!a64_ldur_fp(code, is_f32, 0, a)) {
            goto fail;
          }
          if (!a64_stur_fp(code, is_f32, 0, dst))
            goto fail;
          break;
        }
        if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
          if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
            goto fail;
        } else if (!a64_ldur_x(code, 0, a)) {
          goto fail;
        }
        if (!a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NYR_MACH_LEA: {
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
      case NYR_MACH_CONVERT: {
        bool dst_f64 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                       in->dst.as.reg < mach->vreg_len &&
                       mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F64;
        bool dst_f32 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                       in->dst.as.reg < mach->vreg_len &&
                       mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F32;
        bool src_f64 = in->src0.kind == NYR_MACH_OPERAND_VREG &&
                       in->src0.as.reg < mach->vreg_len &&
                       mach->vreg_types[in->src0.as.reg] == NYR_MACH_TYPE_F64;
        bool src_f32 = in->src0.kind == NYR_MACH_OPERAND_VREG &&
                       in->src0.as.reg < mach->vreg_len &&
                       mach->vreg_types[in->src0.as.reg] == NYR_MACH_TYPE_F32;
        if (dst_f64 && !src_f64 && !src_f32) {
          if (!a64_ldur_x(code, 0, a) || !a64_u32(code, 0x9E620000u) ||
              !a64_stur_fp(code, false, 0, dst))
            goto fail;
        } else if (dst_f32 && !src_f64 && !src_f32) {
          if (!a64_ldur_x(code, 0, a) || !a64_u32(code, 0x9E220000u) ||
              !a64_stur_fp(code, true, 0, dst))
            goto fail;
        } else if (!dst_f64 && !dst_f32 && src_f64) {
          if (!a64_ldur_fp(code, false, 0, a) || !a64_u32(code, 0x9E780000u) ||
              !a64_stur_x(code, 0, dst))
            goto fail;
        } else if (!dst_f64 && !dst_f32 && src_f32) {
          if (!a64_ldur_fp(code, true, 0, a) || !a64_u32(code, 0x9E380000u) ||
              !a64_stur_x(code, 0, dst))
            goto fail;
        } else if (dst_f64 && src_f32) {
          if (!a64_ldur_fp(code, true, 0, a) || !a64_u32(code, 0x1E22C000u) ||
              !a64_stur_fp(code, false, 0, dst))
            goto fail;
        } else if (dst_f32 && src_f64) {
          if (!a64_ldur_fp(code, false, 0, a) || !a64_u32(code, 0x1E624000u) ||
              !a64_stur_fp(code, true, 0, dst))
            goto fail;
        } else {
          goto fail;
        }
        break;
      }
      case NYR_MACH_LOAD:
        if (a64_is_v128(mach, &in->dst)) {
          if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
            if (!a64_load_q(code, 0, a))
              goto fail;
          } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
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
          if (in->src0.kind == NYR_MACH_OPERAND_FRAME) {
            if (!a64_ldur_fp(code, f32, 0, a) ||
                !a64_stur_fp(code, f32, 0, dst))
              goto fail;
          } else if (in->src0.kind == NYR_MACH_OPERAND_VREG) {
            if (!a64_ldur_x(code, 0, a) ||
                !a64_ldr_fp_base(code, f32, 0, 0, 0) ||
                !a64_stur_fp(code, f32, 0, dst))
              goto fail;
          } else {
            goto fail;
          }
        } else {
          if (in->src0.kind != NYR_MACH_OPERAND_FRAME)
            goto fail;
          if (!a64_ldur_x(code, 0, a) || !a64_stur_x(code, 0, dst))
            goto fail;
        }
        break;
      case NYR_MACH_STORE:
        if (a64_is_v128(mach, &in->src0)) {
          if (!a64_load_q(code, 1, a))
            goto fail;
          if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
            if (!a64_store_q(code, 1, dst))
              goto fail;
          } else if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
            if (!a64_ldur_x(code, 0, dst) || !a64_q_ptr(code, false, 1, 0))
              goto fail;
          } else {
            goto fail;
          }
          break;
        }
        if (a64_is_float(mach, &in->src0)) {
          bool f32 = a64_is_f32(mach, &in->src0);
          if (in->dst.kind == NYR_MACH_OPERAND_FRAME) {
            if (!a64_ldur_fp(code, f32, 0, a) ||
                !a64_stur_fp(code, f32, 0, dst))
              goto fail;
          } else if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
            if (!a64_ldur_x(code, 1, dst) || !a64_ldur_fp(code, f32, 0, a) ||
                !a64_str_fp_base(code, f32, 0, 1, 0))
              goto fail;
          } else {
            goto fail;
          }
        } else {
          if (in->dst.kind != NYR_MACH_OPERAND_FRAME)
            goto fail;
          if (!a64_ldur_x(code, 0, a) || !a64_stur_x(code, 0, dst))
            goto fail;
        }
        break;
      case NYR_MACH_ADD:
      case NYR_MACH_SUB:
      case NYR_MACH_AND:
      case NYR_MACH_OR:
      case NYR_MACH_XOR:
      case NYR_MACH_MUL: {
        if (a64_is_v128(mach, &in->dst)) {
          nyr_mach_type_t vt = a64_operand_type(mach, &in->dst);
          uint32_t op = 0;
          if (vt == NYR_MACH_TYPE_V128_I64) {
            if (in->opcode == NYR_MACH_ADD)
              op = 0x4EE08400u;
            else if (in->opcode == NYR_MACH_SUB)
              op = 0x6EE08400u;
            else if (in->opcode == NYR_MACH_AND)
              op = 0x4E201C00u;
            else if (in->opcode == NYR_MACH_OR)
              op = 0x4EA01C00u;
            else if (in->opcode == NYR_MACH_XOR)
              op = 0x6E201C00u;
            else
              goto fail;
          } else if (vt == NYR_MACH_TYPE_V128_F64) {
            if (in->opcode == NYR_MACH_ADD)
              op = 0x4E60D400u;
            else if (in->opcode == NYR_MACH_SUB)
              op = 0x4EE0D400u;
            else if (in->opcode == NYR_MACH_MUL)
              op = 0x6E60DC00u;
            else
              goto fail;
          } else if (vt == NYR_MACH_TYPE_V128_F32) {
            if (in->opcode == NYR_MACH_ADD)
              op = 0x4E20D400u;
            else if (in->opcode == NYR_MACH_SUB)
              op = 0x4EA0D400u;
            else if (in->opcode == NYR_MACH_MUL)
              op = 0x6E20DC00u;
            else
              goto fail;
          } else {
            goto fail;
          }
          if (!a64_load_q(code, 1, a) || !a64_load_q(code, 2, b) ||
              !a64_u32(code, op | (2u << 16) | (1u << 5)) ||
              !a64_store_q(code, 0, dst))
            goto fail;
          break;
        }
        bool is_f64 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                      in->dst.as.reg < mach->vreg_len &&
                      mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F64;
        bool is_f32 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                      in->dst.as.reg < mach->vreg_len &&
                      mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F32;
        if (is_f64 || is_f32) {
          uint32_t falu = 0;
          unsigned top = is_f32 ? 0x1E20u : 0x1E60u;
          if (in->opcode == NYR_MACH_ADD)
            falu = (top << 16) | 0x2800u;
          else if (in->opcode == NYR_MACH_SUB)
            falu = (top << 16) | 0x3800u;
          else if (in->opcode == NYR_MACH_MUL)
            falu = (top << 16) | 0x0800u;
          else if (in->opcode == NYR_MACH_DIV)
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
        if (in->opcode == NYR_MACH_ADD)
          alu = 0x8B010000; /* add x0,x0,x1 */
        else if (in->opcode == NYR_MACH_SUB)
          alu = 0xCB010000;
        else if (in->opcode == NYR_MACH_AND)
          alu = 0x8A010000;
        else if (in->opcode == NYR_MACH_OR)
          alu = 0xAA010000;
        else if (in->opcode == NYR_MACH_XOR)
          alu = 0xCA010000;
        else
          alu = 0x9B017C00; /* mul x0,x0,x1 */
        if (!a64_u32(code, alu) || !a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NYR_MACH_SHL:
      case NYR_MACH_SAR: {
        if (a64_is_v128(mach, &in->dst)) {
          if (a64_operand_type(mach, &in->dst) != NYR_MACH_TYPE_V128_I64 ||
              !a64_load_q(code, 1, a) || !a64_load_q(code, 2, b))
            goto fail;
          if (in->opcode == NYR_MACH_SAR && !a64_u32(code, 0x6EE0B842u))
            goto fail;
          if (!a64_u32(code, 0x4EE24420u) || !a64_store_q(code, 0, dst))
            goto fail;
          break;
        }
        if (!a64_ldur_x(code, 0, a) || !a64_ldur_x(code, 1, b))
          goto fail;
        /*
         * lsl/asr x0, x0, x1
         */
        uint32_t sh = in->opcode == NYR_MACH_SHL ? 0x9AC12000u : 0x9AC12800u;
        if (!a64_u32(code, sh))
          goto fail;
        if (!a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NYR_MACH_DIV:
      case NYR_MACH_MOD: {
        if (a64_is_v128(mach, &in->dst)) {
          nyr_mach_type_t vt = a64_operand_type(mach, &in->dst);
          uint32_t op = vt == NYR_MACH_TYPE_V128_F64   ? 0x6E60FC00u
                        : vt == NYR_MACH_TYPE_V128_F32 ? 0x6E20FC00u
                                                       : 0;
          if (in->opcode != NYR_MACH_DIV || !op || !a64_load_q(code, 1, a) ||
              !a64_load_q(code, 2, b) ||
              !a64_u32(code, op | (2u << 16) | (1u << 5)) ||
              !a64_store_q(code, 0, dst))
            goto fail;
          break;
        }
        bool is_f64 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                      in->dst.as.reg < mach->vreg_len &&
                      mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F64;
        bool is_f32 = in->dst.kind == NYR_MACH_OPERAND_VREG &&
                      in->dst.as.reg < mach->vreg_len &&
                      mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F32;
        if (is_f64 || is_f32) {
          if (in->opcode != NYR_MACH_DIV || !a64_ldur_fp(code, is_f32, 0, a) ||
              !a64_ldur_fp(code, is_f32, 1, b) ||
              !a64_u32(code,
                       (is_f32 ? 0x1E211800u : 0x1E611800u) | (1u << 16)) ||
              !a64_stur_fp(code, is_f32, 0, dst))
            goto fail;
          break;
        }
        if (!a64_ldur_x(code, 0, a) || !a64_ldur_x(code, 1, b))
          goto fail;
        if (in->opcode == NYR_MACH_DIV) {
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
      case NYR_MACH_FMA: {
        if (!a64_is_v128(mach, &in->dst))
          goto fail;
        nyr_mach_type_t vt = a64_operand_type(mach, &in->dst);
        uint32_t op = vt == NYR_MACH_TYPE_V128_F64   ? 0x4E60CC00u
                      : vt == NYR_MACH_TYPE_V128_F32 ? 0x4E20CC00u
                                                     : 0;
        int c_off = a64_slot_off(mach, &in->src2);
        if (!op || !a64_load_q(code, 0, c_off) || !a64_load_q(code, 1, a) ||
            !a64_load_q(code, 2, b) ||
            !a64_u32(code, op | (2u << 16) | (1u << 5)) ||
            !a64_store_q(code, 0, dst))
          goto fail;
        break;
      }
      case NYR_MACH_SHUFFLE: {
        if (!a64_is_v128(mach, &in->dst) ||
            in->src1.kind != NYR_MACH_OPERAND_IMM || !a64_load_q(code, 1, a))
          goto fail;
        nyr_mach_type_t vt = a64_operand_type(mach, &in->dst);
        unsigned lane_bytes = vt == NYR_MACH_TYPE_V128_F64   ? 8u
                              : vt == NYR_MACH_TYPE_V128_F32 ? 4u
                                                             : 0u;
        unsigned lane_count = lane_bytes == 8 ? 2u : lane_bytes == 4 ? 4u : 0u;
        if (!lane_count)
          goto fail;
        uint64_t lo = a64_shuffle_half((unsigned)in->src1.as.imm, lane_bytes, 0,
                                       lane_count);
        uint64_t hi = a64_shuffle_half((unsigned)in->src1.as.imm, lane_bytes,
                                       8 / lane_bytes, lane_count);
        if (!a64_mov_imm64(code, 0, lo) || !a64_mov_imm64(code, 1, hi) ||
            !a64_u32(code, 0x9E670002u) || !a64_u32(code, 0x4E181C22u) ||
            !a64_u32(code, 0x4E020020u) || !a64_store_q(code, 0, dst))
          goto fail;
        break;
      }
      case NYR_MACH_CMP: {
        if (!a64_ldur_x(code, 0, a) || !a64_ldur_x(code, 1, b))
          goto fail;
        if (!a64_u32(code, 0xEB01001F)) /* cmp x0,x1 */
          goto fail;
        /*
         * cset x0, cond
         */
        unsigned cond = 0xb; /* lt */
        switch (in->condition) {
        case NYR_MACH_COND_EQ:
          cond = 0x0;
          break;
        case NYR_MACH_COND_NE:
          cond = 0x1;
          break;
        case NYR_MACH_COND_LT:
          cond = 0xb;
          break;
        case NYR_MACH_COND_LE:
          cond = 0xd;
          break;
        case NYR_MACH_COND_GT:
          cond = 0xc;
          break;
        case NYR_MACH_COND_GE:
          cond = 0xa;
          break;
        default:
          goto fail;
        }
        uint32_t cset = 0x9A9F07E0u | ((cond ^ 1u) << 12);
        if (!a64_u32(code, cset))
          goto fail;
        if (!a64_stur_x(code, 0, dst))
          goto fail;
        break;
      }
      case NYR_MACH_BR:
      case NYR_MACH_BR_IF: {
        if (in->opcode == NYR_MACH_BR_IF) {
          if (!a64_ldur_x(code, 0, a))
            goto fail;
          if (!a64_u32(code,
                       0xB4000000)) /* cbz x0, +0 placeholder - use b.ne */
            goto fail;
          /*
           * Use cbnz x0, label: 0xB5000000 | imm19 | Rt
           * We'll emit b.ne with patch: actually emit unconditional b with
           * separate path — simplify: cbnz x0, target
           */
          code->len -= 4;                 /* undo */
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
          if (!na || !nb) {
            free(na);
            free(nb);
            goto fail;
          }
          patch_at = na;
          patch_blk = nb;
        }
        patch_at[npatch] = code->len - 4;
        patch_blk[npatch] = in->src1.as.block_index;
        npatch++;
        break;
      }
      case NYR_MACH_CALL: {
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
            if (c0 == NYIR_ARG_CLASS_HFA_F32 || c0 == NYIR_ARG_CLASS_HFA_F64 ||
                c0 == NYIR_ARG_CLASS_HVA_V128) {
              unsigned elem_size = c0 == NYIR_ARG_CLASS_HFA_F32   ? 4u
                                   : c0 == NYIR_ARG_CLASS_HFA_F64 ? 8u
                                                                  : 16u;
              unsigned count =
                  elem_size && size % elem_size == 0 ? size / elem_size : 0;
              chunks_arg[ai] = count;
              if (count >= 1 && count <= 4 && fp + count <= 8) {
                kinds[ai] = c0 == NYIR_ARG_CLASS_HFA_F32   ? MARG_HFA_F32
                            : c0 == NYIR_ARG_CLASS_HFA_F64 ? MARG_HFA_F64
                                                           : MARG_HVA_V128;
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
          nyr_mach_type_t type = a64_operand_type(mach, &in->args[ai]);
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
          } else if (type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64) {
            if (fp < 8) {
              kinds[ai] = type == NYR_MACH_TYPE_F32 ? MARG_F32 : MARG_F64;
              regs_arg[ai] = fp++;
            } else {
              stack_used = (stack_used + 7u) & ~7u;
              kinds[ai] =
                  type == NYR_MACH_TYPE_F32 ? MARG_STACK_F32 : MARG_STACK_F64;
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
                !a64_u32(code, 0x390003EAu | ((copy_arg[ai] + byte) << 10)))
              goto fail;
          }
        }
        for (size_t ai = 0; ai < in->args_len; ++ai) {
          int off = a64_slot_off(mach, &in->args[ai]);
          switch (kinds[ai]) {
          case MARG_STACK_GP:
            if (!a64_ldur_x(code, 10, off) ||
                !a64_u32(code,
                         0xF90003EAu | ((uint32_t)(stack_arg[ai] / 8) << 10)))
              goto fail;
            break;
          case MARG_STACK_PTR:
            if (!a64_u32(code, 0x910003EAu | (copy_arg[ai] << 10)) ||
                !a64_u32(code,
                         0xF90003EAu | ((uint32_t)(stack_arg[ai] / 8) << 10)))
              goto fail;
            break;
          case MARG_STACK_F32:
            if (!a64_ldur_fp(code, true, 16, off) ||
                !a64_u32(code,
                         0xBD0003F0u | ((uint32_t)(stack_arg[ai] / 4) << 10)))
              goto fail;
            break;
          case MARG_STACK_F64:
            if (!a64_ldur_fp(code, false, 16, off) ||
                !a64_u32(code,
                         0xFD0003F0u | ((uint32_t)(stack_arg[ai] / 8) << 10)))
              goto fail;
            break;
          case MARG_STACK_V128:
            if (!a64_load_q(code, 16, off) ||
                !a64_u32(code,
                         0x3D8003F0u | ((uint32_t)(stack_arg[ai] / 16) << 10)))
              goto fail;
            break;
          case MARG_STACK_AGG:
            if (!a64_ldur_x(code, 9, off))
              goto fail;
            for (uint32_t byte = 0; byte < sizes_arg[ai]; ++byte) {
              if (!a64_u32(code, 0x3940012Au | (byte << 10)) ||
                  !a64_u32(code, 0x390003EAu | ((stack_arg[ai] + byte) << 10)))
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
            if (!a64_u32(code,
                         0x910003E0u | (copy_arg[ai] << 10) | regs_arg[ai]))
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
              if (!a64_ldr_x_base(code, regs_arg[ai] + ch, 9, (int)(ch * 8)))
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
              bool ok =
                  kinds[ai] == MARG_HVA_V128
                      ? a64_ldr_q_base(code, regs_arg[ai] + ch, 9,
                                       (int)(ch * 16))
                      : a64_ldr_fp_base(
                            code, kinds[ai] == MARG_HFA_F32, regs_arg[ai] + ch,
                            9, (int)(ch * (kinds[ai] == MARG_HFA_F32 ? 4 : 8)));
              if (!ok)
                goto fail;
            }
            break;
          default:
            break;
          }
        }
        const char *raw = in->src0.kind == NYR_MACH_OPERAND_SYMBOL
                              ? in->src0.as.symbol
                              : NULL;
        if (!raw)
          goto fail;
        char call_sym[256];
        const char *pref =
            target && target->symbol_prefix ? target->symbol_prefix : "";
        if (in->call_is_extern || ny_native_runtime_abi_decl(raw) != NULL ||
            strncmp(raw, "ny_fn_", 6) == 0 || raw[0] == '_')
          snprintf(call_sym, sizeof(call_sym), "%s%s", pref, raw);
        else
          snprintf(call_sym, sizeof(call_sym), NY_FMT_FN, pref, raw);
        if (!a64_u32(code, 0x94000000u))
          goto fail;
        /*
         * Unrelocated BL+0 = self-loop; treat cap overflow as failure.
         */
        if (!relocs || !reloc_count || *reloc_count >= reloc_cap)
          goto fail;
        snprintf(relocs[*reloc_count].symbol,
                 sizeof(relocs[*reloc_count].symbol), "%s", call_sym);
        relocs[*reloc_count].disp_off = code->len - 4;
        relocs[*reloc_count].type = NY_RELOC_AARCH64_CALL26;
        (*reloc_count)++;
        if (stack_size && !a64_u32(code, 0x910003FFu | (stack_size << 10)))
          goto fail;
        if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
          nyr_mach_type_t type = a64_operand_type(mach, &in->dst);
          if (a64_mach_type_v128(type)) {
            if (!a64_store_q(code, 0, dst))
              goto fail;
          } else if (type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64) {
            if (!a64_stur_fp(code, type == NYR_MACH_TYPE_F32, 0, dst))
              goto fail;
          } else if (!a64_stur_x(code, 0, dst)) {
            goto fail;
          }
        }
        break;
      }
      case NYR_MACH_RET:
        if (mach->return_aggregate_size) {
          unsigned c0 = mach->return_aggregate_classes[0];
          unsigned c1 = mach->return_aggregate_classes[1];
          if (c0 == NYIR_ARG_CLASS_HFA_F64) {
            unsigned count = mach->return_aggregate_size / 8u;
            if (count == 0 || count > 4 ||
                in->src0.kind == NYR_MACH_OPERAND_NONE ||
                !a64_mach_load_return_piece(mach, code, &in->src0, 9, false,
                                            false))
              goto fail;
            for (unsigned piece = 0; piece < count; ++piece)
              if (!a64_ldr_fp_base(code, false, piece, 9, (int)(piece * 8u)))
                goto fail;
          } else if (c0 == NYIR_ARG_CLASS_MEMORY) {
            if (!a64_mach_load_return_piece(mach, code, &in->src0, 0, false,
                                            false))
              goto fail;
          } else {
            if (c0 != NYIR_ARG_CLASS_INTEGER &&
                c0 != NYIR_ARG_CLASS_AAPCS_INTEGER_A16 &&
                c0 != NYIR_ARG_CLASS_SSE)
              goto fail;
            if (!a64_mach_load_return_piece(mach, code, &in->src0, 0,
                                            c0 == NYIR_ARG_CLASS_SSE, false))
              goto fail;
            if (c1 == NYIR_ARG_CLASS_INTEGER || c1 == NYIR_ARG_CLASS_SSE) {
              if (!a64_mach_load_return_piece(mach, code, &in->src1, 1,
                                              c1 == NYIR_ARG_CLASS_SSE, false))
                goto fail;
            } else if (c1 != NYIR_ARG_CLASS_NONE ||
                       in->src1.kind != NYR_MACH_OPERAND_NONE) {
              goto fail;
            }
          }
        } else if (in->src0.kind != NYR_MACH_OPERAND_NONE) {
          if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
            if (!a64_mov_imm64(code, 0, (uint64_t)in->src0.as.imm))
              goto fail;
          } else {
            nyr_mach_type_t type = a64_operand_type(mach, &in->src0);
            if (a64_mach_type_v128(type)) {
              if (!a64_load_q(code, 0, a))
                goto fail;
            } else if (type == NYR_MACH_TYPE_F32 || type == NYR_MACH_TYPE_F64) {
              if (!a64_ldur_fp(code, type == NYR_MACH_TYPE_F32, 0, a))
                goto fail;
            } else if (!a64_ldur_x(code, 0, a)) {
              goto fail;
            }
          }
        }
        if (frame) {
          uint32_t add = 0x910003FF | ((uint32_t)frame << 10);
          if (!a64_u32(code, add))
            goto fail;
        }
        if (!a64_u32(code, 0xA8C17BFD) ||
            !a64_ret(code)) /* ldp x29,x30,[sp],#16; ret */
          goto fail;
        break;
      case NYR_MACH_NOP:
        break;
      case NYR_MACH_INLINE_ASM: {
        /*
         * Encode only templates with a defined AArch64 lowering.
         */
        if (in->src0.kind != NYR_MACH_OPERAND_SYMBOL || !in->src0.as.symbol) {
          if (err && err_len)
            snprintf(err, err_len, "inline asm needs template");
          goto fail;
        }
        const char *t = in->src0.as.symbol;
        if (t[0] == '\0') {
          /*
           * Empty volatile asm is a compiler barrier, not an instruction.
           */
        } else if (strcmp(t, "yield") == 0) {
          if (!a64_u32(code, 0xD503203Fu))
            goto fail;
        } else if (strcmp(t, "wfe") == 0) {
          if (!a64_u32(code, 0xD503205Fu))
            goto fail;
        } else if (strcmp(t, "wfi") == 0) {
          if (!a64_u32(code, 0xD503207Fu))
            goto fail;
        } else if (strcmp(t, "sev") == 0) {
          if (!a64_u32(code, 0xD503209Fu))
            goto fail;
        } else if (strcmp(t, "isb") == 0 || strcmp(t, "isb sy") == 0) {
          if (!a64_u32(code, 0xD5033FDFu))
            goto fail;
        } else if (strcmp(t, "dmb") == 0 || strcmp(t, "dmb sy") == 0) {
          if (!a64_u32(code, 0xD5033FBFu))
            goto fail;
        } else if (strcmp(t, "dmb ish") == 0) {
          if (!a64_u32(code, 0xD5033BBFu))
            goto fail;
        } else if (strcmp(t, "dsb") == 0 || strcmp(t, "dsb sy") == 0) {
          if (!a64_u32(code, 0xD5033F9Fu))
            goto fail;
        } else if (strcmp(t, "dsb ish") == 0) {
          if (!a64_u32(code, 0xD5033B9Fu))
            goto fail;
        } else if (strncmp(t, "nop", 3) == 0) {
          bool emitted = false;
          for (const char *p = t; *p; ++p) {
            if ((p == t || p[-1] == ';') && (p[0] == 'n' || p[0] == 'N')) {
              if (!a64_u32(code, 0xD503201Fu))
                goto fail;
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
      case NYR_MACH_INTRINSIC: {
        /*
         * CAPTURE_RET selectors: 0=x1, 1=x0, 2=d0, 3=d1,
         * 4..7=s0..s3, 8=d2, 9=d3, 10..13=q0..q3.
         */
        if (in->src0.kind == NYR_MACH_OPERAND_IMM) {
          unsigned sel = (unsigned)(in->src0.as.imm & 0xff);
          if (in->dst.kind == NYR_MACH_OPERAND_VREG) {
            int doff = a64_slot_off(mach, &in->dst);
            unsigned src_reg = 0;
            if (sel == 0)
              src_reg = 1;
            else if (sel == 1)
              src_reg = 0;
            else if (sel == 2 || sel == 3) {
              unsigned v = sel - 2;
              if (!a64_u32(code, 0x9E660000u | (v << 5)))
                goto fail;
            } else if (sel >= 4 && sel <= 7) {
              unsigned v = sel - 4;
              if (!a64_u32(code, 0x1E260000u | (v << 5)))
                goto fail;
            } else if (sel == 8 || sel == 9) {
              unsigned v = sel - 6;
              if (!a64_u32(code, 0x9E660000u | (v << 5)))
                goto fail;
            } else if (sel >= 10 && sel <= 13) {
              if (!a64_store_q(code, sel - 10, doff))
                goto fail;
              break;
            } else {
              if (err && err_len)
                snprintf(err, err_len, "unsupported capture.ret selector %u",
                         sel);
              goto fail;
            }
            if (!a64_stur_x(code, src_reg, doff))
              goto fail;
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
    if (blk >= mach->block_len)
      goto fail;
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
  if (inst_off_out)
    *inst_off_out = inst_offsets;
  else
    free(inst_offsets);
  return true;
fail:
  if (err && err_len && !err[0])
    snprintf(err, err_len, "instruction %zu opcode=%u encode failed",
             current_inst, current_opcode);
  free(patch_at);
  free(patch_blk);
  free(block_off);
  free(inst_offsets);
  return false;
}

static bool ny_a64_debug_line_push(nyr_native_debug_line_t **lines,
                                   size_t *count, size_t *cap,
                                   nyr_native_debug_line_t line) {
  if (!lines || !count || !cap)
    return false;
  if (*count == *cap) {
    if (*count == SIZE_MAX)
      return false;
    size_t next = *cap > SIZE_MAX / 2 ? *count + 1 : (*cap ? *cap * 2 : 32);
    if (next < *count + 1)
      next = *count + 1;
    if (next > SIZE_MAX / sizeof(**lines))
      return false;
    nyr_native_debug_line_t *grown = realloc(*lines, next * sizeof(**lines));
    if (!grown)
      return false;
    *lines = grown;
    *cap = next;
  }
  (*lines)[(*count)++] = line;
  return true;
}

static bool ny_a64_debug_machine_function(const nyr_mach_func_t *mach,
                                          const nyr_func_t *source,
                                          size_t def_index,
                                          const size_t *inst_offsets,
                                          nyr_native_debug_line_t **lines,
                                          size_t *count, size_t *cap) {
  if (!mach || !source || !inst_offsets || !lines || !count || !cap)
    return true;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    uint32_t source_pc = mach->insts[i].source_pc;
    if (inst_offsets[i] == SIZE_MAX || source_pc >= source->len ||
        !source->data[source_pc].debug.line)
      continue;
    const nyr_debug_loc_t *loc = &source->data[source_pc].debug;
    if (!ny_a64_debug_line_push(
            lines, count, cap,
            (nyr_native_debug_line_t){def_index, inst_offsets[i], loc->file,
                                      loc->line, loc->column}))
      return false;
  }
  return true;
}

static bool ny_a64_mach_build_bundle_debug(
    const nyr_mach_func_t *rt_main_mir, const nyr_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool rt_main_user_function, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_func_t *rt_main_source, const nyr_func_t *func_sources,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
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
  if (debug_lines)
    *debug_lines = NULL;
  if (debug_line_count)
    *debug_line_count = 0;
  if (debug_line_cap)
    *debug_line_cap = 0;
  size_t reloc_cap = NY_X64_OBJ_MAX_RELOCS;

  /*
   * The machine form allocates one Q register per vector value; four-lane
   * f64 needs a register pair. Fail closed here so the from_nirs caller
   * selects the direct NYIR encoder, which carries four-lane values in
   * 32-byte homes.
   */
  for (size_t fi = 0; fi <= func_count; ++fi) {
    const nyr_mach_func_t *f = fi == 0 ? rt_main_mir : &func_mirs[fi - 1];
    for (size_t v = 0; v < f->vreg_len; ++v)
      if (f->vreg_types[v] == NYR_MACH_TYPE_V256_F64) {
        if (err && err_len)
          snprintf(err, err_len,
                   "a64 machine form: four-lane f64 values need a register "
                   "pair; the direct AArch64 encoder handles this shape");
        return false;
      }
  }

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
    size_t *inst_offsets = NULL;
    char ebuf[128];
    if (!a64_encode_func(&func_mirs[i], code, NULL, &inst_offsets, true, relocs,
                         reloc_count, reloc_cap, start, target, ebuf,
                         sizeof(ebuf))) {
      free(inst_offsets);
      if (err && err_len)
        snprintf(err, err_len, "a64 machine form: %s",
                 ebuf[0] ? ebuf : "encode fail");
      return false;
    }
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      free(inst_offsets);
      return false;
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             symbol);
    defs[*def_count].off = start;
    defs[*def_count].size = code->len - start;
    if (!ny_a64_debug_machine_function(
            &func_mirs[i], func_sources ? &func_sources[i] : NULL, *def_count,
            inst_offsets, debug_lines, debug_line_count, debug_line_cap)) {
      free(inst_offsets);
      if (err && err_len)
        snprintf(err, err_len,
                 "a64 machine form: source map allocation failed");
      return false;
    }
    (*def_count)++;
    free(inst_offsets);
  }
  {
    size_t start = code->len;
    size_t *inst_offsets = NULL;
    char ebuf[128] = {0};
    if (!a64_encode_func(rt_main_mir, code, NULL, &inst_offsets,
                         rt_main_user_function, relocs, reloc_count, reloc_cap,
                         start, target, ebuf, sizeof(ebuf))) {
      free(inst_offsets);
      if (err && err_len)
        snprintf(err, err_len, "a64 machine form: %s",
                 ebuf[0] ? ebuf : "encode fail");
      return false;
    }
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      free(inst_offsets);
      return false;
    }
    snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s",
             entry_symbol);
    defs[*def_count].off = start;
    defs[*def_count].size = code->len - start;
    if (!ny_a64_debug_machine_function(rt_main_mir, rt_main_source, *def_count,
                                       inst_offsets, debug_lines,
                                       debug_line_count, debug_line_cap)) {
      free(inst_offsets);
      if (err && err_len)
        snprintf(err, err_len,
                 "a64 machine form: source map allocation failed");
      return false;
    }
    (*def_count)++;
    free(inst_offsets);
  }
  return true;
}

bool nyr_a64_mach_build_bundle(const nyr_mach_func_t *rt_main_mir,
                               const nyr_mach_func_t *func_mirs,
                               const char *const *func_names, size_t func_count,
                               const ny_native_target_info_t *target,
                               const char *entry_symbol, bool tag_return,
                               nyr_obj_buf_t *code,
                               nyr_x64_obj_symbol_def_t *defs,
                               size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                               size_t *reloc_count, char *err, size_t err_len) {
  return ny_a64_mach_build_bundle_debug(
      rt_main_mir, func_mirs, func_names, func_count, target, entry_symbol,
      tag_return, false, code, defs, def_count, relocs, reloc_count, NULL, NULL,
      NULL, NULL, NULL, err, err_len);
}
