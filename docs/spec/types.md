<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":20,"summary":"Values, type annotations, inference, mutability, and the boundaries between static and dynamic data."} -->
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
Pointers (`*T`), `handle`, and `fnptr` are distinct. Use `layout` for a
record whose field order and representation are part of an ABI; use `struct`
for an ordinary Nytrix value. Do not use a handle as a pointer unless the
foreign API documents that conversion.

```ny
layout Pixel {
   u8 r,
   u8 g,
   u8 b,
   u8 a
}
```

The full boundary contract-headers, ownership, strings, packing, and
alignment-is in [Native](native.md).

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

`fn lemma` declares a named proposition for use with `prove`. A lemma body is
one proposition and may use `A → B` implication syntax:

```ny
fn lemma positive_sum(int x, int y) {
  x > 0 && y > 0 → x + y > 0
}

def proof sum_is_positive = prove(positive_sum(3, 4))
```

The application is checked at compile time and the resulting witness is
erased, like any other `proof<P>` value.

Proposition matching is structural: equivalent equality/order spellings are
normalized, while unrelated propositions are rejected. `prove` accepts only a
condition the compiler can establish; false and unknown conditions fail.

This is refinement-proof support, not full dependent typing. Some
parameter-dependent propositions cannot yet be resolved through calls, and
proofs do not survive mutation of their referenced values. Unsupported forms
are rejected rather than accepted as evidence. See [Comptime](comptime.md) for
compile-time assertions and proof construction rules.

## Related

- [Runtime](runtime.md) for mutability, ownership, and collections.
- [Native](native.md) for FFI and ABI rules.
- [Numbers](../learn/numbers.md) for the numeric tower and conversions.
- [Comptime](comptime.md) for compile-time assertions and refinements.
- [Errors](errors.md) for diagnostics and result refinement.