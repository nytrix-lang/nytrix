# Nytrix documentation

This directory has two layers:

- `learn/` explains how to get work done with the language and tools.
- `spec/` records the exact rule that the parser, compiler, runtime, or
  standard tooling is expected to follow.

Use the generated reference for current signatures, exported names, module
docstrings, and keyword search:

```bash
ny doc search json
ny doc search --symbols recvuntil
ny doc get std.parse.data.json
```

## Start here

The task determines the first page. `spec/` contains precise rules and
diagnostic meanings.

| Question | Start here | Then use |
| --- | --- | --- |
| How do I run one file? | [learn/start.md](learn/start.md) | [learn/tooling.md](learn/tooling.md) |
| How is source structured? | [learn/programs.md](learn/programs.md) | [spec/source.md](spec/source.md), [spec/modules.md](spec/modules.md) |
| Which stdlib module owns this? | [learn/library.md](learn/library.md) | `ny doc search`, `ny doc get` |
| Why did this fail? | [learn/diagnostics.md](learn/diagnostics.md) | [spec/errors.md](spec/errors.md), [learn/troubleshooting.md](learn/troubleshooting.md) |
| How do safety checks work? | [learn/tooling.md](learn/tooling.md) | [spec/runtime.md](spec/runtime.md), [spec/types.md](spec/types.md) |
| How do native/FFI boundaries work? | [learn/native.md](learn/native.md) | [spec/native.md](spec/native.md) |
| How do I measure performance? | [learn/performance.md](learn/performance.md) | `ny perf compare` |

## Language

Nytrix source files run directly. A file can also declare a `module` and export
names for other files. Imports are explicit. Standard-library APIs live under
their owning modules; `ny doc` confirms exact names and signatures.

Core surface: native binaries, typed bindings, ADTs/generics, async tasks,
comptime tables/templates/checks, native layouts, C header imports, inline
assembly, default compile-time type checks, and opt-in ownership/raw-memory
diagnostics.

## Learn guides

| Page | Use it for |
| --- | --- |
| [start.md](learn/start.md) | Toolchain check, first program, API search, diagnostics. |
| [programs.md](learn/programs.md) | Scripts, modules, imports, entrypoints, exported names. |
| [repl.md](learn/repl.md) | Interactive probes, paste behavior, completion, REPL-to-file handoff. |
| [examples.md](learn/examples.md) | Small complete programs and project entry points. |
| [ui.md](learn/ui.md) | Windows, resize-safe frame loops, drawing, text, textures, input, 3D start. |
| [library.md](learn/library.md) | Facades and domain choice before opening generated API pages. |
| [tooling.md](learn/tooling.md) | Command forms, docs generation, formatting, tests, audits. |
| [networking.md](learn/networking.md) | Requests, local servers, sockets, tubes, transport logs. |
| [packages.md](learn/packages.md) | Manifests, sources, package repos, install roots, lockfiles. |
| [performance.md](learn/performance.md) | Timing, profiling, cache discipline, report shape. |
| [metaprogramming.md](learn/metaprogramming.md) | Compile-time tables, templates, generated modules. |
| [native.md](learn/native.md) | Layouts, externs, `#include`, pointers, handles, strings, ownership. |
| [testing.md](learn/testing.md) | Executable checks and test command shape. |

## Specification

| Page | Scope |
| --- | --- |
| [spec/language.md](spec/language.md) | Language structure and execution model. |
| [spec/source.md](spec/source.md) | Source units, imports, modules, script execution. |
| [spec/imports.md](spec/imports.md) | Import forms, aliases, selected names, resolution, package imports. |
| [spec/modules.md](spec/modules.md) | Module declarations, export lists, grouped modules, generated modules. |
| [spec/values.md](spec/values.md) | Literals, strings, containers, receiver methods. |
| [spec/functions.md](spec/functions.md) | Bindings, parameters, blocks, returns, lambdas, attributes, contracts. |
| [spec/types.md](spec/types.md) | Type expressions, generics, ADTs, typed bindings, default checks. |
| [spec/operators.md](spec/operators.md) | Arithmetic, power, comparison, logic, bitwise, ternary, coalescing, calls. |
| [spec/patterns.md](spec/patterns.md) | `case` and `match` patterns, wildcard arms, dispatch clarity. |
| [spec/control-flow.md](spec/control-flow.md) | Conditionals, loops, `case`, `match`, `try`, `defer`, `with`. |
| [spec/errors.md](spec/errors.md) | Assertions, panics, result shape, diagnostics. |
| [spec/comptime.md](spec/comptime.md) | Compile-time blocks, tables, matches, templates, generated modules. |
| [spec/native.md](spec/native.md) | Layouts, externs, `#include`, pointers, handles, FFI boundaries. |
| [spec/runtime.md](spec/runtime.md) | Execution modes, ownership, cleanup, async/concurrency, effects. |
| [spec/syntax.md](spec/syntax.md) | Lexical forms and grammar-shaped source spellings. |

## Core commands

```bash
./make doctor
./make doctor --install
./make env
./make targets
ny file.ny
ny -c 'print(1 + 1)'
ny -run file.ny
ny -o app file.ny
ny fmt --check file.ny
ny doc search [--docs|--symbols] query
ny doc get query
ny pkg repo list
ny test
```

Native `-o` builds use the default optimized native profile. JIT/REPL paths
default to edit-latency settings. `-O3` and `--profile=peak` are explicit
peak-speed measurement modes.

## Conventions

Fenced `ny` blocks are Nytrix source. The surrounding text names required
local services, files, or packages. Shell commands use `bash`; manifests and
wire formats use their own fences. Silent success means all assertions passed.

Examples use explicit imports and small assertions. Spec pages use exact forms
and behavior tables. If an example depends on a local server, native library,
network service, or fixture, the text names it before the code block.
