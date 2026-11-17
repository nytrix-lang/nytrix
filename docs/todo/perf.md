<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":4,"summary":"Open NYR and compiler build-performance work with acceptance criteria."} -->
# Performance work

An item closes only when its acceptance check has passed on the current tree.
Historical measurements are investigation snapshots, not gate evidence.

Working rules:

- Compare generated code by instructions executed, then read the assembly.
- A change counts only if checksums still match and the suite stays green.
- Reproduce one benchmark or fixture at a time before changing anything.

## NYR

- [ ] **At most 1.2x of C on every backend.** No valid, comparable benchmark
  above 1.2x on NYR, NYC, or LLVM. Accept: a fresh complete `./make bench`
  report on an idle host with valid checksums, stable measurements, and
  mixed-assembly review for every row above 1.2x (including the expanded corpus).
- [ ] **Reduce >1.2x runtime gaps (measured).** Use fresh stable bench evidence
  and assembly/instruction comparison to close remaining gaps (e.g. `page-rank`
  f64 alias/reduction/vectorization, `knight` home/join-boundary traffic,
  `currying` closure-call overhead, and NYC/vector-memory rows), with
  checksum-correct fixes only. Use instruction counts for allocation-heavy
  rows; their wall time is bimodal on this host. Measured leads: `page-rank`'s
  `newrank[0] +=` loop carries its sum through memory (needs alias-proven
  store promotion), and the x86-64 spill-home pool split costs `qrsyndrome`
  2.1% instructions, which a sticky-RAX tag keyed by vreg instead of home
  offset would recover.

Measurement rule: use focused fixtures while diagnosing; use the complete
benchmark and inspect timed-kernel assembly only after correctness is restored.

## Compiler build-time performance

- [ ] **Establish a reproducible compile-time baseline before optimizing.**
  Profile all configured project targets and common editor/engine launch paths.
  Separate clean/cold build, warm build, no-op build, one-file incremental
  build, code-generation, link, and startup costs; record hardware, cache state,
  target set, wall time, peak memory, and compiler phase timings.
- [ ] **Bring every project compile under the requested 15-second target.**
  Apply phase-level profiles to identify dominant costs, then optimize measured
  bottlenecks. Report every configured project separately and the full
  all-project command. Acceptance: clean all-project build and documented common
  incremental workflows each complete in under 15 seconds on the same recorded
  machine, with correct outputs and no stale-cache dependency. If unmet, keep
  open and publish the measured blocker.
- [ ] **Preserve compiler correctness and determinism while speeding builds.**
  Add before/after phase timings and cache invalidation tests; compare clean
  and incremental outputs and run affected project tests.

Dated measurements and fixes are recorded in
[Recorded measurements](../learn/measurements.md#compiler-build-time-fixes-2026-11-12).
