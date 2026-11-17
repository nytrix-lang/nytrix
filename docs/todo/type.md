<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":3,"summary":"Proposal and TODO for a minimal, coherent, multiparadigm type/proof core that can express theorem prover–like systems."} -->
# Type system and proof power

Proposal to evolve Nytrix into a top-tier, **multiparadigm** dependently typed
language/proof system without syntax or kernel bloat. It should be practical as
an everyday language (imperative, procedural, functional, systems, compile-time
metaprogramming, FFI, and more styles used together) while also being powerful
enough that **people can program theorem prover–like stuff** in it: build
proof assistants, tactic/automation engines, or verified tooling by composing
the core language, libraries, and compile-time/meta facilities — not by
requiring a separate proof language.

Goal: maximum expressive power from a **small, coherent, trusted core**; the
rest (tactics, custom solvers, proof DSLs, search/automation, alternative
front ends, interactive tooling) is derived in ordinary Nytrix code and
libraries when possible, with elaboration/compilation as untrusted producers
around the kernel.

Current baseline: [Types](../spec/types.md), [Dependent types](../spec/dependent.md),
[Functions/contracts](../spec/functions.md), [Traits](../spec/traits.md),
[Comptime](../spec/comptime.md). Refinements, erased `proof<P>`, `prove`/lemmas,
`Fin<N>`, bounded Pi/Sigma/GADT/indexing, first-class `Type` values, bounded
type-level functions, and fail-closed bounded proving exist today; this document
defines the path to a general, sound, total-proof-capable core that can host
theorem prover–like systems.

## Principles (suckless / low bloat)

- **Multiparadigm by design.** The same language supports imperative/procedural,
  functional, dataflow/collection, metaprogramming/compile-time, and
  proof-oriented styles interchangeably; the kernel stays small, surface forms
  stay familiar, and power comes from composition, not a new “proof-only”
  dialect.
- **Small trusted kernel.** One checker for core terms (universes, binders,
  inductives, equality, computation). Parsers, elaborators, tactics, formatters,
  and codegen are untrusted producers/consumers around it (so users can program
  their own theorem prover–like layers on top).
- **Few orthogonal primitives.** Prefer one general mechanism per concept;
  extend existing forms (`enum`, `fn`, `match`, `Type`, generics, `comptime`)
  before adding keywords.
- **Proofs are programs (Curry-Howard).** A proof is a term whose type is a
  proposition. `proof<P>`, `prove`, `lemma`, refinements remain as ergonomic and
  erasure-aware surface forms elaborating to core terms.
- **Derive, do not duplicate.** Pairs, nats, vectors, logical connectives,
  decidability helpers, common typeclasses/patterns, tactics/automation, and
  proof-assistant features (term views, environments, unification/search,
  proof objects) live in libraries or elaboration/meta passes when expressible
  from primitives and the existing compile-time/meta story.
- **Programmable meta/proof tooling.** The language must make it feasible to
  implement theorem prover–like functionality in Nytrix itself (or tightly
  integrated libraries): represent terms/goals/hyps, rewrite/unify/search,
  produce checked core terms/certificates, and expose editor/REPL hooks via
  stable compiler/tooling APIs — all while the kernel remains the sole soundness
  gate.
- **Explicit boundaries.** Runtime/ABI behavior, erasure, totality, partial/FFI
  effects, and “trusted” vs “checked” automation are explicit. Proof-only data
  must erase cleanly.
- **Fail closed.** Unknown obligations, non-termination in the kernel, or
  unverifiable boundaries are rejected or require an explicit escape hatch; no
  unsound assumption by default.

## Target capability (definition of "full power")

The kernel + standard derivations should be able to express, in principle:

- **Multiparadigm programs:** everyday code in imperative, procedural,
  functional, data-oriented, systems/FFI, and compile-time styles composed with
  proof-carrying and total fragments in one language.
- **Dependent type theory core:** arbitrary dependent functions (Pi), dependent
  pairs/records (Sigma), indexed/recursive families, and general inductive (and
  inductive-inductive-style, via a single inductive scheme) constructions.
- **Proofs and equality:** propositional equality, transport/substitution,
  congruence, and proof-relevant reasoning; extensional principles only when
  added as explicit axioms/modules.
- **Sound foundations:** a predicative universe hierarchy (and optional
  proof-irrelevant/erased sort) sufficient to avoid paradox and
  `Type : Type` unsoundness; enforced totality for trusted/type-level terms.
- **Verified programs:** specifications, invariants, and executable code sharing
  one term language with erasure of proof content.
- **Theorem prover–like systems programmable in Nytrix:** enough primitive and
  meta power to implement, as libraries/tools in Nytrix (or tightly integrated
  with it), proof assistants, tactic engines, custom elaborators/solvers,
  proof search, proof-term construction/checking, and verified or
  certificate-producing automation — the kernel remains the only soundness gate.

Not required in-kernel (derivable, library, or external tooling): a specific
tactic language, heavy automation, Mathlib-scale libraries, a particular IDE
protocol, or a privileged/built-in proof assistant UI. These must be
*implementable and integrable* without kernel hacks.

## Minimal kernel primitives

| Primitive | Role | Reuse/extend |
| --- | --- | --- |
| `Type u` universes | Stratification, generic programming over types | Extend first-class `Type` with level/index |
| Pi / dependent binders | Dependent functions, quantification, parametricity | Generalize existing fn/param/generic binding |
| Inductive families | Data, propositions, indexed structures, recursion principles | Generalize `enum` (+ constructors/indices/`where`) |
| Identity/equality | Equations, rewrite/transport, propositional reasoning | Small primitive (`Eq`/`refl`/eliminator or equivalent) |
| Totality discipline | Trusted computation in types/proofs | Structural recursion + `decreases`/well-founded accessibility |

Conservatively optional kernel additions (only if required for coherence/erasure):
`Prop`/erased sort, definitional proof irrelevance rules, or quotient axioms as
explicit trusted extensions — never implicit.

## Syntax sketches (proposed, illustrative)

Sketches prioritize familiarity with current Nytrix and minimal new forms.
Exact spelling is negotiable; coherence and kernel mapping are not.

### Universes and implicit/general binders

```ny
; explicit universe levels (default level inferred/elaborated where safe)
def id(Type 0 A, A x) A { x }

; implicit binder (elaborated/inferred; kernel receives explicit arg)
fn id{Type A}(A x) A { x }
fn map{Type A, Type B, int n}(fn(A) B f, Vec<A, n> xs) Vec<B, n> { ... }

; named dependent binder in types/signatures (unifies Pi/params)
fn replicate(Type A, int n, A x) Vec<A, n> { ... }
```

### Generalized inductive families (extend `enum`)

```ny
enum Nat {
   Zero,
   Succ(Nat n)
}

enum Vec(Type A, Nat n) {
   Nil                                      where n is Nat.Zero,
   Cons{Nat k}(A head, Vec<A, k> tail)      where n is Nat.Succ(k)
}

enum Eq(Type A, A x, A y) {
   Refl(A x)                                where y is x
}

; propositions as types (proofs are values/terms)
enum And(Type P, Type Q) { Pair(P left, Q right) }
enum False { }
type Not(Type P) = fn(P) False
```

### Dependent match (motive/as-patterns, refinement)

```ny
fn head{Type A, Nat n}(Vec<A, Nat.Succ(n)> xs) A {
   match xs {
      Vec.Cons(h, _) -> h
      ; Vec.Nil branch impossible by index; checker must accept exclusion
   }
}

fn subst{Type A, A x, A y}(fn(A) Type P, Eq<A, x, y> e, P x px) P y {
   match e {
      Eq.Refl(_) -> px
   }
}

; explicit motive when inference is insufficient (spelling TBD, minimal)
match xs as (v) -> Motive(v, n) { ... }
```

### Total/proof functions and recursion

```ny
@total
fn add(Nat a, Nat b) Nat
   decreases(a)
{
   match a {
      Nat.Zero    -> b
      Nat.Succ(k) -> Nat.Succ(add(k, b))
   }
}

; well-founded recursion for non-structural cases
@total
fn gcd(Nat a, Nat b) Nat
   decreases(measure(a, b))
{ ... }
```

### Equality, computation, and proof ergonomics (surface over kernel)

```ny
; kernel-level: Eq/Refl/eliminator; surface sugar may include rewrite/calc
def proof two : Eq<Nat, add(one, one), two> = Eq.Refl(two)

fn cong{Type A, Type B, A x, A y}(fn(A) B f, Eq<A, x, y> e) Eq<B, f x, f y> {
   subst(fn(A z) Eq<B, f x, f z>, e, Eq.Refl(f x))
}

; keep current ergonomic forms, elaborated to terms/proofs
static_assert(n < len(xs), "index")
def Fin<n> i = checked_fin(raw, n)
fn lemma add_zero(Nat n) { Eq<Nat, add(n, Nat.Zero), n> }
def proof p = prove(add_zero(x))
```

### Holes and elaboration (tooling/surface, not kernel syntax bloat)

```ny
; placeholders solved by elaboration/unification; rejected if unsolved
def v = map(_, xs)          ; infer implicit/type arg
fn f(Vec<A, n> xs) _ { ... } ; infer result type where possible

; optional named hole for diagnostics/IDE (no kernel semantics)
def t : T = ?goal
```

## Derived features (library / elaboration, not kernel primitives)

Implement on top of the primitives above; keep out of the trusted core:

| Feature | Derived from |
| --- | --- |
| Sigma/`exists` records, tuples-as-records, unit/void/bool | Inductives (+ params/indices) |
| Nat/int encodings, `Fin`, `Vec`, sized containers | Inductive families + indices |
| Logical connectives, decidability, classical axioms (opt-in) | Props/types-as-terms + modules/axioms |
| Generic maps/folds/eq/show, proof helpers | `comptime`, traits/impl, generics, libs |
| Tactics, automation, simp/rewrite/decide, search, SMT hooks | Untrusted elaboration/meta passes producing kernel terms/certificates |
| Typeclass-style search/resolution | Traits/impl + implicit resolution/elaboration |
| Term/goal/context representation, quoting/reflection, proof-state APIs | `comptime`/compiler APIs with typed, checked boundaries (for provers in Nytrix) |
| Proof assistant–like layers (REPL/IDE hooks, custom solvers, DSLs) | Stable tool APIs + libraries over kernel checking; no privileged kernel path |

## Wiring and coherence

1. **Parse** source forms (existing + generalized enum/binders/match motives/holes).
2. **Elaborate**: resolve implicits, insert coercions/proofs where specified,
   desugar surface syntax (`prove`, `lemma`, refinements, `exists` sugar) to
   explicit core terms; record erasure metadata. User/library automation and
   prover components run here (or as tools) and may be written in Nytrix.
3. **Kernel check**: typecheck core terms against universe/positivity/termination
   and conversion rules; this is the only soundness-critical path and the
   authority behind all proof/tactic outputs.
4. **Lower/codegen**: erase proof-irrelevant/erased arguments and types per
   ABI/runtime model; total proof-only functions need no runtime body unless
   reflected; partial/runtime code follows existing effect/FFI/ownership rules.
5. **Interop/tooling**: FFI, runtime values, and `--mode`/backend routes cannot
   assume proof terms exist at runtime; dependent indices that affect layout/ABI
   must have explicit, checked representation rules. Compiler/tool APIs expose
   terms/diagnostics/goals for REPL/editor/prover tooling without bypassing
   kernel checking.

Required coherence invariants:
- Conversion is consistent with evaluation/termination rules used by the kernel.
- Inductive positivity/recursor or pattern-matching rules prevent paradoxes.
- Universe levels/cumulativity (if any) are sound and predictable.
- Erasure does not change runtime-observable behavior of executable code.
- Untrusted automation/meta/prover code cannot forge kernel acceptance except by
  producing terms the kernel checks (or explicit, tainting trusted axioms).

## TODO / roadmap

Audit (2026-11-16): 22 items open and none closeable today — every item's
acceptance is only partly met. Solved sub-parts were removed from the item text
and recorded in [resolved decisions](../spec/kernel.md#resolved-decisions);
what remains is stated as a measured blocker rather than a plan.

### Phase 0 — Design and invariants

**Complete (2026-11-16).** The kernel specification, the erasure and ABI model,
and the soundness risk register are in
[Kernel, erasure, and soundness](../spec/kernel.md), with the
[resolved decisions](../spec/kernel.md#resolved-decisions) that back them. The
spec cross-review is done too: `kernel.md` was read end-to-end against
`dependent.md` and `types.md` and the disagreements were fixed (proof-value
syntax, the `Fin` erasure split, the universe-rejection wording, and the solver's
name). Resolved items are removed from this roadmap.

### Phase 1 — Elaboration and universes

**Complete (2026-11-17).** Level variables (`{level u}`, `Type u`,
`Type max(u, n)`, per-call level instantiation and checks), static per-call
instantiation of type parameters including nested types, explicit type binders
(`fn id(Type A, A x) A`), implicit value binders (`{int n}`) solved from
argument types, and anonymous holes `_` for result types, type arguments, and
implicit binders are recorded in
[resolved decisions](../spec/kernel.md#resolved-decisions).

### Phase 2 — Inductive families and pattern matching

**Complete (2026-11-17).** Strict positivity for higher-order constructor
arguments, value-indexed families (`Vec<A, n>` with `where n is k + 1`),
explicit and per-arm dependent motives, exhaustiveness with impossible arms,
and structural recursion with derived recursors are recorded in
[resolved decisions](../spec/kernel.md#resolved-decisions). Fin and equality
are primitives (`Fin<N>`, `proof<a == b>` with the Phase 3 rules).

### Phase 3 — Equality and proof kernel

**Complete (2026-11-17).** The identity type `proof<a == b>` with `refl`,
`J`/`transport` (on proofs and, along indices, on values), `eq_sym`,
`eq_trans`, `eq_cong`, and the bridge from `prove`, `requires`, and lemma
instances are recorded in [Equality](../spec/kernel.md#equality) and the
[resolved decisions](../spec/kernel.md#resolved-decisions); the bounded solver
is reported as automation.

### Phase 4 — Erasure, codegen, and interop

**Complete (2026-11-17).** The erasure pass, its representation rules
(including FFI rejection of proofs, type values, and implicit binders), and the
route matrix (native `-O0`/`-O3`, LLVM ORC, Wasm) are recorded in
[Erasure and the ABI model](../spec/kernel.md#erasure-and-the-abi-model).

### Phase 5 — Derivations, meta-programmability, and theorem-prover layers (non-kernel)
- [ ] **Core proof/data library.** Derive standard structures (Sigma, Eq lemmas,
  Nat/Vec/Fin utilities, decidable predicates) from primitives.
- [ ] **Programmable meta/term APIs.** Stable, typed APIs for representing and
  constructing core/surface terms, contexts, goals, diagnostics, and checked
  artifacts from Nytrix code (extending `comptime`/compiler/reflection APIs as
  needed), so prover components can be written in Nytrix.
- [ ] **Elaboration automation and user tactics.** Rewrite/simplification/
  decision procedures, search, and optional SMT/Z3 integration; support
  user-defined tactics/solvers/DSLs that emit kernel-checkable terms or
  certificates (never bypass the kernel).
- [ ] **Tooling integration.** REPL/LSP hooks for goals, holes, and proof
  state; enable external or in-tree proof-assistant UIs built on the same APIs.
- [ ] **Ergonomics.** Better hole solving, notations/helpers, and documentation
  examples — all reducible to explicit terms.

Acceptance: nontrivial verified examples (e.g. safe indexing, verified small
algorithms, algebraic/data-structure properties) and a minimal theorem
prover–like prototype (e.g. goal state + user tactics/rewriting/search producing
kernel-checked proofs) built with Nytrix libraries/tools, with no kernel
modification and no soundness bypass; automation never bypasses kernel checking.

Open in this phase (2026-11-16): term/goal/context APIs, user tactics or a
traversable goal state, REPL/LSP hooks for goals, a core proof library, and an
in-tree prover prototype. The proof surface that exists is the built-in
refinement/`prove` path, not a library derived from core primitives. It does
carry the verified-small-algorithm shape for obligations the solver closes —
literal initializers and refinement identities
([verified-small-algorithm](../../etc/tests/lang/types/proofs/verified-small-algorithm.nshape)) —
while bounded arithmetic stays an error, `refinement obligation is not proved`
([negative](../../etc/tests/lang/errors/type/verified-small-algorithm-negative.nshape)),
which is solver coverage rather than a soundness hole.

## Non-goals and guardrails

- No unsound escape hatches enabled by default; any axiom/unsafe mode must be
  explicit and tainting.
- No duplicate "second kernel" in tactics, macros, or backends; user-written
  provers/tactics are untrusted until their output is kernel-checked.
- No promise of automatic proof for arbitrary propositions; full power means
  *expressible, programmable, and checkable*, with automation as best-effort.
- No privileged proof-assistant mode required in-kernel: prover UX and tactics
  are implementable as ordinary libraries/tools over stable APIs.
- Preserve Nytrix's multiparadigm, systems identity: imperative/runtime code
  coexists with total proof code, with predictable erasure and explicit
  FFI/runtime costs, and compatibility with existing modules/tooling.

## Execution brief

- [ ] Implement in dependency order (Phases 0→5); do not ship surface sugar that
  lacks a kernel/elaboration mapping.
- [ ] For each milestone: land spec text, kernel/elaborator/tool-API change,
  positive and negative fixtures, and route/erasure checks where applicable.
- [ ] Prefer extending `enum`/`fn`/`match`/`Type`/`comptime` over new keywords;
  justify any new syntax by a primitive it cannot replace.
- [ ] Keep the theorem prover–like path *programmable in Nytrix*: expose stable
  term/goal/check APIs and prove them with at least one in-tree prototype prover
  library/tool (no kernel special-casing).
- [ ] Record resolved decisions and remaining limits in this file and the linked
  spec pages; close items only with passing acceptance evidence on the current
  tree.
