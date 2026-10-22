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
module sample.units(answer)

fn answer() int { 42 }
```

A file is both a script and a module when it declares `module` and also keeps
direct-run statements in a `#main` block. Imported users see only the exports;
direct execution runs the startup block.

## Module declaration forms

| Form | Meaning |
| --- | --- |
| `module name *` | Export every top-level declaration in the file. |
| `module name (a, b, c)` | Export exactly `a`, `b`, `c`; `pub` elsewhere is ignored by imports. |
| `module name { export group(a, b) internal(_helper) }` | Grouped exports; `internal` names stay private. |
| `module name generated from Spec { key = value emit make_backend(Contract) }` | Attach compile-time configuration and emit generated declarations. |

The `export group(...)` form names a profile. A `use module:profile` selects
the `core` surface plus that profile.

```text
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
| `use core` | Imports the module namespace. |
| `use core as core` | Imports it under an alias. |
| `use core: print` | Imports selected exported names. |
| `use mod:debug` | Selects the `debug` profile when that module provides one. |

Imports resolve before ordinary declarations. An unresolved module or export is
a diagnostic. Imports do not execute an entry point.

```ny
use core: assert
assert(true, "selected import")

use core as core
assert(core.len([1, 2, 3]) == 3, "module alias")
```

Import forms apply to relative files as well as module paths:

```text
use "./parser.ny" as parser
use "./parser.ny" (parse_node)
use "./parser.ny":debug
```

## Exports and visibility

The module declaration owns visibility. List exported names in parentheses,
or use `*` to export every top-level declaration. Names omitted from a selected
export list remain private; this language version has no `pub` keyword.

```ny
module sample.visibility(public_value)

def public_value = 1
def private_value = 2
```

## Entry points

`#main { body }` gates `body` to the program entry path. It is equivalent to:

```ny
use core

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
handwritten module. See [compile-time execution](comptime.md#generated-modules).

## Diagnostics

| Condition | Result |
| --- | --- |
| More than one module declaration | Source-unit diagnostic. |
| Import after a declaration requiring resolved imports | Source-order diagnostic. |
| Missing selected export | Import diagnostic naming the module and export. |
| Profile unavailable | Import diagnostic naming the requested profile. |
| `module name (a, b)` selects a missing name | Export diagnostic naming the requested name. |

## Related

- [Syntax](syntax.md#files-modules-and-imports)
- [Functions](functions.md#attributes)
- [Programs](../learn/programs.md)
