<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":14,"summary":"Evaluate whether Nytrix's current compiler, targets, and library fit a project."} -->
# Where Nytrix fits

Nytrix is a statically typed language and compiler project. Its current
strength is an integrated environment for trying language features, running
native code, and inspecting compiler behavior. Its compiler and target support
are still changing; evaluate a real project against the features it needs.

## A reasonable fit

- Small tools, experiments, and learning projects where source and tests can
  evolve with the compiler.
- Native programs that need direct C interoperability and can use a documented
  target/ABI combination.
- Compiler or language experiments where generated artifacts, type checks, and
  route behavior need to be inspected.

## Plan around limits

- Do not assume identical support across LLVM JIT, native-only, Wasm, and browser
  execution.
- Do not treat bounded proof, ownership, or effect analysis as a general formal
  verification system.
- Check C aggregate ABI and host APIs against the exact target and signature.
- Expect APIs, compiler behavior, and package support to change as the project
  develops.

## Evaluate a project

1. List required targets, ABIs, host services, and language features.
2. Check the [implementation status](status.md) and the relevant
   [specification](../spec/index.md).
3. Compile and run the smallest representative program through the route you
   will ship.
4. Add tests for the boundaries your program depends on before expanding it.

For the first run, see [Start](start.md). For exact current support, prefer the
focused spec and fixture over this project-level summary.
