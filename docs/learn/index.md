<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":0,"summary":"Choose a practical route through Nytrix tutorials, language guides, and workflows."} -->
# Learn Nytrix

Start with a small program, then choose a guide for your task.

Prerequisites: none; start with the [first program](start.md).

## How to read these docs

- **Learn** pages teach workflows and concepts with examples.
- **Spec** pages define language rules and implementation boundaries.
- **Cookbook** recipes are copy-paste starting points; check linked contracts
  before adapting them.

Next:

- [Start](start.md) — compile and run a program.
- [Tour](tour.md) — read core language forms.
- [Specification](../spec/index.md) — look up exact language rules.

## Start here

1. [Start](start.md): install or locate the compiler, compile a file, and run it.
2. [Tour](tour.md): read and extend a small Ny program.
3. [Programs](programs.md): organize files, imports, and entry points.
4. [Library](library.md): find an exported function with `ny doc`.

## First concepts

- [Quick reference](quirks.md) for syntax and behavior that commonly surprise
  users.
- [Numbers](numbers.md), [ownership](ownership.md), [effects](effect.md),
  [proofs](proofs.md), and [compile-time programming](comptime.md).
- [Concurrency](concurrency.md) and [native interfaces](native.md) when your
  program needs those boundaries.

## Application guides

- [Networking](networking.md), [UI](ui.md), and
  [browser and WebAssembly](browser.md).
- [Cookbook](cookbook.md) for short task-oriented patterns.

## Workflows

- [Build](build.md), [testing](testing.md), [tooling](tooling.md), and
  [troubleshooting](troubleshooting.md).
- [REPL](repl.md) for interactive use; [packages](packages.md) for dependency
  workflows.
- [Browser and WebAssembly](browser.md) for the browser target and its host
  boundary; [examples](examples.md) for runnable projects.
- [Performance](performance.md) for timing, profiles, and contention;
  [compiler debugging](debugging.md) for diagnostics and pass isolation.
- [Recorded measurements](measurements.md) for historical experiments, not
  current performance guarantees; [execution routes](status.md#execution-routes)
  for execution-path differences.

## Project boundaries

[Status and execution routes](status.md) covers current support, adoption, and known limits.
[Architecture](architecture.md) is for contributors who need to locate a
compiler subsystem.
