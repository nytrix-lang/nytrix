# Functions and blocks

Functions cover bindings, parameters, lambdas, return behavior, docstrings, and
block values.

## Bindings

```ny
def name = expr
mut name = expr
def Type: name = expr
mut Type: name = expr
def a, b = expr
mut a, b
del name
```

`def` creates an immutable binding. `mut` creates a mutable binding. Typed
binding order is `Type: name`.

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

`lambda(...) { ... }` is accepted as a legacy spelling. The parser accepts the
same parameter-list and return-type syntax as `fn(...) { ... }`; `fn` is the
canonical spelling in new code.

Return types follow the parameter list without `:`. `->` is reserved for
pattern/case arms, not function signatures.

## Parameters

```ny
name
Type name
name = default
Type name = default
...rest
```

Parameter types are part of the callable surface. Defaults are evaluated by the
call path according to the function definition.

## Blocks

Blocks use braces:

```ny
{ statement* expr? }
```

An empty block evaluates to `nil`. A block with a final expression evaluates to
that expression unless control exits earlier.

## Returns

`return` exits the current function. Without explicit `return`, the final
expression is the function result.

```ny
fn clamp(number x, number lo, number hi) number {
   if(x < lo){ return lo }
   if(x > hi){ return hi }
   x
}
```

## Attributes

Function attributes attach compile-time metadata to the following function.
The currently supported surface includes codegen hints, effects, async
lowering, and ownership contracts.

```ny
@pure
@effects(none|io|alloc|ffi|thread|all)
@async_effects
@jit
@thread
@naked
@consteval
@constant_time
@llvm(noinline)
@llvm("frame-pointer", "all")
@readnone
@readonly
@writeonly
@argmemonly
@nounwind
@mustprogress
@willreturn
@hot
@cold
@flatten
fn work(){ 0 }
```

`@pure` is shorthand for `@effects(none)`. Declared effect contracts are
checked by the compiler: inferred `io`, `alloc`, `ffi`, or `thread` effects
outside the declared mask are errors.

`@async_effects` marks eligible `io`-effect functions for the stackless async
lowering path after their effect contract passes.

`@jit`, `@thread`, `@naked`, `@llvm(...)`, and the LLVM-style memory/progress
attributes affect lowering and native code metadata. A `@thread` call in
statement position detaches; a value-position call joins and returns the
worker result.

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
through a function when borrow checking is enabled.

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

These attributes are checked by `ny --borrow-check`, `ny --strict`, and
`ny --borrow-check --ownership-strict`. Without ownership checking, the
compiler parses them and warns that they are not enforced. In strict mode,
returning an owned
tracked slot requires `@returns_owned`, returning a borrow of a local owner is
rejected unless it is a declared parameter borrow, and moves/releases/mutations
are rejected while live borrows exist.

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
