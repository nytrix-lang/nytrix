<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":170,"summary":"Build, run, format, test, inspect documentation, and validate target behavior."} -->
# Tooling

`./make` is the incremental build entry point. Use the smallest command that
proves the layer you changed.

## Everyday loop

```bash
./make bin --color=never
./make ny --no-progress --color=never file.ny
ny fmt --check file.ny
ny --strict-types file.ny
ny test --with-stdlib module-or-path
git diff --check
```

`./make ny` rebuilds affected compiler and standard-library inputs before it
runs the file. Use `./make clean && ./make` only when diagnosing a stale build
or changing build configuration.

## Run modes

| Form | Behavior |
| --- | --- |
| `ny` | Start the REPL. |
| `ny file.ny` | Run the ordinary execution path. |
| `ny -c 'code'` | Run inline source. |
| `ny -run file.ny` | Build and run a temporary native executable. |
| `ny -o app file.ny` | Emit a native executable. |

## Diagnose a program

```bash
ny --diag-compact --collect-errors file.ny
ny --diag-rich file.ny
ny --strict-types file.ny
ny --borrow-check --ownership-strict file.ny
ny doc search --symbols name
ny doc get std.module
```

Use compact diagnostics for the failure set. Use rich diagnostics for source
context. Run the same command after each repair.

## Format and review

```bash
ny fmt --fix file.ny
ny fmt --check file.ny
ny fmt --metaprog file.ny
ny fmt --specialize file.ny
ny fmt --trim --check file.ny
```

Formatting changes layout. Audit commands report review leads. They do not
prove a bug or authorize an automatic rewrite.

## Test a target

```bash
ny test --pattern name
ny test --with-stdlib module-or-path
./make web-test
./make web-check etc/projects/ui/pong.ny
```

Use browser checks for browser-facing code. Set `NYTRIX_BROWSER=chromium` or
`NYTRIX_BROWSER=firefox` to select an installed engine. A headless fixture may
be explicitly skipped when it requires a live browser lifecycle.

## Browser artifact targets

`wasm-bare` is the default browser artifact. `wasm-emscripten` is a separate
adapter contract, selected explicitly:

```bash
./make web --target wasm-emscripten etc/projects/ui/pong.ny --out build/web/pong-emcc
./make web-check --target wasm-emscripten etc/projects/ui/pong.ny
```

The adapter emits standalone Wasm through the installed Emscripten toolchain,
but it is loaded by the same Nytrix WebGL2 runner and has the same explicit
`env.*` host-import contract as `wasm-bare`. It supports the documented
portable rendering baseline, Asyncify-based browser operations, packaged
assets, and capability-gated browser APIs. It does not promise native
filesystem, process, threads, or Vulkan APIs. Use `./make web-test` after an
adapter change: it checks import rejection, link output, browser execution,
and the shared renderer fixture for both targets.

## Build output

Build output is grouped beneath `build/`. Browser output is grouped beneath
`build/web/`. Treat generated files as artifacts, not source files.

## Related

- [Testing](testing.md)
- [Troubleshooting](troubleshooting.md)
- [Performance](performance.md)
- [Contributing](../CONTRIBUTE.md)