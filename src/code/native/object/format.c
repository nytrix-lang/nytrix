/*
 * Object format dispatch: routes machine-form to the correct object
 * encoder (x64, aarch64) and format writer (ELF, COFF, Mach-O).
 */
#include "code/native/object/internal.h"
#include "base/parallel.h"


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



static bool ny_i386_obj_reserve_relocs(ny_i386_obj_reloc_t **data,
                                       size_t *cap, size_t want,
                                       char *err, size_t err_len) {
  if (!data || !cap) {
    ny_native_set_err(err, err_len,
                      "i386 ELF object writer: invalid relocation buffer");
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
    ny_native_set_err(err, err_len,
                      "i386 ELF object writer: relocation buffer too large");
    return false;
  }
  ny_i386_obj_reloc_t *grown = realloc(*data, next * sizeof(*grown));
  if (!grown) {
    ny_native_set_err(err, err_len,
                      "i386 ELF object writer: relocation allocation failed");
    return false;
  }
  *data = grown;
  *cap = next;
  return true;
}


/*
 * ELF32/ELF64, COFF, and Mach-O packaging over encoded code, symbols, and
 * relocation records produced by the architecture encoders.
 */

/*
 * Independence metrics: machine form encode success vs per-function NYIR
 * object fallback. Unsupported machine shapes remain explicit in the
 * per-function reason so a mixed bundle cannot hide its owner.
 */
unsigned long long ny_native_stat_mach_ok = 0;
unsigned long long ny_native_stat_nir_fallback = 0;
unsigned long long ny_native_stat_mach_div_magic = 0;
unsigned long long ny_native_stat_mach_div_idiv = 0;
static char ny_native_first_mach_fallback[512];
static int ny_native_first_mach_fallback_set = 0;
unsigned long long ny_native_stat_regalloc_segments = 0;
unsigned long long ny_native_stat_regalloc_colored = 0;
unsigned long long ny_native_stat_regalloc_spilled = 0;
unsigned long long ny_native_stat_regalloc_reloads = 0;
unsigned long long ny_native_stat_regalloc_peak_live = 0;
unsigned long long ny_native_stat_fpr_segments = 0;
unsigned long long ny_native_stat_fpr_colored = 0;
unsigned long long ny_native_stat_fpr_spilled = 0;
unsigned long long ny_native_stat_fpr_reloads = 0;
unsigned long long ny_native_stat_fpr_peak_live = 0;
unsigned long long ny_native_stat_vector_segments = 0;
unsigned long long ny_native_stat_vector_colored = 0;
unsigned long long ny_native_stat_vector_spilled = 0;
unsigned long long ny_native_stat_vector_reloads = 0;
unsigned long long ny_native_stat_vector_peak_live = 0;


static void ny_native_atomic_max_ull(unsigned long long *dst,
                                     unsigned long long value) {
  unsigned long long cur = __atomic_load_n(dst, __ATOMIC_RELAXED);
  while (cur < value &&
         !__atomic_compare_exchange_n(dst, &cur, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

static void ny_native_mach_record(bool machine, const char *symbol,
                                   const char *reason) {
  if (machine) {
    __atomic_fetch_add(&ny_native_stat_mach_ok, 1, __ATOMIC_RELAXED);
    return;
  }
  __atomic_fetch_add(&ny_native_stat_nir_fallback, 1, __ATOMIC_RELAXED);
  if (__sync_bool_compare_and_swap(&ny_native_first_mach_fallback_set, 0, 1))
    snprintf(ny_native_first_mach_fallback,
             sizeof(ny_native_first_mach_fallback), "%s: %s",
             symbol && symbol[0] ? symbol : "<unknown>",
             reason && reason[0] ? reason : "machine form unavailable");
}

void ny_native_mach_encode_fallback_detail(char *out, size_t out_len) {
  if (!out || out_len == 0)
    return;
  snprintf(out, out_len, "%s",
           ny_native_first_mach_fallback_set
               ? ny_native_first_mach_fallback
               : "none");
}

void ny_native_mach_encode_stats(unsigned long long *mach_ok,
                                unsigned long long *nir_fallback) {
  if (mach_ok)
    *mach_ok = __atomic_load_n(&ny_native_stat_mach_ok, __ATOMIC_RELAXED);
  if (nir_fallback)
    *nir_fallback =
        __atomic_load_n(&ny_native_stat_nir_fallback, __ATOMIC_RELAXED);
}

void ny_native_mach_div_record(bool magic) {
  unsigned long long *counter = magic ? &ny_native_stat_mach_div_magic
                                      : &ny_native_stat_mach_div_idiv;
  __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

void ny_native_mach_div_stats(unsigned long long *magic,
                              unsigned long long *idiv) {
  if (magic)
    *magic = __atomic_load_n(&ny_native_stat_mach_div_magic, __ATOMIC_RELAXED);
  if (idiv)
    *idiv = __atomic_load_n(&ny_native_stat_mach_div_idiv, __ATOMIC_RELAXED);
}

void ny_native_mach_regalloc_record(size_t segments, size_t colored,
                                     size_t spilled, size_t reloads,
                                     size_t peak_live);
__attribute__((used)) void ny_native_mach_regalloc_record(
    size_t segments, size_t colored, size_t spilled, size_t reloads,
    size_t peak_live) {
  __atomic_fetch_add(&ny_native_stat_regalloc_segments,
                     (unsigned long long)segments, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_regalloc_colored,
                     (unsigned long long)colored, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_regalloc_spilled,
                     (unsigned long long)spilled, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_regalloc_reloads,
                     (unsigned long long)reloads, __ATOMIC_RELAXED);
  ny_native_atomic_max_ull(&ny_native_stat_regalloc_peak_live,
                           (unsigned long long)peak_live);
}

void ny_native_mach_regalloc_stats(unsigned long long *segments,
                                   unsigned long long *colored,
                                   unsigned long long *spilled,
                                   unsigned long long *reloads,
                                   unsigned long long *peak_live) {
  if (segments)
    *segments = __atomic_load_n(&ny_native_stat_regalloc_segments,
                                __ATOMIC_RELAXED);
  if (colored)
    *colored = __atomic_load_n(&ny_native_stat_regalloc_colored,
                               __ATOMIC_RELAXED);
  if (spilled)
    *spilled = __atomic_load_n(&ny_native_stat_regalloc_spilled,
                               __ATOMIC_RELAXED);
  if (reloads)
    *reloads = __atomic_load_n(&ny_native_stat_regalloc_reloads,
                               __ATOMIC_RELAXED);
  if (peak_live)
    *peak_live = __atomic_load_n(&ny_native_stat_regalloc_peak_live,
                                 __ATOMIC_RELAXED);
}

void ny_native_mach_fpr_record(size_t segments, size_t colored, size_t spilled,
                               size_t reloads, size_t peak_live);
__attribute__((used)) void ny_native_mach_fpr_record(
    size_t segments, size_t colored, size_t spilled, size_t reloads,
    size_t peak_live) {
  __atomic_fetch_add(&ny_native_stat_fpr_segments,
                     (unsigned long long)segments, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_fpr_colored,
                     (unsigned long long)colored, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_fpr_spilled,
                     (unsigned long long)spilled, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_fpr_reloads,
                     (unsigned long long)reloads, __ATOMIC_RELAXED);
  ny_native_atomic_max_ull(&ny_native_stat_fpr_peak_live,
                           (unsigned long long)peak_live);
}

void ny_native_mach_fpr_stats(unsigned long long *segments,
                              unsigned long long *colored,
                              unsigned long long *spilled,
                              unsigned long long *reloads,
                              unsigned long long *peak_live) {
  if (segments)
    *segments = __atomic_load_n(&ny_native_stat_fpr_segments,
                                __ATOMIC_RELAXED);
  if (colored)
    *colored = __atomic_load_n(&ny_native_stat_fpr_colored,
                               __ATOMIC_RELAXED);
  if (spilled)
    *spilled = __atomic_load_n(&ny_native_stat_fpr_spilled,
                               __ATOMIC_RELAXED);
  if (reloads)
    *reloads = __atomic_load_n(&ny_native_stat_fpr_reloads,
                               __ATOMIC_RELAXED);
  if (peak_live)
    *peak_live = __atomic_load_n(&ny_native_stat_fpr_peak_live,
                                 __ATOMIC_RELAXED);
}

void ny_native_mach_vector_record(size_t segments, size_t colored,
                                  size_t spilled, size_t reloads,
                                  size_t peak_live);
__attribute__((used)) void ny_native_mach_vector_record(
    size_t segments, size_t colored, size_t spilled, size_t reloads,
    size_t peak_live) {
  __atomic_fetch_add(&ny_native_stat_vector_segments,
                     (unsigned long long)segments, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_vector_colored,
                     (unsigned long long)colored, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_vector_spilled,
                     (unsigned long long)spilled, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_vector_reloads,
                     (unsigned long long)reloads, __ATOMIC_RELAXED);
  ny_native_atomic_max_ull(&ny_native_stat_vector_peak_live,
                           (unsigned long long)peak_live);
}

void ny_native_mach_vector_stats(unsigned long long *segments,
                                 unsigned long long *colored,
                                 unsigned long long *spilled,
                                 unsigned long long *reloads,
                                 unsigned long long *peak_live) {
  if (segments)
    *segments = __atomic_load_n(&ny_native_stat_vector_segments,
                                __ATOMIC_RELAXED);
  if (colored)
    *colored = __atomic_load_n(&ny_native_stat_vector_colored,
                               __ATOMIC_RELAXED);
  if (spilled)
    *spilled = __atomic_load_n(&ny_native_stat_vector_spilled,
                               __ATOMIC_RELAXED);
  if (reloads)
    *reloads = __atomic_load_n(&ny_native_stat_vector_reloads,
                               __ATOMIC_RELAXED);
  if (peak_live)
    *peak_live = __atomic_load_n(&ny_native_stat_vector_peak_live,
                                 __ATOMIC_RELAXED);
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
  char reloc_symbols[NY_X64_OBJ_MAX_RELOCS][256];
  size_t reloc_symbol_count = 0;
  uint32_t def_name_off = 0;
  uint32_t reloc_name_offs[NY_X64_OBJ_MAX_RELOCS] = {0};
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
  char reloc_symbols[NY_X64_OBJ_MAX_RELOCS][256];
  size_t reloc_symbol_count = 0;
  uint32_t reloc_name_offs[NY_X64_OBJ_MAX_RELOCS] = {0};
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

static bool ny_obj_uleb128(ny_obj_buf_t *b, uint64_t value) {
  do {
    unsigned char byte = (unsigned char)(value & 0x7f);
    value >>= 7;
    if (value)
      byte |= 0x80;
    if (!ny_obj_u8(b, byte))
      return false;
  } while (value);
  return true;
}

static bool ny_obj_sleb128(ny_obj_buf_t *b, int64_t value) {
  bool more = true;
  while (more) {
    unsigned char byte = (unsigned char)(value & 0x7f);
    value >>= 7;
    if ((value == 0 && !(byte & 0x40)) ||
        (value == -1 && (byte & 0x40)))
      more = false;
    else
      byte |= 0x80;
    if (!ny_obj_u8(b, byte))
      return false;
  }
  return true;
}

static int ny_native_debug_line_cmp(const void *lhs, const void *rhs) {
  const ny_native_debug_line_t *a = lhs;
  const ny_native_debug_line_t *b = rhs;
  if (a->offset < b->offset)
    return -1;
  if (a->offset > b->offset)
    return 1;
  return 0;
}

static uint32_t ny_elf_shstr_offset(const char *table, size_t table_len,
                                    const char *name) {
  if (!table || !name)
    return 0;
  size_t off = 1;
  while (off < table_len && table[off]) {
    if (strcmp(table + off, name) == 0)
      return (uint32_t)off;
    off += strlen(table + off) + 1;
  }
  return 0;
}

/*
 * Build a compact DWARF v4 line program whose initial address is relocated
 * against the first emitted function.  Relative advances then remain valid
 * when the object is linked at any address, without inventing absolute
 * addresses in a relocatable object.
 */
bool ny_native_build_debug_line(
    const ny_native_debug_line_t *lines, size_t line_count, size_t text_len,
    ny_obj_buf_t *out, size_t *base_reloc_offset, size_t *base_def_index,
    char *err, size_t err_len) {
  if (!out || !base_reloc_offset || !base_def_index ||
      (!lines && line_count))
    return false;
  out->len = 0;
  if (!line_count)
    return true;
  ny_native_debug_line_t *ordered =
      calloc(line_count, sizeof(*ordered));
  const char **files = calloc(line_count, sizeof(*files));
  unsigned *file_ids = calloc(line_count, sizeof(*file_ids));
  if (!ordered || !files || !file_ids) {
    free(ordered);
    free(files);
    free(file_ids);
    ny_native_set_err(err, err_len, "DWARF line table allocation failed");
    return false;
  }
  memcpy(ordered, lines, line_count * sizeof(*ordered));
  qsort(ordered, line_count, sizeof(*ordered), ny_native_debug_line_cmp);
  size_t file_count = 0;
  for (size_t i = 0; i < line_count; ++i) {
    const char *file = ordered[i].file && ordered[i].file[0]
                           ? ordered[i].file
                           : "<source>";
    size_t file_id = 0;
    for (; file_id < file_count; ++file_id)
      if (strcmp(files[file_id], file) == 0)
        break;
    if (file_id == file_count)
      files[file_count++] = file;
    file_ids[i] = (unsigned)file_id + 1;
  }

  if (!ny_obj_u32(out, 0))
    goto oom;
  if (!ny_obj_u16(out, 4))
    goto oom;
  size_t header_len_off = out->len;
  if (!ny_obj_u32(out, 0) ||
      !ny_obj_u8(out, 1) ||       /* minimum instruction length */
      !ny_obj_u8(out, 1) ||       /* maximum operations per instruction */
      !ny_obj_u8(out, 1) ||       /* default_is_stmt */
      !ny_obj_u8(out, (unsigned char)-5) ||
      !ny_obj_u8(out, 14) ||
      !ny_obj_u8(out, 13))
    goto oom;
  static const unsigned char standard_lengths[12] =
      {0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1};
  if (!ny_obj_emit(out, standard_lengths, sizeof(standard_lengths)) ||
      !ny_obj_u8(out, 0)) /* include_directories terminator */
    goto oom;
  for (size_t i = 0; i < file_count; ++i) {
    if (!ny_obj_emit(out, files[i], strlen(files[i]) + 1) ||
        !ny_obj_uleb128(out, 0) || !ny_obj_uleb128(out, 0) ||
        !ny_obj_uleb128(out, 0))
      goto oom;
  }
  if (!ny_obj_u8(out, 0)) /* file_names terminator */
    goto oom;
  size_t program_off = out->len;
  if (!ny_obj_u8(out, 0) || !ny_obj_u8(out, 9) || !ny_obj_u8(out, 2))
    goto oom;
  *base_reloc_offset = out->len;
  if (!ny_obj_u64(out, 0))
    goto oom;
  *base_def_index = ordered[0].def_index;
  size_t previous_offset = ordered[0].offset;
  unsigned previous_file = 1;
  int64_t previous_line = 1;
  unsigned previous_column = 0;
  for (size_t i = 0; i < line_count; ++i) {
    if (ordered[i].offset < previous_offset)
      continue;
    size_t delta = ordered[i].offset - previous_offset;
    if (!ny_obj_u8(out, 2) || !ny_obj_uleb128(out, delta))
      goto oom;
    if (file_ids[i] != previous_file) {
      if (!ny_obj_u8(out, 4) || !ny_obj_uleb128(out, file_ids[i]))
        goto oom;
      previous_file = file_ids[i];
    }
    unsigned column = ordered[i].column ? ordered[i].column : 1;
    if (column != previous_column &&
        (!ny_obj_u8(out, 5) || !ny_obj_uleb128(out, column)))
      goto oom;
    previous_column = column;
    int64_t line = ordered[i].line ? (int64_t)ordered[i].line : 1;
    if (!ny_obj_u8(out, 3) || !ny_obj_sleb128(out, line - previous_line) ||
        !ny_obj_u8(out, 1))
      goto oom;
    previous_line = line;
    previous_offset = ordered[i].offset;
  }
  if (text_len >= previous_offset &&
      (!ny_obj_u8(out, 2) ||
       !ny_obj_uleb128(out, text_len - previous_offset)))
    goto oom;
  /*
   * DW_LNE_end_sequence
   */
  if (!ny_obj_u8(out, 0) || !ny_obj_u8(out, 1) || !ny_obj_u8(out, 1))
    goto oom;
  ny_obj_patch_u32(out, header_len_off,
                   (uint32_t)(program_off - (header_len_off + 4)));
  ny_obj_patch_u32(out, 0, (uint32_t)(out->len - 4));
  free(ordered);
  free(files);
  free(file_ids);
  return true;

oom:
  free(ordered);
  free(files);
  free(file_ids);
  ny_native_set_err(err, err_len, "DWARF line table emission failed");
  return false;
}

/*
 * Emit a compact CodeView C13 line subsection for COFF consumers.
 */
static bool ny_native_build_codeview_lines(
    const ny_native_debug_line_t *lines, size_t line_count, size_t code_len,
    const char *path, ny_obj_buf_t *out, char *err, size_t err_len) {
  if (!out || (!lines && line_count))
    return false;
  out->len = 0;
  if (!line_count)
    return true;
  if (!ny_obj_u32(out, 4))
    goto fail;
  size_t name_len = strlen(path && path[0] ? path : "nytrix");
  size_t obj_payload = 4 + name_len + 1;
  if (obj_payload > UINT16_MAX - 2 ||
      !ny_obj_u16(out, (uint16_t)(obj_payload + 2)) ||
      !ny_obj_u16(out, 0x1101) || !ny_obj_u32(out, 0) ||
      !ny_obj_emit(out, path && path[0] ? path : "nytrix", name_len + 1))
    goto fail;
  while (out->len & 3u)
    if (!ny_obj_u8(out, 0))
      goto fail;
  size_t payload = 12 + 12 + line_count * 8;
  if (payload > UINT16_MAX - 2 ||
      !ny_obj_u16(out, (uint16_t)(payload + 2)) ||
      !ny_obj_u16(out, 0x00f2) || !ny_obj_u32(out, 0) ||
      !ny_obj_u16(out, 1) || !ny_obj_u16(out, 0) ||
      !ny_obj_u32(out, (uint32_t)code_len) || !ny_obj_u32(out, 0) ||
      !ny_obj_u32(out, (uint32_t)line_count) ||
      !ny_obj_u32(out, (uint32_t)(line_count * 8)))
    goto fail;
  for (size_t i = 0; i < line_count; ++i) {
    uint32_t line = lines[i].line ? lines[i].line : 1;
    uint32_t column = lines[i].column ? lines[i].column : 1;
    uint32_t flags = (line & 0x00ffffffu) | ((column & 0x7fu) << 24);
    if (!ny_obj_u32(out, (uint32_t)lines[i].offset) ||
        !ny_obj_u32(out, flags))
      goto fail;
  }
  while (out->len & 3u)
    if (!ny_obj_u8(out, 0))
      goto fail;
  return true;
fail:
  out->len = 0;
  ny_native_set_err(err, err_len, "CodeView line table emission failed");
  return false;
}

static bool ny_native_emit_elf64_x64_object_bundle_code(
    const unsigned char *code, size_t code_len,
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count,
    const ny_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    const ny_native_debug_line_t *debug_lines, size_t debug_line_count,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    ny_native_set_err(err, err_len, "x86-64 ELF object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  bool ok = false;
  char (*reloc_symbols)[256] =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t *def_name_offs = calloc(def_count, sizeof(*def_name_offs));
  uint32_t *reloc_name_offs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_name_offs));
  if (!reloc_symbols || !def_name_offs || !reloc_name_offs) {
    free(reloc_symbols);
    free(def_name_offs);
    free(reloc_name_offs);
    ny_native_set_err(err, err_len, "x86-64 ELF object writer: allocation failed");
    goto done;
  }
  if (!ny_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;

  const char shstr[] =
      "\0.text\0.rela.text\0.data\0.debug_line\0.rela.debug_line\0.symtab\0.strtab\0.shstrtab\0";
  const uint32_t sh_text = ny_elf_shstr_offset(shstr, sizeof(shstr), ".text");
  const uint32_t sh_rela_text =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".rela.text");
  const uint32_t sh_data = ny_elf_shstr_offset(shstr, sizeof(shstr), ".data");
  const uint32_t sh_debug_line =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".debug_line");
  const uint32_t sh_rela_debug_line =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".rela.debug_line");
  const uint32_t sh_symtab =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".symtab");
  const uint32_t sh_strtab =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".strtab");
  const uint32_t sh_shstrtab =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".shstrtab");
  /*
   * Function bodies and backend pools are emitted contiguously.  Optional
   * debug sections are inserted after .data and before the symbol tables.
   */
  const uint32_t sec_data = 3;
  size_t data_start = code_len;
  size_t data_end = code_len;
  bool have_data = false;
  for (size_t i = 0; i < def_count; ++i) {
    if (!defs[i].is_data)
      continue;
    if (!have_data || defs[i].off < data_start)
      data_start = defs[i].off;
    size_t end = defs[i].off + defs[i].size;
    if (!have_data || end > data_end)
      data_end = end;
    have_data = true;
  }
  size_t text_len = have_data ? data_start : code_len;
  size_t data_len = have_data ? data_end - data_start : 0;
  ny_obj_buf_t debug_line = {0};
  size_t debug_base_reloc = 0;
  size_t debug_base_def = 0;
  bool have_debug = debug_line_count > 0;
  if (have_debug && !ny_native_build_debug_line(
                        debug_lines, debug_line_count, text_len, &debug_line,
                        &debug_base_reloc, &debug_base_def, err, err_len))
    goto done;
  if (have_debug && debug_base_def >= def_count)
    goto done;
  uint32_t sec_debug = have_debug ? (have_data ? 4u : 3u) : 0u;
  uint32_t sec_rela_debug = have_debug ? sec_debug + 1u : 0u;
  uint32_t symtab_idx = have_debug
                            ? sec_rela_debug + 1u
                            : (have_data ? 4u : 3u);
  uint32_t strtab_idx = symtab_idx + 1u;
  uint32_t shstrtab_idx = strtab_idx + 1u;
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
  if (!ny_obj_emit(&file, code, text_len) || !ny_obj_pad_to(&file, 8))
    goto done;
  size_t data_off = file.len;
  if (have_data) {
    if (!ny_obj_emit(&file, code + data_start, data_len) ||
        !ny_obj_pad_to(&file, 8))
      goto done;
  }
  size_t debug_off = file.len;
  if (have_debug &&
      (!ny_obj_emit(&file, debug_line.data, debug_line.len) ||
       !ny_obj_pad_to(&file, 8)))
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
    /*
     * R_X86_64_PC32 = 2, R_X86_64_PLT32 = 4
     */
    uint64_t rtype = (relocs[i].type == NY_RELOC_PC32) ? 2u : 4u;
    uint64_t info = ((uint64_t)sym_index << 32) | rtype;
    if (!ny_obj_u64(&file, relocs[i].disp_off) || !ny_obj_u64(&file, info) ||
        !ny_obj_u64(&file, (uint64_t)-4LL))
      goto done;
  }
  size_t rela_size = file.len - rela_off;
  size_t rela_debug_off = file.len;
  if (have_debug) {
    uint64_t info = ((uint64_t)(1u + (uint32_t)debug_base_def) << 32) | 1u;
    if (!ny_obj_u64(&file, debug_base_reloc) || !ny_obj_u64(&file, info) ||
        !ny_obj_u64(&file, 0))
      goto done;
  }
  size_t rela_debug_size = file.len - rela_debug_off;
  size_t symtab_off = file.len;
  if (!ny_elf64_write_sym(&file, 0, 0, 0, 0, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    bool in_data = have_data && defs[i].off >= data_start &&
                   defs[i].off < data_end;
    uint64_t value = in_data ? defs[i].off - data_start : defs[i].off;
    /*
     * Symbols living in the writable pool (string literals, consttab
     * constants, array storage) must be typed STT_OBJECT in .data;
     * typing them STT_FUNC in .text made strict consumers (the test
     * suite's internal ELF linker) reject otherwise-valid objects.
     */
    unsigned info = in_data ? 0x11u /* GLOBAL|OBJECT */ : 0x12u /* GLOBAL|FUNC */;
    if (!ny_elf64_write_sym(&file, def_name_offs[i], info,
                            in_data ? sec_data : 1, value, defs[i].size))
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
  /*
   * The .data header exists ONLY when the pool contributed bytes.
   * Writing a placeholder unconditionally shifted every later section
   * index by one while the link fields and e_shstrndx still assumed the
   * dense layout -- symtab linked to itself and section names were read
   * from the symbol string table (<corrupt> symbols at link time).
   */
  size_t shoff = file.len;
  if (!ny_elf64_write_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !ny_elf64_write_sh(&file, sh_text, 1, 0x6, text_off, text_len, 0, 0,
                         16, 0) ||
      !ny_elf64_write_sh(&file, sh_rela_text, 4, 0, rela_off, rela_size,
                         symtab_idx, 1, 8, 24))
    goto done;
  if (have_data &&
      !ny_elf64_write_sh(&file, sh_data, 1, 0x3, data_off, data_len, 0, 0,
                         8, 0))
    goto done;
  if (have_debug &&
      (!ny_elf64_write_sh(&file, sh_debug_line, 1, 0, debug_off,
                          debug_line.len, 0, 0, 1, 0) ||
       !ny_elf64_write_sh(&file, sh_rela_debug_line, 4, 0, rela_debug_off,
                          rela_debug_size, symtab_idx, sec_debug, 8, 24)))
    goto done;
  if (!ny_elf64_write_sh(&file, sh_symtab, 2, 0, symtab_off, symtab_size,
                         strtab_idx, 1, 8, 24) ||
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
  ny_obj_patch_u16(&file, 60, (uint16_t)(shstrtab_idx + 1));
  ny_obj_patch_u16(&file, 62, (uint16_t)shstrtab_idx);

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(reloc_symbols);
  free(def_name_offs);
  free(reloc_name_offs);
  ny_obj_free(&debug_line);
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
    const ny_native_debug_line_t *debug_lines, size_t debug_line_count,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    ny_native_set_err(err, err_len, "x86-64 COFF object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strings = {0};
  ny_obj_buf_t debug = {0};
  bool ok = false;
  char (*reloc_symbols)[256] =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t *def_name_offs = calloc(def_count, sizeof(*def_name_offs));
  uint32_t *reloc_name_offs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_name_offs));
  if (!reloc_symbols || !def_name_offs || !reloc_name_offs) {
    free(reloc_symbols);
    free(def_name_offs);
    free(reloc_name_offs);
    ny_native_set_err(err, err_len, "x86-64 COFF object writer: allocation failed");
    goto done;
  }
  const size_t header_size = 20;
  if (!ny_native_build_codeview_lines(debug_lines, debug_line_count, code_len,
                                      path, &debug, err, err_len))
    goto done;
  const size_t section_count = debug.len ? 2 : 1;
  const size_t section_table_size = 40 * section_count;
  const size_t text_off = header_size + section_table_size;
  const size_t reloc_off = text_off + code_len;
  const size_t reloc_size = reloc_count * 10;
  const size_t debug_off = reloc_off + reloc_size;
  const size_t symtab_off = debug_off + debug.len;

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
  if (section_count == 2) {
    char debug_name[8] = {0};
    memcpy(debug_name, ".debug$S", 8);
    if (!ny_obj_emit(&file, debug_name, sizeof(debug_name)) ||
        !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0) ||
        !ny_obj_u32(&file, (uint32_t)debug.len) ||
        !ny_obj_u32(&file, (uint32_t)debug_off) || !ny_obj_u32(&file, 0) ||
        !ny_obj_u32(&file, 0) || !ny_obj_u16(&file, 0) || !ny_obj_u16(&file, 0) ||
        !ny_obj_u32(&file, 0x42000040u))
      goto done;
  }

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
  if (debug.len && !ny_obj_emit(&file, debug.data, debug.len))
    goto done;
  if (!ny_obj_emit(&file, strings.data, strings.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(reloc_symbols);
  free(def_name_offs);
  free(reloc_name_offs);
  ny_obj_free(&strings);
  ny_obj_free(&debug);
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
    const ny_native_debug_line_t *debug_lines, size_t debug_line_count,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer: missing input");
    return false;
  }
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  ny_obj_buf_t debug = {0};
  bool ok = false;
  char (*reloc_symbols)[256] =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t *def_name_offs = calloc(def_count, sizeof(*def_name_offs));
  uint32_t *reloc_name_offs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_name_offs));
  char (*macho_defs)[256] = calloc(def_count, sizeof(*macho_defs));
  char (*macho_relocs)[256] =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*macho_relocs));
  if (!reloc_symbols || !def_name_offs || !reloc_name_offs ||
      !macho_defs || !macho_relocs) {
    free(reloc_symbols);
    free(def_name_offs);
    free(reloc_name_offs);
    free(macho_defs);
    free(macho_relocs);
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer: allocation failed");
    goto done;
  }

  if (!ny_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;
  size_t debug_base_reloc = 0, debug_base_def = 0;
  if (!ny_native_build_debug_line(
          debug_lines, debug_line_count, code_len, &debug, &debug_base_reloc,
          &debug_base_def, err, err_len))
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

  const uint32_t section_count = debug.len ? 2 : 1;
  const uint32_t seg_cmdsize = 72 + 80 * section_count;
  const uint32_t sym_cmdsize = 24;
  const uint32_t sizeofcmds = seg_cmdsize + sym_cmdsize;
  const uint32_t text_off = 32 + sizeofcmds;
  const uint32_t reloc_off = text_off + (uint32_t)code_len;
  const uint32_t debug_off = reloc_off + (uint32_t)(reloc_count * 8);
  const uint32_t symoff = debug_off + (uint32_t)debug.len;
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
      !ny_obj_u32(&file, 5) || !ny_obj_u32(&file, section_count) ||
      !ny_obj_u32(&file, 0))
    goto done;
  if (!ny_macho_write_padded_name(&file, "__text") ||
      !ny_macho_write_padded_name(&file, "__TEXT") || !ny_obj_u64(&file, 0) ||
      !ny_obj_u64(&file, code_len) || !ny_obj_u32(&file, text_off) ||
      !ny_obj_u32(&file, 4) || !ny_obj_u32(&file, reloc_count ? reloc_off : 0) ||
      !ny_obj_u32(&file, (uint32_t)reloc_count) || !ny_obj_u32(&file, 0x80000400u) ||
      !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0))
    goto done;
  if (section_count == 2) {
    if (!ny_macho_write_padded_name(&file, "__debug_line") ||
        !ny_macho_write_padded_name(&file, "__DWARF") ||
        !ny_obj_u64(&file, 0) || !ny_obj_u64(&file, debug.len) ||
        !ny_obj_u32(&file, debug_off) || !ny_obj_u32(&file, 0) ||
        !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0) ||
        !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0) ||
        !ny_obj_u32(&file, 0) || !ny_obj_u32(&file, 0))
      goto done;
  }
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
  if (debug.len && !ny_obj_emit(&file, debug.data, debug.len))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_obj_u32(&file, def_name_offs[i]) || !ny_obj_u8(&file, 0x0f) ||
        !ny_obj_u8(&file, 1) || !ny_obj_u16(&file, 0) ||
        !ny_obj_u16(&file, 0) || !ny_obj_u64(&file, defs[i].off))
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
  free(reloc_symbols);
  free(def_name_offs);
  free(reloc_name_offs);
  free(macho_defs);
  free(macho_relocs);
  ny_obj_free(&strtab);
  ny_obj_free(&debug);
  ny_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "x86-64 Mach-O object writer failed");
  return ok;
}

bool ny_x64_obj_build_bundle(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
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
  /*
   * The NYIR object writer cannot encode PHIs.  Callers that keep PHIs alive
   * through optimization (preserve-phi backends) must destroy them before
   * reaching this encoder.
   */
  nyir_phi_elim((nyir_func_t *)rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyir_phi_elim((nyir_func_t *)&funcs[i]);
  for (size_t i = 0; i < func_count; ++i) {
    const char *name = func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (!ny_x64_obj_append_function(code, defs, def_count, relocs, reloc_count,
                                    &funcs[i], target, symbol, false, err,
                                    err_len)) {
      if (err && err_len && !err[0])
        ny_native_set_err(err, err_len,
                          "x86-64 object writer: function append failed for %s",
                          symbol);
      return false;
    }
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s", target->symbol_prefix ? target->symbol_prefix : "",
           entry_symbol);
  if (!ny_x64_obj_append_function(code, defs, def_count, relocs, reloc_count,
                                  rt_main, target, entry, tag_return, err,
                                  err_len))
    return false;
  if (!ny_native_strtab_append_defs(code, defs, def_count, err, err_len))
    return false;
  return ny_native_arraytab_append_defs(code, defs, def_count, err, err_len);
}

/*
 * Build an x86-64 object bundle one function at a time.  A machine-form
 * failure is recorded with its symbol and owning reason, then only that
 * function uses the established NYIR object encoder.
 */
static bool ny_native_x64_build_mixed_bundle_debug(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    ny_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
  if (!rt_main || !target || !entry_symbol || !entry_symbol[0] || !code ||
      !defs || !def_count || !relocs || !reloc_count ||
      (func_count && !funcs))
    return false;
  code->len = 0;
  *def_count = 0;
  *reloc_count = 0;
  if (debug_lines)
    *debug_lines = NULL;
  if (debug_line_count)
    *debug_line_count = 0;
  if (debug_line_cap)
    *debug_line_cap = 0;
  for (size_t i = 0; i < func_count; ++i) {
    const char *name = func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (ny_x64_obj_def_index(defs, *def_count, symbol) >= 0)
      continue;
    char reason[256] = {0};
    ny_mach_func_t mach = {0};
    bool machine = ny_mach_lower_nir(&funcs[i], &mach, target->caps, reason,
                                     sizeof(reason));
    if (machine)
      machine = ny_x64_mach_append_function_debug(
          code, defs, def_count, relocs, reloc_count, &mach, target, symbol,
          false, funcs ? &funcs[i] : NULL, debug_lines, debug_line_count,
          debug_line_cap, reason, sizeof(reason));
    ny_mach_func_free(&mach);
    if (machine) {
      ny_native_mach_record(true, symbol, NULL);
      continue;
    }
    ny_native_mach_record(false, symbol, reason);
    char fallback_err[256] = {0};
    /*
     * The NYIR object writer has no PHI support.  When the machine path is
     * unavailable (e.g. shapes mach lowering rejects), destroy PHIs in this
     * function first instead of failing the whole object with
     * "unsupported op phi".
     */
    nyir_phi_elim((nyir_func_t *)&funcs[i]);
    if (!ny_x64_obj_append_function_debug(
            code, defs, def_count, relocs, reloc_count, &funcs[i], target,
            symbol, false, &funcs[i], debug_lines, debug_line_count,
            debug_line_cap, fallback_err, sizeof(fallback_err))) {
      if (getenv("NY_DUMP_MACH"))
        fprintf(stderr, "native bundle fallback failed %s: machine=%s nir=%s\n",
                symbol, reason[0] ? reason : "unsupported",
                fallback_err[0] ? fallback_err : "unknown");
      ny_native_set_err(
          err, err_len,
          "native function %s: machine form failed: %s; NYIR fallback failed: %s",
          symbol, reason[0] ? reason : "unsupported machine shape",
          fallback_err[0] ? fallback_err : "object emission failed");
      return false;
    }
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  char reason[256] = {0};
  ny_mach_func_t top_mach = {0};
  bool machine = ny_mach_lower_nir(rt_main, &top_mach, target->caps, reason,
                                   sizeof(reason));
  if (machine)
    machine = ny_x64_mach_append_function_debug(
        code, defs, def_count, relocs, reloc_count, &top_mach, target, entry,
        tag_return, rt_main, debug_lines, debug_line_count, debug_line_cap,
        reason, sizeof(reason));
  ny_mach_func_free(&top_mach);
  if (machine) {
    ny_native_mach_record(true, entry, NULL);
  } else {
    if (getenv("NY_DUMP_MACH"))
      fprintf(stderr, "ENTRY-FALLBACK %s reason=%s\n", entry, reason);
    ny_native_mach_record(false, entry, reason);
    char fallback_err[256] = {0};
    nyir_phi_elim((nyir_func_t *)rt_main);
    if (!ny_x64_obj_append_function_debug(
            code, defs, def_count, relocs, reloc_count, rt_main, target, entry,
            tag_return, rt_main, debug_lines, debug_line_count, debug_line_cap,
            fallback_err, sizeof(fallback_err))) {
      ny_native_set_err(
          err, err_len,
          "native function %s: machine form failed: %s; NYIR fallback failed: %s",
          entry, reason[0] ? reason : "unsupported machine shape",
          fallback_err[0] ? fallback_err : "object emission failed");
      return false;
    }
  }
  if (!ny_native_strtab_append_defs(code, defs, def_count, err, err_len))
    return false;
  if (!ny_native_consttab_append_defs(code, defs, def_count, err, err_len))
    return false;
  if (!ny_native_globaltab_append_defs(code, defs, def_count, err, err_len))
    return false;
  return ny_native_arraytab_append_defs(code, defs, def_count, err, err_len);
}

bool ny_native_x64_build_mixed_bundle(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len) {
  return ny_native_x64_build_mixed_bundle_debug(
      rt_main, funcs, func_names, func_count, target, entry_symbol, tag_return,
      code, defs, def_count, relocs, reloc_count, NULL, NULL, NULL, err,
      err_len);
}

bool ny_native_emit_elf64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t *defs =
      calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  ny_x64_obj_reloc_t *relocs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0;
  size_t reloc_count = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    ny_native_set_err(err, err_len, "x86-64 ELF object pool allocation failed");
    return false;
  }
  ny_native_debug_line_t *debug_lines = NULL;
  size_t debug_line_count = 0;
  size_t debug_line_cap = 0;
  bool ok = ny_native_x64_build_mixed_bundle_debug(
      rt_main, funcs, func_names, func_count, target,
      entry_symbol ? entry_symbol : "rt_main", tag_return, &code, defs,
      &def_count, relocs, &reloc_count, &debug_lines, &debug_line_count,
      &debug_line_cap, err, err_len);
  if (ok && target && target->target == NY_NATIVE_TARGET_X86_64) {
    if (!debug_lines) {
      debug_line_cap = func_count + 1;
      if (debug_line_cap < 32)
        debug_line_cap = 32;
      debug_lines = calloc(debug_line_cap, sizeof(*debug_lines));
      if (!debug_lines) {
        ny_native_set_err(err, err_len, "x86-64 source map allocation failed");
        ok = false;
      }
    }
    if (ok) {
      for (size_t i = 0; i < func_count; ++i) {
        const char *name = func_names && func_names[i]
                               ? func_names[i]
                               : "unknown_fn";
        char symbol[256];
        snprintf(symbol, sizeof(symbol), NY_FMT_FN,
                 target->symbol_prefix ? target->symbol_prefix : "", name);
        int def_i = ny_x64_obj_def_index(defs, def_count, symbol);
        if (def_i < 0 || defs[def_i].is_data)
          continue;
        for (size_t j = 0; funcs && j < funcs[i].len; ++j) {
          const nyir_debug_loc_t *loc = &funcs[i].data[j].debug;
          if (!loc->line)
            continue;
          if (!ny_native_debug_line_append(
                  &debug_lines, &debug_line_count, &debug_line_cap,
                  (ny_native_debug_line_t){(size_t)def_i, defs[def_i].off,
                                           loc->file, loc->line, loc->column})) {
            ny_native_set_err(err, err_len, "x86-64 source map allocation failed");
            ok = false;
          }
          break;
        }
      }
      const char *entry = entry_symbol && entry_symbol[0]
                              ? entry_symbol
                              : "rt_main";
      char entry_name[256];
      snprintf(entry_name, sizeof(entry_name), "%s%s",
               target->symbol_prefix ? target->symbol_prefix : "", entry);
      int entry_i = ny_x64_obj_def_index(defs, def_count, entry_name);
      if (entry_i >= 0 && !defs[entry_i].is_data)
        for (size_t j = 0; j < rt_main->len; ++j) {
          const nyir_debug_loc_t *loc = &rt_main->data[j].debug;
          if (!loc->line)
            continue;
          if (!ny_native_debug_line_append(
                  &debug_lines, &debug_line_count, &debug_line_cap,
                  (ny_native_debug_line_t){(size_t)entry_i, defs[entry_i].off,
                                           loc->file, loc->line, loc->column})) {
            ny_native_set_err(err, err_len, "x86-64 source map allocation failed");
            ok = false;
          }
          break;
        }
    }
  }
  /*
   * Prefer denser I-cache when NyP heat is high: keep hot entry last, sort
   * helpers by ascending size (already emit order in machine form bundle).
   */
  ok = ok && ny_native_emit_elf64_x64_object_bundle_code(
                 code.data, code.len, relocs, reloc_count, defs, def_count,
                 path, debug_lines, debug_line_count, err, err_len);
  free(debug_lines);
  ny_obj_free(&code);
  free(defs);
  free(relocs);
  return ok;
}

typedef bool (*ny_x64_bundle_writer_fn)(const unsigned char *code, size_t code_len,
                                        const ny_x64_obj_reloc_t *relocs,
                                        size_t reloc_count,
                                        const ny_x64_obj_symbol_def_t *defs,
                                        size_t def_count, const char *path,
                                        const ny_native_debug_line_t *debug_lines,
                                        size_t debug_line_count, char *err,
                                        size_t err_len);
typedef bool (*ny_x64_single_writer_fn)(const unsigned char *code, size_t code_len,
                                        const ny_x64_obj_reloc_t *relocs,
                                        size_t reloc_count, const char *path,
                                        const char *symbol_name, char *err,
                                        size_t err_len);

static bool ny_native_append_bundle_source_map(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    const ny_x64_obj_symbol_def_t *defs, size_t def_count,
    ny_native_debug_line_t **lines, size_t *line_count, size_t *line_cap,
    char *err, size_t err_len) {
  if (!target || !defs || !lines || !line_count || !line_cap)
    return false;
  if (!*lines) {
    *line_cap = func_count + 1;
    if (*line_cap < 32)
      *line_cap = 32;
    *lines = calloc(*line_cap, sizeof(**lines));
    if (!*lines) {
      ny_native_set_err(err, err_len, "native source map allocation failed");
      return false;
    }
  }
  for (size_t i = 0; i < func_count; ++i) {
    const char *name = func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    int def_i = ny_x64_obj_def_index(defs, def_count, symbol);
    if (def_i < 0 || defs[def_i].is_data)
      continue;
    for (size_t j = 0; funcs && j < funcs[i].len; ++j) {
      const nyir_debug_loc_t *loc = &funcs[i].data[j].debug;
      if (!loc->line)
        continue;
      if (!ny_native_debug_line_append(
              lines, line_count, line_cap,
              (ny_native_debug_line_t){(size_t)def_i, defs[def_i].off,
                                       loc->file, loc->line, loc->column})) {
        ny_native_set_err(err, err_len, "native source map allocation failed");
        return false;
      }
      break;
    }
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "",
           entry_symbol && entry_symbol[0] ? entry_symbol : "rt_main");
  int entry_i = ny_x64_obj_def_index(defs, def_count, entry);
  if (entry_i >= 0 && !defs[entry_i].is_data && rt_main) {
    for (size_t j = 0; j < rt_main->len; ++j) {
      const nyir_debug_loc_t *loc = &rt_main->data[j].debug;
      if (!loc->line)
        continue;
      if (!ny_native_debug_line_append(
              lines, line_count, line_cap,
              (ny_native_debug_line_t){(size_t)entry_i, defs[entry_i].off,
                                       loc->file, loc->line, loc->column})) {
        ny_native_set_err(err, err_len, "native source map allocation failed");
        return false;
      }
      break;
    }
  }
  return true;
}

static bool ny_native_emit_x64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len,
    ny_x64_bundle_writer_fn write_bundle) {
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t *defs =
      calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  ny_x64_obj_reloc_t *relocs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0, reloc_count = 0;
  ny_native_debug_line_t *debug_lines = NULL;
  size_t debug_line_count = 0, debug_line_cap = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    ny_native_set_err(err, err_len, "x86-64 object pool allocation failed");
    return false;
  }
  bool built = ny_native_x64_build_mixed_bundle_debug(
      rt_main, funcs, func_names, func_count, target, entry_symbol, tag_return,
      &code, defs, &def_count, relocs, &reloc_count, &debug_lines,
      &debug_line_count, &debug_line_cap, err, err_len);
  if (built && target && target->target == NY_NATIVE_TARGET_X86_64 &&
      !ny_native_append_bundle_source_map(
          rt_main, funcs, func_names, func_count, target, entry_symbol, defs,
          def_count, &debug_lines, &debug_line_count, &debug_line_cap, err,
          err_len))
    built = false;
  bool ok = built &&
            write_bundle(code.data, code.len, relocs, reloc_count, defs,
                         def_count, path, debug_lines, debug_line_count, err,
                         err_len);
  free(debug_lines);
  ny_obj_free(&code);
  free(defs);
  free(relocs);
  return ok;
}

static bool ny_native_emit_x64_object_from_nir(
    const nyir_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len, const char *missing_msg, ny_x64_single_writer_fn write) {
  if (!nyir || !path || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len, "%s", missing_msg);
    return false;
  }
  ny_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_x64_obj_emit_code(&ctx, nyir, tag_return)) {
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }
  bool ok = write(ctx.code.data, ctx.code.len, ctx.relocs, ctx.reloc_count, path,
                  symbol_name, err, err_len);
  ny_x64_obj_ctx_free(&ctx);
  return ok;
}

bool ny_native_emit_coff_x64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  return ny_native_emit_x64_object_from_nirs(
      rt_main, funcs, func_names, func_count, target, path, entry_symbol,
      tag_return, err, err_len, ny_native_emit_coff_x64_object_bundle_code);
}

bool ny_native_emit_macho_x64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  return ny_native_emit_x64_object_from_nirs(
      rt_main, funcs, func_names, func_count, target, path, entry_symbol,
      tag_return, err, err_len, ny_native_emit_macho_x64_object_bundle_code);
}

bool ny_native_emit_coff_x64_object_from_nir(const nyir_func_t *nyir,
                                             const ny_native_target_info_t *target,
                                             const char *path,
                                             const char *symbol_name,
                                             bool tag_return, char *err,
                                             size_t err_len) {
  return ny_native_emit_x64_object_from_nir(
      nyir, target, path, symbol_name, tag_return, err, err_len,
      "x86-64 COFF object writer: missing input",
      ny_native_emit_coff_x64_object_code);
}

bool ny_native_emit_macho_x64_object_from_nir(const nyir_func_t *nyir,
                                              const ny_native_target_info_t *target,
                                              const char *path,
                                              const char *symbol_name,
                                              bool tag_return, char *err,
                                              size_t err_len) {
  return ny_native_emit_x64_object_from_nir(
      nyir, target, path, symbol_name, tag_return, err, err_len,
      "x86-64 Mach-O object writer: missing input",
      ny_native_emit_macho_x64_object_code);
}

bool ny_native_emit_elf64_object_from_nir(const nyir_func_t *nyir,
                                          const ny_native_target_info_t *target,
                                          const char *path,
                                          const char *symbol_name,
                                          bool tag_return, char *err,
                                          size_t err_len) {
  if (!nyir || !path || !symbol_name || !symbol_name[0]) {
    ny_native_set_err(err, err_len,
                      "x86-64 ELF object writer: missing input");
    return false;
  }
  ny_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_x64_obj_emit_code(&ctx, nyir, tag_return)) {
    ny_x64_obj_ctx_free(&ctx);
    return false;
  }

  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  bool ok = false;
  char reloc_symbols[NY_X64_OBJ_MAX_RELOCS][256];
  size_t reloc_symbol_count = 0;
  uint32_t reloc_name_offs[NY_X64_OBJ_MAX_RELOCS] = {0};
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
    /*
     * R_X86_64_PC32 = 2, R_X86_64_PLT32 = 4
     */
    uint64_t rtype = (ctx.relocs[i].type == NY_RELOC_PC32) ? 2u : 4u;
    uint64_t info = ((uint64_t)(2 + sym_i) << 32) | rtype;
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
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
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
  ny_obj_buf_t file = {0};
  ny_obj_buf_t strtab = {0};
  ny_i386_obj_symbol_def_t *defs =
      calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  ny_i386_obj_reloc_t *relocs = NULL;
  size_t def_count = 0;
  size_t reloc_count = 0;
  size_t reloc_cap = 0;
  char (*reloc_symbols)[256] = NULL;
  uint32_t *reloc_name_offs = NULL;
  bool ok = false;
  uint32_t *def_name_offs = NULL;
  if (!defs) {
    ny_native_set_err(err, err_len,
                      "i386 ELF object writer: definition allocation failed");
    goto done;
  }

  for (size_t i = 0; i < func_count; ++i) {
    if (def_count >= NY_NATIVE_MAX_DEFS) {
      ny_native_set_err(err, err_len, "i386 ELF object writer: too many functions");
      goto done;
    }
    const char *name = func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (ny_i386_obj_def_index(defs, def_count, symbol) >= 0) {
      ny_native_set_err(err, err_len,
                        "i386 ELF object writer: duplicate symbol %s", symbol);
      goto done;
    }
    if (!ny_obj_pad_to(&code, 16)) {
      ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
      goto done;
    }
    size_t base = code.len;
    ny_i386_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
    if (!ny_i386_obj_emit_code(&ctx, &funcs[i], false)) {
      ny_i386_obj_ctx_free(&ctx);
      goto done;
    }
    if (!ny_i386_obj_reserve_relocs(&relocs, &reloc_cap,
                                    reloc_count + ctx.reloc_count, err,
                                    err_len)) {
      ny_i386_obj_ctx_free(&ctx);
      goto done;
    }
    if (!ny_obj_emit(&code, ctx.code.data, ctx.code.len)) {
      ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
      ny_i386_obj_ctx_free(&ctx);
      goto done;
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

  if (def_count >= NY_NATIVE_MAX_DEFS) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: too many functions");
    goto done;
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  if (ny_i386_obj_def_index(defs, def_count, entry) >= 0) {
    ny_native_set_err(err, err_len,
                      "i386 ELF object writer: duplicate symbol %s", entry);
    goto done;
  }
  if (!ny_obj_pad_to(&code, 16)) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
    goto done;
  }
  size_t entry_base = code.len;
  ny_i386_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!ny_i386_obj_emit_code(&ctx, rt_main, tag_return)) {
    ny_i386_obj_ctx_free(&ctx);
    goto done;
  }
  if (!ny_i386_obj_reserve_relocs(&relocs, &reloc_cap,
                                  reloc_count + ctx.reloc_count, err,
                                  err_len)) {
    ny_i386_obj_ctx_free(&ctx);
    goto done;
  }
  if (!ny_obj_emit(&code, ctx.code.data, ctx.code.len)) {
    ny_native_set_err(err, err_len, "i386 ELF object writer: out of memory");
    ny_i386_obj_ctx_free(&ctx);
    goto done;
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

  size_t reloc_symbol_count = 0;
  def_name_offs = calloc(def_count, sizeof(*def_name_offs));
  if (!def_name_offs) {
    ny_native_set_err(err, err_len,
                      "i386 ELF object writer: definition allocation failed");
    goto done;
  }
  if (reloc_count > 0) {
    reloc_symbols = calloc(reloc_count, sizeof(*reloc_symbols));
    reloc_name_offs = calloc(reloc_count, sizeof(*reloc_name_offs));
    if (!reloc_symbols || !reloc_name_offs) {
      ny_native_set_err(err, err_len,
                        "i386 ELF object writer: relocation symbol allocation failed");
      goto done;
    }
  }
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

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(defs);
  free(def_name_offs);
  free(reloc_name_offs);
  free(reloc_symbols);
  free(relocs);
  ny_obj_free(&strtab);
  ny_obj_free(&file);
  ny_obj_free(&code);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "i386 ELF object writer failed");
  return ok;
}
