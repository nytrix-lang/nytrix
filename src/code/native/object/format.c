#include "code/native/object/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ELF32/ELF64, COFF, and Mach-O packaging over encoded code, symbols, and
 * relocation records produced by the architecture encoders. */

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
    /* R_X86_64_PC32 = 2, R_X86_64_PLT32 = 4 */
    uint64_t rtype = (relocs[i].type == NY_RELOC_PC32) ? 2u : 4u;
    uint64_t info = ((uint64_t)sym_index << 32) | rtype;
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

bool ny_x64_obj_build_bundle(
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
    /* R_X86_64_PC32 = 2, R_X86_64_PLT32 = 4 */
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
