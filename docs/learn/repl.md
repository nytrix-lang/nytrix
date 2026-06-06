# REPL

The REPL evaluates Nytrix source interactively. It keeps imports, definitions,
and lazy-loaded modules between prompts, so file execution remains the cleaner
record for bugs, tests, and benchmarks.

## Start

```bash
ny
ny -i
ny --interactive
ny --plain-repl
ny -ic 'a=1337'
ny --repl < file.ny
```

`--plain-repl` uses plain terminal input. `--repl` reads stdin as source once;
prompt commands such as `:help`, `:snapshot`, and `:load` are interactive-only.

## Use

| Task | Form |
| --- | --- |
| Evaluate expression | Type it directly. |
| Import a module | `use module.path` |
| Search APIs | `ny doc search --symbols name` |
| Inspect a concept | `ny doc search --docs topic` |
| Reproduce exactly | Move the snippet to a file and run `ny file.ny`. |

Example:

```ny
use std.core
use std.math

fn midpoint(number a, number b) number {
   (a + b) / 2
}

assert(midpoint(2, 6) == 4, "midpoint")
```

## State Model

A REPL session may already contain:

- previous imports;
- previous definitions;
- lazy stdlib modules loaded by earlier expressions;
- an unfinished multiline form after a bad paste.

For file-equivalent diagnostics:

```bash
ny --diag-rich file.ny
ny -run file.ny
ny fmt --check file.ny
```

`comptime{ ... }` follows normal import rules in the REPL: session imports are
visible, immutable compile-time constants can be reused, and runtime globals
are not captured.

## Imports And Completion

Owning-module imports keep completion and file behavior aligned:

```ny
use std.math
abs(10)

use std.math as math
math.abs(10)
```

Receiver completion depends on the imported API and the receiver shape:

```ny
[1, 2, 3].len
```

Indexed loops bind value first, index second:

```ny
for ch, i in "test" { print(f"{ch}:{i}") }
```

## One-Shot Probes

Use `-c` for source strings that do not need prompt state:

```bash
ny -c 'use std.math
assert(abs(10) == 10, "abs")'
```

Use files for quotes, multiline source, imports, packages, native declarations,
and reproducible output.

## Snapshots

```text
ny> def doc_value = 7
ny> :snapshot app.nys
ny> :load app.nys
ny> doc_value
7
```

Use `:snapshot app.nys -o app` to export through the native AOT path.

## Debug Loop

| Symptom | Next command |
| --- | --- |
| Unknown name | `ny doc search --symbols name` |
| Import shape unclear | `ny doc search --docs import` |
| Type mismatch | `ny --diag-rich file.ny` |
| REPL/file disagreement | `ny file.ny` then `ny -run file.ny` |
| Formatter issue | `ny fmt --check file.ny` |
| Parser issue | `ny -dump-tokens file.ny` or `ny --expand file.ny` |
| Slow paste | Move to a file and run `ny -time file.ny`. |

## Transcript Rule

A useful transcript records imports, source, result or diagnostic, and no
hidden setup.

## Related

- [tooling.md](tooling.md)
- [diagnostics.md](diagnostics.md)
- [programs.md](programs.md)
