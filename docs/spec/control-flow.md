# Control flow

Control flow covers conditionals, loops, dispatch, cleanup, and error-control
forms.

## Conditionals

```text
if(cond){ a } else { b }
if(cond){ a } elif(other){ b } else { c }
if cond { a } elif other { b } else { c }
if(def x = value x > 0){ a } else { b }
def value = if(cond){ a } else { b }
```

Parenthesized conditions and whitespace-separated conditions are both accepted.

`if` is expression-shaped when its branches produce values. Branch types are
merged according to the type rules for the surrounding context. In binding or
expression position, each branch must currently contain one value-producing
statement, such as a final expression or a nested value-producing `if`.

An `else` branch is required in expression position:

```text
def label = if(code == 200){ "ok" } else { "error" }
```

## Loops

```text
while(cond){ body }
while(mut i = 0 i < n ++i){ body }
for item in iterable { body }
for item, index in iterable { body }
for item in lo..hi { body }
break
continue
```

`while` repeats while the condition is true. `for` iterates over iterable
values such as lists, ranges, strings, and standard-library iterable helpers.
The comma form binds the current value first and the zero-based index second.
The `lo..hi` range expression is inclusive in source-level `for` loops.

```ny
mut seen = []
for ch, i in "test" {
   seen = seen.append(f"{ch}:{i}")
}
assert(seen == ["t:0", "e:1", "s:2", "t:3"], "indexed loop")
```

## Case

`case` is value dispatch.

```text
case value {
   literal -> expr
   a, b, c -> expr
   lo..hi if guard -> expr
   lo..hi -> expr
   _ -> fallback
}
```

`case` handles compact literal, set-of-literals, and range dispatch. Range
arms are inclusive and use the same ordered comparisons as `>=` and `<=`;
integers, floats, and strings are supported.
`case` can be used in binding position when every selected arm produces a
value.

## Match

`match` is pattern dispatch.

```text
match value {
   pattern -> expr
   Pattern(field: name) -> expr
   _ -> fallback
}
```

`match` handles dispatch that depends on value shape rather than only literal
equality. ADT variants can be matched by qualified constructor name and named
payload fields.

## Try and catch

```text
try { body } catch err { handler }
try { body } catch(_) { handler }
```

`try` catches failures represented by the language/runtime path. Standard
library APIs may also return structured `Result` values instead of throwing.

## Defer

```text
defer { cleanup }
```

`defer` schedules cleanup for the current scope. It is used for deterministic
cleanup around files, handles, processes, and native resources.
Multiple defers in one scope run in last-in-first-out order. Defers also run
during panic unwinding.

## Labels and goto

```text
start:
goto start
```

Labels name a position inside the current function. `goto` jumps only to a
label in that function. It can leave inner scopes, running pending `defer`
cleanup on the way out, but it cannot jump into a deeper scope because that
would skip binding initialization. Undefined labels are compile errors.

## With

```text
with Type: name = value { body }
```

`with` binds a scoped value and runs the body with resource-style setup and
cleanup semantics where the API supports them.
Cleanup runs when the body falls through, returns, or unwinds through a panic.
`with ptr` scopes raw allocations from `malloc`.

## Related

- [patterns.md](patterns.md) for `case` and `match` arm shapes.
- [errors.md](errors.md) for failure behavior.
- [runtime.md](runtime.md) for cleanup and resource boundaries.
