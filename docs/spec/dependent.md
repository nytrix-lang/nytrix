<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":21,"summary":"Design and implementation status for dependent types, GADTs, type-level values, and proof extensions."} -->
# Dependent types and indexed data

> **Status:** This page describes the planned language model, not a claim that
> the syntax below is currently accepted. Refinements and compile-time proofs
> are implemented; the type-graph has internal dependent-arrow, sigma, and type
> universe nodes, but user-facing dependent typing is still in progress. See
> [Implementation status](../learn/status.md) and the phase checklist in
> [Open work](../todo/dependent-types.md).

Dependent types let a type mention a value. They can express relationships such
as “this vector has length `n`,” “this function returns a vector whose length is
the argument,” or “this enum constructor produces an expression indexed by
`int`.” Nytrix's plan builds these capabilities on its existing refinement and
proof system while preserving its ordinary function, enum, and ABI conventions.

The syntax in examples is **proposed** and is illustrative until its phase's
fixtures pass. It uses Nytrix's current style: no colon-separated annotations,
return types after `)`, generic arguments in `<>`, and `->` match arms.
Every fenced Nytrix example on this page is a syntax fragment, not a complete
program that can currently be compiled.

## Contents

- [What works today: refinements and proofs](#what-works-today-refinements-and-proofs)
- [Planned Pi types](#planned-dependent-function-types-pi-types)
- [Planned Sigma types](#planned-dependent-pairs-sigma-types)
- [Planned GADTs](#planned-gadts-and-indexed-enums)
- [Type-level computation](#planned-first-class-types-and-type-level-computation)
- [Runtime indices and proof improvements](#runtime-indices-and-proof-improvements)
- [Implementation status](#implementation-status-and-order)
- [Soundness invariants](#soundness-and-implementation-invariants)

## What works today: refinements and proofs

A refinement names a base type and predicate. It keeps the base type's runtime
representation; the predicate is a compile-time obligation, not a wrapper or a
runtime flag.

```ny
use core

refine Positive = int where (v) { v > 0 }
refine Even = int where (v) { v % 2 == 0 }
refine PositiveEven = Even where (v) { Positive(v) }

def PositiveEven n = 4
assert(PositiveEven(6), "positive even")
assert(!PositiveEven(5), "odd value is not positive-even")
```

Refining a refinement composes its predicates: membership in `PositiveEven`
checks both positivity and evenness. A successful proof may be carried as
`proof<P>`; it is erased after compile-time checking. Unknown or false
obligations fail closed rather than becoming evidence. `Fin<N>` is currently a
bounded integer refinement with a positive compile-time bound: its values obey
`0 <= value < N` and retain the integer ABI.

There are important limits. Refinement predicates are written by the user;
the compiler does not synthesize refinements. Some parameter-dependent
propositions cannot yet be discharged through calls, and proofs do not
generally survive mutation of referenced values. Nested-membership evaluation
is implemented, while a constant-initializer path remains tracked separately.
See [Types](types.md#proof-and-refinement-types) for the supported proof rules,
interfaces, and tested rejection cases.

## Planned dependent function types (Pi types)

A dependent function type is written mathematically as `Π (x : A). B(x)`: it
takes an `x` of type `A`, and the result type may refer to that value. In the
planned Nytrix syntax, a parameter's name is in scope in its return type; later
parameter types may also refer to earlier parameters.

```ny
;; Proposed syntax; not yet a supported user program.
fn make_prefix(Fin<4> n) Vec<f64, n> {
   ...
}

fn copy_prefix(Fin<8> n, Vec<f64, n> source) int {
   ...
}
```

At a call, the checker substitutes the actual argument for the bound name in
the result type. Thus a call with `2` would have a result indexed by `2`, not
by an unrelated or unconstrained length. A value may be used for this purpose
only where the checker can represent and substitute it soundly. A dependent
`Fin` obligation must be checked at the call site; an out-of-range argument
must be rejected, never silently truncated or accepted as a runtime integer.

## Planned dependent pairs (Sigma types)

A dependent pair `Σ (n : A). B(n)` packages an index and a value whose type
depends on that index. The proposed `exists` annotation introduces the binder
for the following type expression:

```ny
;; Proposed syntax; examples do not imply parser or type-checker support.
def p exists(Fin<8> n) Vec<f64, n> =
   pair(3, [1.0, 2.0, 3.0])
```

Construction checks the first component against the index type, substitutes
that component into the second component's expected type, and checks the
payload. Elimination must keep the relationship in scope: when matching a pair
as `(n, values)`, `values` has the type indexed by that arm's `n`. Projection
of the second component must retain the same dependency; it cannot return a
plain vector type that forgets the length proof.

## Planned GADTs and indexed enums

An ordinary enum variant selects a constructor. A generalized algebraic data
type (GADT) additionally states the type index produced by each constructor.
Matching a constructor refines that index for the arm, so payloads and
expressions in the arm are checked under the refined type.

```ny
;; Proposed syntax; constructor constraints and type names are under design.
enum Expr<t> {
   IntLit(int) where t is int,
   BoolLit(bool) where t is bool,
   Add(Expr<int>, Expr<int>) where t is int
}

fn eval_int(Expr<int> expression) int {
   match expression {
      Expr.IntLit(n) -> n
      Expr.Add(left, right) -> eval_int(left) + eval_int(right)
   }
}
```

For example, `IntLit(n)` establishes the `int` index in its arm. The checker
must use that fact when typing `n` and any recursive `Expr<t>` values. A
constructor's evidence must not be forgeable by an ordinary value. Exhaustive
matching and the existing distinction between raw unit-enum discriminants and
tagged dynamic values remain in force. Any runtime evidence or layout change
must be explicit and ABI-compatible; index refinement is not permission to
reinterpret arbitrary integers as constructors.

## Planned first-class types and type-level computation

The `Type` universe is intended to represent types during compilation. Type
values are compile-time entities and should erase at runtime; they are not
runtime reflection objects unless a separate runtime API explicitly requests
that. A type-level function computes a type and must normalize predictably.

```ny
;; Proposed syntax; type values and type functions are not yet supported.
def T Type = int
type Element(list<T>) = T
```

For example, normalizing `Element(list<int>)` should yield `int`. Recursive
type-level definitions need a termination/decrease check (or another bounded
normalization rule); a definition such as `Bad<T> = Bad<T>` must be rejected
instead of looping or exhausting compiler resources.

## Runtime indices and proof improvements

Today, `Fin<N>` and similar bounds require a positive compile-time `N`. The
planned runtime-index extension would allow a bound that is known only while
the program runs, but it must then either emit a checked dynamic bounds test or
require proof evidence that includes the runtime value and bound. It must not
pretend a runtime fact was proved at compile time.

Planned proof-system work also includes preserving a refinement across a
mutation only when alias analysis justifies it, mapping caller facts to a
callee's parameter-dependent preconditions, and optionally synthesizing
refinement qualifiers. Synthesis is opt-in and is distinct from the existing
heuristic flow facts; it must never silently turn an unknown proposition into
a proof. Negative tests must continue to reject unknown bounds, invalid
indices, unsupported type-level recursion, and unsound mutation/call cases.

## Implementation status and order

| Area | Current status |
| --- | --- |
| Refinements, proof values, compile-time `Fin<N>` | Implemented within the boundaries in [Types](types.md#proof-and-refinement-types). |
| Internal dependent-arrow, sigma, and `Type` graph nodes | Present as internal type-system plumbing; not equivalent to accepted source syntax. |
| Pi types: dependent returns/parameters and call substitution | In progress; [`pi-return`](../../etc/tests/lang/types/dependent/pi/pi-return.nshape), [`pi-param`](../../etc/tests/lang/types/dependent/pi/pi-param.nshape), [`pi-call`](../../etc/tests/lang/types/dependent/pi/pi-call.nshape), and [`pi-fin-bound`](../../etc/tests/lang/types/dependent/pi/pi-fin-bound.nshape) exist, but full route acceptance is not established. |
| Sigma types and dependent-pair construction/elimination/projection | Planned; no completed phase acceptance. |
| GADTs and index refinement in matches | Planned; ordinary ADTs do not imply GADT support. |
| First-class type values, type-level functions, runtime indices | Planned; syntax and normalization are not supported yet. |
| Mutation/call proof transport and qualifier synthesis | Planned; existing proof limits remain. |
| Refinement intersections | Membership covers nested predicates ([fixture](../../etc/tests/lang/types/refinements/refinement-intersection.nshape)); constant `def` initialization still has an [open tracked defect](../todo/dependent-types.md#phase-7-refinement-composition). |

Work is staged in dependency order: Pi types, Sigma types, GADTs,
first-class/type-level features, proof-system extensions, then remaining
refinement composition. A phase is complete only after its positive and
negative fixtures pass the required native, LLVM JIT (MCJIT and ORC), and
native-only routes, with existing proof, `Fin`, tuple, and ADT behavior
preserved. The current phase-by-phase acceptance record lives in
[Open work](../todo/dependent-types.md).

## Soundness and implementation invariants

- Substitution is capture-avoiding and preserves the binder's scope.
- A type index is checked at construction and call boundaries; casts do not
  manufacture proof evidence.
- Unknown proof obligations remain errors or explicit dynamic checks, never
  implicit success.
- Erased compile-time types do not change runtime layout. Runtime-dependent
  checks, if needed, are explicit and observable in the generated program.
- Enum tags, payload layout, native calling conventions, optimizer behavior,
  and the unit-enum/`any` distinction remain correct across execution routes.
- Unsupported or non-terminating type-level forms receive a diagnostic rather
  than hanging or being accepted by a fallback.
