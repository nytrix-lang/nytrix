# Nytrix Proofs and Dependent Types

Nytrix incorporates advanced type-theory features designed to support Lean-like proofs and Agda-level dependent types directly in the language syntax.

Currently, the grammar, parser, AST generation, and lowering-skip passes are completely implemented and verified ("green") for the following constructs. The underlying semantic discharge (the proof kernel) has now been implemented.

## `where` Refinement Types

Nytrix supports defining refinement types. The declarations parse, produce AST nodes (`stmt_refine_t`/`NY_S_REFINE`), pass statement verification, and safely skip lowering.

```ny
use std.core

refine Even = int where (v) { v % 2 == 0 }
refine Positive = int where (v) { v > 0 }
refine PositiveEven = Even where (v) { Positive(v) }   ; refinement of a refinement

def Even x = 2
```

## Existential Refinements (`exists`)

Comptime-only `exists` predicates are supported in the AST (`NY_E_EXISTS`).

```ny
use std.core

refine Even from int where (v) { exists(k) { v == 2 * k } }
refine Nat  from int where (v) { v >= 0 }
```

## Dependent Functions & `match`

Nytrix's grammar supports parameter-dependent results, preconditions (`requires`), termination metrics (`decreases`), and indexed families (`Fin<n>`).

```ny
use std.core

fn lemma nonneg(int n) { n >= 0 }

fn natsum(int n) int requires nonneg(n) {
   n * (n + 1) / 2
}

enum Vec(Fin<n>) {
   Nil(Fin<0>),
   Cons(int h, Vec(t))
}

fn sum(Vec(n) v) int decreases(n) {
   match v {
      Vec.Nil -> 0
      Vec.Cons(h, t) -> h + sum(t)
   }
}
```

## `law`-based Traits

Traits can include comptime-checked `law` blocks to enforce semantic guarantees.

```ny
use std.core

trait Show {
   fn show(any v) str
}

impl Show for Shape {
   fn show(s) str {
      match s {
         Shape.Circle(r) -> "Circle(" + to_str(r) + ")"
         Shape.Empty -> "Empty"
      }
   }
   law { show(s) != "" }
   law { show(s) == __show_canonical(s) }
}
```

## Completed Kernel Discharge Tasks
- **NEWT-01**: Refinement predicate discharge (`Even where Positive`-style stacking) is implemented and emitted as proof-typed checks natively.
- **NEWT-02**: Existential discharge (`exists(k){...}`) is handled via Fourier-Motzkin elimination and witness synthesis.
- **NEWT-03**: Evidence-gated erasure for `@pure`/`@total` correctly lowers to real erasure instead of defaulting to `unknown=true`.
- **NEWT-10/11**: Dependent-function discharge (e.g. `requires`, `decreases(n)` termination measures) is successfully modeled and discharged in the SMT engine.
