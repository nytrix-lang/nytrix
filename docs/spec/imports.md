# Imports

Imports make names from modules, files, and packages visible in the current
source unit. Import resolution happens before normal execution.

## Forms

```text
use std
use std.core
use std math as math
use std.math as math
use std.parse.data.json (json_decode, json_encode)
use std.parse.data.json (json_decode as decode_json)
use "./local_helpers.ny" as helpers
use "./local_helpers.ny" (helper)
use "./local_helpers.ny":debug
use package_name
use module.path *
```

`use module.path` imports exported names. `as` binds an explicit module alias.
Item lists import only selected public names.

`use std` imports the root standard-library surface. It exposes common root
names and standard namespace aliases such as `std.math`, `math`, `os`, `OS`,
and `ARCH`. `use std module as alias` is accepted as a loose spelling for
`use std.module as alias`.

Profile imports select a grouped export profile:

```ny
use "./local_helpers.ny":debug
use std.core:debug *
```

The unqualified profile import brings the module core export group plus the
named profile into scope. Item imports still use the module's exported public
surface.

`use module.path *` is accepted as a broad legacy import form. Public examples
use explicit module imports, aliases, or item lists.

There is no implicit global `std` object before import. This is not valid unless
`std` or another alias is introduced by the current source:

```text
std.math.abs(-3)
```

Use a direct import or an alias.

## Import style

| Shape | Use when |
| --- | --- |
| `use std.core` | The module exports common names used directly. |
| `use std.math as math` | Calls stay visibly namespaced. |
| `use std.parse.data.json (json_decode)` | Only one or two names are needed. |
| `use "./helper.ny" (helper)` | A local file provides a private helper. |
| `use module.path *` | Legacy or local code that intentionally imports the full surface. |

Choose the most local import that keeps the call site clear. A crypto solver
that uses only byte conversion can import `std.math.bin as bin` instead of a
broad crypto namespace.

## Resolution

Resolution checks the active source root, standard library modules, package
roots, configured package paths, and relative file imports according to the
toolchain resolver. Missing imports are compile-time errors.

`comptime{ ... }` uses the same resolved imports and aliases as ordinary code.
Unimported names stay unavailable. Immutable compile-time constants can be used
by later blocks; runtime globals are not captured.

Import statements are source forms, not shell commands:

```ny
use std.math
```

There are no semicolon terminators. `;` starts a comment.

## Packages

Installed package names can be imported like modules:

```text
use package_name
use package_name.submodule as sub
```

Use package commands to inspect what the resolver can see:

```bash
ny pkg info
ny pkg path
ny pkg search query
ny pkg repo list
```

If package import fails, check the selected install root and lockfile before
changing source code.

## Standard-library root

```ny
use std
std.math.abs(-11)
math.abs(-9)
os.getcwd()

use std.math
abs(10)

use std.math.crypto.hash
sha1("abc")
```

`use std` exposes root namespaces and common globals. Importing the owning
module still gives narrower diagnostics and call sites when a file uses a small
part of the standard library.

## Aliases

Aliases keep call sites short while preserving the real module boundary:

```ny
use std.parse.data.json as json

def obj = json.json_decode("{\"ok\": true}")
```

Use aliases when a module is used repeatedly or when two modules export similar
names.

## Qualified names

Alias-qualified calls keep the origin visible when local imports would hide it:

```ny
use std.math as math

math.abs(-3)
```

If a diagnostic suggests a module-qualified name, import that module directly or
bind an alias and call through the alias.

## Item imports

Item imports fit short public examples:

```ny
use std.math (abs, max)

assert(abs(-3) == 3, "abs")
assert(max(2, 5) == 5, "max")
```

For large item lists, import an alias and make the namespace visible.

## Re-exports and child modules

A local module can re-export imported names:

```ny
module ReExportMath(ceil, floor){
   use std.math.float (ceil, floor)
}

use ReExportMath as remath
use ReExportMath (floor as re_floor)
```

Package and parent modules can expose child modules as part of their public
surface. Importing the parent makes those exported child names available
through the parent alias and through normal exported names.

## Diagnostics

Import failures fall into these groups:

| Diagnostic shape | Check |
| --- | --- |
| Module not found | Wrong package root, missing std bundle, or typo in path. |
| Symbol not found | Module imported, but selected item is not exported. |
| Ambiguous short name | Use an alias and call `alias.name`. |
| REPL differs from file | Previous REPL imports are hiding the real file state. |

Run:

```bash
ny --diag-rich file.ny
ny doc search --symbols name
ny doc search --docs import
```

## Related

- [source.md](source.md) for source-unit layout.
- [modules.md](modules.md) for exported names.
- [packages.md](../learn/packages.md) for package resolution.
- [library.md](../learn/library.md) for standard-library lookup.
