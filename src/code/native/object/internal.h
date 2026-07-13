#ifndef NY_NATIVE_OBJECT_INTERNAL_H
#define NY_NATIVE_OBJECT_INTERNAL_H

#include "code/native/internal.h"

#include <stdint.h>

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} ny_obj_buf_t;

#define NY_RELOC_PC32 1
#define NY_RELOC_PLT32 2
#define NY_RELOC_AARCH64_CALL26 3

typedef struct { int64_t label; size_t off; } ny_x64_obj_label_t;
typedef struct { int64_t label; size_t disp_off; } ny_x64_obj_patch_t;
typedef struct { char symbol[256]; size_t disp_off; int type; } ny_x64_obj_reloc_t;
typedef struct { char name[256]; size_t off; size_t size; } ny_x64_obj_symbol_def_t;
typedef struct { const ny_nir_inst_t **defs; int count; } ny_x64_obj_valmap_t;

typedef struct {
  ny_obj_buf_t code;
  const ny_native_target_info_t *target;
  const ny_nir_func_t *nir;
  ny_x64_obj_valmap_t valmap;
  int value_slots, spill_slots, callee_save_slots, local_slots, frame_bytes;
  bool *value_f64, *value_f32, *local_f64, *local_f32;
  bool *value_immediate;
  int8_t *value_reg, *value_xmm;
  int *value_spill;
  int callee_save_slot[16];
  ny_x64_obj_label_t labels[256]; size_t label_count;
  ny_x64_obj_patch_t patches[256]; size_t patch_count;
  ny_x64_obj_reloc_t relocs[256]; size_t reloc_count;
  char *err; size_t err_len;
} ny_x64_obj_ctx_t;

typedef struct { int64_t label; size_t off; } ny_i386_obj_label_t;
typedef struct { int64_t label; size_t disp_off; } ny_i386_obj_patch_t;
typedef struct { char symbol[256]; size_t disp_off; } ny_i386_obj_reloc_t;
typedef struct { char name[256]; size_t off; size_t size; } ny_i386_obj_symbol_def_t;

typedef struct {
  ny_obj_buf_t code;
  const ny_native_target_info_t *target;
  int value_slots, local_slots, frame_bytes, local_base, max_local_slot;
  bool *value_f64, *value_f32, *local_f64, *local_f32;
  ny_i386_obj_label_t labels[256]; size_t label_count;
  ny_i386_obj_patch_t patches[256]; size_t patch_count;
  size_t epilogue_patches[256]; size_t epilogue_patch_count;
  ny_i386_obj_reloc_t relocs[256]; size_t reloc_count;
  char *err; size_t err_len;
} ny_i386_obj_ctx_t;

void ny_obj_free(ny_obj_buf_t *b);
bool ny_obj_emit(ny_obj_buf_t *b, const void *data, size_t len);
bool ny_obj_pad_to(ny_obj_buf_t *b, size_t align);
bool ny_obj_zero(ny_obj_buf_t *b, size_t len);
bool ny_obj_u8(ny_obj_buf_t *b, unsigned v);
bool ny_obj_u16(ny_obj_buf_t *b, uint16_t v);
bool ny_obj_u32(ny_obj_buf_t *b, uint32_t v);
bool ny_obj_u64(ny_obj_buf_t *b, uint64_t v);
void ny_obj_patch_u16(ny_obj_buf_t *b, size_t off, uint16_t v);
void ny_obj_patch_u32(ny_obj_buf_t *b, size_t off, uint32_t v);
void ny_obj_patch_u64(ny_obj_buf_t *b, size_t off, uint64_t v);
bool ny_elf32_write_sym(ny_obj_buf_t *b, uint32_t name, uint32_t value,
                        uint32_t size, unsigned char info, uint16_t shndx);
bool ny_elf32_write_sh(ny_obj_buf_t *b, uint32_t name, uint32_t type,
                       uint32_t flags, uint32_t off, uint32_t size,
                       uint32_t link, uint32_t info, uint32_t addralign,
                       uint32_t entsize);

void ny_x64_obj_ctx_free(ny_x64_obj_ctx_t *c);
bool ny_x64_obj_emit_code(ny_x64_obj_ctx_t *c, const ny_nir_func_t *nir,
                          bool tag_return);
int ny_x64_obj_symbol_index(char symbols[][256], size_t count,
                            const char *name);
int ny_x64_obj_def_index(const ny_x64_obj_symbol_def_t *defs, size_t count,
                         const char *name);
bool ny_x64_obj_collect_external_reloc_symbols(
    const ny_x64_obj_reloc_t *relocs, size_t reloc_count,
    const ny_x64_obj_symbol_def_t *defs, size_t def_count,
    char symbols[][256], size_t *symbol_count, char *err, size_t err_len);
bool ny_x64_obj_collect_reloc_symbols(const ny_x64_obj_reloc_t *relocs,
                                      size_t reloc_count,
                                      char symbols[][256],
                                      size_t *symbol_count, char *err,
                                      size_t err_len);
bool ny_x64_obj_append_function(
    ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs, size_t *def_count,
    ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const ny_nir_func_t *nir, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, char *err, size_t err_len);
bool ny_x64_obj_build_bundle(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len);
bool ny_a64_obj_build_bundle(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len);

void ny_i386_obj_ctx_free(ny_i386_obj_ctx_t *c);
bool ny_i386_obj_emit_code(ny_i386_obj_ctx_t *c, const ny_nir_func_t *nir,
                           bool tag_return);
int ny_i386_obj_symbol_index(char symbols[][256], size_t count,
                             const char *name);
int ny_i386_obj_def_index(const ny_i386_obj_symbol_def_t *defs, size_t count,
                          const char *name);
bool ny_i386_obj_collect_external_reloc_symbols(
    const ny_i386_obj_reloc_t *relocs, size_t reloc_count,
    const ny_i386_obj_symbol_def_t *defs, size_t def_count,
    char symbols[][256], size_t *symbol_count, char *err, size_t err_len);

#endif
