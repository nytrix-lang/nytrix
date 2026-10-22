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

These commands exercise different paths. See [Execution routes](status.md#execution-routes)
before treating a successful run as evidence for another backend.

For source inspection without execution, use `ny --dump-ast file.ny` or
`ny --dump-ast-typed --dump-ast-filter=name file.ny`.
The [debugging guide](debugging.md#ast-inspection) explains the tree and checks.

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

In the generated website, **Contents** links open any containing disclosure
before scrolling to a heading. Long outlines scroll within their own panel.
Release highlights are visible in the [changelog](../CHANGELOG.md); each
version's **Details** contains its development record.

The sidebar separates folder groups from page links with disclosure arrows,
indentation, and tree guides. Click an API namespace or module name to open its
page; use its arrow to expand children without navigating. Folder and arrow
buttons support Enter and Space. The selected page stays visible in the tree.
Typing outside interactive controls starts a search; focused navigation
controls keep their normal keyboard behavior.

## Editor navigation

`ny-lsp` provides symbols, definitions, references, rename, and
`textDocument/prepareRename`. Reference/rename discovery excludes line comments
and quoted strings, avoiding edits to those lexical lookalikes. Clients can
preview the selected name and range before applying a workspace edit. This does
not yet provide interactive move-declaration or change-signature refactors.

## Diagnose a failure

First rerun the failing command without changing its route or flags. Then
separate parse/type errors from runtime, linker, and host-capability failures.
Use `ny --help` for diagnostics and NYIR options; use
[Troubleshooting](troubleshooting.md) to identify the next owner to inspect.
