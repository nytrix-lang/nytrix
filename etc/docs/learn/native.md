<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":40,"summary":"Declare native layouts and calls, then verify ABI, ownership, and target support."} -->
# Native interfaces

Treat every native declaration as a boundary with an ABI, lifetime, and error
contract. Native execution is a separate compiler route; it does not make an
unsupported signature portable.

Prerequisites: familiarity with Nytrix [types](../spec/types.md) and the
external library or C header being called.

Skip this guide unless your program crosses a native ABI boundary.

> **Warning:** ABI or ownership mistakes can corrupt memory; verify the actual declaration and lifetime.

## Choose a declaration form

| Need | Form |
| --- | --- |
| Describe a fixed native field layout | `layout` |
| Declare a known function symbol | `extern` |
| Import declarations from a C header | `#include` |
| Explore a symbol dynamically | `core.os.ffi` |

Prefer `extern`, `#include`, and `layout` when the compiler can know the ABI
shape. Use dynamic FFI for experiments or APIs whose function signatures are
not statically available.

This complete example imports C's `sqrt` from `<math.h>` and checks its result:

```ny
use core
#include <math.h> as "c"

def f64 x = 3.0
def f64 y = 4.0
def f64 length = c.sqrt(x * x + y * y)
assert(length > 4.999 && length < 5.001, "C sqrt")
```

The full project adds a descriptive helper and output:

```bash
ny etc/labs/ffi.ny
```

The [complete `ffi.ny` example](../../../etc/labs/ffi.ny) checks that the
computed value crosses the C floating-point argument ABI and that
`sqrt` returns the expected result. The `test_ffi` fixture in
[`etc/tests/lang/interop/ny/ffi.nshape`](../../../etc/tests/lang/interop/ny/ffi.nshape)
also covers a runtime-computed C `sqrt` argument.

## Check the boundary

Before calling native code, establish:

- the library or process that owns the symbol;
- the calling convention and argument/return layout;
- whether pointers are borrowed, consumed, or returned-owned;
- whether text is UTF-8, null-terminated, or passed with an explicit length;
- how failure is reported and who releases resources.

Use `handle` for opaque native resources and pointers only when the API exposes
an address. Do not assume the garbage collector releases native resources.
Wrap ownership and error handling close to the foreign declaration.

## Select and test the route

`ny --run=aot` and `ny --output` build a native executable. `--native-only` requires the
NYIR/native path and reports unsupported shapes rather than falling back through
LLVM. Target, aggregate ABI, and C frontend coverage vary; check the
[Native specification](../spec/native.md) for the exact boundary.

To build a native executable from a saved NYIR bundle without reopening source,
use `--native-from-nyr=PATH --output=OUTPUT`. The compiler links the Nytrix runtime;
pass `-L` and `-l` for any external libraries.

Test a small caller/callee pair on the actual target and flags. Include the
layout or scalar arguments, the return value, and one failure or cleanup path.
Do not infer support for one ABI from a passing test on another.

## Debugging

Optimized code may move or eliminate source bindings. If a native failure is
hard to locate, reproduce with lower optimization and inspect the NYIR and
native artifacts for the failing boundary. See
[Execution routes](status.md#execution-routes), [Testing](testing.md), and
[Troubleshooting](troubleshooting.md).

Next:

- [Native specification](../spec/native.md) — exact ABI and support rules.
- [Programs](programs.md) — module and import structure.
- [Implementation status](status.md) — check target-specific support.
