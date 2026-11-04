<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":150,"summary":"Branches, loops, pattern cases, exits, and control-flow expressions."} -->
# Control Flow

Control flow selects branches, repeats loops, and runs deferred cleanup when a
scope exits.

For pattern matching and failure handling, see [Patterns](patterns.md) and
[Errors](errors.md).

## Contents

- [Branches and loops](#conditionals)
- [Value and pattern dispatch](#case)
- [Exits and cleanup](#defer)

## Conditionals

> **Note:** These grammar shapes use placeholder names; they are not standalone
> programs.

```ny
if(cond){ a } else { b }
if(cond){ a } elif(other){ b } else { c }
if cond { a } elif other { b } else { c }
if(def x = value x > 0){ a } else { b }
def value = if(cond){ a } else { b }
```

Use the complete examples below when you want a REPL input.

The parser accepts parenthesized conditions and whitespace-separated
conditions.

Use `if` as an expression when its branches produce values. The compiler merges
branch types against the surrounding context. In binding or expression
position, each branch needs one value-producing statement, such as a final
expression or a nested value-producing `if`.

## Rules

1. An expression-position `if` needs an `else` branch, and each selected branch
   must produce a value.
2. Source-level `for item in lo..hi` includes both endpoints. `break` and
   `continue` affect only the innermost enclosing loop.
3. `case` selects by literal or range; `match` selects by value shape and checks
   ADT exhaustiveness.
4. `defer` runs on scope exit in last-in-first-out order, including panic
   unwinding. `with` releases its scoped resource when the body exits.

Expression-position `if` needs an `else` branch:

```ny
def code = 200
def label = if(code == 200){ "ok" } else { "error" }
```

### Truthiness

Conditions use truthiness. `nil`, `false`, `0`, empty strings, and empty
containers are false. Non-zero numbers, non-empty strings, non-empty
containers, pointers, handles, and callable values are true. Use an explicit
comparison in code that should stay readable under `--check=types`.

| Value | Truthiness |
| --- | --- |
| `nil`, `false`, `0`, `0.0` | False |
| `""`, `[]`, `{}`, empty set/tuple/range | False |
| Non-zero numbers, non-empty strings and containers | True |
| Pointers, handles, callable values | True |

## Loops

> **Note:** These lines show loop grammar with placeholder names; use the
> complete examples below for compilable programs.

```ny
while(cond){ body }
while(mut i = 0 i < n ++i){ body }
for item in iterable { body }
for item, index in iterable { body }
for(index in iterable){ body }
for(mut i = 0 i < n ++i){ body }
for item in lo..hi { body }
while true {
   if done { break }
   continue
}
```

`while` repeats while the condition stays true. The parser accepts
`while(init cond update)`. Prefer `for(init cond update)` for counter loops.

The `for(init cond update)` loop keeps an explicit counter binding:

```ny
use core

mut total = 0
for(mut i = 0 i < 5 ++i){ total += i }
assert(total == 10, "counter loop")
```

`for` iterates over iterable values such as lists, ranges, strings, and
standard-library iterable helpers. The comma form binds the current value first
and the zero-based index second. The `lo..hi` range expression is inclusive in
source-level `for` loops.

Parenthesized counter loops use spaces between the initializer, condition, and
update. Semicolons start comments in Nytrix:

```ny
use core

mut total = 0
for (mut i = 0 i < 5 i += 1) { total += i }
assert(total == 10, "parenthesized counter loop")
```

The parenthesized `for(index in xs){ ... }` form binds the zero-based index.
Use it when the body reads `xs[index]`.

```ny
use core

mut seen = []
for ch, i in "test" {
   seen = seen.append(f"{ch}:{i}")
}
assert(seen == ["t:0", "e:1", "s:2", "t:3"], "indexed loop")
```

## Loop control: break and continue

Use `break` to immediately exit the innermost enclosing loop.
Use `continue` to skip the remainder of the innermost loop's body and proceed to the next iteration.

```ny
mut sum = 0
for i in 1..10 {
   if i % 2 == 0 {
      continue
   }
   sum += i
   if sum > 20 {
      break
   }
}
```

## Case

`case` is value dispatch.

See [Patterns](patterns.md#guards-and-clarity) for full arm syntax and guards.

```ny
case value {
   literal -> expr
   a, b, c -> expr
   lo..hi if guard -> expr
   lo..hi -> expr
   _ -> fallback
}
```

`case` handles literal, set-of-literals, and range dispatch. Range arms include
both endpoints and use the same ordered comparisons as `>=` and `<=`. `case`
supports integers, floats, and strings.

Use `case` in binding position when each selected arm produces a value.

## Match

`match` is pattern dispatch.

See [Patterns](patterns.md#match-patterns) for full arm syntax and guards.

```ny
match value {
   pattern -> expr
   Pattern(value) -> expr
   _ -> fallback
}
```

`match` handles dispatch by value shape. Match ADT variants by qualified
constructor name and positional payload bindings.

## Try and catch

```ny
try { body } catch err { handler }
try { body } catch(_) { handler }
```

`try` catches language/runtime failures. Standard-library APIs may return
structured `Result` values instead.

Panic unwinding stores and restores control state through a platform
`jmpbuf`; `__jmpbuf_size()` and `__jmpbuf_align()` report its measured byte
size and alignment, and `__get_backtrace(n)` returns up to `n` captured call
frames, newest first, each a `[file, line, function]` list (`0` or an
over-large count returns every frame). The fixtures
`etc/tests/lang/control-flow/control.nshape` and
`etc/tests/lang/execution/panic.nshape` verify the positive sizes and the
list shape.

## Defer

```ny
defer { cleanup }
```

`defer` schedules cleanup for the current scope. Use it around files, handles,
processes, and native resources. The runtime runs multiple defers in
last-in-first-out order and also runs them during panic unwinding.

Deferred bodies snapshot locals they only read at registration. When a defer
assigns a captured mutable local, it updates that local's enclosing home before
the scope continues. Defers still run in last-in-first-out order, including
during panic unwinding.

```ny
use core

fn deferred_order() str {
   mut order = ""
   if true {
      defer { order = order + "a" }
      defer { order = order + "b" }
   }
   order
}

assert(deferred_order() == "ba", "defers run LIFO at scope exit")
```

This example is the gate fixture `etc/tests/lang/control-flow/control.nshape`.

A `defer` runs when its enclosing scope exits, whether by falling through, an
early `return`, a `goto` that leaves the scope, or panic unwinding. Defers do
not run at program end for scopes that never exited normally.

The runtime defer queue has diagnostics built for the runtime and gate
fixtures: `__push_defer(fn, env)` appends a callback to the process defer
stack, `__pop_run_defer()` pops and runs one entry, and
`__run_defers_to(depth)` runs entries down to `depth` and clears them.
Empty stacks and null callbacks report `nil`. Use `defer` for scope work;
these helpers are runtime diagnostics, not a scheduling API. The
fixture `etc/tests/lang/control-flow/control.nshape` asserts the
empty-stack and null-callback edge cases.

## Labels and goto

```ny
start:
if(done){ goto end }
goto start
end:
```

Labels name a position inside the current function. `goto` jumps only to a
label in that function. It can leave inner scopes; the runtime runs pending
`defer` cleanup on the way out. It cannot jump into a deeper scope because
that would skip binding initialization. The compiler rejects undefined labels.

Use `goto` for small local state machines and cleanup exits where structured
loops make the control path harder to see. Prefer `while`, `for`, `break`, and
`continue` for ordinary iteration.

## With

> **Note:** `Type`, `name`, `value`, and `body` are placeholders for the
> resource grammar.

```ny
with Type name = value { body }
```

`with` is a type-first resource binding: `with Type name = value`. Do not put a
colon between the type and name. Cleanup runs when the body falls through,
returns, or unwinds through a panic. `with ptr` scopes raw allocations from
`malloc`.

## Related

- [Patterns](patterns.md#guards-and-clarity) for `case` and `match` arm shapes.
- [Errors](errors.md) for failure behavior.
- [Runtime](runtime.md) for cleanup and resource boundaries.
