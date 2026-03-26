# REPL

The REPL evaluates Nytrix source interactively. It keeps imports, definitions,
and loaded lazy modules between prompts.

## Start

```bash
ny
ny -i
ny --interactive
ny -ic 'a=1337'
ny -ci 'a=1337'
```

Small check:

```ny
use std.core
def xs = [1, 2, 3]
xs.len
```

Inside the REPL, `:help syntax` prints syntax differences that affect pasted
source. Files use the same parser, compiler, formatter, and test commands.

## Common actions

| Task | Form |
| --- | --- |
| Evaluate an expression | Type the expression directly. |
| Load an import | Run `use module.path`. |
| Search APIs | `ny doc search --symbols name` or completion. |
| Check receiver members | Import the owning module, then type `value.`. |
| Reproduce file behavior | Put the same source in a file and run `ny file.ny`. |

## State

The REPL is stateful after the first prompt. It may already have:

- earlier imports;
- earlier definitions;
- failed multiline input;
- lazy stdlib modules loaded by previous expressions.

For file-equivalent diagnostics, put the source in a file and run:

```bash
ny --diag-rich scratch.ny
ny -run scratch.ny
```

Pasted source can also fail while the interactive buffer is inside an unfinished
form. A complete file removes previous prompt state from the result.

## Search while typing

The REPL completion surface is backed by the current parser, imports, and
generated docs:

```bash
ny doc search --symbols abs
ny doc search --docs import
ny doc get abs
```

Symbols exported by modules are available through direct imports or aliases:

```ny
use std.math
abs(10)

use std.math as math
math.abs(10)
```

Narrower imports reduce the completion surface:

```ny
use std.math
abs

use std.math.crypto.hash
sha1
```

Indexed loops bind the value first and the counter second:

```ny
for x, i in "test" { print(f"{x} iter is {i}") }
```

Member-style API shape:

```ny
[1, 2, 3].len
```

Completion prioritizes members that match the receiver shape. File diagnostics
provide a stable source unit when completion and compilation differ.

## Lazy imports

`use std` exposes the root standard-library surface. Module-owned symbols are
available through their module import:

```ny
use std.math
abs(10)
```

Alias-qualified form:

```ny
use std.math as math
math.abs(10)
```

Owning-module imports keep REPL behavior aligned with file behavior.

`comptime{ ... }` follows the same rule in the REPL. Session imports are
visible, immutable compile-time constants can be reused, and runtime globals are
not captured.

## Pasted source

Complete pasted source includes imports, declarations, and checks:

```ny
use std.core
use std.math

fn midpoint(number: a, number: b): number {
   (a + b) / 2
}

assert(midpoint(2, 6) == 4, "midpoint")
```

File execution makes import state, prior bindings, and execution mode explicit.

## Multi-line snippets

Declarations are complete forms:

```ny
fn midpoint(number: a, number: b): number {
   (a + b) / 2
}
```

File commands:

```bash
ny scratch.ny
ny -run scratch.ny
ny fmt --check scratch.ny
```

Module declarations are valid in the REPL. Repeated edits to the same module
accumulate in the stateful session.

## One-line probes

`-c` runs one source string without entering a stateful prompt:

```bash
ny -c 'use std.math
assert(abs(10) == 10, "abs")'
```

Files avoid shell-escaping issues for quotes, multiline blocks, and imports.

## Debug loop

| Symptom | Next command |
| --- | --- |
| Unknown name | `ny doc search --symbols name` |
| Import shape unclear | `ny doc search --docs import` |
| Type mismatch | `ny --diag-rich file.ny` |
| REPL/file disagreement | `ny file.ny` and then `ny -run file.ny` |
| Formatter suggestion | `ny fmt --check file.ny` |
| Suspected parser issue | `ny -dump-tokens file.ny` or `ny --expand file.ny` |
| Slow pasted program | `ny -time file.ny` after moving it to a file |

## File boundary

File-based checks record:

- more than one import;
- a `module` declaration;
- native `layout` or `extern` declarations;
- package dependencies;
- reproducible benchmark or test output;
- a bug report.

The REPL and file runner share the compiler, but the REPL also has previous
prompt state.

## Transcript shape

A transcript records:

- the exact imports used;
- the expression or snippet;
- the diagnostic or result;
- no unrelated earlier state.

Missing setup belongs in the transcript or in a file.

## REPL session shape

```text
ny> use std.math
ny> abs(10)
10
ny> [1, 2, 3].
```

This transcript shows the import, call, result, and completion context. Hidden
setup is part of the result when it changes the final value.

## Related

- [tooling.md](tooling.md) for command modes.
- [diagnostics.md](diagnostics.md) for error triage.
