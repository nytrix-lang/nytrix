<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":140,"summary":"Function declarations, calls, closures, returns, and checked function contracts."} -->
# Functions

Function rules are shared across execution backends; support for a particular
native or foreign signature can still vary. See [Native](native.md) for ABI
boundaries.

## Bindings and declarations

`def` declares an immutable binding; `mut` permits reassignment. An optional
type annotation precedes the binding name. `del` clears a mutable binding; it
does not free an object.

```text
def name = expression
mut name = expression
def Type name = expression
mut Type name = expression

def name(parameters) { body }
fn name(parameters) Type { body }
fn name(parameters) Type = expression
```

A function's final expression is its result when no explicit `return` exits
first. A string literal at the start of a function body is its docstring.

## Parameters and calls

Parameters may be inferred or explicitly typed. A parameter can have a default;
`...rest` collects additional positional arguments. Arguments are evaluated
left to right. Omitted defaults are evaluated at the call site in declaration
order.

```text
name
Type name
name = default
Type name = default
...rest

function(positional)
function(name = value)
```

A lambda uses `fn(parameters) { body }`. Function values and lambdas can capture
bindings from their enclosing scope. Their parameter and return types may be
inferred when the call site provides enough information. For explicit callable
shapes, use `fnptr` where required by an API.

## Return and container behavior

`return expression` exits the current function. Without an explicit return, the
last expression supplies the result. A function call does not implicitly copy
or deep-clone a managed container; use the owning collection API or `clone`
when a detached value is required. See [Values](values.md) for collection
mutation and alias behavior, and [Ownership](ownership.md) for resource
contracts.

## Attributes

Attributes attach compiler metadata to a function. They are contracts only to
the extent the checker or selected backend implements them; see
[Implementation status](../learn/status.md) before relying on a partial rule.

| Goal | Form | Meaning |
| --- | --- | --- |
| Declare effects | `@effects(...)`, `@pure` | Restrict the effects accepted by the effect checker. `@pure` means no declared effects. |
| Describe ownership | `@borrows(x)`, `@returns_borrow(x)`, `@returns_owned`, `@consumes(x)`, `@mutates(x)`, `@releases(x)`, `@forgets(x)` | State argument and return ownership facts for supported ownership checks. |
| Bound use counts | `@allowance(x, [lo, hi])` | State a supported path-sensitive usage interval. |
| Require compile-time evaluation | `@consteval` | Mark a function for compile-time-safe evaluation. |
| Lower eligible IO calls as tasks | `@async_effects` | Request stackless task lowering for supported direct calls. |
| Run work on a thread | `@thread` | Statement-position calls detach; value-position calls join on supported routes. |
| Set a per-function optimization level | `@optimize(0|1|2|3)` | Override NYIR optimization level for that function. |
| Guide call handling | `@inline`, `@noinline`, `@tailcall`, `@flatten` | Request supported inlining, tail-call, or flattening behavior. |
| Mark frequency or timing intent | `@hot`, `@cold`, `@constant_time` | Provide compiler policy metadata; target support varies. |
| Request accelerator code | `@accel`, `@accel(spirv)` | Request a validated accelerator path or receive an unsupported-target error. |
| Require target-specific code | `@naked`, `asm(...)`, `intrinsic(...)` | Use only on a backend that supports the requested form. |

Use semantic contracts for portable behavior. Optimization hints do not replace
measurement or change source-level operator precedence.

## Preconditions and termination measures

`requires(expression)` states a condition that each call must satisfy.
`decreases(expression)` supplies a measure for supported recursive
`@total` checks. The solver is bounded: false and unknown obligations are
rejected, and `decreases` alone is not a termination proof. See
[Proofs](../learn/proofs.md) for examples and [Types](types.md) for the accepted
proof boundary.

## Example

```ny
use core

fn clamp(int value, int lower, int upper) int {
   if value < lower { return lower }
   if value > upper { return upper }
   value
}

#main {
   assert_eq(clamp(12, 0, 10), 10, "upper bound")
}
```

## Related

- [Types](types.md) for parameter and return types.
- [Control Flow](control-flow.md) for exits and cleanup.
- [Programs](../learn/programs.md) for scripts, modules, and entry points.
- [Ownership](ownership.md) for resource and borrow contracts.
