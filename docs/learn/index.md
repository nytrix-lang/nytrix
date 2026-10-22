<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":0,"summary":"Choose a practical route through Nytrix tutorials, language guides, and workflows."} -->
# Learn Nytrix

Start with a small program, then open only the guide for the task at hand.
Language rules live in the [specification](../spec/index.md); this section
focuses on using the compiler and library.

## Start here

1. [Start](start.md): install or locate the compiler, compile a file, and run it.
2. [Tour](tour.md): read and extend a small Ny program.
3. [Programs](programs.md): organize files, imports, and entry points.
4. [Library](library.md): find an exported function with `ny doc`.

## Language and application guides

- [Quick reference](quirks.md) for syntax and runtime behavior that commonly
  surprises users.
- [Ownership](ownership.md), [effects](effect.md), [concurrency](concurrency.md),
  [compile-time programming](comptime.md), and [proofs](proofs.md).
- [Numbers](numbers.md), [SIMD](simd.md), [networking](networking.md),
  [native interfaces](native.md), and [UI](ui.md).
- [Cookbook](cookbook.md) for short task-oriented patterns.

## Workflows

- [Build](build.md), [testing](testing.md), [tooling](tooling.md), and
  [troubleshooting](troubleshooting.md).
- [REPL](repl.md) for interactive use; [packages](packages.md) for dependency
  workflows.
- [Browser and WebAssembly](browser.md) for the browser target and its host
  boundary; [examples](examples.md) for runnable projects.
- [Performance](performance.md) for measurement; [routes](routes.md) for
  execution-path differences.

## Project boundaries

[Implementation status](status.md) summarizes current support and known limits.
[Architecture](architecture.md) is for contributors who need to locate a
compiler subsystem. [Where Nytrix fits](fits.md) describes suitable and
unsuitable project shapes.
