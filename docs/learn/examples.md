# Examples

The checkout keeps runnable examples under `etc/projects/` and focused runtime
checks under `etc/tests/`.

## Runnable Projects

| Area | Files |
| --- | --- |
| CLI | [conway.ny](../../etc/projects/cli/conway.ny), [matrix.ny](../../etc/projects/cli/matrix.ny), [ant.ny](../../etc/projects/cli/ant.ny) |
| OS | [args.ny](../../etc/projects/os/args.ny), [server.ny](../../etc/projects/os/server.ny), [ffi.ny](../../etc/projects/os/ffi.ny), [sound.ny](../../etc/projects/os/sound.ny) |
| UI | [term.ny](../../etc/projects/ui/term.ny), [input.ny](../../etc/projects/ui/input.ny), [monitor.ny](../../etc/projects/ui/monitor.ny), [engine.ny](../../etc/projects/ui/engine.ny), [editor.ny](../../etc/projects/ui/editor.ny) |

Run one directly:

```bash
ny etc/projects/cli/conway.ny
```

Build a native executable:

```bash
ny -o build/conway etc/projects/cli/conway.ny
```

Build the browser wasm runner:

```bash
./make web-demos
```

The generated `build/wasm/index.html` is a compact WebGL-backed wasm runner.
Load a `.wasm` file from the page, or add explicit browser-ready entries to
`etc/assets/website/wasm/demos.json`.

## Focused Checks

Runtime checks under `etc/tests/rt/` are the quickest way to inspect one
language surface in isolation:

| Topic | Files |
| --- | --- |
| ADTs and matching | [adt.ny](../../etc/tests/rt/adt.ny), [match.ny](../../etc/tests/rt/match.ny) |
| Async and threads | [async.ny](../../etc/tests/rt/async.ny), [thread.ny](../../etc/tests/rt/thread.ny) |
| Comptime | [comptime.ny](../../etc/tests/rt/comptime.ny), [table.ny](../../etc/tests/rt/table.ny) |
| Native boundary | [ffi.ny](../../etc/tests/rt/ffi.ny), [extern.ny](../../etc/tests/rt/extern.ny), [asm.ny](../../etc/tests/rt/asm.ny) |
| Ownership and safety | [ownership.ny](../../etc/tests/rt/ownership.ny), [safe.ny](../../etc/tests/rt/safe.ny), [memory.ny](../../etc/tests/rt/memory.ny) |

Run a focused test by pattern:

```bash
ny test --pattern comptime
ny test --pattern ownership
```

## Benchmarks

Bench examples live under `etc/tests/fuzz/bench/`. Use them for rough comparisons:

```bash
ny perf
ny -o build/cache/bench/sieve etc/tests/fuzz/bench/sieve.nshape
```

## Related

- [start.md](start.md) for the first file.
- [programs.md](programs.md) for script and module shape.
- [testing.md](testing.md) for test commands.
- [performance.md](performance.md) for measurement discipline.
