<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":112,"summary":"Express borrowing, ownership transfer, and cleanup at resource boundaries."} -->
# Ownership

Ownership contracts make resource transfer visible in a function signature.
Use them at APIs that retain, release, or return managed values.

## Borrow or move

| Form | Meaning | Use |
| --- | --- | --- |
| `borrow(x)` | Explicit borrowed view. | Name a borrow at an API boundary. |
| `&x` | Borrow shorthand. | Keep a local call compact. |
| `@borrows(x)` | Function reads a borrowed argument. | Callee does not take ownership. |
| `@consumes(x)` | Function takes ownership. | Caller must not use the moved value. |

Both borrow spellings use the same ownership checker.

## Decide: borrow or move

Ask one question at each boundary: does the callee keep using the value after
the call, and does the caller still need it?

| Situation | Use |
| --- | --- |
| Callee only reads; caller keeps using the value | `borrow(x)` or `@borrows(x)` |
| Callee stores, closes, or releases the value | `own(value)`, `@consumes(x)`, and/or `@releases(x)` |
| Callee returns a view tied to a parameter | `@returns_borrow(x)` |
| Callee hands a fresh object to the caller | `@returns_owned` |

When a borrow would outlive its owner, the checker reports it. End the borrow
scope earlier, clone, or return an owned value instead.

## Return ownership deliberately

| Attribute | Contract |
| --- | --- |
| `@returns_owned` | Caller receives an owned result. |
| `@returns_borrow(x)` | Result borrows from parameter `x`. |
| `@mutates(x)` | Function changes the argument. |
| `@releases(x)` | Function releases the owned argument. |
| `@forgets(x)` | Function intentionally abandons automatic cleanup. |

```ny
use std.core

@borrows(x)
@returns_borrow(x)
fn identity(any x) any { borrow(x) }

assert(identity(7) == 7, "borrowed result")
```

## Common diagnostics

| Diagnostic | Fix direction |
| --- | --- |
| `cannot release owned slot ... while borrow ... is live` | End the borrow scope, clone, or keep passing a borrow. |
| `use after move of owned slot` | Borrow before moving, clone, or create a new owned value. |
| `returning owned slot ... requires @returns_owned` | Add `@returns_owned` or return a borrow or clone. |
| `returning borrow of local owner would outlive its slot` | Return an owned value or tie the borrow to a parameter with `@returns_borrow(name)`. |

## Check ownership

```bash
ny --borrow-check file.ny
ny --borrow-check --ownership-strict file.ny
ny --safe-mode file.ny
```

Use the smallest scope that keeps a borrow live. Return an owned value when a
borrow would outlive its owner. Use a `with` scope for resources whose cleanup
must happen at scope exit.

## Related

- [Runtime](../spec/runtime.md#ownership)
- [Functions](../spec/functions.md#ownership-contracts)
- [Proofs](proofs.md)