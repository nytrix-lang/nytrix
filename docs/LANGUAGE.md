# Nytrix Language Reference

Compact systems language with explicit imports and a small runtime core.

> Status: WIP. Runtime tests in `etc/tests/runtime/` and std tests in `std/*/test/` are the source of truth.

## Execution Model

- `ny file.ny` parses/compiles in emit-only mode by default.
- `ny -run file.ny` compiles and JIT-runs `main()`.
- `ny -c 'code'` executes inline source.
- No implicit prelude: import everything you use with `use`.

## Lexical Rules

- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`
- Single-line comments start with `;` (commonly `;;`).
- Strings: `"..."`, `'...'`, `"""..."""`, `'''...'''`
- f-strings: `f"len={len(xs)}"`

## Values and Literals

- Primitive values: integers, floats, booleans, `nil`
- Integer literals: decimal, `0x`, `0o`, `0b`
- Numeric suffixes tested in runtime suite:
  - ints: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`
  - floats: `f32`, `f64`, `f128`
- List literals: `[1, 2, 3]`
- Dict literals: `{"k": v}`
- Sets and tuples are provided by std constructors (`set(...)`, `tuple([...])`).

## Strings and text

- Runtime strings are byte strings with explicit length metadata.
- `len(s)` / `str_len(s)` return byte length.
- UTF-8 helpers in `std.str`:
  - `utf8_valid(s)` validates a byte string as UTF-8.
  - `utf8_len(s)` returns code-point length.
  - `ord(s)` / `ord_at(s, idx)` return Unicode code points.
  - `utf8_slice(s, start, stop, step=1)` slices by code points.
- `chr(cp)` encodes a Unicode code point as UTF-8.
- Generic `slice(str, ...)` uses UTF-8 code-point indices.

## Declarations and Types

- `def` creates an immutable binding.
- `mut` creates a mutable binding.
- `undef` clears a binding.
- Optional type annotations are supported on variables and functions:

```ny
def x: int = 1
fn add(a: int, b: int): int { return a + b }
```

- Nullable types use `?T`.
- Pointer types use `*T`.
- `nil` is valid for nullable and pointer-typed values.

## Functions

- Definition: `fn name(a, b=0) { ... }`
- Variadics: `fn log(...args) { ... }`
- Lambdas/closures: `lambda(x){ x + 1 }`
- External declarations with aliasing:

```ny
extern fn c_getpid(): i32 as "getpid"
```

## Control Flow

- `if` / `elif` / `else`
- `while` and `for x in iter` (paren and no-paren forms are both tested)
- `match` with guards and pattern-style arms
- `case` with comma-separated labels and `_` fallback
- `try { ... } catch err { ... }`
- `defer { ... }` at scope exit
- `return`, `break`, `continue`

## Compile-Time and Low-Level

- `comptime { ... }` runs at compile time.
- `embed("path")` embeds file content.
- `asm(...)` provides inline assembly.
- `sizeof(T)` plus `layout`/`struct` declarations are supported.

## Operators

- Arithmetic: `+ - * / %`
- Comparison: `== != < <= > >=`
- Logical: `&& || !`
- Bitwise/integer: `& | ^ << >>`
- Ternary: `cond ? a : b`
- Index/call: `x[i]`, `f(a, b)`

`?` is used for nullable types (`?T`) and ternary expressions, not error propagation.

## Modules and Imports

Module declarations:

- `module pkg.name (sym1, sym2)`
- `module pkg.name *`

Import forms used across std and runtime tests:

- `use std.core`
- `use std.core *`
- `use std.str as str`
- `use std.core (len, append as push)`
- `use "./local.ny" (helper as h)`

Common std tree layout:

- `std/<pkg>/lib/mod.ny`
- `std/<pkg>/test/*.ny`
- `std/<pkg>/lib/*.ny`

## Extensible Syntax (`std.core.syntax`)

Import:

```ny
use std.core.syntax as syntax
```

Public registry API includes:

- `new_registry`, `registry`, `reset_registry`
- `register_macro`, `register_attribute`
- `expand_macro`, `expand_form`, `expand_form_deep`
- `new_rewriter`, `register_rewrite`, `rewrite_fixpoint`
- `apply_attribute`

Behavior goals of the current API:

- deterministic registration order
- explicit registry-based state (local registries supported)
- safe fallback behavior (`expand_macro*` returns `nil` when missing; `apply_attribute*` returns input node when missing)

## Built-in Attributes

Built-ins include `@extern`, `@naked`, `@jit`, `@thread`, `@pure`, `@effects(...)`, and `@llvm(...)`.
Custom attributes can be registered through `std.core.syntax`.
