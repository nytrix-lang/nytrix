# Source units

Source units are UTF-8 files containing imports, declarations, modules, and
executable statements.

## Authority

The parser, compiler, and runtime define behavior. These pages document the
public surface and the rules exposed to source, tools, diagnostics, and
generated documentation. When behavior changes, the spec page for that area
changes with it.

This page is the spec overview. Syntax pages record spellings. Topic pages
define behavior. Learn pages show workflow and examples.

## Spec conventions

| Term | Meaning |
| --- | --- |
| Source unit | One UTF-8 Nytrix file. |
| Declaration | A named language form such as `fn`, `module`, `struct`, `enum`, `layout`, `extern`, or a compile-time declaration. |
| Statement | A form that executes for effect or control flow. |
| Expression | A form that produces a value. |
| Runtime object | A managed Nytrix value. |
| Native boundary | Pointers, handles, layouts, externs, C strings, and raw buffers. |

## Source file

A source unit is UTF-8 text. It can contain imports, declarations, and
statements.

| Item | Behavior |
| --- | --- |
| Line comment | `;` starts a comment until newline. |
| Top-level statement | Executes when the file is run as a script. |
| Declaration | Defines functions, modules, layouts, externs, and compile-time forms. |
| Function docstring | A leading string literal in a function body is the function docstring. |

There are no semicolon statement terminators. Semicolon means comment.

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

## Import forms

```ny
use std
use module.path
use module.path as alias
use std module.path as alias
use module.path (name, other)
use module.path (name as alias)
use "./relative.ny" as alias
use "./relative.ny" (helper)
use "./relative.ny":debug
use module.path *
```

`use module.path` imports normal exported names and keeps the leaf name
available as a module alias. `use module.path *` is accepted as compatibility broad
import spelling.

`use path:profile` imports the module's core export group plus the named export
profile. `use std module.path as alias` normalizes to `use std.module.path as
alias`.

Imports are resolved before normal execution. Missing imports are compile-time
errors.

## Module forms

```ny
module name *
module name (a, b, c)
module pkg.name {
   export core(a, b)
   export debug(dump)
   internal(_helper)
}
```

`module name(exports)` declares the exported names for the file. Grouped module
blocks can mark public profiles and internal names explicitly. Profile names
are importable with `use module:profile`.

## Direct execution guard

`#main { ... }` is the compact direct-execution guard. It is equivalent to
guarding the block with `comptime{ __main() }`.

```ny
use std.core

#main {
   assert(true, "direct execution")
}
```

`__main()` is true only for the source file being run directly. Imported files
see `__main()` as false, including inside `comptime{ ... }`.

## Execution forms

| Form | Behavior |
| --- | --- |
| `ny` | Start the REPL, or read piped stdin as REPL batch input. |
| `ny file.ny` | Run a source file through the JIT path. |
| `ny -c 'code'` | Run inline source. |
| `ny -ic 'code'`, `ny -ci 'code'` | Run inline source, then enter the REPL. |
| `ny --repl < file.ny` | Run stdin source once through the REPL batch path. |
| `ny -run file.ny` | Build and run a temporary native executable. |
| `ny -o app file.ny` | Emit a native executable. |
| `ny -i`, `ny --interactive`, `ny --plain-repl` | Start the REPL. |

Arguments, files, paths, time, processes, networking, and terminal behavior are
standard-library surfaces. See [tooling.md](../learn/tooling.md) and
[library.md](../learn/library.md).

## Script and module together

A file can export names and also contain script checks:

```ny
use std.core

module stats(mean)

fn mean(list xs) number {
   mut total = 0
   for x in xs { total += x }
   total / xs.len
}

assert_eq(mean([2, 4]), 3, "mean")
```

When imported, exported names are visible. When run directly, top-level
statements execute.

## Generated modules

Generated modules bind compile-time data and emit declarations:

```ny
module pkg.generated generated from Spec {
   native_prefix = "x"
   emit make_backend(Contract)
}
```

See [comptime.md](comptime.md) for compile-time generation.

## Language shape

```ny
use std.core

module sample(add)

fn add(int a, int b) int {
   a + b
}

assert_eq(add(1, 2), 3, "add")
```

## Topic pages

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

## Learning path

- [start.md](../learn/start.md) runs the first file.
- [programs.md](../learn/programs.md) explains script/module shape.
- [repl.md](../learn/repl.md) covers interactive checks and REPL-to-file handoff.
- [tooling.md](../learn/tooling.md) lists command families.
- [troubleshooting.md](../learn/troubleshooting.md) covers common failures.

## Related

- [imports.md](imports.md) for exact import forms and resolution.
- [modules.md](modules.md) for exports and module boundaries.
- [syntax.md](syntax.md) for exact spellings.
- [programs.md](../learn/programs.md) for practical file layout.
- [tooling.md](../learn/tooling.md) for command forms.
