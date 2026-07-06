# TODO

## Roadmap

- [ ] LEAN proof like type to allow proof validations to solve problems like erdosproblems.
- [ ] dependent types
- [ ] refinement types

## Native Definition

A feature is truly native only when it works in both the NYIR VM and native codegen, without AST fallback or external compiler infrastructure. LLVM and libclang can be useful references or fallbacks, but they are not the compiler’s core.

## Implemented (skeleton+real)

- [x] Language-level hot reloading via dynamically linked `.so`/`.dll`/`.dylib` (dlopen + native shared emission) and using file watchers (real inotify backend + poll; std.os.fs.watch_* + --hot/--watch CLI auto-restart on source change).
