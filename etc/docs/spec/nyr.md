<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"spec","order":350,"summary":"The versioned C API and the NYR text language for constructing, verifying, optimizing, and executing NYIR."} -->
# Nyr API

> **Note:** Usually needed only to implement a frontend, backend, or NYIR host.

## Contents

- [Construct and execute NYIR](#brainfuck-frontend)
- [Ownership and errors](#ownership-and-errors)
- [Targets and thread-safety](#target-and-operation-limits)
- [NYR text: write NYIR by hand](#nyr-text)

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

`nyr_cfg_build` in `<nyr/ir.h>` constructs CFG topology, reachability,
dominators, and dominance frontiers. It solves immediate dominators by
intersecting ancestor chains in reverse postorder, then materializes the
dominance bitsets. Unreachable blocks have self-only dominance and `idom=-1`;
the entry also has `idom=-1`. Use `nyr_cfg_build_topology` when only block
membership, edges, and reachability are needed.

On Linux x86-64, `nyr_emit_assembly` writes native x86-64 assembly text for a
verified function; pass `"-"` as its path to write to standard output without
creating a file. `nyr_jit_run` executes a verified no-argument `i64`
function in the in-process native JIT, resolving external symbols from the
current process. It is intended for trusted code, not as a sandbox. The JIT
runner currently requires a host x86-64 backend.

`nyr_link` links existing objects and archives. Host Linux x86-64 requests can
use the in-process ELF linker; other requests invoke external LLVM tools.
The [linking reference](linker.md) defines request fields, path selection,
entry-point conventions, and rejected inputs. Linking does not replace the
compiler driver's application build or supply a target runtime automatically.

## Brainfuck frontend

The complete frontend below is also maintained at `etc/labs/brainfuck.ny`.
It includes `<nyr/nyr.h>`, builds NYIR directly with Nyr's constructors,
verifies and optimizes it, emits native assembly, then runs the function in
Nyr's in-process native JIT. Assembly is emitted to standard output before
execution, without creating an assembly file; Brainfuck output follows it. The
emitted function calls `calloc`, `getchar`, and `putchar` as external ABI
symbols resolved by the JIT. Runtime failures produce a nonzero process
status; the successful function return value is not presented as the program's
output.

Run the build and commands from the repository root, where the header search
path and `build/release/libnytrixrt.so` link path used by the example resolve:

```sh
cmake --build build/release --target nyr-backend-ir-acceptance-test nytrixrt ny
printf '++++++++[>++++++++<-]>+.' > /tmp/hello.bf
build/release/ny etc/labs/brainfuck.ny /tmp/hello.bf
build/release/ny etc/labs/brainfuck.ny -c '++++++++[>++++++++<-]>+.'
```

Both commands print the assembly listing first and then write `A`. `-c` takes
Brainfuck source directly; otherwise the first argument is read as a source-file
path. No assembly file is created, and the function's status value is not
printed. To give the Brainfuck program runtime input, pass the source file as
before and pipe input separately:

```sh
printf ',.' > /tmp/echo.bf
printf 'Z' | build/release/ny etc/labs/brainfuck.ny /tmp/echo.bf
```

<!-- ny-doc-include-code: ny etc/labs/brainfuck.ny -->

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

## NYR text

NYR text is a source language for NYIR that you can write by hand. It has
two layers that mix freely in one function:

- A Ny-style layer for everyday code: `fn name(int n) int`, `def` and
  `mut` bindings, `if`/`elif`/`else`, `while`, `for`, expressions.
- An instruction layer, in the spirit of LLVM IR and QBE, for full control:
  SSA values (`%x`), blocks (`@b`), any NYIR opcode by name, explicit
  `phi`, instruction flags (`!u8`, `!volatile`, `!gs`), raw data and
  assembly.

`build/release/nyr` compiles NYR text files:

```sh
nyr run etc/labs/tests/code/intermediate/nyr/tour.nyr [args...]  # build and run; exit status is main's result
nyr build file.nyr -o prog               # executable linked with libc and libm by $CC
nyr check file.nyr                       # parse, type-check and verify only
nyr ir -O2 file.nyr                      # NYIR after optimization, printed as NYR text
nyr asm file.nyr                         # x86-64 assembly
```

`-O0` to `-O3` select the optimization level; the default is `-O2`.
`nyr ir` prints a complete module, including its data as `asm` lines, and
that output runs as it is. The `nyr-text-roundtrip-*` tests check this for
every example. Programs build for Linux x86-64. The API is in `<nyr/text.h>`:
`nyr_text_parse` returns verified `nyr_func_t` functions plus data written by
`nyr_text_write_data`, and `nyr_text_print_function` prints any NYIR
function in this syntax.

### Ny-style layer

```nyr
;; comments start with ;;
extern { fn printf(ptr fmt, ...) i32 }   ;; C functions; `as "sym"` renames
def LIMIT = 100                          ;; top-level constant
def banner = "primes: %ld\n"             ;; read-only string; the name is its address
mut f64 total = 0.0                      ;; mutable global

fn primes(int limit) int {
  def sieve = calloc(limit + 1, 1)       ;; def: immutable, mut: mutable
  mut count = 0
  for (mut i = 2 i <= limit i += 1) {
    if load8(sieve + i) != 0 { continue }
    count++
    for (mut j = i * i j <= limit j += i) { store8(sieve + j, 1) }
  }
  return count
}

pub fn main() int { printf(banner, primes(LIMIT)) return 0 }
```

- **Types.** `int` is a 64-bit word; `ptr`, `bool` and `str` are spellings
  of it. Floats are `f64` and `f32`. Extern results may be `i32`, `u32`,
  `u8` or `u16`, and are sign- or zero-extended to `int`. `void` functions
  return nothing.
- **Functions.** `fn` functions are module-internal (`ny_fn_<name>`); `pub fn`
  and `main` keep their plain C symbol. Parameters are mutable, as in Ny.
- **Statements.** Statements need no separator (`x *= 3 x -= 4` is two
  statements). An operator that starts a line starts a new statement, so
  continue a long expression by ending the line with its operator.
- **Control flow.** `if`/`elif`/`else`, `while`, `for (init cond update)`,
  `for i in lo..hi` (inclusive), `break`, `continue`, `return`, and
  `name:` with `goto name`.
- **Expressions.** Ny's operators: `+ - * / %`, `& | ^ ~ << >>`
  (`>>` is arithmetic), comparisons, `!`/`not`, short-circuit `&&`/`||` and
  `and`/`or`, and `c ? a : b` (only the chosen arm runs). Integer literals take
  the float type of the other operand; any other mix needs `f64(x)`,
  `f32(x)` or `int(x)`. Literals: `0x`, `0b`, `0o`, `1_000`, `'a'`, `1.5`,
  `1.5f` (f32), and strings with `\n \t \0 \xNN` escapes.
- **Builtins.** `load8/16/32/64(addr[, offset])` zero-extend.
  `store8/16/32/64(addr, value[, offset])` truncate. Also `sqrt`, `sin`,
  `cos`, `select(c, a, b)` (both arms run) and `bswap`.

### Instruction layer

Any NYIR instruction is written as `[%dst =] opcode operands [!flags] [-> type]`,
using the opcode names `nyr_op_name` reports (`add.i64`, `cmp.f64`,
`vec4.add.f64`, `select.i64`, `bswap.i64`, ...).

```nyr
fn sum_table(ptr base, int n) int {
@entry
  %b = load.local 0                   ;; parameters are local slots 0..n-1
  %n = load.local 1
  %zero = const.i64 0
  br @loop
@loop
  %i = phi [%zero, @entry], [%i2, @body]     ;; forward references are fine
  %acc = phi [%zero, @entry], [%acc2, @body]
  %more = cmp.i64 lt %i, %n
  br.if %more, @body, @done           ;; optional else target
@body
  %p = add.i64 %b, (%i * 8)           ;; any operand may be a Ny expression
  %acc2 = add.i64 %acc, load64(%p)
  %i2 = add.i64 %i, 1
  br @loop
@done
  ret %acc
}
```

| Operand | Meaning |
|---|---|
| `%name` | An SSA value. Assigned once; it can also read a `def`/`mut` binding of that name. |
| `@name` | A block label. It also starts a block when it begins a line. |
| `$name` | A symbol. A module function's `$name` is its address. Externs and `pub` functions go through a data stub. |
| `42`, `1.5`, `"text"` | A constant, materialized as a value, or the instruction's immediate where the opcode has one: constants, local slots, load/store offsets, `alloca`. |
| `#42` | Always the immediate, for any opcode. |
| `name`, `(a + b)`, `f(x)` | A Ny-style expression. |

- **Comparisons** take a condition: `cmp.i64 lt %a, %b` with `eq ne lt le gt ge`.
- **Calls** are `call $sym(args)`, or `call %fnptr(args)` for an indirect call.
  An extern's signature supplies `!extern`, `!variadic` and the result type.
  `-> f64` sets it explicitly.
- **Memory**: `load.i64 %p, 8` and `store.i64 %p, %v, 8`. Flags select
  narrower access or a segment: `!u8`/`!byte`, `!u16`, `!u32`, `!volatile`,
  `!fs`, `!gs`.
- **Local slots**: `load.local N`, `store.local N, %v`, `addr.local N`.
- **Other flags**: `!noalias`, `!inbounds`, `!nooverflow`,
  `!nonpreemptible`, `!sret` and `!assume`. `!N` sets raw flag bits, and
  `const.f64 0x7ff8000000000000 !bits` writes the exact bit pattern.

### Data and assembly

```nyr
data table align 16 = { i64 10, 20, 30, 40 }        ;; the name is its address
data msg = { "hi\n", u8 0 }
data handlers section ".data.rel.ro" = { ptr twice, ptr thrice }
pub data exported = { i32 1, 2, f64 0.5, zero 16 }  ;; pub: also a plain global symbol

asm {                                ;; copied verbatim into the module's assembly
  "\t.text"
  "\t.globl nyr_mul_add"
  "nyr_mul_add:"
  "\tleaq (%rsi,%rdi,4), %rax"
  "\tret"
}
extern { fn nyr_mul_add(int a, int b) int }
```

Item types are `i64`/`u64`/`ptr` (8 bytes), `i32`/`u32` (4), `i16`/`u16` (2),
`i8`/`u8` (1), and `f64`/`f32`. A type applies until the next one.
`zero N` adds N zero bytes, a string adds its bytes, and a bare name adds the
address of a function or data item. `etc/labs/tests/code/intermediate/nyr/lowlevel.nyr` exercises
all of this.

Diagnostics name the file, line and column, and quote the source with a
caret:

```text
bad-immutable.nyr:4:3: error: 'x' is immutable; declare it with 'mut'
    x = 2
    ^
```

## Related

- [Linking](linker.md) for object and archive link requests.
- [Embedding API](nytrix.md) for host-call resolution and runtime lifecycle.
- [Pipeline](pipeline.md) for compiler route boundaries.