#ifndef NYTRIX_EMBED_H
#define NYTRIX_EMBED_H

/*
 * Embedding API for hosts such as Lisp and Lua.
 *
 * This API runs host-authored NYIR functions in the in-process interpreter.
 * It does not parse Ny source. Hosts can build functions with <nyr/nyr.h>,
 * register them in a VM, bridge unresolved NYIR calls to host functions, and
 * keep raw Ny values alive through opaque GC-root handles.
 *
 * The nytrix_embed_run_* functions and nytrix_embed_compile_source are the
 * exception: they drive complete Ny programs (source text or files) through
 * the full compiler pipeline inside this process — run_* compile and execute,
 * compile_source stops after the type pipeline and returns the semantic
 * artifact. See their contracts below.
 */
#include <nyr/nyr.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NYTRIX_EMBED_API_MAJOR 1u
#define NYTRIX_EMBED_API_MINOR 3u
#define NYTRIX_VM_DEFAULT_MAX_STEPS 1000000u
#define NYTRIX_VM_DEFAULT_MAX_CALL_DEPTH 64u

typedef struct nytrix_vm nytrix_vm_t;
typedef int64_t nytrix_value_t;
typedef struct nytrix_value_handle nytrix_value_handle_t;

/* Initialize or shut down the process runtime used by source embedding and
 * managed-value handles. Startup is idempotent. Shutdown fails while a source
 * run or value handle is live; the caller must also have released any raw Ny
 * values not held by an API handle. */
bool nytrix_embed_startup(char *err, size_t err_len);
bool nytrix_embed_shutdown(char *err, size_t err_len);

/* Root a raw Ny value for the lifetime of an opaque host handle. `value` is
 * borrowed for this call; callers must keep any GC object it references alive
 * until creation returns. Handles root values and their reachable managed
 * values across collections, including values nested in aggregates, and may
 * be read or updated after collection. This is a GC root, not a copy or
 * ownership transfer; host memory referenced by raw pointers anywhere in the
 * value graph remains host-owned. The raw value returned by get is borrowed;
 * put it in another handle before any operation that may allocate if it must
 * stay live. This form accepts runtime values such as lists and tuples without
 * converting their layout. A handle must not be used
 * after release, and operations on one handle must be externally serialized.
 * Typed handle functions below marshal nil, booleans, signed 64-bit integers,
 * doubles, and byte strings. */
bool nytrix_value_handle_create(nytrix_value_t value,
                                nytrix_value_handle_t **out, char *err,
                                size_t err_len);
bool nytrix_value_handle_get(const nytrix_value_handle_t *handle,
                             nytrix_value_t *out, char *err, size_t err_len);
bool nytrix_value_handle_set(nytrix_value_handle_t *handle,
                             nytrix_value_t value, char *err, size_t err_len);
void nytrix_value_handle_release(nytrix_value_handle_t *handle);

/* Marshal host scalars and byte strings through a rooted handle. String
 * lengths are byte counts and embedded NUL bytes are preserved. For get_string,
 * pass buffer=NULL and capacity=0 to query the required byte count, then call
 * again with at least that many bytes of storage. Values of other Ny types,
 * and integers outside the signed 64-bit range, are rejected. */
bool nytrix_value_handle_set_nil(nytrix_value_handle_t *handle, char *err,
                                 size_t err_len);
bool nytrix_value_handle_set_bool(nytrix_value_handle_t *handle, bool value,
                                  char *err, size_t err_len);
bool nytrix_value_handle_get_bool(const nytrix_value_handle_t *handle,
                                  bool *out, char *err, size_t err_len);
bool nytrix_value_handle_set_i64(nytrix_value_handle_t *handle, int64_t value,
                                 char *err, size_t err_len);
bool nytrix_value_handle_get_i64(const nytrix_value_handle_t *handle,
                                 int64_t *out, char *err, size_t err_len);
bool nytrix_value_handle_set_f64(nytrix_value_handle_t *handle, double value,
                                 char *err, size_t err_len);
bool nytrix_value_handle_get_f64(const nytrix_value_handle_t *handle,
                                 double *out, char *err, size_t err_len);
bool nytrix_value_handle_set_string(nytrix_value_handle_t *handle,
                                    const char *data, size_t length, char *err,
                                    size_t err_len);
bool nytrix_value_handle_get_string(const nytrix_value_handle_t *handle,
                                    char *buffer, size_t capacity,
                                    size_t *length, char *err, size_t err_len);

/* Resolve an NYIR call not registered as a VM function. Arguments and result
 * are raw NYIR 64-bit words; args is borrowed for this synchronous callback,
 * and the host owns tagging, object handles, and value marshalling. A callback
 * may re-enter nytrix_vm_call on the same VM; the configured call-depth limit
 * still applies. Do not destroy the VM or registered functions until the
 * outermost call returns. Registry mutation during an active call is rejected.
 * Return false and write err when the host call fails. */
typedef bool (*nytrix_host_call_fn)(void *userdata, const char *symbol,
                                    const int64_t *args, size_t arg_count,
                                    int64_t *result, char *err, size_t err_len);

typedef struct {
  /* Zero selects NYTRIX_VM_DEFAULT_MAX_STEPS. Applied independently to each
   * NYIR function invocation, including nested calls. */
  size_t max_steps;
  /* Zero selects NYTRIX_VM_DEFAULT_MAX_CALL_DEPTH. */
  size_t max_call_depth;
  nytrix_host_call_fn host_call;
  void *host_userdata;
} nytrix_vm_config_t;

/* Create an isolated function registry. The config is read during creation;
 * host_userdata is borrowed for callback invocations and must remain valid
 * until no VM call can invoke the callback. A VM is not thread-safe: callers
 * must externally serialize calls, registry changes, and destruction. */
nytrix_vm_t *nytrix_vm_create(const nytrix_vm_config_t *config, char *err,
                              size_t err_len);
void nytrix_vm_destroy(nytrix_vm_t *vm);

/* Register a verified function by copied symbol name. `func` remains borrowed:
 * it must stay alive and unmodified until unregistered or the VM is destroyed.
 * Duplicate names are rejected. Registry changes are rejected during calls. */
bool nytrix_vm_register_function(nytrix_vm_t *vm, const char *name,
                                 const nyr_func_t *func, char *err,
                                 size_t err_len);
bool nytrix_vm_unregister_function(nytrix_vm_t *vm, const char *name, char *err,
                                   size_t err_len);

/* Execute a registered function. `arg_count` must equal its parameter count.
 * The VM allocates a zeroed local frame and copies args into the first slots.
 * The call initializes result storage; destroy populated result storage
 * before reusing it. result may be NULL. args and each callback's raw words
 * are borrowed only for the duration of the call/callback. A zero per-call
 * step budget uses the VM's configured budget. Calls may be reentrant through
 * host callbacks up to the configured call-depth limit. */
bool nytrix_vm_call(nytrix_vm_t *vm, const char *name, const int64_t *args,
                    size_t arg_count, size_t max_steps,
                    nyr_eval_result_t *result, char *err, size_t err_len);

typedef struct {
  /* 0-3; zero selects the CLI default optimization level. */
  int opt_level;
  /* Reserved; must be zeroed by callers for forward compatibility. */
  int reserved;
} nytrix_embed_run_config_t;

/* Compile and run a complete Ny program inside this process through the full
 * compiler pipeline: parse, type, verify, optimize, and execute. `source` is
 * a NUL-terminated program text and is borrowed only for the duration of the
 * call; `nytrix_embed_run_file` reads `path` instead. Returns the program's
 * exit status (what a `ny` invocation would return), or -1 when the source,
 * path, or config is invalid, or when another thread already has a source run
 * active. A run may recursively invoke this API from a same-thread host call.
 *
 * Compiler diagnostics go to stderr in the exact `ny` CLI format. Program
 * results and globals still do not cross the boundary: the compiler and
 * runtime remain process-global, so independent concurrent compiler sessions
 * are unavailable. A nested same-thread run from a host call is supported;
 * a source run requested by another thread while one is active is rejected.
 * Do not destroy VMs or trigger host callbacks into the pipeline from another
 * thread while a source run is active. */
int nytrix_embed_run_source(const char *source,
                            const nytrix_embed_run_config_t *config);
int nytrix_embed_run_file(const char *path,
                          const nytrix_embed_run_config_t *config);

/* Compile a complete Ny program through parse and the type pipeline — the
 * same path as `ny --stop-after=hm --emit-artifact` — without executing it.
 * On success (*artifact_json_out) receives a NUL-terminated
 * `ny.semantic-artifact.v1` JSON document holding the typed semantic facts
 * (typed_ast.v1 with the nytrix.semantic.* record tables); it is allocated
 * with malloc and must be released by the host through
 * nytrix_embed_compile_artifact_free. `source` is borrowed for the duration
 * of the call. `options_json_or_null` is reserved: pass NULL (or an empty
 * string); any other value is rejected. Compiler diagnostics still go to
 * stderr in the exact `ny` CLI format, including for failed compiles.
 *
 * Returns 0 with *artifact_json_out set on success. Returns the nonzero
 * status a `ny` invocation would return when the program fails to compile
 * (with *artifact_json_out left NULL), and -1 when an argument is invalid or
 * another thread already has a source run or compile active. The guard is
 * shared with nytrix_embed_run_source: a same-thread nested compile is
 * supported, and a cross-thread compile or run is rejected while one is
 * active. The artifact is read-only tooling data, not an execution result:
 * program results and globals do not cross the boundary. */
int nytrix_embed_compile_source(const char *source,
                                const char *options_json_or_null,
                                char **artifact_json_out);

/* Release an artifact buffer returned by nytrix_embed_compile_source. NULL
 * is ignored. */
void nytrix_embed_compile_artifact_free(char *artifact_json);

#ifdef __cplusplus
}
#endif

#endif /* NYTRIX_EMBED_H */
