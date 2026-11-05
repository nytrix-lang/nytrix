<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":21,"summary":"Design and implementation status for dependent types, GADTs, type-level values, and proof extensions."} -->
# Dependent types and indexed data

> **Status:** Refinements, compile-time proofs, value-indexed `Fin<N>`,
> dependent return and parameter types with call-site substitution, dependent
> pairs (Sigma), GADT constructor evidence, first-class `Type` values,
> terminating type-level functions, runtime-index checks, and refinement-safe
> mutation are implemented within the boundaries below. See the
> [implementation status](#implementation-status-and-order) and
> [soundness invariants](#soundness-and-implementation-invariants).

Dependent types let a type mention a value. They can express relationships such
as “this vector has length `n`,” “this function returns a vector whose length is
the argument,” or “this enum constructor produces an expression indexed by
`int`.” Nytrix builds these capabilities on its existing refinement and proof
system while preserving its ordinary function, enum, and ABI conventions.

Use type-first bindings consistently: `def Type name = value`, function
parameters as `Type name`, and return types after `)`. Generic arguments use
`<>`, and match arms use `->`. The parser also accepts selected name-first
spellings, such as `def p exists(int n) Fin<n> = pair(3, 2)`,
`def i Fin<n> = checked_fin(raw, n)`, and `def T Type = int`; these have the
same semantics, but new examples prefer the canonical type-first form.

## Contents

- [What works today: refinements and proofs](#what-works-today-refinements-and-proofs)
- [Dependent function types (Pi types)](#dependent-function-types-pi-types)
- [Dependent pairs (Sigma types)](#dependent-pairs-sigma-types)
- [GADTs](#gadts-and-indexed-enums)
- [Type-level computation](#first-class-types-and-type-level-computation)
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

Indexed enums may also declare constructor evidence. A variant written
`IntLit(int) where t is int` constructs `Expr<int>`; matching an `Expr<int>`
accepts that variant, rejects constructors that refine to another index, and
does not require impossible arms for exhaustiveness. Constructor evidence is
not forgeable because the constructor expression itself carries the constrained
type.

There are important limits. Refinement predicates are user-authored; the
compiler does not synthesize arbitrary liquid or refinement qualifiers. Calls
transfer only the supported caller facts into parameter-dependent preconditions;
unsupported propositions remain unknown. Mutation preserves a fact only when
the analysis proves the write cannot invalidate it; uncertain aliasing fails
closed. Nested-membership evaluation and constant `def` initialization use the
same refinement rules. See [Types](types.md#proof-and-refinement-types) for
proof interfaces and tested rejection cases.

## Dependent function types (Pi types)

A dependent function type is written mathematically as `Π (x : A). B(x)`: it
takes an `x` of type `A`, and the result type may refer to that value. In
Nytrix, a parameter's name is in scope in its return type; later parameter
types may also refer to earlier parameters.

```ny
use core

refine Vec = int where (v) { v >= 0 }

fn vsum(Fin<4> n) Vec<n> {
   return n
}

fn consume_two(Vec<2> value) int {
   return value
}

def result = consume_two(vsum(2))
assert_eq(result, 2, "the call substitutes the literal index into the return type")
```

At a call, the checker substitutes the actual argument for the bound name in
the result type. Thus a call with `2` would have a result indexed by `2`, not
by an unrelated or unconstrained length. A value may be used for this purpose
only where the checker can represent and substitute it soundly. A dependent
`Fin` obligation must be checked at the call site; an out-of-range argument
must be rejected, never silently truncated or accepted as a runtime integer.

## Dependent pairs (Sigma types)

A dependent pair `Σ (n : A). B(n)` packages an index and a value whose type
depends on that index. The `exists` annotation introduces the binder for the
following type expression:

```ny
use core

def exists(int n) Fin<n> witness = pair(3, 2)
assert_eq(witness[0], 3, "index")
assert_eq(witness[1], 2, "payload")
```

`pair(index, value)` has the ordinary two-element tuple representation and is
projected with `[0]` and `[1]`; the dependency is erased at runtime. The
annotation can also follow the binding name (`def witness exists(int n) Fin<n>
= ...`) and can appear in parameter and return types.

Construction checks the first component against the index type. When the index
is a literal, that literal replaces the binder in the payload type before the
second component is checked: `pair(3, 5)` is rejected because the substituted
payload type is `Fin<3>`. The same construction check applies at call
boundaries. Projection of a literal-indexed pair exposes the substituted
payload type, while a pair received as a parameter keeps its symbolic payload
type. Destructuring (`def [n, values] = ...`) provides the components but does
not yet introduce the dependent payload type inside a `match` arm; projection
is the supported elimination form that preserves the dependency.

## GADTs and indexed enums

An ordinary enum variant selects a constructor. A generalized algebraic data
type (GADT) additionally states the type index produced by each constructor.
Matching a constructor refines that index for the arm, so payloads and
expressions in the arm are checked under the refined type.

```ny
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

## First-class types and type-level computation

The `Type` universe represents types during compilation. Type values erase at
runtime to their canonical type-name string; they are not runtime reflection
objects. Write `def Type T = int` as the canonical type-first form; the
name-first spelling `def T Type = int` is also accepted:

```ny
use core

def Type T = int          ;; canonical: type-first
def U Type = int          ;; name-first, matching dependent annotations
type Element(list<T>) = T

def Element(list<int>) n = 5   ;; structural application, parenthesized
def Element<list<str>> s = "x" ;; or angle-bracket application
def T answer = 42             ;; T is a compile-time alias for int
```

Normalizing `Element(list<int>)` yields `int`. Parameter patterns can be bare
binders (`type Box<A> = list<A>`) or structural (`type Elem(list<T>) = T`), and
definitions may refer to earlier definitions, as in
`type Nested<A> = Wrapped<Wrapped<A>>`. Definitions are module-scope
compile-time declarations; they are expanded while parsing type references,
never emitted to IR, and are applied with either `<...>` or `(...)`.

Normalization is bounded and cycle-checked. A definition such as `Bad<T> =
Bad<T>` is rejected with a termination diagnostic instead of looping or
exhausting compiler resources; mismatched structural arguments and the wrong
number of arguments are rejected at the application site. A `Type` value used
as an expression is its canonical name string, so `assert(T == "int")` holds.

## Runtime indices and proof improvements

`Fin<N>` and similar bounds normally require a positive compile-time `N`. A
bound known only at run time uses `checked_fin(value, bound)` from `core`:

```ny
use core

fn pick(int n, Fin<n> i, list<int> xs) int { xs[i] }

fn at(int n, int raw, list<int> xs) int {
   def i Fin<n> = checked_fin(raw, n)
   xs[i]
}
```

`checked_fin` performs the dynamic bounds test and returns the integer ABI
value; the checker records the bound in the result type (`Fin<bound>` when the
bound is a literal or a parameter name). A literal value outside a literal
bound, and a non-positive bound, are compile-time errors. Symbolic bounds are
not treated as proved facts: the runtime test remains mandatory. Calls
substitute a literal argument into later parameter types, so
`pick(3, checked_fin(1, 3), xs)` checks the second argument against `Fin<3>`.

Proof-system extensions preserve a refinement across a mutation only when
alias analysis justifies it, map caller facts to a callee's parameter-dependent
preconditions, and optionally synthesize refinement qualifiers. Synthesis is
opt-in and is distinct from the existing heuristic flow facts; it never
silently turns an unknown proposition into a proof. Negative tests reject
unknown bounds, invalid indices, non-terminating type-level recursion,
mismatched type-level applications, and unsound mutation/call cases.

## Implementation status and order

| Area | Current status |
| --- | --- |
| Refinements, proof values, compile-time `Fin<N>` | Implemented within the boundaries in [Types](types.md#proof-and-refinement-types). |
| Internal representation | Dependent-arrow, sigma, and `Type` graph nodes are implementation details; the accepted source forms and boundaries are listed below. |
| Pi types: dependent returns/parameters and literal call substitution | Implemented and verified on all routes: [`pi-return`](../../etc/tests/lang/types/dependent/pi/pi-return.nshape), [`pi-param`](../../etc/tests/lang/types/dependent/pi/pi-param.nshape), [`pi-call`](../../etc/tests/lang/types/dependent/pi/pi-call.nshape), [`pi-fin-bound`](../../etc/tests/lang/types/dependent/pi/pi-fin-bound.nshape), and [`pi-substitute`](../../etc/tests/lang/types/dependent/pi/pi-substitute.nshape). |
| Sigma types and dependent-pair construction/elimination/projection | Implemented with `exists(IndexType binder) ValueType` annotations and `pair(index, value)`; see [construction](../../etc/tests/lang/types/dependent/sigma/sigma-construct.nshape), [dependent projection](../../etc/tests/lang/types/dependent/sigma/sigma-projection.nshape), [invalid payload](../../etc/tests/lang/types/dependent/sigma/sigma-negative-dependent-payload.nshape), and [malformed binder rejection](../../etc/tests/lang/types/dependent/negative-dependent-pair.nshape). |
| GADTs and index refinement in matches | Implemented for `where t is T` constructor evidence, constructor typing, impossible-arm rejection, exhaustive matching, and method dispatch; see [indexed matching](../../etc/tests/lang/types/dependent/gadt/gadt-match.nshape), [method dispatch](../../etc/tests/lang/types/dependent/gadt/gadt-method.nshape), and [negative pattern rejection](../../etc/tests/lang/types/dependent/gadt/gadt-negative-pattern.nshape). |
| First-class type values, type-level functions, runtime indices | Implemented: `Type` bindings, structural `type` definitions with bounded normalization, and `checked_fin(value, bound)`; see [type values](../../etc/tests/lang/types/dependent/first-class-type/type-alias.nshape), [type-level functions](../../etc/tests/lang/types/dependent/first-class-type/type-level-function.nshape), and [runtime index checks](../../etc/tests/lang/types/dependent/runtime-index/runtime-index-construct.nshape). |
| Mutation/call proof transport and qualifier synthesis | Refined scalar bindings keep their facts across proved mutation; caller refinements discharge `requires`. Qualifier synthesis remains opt-in and off by default. |
| Refinement intersections | Membership covers nested predicates and constant `def` initialization ([fixture](../../etc/tests/lang/types/refinements/refinement-intersection.nshape)). |

The fixture matrix covers Pi types, Sigma types, GADTs, first-class/type-level
features, runtime indices, proof transport, and refinement composition. Each
fixture declares its required native, LLVM JIT (MCJIT and ORC), or native-only
routes; these route results are verified independently of this summary. The
tests also retain controls for malformed types, impossible GADT arms, invalid
bounds, and unsound proof transport.

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
- Type-level normalization is bounded and cycle-checked; recursive, malformed,
  mismatched, and wrong-arity definitions receive a diagnostic instead of
  hanging or being accepted by a fallback.
- First-class `Type` values erase to their canonical name string and never
  change the runtime layout of the types they denote.
