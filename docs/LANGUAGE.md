# Nytrix Language

A compact, predictable surface.

## Lexical

* Identifiers: `[A-Za-z_][A-Za-z0-9_]*`
* Comments: a semicolon `;` starts a single-line comment.
* Multiline Comments: non-docstring strings (e.g. `"""..."""`) on their own lines are treated as comments/ignored.
* Docstrings: Strings immediately following a function definition are captured as docstrings. Supports single (`"`, `'`) or triple (`"""`, `'''`) quotes.

## Literals and types

* Integers: decimal, `0x` hex, `0o` octal, `0b` binary.
* Floats: `1.0`, `.5`, `1e-3`.
* Bool: `true`, `false`.
* None: `nil` (maps to `0`).
* Strings: `"..."` or `'...'`, UTF-8.
* Containers: lists `[a, b]`, sets `{a, b}`, dicts `{"k": v}`.
* Tuples: `tuple(expr)` or `(a, b,)` via std helpers.

## Core syntax

```ny
fn name(param=default, ...){ ... }
if (cond) { ... } elif (cond) { ... } else { ... }
while (cond) { ... }
for (x in iterable) { ... }
try { ... } catch(e) { ... }
def name = value
undef name
```

Work in progress. Behavior may change.
Tests are the source of truth.
Examples will follow.

## Modules
* **Imports**:
	* Module alias: `use std.math as m`, `use lib as l`, or `use ./util/time as t` (then call `m.sqrt`).
	* Import all exports: `use std.math *` (brings exported names into scope).
	* Import list: `use std.math (sqrt, pow as p)`.
* **Resolution**: `std.*` always maps to `src/std/`, and bare/relative paths resolve from the importing file's directory (current directory first, then std/lib). Keep aliasing short so you know the origin at a glance.
* **Namespacing**:
```nytrix
module tui (bold, italic, dim, underline, color)
```
Use `module name ( ... )` to declare exports; the list can be comma-separated. Use `module name *` to export all module-level functions and vars. You can also write `module name` with no parentheses to define a module with no explicit export list; it treats the rest of the file as the module body. Functions and vars declared outside a module remain local to the file unless explicitly exported.

## Operators

* Arithmetic: `+ - * / %`
* Comparison: `== != < > <= >=`
* Logical: `&& || !`
* Unary: `-`
