<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"project","order":1,"summary":"Open compiler, backend, standard-library, and UI work."} -->
# Open work

Use [Contributing](CONTRIBUTE.md) for commands and checks. Keep only unresolved
acceptance here; completed work belongs in the [changelog](CHANGELOG.md), and
historical results belong in [Recorded measurements](learn/measurements.md).

## Compiler recovery (P0)

The completed 2,494-case run passed 2,482 cases and failed 12. Keep only the
confirmed unresolved failures below; closed observations are recorded as
passed and must not be reopened without new evidence.
Paths are relative to `etc/tests/`; reproduce each in its real backend and
execution configuration; never rerun the entire suite to rediscover them, and never
skip, weaken, delete, fake, or silently bypass a failing test. Keep language
semantics valid while fixing: `;` comments, proof soundness, ABI behavior,
and optimizer correctness.

### Confirmed failures and fix directions

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

- [ ] **Boolean accepted as a range index (shared collection/index defect):**
  `lang/values/collections.nshape` and `lang/values/index.nshape` both fail at
  `range[true]` expected to panic; preceding index assertions pass. Likely
  cause: `lib/reflect.ny` range indexing accepts `__is_int` (tagged-integer
  low-bit check), which can overlap boolean immediates. Reject booleans before
  integer conversion. Audit direct `rt_range_index_read_raw` lowering in
  `src/code/lang/lower/expr.h` and `stmt.h` for bypasses. Preserve int/bigint
  keys; add true/false tests through reflection and native/JIT/VM paths; rerun
  both fixtures uncached.

- [ ] **Scalar-leaf recovery through try/catch:**
  `lang/execution/errors.nshape` fails `try/catch flatten should recover scalar
  leaves`; trace enters recursive `flatten_try`, while earlier flatten
  assertions pass. Inspect guarded recursive `mapcat`, try result/caught-value
  normalization in `src/code/lang/lower/stmt.h` (`nyr_try_normalize_value`,
  `NY_S_TRY`), and collection/type predicates in `lib/reflect.ny`. Determine
  whether scalar rejection raises/catches the wrong payload or handler value
  is lost/coerced at the try join. Preserve panic propagation and scalar
  values. Add a minimal scalar leaf plus nested-list regression; verify the
  fixture's JIT mode and compare native/VM where supported.

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

- [ ] **F64 loop result has inconsistent NYIR return ABI:**
  `code/intermediate/opt/vector-f64.nshape` fails its x86-64 `-O2`
  native-result-oracle route at emit/verify. Captured `emit.log` identifies
  `rt_main`, return instruction 76 at block `L3`: instruction 59 uses
  `i64-raw`, while that exit returns `f64`. The fixture accumulates f64 in a
  loop and returns after loop control flow. Inspect return ABI inference and
  loop-exit/phi lowering in `src/code/lang/lower`, plus the native verifier.
  Ensure all exits agree on the function's scalar return class and loop-carried
  f64 does not inherit an integer/control-flow class. Do not relax verification
  or coerce the numeric result. Add zero/one/many-iteration and conditional
  f64-loop tests; verify oracle results at `-O0` and `-O2`.

## Performance

Latest recorded checksums passed, but full performance acceptance is still
open. Use the recorded measurements as history, not a current baseline; keep
workloads, routes, and gates unchanged.

Earlier completed work is already in this checkout; do not restore removed
TODOs or overwrite newer code with saved agent snapshots. In particular,
NYC benchmark pinning and `dict-set` stability are documented in
[Recorded measurements](learn/measurements.md).

- [ ] **Matched runtime performance:** improve NYR and NYC against matched
  LLVM and host C without changing sources, flags, timed regions, checksums,
  samples, or gates. The latest 73-fixture run reported matching workload
  checksums; `binary` missed its NYR/LLVM ceiling (5.29x vs 5.00x), failing
  the algorithms and overall geomean gates. `append-scaling` passed (1.00x);
  `gcbench` was separately flagged as a budget regression. Preserve
  compile-time and code-size checks. See
  [Recorded measurements](learn/measurements.md#latest-full-benchmark-run).

- [ ] **`dce` benchmark gap (not the DCE pass):** the NYIR dead-code-elimination
  pass and its optimizer regressions are already implemented; do not redo them.
  This remaining performance gap is that LLVM folds the 500-step `x == i`
  chain while NYR does not. Locate where the relation is lost across
  if-conversion/SELECTs, then add runtime-seeded edge/effect regressions and
  fold only with path-valid proof. The latest full run measured 18.47x NYR/LLVM;
  see the recorded benchmark table for the run-specific values.
- [ ] **Scalar FPR residency:** reduce measured hot spills/scratch traffic in
  `spectral`, `mandelbrot`, `page-rank`, `linpack`, and `dgemm`; test pressure
  and reject stable cohort regressions.
- [ ] **Boundary coalescing:** isolate arithmetic/SELECT ties from
  rematerialization and quotient encoding; compare `binary`, `json-parser`,
  and affected neighboring workloads.
- [ ] **Independent vectorization:** complete legality, edge-case differential
  coverage, pressure, and profitability for the opt-in outer vectorizer before
  enabling it by default.
- [ ] **Full-gate acceptance:** after candidate work stops, run serial matched
  NYR/LLVM/C/NYC comparisons; retain checksums, medians/spread, tool identity,
  compile cost, and code size. A subset is not acceptance. Follow
  [Performance](learn/performance.md).

## NYC Linux kernel acceptance

- [ ] Produce a clean Linux 7.3 x86-64 `bzImage` and modules build from
  unmodified sources, compiling every C translation unit with NYC and using
  GNU `as`. Require both artifacts, no C fallback, configured-object coverage,
  and reviewed compiler/linker/`objtool` warnings.

The latest broad build exited 2 without either final artifact. Remaining
blockers span x86 inline assembly (string/exception-table, SHA, SIMD), HDA/PCI
initializers, Zstd/decompression, networking and aggregate typing, and i915
parsing/ABI/assembly. Targeted advances, regression results, and warning
locations are in the [changelog](CHANGELOG.md); support boundaries are in
[Native code generation](spec/native.md). Re-run a complete configured-object
inventory before treating this blocker list as exhaustive.
