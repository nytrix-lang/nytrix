# Operators

Operators cover arithmetic, comparison, logic, bitwise work, ternary selection,
coalescing, indexing, calls, and member access.

## Arithmetic

```text
a + b
a - b
a * b
a / b
a % b
a ^ b
```

Arithmetic operators apply to numeric values accepted by the active type and
runtime path. Exact overflow, widening, and native-code behavior follows the
value type and compiler mode in use.

`^` is exponentiation. It is right-associative, so `2^3^2` parses as
`2^(3^2)`.

`+%` and `*%` are not Nytrix operators. Use `%` for remainder, ordinary
arithmetic for the active integer semantics, or make overflow handling explicit
at the value/type boundary.

## Comparison

```text
a == b
a != b
a < b
a <= b
a > b
a >= b
```

Equality is defined by value kind. Ordering comparisons are for ordered values
such as numeric and comparable text-like values supported by the runtime/API.

## Logic

```text
cond && other
cond || other
!cond
```

Logical operators evaluate truthiness. Use explicit comparisons when a native
or numeric boundary needs an exact integer value.

## Bitwise and shifts

```text
x & mask
x | mask
x ^^ mask
~x
x << bits
x >> bits
```

Bitwise operators are integer-oriented. Use typed integer values when the
width, sign, or native ABI result matters.

`^^` is bitwise XOR. Use `bxor(a, b)` when a named helper is clearer.

Unary `&value` is not bitwise; it is ownership borrow syntax and is equivalent
to `borrow(value)`. See [runtime.md](runtime.md) for ownership checks.

## Ternary

```text
cond ? when_true : when_false
```

The ternary form chooses between two expressions. `if` handles branches that
need multiple statements or cleanup.

## Coalescing and pipeline

```text
value ?? fallback
value |> fn_call
value |> [index]
value |> .member
```

`??` selects a fallback when the left side is absent according to the language
coalescing rule. `|>` pipes a value into the next expression form supported by
the parser and compiler surface.

## Optional chaining

```text
value?.member
value?.member ?? fallback
```

Optional member access returns `nil` when the receiver is `nil`; otherwise it
performs the normal member lookup.

## Calls, indexing, and members

```text
fn_name(arg)
value[index]
value.member
module.helper(value)
```

Calls evaluate arguments and invoke a callable value or named function.
Indexing applies to indexable values. Member and receiver forms are API
surface; `value.member` exists only when the module documents that receiver
shape.

## Custom operators

Custom operators are declared on a type with an `impl` block:

```text
impl Meter {
   operator + self: self = add
   operator ^ int: self = pow
   operator ^^ self: self = xor
   operator == self: bool = same
}
```

The operator body is a named function. The declaration only connects the
operator token to that function for the owner type.

## Precedence

Parentheses define grouping explicitly:

```text
(a + b) * c
(flags & mask) != 0
cond ? a : (b ?? fallback)
```

The parser defines exact precedence. Parentheses are valid anywhere explicit
grouping is required.

## Related

- [syntax.md](syntax.md) for source spelling.
- [values.md](values.md) for equality and representation.
- [types.md](types.md) for numeric and native type constraints.
- [control-flow.md](control-flow.md) for `if`, `case`, and `match`.
