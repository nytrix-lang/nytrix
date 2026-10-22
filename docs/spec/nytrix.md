# Nytrix Embedding API

`<nytrix/nytrix.h>` provides a C ABI for embedding Nytrix in hosts such as
Lisp and Lua. A host builds or imports NYIR functions through the
low-level [`<nyr/nyr.h>` API](nyr.md), registers borrowed function objects in a
`nytrix_vm_t`, and calls them by symbol. The embedding VM resolves registered
NYIR functions recursively and can bridge unresolved calls to a host callback.

The VM API executes NYIR; it does not parse or compile Nytrix source. A Lisp or
Lua frontend can lower its own forms to NYIR, or use the host callback to
connect NYIR calls to its own functions. NYIR values are raw 64-bit words.
Opaque value handles root GC-managed words and marshal nil, booleans, signed
64-bit integers, doubles, and byte strings. Other Ny object layouts remain
host-managed.

## Running complete programs

`nytrix_embed_run_source` and `nytrix_embed_run_file` compile and run a
complete Ny program in-process through the full pipeline — parse, type,
verify, optimize, and execute. The source string or file is the whole program,
exactly as if the `ny` CLI had run it: the program's `main` (or its top-level
statements) executes, diagnostics go to stderr in CLI format, and the call
returns the program's exit status.

The compiler and runtime remain process-global. Same-thread nested source runs
from a host call are supported; a source run requested by another thread while
one is active is rejected with -1. Independent concurrent compiler sessions
are not available. Values do not automatically cross the source-run boundary:
the program's globals and spawned threads are not returned to the host after
the call. A host callback can explicitly keep supported scalar or byte-string
values in a value handle. The examples below are raw C snippets in this
reference; there is no separate embedding demo source under `src`.

The source API and compile-only artifact API are independent entry points.
Keep compile artifacts alive until the host has consumed them, but release
them before returning memory or shutting down:

```c
#include <nytrix/nytrix.h>

#include <stdio.h>

int main(void) {
  char err[256] = {0};
  const char *source = "fn main() int { return 6 * 7 }\n";
  nytrix_embed_run_config_t config = {.opt_level = 2, .reserved = 0};
  if (!nytrix_embed_startup(err, sizeof(err))) {
    fprintf(stderr, "startup: %s\n", err);
    return 1;
  }

  int run_status = nytrix_embed_run_source(source, &config);
  char *artifact = NULL;
  int compile_status = nytrix_embed_compile_source(source, NULL, &artifact);
  if (compile_status == 0 && artifact)
    puts("typed semantic artifact is ready for read-only tooling");
  else {
    fprintf(stderr, "compile-only request failed with status %d\n",
            compile_status);
    run_status = run_status ? run_status : 1;
  }
  nytrix_embed_compile_artifact_free(artifact);

  if (!nytrix_embed_shutdown(err, sizeof(err))) {
    fprintf(stderr, "shutdown: %s\n", err);
    return 1;
  }
  return run_status;
}
```

`nytrix_embed_run_file(path, &config)` is the file-based equivalent.
`options_json_or_null` is reserved by the current compile API; pass `NULL`.
Compile diagnostics still go to stderr in CLI format. A failed compile returns
the compiler's nonzero status and leaves the artifact pointer null. A run
returns the Ny program's exit status; `-1` means invalid input/configuration
or a rejected cross-thread request. Compile and run share one process-global
activity guard: same-thread nested calls from a host callback are supported;
a second thread attempting either operation during an active call is rejected.

## Lifecycle and ownership

- Call `nytrix_embed_startup` to initialize the process runtime explicitly.
  Source-run and value-handle APIs also initialize it on first use.
- Call `nytrix_embed_shutdown` after all source runs finish and all API value
  handles are released. Shutdown is idempotent, rejects live runs or handles,
  clears runtime-owned allocations and interpreter globals, then disposes the
  GC. Raw words not held by API handles are not tracked; the host must stop
  using them before shutdown.
- `nytrix_value_handle_create/get/set/release` keeps a raw Ny word rooted in
  the runtime GC, including reachable managed values nested in aggregates. The
  slot follows moving collections. Rooting is not a copy or host-memory
  ownership transfer: raw pointers anywhere in the value graph remain
  host-owned. `get` returns a borrowed raw word; put it in another handle before
  an allocation if it must stay live. The caller keeps an input live until
  `create` returns and serializes operations on each handle. Typed handle APIs
  marshal nil, booleans, signed 64-bit integers (including values represented
  internally as BigInt), doubles, and byte strings. Integer reads reject values
  outside `int64_t`; string reads preserve embedded NUL bytes and use a
  size-query call before copying.
- Create a VM with `nytrix_vm_create`; pass `NULL` for default limits.
- Register verified `nyr_func_t` functions by name. The VM copies names but
  borrows function objects; they must remain alive and unchanged until
  unregistered or VM destruction.
- Duplicate names and registry changes during a VM call are rejected.
- A host callback may make a nested call on the same VM, but must not destroy
  the VM or its registered functions until the outermost call returns.
- Unregister functions explicitly or destroy the VM with
  `nytrix_vm_destroy`.
- The same VM is not thread-safe. Independent VMs may be used independently;
  callback userdata remains host-owned.
- `nytrix_vm_call` initializes its result output; destroy populated
  `nyr_eval_result_t` storage before reusing it. It releases partially
  allocated result storage on execution failure.

`nytrix_vm_call` requires an exact argument count. It allocates a zeroed local
frame, copies arguments into the parameter slots, and returns execution
metadata/result through `nyr_eval_result_t`. A zero per-call step budget uses
the VM's configured budget; nested invocations each receive that per-function
budget. The VM defaults to one million steps per function invocation and a
64-function call depth. Configure lower limits for untrusted input.

## Host calls

The optional `nytrix_host_call_fn` handles NYIR `CALL` symbols not registered
in the VM. Registered NYIR functions take precedence. The callback receives raw
NYIR word arguments and writes one raw word result; returning false aborts
execution and should fill the diagnostic buffer. The VM does not convert host
language objects or provide a garbage-collected object-handle table.

This raw VM example registers a verified NYIR function. The VM owns the
registry entry and copied name, but continues borrowing `caller`:

```c
#include <nytrix/nytrix.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool emit_const(nyr_func_t *f, int64_t value, int *out, char *err,
                      size_t err_len) {
  return nyr_emit(f,
                  (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .a = -1,
                               .b = -1, .c = -1, .d = -1, .e = -1,
                               .f = -1, .imm = value,
                               .scalar_class = NYIR_SCALAR_I64_RAW},
                  out, err, err_len);
}

static bool host_add(void *ctx, const char *symbol, const int64_t *args,
                     size_t count, int64_t *out, char *err, size_t err_len) {
  (void)ctx;
  if (!symbol || strcmp(symbol, "host_add") || !args || count != 2 || !out) {
    if (err && err_len)
    snprintf(err, err_len, "invalid host_add request");
    return false;
  }
  if ((args[1] > 0 && args[0] > INT64_MAX - args[1]) ||
      (args[1] < 0 && args[0] < INT64_MIN - args[1])) {
    if (err && err_len)
    snprintf(err, err_len, "host_add overflow");
    return false;
  }
  *out = args[0] + args[1];
  return true;
}

int main(void) {
  char err[256] = {0};
  nyr_func_t caller;
  nyr_func_init(&caller);
  int left = -1, right = -1, sum = -1;
  bool ok =
      emit_const(&caller, 19, &left, err, sizeof(err)) &&
      emit_const(&caller, 23, &right, err, sizeof(err)) &&
      nyr_emit(&caller,
               (nyr_inst_t){.op = NYIR_CALL, .dst = -1, .a = left,
                            .b = right, .c = -1, .d = -1, .e = -1,
                            .f = -1, .imm = 2, .symbol = "host_add",
                            .effects = NYIR_EFFECT_CALL,
                            .scalar_class = NYIR_SCALAR_I64_RAW},
               &sum, err, sizeof(err)) &&
      nyr_emit(&caller,
               (nyr_inst_t){.op = NYIR_RET, .dst = -1, .a = sum,
                            .b = -1, .c = -1, .d = -1, .e = -1, .f = -1},
               NULL, err, sizeof(err)) &&
      nyr_verify(&caller, err, sizeof(err));

  nytrix_vm_config_t config = {.max_steps = 1000,
                               .max_call_depth = 8,
                               .host_call = host_add,
                               .host_userdata = NULL};
  nytrix_vm_t *vm = ok ? nytrix_vm_create(&config, err, sizeof(err)) : NULL;
  nyr_eval_result_t result;
  nyr_eval_result_init(&result);
  ok = vm && nytrix_vm_register_function(vm, "caller", &caller, err,
                                         sizeof(err)) &&
       nytrix_vm_call(vm, "caller", NULL, 0, 0, &result, err, sizeof(err)) &&
       result.returned && result.result == 42;
  if (!ok)
    fprintf(stderr, "VM call failed: %s\n", err);

  nyr_eval_result_destroy(&result);
  nytrix_vm_destroy(vm);
  nyr_func_destroy(&caller);
  return ok ? 0 : 1;
}
```

## Rooting and marshalling values

Handles root raw Ny words through moving GC collections. Typed accessors cover
nil, booleans, signed 64-bit integers, doubles, and byte strings. This example
round-trips bytes containing an embedded NUL:

```c
#include <nytrix/nytrix.h>

#include <stdio.h>
#include <string.h>

int main(void) {
  nytrix_value_handle_t *handle = NULL;
  char err[256] = {0};
  const char bytes[] = {'A', '\0', 'B'};
  size_t length = 0;
  char copy[sizeof(bytes)];

  if (!nytrix_value_handle_create(0, &handle, err, sizeof(err)) ||
      !nytrix_value_handle_set_string(handle, bytes, sizeof(bytes), err,
                                      sizeof(err)) ||
      !nytrix_value_handle_get_string(handle, NULL, 0, &length, err,
                                      sizeof(err)) ||
      length != sizeof(bytes) ||
      !nytrix_value_handle_get_string(handle, copy, sizeof(copy), &length, err,
                                      sizeof(err)) ||
      memcmp(bytes, copy, sizeof(bytes)) != 0) {
    fprintf(stderr, "string marshalling failed: %s\n", err);
    nytrix_value_handle_release(handle);
    return 1;
  }
  nytrix_value_handle_release(handle);
  puts("embedded NUL preserved");
  return 0;
}
```

The output from `get` is borrowed, and an input object must remain rooted until
`create` returns. Release every handle before shutdown. Typed getters reject a
different Ny type instead of coercing it; integer reads also reject values
outside `int64_t`. The raw handle form also retains aggregate Ny values such
as lists and tuples without converting their representation; callers that need
to inspect or construct those values use the Ny runtime's value operations.
Raw pointers stored inside a value remain host-owned, and the handle does not
copy or free them. The C embedding test covers an aggregate handle across a
moving collection.

The public shared-library build exports this API — the VM, the `nytrix_embed_`
program runners, and the Nyr API — from `nytrixrt`. Consumers include the
installed headers and link `nytrixrt`. Standalone NYIR frontends that need
verification, optimization, interpretation, and object emission without the
Ny runtime should use the [Nyr API](nyr.md).
