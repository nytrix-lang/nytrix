/* Shared internal helpers for native object generation and serialization. */
#ifndef NY_NATIVE_OBJECT_INTERNAL_H
#define NY_NATIVE_OBJECT_INTERNAL_H

#include "code/backend/core/internal.h"
#include <stdint.h>
#include <stdlib.h>

static inline int ny_x86_obj_align(int n, int align) {
  return (n + align - 1) & ~(align - 1);
}

static inline bool nyr_mach_profile_hot_loop_block(const nyr_mach_func_t *mach,
                                                   uint32_t block) {
  if (!mach || block >= mach->block_len)
    return false;
  uint32_t source_pc = mach->blocks[block].source_pc;
  return source_pc != UINT32_MAX && ny_native_profile_loop_hot(source_pc) >= 16;
}

static inline size_t
nyr_mach_regalloc_hot_peak_live(const nyr_mach_func_t *mach,
                                const nyr_mach_regalloc_t *alloc,
                                size_t inst_len) {
  if (!mach || !alloc || !inst_len)
    return 0;
  int *delta = calloc(inst_len + 1, sizeof(*delta));
  if (!delta)
    return 0;
  for (size_t i = 0; i < alloc->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &alloc->segments[i];
    if (!nyr_mach_profile_hot_loop_block(mach, seg->block) ||
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

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} nyr_obj_buf_t;

#define NY_RELOC_PC32 1
#define NY_RELOC_PLT32 2
#define NY_RELOC_AARCH64_CALL26 3
#define NY_RELOC_AARCH64_ADR_PREL_PG_HI21 4
#define NY_RELOC_AARCH64_ADD_ABS_LO12_NC 5
#define NY_RELOC_X86_64_64 6
#define NY_RELOC_GOTPCREL 7
#define NY_RELOC_X86_64_32S 8

/* A single straight-line Nytrix function can legitimately contain hundreds
 * of calls. Keep the program-level relocation transport larger than the
 * function-count limit so x86-64 JIT/object emission does not reject such a
 * program before the encoder or linker sees it. */
#define NY_X64_OBJ_MAX_RELOCS NY_NATIVE_MAX_RELOCS
#define NY_NATIVE_MAX_DEFS NY_NATIVE_MAX_SYMBOLS

void nyr_native_mach_encode_stats(unsigned long long *mach_ok,
                                  unsigned long long *nir_fallback);
void nyr_native_mach_encode_fallback_detail(char *out, size_t out_len);
/* Scalar DIV/MOD encodings selected by the x64 machine object encoder. */
void nyr_native_mach_div_record(bool magic);
void nyr_native_mach_div_stats(unsigned long long *magic,
                               unsigned long long *idiv);
void nyr_native_mach_regalloc_record(size_t segments, size_t colored,
                                     size_t spilled, size_t reloads,
                                     size_t peak_live);
void nyr_native_mach_regalloc_stats(unsigned long long *segments,
                                    unsigned long long *colored,
                                    unsigned long long *spilled,
                                    unsigned long long *reloads,
                                    unsigned long long *peak_live);
void nyr_native_mach_fpr_record(size_t segments, size_t colored, size_t spilled,
                                size_t reloads, size_t peak_live);
void nyr_native_mach_fpr_stats(unsigned long long *segments,
                               unsigned long long *colored,
                               unsigned long long *spilled,
                               unsigned long long *reloads,
                               unsigned long long *peak_live);
void nyr_native_mach_vector_record(size_t segments, size_t colored,
                                   size_t spilled, size_t reloads,
                                   size_t peak_live);
void nyr_native_mach_vector_stats(unsigned long long *segments,
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
} nyr_native_regalloc_metrics_t;

static inline void
nyr_mach_regalloc_summarize(const nyr_mach_func_t *mach,
                            const nyr_mach_regalloc_t *alloc, size_t inst_len,
                            nyr_native_regalloc_metrics_t *out) {
  if (!out)
    return;
  *out = (nyr_native_regalloc_metrics_t){0};
  if (!alloc)
    return;
  out->segments = alloc->segment_len;
  for (size_t i = 0; i < alloc->segment_len; ++i) {
    const nyr_mach_live_segment_t *seg = &alloc->segments[i];
    if (seg->color >= 0)
      ++out->colored;
    else
      ++out->spilled;
    if (seg->reload)
      ++out->reloads;
    if (!nyr_mach_profile_hot_loop_block(mach, seg->block))
      continue;
    ++out->hot_loop_segments;
    if (seg->color < 0)
      ++out->hot_loop_spilled;
    if (seg->reload)
      ++out->hot_loop_reloads;
  }
  out->peak_live = nyr_mach_regalloc_peak_live(alloc, inst_len);
  out->hot_loop_peak_live =
      nyr_mach_regalloc_hot_peak_live(mach, alloc, inst_len);
}

bool nyr_native_x64_regalloc_metrics(const nyr_mach_func_t *mach,
                                     const ny_native_target_info_t *target,
                                     nyr_native_regalloc_metrics_t *gpr,
                                     nyr_native_regalloc_metrics_t *fpr,
                                     nyr_native_regalloc_metrics_t *vector);
bool nyr_native_a64_regalloc_metrics(const nyr_mach_func_t *mach,
                                     nyr_native_regalloc_metrics_t *gpr,
                                     nyr_native_regalloc_metrics_t *fpr,
                                     nyr_native_regalloc_metrics_t *vector);
bool nyr_mach_regalloc_build_class(const nyr_mach_func_t *mach,
                                   nyr_mach_reg_class_t reg_class,
                                   size_t color_count,
                                   nyr_mach_regalloc_t *out);

typedef struct {
  int64_t label;
  size_t off;
} nyr_x64_obj_label_t;
typedef struct {
  int64_t label;
  size_t disp_off;
} nyr_x64_obj_patch_t;
typedef struct {
  char symbol[256];
  size_t disp_off;
  int type;
  bool is_data;
  int64_t addend;
} nyr_x64_obj_reloc_t;
typedef struct {
  char name[256];
  size_t off;
  size_t size;
  bool is_data;
  /* NULL when the symbol lives in the default .text/.data section;
   * non-NULL means the caller asked for a named data section. */
  const char *section;
  uint64_t section_flags;
  size_t alignment;
  size_t section_entsize;
  bool function_symbol;
  bool is_local;
  bool is_weak;
} nyr_x64_obj_symbol_def_t;

typedef struct {
  size_t def_index;
  size_t offset;
  const char *file;
  uint32_t line;
  uint32_t column;
} nyr_native_debug_line_t;

/* Append one source-map record, growing the caller-owned vector safely. */
bool nyr_native_debug_line_append(nyr_native_debug_line_t **lines,
                                  size_t *count, size_t *cap,
                                  nyr_native_debug_line_t line);

typedef struct {
  size_t def_index;
  const char *name;      /* owned copy */
  const char *type_name; /* owned copy, never NULL */
  int offset;            /* rbp-relative frame offset */
  uint32_t decl_line;
  /* 0 variable, 1 parameter, 2 lexical-scope marker (name/type are NULL). */
  uint32_t is_param;
  uint32_t end_line; /* scope markers only */
  int32_t depth;     /* scope markers only */
} nyr_native_debug_var_t;

bool nyr_native_debug_var_append(nyr_native_debug_var_t **vars, size_t *count,
                                 size_t *cap, nyr_native_debug_var_t var);
void nyr_native_debug_vars_free(nyr_native_debug_var_t *vars, size_t count);

bool nyr_native_build_debug_line(const nyr_native_debug_line_t *lines,
                                 size_t line_count, size_t text_len,
                                 nyr_obj_buf_t *out, size_t *base_reloc_offset,
                                 size_t *base_def_index, char *err,
                                 size_t err_len);
bool nyr_globaltab_append_defs(nyr_obj_buf_t *code,
                               nyr_x64_obj_symbol_def_t *defs,
                               size_t *def_count, char *err, size_t err_len);
typedef struct {
  const nyr_inst_t **defs;
  int count;
} nyr_x64_obj_valmap_t;

/* Append interned pure-native C strings into the code blob as local defs. */
bool nyr_strtab_append_defs(nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs,
                            size_t *def_count, char *err, size_t err_len);

/* Append foldable top-level def constants into the code blob as 8-byte
 * .data definitions (conway's HALF_W, CELL_ALIVE, ...). */
bool nyr_consttab_append_defs(nyr_obj_buf_t *code,
                              nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
                              char *err, size_t err_len);

/* Append interned constant arrays (list literals) into the code blob as
 * count * 8 bytes of .data definitions. */
bool nyr_arraytab_append_defs(nyr_obj_buf_t *code,
                              nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
                              char *err, size_t err_len);

typedef struct {
  nyr_obj_buf_t code;
  const ny_native_target_info_t *target;
  const nyr_func_t *nyir;
  nyr_x64_obj_valmap_t valmap;
  bool bare_syscalls;
  bool omit_frame_pointer;
  bool position_independent;
  bool retpoline_indirect_calls;
  bool retpoline_inline_calls;
  bool function_return_thunk_extern;
  bool cf_protection_branch;
  bool stack_guard;
  size_t temporary_stack_bytes;
  int value_slots, spill_slots, callee_save_slots, local_slots, frame_bytes;
  bool *value_f64, *value_f32, *local_f64, *local_f32;
  uint8_t *value_i64_vector_lanes;
  bool *value_immediate;
  int8_t *value_reg, *value_xmm;
  int *value_spill;
  int callee_save_slot[16];
  int callee_save_xmm[16];
  /* Aggregate parameter homes. agg_home_off[i] is the rbp-relative byte
   * offset (negative) of parameter i's home storage when param_types[i] is
   * NYIR_PARAM_AGGREGATE, else INT_MIN. agg_home_words counts the extra
   * 8-byte frame words added below the scalar frame area. */
  size_t agg_home_words;
  int *agg_home_off;
  nyr_x64_obj_label_t *labels;
  size_t label_count;
  size_t label_cap;
  nyr_x64_obj_patch_t *patches;
  size_t patch_count;
  size_t patch_cap;
  nyr_x64_obj_reloc_t *relocs;
  size_t reloc_count;
  size_t reloc_cap;
  size_t *inst_offsets;
  char *err;
  size_t err_len;
} nyr_x64_obj_ctx_t;

typedef struct {
  int64_t label;
  size_t off;
} nyr_i386_obj_label_t;
typedef struct {
  int64_t label;
  size_t disp_off;
} nyr_i386_obj_patch_t;
typedef struct {
  char symbol[256];
  size_t disp_off;
} nyr_i386_obj_reloc_t;
typedef struct {
  char name[256];
  size_t off;
  size_t size;
} nyr_i386_obj_symbol_def_t;

typedef struct {
  nyr_obj_buf_t code;
  const ny_native_target_info_t *target;
  int value_slots, local_slots, frame_bytes, local_base, max_local_slot;
  bool *value_f64, *value_f32, *local_f64, *local_f32;
  nyr_i386_obj_label_t labels[1024];
  size_t label_count;
  nyr_i386_obj_patch_t patches[1024];
  size_t patch_count;
  size_t epilogue_patches[1024];
  size_t epilogue_patch_count;
  nyr_i386_obj_reloc_t *relocs;
  size_t reloc_count;
  size_t reloc_cap;
  char *err;
  size_t err_len;
} nyr_i386_obj_ctx_t;

void nyr_obj_free(nyr_obj_buf_t *b);
bool nyr_obj_emit(nyr_obj_buf_t *b, const void *data, size_t len);
bool nyr_obj_pad_to(nyr_obj_buf_t *b, size_t align);
bool nyr_obj_pad_code(nyr_obj_buf_t *b, size_t align);
bool nyr_obj_zero(nyr_obj_buf_t *b, size_t len);
bool nyr_obj_u8(nyr_obj_buf_t *b, unsigned v);
bool nyr_obj_u16(nyr_obj_buf_t *b, uint16_t v);
bool nyr_obj_u32(nyr_obj_buf_t *b, uint32_t v);
bool nyr_obj_u64(nyr_obj_buf_t *b, uint64_t v);
void nyr_obj_patch_u16(nyr_obj_buf_t *b, size_t off, uint16_t v);
void nyr_obj_patch_u32(nyr_obj_buf_t *b, size_t off, uint32_t v);
void nyr_obj_patch_u64(nyr_obj_buf_t *b, size_t off, uint64_t v);
static inline bool nyr_obj_elf64_write_sym(nyr_obj_buf_t *b, uint32_t name,
                                           unsigned info, uint16_t shndx,
                                           uint64_t value, uint64_t size) {
  return nyr_obj_u32(b, name) && nyr_obj_u8(b, info) && nyr_obj_u8(b, 0) &&
         nyr_obj_u16(b, shndx) && nyr_obj_u64(b, value) && nyr_obj_u64(b, size);
}
bool nyr_elf32_write_sym(nyr_obj_buf_t *b, uint32_t name, uint32_t value,
                         uint32_t size, unsigned char info, uint16_t shndx);
bool nyr_elf32_write_sh(nyr_obj_buf_t *b, uint32_t name, uint32_t type,
                        uint32_t flags, uint32_t off, uint32_t size,
                        uint32_t link, uint32_t info, uint32_t addralign,
                        uint32_t entsize);

void nyr_x64_obj_ctx_free(nyr_x64_obj_ctx_t *c);
bool nyr_x64_obj_emit_code(nyr_x64_obj_ctx_t *c, const nyr_func_t *nyir,
                           bool tag_return);
int nyr_x86_obj_symbol_index(char symbols[][256], size_t count,
                             const char *name);
int nyr_x64_obj_def_index(const nyr_x64_obj_symbol_def_t *defs, size_t count,
                          const char *name);
bool nyr_x64_mach_is_f64_operand(const nyr_mach_func_t *mach,
                                 const nyr_mach_operand_t *op);
bool nyr_x64_obj_collect_external_reloc_symbols(
    const nyr_x64_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_x64_obj_symbol_def_t *defs, size_t def_count, char symbols[][256],
    size_t *symbol_count, char *err, size_t err_len);
bool nyr_x64_obj_collect_reloc_symbols(const nyr_x64_obj_reloc_t *relocs,
                                       size_t reloc_count, char symbols[][256],
                                       size_t *symbol_count, char *err,
                                       size_t err_len);
bool nyr_x64_obj_append_function(nyr_obj_buf_t *code,
                                 nyr_x64_obj_symbol_def_t *defs,
                                 size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                                 size_t *reloc_count, const nyr_func_t *nyir,
                                 const ny_native_target_info_t *target,
                                 const char *symbol, bool tag_return, char *err,
                                 size_t err_len);
bool nyr_x64_obj_append_function_with_frame(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, const nyr_func_t *nyir,
    const ny_native_target_info_t *target, const char *symbol,
    bool omit_frame_pointer, bool position_independent,
    bool retpoline_indirect_calls, bool retpoline_inline_calls,
    bool function_return_thunk_extern, bool cf_protection_branch, char *err,
    size_t err_len);
bool nyr_x64_obj_append_function_debug(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, const nyr_func_t *nyir,
    const ny_native_target_info_t *target, const char *symbol, bool tag_return,
    bool bare_syscalls, const nyr_func_t *source,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len,
    size_t **inst_offsets_out, size_t *inst_offsets_count);
bool nyr_x64_obj_build_bundle(const nyr_func_t *rt_main,
                              const nyr_func_t *funcs,
                              const char *const *func_names, size_t func_count,
                              const ny_native_target_info_t *target,
                              const char *entry_symbol, bool tag_return,
                              nyr_obj_buf_t *code,
                              nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
                              nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
                              char *err, size_t err_len);

/* Independence path: encode finalized machine form to bytes (no NYIR encoder).
 */
#include "code/backend/lower/machine.h"

bool nyr_native_x64_build_mixed_bundle(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs,
    size_t *def_count, nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    char *err, size_t err_len);
bool nyr_x64_mach_build_bundle(const nyr_mach_func_t *rt_main_mir,
                               const nyr_mach_func_t *func_mirs,
                               const char *const *func_names, size_t func_count,
                               const ny_native_target_info_t *target,
                               const char *entry_symbol, bool tag_return,
                               nyr_obj_buf_t *code,
                               nyr_x64_obj_symbol_def_t *defs,
                               size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                               size_t *reloc_count, char *err, size_t err_len);
bool nyr_x64_mach_build_bundle_debug(
    const nyr_mach_func_t *rt_main_mir, const nyr_mach_func_t *func_mirs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *entry_symbol,
    bool tag_return, bool bare_syscalls, const nyr_func_t *rt_main_source,
    const nyr_func_t *func_sources, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    nyr_native_debug_line_t **debug_lines, size_t *debug_line_count,
    size_t *debug_line_cap, char *err, size_t err_len);
bool nyr_x64_mach_append_function(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, char *err, size_t err_len);
bool nyr_x64_mach_append_function_debug(
    nyr_obj_buf_t *code, nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
    const nyr_mach_func_t *mach, const ny_native_target_info_t *target,
    const char *symbol, bool tag_return, bool bare_syscalls,
    const nyr_func_t *source, nyr_native_debug_line_t **debug_lines,
    size_t *debug_line_count, size_t *debug_line_cap,
    nyr_native_debug_var_t **debug_vars, size_t *debug_var_count,
    size_t *debug_var_cap, char *err, size_t err_len,
    size_t **inst_offsets_out, size_t *inst_offsets_count);
bool nyr_x64_try_stencil_bundle(const nyr_func_t *rt_main,
                                const ny_native_target_info_t *target,
                                nyr_obj_buf_t *code,
                                nyr_x64_obj_symbol_def_t *defs,
                                size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                                size_t *reloc_count, char *err, size_t err_len);
/* Same as above but folds pure direct calls into helper NYIR bodies (call
 * stencil): when every CALL target is a pure foldable helper, emit const/local
 * shell without emitting the call. */
bool nyr_x64_try_stencil_bundle_calls(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, nyr_obj_buf_t *code,
    nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
    nyr_x64_obj_reloc_t *relocs, size_t *reloc_count, char *err,
    size_t err_len);
bool nyr_a64_mach_build_bundle(const nyr_mach_func_t *rt_main_mir,
                               const nyr_mach_func_t *func_mirs,
                               const char *const *func_names, size_t func_count,
                               const ny_native_target_info_t *target,
                               const char *entry_symbol, bool tag_return,
                               nyr_obj_buf_t *code,
                               nyr_x64_obj_symbol_def_t *defs,
                               size_t *def_count, nyr_x64_obj_reloc_t *relocs,
                               size_t *reloc_count, char *err, size_t err_len);
bool nyr_a64_obj_build_bundle(const nyr_func_t *rt_main,
                              const nyr_func_t *funcs,
                              const char *const *func_names, size_t func_count,
                              const ny_native_target_info_t *target,
                              const char *entry_symbol, bool tag_return,
                              nyr_obj_buf_t *code,
                              nyr_x64_obj_symbol_def_t *defs, size_t *def_count,
                              nyr_x64_obj_reloc_t *relocs, size_t *reloc_count,
                              char *err, size_t err_len);

void nyr_i386_obj_ctx_free(nyr_i386_obj_ctx_t *c);
bool nyr_i386_obj_emit_code(nyr_i386_obj_ctx_t *c, const nyr_func_t *nyir,
                            bool tag_return);
int nyr_i386_obj_def_index(const nyr_i386_obj_symbol_def_t *defs, size_t count,
                           const char *name);
bool nyr_i386_obj_collect_external_reloc_symbols(
    const nyr_i386_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_i386_obj_symbol_def_t *defs, size_t def_count,
    char symbols[][256], size_t *symbol_count, char *err, size_t err_len);

#endif
