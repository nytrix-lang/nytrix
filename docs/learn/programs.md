# Programs

A Nytrix file can be a script, a module, or both. Use a script when the file is
the program. Use a module when other files import its names. Combine them when
you want exported functions plus local checks.

## Choose the file shape

| Shape | Use when | Main rule |
| --- | --- | --- |
| Script | The file is the whole program or a focused check. | Top-level statements run when the file is executed. |
| Module | Other files import this API. | `module name(exports)` controls the public surface. |
| Script plus module | You want public functions and local assertions together. | Imports see exports; direct execution also runs checks. |
| Package entrypoint | A project has several files or dependencies. | Keep startup in `main` and keep reusable work in modules. |

## Script

A script is a file with top-level statements.

```ny
use std.core

def name = "ny"
assert_eq("hello, " + name, "hello, ny", "greeting")
```

Run:

```bash
ny --color=never hello.ny
```

Silent success means all assertions passed.

## Module

A module declares the names it exports.

```ny
use std.core

module stats(mean)

fn mean(list xs) number {
   mut total = 0
   for x in xs { total += x }
   total / xs.len
}
```

Imports use the module or package name:

```ny
use stats (mean)
```

## Script and module together

A file can export functions and also contain direct-run checks. Imported users
see only the exports. Direct execution runs the `#main` block.

```ny
module mathx(double)

fn double(int x) int { x * 2 }

#main {
   assert_eq(double(21), 42, "double")
}
```

## Entrypoint shape

Small tools can use top-level statements. Files that export reusable helpers can
put only direct startup work in `#main`. Imports see the helpers and skip the
startup block.

```ny
use std.core
use std.os.args as args

fn greet(str name) str {
   "hello, " + name
}

#main {
   def name = args.positionals().get(0, "ny")
   print(greet(name))
}
```

## Imports

Put imports at the top. Aliases keep repeated module calls namespaced.

```ny
use std.core
use std.os.net as net
use std.parse.data.json as json
```

Import the owning module instead of relying on a broad namespace. A visible
alias such as `json.json_decode` or `net.request` keeps the origin at the call
site and in diagnostics.

## Public surface

Public functions use explicit names and types when the type is part of the API.

```ny
fn parse_port(str raw) int {
   int(raw)
}
```

Grouped modules can publish profiles:

```ny
module local {
   export core(run)
   export debug(dump_state)
   internal(_state)
}

use local:debug
```

The default import sees `core`. A profile import sees `core` plus the selected
profile.

## ADTs and typed APIs

Use `enum` when the API returns one of several tagged shapes.

```ny
enum Parse<T> {
   Ok(T: value),
   Err(str: message)
}

fn parse_flag(str raw) Parse<bool> {
   if(raw == "yes"){ Parse.Ok(value: true) }
   else { Parse.Err(message: "expected yes") }
}

match parse_flag("yes") {
   Parse.Ok(value: v) -> assert(v, "flag")
   Parse.Err(message: msg) -> panic(msg)
}
```

Typed containers use angle brackets: `list<int>`, `dict<str, int>`,
`Result<T, E>`, and user ADTs such as `Parse<bool>`.

Use explicit public types when the type is part of the API contract. Inside a
script, inference is valid until a diagnostic or performance profile requires a
narrower type.

## Complete project examples

Project examples live under `etc/projects`. They are complete files, not API
fragments. GitHub shows source links; the generated website embeds selected
project files below.

Keep project files for complete workflows and richer demos. Tiny one-function
snippets belong in learn pages or self-tests instead.

### Rule 110

[Open the source](../../etc/projects/cli/110.ny).

<!-- ny-doc-include-code: ny etc/projects/cli/110.ny -->

### Langton's Ant

[Open the source](../../etc/projects/cli/ant.ny).

<!-- ny-doc-include-code: ny etc/projects/cli/ant.ny -->

### Conway's Game of Life

[Open the source](../../etc/projects/cli/conway.ny).

<!-- ny-doc-include-code: ny etc/projects/cli/conway.ny -->

### Matrix Rain

[Open the source](../../etc/projects/cli/matrix.ny).

<!-- ny-doc-include-code: ny etc/projects/cli/matrix.ny -->

### FFI

[Open the source](../../etc/projects/os/ffi.ny).

<!-- ny-doc-include-code: ny etc/projects/os/ffi.ny -->

### Args

[Open the source](../../etc/projects/os/args.ny).

<!-- ny-doc-include-code: ny etc/projects/os/args.ny -->

### Local Server

[Open the source](../../etc/projects/os/server.ny).

<!-- ny-doc-include-code: ny etc/projects/os/server.ny -->

Run:

```bash
ny etc/projects/os/server.ny
```

### Sound

[Open the source](../../etc/projects/os/sound.ny).

<!-- ny-doc-include-code: ny etc/projects/os/sound.ny -->

### UI Projects

[ui.md](ui.md) covers the engine viewer and focused UI input/project files.

- [ui/engine.ny](../../etc/projects/ui/engine.ny)
- [ui/input.ny](../../etc/projects/ui/input.ny)
- [ui/monitor.ny](../../etc/projects/ui/monitor.ny)
- [ui/term.ny](../../etc/projects/ui/term.ny)

## Related

- [source.md](../spec/source.md) for exact source-unit rules.
- [functions.md](../spec/functions.md) for function and block behavior.
- [testing.md](testing.md) for executable checks.
