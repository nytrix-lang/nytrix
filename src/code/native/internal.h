#ifndef NY_NATIVE_INTERNAL_H
#define NY_NATIVE_INTERNAL_H

#include "code/native/native.h"
#include "code/native/ir.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct ny_native_writer_t {
  char *data;
  size_t len;
  size_t cap;
} ny_native_writer_t;

/*
 * NYIP program-member capacity.
 *
 * This is a hostile-input and allocation bound, not a language limit. Bundle
 * construction allocates one NYIR slot per user function and rejects a larger
 * program before serializing, so no function is silently omitted. Keep this
 * limit in step with the u32 member count written by the bundle format and the
 * loader's bounded allocation policy.
 */
#define NY_NATIVE_NIR_BUNDLE_MAX_FUNCS 4096u

/*
 * Live native work-buffer capacity.
 *
 * JIT and object paths currently use fixed function, symbol, and relocation
 * work arrays. This independent limit protects those arrays and must stay no
 * larger than every owning object encoder's definition table. A request above
 * it is rejected before lowering. Do not describe this implementation bound as
 * a NYIP or language limit; making it dynamic requires changing all of those
 * owners together.
 */
#define NY_NATIVE_LIVE_MAX_FUNCS 128u

bool ny_native_put(ny_native_writer_t *w, const char *s);
bool ny_native_printf(ny_native_writer_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void ny_native_set_err(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
size_t ny_native_nir_local_count(const nyir_func_t *f);
bool ny_native_ensure_parent_dir_for_path(const char *path);
bool ny_native_emit_nir_func(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const nyir_func_t *nyir, const char *label,
                             bool tag_return, char *err, size_t err_len);
bool ny_native_collect_vm_profile(nyir_func_t *rt_main,
                                  nyir_func_t *funcs,
                                  const char **names, size_t count,
                                  const ny_options *opt,
                                  nyir_eval_result_t *profile,
                                  char *err, size_t err_len);
bool ny_native_eval_ir_value(nyir_func_t *rt_main, nyir_func_t *funcs,
                             const char **names, size_t count,
                             const ny_options *opt,
                             nyir_eval_result_t *out, char *err,
                             size_t err_len);
bool ny_native_nir_dump_function(FILE *out, const stmt_t *fn, char *err,
                                 size_t err_len, const ny_options *opt);
bool ny_native_nir_dump_rt_main(FILE *out, const program_t *prog, char *err,
                                size_t err_len, const ny_options *opt);
bool ny_native_nir_dump_program_binary(FILE *out, const program_t *prog,
                                       const ny_options *opt, char *err,
                                       size_t err_len);
bool ny_native_write_nir_metadata_report(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len);

bool ny_native_x86_64_emit_rt_main(ny_native_writer_t *w,
                                   const ny_native_target_info_t *target,
                                   const program_t *prog, char *err,
                                   size_t err_len);

bool ny_native_x86_64_emit_nir(ny_native_writer_t *w,
                               const ny_native_target_info_t *target,
                               const nyir_func_t *nyir,
                               const char *func_name,
                               bool tag_return,
                               char *err, size_t err_len);
/* First executable machine-form consumer. Scalar integer/control-flow subset;
 * callers retain NYIR encode as the explicit fallback for other shapes. */
bool ny_native_x86_64_emit_mach_scalar(ny_native_writer_t *w,
                                       const ny_native_target_info_t *target,
                                       const ny_mach_func_t *mach,
                                       const char *func_name, bool tag_return,
                                       char *err, size_t err_len);

/* C-string pool for pure-native string literals (print, etc.). */
void ny_native_strtab_clear(void);
const char *ny_native_strtab_intern(const char *s, size_t len, char *name_out,
                                    size_t name_cap);

bool ny_native_aarch64_emit_nir(ny_native_writer_t *w,
                                const ny_native_target_info_t *target,
                                const nyir_func_t *nyir,
                                const char *func_name,
                                bool tag_return,
                                char *err, size_t err_len);

bool ny_native_riscv_emit_nir(ny_native_writer_t *w,
                              const ny_native_target_info_t *target,
                              const nyir_func_t *nyir,
                              const char *func_name,
                              bool tag_return,
                              char *err, size_t err_len);

bool ny_native_bpf_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const nyir_func_t *nyir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);

bool ny_native_mips_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const nyir_func_t *nyir,
                             const char *func_name,
                             bool tag_return,
                             char *err, size_t err_len);

bool ny_native_powerpc_emit_nir(ny_native_writer_t *w,
                                const ny_native_target_info_t *target,
                                const nyir_func_t *nyir,
                                const char *func_name,
                                bool tag_return,
                                char *err, size_t err_len);

bool ny_native_avr_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const nyir_func_t *nyir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);

bool ny_native_wasm_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const nyir_func_t *nyir,
                             const char *func_name,
                             bool tag_return,
                             char *err, size_t err_len);

bool ny_native_arm_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const nyir_func_t *nyir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);

bool ny_native_i386_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const nyir_func_t *nyir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);

bool ny_native_emit_elf64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool ny_native_emit_elf64_aarch64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool ny_native_emit_elf32_i386_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool ny_native_emit_coff_x64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool ny_native_emit_macho_x64_object_from_nirs(
    const nyir_func_t *rt_main, const nyir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);

bool ny_native_emit_elf64_object_from_nir(const nyir_func_t *nyir,
                                          const ny_native_target_info_t *target,
                                          const char *path,
                                          const char *symbol_name,
                                          bool tag_return, char *err,
                                          size_t err_len);
bool ny_native_emit_coff_x64_object_from_nir(const nyir_func_t *nyir,
                                             const ny_native_target_info_t *target,
                                             const char *path,
                                             const char *symbol_name,
                                             bool tag_return, char *err,
                                             size_t err_len);
bool ny_native_emit_macho_x64_object_from_nir(const nyir_func_t *nyir,
                                              const ny_native_target_info_t *target,
                                              const char *path,
                                              const char *symbol_name,
                                              bool tag_return, char *err,
                                              size_t err_len);

bool ny_native_result_oracle_for_nir(nyir_func_t *rt_main,
                                     nyir_func_t *funcs,
                                     const char **names, size_t count,
                                     const ny_options *opt, char *err,
                                     size_t err_len);

bool ny_native_oracle_fuzz(const ny_options *opt, int count, char *err,
                            size_t err_len);

/* Shared string literals used by multiple native subsystems. */
#define NY_NATIVE_ALLOC_FAIL "native NYIR lower: allocation failed"
#define NY_NATIVE_OOM        "native NYIR VM: out of memory"
#define NY_NATIVE_LOAD_OOM   "native NYIR load: out of memory"
#define NY_NATIVE_BUNDLE_OOM "native NYIR bundle: out of memory"
#define NY_NATIVE_UNKNOWN_ERR "unknown error"
#define NY_FMT_FN            "%sny_fn_%s"

/* Shared op-classifiers used by backend and object writers. */
static inline bool nyir_op_is_f64(nyir_op_t op) {
  return op == NYIR_CONST_F64 || op == NYIR_ADD_F64 ||
         op == NYIR_SUB_F64 || op == NYIR_MUL_F64 ||
         op == NYIR_DIV_F64 || op == NYIR_I64_TO_F64 ||
         op == NYIR_F32_TO_F64;
}
static inline bool nyir_op_is_f32(nyir_op_t op) {
  return op == NYIR_CONST_F32 || op == NYIR_ADD_F32 ||
         op == NYIR_SUB_F32 || op == NYIR_MUL_F32 ||
         op == NYIR_DIV_F32 || op == NYIR_I64_TO_F32 ||
         op == NYIR_F64_TO_F32;
}

#endif
