#ifndef NY_NATIVE_INTERNAL_H
#define NY_NATIVE_INTERNAL_H

#include "code/native/native.h"
#include "code/native/ir.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct ny_native_writer_t {
  char *data;
  size_t len;
  size_t cap;
} ny_native_writer_t;

bool ny_native_put(ny_native_writer_t *w, const char *s);
bool ny_native_printf(ny_native_writer_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void ny_native_set_err(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

bool ny_native_x86_64_emit_rt_main(ny_native_writer_t *w,
                                   const ny_native_target_info_t *target,
                                   const program_t *prog, char *err,
                                   size_t err_len);

bool ny_native_x86_64_emit_nir(ny_native_writer_t *w,
                               const ny_native_target_info_t *target,
                               const ny_nir_func_t *nir,
                               const char *func_name,
                               bool tag_return,
                               char *err, size_t err_len);

bool ny_native_aarch64_emit_nir(ny_native_writer_t *w,
                                const ny_native_target_info_t *target,
                                const ny_nir_func_t *nir,
                                const char *func_name,
                                bool tag_return,
                                char *err, size_t err_len);

bool ny_native_riscv_emit_nir(ny_native_writer_t *w,
                              const ny_native_target_info_t *target,
                              const ny_nir_func_t *nir,
                              const char *func_name,
                              bool tag_return,
                              char *err, size_t err_len);

bool ny_native_bpf_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);

bool ny_native_mips_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const ny_nir_func_t *nir,
                             const char *func_name,
                             bool tag_return,
                             char *err, size_t err_len);

bool ny_native_powerpc_emit_nir(ny_native_writer_t *w,
                                const ny_native_target_info_t *target,
                                const ny_nir_func_t *nir,
                                const char *func_name,
                                bool tag_return,
                                char *err, size_t err_len);

bool ny_native_avr_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);

bool ny_native_wasm_emit_nir(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const ny_nir_func_t *nir,
                             const char *func_name,
                             bool tag_return,
                             char *err, size_t err_len);

bool ny_native_arm_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);

bool ny_native_i386_emit_nir(ny_native_writer_t *w,
                            const ny_native_target_info_t *target,
                            const ny_nir_func_t *nir,
                            const char *func_name,
                            bool tag_return,
                            char *err, size_t err_len);


bool ny_native_emit_elf64_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool ny_native_emit_elf32_i386_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool ny_native_emit_coff_x64_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool ny_native_emit_macho_x64_object_from_nirs(
    const ny_nir_func_t *rt_main, const ny_nir_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);

bool ny_native_emit_elf64_object_from_nir(const ny_nir_func_t *nir,
                                          const ny_native_target_info_t *target,
                                          const char *path,
                                          const char *symbol_name,
                                          bool tag_return, char *err,
                                          size_t err_len);
bool ny_native_emit_coff_x64_object_from_nir(const ny_nir_func_t *nir,
                                             const ny_native_target_info_t *target,
                                             const char *path,
                                             const char *symbol_name,
                                             bool tag_return, char *err,
                                             size_t err_len);
bool ny_native_emit_macho_x64_object_from_nir(const ny_nir_func_t *nir,
                                              const ny_native_target_info_t *target,
                                              const char *path,
                                              const char *symbol_name,
                                              bool tag_return, char *err,
                                              size_t err_len);

#endif
