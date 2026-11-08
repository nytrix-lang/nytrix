<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":55,"summary":"Bind leading function arguments, reuse the resulting closure, and adapt binary callables."} -->
# Currying and partial application

Prerequisites: function declarations, immutable bindings, and the
[closure rules](../spec/functions.md#parameters-and-calls).

A partial application supplies some arguments now and returns a callable for
those still missing. Nytrix keeps its ordinary type-first parameter syntax;
there is no new keyword, placeholder operator, or arrow return syntax.

## Bind leading arguments

This is a complete program:

```ny
use core

fn add(int a, int b) int { a + b }

def add5 = add(5)
assert(add5(3) == 8)
assert(add5(7) == 12)
assert(add(5)(3) == 8)
assert(add(5, 3) == 8)
```

`add5` captures the supplied value. Creating it does not run `add`'s body.
Invoking it supplies the remaining argument; the closure is reusable when its
body and captured resources permit reuse. Capturing a collection does not
clone its contents.

## Supply arguments in groups

A residual callable can itself be partially applied. This complete program
uses three equivalent groupings:

```ny
use core

fn sum3(int a, int b, int c) int { a + b + c }

assert(sum3(1)(2)(3) == 6)
assert(sum3(1)(2, 3) == 6)
assert(sum3(1, 2)(3) == 6)
```

Full applications retain direct calls. An immediately saturated named partial
application can also be combined into a direct call; it need not allocate the
intermediate closure that an escaping partial requires. Do not assume that
escaping closures have zero allocation cost.

## Evaluation and defaults

The callable and supplied arguments are evaluated when the partial is made,
not each time it is invoked. Arguments retain left-to-right evaluation order.
The original function body runs only after its required arguments are supplied.

Defaults retain precedence: `add_default(5)` below calls the function with its
default; it does not return a partial. This is a complete program:

```ny
use core

fn add_default(int a, int b=3) int { a + b }
assert(add_default(5) == 8)
```

When a default sits before a required parameter, it cannot be skipped
positionally. The default is evaluated in the residual body and the required
parameter stays in the residual signature:

```ny
use core

fn label(int a, int b = a * 2, int c) int { a + b + c }
def g = label(2)
assert(g(5) == 11)
assert(g(7) == 13)
```

Residual signatures keep dependent indices. Binding `pick(3)` leaves a callable
whose index argument is still `Fin<3>`, so the residual call rejects `3` even
though the original call no longer appears in the source.

A zero-parameter function still uses `f()` normally. Automatic partial
application is not a foreign-function or variadic ABI transformation. Variadic
calls require their fixed parameters and are not curried. A foreign declaration
has a fixed ABI; an incomplete foreign call is rejected rather than converted
into a closure.

Partials over named callables and local aliases retain their call signature and
effect row. A partial also converts to `fnptr` and remains callable.

## Explicit binary adapters

`core.functional` supplies adapters when a higher-order API specifically needs
one binary call or two unary calls. This complete program uses both:

```ny
use core
use core.functional as functional

def add = fn(a, b) { a + b }
def curried = functional.curry(add)
assert(curried(20)(22) == 42)
def binary = functional.uncurry(curried)
assert(binary(19, 23) == 42)
```

`curry` and `uncurry` are binary adapters, not arbitrary-arity reflection APIs.
Their `fnptr` boundary erases the concrete signature; prefer automatic partial
application when preserving an inferred signature matters.

## Next

- [Functions](../spec/functions.md) defines calls, defaults, and closure rules.
- [Ownership](ownership.md) explains resource capture and lifetime constraints.
- [Effects](effect.md) distinguishes construction from deferred execution.
