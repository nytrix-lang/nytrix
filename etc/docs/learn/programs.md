<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":160,"summary":"Build and run a program, then choose a focused workflow."} -->
# Programs

A source file can be a script, a module, or both: use a script for a standalone
program and a module when other files need exported names.

Prerequisites: the [first-program guide](start.md) or an installed Nytrix
compiler. The tree below is schematic; these files are examples, not required
repository paths.

```text
project/
├── main.ny       # application entry point
└── mathx.ny      # reusable module
```

## Script

Top-level statements run when a script is executed:

```ny
use core

def name = "Nytrix"
assert_eq("hello, " + name, "hello, Nytrix", "greeting")
print("hello, " + name)
```

Run it with `ny hello.ny`.

## Module with an entry point

A module lists the names available to import. Put direct startup work in
`#main`; importing the module does not run that block.

```ny
use core

module mathx(double)

fn double(int value) int {
   value * 2
}

#main {
   assert_eq(double(21), 42, "module entry")
}
```

The exact export and entry-point rules are in [Units](../spec/units.md).

## Imports

Import a module namespace or selected exported names. Aliases make the owner
visible at each call site.

```ny
use core.os.net as net
use core.math.parse.data.json as json
use mathx (double)
use local:debug
```

Imports resolve before ordinary declarations. A missing module, export, or
profile is a compile-time diagnostic.

For remote URL imports, append `#sha256=` and a 64-digit SHA-256 digest to pin
the downloaded source. The compiler checks the digest on download and on every
cache read. `NYTRIX_REMOTE_RELOAD=1` fetches a fresh copy and replaces the
cached file only after the download and digest check succeed.

## Project layout

Keep reusable code in modules and application startup in one entry point. Use
`ny new <dir>` for a starter project, then add modules around clear public
interfaces. `module name { export profile(...) }` can publish a named profile
when the same module needs multiple public surfaces.

Examples under [`etc/labs/`](../../../etc/labs/) are complete programs.

## Runnable projects

The checkout keeps runnable examples under `etc/labs/` and focused runtime
checks under `etc/labs/tests/`.

| Area | Projects |
| --- | --- |
| Terminal | [conway.ny](../../../etc/labs/conway.ny), [matrix.ny](../../../etc/labs/matrix.ny), [ant.ny](../../../etc/labs/ant.ny) |
| OS | [args.ny](../../../etc/labs/args.ny), [server.ny](../../../etc/labs/server.ny), [ffi.ny](../../../etc/labs/ffi.ny), [sound.ny](../../../etc/labs/sound.ny) |
| UI | [term.ny](../../../etc/labs/term.ny), [input.ny](../../../etc/labs/input.ny), [monitor.ny](../../../etc/labs/monitor.ny), [engine.ny](../../../etc/labs/engine.ny), [editor.ny](../../../etc/labs/editor.ny) |

Run a project directly, build it as a native executable, or build the browser
demo runner:

```bash
ny etc/labs/conway.ny
ny --output=build/conway etc/labs/conway.ny
./make web-demos
```

The generated `build/web/demos/index.html` is a compact WebGL-backed Wasm
runner. Load a `.wasm` file from the page, or add browser-ready entries to
`etc/assets/website/wasm/demos.json`.

## Focused checks

Fixtures under `etc/labs/tests/` exercise one language surface or runtime boundary:

| Topic | Fixtures |
| --- | --- |
| ADTs and matching | [adt.nshape](../../../etc/labs/tests/lang/types/aggregates/adt.nshape), [match.nshape](../../../etc/labs/tests/lang/patterns/match.nshape) |
| Concurrency and threads | [concurrency.nshape](../../../etc/labs/tests/lang/concurrency/concurrency.nshape), [thread.nshape](../../../etc/labs/tests/lang/concurrency/thread.nshape) |
| Comptime | [comptime.nshape](../../../etc/labs/tests/lang/comptime/comptime.nshape), [table.nshape](../../../etc/labs/tests/lang/values/table.nshape) |
| Native boundary | [ffi.nshape](../../../etc/labs/tests/lang/interop/ny/ffi.nshape), [extern.nshape](../../../etc/labs/tests/lang/interop/ny/extern.nshape), [asm.nshape](../../../etc/labs/tests/lang/interop/ny/asm.nshape) |
| Ownership and safety | [ownership.nshape](../../../etc/labs/tests/lang/memory/ownership.nshape), [safe.nshape](../../../etc/labs/tests/lang/memory/safe.nshape), [memory.nshape](../../../etc/labs/tests/lang/memory/memory.nshape) |

Run a focused test by pattern:

```bash
ny test --pattern comptime
ny test --pattern ownership
```

Bench fixtures live under `etc/labs/tests/bench/`; run them through `ny perf` or
`ny test --pattern sieve`. A single fixture run checks correctness, while
comparative measurements require the flags and checksums in the
[performance guide](performance.md).

[Packages](packages.md) covers external source dependencies.

Next:

- [Units](../spec/units.md) — exact import, export, and entry-point rules.
- [Packages](packages.md) — external source dependencies.
- [Start](start.md) — create and run a first file.
