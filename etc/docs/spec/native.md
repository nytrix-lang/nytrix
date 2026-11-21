<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":320,"summary":"The language and ABI contract for native compilation, linking, targets, and unsupported shapes."} -->
# Native

Rules for layouts, extern blocks, pointers, handles, strings, ownership, and
ABI behavior at native boundaries. To compile C translation units, see the
[NYC C compiler](nyc.md).

## Contents

- [Execution routes and target support](#running-native-code)
- [Layouts, declarations, and header imports](#layouts)
- [Pointers, ABI, and ownership](#pointers-and-handles)

## Running native code

Use `--native-only` to force the native execution path. Use `--run=aot` or
`--output` for a native executable. Use the default mode while iterating unless native-only
behavior itself is what you are checking.

For a source file, `--native-only` selects the in-memory native JIT unless
`--run=aot`, `--output=PATH`, or `--run=none` selects an AOT or compile-only
route. Program output appears only when the source prints. The low byte of the
top-level result becomes the process exit status. Use `--output=PATH` to save
an executable or `--run=aot` to compile and execute a native artifact.

Unsupported native shapes fail with a diagnostic; they never silently fall
back to a different execution mode.

## NYIR artifacts and native objects

On x86-64, save a validated binary NYIR bundle and run it later in the
debug VM:

```bash
ny --native-precompile=build/cache/program.nyrb myprog.ny
ny --nyr-run-bin=build/cache/program.nyrb
```

The bundle also contains the native data tables needed by its lowered code.
`--emit=nyr` writes text, not a reusable binary. Build a native executable
without reparsing the source:

```bash
ny --native-from-nyr=build/cache/program.nyrb --output=build/cache/program
```

The compiler links the Nytrix runtime. External libraries are not recorded in
the bundle; pass their `-L` and `-l` flags when building. Select another
supported target with `--native-backend` and `--host-triple` when needed.
Native object support remains target- and shape-dependent; unsupported output
fails with a diagnostic.

New bundles use NYIP v2. Older v1 bundles remain runnable in the VM but do not
contain the native data tables required for object emission. Bundle capacity is
checked before output is written; a program is never silently truncated.

The public NYIR construction, verification, optimization, and native object
API is documented in the [Nyr API reference](nyr.md). Its current
object-emission target limits are narrower than the full in-tree native backend.

## Layouts

> **Note:** `Name` and `Type` are placeholders; this is not a complete layout
> declaration.

```ny
layout Name {
   Type field,
   Type field2
}
```

Field order is part of the ABI. Reordering fields changes the ABI. On the LLVM
route, a by-value FFI parameter or result requires a fully materialized LLVM
type matching the layout; unresolved layout types are rejected rather than
represented by an empty struct. Pointer parameters may use incomplete layouts.

## Extern blocks

> **Note:** The library and symbol names are placeholders. Use concrete ABI
> types and link the named library in a complete program.

```ny
extern "library" {
   fn symbol(Type arg) Type
}

extern {
   fn process_symbol(Type arg) Type
}
```

This declaration runs only when the named library exports the requested
symbol with the declared ABI and the build links it. The syntax itself does
not provide a symbol. Native examples need a build command that links the
selected library.

A bare `extern` names symbols from the current process. The library string
selects which linked or loaded library provides the symbol.

## Header imports

`#include` imports declarations from C headers:

```ny
#include <stdlib.h>
#include "./ffi.h"
```

An optional namespace alias exposes declarations under that name:

```ny
#include <stdlib.h> as "c"
c.malloc(64)
```

Without `as`, constants, macros, layouts, and non-conflicting functions are
available directly. Existing Nytrix names win on collisions.

### C frontend selector

| `--c-frontend` | Behavior |
|---|---|
| *(default)* | Internal frontend for diagnostics; libclang does lowering |
| `nytrix` | Internal frontend does all lowering; no libclang fallback |
| `libclang` | Always use libclang; skip the internal frontend |

### Include resolution

1. `"path"` resolves relative to the current file.
2. `<path>` searches `/usr/include`, `/usr/local/include`, and platform paths.
3. Max depth is 64. Recursive includes inherit the parser's typedef, tag, and
 macro state.

### Supported C header constructs

`#include` imports declarations into Nytrix; it does not compile C function
bodies. See [NYC’s C language subset](nyc.md#c-language-subset) for that route.

Types: `void`, `_Bool`/`bool`, `char`, `short`, `int`, `long`, `float`,
`double`, `long double`, `_Complex`/`_Imaginary` (base type), `_BitInt(N)`,
`signed`/`unsigned`, `const`, `volatile`, `restrict`, `_Atomic`.

Declarators: pointers, arrays (flexible and unknown-size), function
parameters (variadic), deep function pointer nesting, typedefs.

Declarations: `extern`, `static`, `inline`; struct, union, enum with
fields, bitfields, anonymous members, and nested definitions;
`#pragma pack(push, N)` / `#pragma pack(pop)`.

Attributes: `packed`, `aligned(N)`, `unused`, `deprecated`, `weak`,
`format(...)`, `visibility(...)`, `alloc_size(...)`, `const`, `pure`,
`noreturn`, `malloc`, `warn_unused_result`, `vector_size(...)`, `__declspec`,
`_Alignas`, `_Static_assert`, `__asm__`, `__extension__`.

Preprocessor: object-like and function-like `#define` with `__VA_ARGS__`
expansion, `#undef`, `#if`/`#ifdef`/`#ifndef`/`#elif`, `#include` with
recursive resolution, `__has_include`, `__has_builtin`, predefined platform
macros.

### Unsupported

- Function definitions (prototypes only)
- Statements, expressions, compound literals, designated initializers
- `_Generic` selections

### C-to-Nytrix type mapping

| C type | Nytrix | C type | Nytrix |
|---|---|---|---|
| `void` | `void` | `_Bool`/`bool` | `u8` |
| `char` | `i8`/`u8` | `short` | `i16`/`u16` |
| `int` | `i32`/`u32` | `long` | `i64`/`u64` |
| `float` | `f32` | `double` | `f64` |
| `long double` | `f64` | `enum` | `i32` |
| pointers | `ptr` | function pointers | `fnptr` |
| struct/union | named type | arrays | `ptr` |

Use `&value` when a C function expects an out pointer:

```ny
#include <sys/time.h>
mut timeval tv = timeval(0, 0)
gettimeofday(&tv, nil)
```

`NULL` lowers to `nil`/`0`.

## Pointers and handles

`*T` is an addressable pointer to `T`. `handle` is an opaque native scalar -
not a pointer unless the API documents the conversion.

## Calling convention

Typed native scalar integers use the Nytrix `i64` value representation.
In x86-64 NYIR emission, `u8`/`bool` parameters are read from the ABI
argument's low byte and zero-extended; `i8` parameters are sign-extended before
NYIR operations. This applies to register and stack arguments. Other scalar
integers and `f64` retain their existing raw value representation; dynamic
values keep their tagged representation through `any` boundaries. Layout
values cross by field in declaration order, which is part of the ABI. Function
pointers use `fnptr`. Variadic foreign calls follow the target calling
convention and pass arguments positionally; the exact varargs behavior belongs
to the selected backend and is checked at the boundary.

Foreign declarations require a matching symbol and native link configuration;
the REPL does not link arbitrary external libraries.

```ny
extern "library" {
   fn host_call(int a, f64 b) int
}

def out = host_call(1, 2.0)
```

A minimal native program checks a C-library call through its declared ABI.
Save the source as `ffi.ny` and run it on a host with the C runtime available:

```bash
ny --run=aot ffi.ny
```

```ny
use core

extern "c" {
   fn abs(int value) int
}

#main {
   assert_eq(abs(-42), 42, "libc boundary")
}
```

For the portable WebAssembly backend, scalar `extern "c"` calls import from
the `env` module. Parameters and results may be `i64`, `f32`, or `f64`. The
host must provide matching exports. Unsupported runtime helpers and
aggregate/vector signatures are rejected during emission.

## Layout helpers

> **Note:** `Name`, `storage`, and field values are placeholders; helper calls
> require a matching declared layout.

```ny
__layout_size("Name")
__layout_align("Name")
__layout_offset("Name", "field")
store_layout(storage, "Name", first_value, second_value)
load_layout(storage, "Name", "field")
```

Raw load/store helpers:

| Helper | Use |
|---|---|
| `load8`, `store8` | Raw bytes |
| `load16`, `load32`, `load64` | Tagged int/value |
| `load64_i`, `store64_i` | Typed integer |
| `load64_h`, `store64_h` | Handle/pointer |
| `load32_f32`, `load64_f64` | Float read |
| `store32_f32`, `store64_f64` | Float write |

Tag and word introspection: `__tagof(value)` returns the value's runtime
tag. Raw integers, floats, and `cstr` values resolve at compile time;
pointers, containers, and unknown shapes call the runtime. `__runtime_tag("Name")`
returns the tag of a declared runtime type name, so tests can pin tower
behavior with equality against the same helper:

```ny
assert(__tagof(a) == __runtime_tag("bigfloat"), "runtime tag")
assert(__tagof(promoted_add) == __runtime_tag("bigint"), "add overflow promotion")
```

`__handle_valid(h)` reports whether a handle refers to memory the runtime
knows: a typed-buffer handle, a dictionary map, or a readable heap
allocation. It returns false for `0` and unknown addresses. These helpers
exist for diagnostics, comptime reflection, and fixture assertions;
application code should use type predicates and conversion APIs instead of
tag words. The fixtures `etc/tests/lang/values/bigfloat.nshape`,
`etc/tests/lang/values/bigint-pidigits.nshape`, and
`etc/tests/lang/types/inference/stale_handle_validation.nshape`
verify these behaviors.

## Inline assembly

```ny
asm("mov $1, $0", "=r,r", value)
intrinsic("ctpop.i64", value)
```

`asm` emits target-specific code. `intrinsic` resolves a backend intrinsic.
Both are backend-specific; use ordinary operations when source must run
everywhere.

## Native target support tiers

Backend names do not imply equal production maturity. The source tree uses
these support tiers:

| Tier | Backends | Contract |
| --- | --- | --- |
| Primary native | `x86_64`, `aarch64` | Main native architecture families, with machine-form lowering and target-specific object infrastructure; feature coverage still depends on the target and source shape. |
| Secondary native | `x86`/i386, `arm` | Target/ABI lowering with narrower optimization, object, and ABI coverage than the primary tier. |
| Bring-up / debug | `riscv`, `bpf`, `mips`, `powerpc`, `avr` and the portable NYIR text emitters | Intended for bring-up, cross-checking, and diagnostics; not a promise of host-quality native-only parity. |
| Browser/Wasm | `wasm` | Separate browser/WebAssembly execution contract; do not infer native object/JIT parity from the backend selector. |

Capability checks determine support for each object format, JIT path, machine
feature, and ABI form. Unsupported requests must produce diagnostics.

## Machine form

NYIR lowers to typed machine form before target emitters. The target descriptor
owns register-assignment tables; machine form must use those tables rather than
embedding a calling convention. Scalar calls beyond the register set place
overflow arguments in aligned stack slots.

- `--nyr-check=oracle-per-pass`: compare interpreter and native result after
 every optimization pass.
- `--nyr-check=tv-seed=N`: translation-validation trials for pure i64 scalar
 NYIR after each optimization pass. Translation validation is off unless this
 flag is supplied; the flag without `=N` selects `16` trials, and `N=0` turns
 it back off.
- `--dump=nyr`: print optimized NYR.
- `--inspect`: show optimized IR and optimizer pass summaries.
- `--nyr-dump=cfg,raw,stats,passes`: request CFG, before/after snapshots,
  instruction summaries, and pass detail. `--inspect` is the concise
  inspection preset.
- `--nyr-check=verify`: enable verifier after every pass (useful for CI).
- `--nyr-pass=disable:NAME`, `--nyr-pass=stop-after:NAME`: diagnostic controls.

### Whole-program LLVM optimization

`--opt=lto` preserves optimization after independently compiled module bitcode
is linked into the program module. Pair it with an optimization level, for
example `ny --opt-level=3 --opt=lto --output=app main.ny`.

For a selected native backend, the same flag selects the aggressive
whole-program NYIR pipeline. It raises that pipeline to its highest level and
enables its interprocedural small-function inlining before machine lowering.
It is not LLVM bitcode LTO, and unsupported native shapes still fail with their
own diagnostic.

## Strings and bytes

FFI text handling is a boundary. APIs document whether strings are managed
Nytrix text, UTF-8 bytes, null-terminated native strings, or raw buffers with
explicit length. These forms are not interchangeable.

## FFI trust boundary

Foreign declarations are a trust boundary. Unknown foreign calls are assumed
to have externally observable effects and may read or mutate reachable memory.
The optimizer may refine that only from explicit, trusted declaration metadata
or contracts that the program has chosen to rely on. Imported C `const` and
`restrict` information is descriptive input; it is not permission to invent a
Nytrix ownership or noalias guarantee when the call boundary cannot justify it.

Use three labels when documenting an FFI assumption:

- **unknown** — no ownership/effect refinement is trusted; keep conservative
  effects and aliasing;
- **checked** — a Nytrix wrapper performs a runtime/structural check before
  exposing a narrower safe value; the optimizer may rely only on facts proved
  after that check;
- **trusted** — the declaration or wrapper asserts a property that cannot be
  checked cheaply (for example an external lifetime/noalias contract). A wrong
  assertion is a boundary bug and must not be presented as compiler inference.

Low-level code can still use raw `extern` declarations and imported C
declarations directly. Checked/trusted wrappers are an opt-in safety layer, not
a requirement that prevents direct ABI calls.

Pointers returned from foreign code are `unknown` ownership by default. Use an
explicit ownership contract when the API guarantees a borrowed, consumed, or
returned-owned lifetime, and keep the raw declaration available when no safe
contract can be stated. Callback userdata follows the same rule: the caller must
keep borrowed userdata alive for every callback invocation, while a transferred
value needs an explicit consuming/owned contract.

Unsafe assumptions should be concentrated at the extern/wrapper boundary. They
should not leak into ordinary optimizer metadata as if they had been proved by
Nytrix itself.

## Ownership

Ownership attributes define who allocates and who frees:

```ny
@returns_owned
fn make_buffer(){ malloc(64) }

@consumes(p)
@releases(p)
fn free_buffer(p) int {
   free(p)
   0
}
```

## Related

- [Types](types.md) - pointer and handle type forms.
- [Runtime](runtime.md) - ownership and resource scopes.
- [Native](../learn/native.md) - practical FFI checks.
