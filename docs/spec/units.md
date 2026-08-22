<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":10,"summary":"Rules for source files, imports, modules, profiles, and entry points."} -->
# Units

This page defines source-file structure. Use [Programs](../learn/programs.md)
for project workflow.

## File forms

| Form | Meaning |
| --- | --- |
| Script | A file without `module`; its top-level expressions execute. |
| Module | A file beginning with `module name`; it exports declared public names. |
| Generated module | A module emitted by compile-time generation. |

The module declaration follows the header comments. A source unit has one
module declaration at most.

```ny
;; Keywords: source module
module sample.units

pub fn answer() int { 42 }
```

A file is both a script and a module when it declares `module` and also keeps
direct-run statements in a `#main` block. Imported users see only the exports;
direct execution runs the startup block.

## Module declaration forms

| Form | Meaning |
| --- | --- |
| `module name *` | Export every `pub` declaration in the file. |
| `module name (a, b, c)` | Export exactly `a`, `b`, `c`; `pub` elsewhere is ignored by imports. |
| `module name { export group(a, b) internal(_helper) }` | Grouped exports; `internal` names stay private. |
| `module name generated from Spec { key = value emit make_backend(Contract) }` | Attach compile-time configuration and emit generated declarations. |

The `export group(...)` form names a profile. A `use module:profile` selects
the `core` surface plus that profile.

```ny
module sample.groups {
   export core(run)
   export debug(dump_state)
   internal(_state)
}

use sample.groups:debug
```

## Imports

| Form | Binding |
| --- | --- |
| `use std.core` | Imports the module namespace. |
| `use std.core as core` | Imports it under an alias. |
| `use std.core: print` | Imports selected exported names. |
| `use mod:debug` | Selects the `debug` profile when that module provides one. |

Imports resolve before ordinary declarations. An unresolved module or export is
a diagnostic. Imports do not execute an entry point.

```ny
use std.core: assert
use std.math as math

assert(math.sqrt(9) == 3, "imported name")
```

Import forms apply to relative files as well as module paths:

```ny
use "./parser.ny" as parser
use "./parser.ny" (parse_node)
use "./parser.ny":debug
```

## Exports and visibility

`pub` exports a declaration. Declarations without `pub` remain private to the
module. Selected imports may name only exported declarations.

```ny
module sample.visibility

pub def public_value = 1
def private_value = 2
```

## Entry points

`#main { body }` gates `body` to the program entry path. It is equivalent to:

```ny
if comptime { __main() } { print("entry") }
```

The gate evaluates false for import-time evaluation. `__main()` is a
compile-time predicate; it is not a user-defined replacement for an entry
function.

A file without a module declaration runs its top-level statements directly on
the program entry path. A file with `module` runs only the `#main` body on the
entry path; imports of that module never execute that body.

## Generated source

Generated source must declare its module identity when it forms a module. The
generated declaration participates in the same import and export rules as a
handwritten module. See [compile-time execution](comptime.md#emission-and-generated-source).

## Diagnostics

| Condition | Result |
| --- | --- |
| More than one module declaration | Source-unit diagnostic. |
| Import after a declaration requiring resolved imports | Source-order diagnostic. |
| Missing selected export | Import diagnostic naming the module and export. |
| Profile unavailable | Import diagnostic naming the requested profile. |
| `module name (a, b)` selects a missing name | Export diagnostic naming the requested name. |

## Related

- [Syntax](syntax.md#source-structure)
- [Functions](functions.md#visibility-and-attributes)
- [Programs](../learn/programs.md)