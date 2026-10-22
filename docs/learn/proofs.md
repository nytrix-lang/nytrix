<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":105,"summary":"Use compile-time assertions, bounded index types, and checked function preconditions."} -->
# Proofs

Use a proof feature only when it expresses a real program boundary. Nytrix's
solver handles a bounded set of propositions; it is not a general theorem
prover. Unknown obligations are rejected rather than treated as true. The exact
accepted forms and regression boundaries are in [Types](../spec/types.md).

## Check a compile-time fact

```ny
use core

static_assert(3 * 7 == 21, "constant arithmetic")

#main {
   assert_eq(3 * 7, 21, "runtime result")
}
```

`static_assert` checks a condition while compiling. Use an ordinary `assert`
for a condition that depends on runtime values.

## Bound an index

`Fin<N>` represents a finite index range. Pass it where the API requires an
index known to be below `N`:

```ny
use core

fn at(Fin<3> index) int {
   [10, 20, 30][index]
}

#main {
   assert_eq(at(1), 20, "finite index")
}
```

## State a precondition

A function's `requires(condition)` clause makes callers prove the precondition.
A recursive `@total` function also needs a supported `decreases` measure on
recursive calls. These checks are bounded and fail closed when a fact cannot be
established; they do not prove arbitrary termination or safety properties.

For user-defined refinement types, witnesses, lemmas, and solver limits, see
the [type specification](../spec/types.md) and
[function contract reference](../spec/functions.md#preconditions-and-termination-measures).

## Evidence

When a proof affects memory safety or a public API, run the paired positive and
negative fixture for the exact construct and backend. A passing example proves
only that obligation; it does not validate a broader theorem.
