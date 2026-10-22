<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":30,"summary":"Find an exported standard-library function and inspect its documentation with ny doc."} -->
# Find a library API

Search before guessing a module or function name. `ny doc` indexes generated
API documentation and source documentation from the current checkout.

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

## Read an API entry

Check the documented signature, argument order, return type, effects, ownership,
and failure result. Prefer the narrowest module that owns the behavior. If an
entry is missing or its contract is unclear, inspect the defining module under
`lib/` and add or improve its source documentation; do not duplicate an
unverified signature in a tutorial.

Related guides: [Networking](networking.md), [Native interfaces](native.md),
[UI](ui.md), and [Comptime](comptime.md).
