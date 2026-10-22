<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":170,"summary":"Use the compiler, formatter, test runner, documentation search, and target checks."} -->
# Tooling

Use `./make` for repository builds and test suites; use `ny` for source-level
commands. Query the installed command when options matter:

```bash
./make --help
ny --help
ny doc --help
```

## Run and inspect

```bash
ny file.ny
ny -c 'print(1 + 1)'
ny -run file.ny
ny -o app file.ny
ny --native-only file.ny
```

These commands exercise different paths. See [Execution routes](routes.md)
before treating a successful run as evidence for another backend.

## Format and test

```bash
ny fmt --check file.ny
ny fmt --fix file.ny
./make test --pattern path-or-name
./make web-test
```

`--check` is read-only; `--fix` writes formatting changes. Use the narrowest
fixture pattern that covers the behavior, then run broader tests as required by
the change. See [Testing](testing.md).

## Find API documentation

```bash
ny doc search --symbols function_name
ny doc search --docs topic
ny doc get module.name
```

The results depend on the checked-out source and its generated docs. See
[Library](library.md) for a search workflow.

## Diagnose a failure

First rerun the failing command without changing its route or flags. Then
separate parse/type errors from runtime, linker, and host-capability failures.
Use `ny --help` for diagnostics and NYIR options; use
[Troubleshooting](troubleshooting.md) to identify the next owner to inspect.
