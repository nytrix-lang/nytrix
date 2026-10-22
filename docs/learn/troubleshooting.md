<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":180,"summary":"Reduce failures to one source, compiler, runtime, or host boundary and reproduce them."} -->
# Troubleshooting

Keep the failing command, source, flags, and first useful diagnostic. Reduce the
input until one import, type, runtime service, or native boundary owns the
failure.

## Triage

1. Rerun the command without changing its route or flags.
2. Use compact diagnostics to collect failures; use rich diagnostics to inspect
   source context.
3. Search the current API docs for unresolved names.
4. Separate compile-time errors from runtime and host-capability failures.
5. Reduce the input and add an assertion or focused fixture for the result.

```bash
ny --diag-compact --collect-errors file.ny
ny --diag-rich file.ny
ny doc search --symbols missing_name
```

## Imports and methods

Check that the module exports the exact name and that the import is in scope.
Receiver syntax is available only when the owning module documents it; otherwise
call the exported function directly. Use `ny doc get module.name` to inspect an
entry.

## Type and contract failures

Read the first source-located diagnostic. If a value is dynamic, add the
required type annotation or use a checked conversion. Use `--strict-types` to
reject supported dynamic type cliffs. For proof, effect, or ownership
boundaries, read the corresponding spec; an unknown obligation is not evidence
that the program is safe.

## Collections and strings

`list(n)` reserves capacity but starts empty. Keep the returned value from
`append` or another operation that may replace the list:

```ny
use core

mut xs = list(4)
xs = xs.append("first")
assert_eq(xs[0], "first", "retained list update")
```

Strings have byte length; string indexing/slicing uses the documented
code-point rules. Use bytes APIs for binary protocols and explicit length-based
FFI calls when the native contract requires them.

## Ownership and native failures

Run borrow checking when values cross resource or FFI boundaries:

```bash
ny --borrow-check --ownership-strict file.ny
ny --safe-mode file.ny
```

For native failures, check ABI layout, pointer lifetime, handle ownership, and
text-versus-byte conversion. Verify the same target and backend that the
application uses; a successful default route does not establish native-only or
Wasm support.

## External services and packages

Set explicit timeouts and check returned status/error values for network calls.
Prefer local services in tests. For an import failure, compare the import name
with the manifest dependency, then inspect `ny pkg path` and the lockfile.

See [Native](native.md), [Networking](networking.md), [Packages](packages.md),
[Execution routes](status.md#execution-routes), and [Testing](testing.md).
