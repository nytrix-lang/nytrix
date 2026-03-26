# Source units

Source units are UTF-8 files containing imports, declarations, modules, and
executable statements.

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

## Import forms

```text
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
available as a module alias. `use module.path *` is accepted as legacy broad
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

`__main()` is true only for the source file being run directly. Imported files
see `__main()` as false, including inside `comptime{ ... }`.

```ny
use std.core

if(__main()){
   assert(true, "direct execution")
}
```

## Script and module together

A file can export names and also contain script checks:

```ny
use std.core

module stats(mean)

fn mean(list: xs): number {
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

```text
module pkg.generated generated from Spec {
   native_prefix = "x"
   emit make_backend(Contract)
}
```

See [comptime.md](comptime.md) for compile-time generation.

## Related

- [imports.md](imports.md) for exact import forms and resolution.
- [modules.md](modules.md) for exports and module boundaries.
- [syntax.md](syntax.md) for exact spellings.
- [programs.md](../learn/programs.md) for practical file layout.
- [tooling.md](../learn/tooling.md) for command forms.
