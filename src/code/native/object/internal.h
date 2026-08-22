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

/* A single straight-line Nytrix function can legitimately contain hundreds
 * of calls. Keep the program-level relocation transport larger than the
 * function-count limit so x86-64 JIT/object emission does not reject such a
 * program before the encoder or linker sees it. */
#define NY_X64_OBJ_MAX_RELOCS NY_NATIVE_MAX_RELOCS
#define NY_NATIVE_MAX_DEFS NY_NATIVE_MAX_SYMBOLS

void ny_native_mach_encode_stats(unsigned long long *mach_ok,
                                unsigned long long *nir_fallback);
void ny_native_mach_encode_fallback_detail(char *out, size_t out_len);
void ny_native_mach_regalloc_record(size_t segments, size_t colored,
                                    size_t spilled, size_t reloads,
                                    size_t peak_live);
void ny_native_mach_regalloc_stats(unsigned long long *segments,
                                   unsigned long long *colored,
                                   unsigned long long *spilled,
                                   unsigned long long *reloads,
                                   unsigned long long *peak_live);
void ny_native_mach_fpr_record(size_t segments, size_t colored, size_t spilled,
                               size_t reloads, size_t peak_live);
void ny_native_mach_fpr_stats(unsigned long long *segments,
                              unsigned long long *colored,
                              unsigned long long *spilled,
                              unsigned long long *reloads,
                              unsigned long long *peak_live);
void ny_native_mach_vector_record(size_t segments, size_t colored,
                                  size_t spilled, size_t reloads,
                                  size_t peak_live);
void ny_native_mach_vector_stats(unsigned long long *segments,
                                 unsigned long long *colored,
                                 unsigned long long *spilled,
                                 unsigned long long *reloads,
                                 unsigned long long *peak_live);
typedef struct {
  size_t segments;
  size_t colored;
  size_t spilled;
  size_t reloads;
  size_t peak_live;
  /* Subset whose machine block maps to a profile-hot NYIR loop region. */
  size_t hot_loop_segments;
  size_t hot_loop_spilled;
  size_t hot_loop_reloads;
  size_t hot_loop_peak_live;
} ny_native_regalloc_metrics_t;

bool ny_native_x64_regalloc_metrics(const ny_mach_func_t *mach,
                                    ny_native_regalloc_metrics_t *gpr,
                                    ny_native_regalloc_metrics_t *fpr,
                                    ny_native_regalloc_metrics_t *vector);
bool ny_native_a64_regalloc_metrics(const ny_mach_func_t *mach,
                                    ny_native_regalloc_metrics_t *gpr,
                                    ny_native_regalloc_metrics_t *fpr,
                                    ny_native_regalloc_metrics_t *vector);
bool ny_mach_regalloc_build_class(const ny_mach_func_t *mach,
                                  ny_mach_reg_class_t reg_class,
                                  size_t color_count,
                                  ny_mach_regalloc_t *out);

typedef struct { int64_t label; size_t off; } ny_x64_obj_label_t;
typedef struct { int64_t label; size_t disp_off; } ny_x64_obj_patch_t;
typedef struct { char symbol[256]; size_t disp_off; int type; } ny_x64_obj_reloc_t;
typedef struct { char name[256]; size_t off; size_t size; } ny_x64_obj_symbol_def_t;
typedef struct { const nyir_inst_t **defs; int count; } ny_x64_obj_valmap_t;

/* Append interned pure-native C strings into the code blob as local defs. */
bool ny_native_strtab_append_defs(ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
                                  size_t *def_count, char *err, size_t err_len);

/* Append foldable top-level def constants into the code blob as 8-byte
 * .data definitions (conway's HALF_W, CELL_ALIVE, ...). */
bool ny_native_consttab_append_defs(ny_obj_buf_t *code,
                                    ny_x64_obj_symbol_def_t *defs,
                                    size_t *def_count, char *err,
                                    size_t err_len);

/* Append interned constant arrays (list literals) into the code blob as
 * count * 8 bytes of .data definitions. */
bool ny_native_arraytab_append_defs(ny_obj_buf_t *code,
                                    ny_x64_obj_symbol_def_t *defs,
                                    size_t *def_count, char *err,
                                    size_t err_len);

typedef struct {
  ny_obj_buf_t code;
  const ny_native_target_info_t *target;
  const nyir_func_t *nyir;
  ny_x64_obj_valmap_t valmap;
  int value_slots, spill_slots, callee_save_slots, local_slots, frame_bytes;
  bool *value_f64, *value_f32, *local_f64, *local_f32;
  bool *value_immediate;
  int8_t *value_reg, *value_xmm;
  int *value_spill;
  int callee_save_slot[16];
  ny_x64_obj_label_t labels[1024]; size_t label_count;
  ny_x64_obj_patch_t patches[1024]; size_t patch_count;
  ny_x64_obj_reloc_t relocs[NY_X64_OBJ_MAX_RELOCS]; size_t reloc_count;
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
  ny_i386_obj_label_t labels[1024]; size_t label_count;
  ny_i386_obj_patch_t patches[1024]; size_t patch_count;
  size_t epilogue_patches[1024]; size_t epilogue_patch_count;
  ny_i386_obj_reloc_t relocs[NY_X64_OBJ_MAX_RELOCS]; size_t reloc_count;
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
bool ny_x64_obj_emit_code(ny_x64_obj_ctx_t *c, const nyir_func_t *nyir,
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
    const nyir_func_t *nyir, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, char *err, size_t err_len);
bool ny_x64_obj_build_bundle(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len);

/* Independence path: encode finalized machine form to bytes (no NYIR encoder). */
#include "code/native/ir/machine.h"
bool ny_x64_mach_build_bundle(
    const ny_mach_func_t *rt_main_mir, const ny_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len);
bool ny_x64_mach_append_function(
    ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs, size_t *def_count,
    ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const ny_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, char *err, size_t err_len);
bool ny_x64_try_stencil_bundle(const nyir_func_t *rt_main,
                               const ny_native_target_info_t *target,
                               ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
                               size_t *def_count, ny_x64_obj_reloc_t *relocs,
                               size_t *reloc_count, char *err, size_t err_len);
/* Same as above but folds pure direct calls into helper NYIR bodies (call
 * stencil): when every CALL target is a pure foldable helper, emit const/local
 * shell without emitting the call. */
bool ny_x64_try_stencil_bundle_calls(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, ny_obj_buf_t *code,
    ny_x64_obj_symbol_def_t *defs, size_t *def_count, ny_x64_obj_reloc_t *relocs,
    size_t *reloc_count, char *err, size_t err_len);
bool ny_a64_mach_build_bundle(
    const ny_mach_func_t *rt_main_mir, const ny_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len);
bool ny_a64_obj_build_bundle(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, ny_obj_buf_t *code, ny_x64_obj_symbol_def_t *defs,
    size_t *def_count, ny_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len);

void ny_i386_obj_ctx_free(ny_i386_obj_ctx_t *c);
bool ny_i386_obj_emit_code(ny_i386_obj_ctx_t *c, const nyir_func_t *nyir,
                           bool tag_return);
int ny_i386_obj_symbol_index(char symbols[][256], size_t count,
                             const char *name);
int ny_i386_obj_def_index(const ny_i386_obj_symbol_def_t *defs, size_t count,
                          const char *name);
bool ny_i386_obj_collect_external_reloc_symbols(
    const ny_i386_obj_reloc_t *relocs, size_t reloc_count,
    const ny_i386_obj_symbol_def_t *defs, size_t def_count,
    char symbols[][256], size_t *symbol_count,
    char *err, size_t err_len);

#endif
