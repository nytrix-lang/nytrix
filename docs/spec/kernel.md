<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"spec","order":22,"summary":"Trusted kernel rules, erasure and ABI model, and the soundness risk register for dependent and proof-carrying Nytrix."} -->
# Kernel, erasure, and soundness

This page fixes the rules the checker is allowed to trust. Everything else —
parsers, elaborators, tactics, codegen, user-written provers — is an untrusted
producer that must hand the kernel a term it checks. The roadmap that consumes
these rules is [Type system and proof power](../todo/type.md); the user-facing
behaviour is described in [Dependent types](dependent.md) and
[Types](types.md).

> **Status:** This page records the target core and the boundaries of what ships
> today. Sections marked *implemented* describe behaviour on the current tree;
> sections marked *proposed* are normative design that is not wired yet. Where
> the two differ, the implemented text wins until the roadmap phase lands.

## Contents

- [Resolved decisions](#resolved-decisions)
- [Core terms](#core-terms)
- [Typing and conversion](#typing-and-conversion)
- [Universes](#universes)
- [Inductives and elimination](#inductives-and-elimination)
- [Equality](#equality)
- [Totality](#totality)
- [Erasure and the ABI model](#erasure-and-the-abi-model)
- [Soundness risk register](#soundness-risk-register)
- [Rejection cases](#rejection-cases)

## Resolved decisions

Rules that are implemented and fixture-backed. A roadmap item moves here when it
lands, and then leaves the roadmap; anything not in this table is still open.

| Rule | Behaviour | Evidence |
| --- | --- | --- |
| Kernel specification | Core terms, typing and conversion, universes, inductive rules, equality, and totality are fixed by this page. | This page |
| Erasure and ABI model | Proof-only content erases; argument slots, layout indices, and FFI boundaries are specified. | [Erasure and the ABI model](#erasure-and-the-abi-model) |
| Soundness risk register | Every accepted axiom or escape is listed and off by default. | [Register](#soundness-risk-register) |
| Spec cross-review | Read end-to-end against [Dependent types](dependent.md) and [Types](types.md); disagreements fixed for proof-value syntax, the `Fin` erasure split, the universe-rejection wording, and solver naming. | This page (2026-11-16) |
| Literal universe levels | `Type u` parses and canonicalizes; a bare `Type` is level 0; `Type n` never inhabits `Type n`. | [universe-levels](../../etc/tests/lang/types/dependent/universes/universe-levels.nshape), [negative](../../etc/tests/lang/types/dependent/universes/universe-type-type-negative.nshape) |
| Cumulativity | `Type n` inhabits every `Type m` with `m > n`, not only `m = n + 1`. | [universe-levels](../../etc/tests/lang/types/dependent/universes/universe-levels.nshape) |
| Universe-as-value rejection | `def Type T = Type` fails at parse time by the level rule instead of escaping to runtime symbols. | [kernel-universe-value-negative](../../etc/tests/lang/types/dependent/kernel/kernel-universe-value-negative.nshape) |
| Type-value self-alias rejection | `def Type T = T` fails with `type value cannot refer to itself`, like `type Bad<T> = Bad<T>`. | [kernel-type-self-alias-negative](../../etc/tests/lang/types/dependent/kernel/kernel-type-self-alias-negative.nshape) |
| Type-value validation | `def Type` bindings validate through the same normalization/cycle path as `type` declarations. | the two `def Type` fixtures above |
| Universe values | `Type` and `Type n` are first-class in expression position and evaluate to the erased universe tag (`"Type"`, `"Type n"`), matching the `str` runtime representation, instead of failing symbol resolution. Levels stay static indices, not runtime data. | [universe-values](../../etc/tests/lang/types/dependent/universes/universe-values.nshape) |
| Closed level constraints | `Type max(n, m)` folds to the larger literal at parse time in both type and expression position (`max(3, 5)` behaves as `Type 5`), including idempotence; a malformed constraint is an error, never a silent level 0. | [level-max](../../etc/tests/lang/types/dependent/universes/level-max.nshape) |
| Function type parameters | `fn id<A>(A x) A` declares its parameters, and annotations that name a declared parameter are erased to the unannotated dynamic path, so one declaration serves both value shapes: `id(3)` prints `3` and `id("s")` prints `s`, and `fn first<A, B>(A a, B b) A` instantiates independently. Annotations that mention a parameter inside a larger type (`list<A>`) are not erased; the resolver accepts the generic container type and the parameter stays unrelated to the call's argument type. A parameter that appears in more than one argument position is checked statically at the call site: `same(1, "s")` for `fn same<A>(A a, A b)` is rejected with `type parameter 'A' instantiated inconsistently`, and the pipeline never runs a program that reported an error. Instantiation is otherwise dynamic: per-instance specialization and name-based monomorphization are not implemented. | [fn-type-param](../../etc/tests/lang/types/generics/fn-type-param.nshape) |
| Parameterised enums | Uniform and multi-parameter enums, plus type-level aliases over them, compile and run. | [enum-uniform-params](../../etc/tests/lang/types/dependent/enum-params/enum-uniform-params.nshape) |
| Positive recursion | Constructor arguments may recurse as ordinary data and eliminate by `match`. | [enum-positive-recursion](../../etc/tests/lang/types/dependent/enum-recursion/enum-positive-recursion.nshape) |
| Structural positivity | Negative occurrences are inexpressible; a function-typed payload is rejected with `enum payload fields must be data types`. | [negative fixture](../../etc/tests/lang/errors/type/enum-payload-function-negative.nshape), note in [Inductives](#inductives-and-elimination) |
| Bounded indexing | Literal `Fin<N>` indexing is checked at construction; a symbolic bound keeps the runtime `checked_fin` test. | [kernel-safe-index](../../etc/tests/lang/types/dependent/kernel/kernel-safe-index.nshape) |
| Indexed elimination | Constructors refine the index; impossible arms are not demanded. | [kernel-verified-algorithm](../../etc/tests/lang/types/dependent/kernel/kernel-verified-algorithm.nshape), GADT fixtures in [Dependent types](dependent.md) |
| Closed-equation evidence | `proof<a == b>` with `prove` discharges convertible closed equations and erases. | [eq-closed-refl](../../etc/tests/lang/types/proofs/eq-closed-refl.nshape) |
| Symbolic reflexivity | `prove(t == t)` (also `<=`, `>=`) discharges for any pure, non-floating term, including parameters unknown at compile time; the sides are compared structurally on the canonical proposition, so this is the `refl` rule, not evaluation. Calls, member access, indexing, and float operands fail closed (NaN is not reflexive), and `prove(x == y)` for distinct names stays an error. | [eq-refl-symbolic](../../etc/tests/lang/types/proofs/eq-refl-symbolic.nshape), [negative](../../etc/tests/lang/errors/type/eq-refl-symbolic-negative.nshape) |
| Proof-parameter instantiation | A proof parameter whose proposition mentions earlier value parameters is instantiated with integer-literal arguments and re-normalized before the witness is compared (`fn need(int x, int y, proof<x + 1 == y> p)` accepts `need(2, 3, prove(2 + 1 == 3))`); the native call check substitutes every literal binder at once. A false instance is a proposition mismatch, and a non-literal argument still has to match by name. | same fixtures |
| `@proves` from preconditions | On the NYIR route a post-condition is checked against the range of each returned expression, where an `int` parameter that the body never reassigns takes the bounds stated by its `requires` conjunction (`requires n > 0` proves `result > 1` for `return n + 1`); a reassigned parameter or a weaker bound is rejected. | [proves-requires-range](../../etc/tests/lang/types/proofs/proves-requires-range.nshape), [negative](../../etc/tests/lang/errors/type/proves-requires-range-negative.nshape) |
| Fail-closed proofs | A false equation and an unproved bounded-arithmetic refinement stay errors. | [eq negative](../../etc/tests/lang/errors/type/eq-closed-refl-negative.nshape), [refinement negative](../../etc/tests/lang/errors/type/verified-small-algorithm-negative.nshape) |
| Verified small algorithm | Literal initializers and refinement-to-refinement identities discharge without a hand-written proof. | [verified-small-algorithm](../../etc/tests/lang/types/proofs/verified-small-algorithm.nshape) |
| Undefined-name diagnostic | A name with no local, function, module export, runtime symbol, constant, or alloc builtin reports a located `[E2001] error: undefined symbol 'x'` with a hint at the point resolution becomes final, instead of surfacing only as a NYIR build failure. | [lexer-invalid-byte-stops](../../etc/tests/lang/errors/parser/lexer-invalid-byte-stops.nshape), [undefined-call](../../etc/tests/lang/errors/type/undefined-call.nshape) |
| Call arity | Omitted trailing arguments are partial application, so only excess arguments are an arity error: `too many arguments: expected at most N, got M`. | [excess-arg](../../etc/tests/lang/errors/desugar/excess-arg.nshape), [excess-call-arity](../../etc/tests/lang/errors/invariant/excess-call-arity.nshape), [Partial application](functions.md#partial-application) |

## Core terms

The kernel sees only these forms. Surface syntax is desugared into them.

| Term | Meaning | Surface today |
| --- | --- | --- |
| `Type u` | Universe at level `u`. | `Type`, `Fin<N>`, refinements (*level index proposed*) |
| `Pi (x : A). B` | Dependent function; `x` may occur in `B`. | `fn` parameters, dependent returns (*implemented*) |
| `Lam (x : A). t` | Function term. | `fn` bodies |
| `App t a` | Application; substitutes `a` for `x` in `B`. | calls |
| `Ind D (params) (indices)` | Inductive family with constructors. | `enum`, indexed `enum` (*params/indices implemented for GADT evidence*) |
| `Con_i args` | Constructor application. | variant construction |
| `Match (motive) scrutinee branches` | Elimination; every branch is checked against the motive. | `match` |
| `Eq A x y` / `refl` | Identity type and its constructor. | proof existentials only (*proposed as a primitive*) |
| `Sort`-bound variables, `let`, `Def` | Local and named definitions with unfolding. | `let`, `def` |

Not core terms, and deliberately so: refinement predicates, `proof<P>`, `prove`,
`lemma`, `Fin<N>` bounds, `assert_compile*`, and `decreases` clauses. Each is
surface sugar that elaborates to a term or to an obligation the checker
discharges; none may appear in a kernel judgement.

## Typing and conversion

A judgement is `Γ ⊢ t : T`. The rules are ordinary: variable lookup, `Pi`
formation with the binder in scope, application elimination by substitution,
`let`/`def` unfolding, and `Match` checking each branch against the motive
instantiated at that constructor.

Conversion `t ≡ u` is definitional equality, decided by:

1. beta for `App (Lam …)`;
2. delta for transparent `def`/`let`;
3. iota for `Match` on a constructor;
4. eta for functions, only where the surface already erases arity differences;
5. congruence, structurally.

Conversion never unfolds a `def` marked opaque and never uses rewrite lemmas.
Anything that needs propositional reasoning is a proof obligation, not
conversion.

## Universes

*Proposed.* Universes are predicative and cumulative, written `Type 0`,
`Type 1`, …. `Pi` lives in the maximum of its domain and codomain levels;
inductives live in the maximum of their parameters, indices, and constructor
argument levels. `Type u : Type (u + 1)`, so `Type : Type` is rejected.

*Implemented today.* `Type u` carries a literal universe level. `Type n`
inhabits `Type (n + 1)` and never `Type n`, so `Type : Type` is rejected
instead of escaping into runtime symbol resolution; a bare `Type` is level 0.
Cumulativity holds for type values: `Type n` inhabits every `Type m` with
`m > n`, not merely `m = n + 1`. Level variables, level constraints (`max`),
and level-polymorphic definitions remain open Phase 1 work, so an unsolved level
is an error rather than silently 0.

Probed on the current tree (2026-11-16):

| Program | Result |
| --- | --- |
| `type Bad<T> = Bad<T>` | Rejected with the bounded-normalization cycle diagnostic. |
| `def Type T = T` | Rejected at parse time with `type value cannot refer to itself`. |
| `def Type T = Type` | Rejected: `universe level too low for this type value`. |
| `def Type 0 T = Type 0` | Rejected: that is `Type : Type`. |
| `def Type 1 T = Type 0` | Accepted: level 0 inhabits level 1. |
| `def Type 2 T = Type 1` | Accepted: levels compose one step at a time. |
| `def Type 5 T = Type 0` | Accepted: cumulativity, level 0 inhabits any higher level. |
| `def Type 1 T = Type 1` | Rejected: a level never inhabits itself. |

The level rule is checked where a type value is bound; the self-alias case keeps
its cycle diagnostic.

## Inductives and elimination

An inductive declaration carries parameters (uniform across constructors),
indices (may vary per constructor), and constructors with their argument types.
Elimination is `Match` with a motive; the kernel checks:

- **Positivity:** the recursive occurrence may appear only in strictly positive
  position in constructor arguments. Negative or double-negative occurrences are
  rejected. Measured today (2026-11-16): constructor arguments admit data types
  only, so a negative occurrence cannot be written at all —
  `enum Bad { Mk(fn(Bad) int) }` is a parse error, not a checked rejection.
  Positivity is therefore structural for the shipped grammar, and an explicit
  kernel check is required before higher-order constructor arguments are
  admitted; do not relax the grammar without it. Since 2026-11-16 that rejection
  carries a dedicated diagnostic (`enum payload fields must be data types`)
  rather than failing through the generic payload parse.
- **Index refinement:** matching a value at index `i` refines the constructor's
  indices to `i`. A branch whose constructor cannot produce `i` is impossible
  and is not required for exhaustiveness. *Implemented* for GADT constructor
  evidence; impossible arms may be omitted.
- **Motive coverage:** every reachable constructor has a branch, and each branch
  body has the motive's type at that constructor's indices.

Recursors (`nat_rect`, `vec_rect`, …) are derived from `Match` in libraries, not
added to the kernel.

## Equality

*Proposed as a primitive; today equality is reasoning inside refinement
predicates.* The kernel provides `Eq A x y`, `refl : Eq A x x`, and the `J`
eliminator (equivalently `subst`), with the computation rule

    J A x (λ y e. P y e) p x refl  ≡  p

`refl` is available only when both sides are convertible. There is no
proof-irrelevance rule, no univalence, and no function extensionality in the
core: those are opt-in axioms, which the risk register below tracks.

Measured today (2026-11-16): there is no kernel identity type, `refl`
constructor, or eliminator. What exists is the proof-obligation path —
`proof<2 == 2>` with `prove(2 == 2)` discharges a closed equation and erases,
while `prove(2 == 3)` fails with `proof obligation failed`. A surface
`type Eq<a, x, y> = proof<x == y>` does not parse, because the proposition's
`>` closes the alias argument list, so `Eq` has no surface spelling yet and
transport is unimplemented. A parser-level `Eq<A, x, y>` sugar was attempted and
reverted: `prove` induces `proof<binary:==(int:2,int:2)>`, and writing that
canonical proposition by hand as an annotation is also rejected, so the gap is
proposition normalization in HM, not parser rewriting. The Phase 3 items stay
open.

## Totality

The kernel checks only terminating terms; a non-terminating term in a type or
proof would make conversion diverge and the system unsound.

- Structural recursion on a constructor argument is accepted, with the
  decreasing argument explicit.
- `decreases(expr)` accepts a well-founded measure when structural descent
  cannot be shown, and is *implemented* for the `@total` surface form.
- A term that cannot be shown total in a type/proof position is rejected, not
  assumed total. Runtime (partial) code is a different judgement and may
  diverge.

Type-level functions are additionally bounded today (terminating, bounded
recursion); unbounded type-level computation is proposed, gated on the same
totality check.

## Erasure and the ABI model

Proof-only content must not reach the object file. Erasure runs after kernel
checking and before lowering:

| Content | Erased | Notes |
| --- | --- | --- |
| `proof<P>` values and parameters | yes | Implemented; no ABI slot, no runtime body. |
| Refinement predicates on a value | yes | The predicate is a compile-time obligation; the value keeps its base representation. |
| `Fin<N>` bound with a literal `N` | yes | Implemented; values keep the integer ABI and `N` is checked at construction. |
| `Fin<N>` bound with a symbolic `N` | no | `checked_fin(value, bound)` keeps its mandatory runtime test; a symbolic bound is not a proved fact. |
| Type parameters (`Type A`) | yes | No runtime slot for the type argument. |
| Indices that do not affect layout | yes | `Vec<A, n>`-style length evidence. |
| Indices used for layout or bounds checks | no | Must be materialised; the layout rule below applies. |
| `@total` proof functions reached only from proofs | yes | Emitted only if reflected or called from computational code. |

Representation rules:

1. A dependent parameter whose value is needed at runtime has one ABI slot, in
   declaration order, exactly as if it had been written as an ordinary
   parameter. Erasing a proof parameter never shifts a runtime argument.
2. A value whose type carries an index used for layout (sized arrays, checked
   buffers) must have that index available at runtime; if it cannot be
   recovered, the program is rejected rather than guessed.
3. `extern`/FFI boundaries accept only fully erased, runtime-representable
   signatures. A proof or type parameter at an FFI boundary is an error.
4. Erasure must not change observable behaviour: the pass may remove arguments
   and bodies, never reorder or coalesce runtime parameters.

## Soundness risk register

Every accepted axiom or escape hatch is listed here; all are off by default and
none may become implicit.

| Risk | Current stance | Status |
| --- | --- | --- |
| `Type : Type` | Rejected by the level rule: `Type n : Type (n + 1)`. | Implemented (2026-11-16) for literal levels; inference and cumulativity open. |
| Non-terminating term in a type/proof | Rejected by the totality check. | Implemented (`@total`, `decreases`); kernel-level check proposed. |
| Negative inductive occurrence | Not expressible in the shipped `enum` forms; kernel positivity check proposed. | Partial |
| Refinement predicates authored by users | Accepted as obligations the checker must discharge; unknown obligations fail closed, never become evidence. | Implemented |
| Bounded solver / `prove` / `assert_compile*` | Automation only: the Presburger/Fourier-Motzkin solver and compile-time evaluator may discharge an obligation; they may never manufacture evidence in the kernel. `lemma` declares a named proposition, `prove` requests its discharge. | Implemented |
| GADT constructor evidence | Carried by the constructor expression; not forgeable, not a cast. | Implemented |
| Function extensionality, univalence, quotients | Not in the kernel; explicit axioms only. | Proposed (axiom modules) |
| `def Type` universe-as-value (`def Type T = Type`) | Rejected at parse time by the universe level rule instead of escaping to runtime symbol resolution. | Implemented (2026-11-16) |
| `def Type` self-alias (`def Type T = T`) | Rejected at parse time with `type value cannot refer to itself`, matching the `type` declaration cycle check. | Implemented (2026-11-16) |
| `unsafe`/trusted escape flags | Must be explicit, tainting, and off by default. | Not shipped; guardrail in the roadmap |

Rule: an untrusted producer (tactic, macro, external solver, user prover) can
only affect the result by emitting a term the kernel checks. If it cannot, the
obligation is reported as unsolved.

## Level terms (design, not shipped)

Status: checkpoints 1 and 2 landed; the level item stays open. A universe level
is a `ny_level_t` term, and the two structural-equality paths compare it through
`ny_level_equal`. Literal levels keep their old meaning exactly (the `lang/types`,
`universe-`, and `kernel-` suites pass with no fixture edits), and a closed
constraint `Type max(n, m)` now folds to the larger literal in both type and
expression position. Variables, constraint solving, and level polymorphism are
still unimplemented, so nothing here closes a roadmap item.

A level is a literal index today: `Type` is level 0, `Type n` is level `n`,
stored as a `ny_level_t` literal term in `ny_type_t` (built by
`ny_type_type_univ_level` in `semantics/typeinfer.c`, still clamped at 1000) and
compared by `ny_level_equal` at the two structural-equality paths for universe
types in that same file. `Type max(n, m)` over literals is folded by the parser
to `Type max(n, m)`'s larger operand, so a closed constraint never reaches the
checker as a term; that is enough for the shipped literal hierarchy and
cumulativity, and no more.

The term language to implement:

```
L ::= n            ; literal level
    | u            ; level variable, bound by a definition or binder
    | max(L, L)    ; least upper bound
```

Rules the implementation must satisfy, in the order they can be checked:

1. **Literal parity.** Every existing program keeps its meaning: `Type n` is the
   literal `n`, `Type` is `0`, and the clamps and diagnostics are unchanged.
   This checkpoint is proven by the existing `lang/types` and `universe-`
   suites passing without edits.
2. **Normalization.** `max` is associative, commutative, and idempotent, folds
   over literals (`max(3, 5) = 5`), and commutes with the successor rule:
   `Type L : Type (L + 1)` with `max(L1, L2) + 1 = max(L1 + 1, L2 + 1)`.
3. **Cumulativity over terms.** `Type L1` inhabits `Type L2` when `L1 < L2`
   after normalization; equality of universe values compares normalized terms,
   not strings.
4. **Variables and constraints.** A level variable is introduced by a binder,
   collected as `L1 <= L2` constraints at use sites, and solved by substitution;
   an unsolved variable at the end of elaboration is an error, never a default
   of 0.
5. **Fail closed.** A level that would need a variable where the current literal
   rule applies stays an error until variables are implemented; nothing is
   silently widened.

Planned fixtures, each of which must pass before the roadmap item moves:
`level-max-normalization` (`Type max(3, 5)` behaves as `Type 5`),
`level-cumulativity-terms` (`Type max(1, 2)` inhabits `Type 3`),
`level-polymorphic-id` (one definition usable at two levels), and negatives for
an unsolved level variable and for a level that is too low.

## Rejection cases

These must fail, and are the negative fixtures for the phases above:

```ny
; Type : Type — rejected by the level rule: `Type n` inhabits
; `Type (n + 1)`, so `Type 0` cannot live in `Type 0` even
; though it is a first-class value.

; non-structural, non-measured recursion in a proof position
@total
fn loop(int n) int { return loop(n + 1) }

; negative occurrence in a constructor argument
enum Bad { Mk(fn(Bad) int) }        ; kernel positivity check (proposed)

; unproved refinement membership
refine Positive = int where (v) { v > 0 }
def Positive x = 0                   ; rejected: 0 > 0 is false

; out-of-range index
def Fin<4> i = checked_fin(4, 4)     ; rejected: requires 0 <= i < 4

; proof parameter at an FFI boundary
extern fn take(proof<5 > 0>, int) int   ; rejected: erased parameter
```

Positive counterparts live next to their fixtures:

- [kernel-safe-index](../../etc/tests/lang/types/dependent/kernel/kernel-safe-index.nshape)
  covers bounded indexing with a literal `Fin<N>` and with a runtime
  `checked_fin` bound.
- [kernel-verified-algorithm](../../etc/tests/lang/types/dependent/kernel/kernel-verified-algorithm.nshape)
  eliminates an indexed enum into a bounded lookup.

The two closed universe holes are guarded by negative fixtures:
[kernel-universe-value-negative](../../etc/tests/lang/types/dependent/kernel/kernel-universe-value-negative.nshape)
and
[kernel-type-self-alias-negative](../../etc/tests/lang/types/dependent/kernel/kernel-type-self-alias-negative.nshape).

A `Vec`-head example and an equality-transport example are deliberately absent:
`Vec` needs the Phase 2 inductive families and transport needs the Phase 3 `Eq`
primitive, so neither can be a passing fixture yet. This page only fixes the
judgements they must satisfy.
