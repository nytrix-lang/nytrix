<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":108,"summary":"Declare and check observable work at function boundaries."} -->
# Effects

Effects state which observable work a function may perform. Put the contract on
the function that owns the boundary, then run strict checks in the target that
requires it.

## Declare a contract

```ny
use std.core

@effects(none)
fn add(int a, int b) int { a + b }

@pure
fn square(int n) int { n * n }

assert(add(2, 3) == 5, "pure arithmetic")
assert(square(4) == 16, "pure helper")
```

`@pure` is the concise contract for a function without observable effects.
`@effects(...)` names the allowed set.

| Contract | Meaning |
| --- | --- |
| `none` | No observable effect. |
| `io` | Input or output. |
| `alloc` | Allocation. |
| `ffi` | Foreign boundary. |
| `thread` | Threading work. |
| `all` | Any supported effect. |

## Choose the smallest mask

| The function does | Declare |
| --- | --- |
| Pure arithmetic and reads | `@effects(none)` or `@pure` |
| Terminal, file, or network work | `@effects(io)` |
| Allocation beyond scalar results | `@effects(alloc)` |
| A call into a foreign library | `@effects(ffi)` |
| Background threads or workers | `@effects(thread)` |

Widening a helper to `all` hides which boundary the work crosses. Move
effectful work behind a declared boundary instead of widening unrelated
helpers to `all`.

## Async boundaries

Use `@async_effects` when a function's asynchronous work has an explicit
effect contract. Keep the declaration at the public async boundary; the
compiler checks calls and inferred work against it.

## Check a target

```bash
ny --strict-types file.ny
ny --safe-mode file.ny
```

The compiler rejects inferred `io`, `alloc`, `ffi`, or `thread` effects outside
the declared mask. Run the strict checks in the target that requires the
contract.

## Related

- [Functions](../spec/functions.md#attributes)
- [Runtime](../spec/runtime.md#attributes-and-effects)
- [Ownership](ownership.md)