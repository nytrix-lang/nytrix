<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":30,"summary":"Find an exported standard-library function and inspect its documentation with ny doc."} -->
# Find a library API

Search before guessing a module or function name. `ny doc` indexes generated
API documentation and source documentation from the current checkout.

Prerequisites: `ny` on `PATH` with its library documentation available.

```bash
ny doc search --symbols print
ny doc search --docs socket
ny doc get core.os.net
```

Use `--symbols` when you know the operation you want; use `--docs` for a topic
or behavior. `get` prints the best matching documented item. Run `ny doc
--help` for input, output, and site-generation options.

## Start with the owning module

| Need | First place to search |
| --- | --- |
| Strings, collections, assertions, and common values | `core` and its focused modules |
| Files, processes, time, threads, and host services | `core.os` |
| Numeric algorithms, parsers, data formats, and cryptography | `core.math` |
| Windowing, input, and rendering | `core.os.ui` |
| HTTP, sockets, and related transports | `core.os.net` |

These are starting namespaces, not guarantees that every API works on every
target. Check the symbol documentation and the matching platform guide before
relying on a host capability.

## Parse command-line arguments

Use `core.os.args` for direct access to `argv`, positional arguments, and
simple value/flag lookups. Use `core.os.cli` when a script needs a declared
interface with generated help, typed options, short clusters, families,
subcommands, or response files.

`core.os.cli` builds a specification from descriptors and returns a result
dictionary. On success, `values` contains named options and positional fields;
`provided` identifies which options were explicitly present (including options
given an empty value);
`help` and `text` carry generated help, version, or completion output. On
failure, `format_error(result)` produces a one-line `program: message` string.
The parser does not terminate the process, so the script controls whether to
print help or how to handle errors.

Options support string/path, integer, float, and comma-separated list values,
plus repeatable values and environment defaults. Response files split on
spaces; shell quotes and escapes are not interpreted.
The parser's `--` boundary is consumed. Arguments after it are preserved in a
declared `rest` field. A response-file argument such as `@flags.txt` expands
before the boundary; `@...` after `--` remains program data.

See the executable regression example in
[`cli-parser.nshape`](../../etc/tests/code/intermediate/ny/cli-parser.nshape)
for short clusters, families, `--no-` booleans, value validation, generated
help/completions, subcommands, repeatable options, conflicts, and response
file boundaries.

## Read an API entry

Check the documented signature, argument order, return type, effects, ownership,
and failure result. Prefer the narrowest module that owns the behavior. If an
entry is missing or its contract is unclear, inspect the defining module under
`core/` and add or improve its source documentation; do not duplicate an
unverified signature in a tutorial.

Related guides: [Networking](networking.md), [Native interfaces](native.md),
[UI](ui.md), and [Comptime](comptime.md).

Next:

- [Networking](networking.md) — look up a network API.
- [Native interfaces](native.md) — check foreign function declarations.
- [Comptime](comptime.md) — inspect compiler APIs and generated forms.
