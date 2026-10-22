<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":20,"summary":"Practical Nytrix examples covering expressions, modules, native builds, and everyday patterns."} -->
# Examples

The checkout keeps runnable examples under `etc/projects/` and focused runtime
checks under `etc/tests/`.

## Runnable Projects

| Area | Files |
| --- | --- |
| Terminal | [conway.ny](../../etc/projects/conway.ny), [matrix.ny](../../etc/projects/matrix.ny), [ant.ny](../../etc/projects/ant.ny) |
| OS | [args.ny](../../etc/projects/args.ny), [server.ny](../../etc/projects/server.ny), [ffi.ny](../../etc/projects/ffi.ny), [sound.ny](../../etc/projects/sound.ny) |
| UI | [term.ny](../../etc/projects/ui/term.ny), [input.ny](../../etc/projects/ui/input.ny), [monitor.ny](../../etc/projects/ui/monitor.ny), [engine.ny](../../etc/projects/ui/engine.ny), [editor.ny](../../etc/projects/ui/editor.ny) |

Run one directly:

```bash
ny etc/projects/conway.ny
```

Build a native executable:

```bash
ny -o build/conway etc/projects/conway.ny
```

Build the browser Wasm runner:

```bash
./make web-demos
```

The generated `build/web/demos/index.html` is a compact WebGL-backed wasm runner.
Load a `.wasm` file from the page, or add explicit browser-ready entries to
`etc/assets/website/wasm/demos.json`.

## Focused Checks

Focused checks under `etc/tests/` exercise one language surface or runtime
boundary in isolation:

| Topic | Files |
| --- | --- |
| ADTs and matching | [adt.nshape](../../etc/tests/lang/types/aggregates/adt.nshape), [match.nshape](../../etc/tests/lang/patterns/match.nshape) |
| Concurrency and threads | [concurrency.nshape](../../etc/tests/lang/concurrency/concurrency.nshape), [thread.nshape](../../etc/tests/lang/concurrency/thread.nshape) |
| Comptime | [comptime.nshape](../../etc/tests/lang/comptime/comptime.nshape), [table.nshape](../../etc/tests/lang/values/table.nshape) |
| Native boundary | [ffi.nshape](../../etc/tests/lang/interop/ny/ffi.nshape), [extern.nshape](../../etc/tests/lang/interop/ny/extern.nshape), [asm.nshape](../../etc/tests/lang/interop/ny/asm.nshape) |
| Ownership and safety | [ownership.nshape](../../etc/tests/lang/memory/ownership.nshape), [safe.nshape](../../etc/tests/lang/memory/safe.nshape), [memory.nshape](../../etc/tests/lang/memory/memory.nshape) |

Run a focused test by pattern:

```bash
ny test --pattern comptime
ny test --pattern ownership
```

## Benchmarks

Bench examples live under `etc/tests/bench/`. The `.nshape` files are harness
fixtures, not standalone Nytrix programs. Run them through the benchmark
runner:

```bash
ny perf
ny test --pattern sieve
```

Use the benchmark guide for compiler flags, warmups, checksums, and comparative
measurements; a single fixture run is only a correctness check.

## Related

- [Start](start.md) for the first file.
- [Programs](programs.md) for script and module shape.
- [Testing](testing.md) for test commands.
- [Performance](performance.md) for measurement discipline.
