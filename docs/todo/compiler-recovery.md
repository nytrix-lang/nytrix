<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":3,"summary":"Confirmed compiler failures and their acceptance criteria."} -->
# Compiler recovery (P0)

The completed 2,494-case run passed 2,482 cases and failed 12. Keep only the
confirmed unresolved failures below; closed observations are recorded as
passed and must not be reopened without new evidence.
Paths are relative to `etc/tests/`; reproduce each in its real backend and
execution configuration; never rerun the entire suite to rediscover them, and never
skip, weaken, delete, fake, or silently bypass a failing test. Keep language
semantics valid while fixing: `;` comments, proof soundness, ABI behavior,
and optimizer correctness.

### Confirmed failures

- [ ] **UI engine replay:** `code/probes/ui/ui-smoke.nshape:ui_replay_engine`.
  Full-run artifact says engine/JIT and engine/VM each exited 1 after 25.850 s;
  fixture assertion is `engine native, JIT, and VM replay pass`. Earlier cold
  evidence reported a JIT startup segfault before frame markers. The completed
  run's JIT and VM logs now localize the failure before replay: NYIR verification
  rejects `_draw_frame` return instruction 403 in block `L19` because its
  producer is `f64` but the inferred return ABI is non-float/unknown. This is
  the same return-class family as `vector-f64.nshape`; fix shared return ABI
  inference/merge, then re-run the replay. Native route log has only dynamic
  type fallback warnings in the captured prefix, so inspect its final status
  separately. Require all routes to exit zero and produce identical expected
  state/frame streams. Preserve per-route deadlines and marker comparisons.

- [ ] **ADT payload constructor/matcher ABI:**
  `lang/types/aggregates/adt.nshape` and `lang/types/aggregates/types.nshape`
  both report `Circle payload match: expected 16, got 0`; the arm is reached
  but the payload binding is wrong. Constructor stores are in
  `src/code/lang/lower/call.h` near `rt_adt_alloc`; tag matching and payload
  binding are in `src/code/lang/lower/stmt.h` (`nyr_lower_match_pattern`,
  `nyr_bind_result_pattern`). Compare actual object pointer/layout and field
  offsets across constructor, allocator, matcher, heap-tag/GC handling, and
  method receiver ABI. **Hypothesis:** producer and consumer disagree whether
  returned pointer addresses field zero or header/tag. Confirm with NYIR/ASM
  before changing offsets. Preserve mixed empty/payload enums and backend ABI.
  Add one-/multi-field, qualified/unqualified, guarded-binding, and method
  tests; verify both fixtures across declared routes.

- [ ] **Enum method ABI and dynamic unit-enum patterns:**
  `lang/types/traits/enum-method-abi.nshape` fails all four declared routes at
  `payload enum method: expected 25, got 0`; prior replay also observed dynamic
  `Color.Red` matching return `nil` instead of `"red"`. Keep unit-enum and
  payload-zero symptoms distinct until proven to share a cause. Audit enum
  boxing/unboxing/member lookup in `src/code/lang/lower/expr.h`, comparison and
  binding in `src/code/lang/lower/stmt.h`, and bound/unbound method receiver
  ABI. Unit enums use raw discriminants while `any` is tagged: preserve this
  distinction; do not treat arbitrary odd integers as enum tags or change the
  public ABI. Add zero/nonzero enum-through-`any` matches and payload/empty
  method calls; verify all four routes and output parity.

- [ ] **Enum constructor patterns misclassified by proof-purity analysis:**
  `lang/types/proofs/portable-contracts.nshape:portable_proof_contracts`
  fails on all four strict-types routes with `enum constructor patterns are
  syntax, not impure function calls`. Inspect match-pattern traversal/call
  classification in `src/code/lang/types/funcpurity.c`, especially
  `ny_purity_match_pattern` and expression purity checks. Constructor-shaped
  patterns are syntax and payload names are bindings, not calls. Preserve
  rejection of genuinely effectful calls and proof soundness. Add a pure
  qualified/unqualified enum-pattern regression plus a negative effectful-call
  control; verify all four variants.

- [ ] **`parse_code_typed` declaration-sequence failure:**
  `code/ast/ast-code-queries.nshape` fails native and native-only routes at
  `parse_code_typed returns declaration sequences`. In `lib/compiler/ast.ny`,
  `parse_code` validates snapshot schema/version/nodes and
  `_code_sequence_category`, then passes the list to `code_decl`. Inspect the
  exact `parse_ast_snapshot` JSON contract in `src/code/lang/parse/ast.c` and
  typed-code sequence serialization/parsing in `lib/compiler/ast.ny`. Likely
  mismatch is parsed record shape/category metadata versus validator/wrapper
  assumptions. Capture the snapshot for this input and repair the contract at
  its source; do not weaken declaration-only validation. Retain rejection of
  executable/malformed source; rerun both routes uncached.

- [ ] **Ownership allowance error not propagated by every backend:**
  `lang/errors/ownership/allowance-trailing-defer-reassignment-upper-bound.nshape`
  expects compile failure for two uses of `x` before deferred reassignment
  under allowance `[0,1]`. Full-run replay: native `--run` and native-only
  return 0 despite printing E2001; LLVM MCJIT and ORC return 1 at emit. In
  `src/code/lang/types/ownership.c`, `stmt_ownership_diag` sets `cg->had_error`
  in strict mode and path analysis reports observed 2 > 1. Trace why this
  state is not propagated through native NYIR emit/run and the compile-failure
  harness. Make all strict routes fail compilation with the diagnostic; do
  not suppress it or convert this to a runtime assertion. Add valid allowance
  and defer/reassignment ordering controls; verify every matrix variant and
  required message.
