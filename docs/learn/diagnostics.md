# Diagnostics

Use diagnostics as a narrowing loop: collect the failure set, open source
context when needed, then classify the error as import, type, runtime, or
native-boundary work.

## Triage order

1. Run the same command again with compact collection.
2. Switch to rich diagnostics when the source location matters.
3. Find the owner of unknown names with `ny doc search --symbols`.
4. Turn on strict types when the value shape is unclear.
5. Turn on borrow checking when ownership, returned references, or native
   resources are involved.
6. Shrink the input until one file, value, or boundary owns the failure.

## First command

```bash
ny --diag-compact --collect-errors file.ny
```

Compact diagnostics collect the failure set without source snippets. Rich
diagnostics trade density for source context.

```bash
ny --diag-rich file.ny
```

## Import failures

Undefined names mean one of:

- missing `use`
- wrong exported name
- local name out of scope
- package not installed in the active root

Search before guessing:

```bash
ny doc search --symbols symbol_name
ny doc get module.name
```

If the symbol exists, import that module. If it does not, check spelling,
package roots, and whether the name is private to another module.

Undefined-symbol diagnostics include close spelling matches when the compiler
can find one in scope. For module-qualified calls, the diagnostic also prints
the module's exported names when that list is available.

## Type failures

Run strict mode when a value shape is unclear:

```bash
ny --strict-types file.ny
```

Then isolate where the value is built. Dict literals, receiver calls, index
access, nullable values, and `Result` payloads are common refinement points.

For list-building code, remember that `append` returns the updated list:

```ny
mut xs = []
xs = xs.append(1)
```

## Ownership failures

Run borrow checking when a value crosses ownership boundaries:

```bash
ny --borrow-check --ownership-strict file.ny
```

Common ownership diagnostics mean:

| Diagnostic | Fix direction |
| --- | --- |
| `cannot release owned slot ... while borrow ... is live` | End the borrow scope, clone, or keep passing a borrow. |
| `use after move of owned slot` | Borrow before moving, clone, or create a new owned value. |
| `returning owned slot ... requires @returns_owned` | Add `@returns_owned` or return a borrow/clone instead. |
| `returning borrow of local owner would outlive its slot` | Return an owned value or tie the borrow to a parameter with `@returns_borrow(name)`. |

Borrow syntax is either `borrow(x)` or `&x`; both go through the same checker.

## Runtime failures

Use a small input and one assertion per behavior. For processes, sockets, and
HTTP, set explicit timeouts and capture the transcript or response metadata.

If a runtime failure depends on environment state, record the environment value
or local fixture command beside the reproducer.

## Native failures

Check these in order:

| Area | Check |
| --- | --- |
| Layout | Field order, field size, alignment. |
| Pointer | Lifetime, nullability, element type. |
| Handle | Create/use/close contract. |
| String | Text, UTF-8 bytes, C string, or pointer plus length. |
| Ownership | Who frees returned memory or closes the resource. |

## Formatting and audit

```bash
ny fmt --check file.ny
ny fmt --bugs --limit 80 file.ny
```

Formatting checks layout. Bug audits report suspicious source patterns without
changing behavior. For command families, use [tooling.md](tooling.md).
