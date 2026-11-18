<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":4,"summary":"Open matched-performance, vectorization, and benchmark-gate work."} -->
# Performance

Latest recorded checksums passed, but full performance acceptance is still
open. Use the recorded measurements as history, not a current baseline; keep
workloads, routes, and gates unchanged.

Earlier completed work is already in this checkout; do not restore removed
TODOs or overwrite newer code with saved agent snapshots. In particular,
NYC benchmark pinning and `dict-set` stability are documented in
[Recorded measurements](../learn/measurements.md).

- [ ] **`binary` scalar fallback:** the gate now passes with outer
  vectorization on by default (919 us vs 266 us LLVM, 3.45x, 2026-11-05 full
  run). With `NYTRIX_OUTER_VECTORIZE=0` the scalar loop is still 1.45 ms
  (5.45x). Remaining scalar ideas, about 5% each: branch on the decrementing
  IV's flags instead of `cmp i, d`, and drop the second scalar IV.

- [ ] **FPR/vector residency in `linpack` and `dgemm`:** `spectral`,
  `mandelbrot`, and `page-rank` are done (see
  [measurements](../learn/measurements.md#performance-todo-batch-2026-11-04)).
  `dgemm` is 154 us (LLVM 26 us; 5.97x in the 2026-11-05 targeted run). Three
  of its four invariant broadcasts now stay in registers and are built with
  register-form `vbroadcastsd`. What remains is scalar GPR work in the inner
  loop: `r*M` and `k*M` are recomputed every iteration because LICM refuses to
  hoist address arithmetic (`licm_memory_address_component`), dead address
  temporaries are stored to homes, and base pointers are reloaded. LLVM's loop
  is 13 instructions; ours is about 30. `linpack` (739 us vs 240 us) needs
  vectorization of its row update once rows `r != k` are proven disjoint.

- [ ] **Full-gate acceptance:** the 2026-11-05 06:25 serial run (3+1, workers
  1) passed every per-class gate, but other sessions were loading the host
  (load average about 14), and `collatz`, `euler9`, `knight`, and `queen`
  FAIL because their working-tree C sources print no `elapsed_ns`. That
  happens with the baseline compiler too. Fix those fixtures, then run
  serial matched NYR/LLVM/C/NYC comparisons with no concurrent builds;
  retain checksums, medians/spread, tool identity, compile cost, and code
  size. A subset is not acceptance. Follow
  [Performance](../learn/performance.md).
