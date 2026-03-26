# Language

This is the language specification index: source organization, execution
model, and links to exact behavior by topic.

## Authority

The parser, compiler, and runtime define behavior. These pages document the
public surface and the rules exposed to source, tools, diagnostics, and
generated documentation. When behavior changes, the spec page for that area
changes with it.

## Spec conventions

| Term | Meaning |
| --- | --- |
| Source unit | One UTF-8 Nytrix file. |
| Declaration | A named language form such as `fn`, `module`, `struct`, `enum`, `layout`, `extern`, or a compile-time declaration. |
| Statement | A form that executes for effect or control flow. |
| Expression | A form that produces a value. |
| Runtime object | A managed Nytrix value. |
| Native boundary | Pointers, handles, layouts, externs, C strings, and raw buffers. |

Syntax pages record spellings. Topic pages define behavior. Learn pages show
workflow and examples.

## Spec pages

| Page | Scope |
| --- | --- |
| [source.md](source.md) | Source units, imports, modules, script execution, command forms. |
| [imports.md](imports.md) | Import forms, aliases, selected names, resolution, package imports. |
| [modules.md](modules.md) | Module declarations, exports, grouped modules, generated modules. |
| [values.md](values.md) | Literals, strings, containers, receiver methods, equality shape. |
| [functions.md](functions.md) | Bindings, parameters, blocks, returns, lambdas, attributes, ownership contracts. |
| [types.md](types.md) | Type expressions, generics, ADTs, nullable/pointer/native types, strict mode. |
| [operators.md](operators.md) | Arithmetic, power, comparison, logic, bitwise, ternary, coalescing, calls. |
| [patterns.md](patterns.md) | `case` and `match` patterns, wildcard arms, dispatch clarity. |
| [control-flow.md](control-flow.md) | `if`, loops, `case`, `match`, `try`, `defer`, `with`. |
| [errors.md](errors.md) | Assertions, panics, recoverable results, diagnostic meaning. |
| [comptime.md](comptime.md) | Compile-time blocks, tables, matches, templates, generated modules. |
| [native.md](native.md) | Layouts, externs, pointers, handles, FFI strings, ABI rules. |
| [runtime.md](runtime.md) | Memory boundaries, ownership, async/concurrency, effects, execution modes. |
| [syntax.md](syntax.md) | Lexical spellings and grammar-shaped forms. |

## Core model

A Nytrix source file is UTF-8 text containing imports, declarations, and
statements. Top-level statements make the file executable as a script.
`module` declarations define exported names. `use` imports public names from a
module or file.

Values are runtime objects unless a type, compile-time form, ownership mode, or
native boundary requires static treatment. `def` creates an immutable binding.
`mut` creates a mutable binding. Blocks can produce a value from their final
expression when used by an expression-shaped language form.

The current compiler surface includes algebraic data types with payloads,
generic type expressions such as `list<int>` and `Option<int>`, stackless
`async`/`await`, compile-time tables/templates/proofs, FFI `#include`, and
strict ownership checks through `--borrow-check` and `--ownership-strict`.

## Execution

| Form | Behavior |
| --- | --- |
| `ny` | Start the REPL, or read piped stdin as REPL batch input. |
| `ny file.ny` | Run a source file through the JIT path. |
| `ny -c 'code'` | Run inline source. |
| `ny -ic 'code'`, `ny -ci 'code'` | Run inline source, then enter the REPL. |
| `ny -run file.ny` | Build and run a temporary native executable. |
| `ny -o app file.ny` | Emit a native executable. |
| `ny -i`, `ny --interactive` | Start the REPL. |

Arguments, files, paths, time, processes, networking, and terminal behavior are
standard-library surfaces. See [tooling.md](../learn/tooling.md) and
[library.md](../learn/library.md).

## Language shape

```ny
use std.core

module sample(add)

fn add(int: a, int: b): int {
   a + b
}

assert_eq(add(1, 2), 3, "add")
```

## Learning path

- [start.md](../learn/start.md) runs the first file.
- [programs.md](../learn/programs.md) explains script/module shape.
- [repl.md](../learn/repl.md) covers interactive checks and REPL-to-file handoff.
- [tooling.md](../learn/tooling.md) lists command families.
- [troubleshooting.md](../learn/troubleshooting.md) covers common failures.
