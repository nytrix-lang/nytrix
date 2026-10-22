<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":40,"summary":"Declare native layouts and calls, then verify ABI, ownership, and target support."} -->
# Native interfaces

Treat every native declaration as a boundary with an ABI, lifetime, and error
contract. Native execution is a separate compiler route; it does not make an
unsupported signature portable.

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

```text
layout Pixel { u8 r, u8 g, u8 b, u8 a }
extern "library" { fn native_call(int value) int }
#include <stdlib.h> as "c"
```

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

`ny -run` and `ny -o` build a native executable. `--native-only` requires the
NYIR/native path and reports unsupported shapes rather than falling back through
LLVM. Target, aggregate ABI, and C frontend coverage vary; check the
[Native specification](../spec/native.md) for the exact boundary.

Test a small caller/callee pair on the actual target and flags. Include the
layout or scalar arguments, the return value, and one failure or cleanup path.
Do not infer support for one ABI from a passing test on another.

## Debugging

Optimized code may move or eliminate source variables. If a native failure is
hard to locate, reproduce with lower optimization and inspect the NYIR and
native artifacts for the failing boundary. See
[Execution routes](routes.md), [Testing](testing.md), and
[Troubleshooting](troubleshooting.md).
