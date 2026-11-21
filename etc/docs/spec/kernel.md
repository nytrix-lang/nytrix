<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"spec","order":220,"summary":"Trusted kernel rules, erasure and ABI model, and the soundness risk register for dependent and proof-carrying Nytrix."} -->
# Kernel, erasure, and soundness

This page fixes the rules the checker is allowed to trust. Everything else —
parsers, elaborators, tactics, codegen, user-written provers — is an untrusted
producer that must hand the kernel a term it checks. The user-facing
behaviour is described in [Dependent types](dependent.md) and
[Types](types.md); the libraries and tools built on these rules are in
[Prover layer and tooling](#prover-layer-and-tooling).

> **Status:** This page records the target core and the boundaries of what ships
> today. Sections marked *implemented* describe behaviour on the current tree;
> sections marked *proposed* are normative design that is not wired yet. Where
> the two differ, the implemented text wins.

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
- [Guardrails](#guardrails)
- [Prover layer and tooling](#prover-layer-and-tooling)
- [Rejection cases](#rejection-cases)

## Resolved decisions

Rules that are implemented and fixture-backed; anything not in this table, or
marked *proposed* below, is not implemented.

| Rule | Behaviour | Evidence |
| --- | --- | --- |
| Kernel specification | Core terms, typing and conversion, universes, inductive rules, equality, and totality are fixed by this page. | This page |
| Erasure and ABI model | Proof-only content erases; argument slots, layout indices, and FFI boundaries are specified. | [Erasure and the ABI model](#erasure-and-the-abi-model) |
| Soundness risk register | Every accepted axiom or escape is listed and off by default. | [Register](#soundness-risk-register) |
| Spec cross-review | Read end-to-end against [Dependent types](dependent.md) and [Types](types.md); disagreements fixed for proof-value syntax, the `Fin` erasure split, the universe-rejection wording, and solver naming. | This page (2026-11-16) |
| Literal universe levels | `Type u` parses and canonicalizes; a bare `Type` is level 0; `Type n` never inhabits `Type n`. | [universe-levels](../../../etc/tests/lang/types/dependent/universes/universe-levels.nshape), [negative](../../../etc/tests/lang/types/dependent/universes/universe-type-type-negative.nshape) |
| Cumulativity | `Type n` inhabits every `Type m` with `m > n`, not only `m = n + 1`. | [universe-levels](../../../etc/tests/lang/types/dependent/universes/universe-levels.nshape) |
| Universe-as-value rejection | `def Type T = Type` fails at parse time by the level rule instead of escaping to runtime symbols. | [kernel-universe-value-negative](../../../etc/tests/lang/types/dependent/kernel/kernel-universe-value-negative.nshape) |
| Type-value self-alias rejection | `def Type T = T` fails with `type value cannot refer to itself`, like `type Bad<T> = Bad<T>`. | [kernel-type-self-alias-negative](../../../etc/tests/lang/types/dependent/kernel/kernel-type-self-alias-negative.nshape) |
| Type-value validation | `def Type` bindings validate through the same normalization/cycle path as `type` declarations. | the two `def Type` fixtures above |
| Universe values | `Type` and `Type n` are first-class in expression position and evaluate to the erased universe tag (`"Type"`, `"Type n"`), matching the `str` runtime representation, instead of failing symbol resolution. Levels stay static indices, not runtime data. | [universe-values](../../../etc/tests/lang/types/dependent/universes/universe-values.nshape) |
| Closed level constraints | `Type max(n, m)` folds to the larger literal at parse time in both type and expression position (`max(3, 5)` behaves as `Type 5`), including idempotence; a malformed constraint is an error, never a silent level 0. | [level-max](../../../etc/tests/lang/types/dependent/universes/level-max.nshape) |
| Function type parameters | `fn id<A>(A x) A` declares erased type parameters: annotations naming one take the dynamic runtime path, but checking is static and per call. Each parameter is one variable of the signature, instantiated at every call, also through nested types (`fn first<A>(list<A> xs) A` makes `first([1, 2])` an `int`), and a parameter fixed by one argument and contradicted by another is rejected with `type parameter 'A' instantiated inconsistently`. | [fn-type-param](../../../etc/tests/lang/types/generics/fn-type-param.nshape), [binders-and-holes](../../../etc/tests/lang/types/generics/binders-and-holes.nshape) |
| Implicit binders | `fn id{Type A}(A x) A` (or `{A}`) is the implicit spelling of `<A>`. `{int n}` is an implicit value binder: a leading parameter the checker fills at each call from the other arguments' types (`fn bound{int n}(Fin<n> i)` called on a `Fin<5>` passes 5); it must occur in some parameter type. The argument types fix it to a literal, or to a binder of the calling function (`vto_list(xs)` inside `fn f{int n}(Vec<int, n> xs)` passes `n`); an index written `n + k` is a successor and never solves to a negative `n`; any other call is rejected with `cannot infer implicit binder`. Result types are instantiated with the solved values (`vcons(7, vnil())` is a `Vec<int, 1>`). | [fn-type-param](../../../etc/tests/lang/types/generics/fn-type-param.nshape), [binders-and-holes](../../../etc/tests/lang/types/generics/binders-and-holes.nshape) |
| Explicit binders (Pi over types) | A parameter typed by a universe (`fn id(Type A, A x) A`) binds `A` for the parameters and result after it. A written type as its argument instantiates `A` and is passed as the erased type value, its name; inside the body the binder is that `str`. | [binders-and-holes](../../../etc/tests/lang/types/generics/binders-and-holes.nshape) |
| Anonymous holes | `_` is filled by elaboration in three places: a result type (`fn twice(int x) _`), a type argument (`id(_, 3)` takes `A` from the other arguments), and an implicit binder written out (`bound(_, i)`). A type-argument hole nothing determines is rejected with `unsolved hole '_'`; `_` in any other expression position is a typed hole (see [Prover layer and tooling](#prover-layer-and-tooling)). | [binders-and-holes](../../../etc/tests/lang/types/generics/binders-and-holes.nshape), negatives in [eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape) |
| Level variables | `{level u}` declares a universe level variable on a function; `Type u` and `Type max(u, n)` may then appear in its signature (`max(u, u)` is `u`). Each call instantiates the levels: a written type is level 0 and a universe value `Type n` passed as a type is level n + 1; a level variable takes the argument's level, a literal level must be at least it. An undeclared level is a parse error. | [binders-and-holes](../../../etc/tests/lang/types/generics/binders-and-holes.nshape), negatives in [eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape) |
| Typed holes and goals | `?name` and an unbound `_` report the goal type and the locals in scope; the record is in `errors.v1.json` and relayed by `ny-lsp`. | [Prover layer and tooling](#prover-layer-and-tooling) |
| Prover library | `core.prover`: terms, goals, contexts, checked structural rules, search, and user tactics whose output the checker must entail; `qed` re-checks every certificate. | [prover-library](../../../etc/tests/lang/types/proofs/prover-library.nshape) |
| Indexed data library | `core.indexed`: `Vec<int, n>` with length-typed constructors, total head/tail, and `Fin<n>` helpers. | [indexed-library](../../../etc/tests/lang/types/dependent/value-index/indexed-library.nshape) |
| Named holes | `?name` in expression position is a typed placeholder: HM infers its goal type and reports `unsolved hole '?name' : T` (or `unconstrained`), so a program with a hole never lowers or runs. Holes carry no kernel semantics. | [named-hole](../../../etc/tests/lang/errors/type/named-hole.nshape) |
| Parameterised enums | Uniform and multi-parameter enums, plus type-level aliases over them, compile and run. | [enum-uniform-params](../../../etc/tests/lang/types/dependent/enum-params/enum-uniform-params.nshape) |
| Positive recursion | Constructor arguments may recurse as ordinary data and eliminate by `match`. | [enum-positive-recursion](../../../etc/tests/lang/types/dependent/enum-recursion/enum-positive-recursion.nshape) |
| Strict positivity | Constructor arguments may be function types; the family may occur only in strictly positive positions, never in a function domain at any depth. The check runs over the whole program once every enum is known, so an occurrence behind a type-level definition (`type F = fn(Bad) int`, also applied), in a function-valued result (`fn(int) fn(Bad) int`), in a mutual group (the family set is the strongly connected component of enums that mention each other), or through another enum's parameter (`W(T<Bad>)` where `T<A>` uses `A` in a domain) is rejected with `enum payload breaks strict positivity`. A parameter used only in results may be instantiated with the family (`Node(Stream<Tree>)`). | [enum-strict-positivity](../../../etc/tests/lang/types/dependent/enum-recursion/enum-strict-positivity.nshape), [negatives](../../../etc/tests/lang/errors/type/enum-payload-function-negative.nshape) |
| Value-indexed families | Enum parameters may be value indices with linear evidence (`Cons(A head, Vec<A, k> tail) where n is k + 1`); index terms in type arguments are linear over at most one name and have one canonical spelling (`n+1`, `3`). Construction binds the evidence's names from the argument types and computes the index (`Cons(5, Nil)` is a `Vec<int, 1>`); a match solves them from the scrutinee, so payloads get their own indices (the tail of a `Vec<int, 2>` is a `Vec<int, 1>`). A literal index rules out a constructor whose literal evidence differs; a symbolic index keeps every constructor possible, because index names are not known to be natural. | [vec-length-index](../../../etc/tests/lang/types/dependent/value-index/vec-length-index.nshape), negatives in [eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape) |
| Dependent match motives | `match e as T { ... }` checks every arm against `T`; `match e as (v) -> proof<P(v)>` instantiates the motive at the scrutinee for the match and at the pattern for each literal or nullary-constructor arm, where evidence at either the pattern or the scrutinee is accepted (they are equal on that arm). Non-exhaustive enum matches are errors, naming the missing constructors; constructors that cannot produce the scrutinee's index are rejected and need no arm. | [match-motive](../../../etc/tests/lang/types/dependent/gadt/match-motive.nshape), negatives in [eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape) |
| Structural recursion and recursors | `@total` with `decreases(x)` accepts recursive calls on constructor payloads of `x`, including calls nested in a constructor of the result, and refuses any other recursive argument. Recursors are ordinary total functions: one that receives its step as a callback under an open effect row (`@total @effects(e) fn nat_rec(Nat n, int z, fnptr step)`) is total relative to that callback, while its own body stays effect-checked and its recursion structural. A measure over names alone survives calls that may write memory, which cannot rebind a parameter. | [total-structural-recursion](../../../etc/tests/lang/types/proofs/total-structural-recursion.nshape), negatives in [eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape) |
| Bounded indexing | Literal `Fin<N>` indexing is checked at construction; a symbolic bound keeps the runtime `checked_fin` test. | [kernel-safe-index](../../../etc/tests/lang/types/dependent/kernel/kernel-safe-index.nshape) |
| Indexed elimination | Constructors refine the index; impossible arms are not demanded. | [kernel-verified-algorithm](../../../etc/tests/lang/types/dependent/kernel/kernel-verified-algorithm.nshape), GADT fixtures in [Dependent types](dependent.md) |
| Closed-equation evidence | `proof<a == b>` with `prove` discharges convertible closed equations and erases. | [eq-closed-refl](../../../etc/tests/lang/types/proofs/eq-closed-refl.nshape) |
| Symbolic reflexivity | `prove(t == t)` (also `<=`, `>=`) discharges for any pure, non-floating term, including parameters unknown at compile time; the sides are compared structurally on the canonical proposition, so this is the `refl` rule, not evaluation. Calls, member access, indexing, and float operands fail closed (NaN is not reflexive), and `prove(x == y)` for distinct names stays an error. | [eq-refl-symbolic](../../../etc/tests/lang/types/proofs/eq-refl-symbolic.nshape), [negative](../../../etc/tests/lang/errors/type/eq-refl-symbolic-negative.nshape) |
| Proof-parameter instantiation | A proof parameter whose proposition mentions earlier value parameters is instantiated with integer-literal arguments and re-normalized before the witness is compared (`fn need(int x, int y, proof<x + 1 == y> p)` accepts `need(2, 3, prove(2 + 1 == 3))`); the native call check substitutes every literal binder at once. A false instance is a proposition mismatch, and a non-literal argument still has to match by name. | same fixtures |
| `@proves` from preconditions | On the NYIR route a post-condition is checked against the range of each returned expression, where an `int` parameter that the body never reassigns takes the bounds stated by its `requires` conjunction (`requires n > 0` proves `result > 1` for `return n + 1`); a reassigned parameter or a weaker bound is rejected. | [proves-requires-range](../../../etc/tests/lang/types/proofs/proves-requires-range.nshape), [negative](../../../etc/tests/lang/errors/type/proves-requires-range-negative.nshape) |
| Fail-closed proofs | A false equation and an unproved bounded-arithmetic refinement stay errors. | [eq negative](../../../etc/tests/lang/errors/type/eq-closed-refl-negative.nshape), [refinement negative](../../../etc/tests/lang/errors/type/verified-small-algorithm-negative.nshape) |
| Verified small algorithm | Literal initializers and refinement-to-refinement identities discharge without a hand-written proof. | [verified-small-algorithm](../../../etc/tests/lang/types/proofs/verified-small-algorithm.nshape) |
| Undefined-name diagnostic | A name with no local, function, module export, runtime symbol, constant, or alloc builtin reports a located `[E2001] error: undefined symbol 'x'` with a hint at the point resolution becomes final, instead of surfacing only as a NYIR build failure. | [lexer-invalid-byte-stops](../../../etc/tests/lang/errors/parser/lexer-invalid-byte-stops.nshape), [undefined-call](../../../etc/tests/lang/errors/type/undefined-call.nshape) |
| Equality rules | `eq_sym`, `eq_trans`, `eq_cong`, and `transport` (`J`) compute their result proposition from canonical proof types and erase; `transport` also moves a value along an equation between indices of its type. | [eq-kernel-rules](../../../etc/tests/lang/types/proofs/eq-kernel-rules.nshape), [eq-transport-index](../../../etc/tests/lang/types/proofs/eq-transport-index.nshape), negatives in [eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape) |
| Legacy proof bridge | `prove(P)` is `proof<P>`; `requires` bounds are evidence; `prove(lemma(args))` is a proof of the lemma's conclusion instance; the bounded solver's verdicts are reported as automation (`"authority":"automation"`). | [eq-transport-index](../../../etc/tests/lang/types/proofs/eq-transport-index.nshape), [proves-requires-range](../../../etc/tests/lang/types/proofs/proves-requires-range.nshape) |
| Call arity | Omitted trailing arguments are partial application, so only excess arguments are an arity error: `too many arguments: expected at most N, got M`. | [excess-arg](../../../etc/tests/lang/errors/desugar/excess-arg.nshape), [excess-call-arity](../../../etc/tests/lang/errors/invariant/excess-call-arity.nshape), [Partial application](functions.md#partial-application) |

## Core terms

The kernel sees only these forms. Surface syntax is desugared into them.

| Term | Meaning | Surface today |
| --- | --- | --- |
| `Type u` | Universe at level `u`. | `Type`, `Type n`, `Type u` under `{level u}`, `Fin<N>`, refinements |
| `Pi (x : A). B` | Dependent function; `x` may occur in `B`. | `fn` parameters, dependent returns (*implemented*) |
| `Lam (x : A). t` | Function term. | `fn` bodies |
| `App t a` | Application; substitutes `a` for `x` in `B`. | calls |
| `Ind D (params) (indices)` | Inductive family with constructors. | `enum`, indexed `enum` (*params/indices implemented for GADT evidence*) |
| `Con_i args` | Constructor application. | variant construction |
| `Match (motive) scrutinee branches` | Elimination; every branch is checked against the motive. | `match` |
| `Eq A x y` / `refl` | Identity type and its constructor. | `proof<a == b>`, `prove(t == t)`, and the rules in [Equality](#equality) |
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
`m > n`, not merely `m = n + 1`. Level variables are declared with
`{level u}` and instantiated per call; see
[resolved decisions](#resolved-decisions).

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
  rejected. *Implemented* as a whole-program check (`semantics/positivity.c`)
  over the payload types as written, with type-level definitions expanded,
  mutual groups taken as one family, and enum parameters followed into their
  own constructors.
- **Index refinement:** matching a value at index `i` refines the constructor's
  indices to `i`. A branch whose constructor cannot produce `i` is impossible
  and is not required for exhaustiveness. *Implemented* for GADT constructor
  evidence; impossible arms may be omitted.
- **Motive coverage:** every reachable constructor has a branch, and each branch
  body has the motive's type at that constructor's indices.

Recursors (`nat_rect`, `vec_rect`, …) are derived from `Match` in libraries, not
added to the kernel.

## Equality

The identity type is `proof<a == b>`; `refl` is `prove(t == t)`, accepted for
any pure term structurally. The eliminator and its derived rules are checked
on canonical propositions by the type checker and erase at runtime:

| Rule | Type |
| --- | --- |
| `eq_sym(p)` | `proof<a == b>` to `proof<b == a>` |
| `eq_trans(p, q)` | `proof<a == b>`, `proof<b == c>` to `proof<a == c>` (any shared side) |
| `eq_cong(p, f)` | `proof<a == b>` to `proof<f(a) == f(b)>` |
| `transport(p, q)` | `J`/`subst`: a proof of `P(a)` becomes a proof of `P(b)` |
| `transport(p, v)` | indexed transport: a value of type `T<..., a, ...>` is the same value at `T<..., b, ...>` |

The computation rule holds by construction: transporting along `refl`
rewrites a term to itself. There is no proof irrelevance, univalence, or
function extensionality in the core; those would be opt-in axioms, tracked by
the risk register below.

Evidence from the rest of the proof surface feeds the same rules:
`prove(P)` is `proof<P>`, a `requires` bound is usable evidence inside the body,
and a lemma instance `prove(lemma(args))` is a proof of the lemma's conclusion
at those arguments (its premise is checked by `prove`). The bounded solver that
decides `prove` of arithmetic is automation, not the kernel: every obligation it
decides is reported with `"authority":"automation"` in the semantic artifact's
proof-obligation records.

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

Proof-only content must not reach the object file. Erasure is a pass of its
own (`src/code/lang/ny/semantics/erasure.c`) that runs after kernel checking and
before any backend lowers the program, so the native, LLVM, and Wasm routes see
the same runtime signatures. It removes a `proof<P>` parameter, and the matching
argument at every call, when the function is declared at the top level of the
main file, every use of it is a direct positional call, and the body only
forwards the witness to other erased slots. An erased argument must be an
identifier, a literal, or a `prove(...)` witness; a witness is hoisted to an
expression statement before its statement, so lowering still discharges it, and
is then discarded. Anything else (a function used as a value, a witness fed to
an equality rule, a witness in a match arm or lambda) keeps the parameter,
which is always correct, only not minimal. `NYTRIX_TRACE_ERASURE=1` reports each
erased function; `NYTRIX_NO_PROOF_ERASURE=1` disables the pass for comparison.
Proof:
[proof-erasure](../../../etc/tests/lang/types/proofs/proof-erasure.nshape)
(results on native `-O0`/`-O3`, LLVM ORC, and Wasm; the NYIR call passes only
the runtime argument).

| Content | Erased | Notes |
| --- | --- | --- |
| `proof<P>` parameters | yes, by the pass above | No ABI slot and no argument; a parameter the pass cannot prove irrelevant keeps a slot holding the witness digest. |
| `proof<P>` values | yes | A witness is a compile-time digest constant; no runtime check runs. |
| Refinement predicates on a value | yes | The predicate is a compile-time obligation; the value keeps its base representation. |
| `Fin<N>` bound with a literal `N` | yes | Implemented; values keep the integer ABI and `N` is checked at construction. |
| `Fin<N>` bound with a symbolic `N` | no | `checked_fin(value, bound)` keeps its mandatory runtime test; a symbolic bound is not a proved fact. |
| Type parameters (`<A>`, `{Type A}`) | yes | No runtime slot for the type argument. |
| Explicit type binders (`fn f(Type A, ...)`) | no | The binder is a value parameter holding the type's name (`str`). |
| Implicit value binders (`{int n}`) | no | Filled at the call and passed like any parameter (rule 1). |
| Universe levels (`{level u}`) | yes | Static only; never a parameter. |
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
   signatures. A proof, type value, or implicit binder at an FFI boundary is
   rejected (`extern 'f' takes an erased parameter`); negatives in
   [eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape).
4. Erasure must not change observable behaviour: the pass may remove arguments
   and bodies, never reorder or coalesce runtime parameters.

## Soundness risk register

Every accepted axiom or escape hatch is listed here; all are off by default and
none may become implicit.

| Risk | Current stance | Status |
| --- | --- | --- |
| `Type : Type` | Rejected by the level rule: `Type n : Type (n + 1)`. | Implemented (2026-11-16); cumulativity and per-call level variables (2026-11-17). |
| Non-terminating term in a type/proof | Rejected by the totality check. | Implemented (`@total`, `decreases`, structural recursion, relative totality for recursors). |
| Negative inductive occurrence | Rejected by the whole-program strict positivity check (aliases, mutual families, nested parameters). | Implemented (2026-11-17) |
| Refinement predicates authored by users | Accepted as obligations the checker must discharge; unknown obligations fail closed, never become evidence. | Implemented |
| Bounded solver / `prove` / `assert_compile*` | Automation only: the Presburger/Fourier-Motzkin solver and compile-time evaluator may discharge an obligation; they may never manufacture evidence in the kernel. `lemma` declares a named proposition, `prove` requests its discharge. | Implemented |
| GADT constructor evidence | Carried by the constructor expression; not forgeable, not a cast. | Implemented |
| Function extensionality, univalence, quotients | Not in the kernel; explicit axioms only. | Proposed (axiom modules) |
| `def Type` universe-as-value (`def Type T = Type`) | Rejected at parse time by the universe level rule instead of escaping to runtime symbol resolution. | Implemented (2026-11-16) |
| `def Type` self-alias (`def Type T = T`) | Rejected at parse time with `type value cannot refer to itself`, matching the `type` declaration cycle check. | Implemented (2026-11-16) |
| `unsafe`/trusted escape flags | Must be explicit, tainting, and off by default. | None shipped; guardrail below |

Rule: an untrusted producer (tactic, macro, external solver, user prover) can
only affect the result by emitting a term the kernel checks. If it cannot, the
obligation is reported as unsolved.

## Guardrails

- No unsound escape hatch is enabled by default; any axiom or unsafe mode must
  be explicit and tainting.
- No second kernel in tactics, macros, or backends: user-written provers and
  tactics are untrusted until their output is checked here.
- No promise of automatic proof for arbitrary propositions: the goal is that
  proofs are expressible, programmable, and checkable; automation is
  best-effort.
- No privileged proof-assistant mode in the kernel: prover UX and tactics are
  ordinary libraries and tools over stable APIs.
- Nytrix stays multiparadigm and systems-oriented: runtime code coexists with
  total proof code, with predictable erasure and explicit FFI and runtime
  costs.

## Prover layer and tooling

Everything here is ordinary Nytrix code or compiler output over the kernel;
nothing in it can make the kernel accept a proof it would reject.

**Goals and holes.** `?name`, and an unbound `_` in expression position, are
typed holes. The checker reports each one as an error that carries the goal and
the locals in scope, a proposition goal in source spelling:
`unsolved hole '?evidence' : proof<0 < y> | in scope: y : int`. The same record
is machine-readable in `errors.v1.json` (`ny --stop-after=hm --collect-errors`,
code `hm-unsolved-hole`, goal in `expected`), and `ny-lsp` relays it as an
information diagnostic (`NYGOAL`) at the hole, so editors show goals while
typing (`NYTRIX_LSP_GOALS=0` turns this off). The REPL prints the same
diagnostic. Fixtures: `typed_hole_*` in
[eq-kernel-negative](../../../etc/tests/lang/errors/type/eq-kernel-negative.nshape).

**Terms, goals, and tactics (`core.prover`).** A small prover written in
Nytrix over `core.proof`. `term` reads a proposition from source spelling
(`x + 1 > 0 && (y == x -> y >= 0)`, `->` is `!a || b`), `show` and `canon`
print it back. A goal is hypotheses, a target, and variable ranges; a state is
the open goals, the certificates that closed the others, and a log, printed by
`state_text`. Goals close only through `decide`, which keeps a certificate for
`hyps -> target` only when `core.proof` proposes it and the independent C
checker re-derives it. The structural rules are `split_conj`, `intro`, `cases`,
`subst` (Leibniz, dropping the variable's range), and `split_range` (which
substitutes a one-value range); `auto` searches with them. User tactics are any
`fn(any goal) any` run with `by`: the tactic sees a deep copy of the goal, and
its subgoals replace the goal only when the checker proves that they entail
the goal's text taken before the tactic ran, so a wrong or mutating tactic
fails and never proves. `qed` re-checks every certificate. Fixture:
[prover-library](../../../etc/tests/lang/types/proofs/prover-library.nshape).

**Indexed data (`core.indexed`).** `Vec<int, n>` with `vnil`, `vcons`
(result length computed from the argument's), `vlength` (the length read from
the type through an implicit binder), total `vhead`/`vtail` on
`Vec<int, n + 1>` (applying them to an empty vector is a type error),
`vto_list`, `vsum`, and `fin_bound`/`fin_value` for `Fin<n>`. Fixture:
[indexed-library](../../../etc/tests/lang/types/dependent/value-index/indexed-library.nshape).
The equality lemmas are the kernel rules (`eq_sym`, `eq_trans`, `eq_cong`,
`transport`); dependent pairs are `exists(T n) U`; decidable propositions are
decided by `core.prover.decide`.

Limits, each a reported failure rather than a wrong answer:

- The checker decides bounded integer obligations; an induction step over an
  unbounded variable is proposed by the solver but not re-derived, so it stays
  unproved.
- `core.indexed` elements are `int`: an `any`-typed payload loses its tag
  through the module boundary.
- A module's enum constructors are not reachable as `mod.Enum.Ctor` from
  outside the module; libraries export constructor functions instead.
- The Wasm route does not yet check module-defined indexed types
  (`core.indexed`); user-defined families run there (the dependent fixtures
  include `--wasm`).

Compiler defects found while writing these libraries (worked around in them,
still open): a local named `s` that is assigned a dict reads as `str`
(`mut s = {}` then `s` has type `str`); an empty list stored in a record can
read back as an integer across a module boundary; `return d.set(k, v)` can hand
back a raw handle, so the libraries bind the result before returning it; an int
match payload appended to a list inside a stdlib module is stored untagged
(`out.append(h + 0)` stores a fresh int).

## Level terms (design, not shipped)

Status: level variables shipped at the signature level (see
[resolved decisions](#resolved-decisions)): `{level u}` binders are
instantiated per call by the HM checker, which checks `Type u` and
`Type max(u, n)` binders against the levels of the arguments. The `ny_level_t`
term solver below remains a design for levels inside type-level computation;
nothing user-visible depends on it.

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

The level-polymorphic behaviour this design targets (one definition usable
at two levels, a level that is too low, an undeclared level) is covered at the
signature level by
[binders-and-holes](../../../etc/tests/lang/types/generics/binders-and-holes.nshape)
and its negatives.

## Rejection cases

These must fail:

```ny
; Type : Type — rejected by the level rule: `Type n` inhabits
; `Type (n + 1)`, so `Type 0` cannot live in `Type 0` even
; though it is a first-class value.

; non-structural, non-measured recursion in a proof position
@total
fn loop(int n) int { return loop(n + 1) }

; negative occurrence in a constructor argument
enum Bad { Mk(fn(Bad) int) }        ; rejected: strict positivity

; unproved refinement membership
refine Positive = int where (v) { v > 0 }
def Positive x = 0                   ; rejected: 0 > 0 is false

; out-of-range index
def Fin<4> i = checked_fin(4, 4)     ; rejected: requires 0 <= i < 4

; proof parameter at an FFI boundary
extern fn take(proof<5 > 0>, int) int   ; rejected: erased parameter
```

Positive counterparts live next to their fixtures:

- [kernel-safe-index](../../../etc/tests/lang/types/dependent/kernel/kernel-safe-index.nshape)
  covers bounded indexing with a literal `Fin<N>` and with a runtime
  `checked_fin` bound.
- [kernel-verified-algorithm](../../../etc/tests/lang/types/dependent/kernel/kernel-verified-algorithm.nshape)
  eliminates an indexed enum into a bounded lookup.

The two closed universe holes are guarded by negative fixtures:
[kernel-universe-value-negative](../../../etc/tests/lang/types/dependent/kernel/kernel-universe-value-negative.nshape)
and
[kernel-type-self-alias-negative](../../../etc/tests/lang/types/dependent/kernel/kernel-type-self-alias-negative.nshape).

The `Vec` and equality-transport examples are
[vec-length-index](../../../etc/tests/lang/types/dependent/value-index/vec-length-index.nshape)
and [eq-transport-index](../../../etc/tests/lang/types/proofs/eq-transport-index.nshape).
