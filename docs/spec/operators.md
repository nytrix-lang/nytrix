# Operators

Operators cover arithmetic, comparison, logic, bitwise work, ternary selection,
coalescing, indexing, calls, and member access.

## Arithmetic

```ny
a + b
a - b
a * b
a / b
a % b
a ^ b
```

Arithmetic operators work on numeric values that the active type and runtime
path accept. The value type and compiler mode choose overflow, widening, and
native-code behavior.

`^` is exponentiation. It is right-associative, so `2^3^2` parses as
`2^(3^2)`.

Nytrix has no `+%` or `*%` operators. Use `%` for remainder. Put overflow
handling at the value/type boundary.

## Assignment

```ny
name = expr
name += expr
name -= expr
name *= expr
name /= expr
name %= expr
++name
--name
```

Plain assignment writes a mutable binding or settable target. Compound
assignment reads the current value, applies the matching operator, and writes
the result back through the same assignment path. The operator keeps its type,
overflow, and native-boundary rules.

Nytrix has no bitwise or shift compound assignment forms such as `&=` or
`<<=`. Write the assignment out.

`++name` and `--name` are prefix increment/decrement statement forms for
mutable numeric targets. Use `name += 1` or `name -= 1` when that is clearer.

## Comparison

```ny
a == b
a != b
a < b
a <= b
a > b
a >= b
```

Value kind defines equality. Ordering comparisons work on numeric values and
text-like values that the runtime/API exposes as comparable.

## Logic

```ny
cond && other
cond || other
!cond
```

Logical operators evaluate truthiness. Use explicit comparisons when a native
or numeric boundary needs an exact integer value.

## Bitwise and shifts

```ny
x & mask
x | mask
x ^^ mask
~x
x << bits
x >> bits
```

Bitwise operators work on integers. Use typed integer values when width, sign,
or native ABI result matters.

`^^` is bitwise XOR. Use `bxor(a, b)` when a named helper is clearer.

Unary `&value` is not bitwise; it is ownership borrow syntax and is equivalent
to `borrow(value)`. See [runtime.md](runtime.md) for ownership checks.

## Ternary

```ny
cond ? when_true : when_false
```

The ternary form chooses between two expressions. Use `if` for branches that
need multiple statements or cleanup.

## Coalescing and pipeline

```ny
value ?? fallback
value |> fn_call
value |> [index]
value |> .member
```

`??` selects a fallback when the language coalescing rule treats the left side
as absent. `|>` pipes a value into the next expression form that the parser and
compiler support.

## Optional chaining

```ny
value?.member
value?.member ?? fallback
```

Optional member access returns `nil` for a `nil` receiver. Otherwise, it
performs the normal member lookup.

## Calls, indexing, and members

```ny
fn_name(arg)
value[index]
value.member
module.helper(value)
```

Calls evaluate arguments and invoke a callable value or named function.
Indexing uses indexable values. Modules define member and receiver forms;
`value.member` exists only when a module documents that receiver shape.

## Custom operators

Declare custom operators on a type with an `impl` block:

```ny
impl Meter {
   operator + self: self = add
   operator ^ int: self = pow
   operator ^^ self: self = xor
   operator == self: bool = same
}
```

The operator body is a named function. The declaration connects the operator
token to that function for the owner type.

## Precedence

Parentheses define grouping:

```ny
(a + b) * c
(flags & mask) != 0
cond ? a : (b ?? fallback)
```

The parser defines precedence. Use parentheses anywhere you need grouping.

## Related

- [syntax.md](syntax.md) for source spelling.
- [values.md](values.md) for equality and representation.
- [types.md](types.md) for numeric and native type constraints.
- [control-flow.md](control-flow.md) for `if`, loops, `case`, and `match`.
