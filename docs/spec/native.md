# Native boundary

Rules for layouts, extern blocks, pointers, handles, strings, ownership, and
ABI behavior at native boundaries.

## Running native code

Use `--native-only` to force the native execution path. Use `-run` or `-o` for
a native executable. Use the default mode while iterating unless native-only
behavior itself is what you are checking.

Unsupported native shapes fail with a diagnostic — they never silently fall
back to a different execution mode.

## Portable artifacts

Save a validated program artifact and run it later:

```bash
ny --nyir-dump-bin=build/cache/program.nyir myprog.ny
ny --nyir-run-bin=build/cache/program.nyir
```

Capacity is checked before output is written. A program is never silently
truncated.

## Layouts

```ny
layout Name {
   Type field,
   Type field2
}
```

Field order is part of the ABI. Reordering fields changes the ABI.

## Extern blocks

```ny
extern "library" {
   fn symbol(Type arg) Type
}

extern {
   fn process_symbol(Type arg) Type
}
```

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
3. Max depth is 16. Recursive includes inherit the parser's typedef, tag, and
   macro state.

### Supported C constructs

**Types:** `void`, `_Bool`/`bool`, `char`, `short`, `int`, `long`, `float`,
`double`, `long double`, `_Complex`/`_Imaginary` (base type), `_BitInt(N)`,
`signed`/`unsigned`, `const`, `volatile`, `restrict`, `_Atomic`.

**Declarators:** pointers, arrays (flexible and unknown-size), function
parameters (variadic), deep function pointer nesting, typedefs.

**Declarations:** `extern`, `static`, `inline`; struct, union, enum with
fields, bitfields, anonymous members, and nested definitions;
`#pragma pack(push, N)` / `#pragma pack(pop)`.

**Attributes:** `packed`, `aligned(N)`, `unused`, `deprecated`, `weak`,
`format(...)`, `visibility(...)`, `alloc_size(...)`, `const`, `pure`,
`noreturn`, `malloc`, `warn_unused_result`, `vector_size(...)`, `__declspec`,
`_Alignas`, `_Static_assert`, `__asm__`, `__extension__`.

**Preprocessor:** object-like and function-like `#define` with `__VA_ARGS__`
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

`*T` is an addressable pointer to `T`. `handle` is an opaque native scalar —
not a pointer unless the API documents the conversion.

## Layout helpers

```ny
__layout_size("Name")
__layout_align("Name")
__layout_offset("Name", "field")
store_layout(ptr, "Name", values...)
load_layout(ptr, "Name", "field")
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

## Inline assembly

```ny
asm("mov $1, $0", "=r,r", value)
intrinsic("ctpop.i64", value)
```

`asm` emits target-specific code. `intrinsic` resolves a backend intrinsic.
Both are backend-specific; use ordinary operations when source must run
everywhere.

## Machine form

NYIR lowers to typed machine form before target emitters. The target descriptor
owns register-assignment tables; machine form must use those tables rather than
embedding a calling convention. Scalar calls beyond the register set place
overflow arguments in aligned stack slots.

- `--native-oracle-per-pass`: compare interpreter and native result after
  every optimization pass.
- `--native-tv-seed[=N]`: translation-validation seed for pure i64 scalar NYIR.
- `--nyir-dump-cfg`: print reconstructed blocks, predecessors, successors.
- `--nyir-dump-raw`: deterministic before/after dump per pass.
- `--nyir-verify`: enable verifier after every pass (useful for CI).
- `--nyir-disable-pass=NAME`, `--nyir-stop-after=NAME`: diagnostic controls.

## Strings and bytes

FFI text handling is a boundary. APIs document whether strings are managed
Nytrix text, UTF-8 bytes, null-terminated native strings, or raw buffers with
explicit length. These forms are not interchangeable.

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

## Unity build boundaries

The compiler uses unity builds (single translation units that `#include`
other `.c` files) for faster compilation:

- **Runtime** (`src/rt/init.c`): 13 modules — `ast.c`, `bigint.c`,
  `core.c`, `simmd.c`, `ffi.c`, `ffigates.c`, `gc.c`, `math.c`,
  `bigfloat.c`, `memory.c`, `os.c`, `proof.c`, `string.c`.
- **Pipeline** (`src/wire/pipe/init.c`): compilation and bundling stages.
- **C frontend** (`src/code/fficlang.c`): includes `lex.c` and `parse.c`.

Each module must be self-contained. Changes to a module must not break other
modules in the same unity build.

## Related

- [types.md](types.md) — pointer and handle type forms.
- [runtime.md](runtime.md) — ownership and resource scopes.
- [native.md](../learn/native.md) — practical FFI checks.
