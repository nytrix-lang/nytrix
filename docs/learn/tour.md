<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":90,"summary":"A ten-minute pass over the language: values, functions, running, and checking."} -->
# Tour

Nytrix is a native systems language with a testable, teachable core. This tour
takes about ten minutes and touches every other page in the guide.

## A first program

```ny
use std.core

fn greet(str name) str { "hello, " + name }

assert(greet("ny") == "hello, ny", "greeting")
```

```bash
ny tour.ny
```

`use` imports a module, `fn` declares a function, `assert` proves a result.
The program is the assertion: correctness is part of the source.

## Values

Values are either tagged objects (lists, dicts, strings) or immediate machine
words. `int` is a tagged immediate; literals are `int` unless narrowed:

```ny
use std.core

def total = 2 + 3
assert(total == 5, "arithmetic")
assert(total * 2 == 10, "precedence")
```

See [Values](../spec/values.md) for equality, literals, and mutation rules.

## Functions

Functions are first-class. They evaluate arguments left to right and return a
typed value:

```ny
use std.core

fn scale(int x, int f) int { x * f }
assert(scale(3, 4) == 12, "call")

def double = fn(int x) int { x * 2 }
assert(double(7) == 14, "closure")
```

Attributes attach ownership and effect contracts to a function
([Functions](../spec/functions.md)).

## Check as you go

The compiler enforces more than type shape:

```bash
ny --strict-types tour.ny
ny --borrow-check --ownership-strict tour.ny
ny fmt --check tour.ny
ny test --with-stdlib .
```

Strict checks reject dynamic-fallback cliffs, ownership mistakes, and
formatting drift. Run them in the target that requires the contract
([Tooling](tooling.md)).

## Compile a native binary

```bash
ny -O2 -o build/tour tour.ny
build/tour
```

`-o` emits a native executable. Flags come before the file argument. See
[Programs](programs.md) and [Pipeline](../spec/pipeline.md) for the full path
from source to artifact.

## Where to go next

| You want to | Read |
| --- | --- |
| Run code and see numbers | [Start](start.md) |
| Learn value and type rules exactly | [Values](../spec/values.md), [Types](../spec/types.md) |
| Structure larger programs | [Programs](programs.md) |
| Make work happen in parallel | [Concurrency](concurrency.md) |
| Move resources safely | [Ownership](ownership.md) |
| Build a real feature | [Cookbook](cookbook.md) |
| Measure and improve | [Performance](performance.md) |

## Related

- [Start](start.md)
- [Programs](programs.md)
- [Testing](testing.md)