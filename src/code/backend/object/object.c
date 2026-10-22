/*
 * Object-file writer: produces ELF64, ELF32, COFF, and Mach-O object
 * files from machine-form with relocations and section management.
 *
 * References:
 *  System V ABI (x86-64): ELF64 object format, relocation types,
 *   section layout conventions.
 *  PE/COFF Specification (Microsoft): COFF and PE32+ object formats.
 *  OS X ABI Mach-O Reference (Apple): Mach-O object format.
 */
#include "base/trace.h"
#include "code/backend/abi.h"
#include "code/backend/object/internal.h"

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

static void ny_x64_obj_valmap_free(nyr_x64_obj_valmap_t *m);

void nyr_obj_free(nyr_obj_buf_t *b) {
  if (!b)
    return;
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

void nyr_x64_obj_ctx_free(nyr_x64_obj_ctx_t *c) {
  if (!c)
    return;
  nyr_obj_free(&c->code);
  ny_x64_obj_valmap_free(&c->valmap);
  free(c->value_f64);
  free(c->value_f32);
  free(c->local_f64);
  free(c->local_f32);
  free(c->value_immediate);
  free(c->value_reg);
  free(c->value_xmm);
  free(c->value_spill);
  free(c->agg_home_off);
  c->agg_home_off = NULL;
  c->agg_home_words = 0;
  free(c->labels);
  free(c->patches);
  free(c->relocs);
  free(c->inst_offsets);
  c->value_f64 = NULL;
  c->value_f32 = NULL;
  c->local_f64 = NULL;
  c->local_f32 = NULL;
  c->value_immediate = NULL;
  c->value_reg = NULL;
  c->value_xmm = NULL;
  c->value_spill = NULL;
  c->labels = NULL;
  c->patches = NULL;
  c->relocs = NULL;
  c->inst_offsets = NULL;
}

bool nyr_native_debug_line_append(nyr_native_debug_line_t **lines,
                                  size_t *count, size_t *cap,
                                  nyr_native_debug_line_t line) {
  if (!lines || !count || !cap || *count > *cap)
    return false;
  if (*count == *cap) {
    if (*count == SIZE_MAX)
      return false;
    size_t next = *cap > SIZE_MAX / 2 ? *count + 1 : (*cap ? *cap * 2 : 32);
    if (next < *count + 1 || next > SIZE_MAX / sizeof(**lines))
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

static bool ny_obj_reserve(nyr_obj_buf_t *b, size_t add) {
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

bool nyr_obj_emit(nyr_obj_buf_t *b, const void *data, size_t len) {
  if (!ny_obj_reserve(b, len))
    return false;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return true;
}

bool nyr_obj_u8(nyr_obj_buf_t *b, unsigned v) {
  unsigned char c = (unsigned char)v;
  return nyr_obj_emit(b, &c, 1);
}

bool nyr_obj_pad_to(nyr_obj_buf_t *b, size_t align) {
  if (align == 0)
    return true;
  while ((b->len % align) != 0) {
    if (!nyr_obj_u8(b, 0))
      return false;
  }
  return true;
}

bool nyr_obj_zero(nyr_obj_buf_t *b, size_t len) {
  if (!ny_obj_reserve(b, len))
    return false;
  memset(b->data + b->len, 0, len);
  b->len += len;
  return true;
}

bool nyr_obj_u16(nyr_obj_buf_t *b, uint16_t v) {
  unsigned char c[2] = {(unsigned char)(v & 0xff), (unsigned char)(v >> 8)};
  return nyr_obj_emit(b, c, sizeof(c));
}

bool nyr_obj_u32(nyr_obj_buf_t *b, uint32_t v) {
  unsigned char c[4] = {
      (unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
      (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)};
  return nyr_obj_emit(b, c, sizeof(c));
}

bool nyr_obj_u64(nyr_obj_buf_t *b, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    if (!nyr_obj_u8(b, (unsigned)((v >> (i * 8)) & 0xff)))
      return false;
  }
  return true;
}

void nyr_obj_patch_u16(nyr_obj_buf_t *b, size_t off, uint16_t v) {
  b->data[off + 0] = (unsigned char)(v & 0xff);
  b->data[off + 1] = (unsigned char)(v >> 8);
}

void nyr_obj_patch_u32(nyr_obj_buf_t *b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

void nyr_obj_patch_u64(nyr_obj_buf_t *b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b->data[off + (size_t)i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

bool nyr_elf32_write_sym(nyr_obj_buf_t *b, uint32_t name, uint32_t value,
                         uint32_t size, unsigned char info, uint16_t shndx) {
  return nyr_obj_u32(b, name) && nyr_obj_u32(b, value) &&
         nyr_obj_u32(b, size) && nyr_obj_u8(b, info) && nyr_obj_u8(b, 0) &&
         nyr_obj_u16(b, shndx);
}

bool nyr_elf32_write_sh(nyr_obj_buf_t *b, uint32_t name, uint32_t type,
                        uint32_t flags, uint32_t off, uint32_t size,
                        uint32_t link, uint32_t info, uint32_t addralign,
                        uint32_t entsize) {
  return nyr_obj_u32(b, name) && nyr_obj_u32(b, type) &&
         nyr_obj_u32(b, flags) && nyr_obj_u32(b, 0) && nyr_obj_u32(b, off) &&
         nyr_obj_u32(b, size) && nyr_obj_u32(b, link) && nyr_obj_u32(b, info) &&
         nyr_obj_u32(b, addralign) && nyr_obj_u32(b, entsize);
}

void nyr_i386_obj_ctx_free(nyr_i386_obj_ctx_t *c) {
  if (c) {
    nyr_obj_free(&c->code);
    free(c->value_f64);
    free(c->value_f32);
    free(c->local_f64);
    free(c->local_f32);
    free(c->relocs);
  }
}

static int ny_i386_obj_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static int ny_i386_obj_value_off(int value) { return -4 - 8 * (value + 1); }

static int ny_i386_obj_local_off(const nyr_i386_obj_ctx_t *c, int local) {
  return -4 - c->local_base - 8 * (local + 1);
}

static bool ny_i386_obj_bytes(nyr_i386_obj_ctx_t *c, const unsigned char *p,
                              size_t len) {
  if (!nyr_obj_emit(&c->code, p, len)) {
    nyr_set_error(c->err, c->err_len, "i386 ELF object writer: out of memory");
    return false;
  }
  return true;
}

static bool ny_i386_obj_u8(nyr_i386_obj_ctx_t *c, unsigned v) {
  return ny_i386_obj_bytes(c, (const unsigned char *)&(unsigned char){v}, 1);
}

static bool ny_i386_obj_i32(nyr_i386_obj_ctx_t *c, int32_t v) {
  unsigned char b[4] = {(unsigned char)((uint32_t)v & 0xff),
                        (unsigned char)(((uint32_t)v >> 8) & 0xff),
                        (unsigned char)(((uint32_t)v >> 16) & 0xff),
                        (unsigned char)(((uint32_t)v >> 24) & 0xff)};
  return ny_i386_obj_bytes(c, b, sizeof(b));
}

/*
 * disp32(%ebp) memops: opcode bytes then i32 displacement.
 */
#define NY_I386_EBP_OP(name, ...)                                              \
  static bool name(nyr_i386_obj_ctx_t *c, int off) {                           \
    static const unsigned char op[] = {__VA_ARGS__};                           \
    return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off);    \
  }
NY_I386_EBP_OP(ny_i386_obj_load_eax, 0x8b, 0x85)
NY_I386_EBP_OP(ny_i386_obj_lea_eax, 0x8d, 0x85)
NY_I386_EBP_OP(ny_i386_obj_store_eax, 0x89, 0x85)
NY_I386_EBP_OP(ny_i386_obj_load_ebx, 0x8b, 0x9d)
NY_I386_EBP_OP(ny_i386_obj_load_ecx, 0x8b, 0x8d)
NY_I386_EBP_OP(ny_i386_obj_store_edx, 0x89, 0x95)
NY_I386_EBP_OP(ny_i386_obj_fldl, 0xdd, 0x85)
NY_I386_EBP_OP(ny_i386_obj_fstpl, 0xdd, 0x9d)
NY_I386_EBP_OP(ny_i386_obj_fildl, 0xdb, 0x85)
NY_I386_EBP_OP(ny_i386_obj_flds, 0xd9, 0x85)
NY_I386_EBP_OP(ny_i386_obj_fstps, 0xd9, 0x9d)
#undef NY_I386_EBP_OP

static bool ny_i386_obj_mov_imm32(nyr_i386_obj_ctx_t *c, int off,
                                  uint32_t imm) {
  static const unsigned char op[] = {0xc7, 0x85};
  return ny_i386_obj_bytes(c, op, sizeof(op)) && ny_i386_obj_i32(c, off) &&
         ny_i386_obj_i32(c, (int32_t)imm);
}

static bool ny_i386_obj_store_f64_bits(nyr_i386_obj_ctx_t *c, int off,
                                       uint64_t bits) {
  return ny_i386_obj_mov_imm32(c, off, (uint32_t)bits) &&
         ny_i386_obj_mov_imm32(c, off + 4, (uint32_t)(bits >> 32));
}

static bool ny_i386_obj_store_f32_bits(nyr_i386_obj_ctx_t *c, int off,
                                       uint32_t bits) {
  return ny_i386_obj_mov_imm32(c, off, bits);
}

static bool ny_i386_obj_f64_memop(nyr_i386_obj_ctx_t *c, unsigned modrm,
                                  int off) {
  return ny_i386_obj_u8(c, 0xdc) && ny_i386_obj_u8(c, modrm) &&
         ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_f32_memop(nyr_i386_obj_ctx_t *c, unsigned modrm,
                                  int off) {
  return ny_i386_obj_u8(c, 0xd8) && ny_i386_obj_u8(c, modrm) &&
         ny_i386_obj_i32(c, off);
}

static bool ny_i386_obj_load_value_f64(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid f64 value v%d", value);
    return false;
  }
  return ny_i386_obj_fldl(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_f64(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid f64 destination v%d", value);
    return false;
  }
  return ny_i386_obj_fstpl(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_f32(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid f32 value v%d", value);
    return false;
  }
  return ny_i386_obj_flds(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_f32(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid f32 destination v%d", value);
    return false;
  }
  return ny_i386_obj_fstps(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_eax(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_i386_obj_load_eax(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_eax(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid destination v%d", value);
    return false;
  }
  return ny_i386_obj_store_eax(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_store_value_edx(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid destination v%d", value);
    return false;
  }
  return ny_i386_obj_store_edx(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_ebx(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_i386_obj_load_ebx(c, ny_i386_obj_value_off(value));
}

static bool ny_i386_obj_load_value_ecx(nyr_i386_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: invalid value v%d", value);
    return false;
  }
  return ny_i386_obj_load_ecx(c, ny_i386_obj_value_off(value));
}

static unsigned ny_i386_obj_setcc(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return 0x94;
  case NYIR_CMP_NE:
    return 0x95;
  case NYIR_CMP_LT:
    return 0x9c;
  case NYIR_CMP_LE:
    return 0x9e;
  case NYIR_CMP_GT:
    return 0x9f;
  case NYIR_CMP_GE:
    return 0x9d;
  }
  return 0x94;
}

static unsigned ny_i386_obj_f64_setcc(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return 0x94;
  case NYIR_CMP_NE:
    return 0x95;
  case NYIR_CMP_LT:
    return 0x92;
  case NYIR_CMP_LE:
    return 0x96;
  case NYIR_CMP_GT:
    return 0x97;
  case NYIR_CMP_GE:
    return 0x93;
  }
  return 0x94;
}

static bool ny_i386_obj_label(nyr_i386_obj_ctx_t *c, int64_t label) {
  if (c->label_count >= sizeof(c->labels) / sizeof(c->labels[0])) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: too many labels");
    return false;
  }
  c->labels[c->label_count++] = (nyr_i386_obj_label_t){label, c->code.len};
  return true;
}

static bool ny_i386_obj_add_patch(nyr_i386_obj_ctx_t *c, int64_t label,
                                  size_t disp_off) {
  if (c->patch_count >= sizeof(c->patches) / sizeof(c->patches[0])) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: too many branches");
    return false;
  }
  c->patches[c->patch_count++] = (nyr_i386_obj_patch_t){label, disp_off};
  return true;
}

static bool ny_i386_obj_add_epilogue_patch(nyr_i386_obj_ctx_t *c,
                                           size_t disp_off) {
  if (c->epilogue_patch_count >=
      sizeof(c->epilogue_patches) / sizeof(c->epilogue_patches[0])) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: too many returns");
    return false;
  }
  c->epilogue_patches[c->epilogue_patch_count++] = disp_off;
  return true;
}

static bool ny_obj_reloc_symbol(char *out, size_t out_len, const char *prefix,
                                const char *symbol) {
  if (!out || out_len == 0 || !symbol || !symbol[0])
    return false;
  /*
   * Lowering emits pooled literals as local object symbols (for example
   * pooled ny_str_0).  They already name a definition in this object and must
   * not pass through the Nytrix function-name formatter.
   */
  if (symbol[0] == '.' || strncmp(symbol, "ny_str_", 7) == 0) {
    int n = snprintf(out, out_len, "%s", symbol);
    return n > 0 && (size_t)n < out_len;
  }
  if (nyr_globaltab_has(symbol) || nyr_consttab_has(symbol) ||
      strncmp(symbol, "ny_fn_", 6) == 0 ||
      ny_native_runtime_abi_decl(symbol) != NULL) {
    int n = snprintf(out, out_len, "%s", symbol);
    return n > 0 && (size_t)n < out_len;
  }
  int n = snprintf(out, out_len, NY_FMT_FN, prefix ? prefix : "", symbol);
  return n > 0 && (size_t)n < out_len;
}

static bool ny_i386_obj_reloc_symbol(char *out, size_t out_len,
                                     const nyr_i386_obj_ctx_t *c,
                                     const char *symbol) {
  const char *prefix = c && c->target && c->target->symbol_prefix
                           ? c->target->symbol_prefix
                           : "";
  return ny_obj_reloc_symbol(out, out_len, prefix, symbol);
}

static bool ny_i386_obj_add_reloc(nyr_i386_obj_ctx_t *c, const char *symbol,
                                  size_t disp_off) {
  if (c->reloc_count == c->reloc_cap) {
    size_t cap = c->reloc_cap ? c->reloc_cap * 2u : 64u;
    if (cap < c->reloc_cap || cap > NY_X64_OBJ_MAX_RELOCS) {
      nyr_set_error(c->err, c->err_len,
                    "i386 ELF object writer: relocation transport too large");
      return false;
    }
    nyr_i386_obj_reloc_t *grown = realloc(c->relocs, cap * sizeof(*grown));
    if (!grown) {
      nyr_set_error(c->err, c->err_len,
                    "i386 ELF object writer: relocation allocation failed");
      return false;
    }
    c->relocs = grown;
    c->reloc_cap = cap;
  }
  nyr_i386_obj_reloc_t *r = &c->relocs[c->reloc_count++];
  snprintf(r->symbol, sizeof(r->symbol), "%s", symbol ? symbol : "");
  r->disp_off = disp_off;
  return true;
}

static bool ny_i386_obj_rel32(nyr_i386_obj_ctx_t *c, size_t disp_off,
                              size_t target_off) {
  int64_t rel = (int64_t)target_off - (int64_t)(disp_off + 4);
  if (rel < INT32_MIN || rel > INT32_MAX) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: branch out of range");
    return false;
  }
  nyr_obj_patch_u32(&c->code, disp_off, (uint32_t)(int32_t)rel);
  return true;
}

int nyr_i386_obj_symbol_index(char symbols[][256], size_t count,
                              const char *name) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(symbols[i], name) == 0)
      return (int)i;
  }
  return -1;
}

int nyr_i386_obj_def_index(const nyr_i386_obj_symbol_def_t *defs,
                           size_t def_count, const char *name) {
  for (size_t i = 0; defs && i < def_count; ++i) {
    if (strcmp(defs[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

int nyr_x64_obj_def_index(const nyr_x64_obj_symbol_def_t *defs,
                          size_t def_count, const char *name) {
  for (size_t i = 0; defs && i < def_count; ++i) {
    if (strcmp(defs[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

bool nyr_i386_obj_collect_external_reloc_symbols(
    const nyr_i386_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_i386_obj_symbol_def_t *defs, size_t def_count,
    char symbols[][256], size_t *symbol_count, char *err, size_t err_len) {
  size_t count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].symbol[0]) {
      nyr_set_error(err, err_len,
                    "i386 ELF object writer: empty relocation symbol");
      return false;
    }
    if (nyr_i386_obj_def_index(defs, def_count, relocs[i].symbol) >= 0)
      continue;
    if (nyr_i386_obj_symbol_index(symbols, count, relocs[i].symbol) >= 0)
      continue;
    if (count >= NY_X64_OBJ_MAX_RELOCS) {
      nyr_set_error(err, err_len,
                    "i386 ELF object writer: too many relocation symbols");
      return false;
    }
    snprintf(symbols[count++], 256, "%s", relocs[i].symbol);
  }
  *symbol_count = count;
  return true;
}

static bool ny_i386_obj_patch_branches(nyr_i386_obj_ctx_t *c,
                                       size_t epilogue_off) {
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
      nyr_set_error(c->err, c->err_len,
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

static void ny_i386_obj_mark_value_f64(nyr_i386_obj_ctx_t *c, int v,
                                       bool *changed) {
  if (!c || !c->value_f64 || v < 0 || v >= c->value_slots || c->value_f64[v])
    return;
  c->value_f64[v] = true;
  if (changed)
    *changed = true;
}

static void ny_i386_obj_mark_value_f32(nyr_i386_obj_ctx_t *c, int v,
                                       bool *changed) {
  if (!c || !c->value_f32 || v < 0 || v >= c->value_slots || c->value_f32[v])
    return;
  c->value_f32[v] = true;
  if (changed)
    *changed = true;
}

static void ny_i386_obj_mark_local_f64(nyr_i386_obj_ctx_t *c, int local,
                                       bool *changed) {
  if (!c || !c->local_f64 || local < 0 || local >= c->local_slots ||
      c->local_f64[local])
    return;
  c->local_f64[local] = true;
  if (changed)
    *changed = true;
}

static void ny_i386_obj_mark_local_f32(nyr_i386_obj_ctx_t *c, int local,
                                       bool *changed) {
  if (!c || !c->local_f32 || local < 0 || local >= c->local_slots ||
      c->local_f32[local])
    return;
  c->local_f32[local] = true;
  if (changed)
    *changed = true;
}

static bool ny_i386_obj_compute_frame(nyr_i386_obj_ctx_t *c,
                                      const nyr_func_t *nyir) {
  c->max_local_slot = 0;
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= c->max_local_slot)
      c->max_local_slot = (int)in->imm + 1;
  }
  if (nyir->param_count <= (size_t)INT_MAX &&
      (int)nyir->param_count > c->max_local_slot)
    c->max_local_slot = (int)nyir->param_count;
  c->value_slots = nyir->next_value > 0 ? nyir->next_value : 0;
  c->local_slots = c->max_local_slot;
  c->local_base = c->value_slots * 8;
  c->frame_bytes = ny_i386_obj_align((c->value_slots + c->local_slots) * 8, 16);
  c->value_f64 =
      c->value_slots > 0 ? calloc((size_t)c->value_slots, sizeof(bool)) : NULL;
  c->value_f32 =
      c->value_slots > 0 ? calloc((size_t)c->value_slots, sizeof(bool)) : NULL;
  c->local_f64 =
      c->local_slots > 0 ? calloc((size_t)c->local_slots, sizeof(bool)) : NULL;
  c->local_f32 =
      c->local_slots > 0 ? calloc((size_t)c->local_slots, sizeof(bool)) : NULL;
  if ((c->value_slots > 0 && !c->value_f64) ||
      (c->value_slots > 0 && !c->value_f32) ||
      (c->local_slots > 0 && !c->local_f64) ||
      (c->local_slots > 0 && !c->local_f32)) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: OOM f64 frame classification");
    return false;
  }
  for (size_t i = 0; i < nyir->param_count && i < (size_t)c->local_slots; ++i) {
    if (!nyir->param_types)
      break;
    c->local_f64[i] = nyir->param_types[i] == NYIR_PARAM_F64;
    c->local_f32[i] = nyir->param_types[i] == NYIR_PARAM_F32;
  }
  bool changed = true;
  for (int pass = 0; changed && pass < 8; ++pass) {
    changed = false;
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyr_inst_t *in = &nyir->data[i];
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
      case NYIR_COPY:
        if (in->a >= 0 && in->a < c->value_slots && c->value_f64[in->a])
          ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f64[in->dst])
          ny_i386_obj_mark_value_f64(c, in->a, &changed);
        if (in->a >= 0 && in->a < c->value_slots && c->value_f32[in->a])
          ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f32[in->dst])
          ny_i386_obj_mark_value_f32(c, in->a, &changed);
        break;
      case NYIR_LOAD_LOCAL:
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f64[in->imm])
          ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f64[in->dst])
          ny_i386_obj_mark_local_f64(c, (int)in->imm, &changed);
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f32[in->imm])
          ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        if (in->dst >= 0 && in->dst < c->value_slots && c->value_f32[in->dst])
          ny_i386_obj_mark_local_f32(c, (int)in->imm, &changed);
        break;
      case NYIR_STORE_LOCAL:
        if (in->a >= 0 && in->a < c->value_slots && c->value_f64[in->a])
          ny_i386_obj_mark_local_f64(c, (int)in->imm, &changed);
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f64[in->imm])
          ny_i386_obj_mark_value_f64(c, in->a, &changed);
        if (in->a >= 0 && in->a < c->value_slots && c->value_f32[in->a])
          ny_i386_obj_mark_local_f32(c, (int)in->imm, &changed);
        if (in->imm >= 0 && in->imm < c->local_slots && c->local_f32[in->imm])
          ny_i386_obj_mark_value_f32(c, in->a, &changed);
        break;
      case NYIR_CALL:
        if (in->scalar_class == NYIR_SCALAR_F64)
          ny_i386_obj_mark_value_f64(c, in->dst, &changed);
        if (in->scalar_class == NYIR_SCALAR_F32)
          ny_i386_obj_mark_value_f32(c, in->dst, &changed);
        break;
      default:
        break;
      }
    }
  }
  return true;
}

static bool ny_i386_obj_emit_binop(nyr_i386_obj_ctx_t *c, const nyr_inst_t *in,
                                   const unsigned char *op, size_t op_len) {
  return ny_i386_obj_load_value_eax(c, in->a) &&
         ny_i386_obj_load_value_ebx(c, in->b) &&
         ny_i386_obj_bytes(c, op, op_len) &&
         ny_i386_obj_store_value_eax(c, in->dst);
}

static bool ny_i386_obj_emit_f64_binop(nyr_i386_obj_ctx_t *c,
                                       const nyr_inst_t *in, unsigned modrm) {
  return ny_i386_obj_load_value_f64(c, in->a) &&
         ny_i386_obj_f64_memop(c, modrm, ny_i386_obj_value_off(in->b)) &&
         ny_i386_obj_store_value_f64(c, in->dst);
}

static bool ny_i386_obj_emit_f32_binop(nyr_i386_obj_ctx_t *c,
                                       const nyr_inst_t *in, unsigned modrm) {
  return ny_i386_obj_load_value_f32(c, in->a) &&
         ny_i386_obj_f32_memop(c, modrm, ny_i386_obj_value_off(in->b)) &&
         ny_i386_obj_store_value_f32(c, in->dst);
}

static bool ny_i386_obj_emit_param_spills(nyr_i386_obj_ctx_t *c,
                                          const nyr_func_t *nyir) {
  if (!c || !nyir || c->max_local_slot <= 0)
    return true;
  bool *stored = calloc((size_t)c->max_local_slot, sizeof(bool));
  bool *param = calloc((size_t)c->max_local_slot, sizeof(bool));
  if (!stored || !param) {
    free(stored);
    free(param);
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: OOM param spill");
    return false;
  }
  if (nyir->param_count > 0) {
    if (nyir->param_count > (size_t)c->max_local_slot) {
      free(stored);
      free(param);
      nyr_set_error(
          c->err, c->err_len,
          "i386 ELF object writer: parameter prefix exceeds local slots");
      return false;
    }
    for (size_t i = 0; i < nyir->param_count; ++i)
      param[i] = true;
  } else {
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyr_inst_t *in = &nyir->data[i];
      if (in->op != NYIR_LOAD_LOCAL && in->op != NYIR_STORE_LOCAL)
        continue;
      int local = (int)in->imm;
      if (local < 0 || local >= c->max_local_slot)
        continue;
      if (in->op == NYIR_STORE_LOCAL) {
        stored[local] = true;
      } else if (!stored[local]) {
        param[local] = true;
      }
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

static bool ny_i386_obj_collect_call_args(nyr_i386_obj_ctx_t *c,
                                          const nyr_inst_t *in, int *args,
                                          int *argc_out) {
  int argc = (int)in->imm;
  if (argc < 0 || argc > NYIR_CALL_MAX_ARGS) {
    nyr_set_error(
        c->err, c->err_len,
        "i386 ELF object writer: call exceeds maximum supported arg count");
    return false;
  }
  if (argc > 0)
    args[0] = in->a;
  if (argc > 1)
    args[1] = in->b;
  if (argc > 2)
    args[2] = in->c;
  if (argc > 3)
    args[3] = in->d;
  if (argc > 4)
    args[4] = in->e;
  if (argc > 5)
    args[5] = in->f;
  for (int i = 6; i < argc; ++i)
    args[i] = (in->extra_args && (size_t)(i - 6) < in->extra_args_len)
                  ? in->extra_args[i - 6]
                  : -1;
  for (int i = 0; i < argc; ++i) {
    if (args[i] < 0 || args[i] >= c->value_slots) {
      nyr_set_error(c->err, c->err_len,
                    "i386 ELF object writer: invalid call arg");
      return false;
    }
  }
  *argc_out = argc;
  return true;
}

bool nyr_i386_obj_emit_code(nyr_i386_obj_ctx_t *c, const nyr_func_t *nyir,
                            bool tag_return) {
  if (!nyir)
    return false;
  for (size_t i = 0; i < nyir->param_count; ++i)
    if (nyir->param_types && nyir->param_types[i] == NYIR_PARAM_AGGREGATE) {
      nyr_set_error(c->err, c->err_len,
                    "i386 ELF object writer: aggregate parameters are "
                    "unsupported");
      return false;
    }
  if (nyir->return_aggregate_size) {
    nyr_set_error(c->err, c->err_len,
                  "i386 ELF object writer: aggregate returns are "
                  "unsupported");
    return false;
  }
  if (!ny_i386_obj_compute_frame(c, nyir))
    return false;
  if (!ny_i386_obj_bytes(c, (const unsigned char[]){0x55, 0x89, 0xe5, 0x53}, 4))
    return false;
  if (c->frame_bytes > 0 &&
      (!ny_i386_obj_bytes(c, (const unsigned char[]){0x81, 0xec}, 2) ||
       !ny_i386_obj_i32(c, c->frame_bytes)))
    return false;
  if (!ny_i386_obj_emit_param_spills(c, nyir))
    return false;
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    switch (in->op) {
    case NYIR_NOP:
      break;
    case NYIR_CONST_I64:
      if (in->imm < INT32_MIN || in->imm > UINT32_MAX) {
        nyr_set_error(c->err, c->err_len,
                      "i386 ELF object writer: constant out of i32 range");
        return false;
      }
      if (!ny_i386_obj_u8(c, 0xb8) ||
          !ny_i386_obj_i32(c, (int32_t)(uint32_t)in->imm) ||
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
    case NYIR_COPY:
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
    case NYIR_ADD_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x01, 0xd8},
                                  2))
        return false;
      break;
    case NYIR_SUB_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x29, 0xd8},
                                  2))
        return false;
      break;
    case NYIR_MUL_I64:
      if (!ny_i386_obj_emit_binop(c, in,
                                  (const unsigned char[]){0x0f, 0xaf, 0xc3}, 3))
        return false;
      break;
    case NYIR_AND_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x21, 0xd8},
                                  2))
        return false;
      break;
    case NYIR_OR_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x09, 0xd8},
                                  2))
        return false;
      break;
    case NYIR_XOR_I64:
      if (!ny_i386_obj_emit_binop(c, in, (const unsigned char[]){0x31, 0xd8},
                                  2))
        return false;
      break;
    case NYIR_DIV_I64:
    case NYIR_MOD_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_load_value_ebx(c, in->b) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x99, 0xf7, 0xfb}, 3))
        return false;
      if (in->op == NYIR_DIV_I64) {
        if (!ny_i386_obj_store_value_eax(c, in->dst))
          return false;
      } else if (!ny_i386_obj_store_value_edx(c, in->dst)) {
        return false;
      }
      break;
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_load_value_ecx(c, in->b) ||
          !ny_i386_obj_bytes(c,
                             in->op == NYIR_SHL_I64
                                 ? (const unsigned char[]){0xd3, 0xe0}
                                 : (const unsigned char[]){0xd3, 0xf8},
                             2) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_CMP_I64:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_load_value_ebx(c, in->b) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x39, 0xd8, 0x0f}, 3) ||
          !ny_i386_obj_u8(c, ny_i386_obj_setcc(in->cmp)) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0xc0, 0x0f, 0xb6, 0xc0},
                             4) ||
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
      /*
       * FCOMIP writes EFLAGS directly.  Keep the x87 stack balanced after
       * its implicit pop so conditional branches do not depend on SAHF.
       */
      if (!ny_i386_obj_load_value_f64(c, in->b) ||
          !ny_i386_obj_load_value_f64(c, in->a) ||
          !ny_i386_obj_bytes(
              c, (const unsigned char[]){0xdf, 0xf1, 0xdd, 0xd8, 0x0f}, 5) ||
          !ny_i386_obj_u8(c, ny_i386_obj_f64_setcc(in->cmp)) ||
          !ny_i386_obj_u8(c, 0xc0) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x0f, 0xb6, 0xc0}, 3) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_CMP_F32:
      if (!ny_i386_obj_load_value_f32(c, in->b) ||
          !ny_i386_obj_load_value_f32(c, in->a) ||
          !ny_i386_obj_bytes(
              c, (const unsigned char[]){0xdf, 0xf1, 0xdd, 0xd8, 0x0f}, 5) ||
          !ny_i386_obj_u8(c, ny_i386_obj_f64_setcc(in->cmp)) ||
          !ny_i386_obj_u8(c, 0xc0) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x0f, 0xb6, 0xc0}, 3) ||
          !ny_i386_obj_store_value_eax(c, in->dst))
        return false;
      break;
    case NYIR_LABEL:
      if (!ny_i386_obj_label(c, in->imm))
        return false;
      break;
    case NYIR_LOAD_LOCAL:
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
      } else if (!ny_i386_obj_load_eax(
                     c, ny_i386_obj_local_off(c, (int)in->imm)) ||
                 !ny_i386_obj_store_value_eax(c, in->dst)) {
        return false;
      }
      break;
    case NYIR_ADDR_LOCAL:
      if (in->imm < 0 || in->imm >= c->local_slots) {
        nyr_set_error(c->err, c->err_len,
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
        nyr_set_error(
            c->err, c->err_len,
            "i386 ELF object writer: addr.symbol missing symbol name");
        return false;
      }
      if (!ny_i386_obj_u8(c, 0xb8))
        return false;
      {
        char sym[256];
        if (!ny_i386_obj_reloc_symbol(sym, sizeof(sym), c, in->symbol)) {
          nyr_set_error(c->err, c->err_len,
                        "i386 ELF object writer: invalid addr symbol");
          return false;
        }
        size_t disp = c->code.len;
        if (!ny_i386_obj_i32(c, 0) || !ny_i386_obj_add_reloc(c, sym, disp) ||
            !ny_i386_obj_store_value_eax(c, in->dst))
          return false;
      }
      break;
    case NYIR_STORE_LOCAL:
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
                 !ny_i386_obj_store_eax(
                     c, ny_i386_obj_local_off(c, (int)in->imm))) {
        return false;
      }
      break;
    case NYIR_RET:
      if (in->a >= 0) {
        if (in->a < c->value_slots && c->value_f64 && c->value_f64[in->a]) {
          if (!ny_i386_obj_load_value_f64(c, in->a))
            return false;
        } else if (in->a < c->value_slots && c->value_f32 &&
                   c->value_f32[in->a]) {
          if (!ny_i386_obj_load_value_f32(c, in->a))
            return false;
        } else if (!ny_i386_obj_load_value_eax(c, in->a) ||
                   !ny_i386_obj_u8(c, 0x99)) {
          return false;
        }
      }
      if (!ny_i386_obj_u8(c, 0xe9))
        return false;
      if (!ny_i386_obj_add_epilogue_patch(c, c->code.len) ||
          !ny_i386_obj_i32(c, 0))
        return false;
      break;
    case NYIR_BR:
      if (!ny_i386_obj_u8(c, 0xe9))
        return false;
      if (!ny_i386_obj_add_patch(c, in->imm, c->code.len) ||
          !ny_i386_obj_i32(c, 0))
        return false;
      break;
    case NYIR_BR_IF:
      if (!ny_i386_obj_load_value_eax(c, in->a) ||
          !ny_i386_obj_bytes(c, (const unsigned char[]){0x85, 0xc0, 0x0f, 0x85},
                             4))
        return false;
      if (!ny_i386_obj_add_patch(c, in->imm, c->code.len) ||
          !ny_i386_obj_i32(c, 0))
        return false;
      break;
    case NYIR_CALL: {
      int args[NYIR_CALL_MAX_ARGS];
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
          if (!ny_i386_obj_load_eax(c, off + 4) || !ny_i386_obj_u8(c, 0x50) ||
              !ny_i386_obj_load_eax(c, off) || !ny_i386_obj_u8(c, 0x50))
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
        if (!ny_i386_obj_load_value_eax(c, args[j]) || !ny_i386_obj_u8(c, 0x50))
          return false;
        stack_bytes += 4;
      }
      if (!ny_i386_obj_u8(c, 0xe8))
        return false;
      size_t disp = c->code.len;
      char symbol[256];
      if (in->flags & NYIR_INST_F_EXTERN) {
        const char *prefix = c->target && c->target->symbol_prefix
                                 ? c->target->symbol_prefix
                                 : "";
        int n = snprintf(symbol, sizeof(symbol), "%s%s", prefix,
                         in->symbol ? in->symbol : "<null>");
        if (n < 0 || (size_t)n >= sizeof(symbol)) {
          nyr_set_error(c->err, c->err_len,
                        "i386 ELF object writer: extern symbol too long");
          return false;
        }
      } else if (!ny_i386_obj_reloc_symbol(symbol, sizeof(symbol), c,
                                           in->symbol)) {
        nyr_set_error(c->err, c->err_len,
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
        } else if (in->dst < c->value_slots && c->value_f32 &&
                   c->value_f32[in->dst]) {
          if (!ny_i386_obj_store_value_f32(c, in->dst))
            return false;
        } else if (!ny_i386_obj_store_value_eax(c, in->dst)) {
          return false;
        }
      }
      break;
    }
    case NYIR_BOUNDS_CHECK:
      /*
       * Reject negative indexes before doing the unsigned upper-bound test.
       * i386 values occupy 32-bit slots in this encoder, so a constant limit
       * larger than UINT32_MAX is unconditionally above every non-negative
       * index representable here.  Keep this inline: a helper call could
       * clobber values that remain live across the check.
       */
      if (!ny_i386_obj_load_value_eax(c, in->b) ||
          !ny_i386_obj_bytes(
              c,
              (const unsigned char[]){0x83, 0xf8, 0x00, 0x7d, 0x02, 0x0f, 0x0b},
              7))
        return false;
      if (in->c >= 0) {
        if (!ny_i386_obj_load_value_ebx(c, in->c) ||
            !ny_i386_obj_bytes(
                c, (const unsigned char[]){0x85, 0xdb, 0x7d, 0x02, 0x0f, 0x0b},
                6) ||
            !ny_i386_obj_bytes(
                c, (const unsigned char[]){0x39, 0xd8, 0x72, 0x02, 0x0f, 0x0b},
                6))
          return false;
      } else if (in->imm < 0) {
        if (!ny_i386_obj_bytes(c, (const unsigned char[]){0x0f, 0x0b}, 2))
          return false;
      } else if ((uint64_t)in->imm <= UINT32_MAX) {
        if (!ny_i386_obj_u8(c, 0x3d) ||
            !ny_i386_obj_i32(c, (int32_t)(uint32_t)in->imm) ||
            !ny_i386_obj_bytes(
                c, (const unsigned char[]){0x72, 0x02, 0x0f, 0x0b}, 4))
          return false;
      }
      break;
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
      nyr_set_error(c->err, c->err_len,
                    "i386 ELF object writer: unsupported op %s",
                    nyr_op_name(in->op));
      return false;
    }
  }
  size_t epilogue_off = c->code.len;
  if (tag_return &&
      (!ny_i386_obj_bytes(c, (const unsigned char[]){0x8d, 0x04, 0x45}, 3) ||
       !ny_i386_obj_i32(c, 1)))
    return false;
  if (!ny_i386_obj_bytes(
          c, (const unsigned char[]){0x8b, 0x5d, 0xfc, 0xc9, 0xc3}, 5))
    return false;
  return ny_i386_obj_patch_branches(c, epilogue_off);
}

static bool ny_x64_obj_bytes(nyr_x64_obj_ctx_t *c, const unsigned char *p,
                             size_t len) {
  if (!nyr_obj_emit(&c->code, p, len)) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: out of memory");
    return false;
  }
  return true;
}

static bool ny_x64_obj_u8(nyr_x64_obj_ctx_t *c, unsigned v) {
  return ny_x64_obj_bytes(c, (const unsigned char *)&(unsigned char){v}, 1);
}

static bool ny_x64_obj_i32(nyr_x64_obj_ctx_t *c, int32_t v) {
  unsigned char b[4] = {
      (unsigned char)(v & 0xff), (unsigned char)((uint32_t)v >> 8),
      (unsigned char)((uint32_t)v >> 16), (unsigned char)((uint32_t)v >> 24)};
  return ny_x64_obj_bytes(c, b, sizeof(b));
}

static bool ny_x64_obj_i64(nyr_x64_obj_ctx_t *c, int64_t v) {
  for (int i = 0; i < 8; ++i) {
    if (!ny_x64_obj_u8(c, (unsigned)(((uint64_t)v >> (i * 8)) & 0xff)))
      return false;
  }
  return true;
}

static int ny_x64_obj_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static int ny_x64_obj_value_off(const nyr_x64_obj_ctx_t *c, int value) {
  int slot = c->value_spill ? c->value_spill[value] : value;
  return -8 * (slot + 1);
}

static int ny_x64_obj_local_off(const nyr_x64_obj_ctx_t *c, int local) {
  return -8 * (c->spill_slots + c->callee_save_slots + local + 1);
}

#define NY_X64_EBP_OP(name, ...)                                               \
  static bool name(nyr_x64_obj_ctx_t *c, int off) {                            \
    static const unsigned char op[] = {__VA_ARGS__};                           \
    return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);      \
  }
NY_X64_EBP_OP(ny_x64_obj_load_rax, 0x48, 0x8b, 0x85)
NY_X64_EBP_OP(ny_x64_obj_lea_rax, 0x48, 0x8d, 0x85)
NY_X64_EBP_OP(ny_x64_obj_store_rax, 0x48, 0x89, 0x85)
NY_X64_EBP_OP(ny_x64_obj_load_r10, 0x4c, 0x8b, 0x95)
NY_X64_EBP_OP(ny_x64_obj_load_rcx, 0x48, 0x8b, 0x8d)
#undef NY_X64_EBP_OP

static bool ny_x64_obj_mov_rax_imm(nyr_x64_obj_ctx_t *c, int64_t v) {
  static const unsigned char op[] = {0x48, 0xb8};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i64(c, v);
}

static bool ny_x64_obj_lea_reg(nyr_x64_obj_ctx_t *c, int reg, int off) {
  unsigned char op[] = {(unsigned char)(0x48 | (reg >= 8 ? 0x04 : 0)), 0x8d,
                        (unsigned char)(0x85 | ((reg & 7) << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_mov_reg_reg(nyr_x64_obj_ctx_t *c, int src, int dst) {
  if (src < 0 || src > 15 || dst < 0 || dst > 15)
    return false;
  unsigned char op[] = {
      (unsigned char)(0x48 | (src >= 8 ? 0x04 : 0) | (dst >= 8 ? 0x01 : 0)),
      0x89, (unsigned char)(0xc0 | ((src & 7) << 3) | (dst & 7))};
  return ny_x64_obj_bytes(c, op, sizeof(op));
}

static bool ny_x64_obj_store_reg(nyr_x64_obj_ctx_t *c, int reg, int off) {
  unsigned char op[] = {(unsigned char)(0x48 | (reg >= 8 ? 0x04 : 0)), 0x89,
                        (unsigned char)(0x85 | ((reg & 7) << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_reg(nyr_x64_obj_ctx_t *c, int reg, int off) {
  unsigned char op[] = {(unsigned char)(0x48 | (reg >= 8 ? 0x04 : 0)), 0x8b,
                        (unsigned char)(0x85 | ((reg & 7) << 3))};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_value_rax(nyr_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, c->value_reg[value], 0);
  if (c->value_immediate && c->value_immediate[value]) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 object writer: immediate-only v%d was loaded", value);
    return false;
  }
  return ny_x64_obj_load_rax(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_load_value_r10(nyr_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, c->value_reg[value], 10);
  if (c->value_immediate && c->value_immediate[value]) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 object writer: immediate-only v%d was loaded", value);
    return false;
  }
  return ny_x64_obj_load_r10(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_load_value_rcx(nyr_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, c->value_reg[value], 1);
  if (c->value_immediate && c->value_immediate[value]) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 object writer: immediate-only v%d was loaded", value);
    return false;
  }
  return ny_x64_obj_load_rcx(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_store_value_rax(nyr_x64_obj_ctx_t *c, int value) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid destination v%d", value);
    return false;
  }
  if (c->value_reg && c->value_reg[value] >= 0)
    return ny_x64_obj_mov_reg_reg(c, 0, c->value_reg[value]);
  return ny_x64_obj_store_rax(c, ny_x64_obj_value_off(c, value));
}

static bool ny_x64_obj_load_xmm(nyr_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 15)
    return false;
  unsigned char modrm = (unsigned char)(0x85 | ((xmm & 7) << 3));
  if (xmm >= 8) {
    unsigned char op[] = {0xf2, 0x44, 0x0f, 0x10, modrm};
    return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
  }
  unsigned char op[] = {0xf2, 0x0f, 0x10, modrm};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_store_xmm(nyr_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 15)
    return false;
  unsigned char modrm = (unsigned char)(0x85 | ((xmm & 7) << 3));
  if (xmm >= 8) {
    unsigned char op[] = {0xf2, 0x44, 0x0f, 0x11, modrm};
    return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
  }
  unsigned char op[] = {0xf2, 0x0f, 0x11, modrm};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_load_xmm_f32(nyr_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 15)
    return false;
  unsigned char modrm = (unsigned char)(0x85 | ((xmm & 7) << 3));
  if (xmm >= 8) {
    unsigned char op[] = {0xf3, 0x44, 0x0f, 0x10, modrm};
    return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
  }
  unsigned char op[] = {0xf3, 0x0f, 0x10, modrm};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

static bool ny_x64_obj_store_xmm_f32(nyr_x64_obj_ctx_t *c, int off, int xmm) {
  if (xmm < 0 || xmm > 15)
    return false;
  unsigned char modrm = (unsigned char)(0x85 | ((xmm & 7) << 3));
  if (xmm >= 8) {
    unsigned char op[] = {0xf3, 0x44, 0x0f, 0x11, modrm};
    return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
  }
  unsigned char op[] = {0xf3, 0x0f, 0x11, modrm};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, off);
}

/*
 * Register-indirect SIMD move: movsd xmm, [base] / movsd [base], xmm. Used
 * for f64-class loads/stores whose address lives in a general register (the
 * common `buf + index*8` buffer access pattern), avoiding an integer->xmm
 * round-trip through rax.
 */
static bool ny_x64_obj_load_xmm_reg(nyr_x64_obj_ctx_t *c, int xmm, int base) {
  if (xmm < 0 || xmm > 15 || base < 0 || base > 15)
    return false;
  unsigned char modrm = (unsigned char)(((xmm & 7) << 3) | (base & 7));
  unsigned char rex =
      (unsigned char)(0x40 | (xmm >= 8 ? 0x04 : 0) | (base >= 8 ? 0x01 : 0));
  unsigned char op[] = {0xf2, rex, 0x0f, 0x10, modrm};
  return ny_x64_obj_bytes(c, op, sizeof(op));
}
static bool ny_x64_obj_store_xmm_reg(nyr_x64_obj_ctx_t *c, int xmm, int base) {
  if (xmm < 0 || xmm > 15 || base < 0 || base > 15)
    return false;
  unsigned char modrm = (unsigned char)(((xmm & 7) << 3) | (base & 7));
  unsigned char rex =
      (unsigned char)(0x40 | (xmm >= 8 ? 0x04 : 0) | (base >= 8 ? 0x01 : 0));
  unsigned char op[] = {0xf2, rex, 0x0f, 0x11, modrm};
  return ny_x64_obj_bytes(c, op, sizeof(op));
}

static bool ny_x64_obj_mov_xmm(nyr_x64_obj_ctx_t *c, int src, int dst,
                               bool f32) {
  if (src < 0 || src > 15 || dst < 0 || dst > 15)
    return false;
  unsigned char modrm = (unsigned char)(0xc0 | ((dst & 7) << 3) | (src & 7));
  unsigned char rex =
      (unsigned char)(0x40 | ((dst & 8) ? 0x04 : 0) | ((src & 8) ? 0x01 : 0));
  if (rex != 0x40) {
    unsigned char op[] = {(unsigned char)(f32 ? 0xf3 : 0xf2), rex, 0x0f, 0x10,
                          modrm};
    return ny_x64_obj_bytes(c, op, sizeof(op));
  }
  unsigned char op[] = {(unsigned char)(f32 ? 0xf3 : 0xf2), 0x0f, 0x10, modrm};
  return ny_x64_obj_bytes(c, op, sizeof(op));
}

static bool ny_x64_obj_store_float_bits(nyr_x64_obj_ctx_t *c, int value,
                                        bool f32) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid destination v%d", value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0) {
    int xmm = c->value_xmm[value];
    unsigned char modrm = (unsigned char)(0xc0 | ((xmm & 7) << 3));
    if (xmm >= 8) {
      unsigned char op64[] = {0x66, 0x48, 0x44, 0x0f, 0x6e, modrm};
      unsigned char op32[] = {0x66, 0x44, 0x0f, 0x6e, modrm};
      return f32 ? ny_x64_obj_bytes(c, op32, sizeof(op32))
                 : ny_x64_obj_bytes(c, op64, sizeof(op64));
    }
    unsigned char op64[] = {0x66, 0x48, 0x0f, 0x6e, modrm};
    unsigned char op32[] = {0x66, 0x0f, 0x6e, modrm};
    return f32 ? ny_x64_obj_bytes(c, op32, sizeof(op32))
               : ny_x64_obj_bytes(c, op64, sizeof(op64));
  }
  return ny_x64_obj_store_value_rax(c, value);
}

static bool ny_x64_obj_load_value_xmm(nyr_x64_obj_ctx_t *c, int value,
                                      int xmm) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, c->value_xmm[value], xmm, false);
  return ny_x64_obj_load_xmm(c, ny_x64_obj_value_off(c, value), xmm);
}

static bool ny_x64_obj_load_value_xmm_f32(nyr_x64_obj_ctx_t *c, int value,
                                          int xmm) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid value v%d", value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, c->value_xmm[value], xmm, true);
  return ny_x64_obj_load_xmm_f32(c, ny_x64_obj_value_off(c, value), xmm);
}

static bool ny_x64_obj_store_value_xmm_f32(nyr_x64_obj_ctx_t *c, int value,
                                           int xmm) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid destination v%d", value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, xmm, c->value_xmm[value], true);
  return ny_x64_obj_store_xmm_f32(c, ny_x64_obj_value_off(c, value), xmm);
}

static bool ny_x64_obj_store_value_xmm(nyr_x64_obj_ctx_t *c, int value,
                                       int xmm) {
  if (value < 0 || value >= c->value_slots) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: invalid destination v%d", value);
    return false;
  }
  if (c->value_xmm && c->value_xmm[value] >= 0)
    return c->value_xmm[value] == xmm
               ? true
               : ny_x64_obj_mov_xmm(c, xmm, c->value_xmm[value], false);
  return ny_x64_obj_store_xmm(c, ny_x64_obj_value_off(c, value), xmm);
}

/*
 * Table mapping value ID to its defining instruction index in the NYIR
 * function. Built once per function emission, enables O(1) constant detection.
 */
static void ny_x64_obj_valmap_init(nyr_x64_obj_valmap_t *m,
                                   const nyr_func_t *nyir) {
  m->count = 0;
  m->defs = NULL;
  if (!nyir || nyir->len == 0)
    return;
  int max_v = 0;
  for (size_t i = 0; i < nyir->len; ++i) {
    if (nyir->data[i].dst >= 0 && nyir->data[i].dst > max_v)
      max_v = nyir->data[i].dst;
  }
  m->count = max_v + 1;
  m->defs =
      (const nyr_inst_t **)calloc((size_t)m->count, sizeof(nyr_inst_t *));
  if (!m->defs) {
    m->count = 0;
    return;
  }
  for (size_t i = 0; i < nyir->len; ++i) {
    int v = nyir->data[i].dst;
    if (v >= 0 && v < m->count)
      m->defs[v] = &nyir->data[i];
  }
}

static void ny_x64_obj_valmap_free(nyr_x64_obj_valmap_t *m) {
  free(m->defs);
  m->defs = NULL;
  m->count = 0;
}

static const nyr_inst_t *ny_x64_obj_valmap_def(nyr_x64_obj_valmap_t *m,
                                                int v) {
  if (v < 0 || v >= m->count || !m->defs)
    return NULL;
  return m->defs[v];
}

/*
 * Check if value `v` is defined by a CONST_I64 instruction with immediate
 * `imm`. Returns true and sets *out_imm on success.
 */
static bool ny_x64_obj_try_const_i64(nyr_x64_obj_ctx_t *c, int v,
                                     int64_t *out_imm) {
  if (v < 0)
    return false;
  const nyr_inst_t *inst = ny_x64_obj_valmap_def(&c->valmap, v);
  if (!inst || inst->op != NYIR_CONST_I64)
    return false;
  if (out_imm)
    *out_imm = inst->imm;
  return true;
}

/*
 * Emit `op $imm, %rax` (sign-extended 32-bit immediate in %eax).
 * Returns true if the immediate fits in 32 bits.
 */
static bool ny_x64_obj_alu_imm_rax(nyr_x64_obj_ctx_t *c, int64_t imm,
                                   const unsigned char *op_bytes,
                                   size_t op_len) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  return ny_x64_obj_bytes(c, op_bytes, op_len) &&
         ny_x64_obj_i32(c, (int32_t)imm);
}

/*
 * Emit `cmp $imm, %rax` (comparison against immediate).
 */
static bool ny_x64_obj_cmp_imm_rax(nyr_x64_obj_ctx_t *c, int64_t imm) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  static const unsigned char op[] = {0x48, 0x3d};
  return ny_x64_obj_bytes(c, op, sizeof(op)) && ny_x64_obj_i32(c, (int32_t)imm);
}

/*
 * Emit `add $imm, %rax` (or sub, and, or, xor with immediate).
 */
static bool ny_x64_obj_binop_imm(nyr_x64_obj_ctx_t *c, int64_t imm,
                                 nyr_op_t binop) {
  switch (binop) {
  case NYIR_ADD_I64: {
    static const unsigned char op[] = {0x48, 0x05};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NYIR_SUB_I64: {
    static const unsigned char op[] = {0x48, 0x2d};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NYIR_AND_I64: {
    static const unsigned char op[] = {0x48, 0x25};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NYIR_OR_I64: {
    static const unsigned char op[] = {0x48, 0x0d};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  case NYIR_XOR_I64: {
    static const unsigned char op[] = {0x48, 0x35};
    return ny_x64_obj_alu_imm_rax(c, imm, op, sizeof(op));
  }
  default:
    return false;
  }
}

static int ny_x64_obj_alu_opcode(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
    return 0x03;
  case NYIR_SUB_I64:
    return 0x2b;
  case NYIR_AND_I64:
    return 0x23;
  case NYIR_OR_I64:
    return 0x0b;
  case NYIR_XOR_I64:
    return 0x33;
  default:
    return -1;
  }
}

static int ny_x64_obj_alu_imm_group(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
    return 0;
  case NYIR_OR_I64:
    return 1;
  case NYIR_AND_I64:
    return 4;
  case NYIR_SUB_I64:
    return 5;
  case NYIR_XOR_I64:
    return 6;
  default:
    return -1;
  }
}

static bool ny_x64_obj_alu_reg_source(nyr_x64_obj_ctx_t *c, nyr_op_t op,
                                      int dst_reg, int src_reg, int src_off,
                                      bool src_is_reg) {
  unsigned char rex = (unsigned char)(0x48 | (dst_reg >= 8 ? 0x04 : 0) |
                                      (src_is_reg && src_reg >= 8 ? 0x01 : 0));
  unsigned char modrm =
      (unsigned char)((src_is_reg ? 0xc0 : 0x85) | ((dst_reg & 7) << 3) |
                      (src_is_reg ? src_reg & 7 : 0));
  if (op == NYIR_MUL_I64) {
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

static bool ny_x64_obj_alu_reg_imm(nyr_x64_obj_ctx_t *c, nyr_op_t op,
                                   int dst_reg, int64_t imm) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  unsigned char rex = (unsigned char)(0x48 | (dst_reg >= 8 ? 0x01 : 0));
  if (op == NYIR_MUL_I64) {
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

static bool ny_x64_obj_try_direct_binop(nyr_x64_obj_ctx_t *c,
                                        const nyr_inst_t *in, bool *handled) {
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
  bool commutative = in->op == NYIR_ADD_I64 || in->op == NYIR_MUL_I64 ||
                     in->op == NYIR_AND_I64 || in->op == NYIR_OR_I64 ||
                     in->op == NYIR_XOR_I64;
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
  if (ny_x64_obj_try_const_i64(c, source, &imm) && imm >= INT32_MIN &&
      imm <= INT32_MAX)
    return ny_x64_obj_alu_reg_imm(c, in->op, dst, imm);
  if (source_reg >= 0)
    return ny_x64_obj_alu_reg_source(c, in->op, dst, source_reg, 0, true);
  return ny_x64_obj_alu_reg_source(c, in->op, dst, 0,
                                   ny_x64_obj_value_off(c, source), false);
}

static bool ny_x64_obj_binop(nyr_x64_obj_ctx_t *c, const nyr_inst_t *in,
                             const unsigned char *op, size_t op_len) {
  bool handled = false;
  if (!ny_x64_obj_try_direct_binop(c, in, &handled))
    return false;
  if (handled)
    return true;
  int64_t imm = 0;
  /*
   * Try immediate operand for B: load A, op $imm, store.
   * We detect the operation from the opcode's last byte to pick the
   * correct immediate encoding.
   */
  if (ny_x64_obj_try_const_i64(c, in->b, &imm) && imm >= INT32_MIN &&
      imm <= INT32_MAX && ny_x64_obj_load_value_rax(c, in->a) &&
      ny_x64_obj_binop_imm(c, imm, in->op) &&
      ny_x64_obj_store_value_rax(c, in->dst))
    return true;
  if (in->op != NYIR_SUB_I64 && ny_x64_obj_try_const_i64(c, in->a, &imm) &&
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

static bool ny_x64_obj_reserve_labels(nyr_x64_obj_ctx_t *c, size_t want) {
  if (want <= c->label_cap)
    return true;
  size_t cap = c->label_cap ? c->label_cap : 64;
  while (cap < want) {
    if (cap > SIZE_MAX / 2) {
      cap = want;
      break;
    }
    cap *= 2;
  }
  if (cap > SIZE_MAX / sizeof(*c->labels)) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: label storage too large");
    return false;
  }
  nyr_x64_obj_label_t *labels = realloc(c->labels, cap * sizeof(*labels));
  if (!labels) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: label allocation failed");
    return false;
  }
  c->labels = labels;
  c->label_cap = cap;
  return true;
}

static bool ny_x64_obj_reserve_patches(nyr_x64_obj_ctx_t *c, size_t want) {
  if (want <= c->patch_cap)
    return true;
  size_t cap = c->patch_cap ? c->patch_cap : 64;
  while (cap < want) {
    if (cap > SIZE_MAX / 2) {
      cap = want;
      break;
    }
    cap *= 2;
  }
  if (cap > SIZE_MAX / sizeof(*c->patches)) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: patch storage too large");
    return false;
  }
  nyr_x64_obj_patch_t *patches = realloc(c->patches, cap * sizeof(*patches));
  if (!patches) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 ELF object writer: patch allocation failed");
    return false;
  }
  c->patches = patches;
  c->patch_cap = cap;
  return true;
}

static bool ny_x64_obj_add_label(nyr_x64_obj_ctx_t *c, int64_t label) {
  if (!ny_x64_obj_reserve_labels(c, c->label_count + 1))
    return false;
  for (size_t i = 0; i < c->label_count; ++i) {
    if (c->labels[i].label == label) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 ELF object writer: duplicate label L%lld",
                    (long long)label);
      return false;
    }
  }
  c->labels[c->label_count++] =
      (nyr_x64_obj_label_t){.label = label, .off = c->code.len};
  return true;
}

static bool ny_x64_obj_add_patch(nyr_x64_obj_ctx_t *c, int64_t label,
                                 size_t disp_off) {
  if (!ny_x64_obj_reserve_patches(c, c->patch_count + 1))
    return false;
  c->patches[c->patch_count++] =
      (nyr_x64_obj_patch_t){.label = label, .disp_off = disp_off};
  return true;
}

static bool ny_x64_obj_reloc_symbol(char *out, size_t out_len,
                                    const nyr_x64_obj_ctx_t *c,
                                    const char *symbol) {
  const char *prefix = c && c->target && c->target->symbol_prefix
                           ? c->target->symbol_prefix
                           : "";
  return ny_obj_reloc_symbol(out, out_len, prefix, symbol);
}

static bool ny_x64_obj_add_reloc(nyr_x64_obj_ctx_t *c, const char *symbol,
                                 size_t disp_off, int type) {
  if (c->reloc_count == c->reloc_cap) {
    size_t cap = c->reloc_cap ? c->reloc_cap * 2u : 64u;
    if (cap < c->reloc_cap || cap > NY_X64_OBJ_MAX_RELOCS) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: relocation transport too large");
      return false;
    }
    nyr_x64_obj_reloc_t *grown = realloc(c->relocs, cap * sizeof(*grown));
    if (!grown) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: relocation allocation failed");
      return false;
    }
    c->relocs = grown;
    c->reloc_cap = cap;
  }
  snprintf(c->relocs[c->reloc_count].symbol, sizeof(c->relocs[0].symbol), "%s",
           symbol ? symbol : "");
  c->relocs[c->reloc_count].disp_off = disp_off;
  c->relocs[c->reloc_count].type = type;
  c->reloc_count++;
  return true;
}

static bool ny_x64_obj_mov_rax_to_arg(nyr_x64_obj_ctx_t *c, const char *reg) {
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
  nyr_set_error(c->err, c->err_len,
                "x86-64 object writer: unsupported arg register %s", reg);
  return false;
}

static int ny_x64_obj_arg_reg_code(const char *reg) {
  if (!reg)
    return -1;
  if (strcmp(reg, "%rdi") == 0)
    return 7;
  if (strcmp(reg, "%rsi") == 0)
    return 6;
  if (strcmp(reg, "%rdx") == 0)
    return 2;
  if (strcmp(reg, "%rcx") == 0)
    return 1;
  if (strcmp(reg, "%r8") == 0)
    return 8;
  if (strcmp(reg, "%r9") == 0)
    return 9;
  return -1;
}

static bool ny_x64_obj_load_aggregate_gp(nyr_x64_obj_ctx_t *c, int offset,
                                         const char *reg) {
  int code = ny_x64_obj_arg_reg_code(reg);
  if (code < 0 || offset < 0 || offset > 127)
    return false;
  unsigned char bytes[] = {(unsigned char)(0x48 | ((code & 8) ? 0x04 : 0)),
                           0x8b, (unsigned char)(0x40 | ((code & 7) << 3)),
                           (unsigned char)offset};
  return ny_x64_obj_bytes(c, bytes, sizeof(bytes));
}

static bool ny_x64_obj_load_aggregate_sse(nyr_x64_obj_ctx_t *c, int offset,
                                          int xmm) {
  if (xmm < 0 || xmm > 7 || offset < 0 || offset > 127)
    return false;
  unsigned char bytes[] = {0xf3, 0x0f, 0x7e, (unsigned char)(0x40 | (xmm << 3)),
                           (unsigned char)offset};
  return ny_x64_obj_bytes(c, bytes, sizeof(bytes));
}

static bool ny_x64_obj_sub_rsp(nyr_x64_obj_ctx_t *c, size_t bytes) {
  if (bytes == 0)
    return true;
  if (bytes <= 127)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x83, 0xec}, 3) &&
           ny_x64_obj_u8(c, (unsigned)bytes);
  if (bytes <= INT32_MAX)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xec}, 3) &&
           ny_x64_obj_i32(c, (int32_t)bytes);
  nyr_set_error(c->err, c->err_len,
                "x86-64 object writer: stack adjustment too large");
  return false;
}

static bool ny_x64_obj_add_rsp(nyr_x64_obj_ctx_t *c, size_t bytes) {
  if (bytes == 0)
    return true;
  if (bytes <= 127)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x83, 0xc4}, 3) &&
           ny_x64_obj_u8(c, (unsigned)bytes);
  if (bytes <= INT32_MAX)
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xc4}, 3) &&
           ny_x64_obj_i32(c, (int32_t)bytes);
  nyr_set_error(c->err, c->err_len,
                "x86-64 object writer: stack adjustment too large");
  return false;
}

bool nyr_x64_obj_emit_code(nyr_x64_obj_ctx_t *c, const nyr_func_t *nyir,
                           bool tag_return);
static bool ny_x64_obj_emit_epilogue(nyr_x64_obj_ctx_t *c);

int nyr_x64_obj_symbol_index(char symbols[][256], size_t count,
                             const char *name) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(symbols[i], name) == 0)
      return (int)i;
  }
  return -1;
}

bool nyr_x64_obj_collect_external_reloc_symbols(
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count, char symbols[][256],
    size_t *symbol_count, char *err, size_t err_len) {
  size_t count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].symbol[0]) {
      nyr_set_error(err, err_len,
                    "x86-64 object writer: empty relocation symbol");
      return false;
    }
    if (nyr_x64_obj_def_index(defs, def_count, relocs[i].symbol) >= 0)
      continue;
    if (nyr_x64_obj_symbol_index(symbols, count, relocs[i].symbol) >= 0)
      continue;
    if (count >= NY_X64_OBJ_MAX_RELOCS) {
      nyr_set_error(err, err_len,
                    "x86-64 object writer: too many relocation symbols");
      return false;
    }
    snprintf(symbols[count++], 256, "%s", relocs[i].symbol);
  }
  *symbol_count = count;
  return true;
}

bool nyr_x64_obj_append_function_debug(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, const nyr_func_t *nyir,
    const ny_native_target_info_t *target, const char *symbol_name,
    bool tag_return, const nyr_func_t *source,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
  if (!code || !defs || !def_count || !relocs || !reloc_count || !nyir ||
      !target || !symbol_name || !symbol_name[0]) {
    nyr_set_error(err, err_len, "x86-64 object writer: missing function input");
    return false;
  }
  if (*def_count >= NY_NATIVE_MAX_DEFS) {
    nyr_set_error(err, err_len, "x86-64 object writer: too many functions");
    return false;
  }
  if (nyr_x64_obj_def_index(defs, *def_count, symbol_name) >= 0) {
    nyr_set_error(err, err_len, "x86-64 object writer: duplicate symbol %s",
                  symbol_name);
    return false;
  }
  if (!nyr_obj_pad_to(code, 16)) {
    nyr_set_error(err, err_len, "x86-64 object writer: out of memory");
    return false;
  }
  size_t base = code->len;
  nyr_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!nyr_x64_obj_emit_code(&ctx, nyir, tag_return)) {
    nyr_x64_obj_ctx_free(&ctx);
    return false;
  }
  if (*reloc_count + ctx.reloc_count > NY_X64_OBJ_MAX_RELOCS) {
    nyr_set_error(err, err_len, "x86-64 object writer: too many relocations");
    nyr_x64_obj_ctx_free(&ctx);
    return false;
  }
  if (!nyr_obj_emit(code, ctx.code.data, ctx.code.len)) {
    nyr_set_error(err, err_len, "x86-64 object writer: out of memory");
    nyr_x64_obj_ctx_free(&ctx);
    return false;
  }
  nyr_x64_obj_symbol_def_t *def = &defs[(*def_count)++];
  snprintf(def->name, sizeof(def->name), "%s", symbol_name);
  def->off = base;
  def->size = ctx.code.len;
  for (size_t i = 0; i < ctx.reloc_count; ++i) {
    relocs[*reloc_count] = ctx.relocs[i];
    relocs[*reloc_count].disp_off += base;
    (*reloc_count)++;
  }
  if (source && debug_lines && debug_line_count && debug_line_cap) {
    size_t map_len = ctx.inst_offsets ? source->len : 0;
    if (map_len > nyir->len)
      map_len = nyir->len;
    for (size_t i = 0; i < map_len; ++i) {
      const nyr_debug_loc_t *loc = &source->data[i].debug;
      if (!loc->line || ctx.inst_offsets[i] == SIZE_MAX)
        continue;
      if (!nyr_native_debug_line_append(
              debug_lines, debug_line_count, debug_line_cap,
              (nyr_native_debug_line_t){*def_count - 1,
                                        base + ctx.inst_offsets[i], loc->file,
                                        loc->line, loc->column})) {
        nyr_set_error(err, err_len, "x86-64 source map allocation failed");
        nyr_x64_obj_ctx_free(&ctx);
        return false;
      }
    }
  }
  nyr_x64_obj_ctx_free(&ctx);
  return true;
}

bool nyr_x64_obj_append_function(nyr_obj_buf_t *code,
                                 nyr_x64_obj_symbol_def_t *defs,
                                 size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                                 size_t *reloc_count, const nyr_func_t *nyir,
                                 const ny_native_target_info_t *target,
                                 const char *symbol_name, bool tag_return,
                                 char *err, size_t err_len) {
  return nyr_x64_obj_append_function_debug(
      code, defs, def_count, relocs, reloc_count, nyir, target, symbol_name,
      tag_return, NULL, NULL, NULL, NULL, err, err_len);
}

bool nyr_x64_obj_collect_reloc_symbols(const nyr_x64_obj_reloc_t *relocs,
                                       size_t reloc_count, char symbols[][256],
                                       size_t *symbol_count, char *err,
                                       size_t err_len) {
  size_t count = 0;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].symbol[0]) {
      nyr_set_error(err, err_len,
                    "x86-64 object writer: empty relocation symbol");
      return false;
    }
    if (nyr_x64_obj_symbol_index(symbols, count, relocs[i].symbol) >= 0)
      continue;
    if (count >= NY_X64_OBJ_MAX_RELOCS) {
      nyr_set_error(err, err_len,
                    "x86-64 object writer: too many relocation symbols");
      return false;
    }
    snprintf(symbols[count++], 256, "%s", relocs[i].symbol);
  }
  *symbol_count = count;
  return true;
}

static bool ny_x64_obj_patch_branches(nyr_x64_obj_ctx_t *c) {
  for (size_t i = 0; i < c->patch_count; ++i) {
    nyr_x64_obj_patch_t *p = &c->patches[i];
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
      nyr_set_error(c->err, c->err_len,
                    "x86-64 ELF object writer: unresolved label L%lld",
                    (long long)p->label);
      return false;
    }
    int64_t rel = (int64_t)target - (int64_t)(p->disp_off + 4);
    if (rel < INT32_MIN || rel > INT32_MAX) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 ELF object writer: branch out of range");
      return false;
    }
    nyr_obj_patch_u32(&c->code, p->disp_off, (uint32_t)(int32_t)rel);
  }
  return true;
}

static unsigned ny_x64_obj_setcc(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return 0x94;
  case NYIR_CMP_NE:
    return 0x95;
  case NYIR_CMP_LT:
    return 0x9c;
  case NYIR_CMP_LE:
    return 0x9e;
  case NYIR_CMP_GT:
    return 0x9f;
  case NYIR_CMP_GE:
    return 0x9d;
  }
  return 0x94;
}

static unsigned ny_x64_obj_f64_setcc(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return 0x94; /* sete */
  case NYIR_CMP_NE:
    return 0x95; /* setne */
  case NYIR_CMP_LT:
    return 0x92; /* setb */
  case NYIR_CMP_LE:
    return 0x96; /* setbe */
  case NYIR_CMP_GT:
    return 0x97; /* seta */
  case NYIR_CMP_GE:
    return 0x93; /* setae */
  }
  return 0x94;
}

/*
 * Signed division by a positive power of two truncates toward zero only after
 * a sign correction. Restrict the mask to imm32 so the encoder never relies
 * on a sign-extended immediate for a wider mask.
 */
static bool ny_x64_obj_try_pow2_divisor(nyr_x64_obj_ctx_t *c, int value,
                                        unsigned *shift_out) {
  int64_t divisor = 0;
  if (!shift_out || !ny_x64_obj_try_const_i64(c, value, &divisor) ||
      divisor <= 1 || divisor > INT32_MAX ||
      ((uint64_t)divisor & ((uint64_t)divisor - 1)) != 0)
    return false;
  unsigned shift = 0;
  while (divisor > 1) {
    divisor >>= 1;
    shift++;
  }
  if (shift >= 31)
    return false;
  *shift_out = shift;
  return true;
}

static bool ny_x64_obj_emit_pow2_divmod(nyr_x64_obj_ctx_t *c,
                                        const nyr_inst_t *in, unsigned shift) {
  uint32_t mask = ((uint32_t)1 << shift) - 1;
  if (!ny_x64_obj_load_value_rax(c, in->a))
    return false;
  if (in->op == NYIR_DIV_I64) {
    return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc1}, 3) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, 0xf9, 0x3f},
                            4) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xe1}, 3) &&
           ny_x64_obj_i32(c, (int32_t)mask) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x01, 0xc8}, 3) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, 0xf8}, 3) &&
           ny_x64_obj_u8(c, shift) && ny_x64_obj_store_value_rax(c, in->dst);
  }
  if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc1}, 3))
    return false; /* mov %rax, %rcx */
  return ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc2}, 3) &&
         ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, 0xfa, 0x3f},
                          4) &&
         ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xe2}, 3) &&
         ny_x64_obj_i32(c, (int32_t)mask) &&
         ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x01, 0xd0}, 3) &&
         ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, 0xf8}, 3) &&
         ny_x64_obj_u8(c, shift) &&
         ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, 0xe0}, 3) &&
         ny_x64_obj_u8(c, shift) &&
         ny_x64_obj_bytes(
             c, (const unsigned char[]){0x48, 0x29, 0xc1, 0x48, 0x89, 0xc8},
             6) &&
         ny_x64_obj_store_value_rax(c, in->dst);
}

static bool ny_x64_obj_emit_sdiv_magic(nyr_x64_obj_ctx_t *c,
                                       const nyr_inst_t *in, int64_t divisor,
                                       bool want_rem) {
  uint64_t ad =
      divisor >= 0 ? (uint64_t)divisor : (uint64_t)(-(divisor + 1)) + 1;
  int64_t magic = 0;
  unsigned s = 0;
  nyr_sdiv_magic_i64(ad, &magic, &s);
  bool negate = divisor < 0;

  if (!ny_x64_obj_load_value_rax(c, in->a))
    return false;
  if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0x89, 0xc1}, 3))
    return false;
  if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0xba}, 2) ||
      !ny_x64_obj_i64(c, magic))
    return false;
  if (!ny_x64_obj_bytes(
          c, (const unsigned char[]){0x49, 0xf7, 0xea, 0x48, 0x89, 0xd0}, 6))
    return false;
  if (magic < 0) {
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x4c, 0x01, 0xc8}, 3))
      return false;
  }
  if (s > 0) {
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, 0xf8}, 3) ||
        !ny_x64_obj_u8(c, (unsigned char)s))
      return false;
  }
  if (!ny_x64_obj_bytes(c,
                        (const unsigned char[]){0x4c, 0x89, 0xc9, 0x48, 0xc1,
                                                0xf9, 0x3f, 0x48, 0x29, 0xc8},
                        10))
    return false;
  if (negate) {
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xf7, 0xd8}, 3))
      return false;
  }
  if (want_rem) {
    if (!ny_x64_obj_bytes(
            c, (const unsigned char[]){0x49, 0x89, 0xc2, 0x4c, 0x89, 0xc8}, 6))
      return false;
    if ((int64_t)(int32_t)ad == (int64_t)ad) {
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x4d, 0x69, 0xd2}, 3) ||
          !ny_x64_obj_i32(c, (int32_t)ad))
        return false;
    } else {
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0xbb}, 2) ||
          !ny_x64_obj_i64(c, (int64_t)ad) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x4d, 0x0f, 0xaf, 0xd3},
                            4))
        return false;
    }
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x4c, 0x29, 0xd0}, 3))
      return false;
  }
  return ny_x64_obj_store_value_rax(c, in->dst);
}

static bool ny_x64_obj_emit_inst(nyr_x64_obj_ctx_t *c, const nyr_inst_t *in) {
  static const unsigned char add[] = {0x4c, 0x01, 0xd0};
  static const unsigned char sub[] = {0x4c, 0x29, 0xd0};
  static const unsigned char imul[] = {0x49, 0x0f, 0xaf, 0xc2};
  static const unsigned char andq[] = {0x4c, 0x21, 0xd0};
  static const unsigned char orq[] = {0x4c, 0x09, 0xd0};
  static const unsigned char xorq[] = {0x4c, 0x31, 0xd0};
  switch (in->op) {
  case NYIR_NOP:
    return true;
  case NYIR_CONST_I64:
    if (c->value_immediate && in->dst >= 0 && in->dst < c->value_slots &&
        c->value_immediate[in->dst])
      return true;
    return ny_x64_obj_mov_rax_imm(c, in->imm) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_CONST_F64:
    return ny_x64_obj_mov_rax_imm(c, in->imm) &&
           ny_x64_obj_store_float_bits(c, in->dst, false);
  case NYIR_CONST_F32:
    return ny_x64_obj_mov_rax_imm(c, in->imm) &&
           ny_x64_obj_store_float_bits(c, in->dst, true);
  case NYIR_COPY: {
    /*
     * Float copies must move through the SSE registers: an f64/f32 value
     * may be register-resident (value_spill[v] == -1), and routing its
     * bits through the integer spill path computes a zero displacement and
     * corrupts the frame.
     */
    bool copy_f64 = (in->a >= 0 && in->a < c->value_slots && c->value_f64 &&
                     c->value_f64[in->a]) ||
                    (in->dst >= 0 && in->dst < c->value_slots && c->value_f64 &&
                     c->value_f64[in->dst]);
    bool copy_f32 = (in->a >= 0 && in->a < c->value_slots && c->value_f32 &&
                     c->value_f32[in->a]) ||
                    (in->dst >= 0 && in->dst < c->value_slots && c->value_f32 &&
                     c->value_f32[in->dst]);
    if (copy_f64)
      return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
             ny_x64_obj_store_value_xmm(c, in->dst, 0);
    if (copy_f32)
      return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
             ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NYIR_ADD_I64:
    return ny_x64_obj_binop(c, in, add, sizeof(add));
  case NYIR_SUB_I64:
    return ny_x64_obj_binop(c, in, sub, sizeof(sub));
  case NYIR_MUL_I64: {
    int64_t imm = 0;
    /*
     * imul $imm, %rax, %rax (0x69 c0 imm32) for small constants
     */
    if (ny_x64_obj_try_const_i64(c, in->b, &imm) && imm >= INT32_MIN &&
        imm <= INT32_MAX && ny_x64_obj_load_value_rax(c, in->a) &&
        ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x69, 0xc0}, 3) &&
        ny_x64_obj_i32(c, (int32_t)imm))
      return ny_x64_obj_store_value_rax(c, in->dst);
    /*
     * Commutative: try A as immediate
     */
    if (ny_x64_obj_try_const_i64(c, in->a, &imm) && imm >= INT32_MIN &&
        imm <= INT32_MAX && ny_x64_obj_load_value_rax(c, in->b) &&
        ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x69, 0xc0}, 3) &&
        ny_x64_obj_i32(c, (int32_t)imm))
      return ny_x64_obj_store_value_rax(c, in->dst);
    return ny_x64_obj_binop(c, in, imul, sizeof(imul));
  }
  case NYIR_AND_I64:
    return ny_x64_obj_binop(c, in, andq, sizeof(andq));
  case NYIR_OR_I64:
    return ny_x64_obj_binop(c, in, orq, sizeof(orq));
  case NYIR_XOR_I64:
    return ny_x64_obj_binop(c, in, xorq, sizeof(xorq));
  case NYIR_DIV_I64:
  case NYIR_MOD_I64: {
    unsigned shift = 0;
    if (ny_x64_obj_try_pow2_divisor(c, in->b, &shift))
      return ny_x64_obj_emit_pow2_divmod(c, in, shift);
    int64_t d = 0;
    if (ny_x64_obj_try_const_i64(c, in->b, &d) && d != 0 && d != 1 && d != -1) {
      uint64_t ad = d >= 0 ? (uint64_t)d : (uint64_t)(-(d + 1)) + 1;
      if ((ad & (ad - 1)) != 0)
        return ny_x64_obj_emit_sdiv_magic(c, in, d, in->op == NYIR_MOD_I64);
    }
    if (!ny_x64_obj_load_value_rax(c, in->a) ||
        !ny_x64_obj_load_value_r10(c, in->b))
      return false;
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x99}, 2) ||
        !ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0xf7, 0xfa}, 3))
      return false;
    if (in->op == NYIR_MOD_I64 &&
        !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xd0}, 3))
      return false;
    return ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NYIR_SHL_I64:
  case NYIR_SAR_I64: {
    int64_t shift_imm = 0;
    bool is_shl = (in->op == NYIR_SHL_I64);
    /*
     * Try immediate shift count: shl $imm, %rax (0xc1 e0/ f8)
     */
    if (ny_x64_obj_try_const_i64(c, in->b, &shift_imm) && shift_imm >= 1 &&
        shift_imm <= 63 && ny_x64_obj_load_value_rax(c, in->a)) {
      unsigned char base = is_shl ? 0xe0 : 0xf8;
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc1, base}, 3) ||
          !ny_x64_obj_u8(c, (unsigned char)shift_imm))
        return false;
      return ny_x64_obj_store_value_rax(c, in->dst);
    }
    /*
     * Fallback: shift by %rcx
     */
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
  case NYIR_SELECT_I64:
    /*
     * Branchless scalar select.  TEST establishes the condition flags; MOV
     * loads do not change them, so load false->rax and true->r10 before the
     * final cmovne r10, rax.
     */
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x85, 0xc0}, 3) &&
           ny_x64_obj_load_value_rax(c, in->c) &&
           ny_x64_obj_load_value_r10(c, in->b) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x49, 0x0f, 0x45, 0xc2},
                            4) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_CMP_I64: {
    int64_t imm = 0;
    /*
     * cmp $imm, value_a if B is constant
     */
    if (ny_x64_obj_try_const_i64(c, in->b, &imm) && imm >= INT32_MIN &&
        imm <= INT32_MAX && ny_x64_obj_load_value_rax(c, in->a) &&
        ny_x64_obj_cmp_imm_rax(c, imm)) {
      if (!ny_x64_obj_u8(c, 0x0f) ||
          !ny_x64_obj_u8(c, ny_x64_obj_setcc(in->cmp)) ||
          !ny_x64_obj_u8(c, 0xc0) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x0f, 0xb6, 0xc0},
                            4))
        return false;
      return ny_x64_obj_store_value_rax(c, in->dst);
    }
    /*
     * For EQ/NE, swap and try A as immediate (commutative)
     */
    if ((in->cmp == NYIR_CMP_EQ || in->cmp == NYIR_CMP_NE) &&
        ny_x64_obj_try_const_i64(c, in->a, &imm) && imm >= INT32_MIN &&
        imm <= INT32_MAX && ny_x64_obj_load_value_rax(c, in->b) &&
        ny_x64_obj_cmp_imm_rax(c, imm)) {
      if (!ny_x64_obj_u8(c, 0x0f) ||
          !ny_x64_obj_u8(c, ny_x64_obj_setcc(in->cmp)) ||
          !ny_x64_obj_u8(c, 0xc0) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x0f, 0xb6, 0xc0},
                            4))
        return false;
      return ny_x64_obj_store_value_rax(c, in->dst);
    }
    /*
     * Fallback: both operands from memory
     */
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
    unsigned char op = in->op == NYIR_ADD_F64   ? 0x58
                       : in->op == NYIR_SUB_F64 ? 0x5c
                       : in->op == NYIR_MUL_F64 ? 0x59
                                                : 0x5e;
    return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm(c, in->b, 1) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf2, 0x0f, op, 0xc1},
                            4) &&
           ny_x64_obj_store_value_xmm(c, in->dst, 0);
  }
  case NYIR_I64_TO_F64:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_bytes(
               c, (const unsigned char[]){0xf2, 0x48, 0x0f, 0x2a, 0xc0}, 5) &&
           ny_x64_obj_store_value_xmm(c, in->dst, 0);
  case NYIR_SQRT_F64:
    return ny_x64_obj_load_value_xmm(c, in->a, 1) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf2, 0x0f, 0x51, 0xc1},
                            4) &&
           ny_x64_obj_store_value_xmm(c, in->dst, 0);
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
    return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
           ny_x64_obj_sub_rsp(c, c->target->shadow_space_bytes) &&
           ny_x64_obj_u8(c, 0xe8) && ny_x64_obj_i32(c, 0) &&
           ny_x64_obj_add_reloc(c, in->op == NYIR_SIN_F64 ? "sin" : "cos",
                                c->code.len - 4, NY_RELOC_PLT32) &&
           ny_x64_obj_add_rsp(c, c->target->shadow_space_bytes) &&
           ny_x64_obj_store_value_xmm(c, in->dst, 0);
  case NYIR_BOUNDS_CHECK: {
    if (!ny_x64_obj_load_value_rax(c, in->b))
      return false;
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc7}, 3))
      return false;
    if (in->c >= 0) {
      if (!ny_x64_obj_load_value_r10(c, in->c))
        return false;
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x4c, 0x89, 0xd6}, 3))
        return false;
    } else {
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xbe}, 2) ||
          !ny_x64_obj_i64(c, in->imm))
        return false;
    }
    /*
     * Inline unsigned trap: cmp rdi(value), rsi(limit); jae ok; ud2.
     * A call would clobber caller-saved registers holding live values.
     */
    return ny_x64_obj_bytes(
        c, (const unsigned char[]){0x48, 0x39, 0xf7, 0x72, 0x02, 0x0f, 0x0b},
        7);
  }
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32: {
    unsigned char op = in->op == NYIR_ADD_F32   ? 0x58
                       : in->op == NYIR_SUB_F32 ? 0x5c
                       : in->op == NYIR_MUL_F32 ? 0x59
                                                : 0x5e;
    return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm_f32(c, in->b, 1) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0x0f, op, 0xc1},
                            4) &&
           ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
  }
  case NYIR_I64_TO_F32:
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_bytes(
               c, (const unsigned char[]){0xf3, 0x48, 0x0f, 0x2a, 0xc0}, 5) &&
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
    unsigned unordered = in->cmp == NYIR_CMP_NE ? 1u : 0u;
    return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm(c, in->b, 1) &&
           ny_x64_obj_bytes(c,
                            (const unsigned char[]){0x31, 0xc0, 0x66, 0x0f,
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
    unsigned unordered = in->cmp == NYIR_CMP_NE ? 1u : 0u;
    return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
           ny_x64_obj_load_value_xmm_f32(c, in->b, 1) &&
           ny_x64_obj_bytes(c,
                            (const unsigned char[]){0x31, 0xc0, 0x0f, 0x2e,
                                                    0xc1, 0x7a, 0x05, 0x0f},
                            8) &&
           ny_x64_obj_u8(c, ny_x64_obj_f64_setcc(in->cmp)) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0xc0, 0xeb, 0x02, 0xb0},
                            4) &&
           ny_x64_obj_u8(c, unordered) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  }
  case NYIR_LABEL:
    return ny_x64_obj_add_label(c, in->imm);
  case NYIR_LOAD_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 ELF object writer: invalid local slot %lld",
                    (long long)in->imm);
      return false;
    }
    if (c->local_f64 && c->local_f64[in->imm])
      return ny_x64_obj_load_xmm(c, ny_x64_obj_local_off(c, (int)in->imm), 0) &&
             ny_x64_obj_store_value_xmm(c, in->dst, 0);
    if (c->local_f32 && c->local_f32[in->imm])
      return ny_x64_obj_load_xmm_f32(c, ny_x64_obj_local_off(c, (int)in->imm),
                                     0) &&
             ny_x64_obj_store_value_xmm_f32(c, in->dst, 0);
    if (c->value_reg && c->value_reg[in->dst] >= 0)
      return ny_x64_obj_load_reg(c, c->value_reg[in->dst],
                                 ny_x64_obj_local_off(c, (int)in->imm));
    return ny_x64_obj_load_rax(c, ny_x64_obj_local_off(c, (int)in->imm)) &&
           ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_ADDR_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      nyr_set_error(c->err, c->err_len,
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
      nyr_set_error(
          c->err, c->err_len,
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
        nyr_set_error(c->err, c->err_len,
                      "x86-64 ELF object writer: invalid addr symbol");
        return false;
      }
      size_t disp = c->code.len;
      if (!ny_x64_obj_i32(c, -4) ||
          !ny_x64_obj_add_reloc(c, sym, disp, NY_RELOC_PC32))
        return false;
      if (!c->value_reg || c->value_reg[in->dst] < 0) {
        if (!ny_x64_obj_store_value_rax(c, in->dst))
          return false;
      }
    }
    return true;
  case NYIR_STORE_LOCAL:
    if (in->imm < 0 || in->imm >= c->local_slots) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 ELF object writer: invalid local slot %lld",
                    (long long)in->imm);
      return false;
    }
    if (c->local_f64 && c->local_f64[in->imm])
      return ny_x64_obj_load_value_xmm(c, in->a, 0) &&
             ny_x64_obj_store_xmm(c, ny_x64_obj_local_off(c, (int)in->imm), 0);
    if (c->local_f32 && c->local_f32[in->imm])
      return ny_x64_obj_load_value_xmm_f32(c, in->a, 0) &&
             ny_x64_obj_store_xmm_f32(c, ny_x64_obj_local_off(c, (int)in->imm),
                                      0);
    if (c->value_reg && c->value_reg[in->a] >= 0)
      return ny_x64_obj_store_reg(c, c->value_reg[in->a],
                                  ny_x64_obj_local_off(c, (int)in->imm));
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_store_rax(c, ny_x64_obj_local_off(c, (int)in->imm));
  case NYIR_BR: {
    if (!ny_x64_obj_u8(c, 0xe9))
      return false;
    size_t disp = c->code.len;
    return ny_x64_obj_i32(c, 0) && ny_x64_obj_add_patch(c, in->imm, disp);
  }
  case NYIR_BR_IF: {
    if (!ny_x64_obj_load_value_rax(c, in->a) ||
        !ny_x64_obj_bytes(
            c, (const unsigned char[]){0x48, 0x85, 0xc0, 0x0f, 0x85}, 5))
      return false;
    size_t disp = c->code.len;
    return ny_x64_obj_i32(c, 0) && ny_x64_obj_add_patch(c, in->imm, disp);
  }
  case NYIR_RET: {
    const nyr_func_t *fn = c->nyir;
    bool agg_ret = fn && fn->return_aggregate_size > 0 &&
                   fn->return_aggregate_size <= 16 &&
                   fn->return_aggregate_classes[0] != NYIR_ARG_CLASS_MEMORY &&
                   fn->return_aggregate_classes[0] != NYIR_ARG_CLASS_NONE;
    if (agg_ret) {
      /*
       * Callee side of the SysV two-eightbyte return: eightbyte 0 goes to
       * rax (INTEGER) or xmm0 (SSE), eightbyte 1 to rdx (INTEGER) or xmm1
       * (SSE). The pieces are loaded before the epilogue, which only
       * restores callee-saved registers and never touches rax/rdx/xmm0/xmm1.
       */
      unsigned class0 = fn->return_aggregate_classes[0];
      unsigned class1 = fn->return_aggregate_classes[1];
      if (in->a < 0) {
        nyr_set_error(c->err, c->err_len,
                      "x86-64 object writer: aggregate return is missing "
                      "its first piece");
        return false;
      }
      if ((class1 == NYIR_ARG_CLASS_INTEGER || class1 == NYIR_ARG_CLASS_SSE)) {
        if (in->b < 0) {
          nyr_set_error(c->err, c->err_len,
                        "x86-64 object writer: aggregate return is "
                        "missing its second piece");
          return false;
        }
        if (class1 == NYIR_ARG_CLASS_INTEGER) {
          if (!ny_x64_obj_load_value_r10(c, in->b) ||
              !ny_x64_obj_bytes(c, (const unsigned char[]){0x4c, 0x89, 0xd2},
                                3))
            return false;
        } else if (!ny_x64_obj_load_value_xmm(c, in->b, 1)) {
          return false;
        }
      }
      if (class0 == NYIR_ARG_CLASS_SSE) {
        if (!ny_x64_obj_load_value_xmm(c, in->a, 0))
          return false;
      } else if (!ny_x64_obj_load_value_rax(c, in->a)) {
        return false;
      }
      return ny_x64_obj_emit_epilogue(c);
    }
    if (in->a >= 0) {
      if (c->value_f64 && in->a < c->value_slots && c->value_f64[in->a]) {
        if (!ny_x64_obj_load_value_xmm(c, in->a, 0))
          return false;
      } else if (c->value_f32 && in->a < c->value_slots &&
                 c->value_f32[in->a]) {
        if (!ny_x64_obj_load_value_xmm_f32(c, in->a, 0))
          return false;
      } else if (!ny_x64_obj_load_value_rax(c, in->a)) {
        return false;
      }
    }
    return ny_x64_obj_emit_epilogue(c);
  }
  case NYIR_CALL: {
    int argc = (int)in->imm;
    if (argc < 0 || argc > NYIR_CALL_MAX_ARGS) {
      nyr_set_error(
          c->err, c->err_len,
          "x86-64 object writer: call exceeds maximum supported arg count");
      return false;
    }
    if (!c->target) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: call ABI metadata unavailable");
      return false;
    }
    int args[NYIR_CALL_MAX_ARGS];
    if (argc > 0)
      args[0] = in->a;
    if (argc > 1)
      args[1] = in->b;
    if (argc > 2)
      args[2] = in->c;
    if (argc > 3)
      args[3] = in->d;
    if (argc > 4)
      args[4] = in->e;
    if (argc > 5)
      args[5] = in->f;
    for (int i = 6; i < argc; ++i)
      args[i] = (in->extra_args && (size_t)(i - 6) < in->extra_args_len)
                    ? in->extra_args[i - 6]
                    : -1;
    bool arg_f64[NYIR_CALL_MAX_ARGS] = {0};
    bool arg_f32[NYIR_CALL_MAX_ARGS] = {0};
    int gp_index[NYIR_CALL_MAX_ARGS];
    int sse_index[NYIR_CALL_MAX_ARGS];
    int agg_gp[NYIR_CALL_MAX_ARGS][2];
    int agg_sse[NYIR_CALL_MAX_ARGS][2];
    bool agg_in_regs[NYIR_CALL_MAX_ARGS] = {0};
    bool agg_indirect[NYIR_CALL_MAX_ARGS] = {0};
    int gp = 0;
    int sse = 0;
    int ordinal = 0;
    int stack_argc = 0;
    bool is_win = c->target && c->target->abi == NY_NATIVE_ABI_WIN64;
    for (int i = 0; i < argc; ++i) {
      gp_index[i] = -1;
      sse_index[i] = -1;
      agg_gp[i][0] = agg_gp[i][1] = -1;
      agg_sse[i][0] = agg_sse[i][1] = -1;
      if (args[i] < 0) {
        nyr_set_error(c->err, c->err_len,
                      "x86-64 object writer: invalid call arg");
        return false;
      }
      if (in->arg_sizes && in->arg_sizes[i] > 0) {
        arg_f64[i] = false;
        arg_f32[i] = false;
        uint32_t size = NYIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        unsigned gp_need = 0, sse_need = 0;
        bool register_eligible = true;
        for (int chunk = 0; chunk < 2; ++chunk) {
          unsigned cls = NYIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
          gp_need += cls == NYIR_ARG_CLASS_INTEGER;
          sse_need += cls == NYIR_ARG_CLASS_SSE;
          if (cls != NYIR_ARG_CLASS_NONE && cls != NYIR_ARG_CLASS_INTEGER &&
              cls != NYIR_ARG_CLASS_SSE)
            register_eligible = false;
        }
        if (is_win) {
          unsigned cls = NYIR_ARG_AGG_CLASS(in->arg_sizes[i], 0);
          if (cls == NYIR_ARG_CLASS_MEMORY) {
            agg_indirect[i] = true;
            if ((size_t)ordinal < c->target->gp_arg_reg_count)
              gp_index[i] = ordinal;
            else
              ++stack_argc;
          } else if (ordinal < (int)c->target->gp_arg_reg_count &&
                     register_eligible && size <= 8) {
            agg_gp[i][0] = ordinal;
            agg_in_regs[i] = true;
          } else if (ordinal >= (int)c->target->gp_arg_reg_count) {
            ++stack_argc;
          } else {
            nyr_set_error(
                c->err, c->err_len,
                "x86-64 object writer: unsupported Win64 aggregate class");
            return false;
          }
          ++ordinal;
          continue;
        }
        if (register_eligible && size <= 16 &&
            gp + (int)gp_need <= (int)c->target->gp_arg_reg_count &&
            sse + (int)sse_need <= 8) {
          for (int chunk = 0; chunk < 2; ++chunk) {
            unsigned cls = NYIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
            if (cls == NYIR_ARG_CLASS_INTEGER)
              agg_gp[i][chunk] = gp++;
            else if (cls == NYIR_ARG_CLASS_SSE)
              agg_sse[i][chunk] = sse++;
          }
          agg_in_regs[i] = true;
        } else {
          stack_argc += (int)((size + 7) / 8);
        }
        continue;
      }
      arg_f64[i] =
          c->value_f64 && args[i] < c->value_slots && c->value_f64[args[i]];
      arg_f32[i] =
          c->value_f32 && args[i] < c->value_slots && c->value_f32[args[i]];
      if (arg_f64[i] || arg_f32[i]) {
        if (is_win) {
          if (ordinal < (int)c->target->gp_arg_reg_count)
            sse_index[i] = ordinal;
          else
            ++stack_argc;
          ++ordinal;
        } else if (sse < 8)
          sse_index[i] = sse++;
        else
          stack_argc++;
      } else {
        if (is_win) {
          if (ordinal < (int)c->target->gp_arg_reg_count)
            gp_index[i] = ordinal;
          else
            ++stack_argc;
          ++ordinal;
        } else if ((size_t)gp < c->target->gp_arg_reg_count)
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
      if (in->arg_sizes && in->arg_sizes[i] > 0 && !agg_indirect[i]) {
        /*
         * byval: allocate stack space and copy aggregate
         */
        uint32_t size = NYIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        int slots = (int)((size + 7) / 8);
        if (!ny_x64_obj_sub_rsp(c, (size_t)slots * 8))
          return false;
        /*
         * load src ptr -> rsi, rsp -> rdi, movsb
         */
        if (!ny_x64_obj_load_value_rax(c, args[i]))
          return false;
        /*
         * mov %rax, %rsi
         */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc6}, 3))
          return false;
        /*
         * mov %rsp, %rdi
         */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xe7}, 3))
          return false;
        /*
         * mov $size, %rcx
         */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc7, 0xc1},
                              3) ||
            !ny_x64_obj_i32(c, (int32_t)size))
          return false;
        /*
         * rep movsb
         */
        if (!ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0xa4}, 2))
          return false;
        continue;
      }
      if (arg_f64[i]) {
        if (!ny_x64_obj_sub_rsp(c, 8) ||
            !ny_x64_obj_load_value_xmm(c, args[i], 0) ||
            !ny_x64_obj_bytes(
                c, (const unsigned char[]){0xf2, 0x0f, 0x11, 0x04, 0x24}, 5))
          return false;
      } else if (arg_f32[i]) {
        if (!ny_x64_obj_sub_rsp(c, 8) ||
            !ny_x64_obj_load_value_xmm_f32(c, args[i], 0) ||
            !ny_x64_obj_bytes(
                c, (const unsigned char[]){0xf3, 0x0f, 0x11, 0x04, 0x24}, 5))
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
                    c, chunk * 8, c->target->gp_arg_regs[agg_gp[i][chunk]]))
              return false;
          } else if (agg_sse[i][chunk] >= 0 &&
                     !ny_x64_obj_load_aggregate_sse(c, chunk * 8,
                                                    agg_sse[i][chunk])) {
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
    const char *call_sym = in->symbol ? in->symbol : "";
    bool is_mutex_lock = strcmp(call_sym, "rt_mutex_lock64") == 0;
    bool is_mutex_unlock = strcmp(call_sym, "rt_mutex_unlock64") == 0;
    if ((is_mutex_lock || is_mutex_unlock) && argc >= 1) {
      if (!ny_x64_obj_load_value_rax(c, args[0]))
        return false;
      if (!ny_x64_obj_mov_rax_to_arg(c, "rdi"))
        return false;
      if (!ny_x64_obj_u8(c, 0xe8))
        return false;
      size_t disp = c->code.len;
      const char *pthread_sym =
          is_mutex_lock ? "pthread_mutex_lock" : "pthread_mutex_unlock";
      char symbol[256];
      const char *prefix =
          c->target->symbol_prefix ? c->target->symbol_prefix : "";
      int n = snprintf(symbol, sizeof(symbol), "%s%s", prefix, pthread_sym);
      if (n < 0 || (size_t)n >= sizeof(symbol)) {
        nyr_set_error(c->err, c->err_len,
                      "x86-64 object writer: pthread symbol too long");
        return false;
      }
      if (!ny_x64_obj_i32(c, 0) ||
          !ny_x64_obj_add_reloc(c, symbol, disp, NY_RELOC_PLT32))
        return false;
      if (!ny_x64_obj_add_rsp(c, c->target->shadow_space_bytes))
        return false;
      if (stack_argc + pad > 0 &&
          !ny_x64_obj_add_rsp(c, (size_t)(stack_argc + pad) * 8))
        return false;
      if (in->dst < 0)
        return true;
      return ny_x64_obj_store_value_rax(c, in->dst);
    }
    if (!ny_x64_obj_u8(c, 0xe8))
      return false;
    size_t disp = c->code.len;
    char symbol[256];
    if (in->flags & NYIR_INST_F_EXTERN) {
      const char *prefix =
          c->target->symbol_prefix ? c->target->symbol_prefix : "";
      int n = snprintf(symbol, sizeof(symbol), "%s%s", prefix,
                       in->symbol ? in->symbol : "<null>");
      if (n < 0 || (size_t)n >= sizeof(symbol)) {
        nyr_set_error(c->err, c->err_len,
                      "x86-64 object writer: extern symbol too long");
        return false;
      }
    } else if (!ny_x64_obj_reloc_symbol(symbol, sizeof(symbol), c,
                                        in->symbol)) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: invalid call symbol");
      return false;
    }
    if (!ny_x64_obj_i32(c, 0) ||
        !ny_x64_obj_add_reloc(c, symbol, disp, NY_RELOC_PLT32))
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
    if (in->scalar_class == NYIR_SCALAR_F64) {
      /*
       * Float load: read the double straight into an XMM register instead of
       * routing it through rax (which would force an integer->xmm move at the
       * consumer).  dst is a float value (see the register scan).
       */
      if (!ny_x64_obj_load_value_rax(c, in->a))
        return false;
      if (c->value_xmm && c->value_xmm[in->dst] >= 0)
        return ny_x64_obj_load_xmm_reg(c, c->value_xmm[in->dst], 0);
      if (!ny_x64_obj_load_xmm_reg(c, 0, 0))
        return false;
      return ny_x64_obj_store_value_xmm(c, in->dst, 0);
    }
    if (!ny_x64_obj_load_value_rax(c, in->a))
      return false;
    /*
     * load8 is represented as LOAD_I64 + MEM_BYTE.  Keep the load width
     * explicit in machine code: a word load compares adjacent string bytes
     * as well and breaks bytewise search at the first non-NUL suffix.
     */
    if (in->flags & NYIR_INST_F_MEM_BYTE) {
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x0f, 0xb6, 0x00},
                            4))
        return false;
    } else if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x8b, 0x00},
                                 3)) {
      return false;
    }
    return ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_STORE_I64:
    if (in->scalar_class == NYIR_SCALAR_F64) {
      if (!ny_x64_obj_load_value_rax(c, in->a))
        return false;
      if (!ny_x64_obj_load_value_xmm(c, in->c, 0))
        return false;
      return ny_x64_obj_store_xmm_reg(c, 0, 0);
    }
    return ny_x64_obj_load_value_rax(c, in->a) &&
           ny_x64_obj_load_value_r10(c, in->c) &&
           ny_x64_obj_bytes(c, (const unsigned char[]){0x4c, 0x89, 0x10}, 3);
  case NYIR_ALLOCA:
    if (in->dst < 0)
      return true;
    /*
     * subq $size, %rsp; andq $-16, %rsp; movq %rsp, %rax; store
     */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x81, 0xec}, 3) ||
        !ny_x64_obj_i32(c, (int32_t)in->imm))
      return false;
    /*
     * andq $-16, %rsp: 0x48 0x83 0xe4 0xf0
     */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x83, 0xe4, 0xf0},
                          4))
      return false;
    /*
     * mov %rsp, %rax
     */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xe0}, 3))
      return false;
    return ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_COPY_STRUCT:
    if (in->imm <= 0)
      return true;
    /*
     * load src (b) -> rsi, load dst (a) -> rdi, mov size -> rcx, rep movsb
     */
    if (!ny_x64_obj_load_value_rax(c, in->b))
      return false;
    /*
     * mov %rax, %rsi
     */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc6}, 3))
      return false;
    if (!ny_x64_obj_load_value_rax(c, in->a))
      return false;
    /*
     * mov %rax, %rdi
     */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc7}, 3))
      return false;
    /*
     * mov $size, %rcx
     */
    if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0xc7, 0xc1}, 3) ||
        !ny_x64_obj_i32(c, (int32_t)in->imm))
      return false;
    /*
     * rep movsb
     */
    return ny_x64_obj_bytes(c, (const unsigned char[]){0xf3, 0xa4}, 2);
  case NYIR_CAPTURE_RET:
    if (in->dst < 0)
      return true;
    switch (in->imm) {
    case 0: /* rdx -> rax */
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xd0}, 3))
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
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: invalid capture.ret selector");
      return false;
    }
    return ny_x64_obj_store_value_rax(c, in->dst);
  case NYIR_OP_COUNT:
    break;
  default:
    break;
  }
  nyr_set_error(c->err, c->err_len,
                "x86-64 ELF object writer: unsupported op %s",
                nyr_op_name(in->op));
  return false;
}

static void ny_x64_obj_scan_frame(nyr_x64_obj_ctx_t *c,
                                  const nyr_func_t *nyir) {
  c->value_slots = nyir && nyir->next_value > 0 ? nyir->next_value : 0;
  c->local_slots = 0;
  for (size_t i = 0; nyir && i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        in->imm >= c->local_slots)
      c->local_slots = (int)in->imm + 1;
  }
  /*
   * Keep dead hidden ABI lanes addressable: the parameter count describes
   * the full incoming convention even if this function never reads a lane.
   */
  if (nyir && nyir->param_count <= (size_t)INT_MAX &&
      (int)nyir->param_count > c->local_slots)
    c->local_slots = (int)nyir->param_count;
  c->frame_bytes = ny_x64_obj_align((c->value_slots + c->local_slots) * 8, 16);
}

static bool ny_x64_obj_op_is_f64(nyr_op_t op) { return nyr_op_is_f64(op); }

static bool ny_x64_obj_op_is_f32(nyr_op_t op) { return nyr_op_is_f32(op); }

static bool ny_x64_obj_classify_values(nyr_x64_obj_ctx_t *c,
                                       const nyr_func_t *nyir) {
  if (!c)
    return false;
  if (c->value_slots > 0) {
    c->value_f64 = (bool *)calloc((size_t)c->value_slots, sizeof(bool));
    c->value_f32 = (bool *)calloc((size_t)c->value_slots, sizeof(bool));
    if (!c->value_f64 || !c->value_f32) {
      nyr_set_error(c->err, c->err_len, "x86-64 object writer: out of memory");
      return false;
    }
  }
  if (c->local_slots > 0) {
    c->local_f64 = (bool *)calloc((size_t)c->local_slots, sizeof(bool));
    c->local_f32 = (bool *)calloc((size_t)c->local_slots, sizeof(bool));
    if (!c->local_f64 || !c->local_f32) {
      nyr_set_error(c->err, c->err_len, "x86-64 object writer: out of memory");
      return false;
    }
  }
  /*
   * Parameter slots are part of the function ABI even when optimization
   * proves that a particular lane is unused in the body. In particular,
   * string and dynamic parameters occupy value/length/tag lanes; dropping an
   * unused length or tag here shifts every later incoming argument.
   */
  size_t param_count = nyir->param_count < (size_t)c->local_slots
                           ? nyir->param_count
                           : (size_t)c->local_slots;
  for (size_t i = 0; i < param_count; ++i) {
    if (!nyir->param_types)
      break;
    c->local_f64[i] = nyir->param_types[i] == NYIR_PARAM_F64;
    c->local_f32[i] = nyir->param_types[i] == NYIR_PARAM_F32;
  }
  for (size_t i = 0; nyir && i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if (in->dst >= 0 && in->dst < c->value_slots &&
        (ny_x64_obj_op_is_f64(in->op) ||
         (in->op == NYIR_CALL && in->scalar_class == NYIR_SCALAR_F64) ||
         (in->op == NYIR_LOAD_I64 && in->scalar_class == NYIR_SCALAR_F64)))
      c->value_f64[in->dst] = true;
    if (in->dst >= 0 && in->dst < c->value_slots &&
        (ny_x64_obj_op_is_f32(in->op) ||
         (in->scalar_class == NYIR_SCALAR_F32 && in->op == NYIR_CALL)))
      c->value_f32[in->dst] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; nyir && i < nyir->len; ++i) {
      const nyr_inst_t *in = &nyir->data[i];
      if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
          in->dst < c->value_slots && in->a < c->value_slots &&
          c->value_f64[in->a] && !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
                 in->dst < c->value_slots && in->a < c->value_slots &&
                 c->value_f32[in->a] && !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->local_f64[in->imm] && !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->local_f32[in->imm] && !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->value_f64[in->dst] && !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < c->value_slots && in->imm < c->local_slots &&
                 c->value_f32[in->dst] && !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < c->value_slots && in->imm < c->local_slots &&
                 c->value_f64[in->a] && !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < c->value_slots && in->imm < c->local_slots &&
                 c->value_f32[in->a] && !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      }
      if (in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
          in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
          in->op == NYIR_CMP_F64 || in->op == NYIR_SIN_F64 ||
          in->op == NYIR_COS_F64) {
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

/*
 * Keep non-floating SSA intervals in caller-saved r11/r9/r8 when they neither
 * cross nor feed a call. Values with call-sensitive lifetimes remain spilled,
 * so ABI argument setup cannot clobber an allocated value.
 */
static bool ny_x64_obj_allocate_registers(nyr_x64_obj_ctx_t *c,
                                          const nyr_func_t *nyir) {
  const char *disabled = getenv("NYTRIX_NATIVE_NO_REGALLOC");
  if (disabled && disabled[0] && strcmp(disabled, "0") != 0 &&
      strcmp(disabled, "false") != 0 && strcmp(disabled, "off") != 0)
    return true;
  if (!c || !nyir || c->value_slots <= 0)
    return true;
  c->value_reg = malloc((size_t)c->value_slots * sizeof(*c->value_reg));
  c->value_xmm = malloc((size_t)c->value_slots * sizeof(*c->value_xmm));
  int *last_use = malloc((size_t)c->value_slots * sizeof(*last_use));
  bool *call_use = calloc((size_t)c->value_slots, sizeof(*call_use));
  bool *regalloc_safe = calloc((size_t)c->value_slots, sizeof(*regalloc_safe));
  int *def_pos = malloc((size_t)c->value_slots * sizeof(*def_pos));
  int *def_count = calloc((size_t)c->value_slots, sizeof(*def_count));
  int *first_use = malloc((size_t)c->value_slots * sizeof(*first_use));
  int *next_call = malloc((nyir->len + 1u) * sizeof(*next_call));
  int *next_struct = malloc((nyir->len + 1u) * sizeof(*next_struct));
  if (!c->value_reg || !c->value_xmm || !last_use || !call_use ||
      !regalloc_safe || !def_pos || !def_count || !first_use || !next_call ||
      !next_struct) {
    free(last_use);
    free(call_use);
    free(regalloc_safe);
    free(def_pos);
    free(def_count);
    free(first_use);
    free(next_call);
    free(next_struct);
    nyr_set_error(c->err, c->err_len,
                  "x86-64 object writer: out of memory in register allocation");
    return false;
  }
  memset(c->value_reg, -1, (size_t)c->value_slots * sizeof(*c->value_reg));
  memset(c->value_xmm, -1, (size_t)c->value_slots * sizeof(*c->value_xmm));
  /*
   * Map branch labels to instruction positions for loop-aware intervals.
   */
  size_t *label_pc = NULL;
  if (nyir && nyir->len > 0 && nyir->len < SIZE_MAX / sizeof(size_t)) {
    label_pc = malloc((nyir->len + 1u) * sizeof(*label_pc));
    if (!label_pc) {
      free(last_use);
      free(call_use);
      free(regalloc_safe);
      free(def_pos);
      free(def_count);
      free(first_use);
      free(next_call);
      free(next_struct);
      nyr_set_error(
          c->err, c->err_len,
          "x86-64 object writer: out of memory checking loop control flow");
      return false;
    }
    for (size_t i = 0; i <= nyir->len; ++i)
      label_pc[i] = SIZE_MAX;
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyr_inst_t *in = &nyir->data[i];
      if (in->op == NYIR_LABEL && in->imm >= 0 && (uint64_t)in->imm < nyir->len)
        label_pc[(size_t)in->imm] = i;
    }
  }
  for (int v = 0; v < c->value_slots; ++v) {
    last_use[v] = -1;
    first_use[v] = INT_MAX;
    def_pos[v] = -1;
    regalloc_safe[v] = true;
  }
  for (size_t i = 0; nyir && i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if (in->dst >= 0 && in->dst < c->value_slots) {
      if (def_count[in->dst]++ == 0)
        def_pos[in->dst] = (int)i;
      else
        regalloc_safe[in->dst] = false;
    }
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k) {
      int v = operands[k];
      if (v >= 0 && v < c->value_slots) {
        if ((int)i < first_use[v])
          first_use[v] = (int)i;
        last_use[v] = (int)i;
        if (in->op == NYIR_CALL)
          call_use[v] = true;
      }
    }
    if (in->op == NYIR_CALL) {
      for (size_t k = 0; k < in->extra_args_len; ++k) {
        int v = in->extra_args[k];
        if (v >= 0 && v < c->value_slots) {
          if ((int)i < first_use[v])
            first_use[v] = (int)i;
          last_use[v] = (int)i;
          call_use[v] = true;
        }
      }
    }
  }
  /*
   * A linear interval is safe inside a loop only when it starts and ends in
   * that same loop body. Loop-carried values, invariants used in the body,
   * multi-definition values, and edge values remain in canonical homes.
   */
  for (int v = 0; v < c->value_slots; ++v) {
    if (def_count[v] != 1 || def_pos[v] < 0 || first_use[v] < def_pos[v])
      regalloc_safe[v] = false;
  }
  if (nyir->len > 0 && !label_pc) {
    for (int v = 0; v < c->value_slots; ++v)
      regalloc_safe[v] = false;
  }
  for (size_t i = 0; label_pc && i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if ((in->op != NYIR_BR && in->op != NYIR_BR_IF) || in->imm < 0 ||
        (uint64_t)in->imm >= nyir->len)
      continue;
    size_t start = label_pc[(size_t)in->imm];
    if (start == SIZE_MAX || start > i)
      continue;
    for (int v = 0; v < c->value_slots; ++v) {
      if (!regalloc_safe[v] || last_use[v] < 0)
        continue;
      int def = def_pos[v], use = last_use[v];
      bool touches_body = def <= (int)i && use >= (int)start;
      bool contained = def >= (int)start && use <= (int)i;
      if (touches_body && !contained)
        regalloc_safe[v] = false;
    }
  }
  free(label_pc);
  next_call[nyir->len] = INT_MAX;
  next_struct[nyir->len] = INT_MAX;
  for (size_t i = nyir->len; i > 0; --i) {
    unsigned op = nyir->data[i - 1].op;
    next_call[i - 1] = (op == NYIR_CALL || op == NYIR_SIN_F64 ||
                        op == NYIR_COS_F64 || op == NYIR_BOUNDS_CHECK)
                           ? (int)(i - 1)
                           : next_call[i];
    /*
     * COPY_STRUCT clobbers rsi/rdi/rcx via rep movsb, so a value kept in a
     * caller register across one would be destroyed.  Treat it like a call
     * for the liveness decision: values live across a struct copy spill to
     * callee-saved registers (r12-r15/rbx), which survive it untouched.
     */
    next_struct[i - 1] = nyir->data[i - 1].op == NYIR_COPY_STRUCT
                             ? (int)(i - 1)
                             : next_struct[i];
  }
  static const int caller_regs[] = {11, 9, 8, 6, 7};
  static const int callee_regs[] = {12, 13, 14, 15, 3};
  int caller_end[sizeof(caller_regs) / sizeof(caller_regs[0])];
  int callee_end[sizeof(callee_regs) / sizeof(callee_regs[0])];
  for (size_t r = 0; r < sizeof(caller_regs) / sizeof(caller_regs[0]); ++r)
    caller_end[r] = -1;
  for (size_t r = 0; r < sizeof(callee_regs) / sizeof(callee_regs[0]); ++r)
    callee_end[r] = -1;
  /*
   * Pass 1: callee-saved registers for values live across a call or struct
   * copy.  Done first so pass 2 can avoid aliasing r12-r15 with a live
   * callee value.
   */
  for (size_t i = 0; nyir && i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    int v = in->dst;
    if (v < 0 || v >= c->value_slots || !regalloc_safe[v] ||
        last_use[v] < (int)i || (c->value_f64 && c->value_f64[v]) ||
        (c->value_f32 && c->value_f32[v]))
      continue;
    bool crosses_call = next_call[i + 1] < last_use[v];
    bool crosses_struct = next_struct[i + 1] < last_use[v];
    if (!(crosses_call || crosses_struct))
      continue;
    for (size_t r = 0; r < sizeof(callee_regs) / sizeof(callee_regs[0]); ++r) {
      if ((int)i >= callee_end[r]) {
        c->value_reg[v] = (int8_t)callee_regs[r];
        callee_end[r] = last_use[v];
        break;
      }
    }
  }
  /*
   * Pass 2: caller-saved registers for values with call-free lifetimes.
   * r12-r15 are also callee-saved; skip them here while a callee value is
   * live so the two register pools never alias the same register at once.
   */
  for (size_t i = 0; nyir && i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    int v = in->dst;
    if (v < 0 || v >= c->value_slots || !regalloc_safe[v] ||
        last_use[v] < (int)i || (c->value_f64 && c->value_f64[v]) ||
        (c->value_f32 && c->value_f32[v]))
      continue;
    bool crosses_call = next_call[i + 1] < last_use[v];
    bool crosses_struct = next_struct[i + 1] < last_use[v];
    if (in->op == NYIR_CONST_I64 && !crosses_call && !crosses_struct)
      continue;
    if (crosses_call || crosses_struct || call_use[v])
      continue;
    for (size_t r = 0; r < sizeof(caller_regs) / sizeof(caller_regs[0]); ++r) {
      int reg = caller_regs[r];
      if (reg >= 12 && reg <= 15 && callee_end[reg - 12] >= 0 &&
          i < (size_t)callee_end[reg - 12])
        continue;
      if ((int)i >= caller_end[r]) {
        c->value_reg[v] = (int8_t)reg;
        caller_end[r] = last_use[v];
        break;
      }
    }
  }
  static const int xmm_regs[] = {2, 3,  4,  5,  6,  7,  8,
                                 9, 10, 11, 12, 13, 14, 15};
  int xmm_end[sizeof(xmm_regs) / sizeof(xmm_regs[0])];
  for (size_t r = 0; r < sizeof(xmm_regs) / sizeof(xmm_regs[0]); ++r)
    xmm_end[r] = -1;
  for (size_t i = 0; nyir && i < nyir->len; ++i) {
    int v = nyir->data[i].dst;
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
  free(regalloc_safe);
  free(def_pos);
  free(def_count);
  free(first_use);
  free(next_call);
  free(next_struct);
  return true;
}

static bool ny_x64_obj_immediate_use(const nyr_inst_t *in, int operand,
                                     int64_t imm) {
  if (imm < INT32_MIN || imm > INT32_MAX)
    return false;
  switch (in->op) {
  case NYIR_ADD_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_MUL_I64:
    return operand == 0 || operand == 1;
  case NYIR_SUB_I64:
    return operand == 1;
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
    return operand == 1 && imm >= 1 && imm <= 63;
  case NYIR_CMP_I64:
    return operand == 1 ||
           (operand == 0 && (in->cmp == NYIR_CMP_EQ || in->cmp == NYIR_CMP_NE));
  default:
    return false;
  }
}

/*
 * Constants used exclusively by immediate-capable instructions need neither
 * emitted moves nor stack slots. Prove that property once per function so the
 * emitter stays O(1) at each use and an unexpected load fails explicitly.
 */
static bool ny_x64_obj_classify_immediates(nyr_x64_obj_ctx_t *c,
                                           const nyr_func_t *nyir) {
  if (!c || !nyir || c->value_slots <= 0)
    return true;
  c->value_immediate =
      calloc((size_t)c->value_slots, sizeof(*c->value_immediate));
  if (!c->value_immediate) {
    nyr_set_error(c->err, c->err_len,
                  "x86-64 object writer: out of memory classifying immediates");
    return false;
  }
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 && in->dst < c->value_slots)
      c->value_immediate[in->dst] = true;
  }
  for (size_t i = 0; i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    /*
     * The scalar encoder can use one immediate operand, never two.  Keep
     * operand A materialized when both operands are otherwise eligible, so
     * the normal A-register/B-immediate encoding remains valid at -O0 too.
     */
    if (in->a >= 0 && in->a < c->value_slots && in->b >= 0 &&
        in->b < c->value_slots && c->value_immediate[in->a] &&
        c->value_immediate[in->b]) {
      const nyr_inst_t *a_def = ny_x64_obj_valmap_def(&c->valmap, in->a);
      const nyr_inst_t *b_def = ny_x64_obj_valmap_def(&c->valmap, in->b);
      if (a_def && b_def && ny_x64_obj_immediate_use(in, 0, a_def->imm) &&
          ny_x64_obj_immediate_use(in, 1, b_def->imm))
        c->value_immediate[in->a] = false;
    }
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (int operand = 0; operand < 6; ++operand) {
      int value = operands[operand];
      if (value < 0 || value >= c->value_slots || !c->value_immediate[value])
        continue;
      const nyr_inst_t *def = ny_x64_obj_valmap_def(&c->valmap, value);
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

static bool ny_x64_obj_layout_spills(nyr_x64_obj_ctx_t *c,
                                     const nyr_func_t *nyir) {
  if (!c || c->value_slots < 0)
    return false;
  if (c->value_slots > 0) {
    c->value_spill = malloc((size_t)c->value_slots * sizeof(*c->value_spill));
    if (!c->value_spill) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: out of memory laying out spills");
      return false;
    }
  }
  c->spill_slots = 0;
  c->callee_save_slots = 0;
  for (int reg = 0; reg < 16; ++reg)
    c->callee_save_slot[reg] = -1;
  for (int reg = 0; reg < 16; ++reg)
    c->callee_save_xmm[reg] = -1;
  /*
   * The object aggregate ABI shims use REP MOVSB, which clobbers RSI/RDI.
   * Those registers are caller-saved under SysV but nonvolatile under Win64.
   * Reserve their homes for every Win64 function so the shim cannot violate
   * the host ABI, even when the function's allocated values do not use them.
   */
  bool uses_struct_copy = false;
  if (c->target && c->target->abi == NY_NATIVE_ABI_WIN64 && nyir) {
    uses_struct_copy = nyir->return_aggregate_size > 8;
    for (size_t i = 0; i < nyir->param_count; ++i)
      uses_struct_copy |=
          nyir->param_types && nyir->param_types[i] == NYIR_PARAM_AGGREGATE;
    for (size_t i = 0; i < nyir->len; ++i)
      uses_struct_copy |= nyir->data[i].op == NYIR_COPY_STRUCT;
  }
  if (uses_struct_copy) {
    c->callee_save_slot[6] = c->callee_save_slots++;
    c->callee_save_slot[7] = c->callee_save_slots++;
  }
  for (int v = 0; v < c->value_slots; ++v) {
    if ((c->value_reg && c->value_reg[v] >= 0) ||
        (c->value_xmm && c->value_xmm[v] >= 0) ||
        (c->value_immediate && c->value_immediate[v])) {
      c->value_spill[v] = -1;
      int reg = c->value_reg ? c->value_reg[v] : -1;
      if ((reg == 3 || (reg >= 12 && reg <= 15)) &&
          c->callee_save_slot[reg] < 0)
        c->callee_save_slot[reg] = c->callee_save_slots++;
      int xmm = c->value_xmm ? c->value_xmm[v] : -1;
      if (xmm >= 6 && xmm <= 15 && c->callee_save_xmm[xmm] < 0)
        c->callee_save_xmm[xmm] = c->callee_save_slots++;
    } else {
      c->value_spill[v] = c->spill_slots++;
    }
  }
  c->frame_bytes = ny_x64_obj_align(
      (c->spill_slots + c->callee_save_slots + c->local_slots) * 8, 16);
  return true;
}

/*
 * Reserve callee-side home storage for aggregate parameters and record each
 * home's rbp-relative offset. Homes sit below the whole scalar frame area
 * (value spills, callee saves, locals); the parameter's local slot receives
 * the home pointer in the prologue, so ordinary address arithmetic sees an
 * aggregate parameter exactly like a pointer to stack storage.
 */
static bool ny_x64_obj_layout_agg_homes(nyr_x64_obj_ctx_t *c,
                                        const nyr_func_t *nyir) {
  c->agg_home_words = 0;
  if (!c || !nyir || nyir->param_count == 0)
    return true;
  bool any = false;
  for (size_t i = 0; i < nyir->param_count; ++i)
    if (nyir->param_types && nyir->param_types[i] == NYIR_PARAM_AGGREGATE)
      any = true;
  if (!any)
    return true;
  if (nyir->param_count > SIZE_MAX / sizeof(*c->agg_home_off))
    return false;
  c->agg_home_off = malloc(nyir->param_count * sizeof(*c->agg_home_off));
  if (!c->agg_home_off)
    return false;
  for (size_t i = 0; i < nyir->param_count; ++i)
    c->agg_home_off[i] = INT_MIN;
  size_t words = 0;
  int base = c->spill_slots + c->callee_save_slots + c->local_slots;
  for (size_t i = 0; i < nyir->param_count; ++i) {
    if (!nyir->param_types || nyir->param_types[i] != NYIR_PARAM_AGGREGATE)
      continue;
    uint32_t packed = nyir->param_abi ? nyir->param_abi[i] : 0;
    uint32_t size = NYIR_ARG_AGG_SIZE(packed);
    size_t home_words = (size + 7u) / 8u;
    if (home_words == 0)
      home_words = 1;
    if (home_words > SIZE_MAX / 8 - 1 || words > SIZE_MAX - home_words)
      return false;
    words += home_words;
    if (base > INT_MAX - (int)words)
      return false;
    c->agg_home_off[i] = -8 * (base + (int)words);
  }
  c->agg_home_words = words;
  if (base > INT_MAX - (int)words)
    return false;
  int total_words = base + (int)words;
  c->frame_bytes = ny_x64_obj_align(total_words * 8, 16);
  return true;
}

static int ny_x64_obj_callee_off(const nyr_x64_obj_ctx_t *c, int reg) {
  return -8 * (c->spill_slots + c->callee_save_slot[reg] + 1);
}

static int ny_x64_obj_callee_xmm_off(const nyr_x64_obj_ctx_t *c, int xmm) {
  return -8 * (c->spill_slots + c->callee_save_xmm[xmm] + 1);
}

static bool ny_x64_obj_save_callee(nyr_x64_obj_ctx_t *c) {
  for (int reg = 0; reg < 16; ++reg) {
    if (c->callee_save_slot[reg] >= 0 &&
        !ny_x64_obj_store_reg(c, reg, ny_x64_obj_callee_off(c, reg)))
      return false;
  }
  for (int xmm = 6; xmm <= 15; ++xmm) {
    if (c->callee_save_xmm[xmm] >= 0 &&
        !ny_x64_obj_store_xmm(c, ny_x64_obj_callee_xmm_off(c, xmm), xmm))
      return false;
  }
  return true;
}

static bool ny_x64_obj_emit_epilogue(nyr_x64_obj_ctx_t *c) {
  for (int reg = 15; reg >= 0; --reg) {
    if (c->callee_save_slot[reg] >= 0 &&
        !ny_x64_obj_load_reg(c, reg, ny_x64_obj_callee_off(c, reg)))
      return false;
  }
  for (int xmm = 15; xmm >= 6; --xmm) {
    if (c->callee_save_xmm[xmm] >= 0 &&
        !ny_x64_obj_load_xmm(c, ny_x64_obj_callee_xmm_off(c, xmm), xmm))
      return false;
  }
  return ny_x64_obj_bytes(c, (const unsigned char[]){0xc9, 0xc3}, 2);
}

static bool ny_x64_obj_emit_param_spill(nyr_x64_obj_ctx_t *c,
                                        const nyr_func_t *nyir) {
  if (!c || !nyir || c->local_slots <= 0)
    return true;
  int max_local = c->local_slots;
  bool *stored = (bool *)calloc((size_t)max_local, sizeof(bool));
  bool *is_param = (bool *)calloc((size_t)max_local, sizeof(bool));
  if (!stored || !is_param) {
    free(stored);
    free(is_param);
    nyr_set_error(c->err, c->err_len, "x86-64 object writer: OOM param spill");
    return false;
  }
  if (nyir->param_count > 0) {
    if (nyir->param_count > (size_t)max_local) {
      free(stored);
      free(is_param);
      nyr_set_error(
          c->err, c->err_len,
          "x86-64 object writer: parameter prefix exceeds local slots");
      return false;
    }
    for (size_t i = 0; i < nyir->param_count; ++i)
      is_param[i] = true;
  } else {
    /*
     * Legacy NYIR without explicit ABI metadata: infer the parameter prefix
     * from loads before their first store.
     */
    for (size_t i = 0; i < nyir->len; ++i) {
      const nyr_inst_t *in = &nyir->data[i];
      if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && in->imm < max_local)
        stored[(int)in->imm] = true;
      else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
               in->imm < max_local && !stored[(int)in->imm])
        is_param[(int)in->imm] = true;
    }
  }
  /*
   * Classify every parameter's incoming location first. SysV x86-64 shares
   * the integer, SSE, and stack classification counters across scalar and
   * aggregate parameters in parameter order: a register-class aggregate
   * consumes the next INTEGER/SSE eightbyte registers, and any aggregate
   * that does not fit the remaining registers (or carries class MEMORY)
   * travels entirely on the incoming stack like any other overflowed
   * argument.
   */
  int agg_count = 0;
  for (size_t i = 0; i < nyir->param_count; ++i)
    if (nyir->param_types && nyir->param_types[i] == NYIR_PARAM_AGGREGATE)
      ++agg_count;
  uint32_t *agg_packed = NULL;
  bool *agg_indirect = NULL;
  int *agg_gp = NULL;
  int *agg_sse = NULL;
  int *agg_stack_base = NULL;
  int *agg_words = NULL;
  if (agg_count > 0) {
    agg_packed = calloc((size_t)agg_count, sizeof(*agg_packed));
    agg_indirect = calloc((size_t)agg_count, sizeof(*agg_indirect));
    agg_gp = calloc((size_t)agg_count, 2 * sizeof(*agg_gp));
    agg_sse = calloc((size_t)agg_count, 2 * sizeof(*agg_sse));
    agg_stack_base = calloc((size_t)agg_count, sizeof(*agg_stack_base));
    agg_words = calloc((size_t)agg_count, sizeof(*agg_words));
    if (!agg_packed || !agg_indirect || !agg_gp || !agg_sse ||
        !agg_stack_base || !agg_words) {
      free(stored);
      free(is_param);
      free(agg_packed);
      free(agg_indirect);
      free(agg_gp);
      free(agg_sse);
      free(agg_stack_base);
      free(agg_words);
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: OOM param classification");
      return false;
    }
  }
  static const unsigned char gp_reg_code[6] = {7, 6, 2, 1, 0, 1};
  static const bool gp_needs_rex_r[6] = {0, 0, 0, 0, 1, 1};
  int gp = 0;
  int sse = 0;
  int stack = 0;
  bool is_win = c->target && c->target->abi == NY_NATIVE_ABI_WIN64;
  int agg_index = -1;
  for (int i = 0; i < max_local; ++i) {
    if (!is_param[i])
      continue;
    if ((size_t)i < nyir->param_count && c->agg_home_off &&
        c->agg_home_off[i] != INT_MIN) {
      ++agg_index;
      agg_gp[agg_index * 2] = -1;
      agg_gp[agg_index * 2 + 1] = -1;
      agg_sse[agg_index * 2] = -1;
      agg_sse[agg_index * 2 + 1] = -1;
      uint32_t packed = nyir->param_abi ? nyir->param_abi[i] : 0;
      uint32_t size = NYIR_ARG_AGG_SIZE(packed);
      if (is_win) {
        unsigned cls = NYIR_ARG_AGG_CLASS(packed, 0);
        if (cls == NYIR_ARG_CLASS_MEMORY) {
          agg_indirect[agg_index] = true;
          agg_words[agg_index] = (int)((size + 7u) / 8u);
          agg_stack_base[agg_index] = (int)i;
        } else if ((size_t)i < c->target->gp_arg_reg_count) {
          agg_gp[agg_index * 2] = (int)i;
        } else {
          agg_stack_base[agg_index] = (int)(i - c->target->gp_arg_reg_count);
          agg_words[agg_index] = 1;
        }
        agg_packed[agg_index] = packed;
        continue;
      }
      unsigned gp_need = 0, sse_need = 0;
      bool register_eligible = true;
      for (int chunk = 0; chunk < 2; ++chunk) {
        unsigned cls = NYIR_ARG_AGG_CLASS(packed, chunk);
        gp_need += cls == NYIR_ARG_CLASS_INTEGER;
        sse_need += cls == NYIR_ARG_CLASS_SSE;
        if (cls != NYIR_ARG_CLASS_NONE && cls != NYIR_ARG_CLASS_INTEGER &&
            cls != NYIR_ARG_CLASS_SSE)
          register_eligible = false;
      }
      if (register_eligible && size <= 16 &&
          gp + (int)gp_need <= (int)c->target->gp_arg_reg_count &&
          sse + (int)sse_need <= 8) {
        for (int chunk = 0; chunk < 2; ++chunk) {
          unsigned cls = NYIR_ARG_AGG_CLASS(packed, chunk);
          if (cls == NYIR_ARG_CLASS_INTEGER) {
            agg_gp[agg_index * 2 + chunk] = gp;
            ++gp;
          } else if (cls == NYIR_ARG_CLASS_SSE) {
            agg_sse[agg_index * 2 + chunk] = sse;
            ++sse;
          }
        }
      } else {
        agg_words[agg_index] = (int)((size + 7u) / 8u);
        agg_stack_base[agg_index] = stack;
        stack += agg_words[agg_index];
      }
      agg_packed[agg_index] = packed;
      continue;
    }
    int off = ny_x64_obj_local_off(c, i);
    bool is_f64 = c->local_f64 && i < c->local_slots && c->local_f64[i];
    bool is_f32 = c->local_f32 && i < c->local_slots && c->local_f32[i];
    const char *abi_class = is_f64 ? "F64" : is_f32 ? "F32" : "GPR";
    if (is_win) {
      size_t ordinal = (size_t)i;
      if (ordinal < c->target->gp_arg_reg_count) {
        if (is_f64 || is_f32) {
          unsigned char modrm = (unsigned char)(0x85 | ((ordinal & 7u) << 3));
          unsigned char prefix = is_f32 ? 0xF3 : 0xF2;
          if (!ny_x64_obj_bytes(c, (unsigned char[]){prefix, 0x0F, 0x11, modrm},
                                4) ||
              !ny_x64_obj_i32(c, off))
            goto spill_fail;
        } else {
          int reg = ny_x64_obj_arg_reg_code(c->target->gp_arg_regs[ordinal]);
          if (reg < 0 || !ny_x64_obj_store_reg(c, reg, off))
            goto spill_fail;
        }
      } else {
        int src_off = 16 + (int)c->target->shadow_space_bytes +
                      (i - (int)c->target->gp_arg_reg_count) * 8;
        if (!ny_x64_obj_load_rax(c, src_off) || !ny_x64_obj_store_rax(c, off))
          goto spill_fail;
      }
    } else if ((is_f64 || is_f32) && sse < 8) {
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("OBJECT_ABI",
                      "param local=%d class=%s incoming=SSE%d frame=%d", i,
                      abi_class, sse, off);
      unsigned char modrm = (unsigned char)(0x85 | (sse << 3));
      unsigned char prefix = is_f32 ? 0xF3 : 0xF2;
      if (!ny_x64_obj_bytes(c, (unsigned char[]){prefix, 0x0F, 0x11, modrm},
                            4) ||
          !ny_x64_obj_i32(c, off)) {
        goto spill_fail;
      }
      sse++;
    } else if (!is_f64 && !is_f32 && gp < 6) {
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("OBJECT_ABI",
                      "param local=%d class=%s incoming=GP%d frame=%d", i,
                      abi_class, gp, off);
      unsigned char rex = gp_needs_rex_r[gp] ? 0x4C : 0x48;
      unsigned char modrm = (unsigned char)(0x85 | (gp_reg_code[gp] << 3));
      if (!ny_x64_obj_bytes(c, &rex, 1) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x89, modrm}, 2) ||
          !ny_x64_obj_i32(c, off)) {
        goto spill_fail;
      }
      gp++;
    } else {
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("OBJECT_ABI",
                      "param local=%d class=%s incoming=stack%d frame=%d", i,
                      abi_class, stack, off);
      int src_off = 16 + (int)c->target->shadow_space_bytes + stack * 8;
      if (!ny_x64_obj_load_rax(c, src_off) || !ny_x64_obj_store_rax(c, off)) {
        goto spill_fail;
      }
      stack++;
    }
  }
  /*
   * Fill aggregate homes: register pieces come straight from their incoming
   * eightbyte registers; stack-classified aggregates copy their full ABI
   * footprint word by word from the incoming stack area.
   */
  agg_index = -1;
  for (int i = 0; i < max_local; ++i) {
    if (!is_param[i])
      continue;
    if (!((size_t)i < nyir->param_count && c->agg_home_off &&
          c->agg_home_off[i] != INT_MIN))
      continue;
    ++agg_index;
    int home = c->agg_home_off[i];
    if (agg_indirect[agg_index]) {
      int ordinal = (int)i;
      if (ordinal < (int)c->target->gp_arg_reg_count) {
        int reg = ny_x64_obj_arg_reg_code(c->target->gp_arg_regs[ordinal]);
        if (reg < 0 || !ny_x64_obj_mov_reg_reg(c, reg, 0))
          goto spill_fail;
      } else {
        int src_off = 16 + (int)c->target->shadow_space_bytes +
                      (ordinal - (int)c->target->gp_arg_reg_count) * 8;
        if (!ny_x64_obj_load_rax(c, src_off))
          goto spill_fail;
      }
      if (!ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc6}, 3) ||
          !ny_x64_obj_lea_rax(c, home) ||
          !ny_x64_obj_bytes(c, (const unsigned char[]){0x48, 0x89, 0xc7}, 3) ||
          !ny_x64_obj_mov_rax_imm(c,
                                  NYIR_ARG_AGG_SIZE(agg_packed[agg_index])) ||
          !ny_x64_obj_bytes(
              c, (const unsigned char[]){0x48, 0x89, 0xc1, 0xf3, 0xa4}, 5))
        goto spill_fail;
      continue;
    }
    if (agg_gp[agg_index * 2] < 0 && agg_gp[agg_index * 2 + 1] < 0 &&
        agg_sse[agg_index * 2] < 0 && agg_sse[agg_index * 2 + 1] < 0) {
      for (int word = 0; word < agg_words[agg_index]; ++word) {
        int src_off = 16 + (int)c->target->shadow_space_bytes +
                      (agg_stack_base[agg_index] + word) * 8;
        if (!ny_x64_obj_load_rax(c, src_off) ||
            !ny_x64_obj_store_rax(c, home + word * 8)) {
          goto spill_fail;
        }
      }
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("OBJECT_ABI",
                      "param local=%d class=AGG%u incoming=stack words=%d "
                      "frame=%d",
                      i, (unsigned)NYIR_ARG_AGG_SIZE(agg_packed[agg_index]),
                      agg_words[agg_index], home);
      continue;
    }
    if (ny_trace_enabled("NY_TRACE_ABI"))
      ny_trace_line("OBJECT_ABI",
                    "param local=%d class=AGG%u incoming=regs gp=%d,%d "
                    "sse=%d,%d frame=%d",
                    i, (unsigned)NYIR_ARG_AGG_SIZE(agg_packed[agg_index]),
                    agg_gp[agg_index * 2], agg_gp[agg_index * 2 + 1],
                    agg_sse[agg_index * 2], agg_sse[agg_index * 2 + 1], home);
    for (int chunk = 0; chunk < 2; ++chunk) {
      int chunk_off = home + chunk * 8;
      int gp_reg = agg_gp[agg_index * 2 + chunk];
      int sse_reg = agg_sse[agg_index * 2 + chunk];
      if (gp_reg >= 0) {
        int physical_reg =
            is_win ? ny_x64_obj_arg_reg_code(c->target->gp_arg_regs[gp_reg])
                   : gp_reg_code[gp_reg];
        if (physical_reg < 0)
          goto spill_fail;
        unsigned char rex = physical_reg >= 8 ? 0x4C : 0x48;
        unsigned char modrm = (unsigned char)(0x85 | ((physical_reg & 7) << 3));
        if (!ny_x64_obj_bytes(c, &rex, 1) ||
            !ny_x64_obj_bytes(c, (const unsigned char[]){0x89, modrm}, 2) ||
            !ny_x64_obj_i32(c, chunk_off)) {
          goto spill_fail;
        }
      } else if (sse_reg >= 0) {
        unsigned char modrm = (unsigned char)(0x85 | (sse_reg << 3));
        if (!ny_x64_obj_bytes(c, (unsigned char[]){0xF2, 0x0F, 0x11, modrm},
                              4) ||
            !ny_x64_obj_i32(c, chunk_off)) {
          goto spill_fail;
        }
      }
    }
  }
  free(stored);
  free(is_param);
  free(agg_packed);
  free(agg_indirect);
  free(agg_gp);
  free(agg_sse);
  free(agg_stack_base);
  free(agg_words);
  return true;
spill_fail:
  free(stored);
  free(is_param);
  free(agg_packed);
  free(agg_indirect);
  free(agg_gp);
  free(agg_sse);
  free(agg_stack_base);
  free(agg_words);
  return false;
}

/*
 * Store each aggregate parameter's home address into its local slot so the
 * function body can treat an aggregate parameter like a pointer to ordinary
 * stack storage.
 */
static bool ny_x64_obj_emit_agg_home_ptrs(nyr_x64_obj_ctx_t *c,
                                          const nyr_func_t *nyir) {
  if (!c || !c->agg_home_off)
    return true;
  for (size_t i = 0; i < nyir->param_count; ++i) {
    int home = c->agg_home_off[i];
    if (home == INT_MIN)
      continue;
    if (!ny_x64_obj_lea_rax(c, home) ||
        !ny_x64_obj_store_rax(c, ny_x64_obj_local_off(c, (int)i)))
      return false;
  }
  return true;
}

bool nyr_x64_obj_emit_code(nyr_x64_obj_ctx_t *c, const nyr_func_t *nyir,
                           bool tag_return) {
  c->nyir = nyir;
  ny_x64_obj_valmap_init(&c->valmap, nyir);
  ny_x64_obj_scan_frame(c, nyir);
  if (ny_trace_enabled("NY_TRACE_EMIT"))
    ny_trace_line("OBJECT_EMIT", "begin instructions=%zu values=%d",
                  nyir ? nyir->len : 0, nyir ? nyir->next_value : 0);
  if (!ny_x64_obj_classify_values(c, nyir))
    return false;
  if (ny_trace_enabled("NY_TRACE_EMIT"))
    ny_trace_line("OBJECT_EMIT", "classify values complete");
  if (!ny_x64_obj_classify_immediates(c, nyir))
    return false;
  if (ny_trace_enabled("NY_TRACE_EMIT"))
    ny_trace_line("OBJECT_EMIT", "classify immediates complete");
  if (!ny_x64_obj_allocate_registers(c, nyir))
    return false;
  if (ny_trace_enabled("NY_TRACE_EMIT"))
    ny_trace_line("OBJECT_EMIT", "register allocation complete");
  if (!ny_x64_obj_layout_spills(c, nyir))
    return false;
  if (ny_trace_enabled("NY_TRACE_EMIT"))
    ny_trace_line("OBJECT_EMIT", "spill layout complete");
  if (!ny_x64_obj_layout_agg_homes(c, nyir))
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
  if (!ny_x64_obj_emit_agg_home_ptrs(c, nyir))
    return false;
  if (!ny_x64_obj_emit_param_spill(c, nyir))
    return false;
  if (nyir && nyir->len > 0) {
    if (nyir->len > SIZE_MAX / sizeof(*c->inst_offsets)) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: instruction map too large");
      return false;
    }
    c->inst_offsets = malloc(nyir->len * sizeof(*c->inst_offsets));
    if (!c->inst_offsets) {
      nyr_set_error(c->err, c->err_len,
                    "x86-64 object writer: instruction map allocation failed");
      return false;
    }
    for (size_t i = 0; i < nyir->len; ++i)
      c->inst_offsets[i] = SIZE_MAX;
  }
  for (size_t i = 0; nyir && i < nyir->len; ++i) {
    const nyr_inst_t *in = &nyir->data[i];
    if (c->inst_offsets)
      c->inst_offsets[i] = c->code.len;
    if (tag_return && in->op == NYIR_RET && in->a >= 0) {
      if (c->value_f64 && in->a < c->value_slots && c->value_f64[in->a]) {
        if (!ny_x64_obj_load_value_xmm(c, in->a, 0) ||
            !ny_x64_obj_emit_epilogue(c))
          return false;
        continue;
      }
      if (!ny_x64_obj_load_value_rax(c, in->a) ||
          !ny_x64_obj_bytes(c,
                            (const unsigned char[]){0x48, 0x8d, 0x04, 0x45,
                                                    0x01, 0x00, 0x00, 0x00},
                            8) ||
          !ny_x64_obj_emit_epilogue(c))
        return false;
      continue;
    }
    if (!ny_x64_obj_emit_inst(c, in))
      return false;
  }
  if (nyir && (nyir->len == 0 || nyir->data[nyir->len - 1].op != NYIR_RET)) {
    if (!ny_x64_obj_mov_rax_imm(c, 0) || !ny_x64_obj_emit_epilogue(c))
      return false;
  }
  return ny_x64_obj_patch_branches(c);
}
