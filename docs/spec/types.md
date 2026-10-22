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

The full boundary contract—headers, ownership, strings, packing, and
alignment—is in [Native](native.md).

## Proof and refinement types

User-defined refinements name a base type plus a compile-time predicate over
the value:

```ny
use core

refine Even = int where (v) { v % 2 == 0 }
refine Positive = int where (v) { v > 0 }
refine PositiveEven = Even where (v) { Positive(v) }

assert(PositiveEven(4), "positive even")
```

`refine X = Base where (v) { pred }` introduces a new nominal type `X` whose
representation is exactly `Base` (ABI-preserving), carrying the predicate as a
compile-time obligation on construction. A refinement of a refinement
substitutes the inner bindings into the outer predicate, forming an
intersection (`PositiveEven` is both `> 0` and even). Comptime-only predicates
use the bracketed form `exists(k) { pred }`:

```ny
refine EvensUpToPowerTwo = int where (v) { exists(k) { v == 2 * k } }
```

The proof kernel discharges refinement predicates at compile time: constant
initializers and call arguments are verified against the predicate via compile-time
evaluation or the Presburger/Fourier-Motzkin solver. Refinements are subtyped
covariantly (`Even <: int`), preserving the underlying ABI, and membership can be
evaluated as a compile-time value (`assert(Even(6))`).

`Fin<N>` is the bounded integer type whose values satisfy `0 <= value < N`.
`N` must normalize to a positive compile-time integer. It uses the integer ABI,
so passing a `Fin<N>` to an `int` parameter is representation-preserving, but
constructing or passing an integer as `Fin<N>` requires a proven bound.

```ny
use core

fn load_slot(Fin<4> index) int {
   def values = [10, 20, 30, 40]
   values[index]
}

def Fin<4> selected = 2
assert(load_slot(selected) == 30, "bounded index")
```

Different bounds remain different static types: `Fin<4>` is not silently
reinterpreted as `Fin<8>`. Literal and immutable compile-time bounds may be
used in generic specialization keys. Functions may specify parameter-dependent
`requires <expr>` preconditions and `decreases(expr)` termination measures for
recursive descent.

User-defined indexed types such as `Slot<N>` require a declared `Slot` base
type; the index does not synthesize an undeclared type. The indexed form keeps
the declared base ABI, while integer literals passed to `Slot<N>` are checked
against `0 <= value < N`. For that check, a symbolic `N` must resolve to an
immutable positive compile-time integer.

`proof<P>` is erased compile-time evidence that proposition `P` was proved.
It is not a runtime boolean and ordinary values cannot stand in for it.

```ny
use core

fn require_positive(proof<5 > 0> evidence) int {
   5
}

def proof positive = prove(5 > 0, "positive constant")
require_positive(positive)
```

`fn lemma` declares a named proposition for use with `prove`. A lemma body is
one proposition and may use `A → B` implication syntax:

```ny
use core

fn lemma positive_sum(int x, int y) {
  x > 0 && x < 100 && y > 0 && y < 100 → x + y > 0
}

def proof sum_is_positive = prove(positive_sum(3, 4))
```

The application is checked at compile time and the resulting witness is
erased, like any other `proof<P>` value.

### Refinement interfaces

`--emit-refinement-interface=PATH` writes a versioned interface after successful
type validation; `--use-refinement-interface=PATH` imports it before type
validation. Interface records preserve the refinement's qualified name, base
type, formal name, and canonical predicate. The format is tied to the compiler
semantic version and rejects malformed, unsupported, or incompatible records
instead of importing an empty interface.

The interface carries only predicates accepted by the canonical pure
proposition encoding. Unsupported expressions, explicit literal type hints,
and strings with embedded NUL bytes fail export. The importer accepts scalar
literal/formal boolean and comparison expressions; it does not transport
mutable state or aggregate alias facts. Imported refinements participate in
the same guarded-path checks as source declarations.

Proposition matching is structural: equivalent equality/order spellings are
normalized, while unrelated propositions are rejected. `prove` accepts only
a condition the compiler can establish; false and unknown conditions fail.

This is refinement-proof support, not full dependent typing. Nytrix does not
implement liquid-type or refinement-qualifier synthesis: nullable, range,
index, and discriminant facts in the compiler's `refined.v1` artifact are
heuristic flow facts, not user-defined inferred refinements. Some
parameter-dependent propositions cannot yet be resolved through calls, and
proofs do not survive mutation of their referenced values. Unsupported forms
are rejected rather than accepted as evidence.

### Proof boundary regression matrix

The acceptance boundary is exercised by paired positive and negative cases:

| Boundary | Accepted case | Rejected or inconclusive case |
| --- | --- | --- |
| Closed and normalized propositions | [`proof-indexed.nshape`](../../etc/tests/code/intermediate/ny/proof-indexed.nshape), [`arithmetic-normalization.nshape`](../../etc/tests/lang/types/proofs/arithmetic-normalization.nshape) | [`proof-solver-soundness-negative.nshape`](../../etc/tests/lang/errors/type/proof-solver-soundness-negative.nshape), [`proof-proposition-mismatch.nshape`](../../etc/tests/lang/errors/type/proof-proposition-mismatch.nshape) |
| Bounded nonlinear intervals and finite splitting | [`policy-certificates.nshape`](../../etc/tests/lang/types/proofs/policy-certificates.nshape) | Same fixture checks a false bound, unsafe overflow premise, and unproved nonlinear obligation |
| Resource and parser limits | Same fixture checks an over-budget canonical proposition stays unknown | [`policy-recursion.nshape`](../../etc/tests/lang/types/proofs/policy-recursion.nshape) checks depth-limit rejection and fail-closed certificates |
| Refinement construction and transport | [`refinements.nshape`](../../etc/tests/lang/types/refinements/refinements.nshape), [`refinement-interface.nshape`](../../etc/tests/lang/comptime/reflection/refinement-interface.nshape) | [`type-errors.nshape`](../../etc/tests/lang/errors/type/type-errors.nshape), [`relational-refinement-guard.nshape`](../../etc/tests/lang/errors/type/relational-refinement-guard.nshape) cover unknown, mutation, and closure boundaries |
| Caller obligations across execution routes | [`portable-contracts.nshape`](../../etc/tests/lang/types/proofs/portable-contracts.nshape), [`portable-contract-binding-capacity.nshape`](../../etc/tests/lang/types/proofs/portable-contract-binding-capacity.nshape) | [`portable-contracts-errors.nshape`](../../etc/tests/lang/types/proofs/portable-contracts-errors.nshape), [`proof-call-site-unknown-negative.nshape`](../../etc/tests/lang/errors/type/proof-call-site-unknown-negative.nshape) |

These fixtures define the supported calculus and its fail-closed edges. A new
accepted syntax or transport path must add a positive case and a paired false
or unknown case before it is documented as supported.

For **`requires` / `decreases` / `@total`** enforcement levels and trait **`law`**
checking, see [Implementation status](../learn/status.md). For
**`trait` / `impl`** syntax, see [Traits and impl](traits.md).

Networking in the stdlib is documented under [Networking](../learn/networking.md)
(HTTP, sockets, tubes, browser WebSocket). There is no MQTT client today.

See [Comptime](comptime.md) for compile-time assertions and proof construction rules.

## Related

- [Runtime](runtime.md) for mutability, ownership, and collections.
- [Native](native.md) for FFI and ABI rules.
- [Numbers](../learn/numbers.md) for the numeric tower and conversions.
- [Errors](errors.md) for diagnostics and result refinement.
