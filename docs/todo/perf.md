<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":4,"summary":"Open NYR, vectorization, and performance-gate work with acceptance criteria."} -->
# Performance work

An item closes only when its acceptance check has passed on the current tree.
Historical measurements are investigation snapshots, not gate evidence.

Working rules:

- Compare generated code by instructions executed, then read the assembly.
- A change counts only if checksums still match and the suite stays green.
- Reproduce one benchmark or fixture at a time before changing anything.
- Measured results, session logs and kept/reverted evidence belong in
  [measurements.md](../learn/measurements.md); this file carries only open work
  and its acceptance criteria. Dated evidence for every landed or reverted
  change sits in that page's measurement log, not here.

## NYR

The [benchmark dashboard](../learn/measurements.md#benchmark-dashboard) lists
the latest full run and the largest gaps. Use retired instructions and
timed-kernel assembly to compare changes when wall times fluctuate.

- [ ] **At most 1.2x of C on every backend.** No valid, comparable benchmark
  above 1.2x on NYR, NYC, or LLVM. Accept: a fresh complete `./make bench`
  report on an idle host with valid checksums, stable measurements, and
  mixed-assembly review for every row above 1.2x (including the expanded corpus).
- [ ] **Reduce >1.2x runtime gaps (measured).** Use fresh stable bench evidence
  and assembly/instruction comparison, checksum-correct fixes only. Largest
  rows by instructions executed relative to C (whole process, 2026-11-18):
  - `radix-tree` (10.7×, was 24.5×): the 20-step bit loop now unrolls
    (descending loops, up to 32 trips) and folds `node * 2 + bit` into one
    `lea`. Focused run on 2026-11-19: NYR 11.6 ms, 4.72× vs LLVM. The `-vvvvvv`
    candidate trace (readable after the per-thread regalloc batch fix) rules out
    the two earlier explanations for `rt_main`'s 3,434 loop-carried candidate
    evaluations: `store-safe` and `vector-safe` never fail, and the AND already
    uses the commutative two-address form. The 456 top-score (152-167) entries
    are all `unfolded=0` — loop-carried constants whose immediate fold is
    load-bearing (relaxing it measured 21.1 → 26.5 ms). `node`/`depth` live in
    the 16-23 score band, where 320 of 456 evaluations fail colour availability
    and 118 are already coloured, so the remaining work is colour ownership and
    selection order, not another peephole. The trace now names the owner: v397
    (score 168, `carried=0`) blocks the carried walk value v390 1,362 times.
    Restricting dedicated homes to loop-carried values was measured the same
    day — reloads 146 → 107, frame homes 2,984 → 2,680 bytes, wall time inside
    noise (24.16 → 24.05 ms median) — and reverted.

    Home accounting for `rt_main` (2026-11-19): `home-layout vregs=545
    single-block=189 unique=17 reused=172 bytes=2984`. Seventeen slots serve
    the 189 same-block residents (172 reused); the other 356 values take
    private 8-byte slots and account for 2,848 of the 2,984 bytes.
    `NYTRIX_HOME_RANGE_SCAN=1` produces a byte-identical layout, and
    `NYTRIX_NO_SPILL_HOME_COLOR=1` gives 3,064 bytes, so slot sharing works but
    cannot reach multi-segment values: `mach_vreg_home_spill_interval` refuses
    to pool any value that is register-resident in any segment because the
    encoder's sticky-RAX tag is keyed by home offset. Shrinking the frame needs
    that guard replaced with a home-read-after-write proof, not more colours.

    That proof now exists: a register-resident value may pool its slot when
    every home-reading segment writes the slot before its first read (a live-in
    read keeps a private slot, `NYTRIX_NO_MULTISEG_HOME_SHARE=1` restores the old
    rule). Frame homes on 2026-11-19: `rt_main` 2,984 → 2,704 bytes, viterbi
    2,584 → 2,472, `cd` 2,672. Checksums unchanged (`62531`, `3270530`, `387`)
    and radix-tree wall time inside noise (22.7-25.4 ms vs 23.9-24.5 ms), so
    this is a frame-size reduction, not a measured speedup.
  - `cd` (8.5×): per-iteration reloads of `px[i]`/`py[i]` (needs load
    hoisting with a no-store proof) and C's vectorized pair loop. Deep trace
    (2026-11-19, 18,086 candidate evaluations): carried rejections are
    dominated by `unfolded=0` (6,762) and `available=0` (6,022); store safety
    and vector safety never fail, the same residency picture as `radix-tree`.
  - `viterbi` (5.9×, was 6.6×): the running max is now a select; `rt_main`
    register pressure still spills the loaded value across the modulo. Deep
    trace: `unfolded=0` 2,706, `available=0` 2,514 and `vector-safe=0` 462 —
    the only one of these three rows where vector safety rejects carried
    values. Those 462 are all on caller-saved scratch colours (preg 8, 9, 1,
    2 = r8, r9, rcx, rdx); the accepted homes are callee-saved r12-r15, which
    the eligibility rule already exempts, so vector safety costs this row no
    viable colour and needs no fix.
  - `stream`: NYR emits a four-lane i64 multiply for the triad. Investigate
    why LLVM and NYC remain slower than host C on the same workload.
  - `mont64` (3.3×, was 4.8×): fixed-trip limb loops with stores now unroll;
    the remaining limb arrays live in memory (scalar replacement of small
    fixed buffers would keep them in registers).
  - `bitcount`: NYR and NYC emit register `popcnt` for the scalar word
    loop. C vectorizes the loop and its words occupy four bytes each,
    compared with eight bytes in the Ny fixture.
  - `ackermann`: both references now execute the recursion with a runtime
    input. NYR and NYC avoid carrying the decrement in a separate location
    across the recursive call. The remaining gap includes callee-save and
    loop branch overhead.
  - `bce-matrix` (2.9×), `nussinov` (2.7×),
    `dijkstra` (2.7×), `sssp` (2.2×), `qrsyndrome` (2.2×), `permute` (1.5×,
    was 2.1×), `nqueens` (1.95×), `gcbench` (2.0×): loop home traffic and
    allocation; `permute`'s fixed 10-trip inner loop is not yet unrolled.
  - `towers` (1.30× wall, was 2.5×): shrink-wrapped base case and parameter
    slots used as homes; the remaining cost is five callee-save pushes per
    call.
  - `currying` (sub-ms): every partial application allocates a closure and
    calls through the dynamic closure path.
  - `fhourstones` (2026-11-19, 2.48× of LLVM): 25.16M instructions vs C
    7.40M (3.40×), checksum 6010. `perf` splits the work 67% `search` /
    28% `won`; `won` is rejected for inlining by the loop pressure-CFG
    ceiling (96 nested / 128 otherwise). Raising that ceiling inlines it
    (155-inst splice) and measures only 25.16M → 24.46M instructions
    (-2.8%), so the change was reverted. `perf annotate` puts 31% and 47%
    of the samples on single scalar scan `cmp` instructions, so the gap is
    the scalar board scan itself, not call overhead.
  - `radix-tree` (2026-11-19): 123.8M NYR instructions vs 11.7M for C
    (10.5×) at the same checksum 62531. C collapses the 20-step
    `node = node * 2 + bit` loop into `node = 2^20 | keyval` (11.7
    instructions per lookup), while NYR executes all 20 dependent
    iterations. Closing this needs loop strength reduction / bit-reassembly
    recognition, not another register-allocation change.
  - `mont64` (2026-11-19): 74.8M NYR instructions vs 22.4M for C (3.34×),
    checksum 114319663. The unrolled limb loop reloads every buffer pointer
    from its home after each heap store (`mov -0x130(%rbp),%rax` then
    `mov (%rax),%r9`) and re-spills/re-loads the limb accumulator (`s`)
    around the typed-buffer store. The inner arithmetic is already
    two-address and unrolled; the remaining cost is alias-conservative
    pointer residency across stores.
  - `cd` (2026-11-19): 100.4M NYR instructions vs 12.7M for C (7.9×) at
    checksum 387, but the scalar code is not defective: the disassembly
    hoists `px[i]`/`py[i]` into `rdi`/`r8` outside the `j` loop and the pair
    body is ~25 instructions (two loads, two subs, two cmov abs, the two
    `< 8` tests, increment/test/branch). C reaches 3.2 instructions per pair
    only by vectorizing. `NY_TRACE_VECTORIZE` rejects the pair loop with
    `load address is non-affine, aliasing, escaping, or out of proven
    bounds` (the loop-invariant `px[i]` address) and `unsupported
    loop-carried PHI` (the conditional `hits` reduction).
  - `bitcount` (2026-11-19): 74.8M instructions vs 31.5M for C (2.38×) at
    checksum 134217488. The scalar SWAR body is ~9 instructions per word;
    the C reference is vectorized. Vectorizer rejects the word loop with
    `no canonical +1 induction PHI`, `load address is non-affine ...`, and
    `mixed i64 operand is not loop-invariant`.
  - `viterbi` (2026-11-19): 53.9M instructions vs 9.1M for C (5.9×) at
    checksum 3270530. One loop is already vectorized; the DP loop rejects
    with `unsupported loop-carried PHI` and `load address is non-affine,
    aliasing, escaping, or out of proven bounds`.
  - **Bool-return truthiness (2026-11-19).** A call to a declared-`bool`
    function used as a condition lowered through the generic dynamic
    truthiness test (`!= 0 & != 2 & != 1`). The source function boundary
    already carries the canonical tagged boolean, where false is
    `NY_IMM_FALSE` (2) and true is `NY_IMM_TRUE`, so `nyr_emit_truthy` now
    emits one `!= NY_IMM_FALSE`. `nussinov` instructions 120.7M → 98.4M
    (-18.5%), cycles 33.65M → 26.75M (-20.5%), checksum 100 unchanged;
    focused `nussinov` 3.13× → 2.29× of C (2.11× of LLVM). `lang/execution`
    65/65 passed and the full `etc/tests/lang` run was 1472/1476 (the four
    misses were 426 s proof-fixture timeouts under load).
  Use instruction counts for allocation-heavy rows; their wall time is
  bimodal on this host.

Measurement rule: use focused fixtures while diagnosing; use the complete
benchmark and inspect timed-kernel assembly only after correctness is restored.

## Ranked work queue (from the native-compiler optimization plan)

Status is per the evidence in
[the measurement log](../learn/measurements.md#measurement-log); keep this list
ordered by expected payoff and
update it as items land. Do not re-derive the audit, and do not repeat the
negative experiments already listed earlier in this file.

| # | Work | Main beneficiaries | Status |
|---|---|---|---|
| 1 | Bit-reassembly loop folding | radix-tree | **landed 2026-11-19** (NYR/LLVM 2.06× → 1.03×) |
| 2 | Reusable affine typed-buffer legality consumed by the vectorizer (root + affine range + extent + read/write dependence, with the exact missing proof reported) | cd, lbm, viterbi, susan, stencil loops | byte-range dependence and CFG entry proof landed. Handle provenance and reuse across passes remain open. |
| 3 | Conditional count and signed max vector reductions (`hits += predicate`, `best = max(best, x)`) with scalar tails and profitability from `vectorization_cost` | cd, viterbi | pending (needs #2) |
| 4 | Nested-loop register lending / live-range splitting so an outer value does not hold a color across an inner loop it is unused in; per-interval ownership in the existing allocator, not a second one | sssp, mont64, radix-tree, graph/DP | pending |
| 5 | Fixed typed-buffer scalar replacement after unrolling (mont64 limbs) with memory-state forwarding | mont64, small-array loops | pending |
| 6 | Memory-aware GVN/PRE + load availability keyed by (root, byte range, version) | memory-heavy fixtures | pending (needs #2) |
| 7 | Integer narrowing and known bits for loads, arithmetic, and vector lanes | bitcount, intops, crypto | scalar POPCNT verified on Ny and C. Vector reductions and narrower memory traffic remain open. |
| 8 | Machine-level home round-trip and copy elimination (e.g. `lea; mov [rbp-x]; mov rax,[rbp-x]` in cmov-sort) | ackermann, cmov-sort, sssp, heapsort | scheduler sinks safe edge updates past calls. General home round trips remain open. |
| 9 | BigInt/BigFloat runtime cost | bigint-oracle | short Knuth quotients use hardware division where available. Allocation and operand classification remain open. |
| 10 | Compile-time analysis reuse (SCEV/CFG/dominators) once runtime work lands | page-rank, dce, sha256 | pending |

Known-blocked diagnostics worth keeping in mind: viterbi's `% 101` still pays the
signed magic-division sign correction because `nyr_analyze_values` is a single
linear pass (see the loop-carried-ranges entry in
[the measurement log](../learn/measurements.md#measurement-log)); cd's pair loop
still
lowers its nested `if` to branches because the vectorizer cannot prove the
affine typed-buffer access.

## bigint-oracle: measured against GMP, library scope

Harness fixed earlier (`make` now honours a shape's `optional_ldflags`, so the
reference links `-lgmp`); the current cost, its profile and why this is a
library rather than a codegen gap are in
[the measurements log](../learn/measurements.md#2026-11-19-runtime-linking-registry-cache-residency-allocation-bookkeeping).

Accept: a focused `./make bench bigint-oracle` row under its budget with the
checksum matched on all routes. Open candidates, in the order they should be
tried: keep `rt_value_tag` off the managed-registry probe for values that already
carry a bigint/bf tag; classify one `any` per bigint operation instead of once
per limb read in `_bi_read_heap_limbs`; skip `rt_map_oracle_add` when the decoder
oracle is inactive. Parity with GMP is not an acceptance criterion.

- Vectorizer: cd's remaining rejects are the tbuf handle fetch
  (`address is not a payload+offset add chain`) and the pair-loop
  loop-carried PHIs; the first needs the handle to name its payload root,
  which is an ABI/representation change, not a pass change.
