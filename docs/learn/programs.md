<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":150,"summary":"Choose script or module form and keep public exports separate from startup code."} -->
# Programs

A source file can be a script, a module, or both. Use a script for a standalone
program. Add a `module` declaration when other files need its exported names.

## Script

Top-level statements run when a script is executed:

```ny
use core

def name = "Nytrix"
assert_eq("hello, " + name, "hello, Nytrix", "greeting")
print("hello, " + name)
```

Run it with `ny hello.ny`.

## Module with an entry point

A module lists the names available to import. Put direct startup work in
`#main`; importing the module does not run that block.

```ny
use core

module mathx(double)

fn double(int value) int {
   value * 2
}

#main {
   assert_eq(double(21), 42, "module entry")
}
```

The exact export and entry-point rules are in [Units](../spec/units.md).

## Imports

Import a module namespace or selected exported names. Aliases make the owner
visible at each call site.

```text
use core.os.net as net
use core.math.parse.data.json as json
use mathx (double)
use local:debug
```

Imports resolve before ordinary declarations. A missing module, export, or
profile is a compile-time diagnostic.

## Project layout

Keep reusable code in modules and application startup in one entry point. Use
`ny new <dir>` for a starter project, then add modules around clear public
interfaces. `module name { export profile(...) }` can publish a named profile
when the same module needs multiple public surfaces.

Examples under [`etc/projects/`](../../etc/projects/) are complete programs.
[Examples](examples.md) links selected files; [Packages](packages.md) covers
external source dependencies.
