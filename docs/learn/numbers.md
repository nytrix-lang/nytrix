<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":96,"summary":"Choose an integer or floating-point representation and convert explicitly at boundaries."} -->
# Numbers

Choose a numeric type for the range and precision the program needs. Convert at
boundaries rather than relying on mixed-width arithmetic. The type rules are in
[Types](../spec/types.md).

## Integer types

| Type | Use |
| --- | --- |
| `int` | Default signed machine integer (`i64` on the documented native ABI). |
| `i8`, `i16`, `i32`, `i64` | Explicit signed widths. |
| `u8`, `u16`, `u32`, `u64` | Explicit unsigned widths. |
| `bigint` | Exact integers beyond a fixed-width range. |

```ny
use core

def i8 small = i8(7)
def u16 port = u16(8080)
assert_eq(int(small), 7, "explicit integer conversion")
```

Fixed-width conversions can lose out-of-range information. Check the source
range before converting when truncation or wrapping would be incorrect.

For signed 64-bit operations that must report overflow, use
`core.math.checked`. Checked operations return an explicit success/error value;
ordinary arithmetic keeps its own operator semantics. Use `bigint` when the
result itself must exceed the machine integer range.

## Floating point

`f64` is the default floating-point type. Use `f32` when the storage or ABI
contract requires that width. Floating-point arithmetic is approximate; do not
compare independently rounded calculations for exact equality.

```ny
use core

def f64 x = 0.1
assert(x > 0.09 && x < 0.11, "range check for decimal")
```

## Exact and extended values

`core.math.big` provides arbitrary-precision integer helpers;
`core.math.bigrat` provides exact rational arithmetic; and
`core.math.complex` provides complex values. Query the owning module for the
constructors, conversion rules, and available operations.

## Convert explicitly

| From | To | Form |
| --- | --- | --- |
| `int` | `f64` | `f64(value)` |
| `int` | fixed width | `i32(value)`, `u8(value)`, and similar constructors |
| `str` | `int` | `int(text)` |
| `int` | `bigint` | `bigint(value)` after importing `core.math.big` |
| `bigint` | `str` | `bigint_to_str(value)` from `core.math.big` |

Keep ABI types explicit at native boundaries; see
[Memory](../spec/memory.md#function-and-abi-boundaries).
