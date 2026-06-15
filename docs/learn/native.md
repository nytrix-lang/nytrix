# Native interop

Native interop uses layouts, externs, pointers, handles, ownership rules, and
small boundary checks.

Keep native wrappers explicit. A wrapper states the ABI shape, error shape, and
ownership shape close to the declaration that crosses the boundary.

## Boundary contract

Identify the native contract before writing Nytrix declarations:

| Question | Why |
| --- | --- |
| What library owns the symbol? | Selects the extern block library string. |
| What is the ABI type? | Determines layout fields, pointer type, or handle. |
| Who owns returned memory? | Determines cleanup. |
| Is text null-terminated or length-based? | Determines string/byte conversion. |
| Can the call fail? | Determines result/error handling. |

## Layouts

```ny
layout Pixel {
   u8 r,
   u8 g,
   u8 b,
   u8 a
}
```

Field order matches the native ABI.

For field access in compiled code, use `layout` and the layout helpers. Dynamic
`CStruct` descriptors are convenient while exploring a header, but they resolve
fields through runtime descriptor lookups.

## Externs

```ny
extern "library" {
   fn native_call(int value) int
}

extern {
   fn process_symbol(handle h) int
}
```

Use `extern "library"` when Nytrix should link or load a library. Use bare
`extern` for symbols already available from the current process. `extern ""`
means the same thing, but the bare form is easier to read.

Prefer `extern`, `#include`, and `layout` for code that should behave like
native code. The dynamic `std.os.ffi` helpers are useful for quick symbol
experiments, but compiled declarations give the compiler the ABI shape.

## Header imports

Use `#include` when the C header already describes constants, macros, enum
values, layouts, or function declarations:

```ny
#include <stdlib.h> as "c"
#include "./my_header.h" as ""
```

Aliases keep large system headers namespaced. Unprefixed imports expose
non-conflicting names directly; if a C function collides with a Nytrix symbol,
that C function is skipped so the Nytrix wrapper remains callable. Re-import
with an alias to access the C function under a stable namespace.

Imported C typedef structs become layouts:

```ny
#include <sys/time.h> as ""

mut timeval tv = timeval(0, 0)
gettimeofday(&tv, NULL)
```

`NULL` is accepted as the C null pointer spelling and has the same value as
`nil`/`0` at the native boundary.

## Pointers and handles

Pointers model addresses. Handles model opaque scalar resources. A handle is
pointer-addressable only when the API documents that conversion.

## Text and bytes

Native string APIs vary. Check whether the boundary expects:

- managed Nytrix text
- UTF-8 bytes
- a null-terminated C string
- a pointer plus length

## Ownership contracts

When native code returns owned memory or consumes a handle, write the wrapper
contract next to the boundary:

```ny
@returns_owned
fn allocate_block(){ malloc(64) }

@consumes(p)
@releases(p)
fn release_block(p) int {
   free(p)
   0
}
```

Run ownership checks while developing wrappers:

```bash
ny --safe-mode wrapper.ny
```

In safe mode, raw memory operations on compiler-tracked allocations must prove
their byte range. Scoped buffers make the lifetime explicit:

```ny
with ptr: buf = malloc(16){
   def int off = 4
   assert_compile_range(off, 0, 15, "buffer byte offset")
   store8(buf, 1, off)
}
```

## Checks

Native checks verify one boundary at a time:

```ny
use std.core

layout Pixel {
   u8 r,
   u8 g,
   u8 b,
   u8 a
}

assert_eq(sizeof(Pixel), 4, "pixel abi size")
```

For handles, check creation, one normal operation, and cleanup.

For C headers, regenerate docs or run `ny doc search --symbols name` after the
import path works. That confirms the names users import from the wrapper.

## Related

- [native.md](../spec/native.md) for the exact language boundary.
- [runtime.md](../spec/runtime.md) for ownership and cleanup forms.
- [troubleshooting.md](troubleshooting.md) for native crash triage.
