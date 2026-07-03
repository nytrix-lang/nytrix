#include "code/native/internal.h"

#include <errno.h>
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

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} ny_obj_buf_t;

typedef struct {
  int64_t label;
  size_t off;
} ny_x64_obj_label_t;

typedef struct {
  int64_t label;
  size_t disp_off;
} ny_x64_obj_patch_t;

typedef struct {
  char symbol[256];
  size_t disp_off;
} ny_x64_obj_reloc_t;

typedef struct {
  char name[256];
  size_t off;
  size_t size;
} ny_x64_obj_symbol_def_t;

typedef struct {
  ny_obj_buf_t code;
  const ny_native_target_info_t *target;
  int value_slots;
  int local_slots;
  int frame_bytes;
  bool *value_f64;
  bool *value_f32;
  bool *local_f64;
  bool *local_f32;
  ny_x64_obj_label_t labels[256];
  size_t label_count;
  ny_x64_obj_patch_t patches[256];
  size_t patch_count;
  ny_x64_obj_reloc_t relocs[256];
  size_t reloc_count;
  char *err;
  size_t err_len;
} ny_x64_obj_ctx_t;

static void ny_obj_free(ny_obj_buf_t *b) {
  if (!b)
    return;
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

static void ny_x64_obj_ctx_free(ny_x64_obj_ctx_t *c) {
  if (!c)
    return;
  ny_obj_free(&c->code);
  free(c->value_f64);
  free(c->value_f32);
  free(c->local_f64);
  free(c->local_f32);
  c->value_f64 = NULL;
  c->value_f32 = NULL;
  c->local_f64 = NULL;
  c->local_f32 = NULL;
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

static bool ny_obj_emit(ny_obj_buf_t *b, const void *data, size_t len) {
  if (!ny_obj_reserve(b, len))
    return false;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return true;
}

static bool ny_obj_u8(ny_obj_buf_t *b, unsigned v) {
  unsigned char c = (unsigned char)v;
  return ny_obj_emit(b, &c, 1);
}

static bool ny_obj_pad_to(ny_obj_buf_t *b, size_t align) {
  if (align == 0)
    return true;
  while ((b->len % align) != 0) {
    if (!ny_obj_u8(b, 0))
      return false;
  }
  return true;
}

static bool ny_obj_zero(ny_obj_buf_t *b, size_t len) {
  if (!ny_obj_reserve(b, len))
    return false;
  memset(b->data + b->len, 0, len);
  b->len += len;
  return true;
}

static bool ny_obj_u16(ny_obj_buf_t *b, uint16_t v) {
  unsigned char c[2] = {(unsigned char)(v & 0xff), (unsigned char)(v >> 8)};
  return ny_obj_emit(b, c, sizeof(c));
}

static bool ny_obj_u32(ny_obj_buf_t *b, uint32_t v) {
  unsigned char c[4] = {(unsigned char)(v & 0xff),
                        (unsigned char)((v >> 8) & 0xff),
                        (unsigned char)((v >> 16) & 0xff),
                        (unsigned char)((v >> 24) & 0xff)};
  return ny_obj_emit(b, c, sizeof(c));
}

static bool ny_obj_u64(ny_obj_buf_t *b, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    if (!ny_obj_u8(b, (unsigned)((v >> (i * 8)) & 0xff)))
      return false;
  }
  return true;
}

static void ny_obj_patch_u16(ny_obj_buf_t *b, size_t off, uint16_t v) {
  b->data[off + 0] = (unsigned char)(v & 0xff);
  b->data[off + 1] = (unsigned char)(v >> 8);
}

static void ny_obj_patch_u32(ny_obj_buf_t *b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

static void ny_obj_patch_u64(ny_obj_buf_t *b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

static bool ny_elf32_write_sym(ny_obj_buf_t *b, uint32_t name, uint32_t value,
                               uint32_t size, unsigned char info,
                               uint16_t shndx) {
  return ny_obj_u32(b, name) && ny_obj_u32(b, value) && ny_obj_u32(b, size) &&
         ny_obj_u8(b, info) && ny_obj_u8(b, 0) && ny_obj_u16(b, shndx);
}

static bool ny_elf32_write_sh(ny_obj_buf_t *b, uint32_t name, uint32_t type,
                              uint32_t flags, uint32_t off, uint32_t size,
                              uint32_t link, uint32_t info,
                              uint32_t addralign, uint32_t entsize) {
  return ny_obj_u32(b, name) && ny_obj_u32(b, type) && ny_obj_u32(b, flags) &&
         ny_obj_u32(b, 0) && ny_obj_u32(b, off) && ny_obj_u32(b, size) &&
         ny_obj_u32(b, link) && ny_obj_u32(b, info) &&
         ny_obj_u32(b, addralign) && ny_obj_u32(b, entsize);
}

typedef struct {
  int64_t label;
  size_t off;
} ny_i386_obj_label_t;

typedef struct {
  int64_t label;
  size_t disp_off;
} ny_i386_obj_patch_t;

typedef struct {
  char symbol[256];
  size_t disp_off;
} ny_i386_obj_reloc_t;

typedef struct {
  char name[256];
  size_t off;
  size_t size;
} ny_i386_obj_symbol_def_t;

typedef struct {
  ny_obj_buf_t code;
  const ny_native_target_info_t *target;
  int value_slots;
  int local_slots;
  int frame_bytes;
  int local_base;
  int max_local_slot;
  bool *value_f64;
  bool *value_f32;
  bool *local_f64;
  bool *local_f32;
  ny_i386_obj_label_t labels[256];
  size_t label_count;
  ny_i386_obj_patch_t patches[256];
  size_t patch_count;
  size_t epilogue_patches[256];
  size_t epilogue_patch_count;
  ny_i386_obj_reloc_t relocs[256];
  size_t reloc_count;
  char *err;
  size_t err_len;
} ny_i386_obj_ctx_t;

static void ny_i386_obj_ctx_free(ny_i386_obj_ctx_t *c) {
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

static int ny_i386_obj_symbol_index(char symbols[][256], size_t count,
                                    const char *name) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(symbols[i], name) == 0)
      return (int)i;
  }
  return -1;
}

static int ny_i386_obj_def_index(const ny_i386_obj_symbol_def_t *defs,
                                 size_t def_count, const char *name) {
  for (size_t i = 0; defs && i < def_count; ++i) {
    if (strcmp(defs[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

static bool ny_i386_obj_collect_external_reloc_symbols(
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
        if ((in->flags & NYIR_INST_F_RET_F32) != 0)
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

static bool ny_i386_obj_emit_code(ny_i386_obj_ctx_t *c, const ny_nir_func_t *nir,
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

static int ny_x64_obj_value_off(int value) {
  return -8 * (value + 1);
}

static int ny_x64_obj_local_off(const ny_x64_obj_ctx_t *c, int local) {
  return -8 * (c->value_slots + local + 1);
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

static bool ny_x64_obj_load_value_rax(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_x64_obj_load_rax(c, ny_x64_obj_value_off(value));
}

static bool ny_x64_obj_load_value_r10(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_x64_obj_load_r10(c, ny_x64_obj_value_off(value));
}

static bool ny_x64_obj_load_value_rcx(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_x64_obj_load_rcx(c, ny_x64_obj_value_off(value));
}

static bool ny_x64_obj_store_value_rax(ny_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid destination v%d",
                      value);
    return false;
  }
  return ny_x64_obj_store_rax(c, ny_x64_obj_value_off(value));
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

static bool ny_x64_obj_load_value_xmm(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_x64_obj_load_xmm(c, ny_x64_obj_value_off(value), xmm);
}

static bool ny_x64_obj_load_value_xmm_f32(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_x64_obj_load_xmm_f32(c, ny_x64_obj_value_off(value), xmm);
}

static bool ny_x64_obj_store_value_xmm_f32(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid destination v%d",
                      value);
    return false;
  }
  return ny_x64_obj_store_xmm_f32(c, ny_x64_obj_value_off(value), xmm);
}

static bool ny_x64_obj_store_value_xmm(ny_x64_obj_ctx_t *c, int value, int xmm) {
  if (value < 0 || value >= c->value_slots) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid destination v%d",
                      value);
    return false;
  }
  return ny_x64_obj_store_xmm(c, ny_x64_obj_value_off(value), xmm);
}

static bool ny_x64_obj_binop(ny_x64_obj_ctx_t *c, const ny_nir_inst_t *in,
                             const unsigned char *op, size_t op_len) {
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
                                 size_t disp_off) {
  if (c->reloc_count >= sizeof(c->relocs) / sizeof(c->relocs[0])) {
    ny_native_set_err(c->err, c->err_len,
                      "x86-64 object writer: too many relocations");
    return false;
  }
  ny_x64_obj_reloc_t *r = &c->relocs[c->reloc_count++];
  snprintf(r->symbol, sizeof(r->symbol), "%s", symbol ? symbol : "");
  r->disp_off = disp_off;
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


static bool ny_x64_obj_emit_code(ny_x64_obj_ctx_t *c, const ny_nir_func_t *nir,
                                 bool tag_return);

static int ny_x64_obj_symbol_index(char symbols[][256], size_t count,
                                   const char *name) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(symbols[i], name) == 0)
      return (int)i;
  }
  return -1;
}

static int ny_x64_obj_def_index(const ny_x64_obj_symbol_def_t *defs,
                                size_t def_count, const char *name) {
  for (size_t i = 0; defs && i < def_count; ++i) {
    if (strcmp(defs[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

static bool ny_x64_obj_collect_external_reloc_symbols(
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

static bool ny_x64_obj_append_function(ny_obj_buf_t *code,
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

static bool ny_x64_obj_collect_reloc_symbols(const ny_x64_obj_reloc_t *relocs,
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
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
    return ny_x64_obj_mov_rax_imm(c, in->imm) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NY_NIR_COPY:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NY_NIR_ADD_I64:
    return ny_x64_obj_binop(c, in, add, sizeof(add));
  case NY_NIR_SUB_I64:
    return ny_x64_obj_binop(c, in, sub, sizeof(sub));
  case NY_NIR_MUL_I64:
    return ny_x64_obj_binop(c, in, imul, sizeof(imul));
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
  case NY_NIR_SAR_I64:
    if (!ny_x64_obj_load_value_rax(c, in->a) ||
        !ny_x64_obj_load_value_rcx(c, in->b))
      return false;
    if (in->op == NY_NIR_SHL_I64) {
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xd3, 0xe0}, 3))
        return false;
    } else if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xd3, 0xf8},
                                 3))
      return false;
    return ny_x64_obj_store_value_rax(c, in->dst);
  case NY_NIR_CMP_I64:
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
    return ny_x64_obj_load_rax(c, ny_x64_obj_local_off(c, (int)in->imm)) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_ADDR_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: invalid local slot %lld",
                        (long long)in->imm);
      return false;
    }
    return ny_x64_obj_lea_rax(c, ny_x64_obj_local_off(c, (int)in->imm)) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NY_NIR_STORE_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      ny_native_set_err(c->err, c->err_len,
                        "x86-64 ELF object writer: invalid local slot %lld",
                        (long long)in->imm);
      return false;
    }
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
    return ny_x64_obj_bytes(c, (const unsigned char[]){0xc9, 0xc3}, 2);
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
    int gp = 0;
    int sse = 0;
    int stack_argc = 0;
    for (int i = 0; i < argc; ++i) {
      gp_index[i] = -1;
      sse_index[i] = -1;
      if (args[i] < 0) {
        ny_native_set_err(c->err, c->err_len,
                          "x86-64 object writer: invalid call arg");
        return false;
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
      if (gp_index[i] >= 0 || sse_index[i] >= 0)
        continue;
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
      if (sse_index[i] >= 0) {
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
    if (!ny_x64_obj_i32(c, 0) || !ny_x64_obj_add_reloc(c, symbol, disp))
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
         ((in->flags & NYIR_INST_F_RET_F32) &&
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

static bool ny_x64_obj_emit_code(ny_x64_obj_ctx_t *c, const ny_nir_func_t *nir,
                                 bool tag_return) {
  ny_x64_obj_scan_frame(c, nir);
  if (!ny_x64_obj_classify_values(c, nir))
    return false;
  if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x55, 0x48, 0x89, 0xe5}, 4))
    return false;
  if (c->frame_bytes > 0) {
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xec}, 3) ||
        !ny_x64_obj_i32(c, c->frame_bytes))
      return false;
  }
  if (!ny_x64_obj_emit_param_spill(c, nir))
    return false;
  for (size_t i = 0; nir && i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    if (tag_return && in->op == NY_NIR_RET && in->a >= 0) {
      if (c->value_f64 && in->a < c->value_slots && c->value_f64[in->a]) {
        if (!ny_x64_obj_load_value_xmm(c, in->a, 0) ||
            !ny_x64_obj_bytes(c, (const unsigned char[]){0xc9, 0xc3}, 2))
          return false;
        continue;
      }
      if (!ny_x64_obj_load_value_rax(c, in->a) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x8d, 0x04, 0x45,
                                                       0x01, 0x00, 0x00,
                                                       0x00},
                            8) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0xc9, 0xc3}, 2))
        return false;
      continue;
    }
    if (!ny_x64_obj_emit_inst(c, in))
      return false;
  }
  if (nir && (nir->len == 0 || nir->data[nir->len - 1].op != NY_NIR_RET)) {
    if (!ny_x64_obj_mov_rax_imm(c, 0) ||
        !ny_x64_obj_bytes(c, (const unsigned char[]){0xc9, 0xc3}, 2))
      return false;
  }
  return ny_x64_obj_patch_branches(c);
}

static bool ny_elf64_write_sym(ny_obj_buf_t *b, uint32_t name, unsigned info,
                               uint16_t shndx, uint64_t value,
                               uint64_t size) {
  return ny_obj_u32(b, name) && ny_obj_u8(b, info) && ny_obj_u8(b, 0) &&
         ny_obj_u16(b, shndx) && ny_obj_u64(b, value) && ny_obj_u64(b, size);
}

static bool ny_elf64_write_sh(ny_obj_buf_t *b, uint32_t name, uint32_t type,
                              uint64_t flags, uint64_t offset, uint64_t size,
                              uint32_t link, uint32_t info, uint64_t align,
                              uint64_t entsize) {
  return ny_obj_u32(b, name) && ny_obj_u32(b, type) && ny_obj_u64(b, flags) &&
         ny_obj_u64(b, 0) && ny_obj_u64(b, offset) && ny_obj_u64(b, size) &&
         ny_obj_u32(b, link) && ny_obj_u32(b, info) && ny_obj_u64(b, align) &&
         ny_obj_u64(b, entsize);
}

static bool ny_elf64_write_file(const char *path, const unsigned char *data,
                                size_t len, char *err, size_t err_len) {
  FILE *out = fopen(path, "wb");
  if (!out) {
    ny_native_set_err(err, err_len,
                      "x86-64 ELF object writer: cannot open %s: %s", path,
                      strerror(errno));
    return false;
  }
  bool ok = fwrite(data, 1, len, out) == len;
  if (fclose(out) != 0)
    ok = false;
  if (!ok)
    ny_native_set_err(err, err_len,
                      "x86-64 ELF object writer: failed writing %s", path);
  return ok;
}


static bool ny_obj_sym_name8_or_str(ny_obj_buf_t *b, const char *name,
                                    uint32_t str_off) {
  char fixed[8] = {0};
  size_t n = name ? strlen(name) : 0;
  if (n <= 8) {
    memcpy(fixed, name, n);
    return ny_obj_emit(b, fixed, sizeof(fixed));
  }
  return ny_obj_u32(b, 0) && ny_obj_u32(b, str_off);
}

static bool ny_coff_write_sym(ny_obj_buf_t *b, const char *name,
                              uint32_t long_name_off, uint32_t value,
                              int16_t section, uint16_t type,
                              unsigned storage_class) {
  return ny_obj_sym_name8_or_str(b, name, long_name_off) && ny_obj_u32(b, value) &&
         ny_obj_u16(b, (uint16_t)section) && ny_obj_u16(b, type) &&
         ny_obj_u8(b, storage_class) && ny_obj_u8(b, 0);
}

static bool ny_native_emit_coff_x64_object_code(
    const unsigned char *code, size_t code_len,
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count, const char *path,
    const char *symbol_name, char *err, size_t err_len) {
  if (!code || !path || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len, "x86-64 COFF object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strings = {0};
  bool ok = false;
  char reloc_symbols[256][256];
  size_t reloc_symbol_count = 0;
  uint32_t def_name_off = 0;
  uint32_t reloc_name_offs[256] = {0};
  const size_t header_size = 20;
  const size_t section_count = 1;
  const size_t section_table_size = 40 * section_count;
  const size_t text_off = header_size + section_table_size;
  const size_t reloc_off = text_off + code_len;
  const size_t reloc_size = reloc_count * 10;
  const size_t symtab_off = reloc_off + reloc_size;

  if (!ny_x64_obj_collect_reloc_symbols(relocs, reloc_count, reloc_symbols,
                                        &reloc_symbol_count, err, err_len))
    goto done;
  if (!ny_obj_u32(&strings, 0))
    goto done;
  if (strlen(symbol_name) > 8) {
    def_name_off = (uint32_t)strings.len;
    if (!ny_obj_emit(&strings, symbol_name, strlen(symbol_name) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (strlen(reloc_symbols[i]) > 8) {
      reloc_name_offs[i] = (uint32_t)strings.len;
      if (!ny_obj_emit(&strings, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
        goto done;
    }
  }
  ny_obj_patch_u32(&strings, 0, (uint32_t)strings.len);

  uint32_t nsyms = (uint32_t)(2 + reloc_symbol_count);
  if (!ny_obj_u16(&file, 0x8664) ||       /* IMAGE_FILE_MACHINE_AMD64 */
      !ny_obj_u16(&file, (uint16_t)section_count) || !ny_obj_u32(&file, 0) ||
      !ny_obj_u32(&file, (uint32_t)symtab_off) || !ny_obj_u32(&file, nsyms) ||
      !ny_obj_u16(&file, 0) || !ny_obj_u16(&file, 0))
    goto done;

  char sec_name[8] = {0};
  memcpy(sec_name, ".text", 5);
  if (!ny_obj_emit(&file, sec_name, sizeof(sec_name)) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0) ||
      !ny_obj_u32(&file, (uint32_t)code_len) ||
      !ny_obj_u32(&file, (uint32_t)text_off) ||
      !ny_obj_u32(&file, (uint32_t)(reloc_count ? reloc_off : 0)) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u16(&file, (uint16_t)reloc_count) ||
      !ny_obj_u16(&file, 0) ||
      !ny_obj_u32(&file, 0x60500020u)) /* code | execute | read | align16 */
    goto done;

  if (!ny_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int sym_i = ny_x64_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                        relocs[i].symbol);
    if (sym_i < 0)
      goto done;
    if (!ny_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !ny_obj_u32(&file, (uint32_t)(2 + sym_i)) ||
        !ny_obj_u16(&file, 0x0004)) /* IMAGE_REL_AMD64_REL32 */
      goto done;
  }
  if (!ny_coff_write_sym(&file, ".text", 0, 0, 1, 0, 3) ||
      !ny_coff_write_sym(&file, symbol_name, def_name_off, 0, 1, 0x20, 2))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_coff_write_sym(&file, reloc_symbols[i], reloc_name_offs[i], 0, 0,
                           0x20, 2))
      goto done;
  }
  if (!ny_obj_emit(&file, strings.data, strings.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  ny_obj_free(&strings);
  ny_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "x86-64 COFF object writer failed");
  return ok;
}

static bool ny_macho_write_padded_name(ny_obj_buf_t *b, const char *name) {
  char out[16] = {0};
  if (name)
    snprintf(out, sizeof(out), "%s", name);
  return ny_obj_emit(b, out, sizeof(out));
}

static bool ny_native_emit_macho_x64_object_code(
    const unsigned char *code, size_t code_len,
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count, const char *path,
    const char *symbol_name, char *err, size_t err_len) {
  if (!code || !path || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  bool ok = false;
  char reloc_symbols[256][256];
  size_t reloc_symbol_count = 0;
  uint32_t reloc_name_offs[256] = {0};
  char def_name[256];
  snprintf(def_name, sizeof(def_name), "%s%s", symbol_name[0] == '_' ? "" : "_",
           symbol_name);

  if (!ny_x64_obj_collect_reloc_symbols(relocs, reloc_count, reloc_symbols,
                                        &reloc_symbol_count, err, err_len))
    goto done;
  if (!ny_obj_u8(&strtab, 0))
    goto done;
  uint32_t def_name_off = (uint32_t)strtab.len;
  if (!ny_obj_emit(&strtab, def_name, strlen(def_name) + 1))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
      goto done;
  }

  const uint32_t seg_cmdsize = 72 + 80;
  const uint32_t sym_cmdsize = 24;
  const uint32_t sizeofcmds = seg_cmdsize + sym_cmdsize;
  const uint32_t text_off = 32 + sizeofcmds;
  const uint32_t reloc_off = text_off + (uint32_t)code_len;
  const uint32_t symoff = reloc_off + (uint32_t)(reloc_count * 8);
  const uint32_t nsyms = (uint32_t)(1 + reloc_symbol_count);
  const uint32_t stroff = symoff + nsyms * 16;
  const uint32_t strsize = (uint32_t)strtab.len;

  if (!ny_obj_u32(&file, 0xfeedfacf) || !ny_obj_u32(&file, 0x01000007) ||
      !ny_obj_u32(&file, 3) || !ny_obj_u32(&file, 1) ||
      !ny_obj_u32(&file, 2) || !ny_obj_u32(&file, sizeofcmds) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0))
    goto done;

  if (!ny_obj_u32(&file, 0x19) || !ny_obj_u32(&file, seg_cmdsize) ||
      !ny_macho_write_padded_name(&file, "") || !ny_obj_u64(&file, 0) ||
      !ny_obj_u64(&file, code_len) || !ny_obj_u64(&file, text_off) ||
      !ny_obj_u64(&file, code_len) || !ny_obj_u32(&file, 7) ||
      !ny_obj_u32(&file, 5) || !ny_obj_u32(&file, 1) || !ny_obj_u32(&file, 0))
    goto done;
  if (!ny_macho_write_padded_name(&file, "__text") ||
      !ny_macho_write_padded_name(&file, "__TEXT") || !ny_obj_u64(&file, 0) ||
      !ny_obj_u64(&file, code_len) || !ny_obj_u32(&file, text_off) ||
      !ny_obj_u32(&file, 4) || !ny_obj_u32(&file, reloc_count ? reloc_off : 0) ||
      !ny_obj_u32(&file, (uint32_t)reloc_count) || !ny_obj_u32(&file, 0x80000400u) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0))
    goto done;

  if (!ny_obj_u32(&file, 0x2) || !ny_obj_u32(&file, sym_cmdsize) ||
      !ny_obj_u32(&file, symoff) || !ny_obj_u32(&file, nsyms) ||
      !ny_obj_u32(&file, stroff) || !ny_obj_u32(&file, strsize))
    goto done;
  if (!ny_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int sym_i = ny_x64_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                        relocs[i].symbol);
    if (sym_i < 0)
      goto done;
    uint32_t word = (uint32_t)(1 + sym_i) | (1u << 24) | (2u << 25) |
                    (1u << 27) | (2u << 28); /* pcrel long extern branch */
    if (!ny_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !ny_obj_u32(&file, word))
      goto done;
  }
  if (!ny_obj_u32(&file, def_name_off) || !ny_obj_u8(&file, 0x0f) ||
      !ny_obj_u8(&file, 1) || !ny_obj_u16(&file, 0) || !ny_obj_u64(&file, 0))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_obj_u32(&file, reloc_name_offs[i]) || !ny_obj_u8(&file, 0x01) ||
        !ny_obj_u8(&file, 0) || !ny_obj_u16(&file, 0) || !ny_obj_u64(&file, 0))
      goto done;
  }
  if (!ny_obj_emit(&file, strtab.data, strtab.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  ny_obj_free(&strtab);
  ny_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer failed");
  return ok;
}


static bool ny_native_emit_elf64_x64_object_bundle_code(
    const unsigned char *code, size_t code_len,
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count,
    const ny_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    ny_native_set_err(err, err_len, "x86-64 ELF object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  bool ok = false;
  char reloc_symbols[256][256];
  size_t reloc_symbol_count = 0;
  uint32_t def_name_offs[256] = {0};
  uint32_t reloc_name_offs[256] = {0};
  if (!ny_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;

  const char shstr[] = "\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0";
  const uint32_t sh_text = 1;
  const uint32_t sh_rela_text = 7;
  const uint32_t sh_symtab = 18;
  const uint32_t sh_strtab = 26;
  const uint32_t sh_shstrtab = 34;
  if (!ny_obj_u8(&strtab, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    def_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, defs[i].name, strlen(defs[i].name) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
      goto done;
  }

  if (!ny_obj_zero(&file, 64) || !ny_obj_pad_to(&file, 16))
    goto done;
  size_t text_off = file.len;
  if (!ny_obj_emit(&file, code, code_len) || !ny_obj_pad_to(&file, 8))
    goto done;
  size_t rela_off = file.len;
  for (size_t i = 0; i < reloc_count; ++i) {
    int def_i = ny_x64_obj_def_index(defs, def_count, relocs[i].symbol);
    uint32_t sym_index = 0;
    if (def_i >= 0) {
      sym_index = (uint32_t)(1 + def_i);
    } else {
      int ext_i = ny_x64_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                          relocs[i].symbol);
      if (ext_i < 0)
        goto done;
      sym_index = (uint32_t)(1 + def_count + (size_t)ext_i);
    }
    uint64_t info = ((uint64_t)sym_index << 32) | 4u; /* R_X86_64_PLT32 */
    if (!ny_obj_u64(&file, relocs[i].disp_off) || !ny_obj_u64(&file, info) ||
        !ny_obj_u64(&file, (uint64_t)-4LL))
      goto done;
  }
  size_t rela_size = file.len - rela_off;
  size_t symtab_off = file.len;
  if (!ny_elf64_write_sym(&file, 0, 0, 0, 0, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_elf64_write_sym(&file, def_name_offs[i], 0x12, 1, defs[i].off,
                            defs[i].size))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_elf64_write_sym(&file, reloc_name_offs[i], 0x12, 0, 0, 0))
      goto done;
  }
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!ny_obj_emit(&file, strtab.data, strtab.len))
    goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstrtab_off = file.len;
  if (!ny_obj_emit(&file, shstr, sizeof(shstr)) || !ny_obj_pad_to(&file, 8))
    goto done;
  size_t shstrtab_size = sizeof(shstr);
  size_t shoff = file.len;
  if (!ny_elf64_write_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !ny_elf64_write_sh(&file, sh_text, 1, 0x6, text_off, code_len, 0, 0,
                         16, 0) ||
      !ny_elf64_write_sh(&file, sh_rela_text, 4, 0, rela_off, rela_size, 3, 1,
                         8, 24) ||
      !ny_elf64_write_sh(&file, sh_symtab, 2, 0, symtab_off, symtab_size, 4,
                         1, 8, 24) ||
      !ny_elf64_write_sh(&file, sh_strtab, 3, 0, strtab_off, strtab_size, 0, 0,
                         1, 0) ||
      !ny_elf64_write_sh(&file, sh_shstrtab, 3, 0, shstrtab_off, shstrtab_size,
                         0, 0, 1, 0))
    goto done;

  file.data[0] = 0x7f;
  file.data[1] = 'E';
  file.data[2] = 'L';
  file.data[3] = 'F';
  file.data[4] = 2;
  file.data[5] = 1;
  file.data[6] = 1;
  ny_obj_patch_u16(&file, 16, 1);
  ny_obj_patch_u16(&file, 18, 62);
  ny_obj_patch_u32(&file, 20, 1);
  ny_obj_patch_u64(&file, 40, shoff);
  ny_obj_patch_u16(&file, 52, 64);
  ny_obj_patch_u16(&file, 58, 64);
  ny_obj_patch_u16(&file, 60, 6);
  ny_obj_patch_u16(&file, 62, 5);

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  ny_obj_free(&strtab);
  ny_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "x86-64 ELF object writer failed");
  return ok;
}

static bool ny_native_emit_coff_x64_object_bundle_code(
    const unsigned char *code, size_t code_len,
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count,
    const ny_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    ny_native_set_err(err, err_len, "x86-64 COFF object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strings = {0};
  bool ok = false;
  char reloc_symbols[256][256];
  size_t reloc_symbol_count = 0;
  uint32_t def_name_offs[256] = {0};
  uint32_t reloc_name_offs[256] = {0};
  const size_t header_size = 20;
  const size_t section_count = 1;
  const size_t section_table_size = 40 * section_count;
  const size_t text_off = header_size + section_table_size;
  const size_t reloc_off = text_off + code_len;
  const size_t reloc_size = reloc_count * 10;
  const size_t symtab_off = reloc_off + reloc_size;

  if (!ny_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;
  if (!ny_obj_u32(&strings, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (strlen(defs[i].name) > 8) {
      def_name_offs[i] = (uint32_t)strings.len;
      if (!ny_obj_emit(&strings, defs[i].name, strlen(defs[i].name) + 1))
        goto done;
    }
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (strlen(reloc_symbols[i]) > 8) {
      reloc_name_offs[i] = (uint32_t)strings.len;
      if (!ny_obj_emit(&strings, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
        goto done;
    }
  }
  ny_obj_patch_u32(&strings, 0, (uint32_t)strings.len);

  uint32_t nsyms = (uint32_t)(1 + def_count + reloc_symbol_count);
  if (!ny_obj_u16(&file, 0x8664) || !ny_obj_u16(&file, (uint16_t)section_count) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, (uint32_t)symtab_off) ||
      !ny_obj_u32(&file, nsyms) || !ny_obj_u16(&file, 0) || !ny_obj_u16(&file, 0))
    goto done;

  char sec_name[8] = {0};
  memcpy(sec_name, ".text", 5);
  if (!ny_obj_emit(&file, sec_name, sizeof(sec_name)) || !ny_obj_u32(&file, 0) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, (uint32_t)code_len) ||
      !ny_obj_u32(&file, (uint32_t)text_off) ||
      !ny_obj_u32(&file, (uint32_t)(reloc_count ? reloc_off : 0)) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u16(&file, (uint16_t)reloc_count) ||
      !ny_obj_u16(&file, 0) || !ny_obj_u32(&file, 0x60500020u))
    goto done;

  if (!ny_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int def_i = ny_x64_obj_def_index(defs, def_count, relocs[i].symbol);
    uint32_t sym_index = 0;
    if (def_i >= 0) {
      sym_index = (uint32_t)(1 + def_i);
    } else {
      int ext_i = ny_x64_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                          relocs[i].symbol);
      if (ext_i < 0)
        goto done;
      sym_index = (uint32_t)(1 + def_count + (size_t)ext_i);
    }
    if (!ny_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !ny_obj_u32(&file, sym_index) || !ny_obj_u16(&file, 0x0004))
      goto done;
  }
  if (!ny_coff_write_sym(&file, ".text", 0, 0, 1, 0, 3))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_coff_write_sym(&file, defs[i].name, def_name_offs[i],
                           (uint32_t)defs[i].off, 1, 0x20, 2))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_coff_write_sym(&file, reloc_symbols[i], reloc_name_offs[i], 0, 0,
                           0x20, 2))
      goto done;
  }
  if (!ny_obj_emit(&file, strings.data, strings.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  ny_obj_free(&strings);
  ny_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "x86-64 COFF object writer failed");
  return ok;
}

static bool ny_macho_symbol_name(char *out, size_t out_len, const char *name) {
  if (!out || out_len == 0 || !name || !name[0])
    return false;
  int n = snprintf(out, out_len, "%s%s", name[0] == '_' ? "" : "_", name);
  return n > 0 && (size_t)n < out_len;
}

static bool ny_native_emit_macho_x64_object_bundle_code(
    const unsigned char *code, size_t code_len,
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count,
    const ny_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  bool ok = false;
  char reloc_symbols[256][256];
  size_t reloc_symbol_count = 0;
  uint32_t def_name_offs[256] = {0};
  uint32_t reloc_name_offs[256] = {0};
  char macho_defs[256][256];
  char macho_relocs[256][256];

  if (!ny_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;
  if (!ny_obj_u8(&strtab, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_macho_symbol_name(macho_defs[i], sizeof(macho_defs[i]), defs[i].name))
      goto done;
    def_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, macho_defs[i], strlen(macho_defs[i]) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_macho_symbol_name(macho_relocs[i], sizeof(macho_relocs[i]),
                              reloc_symbols[i]))
      goto done;
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, macho_relocs[i], strlen(macho_relocs[i]) + 1))
      goto done;
  }

  const uint32_t seg_cmdsize = 72 + 80;
  const uint32_t sym_cmdsize = 24;
  const uint32_t sizeofcmds = seg_cmdsize + sym_cmdsize;
  const uint32_t text_off = 32 + sizeofcmds;
  const uint32_t reloc_off = text_off + (uint32_t)code_len;
  const uint32_t symoff = reloc_off + (uint32_t)(reloc_count * 8);
  const uint32_t nsyms = (uint32_t)(def_count + reloc_symbol_count);
  const uint32_t stroff = symoff + nsyms * 16;
  const uint32_t strsize = (uint32_t)strtab.len;

  if (!ny_obj_u32(&file, 0xfeedfacf) || !ny_obj_u32(&file, 0x01000007) ||
      !ny_obj_u32(&file, 3) || !ny_obj_u32(&file, 1) ||
      !ny_obj_u32(&file, 2) || !ny_obj_u32(&file, sizeofcmds) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0))
    goto done;
  if (!ny_obj_u32(&file, 0x19) || !ny_obj_u32(&file, seg_cmdsize) ||
      !ny_macho_write_padded_name(&file, "") || !ny_obj_u64(&file, 0) ||
      !ny_obj_u64(&file, code_len) || !ny_obj_u64(&file, text_off) ||
      !ny_obj_u64(&file, code_len) || !ny_obj_u32(&file, 7) ||
      !ny_obj_u32(&file, 5) || !ny_obj_u32(&file, 1) || !ny_obj_u32(&file, 0))
    goto done;
  if (!ny_macho_write_padded_name(&file, "__text") ||
      !ny_macho_write_padded_name(&file, "__TEXT") || !ny_obj_u64(&file, 0) ||
      !ny_obj_u64(&file, code_len) || !ny_obj_u32(&file, text_off) ||
      !ny_obj_u32(&file, 4) || !ny_obj_u32(&file, reloc_count ? reloc_off : 0) ||
      !ny_obj_u32(&file, (uint32_t)reloc_count) || !ny_obj_u32(&file, 0x80000400u) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0))
    goto done;
  if (!ny_obj_u32(&file, 0x2) || !ny_obj_u32(&file, sym_cmdsize) ||
      !ny_obj_u32(&file, symoff) || !ny_obj_u32(&file, nsyms) ||
      !ny_obj_u32(&file, stroff) || !ny_obj_u32(&file, strsize))
    goto done;
  if (!ny_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int def_i = ny_x64_obj_def_index(defs, def_count, relocs[i].symbol);
    uint32_t sym_index = 0;
    if (def_i >= 0) {
      sym_index = (uint32_t)def_i;
    } else {
      int ext_i = ny_x64_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                          relocs[i].symbol);
      if (ext_i < 0)
        goto done;
      sym_index = (uint32_t)(def_count + (size_t)ext_i);
    }
    uint32_t word = sym_index | (1u << 24) | (2u << 25) |
                    (1u << 27) | (2u << 28); /* pcrel long extern branch */
    if (!ny_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !ny_obj_u32(&file, word))
      goto done;
  }
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_obj_u32(&file, def_name_offs[i]) || !ny_obj_u8(&file, 0x0f) ||
        !ny_obj_u8(&file, 1) || !ny_obj_u16(&file, 0) ||
        !ny_obj_u64(&file, defs[i].off))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_obj_u32(&file, reloc_name_offs[i]) || !ny_obj_u8(&file, 0x01) ||
        !ny_obj_u8(&file, 0) || !ny_obj_u16(&file, 0) || !ny_obj_u64(&file, 0))
      goto done;
  }
  if (!ny_obj_emit(&file, strtab.data, strtab.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  ny_obj_free(&strtab);
  ny_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer failed");
  return ok;
}

static bool ny_x64_obj_build_bundle(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len) {
  if (!rt_main || !target || !entry_symbol || !entry_symbol[0] || !code ||
      !defs || !def_count || !relocs || !reloc_count)
    return false;
  *def_count = 0;
  *reloc_count = 0;
  for (size_t i = 0; i < func_count; ++i) {
    const char *name = func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%sny_fn_%s",
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (!ny_x64_obj_append_function(code, defs, def_count, relocs, reloc_count,
                                    &funcs[i], target, symbol, false, err,
                                    err_len))
      return false;
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix ? target->symbol_prefix : "",
           entry_symbol);
  return ny_x64_obj_append_function(code, defs, def_count, relocs, reloc_count,
                                    rt_main, target, entry, tag_return, err,
                                    err_len);
}


bool ny_native_emit_elf64_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[256];
  ny_x64_obj_reloc_t relocs[256];
  size_t def_count = 0;
  size_t reloc_count = 0;
  bool ok = ny_x64_obj_build_bundle(rt_main, funcs, func_names, func_count,
                                    target, entry_symbol, tag_return, &code,
                                    defs, &def_count, relocs, &reloc_count,
                                    err, err_len) &&
            ny_native_emit_elf64_x64_object_bundle_code(
                code.data, code.len, relocs, reloc_count, defs, def_count,
                path, err, err_len);
  ny_obj_free(&code);
  return ok;
}

bool ny_native_emit_coff_x64_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[256];
  ny_x64_obj_reloc_t relocs[256];
  size_t def_count = 0;
  size_t reloc_count = 0;
  bool ok = ny_x64_obj_build_bundle(rt_main, funcs, func_names, func_count,
                                    target, entry_symbol, tag_return, &code,
                                    defs, &def_count, relocs, &reloc_count,
                                    err, err_len) &&
            ny_native_emit_coff_x64_object_bundle_code(
                code.data, code.len, relocs, reloc_count, defs, def_count,
                path, err, err_len);
  ny_obj_free(&code);
  return ok;
}

bool ny_native_emit_macho_x64_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[256];
  ny_x64_obj_reloc_t relocs[256];
  size_t def_count = 0;
  size_t reloc_count = 0;
  bool ok = ny_x64_obj_build_bundle(rt_main, funcs, func_names, func_count,
                                    target, entry_symbol, tag_return, &code,
                                    defs, &def_count, relocs, &reloc_count,
                                    err, err_len) &&
            ny_native_emit_macho_x64_object_bundle_code(
                code.data, code.len, relocs, reloc_count, defs, def_count,
                path, err, err_len);
  ny_obj_free(&code);
  return ok;
}

bool ny_native_emit_coff_x64_object_from_nir(const ny_nir_func_t *nir,
                                             const ny_native_target_info_t *target,
                                             const char *path,
                                             const char *symbol_name,
                                             bool tag_return, char *err,
                                             size_t err_len) {
  if (!nir || !path || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len, "x86-64 COFF object writer: missing input");
    return false;
  }
  ny_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_x64_obj_emit_code(&ctx, nir, tag_return)) {
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }
  bool ok = ny_native_emit_coff_x64_object_code(ctx.code.data, ctx.code.len,
                                                ctx.relocs, ctx.reloc_count,
                                                path, symbol_name, err,
                                                err_len);
  ny_x64_obj_ctx_free(&ctx);
  return ok;
}

bool ny_native_emit_macho_x64_object_from_nir(const ny_nir_func_t *nir,
                                              const ny_native_target_info_t *target,
                                              const char *path,
                                              const char *symbol_name,
                                              bool tag_return, char *err,
                                              size_t err_len) {
  if (!nir || !path || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer: missing input");
    return false;
  }
  ny_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_x64_obj_emit_code(&ctx, nir, tag_return)) {
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }
  bool ok = ny_native_emit_macho_x64_object_code(ctx.code.data, ctx.code.len,
                                                 ctx.relocs, ctx.reloc_count,
                                                 path, symbol_name, err,
                                                 err_len);
  ny_x64_obj_ctx_free(&ctx);
  return ok;
}

bool ny_native_emit_elf64_object_from_nir(const ny_nir_func_t *nir,
                                          const ny_native_target_info_t *target,
                                          const char *path,
                                          const char *symbol_name,
                                          bool tag_return, char *err,
                                          size_t err_len) {
  if (!nir || !path || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len,
                      "x86-64 ELF object writer: missing input");
    return false;
  }
  ny_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_x64_obj_emit_code(&ctx, nir, tag_return)) {
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }

  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  bool ok = false;
  char reloc_symbols[256][256];
  size_t reloc_symbol_count = 0;
  uint32_t reloc_name_offs[256] = {0};
  if (!ny_x64_obj_collect_reloc_symbols(ctx.relocs, ctx.reloc_count,
                                        reloc_symbols, &reloc_symbol_count,
                                        err, err_len))
    goto done;

  const char shstr[] = "\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0";
  const uint32_t sh_text = 1;
  const uint32_t sh_rela_text = 7;
  const uint32_t sh_symtab = 18;
  const uint32_t sh_strtab = 26;
  const uint32_t sh_shstrtab = 34;
  if (!ny_obj_u8(&strtab, 0))
    goto done;
  uint32_t sym_name_off = (uint32_t)strtab.len;
  if (!ny_obj_emit(&strtab, symbol_name, strlen(symbol_name) + 1))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
      goto done;
  }

  if (!ny_obj_zero(&file, 64) || !ny_obj_pad_to(&file, 16))
    goto done;
  size_t text_off = file.len;
  if (!ny_obj_emit(&file, ctx.code.data, ctx.code.len) || !ny_obj_pad_to(&file, 8))
    goto done;
  size_t rela_off = file.len;
  for (size_t i = 0; i < ctx.reloc_count; ++i) {
    int sym_i = ny_x64_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                        ctx.relocs[i].symbol);
    if (sym_i < 0)
      goto done;
    uint64_t info = ((uint64_t)(2 + sym_i) << 32) | 4u; /* R_X86_64_PLT32 */
    if (!ny_obj_u64(&file, ctx.relocs[i].disp_off) || !ny_obj_u64(&file, info) ||
        !ny_obj_u64(&file, (uint64_t)-4LL))
      goto done;
  }
  size_t rela_size = file.len - rela_off;
  size_t symtab_off = file.len;
  if (!ny_elf64_write_sym(&file, 0, 0, 0, 0, 0) ||
      !ny_elf64_write_sym(&file, sym_name_off, 0x12, 1, 0, ctx.code.len))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_elf64_write_sym(&file, reloc_name_offs[i], 0x12, 0, 0, 0))
      goto done;
  }
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!ny_obj_emit(&file, strtab.data, strtab.len))
    goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstrtab_off = file.len;
  if (!ny_obj_emit(&file, shstr, sizeof(shstr)) || !ny_obj_pad_to(&file, 8))
    goto done;
  size_t shstrtab_size = sizeof(shstr);
  size_t shoff = file.len;
  if (!ny_elf64_write_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !ny_elf64_write_sh(&file, sh_text, 1, 0x6, text_off, ctx.code.len, 0, 0,
                         16, 0) ||
      !ny_elf64_write_sh(&file, sh_rela_text, 4, 0, rela_off, rela_size, 3, 1,
                         8, 24) ||
      !ny_elf64_write_sh(&file, sh_symtab, 2, 0, symtab_off, symtab_size, 4, 1,
                         8, 24) ||
      !ny_elf64_write_sh(&file, sh_strtab, 3, 0, strtab_off, strtab_size, 0, 0,
                         1, 0) ||
      !ny_elf64_write_sh(&file, sh_shstrtab, 3, 0, shstrtab_off, shstrtab_size,
                         0, 0, 1, 0))
    goto done;

  file.data[0] = 0x7f;
  file.data[1] = 'E';
  file.data[2] = 'L';
  file.data[3] = 'F';
  file.data[4] = 2;  /* ELFCLASS64 */
  file.data[5] = 1;  /* little-endian */
  file.data[6] = 1;  /* EV_CURRENT */
  ny_obj_patch_u16(&file, 16, 1);      /* ET_REL */
  ny_obj_patch_u16(&file, 18, 62);     /* EM_X86_64 */
  ny_obj_patch_u32(&file, 20, 1);
  ny_obj_patch_u64(&file, 40, shoff);
  ny_obj_patch_u16(&file, 52, 64);
  ny_obj_patch_u16(&file, 58, 64);
  ny_obj_patch_u16(&file, 60, 6);
  ny_obj_patch_u16(&file, 62, 5);

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  ny_obj_free(&strtab);
  ny_obj_free(&file);
  ny_x64_obj_ctx_free(&ctx);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "x86-64 ELF object writer failed");
  return ok;
}

bool ny_native_emit_elf32_i386_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  if (!rt_main || !path || !entry_symbol || !entry_symbol[0]) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: missing input");
    return false;
  }
  if (!target) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: missing target");
    return false;
  }

  ny_obj_buf_t code = {0};
  ny_i386_obj_symbol_def_t defs[256];
  ny_i386_obj_reloc_t relocs[256];
  size_t def_count = 0;
  size_t reloc_count = 0;

  for (size_t i = 0; i < func_count; ++i) {
    if (def_count >= sizeof(defs) / sizeof(defs[0])) {
      ny_native_set_err(err, err_len, "i386 ELF object writer: too many functions");
      ny_obj_free(&code);
      return false;
    }
    const char *name = func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), "%sny_fn_%s",
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (ny_i386_obj_def_index(defs, def_count, symbol) >= 0) {
      ny_native_set_err(err, err_len,
                        "i386 ELF object writer: duplicate symbol %s", symbol);
      ny_obj_free(&code);
      return false;
    }
    if (!ny_obj_pad_to(&code, 16)) {
      ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
      ny_obj_free(&code);
      return false;
    }
    size_t base = code.len;
    ny_i386_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
    if (!ny_i386_obj_emit_code(&ctx, &funcs[i], false)) {
      ny_i386_obj_ctx_free(&ctx);
      ny_obj_free(&code);
      return false;
    }
    if (reloc_count + ctx.reloc_count > sizeof(relocs) / sizeof(relocs[0])) {
      ny_native_set_err(err, err_len,
                        "i386 ELF object writer: too many relocations");
      ny_i386_obj_ctx_free(&ctx);
      ny_obj_free(&code);
      return false;
    }
    if (!ny_obj_emit(&code, ctx.code.data, ctx.code.len)) {
      ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
      ny_i386_obj_ctx_free(&ctx);
      ny_obj_free(&code);
      return false;
    }
    snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", symbol);
    defs[def_count].off = base;
    defs[def_count].size = ctx.code.len;
    def_count++;
    for (size_t r = 0; r < ctx.reloc_count; ++r) {
      relocs[reloc_count] = ctx.relocs[r];
      relocs[reloc_count].disp_off += base;
      reloc_count++;
    }
    ny_i386_obj_ctx_free(&ctx);
  }

  if (def_count >= sizeof(defs) / sizeof(defs[0])) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: too many functions");
    ny_obj_free(&code);
    return false;
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  if (ny_i386_obj_def_index(defs, def_count, entry) >= 0) {
    ny_native_set_err(err, err_len,
                      "i386 ELF object writer: duplicate symbol %s", entry);
    ny_obj_free(&code);
    return false;
  }
  if (!ny_obj_pad_to(&code, 16)) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
    ny_obj_free(&code);
    return false;
  }
  size_t entry_base = code.len;
  ny_i386_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_i386_obj_emit_code(&ctx, rt_main, tag_return)) {
    ny_i386_obj_ctx_free(&ctx);
    ny_obj_free(&code);
    return false;
  }
  if (reloc_count + ctx.reloc_count > sizeof(relocs) / sizeof(relocs[0])) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: too many relocations");
    ny_i386_obj_ctx_free(&ctx);
    ny_obj_free(&code);
    return false;
  }
  if (!ny_obj_emit(&code, ctx.code.data, ctx.code.len)) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
    ny_i386_obj_ctx_free(&ctx);
    ny_obj_free(&code);
    return false;
  }
  snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", entry);
  defs[def_count].off = entry_base;
  defs[def_count].size = ctx.code.len;
  def_count++;
  for (size_t r = 0; r < ctx.reloc_count; ++r) {
    relocs[reloc_count] = ctx.relocs[r];
    relocs[reloc_count].disp_off += entry_base;
    reloc_count++;
  }
  ny_i386_obj_ctx_free(&ctx);

  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  bool ok = false;
  char reloc_symbols[256][256];
  size_t reloc_symbol_count = 0;
  uint32_t def_name_offs[256] = {0};
  uint32_t reloc_name_offs[256] = {0};
  if (!ny_i386_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;

  const char shstr[] = "\0.text\0.rel.text\0.symtab\0.strtab\0.shstrtab\0";
  const uint32_t sh_text = 1;
  const uint32_t sh_rel_text = 7;
  const uint32_t sh_symtab = 17;
  const uint32_t sh_strtab = 25;
  const uint32_t sh_shstrtab = 33;
  if (!ny_obj_u8(&strtab, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    def_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, defs[i].name, strlen(defs[i].name) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!ny_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
      goto done;
  }

  if (!ny_obj_zero(&file, 52) || !ny_obj_pad_to(&file, 16))
    goto done;
  size_t text_off = file.len;
  if (!ny_obj_emit(&file, code.data, code.len) || !ny_obj_pad_to(&file, 4))
    goto done;
  size_t rel_off = file.len;
  for (size_t i = 0; i < reloc_count; ++i) {
    int def_i = ny_i386_obj_def_index(defs, def_count, relocs[i].symbol);
    uint32_t sym_index = 0;
    if (def_i >= 0) {
      sym_index = (uint32_t)(1 + def_i);
    } else {
      int ext_i = ny_i386_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                           relocs[i].symbol);
      if (ext_i < 0)
        goto done;
      sym_index = (uint32_t)(1 + def_count + (size_t)ext_i);
    }
    uint32_t info = (sym_index << 8) | 2u; /* R_386_PC32 */
    if (!ny_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !ny_obj_u32(&file, info))
      goto done;
  }
  size_t rel_size = file.len - rel_off;
  size_t symtab_off = file.len;
  if (!ny_elf32_write_sym(&file, 0, 0, 0, 0, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_elf32_write_sym(&file, def_name_offs[i], (uint32_t)defs[i].off,
                            (uint32_t)defs[i].size, 0x12, 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_elf32_write_sym(&file, reloc_name_offs[i], 0, 0, 0x12, 0))
      goto done;
  }
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!ny_obj_emit(&file, strtab.data, strtab.len))
    goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstrtab_off = file.len;
  if (!ny_obj_emit(&file, shstr, sizeof(shstr)) || !ny_obj_pad_to(&file, 4))
    goto done;
  size_t shstrtab_size = sizeof(shstr);
  size_t shoff = file.len;
  if (!ny_elf32_write_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !ny_elf32_write_sh(&file, sh_text, 1, 0x6, (uint32_t)text_off,
                         (uint32_t)code.len, 0, 0, 16, 0) ||
      !ny_elf32_write_sh(&file, sh_rel_text, 9, 0, (uint32_t)rel_off,
                         (uint32_t)rel_size, 3, 1, 4, 8) ||
      !ny_elf32_write_sh(&file, sh_symtab, 2, 0, (uint32_t)symtab_off,
                         (uint32_t)symtab_size, 4, 1, 4, 16) ||
      !ny_elf32_write_sh(&file, sh_strtab, 3, 0, (uint32_t)strtab_off,
                         (uint32_t)strtab_size, 0, 0, 1, 0) ||
      !ny_elf32_write_sh(&file, sh_shstrtab, 3, 0, (uint32_t)shstrtab_off,
                         (uint32_t)shstrtab_size, 0, 0, 1, 0))
    goto done;

  file.data[0] = 0x7f;
  file.data[1] = 'E';
  file.data[2] = 'L';
  file.data[3] = 'F';
  file.data[4] = 1;  /* ELFCLASS32 */
  file.data[5] = 1;  /* little-endian */
  file.data[6] = 1;  /* EV_CURRENT */
  ny_obj_patch_u16(&file, 16, 1);      /* ET_REL */
  ny_obj_patch_u16(&file, 18, 3);      /* EM_386 */
  ny_obj_patch_u32(&file, 20, 1);
  ny_obj_patch_u32(&file, 32, (uint32_t)shoff);
  ny_obj_patch_u16(&file, 40, 52);
  ny_obj_patch_u16(&file, 46, 40);
  ny_obj_patch_u16(&file, 48, 6);
  ny_obj_patch_u16(&file, 50, 5);

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  ny_obj_free(&strtab);
  ny_obj_free(&file);
  ny_obj_free(&code);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "i386 ELF object writer failed");
  return ok;
}
