<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":160,"summary":"Operator precedence, comparisons, arithmetic, assignment, and overload rules."} -->
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

Nytrix has no `+%` or `*%` wrapping operators. Unsigned arithmetic uses
the ordinary operators and wraps on overflow.

## Precedence

Higher rows bind more tightly.

| Level | Operators | Associativity |
| --- | --- | --- |
| 8 | `^` | right |
| 7 | `&`, `|`, bitwise xor, `<<`, `>>` | left |
| 6 | `*`, `/`, `%` | left |
| 5 | `+`, `-` | left |
| 4 | `<`, `<=`, `>`, `>=`, `lo..hi` range | left |
| 3 | `==`, `!=` | left |
| 2 | `&&`, `??` | left |
| 1 | `||`, `a |> f` | left |
| - | `cond ? a : b`, assignment `=` and compound forms | right |

Consequences worth noting:

- `2^3^2` is `2^(3^2)` because exponentiation is right-associative.
- Bitwise and shift operators bind tighter than `+` and `-`, so
 `a & b + c` is `a & (b + c)`.
- The range operator `..` binds with the ordering comparisons, so
 `1..n == 1..n` compares two range values.
- `??` binds with `&&`, looser than `==`: `x == y ?? z` parses as
 `x == (y ?? z)`.
- Ternary and assignment are loosest. `a ? b : c = d` groups as
 `a ? b : (c = d)`.

Parentheses override any of these rules:

```ny
(a + b) * c
(flags & mask) != 0
cond ? a : (b ?? fallback)
```

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

The operator body is a named function. The colon separates the right operand
type from the return type; parameters elsewhere use the normal `Type name`
spelling. Operator overloading never changes the fixed precedence of the
operator spelling; it only supplies a definition for a type.

## Related

- [Syntax](syntax.md) for source spelling.
- [Values](values.md) for equality and representation.
- [Types](types.md) for numeric and native type constraints.
- [Control Flow](control-flow.md) for `if`, loops, `case`, and `match`.