<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":96,"summary":"Choose the right numeric type for a value, then convert exactly when crossing it."} -->
# Numbers

Pick the narrowest numeric type that proves the value's contract, and convert
exactly at the boundary. Implicit conversion rules are written in
[Types: numeric](../spec/types.md#numeric-types).

## Integer choices

| Type | Typical use |
| --- | --- |
| `int` | Default machine word; literals are `int` unless narrowed. |
| `i8`, `i16`, `i32`, `i64` | Signed fixed-width contracts. |
| `u8`, `u16`, `u32`, `u64` | Unsigned fixed-width contracts. |
| `bigint` | Values that exceed fixed width or need exactness. |

Use an explicit fixed-width conversion when a value crosses a fixed-width
boundary:

```ny
use std.core

def i8 v = i8(7)
def u16 limit = u16(1000)
assert_eq(int(v), 7, "narrowed literal")
```

Fixed-width conversions can truncate or wrap out-of-range values; check the
range before converting when that would be incorrect for the program.

## Floating-point choices

`f64` is the default float type. Declare `f32` only for an explicit storage or
target width contract:

```ny
use std.core

def f64 a = 0.1
def f32 b = 0.5
assert(a + b == 0.6, "float arithmetic")
```

## Arbitrary precision

`bigint` holds integers without a fixed width:

```ny
use std.core
use std.math.big

def huge = bigint(2) ** bigint(200)
assert_eq(bigint_to_str(huge), "1606938044258990275541962092341162602522202993782792835301376", "2**200 exactly")
```

Construct from strings for inputs that exceed literal range:

```ny
use std.math.big

def d = bigint_from_str("123456789012345678901234567890")
```

## Rational and complex values

`std.math.bigrat` exports exact rational arithmetic. `std.math.complex`
exports complex values:

```ny
use std.core
use std.math.complex as c

def z = c.complex(3.0, 4.0)
assert_eq(c.real(z), 3.0, "real part")
```

## Convert explicitly

| From | To | Use |
| --- | --- | --- |
| `int` | `f64` | `f64(x)` |
| `int` | fixed-width | `i32(x)`, `u8(x)`,... |
| `str` | `int` | `int("42")` |
| `int` | `bigint` | `bigint(x)` after `use std.math.big` |
| `bigint` | `str` | `bigint_to_str(x)` after `use std.math.big` |

Prefer converting at the boundary over mixed-width arithmetic. When a value
crosses a native boundary, keep the ABI type explicit (see
[Memory](../spec/memory.md#abi)).

## Related

- [Types](../spec/types.md)
- [Values](../spec/values.md)
- [Operators](../spec/operators.md)