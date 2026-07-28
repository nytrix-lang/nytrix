# Types

Types describe the values a binding, parameter, return value, or container may
hold. Start with the concrete type you expect; use `?T` only when absence is a
real result, and use `any` only at a deliberately dynamic boundary.

## Everyday forms

| Write | Meaning | Common use |
| --- | --- | --- |
| `int`, `str`, `bool`, `f64` | A concrete value type. | Bindings and function signatures. |
| `list<T>`, `dict<K, V>`, `set<T>` | A typed collection. | Data kept within Nytrix. |
| `?T` | `T` or `nil`. | Optional input or lookup result. |
| `T<A>` | A generic type. | `Option<int>`, `Result<T, E>`. |
| `struct Name` | A Nytrix record value. | Ordinary program data. |
| `enum Name` | A finite set of variants. | State and alternatives. |
| `layout Name` | An ABI-shaped record. | FFI and raw memory only. |
| `*T`, `handle`, `fnptr` | Native-boundary values. | Explicit interop. |
| `any` | A dynamic value. | A checked boundary or compatibility API. |
| `proof<P>` | Compile-time evidence for proposition `P`. | A verified precondition. |

`numeric`, `number`, `seq`, `indexable`, `iterable`, and `collection` are type
groups for APIs that accept a family of values. They are useful constraints,
not replacements for a precise result type.

## Integer range and dynamic boundaries

The intended `int` model is a signed 64-bit two's-complement value in typed
code, with range `-9223372036854775808` through `9223372036854775807`.
The typed/native migration that enforces this throughout the compiler remains
in progress; use `bigint` when a calculation intentionally exceeds the
currently supported small-integer dynamic range.

`any` and heterogeneous containers are dynamic boundaries. Their runtime
representation may box an integer, but that representation does not reduce
the range of a typed `int`. Keep conversion or dynamic dispatch at those
boundaries explicit when an API needs to distinguish a numeric zero from an
absent value.

## Bindings and functions

The order is always `Type name`:

```ny
def int port = 8080
mut str name = "ny"

fn add(int left, int right) int {
   left + right
}
```

`def` prevents rebinding; `mut` permits rebinding. That does not by itself
change whether a referenced value, such as a dictionary, may be mutated. See
[runtime.md](runtime.md) for collection and ownership behavior.

Fixed-width conversions use callable type names and take exactly one value:

```ny
def u64 count = u64(42)
def i32 small = i32(count)
def f64 ratio = f64(small) / 2.0
```

## Nullable values

`?T` must be refined before it is used as `T`:

```ny
def ?str maybe_name = nil

if(maybe_name != nil){
   def str name = maybe_name
   print(name)
}
```

Nil checks refine the guarded branch. The same applies to an `else` after an
early nil return and to compatible `&&`/`||` guards. Prefer this explicit flow
over inventing sentinel values when absence matters.

## Structs, enums, and generics

Use a `struct` for named fields that stay in Nytrix:

```ny
struct Box {
   int value
}

fn read(Box box) int {
   box.value
}
```

Use an `enum` when a value has alternatives. Payloads are positional and
`match` binds them in the selected arm:

```ny
enum Shape {
   Circle(int radius),
   Rect(int width, int height),
   Empty
}

fn area(Shape shape) int {
   match shape {
      Shape.Circle(radius) -> radius * radius
      Shape.Rect(width, height) -> width * height
      Shape.Empty -> 0
   }
}
```

Enums can also carry type parameters:

```ny
enum Option<T> {
   Some(T value),
   None
}

def Option<int> answer = Option.Some(41)
```

Typed contexts check generic arguments and payloads. For example,
`Option<int>` cannot accept `Option.Some("text")`.

## Collections and dynamic values

Use typed collections for ordinary code:

```ny
def list<int> ids = [1, 2, 3]
def dict<str, int> scores = {"ny": 1}
```

`any` is still inspectable at runtime. `type_shape`, `is_shape`,
`require_shape`, and `assert_shape` validate values received from a dynamic
source. A shape check is useful at the boundary; a typed binding is clearer
inside the program.

```ny
require_shape(rows, "list<list<int>>")
```

`--strict-types` turns high-risk dynamic fallbacks into errors. Use it for code
that should remain statically explainable; use `--no-strict-types` only for an
intentional compatibility boundary.

## Native boundary types

`layout`, `*T`, `handle`, and `fnptr` are distinct. Use `layout` for a record
whose field order and representation are part of an ABI; use `struct` for an
ordinary Nytrix value. Do not use a handle as a pointer unless the foreign API
documents that conversion.

```ny
layout Pixel {
   u8 r,
   u8 g,
   u8 b,
   u8 a
}
```

The full boundary contract—headers, ownership, strings, packing, and
alignment—is in [native.md](native.md).

## Proof and refinement types

`proof<P>` is erased compile-time evidence that proposition `P` was proved.
It is not a runtime boolean and ordinary values cannot stand in for it.

```ny
fn require_positive(proof<5 > 0> evidence) int {
   5
}

def proof positive = prove(5 > 0, "positive constant")
require_positive(positive)
```

Proposition matching is structural: equivalent equality/order spellings are
normalized, while unrelated propositions are rejected. `prove` accepts only a
condition the compiler can establish; false and unknown conditions fail.

This is refinement-proof support, not full dependent typing. Current indexed
proofs do not substitute parameter-dependent propositions through calls or
survive mutation of their referenced values; unsupported forms are rejected
rather than accepted as evidence. See [comptime.md](comptime.md) for compile-
time assertions and the proof section in that document for construction rules.

## Related

- [runtime.md](runtime.md) for mutability, ownership, and collections.
- [native.md](native.md) for FFI and ABI rules.
- [comptime.md](comptime.md) for compile-time assertions and refinements.
- [errors.md](errors.md) for diagnostics and result refinement.
