# Functions and blocks

Functions cover bindings, parameters, lambdas, return behavior, docstrings, and
block values.

## Bindings

```ny
def name = expr
mut name = expr
def Type name = expr
mut Type name = expr
def a, b = expr
mut a, b
del name
```

`def` creates an immutable binding. `mut` creates a mutable binding. Typed
binding order is `Type name`.

`del name` sets an existing mutable binding to `nil` and clears the compiler's
static facts for that binding. The binding remains readable. `del` rejects
immutable `def` bindings. It is not a native free operation. Use the owning
API's cleanup function, `with`, `release`, or `forget` for resources that need
explicit lifetime handling.

## Function forms

```ny
fn name(params) { body }
fn name(params) Type { body }
fn name(params) Type = expr
fn(v){ expr }
fn(a, b){ body }
```

Named functions bind a public or local function name. `fn(...) { ... }` creates
an inline callable value.

The parser accepts `lambda(...) { ... }` as a compatibility spelling with the same
parameter-list and return-type syntax as `fn(...) { ... }`. New code should use
`fn`.

Return types follow the parameter list without `:`. The parser reserves `->`
for pattern/case arms.

## Parameters

```ny
name
Type name
name = default
Type name = default
...rest
```

Parameter types belong to the callable surface. The call path evaluates
defaults according to the function definition.

## Blocks

Blocks use braces:

```ny
{ statement* expr? }
```

An empty block evaluates to `nil`. A block with a final expression evaluates to
that expression unless control exits earlier.

## Returns

`return` exits the current function. Without `return`, the final expression
becomes the function result.

```ny
fn clamp(number x, number lo, number hi) number {
   if(x < lo){ return lo }
   if(x > hi){ return hi }
   x
}
```

## Attributes

Function attributes attach compile-time metadata to the following function.
The portable attributes describe effects, ownership, compile-time evaluation,
or a semantic intent that Nytrix can preserve across backends.

```ny
@pure
@effects(none|io|alloc|ffi|thread|all)
@async_effects
@thread
@naked
@consteval
@constant_time
@optimize(0|1|2|3)
fn work(){ 0 }
```

`@pure` is shorthand for `@effects(none)`. The compiler checks declared effect
contracts. It rejects inferred `io`, `alloc`, `ffi`, or `thread` effects
outside the declared mask.

`@async_effects` marks eligible `io`-effect functions for the stackless async
lowering path after their effect contract passes.

`@thread` affects execution: a statement-position call detaches, while a
value-position call joins and returns the worker result. `@naked` is a real
target-specific ABI escape hatch for an assembly body; it is rejected when the
selected backend cannot honor it. Prefer typed standard-library APIs and
semantic contracts for portable application code.

`@optimize(level)` selects NYIR optimization level `0` through `3` for one
function. It is useful when a measured hot function needs a different tradeoff
from the program default. Level `0` also asks the LLVM adapter to preserve that
function without LLVM optimization; higher LLVM per-function pipelines are not
claimed until they are implemented.

## Function policy reference

Use the smallest attribute that states the property the compiler needs. These
attributes are metadata with an implemented compiler effect, not comments.

| Goal | Attribute | Compiler contract |
| --- | --- | --- |
| Declare allowed effects | `@effects(...)`, `@pure` | The effect checker validates calls against the declared set. |
| Describe ownership | `@returns_owned`, `@borrows(x)`, `@consumes(x)`, `@mutates(x)` | Ownership and alias analysis receive an explicit contract. |
| Evaluate during compilation | `@consteval` | Requires a compile-time-safe function contract. |
| Request async lowering | `@async_effects` | Applies only after the declared effect contract passes. |
| Run work in a worker | `@thread` | Statement calls detach; value calls join. |
| Choose NYIR optimization | `@optimize(0|1|2|3)` | Overrides the program NYIR level for this function. |
| Control call shape | `@inline`, `@noinline`, `@tailcall` | Requests implemented call/inlining behavior; conflicting requests diagnose. |
| Communicate execution frequency | `@hot`, `@cold` | Feeds the compiler's function-priority metadata. |
| Prefer call-site expansion | `@flatten` | Requests supported flattening where function shape permits it. |
| Mark constant-time intent | `@constant_time` | Preserves the intent and applies the supported target policy. |
| Request an accelerator target | `@accel`, `@accel(spirv)` | Uses the validated accelerator-lowering path or reports an unsupported target. |
| Supply target-specific code | `@naked`, `asm(...)`, `intrinsic(...)` | Requires a backend that explicitly supports the requested form. |

Attributes do not replace algorithm choice. Measure a workload first, use
`@optimize` or call-shape controls only where the measurement identifies that
function, and keep the portable source behavior correct without them.

## Callable inference

Function values and lambdas keep inferred parameter and return shapes when the
call site provides enough information:

```ny
fn compose(f, g, x){ f(g(x)) }

def out = compose(fn(x){ x + 1 }, fn(x){ x * 2 }, 20)
assert(out == 41, "compose")
```

Typed function expressions can declare parameter and return types inline:

```ny
def shout = fn(str x) str { x + "!" }
```

## Ownership contracts

Ownership attributes document and enforce how arguments and returns move
through a function under borrow checking.

```ny
@borrows(x)
@returns_borrow(x)
fn peek(x){ x }

@returns_owned
@consumes(x)
fn adopt(x){ x }

@consumes(x)
@releases(x)
fn close_owned(x) int {
   __drop_owned(x)
   0
}
```

`ny --borrow-check`, `ny --strict`, and `ny --borrow-check --ownership-strict`
check these attributes. Without ownership checking, the compiler parses them
and warns that it will not enforce them. In strict mode, owned tracked returns
need `@returns_owned`; local-owner borrows cannot escape unless they borrow a
declared parameter; live borrows block moves, releases, and mutations.

## Docstrings

A string literal at the start of a function body is the function docstring. It
is documentation metadata; it is not ordinary executable work.

```ny
fn normalize_port(int raw) int {
   "Return a TCP port after a boundary check."
   assert(raw >= 0 && raw <= 65535, "port range")
   raw
}
```

## Related

- [types.md](types.md) for typed parameters and return types.
- [control-flow.md](control-flow.md) for early exits and cleanup forms.
- [programs.md](../learn/programs.md) for script/module file shape.
