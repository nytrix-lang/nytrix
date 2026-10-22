# Nyr API

The versioned C API in `<nyr/nyr.h>` and instruction model in `<nyr/ir.h>`
let an independently authored frontend construct, verify, optimize,
interpret, emit a native object or assembly text, and run a native JIT function.
The `nyr` static library is built independently of the Ny CLI and LLVM backend
on Linux x86-64. The installed `nytrixrt` shared library also exports this API
when the shared-library build is enabled.
`<nytrix/nytrix.h>` adds a function registry and host-call resolver for
embedding NYIR execution; see the [embedding API reference](nytrix.md).

NYIR is a machine-word IR, not a source-language-neutral object model.
Frontends own source-language types, value tagging, aggregate layout, and
runtime helper semantics. The API exposes the current NYIR structures so
frontends can populate the full instruction metadata used by verification,
optimization, interpretation, and native lowering. Check `nyr_version()` at
startup and rebuild against the installed header when its major version
changes. Use `nyr_host_target` for the running host or `nyr_target_query` for
an explicit triple; the latter reports whether the standalone API can emit an
object for that target before a frontend starts code generation.

`nyr_inst_init(op)` creates a safe instruction template with `dst` and unused
operands set to `-1`. The `nyr_emit_*` helpers cover common scalar values,
comparisons, arithmetic, calls, local slots, memory access, PHIs, labels,
branches, and returns; `nyr_emit` remains the generic route for other NYIR
opcodes. Helpers copy instruction symbols and owned arrays, so callers can
reuse temporary buffers after emission. The helpers do not impose a source
type system: a frontend decides its language's types and chooses the
appropriate `nyir_scalar_class_t` for generic operations. `nyr_emit_call`
emits an internal Nyr function call; use `nyr_emit_extern_call` for a C ABI
symbol that must remain unprefixed and resolve from the process. Both retain
NYIR's conservative external-call effects. A call marked
`NYIR_INST_F_EFFECTS_KNOWN` retains its supplied effect mask; the frontend
must ensure that summary is sound and includes `NYIR_EFFECT_CALL`.
`nyr_func_set_signature` copies function parameter descriptors and records
aggregate return metadata before instruction emission; `nyr_verify` checks
target ABI encodings when the function is complete. `nyr_func_clone`
deep-copies instructions and their owned metadata into an initialized empty
destination, allowing a frontend to preserve source IR for rollback or
independent optimization attempts.

`nyr_optimize` publishes optimized IR only after the full pipeline succeeds.
On failure, the caller's function remains unchanged. The API makes a temporary
deep copy to provide this guarantee, so optimization needs memory proportional
to the function in addition to the pass workspace. Use `nyr_func_clone` when a
frontend needs a second independently owned copy for other purposes.

On Linux x86-64, `nyr_emit_assembly` writes native x86-64 assembly text for a
verified function. `nyr_jit_run` executes a verified no-argument `i64`
function in the in-process native JIT, resolving external symbols from the
current process. It is intended for trusted code, not as a sandbox. The JIT
runner currently requires a host x86-64 backend.

On Linux x86-64, `nyr_link_elf64_executable` can link a single self-contained
ELF64 relocatable object into a static executable without starting a linker
process. Its no-argument entry function returns an `i64`, whose low byte is
used as the process exit code. The current linker handles the x86-64 absolute
and PC-relative relocations emitted for simple NYIR functions; unresolved
strong symbols, unsupported relocations, writable executable sections, and
other object formats are rejected with an error. It does not yet replace the
compiler driver's normal application link, which combines runtime objects,
libraries, startup files, platform flags, and user link options.
`nyr_link_elf64_executable_objects` extends this path to multiple ELF objects
and static archives, lazily extracting members that satisfy unresolved strong
symbols. It resolves global and weak symbols as one static executable. It
still rejects unresolved strong symbols, unsupported relocations, and
shared-library inputs.
See the [linker design and current capability boundary](linker.md) for the
format architecture, performance criteria, and default-transition
requirements.

## Brainfuck frontend

The complete frontend below is also maintained at `etc/projects/brainfuck.ny`.
It includes `<nyr/nyr.h>`, builds NYIR directly with Nyr's constructors,
verifies and optimizes it, asks Nyr to emit native assembly, then runs the same
function in Nyr's in-process native JIT. The emitted Brainfuck function calls
`calloc`, `getchar`, and `putchar` as external ABI symbols resolved by the JIT.
No assembler, object linker, or disassembler subprocess is used by the
frontend.

Run the build and commands from the repository root, where the header search
path and `build/release/libnytrixrt.so` link path used by the example resolve:

```sh
cmake --build build/release --target nyr-backend-ir-acceptance-test nytrixrt ny
printf '++++++++[>++++++++<-]>+.' > /tmp/hello.bf
build/release/ny etc/projects/brainfuck.ny /tmp/hello.bf
```

The JIT program writes `A`, reports its exit code, and the frontend prints the
assembly to standard output and saves it as `out.s`. To give the Brainfuck
program runtime input, pass the source file as before and pipe input separately:

```sh
printf ',.' > /tmp/echo.bf
printf 'Z' | build/release/ny etc/projects/brainfuck.ny /tmp/echo.bf
```

<!-- ny-doc-include-code: ny etc/projects/brainfuck.ny -->

## Ownership and errors

- Initialize each `nyr_func_t` with `nyr_func_init` and destroy it with
  `nyr_func_destroy`, or use `nyr_func_create`/`nyr_func_free` when the
  frontend needs an opaque allocated handle. Instruction-owned symbols, PHI
  edges, and call metadata are released with the function.
- The API does not take ownership of caller-provided paths, error buffers,
  locals, or their storage. `nyr_emit` copies each instruction symbol into the
  function; symbols passed to object/assembly emitters are used only for that
  call. `inst.debug.file` is borrowed until the function is destroyed. Use
  `nyir_func_own_symbol_copy` for a temporary path; `nyr_func_clone` copies
  non-empty debug paths into the clone.
- Initialize `nyr_eval_result_t` with `nyr_eval_result_init` before first use.
  Destroy it before reuse; `nyr_eval` initializes the output and owns any
  profiling allocations until `nyr_eval_result_destroy`.
- `nyr_emit` returns success separately from the optional result ID. A
  successful append of a non-value instruction reports `-1` in `value_id`;
  `false` means the instruction was not appended.
- `nyr_eval_with_calls` invokes a caller-provided resolver synchronously for
  each `NYIR_CALL`. Arguments and results are raw 64-bit NYIR words; the
  resolver owns source-language conversion and may return a diagnostic through
  the supplied error buffer. The symbol, read-only argument array, and error
  buffer are borrowed for the callback; copy data that must outlive it. The
  resolver function pointer and userdata are borrowed for the whole evaluation.
- `nyr_jit_run` executes machine code in-process and is not a sandbox. Its
  external calls resolve against the current process; only run trusted IR.
- Verification, assembly, JIT, and object-emission errors are written into the
  caller's buffer when one is provided. A `false` result means the function was
  not accepted; the diagnostic may identify an unsupported operation or
  target.
- All fallible Nyr API calls clear a non-empty error buffer on success.
- Treat a major-version change as an ABI break and rebuild consumers.

## Target and operation limits

`nyr_host_target` reports the current compiler target and the capabilities
implemented by this public API: `NYR_CAP_VM` for evaluation and one of
`NYR_CAP_ELF_OBJECT`, `NYR_CAP_COFF_OBJECT`, or `NYR_CAP_MACHO_OBJECT` when the
corresponding writer is available. These do not mirror internal backend flags.
`nyr_emit_object` emits for the host; `nyr_emit_object_for_target` can request
x86-64 ELF, COFF, or Mach-O and AArch64 ELF by target triple. The standalone
API preserves target ABI metadata for AArch64; frontends must classify
aggregate signatures according to AAPCS64 before emitting them. Text assembly
and `nyr_jit_run` currently require a host x86-64 backend.

`nyr_link` links object files and static archives into executables, shared
libraries, or static archives. Host Linux x86-64 executable requests with an
entry symbol use the in-process ELF linker. Other format and target requests
use `clang` with LLD, `wasm-ld`, or `llvm-ar`, selected from the target triple
and output kind. These tools are launched directly without a shell. Set
`NYR_LINK_CLANG`, `NYR_LINK_WASM_LD`, or `NYR_LINK_AR` to select tool paths.
Cross-target linking still requires a compatible linker and any target sysroot
or runtime libraries; pass library files as ordinary inputs. Wasm shared
libraries are rejected. For non-native ELF, COFF, Mach-O, and Wasm executable
requests, `entry_symbol` is the raw platform linker entry symbol. The native
in-process ELF entry instead follows `nyr_link_elf64_executable`'s no-argument
`i64` convention.

`nyr_host_target` reports object-emission capabilities, not whether external
linker tools are installed. The API does not claim full target coverage,
complete source-language semantics, or LLVM replacement across all ABIs. Use
the evaluator to exercise the supported NYIR subset before object emission; an
interpreter success does not guarantee that a machine encoder supports every
operation.

## Threading

Nyr functions are caller-owned and may be used independently, but callers must
not mutate or destroy a function while another operation reads it. The
compiler/runtime library is not a general reentrant compilation service:
compiler and runtime subsystems still contain process-global state. Do not run
concurrent compiler pipeline invocations in one process. The embedding API
provides raw-word host callbacks, not a general Ny value/GC handle API; the
host remains responsible for object handles and value marshalling.