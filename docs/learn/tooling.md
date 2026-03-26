# Tooling

These commands cover common build, run, docs, and test loops. `ny --help` and
subcommand help list every flag.

## Common loop

For a single-file change:

```bash
ny file.ny
ny fmt --check file.ny
ny --diag-rich file.ny
ny test --pattern name
```

Use `-run` or `-o` for native executable checks. Use `ny doc search` to find
module or function names.

## Build or install

From a source checkout:

```bash
chmod +x make
./make all
./make install
ny --version
```

Without installing, run commands through the produced `ny` binary from the
build output.

## Run modes

```bash
ny
ny file.ny
ny -c '1 + 1'
ny -ic 'a=1337'
ny --eval-repl 'a=1337'
ny -run file.ny
ny -o app file.ny
ny -i
ny --interactive
```

| Form | Behavior |
| --- | --- |
| `ny` | Start the REPL, or read piped stdin as REPL batch input. |
| `ny file.ny` | Run through the JIT path. |
| `ny -c 'code'` | Run inline source. |
| `ny -ic 'code'`, `ny -ci 'code'` | Run inline source, then enter the REPL with that state. |
| `ny --eval-repl 'code'` | Long spelling for inline source followed by REPL. |
| `ny -run file.ny` | Build and run a temporary native executable. |
| `ny -o app file.ny` | Emit a native executable. |
| `ny -i`, `ny --interactive` | Start the REPL. |

Default `ny file.ny` uses the JIT path. Native `-o` builds use the default
optimized native profile. JIT and REPL defaults use edit-latency settings.

## Format and audit

```bash
ny fmt --fix file.ny
ny fmt --check file.ny
ny fmt --smart --checks file.ny
ny fmt --bugs --limit 80 file.ny
ny fmt --trim --check file.ny
ny fmt --cloc path
ny fmt --dead path --limit 80
ny fmt --specialize file.ny
ny fmt --metaprog file.ny
ny fmt --modules path
```

Formatting changes source layout. Audit modes report cleanup candidates, likely
bugs, stricter checks, specialization candidates, compile-time generation
candidates, module shape, and line counts. Use `--apply` only after reviewing
the reported change class.

## Documentation search

```bash
ny doc search [--docs|--symbols] query
ny doc get query
ny doc -o docs
```

`ny doc search` searches prose pages, modules, exported symbols, docstrings,
and keyword tags. Use `--symbols` when you know you need an API name. Use
`--docs` when you are looking for a concept such as imports, packages, or
native ownership. `ny doc -o docs` writes the static HTML reference.

## Diagnostics

```bash
ny --diag-compact --collect-errors file.ny
ny --diag-rich file.ny
ny --safe-mode file.ny
ny --strict file.ny
ny --strict-types file.ny
ny --borrow-check file.ny
ny --borrow-check --ownership-strict file.ny
ny --heap=gc file.ny
ny --max-errors=20 file.ny
ny --warn=none file.ny
ny --warn=useful file.ny
ny --warn=all file.ny
ny --clean-cache
```

Compact diagnostics collect the failure set. Rich diagnostics print wider
source spans. `--safe-mode` is the full safety profile: strict types,
ownership/borrow checks, RC/RAII cleanup, strict effect/alias policy, and
safe raw-memory diagnostics. `--strict` enables strict types plus
ownership/borrow diagnostics without enabling the full safe-mode profile.
`--heap=gc` or `-gc` enables the opt-in runtime collector. Strict type mode
alone promotes dynamic type
cliffs to compile-time errors. Borrow-check mode validates ownership/borrow
contracts and `--ownership-strict` promotes ownership escapes to hard
compile-time errors. `--max-errors=N` controls the parser error cap; use `0`
to disable the cap.

See [diagnostics.md](diagnostics.md) for the debugging order and
[errors.md](../spec/errors.md) for language-level failure forms.

## Packages

```bash
ny new myapp
ny pkg init myapp
ny pkg info
ny pkg search [--interactive] query
ny pkg repo list
ny pkg add foo ./deps/foo
ny get bar
```

Package layout and resolver behavior are in [packages.md](packages.md).

## Tests

```bash
ny test
ny test --pattern name
ny test --with-stdlib module-or-path
```

Executable checks are in [testing.md](testing.md).

Use a focused pattern for one area. Run the wider matrix for compiler, runtime,
stdlib, docs-generator, or public API changes.

## Compile-time audits

```bash
ny fmt --metaprog file.ny
ny fmt --specialize file.ny
ny fmt --trim --check file.ny
```

Compile-time source generation is in [comptime.md](../spec/comptime.md) and
[metaprogramming.md](metaprogramming.md).

For code that needs compile-time range or bounds guarantees, use
`assert_compile`, `assert_compile_range`, and `assert_compile_index` in the
source and run the file normally; failures are compiler diagnostics.

## Performance

```bash
ny perf
ny -o build/cache/bench/app bench.ny
ny -O3 --profile=peak -o build/cache/bench/app.peak bench.ny
ny fmt --cloc path
```

Performance comparisons are identified by command line, binary, input, cache
state, and environment. Native `-o` builds use the default optimized profile.
`--profile=peak` is for peak native-speed checks. `--profile=compile`
measures compiler latency rather than runtime throughput.

Use [performance.md](performance.md) for timing and profiling discipline.

For a first-file path, use [start.md](start.md). For common failures, use
[troubleshooting.md](troubleshooting.md).
