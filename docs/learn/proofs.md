<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":105,"summary":"Establish compile-time facts and use witnesses where a checked boundary requires proof."} -->
# Proofs

Proofs turn a compile-time fact into an explicit program boundary. Use an
assertion when a fact belongs to one file. Pass a `proof<P>` witness when an
API requires that fact from its caller.

## Start with compile-time checks

```ny
use std.core

static_assert(3 * 7 == 21, "arithmetic")
assert_compile(4 * 11 == 44, "compile-time expression")

def xs = [10, 20, 30]
def i = 1
assert_compile_range(i, 0, 2, "index range")
assert_compile_index(xs, i, "index bounds")
assert(xs[i] == 20, "indexed value")
```

`static_assert` and `assert_compile` reject a source unit when their condition
is not proven. `assert_compile_range` checks inclusive bounds.
`assert_compile_index` checks the static bounds of a list access.

## Carry a witness

```ny
use std.core

fn require_positive(proof<5 > 0> witness) int { 5 }

def proof positive = prove(5 > 0, "positive literal")
assert(require_positive(positive) == 5, "witness accepted")
```

`prove(P)` creates a witness only when `P` is established during compilation.
The `proof<P>` parameter makes the required fact visible in the API signature.

## Name a reusable proposition

```ny
use std.core

fn lemma positive_sum(int x, int y) {
  x > 0 && y > 0 → x + y > 0
}

def proof positive = prove(positive_sum(3, 4))
assert(proof_matches(positive, positive_sum(3, 4)), "lemma witness")
```

`fn lemma` declares a proposition with named parameters. Its body is a single
proposition; `A → B` means `!A || B`. Applying a lemma supplies its
proposition to `prove`, so the same checked fact can be shared without copying
the expression into every caller.

Lemma applications are compile-time proof obligations. A call whose full
proposition is false or unknown is rejected; a successful proof remains erased
at runtime.

## Use inferred facts

`range_proven(value, low, high)` and `index_proven(list, index)` query facts
already established by the compiler. Keep proof checks close to the layout,
index, or pointer use they protect.

## What the kernel proves

The proof kernel normalizes equality symmetry (`a == b` and `b == a`) and
reversed ordered comparisons (`n > 0` and `0 < n`). It proves folded
constants, conservative integer ranges, and recursively checked calls to
previously proved lemmas. For proven non-negative ranges, division and modulo
by a positive constant are range-checked; products whose operand ranges fit
the checked integer domain are also supported. Unsupported arithmetic is
delegated to the configured external prover when available and is never
accepted merely because it is unknown. It does not turn a runtime boolean
into a proof and does not attempt general theorem proving.

Current limits:

- The kernel proves only its supported normalized forms, folded constants, and
 known integer ranges; it is not a general theorem prover.
- Range division requires a positive constant divisor and a non-negative
 numerator range. Other division forms require the external prover.
- Lemma composition is bounded by the proof recursion limit; cyclic or
 unsupported compositions are rejected as unknown.
- Proofs do not survive mutation of their referenced values.
- Unsupported forms are rejected rather than accepted as evidence.

When the kernel cannot prove a needed fact, restructure the check: bind the
value the compiler can see, keep the index narrow and loop-invariant, or move
the assertion closer to the layout or pointer use.

## Safe native boundaries

```bash
ny --safe-mode file.ny
ny --borrow-check --ownership-strict file.ny
```

In safe mode, raw memory loads and stores against a compiler-tracked allocation
require a proven byte range. Do not replace a failed proof with a dynamic
fallback.

## Related

- [Compile-time execution](../spec/comptime.md)
- [Types](../spec/types.md#proof-and-refinement-types)
- [Ownership](ownership.md)