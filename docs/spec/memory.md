<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":45,"summary":"Runtime value representation, allocation prefixes, container slots, and ABI conversion boundaries."} -->
# Memory

This is the runtime representation contract used by compiler and runtime work.
It is not a license to depend on internal addresses from ordinary Nytrix source.

## Values

A dynamic `NyValue` uses a tagged representation. A small integer `n` is
stored as `(n << 1) | 1`; the low bit distinguishes it from aligned heap
pointers. `nil`, booleans, heap objects, and native handles have their own
runtime forms. Heap object kinds are recorded near the object by tags such as
`TAG_LIST`, `TAG_DICT`, `TAG_DICT_TBL`, `TAG_STR`, and `TAG_FLOAT`.

```text
tagged int n:      bits: | n<<1 | 1 |        (low bit set, value in the rest)
heap pointer p:    low bits zero (16-byte alignment)   (low bit clear)
```

The low bit is the discriminator: a dynamic value with bit 0 set is a tagged
integer; bit 0 clear means a heap pointer, nil, or a native handle form.

Never apply integer untagging to an arbitrary pointer or object value. A right
shift is valid only when the owning boundary has established that the value is
a tagged integer.

## Allocation and collection

The ordinary allocator returns a 16-byte-aligned object pointer with a 32-byte
runtime prefix immediately before it. The prefix contains the allocation magic
and tagged body size; small allocations may be reused from thread-local pools.
Reference accounting tracks ordinary managed allocations. With the opt-in GC,
the collector adds its own 16-byte collector header before that same runtime
prefix and moves/scans supported heap objects.

```text
addresses grow ->

  [ GC header 16B ] [ runtime prefix 32B ] [ body ... ]    (GC mode)
                   [ runtime prefix 32B ] [ body ... ]     (default)

  object pointer returned by the allocator points at the first body byte
```

The runtime header/prefix layout is internal. Code must allocate and release
through its owning runtime or library operation rather than constructing a
header manually.

## Container slots

Dynamic containers contain both tagged values and raw implementation fields.
For a dictionary, logical count and capacity slots are tagged integer values;
the table slot is a raw heap pointer. A list stores its logical length as a
tagged value and its elements as dynamic values. Mixing these forms-for
example, untagging a raw table pointer as if it were capacity-corrupts the
representation.

A dictionary object with `n` entries uses:

```text
dict object:
  +0x00  count    (tagged integer)
  +0x08  capacity (tagged integer)
  +0x10  table    (raw heap pointer to the entry array)

entry array:
  -0x08  TAG_DICT_TBL (raw tag slot)
  +0x00  slot 0: [state|key]   two 8-byte slots per entry
  +0x10  slot 1: [state|key]
  ...
```

The table pointer is a raw address, not a tagged value. Reading it through a
dynamic-value load applies the tagged-integer untag, which corrupts it. Use
the owning list/dictionary helpers for dynamic containers. Native code must
preserve the exact slot convention established by the runtime layout.

## Function and ABI boundaries

Typed native `i64` values are raw machine integers. Dynamic values remain
tagged. Entering a dynamic boundary boxes a raw integer; leaving one unboxes it
only after an integer check. Function lowering applies the corresponding
conversion at typed/dynamic boundaries, including parameters, returns, and
loads.

| Boundary | Value form | Rule |
| --- | --- | --- |
| Typed `int` parameter/slot | Raw signed 64-bit `i64` | No tag applied. |
| Dynamic `any` parameter | Tagged `NyValue` | Box on entry, check-and-unbox on exit. |
| Typed `int` load result | Raw `i64` | No untag; the slot is raw. |
| Dynamic load result | Tagged `NyValue` | Untag (`>> 1`) only after an integer check. |
| `int`-typed helper returning a pointer | Undefined | Raw pointers are not integers; see below. |

A raw pointer is not an `int`. Pass it through a pointer/handle/`any` boundary
appropriate to the API, never by casting it to an integer slot and relying on
integer retagging. This prevents the class of failures where a pointer is
shifted or retagged as a numeric value during a call or load. When a function
returns a raw implementation pointer, declare the return and parameter types so
the pointer crosses as a heap/`any` value, not as an `int`.

## Debugging representation faults

Start with the narrowest executable oracle. Inspect NYIR and machine-form
output only to identify the owning conversion boundary, then add a focused
native regression that crosses it. Do not use emitted assembly alone as proof
of a correct runtime representation.

A fault pattern that should never appear in native checks:

- a helper declared `fn ... int` returning a heap pointer;
- code that retags the shifted result with `(v << 1) | 1`;
- stores against that retagged base that silently miss the real object.

When those shapes appear, the owning conversion is the `int` boundary, not the
store. Move the value across as `any` and add a regression that reads back
through the same boundary.

## Related

- [Pipeline](pipeline.md) - where lowering and runtime selection occur.
- [Values](values.md) - language-level value semantics.
- [Native](native.md) - ABI and pointer contracts.