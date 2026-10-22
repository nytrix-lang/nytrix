#ifndef NY_NATIVE_INTERNAL_H
#define NY_NATIVE_INTERNAL_H

#include "code/backend/ir/machine.h"
#include "code/backend/native.h"
#include "code/backend/target.h"
#include <nyr/ir.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Signed 64-bit constant division via a Granlund-Montgomery multiplier.
 * `divisor` must be positive and greater than one. */
static inline void nyr_sdiv_magic_i64(uint64_t divisor, int64_t *magic_out,
                                      unsigned *shift_out) {
  const uint64_t two63 = (uint64_t)1 << 63;
  uint64_t anc = two63 - 1 - (two63 % divisor);
  unsigned p = 63;
  uint64_t q1 = two63 / anc;
  uint64_t r1 = two63 - q1 * anc;
  uint64_t q2 = two63 / divisor;
  uint64_t r2 = two63 - q2 * divisor;
  uint64_t delta;
  do {
    ++p;
    q1 <<= 1;
    r1 <<= 1;
    if (r1 >= anc) {
      ++q1;
      r1 -= anc;
    }
    q2 <<= 1;
    r2 <<= 1;
    if (r2 >= divisor) {
      ++q2;
      r2 -= divisor;
    }
    delta = divisor - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));
  *magic_out = (int64_t)(q2 + 1);
  *shift_out = p - 64;
}

static inline bool nyr_size_mul_ok(size_t count, size_t size, size_t *out) {
  if (size != 0 && count > SIZE_MAX / size)
    return false;
  if (out)
    *out = count * size;
  return true;
}

typedef struct nyr_writer_impl_t {
  char *data;
  size_t len;
  size_t cap;
} nyr_writer_t;

/*
 * NYIP program-member capacity.
 *
 * This is a hostile-input and allocation bound, not a language limit. Bundle
 * construction allocates one NYIR slot per user function and rejects a larger
 * program before serializing, so no function is silently omitted. Keep this
 * limit in step with the u32 member count written by the bundle format and the
 * loader's bounded allocation policy.
 */
#define NYR_BUNDLE_MAX_FUNCS 16384u

/* Library precompiles intentionally root every named function in the module,
 * unlike a live JIT build's reachable-function subset. Keep enough room for
 * the full bundled stdlib and generated/reachable helpers while retaining the
 * smaller live-work bound for ordinary programs. */
#define NY_NATIVE_PRECOMPILE_MAX_FUNCS 65536u

/*
 * Live native work-buffer allocation bound. Keep it aligned with the maximum
 * number of functions accepted by the NYIR bundle format so large but valid
 * source programs cannot lose reachable callees during collection.
 */
#define NY_NATIVE_LIVE_MAX_FUNCS NYR_BUNDLE_MAX_FUNCS

/* Shared native transport capacities. Builders grow heap storage up to these
 * hostile-input guards, preserving bounded diagnostics without stack tables.
 * Large UI bundles need a wider relocation transport than small fixtures. */
#define NY_NATIVE_MAX_SYMBOLS 16384u
#define NY_NATIVE_MAX_RELOCS 1048576u

bool nyr_put(nyr_writer_t *w, const char *s);
bool nyr_printf(nyr_writer_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void nyr_set_error(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
size_t nyr_local_count(const nyr_func_t *f);

bool nyr_strtab_append_asm(nyr_writer_t *w, char *err, size_t err_len);
bool nyr_arraytab_append_asm(nyr_writer_t *w, char *err, size_t err_len);
bool nyr_ensure_parent_dir_for_path(const char *path);
bool nyr_emit_func(nyr_writer_t *w, const ny_native_target_info_t *target,
                   const nyr_func_t *nyir, const char *label, bool tag_return,
                   char *err, size_t err_len);
bool nyr_x86_64_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                        const nyr_func_t *nyir, const char *func_name,
                        bool tag_return, char *err, size_t err_len);
/* First executable machine-form consumer. Scalar integer/control-flow subset;
 * callers retain NYIR encode as the explicit fallback for other shapes. */
bool nyr_x86_64_emit_mach_scalar(nyr_writer_t *w,
                                 const ny_native_target_info_t *target,
                                 const nyr_mach_func_t *mach,
                                 const char *func_name, bool tag_return,
                                 char *err, size_t err_len);

/* C-string pool for pure-native string literals (print, etc.). */
void nyr_strtab_clear(void);
const char *nyr_strtab_intern(const char *s, size_t len, char *name_out,
                              size_t name_cap);
const char *nyr_strtab_get(const char *name, size_t *len_out);

/* Session-local pool of foldable top-level def constants emitted as
 * 8-byte .data definitions in object files. */
void nyr_consttab_clear(void);
bool nyr_consttab_add(const char *name, int64_t value);
bool nyr_consttab_has(const char *name);
bool nyr_consttab_get(const char *name, int64_t *value);
bool nyr_consttab_get_tail(const char *name, int64_t *value);
bool nyr_globaltab_add(const char *name);
bool nyr_globaltab_has(const char *name);
const char *nyr_globaltab_name(const char *name);
const char *nyr_globaltab_name_tail(const char *name);
void nyr_globaltab_clear(void);

typedef struct {
  int64_t value;
  int64_t tag;
  const char *str;
  size_t str_len;
} nyr_array_elem_t;

typedef struct {
  char name[32];
  nyr_array_elem_t *values;
  char **strings;
  size_t count;
  size_t stride;
  void *data_ptr;
} ny_arraytab_ent_t;

extern ny_arraytab_ent_t *nyr_arraytab;
extern size_t nyr_arraytab_len;

/* Session-local pool of constant list storage. Scalar lists use one 8-byte
 * word per element. String lists use explicit 24-byte dynamic descriptors:
 * payload-relative pointer, byte length, and runtime string tag. */
void nyr_arraytab_clear(void);
const char *nyr_arraytab_intern(const nyr_array_elem_t *values, size_t count,
                                size_t stride, char *name_out, size_t name_cap);
bool nyr_arraytab_get(const char *name, const nyr_array_elem_t **elems_out,
                      size_t *count_out, size_t *stride_out);
void *nyr_arraytab_data(const char *name);

bool nyr_aarch64_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                         const nyr_func_t *nyir, const char *func_name,
                         bool tag_return, char *err, size_t err_len);

bool nyr_riscv_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                       const nyr_func_t *nyir, const char *func_name,
                       bool tag_return, char *err, size_t err_len);

bool nyr_bpf_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                     const nyr_func_t *nyir, const char *func_name,
                     bool tag_return, char *err, size_t err_len);

bool nyr_mips_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                      const nyr_func_t *nyir, const char *func_name,
                      bool tag_return, char *err, size_t err_len);

bool nyr_powerpc_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                         const nyr_func_t *nyir, const char *func_name,
                         bool tag_return, char *err, size_t err_len);

bool nyr_avr_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                     const nyr_func_t *nyir, const char *func_name,
                     bool tag_return, char *err, size_t err_len);

bool nyr_wasm_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                      const nyr_func_t *nyir, const char *func_name,
                      bool tag_return, char *err, size_t err_len);

typedef enum {
  NYR_WASM_IMPORT_I32,
  NYR_WASM_IMPORT_I64,
  NYR_WASM_IMPORT_F32,
  NYR_WASM_IMPORT_F64,
  NYR_WASM_IMPORT_VOID,
  NYR_WASM_IMPORT_UNSUPPORTED,
} nyr_wasm_import_type_t;

bool nyr_wasm_call_signature(const nyr_func_t *func, const nyr_inst_t *call,
                             const int *args, int argc,
                             nyr_wasm_import_type_t *arg_types,
                             nyr_wasm_import_type_t *result_type);

bool nyr_arm_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                     const nyr_func_t *nyir, const char *func_name,
                     bool tag_return, char *err, size_t err_len);

bool nyr_i386_emit_ir(nyr_writer_t *w, const ny_native_target_info_t *target,
                      const nyr_func_t *nyir, const char *func_name,
                      bool tag_return, char *err, size_t err_len);

bool nyr_native_emit_elf64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool nyr_native_emit_elf64_aarch64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool nyr_native_emit_elf32_i386_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool nyr_native_emit_coff_x64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);
bool nyr_native_emit_macho_x64_object_from_nirs(
    const nyr_func_t *rt_main, const nyr_func_t *funcs,
    const char *const *func_names, size_t func_count,
    const ny_native_target_info_t *target, const char *path,
    const char *entry_symbol, bool tag_return, char *err, size_t err_len);

bool nyr_native_emit_elf64_object_from_nir(
    const nyr_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len);
bool nyr_native_emit_coff_x64_object_from_nir(
    const nyr_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len);
bool nyr_native_emit_macho_x64_object_from_nir(
    const nyr_func_t *nyir, const ny_native_target_info_t *target,
    const char *path, const char *symbol_name, bool tag_return, char *err,
    size_t err_len);

bool nyr_native_result_oracle_for_nir(
    nyr_func_t *rt_main, nyr_func_t *funcs, const char *const *names,
    size_t count, const ny_options *opt,
    const nyr_runtime_services_t *runtime_services, char *err, size_t err_len);

bool nyr_oracle_fuzz(const ny_options *opt, int count,
                     const nyr_runtime_services_t *runtime_services, char *err,
                     size_t err_len);

/* Backend regression probe used by the native test command. */
bool nyr_vector_selftest(char *err, size_t err_len);

/* Shared string literals used by multiple native subsystems. */
#define NY_NATIVE_ALLOC_FAIL "native NYIR lower: allocation failed"
#define NY_NATIVE_OOM "native NYIR VM: out of memory"
#define NY_NATIVE_LOAD_OOM "native NYIR load: out of memory"
#define NY_NATIVE_BUNDLE_OOM "native NYIR bundle: out of memory"
#define NY_NATIVE_UNKNOWN_ERR "unknown error"
#define NY_FMT_FN "%sny_fn_%s"

/* Shared op-classifiers used by backend and object writers. */
static inline bool nyr_op_is_f64(nyr_op_t op) {
  return op == NYIR_CONST_F64 || op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
         op == NYIR_MUL_F64 || op == NYIR_DIV_F64 || op == NYIR_I64_TO_F64 ||
         op == NYIR_F32_TO_F64 || op == NYIR_SQRT_F64 || op == NYIR_SIN_F64 ||
         op == NYIR_COS_F64 || op == NYIR_VEC4_REDUCE_ADD_F64;
}
static inline bool nyr_op_is_f32(nyr_op_t op) {
  return op == NYIR_CONST_F32 || op == NYIR_ADD_F32 || op == NYIR_SUB_F32 ||
         op == NYIR_MUL_F32 || op == NYIR_DIV_F32 || op == NYIR_I64_TO_F32 ||
         op == NYIR_F64_TO_F32;
}

#endif
