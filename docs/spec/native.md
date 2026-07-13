# Native boundary

Native boundary rules cover layouts, extern blocks, pointers, handles, strings,
ownership, and ABI behavior.

## Native backend capability boundary

`--native-only` selects the host encoder on x86-64 and AArch64. Both paths
lower NYIR, encode machine code, relocate an in-memory W^X image, and support
persistent REPL definitions without constructing LLVM state. AArch64 ELF64
objects and internal-link runtime probes cover AAPCS64 scalar/f32/f64 calls,
eight integer arguments, local pointer memory, branches, and signed div/mod.

i386 has its separately tested ELF32 object/link slice. ARM, RISC-V, MIPS,
PowerPC, BPF, AVR, and WebAssembly target names are assembly/inspection paths
unless their capability record explicitly enables an object format. An
assembly-only target fails object or JIT requests instead of invoking a hidden
fallback. General C aggregate classification remains target-specific: the
documented nested INTEGER/SSE/MEMORY aggregate ABI is the x86-64 System V
boundary, not an implied cross-ABI promise.

## Layouts

```ny
layout Name {
   Type field,
   Type field2
}
```

Layout field order is part of the ABI. A layout describes memory shape at a
native boundary. Reordering fields changes the ABI.

## Extern blocks

```ny
extern "library" {
   fn symbol(Type arg) Type
}

extern {
   fn process_symbol(Type arg) Type
}
```

An extern block declares native symbols. The library string selects the linked
or loaded library. A bare extern block names symbols already available from the
current process; `extern ""` is accepted as the older spelling.

## Header imports

`#include` imports declarations from C headers through the selected C frontend.
The Nytrix-owned frontend handles the supported preprocessor, declaration,
layout, callback, and aggregate-ABI subset; unsupported shapes fail explicitly
unless an explicitly selected adapter is responsible for that import.

```ny
#include <stdlib.h> as "c"
#include "./ffi.h"
```

An explicit namespace alias exposes declarations through that namespace. An
unaliased import (or `as ""`) exposes constants, macros, enum values, layouts,
and non-conflicting functions directly; it does not create an implicit
namespace. Existing Nytrix names remain authoritative on collisions; an
explicit namespace alias makes the C declaration separately addressable.

Header imports follow transitive includes. Object-like integer macros,
shift/bitwise macro expressions, enum constants, typedef structs, and
pointer-bearing structs become visible when the selected frontend supports
their declarations. The internal frontend resolves installed system include
roots and recovers past unsupported system declarations while retaining strict
diagnostics for project headers.

Imported typedef structs are available as layout constructors. Use `&value`
when a C function expects an out pointer:

```ny
#include <sys/time.h> as ""

mut timeval tv = timeval(0, 0)
gettimeofday(&tv, NULL)
```

`NULL` is accepted as the C null pointer spelling and lowers to `nil`/`0`.

For production native calls, prefer `extern`, `#include`, and `layout`. The
dynamic `std.os.ffi` helpers are useful at the REPL or for exploratory probes,
but they trade compile-time ABI knowledge for runtime descriptors and capped
dynamic dispatch.

## Pointers

`*T` is an addressable pointer to `T`. Pointer values model memory addresses.
They require correct lifetime, alignment, and element type.

## Handles

`handle` is an opaque native scalar. A handle is not a pointer unless the API
documents that conversion. Handle cleanup uses the close, destroy, or release
function documented by the owning API.

## Layout helpers

Runtime layout helpers expose ABI metadata and field access:

```ny
__layout_size("Name")
__layout_align("Name")
__layout_offset("Name", "field")
store_layout(ptr, "Name", values...)
load_layout(ptr, "Name", "field")
```

Typed raw loads and stores include integer, float, bool, pointer, and handle
forms such as `load8`, `load16`, `load32`, `load32_f32`, `load64_f64`,
`load64_h`, `store8`, `store32`, and `store_layout`.

Raw memory helpers use byte offsets. The public wrappers default the offset to
zero:

| Helper | Shape | Use |
| --- | --- | --- |
| `load8(p, i=0)` / `store8(p, v, i=0)` | byte | Raw bytes. |
| `load16`, `load32`, `load64` | tagged int/value load | Nytrix scalar slots and raw integer data. |
| `load64_i(p, i=0)` / `store64_i(p, v, i=0)` | `int` view | Typed integer reads and writes. |
| `load64_h(p, i=0)` / `store64_h(p, v, i=0)` | handle view | Pointer or handle-sized native values. |
| `load32_f32`, `load64_f64` | float view | Native float fields. |
| `store32_f32`, `store64_f64` | float store | Native float fields. |

The runtime intrinsics behind these wrappers use `(p, offset, value)` store
order. User code should call the `std.core` wrappers above.

`std.os.ffi.CStruct` is a dynamic descriptor form. It is intentionally flexible
and slower than `layout`; compiled layout access resolves offsets directly.

## Inline assembly and intrinsics

```ny
asm("mov $1, $0", "=r,r", value)
llvm("ctpop.i64", value)
llvm("llvm.cttz.i64", value, false)
```

`asm` lowers inline assembly for the active backend and target architecture.
`@naked` functions can contain complete target-specific assembly bodies. The
`llvm` builtin calls LLVM intrinsics; the `llvm.` prefix is optional for
intrinsic names.

## Strings and bytes

FFI text handling is a boundary. APIs document whether strings are:

- managed Nytrix text
- UTF-8 bytes
- null-terminated native strings
- raw buffers with explicit length

These forms are not interchangeable.

## Ownership

Ownership attributes and API docs define who allocates and who frees native
values. A wrapper can provide scoped cleanup, but raw native values require
explicit ownership handling.

When a native call returns owned memory or a handle, model that in the wrapper
with ownership contracts and cleanup:

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

- [types.md](types.md) for pointer and handle type forms.
- [runtime.md](runtime.md) for ownership and resource scopes.
- [native.md](../learn/native.md) for practical FFI checks.
