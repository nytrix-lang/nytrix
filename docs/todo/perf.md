<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":4,"summary":"Open NYR, vectorization, and performance-gate work with acceptance criteria."} -->
# Performance work

- [ ] **No valid, comparable fixture above 2× host C** on NYR, LLVM, or NYC.
  Accept a complete `./make bench` run with matching checksums, stable
  measurements, and every comparable backend result at or below 2×.
  Passing the existing class and fixture budgets does not by itself close
  this item.

Use the [dashboard](../learn/measurements.md#benchmark-dashboard) to rank
misses and the [measurement log](../learn/measurements.md#measurement-log)
for traces and rejected experiments. Diagnose one fixture at a time; keep
its workload and checksum intact.

## Open work

| Work | Main beneficiaries |
|---|---|
| Prove typed-buffer payload provenance, affine bounds, and memory dependence across passes | `cd`, `lbm`, `viterbi`, `susan`, stencil loops |
| Vectorize conditional counts and signed max reductions, with scalar tails and a cost check | `cd`, `viterbi` |
| Keep live ranges from occupying registers across inner loops where they are unused; eliminate home round trips | `sssp`, `mont64`, graph/DP, recursion |
| Scalar-replace fixed typed-buffer limbs after unrolling | `mont64`, small-array loops |
| Track available loads by root, byte range, and memory version; remove redundant 128-bit vector homes | NYC `bce-matrix`, memory-heavy loops |
| Narrow integer lanes and vectorize reductions when bit ranges permit | `bitcount`, `intops`, crypto |
| Reduce managed allocation and operand validation without weakening pointer checks | `bigint-oracle` (library vs GMP), allocation-heavy workloads |
| Separate startup cost from kernel cost; fix real startup overhead | `native-oracle`, short workloads |
| Reuse CFG, dominator, and SCEV analysis where pass invalidation permits | `page-rank`, `dce`, `sha256` |
