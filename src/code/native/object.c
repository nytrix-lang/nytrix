#include "code/native/object/internal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * In-process native object writers. The x86-64 path owns the current
 * ELF64/COFF/Mach-O raw-int/f32/f64/direct-call slices with relocations. The
 * i386 path currently owns a narrow ELF32 raw-int/local-call slice with
 * R_386_PC32 relocations.
 */

static void ny_x64_obj_valmap_free(ny_x64_obj_valmap_t *m);

void ny_obj_free(ny_obj_buf_t *b) {
  if (!b)
    return;
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

void ny_x64_obj_ctx_free(ny_x64_obj_ctx_t *c) {
  if (!c)
    return;
  ny_obj_free(&c->code);
  ny_x64_obj_valmap_free(&c->valmap);
  free(c->value_f64);
  free(c->value_f32);
  free(c->local_f64);
  free(c->local_f32);
  free(c->value_immediate);
  free(c->value_reg);
  free(c->value_xmm);
  free(c->value_spill);
  c->value_f64 = NULL;
  c->value_f32 = NULL;
  c->local_f64 = NULL;
  c->local_f32 = NULL;
  c->value_immediate = NULL;
  c->value_reg = NULL;
  c->value_xmm = NULL;
  c->value_spill = NULL;
}

static bool ny_obj_reserve(ny_obj_buf_t *b, size_t add) {
  if (!b)
    return false;
  if (add > (size_t)-1 - b->len)
    return false;
  size_t need = b->len + add;
  if (need <= b->cap)
    return true;
  size_t cap = b->cap ? b->cap * 2 : 256;
  while (cap < need) {
    if (cap > (size_t)-1 / 2)
      return false;
    cap *= 2;
  }
  unsigned char *p = (unsigned char *)realloc(b->data, cap);
  if (!p)
    return false;
  b->data = p;
  b->cap = cap;
  return true;
}

bool ny_obj_emit(ny_obj_buf_t *b, const void *data, size_t len) {
  if (!ny_obj_reserve(b, len))
    return false;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return true;
}

bool ny_obj_u8(ny_obj_buf_t *b, unsigned v) {
  unsigned char c = (unsigned char)v;
  return ny_obj_emit(b, &c, 1);
}

bool ny_obj_pad_to(ny_obj_buf_t *b, size_t align) {
  if (align == 0)
    return true;
  while ((b->len % align) != 0) {
    if (!ny_obj_u8(b, 0))
      return false;
  }
  return true;
}

bool ny_obj_zero(ny_obj_buf_t *b, size_t len) {
  if (!ny_obj_reserve(b, len))
    return false;
  memset(b->data + b->len, 0, len);
  b->len += len;
  return true;
}

bool ny_obj_u16(ny_obj_buf_t *b, uint16_t v) {
  unsigned char c[2] = {(unsigned char)(v & 0xff), (unsigned char)(v >> 8)};
  return ny_obj_emit(b, c, sizeof(c));
}

bool ny_obj_u32(ny_obj_buf_t *b, uint32_t v) {
  unsigned char c[4] = {(unsigned char)(v & 0xff),
                        (unsigned char)((v >> 8) & 0xff),
                        (unsigned char)((v >> 16) & 0xff),
                        (unsigned char)((v >> 24) & 0xff)};
  return ny_obj_emit(b, c, sizeof(c));
}

bool ny_obj_u64(ny_obj_buf_t *b, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    if (!ny_obj_u8(b, (unsigned)((v >> (i * 8)) & 0xff)))
      return false;
  }
  return true;
}

void ny_obj_patch_u16(ny_obj_buf_t *b, size_t off, uint16_t v) {
  b->data[off + 0] = (unsigned char)(v & 0xff);
  b->data[off + 1] = (unsigned char)(v >> 8);
}

void ny_obj_patch_u32(ny_obj_buf_t *b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

void ny_obj_patch_u64(ny_obj_buf_t *b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

bool ny_elf32_write_sym(ny_obj_buf_t *b, uint32_t name, uint32_t value,
                               uint32_t size, unsigned char info,
                               uint16_t shndx) {
  return ny_obj_u32(b, name) && ny_obj_u32(b, value) && ny_obj_u32(b, size) &&
         ny_obj_u8(b, info) && ny_obj_u8(b, 0) && ny_obj_u16(b, shndx);
}

bool ny_elf32_write_sh(ny_obj_buf_t *b, uint32_t name, uint32_t type,
                              uint32_t flags, uint32_t off, uint32_t size,
                              uint32_t link, uint32_t info,
                              uint32_t addralign, uint32_t entsize) {
  return ny_obj_u32(b, name) && ny_obj_u32(b, type) && ny_obj_u32(b, flags) &&
         ny_obj_u32(b, 0) && ny_obj_u32(b, off) && ny_obj_u32(b, size) &&
         ny_obj_u32(b, link) && ny_obj_u32(b, info) &&
         ny_obj_u32(b, addralign) && ny_obj_u32(b, entsize);
}


void ny_i386_obj_ctx_free(ny_i386_obj_ctx_t *c) {
  if (c) {
    ny_obj_free(&c->code);
    free(c->value_f64);
    free(c->value_f32);
    free(c->local_f64);
    free(c->local_f32);
  }
}

static int ny_i386_obj_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static int ny_i386_obj_value_off(int value) {
  return -4 - 8 * (value + 1);
}

static int ny_i386_obj_local_off(const ny_i386_obj_ctx_t *c, int local) {
  return -4 - c->local_base - 8 * (local + 1);
}

static bool ny_i386_obj_bytes(ny_i386_obj_ctx_t *c, const unsigned char *p,
                              size_t len) {
  if (!ny_obj_emit(&c->code, p, len)) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: out of memory");
    return false;
  }
  return true;
}

static bool ny_i386_obj_u8(ny_i386_obj_ctx_t *c, unsigned v) {
  return ny_i386_obj_bytes(c, (const unsigned char *)&(unsigned char){v}, 1);
}

static bool ny_i386_obj_i32(ny_i386_obj_ctx_t *c, int32_t v) {
  unsigned char b[4] = {(unsigned char)((uint32_t)v & 0xff),
                        (unsigned char)(((uint32_t)v >> 8) & 0xff),
                        (unsigned char)(((uint32_t)v >> 16) & 0xff),
                        (unsigned char)(((uint32_t)v >> 24) & 0xff)};
  return ny_i386_obj_bytes(c, b, sizeof(b));
}

static bool ny_i386_obj_load_eax(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x8b, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_lea_eax(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x8d, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_store_eax(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x89, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_load_ebx(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x8b, 0x9d};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_load_ecx(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x8b, 0x8d};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_store_edx(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x89, 0x95};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_mov_imm32(ny_i386_obj_ctx_t *c, int off, uint32_t imm) {
  static const unsigned char op[] = {0xc7, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off) &&
         ny_i386_obj_i32(c, (int32_t)imm);
}

static bool ny_i386_obj_store_f64_bits(ny_i386_obj_ctx_t *c, int off,
                                       uint64_t bits) {
  return ny_i386_obj_mov_imm32(c, off, (uint32_t)bits) &&
         ny_i386_obj_mov_imm32(c, off + 4, (uint32_t)(bits >> 32));
}

static bool ny_i386_obj_store_f32_bits(ny_i386_obj_ctx_t *c, int off,
                                       uint32_t bits) {
  return ny_i386_obj_mov_imm32(c, off, bits);
}

static bool ny_i386_obj_fldl(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0xdd, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_fstpl(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0xdd, 0x9d};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_fildl(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0xdb, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_flds(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0xd9, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_fstps(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0xd9, 0x9d};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_f64_memop(ny_i386_obj_ctx_t *c, unsigned modrm,
                                  int off) {
  return ny_i386_obj_u8(c, 0xdc) && ny_i386_obj_u8(c, modrm) &&
         ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_f32_memop(ny_i386_obj_ctx_t *c, unsigned modrm,
                                  int off) {
  return ny_i386_obj_u8(c, 0xd8) && ny_i386_obj_u8(c, modrm) &&
         ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_fcompl(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0xdc, 0x9d};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_fcomps(ny_i386_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0xd8, 0x9d};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_load_value_f64(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid f64 value v%d", value);
    return false;
  }
  return ny_i386_obj_fldl(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_f64(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid f64 destination v%d", value);
    return false;
  }
  return ny_i386_obj_fstpl(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_f32(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid f32 value v%d", value);
    return false;
  }
  return ny_i386_obj_flds(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_f32(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid f32 destination v%d", value);
    return false;
  }
  return ny_i386_obj_fstps(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_eax(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_i386_obj_load_eax(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_eax(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid destination v%d", value);
    return false;
  }
  return ny_i386_obj_store_eax(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_edx(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid destination v%d", value);
    return false;
  }
  return ny_i386_obj_store_edx(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_ebx(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_i386_obj_load_ebx(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_ecx(ny_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_i386_obj_load_ecx(c, ny_i386_obj_value_off(value));
}

static unsigned ny_i386_obj_setcc(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ: return 0x94;
  case NY_NIR_CMP_NE: return 0x95;
  case NY_NIR_CMP_LT: return 0x9c;
  case NY_NIR_CMP_LE: return 0x9e;
  case NY_NIR_CMP_GT: return 0x9f;
  case NY_NIR_CMP_GE: return 0x9d;
  }
  return 0x94;
}

static unsigned ny_i386_obj_f64_setcc(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ: return 0x94;
  case NY_NIR_CMP_NE: return 0x95;
  case NY_NIR_CMP_LT: return 0x92;
  case NY_NIR_CMP_LE: return 0x96;
  case NY_NIR_CMP_GT: return 0x97;
  case NY_NIR_CMP_GE: return 0x93;
  }
  return 0x94;
}

static bool ny_i386_obj_label(ny_i386_obj_ctx_t *c, int64_t label) {
  if (c->label_count >= sizeof(c->labels) / sizeof(c->labels[0])) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: too many labels");
    return false;
  }
  c->labels[c->label_count++] = (ny_i386_obj_label_t){label, c->code.len};
  return true;
}

static bool ny_i386_obj_add_patch(ny_i386_obj_ctx_t *c, int64_t label,
                                  size_t disp_off) {
  if (c->patch_count >= sizeof(c->patches) / sizeof(c->patches[0])) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: too many branches");
    return false;
  }
  c->patches[c->patch_count++] = (ny_i386_obj_patch_t){label, disp_off};
  return true;
}

static bool ny_i386_obj_add_epilogue_patch(ny_i386_obj_ctx_t *c,
                                           size_t disp_off) {
  if (c->epilogue_patch_count >=
      sizeof(c->epilogue_patches) / sizeof(c->epilogue_patches[0])) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: too many returns");
    return false;
  }
  c->epilogue_patches[c->epilogue_patch_count++] = disp_off;
  return true;
}

static bool ny_i386_obj_reloc_symbol(char *out, size_t out_len,
                                     const ny_i386_obj_ctx_t *c,
                                     const char *symbol) {
  if (!out || out_len == 0 || !symbol || !symbol[0])
    return false;
  const char *prefix = c && c->target && c->target->symbol_prefix
                           ? c->target->symbol_prefix
                           : "";
  int n = snprintf(out, out_len, "%sny_fn_%s", prefix, symbol);
  return n > 0 && (size_t)n < out_len;
}

static bool ny_i386_obj_add_reloc(ny_i386_obj_ctx_t *c, const char *symbol,
                                  size_t disp_off) {
  if (c->reloc_count >= sizeof(c->relocs) / sizeof(c->relocs[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "i386 ELF object writer: too many relocations");
    return false;
  }
  ny_i386_obj_reloc_t *r = &c->relocs[c->reloc_count++];
  snprintf(r->symbol, sizeof(r->symbol), "%s", symbol ? symbol : "");
  r->disp_off = disp_off;
  return true;
}

static bool ny_i386_obj_rel32(ny_i386_obj_ctx_t *c, size_t disp_off,
                              size_t target_off) {
  int64_t rel = (int64_t)target_off - (int64_t)(disp_off + 4);
  if (rel < INT32_MIN || rel > INT32_MAX) {
    ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: branch out of range");
    return false;
  }
  ny_obj_patch_u32(&c->code, disp_off, (uint32_t)(int32_t)rel);
  return true;
}

int ny_i386_obj_symbol_index(char symbols[][256], size_t count,
                                    const char *name) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(symbols[i], name) == 0)
      return (int)i;
  }
  return -1;
}

int ny_i386_obj_def_index(const ny_i386_obj_symbol_def_t *defs,
                                 size_t def_count, const char *name) {
  for (size_t i = 0; defs && i < def_count; ++i) {
    if (strcmp(defs[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

bool ny_i386_obj_collect_external_reloc_symbols(
    const ny_i386_obj_reloc_t *relocs, size_t reloc_count,
    const ny_i386_obj_symbol_def_t *defs, size_t def_count,
    char symbols[][256], size_t *symbol_count, char *err, size_t err_len) {
  size_t count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].symbol[0]) {
      ny_native_set_err(err, err_len, "i386 ELF object writer: empty relocation symbol");
      return false;
    }
    if (ny_i386_obj_def_index(defs, def_count, relocs[i].symbol) >= 0)
      continue;
    if (ny_i386_obj_symbol_index(symbols, count, relocs[i].symbol) >= 0)
      continue;
    if (count >= 256) {
      ny_native_set_err(err, err_len,
                        "i386 ELF object writer: too many relocation symbols");
      return false;
    }
    snprintf(symbols[count++], 256, "%s", relocs[i].symbol);
  }
  *symbol_count = count;
  return true;
}

static bool ny_i386_obj_patch_branches(ny_i386_obj_ctx_t *c, size_t epilogue_off) {
  for (size_t i = 0; i < c->patch_count; ++i) {
    bool found = false;
    for (size_t j = 0; j < c->label_count; ++j) {
      if (c->labels[j].label == c->patches[i].label) {
        if (!ny_i386_obj_rel32(c, c->patches[i].disp_off, c->labels[j].off))
          return false;
        found = true;
        break;
      }
    }
    if (!found) {
      ny_native_set_err(c->err, c->err_len,
                        "i386 ELF object writer: unresolved label L%lld",
                        (long long)c->patches[i].label);
      return false;
    }
  }
  for (size_t i = 0; i < c->epilogue_patch_count; ++i) {
    if (!ny_i386_obj_rel32(c, c->epilogue_patches[i], epilogue_off))
      return false;
  }
  return true;
}

static bool ny_i386_obj_is_f64_op(ny_nir_op_t op) {
  return op == NYIR_CONST_F64 || op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
         op == NYIR_MUL_F64 || op == NYIR_DIV_F64 || op == NYIR_I64_TO_F64 ||
         op == NYIR_CMP_F64;
}

static void ny_i386_obj_mark_value_f64(ny_i386_obj_ctx_t *c, int v,
                                       bool *changed) {
  if (!c || !c->value_f64 || v < 0 || v >= c->value_slots ||
      c->value_f64[v])
    return;
  c->value_f64[v] = true;
  if (changed)
    *changed = true;
}

static void ny_i386_obj_mark_value_f32(ny_i386_obj_ctx_t *c, int v,
                                       bool *changed) {
  if (!c || !c->value_f32 || v < 0 || v >= c->value_slots ||
      c->value_f32[v])
    return;
  c->value_f32[v] = true;
  if (changed)
    *changed = true;
}

static void ny_i386_obj_mark_local_f64(ny_i386_obj_ctx_t *c, int local,
                                       bool *changed) {
  if (!c || !c->local_f64 || local < 0 || local >= c->local_slots ||
      c->local_f64[local])
    return;
  c->local_f64[local] = true;
  if (changed)
    *changed = true;
}

static void ny_i386_obj_mark_local_f32(ny_i386_obj_ctx_t *c, int local,
                                       bool *changed) {
  if (!c || !c->local_f32 || local < 0 || local >= c->local_slots ||
      c->local_f32[local])
    return;
  c->local_f32[local] = true;
  if (changed)
    *changed = true;
}

static bool ny_i386_obj_compute_frame(ny_i386_obj_ctx_t *c,
                                      const ny_nir_func_t *nir) {
  c->max_local_slot = 0;
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  c->value_slots = nir->next_value > 0 ? nir->next_value : 0;
  c->local_slots = c->max_local_slot;
  c->local_base = c->value_slots * 8;
  c->frame_bytes = ny_i386_obj_align((c->value_slots + c->local_slots) * 8, 16);
  c->value_f64 = c->value_slots > 0 ? calloc((size_t)c->value_slots, sizeof(bool)) : NULL;
  c->value_f32 = c->value_slots > 0 ? calloc((size_t)c->value_slots, sizeof(bool)) : NULL;
  c->local_f64 = c->local_slots > 0 ? calloc((size_t)c->local_slots, sizeof(bool)) : NULL;
  c->local_f32 = c->local_slots > 0 ? calloc((size_t)c->local_slots, sizeof(bool)) : NULL;
  if ((c->value_slots > 0 && !c->value_f64) ||
      (c->value_slots > 0 && !c->value_f32) ||
      (c->local_slots > 0 && !c->local_f64) ||
      (c->local_slots > 0 && !c->local_f32)) {
    ny_native_set_err(c->err, c->err_len,
                      "i386 ELF object writer: OOM f64 frame classification");
    return false;
  }
  bool changed = true;
  for (int pass = 0; changed && pass < 8; ++pass) {
    changed = false;
    for (size_t i = 0; i < nir->len; ++i) {
      const ny_nir_inst_t *in = &nir->data[i];
      switch (in->op) {
      case NYIR_CONST_F64:
      case NYIR_I64_TO_F64:
      case NYIR_F32_TO_F64:
        ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        break;
      case NYIR_CONST_F32:
      case NYIR_I64_TO_F32:
      case NYIR_F64_TO_F32:
        ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        break;
      case NYIR_ADD_F64:
      case NYIR_SUB_F64:
      case NYIR_MUL_F64:
      case NYIR_DIV_F64:
        ny_i386_obj_mark_value_f64(c, in->a, &changed);
        ny_i386_obj_mark_value_f64(c, in->b, &changed);
        ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        break;
      case NYIR_ADD_F32:
      case NYIR_SUB_F32:
      case NYIR_MUL_F32:
      case NYIR_DIV_F32:
        ny_i386_obj_mark_value_f32(c, in->a, &changed);
        ny_i386_obj_mark_value_f32(c, in->b, &changed);
        ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        break;
      case NYIR_CMP_F64:
        ny_i386_obj_mark_value_f64(c, in->a, &changed);
        ny_i386_obj_mark_value_f64(c, in->b, &changed);
        break;
      case NYIR_CMP_F32:
        ny_i386_obj_mark_value_f32(c, in->a, &changed);
        ny_i386_obj_mark_value_f32(c, in->b, &changed);
        break;
      case NY_NIR_COPY:
        if (in->a >= 0 && in->a < c->value_slots && c->value_f64[in->a])
          ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f64[in->dst])
          ny_i386_obj_mark_value_f64(c, in->a, &changed);
        if (in->a >= 0 && in->a < c->value_slots && c->value_f32[in->a])
          ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f32[in->dst])
          ny_i386_obj_mark_value_f32(c, in->a, &changed);
        break;
      case NY_NIR_LOAD_LOCAL:
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f64[in->imm])
          ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f64[in->dst])
          ny_i386_obj_mark_local_f64(c, (int)in->imm, &changed);
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f32[in->imm])
          ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f32[in->dst])
          ny_i386_obj_mark_local_f32(c, (int)in->imm, &changed);
        break;
      case NY_NIR_STORE_LOCAL:
        if (in->a >= 0 && in->a < c->value_slots && c->value_f64[in->a])
          ny_i386_obj_mark_local_f64(c, (int)in->imm, &changed);
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f64[in->imm])
          ny_i386_obj_mark_value_f64(c, in->a, &changed);
        if (in->a >= 0 && in->a < c->value_slots && c->value_f32[in->a])
          ny_i386_obj_mark_local_f32(c, (int)in->imm, &changed);
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f32[in->imm])
          ny_i386_obj_mark_value_f32(c, in->a, &changed);
        break;
      case NY_NIR_CALL:
        if ((in->flags & NY_NIR_INST_F_RET_F64) != 0)
          ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        if ((in->flags & NY_NIR_INST_F_RET_F32) != 0)
          ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        break;
      default:
        break;
      }
    }
  }
  return true;
}

static bool ny_i386_obj_emit_binop(ny_i386_obj_ctx_t *c, const ny_nir_inst_t *in,
                                   const unsigned char *op, size_t op_len) {
  return ny_i386_obj_load_value_eax(c, in->a) &&
         ny_i386_obj_load_value_ebx(c, in->b) &&
         ny_i386_obj_bytes(c, op, op_len) &&
         ny_i386_obj_store_value_eax(c, in->dst);
}

static bool ny_i386_obj_emit_f64_binop(ny_i386_obj_ctx_t *c,
                                       const ny_nir_inst_t *in,
                                       unsigned modrm) {
  return ny_i386_obj_load_value_f64(c, in->a) &&
         ny_i386_obj_f64_memop(c, modrm, ny_i386_obj_value_off(in->b)) &&
         ny_i386_obj_store_value_f64(c, in->dst);
}

static bool ny_i386_obj_emit_f32_binop(ny_i386_obj_ctx_t *c,
                                       const ny_nir_inst_t *in,
                                       unsigned modrm) {
  return ny_i386_obj_load_value_f32(c, in->a) &&
         ny_i386_obj_f32_memop(c, modrm, ny_i386_obj_value_off(in->b)) &&
         ny_i386_obj_store_value_f32(c, in->dst);
}

static bool ny_i386_obj_emit_param_spills(ny_i386_obj_ctx_t *c,
                                          const ny_nir_func_t *nir) {
  if (!c || !nir || c->max_local_slot <= 0)
    return true;
  bool *stored = calloc((size_t)c->max_local_slot, sizeof(bool));
  bool *param = calloc((size_t)c->max_local_slot, sizeof(bool));
  if (!stored || !param) {
    free(stored);
    free(param);
    ny_native_set_err(c->err, c->err_len,
                      "i386 ELF object writer: OOM param spill");
    return false;
  }
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if (in->op != NY_NIR_LOAD_LOCAL && in->op != NY_NIR_STORE_LOCAL)
      continue;
    int local = (int)in->imm;
    if (local < 0 || local >= c->max_local_slot)
      continue;
    if (in->op == NY_NIR_STORE_LOCAL) {
      stored[local] = true;
    } else if (!stored[local]) {
      param[local] = true;
    }
  }
  bool ok = true;
  int incoming = 8;
  for (int local = 0; local < c->max_local_slot && ok; ++local) {
    if (!param[local])
      continue;
    bool is_f64 = c->local_f64 && local < c->local_slots && c->local_f64[local];
    bool is_f32 = c->local_f32 && local < c->local_slots && c->local_f32[local];
    if (is_f64) {
      ok = ny_i386_obj_fldl(c, incoming) &&
           ny_i386_obj_fstpl(c, ny_i386_obj_local_off(c, local));
      incoming += 8;
    } else if (is_f32) {
      ok = ny_i386_obj_flds(c, incoming) &&
           ny_i386_obj_fstps(c, ny_i386_obj_local_off(c, local));
      incoming += 4;
    } else {
      ok = ny_i386_obj_load_eax(c, incoming) &&
           ny_i386_obj_store_eax(c, ny_i386_obj_local_off(c, local));
      incoming += 4;
    }
  }
  free(stored);
  free(param);
  return ok;
}

static bool ny_i386_obj_collect_call_args(ny_i386_obj_ctx_t *c,
                                          const ny_nir_inst_t *in,
                                          int *args, int *argc_out) {
  int argc = (int)in->imm;
  if (argc < 0 || argc > NY_NIR_CALL_MAX_ARGS) {
    ny_native_set_err(c->err, c->err_len,
                      "i386 ELF object writer: call exceeds maximum supported arg count");
    return false;
  }
  if (argc > 0) args[0] = in->a;
  if (argc > 1) args[1] = in->b;
  if (argc > 2) args[2] = in->c;
  if (argc > 3) args[3] = in->d;
  if (argc > 4) args[4] = in->e;
  if (argc > 5) args[5] = in->f;
  for (int i = 6; i < argc; ++i)
    args[i] = (in->extra_args && (size_t)(i - 6) < in->extra_args_len)
                  ? in->extra_args[i - 6]
                  : -1;
  for (int i = 0; i < argc; ++i) {
    if (args[i] < 0 || args[i] >= c->value_slots) {
      ny_native_set_err(c->err, c->err_len,
                        "i386 ELF object writer: invalid call arg");
      return false;
    }
  }
  *argc_out = argc;
  return true;
}

bool ny_i386_obj_emit_code(ny_i386_obj_ctx_t *c, const ny_nir_func_t *nir,
                                  bool tag_return) {
  if (!nir || !ny_i386_obj_compute_frame(c, nir))
    return false;
  if (!ny_i386_obj_bytes(c, (const unsigned char[]){0x55, 0x89, 0xe5, 0x53}, 4))
    return false;
  if (c->frame_bytes > 0 &&
      (!ny_i386_obj_bytes(c, (const unsigned char[]){0x81, 0xec}, 2) ||
       !ny_i386_obj_i32(c, c->frame_bytes)))
    return false;
  if (!ny_i386_obj_emit_param_spills(c, nir))
    return false;
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    switch (in->op) {
    case NY_NIR_NOP:
      break;
    case NY_NIR_CONST_I64:
      if (in->imm < INT32_MIN || in->imm > UINT32_MAX) {
        ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: constant out of i32 range");
        return false;
      }
      if (!ny_i386_obj_u8(c, 0xb8) || !ny_i386_obj_i32(c, (int32_t)(uint32_t)in->imm) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_CONST_F64:
      if (!ny_i386_obj_store_f64_bits(c, ny_i386_obj_value_off(in->dst),
                                      (uint64_t)in->imm))
        return false;
      break;
    case NYIR_CONST_F32:
      if (!ny_i386_obj_store_f32_bits(c, ny_i386_obj_value_off(in->dst),
                                      (uint32_t)in->imm))
        return false;
      break;
    case NY_NIR_COPY:
      if ((in->a >= 0 && in->a < c->value_slots && c->value_f64 &&
           c->value_f64[in->a]) ||
          (in->dst >= 0 && in->dst < c->value_slots && c->value_f64 &&
           c->value_f64[in->dst])) {
        if (!ny_i386_obj_load_value_f64(c, in->a) ||
            !ny_i386_obj_store_value_f64(c, in->dst))
          return false;
      } else if ((in->a >= 0 && in->a < c->value_slots && c->value_f32 &&
                  c->value_f32[in->a]) ||
                 (in->dst >= 0 && in->dst < c->value_slots && c->value_f32 &&
                  c->value_f32[in->dst])) {
        if (!ny_i386_obj_load_value_f32(c, in->a) ||
            !ny_i386_obj_store_value_f32(c, in->dst))
          return false;
      } else if (!ny_i386_obj_load_value_eax(c, in->a) ||
                 !ny_i386_obj_store_value_eax(c, in->dst)) {
        return false;
      }
      break;
    case NY_NIR_ADD_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x01, 0xd8}, 2))
        return false;
      break;
    case NY_NIR_SUB_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x29, 0xd8}, 2))
        return false;
      break;
    case NY_NIR_MUL_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x0f, 0xaf, 0xc3}, 3))
        return false;
      break;
    case NY_NIR_AND_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x21, 0xd8}, 2))
        return false;
      break;
    case NY_NIR_OR_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x09, 0xd8}, 2))
        return false;
      break;
    case NY_NIR_XOR_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x31, 0xd8}, 2))
        return false;
      break;
    case NY_NIR_DIV_I64:
    case NY_NIR_MOD_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_load_value_ebx(c, in->b) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x99, 0xf7, 0xfb}, 3))
        return false;
      if (in->op == NY_NIR_DIV_I64) {
        if (!ny_i386_obj_store_value_eax(c, in->dst))
          return false;
      } else if (!ny_i386_obj_store_value_edx(c, in->dst)) {
        return false;
      }
      break;
    case NY_NIR_SHL_I64:
    case NY_NIR_SAR_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_load_value_ecx(c, in->b) ||
          !ny_i386_obj_bytes(c, in->op == NY_NIR_SHL_I64
                                    ? (const unsigned char[]){0xd3, 0xe0}
                                    : (const unsigned char[]){0xd3, 0xf8},
                              2) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NY_NIR_CMP_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_load_value_ebx(c, in->b) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x39, 0xd8, 0x0f}, 3) ||
          !ny_i386_obj_u8(c, ny_i386_obj_setcc(in->cmp)) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0xc0, 0x0f, 0xb6, 0xc0}, 4) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_ADD_F64:
      if (!ny_i386_obj_emit_f64_binop(c, in, 0x85))
        return false;
      break;
    case NYIR_SUB_F64:
      if (!ny_i386_obj_emit_f64_binop(c, in, 0xa5))
        return false;
      break;
    case NYIR_MUL_F64:
      if (!ny_i386_obj_emit_f64_binop(c, in, 0x8d))
        return false;
      break;
    case NYIR_DIV_F64:
      if (!ny_i386_obj_emit_f64_binop(c, in, 0xb5))
        return false;
      break;
    case NYIR_ADD_F32:
      if (!ny_i386_obj_emit_f32_binop(c, in, 0x85))
        return false;
      break;
    case NYIR_SUB_F32:
      if (!ny_i386_obj_emit_f32_binop(c, in, 0xa5))
        return false;
      break;
    case NYIR_MUL_F32:
      if (!ny_i386_obj_emit_f32_binop(c, in, 0x8d))
        return false;
      break;
    case NYIR_DIV_F32:
      if (!ny_i386_obj_emit_f32_binop(c, in, 0xb5))
        return false;
      break;
    case NYIR_I64_TO_F64:
      if (!ny_i386_obj_fildl(c, ny_i386_obj_value_off(in->a)) ||
          !ny_i386_obj_store_value_f64(c, in->dst))
        return false;
      break;
    case NYIR_I64_TO_F32:
      if (!ny_i386_obj_fildl(c, ny_i386_obj_value_off(in->a)) ||
          !ny_i386_obj_store_value_f32(c, in->dst))
        return false;
      break;
    case NYIR_F32_TO_F64:
      if (!ny_i386_obj_load_value_f32(c, in->a) ||
          !ny_i386_obj_store_value_f64(c, in->dst))
        return false;
      break;
    case NYIR_F64_TO_F32:
      if (!ny_i386_obj_load_value_f64(c, in->a) ||
          !ny_i386_obj_store_value_f32(c, in->dst))
        return false;
      break;
    case NYIR_CMP_F64:
      if (!ny_i386_obj_load_value_f64(c, in->a) ||
          !ny_i386_obj_fcompl(c, ny_i386_obj_value_off(in->b)) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0xdf, 0xe0, 0x9e, 0x0f},
                             4) ||
          !ny_i386_obj_u8(c, ny_i386_obj_f64_setcc(in->cmp)) ||
          !ny_i386_obj_u8(c, 0xc0) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x0f, 0xb6, 0xc0}, 3) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_CMP_F32:
      if (!ny_i386_obj_load_value_f32(c, in->a) ||
          !ny_i386_obj_fcomps(c, ny_i386_obj_value_off(in->b)) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0xdf, 0xe0, 0x9e, 0x0f},
                             4) ||
          !ny_i386_obj_u8(c, ny_i386_obj_f64_setcc(in->cmp)) ||
          !ny_i386_obj_u8(c, 0xc0) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x0f, 0xb6, 0xc0}, 3) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NY_NIR_LABEL:
      if (!ny_i386_obj_label(c, in->imm))
        return false;
      break;
    case NY_NIR_LOAD_LOCAL:
      if (in->imm >= 0 && in->imm < c->local_slots && c->local_f64 &&
          c->local_f64[in->imm]) {
        if (!ny_i386_obj_fldl(c, ny_i386_obj_local_off(c, (int)in->imm)) ||
            !ny_i386_obj_store_value_f64(c, in->dst))
          return false;
      } else if (in->imm >= 0 && in->imm < c->local_slots && c->local_f32 &&
                 c->local_f32[in->imm]) {
        if (!ny_i386_obj_flds(c, ny_i386_obj_local_off(c, (int)in->imm)) ||
            !ny_i386_obj_store_value_f32(c, in->dst))
          return false;
      } else if (!ny_i386_obj_load_eax(c, ny_i386_obj_local_off(c, (int)in->imm)) ||
                 !ny_i386_obj_store_value_eax(c, in->dst)) {
        return false;
      }
      break;
    case NYIR_ADDR_LOCAL:
      if (in->imm < 0 || in->imm >= c->local_slots) {
        ny_native_set_err(c->err, c->err_len,
                          "i386 ELF object writer: invalid local slot %lld",
                          (long long)in->imm);
        return false;
      }
      if (!ny_i386_obj_lea_eax(c, ny_i386_obj_local_off(c, (int)in->imm)) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_ADDR_SYMBOL:
      if (!in->symbol || !in->symbol[0]) {
        ny_native_set_err(c->err, c->err_len,
                          "i386 ELF object writer: addr.symbol missing symbol name");
        return false;
      }
      if (!ny_i386_obj_u8(c, 0xb8))
        return false;
      {
        char sym[256];
        if (!ny_i386_obj_reloc_symbol(sym, sizeof(sym), c, in->symbol)) {
          ny_native_set_err(c->err, c->err_len,
                            "i386 ELF object writer: invalid addr symbol");
          return false;
        }
        size_t disp = c->code.len;
        if (!ny_i386_obj_i32(c, 0) || !ny_i386_obj_add_reloc(c, sym, disp) ||
            !ny_i386_obj_store_value_eax(c, in->dst))
          return false;
      }
      break;
    case NY_NIR_STORE_LOCAL:
      if (in->imm >= 0 && in->imm < c->local_slots && c->local_f64 &&
          c->local_f64[in->imm]) {
        if (!ny_i386_obj_load_value_f64(c, in->a) ||
            !ny_i386_obj_fstpl(c, ny_i386_obj_local_off(c, (int)in->imm)))
          return false;
      } else if (in->imm >= 0 && in->imm < c->local_slots && c->local_f32 &&
                 c->local_f32[in->imm]) {
        if (!ny_i386_obj_load_value_f32(c, in->a) ||
            !ny_i386_obj_fstps(c, ny_i386_obj_local_off(c, (int)in->imm)))
          return false;
      } else if (!ny_i386_obj_load_value_eax(c, in->a) ||
                 !ny_i386_obj_store_eax(c, ny_i386_obj_local_off(c, (int)in->imm))) {
        return false;
      }
      break;
    case NY_NIR_RET:
      if (in->a >= 0) {
        if (in->a < c->value_slots && c->value_f64 && c->value_f64[in->a]) {
          if (!ny_i386_obj_load_value_f64(c, in->a))
            return false;
        } else if (in->a < c->value_slots && c->value_f32 && c->value_f32[in->a]) {
          if (!ny_i386_obj_load_value_f32(c, in->a))
            return false;
        } else if (!ny_i386_obj_load_value_eax(c, in->a)) {
          return false;
        }
      }
      if (!ny_i386_obj_u8(c, 0xe9))
        return false;
      if (!ny_i386_obj_add_epilogue_patch(c, c->code.len) ||
          !ny_i386_obj_i32(c, 0))
        return false;
      break;
    case NY_NIR_BR:
      if (!ny_i386_obj_u8(c, 0xe9))
        return false;
      if (!ny_i386_obj_add_patch(c, in->imm, c->code.len) ||
          !ny_i386_obj_i32(c, 0))
        return false;
      break;
    case NY_NIR_BR_IF:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x85, 0xc0, 0x0f, 0x85}, 4))
        return false;
      if (!ny_i386_obj_add_patch(c, in->imm, c->code.len) ||
          !ny_i386_obj_i32(c, 0))
        return false;
      break;
    case NY_NIR_CALL: {
      int args[NY_NIR_CALL_MAX_ARGS];
      int argc = 0;
      if (!ny_i386_obj_collect_call_args(c, in, args, &argc))
        return false;
      int stack_bytes = 0;
      for (int j = argc - 1; j >= 0; --j) {
        bool is_f64 = args[j] >= 0 && args[j] < c->value_slots &&
                      c->value_f64 && c->value_f64[args[j]];
        bool is_f32 = args[j] >= 0 && args[j] < c->value_slots &&
                      c->value_f32 && c->value_f32[args[j]];
        if (is_f64) {
          int off = ny_i386_obj_value_off(args[j]);
          if (!ny_i386_obj_load_eax(c, off + 4) ||
              !ny_i386_obj_u8(c, 0x50) ||
              !ny_i386_obj_load_eax(c, off) ||
              !ny_i386_obj_u8(c, 0x50))
            return false;
          stack_bytes += 8;
          continue;
        }
        if (is_f32) {
          if (!ny_i386_obj_load_eax(c, ny_i386_obj_value_off(args[j])) ||
              !ny_i386_obj_u8(c, 0x50))
            return false;
          stack_bytes += 4;
          continue;
        }
        if (!ny_i386_obj_load_value_eax(c, args[j]) ||
            !ny_i386_obj_u8(c, 0x50))
          return false;
        stack_bytes += 4;
      }
      if (!ny_i386_obj_u8(c, 0xe8))
        return false;
      size_t disp = c->code.len;
      char symbol[256];
      if (in->flags & NY_NIR_INST_F_EXTERN) {
        const char *prefix = c->target && c->target->symbol_prefix
                                 ? c->target->symbol_prefix
                                 : "";
        int n = snprintf(symbol, sizeof(symbol), "%s%s", prefix,
                         in->symbol ? in->symbol : "<null>");
        if (n < 0 || (size_t)n >= sizeof(symbol)) {
          ny_native_set_err(c->err, c->err_len,
                            "i386 ELF object writer: extern symbol too long");
          return false;
        }
      } else if (!ny_i386_obj_reloc_symbol(symbol, sizeof(symbol), c, in->symbol)) {
        ny_native_set_err(c->err, c->err_len,
                          "i386 ELF object writer: invalid call symbol");
        return false;
      }
      if (!ny_i386_obj_i32(c, -4) || !ny_i386_obj_add_reloc(c, symbol, disp))
        return false;
      if (stack_bytes > 0 &&
          (!ny_i386_obj_bytes(c, (const unsigned char[]){0x81, 0xc4}, 2) ||
           !ny_i386_obj_i32(c, stack_bytes)))
        return false;
      if (in->dst >= 0) {
        if (in->dst < c->value_slots && c->value_f64 && c->value_f64[in->dst]) {
          if (!ny_i386_obj_store_value_f64(c, in->dst))
            return false;
        } else if (in->dst < c->value_slots && c->value_f32 && c->value_f32[in->dst]) {
          if (!ny_i386_obj_store_value_f32(c, in->dst))
            return false;
        } else if (!ny_i386_obj_store_value_eax(c, in->dst)) {
          return false;
        }
      }
      break;
    }
    case NYIR_LOAD_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x8b, 0x00}, 2) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_STORE_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_load_value_ebx(c, in->c) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x89, 0x18}, 2))
        return false;
      break;
    default:
      ny_native_set_err(c->err, c->err_len, "i386 ELF object writer: unsupported op %s",
                        ny_nir_op_name(in->op));
      return false;
    }
  }
  size_t epilogue_off = c->code.len;
  if (tag_return &&
      (!ny_i386_obj_bytes(c, (const unsigned char[]){0x8d, 0x04, 0x45}, 3) ||
       !ny_i386_obj_i32(c, 1)))
    return false;
  if (!ny_i386_obj_bytes(c, (const unsigned char[]){0x8b, 0x5d, 0xfc, 0xc9, 0xc3}, 5))
    return false;
  return ny_i386_obj_patch_branches(c, epilogue_off);
}

static bool ny_x64_obj_bytes(ny_x64_obj_ctx_t *c, const unsigned char *p,
                             size_t len) {
  if (!ny_obj_emit(&c->code, p, len)) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: out of memory");
    return false;
  }
  return true;
}

static bool ny_x64_obj_u8(ny_x64_obj_ctx_t *c, unsigned v) {
  return ny_x64_obj_bytes(c, (const unsigned char *)&(unsigned char){v}, 1);
}

static bool ny_x64_obj_i32(ny_x64_obj_ctx_t *c, int32_t v) {
  unsigned char b[4] = {(unsigned char)(v & 0xff),
                        (unsigned char)((uint32_t)v >> 8),
                        (unsigned char)((uint32_t)v >> 16),
                        (unsigned char)((uint32_t)v >> 24)};
  return ny_x64_obj_bytes(c, b, sizeof(b));
}

static bool ny_x64_obj_i64(ny_x64_obj_ctx_t *c, int64_t v) {
  for (int i = 0; i < 8; ++i) {
    if (!ny_x64_obj_u8(c, (unsigned)(((uint64_t)v >> (i * 8)) & 0xff)))
      return false;
  }
  return true;
}

static int ny_x64_obj_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static int ny_x64_obj_value_off(const ny_x64_obj_ctx_t *c, int value) {
  int slot = c->value_spill ? c->value_spill[value] : value;
  return -8 * (slot + 1);
}

static int ny_x64_obj_local_off(const ny_x64_obj_ctx_t *c, int local) {
  return -8 * (c->spill_slots + c->callee_save_slots + local + 1);
}

static bool ny_x64_obj_mov_rax_imm(ny_x64_obj_ctx_t *c, int64_t v) {
  static const unsigned char op[] = {0x48, 0xb8};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i64(c, v);
}

static bool ny_x64_obj_load_rax(ny_x64_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x48, 0x8b, 0x85};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_lea_rax(ny_x64_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x48, 0x8d, 0x85};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_lea_reg(ny_x64_obj_ctx_t *c, int reg, int off) {
  unsigned char op[] = {(unsigned char)(0x48 | (reg >= 8 ? 0x04 : 0)), 0x8d,
                        (unsigned char)(0x85 | ((reg & 7) << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_store_rax(ny_x64_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x48, 0x89, 0x85};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_r10(ny_x64_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x4c, 0x8b, 0x95};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_rcx(ny_x64_obj_ctx_t *c, int off) {
  static const unsigned char op[] = {0x48, 0x8b, 0x8d};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_mov_reg_reg(ny_x64_obj_ctx_t *c, int src, int dst) {
  if (src < 0 || src > 15 || dst < 0 || dst > 15)
    return false;
  unsigned char op[] = {
      (unsigned char)(0x48 | (src >= 8 ? 0x04 : 0) |
                      (dst >= 8 ? 0x01 : 0)),
      0x89,
      (unsigned char)(0xc0 | ((src & 7) << 3) | (dst & 7))};
  return ny_x64_obj_bytes(c, op, sizeof(op));
}

static bool ny_x64_obj_store_reg(ny_x64_obj_ctx_t *c, int reg, int off) {
  unsigned char op[] = {(unsigned char)(0x48 | (reg >= 8 ? 0x04 : 0)), 0x89,
                        (unsigned char)(0x85 | ((reg & 7) << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_reg(ny_x64_obj_ctx_t *c, int reg, int off) {
  unsigned char op[] = {(unsigned char)(0x48 | (reg >= 8 ? 0x04 : 0)), 0x8b,
                        (unsigned char)(0x85 | ((reg & 7) << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_value_rax(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, c->value_reg[value], 0);
  if (c->value_immediate && c->value_immediate[value]) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: immediate-only v%d was loaded",
                      value);
    return false;
  }
  return ny_x64_obj_load_rax(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_load_value_r10(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, c->value_reg[value], 10);
  if (c->value_immediate && c->value_immediate[value]) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: immediate-only v%d was loaded",
                      value);
    return false;
  }
  return ny_x64_obj_load_r10(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_load_value_rcx(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, c->value_reg[value], 1);
  if (c->value_immediate && c->value_immediate[value]) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: immediate-only v%d was loaded",
                      value);
    return false;
  }
  return ny_x64_obj_load_rcx(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_store_value_rax(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid destination v%d",
                      value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, 0, c->value_reg[value]);
  return ny_x64_obj_store_rax(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_load_xmm(ny_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 7)
    return false;
  unsigned char op[] = {0xf2, 0x0f, 0x10, (unsigned char)(0x85 | (xmm << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_store_xmm(ny_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 7)
    return false;
  unsigned char op[] = {0xf2, 0x0f, 0x11, (unsigned char)(0x85 | (xmm << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_xmm_f32(ny_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 7)
    return false;
  unsigned char op[] = {0xf3, 0x0f, 0x10, (unsigned char)(0x85 | (xmm << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_store_xmm_f32(ny_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 7)
    return false;
  unsigned char op[] = {0xf3, 0x0f, 0x11, (unsigned char)(0x85 | (xmm << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_mov_xmm(ny_x64_obj_ctx_t *c, int src, int dst,
                               bool f32) {
  if (src < 0 || src > 7 || dst < 0 || dst > 7)
    return false;
  unsigned char op[] = {(unsigned char)(f32 ? 0xf3 : 0xf2), 0x0f, 0x10,
                        (unsigned char)(0xc0 | (dst << 3) | src)};
  return ny_x64_obj_bytes(c, op, sizeof(op));
}

static bool ny_x64_obj_store_float_bits(ny_x64_obj_ctx_t *c, int value,
                                        bool f32) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid destination v%d",
                      value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0) {
    int xmm = c->value_xmm[value];
    unsigned char op64[] = {0x66, 0x48, 0x0f, 0x6e,
                            (unsigned char)(0xc0 | (xmm << 3))};
    unsigned char op32[] = {0x66, 0x0f, 0x6e,
                            (unsigned char)(0xc0 | (xmm << 3))};
    return f32 ? ny_x64_obj_bytes(c, op32, sizeof(op32))
               : ny_x64_obj_bytes(c, op64, sizeof(op64));
  }
  return ny_x64_obj_store_value_rax(c, value);
}

static bool ny_x64_obj_load_value_xmm(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, c->value_xmm[value], xmm, false);
  return ny_x64_obj_load_xmm(c, ny_x64_obj_value_off(c, value), xmm);
}

static bool ny_x64_obj_load_value_xmm_f32(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, c->value_xmm[value], xmm, true);
  return ny_x64_obj_load_xmm_f32(c, ny_x64_obj_value_off(c, value), xmm);
}

static bool ny_x64_obj_store_value_xmm_f32(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid destination v%d",
                      value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, xmm, c->value_xmm[value], true);
  return ny_x64_obj_store_xmm_f32(c, ny_x64_obj_value_off(c, value), xmm);
}

static bool ny_x64_obj_store_value_xmm(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid destination v%d",
                      value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, xmm, c->value_xmm[value], false);
  return ny_x64_obj_store_xmm(c, ny_x64_obj_value_off(c, value), xmm);
}

/* Table mapping value ID to its defining instruction index in the NYIR function.
 * Built once per function emission, enables O(1) constant detection. */
static void ny_x64_obj_valmap_init(ny_x64_obj_valmap_t *m, const ny_nir_func_t *nir) {
  m->count = 0;
  m->defs = NULL;
  if (!nir || nir->len == 0)
    return;
  int max_v = 0;
  for (size_t i = 0; i < nir->len; ++i) {
    if (nir->data[i].dst >= 0 && nir->data[i].dst > max_v)
      max_v = nir->data[i].dst;
  }
  m->count = max_v + 1;
  m->defs = (const ny_nir_inst_t **)calloc((size_t)m->count, sizeof(ny_nir_inst_t *));
  if (!m->defs) { m->count = 0; return; }
  for (size_t i = 0; i < nir->len; ++i) {
    int v = nir->data[i].dst;
    if (v >= 0 && v < m->count)
      m->defs[v] = &nir->data[i];
  }
}

static void ny_x64_obj_valmap_free(ny_x64_obj_valmap_t *m) {
  free(m->defs);
  m->defs = NULL;
  m->count = 0;
}

static const ny_nir_inst_t *ny_x64_obj_valmap_def(ny_x64_obj_valmap_t *m, int v) {
  if (v < 0 || v >= m->count || !m->defs)
    return NULL;
  return m->defs[v];
}

/* Check if value `v` is defined by a CONST_I64 instruction with immediate `imm`.
 * Returns true and sets *out_imm on success. */
static bool ny_x64_obj_try_const_i64(ny_x64_obj_ctx_t *c, int v, int64_t *out_imm) {
  if (v < 0)
    return false;
  const ny_nir_inst_t *inst = ny_x64_obj_valmap_def(&c->valmap, v);
  if (!inst || inst->op != NY_NIR_CONST_I64)
    return false;
  if (out_imm)
    *out_imm = inst->imm;
  return true;
}

/* Emit `op $imm, %rax` (sign-extended 32-bit immediate in %eax).
 * Returns true if the immediate fits in 32 bits. */
static bool ny_x64_obj_alu_imm_rax(ny_x64_obj_ctx_t *c, int64_t imm,
                                    const unsigned char *op_bytes, size_t op_len) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  return ny_x64_obj_bytes(c, op_bytes, op_len) && ny_x64_obj_i32(c, (int32_t)imm);
}

/* Emit `cmp $imm, %rax` (comparison against immediate). */
static bool ny_x64_obj_cmp_imm_rax(ny_x64_obj_ctx_t *c, int64_t imm) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  static const unsigned char op[] = {0x48, 0x3d};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, (int32_t)imm);
}

/* Emit `add $imm, %rax` (or sub, and, or, xor with immediate). */
static bool ny_x64_obj_binop_imm(ny_x64_obj_ctx_t *c, int64_t imm,
                                  ny_nir_op_t binop) {
  switch (binop) {
  case NY_NIR_ADD_I64: {
    static const unsigned char op[] = {0x48, 0x05};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NY_NIR_SUB_I64: {
    static const unsigned char op[] = {0x48, 0x2d};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NY_NIR_AND_I64: {
    static const unsigned char op[] = {0x48, 0x25};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NY_NIR_OR_I64: {
    static const unsigned char op[] = {0x48, 0x0d};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NY_NIR_XOR_I64: {
    static const unsigned char op[] = {0x48, 0x35};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  default:
    return false;
  }
}

static int ny_x64_obj_alu_opcode(ny_nir_op_t op) {
  switch (op) {
  case NY_NIR_ADD_I64: return 0x03;
  case NY_NIR_SUB_I64: return 0x2b;
  case NY_NIR_AND_I64: return 0x23;
  case NY_NIR_OR_I64: return 0x0b;
  case NY_NIR_XOR_I64: return 0x33;
  default: return -1;
  }
}

static int ny_x64_obj_alu_imm_group(ny_nir_op_t op) {
  switch (op) {
  case NY_NIR_ADD_I64: return 0;
  case NY_NIR_OR_I64: return 1;
  case NY_NIR_AND_I64: return 4;
  case NY_NIR_SUB_I64: return 5;
  case NY_NIR_XOR_I64: return 6;
  default: return -1;
  }
}

static bool ny_x64_obj_alu_reg_source(ny_x64_obj_ctx_t *c, ny_nir_op_t op,
                                      int dst_reg, int src_reg, int src_off,
                                      bool src_is_reg) {
  unsigned char rex = (unsigned char)(0x48 | (dst_reg >= 8 ? 0x04 : 0) |
                                       (src_is_reg && src_reg >= 8 ? 0x01 : 0));
  unsigned char modrm = (unsigned char)((src_is_reg ? 0xc0 : 0x85) |
                                        ((dst_reg & 7) << 3) |
                                        (src_is_reg ? src_reg & 7 : 0));
  if (op == NY_NIR_MUL_I64) {
    unsigned char bytes[] = {rex, 0x0f, 0xaf, modrm};
    return ny_x64_obj_bytes(c, bytes, sizeof(bytes)) &&
           (src_is_reg || ny_x64_obj_i32(c, src_off));
  }
  int opcode = ny_x64_obj_alu_opcode(op);
  if (opcode < 0)
    return false;
  unsigned char bytes[] = {rex, (unsigned char)opcode, modrm};
  return ny_x64_obj_bytes(c, bytes, sizeof(bytes)) &&
         (src_is_reg || ny_x64_obj_i32(c, src_off));
}

static bool ny_x64_obj_alu_reg_imm(ny_x64_obj_ctx_t *c, ny_nir_op_t op,
                                   int dst_reg, int64_t imm) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  unsigned char rex = (unsigned char)(0x48 | (dst_reg >= 8 ? 0x01 : 0));
  if (op == NY_NIR_MUL_I64) {
    unsigned char bytes[] = {
        rex, 0x69,
        (unsigned char)(0xc0 | ((dst_reg & 7) << 3) | (dst_reg & 7))};
    return ny_x64_obj_bytes(c, bytes, sizeof(bytes)) &&
           ny_x64_obj_i32(c, (int32_t)imm);
  }
  int group = ny_x64_obj_alu_imm_group(op);
  if (group < 0)
    return false;
  unsigned char bytes[] = {
      rex, 0x81, (unsigned char)(0xc0 | (group << 3) | (dst_reg & 7))};
  return ny_x64_obj_bytes(c, bytes, sizeof(bytes)) &&
         ny_x64_obj_i32(c, (int32_t)imm);
}

static bool ny_x64_obj_try_direct_binop(ny_x64_obj_ctx_t *c,
                                        const ny_nir_inst_t *in,
                                        bool *handled) {
  *handled = false;
  const char *disabled = getenv("NYTRIX_NATIVE_NO_DIRECT_ALU");
  if (disabled && disabled[0] && strcmp(disabled, "0") != 0 &&
      strcmp(disabled, "false") != 0 && strcmp(disabled, "off") != 0)
    return true;
  if (!c->value_reg || in->dst < 0 || in->a < 0 || in->b < 0)
    return true;
  int dst = c->value_reg[in->dst];
  int lhs = c->value_reg[in->a];
  int rhs = c->value_reg[in->b];
  bool commutative = in->op == NY_NIR_ADD_I64 ||
                     in->op == NY_NIR_MUL_I64 ||
                     in->op == NY_NIR_AND_I64 ||
                     in->op == NY_NIR_OR_I64 || in->op == NY_NIR_XOR_I64;
  int source = in->b;
  int source_reg = rhs;
  if (dst < 0)
    return true;
  if (dst != lhs) {
    if (!commutative || dst != rhs)
      return true;
    source = in->a;
    source_reg = lhs;
  }
  int64_t imm = 0;
  *handled = true;
  if (ny_x64_obj_try_const_i64(c, source, &imm) &&
      imm >= INT32_MIN && imm <= INT32_MAX)
    return ny_x64_obj_alu_reg_imm(c, in->op, dst, imm);
  if (source_reg >= 0)
    return ny_x64_obj_alu_reg_source(c, in->op, dst, source_reg, 0, true);
  return ny_x64_obj_alu_reg_source(
      c, in->op, dst, 0, ny_x64_obj_value_off(c, source), false);
}

static bool ny_x64_obj_binop(ny_x64_obj_ctx_t *c, const ny_nir_inst_t *in,
                             const unsigned char *op, size_t op_len) {
  bool handled = false;
  if (!ny_x64_obj_try_direct_binop(c, in, &handled))
    return false;
  if (handled)
    return true;
  int64_t imm = 0;
  /* Try immediate operand for B: load A, op $imm, store.
   * We detect the operation from the opcode's last byte to pick the
   * correct immediate encoding. */
  if (ny_x64_obj_try_const_i64(c, in->b, &imm) &&
      imm >= INT32_MIN && imm <= INT32_MAX &&
      ny_x64_obj_load_value_rax(c, in->a) &&
      ny_x64_obj_binop_imm(c, imm, in->op) &&
      ny_x64_obj_store_value_rax(c, in->dst))
    return true;
  if (in->op != NY_NIR_SUB_I64 &&
      ny_x64_obj_try_const_i64(c, in->a, &imm) &&
      imm >= INT32_MIN && imm <= INT32_MAX &&
      ny_x64_obj_load_value_rax(c, in->b) &&
      ny_x64_obj_binop_imm(c, imm, in->op) &&
      ny_x64_obj_store_value_rax(c, in->dst))
    return true;
  return ny_x64_obj_load_value_rax(c, in->a) &&
         ny_x64_obj_load_value_r10(c, in->b) &&
         ny_x64_obj_bytes(c, op, op_len) &&
         ny_x64_obj_store_value_rax(c, in->dst);
}

static bool ny_x64_obj_add_label(ny_x64_obj_ctx_t *c, int64_t label) {
  if (c->label_count >= sizeof(c->labels) / sizeof(c->labels[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: too many labels");
    return false;
  }
  for (size_t i = 0; i < c->label_count; ++i) {
    if (c->labels[i].label == label) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: duplicate label L%lld",
                        (long long)label);
      return false;
    }
  }
  c->labels[c->label_count++] =
      (ny_x64_obj_label_t){.label = label, .off = c->code.len};
  return true;
}

static bool ny_x64_obj_add_patch(ny_x64_obj_ctx_t *c, int64_t label,
                                 size_t disp_off) {
  if (c->patch_count >= sizeof(c->patches) / sizeof(c->patches[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: too many branches");
    return false;
  }
  c->patches[c->patch_count++] =
      (ny_x64_obj_patch_t){.label = label, .disp_off = disp_off};
  return true;
}


static bool ny_x64_obj_reloc_symbol(char *out, size_t out_len,
                                    const ny_x64_obj_ctx_t *c,
                                    const char *symbol) {
  if (!out || out_len == 0 || !symbol || !symbol[0])
    return false;
  const char *prefix = c && c->target && c->target->symbol_prefix
                           ? c->target->symbol_prefix
                           : "";
  int n = snprintf(out, out_len, "%sny_fn_%s", prefix, symbol);
  return n > 0 && (size_t)n < out_len;
}

static bool ny_x64_obj_add_reloc(ny_x64_obj_ctx_t *c, const char *symbol,
                                 size_t disp_off, int type) {
  if (c->reloc_count >= sizeof(c->relocs) / sizeof(c->relocs[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: too many relocations");
    return false;
  }
  snprintf(c->relocs[c->reloc_count].symbol,
           sizeof(c->relocs[0].symbol), "%s", symbol ? symbol : "");
  c->relocs[c->reloc_count].disp_off = disp_off;
  c->relocs[c->reloc_count].type = type;
  c->reloc_count++;
  return true;
}

static bool ny_x64_obj_mov_rax_to_arg(ny_x64_obj_ctx_t *c, const char *reg) {
  if (!reg)
    return false;
  if (strcmp(reg, "%rdi") == 0)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc7}, 3);
  if (strcmp(reg, "%rsi") == 0)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc6}, 3);
  if (strcmp(reg, "%rdx") == 0)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc2}, 3);
  if (strcmp(reg, "%rcx") == 0)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc1}, 3);
  if (strcmp(reg, "%r8") == 0)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0x89, 0xc0}, 3);
  if (strcmp(reg, "%r9") == 0)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0x89, 0xc1}, 3);
  ny_native_set_err(c->err, c->err_len,
                    "x86-64 object writer: unsupported arg register %s", reg);
  return false;
}

static int ny_x64_obj_arg_reg_code(const char *reg) {
  if (!reg) return -1;
  if (strcmp(reg, "%rdi") == 0) return 7;
  if (strcmp(reg, "%rsi") == 0) return 6;
  if (strcmp(reg, "%rdx") == 0) return 2;
  if (strcmp(reg, "%rcx") == 0) return 1;
  if (strcmp(reg, "%r8") == 0) return 8;
  if (strcmp(reg, "%r9") == 0) return 9;
  return -1;
}

static bool ny_x64_obj_load_aggregate_gp(ny_x64_obj_ctx_t *c, int offset,
                                         const char *reg) {
  int code = ny_x64_obj_arg_reg_code(reg);
  if (code < 0 || offset < 0 || offset > 127)
    return false;
  unsigned char bytes[] = {
      (unsigned char)(0x48 | ((code & 8) ? 0x04 : 0)), 0x8b,
      (unsigned char)(0x40 | ((code & 7) << 3)), (unsigned char)offset};
  return ny_x64_obj_bytes(c, bytes, sizeof(bytes));
}

static bool ny_x64_obj_load_aggregate_sse(ny_x64_obj_ctx_t *c, int offset,
                                          int xmm) {
  if (xmm < 0 || xmm > 7 || offset < 0 || offset > 127)
    return false;
  unsigned char bytes[] = {0xf3, 0x0f, 0x7e,
                           (unsigned char)(0x40 | (xmm << 3)),
                           (unsigned char)offset};
  return ny_x64_obj_bytes(c, bytes, sizeof(bytes));
}

static bool ny_x64_obj_sub_rsp(ny_x64_obj_ctx_t *c, size_t bytes) {
  if (bytes == 0)
    return true;
  if (bytes <= 127)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x83, 0xec}, 3) &&
           ny_x64_obj_u8(c, (unsigned)bytes);
  if (bytes <= INT32_MAX)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xec}, 3) &&
           ny_x64_obj_i32(c, (int32_t)bytes);
  ny_native_set_err(c->err, c->err_len,
                    "x86-64 object writer: stack adjustment too large");
  return false;
}

static bool ny_x64_obj_add_rsp(ny_x64_obj_ctx_t *c, size_t bytes) {
  if (bytes == 0)
    return true;
  if (bytes <= 127)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x83, 0xc4}, 3) &&
           ny_x64_obj_u8(c, (unsigned)bytes);
  if (bytes <= INT32_MAX)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xc4}, 3) &&
           ny_x64_obj_i32(c, (int32_t)bytes);
  ny_native_set_err(c->err, c->err_len,
                    "x86-64 object writer: stack adjustment too large");
  return false;
}


bool ny_x64_obj_emit_code(ny_x64_obj_ctx_t *c, const ny_nir_func_t *nir,
                          bool tag_return);
static bool ny_x64_obj_emit_epilogue(ny_x64_obj_ctx_t *c);

int ny_x64_obj_symbol_index(char symbols[][256], size_t count,
                                   const char *name) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(symbols[i], name) == 0)
      return (int)i;
  }
  return -1;
}

int ny_x64_obj_def_index(const ny_x64_obj_symbol_def_t *defs,
                                size_t def_count, const char *name) {
  for (size_t i = 0; defs && i < def_count; ++i) {
    if (strcmp(defs[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

bool ny_x64_obj_collect_external_reloc_symbols(
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count,
    const ny_x64_obj_symbol_def_t *defs, size_t def_count,
    char symbols[][256], size_t *symbol_count, char *err, size_t err_len) {
  size_t count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].symbol[0]) {
      ny_native_set_err(err, err_len,
                        "x86-64 object writer: empty relocation symbol");
      return false;
    }
    if (ny_x64_obj_def_index(defs, def_count, relocs[i].symbol) >= 0)
      continue;
    if (ny_x64_obj_symbol_index(symbols, count, relocs[i].symbol) >= 0)
      continue;
    if (count >= 256) {
      ny_native_set_err(err, err_len,
                        "x86-64 object writer: too many relocation symbols");
      return false;
    }
    snprintf(symbols[count++], 256, "%s", relocs[i].symbol);
  }
  *symbol_count = count;
  return true;
}

bool ny_x64_obj_append_function(ny_obj_buf_t *code,
                                       ny_x64_obj_symbol_def_t *defs,
                                       size_t *def_count,
                                       ny_x64_obj_reloc_t *relocs,
                                       size_t *reloc_count,
                                       const ny_nir_func_t *nir,
                                       const ny_native_target_info_t *target,
                                       const char *symbol_name,
                                       bool tag_return,
                                       char *err, size_t err_len) {
  if (!code || !defs || !def_count || !relocs || !reloc_count || !nir ||
      !target || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len, "x86-64 object writer: missing function input");
    return false;
  }
  if (*def_count >= 256) {
    ny_native_set_err(err, err_len, "x86-64 object writer: too many functions");
    return false;
  }
  if (ny_x64_obj_def_index(defs, *def_count, symbol_name) >= 0) {
    ny_native_set_err(err, err_len,
                      "x86-64 object writer: duplicate symbol %s", symbol_name);
    return false;
  }
  if (!ny_obj_pad_to(code, 16)) {
    ny_native_set_err(err, err_len, "x86-64 object writer: out of memory");
    return false;
  }
  size_t base = code->len;
  ny_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_x64_obj_emit_code(&ctx, nir, tag_return)) {
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }
  if (*reloc_count + ctx.reloc_count > 256) {
    ny_native_set_err(err, err_len,
                      "x86-64 object writer: too many relocations");
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }
  if (!ny_obj_emit(code, ctx.code.data, ctx.code.len)) {
    ny_native_set_err(err, err_len, "x86-64 object writer: out of memory");
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }
  ny_x64_obj_symbol_def_t *def = &defs[(*def_count)++];
  snprintf(def->name, sizeof(def->name), "%s", symbol_name);
  def->off = base;
  def->size = ctx.code.len;
  for (size_t i = 0; i < ctx.reloc_count; ++i) {
    relocs[*reloc_count] = ctx.relocs[i];
    relocs[*reloc_count].disp_off += base;
    (*reloc_count)++;
  }
  ny_x64_obj_ctx_free(&ctx);
  return true;
}

bool ny_x64_obj_collect_reloc_symbols(const ny_x64_obj_reloc_t *relocs,
                                             size_t reloc_count,
                                             char symbols[][256],
                                             size_t *symbol_count,
                                             char *err, size_t err_len) {
  size_t count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].symbol[0]) {
      ny_native_set_err(err, err_len, "x86-64 object writer: empty relocation symbol");
      return false;
    }
    if (ny_x64_obj_symbol_index(symbols, count, relocs[i].symbol) >= 0)
      continue;
    if (count >= 256) {
      ny_native_set_err(err, err_len, "x86-64 object writer: too many relocation symbols");
      return false;
    }
    snprintf(symbols[count++], 256, "%s", relocs[i].symbol);
  }
  *symbol_count = count;
  return true;
}

static bool ny_x64_obj_patch_branches(ny_x64_obj_ctx_t *c) {
  for (size_t i = 0; i < c->patch_count; ++i) {
    ny_x64_obj_patch_t *p = &c->patches[i];
    bool found = false;
    size_t target = 0;
    for (size_t j = 0; j < c->label_count; ++j) {
      if (c->labels[j].label == p->label) {
        found = true;
        target = c->labels[j].off;
        break;
      }
    }
    if (!found) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: unresolved label L%lld",
                        (long long)p->label);
      return false;
    }
    int64_t rel = (int64_t)target - (int64_t)(p->disp_off + 4);
    if (rel < INT32_MIN || rel > INT32_MAX) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: branch out of range");
      return false;
    }
    ny_obj_patch_u32(&c->code, p->disp_off, (uint32_t)(int32_t)rel);
  }
  return true;
}

static unsigned ny_x64_obj_setcc(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    return 0x94;
  case NY_NIR_CMP_NE:
    return 0x95;
  case NY_NIR_CMP_LT:
    return 0x9c;
  case NY_NIR_CMP_LE:
    return 0x9e;
  case NY_NIR_CMP_GT:
    return 0x9f;
  case NY_NIR_CMP_GE:
    return 0x9d;
  }
  return 0x94;
}

static unsigned ny_x64_obj_f64_setcc(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    return 0x94; /* sete */
  case NY_NIR_CMP_NE:
    return 0x95; /* setne */
  case NY_NIR_CMP_LT:
    return 0x92; /* setb */
  case NY_NIR_CMP_LE:
    return 0x96; /* setbe */
  case NY_NIR_CMP_GT:
    return 0x97; /* seta */
  case NY_NIR_CMP_GE:
    return 0x93; /* setae */
  }
  return 0x94;
}

static bool ny_x64_obj_emit_inst(ny_x64_obj_ctx_t *c,
                                 const ny_nir_inst_t *in) {
  static const unsigned char add[] = {0x4c, 0x01, 0xd0};
  static const unsigned char sub[] = {0x4c, 0x29, 0xd0};
  static const unsigned char imul[] = {0x49, 0x0f, 0xaf, 0xc2};
  static const unsigned char andq[] = {0x4c, 0x21, 0xd0};
  static const unsigned char orq[] = {0x4c, 0x09, 0xd0};
  static const unsigned char xorq[] = {0x4c, 0x31, 0xd0};
  switch (in->op) {
  case NY_NIR_NOP:
    return true;
  case NY_NIR_CONST_I64:
    if (c->value_immediate && in->dst >= 0 &&
        in->dst < c->value_slots && c->value_immediate[in->dst])
      return true;
    return ny_x64_obj_mov_rax_imm(c, in->imm) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_CONST_F64:
    return ny_x64_obj_mov_rax_imm(c, in->imm) &&
           ny_x64_obj_store_float_bits(c, in->dst, false);
  case NYIR_CONST_F32:
    return ny_x64_obj_mov_rax_imm(c, in->imm) &&
           ny_x64_obj_store_float_bits(c, in->dst, true);
  case NY_NIR_COPY:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NY_NIR_ADD_I64:
    return ny_x64_obj_binop(c, in, add, sizeof(add));
  case NY_NIR_SUB_I64:
    return ny_x64_obj_binop(c, in, sub, sizeof(sub));
  case NY_NIR_MUL_I64: {
    int64_t imm = 0;
    /* imul $imm, %rax, %rax (0x69 c0 imm32) for small constants */
    if (ny_x64_obj_try_const_i64(c, in->b, &imm) &&
        imm >= INT32_MIN && imm <= INT32_MAX &&
        ny_x64_obj_load_value_rax(c, in->a) &&
        ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x69, 0xc0}, 3) &&
        ny_x64_obj_i32(c, (int32_t)imm))
      return ny_x64_obj_store_value_rax(c, in->dst);
    /* Commutative: try A as immediate */
    if (ny_x64_obj_try_const_i64(c, in->a, &imm) &&
        imm >= INT32_MIN && imm <= INT32_MAX &&
        ny_x64_obj_load_value_rax(c, in->b) &&
        ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x69, 0xc0}, 3) &&
        ny_x64_obj_i32(c, (int32_t)imm))
      return ny_x64_obj_store_value_rax(c, in->dst);
    return ny_x64_obj_binop(c, in, imul, sizeof(imul));
  }
  case NY_NIR_AND_I64:
    return ny_x64_obj_binop(c, in, andq, sizeof(andq));
  case NY_NIR_OR_I64:
    return ny_x64_obj_binop(c, in, orq, sizeof(orq));
  case NY_NIR_XOR_I64:
    return ny_x64_obj_binop(c, in, xorq, sizeof(xorq));
  case NY_NIR_DIV_I64:
  case NY_NIR_MOD_I64:
    if (!ny_x64_obj_load_value_rax(c, in->a) ||
        !ny_x64_obj_load_value_r10(c, in->b))
      return false;
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x99}, 2) ||
        !ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0xf7, 0xfa}, 3))
      return false;
    if (in->op == NY_NIR_MOD_I64 &&
        !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xd0}, 3))
      return false;
    return ny_x64_obj_store_value_rax(c, in->dst);
  case NY_NIR_SHL_I64:
  case NY_NIR_SAR_I64: {
    int64_t shift_imm = 0;
    bool is_shl = (in->op == NY_NIR_SHL_I64);
    /* Try immediate shift count: shl $imm, %rax (0xc1 e0/ f8) */
    if (ny_x64_obj_try_const_i64(c, in->b, &shift_imm) &&
        shift_imm >= 1 && shift_imm <= 63 &&
        ny_x64_obj_load_value_rax(c, in->a)) {
      unsigned char base = is_shl ? 0xe0 : 0xf8;
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, base}, 3) ||
          !ny_x64_obj_u8(c, (unsigned char)shift_imm))
        return false;
      return ny_x64_obj_store_value_rax(c, in->dst);
    }
    /* Fallback: shift by %rcx */
    if (!ny_x64_obj_load_value_rax(c, in->a) ||
        !ny_x64_obj_load_value_rcx(c, in->b))
      return false;
    if (is_shl) {
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xd3, 0xe0}, 3))
        return false;
    } else if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xd3, 0xf8},
                                   3))
      return false;
    return ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NY_NIR_CMP_I64: {
    int64_t imm = 0;
    /* cmp $imm, value_a if B is constant */
    if (ny_x64_obj_try_const_i64(c, in->b, &imm) &&
        imm >= INT32_MIN && imm <= INT32_MAX &&
        ny_x64_obj_load_value_rax(c, in->a) &&
        ny_x64_obj_cmp_imm_rax(c, imm)) {
      if (!ny_x64_obj_u8(c, 0x0f) ||
          !ny_x64_obj_u8(c, ny_x64_obj_setcc(in->cmp)) ||
          !ny_x64_obj_u8(c, 0xc0) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x0f, 0xb6, 0xc0},
                            4))
        return false;
      return ny_x64_obj_store_value_rax(c, in->dst);
    }
    /* For EQ/NE, swap and try A as immediate (commutative) */
    if ((in->cmp == NY_NIR_CMP_EQ || in->cmp == NY_NIR_CMP_NE) &&
        ny_x64_obj_try_const_i64(c, in->a, &imm) &&
        imm >= INT32_MIN && imm <= INT32_MAX &&
        ny_x64_obj_load_value_rax(c, in->b) &&
        ny_x64_obj_cmp_imm_rax(c, imm)) {
      if (!ny_x64_obj_u8(c, 0x0f) ||
          !ny_x64_obj_u8(c, ny_x64_obj_setcc(in->cmp)) ||
          !ny_x64_obj_u8(c, 0xc0) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x0f, 0xb6, 0xc0},
                            4))
        return false;
      return ny_x64_obj_store_value_rax(c, in->dst);
    }
    /* Fallback: both operands from memory */
    if (!ny_x64_obj_load_value_rax(c, in->a) ||
        !ny_x64_obj_load_value_r10(c, in->b) ||
        !ny_x64_obj_bytes(c, (const unsigned char[]){0x4c, 0x39, 0xd0}, 3) ||
        !ny_x64_obj_u8(c, 0x0f) ||
        !ny_x64_obj_u8(c, ny_x64_obj_setcc(in->cmp)) ||
        !ny_x64_obj_u8(c, 0xc0) ||
        !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x0f, 0xb6, 0xc0},
                          4))
      return false;
    return ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64: {
    unsigned char op = in->op == NYIR_ADD_F64 ? 0x58 :
                       in->op == NYIR_SUB_F64 ? 0x5c :
                       in->op == NYIR_MUL_F64 ? 0x59 : 0x5e;
    return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm(c, in->b, 1) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf2, 0x0f, op, 0xc1},
                            4) &&
           ny_x64_obj_store_value_xmm(c, in->dst, 0);
  }
  case NYIR_I64_TO_F64:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf2, 0x48, 0x0f, 0x2a,
                                                       0xc0},
                            5) &&
           ny_x64_obj_store_value_xmm(c, in->dst, 0);
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32: {
    unsigned char op = in->op == NYIR_ADD_F32 ? 0x58 :
                       in->op == NYIR_SUB_F32 ? 0x5c :
                       in->op == NYIR_MUL_F32 ? 0x59 : 0x5e;
    return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm_f32(c, in->b, 1) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0x0f, op, 0xc1},
                            4) &&
           ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
  }
  case NYIR_I64_TO_F32:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0x48, 0x0f, 0x2a,
                                                       0xc0},
                            5) &&
           ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
  case NYIR_F32_TO_F64:
    return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0x0f, 0x5a, 0xc0},
                            4) &&
           ny_x64_obj_store_value_xmm(c, in->dst, 0);
  case NYIR_F64_TO_F32:
    return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf2, 0x0f, 0x5a, 0xc0},
                            4) &&
           ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
  case NYIR_CMP_F64: {
    unsigned unordered = in->cmp == NY_NIR_CMP_NE ? 1u : 0u;
    return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm(c, in->b, 1) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x31, 0xc0, 0x66, 0x0f,
                                                       0x2e, 0xc1, 0x7a, 0x05,
                                                       0x0f},
                            9) &&
           ny_x64_obj_u8(c, ny_x64_obj_f64_setcc(in->cmp)) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xc0, 0xeb, 0x02, 0xb0},
                            4) &&
           ny_x64_obj_u8(c, unordered) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NYIR_CMP_F32: {
    unsigned unordered = in->cmp == NY_NIR_CMP_NE ? 1u : 0u;
    return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm_f32(c, in->b, 1) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x31, 0xc0, 0x0f, 0x2e,
                                                       0xc1, 0x7a, 0x05, 0x0f},
                            8) &&
           ny_x64_obj_u8(c, ny_x64_obj_f64_setcc(in->cmp)) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xc0, 0xeb, 0x02, 0xb0},
                            4) &&
           ny_x64_obj_u8(c, unordered) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NY_NIR_LABEL:
    return ny_x64_obj_add_label(c, in->imm);
  case NY_NIR_LOAD_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: invalid local slot %lld",
                        (long long)in->imm);
      return false;
    }
    if (c->local_f64 && c->local_f64[in->imm])
      return ny_x64_obj_load_xmm(c,
                                  ny_x64_obj_local_off(c, (int)in->imm), 0) &&
             ny_x64_obj_store_value_xmm(c, in->dst, 0);
    if (c->local_f32 && c->local_f32[in->imm])
      return ny_x64_obj_load_xmm_f32(
                 c, ny_x64_obj_local_off(c, (int)in->imm), 0) &&
             ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
    if (c->value_reg && c->value_reg[in->dst] >= 0)
      return ny_x64_obj_load_reg(c, c->value_reg[in->dst],
                                 ny_x64_obj_local_off(c, (int)in->imm));
    return ny_x64_obj_load_rax(c, ny_x64_obj_local_off(c, (int)in->imm)) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_ADDR_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: invalid local slot %lld",
                        (long long)in->imm);
      return false;
    }
    if (c->value_reg && c->value_reg[in->dst] >= 0)
      return ny_x64_obj_lea_reg(c, c->value_reg[in->dst],
                                ny_x64_obj_local_off(c, (int)in->imm));
    return ny_x64_obj_lea_rax(c, ny_x64_obj_local_off(c, (int)in->imm)) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_ADDR_SYMBOL:
    if (!in->symbol || !in->symbol[0]) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: addr.symbol missing symbol name");
      return false;
    }
    if (c->value_reg && c->value_reg[in->dst] >= 0) {
      int reg = c->value_reg[in->dst];
      unsigned char op[] = {(unsigned char)(0x48 | (reg >= 8 ? 0x04 : 0)), 0x8d,
                            (unsigned char)(0x05 | ((reg & 7) << 3))};
      if (!ny_x64_obj_bytes(c, op, sizeof(op)))
        return false;
    } else {
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x8d, 0x05}, 3))
        return false;
    }
    {
      char sym[256];
      if (!ny_x64_obj_reloc_symbol(sym, sizeof(sym), c, in->symbol)) {
        ny_native_set_err(c->err, c->err_len,
                          "x86-64 ELF object writer: invalid addr symbol");
        return false;
      }
      size_t disp = c->code.len;
      if (!ny_x64_obj_i32(c, -4) || !ny_x64_obj_add_reloc(c, sym, disp, NY_RELOC_PC32))
        return false;
      if (!c->value_reg || c->value_reg[in->dst] < 0) {
        if (!ny_x64_obj_store_value_rax(c, in->dst))
          return false;
      }
    }
    return true;
  case NY_NIR_STORE_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: invalid local slot %lld",
                        (long long)in->imm);
      return false;
    }
    if (c->local_f64 && c->local_f64[in->imm])
      return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
             ny_x64_obj_store_xmm(c,
                                  ny_x64_obj_local_off(c, (int)in->imm), 0);
    if (c->local_f32 && c->local_f32[in->imm])
      return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
             ny_x64_obj_store_xmm_f32(
                 c, ny_x64_obj_local_off(c, (int)in->imm), 0);
    if (c->value_reg && c->value_reg[in->a] >= 0)
      return ny_x64_obj_store_reg(c, c->value_reg[in->a],
                                  ny_x64_obj_local_off(c, (int)in->imm));
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_store_rax(c, ny_x64_obj_local_off(c, (int)in->imm));
  case NY_NIR_BR: {
    if (!ny_x64_obj_u8(c, 0xe9))
      return false;
    size_t disp = c->code.len;
    return ny_x64_obj_i32(c, 0) && ny_x64_obj_add_patch(c, in->imm, disp);
  }
  case NY_NIR_BR_IF: {
    if (!ny_x64_obj_load_value_rax(c, in->a) ||
        !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x85, 0xc0, 0x0f,
                                                     0x85},
                          5))
      return false;
    size_t disp = c->code.len;
    return ny_x64_obj_i32(c, 0) && ny_x64_obj_add_patch(c, in->imm, disp);
  }
  case NY_NIR_RET:
    if (in->a >= 0) {
      if (c->value_f64 && in->a < c->value_slots && c->value_f64[in->a]) {
        if (!ny_x64_obj_load_value_xmm(c, in->a, 0))
          return false;
      } else if (c->value_f32 && in->a < c->value_slots && c->value_f32[in->a]) {
        if (!ny_x64_obj_load_value_xmm_f32(c, in->a, 0))
          return false;
      } else if (!ny_x64_obj_load_value_rax(c, in->a)) {
        return false;
      }
    }
    return ny_x64_obj_emit_epilogue(c);
  case NY_NIR_CALL: {
    int argc = (int)in->imm;
    if (argc < 0 || argc > NY_NIR_CALL_MAX_ARGS) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 object writer: call exceeds maximum supported arg count");
      return false;
    }
    if (!c->target) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 object writer: call ABI metadata unavailable");
      return false;
    }
    int args[NY_NIR_CALL_MAX_ARGS];
    if (argc > 0) args[0] = in->a;
    if (argc > 1) args[1] = in->b;
    if (argc > 2) args[2] = in->c;
    if (argc > 3) args[3] = in->d;
    if (argc > 4) args[4] = in->e;
    if (argc > 5) args[5] = in->f;
    for (int i = 6; i < argc; ++i)
      args[i] = (in->extra_args && (size_t)(i - 6) < in->extra_args_len)
                    ? in->extra_args[i - 6]
                    : -1;
    bool arg_f64[NY_NIR_CALL_MAX_ARGS] = {0};
    bool arg_f32[NY_NIR_CALL_MAX_ARGS] = {0};
    int gp_index[NY_NIR_CALL_MAX_ARGS];
    int sse_index[NY_NIR_CALL_MAX_ARGS];
    int agg_gp[NY_NIR_CALL_MAX_ARGS][2];
    int agg_sse[NY_NIR_CALL_MAX_ARGS][2];
    bool agg_in_regs[NY_NIR_CALL_MAX_ARGS] = {0};
    int gp = 0;
    int sse = 0;
    int stack_argc = 0;
    for (int i = 0; i < argc; ++i) {
      gp_index[i] = -1;
      sse_index[i] = -1;
      agg_gp[i][0] = agg_gp[i][1] = -1;
      agg_sse[i][0] = agg_sse[i][1] = -1;
      if (args[i] < 0) {
        ny_native_set_err(c->err, c->err_len,
                          "x86-64 object writer: invalid call arg");
        return false;
      }
      if (in->arg_sizes && in->arg_sizes[i] > 0) {
        arg_f64[i] = false;
        arg_f32[i] = false;
        uint32_t size = NY_NIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        unsigned gp_need = 0, sse_need = 0;
        bool register_eligible = true;
        for (int chunk = 0; chunk < 2; ++chunk) {
          unsigned cls = NY_NIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
          gp_need += cls == NY_NIR_ARG_CLASS_INTEGER;
          sse_need += cls == NY_NIR_ARG_CLASS_SSE;
          if (cls != NY_NIR_ARG_CLASS_NONE &&
              cls != NY_NIR_ARG_CLASS_INTEGER &&
              cls != NY_NIR_ARG_CLASS_SSE)
            register_eligible = false;
        }
        if (register_eligible && size <= 16 &&
            gp + (int)gp_need <= (int)c->target->gp_arg_reg_count &&
            sse + (int)sse_need <= 8) {
          for (int chunk = 0; chunk < 2; ++chunk) {
            unsigned cls = NY_NIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
            if (cls == NY_NIR_ARG_CLASS_INTEGER)
              agg_gp[i][chunk] = gp++;
            else if (cls == NY_NIR_ARG_CLASS_SSE)
              agg_sse[i][chunk] = sse++;
          }
          agg_in_regs[i] = true;
        } else {
          stack_argc += (int)((size + 7) / 8);
        }
        continue;
      }
      arg_f64[i] = c->value_f64 && args[i] < c->value_slots &&
                   c->value_f64[args[i]];
      arg_f32[i] = c->value_f32 && args[i] < c->value_slots &&
                   c->value_f32[args[i]];
      if (arg_f64[i] || arg_f32[i]) {
        if (sse < 8)
          sse_index[i] = sse++;
        else
          stack_argc++;
      } else {
        if ((size_t)gp < c->target->gp_arg_reg_count)
          gp_index[i] = gp++;
        else
          stack_argc++;
      }
    }
    int pad = stack_argc % 2;
    if (pad && !ny_x64_obj_sub_rsp(c, 8))
      return false;
    for (int i = argc - 1; i >= 0; --i) {
      if (gp_index[i] >= 0 || sse_index[i] >= 0 || agg_in_regs[i])
        continue;
      if (in->arg_sizes && in->arg_sizes[i] > 0) {
        /* byval: allocate stack space and copy aggregate */
        uint32_t size = NY_NIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        int slots = (int)((size + 7) / 8);
        if (!ny_x64_obj_sub_rsp(c, (size_t)slots * 8))
          return false;
        /* load src ptr -> rsi, rsp -> rdi, movsb */
        if (!ny_x64_obj_load_value_rax(c, args[i]))
          return false;
        /* mov %rax, %rsi */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc6}, 3))
          return false;
        /* mov %rsp, %rdi */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xe7}, 3))
          return false;
        /* mov $size, %rcx */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc7, 0xc1}, 3) ||
            !ny_x64_obj_i32(c, (int32_t)size))
          return false;
        /* rep movsb */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0xa4}, 2))
          return false;
        continue;
      }
      if (arg_f64[i]) {
        if (!ny_x64_obj_sub_rsp(c, 8) ||
            !ny_x64_obj_load_value_xmm(c, args[i], 0) ||
            !ny_x64_obj_bytes(c, (const unsigned char[]){0xf2, 0x0f, 0x11,
                                                         0x04, 0x24},
                              5))
          return false;
      } else if (arg_f32[i]) {
        if (!ny_x64_obj_sub_rsp(c, 8) ||
            !ny_x64_obj_load_value_xmm_f32(c, args[i], 0) ||
            !ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0x0f, 0x11,
                                                         0x04, 0x24},
                              5))
          return false;
      } else if (!ny_x64_obj_load_value_rax(c, args[i]) ||
                 !ny_x64_obj_bytes(c, (const unsigned char[]){0x50}, 1)) {
        return false;
      }
    }
    for (int i = 0; i < argc; ++i) {
      if (agg_in_regs[i]) {
        if (!ny_x64_obj_load_value_rax(c, args[i]))
          return false;
        for (int chunk = 0; chunk < 2; ++chunk) {
          if (agg_gp[i][chunk] >= 0) {
            if (!ny_x64_obj_load_aggregate_gp(
                    c, chunk * 8,
                    c->target->gp_arg_regs[agg_gp[i][chunk]]))
              return false;
          } else if (agg_sse[i][chunk] >= 0 &&
                     !ny_x64_obj_load_aggregate_sse(
                         c, chunk * 8, agg_sse[i][chunk])) {
            return false;
          }
        }
      } else if (sse_index[i] >= 0) {
        if (arg_f32[i]) {
          if (!ny_x64_obj_load_value_xmm_f32(c, args[i], sse_index[i]))
            return false;
        } else if (!ny_x64_obj_load_value_xmm(c, args[i], sse_index[i])) {
          return false;
        }
      } else if (gp_index[i] >= 0) {
        if (!ny_x64_obj_load_value_rax(c, args[i]) ||
            !ny_x64_obj_mov_rax_to_arg(c, c->target->gp_arg_regs[gp_index[i]]))
          return false;
      }
    }
    if (!ny_x64_obj_sub_rsp(c, c->target->shadow_space_bytes))
      return false;
    if (!ny_x64_obj_u8(c, 0xe8))
      return false;
    size_t disp = c->code.len;
    char symbol[256];
    if (in->flags & NY_NIR_INST_F_EXTERN) {
      const char *prefix = c->target->symbol_prefix ? c->target->symbol_prefix : "";
      int n = snprintf(symbol, sizeof(symbol), "%s%s", prefix,
                       in->symbol ? in->symbol : "<null>");
      if (n < 0 || (size_t)n >= sizeof(symbol)) {
        ny_native_set_err(c->err, c->err_len,
                          "x86-64 object writer: extern symbol too long");
        return false;
      }
    } else if (!ny_x64_obj_reloc_symbol(symbol, sizeof(symbol), c, in->symbol)) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 object writer: invalid call symbol");
      return false;
    }
    if (!ny_x64_obj_i32(c, 0) || !ny_x64_obj_add_reloc(c, symbol, disp, NY_RELOC_PLT32))
      return false;
    if (!ny_x64_obj_add_rsp(c, c->target->shadow_space_bytes))
      return false;
    if (stack_argc + pad > 0 &&
        !ny_x64_obj_add_rsp(c, (size_t)(stack_argc + pad) * 8))
      return false;
    if (in->dst < 0)
      return true;
    if (c->value_f64 && in->dst < c->value_slots && c->value_f64[in->dst])
      return ny_x64_obj_store_value_xmm(c, in->dst, 0);
    if (c->value_f32 && in->dst < c->value_slots && c->value_f32[in->dst])
      return ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
    return ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NYIR_LOAD_I64:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x8b, 0x00}, 3) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_STORE_I64:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_load_value_r10(c, in->c) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x4c, 0x89, 0x10}, 3);
  case NYIR_ALLOCA:
    if (in->dst < 0)
      return true;
    /* subq $size, %rsp; andq $-16, %rsp; movq %rsp, %rax; store */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xec}, 3) ||
        !ny_x64_obj_i32(c, (int32_t)in->imm))
      return false;
    /* andq $-16, %rsp: 0x48 0x83 0xe4 0xf0 */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x83, 0xe4, 0xf0}, 4))
      return false;
    /* mov %rsp, %rax */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xe0}, 3))
      return false;
    return ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_COPY_STRUCT:
    if (in->imm <= 0)
      return true;
    /* load src (b) -> rsi, load dst (a) -> rdi, mov size -> rcx, rep movsb */
    if (!ny_x64_obj_load_value_rax(c, in->b))
      return false;
    /* mov %rax, %rsi */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc6}, 3))
      return false;
    if (!ny_x64_obj_load_value_rax(c, in->a))
      return false;
    /* mov %rax, %rdi */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc7}, 3))
      return false;
    /* mov $size, %rcx */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc7, 0xc1}, 3) ||
        !ny_x64_obj_i32(c, (int32_t)in->imm))
      return false;
    /* rep movsb */
    return ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0xa4}, 2);
  case NYIR_CAPTURE_RET:
    if (in->dst < 0)
      return true;
    switch (in->imm) {
    case 0: /* rdx -> rax */
      if (!ny_x64_obj_bytes(c,
                            (const unsigned char[]){0x48, 0x89, 0xd0}, 3))
        return false;
      break;
    case 1: /* rax */
      break;
    case 2: /* xmm0 -> rax */
      if (!ny_x64_obj_bytes(
              c, (const unsigned char[]){0x66, 0x48, 0x0f, 0x7e, 0xc0}, 5))
        return false;
      break;
    case 3: /* xmm1 -> rax */
      if (!ny_x64_obj_bytes(
              c, (const unsigned char[]){0x66, 0x48, 0x0f, 0x7e, 0xc8}, 5))
        return false;
      break;
    default:
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 object writer: invalid capture.ret selector");
      return false;
    }
    return ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_OP_COUNT:
    break;
  }
  ny_native_set_err(c->err, c->err_len,
                    "x86-64 ELF object writer: unsupported op %s",
                    ny_nir_op_name(in->op));
  return false;
}

static void ny_x64_obj_scan_frame(ny_x64_obj_ctx_t *c, const ny_nir_func_t *nir) {
  c->value_slots = nir && nir->next_value > 0 ? nir->next_value : 0;
  c->local_slots = 0;
  for (size_t i = 0; nir && i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL) &&
        in->imm >= c->local_slots)
      c->local_slots = (int)in->imm + 1;
  }
  c->frame_bytes = ny_x64_obj_align((c->value_slots + c->local_slots) * 8, 16);
}

static bool ny_x64_obj_op_is_f64(ny_nir_op_t op) {
  return op == NYIR_CONST_F64 || op == NYIR_ADD_F64 ||
         op == NYIR_SUB_F64 || op == NYIR_MUL_F64 ||
         op == NYIR_DIV_F64 || op == NYIR_I64_TO_F64 ||
         op == NYIR_F32_TO_F64;
}

static bool ny_x64_obj_op_is_f32(ny_nir_op_t op) {
  return op == NYIR_CONST_F32 || op == NYIR_ADD_F32 ||
         op == NYIR_SUB_F32 || op == NYIR_MUL_F32 ||
         op == NYIR_DIV_F32 || op == NYIR_I64_TO_F32 ||
         op == NYIR_F64_TO_F32;
}

static bool ny_x64_obj_classify_values(ny_x64_obj_ctx_t *c,
                                       const ny_nir_func_t *nir) {
  if (!c)
    return false;
  if (c->value_slots > 0) {
    c->value_f64 = (bool *)calloc((size_t)c->value_slots, sizeof(bool));
    c->value_f32 = (bool *)calloc((size_t)c->value_slots, sizeof(bool));
    if (!c->value_f64 || !c->value_f32) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 object writer: out of memory");
      return false;
    }
  }
  if (c->local_slots > 0) {
    c->local_f64 = (bool *)calloc((size_t)c->local_slots, sizeof(bool));
    c->local_f32 = (bool *)calloc((size_t)c->local_slots, sizeof(bool));
    if (!c->local_f64 || !c->local_f32) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 object writer: out of memory");
      return false;
    }
  }
  for (size_t i = 0; nir && i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if (in->dst >= 0 && in->dst < c->value_slots &&
        (ny_x64_obj_op_is_f64(in->op) ||
         (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F64))))
      c->value_f64[in->dst] = true;
    if (in->dst >= 0 && in->dst < c->value_slots &&
        (ny_x64_obj_op_is_f32(in->op) ||
         ((in->flags & NY_NIR_INST_F_RET_F32) &&
          in->op == NY_NIR_CALL)))
      c->value_f32[in->dst] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; nir && i < nir->len; ++i) {
      const ny_nir_inst_t *in = &nir->data[i];
      if (in->op == NY_NIR_COPY && in->dst >= 0 && in->a >= 0 &&
          in->dst < c->value_slots && in->a < c->value_slots &&
          c->value_f64[in->a] && !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_COPY && in->dst >= 0 && in->a >= 0 &&
                 in->dst < c->value_slots && in->a < c->value_slots &&
                 c->value_f32[in->a] && !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->local_f64[in->imm] && !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->local_f32[in->imm] && !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->value_f64[in->dst] && !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->value_f32[in->dst] && !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      } else if (in->op == NY_NIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < c->value_slots && in->imm < c->local_slots &&
                 c->value_f64[in->a] && !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NY_NIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < c->value_slots && in->imm < c->local_slots &&
                 c->value_f32[in->a] && !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      }
      if (in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
          in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
          in->op == NYIR_CMP_F64) {
        if (in->a >= 0 && in->a < c->value_slots && !c->value_f64[in->a]) {
          c->value_f64[in->a] = true;
          changed = true;
        }
        if (in->b >= 0 && in->b < c->value_slots && !c->value_f64[in->b]) {
          c->value_f64[in->b] = true;
          changed = true;
        }
      }
      if (in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
          in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32 ||
          in->op == NYIR_CMP_F32) {
        if (in->a >= 0 && in->a < c->value_slots && !c->value_f32[in->a]) {
          c->value_f32[in->a] = true;
          changed = true;
        }
        if (in->b >= 0 && in->b < c->value_slots && !c->value_f32[in->b]) {
          c->value_f32[in->b] = true;
          changed = true;
        }
      }
    }
  }
  return true;
}

/* Keep non-floating SSA intervals in caller-saved r11/r9/r8 when they neither
 * cross nor feed a call. Values with call-sensitive lifetimes remain spilled,
 * so ABI argument setup cannot clobber an allocated value. */
static bool ny_x64_obj_allocate_registers(ny_x64_obj_ctx_t *c,
                                          const ny_nir_func_t *nir) {
  const char *disabled = getenv("NYTRIX_NATIVE_NO_REGALLOC");
  if (disabled && disabled[0] && strcmp(disabled, "0") != 0 &&
      strcmp(disabled, "false") != 0 && strcmp(disabled, "off") != 0)
    return true;
  if (!c || !nir || c->value_slots <= 0)
    return true;
  c->value_reg = malloc((size_t)c->value_slots * sizeof(*c->value_reg));
  c->value_xmm = malloc((size_t)c->value_slots * sizeof(*c->value_xmm));
  int *last_use = malloc((size_t)c->value_slots * sizeof(*last_use));
  bool *call_use = calloc((size_t)c->value_slots, sizeof(*call_use));
  int *next_call = malloc((nir->len + 1u) * sizeof(*next_call));
  if (!c->value_reg || !c->value_xmm || !last_use || !call_use || !next_call) {
    free(last_use);
    free(call_use);
    free(next_call);
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: out of memory in register allocation");
    return false;
  }
  memset(c->value_reg, -1,
         (size_t)c->value_slots * sizeof(*c->value_reg));
  memset(c->value_xmm, -1,
         (size_t)c->value_slots * sizeof(*c->value_xmm));
  for (int v = 0; v < c->value_slots; ++v)
    last_use[v] = -1;
  for (size_t i = 0; nir && i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k) {
      int v = operands[k];
      if (v >= 0 && v < c->value_slots) {
        last_use[v] = (int)i;
        if (in->op == NY_NIR_CALL)
          call_use[v] = true;
      }
    }
    if (in->op == NY_NIR_CALL) {
      for (size_t k = 0; k < in->extra_args_len; ++k) {
        int v = in->extra_args[k];
        if (v >= 0 && v < c->value_slots) {
          last_use[v] = (int)i;
          call_use[v] = true;
        }
      }
    }
  }
  next_call[nir->len] = INT_MAX;
  for (size_t i = nir->len; i > 0; --i)
    next_call[i - 1] = nir->data[i - 1].op == NY_NIR_CALL
                           ? (int)(i - 1)
                           : next_call[i];
  static const int caller_regs[] = {11, 9, 8};
  static const int callee_regs[] = {12, 13, 14, 15, 3};
  int caller_end[sizeof(caller_regs) / sizeof(caller_regs[0])];
  int callee_end[sizeof(callee_regs) / sizeof(callee_regs[0])];
  for (size_t r = 0; r < sizeof(caller_regs) / sizeof(caller_regs[0]); ++r)
    caller_end[r] = -1;
  for (size_t r = 0; r < sizeof(callee_regs) / sizeof(callee_regs[0]); ++r)
    callee_end[r] = -1;
  for (size_t i = 0; nir && i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    int v = in->dst;
    if (v < 0 || v >= c->value_slots || last_use[v] < (int)i ||
        (c->value_f64 && c->value_f64[v]) ||
        (c->value_f32 && c->value_f32[v]))
      continue;
    bool crosses_call = next_call[i + 1] < last_use[v];
    if (in->op == NY_NIR_CONST_I64 && !crosses_call)
      continue;
    if (!crosses_call && !call_use[v]) {
      for (size_t r = 0; r < sizeof(caller_regs) / sizeof(caller_regs[0]); ++r) {
        if ((int)i >= caller_end[r]) {
          c->value_reg[v] = (int8_t)caller_regs[r];
          caller_end[r] = last_use[v];
          break;
        }
      }
    } else if (crosses_call) {
      for (size_t r = 0; r < sizeof(callee_regs) / sizeof(callee_regs[0]); ++r) {
        if ((int)i >= callee_end[r]) {
          c->value_reg[v] = (int8_t)callee_regs[r];
          callee_end[r] = last_use[v];
          break;
        }
      }
    }
  }
  static const int xmm_regs[] = {4, 5, 6, 7};
  int xmm_end[sizeof(xmm_regs) / sizeof(xmm_regs[0])];
  for (size_t r = 0; r < sizeof(xmm_regs) / sizeof(xmm_regs[0]); ++r)
    xmm_end[r] = -1;
  for (size_t i = 0; nir && i < nir->len; ++i) {
    int v = nir->data[i].dst;
    if (v < 0 || v >= c->value_slots || last_use[v] < (int)i || call_use[v] ||
        !((c->value_f64 && c->value_f64[v]) ||
          (c->value_f32 && c->value_f32[v])))
      continue;
    bool crosses_call = next_call[i + 1] < last_use[v];
    if (crosses_call)
      continue;
    for (size_t r = 0; r < sizeof(xmm_regs) / sizeof(xmm_regs[0]); ++r) {
      if ((int)i >= xmm_end[r]) {
        c->value_xmm[v] = (int8_t)xmm_regs[r];
        xmm_end[r] = last_use[v];
        break;
      }
    }
  }
  free(last_use);
  free(call_use);
  free(next_call);
  return true;
}

static bool ny_x64_obj_immediate_use(const ny_nir_inst_t *in, int operand,
                                     int64_t imm) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  switch (in->op) {
  case NY_NIR_ADD_I64:
  case NY_NIR_AND_I64:
  case NY_NIR_OR_I64:
  case NY_NIR_XOR_I64:
  case NY_NIR_MUL_I64:
    return operand == 0 || operand == 1;
  case NY_NIR_SUB_I64:
    return operand == 1;
  case NY_NIR_SHL_I64:
  case NY_NIR_SAR_I64:
    return operand == 1 && imm >= 1 && imm <= 63;
  case NY_NIR_CMP_I64:
    return operand == 1 ||
           (operand == 0 &&
            (in->cmp == NY_NIR_CMP_EQ || in->cmp == NY_NIR_CMP_NE));
  default:
    return false;
  }
}

/* Constants used exclusively by immediate-capable instructions need neither
 * emitted moves nor stack slots. Prove that property once per function so the
 * emitter stays O(1) at each use and an unexpected load fails explicitly. */
static bool ny_x64_obj_classify_immediates(ny_x64_obj_ctx_t *c,
                                           const ny_nir_func_t *nir) {
  if (!c || !nir || c->value_slots <= 0)
    return true;
  c->value_immediate = calloc((size_t)c->value_slots,
                              sizeof(*c->value_immediate));
  if (!c->value_immediate) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: out of memory classifying immediates");
    return false;
  }
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if (in->op == NY_NIR_CONST_I64 && in->dst >= 0 &&
        in->dst < c->value_slots)
      c->value_immediate[in->dst] = true;
  }
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (int operand = 0; operand < 6; ++operand) {
      int value = operands[operand];
      if (value < 0 || value >= c->value_slots ||
          !c->value_immediate[value])
        continue;
      const ny_nir_inst_t *def = ny_x64_obj_valmap_def(&c->valmap, value);
      if (!def || !ny_x64_obj_immediate_use(in, operand, def->imm))
        c->value_immediate[value] = false;
    }
    for (size_t arg = 0; arg < in->extra_args_len; ++arg) {
      int value = in->extra_args[arg];
      if (value >= 0 && value < c->value_slots)
        c->value_immediate[value] = false;
    }
  }
  return true;
}

static bool ny_x64_obj_layout_spills(ny_x64_obj_ctx_t *c) {
  if (!c || c->value_slots < 0)
    return false;
  if (c->value_slots > 0) {
    c->value_spill = malloc((size_t)c->value_slots * sizeof(*c->value_spill));
    if (!c->value_spill) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 object writer: out of memory laying out spills");
      return false;
    }
  }
  c->spill_slots = 0;
  c->callee_save_slots = 0;
  for (int reg = 0; reg < 16; ++reg)
    c->callee_save_slot[reg] = -1;
  for (int v = 0; v < c->value_slots; ++v) {
    if ((c->value_reg && c->value_reg[v] >= 0) ||
        (c->value_xmm && c->value_xmm[v] >= 0) ||
        (c->value_immediate && c->value_immediate[v])) {
      c->value_spill[v] = -1;
      int reg = c->value_reg ? c->value_reg[v] : -1;
      if ((reg == 3 || (reg >= 12 && reg <= 15)) &&
          c->callee_save_slot[reg] < 0)
        c->callee_save_slot[reg] = c->callee_save_slots++;
    } else {
      c->value_spill[v] = c->spill_slots++;
    }
  }
  c->frame_bytes =
      ny_x64_obj_align(
          (c->spill_slots + c->callee_save_slots + c->local_slots) * 8, 16);
  return true;
}

static int ny_x64_obj_callee_off(const ny_x64_obj_ctx_t *c, int reg) {
  return -8 * (c->spill_slots + c->callee_save_slot[reg] + 1);
}

static bool ny_x64_obj_save_callee(ny_x64_obj_ctx_t *c) {
  for (int reg = 0; reg < 16; ++reg) {
    if (c->callee_save_slot[reg] >= 0 &&
        !ny_x64_obj_store_reg(c, reg, ny_x64_obj_callee_off(c, reg)))
      return false;
  }
  return true;
}

static bool ny_x64_obj_emit_epilogue(ny_x64_obj_ctx_t *c) {
  for (int reg = 15; reg >= 0; --reg) {
    if (c->callee_save_slot[reg] >= 0 &&
        !ny_x64_obj_load_reg(c, reg, ny_x64_obj_callee_off(c, reg)))
      return false;
  }
  return ny_x64_obj_bytes(c, (const unsigned char[]){0xc9, 0xc3}, 2);
}

static bool ny_x64_obj_emit_param_spill(ny_x64_obj_ctx_t *c,
                                        const ny_nir_func_t *nir) {
  if (!c || !nir || c->local_slots <= 0)
    return true;
  int max_local = c->local_slots;
  bool *stored = (bool *)calloc((size_t)max_local, sizeof(bool));
  bool *is_param = (bool *)calloc((size_t)max_local, sizeof(bool));
  if (!stored || !is_param) {
    free(stored);
    free(is_param);
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: OOM param spill");
    return false;
  }
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if (in->op == NY_NIR_STORE_LOCAL && in->imm >= 0 && in->imm < max_local)
      stored[(int)in->imm] = true;
    else if (in->op == NY_NIR_LOAD_LOCAL && in->imm >= 0 &&
             in->imm < max_local && !stored[(int)in->imm])
      is_param[(int)in->imm] = true;
  }
  static const unsigned char gp_reg_code[6] = {7, 6, 2, 1, 0, 1};
  static const bool gp_needs_rex_r[6] = {0, 0, 0, 0, 1, 1};
  int gp = 0;
  int sse = 0;
  int stack = 0;
  for (int i = 0; i < max_local; ++i) {
    if (!is_param[i])
      continue;
    int off = ny_x64_obj_local_off(c, i);
    bool is_f64 = c->local_f64 && i < c->local_slots && c->local_f64[i];
    bool is_f32 = c->local_f32 && i < c->local_slots && c->local_f32[i];
    if ((is_f64 || is_f32) && sse < 8) {
      unsigned char modrm = (unsigned char)(0x85 | (sse << 3));
      unsigned char prefix = is_f32 ? 0xF3 : 0xF2;
      if (!ny_x64_obj_bytes(c, (unsigned char[]){prefix, 0x0F, 0x11, modrm}, 4) ||
          !ny_x64_obj_i32(c, off)) {
        free(stored);
        free(is_param);
        return false;
      }
      sse++;
    } else if (!is_f64 && !is_f32 && gp < 6) {
      unsigned char rex = gp_needs_rex_r[gp] ? 0x4C : 0x48;
      unsigned char modrm = (unsigned char)(0x85 | (gp_reg_code[gp] << 3));
      if (!ny_x64_obj_bytes(c, &rex, 1) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x89, modrm}, 2) ||
          !ny_x64_obj_i32(c, off)) {
        free(stored);
        free(is_param);
        return false;
      }
      gp++;
    } else {
      int src_off = 16 + (int)c->target->shadow_space_bytes + stack * 8;
      if (!ny_x64_obj_load_rax(c, src_off) ||
          !ny_x64_obj_store_rax(c, off)) {
        free(stored);
        free(is_param);
        return false;
      }
      stack++;
    }
  }
  free(stored);
  free(is_param);
  return true;
}

bool ny_x64_obj_emit_code(ny_x64_obj_ctx_t *c, const ny_nir_func_t *nir,
                                 bool tag_return) {
  c->nir = nir;
  ny_x64_obj_valmap_init(&c->valmap, nir);
  ny_x64_obj_scan_frame(c, nir);
  if (!ny_x64_obj_classify_values(c, nir))
    return false;
  if (!ny_x64_obj_classify_immediates(c, nir))
    return false;
  if (!ny_x64_obj_allocate_registers(c, nir))
    return false;
  if (!ny_x64_obj_layout_spills(c))
    return false;
  if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x55, 0x48, 0x89, 0xe5}, 4))
    return false;
  if (c->frame_bytes > 0) {
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xec}, 3) ||
        !ny_x64_obj_i32(c, c->frame_bytes))
      return false;
  }
  if (!ny_x64_obj_save_callee(c))
    return false;
  if (!ny_x64_obj_emit_param_spill(c, nir))
    return false;
  for (size_t i = 0; nir && i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if (tag_return && in->op == NY_NIR_RET && in->a >= 0) {
      if (c->value_f64 && in->a < c->value_slots && c->value_f64[in->a]) {
        if (!ny_x64_obj_load_value_xmm(c, in->a, 0) ||
            !ny_x64_obj_emit_epilogue(c))
          return false;
        continue;
      }
      if (!ny_x64_obj_load_value_rax(c, in->a) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x8d, 0x04, 0x45,
                                                       0x01, 0x00, 0x00,
                                                       0x00},
                            8) ||
          !ny_x64_obj_emit_epilogue(c))
        return false;
      continue;
    }
    if (!ny_x64_obj_emit_inst(c, in))
      return false;
  }
  if (nir && (nir->len == 0 || nir->data[nir->len - 1].op != NY_NIR_RET)) {
    if (!ny_x64_obj_mov_rax_imm(c, 0) ||
        !ny_x64_obj_emit_epilogue(c))
      return false;
  }
  return ny_x64_obj_patch_branches(c);
}
