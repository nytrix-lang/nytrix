<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":15,"summary":"Choose a compiler execution route and verify the backend your program uses."} -->
# Execution routes

Choose a route based on the artifact or host you need. Similar source syntax
does not guarantee identical platform APIs or target coverage.

| Goal | Command | Route |
| --- | --- | --- |
| Run a source file during development | `ny file.ny` | Default execution route; LLVM JIT on supported builds |
| Run a native executable | `ny -run file.ny` | AOT compile, then execute |
| Emit an executable | `ny -o app file.ny` | AOT compile without execution |
| Check NYIR/native support directly | `ny --native-only file.ny` | NYIR and native lowering; no LLVM code generation |
| Emit WebAssembly | `ny --wasm file.ny` | Wasm target and its explicit import boundary |
| Verify browser behavior | `./make web-test` | Browser runner and browser-specific fixtures |
| Inspect NYIR execution | `ny --nyir-run file.ny` | Diagnostic VM, not a substitute for native or JIT execution |

The exact options and defaults are shown by `ny --help`. `-run` builds a native
executable; `-o` implies compile-only. `--native-only` deliberately reports an
unsupported native shape rather than retrying through LLVM. See the
[Native specification](../spec/native.md) and [Browser guide](browser.md) for
target-specific constraints.

## Test the route you ship

A test run should name both the source case and the route. Use the focused
fixture or compile the same small program with the target flags used by your
application. A pass on the default route does not prove native-only, Wasm, or
browser compatibility. Host APIs also depend on available devices and
permissions; treat absence and failure as part of the API contract.

For fixture commands, see [Testing](testing.md). For current feature limits,
see [Implementation status](status.md).
