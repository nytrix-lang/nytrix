# Modules

Modules define exported names and public boundaries. A file can be both a
script and a module: imports see exported names, while direct execution runs
top-level statements.

## File shape

```ny
use std.core

module stats(mean, median)

fn mean(list<number> xs) number {
   mut number: total = 0
   for x in xs { total += x }
   total / xs.len
}

fn median(xs) number {
   xs.get(xs.len / 2)
}
```

Imports come first, the module declaration names the public surface, and helper
functions follow.

The module declaration defines the import surface. Names missing from the
export list are not public API.

## Export lists

```ny
module stats(mean, median)

fn mean(xs) number { 0 }
fn median(xs) number { 0 }
```

Only exported names are part of the module surface. Helper names can remain
private by staying out of the export list.

Export lists document and enforce the module surface. A new public API name is
visible because it must be added to the list.

Public export names describe the API role. Helper names, migration aliases,
and local experiment names stay out of the public surface unless they are
intentionally supported.

## Broad export

```ny
module tools *
```

Broad export fits small local modules and generated surfaces. Public library
modules use explicit export lists when the public surface needs to stay clear.

Broad export exposes all helpers in the file. Use it for local code and
generated tables; switch to an explicit list before the module becomes part of
a public namespace.

## Grouped modules

```ny
module pkg.name {
   export core(run, stop)
   export debug(dump_state)
   internal(_state, _helper)
}
```

Grouped module declarations document public and internal regions explicitly.
They fit larger files where a single flat export list is hard to scan.

`core` is the default export profile. Named profiles are imported with
`use module.path:profile`:

```ny
use pkg.name:debug
```

The import makes `core` exports and the selected profile exports visible.

## Private helpers

Private helper names are implementation details. A `_` prefix is a convention,
not a substitute for an export list:

```ny
module text(clean)

fn _trim_edges(s) str { s }

fn clean(s) str {
   _trim_edges(s)
}
```

Importers see `clean`, not `_trim_edges`.

Private helpers can use narrower types when the public function validates the
input shape before calling them.

## Script checks

Direct-run checks can live beside module declarations:

```ny
module math_extra(double)

fn double(int x) int {
   x * 2
}

#main {
   assert(double(4) == 8, "double")
}
```

When imported, `double` is visible. When run directly, the `#main` block also
executes.

Use module self-checks for cheap invariants that protect public APIs. Keep
external services, timing assumptions, private files, and large fixtures in
focused tests or examples.

## Generated modules

Compile-time generation can emit module declarations:

```ny
module generated.api generated from Spec {
   emit make_api(Spec)
}
```

Generated modules still expose a documented public surface.

## Publication check

- Are imports explicit and at the top?
- Does the export list match the declared API?
- Are private helpers kept out of exports?
- Do script checks exclude external services and slow work?
- Does `ny doc search --symbols exported_name` find the public symbol after
  docs are regenerated?

## Common revisions

| Finding | Revision |
| --- | --- |
| Export list includes `_helper` | Keep helper private or rename it as public API. |
| Module imports itself to reach helpers | Call local helpers directly. |
| Script check performs slow IO | Move it to a focused test or example. |
| Generated module lacks docs | Add docstrings to emitted public functions. |
| Many unrelated exports | Split the file by domain before the surface becomes hard to scan. |

## Related

- [imports.md](imports.md) for using modules.
- [source.md](source.md) for file layout.
- [comptime.md](comptime.md) for generated declarations.
- [programs.md](../learn/programs.md) for practical project shape.
