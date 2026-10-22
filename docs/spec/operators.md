<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":160,"summary":"Operator precedence, comparisons, arithmetic, assignment, and overload rules."} -->
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
a ** b
a ^^ b
a ^ b
```

Arithmetic operators work on numeric values that the active type and runtime
path accept. The value type and compiler mode choose overflow, widening, and
native-code behavior.

`**` and `^^` are exponentiation spellings and are right-associative, so
`2 ** 3 ** 2` parses as `2 ** (3 ** 2)`. `^^` is an alias for `**`; `^` is
bitwise XOR.
Since legacy `^` power and modern `^` XOR are lexically indistinguishable,
migration requires an explicit dialect: `ny fmt --migrate-operators
--assume-legacy-operators path` converts legacy `^` power and `^^` XOR to
`**` and `^`. Without `--assume-legacy-operators`, the migration command
leaves ambiguous expressions unchanged. It preserves strings and comments;
for `.nshape` fixtures it rewrites only embedded `source ny` blocks.

Nytrix has no `+%` or `*%` wrapping operators. Unsigned arithmetic uses
the ordinary operators and wraps on overflow.
For signed 64-bit operations that need explicit overflow results or saturation,
use the checked helpers in [`core.math.checked`](../learn/numbers.md#integer-types).

## Bitwise and shift operations

`^` is bitwise XOR.

`>>` is an arithmetic right shift: it preserves the sign of negative integers.
A negative shift count leaves the value unchanged. For native signed 64-bit
values, a count of 64 or more yields the sign-fill value (`0` or `-1`); BigInt
values continue to shift at arbitrary precision. The `bshr` helper is a logical
right shift and fills with zero bits.

## Precedence

Higher rows bind more tightly.

| Level | Operators | Associativity |
| --- | --- | --- |
| 8 | `**`, `^^` | right |
| 7 | `&`, `|`, `^`, `<<`, `>>` | left |
| 6 | `*`, `/`, `%` | left |
| 5 | `+`, `-` | left |
| 4 | `<`, `<=`, `>`, `>=`, `lo..hi` range | left |
| 3 | `==`, `!=` | left |
| 2 | `&&`, `??` | left |
| 1 | `||`, `a |> f` | left |
| - | `cond ? a : b`, assignment `=` and compound forms | right |

Consequences worth noting:

- Bitwise and shift operators bind tighter than `+` and `-`, so
  `a & b + c` is `(a & b) + c`.
- The range operator `..` binds with the ordering comparisons, so
 `1..n == 1..n` compares two range values.
- `??` binds with `&&`, looser than `==`: `x == y ?? z` parses as
  `(x == y) ?? z`.
- Ternary and assignment are loosest. `a ? b : c = d` groups as
 `a ? b : (c = d)`.

Parentheses override any of these rules:

```text
(a + b) * c
(flags & mask) != 0
cond ? a : (b ?? fallback)
```

## Custom operators

Operator overloading binds a fixed spelling (such as `+`) to a **target function**
for a `(left type, right type)` pair. Precedence never changes.

Declare custom operators on a type with an `impl` block (left operand type is the
impl owner). Full grammar, top-level declarations, and trait-adjacent rules are
in [Traits and impl](traits.md).

```text
impl Meter {
   operator + self: self = add
   operator ** int: self = pow
   operator ^ self: self = xor
   operator == self: bool = same
}
```

The operator body is a named function. The colon separates the right operand
type from the return type; parameters elsewhere use the normal `Type name`
spelling.

## Related

- [Syntax](syntax.md) for source spelling.
- [Values](values.md) for equality and representation.
- [Types](types.md) for numeric and native type constraints.
- [Control Flow](control-flow.md) for `if`, loops, `case`, and `match`.
