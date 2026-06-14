# Control flow

Control flow covers conditionals, loops, dispatch, cleanup, and error-control
forms.

## Conditionals

```ny
if(cond){ a } else { b }
if(cond){ a } elif(other){ b } else { c }
if cond { a } elif other { b } else { c }
if(def x = value x > 0){ a } else { b }
def value = if(cond){ a } else { b }
```

The parser accepts parenthesized conditions and whitespace-separated
conditions.

Use `if` as an expression when its branches produce values. The compiler merges
branch types against the surrounding context. In binding or expression
position, each branch needs one value-producing statement, such as a final
expression or a nested value-producing `if`.

Expression-position `if` needs an `else` branch:

```ny
def label = if(code == 200){ "ok" } else { "error" }
```

Conditions use truthiness. `nil`, `false`, `0`, empty strings, and empty
containers are false. Non-zero numbers, non-empty strings, non-empty
containers, pointers, handles, and callable values are true. Use an explicit
comparison in code that should stay readable under `--strict-types`.

## Loops

```ny
while(cond){ body }
while(mut i = 0 i < n ++i){ body }
for item in iterable { body }
for item, index in iterable { body }
for(index in iterable){ body }
for(mut i = 0 i < n ++i){ body }
for item in lo..hi { body }
break
continue
```

`while` repeats while the condition stays true. The parser accepts
`while(init cond update)`. Prefer `for(init cond update)` for counter loops.

`for` iterates over iterable values such as lists, ranges, strings, and
standard-library iterable helpers. The comma form binds the current value first
and the zero-based index second. The `lo..hi` range expression is inclusive in
source-level `for` loops.

The parenthesized `for(index in xs){ ... }` form binds the zero-based index.
Use it when the body reads `xs[index]`.

```ny
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

See [patterns.md](patterns.md) for full arm syntax and guards.

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

See [patterns.md](patterns.md) for full arm syntax and guards.

```ny
match value {
   pattern -> expr
   Pattern(field: name) -> expr
   _ -> fallback
}
```

`match` handles dispatch by value shape. Match ADT variants by qualified
constructor name and named payload fields.

## Try and catch

```ny
try { body } catch err { handler }
try { body } catch(_) { handler }
```

`try` catches language/runtime failures. Standard-library APIs may return
structured `Result` values instead.

## Defer

```ny
defer { cleanup }
```

`defer` schedules cleanup for the current scope. Use it around files, handles,
processes, and native resources. The runtime runs multiple defers in
last-in-first-out order and also runs them during panic unwinding.

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

```ny
with Type name = value { body }
```

`with` binds a scoped value and runs the body with API-defined setup and
cleanup. The runtime runs cleanup when the body falls through, returns, or
unwinds through a panic. `with ptr` scopes raw allocations from `malloc`.

## Related

- [patterns.md](patterns.md) for `case` and `match` arm shapes.
- [errors.md](errors.md) for failure behavior.
- [runtime.md](runtime.md) for cleanup and resource boundaries.
