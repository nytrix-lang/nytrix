<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":4,"summary":"Open matched-performance, vectorization, and benchmark-gate work."} -->
# Performance

Latest recorded checksums passed, but full performance acceptance is still
open. Use the recorded measurements as history, not a current baseline; keep
workloads, routes, and gates unchanged.

Earlier completed work is already in this checkout; do not restore removed
TODOs or overwrite newer code with saved agent snapshots. In particular,
NYC benchmark pinning and `dict-set` stability are documented in
[Recorded measurements](../learn/measurements.md).

- [ ] **Matched runtime performance:** improve NYR and NYC against matched
  LLVM and host C without changing sources, flags, timed regions, checksums,
  samples, or gates. The latest 73-fixture run reported matching workload
  checksums; `binary` missed its NYR/LLVM ceiling (5.29x vs 5.00x), failing
  the algorithms and overall geomean gates. `append-scaling` passed (1.00x);
  `gcbench` was separately flagged as a budget regression. Preserve
  compile-time and code-size checks. See
  [Recorded measurements](../learn/measurements.md#latest-full-benchmark-run).
  The `binary` gap is structural: LLVM inlines `branch_mix`, unrolls its
  depth loop, and runs the `i` loop four lanes wide (`vpaddq`/`vblendvpd`),
  3.7M instructions vs NYR's 20.9M. NYR inlines the call too, which leaves
  the outer-vectorizer shape (uniform inner trip count `d`); closing the gate
  needs the vectorization item below.

- [ ] **FPR/vector residency in `linpack` and `dgemm`:** `spectral`,
  `mandelbrot`, and `page-rank` are done (see
  [measurements](../learn/measurements.md#performance-todo-batch-2026-11-04)).
  `dgemm` (176 µs vs 47 µs LLVM) no longer re-stores its invariant
  broadcasts, but each inner iteration still stores and reloads `C[cidx]` four
  times because NYIR cannot prove the `B` and `C` buffers disjoint. Keeping the
  sum in a register needs that no-alias fact for distinct `f64buf_new`
  objects. `linpack` (745 µs vs 240 µs) has the same shape in its row update
  (`A[r,c] -= m * A[k,c]`): the `A` base is reloaded from its home three times
  per iteration and the address arithmetic is not strength-reduced.
- [ ] **Independent vectorization:** complete legality, edge-case differential
  coverage, pressure, and profitability for the opt-in outer vectorizer before
  enabling it by default. On `binary` it never analyses the inlined `i` loop
  in `rt_main`, and `NYTRIX_OUTER_VECTORIZE=1` slows the fixture from 1.52 to
  1.90 ms through other widened loops. The header comment's remaining blocker,
  vector loop carries that stay register-resident, still applies.
- [ ] **Full-gate acceptance:** after candidate work stops, run serial matched
  NYR/LLVM/C/NYC comparisons; retain checksums, medians/spread, tool identity,
  compile cost, and code size. A subset is not acceptance. Follow
  [Performance](../learn/performance.md).
