/*
 * x86-family object format writers for ELF, COFF, and Mach-O.
 */
#include "base/compat.h"
#include "base/debug.h"
#include "base/parallel.h"
#include "code/backend/lower/machine.h"
#include "code/backend/mach/object.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool ny_i386_obj_reserve_relocs(nyr_i386_obj_reloc_t **data, size_t *cap,
                                       size_t want, char *err, size_t err_len) {
  if (!data || !cap) {
    nyr_set_error(err, err_len,
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
    nyr_set_error(err, err_len,
                  "i386 ELF object writer: relocation buffer too large");
    return false;
  }
  nyr_i386_obj_reloc_t *grown = realloc(*data, next * sizeof(*grown));
  if (!grown) {
    nyr_set_error(err, err_len,
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
         !__atomic_compare_exchange_n(dst, &cur, value, false, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED)) {
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

void nyr_native_mach_encode_fallback_detail(char *out, size_t out_len) {
  if (!out || out_len == 0)
    return;
  snprintf(out, out_len, "%s",
           ny_native_first_mach_fallback_set ? ny_native_first_mach_fallback
                                             : "none");
}

void nyr_native_mach_encode_stats(unsigned long long *mach_ok,
                                  unsigned long long *nir_fallback) {
  if (mach_ok)
    *mach_ok = __atomic_load_n(&ny_native_stat_mach_ok, __ATOMIC_RELAXED);
  if (nir_fallback)
    *nir_fallback =
        __atomic_load_n(&ny_native_stat_nir_fallback, __ATOMIC_RELAXED);
}

void nyr_native_mach_div_record(bool magic) {
  unsigned long long *counter =
      magic ? &ny_native_stat_mach_div_magic : &ny_native_stat_mach_div_idiv;
  __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

void nyr_native_mach_div_stats(unsigned long long *magic,
                               unsigned long long *idiv) {
  if (magic)
    *magic = __atomic_load_n(&ny_native_stat_mach_div_magic, __ATOMIC_RELAXED);
  if (idiv)
    *idiv = __atomic_load_n(&ny_native_stat_mach_div_idiv, __ATOMIC_RELAXED);
}

void nyr_native_mach_regalloc_record(size_t segments, size_t colored,
                                     size_t spilled, size_t reloads,
                                     size_t peak_live);
__attribute__((used)) void
nyr_native_mach_regalloc_record(size_t segments, size_t colored, size_t spilled,
                                size_t reloads, size_t peak_live) {
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

void nyr_native_mach_regalloc_stats(unsigned long long *segments,
                                    unsigned long long *colored,
                                    unsigned long long *spilled,
                                    unsigned long long *reloads,
                                    unsigned long long *peak_live) {
  if (segments)
    *segments =
        __atomic_load_n(&ny_native_stat_regalloc_segments, __ATOMIC_RELAXED);
  if (colored)
    *colored =
        __atomic_load_n(&ny_native_stat_regalloc_colored, __ATOMIC_RELAXED);
  if (spilled)
    *spilled =
        __atomic_load_n(&ny_native_stat_regalloc_spilled, __ATOMIC_RELAXED);
  if (reloads)
    *reloads =
        __atomic_load_n(&ny_native_stat_regalloc_reloads, __ATOMIC_RELAXED);
  if (peak_live)
    *peak_live =
        __atomic_load_n(&ny_native_stat_regalloc_peak_live, __ATOMIC_RELAXED);
}

void nyr_native_mach_fpr_record(size_t segments, size_t colored, size_t spilled,
                                size_t reloads, size_t peak_live);
__attribute__((used)) void
nyr_native_mach_fpr_record(size_t segments, size_t colored, size_t spilled,
                           size_t reloads, size_t peak_live) {
  __atomic_fetch_add(&ny_native_stat_fpr_segments, (unsigned long long)segments,
                     __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_fpr_colored, (unsigned long long)colored,
                     __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_fpr_spilled, (unsigned long long)spilled,
                     __ATOMIC_RELAXED);
  __atomic_fetch_add(&ny_native_stat_fpr_reloads, (unsigned long long)reloads,
                     __ATOMIC_RELAXED);
  ny_native_atomic_max_ull(&ny_native_stat_fpr_peak_live,
                           (unsigned long long)peak_live);
}

void nyr_native_mach_fpr_stats(unsigned long long *segments,
                               unsigned long long *colored,
                               unsigned long long *spilled,
                               unsigned long long *reloads,
                               unsigned long long *peak_live) {
  if (segments)
    *segments = __atomic_load_n(&ny_native_stat_fpr_segments, __ATOMIC_RELAXED);
  if (colored)
    *colored = __atomic_load_n(&ny_native_stat_fpr_colored, __ATOMIC_RELAXED);
  if (spilled)
    *spilled = __atomic_load_n(&ny_native_stat_fpr_spilled, __ATOMIC_RELAXED);
  if (reloads)
    *reloads = __atomic_load_n(&ny_native_stat_fpr_reloads, __ATOMIC_RELAXED);
  if (peak_live)
    *peak_live =
        __atomic_load_n(&ny_native_stat_fpr_peak_live, __ATOMIC_RELAXED);
}

void nyr_native_mach_vector_record(size_t segments, size_t colored,
                                   size_t spilled, size_t reloads,
                                   size_t peak_live);
__attribute__((used)) void
nyr_native_mach_vector_record(size_t segments, size_t colored, size_t spilled,
                              size_t reloads, size_t peak_live) {
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

void nyr_native_mach_vector_stats(unsigned long long *segments,
                                  unsigned long long *colored,
                                  unsigned long long *spilled,
                                  unsigned long long *reloads,
                                  unsigned long long *peak_live) {
  if (segments)
    *segments =
        __atomic_load_n(&ny_native_stat_vector_segments, __ATOMIC_RELAXED);
  if (colored)
    *colored =
        __atomic_load_n(&ny_native_stat_vector_colored, __ATOMIC_RELAXED);
  if (spilled)
    *spilled =
        __atomic_load_n(&ny_native_stat_vector_spilled, __ATOMIC_RELAXED);
  if (reloads)
    *reloads =
        __atomic_load_n(&ny_native_stat_vector_reloads, __ATOMIC_RELAXED);
  if (peak_live)
    *peak_live =
        __atomic_load_n(&ny_native_stat_vector_peak_live, __ATOMIC_RELAXED);
}

static bool ny_elf64_write_sh(nyr_obj_buf_t *b, uint32_t name, uint32_t type,
                              uint64_t flags, uint64_t offset, uint64_t size,
                              uint32_t link, uint32_t info, uint64_t align,
                              uint64_t entsize) {
  return nyr_obj_u32(b, name) && nyr_obj_u32(b, type) &&
         nyr_obj_u64(b, flags) && nyr_obj_u64(b, 0) && nyr_obj_u64(b, offset) &&
         nyr_obj_u64(b, size) && nyr_obj_u32(b, link) && nyr_obj_u32(b, info) &&
         nyr_obj_u64(b, align) && nyr_obj_u64(b, entsize);
}

static bool ny_elf64_write_file(const char *path, const unsigned char *data,
                                size_t len, char *err, size_t err_len) {
  FILE *out = fopen(path, "wb");
  if (!out) {
    nyr_set_error(err, err_len, "x86-64 ELF object writer: cannot open %s: %s",
                  path, strerror(errno));
    return false;
  }
  bool ok = fwrite(data, 1, len, out) == len;
  if (fclose(out) != 0)
    ok = false;
  if (!ok)
    nyr_set_error(err, err_len, "x86-64 ELF object writer: failed writing %s",
                  path);
  return ok;
}

static bool ny_obj_sym_name8_or_str(nyr_obj_buf_t *b, const char *name,
                                    uint32_t str_off) {
  char fixed[8] = {0};
  size_t n = name ? strlen(name) : 0;
  if (n <= 8) {
    memcpy(fixed, name, n);
    return nyr_obj_emit(b, fixed, sizeof(fixed));
  }
  return nyr_obj_u32(b, 0) && nyr_obj_u32(b, str_off);
}

static bool ny_coff_write_sym(nyr_obj_buf_t *b, const char *name,
                              uint32_t long_name_off, uint32_t value,
                              int16_t section, uint16_t type,
                              unsigned storage_class) {
  return ny_obj_sym_name8_or_str(b, name, long_name_off) &&
         nyr_obj_u32(b, value) && nyr_obj_u16(b, (uint16_t)section) &&
         nyr_obj_u16(b, type) && nyr_obj_u8(b, storage_class) &&
         nyr_obj_u8(b, 0);
}

static bool ny_native_emit_coff_x64_object_code(
    const unsigned char *code, size_t code_len,
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count, const char *path,
    const char *symbol_name, char *err, size_t err_len) {
  if (!code || !path || !symbol_name || !symbol_name[0]) {
    nyr_set_error(err, err_len, "x86-64 COFF object writer: missing input");
    return false;
  }
  nyr_obj_buf_t file = {0};
  nyr_obj_buf_t strings = {0};
  bool ok = false;
  size_t reloc_capacity = reloc_count ? reloc_count : 1;
  char (*reloc_symbols)[256] = calloc(reloc_capacity, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t def_name_off = 0;
  uint32_t *reloc_name_offs = calloc(reloc_capacity, sizeof(*reloc_name_offs));
  if (!reloc_symbols || !reloc_name_offs) {
    nyr_set_error(err, err_len,
                  "x86-64 COFF relocation table allocation failed");
    goto done;
  }
  const size_t header_size = 20;
  const size_t section_count = 1;
  const size_t section_table_size = 40 * section_count;
  const size_t text_off = header_size + section_table_size;
  const size_t reloc_off = text_off + code_len;
  const size_t reloc_size = reloc_count * 10;
  const size_t symtab_off = reloc_off + reloc_size;

  if (!nyr_x64_obj_collect_reloc_symbols(relocs, reloc_count, reloc_symbols,
                                         &reloc_symbol_count, err, err_len))
    goto done;
  if (!nyr_obj_u32(&strings, 0))
    goto done;
  if (strlen(symbol_name) > 8) {
    def_name_off = (uint32_t)strings.len;
    if (!nyr_obj_emit(&strings, symbol_name, strlen(symbol_name) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (strlen(reloc_symbols[i]) > 8) {
      reloc_name_offs[i] = (uint32_t)strings.len;
      if (!nyr_obj_emit(&strings, reloc_symbols[i],
                        strlen(reloc_symbols[i]) + 1))
        goto done;
    }
  }
  nyr_obj_patch_u32(&strings, 0, (uint32_t)strings.len);

  uint32_t nsyms = (uint32_t)(2 + reloc_symbol_count);
  if (!nyr_obj_u16(&file, 0x8664) || /* IMAGE_FILE_MACHINE_AMD64 */
      !nyr_obj_u16(&file, (uint16_t)section_count) || !nyr_obj_u32(&file, 0) ||
      !nyr_obj_u32(&file, (uint32_t)symtab_off) || !nyr_obj_u32(&file, nsyms) ||
      !nyr_obj_u16(&file, 0) || !nyr_obj_u16(&file, 0))
    goto done;

  char sec_name[8] = {0};
  memcpy(sec_name, ".text", 5);
  if (!nyr_obj_emit(&file, sec_name, sizeof(sec_name)) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0) ||
      !nyr_obj_u32(&file, (uint32_t)code_len) ||
      !nyr_obj_u32(&file, (uint32_t)text_off) ||
      !nyr_obj_u32(&file, (uint32_t)(reloc_count ? reloc_off : 0)) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u16(&file, (uint16_t)reloc_count) ||
      !nyr_obj_u16(&file, 0) ||
      !nyr_obj_u32(&file, 0x60500020u)) /* code | execute | read | align16 */
    goto done;

  if (!nyr_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int sym_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                         relocs[i].symbol);
    if (sym_i < 0)
      goto done;
    if (!nyr_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !nyr_obj_u32(&file, (uint32_t)(2 + sym_i)) ||
        !nyr_obj_u16(&file, 0x0004)) /* IMAGE_REL_AMD64_REL32 */
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
  if (!nyr_obj_emit(&file, strings.data, strings.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(reloc_symbols);
  free(reloc_name_offs);
  nyr_obj_free(&strings);
  nyr_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "x86-64 COFF object writer failed");
  return ok;
}

static bool ny_macho_write_padded_name(nyr_obj_buf_t *b, const char *name) {
  char out[16] = {0};
  if (name)
    snprintf(out, sizeof(out), "%s", name);
  return nyr_obj_emit(b, out, sizeof(out));
}

static bool ny_native_emit_macho_x64_object_code(
    const unsigned char *code, size_t code_len,
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count, const char *path,
    const char *symbol_name, char *err, size_t err_len) {
  if (!code || !path || !symbol_name || !symbol_name[0]) {
    nyr_set_error(err, err_len, "x86-64 Mach-O object writer: missing input");
    return false;
  }
  nyr_obj_buf_t file = {0};
  nyr_obj_buf_t strtab = {0};
  bool ok = false;
  /*
   * A function's unique external symbols cannot outnumber its relocations.
   * Keep this proportional to the actual object rather than the global cap.
   */
  size_t reloc_capacity = reloc_count ? reloc_count : 1;
  char (*reloc_symbols)[256] = calloc(reloc_capacity, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t *reloc_name_offs = calloc(reloc_capacity, sizeof(*reloc_name_offs));
  if (!reloc_symbols || !reloc_name_offs) {
    nyr_set_error(err, err_len,
                  "x86-64 Mach-O relocation table allocation failed");
    goto done;
  }
  char def_name[256];
  snprintf(def_name, sizeof(def_name), "%s%s", symbol_name[0] == '_' ? "" : "_",
           symbol_name);

  if (!nyr_x64_obj_collect_reloc_symbols(relocs, reloc_count, reloc_symbols,
                                         &reloc_symbol_count, err, err_len))
    goto done;
  if (!nyr_obj_u8(&strtab, 0))
    goto done;
  uint32_t def_name_off = (uint32_t)strtab.len;
  if (!nyr_obj_emit(&strtab, def_name, strlen(def_name) + 1))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
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

  if (!nyr_obj_u32(&file, 0xfeedfacf) || !nyr_obj_u32(&file, 0x01000007) ||
      !nyr_obj_u32(&file, 3) || !nyr_obj_u32(&file, 1) ||
      !nyr_obj_u32(&file, 2) || !nyr_obj_u32(&file, sizeofcmds) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0))
    goto done;

  if (!nyr_obj_u32(&file, 0x19) || !nyr_obj_u32(&file, seg_cmdsize) ||
      !ny_macho_write_padded_name(&file, "") || !nyr_obj_u64(&file, 0) ||
      !nyr_obj_u64(&file, code_len) || !nyr_obj_u64(&file, text_off) ||
      !nyr_obj_u64(&file, code_len) || !nyr_obj_u32(&file, 7) ||
      !nyr_obj_u32(&file, 5) || !nyr_obj_u32(&file, 1) ||
      !nyr_obj_u32(&file, 0))
    goto done;
  if (!ny_macho_write_padded_name(&file, "__text") ||
      !ny_macho_write_padded_name(&file, "__TEXT") || !nyr_obj_u64(&file, 0) ||
      !nyr_obj_u64(&file, code_len) || !nyr_obj_u32(&file, text_off) ||
      !nyr_obj_u32(&file, 4) ||
      !nyr_obj_u32(&file, reloc_count ? reloc_off : 0) ||
      !nyr_obj_u32(&file, (uint32_t)reloc_count) ||
      !nyr_obj_u32(&file, 0x80000400u) || !nyr_obj_u32(&file, 0) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0))
    goto done;

  if (!nyr_obj_u32(&file, 0x2) || !nyr_obj_u32(&file, sym_cmdsize) ||
      !nyr_obj_u32(&file, symoff) || !nyr_obj_u32(&file, nsyms) ||
      !nyr_obj_u32(&file, stroff) || !nyr_obj_u32(&file, strsize))
    goto done;
  if (!nyr_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int sym_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                         relocs[i].symbol);
    if (sym_i < 0)
      goto done;
    uint32_t word = (uint32_t)(1 + sym_i) | (1u << 24) | (2u << 25) |
                    (1u << 27) | (2u << 28); /* pcrel long extern branch */
    if (!nyr_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !nyr_obj_u32(&file, word))
      goto done;
  }
  if (!nyr_obj_u32(&file, def_name_off) || !nyr_obj_u8(&file, 0x0f) ||
      !nyr_obj_u8(&file, 1) || !nyr_obj_u16(&file, 0) || !nyr_obj_u64(&file, 0))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!nyr_obj_u32(&file, reloc_name_offs[i]) || !nyr_obj_u8(&file, 0x01) ||
        !nyr_obj_u8(&file, 0) || !nyr_obj_u16(&file, 0) ||
        !nyr_obj_u64(&file, 0))
      goto done;
  }
  if (!nyr_obj_emit(&file, strtab.data, strtab.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(reloc_symbols);
  free(reloc_name_offs);
  nyr_obj_free(&strtab);
  nyr_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "x86-64 Mach-O object writer failed");
  return ok;
}

static bool ny_obj_uleb128(nyr_obj_buf_t *b, uint64_t value) {
  do {
    unsigned char byte = (unsigned char)(value & 0x7f);
    value >>= 7;
    if (value)
      byte |= 0x80;
    if (!nyr_obj_u8(b, byte))
      return false;
  } while (value);
  return true;
}

static bool ny_obj_sleb128(nyr_obj_buf_t *b, int64_t value) {
  bool more = true;
  while (more) {
    unsigned char byte = (unsigned char)(value & 0x7f);
    value >>= 7;
    if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40)))
      more = false;
    else
      byte |= 0x80;
    if (!nyr_obj_u8(b, byte))
      return false;
  }
  return true;
}

static int nyr_native_debug_line_cmp(const void *lhs, const void *rhs) {
  const nyr_native_debug_line_t *a = lhs;
  const nyr_native_debug_line_t *b = rhs;
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
bool nyr_native_build_debug_line(const nyr_native_debug_line_t *lines,
                                 size_t line_count, size_t text_len,
                                 nyr_obj_buf_t *out, size_t *base_reloc_offset,
                                 size_t *base_def_index, char *err,
                                 size_t err_len) {
  if (!out || !base_reloc_offset || !base_def_index || (!lines && line_count))
    return false;
  out->len = 0;
  if (!line_count)
    return true;
  nyr_native_debug_line_t *ordered = calloc(line_count, sizeof(*ordered));
  const char **files = calloc(line_count, sizeof(*files));
  unsigned *file_ids = calloc(line_count, sizeof(*file_ids));
  if (!ordered || !files || !file_ids) {
    free(ordered);
    free(files);
    free(file_ids);
    nyr_set_error(err, err_len, "DWARF line table allocation failed");
    return false;
  }
  memcpy(ordered, lines, line_count * sizeof(*ordered));
  qsort(ordered, line_count, sizeof(*ordered), nyr_native_debug_line_cmp);
  size_t file_count = 0;
  for (size_t i = 0; i < line_count; ++i) {
    const char *file =
        ordered[i].file && ordered[i].file[0] ? ordered[i].file : "<source>";
    size_t file_id = 0;
    for (; file_id < file_count; ++file_id)
      if (strcmp(files[file_id], file) == 0)
        break;
    if (file_id == file_count)
      files[file_count++] = file;
    file_ids[i] = (unsigned)file_id + 1;
  }

  if (!nyr_obj_u32(out, 0))
    goto oom;
  if (!nyr_obj_u16(out, 4))
    goto oom;
  size_t header_len_off = out->len;
  if (!nyr_obj_u32(out, 0) ||
      !nyr_obj_u8(out, 1) || /* minimum instruction length */
      !nyr_obj_u8(out, 1) || /* maximum operations per instruction */
      !nyr_obj_u8(out, 1) || /* default_is_stmt */
      !nyr_obj_u8(out, (unsigned char)-5) || !nyr_obj_u8(out, 14) ||
      !nyr_obj_u8(out, 13))
    goto oom;
  static const unsigned char standard_lengths[12] = {0, 1, 1, 1, 1, 0,
                                                     0, 0, 0, 0, 0, 1};
  if (!nyr_obj_emit(out, standard_lengths, sizeof(standard_lengths)) ||
      !nyr_obj_u8(out, 0)) /* include_directories terminator */
    goto oom;
  for (size_t i = 0; i < file_count; ++i) {
    if (!nyr_obj_emit(out, files[i], strlen(files[i]) + 1) ||
        !ny_obj_uleb128(out, 0) || !ny_obj_uleb128(out, 0) ||
        !ny_obj_uleb128(out, 0))
      goto oom;
  }
  if (!nyr_obj_u8(out, 0)) /* file_names terminator */
    goto oom;
  size_t program_off = out->len;
  if (!nyr_obj_u8(out, 0) || !nyr_obj_u8(out, 9) || !nyr_obj_u8(out, 2))
    goto oom;
  *base_reloc_offset = out->len;
  if (!nyr_obj_u64(out, 0))
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
    if (!nyr_obj_u8(out, 2) || !ny_obj_uleb128(out, delta))
      goto oom;
    if (file_ids[i] != previous_file) {
      if (!nyr_obj_u8(out, 4) || !ny_obj_uleb128(out, file_ids[i]))
        goto oom;
      previous_file = file_ids[i];
    }
    unsigned column = ordered[i].column ? ordered[i].column : 1;
    if (column != previous_column &&
        (!nyr_obj_u8(out, 5) || !ny_obj_uleb128(out, column)))
      goto oom;
    previous_column = column;
    int64_t line = ordered[i].line ? (int64_t)ordered[i].line : 1;
    if (!nyr_obj_u8(out, 3) || !ny_obj_sleb128(out, line - previous_line) ||
        !nyr_obj_u8(out, 1))
      goto oom;
    previous_line = line;
    previous_offset = ordered[i].offset;
  }
  if (text_len >= previous_offset &&
      (!nyr_obj_u8(out, 2) || !ny_obj_uleb128(out, text_len - previous_offset)))
    goto oom;
  /*
   * DW_LNE_end_sequence
   */
  if (!nyr_obj_u8(out, 0) || !nyr_obj_u8(out, 1) || !nyr_obj_u8(out, 1))
    goto oom;
  nyr_obj_patch_u32(out, header_len_off,
                    (uint32_t)(program_off - (header_len_off + 4)));
  nyr_obj_patch_u32(out, 0, (uint32_t)(out->len - 4));
  free(ordered);
  free(files);
  free(file_ids);
  return true;

oom:
  free(ordered);
  free(files);
  free(file_ids);
  nyr_set_error(err, err_len, "DWARF line table emission failed");
  return false;
}

/*
 * Build a minimal DWARF v4 compile unit for the emitted line table.  The
 * low_pc relocation and high_pc extent make source lines discoverable by
 * debuggers; function and local-variable DIEs require richer compiler facts.
 */
static bool nyr_native_build_debug_info(
    const nyr_native_debug_line_t *lines, size_t line_count, size_t text_len,
    nyr_obj_buf_t *info, nyr_obj_buf_t *abbrev, size_t *low_pc_reloc_offset,
    size_t *base_def_index, char *err, size_t err_len) {
  if (!lines || !line_count || !info || !abbrev || !low_pc_reloc_offset ||
      !base_def_index)
    return false;
  info->len = 0;
  abbrev->len = 0;
  size_t first = 0;
  for (size_t i = 1; i < line_count; ++i)
    if (lines[i].offset < lines[first].offset)
      first = i;
  if (lines[first].offset > text_len) {
    nyr_set_error(err, err_len, "DWARF compile unit has invalid code range");
    return false;
  }
  const char *name = lines[first].file && lines[first].file[0]
                         ? lines[first].file
                         : "<source>";

  /*
   * One compile-unit abbreviation: name, stmt_list, low_pc, high_pc, language.
   */
  if (!ny_obj_uleb128(abbrev, 1) || !ny_obj_uleb128(abbrev, 0x11) ||
      !nyr_obj_u8(abbrev, 0) || !ny_obj_uleb128(abbrev, 0x03) ||
      !ny_obj_uleb128(abbrev, 0x08) || !ny_obj_uleb128(abbrev, 0x10) ||
      !ny_obj_uleb128(abbrev, 0x17) || !ny_obj_uleb128(abbrev, 0x11) ||
      !ny_obj_uleb128(abbrev, 0x01) || !ny_obj_uleb128(abbrev, 0x12) ||
      !ny_obj_uleb128(abbrev, 0x07) || !ny_obj_uleb128(abbrev, 0x13) ||
      !ny_obj_uleb128(abbrev, 0x05) || !ny_obj_uleb128(abbrev, 0) ||
      !ny_obj_uleb128(abbrev, 0) || !ny_obj_uleb128(abbrev, 0))
    goto oom;

  if (!nyr_obj_u32(info, 0) || !nyr_obj_u16(info, 4) || !nyr_obj_u32(info, 0) ||
      !nyr_obj_u8(info, 8) || !ny_obj_uleb128(info, 1) ||
      !nyr_obj_emit(info, name, strlen(name) + 1) || !nyr_obj_u32(info, 0))
    goto oom;
  *low_pc_reloc_offset = info->len;
  if (!nyr_obj_u64(info, 0) ||
      !nyr_obj_u64(info, (uint64_t)(text_len - lines[first].offset)) ||
      !nyr_obj_u16(info, 0x0002))
    goto oom;
  *base_def_index = lines[first].def_index;
  nyr_obj_patch_u32(info, 0, (uint32_t)(info->len - 4));
  return true;

oom:
  info->len = 0;
  abbrev->len = 0;
  nyr_set_error(err, err_len, "DWARF compile unit emission failed");
  return false;
}

/*
 * Emit a compact CodeView C13 line subsection for COFF consumers.
 */
static bool nyr_native_build_codeview_lines(
    const nyr_native_debug_line_t *lines, size_t line_count, size_t code_len,
    const char *path, nyr_obj_buf_t *out, char *err, size_t err_len) {
  if (!out || (!lines && line_count))
    return false;
  out->len = 0;
  if (!line_count)
    return true;
  if (!nyr_obj_u32(out, 4))
    goto fail;
  size_t name_len = strlen(path && path[0] ? path : "nytrix");
  size_t obj_payload = 4 + name_len + 1;
  if (obj_payload > UINT16_MAX - 2 ||
      !nyr_obj_u16(out, (uint16_t)(obj_payload + 2)) ||
      !nyr_obj_u16(out, 0x1101) || !nyr_obj_u32(out, 0) ||
      !nyr_obj_emit(out, path && path[0] ? path : "nytrix", name_len + 1))
    goto fail;
  while (out->len & 3u)
    if (!nyr_obj_u8(out, 0))
      goto fail;
  size_t payload = 12 + 12 + line_count * 8;
  if (payload > UINT16_MAX - 2 || !nyr_obj_u16(out, (uint16_t)(payload + 2)) ||
      !nyr_obj_u16(out, 0x00f2) || !nyr_obj_u32(out, 0) ||
      !nyr_obj_u16(out, 1) || !nyr_obj_u16(out, 0) ||
      !nyr_obj_u32(out, (uint32_t)code_len) || !nyr_obj_u32(out, 0) ||
      !nyr_obj_u32(out, (uint32_t)line_count) ||
      !nyr_obj_u32(out, (uint32_t)(line_count * 8)))
    goto fail;
  for (size_t i = 0; i < line_count; ++i) {
    uint32_t line = lines[i].line ? lines[i].line : 1;
    uint32_t column = lines[i].column ? lines[i].column : 1;
    uint32_t flags = (line & 0x00ffffffu) | ((column & 0x7fu) << 24);
    if (!nyr_obj_u32(out, (uint32_t)lines[i].offset) ||
        !nyr_obj_u32(out, flags))
      goto fail;
  }
  while (out->len & 3u)
    if (!nyr_obj_u8(out, 0))
      goto fail;
  return true;
fail:
  out->len = 0;
  nyr_set_error(err, err_len, "CodeView line table emission failed");
  return false;
}

/*
 * Named data-section cap for one C object bundle.  Falls well below the
 * symbol-table limit, so a kernel-style TU with hundreds of named sections
 * fails closed with an explicit message instead of a truncated object.
 */
enum { NY_C_DATA_MAX_NAMED_SECTIONS = 64 };

/*
 * Write one .rela entry for a text (PC32/PLT32) relocation.
 */
static bool ny_elf64_obj_rela_text_entry(nyr_obj_buf_t *file,
                                         const nyr_x64_obj_reloc_t *reloc,
                                         const nyr_x64_obj_symbol_def_t *defs,
                                         size_t def_count,
                                         char (*reloc_symbols)[256],
                                         size_t reloc_symbol_count, char *err,
                                         size_t err_len) {
  int def_i = nyr_x64_obj_def_index(defs, def_count, reloc->symbol);
  uint32_t sym_index = 0;
  if (def_i >= 0)
    sym_index = (uint32_t)(1 + def_i);
  else {
    int ext_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                         reloc->symbol);
    if (ext_i < 0) {
      nyr_set_error(err, err_len,
                    "x86-64 ELF object writer: missing external relocation "
                    "symbol %s",
                    reloc->symbol);
      return false;
    }
    sym_index = (uint32_t)(1 + def_count + (size_t)ext_i);
  }
  uint64_t rtype = reloc->type == NY_RELOC_PC32         ? 2u
                   : reloc->type == NY_RELOC_GOTPCREL   ? 9u
                   : reloc->type == NY_RELOC_X86_64_32S ? 11u
                                                        : 4u;
  uint64_t info = ((uint64_t)sym_index << 32) | rtype;
  return nyr_obj_u64(file, reloc->disp_off) && nyr_obj_u64(file, info) &&
         nyr_obj_u64(file,
                     reloc->type == NY_RELOC_X86_64_32S ? 0 : (uint64_t)-4LL);
}

/*
 * Write one supported x86-64 data relocation relative to its target section.
 */
static bool ny_elf64_obj_rela_data_entry(
    nyr_obj_buf_t *file, const nyr_x64_obj_reloc_t *reloc,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count,
    char (*reloc_symbols)[256], size_t reloc_symbol_count, size_t section_start,
    char *err, size_t err_len) {
  (void)err;
  (void)err_len;
  int def_i = nyr_x64_obj_def_index(defs, def_count, reloc->symbol);
  uint32_t sym_index = 0;
  if (def_i >= 0)
    sym_index = (uint32_t)(1 + def_i);
  else {
    int ext_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                         reloc->symbol);
    if (ext_i < 0) {
      nyr_set_error(err, err_len,
                    "x86-64 ELF object writer: missing external data "
                    "relocation symbol %s",
                    reloc->symbol);
      return false;
    }
    sym_index = (uint32_t)(1 + def_count + (size_t)ext_i);
  }
  uint64_t rtype = reloc->type == NY_RELOC_PC32         ? 2u
                   : reloc->type == NY_RELOC_X86_64_32S ? 11u
                                                        : 1u;
  uint64_t info = ((uint64_t)sym_index << 32) | rtype;
  return nyr_obj_u64(file, reloc->disp_off - section_start) &&
         nyr_obj_u64(file, info) && nyr_obj_u64(file, (uint64_t)reloc->addend);
}

static bool ny_c_bss_section(const char *name) {
  return !strcmp(name, ".bss") || !strncmp(name, ".bss.", 5);
}

static bool ny_native_emit_elf64_x64_object_bundle_code(
    const unsigned char *code, size_t code_len,
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    const nyr_native_debug_line_t *debug_lines, size_t debug_line_count,
    char *err, size_t err_len) {
  if (!path || (code_len && !code) || (def_count && !defs)) {
    nyr_set_error(err, err_len, "x86-64 ELF object writer: missing input");
    return false;
  }
  nyr_obj_buf_t file = {0};
  nyr_obj_buf_t strtab = {0};
  bool ok = false;
  const char *stage = "input setup";
  bool profile = getenv("NYTRIX_AOT_OBJECT_PROFILE") != NULL;
  clock_t stage_started = profile ? clock() : 0;
#define NY_X64_ELF_PROFILE_STAGE(next_stage)                                   \
  do {                                                                         \
    if (profile) {                                                             \
      clock_t stage_ended = clock();                                           \
      fprintf(stderr,                                                          \
              "[aot-object] phase=elf64-writer stage=%s elapsed=%.6fs "        \
              "defs=%zu relocs=%zu\n",                                         \
              stage, (double)(stage_ended - stage_started) / CLOCKS_PER_SEC,   \
              def_count, reloc_count);                                         \
    }                                                                          \
    stage = (next_stage);                                                      \
    if (profile)                                                               \
      stage_started = clock();                                                 \
  } while (0)
  char (*reloc_symbols)[256] =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t *def_name_offs =
      calloc(def_count ? def_count : 1, sizeof(*def_name_offs));
  uint32_t *reloc_name_offs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_name_offs));
  if (!reloc_symbols || !def_name_offs || !reloc_name_offs) {
    /*
     * `done` releases all three; freeing here double-freed on failure.
     */
    nyr_set_error(err, err_len, "x86-64 ELF object writer: allocation failed");
    goto done;
  }
  if (!nyr_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;
  NY_X64_ELF_PROFILE_STAGE("string table construction");

  const char shstr[] =
      "\0.text\0.rela.text\0.data\0.rela.data\0.debug_line\0.rela.debug_line"
      "\0.debug_info\0.rela.debug_info\0.debug_abbrev\0.symtab\0.strtab\0"
      ".shstrtab\0";
  const uint32_t sh_text = ny_elf_shstr_offset(shstr, sizeof(shstr), ".text");
  const uint32_t sh_rela_text =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".rela.text");
  const uint32_t sh_data = ny_elf_shstr_offset(shstr, sizeof(shstr), ".data");
  const uint32_t sh_rela_data =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".rela.data");
  const uint32_t sh_debug_line =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".debug_line");
  const uint32_t sh_rela_debug_line =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".rela.debug_line");
  const uint32_t sh_debug_info =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".debug_info");
  const uint32_t sh_rela_debug_info =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".rela.debug_info");
  const uint32_t sh_debug_abbrev =
      ny_elf_shstr_offset(shstr, sizeof(shstr), ".debug_abbrev");
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
  struct {
    const char *name;
    size_t start;
    size_t end;
    size_t data_off;
    size_t rela_off;
    size_t rela_size;
    uint32_t name_off;
    uint32_t rela_name_off;
    uint32_t data_idx;
    uint32_t rela_idx;
    bool has_rela;
  } named_code[NY_C_DATA_MAX_NAMED_SECTIONS];
  memset(named_code, 0, sizeof(named_code));
  size_t named_code_count = 0;
  for (size_t i = 0; i < def_count; ++i) {
    if (defs[i].is_data || !defs[i].section)
      continue;
    size_t at;
    for (at = 0; at < named_code_count; ++at)
      if (strcmp(named_code[at].name, defs[i].section) == 0)
        break;
    if (at == named_code_count) {
      if (named_code_count >= NY_C_DATA_MAX_NAMED_SECTIONS) {
        nyr_set_error(err, err_len, "too many named function sections");
        goto done;
      }
      named_code[named_code_count].name = defs[i].section;
      named_code[named_code_count].start = defs[i].off;
      named_code[named_code_count].end = defs[i].off + defs[i].size;
      ++named_code_count;
      continue;
    }
    if (defs[i].off < named_code[at].start)
      named_code[at].start = defs[i].off;
    if (defs[i].off + defs[i].size > named_code[at].end)
      named_code[at].end = defs[i].off + defs[i].size;
  }
  size_t text_len = have_data ? data_start : code_len;
  if (named_code_count)
    text_len = named_code[0].start;
  if (!named_code_count) {
    for (size_t i = 0; i < def_count; ++i) {
      if (defs[i].is_data)
        continue;
      if (defs[i].off > text_len || defs[i].size > text_len - defs[i].off) {
        nyr_set_error(err, err_len,
                      "x86-64 ELF object writer: function %s exceeds .text",
                      defs[i].name);
        goto done;
      }
    }
  }
  bool have_rela_data = false;
  for (size_t i = 0; i < reloc_count; ++i)
    have_rela_data |= relocs[i].is_data;
  /*
   * The bundle groups data by requested section name: pooled symbols first
   * (the default `.data` region), then each named section back-to-back.
   * Slice one contiguous file range per group here.  Group ranges are
   * disjoint, so the default region ends at the first named group.
   */
  struct {
    const char *name;
    uint64_t flags;
    size_t alignment;
    size_t entsize;
    size_t start;
    size_t end;
    size_t data_off;
    size_t rela_off;
    size_t rela_size;
    uint32_t name_off;
    uint32_t rela_name_off;
    uint32_t data_idx;
    uint32_t rela_idx;
    bool has_rela;
  } named[NY_C_DATA_MAX_NAMED_SECTIONS];
  memset(named, 0, sizeof(named));
  size_t named_count = 0;
  for (size_t i = 0; i < def_count; ++i) {
    if (!defs[i].is_data || !defs[i].section)
      continue;
    size_t at;
    for (at = 0; at < named_count; ++at) {
      if (strcmp(named[at].name, defs[i].section) == 0)
        break;
    }
    if (at == named_count) {
      if (named_count >= NY_C_DATA_MAX_NAMED_SECTIONS) {
        nyr_set_error(err, err_len, "too many named data sections");
        goto done;
      }
      named[named_count].name = defs[i].section;
      named[named_count].flags =
          defs[i].section_flags ? defs[i].section_flags : 3;
      named[named_count].alignment = defs[i].alignment ? defs[i].alignment : 1;
      named[named_count].entsize = defs[i].section_entsize;
      named[named_count].start = defs[i].off;
      named[named_count].end = defs[i].off + defs[i].size;
      ++named_count;
      continue;
    }
    uint64_t flags = defs[i].section_flags ? defs[i].section_flags : 3;
    if (named[at].flags != flags) {
      nyr_set_error(err, err_len, "ELF data section %s has conflicting flags",
                    named[at].name);
      goto done;
    }
    if (named[at].entsize != defs[i].section_entsize) {
      nyr_set_error(err, err_len,
                    "ELF data section %s has conflicting entry sizes",
                    named[at].name);
      goto done;
    }
    if (defs[i].alignment > named[at].alignment)
      named[at].alignment = defs[i].alignment;
    if (defs[i].off < named[at].start)
      named[at].start = defs[i].off;
    if (defs[i].off + defs[i].size > named[at].end)
      named[at].end = defs[i].off + defs[i].size;
  }
  for (size_t c = 0; c < named_code_count; ++c) {
    for (size_t n = 0; n < named_count; ++n) {
      if (strcmp(named_code[c].name, named[n].name) == 0) {
        nyr_set_error(err, err_len,
                      "ELF section %s is used for both code and data",
                      named_code[c].name);
        goto done;
      }
    }
  }
  size_t data_len = (named_count ? named[0].start : data_end) - data_start;
  nyr_obj_buf_t debug_line = {0};
  nyr_obj_buf_t debug_info = {0};
  nyr_obj_buf_t debug_abbrev = {0};
  size_t debug_base_reloc = 0;
  size_t debug_base_def = 0;
  size_t debug_info_reloc = 0;
  size_t debug_info_base_def = 0;
  bool have_debug = debug_line_count > 0;
  NY_X64_ELF_PROFILE_STAGE("debug line construction");
  if (have_debug && !nyr_native_build_debug_line(
                        debug_lines, debug_line_count, text_len, &debug_line,
                        &debug_base_reloc, &debug_base_def, err, err_len))
    goto done;
  if (have_debug && debug_base_def >= def_count)
    goto done;
  NY_X64_ELF_PROFILE_STAGE("debug compile-unit construction");
  if (have_debug &&
      !nyr_native_build_debug_info(
          debug_lines, debug_line_count, text_len, &debug_info, &debug_abbrev,
          &debug_info_reloc, &debug_info_base_def, err, err_len))
    goto done;
  if (have_debug && debug_info_base_def >= def_count)
    goto done;
  /*
   * Dense section indices: null/.text/.rela.text are always 0..2, followed
   * by named code sections, optional default data, named data sections,
   * optional debug sections, and the symbol tables.
   */
  bool has_default_data_rela = false;
  for (size_t i = 0; i < reloc_count && !has_default_data_rela; ++i) {
    has_default_data_rela = relocs[i].is_data &&
                            relocs[i].disp_off >= data_start &&
                            relocs[i].disp_off < data_start + data_len;
  }
  for (size_t n = 0; n < named_count; ++n) {
    named[n].has_rela = false;
    for (size_t i = 0; i < reloc_count && !named[n].has_rela; ++i)
      named[n].has_rela = relocs[i].is_data &&
                          relocs[i].disp_off >= named[n].start &&
                          relocs[i].disp_off < named[n].end;
  }
  for (size_t n = 0; n < named_code_count; ++n) {
    named_code[n].has_rela = false;
    for (size_t i = 0; i < reloc_count && !named_code[n].has_rela; ++i)
      named_code[n].has_rela = !relocs[i].is_data &&
                               relocs[i].disp_off >= named_code[n].start &&
                               relocs[i].disp_off < named_code[n].end;
  }
  uint32_t next_idx = 3;
  for (size_t n = 0; n < named_code_count; ++n) {
    named_code[n].data_idx = next_idx++;
    if (named_code[n].has_rela)
      named_code[n].rela_idx = next_idx++;
  }
  uint32_t sec_data = 0;
  if (have_data)
    sec_data = next_idx++;
  uint32_t sec_rela_data = 0;
  if (has_default_data_rela)
    sec_rela_data = next_idx++;
  for (size_t n = 0; n < named_count; ++n) {
    named[n].data_idx = next_idx++;
    if (named[n].has_rela)
      named[n].rela_idx = next_idx++;
  }
  uint32_t sec_debug = have_debug ? next_idx++ : 0u;
  if (have_debug)
    next_idx++; /* .rela.debug_line occupies sec_debug + 1 */
  uint32_t sec_debug_info = have_debug ? next_idx++ : 0u;
  if (have_debug)
    next_idx++; /* .rela.debug_info occupies sec_debug_info + 1 */
  if (have_debug)
    next_idx++; /* .debug_abbrev */
  uint32_t symtab_idx = next_idx++;
  uint32_t strtab_idx = next_idx++;
  uint32_t shstrtab_idx = next_idx++;
  NY_X64_ELF_PROFILE_STAGE("string table construction");
  if (!nyr_obj_u8(&strtab, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    def_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, defs[i].name, strlen(defs[i].name) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
      goto done;
  }

  NY_X64_ELF_PROFILE_STAGE("object section data construction");
  if (!nyr_obj_zero(&file, 64) || !nyr_obj_pad_to(&file, 16))
    goto done;
  size_t text_off = file.len;
  if (!nyr_obj_emit(&file, code, text_len) || !nyr_obj_pad_to(&file, 8))
    goto done;
  for (size_t n = 0; n < named_code_count; ++n) {
    if (!nyr_obj_pad_to(&file, 16))
      goto done;
    named_code[n].data_off = file.len;
    if (!nyr_obj_emit(&file, code + named_code[n].start,
                      named_code[n].end - named_code[n].start) ||
        !nyr_obj_pad_to(&file, 8))
      goto done;
  }
  size_t data_off = file.len;
  if (have_data) {
    if (!nyr_obj_emit(&file, code + data_start, data_len) ||
        !nyr_obj_pad_to(&file, 8))
      goto done;
  }
  for (size_t n = 0; n < named_count; ++n) {
    if (!nyr_obj_pad_to(&file, 8))
      goto done;
    named[n].data_off = file.len;
    if (ny_c_bss_section(named[n].name)) {
      for (size_t b = named[n].start; b < named[n].end; ++b)
        if (code[b]) {
          nyr_set_error(err, err_len, "nonzero initializer in BSS section %s",
                        named[n].name);
          goto done;
        }
      for (size_t r = 0; r < reloc_count; ++r)
        if (relocs[r].disp_off >= named[n].start &&
            relocs[r].disp_off < named[n].end) {
          nyr_set_error(err, err_len, "relocation in BSS section %s",
                        named[n].name);
          goto done;
        }
      continue;
    }
    if (!nyr_obj_emit(&file, code + named[n].start,
                      named[n].end - named[n].start) ||
        !nyr_obj_pad_to(&file, 8))
      goto done;
  }
  size_t debug_off = file.len;
  if (have_debug && (!nyr_obj_emit(&file, debug_line.data, debug_line.len) ||
                     !nyr_obj_pad_to(&file, 8)))
    goto done;
  size_t debug_info_off = file.len;
  if (have_debug && (!nyr_obj_emit(&file, debug_info.data, debug_info.len) ||
                     !nyr_obj_pad_to(&file, 8)))
    goto done;
  size_t debug_abbrev_off = file.len;
  if (have_debug &&
      (!nyr_obj_emit(&file, debug_abbrev.data, debug_abbrev.len) ||
       !nyr_obj_pad_to(&file, 8)))
    goto done;
  NY_X64_ELF_PROFILE_STAGE("text relocation construction");
  size_t rela_off = file.len;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (relocs[i].is_data || relocs[i].disp_off >= text_len)
      continue;
    if (!ny_elf64_obj_rela_text_entry(&file, &relocs[i], defs, def_count,
                                      reloc_symbols, reloc_symbol_count, err,
                                      err_len))
      goto done;
  }
  size_t rela_size = file.len - rela_off;
  for (size_t n = 0; n < named_code_count; ++n) {
    if (!named_code[n].has_rela)
      continue;
    named_code[n].rela_off = file.len;
    for (size_t i = 0; i < reloc_count; ++i) {
      if (relocs[i].is_data || relocs[i].disp_off < named_code[n].start ||
          relocs[i].disp_off >= named_code[n].end)
        continue;
      nyr_x64_obj_reloc_t local = relocs[i];
      local.disp_off -= named_code[n].start;
      if (!ny_elf64_obj_rela_text_entry(&file, &local, defs, def_count,
                                        reloc_symbols, reloc_symbol_count, err,
                                        err_len))
        goto done;
    }
    named_code[n].rela_size = file.len - named_code[n].rela_off;
  }
  NY_X64_ELF_PROFILE_STAGE("data relocation construction");
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].is_data)
      continue;
    bool covered = relocs[i].disp_off >= data_start &&
                   relocs[i].disp_off - data_start < data_len;
    for (size_t n = 0; n < named_count && !covered; ++n)
      covered = relocs[i].disp_off >= named[n].start &&
                relocs[i].disp_off < named[n].end;
    if (!covered) {
      nyr_set_error(err, err_len,
                    "x86-64 ELF object writer: data relocation %zu offset "
                    "%zu is outside every data section",
                    i, relocs[i].disp_off);
      goto done;
    }
  }
  size_t rela_data_off = file.len;
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!relocs[i].is_data)
      continue;
    if (relocs[i].disp_off < data_start ||
        relocs[i].disp_off - data_start >= data_len)
      continue; /* named data groups are written below */
    if (!ny_elf64_obj_rela_data_entry(&file, &relocs[i], defs, def_count,
                                      reloc_symbols, reloc_symbol_count,
                                      data_start, err, err_len))
      goto done;
  }
  size_t rela_data_size = file.len - rela_data_off;
  NY_X64_ELF_PROFILE_STAGE("named data relocation construction");
  for (size_t n = 0; n < named_count; ++n) {
    if (!named[n].has_rela)
      continue;
    named[n].rela_off = file.len;
    for (size_t i = 0; i < reloc_count; ++i) {
      if (!relocs[i].is_data || relocs[i].disp_off < named[n].start ||
          relocs[i].disp_off >= named[n].end)
        continue;
      if (!ny_elf64_obj_rela_data_entry(&file, &relocs[i], defs, def_count,
                                        reloc_symbols, reloc_symbol_count,
                                        named[n].start, err, err_len))
        goto done;
    }
    named[n].rela_size = file.len - named[n].rela_off;
  }
  size_t rela_debug_off = file.len;
  if (have_debug) {
    uint64_t info = ((uint64_t)(1u + (uint32_t)debug_base_def) << 32) | 1u;
    if (!nyr_obj_u64(&file, debug_base_reloc) || !nyr_obj_u64(&file, info) ||
        !nyr_obj_u64(&file, 0))
      goto done;
  }
  size_t rela_debug_size = file.len - rela_debug_off;
  size_t rela_debug_info_off = file.len;
  if (have_debug) {
    uint64_t info = ((uint64_t)(1u + (uint32_t)debug_info_base_def) << 32) | 1u;
    if (!nyr_obj_u64(&file, debug_info_reloc) || !nyr_obj_u64(&file, info) ||
        !nyr_obj_u64(&file, 0))
      goto done;
  }
  size_t rela_debug_info_size = file.len - rela_debug_info_off;
  NY_X64_ELF_PROFILE_STAGE("symbol table construction");
  size_t local_def_count = 0;
  for (size_t i = 0; i < def_count; ++i)
    local_def_count += defs[i].is_local;
  size_t symtab_off = file.len;
  if (!nyr_obj_elf64_write_sym(&file, 0, 0, 0, 0, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    size_t def_group = named_count;
    size_t code_group = named_code_count;
    if (defs[i].section) {
      if (defs[i].is_data) {
        for (def_group = 0; def_group < named_count; ++def_group) {
          if (strcmp(named[def_group].name, defs[i].section) == 0)
            break;
        }
      } else {
        for (code_group = 0; code_group < named_code_count; ++code_group) {
          if (strcmp(named_code[code_group].name, defs[i].section) == 0)
            break;
        }
      }
      if ((defs[i].is_data && def_group == named_count) ||
          (!defs[i].is_data && code_group == named_code_count)) {
        nyr_set_error(err, err_len,
                      "x86-64 ELF object writer: symbol %s names unknown "
                      "section %s",
                      defs[i].name, defs[i].section);
        goto done;
      }
    }
    bool in_data = defs[i].is_data && defs[i].off < data_end &&
                   (def_group < named_count || defs[i].off >= data_start);
    uint64_t value = !defs[i].is_data && code_group < named_code_count
                         ? defs[i].off - named_code[code_group].start
                     : def_group < named_count
                         ? defs[i].off - named[def_group].start
                     : in_data ? defs[i].off - data_start
                               : defs[i].off;
    unsigned type = in_data && !defs[i].function_symbol ? 1u : 2u;
    unsigned info = (defs[i].is_local  ? 0u
                     : defs[i].is_weak ? 0x20u
                                       : 0x10u) |
                    type;
    uint32_t shndx = !defs[i].is_data && code_group < named_code_count
                         ? named_code[code_group].data_idx
                     : def_group < named_count ? named[def_group].data_idx
                     : in_data                 ? sec_data
                                               : 1;
    if (!nyr_obj_elf64_write_sym(&file, def_name_offs[i], info, shndx, value,
                                 defs[i].size))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    /* The relocation names a symbol, not necessarily a function. Leave
     * undefined references untyped so they do not retype assembly labels
     * or data when a relocatable link resolves them. */
    if (!nyr_obj_elf64_write_sym(&file, reloc_name_offs[i], 0x10, 0, 0, 0))
      goto done;
  }
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!nyr_obj_emit(&file, strtab.data, strtab.len))
    goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstrtab_off = file.len;
  if (!nyr_obj_emit(&file, shstr, sizeof(shstr)))
    goto done;
  for (size_t n = 0; n < named_code_count; ++n) {
    char section_name[256];
    int written =
        snprintf(section_name, sizeof(section_name), "%s", named_code[n].name);
    if (written < 0 || (size_t)written >= sizeof(section_name)) {
      nyr_set_error(
          err, err_len,
          "x86-64 ELF object writer: function section name is too long");
      goto done;
    }
    named_code[n].name_off = (uint32_t)(file.len - shstrtab_off);
    if (!nyr_obj_emit(&file, section_name, (size_t)written + 1))
      goto done;
    written = snprintf(section_name, sizeof(section_name), ".rela%s",
                       named_code[n].name);
    if (written < 0 || (size_t)written >= sizeof(section_name)) {
      nyr_set_error(
          err, err_len,
          "x86-64 ELF object writer: function section name is too long");
      goto done;
    }
    named_code[n].rela_name_off = (uint32_t)(file.len - shstrtab_off);
    if (!nyr_obj_emit(&file, section_name, (size_t)written + 1))
      goto done;
  }
  for (size_t n = 0; n < named_count; ++n) {
    char section_name[256];
    /*
     * GNU section attributes spell the complete name, with or without a dot.
     */
    int written =
        snprintf(section_name, sizeof(section_name), "%s", named[n].name);
    if (written < 0 || (size_t)written >= sizeof(section_name)) {
      nyr_set_error(err, err_len,
                    "x86-64 ELF object writer: data section name is too long");
      goto done;
    }
    named[n].name_off = (uint32_t)(file.len - shstrtab_off);
    if (!nyr_obj_emit(&file, section_name, (size_t)written + 1))
      goto done;
    written =
        snprintf(section_name, sizeof(section_name), ".rela%s", named[n].name);
    if (written < 0 || (size_t)written >= sizeof(section_name)) {
      nyr_set_error(err, err_len,
                    "x86-64 ELF object writer: data section name is too long");
      goto done;
    }
    named[n].rela_name_off = (uint32_t)(file.len - shstrtab_off);
    if (!nyr_obj_emit(&file, section_name, (size_t)written + 1))
      goto done;
  }
  if (!nyr_obj_pad_to(&file, 8))
    goto done;
  size_t shstrtab_size = file.len - shstrtab_off;
  if (ny_dbg_is_enabled(NY_DBG_ELF)) {
    ny_dbg_elf_section(".text", text_off, text_len, 0x6);
    ny_dbg_elf_section(".rela.text", rela_off, rela_size, 0);
    if (have_data) {
      ny_dbg_elf_section(".data", data_off, data_len, 0x3);
      if (have_rela_data)
        ny_dbg_elf_section(".rela.data", rela_data_off, rela_data_size, 0);
    }
    if (have_debug) {
      ny_dbg_elf_section(".debug_line", debug_off, debug_line.len, 0);
      ny_dbg_elf_section(".rela.debug_line", rela_debug_off, rela_debug_size,
                         0);
      ny_dbg_elf_section(".debug_info", debug_info_off, debug_info.len, 0);
      ny_dbg_elf_section(".rela.debug_info", rela_debug_info_off,
                         rela_debug_info_size, 0);
      ny_dbg_elf_section(".debug_abbrev", debug_abbrev_off, debug_abbrev.len,
                         0);
    }
    for (size_t n = 0; n < named_code_count; ++n) {
      ny_dbg_elf_section(named_code[n].name, named_code[n].data_off,
                         named_code[n].end - named_code[n].start, 0x6);
      if (named_code[n].has_rela)
        ny_dbg_elf_section(named_code[n].name, named_code[n].rela_off,
                           named_code[n].rela_size, 0);
    }
    for (size_t n = 0; n < named_count; ++n) {
      ny_dbg_elf_section(named[n].name, named[n].data_off,
                         named[n].end - named[n].start, 0x3);
      if (named[n].has_rela)
        ny_dbg_elf_section(named[n].name, named[n].rela_off, named[n].rela_size,
                           0);
    }
    ny_dbg_elf_section(".symtab", symtab_off, symtab_size, 0);
    ny_dbg_elf_section(".strtab", strtab_off, strtab_size, 0);
    ny_dbg_elf_section(".shstrtab", shstrtab_off, shstrtab_size, 0);
    for (size_t i = 0; i < reloc_count; ++i)
      ny_dbg_elf_reloc(
          relocs[i].disp_off, relocs[i].symbol,
          relocs[i].type == NY_RELOC_PC32         ? "R_X86_64_PC32"
          : relocs[i].type == NY_RELOC_GOTPCREL   ? "R_X86_64_GOTPCREL"
          : relocs[i].type == NY_RELOC_X86_64_32S ? "R_X86_64_32S"
                                                  : "R_X86_64_PLT32");
    for (size_t i = 0; i < def_count; ++i) {
      bool in_data =
          have_data && defs[i].off >= data_start && defs[i].off < data_end;
      ny_dbg_elf_symbol(defs[i].name,
                        defs[i].section ? defs[i].section
                        : in_data       ? ".data"
                                        : ".text",
                        in_data ? defs[i].off - data_start : defs[i].off,
                        defs[i].size);
    }
    for (size_t i = 0; i < reloc_symbol_count; ++i)
      ny_dbg_elf_symbol(reloc_symbols[i], "UND", 0, 0);
  }
  /*
   * The .data header exists ONLY when the pool contributed bytes.
   * Writing a placeholder unconditionally shifted every later section
   * index by one while the link fields and e_shstrndx still assumed the
   * dense layout -- symtab linked to itself and section names were read
   * from the symbol string table (<corrupt> symbols at link time).
   */
  NY_X64_ELF_PROFILE_STAGE("section header construction");
  size_t shoff = file.len;
  if (!ny_elf64_write_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !ny_elf64_write_sh(&file, sh_text, 1, 0x6, text_off, text_len, 0, 0, 32,
                         0) ||
      !ny_elf64_write_sh(&file, sh_rela_text, 4, 0, rela_off, rela_size,
                         symtab_idx, 1, 8, 24))
    goto done;
  for (size_t n = 0; n < named_code_count; ++n) {
    if (!ny_elf64_write_sh(
            &file, named_code[n].name_off, 1, 0x6, named_code[n].data_off,
            named_code[n].end - named_code[n].start, 0, 0, 16, 0))
      goto done;
    if (named_code[n].has_rela &&
        !ny_elf64_write_sh(&file, named_code[n].rela_name_off, 4, 0,
                           named_code[n].rela_off, named_code[n].rela_size,
                           symtab_idx, named_code[n].data_idx, 8, 24))
      goto done;
  }
  if (have_data && !ny_elf64_write_sh(&file, sh_data, 1, 0x3, data_off,
                                      data_len, 0, 0, 8, 0))
    goto done;
  if (sec_rela_data &&
      !ny_elf64_write_sh(&file, sh_rela_data, 4, 0, rela_data_off,
                         rela_data_size, symtab_idx, sec_data, 8, 24))
    goto done;
  for (size_t n = 0; n < named_count; ++n) {
    uint32_t type = ny_c_bss_section(named[n].name)            ? 8
                    : strncmp(named[n].name, ".note.", 6) == 0 ? 7
                                                               : 1;
    if (!ny_elf64_write_sh(&file, named[n].name_off, type, named[n].flags,
                           named[n].data_off, named[n].end - named[n].start, 0,
                           0, named[n].alignment, named[n].entsize))
      goto done;
    if (named[n].has_rela &&
        !ny_elf64_write_sh(&file, named[n].rela_name_off, 4, 0,
                           named[n].rela_off, named[n].rela_size, symtab_idx,
                           named[n].data_idx, 8, 24))
      goto done;
  }
  if (have_debug &&
      (!ny_elf64_write_sh(&file, sh_debug_line, 1, 0, debug_off, debug_line.len,
                          0, 0, 1, 0) ||
       !ny_elf64_write_sh(&file, sh_rela_debug_line, 4, 0, rela_debug_off,
                          rela_debug_size, symtab_idx, sec_debug, 8, 24)))
    goto done;
  if (have_debug &&
      (!ny_elf64_write_sh(&file, sh_debug_info, 1, 0, debug_info_off,
                          debug_info.len, 0, 0, 1, 0) ||
       !ny_elf64_write_sh(&file, sh_rela_debug_info, 4, 0, rela_debug_info_off,
                          rela_debug_info_size, symtab_idx, sec_debug_info, 8,
                          24) ||
       !ny_elf64_write_sh(&file, sh_debug_abbrev, 1, 0, debug_abbrev_off,
                          debug_abbrev.len, 0, 0, 1, 0)))
    goto done;
  if (!ny_elf64_write_sh(&file, sh_symtab, 2, 0, symtab_off, symtab_size,
                         strtab_idx, 1u + (uint32_t)local_def_count, 8, 24) ||
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
  nyr_obj_patch_u16(&file, 16, 1);
  nyr_obj_patch_u16(&file, 18, 62);
  nyr_obj_patch_u32(&file, 20, 1);
  nyr_obj_patch_u64(&file, 40, shoff);
  nyr_obj_patch_u16(&file, 52, 64);
  nyr_obj_patch_u16(&file, 58, 64);
  nyr_obj_patch_u16(&file, 60, (uint16_t)(shstrtab_idx + 1));
  nyr_obj_patch_u16(&file, 62, (uint16_t)shstrtab_idx);

  NY_X64_ELF_PROFILE_STAGE("writing object file");
  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  if (profile) {
    clock_t stage_ended = clock();
    fprintf(stderr,
            "[aot-object] phase=elf64-writer stage=%s elapsed=%.6fs "
            "defs=%zu relocs=%zu\n",
            stage, (double)(stage_ended - stage_started) / CLOCKS_PER_SEC,
            def_count, reloc_count);
  }
  free(reloc_symbols);
  free(def_name_offs);
  free(reloc_name_offs);
  nyr_obj_free(&debug_line);
  nyr_obj_free(&debug_info);
  nyr_obj_free(&debug_abbrev);
  nyr_obj_free(&strtab);
  nyr_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "x86-64 ELF object writer failed during %s",
                  stage);
#undef NY_X64_ELF_PROFILE_STAGE
  return ok;
}

static bool ny_native_emit_coff_x64_object_bundle_code(
    const unsigned char *code, size_t code_len,
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    const nyr_native_debug_line_t *debug_lines, size_t debug_line_count,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    nyr_set_error(err, err_len, "x86-64 COFF object writer: missing input");
    return false;
  }
  nyr_obj_buf_t file = {0};
  nyr_obj_buf_t strings = {0};
  nyr_obj_buf_t debug = {0};
  bool ok = false;
  char (*reloc_symbols)[256] =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t *def_name_offs = calloc(def_count, sizeof(*def_name_offs));
  uint32_t *reloc_name_offs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*reloc_name_offs));
  if (!reloc_symbols || !def_name_offs || !reloc_name_offs) {
    /*
     * `done` releases all three; freeing here double-freed on failure.
     */
    nyr_set_error(err, err_len, "x86-64 COFF object writer: allocation failed");
    goto done;
  }
  const size_t header_size = 20;
  if (!nyr_native_build_codeview_lines(debug_lines, debug_line_count, code_len,
                                       path, &debug, err, err_len))
    goto done;
  const size_t section_count = debug.len ? 2 : 1;
  const size_t section_table_size = 40 * section_count;
  const size_t text_off = header_size + section_table_size;
  const size_t reloc_off = text_off + code_len;
  const size_t reloc_size = reloc_count * 10;
  const size_t debug_off = reloc_off + reloc_size;
  const size_t symtab_off = debug_off + debug.len;

  if (!nyr_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;
  if (!nyr_obj_u32(&strings, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (strlen(defs[i].name) > 8) {
      def_name_offs[i] = (uint32_t)strings.len;
      if (!nyr_obj_emit(&strings, defs[i].name, strlen(defs[i].name) + 1))
        goto done;
    }
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (strlen(reloc_symbols[i]) > 8) {
      reloc_name_offs[i] = (uint32_t)strings.len;
      if (!nyr_obj_emit(&strings, reloc_symbols[i],
                        strlen(reloc_symbols[i]) + 1))
        goto done;
    }
  }
  nyr_obj_patch_u32(&strings, 0, (uint32_t)strings.len);

  uint32_t nsyms = (uint32_t)(1 + def_count + reloc_symbol_count);
  if (!nyr_obj_u16(&file, 0x8664) ||
      !nyr_obj_u16(&file, (uint16_t)section_count) || !nyr_obj_u32(&file, 0) ||
      !nyr_obj_u32(&file, (uint32_t)symtab_off) || !nyr_obj_u32(&file, nsyms) ||
      !nyr_obj_u16(&file, 0) || !nyr_obj_u16(&file, 0))
    goto done;

  char sec_name[8] = {0};
  memcpy(sec_name, ".text", 5);
  if (!nyr_obj_emit(&file, sec_name, sizeof(sec_name)) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0) ||
      !nyr_obj_u32(&file, (uint32_t)code_len) ||
      !nyr_obj_u32(&file, (uint32_t)text_off) ||
      !nyr_obj_u32(&file, (uint32_t)(reloc_count ? reloc_off : 0)) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u16(&file, (uint16_t)reloc_count) ||
      !nyr_obj_u16(&file, 0) || !nyr_obj_u32(&file, 0x60500020u))
    goto done;
  if (section_count == 2) {
    char debug_name[8] = {0};
    memcpy(debug_name, ".debug$S", 8);
    if (!nyr_obj_emit(&file, debug_name, sizeof(debug_name)) ||
        !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0) ||
        !nyr_obj_u32(&file, (uint32_t)debug.len) ||
        !nyr_obj_u32(&file, (uint32_t)debug_off) || !nyr_obj_u32(&file, 0) ||
        !nyr_obj_u32(&file, 0) || !nyr_obj_u16(&file, 0) ||
        !nyr_obj_u16(&file, 0) || !nyr_obj_u32(&file, 0x42000040u))
      goto done;
  }

  if (!nyr_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int def_i = nyr_x64_obj_def_index(defs, def_count, relocs[i].symbol);
    uint32_t sym_index = 0;
    if (def_i >= 0) {
      sym_index = (uint32_t)(1 + def_i);
    } else {
      int ext_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                           relocs[i].symbol);
      if (ext_i < 0)
        goto done;
      sym_index = (uint32_t)(1 + def_count + (size_t)ext_i);
    }
    if (!nyr_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !nyr_obj_u32(&file, sym_index) || !nyr_obj_u16(&file, 0x0004))
      goto done;
  }
  if (debug.len && !nyr_obj_emit(&file, debug.data, debug.len))
    goto done;
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
  if (!nyr_obj_emit(&file, strings.data, strings.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(reloc_symbols);
  free(def_name_offs);
  free(reloc_name_offs);
  nyr_obj_free(&strings);
  nyr_obj_free(&debug);
  nyr_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "x86-64 COFF object writer failed");
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
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    const nyr_native_debug_line_t *debug_lines, size_t debug_line_count,
    char *err, size_t err_len) {
  if (!code || !path || !defs || def_count == 0) {
    nyr_set_error(err, err_len, "x86-64 Mach-O object writer: missing input");
    return false;
  }
  nyr_obj_buf_t file = {0};
  nyr_obj_buf_t strtab = {0};
  nyr_obj_buf_t debug = {0};
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
  if (!reloc_symbols || !def_name_offs || !reloc_name_offs || !macho_defs ||
      !macho_relocs) {
    /*
     * `done` releases all five; freeing here double-freed on failure.
     */
    nyr_set_error(err, err_len,
                  "x86-64 Mach-O object writer: allocation failed");
    goto done;
  }

  if (!nyr_x64_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;
  size_t debug_base_reloc = 0, debug_base_def = 0;
  if (!nyr_native_build_debug_line(debug_lines, debug_line_count, code_len,
                                   &debug, &debug_base_reloc, &debug_base_def,
                                   err, err_len))
    goto done;
  if (!nyr_obj_u8(&strtab, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_macho_symbol_name(macho_defs[i], sizeof(macho_defs[i]),
                              defs[i].name))
      goto done;
    def_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, macho_defs[i], strlen(macho_defs[i]) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!ny_macho_symbol_name(macho_relocs[i], sizeof(macho_relocs[i]),
                              reloc_symbols[i]))
      goto done;
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, macho_relocs[i], strlen(macho_relocs[i]) + 1))
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

  if (!nyr_obj_u32(&file, 0xfeedfacf) || !nyr_obj_u32(&file, 0x01000007) ||
      !nyr_obj_u32(&file, 3) || !nyr_obj_u32(&file, 1) ||
      !nyr_obj_u32(&file, 2) || !nyr_obj_u32(&file, sizeofcmds) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0))
    goto done;
  if (!nyr_obj_u32(&file, 0x19) || !nyr_obj_u32(&file, seg_cmdsize) ||
      !ny_macho_write_padded_name(&file, "") || !nyr_obj_u64(&file, 0) ||
      !nyr_obj_u64(&file, code_len) || !nyr_obj_u64(&file, text_off) ||
      !nyr_obj_u64(&file, code_len) || !nyr_obj_u32(&file, 7) ||
      !nyr_obj_u32(&file, 5) || !nyr_obj_u32(&file, section_count) ||
      !nyr_obj_u32(&file, 0))
    goto done;
  if (!ny_macho_write_padded_name(&file, "__text") ||
      !ny_macho_write_padded_name(&file, "__TEXT") || !nyr_obj_u64(&file, 0) ||
      !nyr_obj_u64(&file, code_len) || !nyr_obj_u32(&file, text_off) ||
      !nyr_obj_u32(&file, 4) ||
      !nyr_obj_u32(&file, reloc_count ? reloc_off : 0) ||
      !nyr_obj_u32(&file, (uint32_t)reloc_count) ||
      !nyr_obj_u32(&file, 0x80000400u) || !nyr_obj_u32(&file, 0) ||
      !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0))
    goto done;
  if (section_count == 2) {
    if (!ny_macho_write_padded_name(&file, "__debug_line") ||
        !ny_macho_write_padded_name(&file, "__DWARF") ||
        !nyr_obj_u64(&file, 0) || !nyr_obj_u64(&file, debug.len) ||
        !nyr_obj_u32(&file, debug_off) || !nyr_obj_u32(&file, 0) ||
        !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0) ||
        !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0) ||
        !nyr_obj_u32(&file, 0) || !nyr_obj_u32(&file, 0))
      goto done;
  }
  if (!nyr_obj_u32(&file, 0x2) || !nyr_obj_u32(&file, sym_cmdsize) ||
      !nyr_obj_u32(&file, symoff) || !nyr_obj_u32(&file, nsyms) ||
      !nyr_obj_u32(&file, stroff) || !nyr_obj_u32(&file, strsize))
    goto done;
  if (!nyr_obj_emit(&file, code, code_len))
    goto done;
  for (size_t i = 0; i < reloc_count; ++i) {
    int def_i = nyr_x64_obj_def_index(defs, def_count, relocs[i].symbol);
    uint32_t sym_index = 0;
    if (def_i >= 0) {
      sym_index = (uint32_t)def_i;
    } else {
      int ext_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                           relocs[i].symbol);
      if (ext_i < 0)
        goto done;
      sym_index = (uint32_t)(def_count + (size_t)ext_i);
    }
    uint32_t word = sym_index | (1u << 24) | (2u << 25) | (1u << 27) |
                    (2u << 28); /* pcrel long extern branch */
    if (!nyr_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !nyr_obj_u32(&file, word))
      goto done;
  }
  if (debug.len && !nyr_obj_emit(&file, debug.data, debug.len))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!nyr_obj_u32(&file, def_name_offs[i]) || !nyr_obj_u8(&file, 0x0f) ||
        !nyr_obj_u8(&file, 1) || !nyr_obj_u16(&file, 0) ||
        !nyr_obj_u16(&file, 0) || !nyr_obj_u64(&file, defs[i].off))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!nyr_obj_u32(&file, reloc_name_offs[i]) || !nyr_obj_u8(&file, 0x01) ||
        !nyr_obj_u8(&file, 0) || !nyr_obj_u16(&file, 0) ||
        !nyr_obj_u64(&file, 0))
      goto done;
  }
  if (!nyr_obj_emit(&file, strtab.data, strtab.len))
    goto done;

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(reloc_symbols);
  free(def_name_offs);
  free(reloc_name_offs);
  free(macho_defs);
  free(macho_relocs);
  nyr_obj_free(&strtab);
  nyr_obj_free(&debug);
  nyr_obj_free(&file);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "x86-64 Mach-O object writer failed");
  return ok;
}

bool nyr_x64_obj_build_bundle(const nyr_func_t *rt_main,
                              const nyr_func_t *funcs,
                              const char *const *func_names, size_t func_count,
                              const ny_native_target_info_t *target,
                              const char *entry_symbol, bool tag_return,
                              nyr_obj_buf_t *code,
                              nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
                              nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
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
  nyr_phi_elim((nyr_func_t *)rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyr_phi_elim((nyr_func_t *)&funcs[i]);
  for (size_t i = 0; i < func_count; ++i) {
    const char *name =
        func_names && func_names[i] ? func_names[i] : "unknown_fn";
    ny_dbg_lower_fn(name);
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (!nyr_x64_obj_append_function(code, defs, def_count, relocs, reloc_count,
                                     &funcs[i], target, symbol, false, err,
                                     err_len)) {
      if (err && err_len && !err[0])
        nyr_set_error(err, err_len,
                      "x86-64 object writer: function append failed for %s",
                      symbol);
      return false;
    }
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  if (!nyr_x64_obj_append_function(code, defs, def_count, relocs, reloc_count,
                                   rt_main, target, entry, tag_return, err,
                                   err_len))
    return false;
  if (!nyr_strtab_append_defs(code, defs, def_count, err, err_len))
    return false;
  return nyr_arraytab_append_defs(code, defs, def_count, err, err_len);
}

/*
 * Append every c-object data symbol whose requested section name matches
 * `want` (NULL = the default `.data` region).  Data groups are appended
 * back-to-back so the writer can slice one contiguous range per section.
 */
static bool nyr_x64_c_bundle_append_data_group(
    const nyr_c_object_data_t *data, size_t data_count, const char *want,
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, char *err,
    size_t err_len) {
  for (size_t i = 0; i < data_count; ++i) {
    if (data[i].alias_target)
      continue;
    bool match;
    if (data[i].section == NULL) {
      match = want == NULL;
    } else {
      match = want != NULL && strcmp(data[i].section, want) == 0;
    }
    if (!match)
      continue;
    if (!data[i].name || !data[i].name[0] || (!data[i].bytes && data[i].len) ||
        (data[i].len == 0 && data[i].bytes == NULL) ||
        (data[i].reloc_count && !data[i].relocs)) {
      nyr_set_error(err, err_len,
                    "C object invalid global data: name=%s bytes=%p len=%zu "
                    "relocs=%p rc=%zu sec=%s",
                    data[i].name ? data[i].name : "(null)",
                    (void *)data[i].bytes, data[i].len, (void *)data[i].relocs,
                    data[i].reloc_count,
                    data[i].section ? data[i].section : "(null)");
      return false;
    }
    /* modpost finds the assembler record label at the export relocation
     * offset. A second, compiler-private symbol obscures that label. */
    bool reference_alias =
        !want || strncmp(want, ".export_symbol", sizeof(".export_symbol") - 1);
    char reference_name[256];
    int alias_len = reference_alias
                        ? snprintf(reference_name, sizeof(reference_name),
                                   "__nyc_data_%s", data[i].name)
                        : 0;
    if (alias_len < 0 || (size_t)alias_len >= sizeof(reference_name)) {
      nyr_set_error(err, err_len,
                    "C object global data name exceeds symbol limit");
      return false;
    }
    if (nyr_x64_obj_def_index(defs, *def_count, data[i].name) >= 0 ||
        (reference_alias &&
         nyr_x64_obj_def_index(defs, *def_count, reference_name) >= 0)) {
      nyr_set_error(err, err_len, "duplicate C object symbol %s", data[i].name);
      return false;
    }
    if (*def_count > NY_NATIVE_MAX_DEFS - (reference_alias ? 2 : 1) ||
        strlen(data[i].name) >= sizeof(defs[0].name)) {
      nyr_set_error(err, err_len, "C object global data table is full");
      return false;
    }
    size_t alignment = data[i].alignment ? data[i].alignment : 1;
    /*
     * Per-CPU IRQ stacks are page-aligned and then rounded to IRQ_STACK_SIZE
     * (16 KiB on x86-64), so ELF section alignment may legitimately exceed a
     * page. Keep a finite power-of-two ceiling to reject bogus attributes.
     */
    if ((alignment & (alignment - 1)) != 0 || alignment > 65536) {
      nyr_set_error(err, err_len,
                    "C object data alignment is invalid for %s (%zu)",
                    data[i].name, alignment);
      return false;
    }
    if (!nyr_obj_pad_to(code, alignment))
      return false;
    nyr_x64_obj_symbol_def_t *def = &defs[(*def_count)++];
    snprintf(def->name, sizeof(def->name), "%s", data[i].name);
    def->off = code->len;
    def->size = data[i].len;
    def->is_data = true;
    def->section = data[i].section;
    def->section_flags =
        data[i].section ? (data[i].section_flags ? data[i].section_flags : 3)
                        : 3;
    def->alignment = alignment;
    def->section_entsize = data[i].section_entsize;
    def->function_symbol = data[i].function_symbol;
    def->is_local = data[i].is_local;
    def->is_weak = data[i].is_weak;
    if (reference_alias) {
      nyr_x64_obj_symbol_def_t *reference = &defs[(*def_count)++];
      *reference = *def;
      snprintf(reference->name, sizeof(reference->name), "%s", reference_name);
      reference->is_local = true;
      reference->is_weak = false;
    }
    if (!nyr_obj_emit(code, data[i].bytes, data[i].len))
      return false;
    for (size_t r = 0; r < data[i].reloc_count; ++r) {
      const nyr_c_object_data_reloc_t *source = &data[i].relocs[r];
      size_t width = source->width ? source->width : sizeof(uint64_t);
      if (!source->symbol || !source->symbol[0] ||
          (width != sizeof(uint64_t) && width != sizeof(uint32_t)) ||
          source->offset > data[i].len ||
          width > data[i].len - source->offset ||
          *reloc_count >= NY_X64_OBJ_MAX_RELOCS) {
        nyr_set_error(err, err_len,
                      "C object contains an invalid data relocation");
        return false;
      }
      nyr_x64_obj_reloc_t *reloc = &relocs[(*reloc_count)++];
      memset(reloc, 0, sizeof(*reloc));
      if (strlen(source->symbol) >= sizeof(reloc->symbol)) {
        nyr_set_error(err, err_len, "C object relocation symbol is too long");
        return false;
      }
      snprintf(reloc->symbol, sizeof(reloc->symbol), "%s", source->symbol);
      reloc->disp_off = def->off + source->offset;
      reloc->type = source->type ? source->type : NY_RELOC_X86_64_64;
      if ((reloc->type == NY_RELOC_PC32 && width != sizeof(uint32_t)) ||
          ((reloc->type == NY_RELOC_X86_64_64) && width != sizeof(uint64_t)) ||
          (reloc->type == NY_RELOC_X86_64_32S && width != sizeof(uint32_t))) {
        nyr_set_error(err, err_len,
                      "C object data relocation width does not match type");
        return false;
      }
      reloc->is_data = true;
      reloc->addend = source->addend;
    }
  }
  return true;
}

static bool nyr_x64_c_bundle_pad_section(nyr_obj_buf_t *code,
                                         const nyr_c_object_data_t *data,
                                         size_t data_count, const char *section,
                                         size_t minimum, char *err,
                                         size_t err_len) {
  size_t alignment = minimum;
  for (size_t i = 0; i < data_count; ++i) {
    bool member = section ? data[i].section && !strcmp(data[i].section, section)
                          : !data[i].section;
    if (member && data[i].alignment > alignment && data[i].alignment <= 4096 &&
        !(data[i].alignment & (data[i].alignment - 1)))
      alignment = data[i].alignment;
  }
  if (nyr_obj_pad_to(code, alignment))
    return true;
  nyr_set_error(err, err_len, "C object section alignment failed");
  return false;
}

/*
 * The default section's alignment padding still lands inside .text: the data
 * region starts only after it. Those bytes must decode as instructions, so
 * this variant uses nops instead of zeros.
 */
static bool nyr_x64_c_bundle_pad_code_start(nyr_obj_buf_t *code,
                                            const nyr_c_object_data_t *data,
                                            size_t data_count, size_t minimum,
                                            char *err, size_t err_len) {
  size_t alignment = minimum;
  for (size_t i = 0; i < data_count; ++i)
    if (!data[i].section && data[i].alignment > alignment &&
        data[i].alignment <= 4096 &&
        !(data[i].alignment & (data[i].alignment - 1)))
      alignment = data[i].alignment;
  if (nyr_obj_pad_code(code, alignment))
    return true;
  nyr_set_error(err, err_len, "C object section alignment failed");
  return false;
}

/*
 * Constructs only the direct NYIR encoder implements: SysV by-value aggregate
 * parameters and returns, and inline asm with operands or a result, which
 * machine lowering would silently drop.
 */
static bool nyr_x64_c_func_needs_direct_encoder(const nyr_func_t *f) {
  if (f->return_aggregate_size)
    return true;
  for (size_t i = 0; f->param_types && i < (size_t)f->param_count; ++i)
    if (f->param_types[i] == NYIR_PARAM_AGGREGATE)
      return true;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_CALL && f->data[i].ret_aggregate_size)
      return true;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_INLINE_ASM)
      continue;
    bool arithmetic = in->imm == -21 || in->imm == -22 || in->imm == -23;
    bool operand_free = in->dst < 0 && in->a < 0 && in->b < 0;
    if (!arithmetic && !operand_free && in->imm != -32)
      return true;
  }
  return false;
}

/*
 * x86-64 libgcc has no u64-to-float helpers because GCC expands that
 * conversion inline, but the C frontend calls them. Assembly output defines
 * them as weak symbols; objects get a private copy when a function needs one.
 */
static bool nyr_x64_c_bundle_append_float_helpers(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count, char *err,
    size_t err_len) {
  static const unsigned char to_f64[] = {
      0x48, 0x85, 0xff,             /* test rdi, rdi */
      0x78, 0x06,                   /* js high */
      0xf2, 0x48, 0x0f, 0x2a, 0xc7, /* cvtsi2sd xmm0, rdi */
      0xc3,                         /* ret */
      0x48, 0x89, 0xf8,             /* high: mov rax, rdi */
      0x48, 0x89, 0xf9,             /* mov rcx, rdi */
      0x48, 0x83, 0xe1, 0x01,       /* and rcx, 1 */
      0x48, 0xd1, 0xe8,             /* shr rax, 1 */
      0x48, 0x09, 0xc8,             /* or rax, rcx */
      0xf2, 0x48, 0x0f, 0x2a, 0xc0, /* cvtsi2sd xmm0, rax */
      0xf2, 0x0f, 0x58, 0xc0,       /* addsd xmm0, xmm0 */
      0xc3};
  static const unsigned char to_f32[] = {
      0x48, 0x85, 0xff,             /* test rdi, rdi */
      0x78, 0x06,                   /* js high */
      0xf3, 0x48, 0x0f, 0x2a, 0xc7, /* cvtsi2ss xmm0, rdi */
      0xc3,                         /* ret */
      0x48, 0x89, 0xf8,             /* high: mov rax, rdi */
      0x48, 0x89, 0xf9,             /* mov rcx, rdi */
      0x48, 0x83, 0xe1, 0x01,       /* and rcx, 1 */
      0x48, 0xd1, 0xe8,             /* shr rax, 1 */
      0x48, 0x09, 0xc8,             /* or rax, rcx */
      0xf3, 0x48, 0x0f, 0x2a, 0xc0, /* cvtsi2ss xmm0, rax */
      0xf3, 0x0f, 0x58, 0xc0,       /* addss xmm0, xmm0 */
      0xc3};
  static const struct {
    const char *name;
    const unsigned char *bytes;
    size_t len;
  } helpers[] = {{"__floatundidf", to_f64, sizeof(to_f64)},
                 {"__floatundisf", to_f32, sizeof(to_f32)}};
  for (size_t h = 0; h < sizeof(helpers) / sizeof(helpers[0]); ++h) {
    bool needed = false;
    for (size_t i = 0; i < reloc_count && !needed; ++i)
      needed = strcmp(relocs[i].symbol, helpers[h].name) == 0;
    if (!needed ||
        nyr_x64_obj_def_index(defs, *def_count, helpers[h].name) >= 0)
      continue;
    if (*def_count >= NY_NATIVE_MAX_DEFS) {
      nyr_set_error(err, err_len, "too many C object symbols");
      return false;
    }
    if (!nyr_obj_pad_code(code, 16)) {
      nyr_set_error(err, err_len, "C object helper emission failed");
      return false;
    }
    nyr_x64_obj_symbol_def_t *def = &defs[(*def_count)++];
    memset(def, 0, sizeof(*def));
    snprintf(def->name, sizeof(def->name), "%s", helpers[h].name);
    def->off = code->len;
    def->size = helpers[h].len;
    def->function_symbol = true;
    def->is_local = true;
    if (!nyr_obj_emit(code, helpers[h].bytes, helpers[h].len)) {
      nyr_set_error(err, err_len, "C object helper emission failed");
      return false;
    }
  }
  return true;
}

static bool nyr_x64_obj_build_c_bundle(
    const nyr_func_t *funcs, const char *const *func_names, size_t func_count,
    const char *const *function_sections, const bool *function_local,
    const bool *function_weak, bool omit_frame_pointer,
    bool position_independent, bool retpoline_indirect_calls,
    bool retpoline_inline_calls, bool function_return_thunk_extern,
    bool cf_protection_branch, const nyr_c_object_data_t *data,
    size_t data_count, const ny_native_target_info_t *target,
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, char *err,
    size_t err_len) {
  if ((func_count && (!funcs || !func_names)) || (data_count && !data) ||
      !target || !code || !defs || !def_count || !relocs || !reloc_count) {
    nyr_set_error(err, err_len, "invalid C object emission inputs");
    return false;
  }
  *def_count = 0;
  *reloc_count = 0;
  const char *function_section_names[NY_C_DATA_MAX_NAMED_SECTIONS];
  size_t function_section_count = 0;
  const char *off = getenv("NYC_NO_MACH_OBJECT");
  bool use_machine = !off || !off[0] || off[0] == '0';
  for (size_t i = 0; i < func_count; ++i) {
    const char *section = function_sections ? function_sections[i] : NULL;
    if (!section)
      continue;
    size_t n = 0;
    for (; n < function_section_count; ++n)
      if (strcmp(function_section_names[n], section) == 0)
        break;
    if (n == function_section_count) {
      if (function_section_count >= NY_C_DATA_MAX_NAMED_SECTIONS) {
        nyr_set_error(err, err_len, "too many named function sections");
        return false;
      }
      function_section_names[function_section_count++] = section;
    }
  }
  for (size_t group = 0; group <= function_section_count; ++group) {
    const char *wanted = group == 0 ? NULL : function_section_names[group - 1];
    for (size_t i = 0; i < func_count; ++i) {
      const char *section = function_sections ? function_sections[i] : NULL;
      if ((wanted == NULL) != (section == NULL) ||
          (wanted && strcmp(wanted, section) != 0))
        continue;
      const char *name = func_names[i];
      if (!name || !name[0]) {
        nyr_set_error(err, err_len,
                      "C object emission encountered an empty function name");
        return false;
      }
      char symbol[256];
      int written =
          snprintf(symbol, sizeof(symbol), "%s%s",
                   target->symbol_prefix ? target->symbol_prefix : "", name);
      if (written < 0 || (size_t)written >= sizeof(symbol)) {
        nyr_set_error(err, err_len,
                      "C object function name exceeds the object symbol limit");
        return false;
      }
      if (nyr_x64_obj_def_index(defs, *def_count, symbol) >= 0) {
        nyr_set_error(err, err_len, "duplicate C object symbol %s", symbol);
        return false;
      }
      size_t function_def = *def_count;
      char function_err[256] = {0};
      /*
       * The register-allocating machine encoder is the same one the language
       * uses. Functions that ask for frame-pointer omission, use named
       * sections or PIC, or cannot lower to machine form use the direct
       * NYIR encoder.
       *
       * The machine encoder is the default. It used to be opt-in because
       * stencil-shaped C IR read a store address from a canonical home that was
       * never written: the element index was a dedicated loop GPR, and the
       * loop-GPR exemption skipped the home write even though the SIB
       * rematerializer and the base-only fallback load every non-resident
       * address operand from that home (susan wrote every interior pixel as 0).
       * object64.c's address-operand classification now covers those
       * non-adjacent uses (mach_vreg_feeds_block_address), so both paths always
       * find a current home. NYC_NO_MACH_OBJECT=1 selects the direct encoder.
       */
      bool machine = false;
      if (use_machine && !omit_frame_pointer && !position_independent &&
          !section && !nyr_x64_c_func_needs_direct_encoder(&funcs[i])) {
        nyr_mach_func_t mach = {0};
        char reason[256] = {0};
        if (nyr_mach_lower_nir(&funcs[i], &mach, target->caps, reason,
                               sizeof(reason))) {
          mach.c_abi = true;
          mach.x64_endbr = cf_protection_branch;
          mach.x64_return_thunk = function_return_thunk_extern;
          mach.x64_indirect_thunk = retpoline_indirect_calls;
          mach.x64_inline_thunk = retpoline_inline_calls;
          for (size_t ii = 0; ii < mach.inst_len; ++ii)
            if (mach.insts[ii].opcode == NYR_MACH_CALL)
              mach.insts[ii].call_is_extern = true;
          size_t *inst_offsets = NULL;
          size_t inst_offsets_len = 0;
          machine = nyr_x64_mach_append_function_debug(
              code, defs, def_count, relocs, reloc_count, &mach, target, symbol,
              false, false, NULL, NULL, NULL, NULL, reason, sizeof(reason),
              &inst_offsets, &inst_offsets_len);
          int emitted =
              machine ? nyr_x64_obj_def_index(defs, *def_count, symbol) : -1;
          if (emitted >= 0)
            nyr_mach_dump_mixed(&funcs[i], &mach,
                                code->data + defs[emitted].off,
                                defs[emitted].size, inst_offsets,
                                inst_offsets_len, name, stderr);
          free(inst_offsets);
          if (!machine && !reason[0])
            snprintf(reason, sizeof(reason), "encoder rejected function");
        } else if (!reason[0]) {
          for (size_t ii = 0; ii < funcs[i].len; ++ii) {
            if (!nyr_mach_opcode_supported(funcs[i].data[ii].op)) {
              snprintf(reason, sizeof(reason), "unsupported opcode %d",
                       (int)funcs[i].data[ii].op);
              break;
            }
          }
          if (!reason[0])
            snprintf(reason, sizeof(reason),
                     "machine lowering rejected function");
        }
        nyr_mach_func_free(&mach);
        if (!machine && getenv("NYC_TRACE_MACHINE_FALLBACK"))
          fprintf(stderr, "NYC object fallback: %s: %s\n", name, reason);
      }
      if (!machine) {
        nyr_phi_elim((nyr_func_t *)&funcs[i]);
        if (!nyr_x64_obj_append_function_with_frame(
                code, defs, def_count, relocs, reloc_count, &funcs[i], target,
                symbol, omit_frame_pointer, position_independent,
                retpoline_indirect_calls, retpoline_inline_calls,
                function_return_thunk_extern, cf_protection_branch,
                function_err, sizeof(function_err))) {
          nyr_set_error(err, err_len,
                        "C object writer failed while emitting function %s: %s",
                        name, function_err[0] ? function_err : "unknown error");
          return false;
        }
      }
      defs[function_def].is_local = function_local && function_local[i];
      defs[function_def].is_weak = function_weak && function_weak[i];
      defs[function_def].section = section;
    }
  }
  if (!nyr_x64_c_bundle_append_float_helpers(code, defs, def_count, relocs,
                                             *reloc_count, err, err_len))
    return false;
  /*
   * Data is padded relative to this shared buffer, but each section's offsets
   * are later taken from its own start. Sections must begin at an offset
   * that satisfies their strictest member, whatever length the code had.
   */
  if (!nyr_x64_c_bundle_pad_code_start(code, data, data_count, 16, err,
                                       err_len))
    return false;
  if (!nyr_strtab_append_defs(code, defs, def_count, err, err_len)) {
    if (err && err_len && !err[0])
      nyr_set_error(err, err_len, "C object string table emission failed");
    return false;
  }
  if (!nyr_arraytab_append_defs(code, defs, def_count, err, err_len)) {
    if (err && err_len && !err[0])
      nyr_set_error(err, err_len, "C object array table emission failed");
    return false;
  }
  /*
   * Default-section data first so pools and plain `.data` symbols stay
   * contiguous, then each named section in first-appearance order.
   */
  if (!nyr_x64_c_bundle_append_data_group(data, data_count, NULL, code, defs,
                                          def_count, relocs, reloc_count, err,
                                          err_len))
    return false;
  const char *names[NY_C_DATA_MAX_NAMED_SECTIONS];
  size_t named_count = 0;
  for (size_t i = 0; i < data_count; ++i) {
    if (!data[i].section)
      continue;
    size_t slot = named_count;
    for (size_t n = 0; n < named_count; ++n) {
      if (strcmp(names[n], data[i].section) == 0) {
        slot = n;
        break;
      }
    }
    if (slot == named_count) {
      if (named_count >= NY_C_DATA_MAX_NAMED_SECTIONS) {
        nyr_set_error(err, err_len, "too many named data sections");
        return false;
      }
      names[named_count++] = data[i].section;
    }
  }
  for (size_t n = 0; n < named_count; ++n) {
    if (!nyr_x64_c_bundle_pad_section(code, data, data_count, names[n], 1, err,
                                      err_len))
      return false;
    if (!nyr_x64_c_bundle_append_data_group(data, data_count, names[n], code,
                                            defs, def_count, relocs,
                                            reloc_count, err, err_len))
      return false;
  }
  for (size_t i = 0; i < data_count; ++i) {
    if (!data[i].alias_target)
      continue;
    bool function_alias = data[i].function_symbol;
    int target_index =
        nyr_x64_obj_def_index(defs, *def_count, data[i].alias_target);
    if (!data[i].name || !data[i].name[0] || target_index < 0 ||
        defs[target_index].is_data == function_alias ||
        strlen(data[i].name) >= sizeof(defs[0].name) ||
        *def_count > NY_NATIVE_MAX_DEFS - (function_alias ? 1 : 2) ||
        nyr_x64_obj_def_index(defs, *def_count, data[i].name) >= 0) {
      nyr_set_error(err, err_len, "invalid C object %s alias '%s' to '%s'",
                    function_alias ? "function" : "data",
                    data[i].name ? data[i].name : "", data[i].alias_target);
      return false;
    }
    if (function_alias) {
      nyr_x64_obj_symbol_def_t *alias = &defs[(*def_count)++];
      *alias = defs[target_index];
      snprintf(alias->name, sizeof(alias->name), "%s", data[i].name);
      alias->is_local = data[i].is_local;
      alias->is_weak = data[i].is_weak;
      continue;
    }
    char reference_name[sizeof(defs[0].name)];
    int length = snprintf(reference_name, sizeof(reference_name),
                          "__nyc_data_%s", data[i].name);
    if (length < 0 || (size_t)length >= sizeof(reference_name) ||
        nyr_x64_obj_def_index(defs, *def_count, reference_name) >= 0) {
      nyr_set_error(err, err_len, "invalid C object data alias name");
      return false;
    }
    nyr_x64_obj_symbol_def_t *alias = &defs[(*def_count)++];
    *alias = defs[target_index];
    snprintf(alias->name, sizeof(alias->name), "%s", data[i].name);
    alias->is_local = data[i].is_local;
    alias->is_weak = data[i].is_weak;
    nyr_x64_obj_symbol_def_t *reference = &defs[(*def_count)++];
    *reference = *alias;
    snprintf(reference->name, sizeof(reference->name), "%s", reference_name);
    reference->is_local = true;
  }
  /*
   * The C frontend lowers function addresses using NYIR's internal ny_fn_
   * spelling, while this object exports C functions under their ABI names.
   * Bind such references locally when the corresponding C definition is in
   * this translation unit; leave unresolved ny_fn_ references untouched.
   */
  static const char internal_function_prefix[] = "ny_fn_";
  for (size_t i = 0; i < *reloc_count; ++i) {
    nyr_x64_obj_reloc_t *reloc = &relocs[i];
    if (strncmp(reloc->symbol, internal_function_prefix,
                sizeof(internal_function_prefix) - 1) != 0)
      continue;
    const char *c_name = reloc->symbol + sizeof(internal_function_prefix) - 1;
    int def_index = nyr_x64_obj_def_index(defs, *def_count, c_name);
    if (def_index < 0 || defs[def_index].is_data)
      continue;
    memmove(reloc->symbol, c_name, strlen(c_name) + 1);
  }
  return true;
}

static int nyr_c_symbol_binding_order(const void *left, const void *right) {
  const nyr_x64_obj_symbol_def_t *a = left;
  const nyr_x64_obj_symbol_def_t *b = right;
  return (int)b->is_local - (int)a->is_local;
}

bool nyr_native_emit_elf64_c_object_from_nirs_with_data(
    const nyr_func_t *funcs, const char *const *func_names, size_t func_count,
    const char *const *function_sections, const bool *function_local,
    const bool *function_weak, bool omit_frame_pointer,
    bool position_independent, bool retpoline_indirect_calls,
    bool retpoline_inline_calls, bool function_return_thunk_extern,
    bool cf_protection_branch, const nyr_c_object_data_t *data,
    size_t data_count, const ny_native_target_info_t *target, const char *path,
    char *err, size_t err_len) {
  if (!path || !target || target->target != NY_NATIVE_TARGET_X86_64 ||
      !(target->caps & NY_NATIVE_CAP_ELF_OBJECT)) {
    nyr_set_error(err, err_len,
                  "C object emission requires an x86-64 ELF target and path");
    return false;
  }
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  nyr_x64_obj_reloc_t *relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0, reloc_count = 0;
  bool ok = defs && relocs;
  if (!ok)
    nyr_set_error(err, err_len, "C object emission allocation failed");
  if (ok)
    ok = nyr_x64_obj_build_c_bundle(
        funcs, func_names, func_count, function_sections, function_local,
        function_weak, omit_frame_pointer, position_independent,
        retpoline_indirect_calls, retpoline_inline_calls,
        function_return_thunk_extern, cf_protection_branch, data, data_count,
        target, &code, defs, &def_count, relocs, &reloc_count, err, err_len);
  if (ok) {
    qsort(defs, def_count, sizeof(*defs), nyr_c_symbol_binding_order);
    /* Sorting changes definition indices even when the last name stays put. */
    (void)nyr_x64_obj_def_index(NULL, 0, NULL);
  }
  if (ok)
    ok = ny_native_emit_elf64_x64_object_bundle_code(
        code.data, code.len, relocs, reloc_count, defs, def_count, path, NULL,
        0, err, err_len);
  free(defs);
  free(relocs);
  nyr_obj_free(&code);
  if (ok) {
    if (err && err_len)
      err[0] = '\0';
  } else if (!err || !err_len || !err[0]) {
    nyr_set_error(err, err_len, "C object emission failed");
  }
  return ok;
}

bool nyr_native_emit_elf64_c_object_from_nirs(
    const nyr_func_t *funcs, const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path, char *err,
    size_t err_len) {
  return nyr_native_emit_elf64_c_object_from_nirs_with_data(
      funcs, func_names, func_count, NULL, NULL, NULL, false, false, false,
      false, false, false, NULL, 0, target, path, err, err_len);
}

/*
 * Build an x86-64 object bundle one function at a time.  A machine-form
 * failure is recorded with its symbol and owning reason, then only that
 * function uses the established NYIR object encoder.
 */
static bool ny_native_x64_build_mixed_bundle_debug(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool bare_syscalls, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len);

static bool ny_native_x64_nir_requires_aggregate_fallback(
    const nyr_func_t *func, const ny_native_target_info_t *target) {
  if (!func || !target)
    return true;
  bool aggregate =
      target->abi == NY_NATIVE_ABI_WIN64 && func->return_aggregate_size > 0;
  for (size_t pi = 0; !aggregate && pi < func->param_count; ++pi)
    aggregate =
        func->param_types && func->param_types[pi] == NYIR_PARAM_AGGREGATE;
  for (size_t ii = 0; !aggregate && ii < func->len; ++ii) {
    const nyr_inst_t *in = &func->data[ii];
    if (in->op != NYIR_CALL || !in->arg_sizes)
      continue;
    int argc = (int)in->imm;
    for (int ai = 0; argc >= 0 && ai < argc && ai < NYIR_CALL_MAX_ARGS; ++ai)
      aggregate |= in->arg_sizes[ai] > 0;
  }
  return aggregate;
}

typedef struct {
  const nyr_func_t *funcs;
  const nyr_func_t *rt_main;
  size_t func_count;
  nyr_mach_func_t *mir;
  char (*errors)[256];
  unsigned caps;
} ny_native_x64_mach_lower_ctx_t;

static bool ny_native_x64_mach_lower_task(size_t i, void *opaque) {
  ny_native_x64_mach_lower_ctx_t *ctx =
      (ny_native_x64_mach_lower_ctx_t *)opaque;
  const nyr_func_t *func = i == ctx->func_count ? ctx->rt_main : &ctx->funcs[i];
  bool ok = nyr_mach_lower_nir(func, &ctx->mir[i], ctx->caps, ctx->errors[i],
                               sizeof(ctx->errors[i]));
  /*
   * One function the machine lowerer rejects must not demote the whole
   * program to the serial encoder: leave its slot empty so the bundle skips
   * it and the per-function NYIR fallback emits it.  Only rt_main is
   * mandatory.
   */
  if (!ok && i != ctx->func_count) {
    nyr_mach_func_free(&ctx->mir[i]);
    ctx->mir[i] = (nyr_mach_func_t){0};
    ctx->errors[i][0] = '\0';
    return true;
  }
  return ok;
}

static bool ny_native_x64_append_bundle_nir_fallback(
    const nyr_func_t *func, const char *symbol, const char *name,
    const ny_native_target_info_t *target, bool bare_syscalls,
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
  const char *reason = "parallel machine encoder rejected function";
  char fallback_err[256] = {0};
  size_t *inst_offsets = NULL;
  size_t inst_offsets_len = 0;
  ny_native_mach_record(false, symbol, reason);
  nyr_phi_elim((nyr_func_t *)func);
  if (!nyr_x64_obj_append_function_debug(
          code, defs, def_count, relocs, reloc_count, func, target, symbol,
          false, bare_syscalls, func, debug_lines, debug_line_count,
          debug_line_cap, fallback_err, sizeof(fallback_err), &inst_offsets,
          &inst_offsets_len)) {
    nyr_set_error(err, err_len,
                  "native function %s: machine form failed: %s; NYIR "
                  "fallback failed: %s",
                  symbol, reason,
                  fallback_err[0] ? fallback_err : "object emission failed");
    free(inst_offsets);
    return false;
  }
  int def = nyr_x64_obj_def_index(defs, *def_count, symbol);
  if (def >= 0) {
    nyr_mach_set_mixed_note(reason);
    nyr_mach_dump_mixed(func, NULL, code->data + defs[def].off, defs[def].size,
                        inst_offsets, inst_offsets_len, name, stderr);
  }
  free(inst_offsets);
  return true;
}

static bool ny_native_x64_try_parallel_bundle_debug(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool bare_syscalls, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
  if (!rt_main || !funcs || !target ||
      target->target != NY_NATIVE_TARGET_X86_64 || func_count < 2 ||
      getenv("NY_DUMP_MACH") ||
      ny_native_x64_nir_requires_aggregate_fallback(rt_main, target))
    return false;
  size_t work = rt_main->len;
  for (size_t i = 0; i < func_count; ++i) {
    if (ny_native_x64_nir_requires_aggregate_fallback(&funcs[i], target) ||
        funcs[i].len > SIZE_MAX - work)
      return false;
    work += funcs[i].len;
  }
  if (!ny_parallel_enabled(func_count + 1, work))
    return false;

  nyr_mach_func_t *mir = calloc(func_count + 1, sizeof(*mir));
  char (*lower_errors)[256] = calloc(func_count + 1, sizeof(*lower_errors));
  if (!mir || !lower_errors) {
    free(mir);
    free(lower_errors);
    return false;
  }
  ny_native_x64_mach_lower_ctx_t ctx = {.funcs = funcs,
                                        .rt_main = rt_main,
                                        .func_count = func_count,
                                        .mir = mir,
                                        .errors = lower_errors,
                                        .caps = target->caps};
  bool profile_object = getenv("NYTRIX_AOT_OBJECT_PROFILE") != NULL;
  ny_tick_t lower_start = profile_object ? ny_ticks_now() : 0;
  bool lowered = ny_parallel_for(func_count + 1, work,
                                 ny_native_x64_mach_lower_task, &ctx);
  if (profile_object)
    fprintf(stderr,
            "[aot-object] phase=machine-lower path=parallel functions=%zu "
            "nyir-insts=%zu elapsed=%.6fs\n",
            func_count + 1, work, ny_ticks_elapsed_sec(lower_start));
  if (!lowered)
    for (size_t i = 0; i <= func_count; ++i)
      if (lower_errors[i][0]) {
        if (err && err_len)
          snprintf(err, err_len, "%s", lower_errors[i]);
        break;
      }
  free(lower_errors);
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "",
           entry_symbol && entry_symbol[0] ? entry_symbol : "rt_main");
  if (!lowered) {
    for (size_t i = 0; i <= func_count; ++i)
      nyr_mach_func_free(&mir[i]);
    free(mir);
    return false;
  }

  bool ok = nyr_x64_mach_build_bundle_debug(
      &mir[func_count], mir, func_names, func_count, target,
      entry_symbol && entry_symbol[0] ? entry_symbol : "rt_main", tag_return,
      bare_syscalls, rt_main, funcs, code, defs, def_count, relocs, reloc_count,
      debug_lines, debug_line_count, debug_line_cap, err, err_len);
  if (ok) {
    for (size_t i = 0; i < func_count; ++i) {
      const char *name =
          func_names && func_names[i] ? func_names[i] : "unknown_fn";
      char symbol[256];
      snprintf(symbol, sizeof(symbol), NY_FMT_FN,
               target->symbol_prefix ? target->symbol_prefix : "", name);
      if (nyr_x64_obj_def_index(defs, *def_count, symbol) < 0) {
        if (!ny_native_x64_append_bundle_nir_fallback(
                &funcs[i], symbol, name, target, bare_syscalls, code, defs,
                def_count, relocs, reloc_count, debug_lines, debug_line_count,
                debug_line_cap, err, err_len)) {
          ok = false;
          break;
        }
      } else {
        ny_native_mach_record(true, symbol, NULL);
      }
    }
    if (ok)
      ny_native_mach_record(true, entry, NULL);
  }
  if (ok) {
    ny_tick_t tables_start = profile_object ? ny_ticks_now() : 0;
    ok = nyr_strtab_append_defs(code, defs, def_count, err, err_len) &&
         nyr_consttab_append_defs(code, defs, def_count, err, err_len) &&
         nyr_globaltab_append_defs(code, defs, def_count, err, err_len) &&
         nyr_arraytab_append_defs(code, defs, def_count, err, err_len);
    if (profile_object)
      fprintf(stderr, "[aot-object] phase=bundle-tables elapsed=%.6fs\n",
              ny_ticks_elapsed_sec(tables_start));
  }
  for (size_t i = 0; i <= func_count; ++i)
    nyr_mach_func_free(&mir[i]);
  free(mir);
  if (!ok) {
    if (profile_object)
      fprintf(stderr,
              "[aot-object] phase=machine-bundle path=serial-fallback "
              "functions=%zu reason=%s\n",
              func_count, err && err[0] ? err : "unspecified failure");
    code->len = 0;
    *def_count = 0;
    *reloc_count = 0;
    if (debug_lines) {
      free(*debug_lines);
      *debug_lines = NULL;
    }
    if (debug_line_count)
      *debug_line_count = 0;
    if (debug_line_cap)
      *debug_line_cap = 0;
    if (err && err_len)
      err[0] = '\0';
    return false;
  }

  return true;
}

static bool ny_native_x64_build_mixed_bundle_debug(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool bare_syscalls, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len) {
  if (!rt_main || !target || !entry_symbol || !entry_symbol[0] || !code ||
      !defs || !def_count || !relocs || !reloc_count || (func_count && !funcs))
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
  if (ny_native_x64_try_parallel_bundle_debug(
          rt_main, funcs, func_names, func_count, target, entry_symbol,
          tag_return, bare_syscalls, code, defs, def_count, relocs, reloc_count,
          debug_lines, debug_line_count, debug_line_cap, err, err_len))
    return true;
  for (size_t i = 0; i < func_count; ++i) {
    const char *name =
        func_names && func_names[i] ? func_names[i] : "unknown_fn";
    ny_dbg_lower_fn(name);
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (nyr_x64_obj_def_index(defs, *def_count, symbol) >= 0)
      continue;
    char reason[256] = {0};
    nyr_mach_func_t mach = {0};
    size_t *inst_offsets = NULL;
    size_t inst_offsets_len = 0;
    bool win_aggregate = target->abi == NY_NATIVE_ABI_WIN64 &&
                         (funcs[i].return_aggregate_size > 0);
    for (size_t pi = 0; !win_aggregate && pi < funcs[i].param_count; ++pi)
      win_aggregate = funcs[i].param_types &&
                      funcs[i].param_types[pi] == NYIR_PARAM_AGGREGATE;
    for (size_t ii = 0; !win_aggregate && ii < funcs[i].len; ++ii) {
      const nyr_inst_t *in = &funcs[i].data[ii];
      if (in->op != NYIR_CALL || !in->arg_sizes)
        continue;
      int argc = (int)in->imm;
      for (int ai = 0; argc >= 0 && ai < argc && ai < NYIR_CALL_MAX_ARGS; ++ai)
        win_aggregate |= in->arg_sizes[ai] > 0;
    }
    bool machine =
        !win_aggregate && nyr_mach_lower_nir(&funcs[i], &mach, target->caps,
                                             reason, sizeof(reason));
    if (win_aggregate)
      snprintf(reason, sizeof(reason),
               "Win64 aggregate uses metadata-aware encoder");
    if (machine)
      machine = nyr_x64_mach_append_function_debug(
          code, defs, def_count, relocs, reloc_count, &mach, target, symbol,
          false, bare_syscalls, funcs ? &funcs[i] : NULL, debug_lines,
          debug_line_count, debug_line_cap, reason, sizeof(reason),
          &inst_offsets, &inst_offsets_len);
    if (machine) {
      int mdef = nyr_x64_obj_def_index(defs, *def_count, symbol);
      if (mdef >= 0)
        nyr_mach_dump_mixed(&funcs[i], &mach, code->data + defs[mdef].off,
                            defs[mdef].size, inst_offsets, inst_offsets_len,
                            name, stderr);
    }
    nyr_mach_func_free(&mach);
    free(inst_offsets);
    inst_offsets = NULL;
    inst_offsets_len = 0;
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
    nyr_phi_elim((nyr_func_t *)&funcs[i]);
    if (nyr_x64_obj_append_function_debug(
            code, defs, def_count, relocs, reloc_count, &funcs[i], target,
            symbol, false, bare_syscalls, &funcs[i], debug_lines,
            debug_line_count, debug_line_cap, fallback_err,
            sizeof(fallback_err), &inst_offsets, &inst_offsets_len)) {
      int fdef = nyr_x64_obj_def_index(defs, *def_count, symbol);
      if (fdef >= 0) {
        nyr_mach_set_mixed_note(reason);
        nyr_mach_dump_mixed(&funcs[i], NULL, code->data + defs[fdef].off,
                            defs[fdef].size, inst_offsets, inst_offsets_len,
                            name, stderr);
        free(inst_offsets);
        inst_offsets = NULL;
        inst_offsets_len = 0;
      }
    } else {
      if (getenv("NY_DUMP_MACH"))
        fprintf(stderr, "native bundle fallback failed %s: machine=%s nir=%s\n",
                symbol, reason[0] ? reason : "unsupported",
                fallback_err[0] ? fallback_err : "unknown");
      nyr_set_error(
          err, err_len,
          "native function %s: machine form failed: %s; NYIR fallback "
          "failed: %s",
          symbol, reason[0] ? reason : "unsupported machine shape",
          fallback_err[0] ? fallback_err : "object emission failed");
      free(inst_offsets);
      return false;
    }
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  char reason[256] = {0};
  nyr_mach_func_t top_mach = {0};
  bool win_aggregate =
      target->abi == NY_NATIVE_ABI_WIN64 && rt_main->return_aggregate_size > 0;
  for (size_t pi = 0; !win_aggregate && pi < rt_main->param_count; ++pi)
    win_aggregate = rt_main->param_types &&
                    rt_main->param_types[pi] == NYIR_PARAM_AGGREGATE;
  for (size_t ii = 0; !win_aggregate && ii < rt_main->len; ++ii) {
    const nyr_inst_t *in = &rt_main->data[ii];
    if (in->op != NYIR_CALL || !in->arg_sizes)
      continue;
    int argc = (int)in->imm;
    for (int ai = 0; argc >= 0 && ai < argc && ai < NYIR_CALL_MAX_ARGS; ++ai)
      win_aggregate |= in->arg_sizes[ai] > 0;
  }
  bool machine =
      !win_aggregate && nyr_mach_lower_nir(rt_main, &top_mach, target->caps,
                                           reason, sizeof(reason));
  if (win_aggregate)
    snprintf(reason, sizeof(reason),
             "Win64 aggregate uses metadata-aware encoder");
  size_t *inst_offsets = NULL;
  size_t inst_offsets_len = 0;
  if (machine)
    machine = nyr_x64_mach_append_function_debug(
        code, defs, def_count, relocs, reloc_count, &top_mach, target, entry,
        tag_return, bare_syscalls, rt_main, debug_lines, debug_line_count,
        debug_line_cap, reason, sizeof(reason), &inst_offsets,
        &inst_offsets_len);
  if (machine) {
    int mdef = nyr_x64_obj_def_index(defs, *def_count, entry);
    if (mdef >= 0)
      nyr_mach_dump_mixed(rt_main, &top_mach, code->data + defs[mdef].off,
                          defs[mdef].size, inst_offsets, inst_offsets_len,
                          entry_symbol, stderr);
  }
  nyr_mach_func_free(&top_mach);
  free(inst_offsets);
  inst_offsets = NULL;
  inst_offsets_len = 0;
  if (machine) {
    ny_native_mach_record(true, entry, NULL);
  } else {
    if (getenv("NY_DUMP_MACH"))
      fprintf(stderr, "ENTRY-FALLBACK %s reason=%s\n", entry, reason);
    ny_native_mach_record(false, entry, reason);
    char fallback_err[256] = {0};
    nyr_phi_elim((nyr_func_t *)rt_main);
    if (nyr_x64_obj_append_function_debug(
            code, defs, def_count, relocs, reloc_count, rt_main, target, entry,
            tag_return, bare_syscalls, rt_main, debug_lines, debug_line_count,
            debug_line_cap, fallback_err, sizeof(fallback_err), &inst_offsets,
            &inst_offsets_len)) {
      int edef = nyr_x64_obj_def_index(defs, *def_count, entry);
      if (edef >= 0) {
        nyr_mach_dump_mixed(rt_main, NULL, code->data + defs[edef].off,
                            defs[edef].size, inst_offsets, inst_offsets_len,
                            entry_symbol, stderr);
        free(inst_offsets);
        inst_offsets = NULL;
        inst_offsets_len = 0;
      }
    } else {
      free(inst_offsets);
      nyr_set_error(err, err_len,
                    "native function %s: machine form failed: %s; NYIR "
                    "fallback failed: %s",
                    entry, reason[0] ? reason : "unsupported machine shape",
                    fallback_err[0] ? fallback_err : "object emission failed");
      return false;
    }
  }
  if (!nyr_strtab_append_defs(code, defs, def_count, err, err_len))
    return false;
  if (!nyr_consttab_append_defs(code, defs, def_count, err, err_len))
    return false;
  if (!nyr_globaltab_append_defs(code, defs, def_count, err, err_len))
    return false;
  return nyr_arraytab_append_defs(code, defs, def_count, err, err_len);
}

bool nyr_native_x64_build_mixed_bundle(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs,
    size_t *def_count, nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len) {
  return ny_native_x64_build_mixed_bundle_debug(
      rt_main, funcs, func_names, func_count, target, entry_symbol, tag_return,
      false, code, defs, def_count, relocs, reloc_count, NULL, NULL, NULL, err,
      err_len);
}

bool nyr_native_emit_elf64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, bool bare_syscalls, char *err,
    size_t err_len) {
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  nyr_x64_obj_reloc_t *relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0;
  size_t reloc_count = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    nyr_set_error(err, err_len, "x86-64 ELF object pool allocation failed");
    return false;
  }
  nyr_native_debug_line_t *debug_lines = NULL;
  size_t debug_line_count = 0;
  size_t debug_line_cap = 0;
  bool ok = ny_native_x64_build_mixed_bundle_debug(
      rt_main, funcs, func_names, func_count, target,
      entry_symbol ? entry_symbol : "rt_main", tag_return, bare_syscalls, &code,
      defs, &def_count, relocs, &reloc_count, &debug_lines, &debug_line_count,
      &debug_line_cap, err, err_len);
  if (ok && target && target->target == NY_NATIVE_TARGET_X86_64) {
    if (!debug_lines) {
      debug_line_cap = func_count + 1;
      if (debug_line_cap < 32)
        debug_line_cap = 32;
      debug_lines = calloc(debug_line_cap, sizeof(*debug_lines));
      if (!debug_lines) {
        nyr_set_error(err, err_len, "x86-64 source map allocation failed");
        ok = false;
      }
    }
    if (ok) {
      for (size_t i = 0; i < func_count; ++i) {
        const char *name =
            func_names && func_names[i] ? func_names[i] : "unknown_fn";
        char symbol[256];
        snprintf(symbol, sizeof(symbol), NY_FMT_FN,
                 target->symbol_prefix ? target->symbol_prefix : "", name);
        int def_i = nyr_x64_obj_def_index(defs, def_count, symbol);
        if (def_i < 0 || defs[def_i].is_data)
          continue;
        for (size_t j = 0; funcs && j < funcs[i].len; ++j) {
          const nyr_debug_loc_t *loc = &funcs[i].data[j].debug;
          if (!loc->line)
            continue;
          if (!nyr_native_debug_line_append(
                  &debug_lines, &debug_line_count, &debug_line_cap,
                  (nyr_native_debug_line_t){(size_t)def_i, defs[def_i].off,
                                            loc->file, loc->line,
                                            loc->column})) {
            nyr_set_error(err, err_len, "x86-64 source map allocation failed");
            ok = false;
          }
          break;
        }
      }
      const char *entry =
          entry_symbol && entry_symbol[0] ? entry_symbol : "rt_main";
      char entry_name[256];
      snprintf(entry_name, sizeof(entry_name), "%s%s",
               target->symbol_prefix ? target->symbol_prefix : "", entry);
      int entry_i = nyr_x64_obj_def_index(defs, def_count, entry_name);
      if (entry_i >= 0 && !defs[entry_i].is_data)
        for (size_t j = 0; j < rt_main->len; ++j) {
          const nyr_debug_loc_t *loc = &rt_main->data[j].debug;
          if (!loc->line)
            continue;
          if (!nyr_native_debug_line_append(
                  &debug_lines, &debug_line_count, &debug_line_cap,
                  (nyr_native_debug_line_t){(size_t)entry_i, defs[entry_i].off,
                                            loc->file, loc->line,
                                            loc->column})) {
            nyr_set_error(err, err_len, "x86-64 source map allocation failed");
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
  nyr_obj_free(&code);
  free(defs);
  free(relocs);
  return ok;
}

typedef bool (*ny_x64_bundle_writer_fn)(
    const unsigned char *code, size_t code_len,
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count, const char *path,
    const nyr_native_debug_line_t *debug_lines, size_t debug_line_count,
    char *err, size_t err_len);
typedef bool (*ny_x64_single_writer_fn)(const unsigned char *code,
                                        size_t code_len,
                                        const nyr_x64_obj_reloc_t *relocs,
                                        size_t reloc_count, const char *path,
                                        const char *symbol_name, char *err,
                                        size_t err_len);

static bool nyr_native_append_bundle_source_map(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count,
    nyr_native_debug_line_t **lines, size_t *line_count, size_t *line_cap,
    char *err, size_t err_len) {
  if (!target || !defs || !lines || !line_count || !line_cap)
    return false;
  if (!*lines) {
    *line_cap = func_count + 1;
    if (*line_cap < 32)
      *line_cap = 32;
    *lines = calloc(*line_cap, sizeof(**lines));
    if (!*lines) {
      nyr_set_error(err, err_len, "native source map allocation failed");
      return false;
    }
  }
  for (size_t i = 0; i < func_count; ++i) {
    const char *name =
        func_names && func_names[i] ? func_names[i] : "unknown_fn";
    ny_dbg_lower_fn(name);
    ny_dbg_scope_stdlib(strchr(name, '.') != NULL);
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    int def_i = nyr_x64_obj_def_index(defs, def_count, symbol);
    if (def_i < 0 || defs[def_i].is_data)
      continue;
    for (size_t j = 0; funcs && j < funcs[i].len; ++j) {
      const nyr_debug_loc_t *loc = &funcs[i].data[j].debug;
      if (!loc->line)
        continue;
      if (!nyr_native_debug_line_append(
              lines, line_count, line_cap,
              (nyr_native_debug_line_t){(size_t)def_i, defs[def_i].off,
                                        loc->file, loc->line, loc->column})) {
        nyr_set_error(err, err_len, "native source map allocation failed");
        return false;
      }
      break;
    }
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "",
           entry_symbol && entry_symbol[0] ? entry_symbol : "rt_main");
  int entry_i = nyr_x64_obj_def_index(defs, def_count, entry);
  if (entry_i >= 0 && !defs[entry_i].is_data && rt_main) {
    for (size_t j = 0; j < rt_main->len; ++j) {
      const nyr_debug_loc_t *loc = &rt_main->data[j].debug;
      if (!loc->line)
        continue;
      if (!nyr_native_debug_line_append(
              lines, line_count, line_cap,
              (nyr_native_debug_line_t){(size_t)entry_i, defs[entry_i].off,
                                        loc->file, loc->line, loc->column})) {
        nyr_set_error(err, err_len, "native source map allocation failed");
        return false;
      }
      break;
    }
  }
  return true;
}
static bool nyr_native_emit_x64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len,
    ny_x64_bundle_writer_fn write_bundle) {
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  nyr_x64_obj_reloc_t *relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0, reloc_count = 0;
  nyr_native_debug_line_t *debug_lines = NULL;
  size_t debug_line_count = 0, debug_line_cap = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    nyr_set_error(err, err_len, "x86-64 object pool allocation failed");
    return false;
  }
  bool built = ny_native_x64_build_mixed_bundle_debug(
      rt_main, funcs, func_names, func_count, target, entry_symbol, tag_return,
      false, &code, defs, &def_count, relocs, &reloc_count, &debug_lines,
      &debug_line_count, &debug_line_cap, err, err_len);
  if (built && target && target->target == NY_NATIVE_TARGET_X86_64 &&
      !nyr_native_append_bundle_source_map(
          rt_main, funcs, func_names, func_count, target, entry_symbol, defs,
          def_count, &debug_lines, &debug_line_count, &debug_line_cap, err,
          err_len))
    built = false;
  bool ok = built && write_bundle(code.data, code.len, relocs, reloc_count,
                                  defs, def_count, path, debug_lines,
                                  debug_line_count, err, err_len);
  free(debug_lines);
  nyr_obj_free(&code);
  free(defs);
  free(relocs);
  return ok;
}

static bool ny_native_emit_x64_object_from_nir(
    const nyr_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len, const char *missing_msg, ny_x64_single_writer_fn write) {
  if (!nyir || !path || !symbol_name || !symbol_name[0]) {
    nyr_set_error(err, err_len, "%s", missing_msg);
    return false;
  }
  nyr_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!nyr_x64_obj_emit_code(&ctx, nyir, tag_return)) {
    nyr_x64_obj_ctx_free(&ctx);
    return false;
  }
  bool ok = write(ctx.code.data, ctx.code.len, ctx.relocs, ctx.reloc_count,
                  path, symbol_name, err, err_len);
  nyr_x64_obj_ctx_free(&ctx);
  return ok;
}

bool nyr_native_emit_coff_x64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  return nyr_native_emit_x64_object_from_nirs(
      rt_main, funcs, func_names, func_count, target, path, entry_symbol,
      tag_return, err, err_len, ny_native_emit_coff_x64_object_bundle_code);
}

bool nyr_native_emit_macho_x64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  return nyr_native_emit_x64_object_from_nirs(
      rt_main, funcs, func_names, func_count, target, path, entry_symbol,
      tag_return, err, err_len, ny_native_emit_macho_x64_object_bundle_code);
}

bool nyr_native_emit_coff_x64_object_from_nir(
    const nyr_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len) {
  return ny_native_emit_x64_object_from_nir(
      nyir, target, path, symbol_name, tag_return, err, err_len,
      "x86-64 COFF object writer: missing input",
      ny_native_emit_coff_x64_object_code);
}

bool nyr_native_emit_macho_x64_object_from_nir(
    const nyr_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len) {
  return ny_native_emit_x64_object_from_nir(
      nyir, target, path, symbol_name, tag_return, err, err_len,
      "x86-64 Mach-O object writer: missing input",
      ny_native_emit_macho_x64_object_code);
}

bool nyr_native_emit_elf64_object_from_nir(
    const nyr_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len) {
  if (!nyir || !path || !symbol_name || !symbol_name[0]) {
    nyr_set_error(err, err_len, "x86-64 ELF object writer: missing input");
    return false;
  }
  nyr_x64_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!nyr_x64_obj_emit_code(&ctx, nyir, tag_return)) {
    nyr_x64_obj_ctx_free(&ctx);
    return false;
  }

  nyr_obj_buf_t file = {0};
  nyr_obj_buf_t strtab = {0};
  bool ok = false;
  /*
   * Allocate tables proportional to this function's relocation count.
   */
  size_t reloc_capacity = ctx.reloc_count ? ctx.reloc_count : 1;
  char (*reloc_symbols)[256] = calloc(reloc_capacity, sizeof(*reloc_symbols));
  size_t reloc_symbol_count = 0;
  uint32_t *reloc_name_offs = calloc(reloc_capacity, sizeof(*reloc_name_offs));
  if (!reloc_symbols || !reloc_name_offs) {
    nyr_set_error(err, err_len,
                  "x86-64 ELF relocation table allocation failed");
    goto done;
  }
  if (!nyr_x64_obj_collect_reloc_symbols(ctx.relocs, ctx.reloc_count,
                                         reloc_symbols, &reloc_symbol_count,
                                         err, err_len))
    goto done;

  const char shstr[] = "\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0";
  const uint32_t sh_text = 1;
  const uint32_t sh_rela_text = 7;
  const uint32_t sh_symtab = 18;
  const uint32_t sh_strtab = 26;
  const uint32_t sh_shstrtab = 34;
  if (!nyr_obj_u8(&strtab, 0))
    goto done;
  uint32_t sym_name_off = (uint32_t)strtab.len;
  if (!nyr_obj_emit(&strtab, symbol_name, strlen(symbol_name) + 1))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
      goto done;
  }

  if (!nyr_obj_zero(&file, 64) || !nyr_obj_pad_to(&file, 16))
    goto done;
  size_t text_off = file.len;
  if (!nyr_obj_emit(&file, ctx.code.data, ctx.code.len) ||
      !nyr_obj_pad_to(&file, 8))
    goto done;
  size_t rela_off = file.len;
  for (size_t i = 0; i < ctx.reloc_count; ++i) {
    int sym_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                         ctx.relocs[i].symbol);
    if (sym_i < 0)
      goto done;
    /*
     * R_X86_64_PC32 = 2, R_X86_64_PLT32 = 4
     */
    uint64_t rtype = (ctx.relocs[i].type == NY_RELOC_PC32) ? 2u : 4u;
    uint64_t info = ((uint64_t)(2 + sym_i) << 32) | rtype;
    if (!nyr_obj_u64(&file, ctx.relocs[i].disp_off) ||
        !nyr_obj_u64(&file, info) || !nyr_obj_u64(&file, (uint64_t)-4LL))
      goto done;
  }
  size_t rela_size = file.len - rela_off;
  size_t symtab_off = file.len;
  if (!nyr_obj_elf64_write_sym(&file, 0, 0, 0, 0, 0) ||
      !nyr_obj_elf64_write_sym(&file, sym_name_off, 0x12, 1, 0, ctx.code.len))
    goto done;
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!nyr_obj_elf64_write_sym(&file, reloc_name_offs[i], 0x12, 0, 0, 0))
      goto done;
  }
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!nyr_obj_emit(&file, strtab.data, strtab.len))
    goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstrtab_off = file.len;
  if (!nyr_obj_emit(&file, shstr, sizeof(shstr)) || !nyr_obj_pad_to(&file, 8))
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
  file.data[4] = 2;                 /* ELFCLASS64 */
  file.data[5] = 1;                 /* little-endian */
  file.data[6] = 1;                 /* EV_CURRENT */
  nyr_obj_patch_u16(&file, 16, 1);  /* ET_REL */
  nyr_obj_patch_u16(&file, 18, 62); /* EM_X86_64 */
  nyr_obj_patch_u32(&file, 20, 1);
  nyr_obj_patch_u64(&file, 40, shoff);
  nyr_obj_patch_u16(&file, 52, 64);
  nyr_obj_patch_u16(&file, 58, 64);
  nyr_obj_patch_u16(&file, 60, 6);
  nyr_obj_patch_u16(&file, 62, 5);

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(reloc_symbols);
  free(reloc_name_offs);
  nyr_obj_free(&strtab);
  nyr_obj_free(&file);
  nyr_x64_obj_ctx_free(&ctx);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "x86-64 ELF object writer failed");
  return ok;
}

bool nyr_native_emit_elf32_i386_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len) {
  if (!rt_main || !path || !entry_symbol || !entry_symbol[0]) {
    nyr_set_error(err, err_len, "i386 ELF object writer: missing input");
    return false;
  }
  if (!target) {
    nyr_set_error(err, err_len, "i386 ELF object writer: missing target");
    return false;
  }

  nyr_obj_buf_t code = {0};
  nyr_obj_buf_t file = {0};
  nyr_obj_buf_t strtab = {0};
  nyr_i386_obj_symbol_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  nyr_i386_obj_reloc_t *relocs = NULL;
  size_t def_count = 0;
  size_t reloc_count = 0;
  size_t reloc_cap = 0;
  char (*reloc_symbols)[256] = NULL;
  uint32_t *reloc_name_offs = NULL;
  bool ok = false;
  uint32_t *def_name_offs = NULL;
  if (!defs) {
    nyr_set_error(err, err_len,
                  "i386 ELF object writer: definition allocation failed");
    goto done;
  }
  /*
   * The NYIR object writer cannot encode PHIs.  Eliminate them before
   * reaching the encoder, mirroring the x86-64 and AArch64 paths.
   */
  nyr_phi_elim((nyr_func_t *)rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyr_phi_elim((nyr_func_t *)&funcs[i]);

  for (size_t i = 0; i < func_count; ++i) {
    if (def_count >= NY_NATIVE_MAX_DEFS) {
      nyr_set_error(err, err_len, "i386 ELF object writer: too many functions");
      goto done;
    }
    const char *name =
        func_names && func_names[i] ? func_names[i] : "unknown_fn";
    char symbol[256];
    snprintf(symbol, sizeof(symbol), NY_FMT_FN,
             target->symbol_prefix ? target->symbol_prefix : "", name);
    if (nyr_i386_obj_def_index(defs, def_count, symbol) >= 0) {
      nyr_set_error(err, err_len, "i386 ELF object writer: duplicate symbol %s",
                    symbol);
      goto done;
    }
    if (!nyr_obj_pad_to(&code, 16)) {
      nyr_set_error(err, err_len, "i386 ELF object writer: out of memory");
      goto done;
    }
    size_t base = code.len;
    nyr_i386_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
    if (!nyr_i386_obj_emit_code(&ctx, &funcs[i], false)) {
      nyr_i386_obj_ctx_free(&ctx);
      goto done;
    }
    if (!ny_i386_obj_reserve_relocs(
            &relocs, &reloc_cap, reloc_count + ctx.reloc_count, err, err_len)) {
      nyr_i386_obj_ctx_free(&ctx);
      goto done;
    }
    if (!nyr_obj_emit(&code, ctx.code.data, ctx.code.len)) {
      nyr_set_error(err, err_len, "i386 ELF object writer: out of memory");
      nyr_i386_obj_ctx_free(&ctx);
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
    nyr_i386_obj_ctx_free(&ctx);
  }

  if (def_count >= NY_NATIVE_MAX_DEFS) {
    nyr_set_error(err, err_len, "i386 ELF object writer: too many functions");
    goto done;
  }
  char entry[256];
  snprintf(entry, sizeof(entry), "%s%s",
           target->symbol_prefix ? target->symbol_prefix : "", entry_symbol);
  if (nyr_i386_obj_def_index(defs, def_count, entry) >= 0) {
    nyr_set_error(err, err_len, "i386 ELF object writer: duplicate symbol %s",
                  entry);
    goto done;
  }
  if (!nyr_obj_pad_to(&code, 16)) {
    nyr_set_error(err, err_len, "i386 ELF object writer: out of memory");
    goto done;
  }
  size_t entry_base = code.len;
  nyr_i386_obj_ctx_t ctx = {.target = target, .err = err, .err_len = err_len};
  if (!nyr_i386_obj_emit_code(&ctx, rt_main, tag_return)) {
    nyr_i386_obj_ctx_free(&ctx);
    goto done;
  }
  if (!ny_i386_obj_reserve_relocs(
          &relocs, &reloc_cap, reloc_count + ctx.reloc_count, err, err_len)) {
    nyr_i386_obj_ctx_free(&ctx);
    goto done;
  }
  if (!nyr_obj_emit(&code, ctx.code.data, ctx.code.len)) {
    nyr_set_error(err, err_len, "i386 ELF object writer: out of memory");
    nyr_i386_obj_ctx_free(&ctx);
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
  nyr_i386_obj_ctx_free(&ctx);

  size_t reloc_symbol_count = 0;
  def_name_offs = calloc(def_count, sizeof(*def_name_offs));
  if (!def_name_offs) {
    nyr_set_error(err, err_len,
                  "i386 ELF object writer: definition allocation failed");
    goto done;
  }
  if (reloc_count > 0) {
    reloc_symbols = calloc(reloc_count, sizeof(*reloc_symbols));
    reloc_name_offs = calloc(reloc_count, sizeof(*reloc_name_offs));
    if (!reloc_symbols || !reloc_name_offs) {
      nyr_set_error(
          err, err_len,
          "i386 ELF object writer: relocation symbol allocation failed");
      goto done;
    }
  }
  if (!nyr_i386_obj_collect_external_reloc_symbols(
          relocs, reloc_count, defs, def_count, reloc_symbols,
          &reloc_symbol_count, err, err_len))
    goto done;

  const char shstr[] = "\0.text\0.rel.text\0.symtab\0.strtab\0.shstrtab\0";
  const uint32_t sh_text = 1;
  const uint32_t sh_rel_text = 7;
  const uint32_t sh_symtab = 17;
  const uint32_t sh_strtab = 25;
  const uint32_t sh_shstrtab = 33;
  if (!nyr_obj_u8(&strtab, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    def_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, defs[i].name, strlen(defs[i].name) + 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    reloc_name_offs[i] = (uint32_t)strtab.len;
    if (!nyr_obj_emit(&strtab, reloc_symbols[i], strlen(reloc_symbols[i]) + 1))
      goto done;
  }

  if (!nyr_obj_zero(&file, 52) || !nyr_obj_pad_to(&file, 16))
    goto done;
  size_t text_off = file.len;
  if (!nyr_obj_emit(&file, code.data, code.len) || !nyr_obj_pad_to(&file, 4))
    goto done;
  size_t rel_off = file.len;
  for (size_t i = 0; i < reloc_count; ++i) {
    int def_i = nyr_i386_obj_def_index(defs, def_count, relocs[i].symbol);
    uint32_t sym_index = 0;
    if (def_i >= 0) {
      sym_index = (uint32_t)(1 + def_i);
    } else {
      int ext_i = nyr_x86_obj_symbol_index(reloc_symbols, reloc_symbol_count,
                                           relocs[i].symbol);
      if (ext_i < 0)
        goto done;
      sym_index = (uint32_t)(1 + def_count + (size_t)ext_i);
    }
    uint32_t info = (sym_index << 8) | 2u; /* R_386_PC32 */
    if (!nyr_obj_u32(&file, (uint32_t)relocs[i].disp_off) ||
        !nyr_obj_u32(&file, info))
      goto done;
  }
  size_t rel_size = file.len - rel_off;
  size_t symtab_off = file.len;
  if (!nyr_elf32_write_sym(&file, 0, 0, 0, 0, 0))
    goto done;
  for (size_t i = 0; i < def_count; ++i) {
    if (!nyr_elf32_write_sym(&file, def_name_offs[i], (uint32_t)defs[i].off,
                             (uint32_t)defs[i].size, 0x12, 1))
      goto done;
  }
  for (size_t i = 0; i < reloc_symbol_count; ++i) {
    if (!nyr_elf32_write_sym(&file, reloc_name_offs[i], 0, 0, 0x12, 0))
      goto done;
  }
  size_t symtab_size = file.len - symtab_off;
  size_t strtab_off = file.len;
  if (!nyr_obj_emit(&file, strtab.data, strtab.len))
    goto done;
  size_t strtab_size = file.len - strtab_off;
  size_t shstrtab_off = file.len;
  if (!nyr_obj_emit(&file, shstr, sizeof(shstr)) || !nyr_obj_pad_to(&file, 4))
    goto done;
  size_t shstrtab_size = sizeof(shstr);
  size_t shoff = file.len;
  if (!nyr_elf32_write_sh(&file, 0, 0, 0, 0, 0, 0, 0, 0, 0) ||
      !nyr_elf32_write_sh(&file, sh_text, 1, 0x6, (uint32_t)text_off,
                          (uint32_t)code.len, 0, 0, 16, 0) ||
      !nyr_elf32_write_sh(&file, sh_rel_text, 9, 0, (uint32_t)rel_off,
                          (uint32_t)rel_size, 3, 1, 4, 8) ||
      !nyr_elf32_write_sh(&file, sh_symtab, 2, 0, (uint32_t)symtab_off,
                          (uint32_t)symtab_size, 4, 1, 4, 16) ||
      !nyr_elf32_write_sh(&file, sh_strtab, 3, 0, (uint32_t)strtab_off,
                          (uint32_t)strtab_size, 0, 0, 1, 0) ||
      !nyr_elf32_write_sh(&file, sh_shstrtab, 3, 0, (uint32_t)shstrtab_off,
                          (uint32_t)shstrtab_size, 0, 0, 1, 0))
    goto done;

  file.data[0] = 0x7f;
  file.data[1] = 'E';
  file.data[2] = 'L';
  file.data[3] = 'F';
  file.data[4] = 1;                /* ELFCLASS32 */
  file.data[5] = 1;                /* little-endian */
  file.data[6] = 1;                /* EV_CURRENT */
  nyr_obj_patch_u16(&file, 16, 1); /* ET_REL */
  nyr_obj_patch_u16(&file, 18, 3); /* EM_386 */
  nyr_obj_patch_u32(&file, 20, 1);
  nyr_obj_patch_u32(&file, 32, (uint32_t)shoff);
  nyr_obj_patch_u16(&file, 40, 52);
  nyr_obj_patch_u16(&file, 46, 40);
  nyr_obj_patch_u16(&file, 48, 6);
  nyr_obj_patch_u16(&file, 50, 5); /* .shstrtab section index */

  ok = ny_elf64_write_file(path, file.data, file.len, err, err_len);

done:
  free(defs);
  free(def_name_offs);
  free(reloc_name_offs);
  free(reloc_symbols);
  free(relocs);
  nyr_obj_free(&strtab);
  nyr_obj_free(&file);
  nyr_obj_free(&code);
  if (!ok && err && err_len > 0 && err[0] == '\0')
    nyr_set_error(err, err_len, "i386 ELF object writer failed");
  return ok;
}
