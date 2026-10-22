<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":112,"summary":"Choose borrowed or owned APIs and check cleanup at resource boundaries."} -->
# Ownership

Use a borrow when the callee needs temporary access. Transfer ownership when the
callee must retain, release, or return responsibility for a resource. Ownership
analysis is opt-in and bounded; see the [specification](../spec/ownership.md)
for supported rules.

## Choose the contract

| Situation | Contract |
| --- | --- |
| The callee reads a value without keeping it | `@borrows(value)` |
| A returned view refers to an argument | `@returns_borrow(value)` |
| The callee takes responsibility for an argument | `@consumes(value)` |
| The callee releases an owned resource | `@releases(value)` |
| A fresh result belongs to the caller | `@returns_owned` |
| The callee changes a supported argument | `@mutates(value)` |

Declare the contract on the function that owns the boundary. Keep foreign
resource cleanup in a small wrapper, and use `with` when the resource must be
released at scope exit. A borrow must not outlive its owner; clone or return an
owned value when the lifetime cannot be tied to an argument.

## Static checks

```bash
ny --borrow-check file.ny
ny --borrow-check --ownership-strict file.ny
ny --safe-mode file.ny
```

`--borrow-check` reports supported issues without selecting RAII cleanup.
`--ownership-strict` promotes those diagnostics to errors. `--ownership` also
selects ownership-aware cleanup; `--safe-mode` enables the broader safety
profile. These modes do not turn unsupported source shapes into proven-safe
ones.

`@allowance(parameter, [lower, upper])` can constrain how often a parameter is
used on supported control-flow paths. Use it only when the count is part of the
API contract; the checker rejects an allowance it cannot validate.

## Verify the boundary

Test both valid and invalid use for the resource and control-flow shape you
rely on. A pass on one backend does not prove every alias or aggregate form.
See [Testing](testing.md) for focused fixture commands and
[Implementation status](status.md) for known limits.
