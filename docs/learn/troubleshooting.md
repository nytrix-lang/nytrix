# Troubleshooting

Run the failing command, then identify the owner of the failing name or
boundary. Keep the input small until the error points at one module, one value
shape, or one external resource.

## First checks

```bash
ny --diag-compact --collect-errors file.ny
ny --diag-rich file.ny
ny doc search --symbols missing_name
ny --strict-types file.ny
```

Change one cause at a time: import, type, ownership contract, or external
fixture. Run the same command again after each change.

## Undefined symbol

Cause: the file does not currently import the module that exports the symbol, or
the name is not exported by that module.

Find the owner before changing imports:

```bash
ny doc search --symbols symbol_name
ny doc get module.name
```

Fix shape:

```ny
use std.core
use std.parse.data.json as json
```

A semicolon after a `use` line starts a comment in Nytrix; it is not a
statement terminator.

If the name is module-qualified, use the exported name exactly as documented.
For example, `std.os.thread` exports flat names such as `thread_spawn`, not
`thread.spawn`.

## Unavailable receiver method

Cause: receiver syntax is not available for that value/module pair. Receiver
methods are documented APIs, not automatic aliases for every function.

Call the exported function directly, then use receiver syntax only if the API
page documents it.

```ny
use std.core

assert(str_contains("nytrix", "tri"), "contains")
```

## List size surprise

`list(n)` reserves capacity. It does not initialize `n` elements. Append values
before indexing them as elements. Assign append results back:

```ny
mut xs = []
xs = xs.append(1)
```

## String and byte boundary

Strings are byte-length values. Generic string slicing uses UTF-8 code-point
indices. FFI, socket, and binary parsers may require byte-oriented APIs.

## Type-check failure

Default type checks reject typed contradictions and warn on dynamic cliffs such
as heterogeneous dict literals, unknown member/index access, dynamic
arithmetic, and unrefined `Result` payload use.

Use `--strict-types` when a command line should reject those dynamic cliffs:

```bash
ny --strict-types file.ny
```

See [types.md](../spec/types.md) for compile-time type rules.

## OOM or GC panic

The default runtime uses the manual heap path. The collector is opt-in:

```bash
ny --heap=gc file.ny
NYTRIX_GC_NURSERY_SIZE=64M NYTRIX_GC_TENURED_SIZE=512M ny --heap=gc file.ny
```

If a GC run panics or reports out of memory, reduce the input first, then tune
`NYTRIX_GC_NURSERY_SIZE`, `NYTRIX_GC_TENURED_SIZE`, or
`NYTRIX_GC_LOS_THRESHOLD`. Size values accept bytes, `K`, `M`, or `G`.

## Package not found

Check install roots, then search the same repositories that `ny get` uses:

```bash
ny pkg path
ny pkg info
ny pkg repo list
ny pkg search package_name
ny pkg search --interactive package_name
```

Confirm that the dependency name matches the import name.

## REPL differs from file

Put the snippet in a file and run diagnostics:

```bash
ny --diag-compact --collect-errors file.ny
```

Files make imports, top-level state, and parse errors explicit.

## Doc search misses a symbol

Check whether you are searching prose or exported APIs:

```bash
ny doc search --docs topic
ny doc search --symbols name
```

Search by module path when the symbol name is broad:

```bash
ny doc get std.os.ui.render
ny doc get std.parse.data.json
```

## Network timeout

Set context explicitly:

```ny
use std.os.net as net

net.context({"timeout_ms": 3000, "log_level": "debug", "color": false})
```

For tubes, inspect `transcript_text(io)`.

## Native crash

Check layout field order, pointer lifetime, handle ownership, and text/byte
conversion. Native handles are not automatically pointer-addressable.

See [native interop](native.md) and the [native boundary spec](../spec/native.md).

Native crash reducers normally check three boundaries: ABI size/alignment, one
successful call, and one cleanup path. That makes the failing boundary visible
without depending on the whole application.

For command families, use [tooling.md](tooling.md). For executable checks, use
[testing.md](testing.md).
