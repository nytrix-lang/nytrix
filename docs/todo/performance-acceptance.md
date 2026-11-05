<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":4,"summary":"Open matched-performance, vectorization, and benchmark-gate work."} -->
# Performance

Latest recorded checksums passed, but full performance acceptance is still
open. Use the recorded measurements as history, not a current baseline; keep
workloads, routes, and gates unchanged.

Earlier completed work is already in this checkout; do not restore removed
TODOs or overwrite newer code with saved agent snapshots. In particular,
NYC benchmark pinning and `dict-set` stability are documented in
[Recorded measurements](../learn/measurements.md).

- [ ] **`binary` NYR/LLVM gate (5.00x):** the only remaining gate failure in
  the 2026-11-05 full run (1.46 ms vs 265 us LLVM, 5.50x). Scalar work so far:
  MUL by 5/9 stays one LEA, and `loop_sub_iv` turns `d - i` into a
  decrementing IV (19 -> 18 instructions, 1.60 -> 1.46 ms). The loop is
  throughput-bound. Two more scalar candidates are worth about 5% each: coalesce
  the select's second arm into the loop-carried destination register (drops
  `mov r14, rdi` before `cmove`), and branch on the decrementing IV's flags
  instead of `cmp i, d`. The structural fix is the next item.

- [ ] **Outer vectorization for `binary`:** `loop_guard_hoist` now moves the
  invariant `rounds / d` divide-by-zero guard out of the `i` loop header, so
  the opt-in outer vectorizer (`NYTRIX_OUTER_VECTORIZE=1`) widens the loop 4x
  with the correct checksum. It is still slower (2.1 ms) because the vector
  recurrences `acc` and `x` stay on their stack homes: the allocator's carry
  proof rejects PHIs whose latch reads the old value before the copy. A
  prototype that kept the PHI in its own color and emitted a real latch move
  was correct but slower (3.9 ms: its evictions spilled other values) and was
  reverted. Carry recurrences without displacing other vector values, then
  finish legality, differential coverage, pressure, and profitability before
  enabling the vectorizer by default.

- [ ] **FPR/vector residency in `linpack` and `dgemm`:** `spectral`,
  `mandelbrot`, and `page-rank` are done (see
  [measurements](../learn/measurements.md#performance-todo-batch-2026-11-04)).
  `dgemm` is 160 us (169 us before `mem_dse`; LLVM 27 us): typed-buffer
  allocations now get distinct alias classes, and `mem_dse` drops three of the
  four `C[cidx]` stores per iteration. The remaining overhead is allocator
  traffic: invariant broadcasts reloaded from the stack, dead spills of
  address temporaries, and base pointers reloaded each iteration. `linpack`
  (742 us vs 250 us) needs vectorization of its row update once rows `r != k`
  are proven disjoint.

- [ ] **Full-gate acceptance:** after candidate work stops, run serial matched
  NYR/LLVM/C/NYC comparisons with no concurrent builds; retain checksums,
  medians/spread, tool identity, compile cost, and code size. A subset is not
  acceptance. Follow [Performance](../learn/performance.md).
