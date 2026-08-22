<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":20,"summary":"Practical Nytrix examples covering expressions, modules, native builds, and everyday patterns."} -->
# Examples

The checkout keeps runnable examples under `etc/projects/` and focused runtime
checks under `etc/tests/`.

## Runnable Projects

| Area | Files |
| --- | --- |
| Terminal | [conway.ny](../../etc/projects/os/conway.ny), [matrix.ny](../../etc/projects/os/matrix.ny), [ant.ny](../../etc/projects/os/ant.ny) |
| OS | [args.ny](../../etc/projects/os/args.ny), [server.ny](../../etc/projects/os/server.ny), [ffi.ny](../../etc/projects/os/ffi.ny), [sound.ny](../../etc/projects/os/sound.ny) |
| UI | [term.ny](../../etc/projects/ui/term.ny), [input.ny](../../etc/projects/ui/input.ny), [monitor.ny](../../etc/projects/ui/monitor.ny), [engine.ny](../../etc/projects/ui/engine.ny), [editor.ny](../../etc/projects/ui/editor.ny) |

Run one directly:

```bash
ny etc/projects/os/conway.ny
```

Build a native executable:

```bash
ny -o build/conway etc/projects/os/conway.ny
```

Build the browser wasm runner:

```bash
./make web-demos
```

The generated `build/web/demos/index.html` is a compact WebGL-backed wasm runner.
Load a `.wasm` file from the page, or add explicit browser-ready entries to
`etc/assets/website/wasm/demos.json`.

## Focused Checks

Runtime checks under `etc/tests/runtime/` are the quickest way to inspect one
language surface in isolation:

| Topic | Files |
| --- | --- |
| ADTs and matching | [adt.ny](../../etc/tests/runtime/language/adt.ny), [match.ny](../../etc/tests/runtime/language/match.ny) |
| Concurrency and threads | [concurrency.ny](../../etc/tests/runtime/execution/concurrency.ny), [thread.ny](../../etc/tests/runtime/execution/thread.ny) |
| Comptime | [comptime.ny](../../etc/tests/runtime/compiler/comptime.ny), [table.ny](../../etc/tests/runtime/values/table.ny) |
| Native boundary | [ffi.ny](../../etc/tests/interop/ny/ffi.ny), [extern.ny](../../etc/tests/interop/ny/extern.ny), [asm.ny](../../etc/tests/interop/ny/asm.ny) |
| Ownership and safety | [ownership.ny](../../etc/tests/runtime/ownership/ownership.ny), [safe.ny](../../etc/tests/runtime/ownership/safe.ny), [memory.ny](../../etc/tests/runtime/values/memory.ny) |

Run a focused test by pattern:

```bash
ny test --pattern comptime
ny test --pattern ownership
```

## Benchmarks

Bench examples live under `etc/tests/bench/`. Use them for rough comparisons:

```bash
ny perf
ny -o build/cache/bench/sieve etc/tests/bench/sieve.nshape
```

## Related

- [Start](start.md) for the first file.
- [Programs](programs.md) for script and module shape.
- [Testing](testing.md) for test commands.
- [Performance](performance.md) for measurement discipline.