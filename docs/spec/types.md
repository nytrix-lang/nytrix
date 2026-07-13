# Types

Types cover named values, nullable values, native boundary forms, and
compile-time type checking.

## Type expressions

| Form | Meaning |
| --- | --- |
| `T` | Named type. |
| `T<A>` | Generic type with one type argument. |
| `T<A, B>` | Generic type with multiple type arguments. |
| `?T` | Nullable type. |
| `*T` | Pointer type. |
| `fnptr` | Callable function/lambda pointer. |
| `seq` / `sequence` | List, tuple, string, bytes, or range. |
| `numeric` | Integer, float, bigint, or compatible numeric value. |
| `indexable` | Value accepted by static indexing. |
| `iterable` | Value accepted by static iteration. |
| `allocator` | Pointer/handle allocator capability. |
| `handle` | Opaque native handle scalar. |
| `complex`, `c64`, `c128` | Complex numeric values and ABI-facing forms. |
| `any` | Dynamic value that remains shape-checkable at runtime. |
| `proof` | Erased carrier for a compile-time proven fact (dependent/refinement use). |
| `number` | Language group for integer, float, bigint, and compatible numeric values. |
| `collection` | Language group for list, dict, set, tuple, bytes, and range-like containers. |

Generic type expressions are part of the compiler surface. Common forms are
`list<int>`, `list<list<f64>>`, `dict<str, int>`, `set<str>`,
`Result<T, E>`, and user ADTs such as `Option<int>`.

## Typed bindings

```ny
def int port = 8080
mut str name = "ny"
fn add(int a, int b) int { a + b }
```

Typed binding order is `Type name`.

## Numeric casts

Fixed-width scalar casts use callable type names:

```ny
def u64 n = u64(42)
def i32 small = i32(n)
def f64 ratio = f64(small) / 2.0
```

Available cast names are `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`,
`i64`, `f32`, and `f64`. Casts take one value.

## Nullable values

`?T` allows `nil` or a `T` value. Code that consumes nullable values must refine
or handle `nil` before using the payload as non-null.

```ny
def ?str maybe_name = nil
if(maybe_name != nil){
   def str name = maybe_name
}
```

Nil checks narrow nullable values in the guarded branch. Reversed comparisons
such as `nil != value`, `else` branches after a `nil` return, and logical
`&&`/`||` guards participate in the same narrowing.

## Algebraic data types

Simple enums bind integer constants. Values start at `0` and increase by one
unless a variant sets an explicit value.

```ny
enum Color {
   Red,
   Green,
   Blue
}

enum Status {
   Ok = 0,
   Error = 1,
   Pending = 2
}

assert(Color.Red == 0, "enum value")
```

`enum` declares an algebraic data type. Variants can be payload-less or carry
ordered payload fields.

```ny
enum Shape {
   Circle(int radius),
   Rect(int width, int height),
   Empty
}

def c = Shape.Circle(4)
def also_c = Circle(2)
```

Payload constructors use positional values. Pattern matching binds payload
values positionally in each arm:

```ny
fn area(Shape s) int {
   match s {
      Shape.Circle(r) -> r * r
      Shape.Rect(w, h) -> w * h
      Shape.Empty -> 0
   }
}
```

Generic ADTs declare type parameters and work in typed bindings:

```ny
enum Option<T> {
   Some(T value),
   None
}

def Option<int> value = Option.Some(41)
```

The compiler checks generic ADT payloads in typed contexts; for example,
`Option<int>` rejects `Option.Some("text")`.

## Native types

Pointers, handles, layouts, and function pointers represent native boundary
values. They are not interchangeable.

| Type | Use |
| --- | --- |
| `*T` | Addressable pointer to `T`. |
| `handle` | Opaque native scalar resource. |
| `fnptr` | Callable native/function pointer boundary. |
| `layout Name` | ABI-shaped record. |

## Structs and layouts

Use `struct` for Nytrix values and pass them as tagged values inside Nytrix
code.

```ny
struct Box {
   int value
}

fn read(Box box) int {
   box.value
}
```

Use `layout` or `layout record` when the value needs a native ABI shape for FFI
or raw memory work. Layout fields belong to the native boundary; ordinary
structs need an explicit layout boundary before they become ABI compatible.

Layout forms include packing, alignment, derived helpers, and guards:

```ny
layout Packed pack(1){
   u8 tag,
   i32 value
}

layout record Row derive(default, eq, hash, debug_str) pack(4){
   i32 id
}

layout shape Header derive(load, store, zero) pack(8){
   str sender
}

layout guard Header h = value else {
   return err("bad header")
}
```

`layout guard` checks boundary data and narrows the guarded binding to the
layout pointer type. Derived layout shapes emit `LayoutName_from(value)` and
`*_load_*` helpers when requested.

Structs, layouts, functions, and local declarations use `Type name`.

## Impl self and operators

Inside an `impl`, `self` names the owner type for receivers, parameters, return
types, and operators:

```ny
impl ShapeBox {
   fn value(self b) list { b.get("value", []) }
   fn concat(self a, self b) self { ShapeBox({"value": a.value + b.value}) }
   operator + self: self = concat
}

impl int, f32 {
   fn twice(self x) self { x + x }
}
```

Pointer receivers can use `*self`, and nullable receivers can use `?self`.

## Runtime shape reflection

`type(value)` returns the top-level runtime tag. `type_shape(value)` returns a
recursive shape string such as `list<list<int>>` or `dict<str, int|bool>`.

```ny
type_shape([[1], [2]])
is_shape(rows, "list<list<int>>")
require_shape(rows, "list<list<int>>")
assert_shape(rows, "list<list<int>>")
```

Shape specs can be a string or a list of accepted strings. `require_shape` and
`assert_shape` return the checked value or panic with expected and actual
shapes.

Shape strings validate and debug runtime values. Prefer typed bindings when
the compiler should enforce the shape.

## Type groups

The runtime type helpers can define aliases and groups:

```ny
use std.core.syntax.type as ty

ty.define_type_alias("amount", "number")
ty.define_type_group("math_input", ["amount"])
ty.extend_type_group("math_input", ["seq"])
```

`is_type`, `require_type`, `assert_type`, and typed function annotations such
as `number x` accept groups.

## Compile-time checks

Nytrix runs compile-time type checks by default for typed bindings, function
arguments and returns, ADT payloads, generics, layouts, and native boundaries.
That catches type mistakes without ownership ceremony.
When an expression loses static evidence and falls back to dynamic `any`, the
checker emits capped source warnings for the high-risk cases.

`--strict-types` turns those dynamic-cliff warnings into rejection. In that
mode, the checker rejects places where the compiler would otherwise have to
fall back to unchecked dynamic behavior:

- accidental heterogeneous dict literals
- unknown dynamic arithmetic
- unknown member/index access
- unrefined `Result` payload use
- native values used as the wrong native kind

Use it when a file should stay fully statically explainable:

```bash
ny --strict-types file.ny
```

Use `--no-strict-types` only when an outer tool or environment enabled strict
dynamic checks and a compatibility probe intentionally relies on them:

```bash
ny --no-strict-types old_probe.ny
```

## Proof types

`proof` is a builtin, payload-free witness type (no special syntax). Construct
one with `prove(condition[, message])`. The compiler accepts the construction
only when `condition` reduces to true during compilation; false and unknown
conditions are errors. Ordinary integers and booleans never implicitly convert
to `proof`.

```ny
fn sum_up_to(int n, proof p) int {
   if n <= 1 { 1 } else { n + sum_up_to(n - 1, p ) }
}

def proof arithmetic = prove((2 + 2) == 4, "arith")
```

The current witness records that its own construction succeeded; it does not
yet encode a proposition in its type. Consequently, this is a checked
refinement carrier, not a proposition-indexed theorem term. Its runtime
representation is an opaque unit-like value and programs
must not inspect it.

See `assert_compile*`, `range_proven`, `index_proven` in comptime docs. The
type is usable anywhere a fact must be witnessed for a value-dependent binding.

## Dependent types

Value-dependent parameters are expressed by pairing a value with a `proof`
carrier for a property of that value. This gives practical dependent typing
without Pi/Sigma bloat.

```ny
fn sum_up_to(int n, proof p) int { ... }
```

Callers must supply a `proof` value constructed by `prove`. Range and index
assertions refine compiler facts but do not implicitly synthesize an unrelated
proof argument. Proposition-indexed witnesses are a future kernel feature.

## Refinement types

Refinements are user or engine proofs attached to base values (ranges,
indices, custom predicates). Proofs are erased after the check; the payload
keeps its concrete type.

```ny
def int x = ...
assert_compile_range(x, 0, 99, "refined index")
; x may now be used under a dependent proof param
```

`assert_compile_index`, range/index proofs, and custom `proof` tokens provide
the mechanism. All checks are backed by the existing compile-time proof engine.

## Related

- [native.md](native.md) for FFI and ABI rules.
- [errors.md](errors.md) for diagnostics and result refinement.
- [comptime.md](comptime.md) for assert_compile, range_proven, and the proof engine.
- [troubleshooting.md](../learn/troubleshooting.md) for practical strict-type debugging.
