<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":200,"summary":"Build a browser app or Wasm artifact and check its browser host capabilities."} -->
# Browser and WebAssembly

The browser target has its own host API. It does not inherit native filesystem,
process, thread, audio, or Vulkan support merely because similarly named APIs
exist on native targets. Check the capability used by the program on the exact
browser route.

## Build and test

```bash
./make web etc/projects/ui/pong.ny --out build/web/pong
./make web-check etc/projects/ui/pong.ny
./make web-test
./make wasm path/to/program.ny --out build/program.wasm
./make web-demos
```

`web` builds a deployable browser app; `wasm` emits a Wasm artifact. Use the
project command's help for options and prerequisites:

```bash
./make help web
./make help wasm
./make help web-test
```

## Host boundary

Browser code runs through explicit imports and browser capabilities. APIs that
need a user gesture, permission, secure context, WebSocket server, or live
browser lifecycle can fail or be unavailable even when the program compiles.
Handle those results rather than assuming desktop behavior.

The maintained browser path includes a WebGL2 runner and a minimal Wasm
runtime. The separate Emscripten adapter has its own build and import contract;
use it only when the project requires that toolchain. See [UI](ui.md) for
application code and [Networking](networking.md) for browser networking.

## Diagnose a failure

Record the full command, target, browser, host capability, and first failing
observable result. Separate a compile/link failure from a denied browser API or
a missing service. See [Execution routes](routes.md) for route differences and
[Testing](testing.md) for repository checks.
