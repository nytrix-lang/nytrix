#ifndef NYR_PUBLIC_H
#define NYR_PUBLIC_H

/*
 * Public Nyr API, version 1.
 *
 * NYIR is a machine-word IR. Values are signed 64-bit words; source-language
 * tagging, aggregate layout, and runtime helper contracts belong to the
 * frontend. The structures are currently shared with the in-tree NYIR
 * implementation. Check nyr_version() and rebuild when the API
 * major version changes.
 */
#include <nyr/ir.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NYR_API_MAJOR 1u
#define NYR_API_MINOR 10u

typedef struct {
  nyr_param_type_t type;
  /* Packed NYIR_ARG_AGG_* metadata when type is NYIR_PARAM_AGGREGATE. */
  uint32_t aggregate_abi;
} nyr_param_desc_t;

typedef enum {
  NYR_OPT_O0 = 0,
  NYR_OPT_O1 = 1,
  NYR_OPT_O2 = 2,
  NYR_OPT_O3 = 3,
} nyr_opt_level_t;

/* Capabilities of this public Nyr API on a reported target. */
enum {
  NYR_CAP_VM = 1u << 0,
  NYR_CAP_ELF_OBJECT = 1u << 1,
  NYR_CAP_COFF_OBJECT = 1u << 2,
  NYR_CAP_MACHO_OBJECT = 1u << 3,
};

typedef struct {
  uint32_t major;
  uint32_t minor;
} nyr_version_t;

typedef struct {
  const char *target;
  const char *object_format;
  unsigned pointer_bits;
  unsigned capabilities;
} nyr_target_t;

/* API and target discovery. Returned strings are static and remain valid. */
nyr_version_t nyr_version(void);
bool nyr_host_target(nyr_target_t *target, char *err, size_t err_len);
/* Query a target triple before code generation. Unknown architectures and
 * malformed triples return false; recognized targets may report VM-only
 * capability when no object writer is available. */
bool nyr_target_query(const char *target_triple, nyr_target_t *target,
                      char *err, size_t err_len);

/* Fallible APIs return false and write a diagnostic when possible. Success
 * clears a non-empty error buffer. */

/* Initialize a function to the empty state and release all function-owned
 * instruction metadata. A function must be initialized before use. */
nyr_func_t *nyr_func_create(void);
void nyr_func_free(nyr_func_t *func);
void nyr_func_init(nyr_func_t *func);
void nyr_func_destroy(nyr_func_t *func);
/* Deep-copy a function into an initialized empty destination. Source and
 * destination must differ. Failure leaves an empty destination. */
bool nyr_func_clone(const nyr_func_t *src, nyr_func_t *dst, char *err,
                    size_t err_len);
/* Set a function's ABI signature once, before emitting instructions. The
 * parameter array is copied. Aggregate details are checked by nyr_verify. */
bool nyr_func_set_signature(nyr_func_t *func, const nyr_param_desc_t *params,
                            size_t param_count, uint32_t return_aggregate_size,
                            const uint8_t return_aggregate_classes[2],
                            char *err, size_t err_len);

/* Append one instruction; call nyr_verify after function construction. Returns
 * false if appending failed and writes a diagnostic when possible. On success,
 * value_id receives the produced value, or -1 for instructions without a
 * result. value_id may be null when unused and is unchanged on failure. */
bool nyr_emit(nyr_func_t *func, nyr_inst_t inst, int *value_id, char *err,
              size_t err_len);
/* nyr_emit copies the instruction symbol and owned metadata (call arguments,
 * aggregate metadata, and PHI inputs); callers may release or reuse their
 * buffers after this call. A NYIR_CALL with NYIR_INST_F_EFFECTS_KNOWN retains
 * its supplied effect mask; the caller is responsible for making that summary
 * sound and including NYIR_EFFECT_CALL. Other instructions receive the
 * opcode's standard effect mask. `inst.debug.file` remains borrowed for the
 * function's lifetime; use nyr_func_own_symbol_copy to retain a temporary
 * source path safely. */
/* Start from a safe instruction value: unused operands and dst are -1. This
 * avoids accidentally treating zero-initialized fields as SSA value 0. */
nyr_inst_t nyr_inst_init(nyr_op_t op);
/* Common typed constructors. They retain NYIR's raw-word value model; source
 * type checking and language-specific representations stay in the frontend. */
bool nyr_emit_const_i64(nyr_func_t *func, int64_t value, int *value_id,
                        char *err, size_t err_len);
bool nyr_emit_const_f64(nyr_func_t *func, double value, int *value_id,
                        char *err, size_t err_len);
bool nyr_emit_binary(nyr_func_t *func, nyr_op_t op, int lhs, int rhs,
                     nyr_scalar_class_t result_class, int *value_id, char *err,
                     size_t err_len);
bool nyr_emit_load_local(nyr_func_t *func, int64_t slot, int *value_id,
                         char *err, size_t err_len);
bool nyr_emit_store_local(nyr_func_t *func, int64_t slot, int value,
                          char *err, size_t err_len);
bool nyr_emit_load_i64(nyr_func_t *func, int address, int64_t offset,
                       int *value_id, char *err, size_t err_len);
bool nyr_emit_store_i64(nyr_func_t *func, int address, int value,
                        int64_t offset, char *err, size_t err_len);
bool nyr_emit_call(nyr_func_t *func, const char *symbol, const int *args,
                   size_t arg_count, nyr_scalar_class_t result_class,
                   int *value_id, char *err, size_t err_len);
/* Emit a call to an external ABI symbol without the `ny_fn_` internal prefix. */
bool nyr_emit_extern_call(nyr_func_t *func, const char *symbol,
                          const int *args, size_t arg_count,
                          nyr_scalar_class_t result_class, int *value_id,
                          char *err, size_t err_len);
bool nyr_emit_phi(nyr_func_t *func, const nyr_phi_incoming_t *incoming,
                  size_t incoming_count, int *value_id, char *err,
                  size_t err_len);
bool nyr_emit_compare_i64(nyr_func_t *func, int lhs, int rhs, nyr_cmp_t cmp,
                          int *value_id, char *err, size_t err_len);
bool nyr_emit_label(nyr_func_t *func, int64_t label, char *err, size_t err_len);
bool nyr_emit_branch(nyr_func_t *func, int64_t label, char *err,
                     size_t err_len);
bool nyr_emit_branch_if(nyr_func_t *func, int condition, int64_t label,
                        char *err, size_t err_len);
bool nyr_emit_return(nyr_func_t *func, int value, char *err, size_t err_len);
bool nyr_verify(const nyr_func_t *func, char *err, size_t err_len);
/* Optimize at a named level and publish the transformed function only on
 * success. Failure leaves the original function unchanged. This strong
 * failure guarantee uses a temporary deep copy and therefore needs additional
 * memory proportional to the function while optimization runs. */
bool nyr_optimize(nyr_func_t *func, nyr_opt_level_t opt_level, char *err,
                  size_t err_len);
/* Initialize fresh result storage; destroy before reusing a populated result.
 */
void nyr_eval_result_init(nyr_eval_result_t *result);
bool nyr_eval(const nyr_func_t *func, int64_t *locals, size_t local_count,
              size_t max_steps, nyr_eval_result_t *result, char *err,
              size_t err_len);
/* Evaluate with synchronous external-call resolution. The resolver receives
 * raw NYIR words and must write a raw result; its error text is propagated.
 * The symbol, read-only argument array, and error buffer are borrowed for the
 * callback only; copy data that must outlive it. The callback and userdata are
 * borrowed for the full evaluation. */
bool nyr_eval_with_calls(const nyr_func_t *func, int64_t *locals,
                         size_t local_count, size_t max_steps,
                         nyr_eval_result_t *result,
                         nyr_call_resolver_t resolver, void *resolver_ctx,
                         char *err, size_t err_len);
void nyr_eval_result_destroy(nyr_eval_result_t *result);

/* Emit one verified function for the host target. */
bool nyr_emit_object(const nyr_func_t *func, const char *path,
                     const char *symbol, char *err, size_t err_len);
/* Emit one verified function as an x86-64 ELF/COFF/Mach-O or AArch64 ELF
 * object for the requested triple. Supported triples select the platform's
 * default C ABI; unsupported architectures, formats, or NYIR operations fail
 * with a diagnostic. This API emits an object and does not invoke a system
 * linker. */
bool nyr_emit_object_for_target(const nyr_func_t *func, const char *path,
                                const char *symbol, const char *target_triple,
                                char *err, size_t err_len);
/* Emit the verified function as native x86-64 assembly text for the host. */
bool nyr_emit_assembly(const nyr_func_t *func, const char *path,
                       const char *symbol, char *err, size_t err_len);
/* Execute a verified no-argument i64 function through the host native JIT.
 * External symbols are resolved from the current process. Only run trusted IR. */
bool nyr_jit_run(const nyr_func_t *func, int64_t *result, char *err,
                 size_t err_len);
/* Link one self-contained ELF64 x86-64 relocatable object into a static
 * executable. The entry function must take no arguments; its i64 result
 * becomes the process exit code.
 * Unresolved strong symbols and unsupported relocations fail with a diagnostic;
 * this path does not invoke an external linker or compiler driver. */
bool nyr_link_elf64_executable(const char *object_path,
                               const char *entry_symbol,
                               const char *output_path, char *err,
                               size_t err_len);
/* Link ELF64 x86-64 relocatable objects and static archives into one static
 * executable. Archive members are extracted to satisfy unresolved strong
 * references. Strong and weak global definitions are resolved across inputs;
 * unresolved strong symbols and duplicate strong definitions fail. The entry
 * has the same no-argument i64 contract as nyr_link_elf64_executable. */
bool nyr_link_elf64_executable_objects(const char *const object_paths[],
                                       size_t object_count,
                                       const char *entry_symbol,
                                       const char *output_path, char *err,
                                       size_t err_len);

/* General object-format linker. A host Linux x86-64 executable request with
 * entry_symbol uses the in-process ELF linker and its no-argument i64 entry
 * contract. Other requests use the configured LLVM linker tools (clang/lld,
 * wasm-ld, and llvm-ar), without invoking a shell. For those requests,
 * entry_symbol names the format's raw linker entry symbol. Set
 * NYR_LINK_CLANG, NYR_LINK_WASM_LD, or NYR_LINK_AR to override tool paths.
 * The request links existing objects; runtime and system libraries may be
 * supplied as ordinary input paths. */
typedef enum {
  NYR_LINK_EXECUTABLE = 0,
  NYR_LINK_SHARED_LIBRARY = 1,
  NYR_LINK_STATIC_ARCHIVE = 2,
} nyr_link_kind_t;

typedef struct {
  const char *const *input_paths;
  size_t input_count;
  const char *output_path;
  const char *target_triple; /* NULL selects the host default. */
  const char *entry_symbol;  /* Optional executable entry point. */
  nyr_link_kind_t kind;
} nyr_link_request_t;

bool nyr_link(const nyr_link_request_t *request, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif
