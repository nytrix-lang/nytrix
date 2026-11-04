<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Phase checklist for dependent types, GADTs, and proof extensions."} -->
# Dependent types and GADT support

The language model, proposed syntax, and current implementation boundary are
explained in [Dependent types and indexed data](../spec/dependent.md). Keep this
list to open work and acceptance criteria.

- [ ] **Phase 2 — Pi types (in progress):** dependent return and later-parameter
  types, call-site substitution, and dependent `Fin` bounds. Verify
  `etc/tests/lang/types/dependent/pi/*.nshape` on all four routes, including
  executable and invalid-bound rejection controls.
- [ ] **Phase 3 — Sigma types:** dependent-pair declaration/construction,
  elimination, and projection. Add and pass the four `sigma/*.nshape` fixtures;
  preserve ordinary pair and tuple behavior.
- [ ] **Phase 4 — GADTs:** indexed enum declarations, constructor evidence,
  match-arm refinement, and method dispatch. Add and pass `gadt/*.nshape`; keep
  ordinary ADT, unit-enum, and tagged-`any` behavior intact.
- [ ] **Phase 5 — Type-level features:** first-class `Type`, terminating
  type-level functions, and runtime-index checks or proofs. Add and pass the
  `first-class-type/*.nshape` and `runtime-index/*.nshape` fixtures; reject
  non-terminating definitions and preserve compile-time `Fin<N>` behavior.
- [ ] **Phase 6 — Proof extensions:** retain facts only across justified
  non-aliasing mutation, map caller proofs to callee preconditions, and keep
  optional qualifier synthesis off by default. Add positive and negative
  `proof-mutation/*.nshape` cases; preserve existing proof regressions.
- [ ] **Phase 7 — Refinement composition:** route constant `def` initializers
  through nested-membership evaluation. The other nested-membership paths pass;
  see `etc/tests/lang/types/refinements/refinement-intersection.nshape`.

**Gate:** Complete phases in order. Every new positive and negative fixture
must pass `--native --run`, LLVM JIT with MCJIT and ORC, and `--native-only`;
retain rejection of malformed, executable, out-of-range, and non-terminating
forms. Do not mark a phase complete from parsing or a subset run alone.
