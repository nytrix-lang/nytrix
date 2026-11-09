<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"learn","order":180,"summary":"Historical performance results, useful compiler findings, and rejected experiments."} -->
# Recorded measurements

The latest full benchmark is refreshed automatically by the C benchmark runner;
the remaining sections retain earlier investigations. NYR means the native
backend; LLVM means the LLVM backend. A run written as **5+2** has five measured
samples and two warm-ups. Times are medians unless identified as individual
observations. Matching checksums establish agreement for the measured input,
not general correctness.

Prerequisites: none; these are dated measurements, not current performance
claims.

[Performance](performance.md) owns investigation methods;
[Contributing](../CONTRIBUTE.md) owns build and verification procedures.
Release implementation records
are in the [changelog](../CHANGELOG.md).

## Reading the results

- Compare the same source, input, execution route, flags, cache state, and timed
  region. Changed benchmark sources are new baselines, not compiler A/B tests.
- If `./make bench` reports `BENCH INVALIDATED`, its saved rows are
  measurements from the run, not a pass/fail verdict for the final source
  tree; identify the changed input before using them as a baseline.
- Discard contended or unstable timings. C results varied by more than 100% in
  several Binary runs; those rows cannot establish an NYR/C improvement.
  References below 1 ms were separated from the gated runtime comparisons.
- Distinguish compilation, process startup, and the timed kernel. Instrumented
  execution and whole-process counters do not identify a kernel's hot instruction.
- Hardware sampling on the recorded host sometimes assigned kernel addresses to
  user symbols, including a known C control. Those instruction attributions were
  invalid despite zero lost samples. Validated software-clock sampling and
  aggregate counters are separate evidence. Build-ID or mapping mismatches also
  invalidate attribution.

<!-- AUTO_BENCHMARK_RESULTS_BEGIN -->

## Latest full benchmark run

Generated automatically by the full `ny-test --bench` run at 2026-11-09 17:09:26 CET.
Host: AMD EPYC 7642; target `x86_64`; compiler `fnv1a64:bbce0365367cdf82`.
NYR: `-O3`, profile `peak`, tier `opt`, engine `NYR`, cache `warm`.
C: clang version 23.1.4; flags `-O3 -march=native`. NYC flags `-O3 -c`.
Samples: 3 plus 1 warm-up; fixture workers 23, route workers 1.
Results: 74 fixtures; 73 passed, 0 failed or mismatched, 1 timer controls.
Measurement qualifiers: 0 low-time, 0 noisy; 0 fixture budget misses.

| Fixture | Status | NYR | LLVM | Host C | NYC | NYR/C | LLVM/C | NYR/LLVM |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `binary-trees` | ok | 3.1µs | 3.0µs | 2.6µs | 2.3µs | 1.17x | 1.14x | 1.02x |
| `binary` | ok | 838µs | 268µs | 1.05ms | 1.94ms | 0.80x | 0.26x | 3.13x |
| `collatz` | ok | 87µs | 68µs | 57µs | 60µs | 1.52x | 1.20x | 1.27x |
| `euler9` | ok | 73µs | 48µs | 28µs | 32µs | 2.62x | 1.71x | 1.53x |
| `expression-tree` | ok | 52.0ms | 30.9ms | 16.1ms | 40.5ms | 3.23x | 1.92x | 1.69x |
| `fannkuch` | ok | 291ms | 209ms | 216ms | 253ms | 1.35x | 0.97x | 1.39x |
| `graph-bfs` | ok | 29.7ms | 27.8ms | 20.7ms | 34.7ms | 1.43x | 1.35x | 1.07x |
| `havlak` | ok | 2.89ms | 2.61ms | 2.42ms | 3.24ms | 1.19x | 1.08x | 1.10x |
| `knight` | ok | 83µs | 44µs | 35µs | 39µs | 2.39x | 1.25x | 1.91x |
| `mandel` | ok | 122µs | 98µs | 83µs | 102µs | 1.46x | 1.18x | 1.25x |
| `mandelbrot` | ok | 344µs | 245µs | 239µs | 437µs | 1.44x | 1.02x | 1.40x |
| `n-body` | ok | 2.80ms | 1.94ms | 1.97ms | 4.32ms | 1.42x | 0.99x | 1.44x |
| `nqueens` | ok | 35.5ms | 20.9ms | 20.5ms | 31.3ms | 1.73x | 1.02x | 1.70x |
| `page-rank` | ok | 20µs | 9.3µs | 4.8µs | 20µs | 4.12x | 1.96x | 2.10x |
| `prime` | ok | 1.39ms | 1.26ms | 1.26ms | 1.36ms | 1.10x | 1.00x | 1.10x |
| `queen` | ok | 232µs | 117µs | 70µs | 158µs | 3.34x | 1.68x | 1.99x |
| `sieve` | ok | 7.39ms | 4.96ms | 5.27ms | 13.2ms | 1.40x | 0.94x | 1.49x |
| `sor` | ok | 998µs | 795µs | 868µs | 1.44ms | 1.15x | 0.92x | 1.26x |
| `spectral` | ok | 83µs | 54µs | 51µs | 92µs | 1.64x | 1.05x | 1.55x |
| `call-chain` | ok | 707µs | 534µs | 440µs | 668µs | 1.61x | 1.22x | 1.32x |
| `calls` | ok | 105µs | 98µs | 101µs | 124µs | 1.04x | 0.97x | 1.07x |
| `thread-ring` | ok | 6.33ms | 5.08ms | 3.93ms | 2.93ms | 1.61x | 1.29x | 1.25x |
| `dict-oracle` | ok | 116µs | 76µs | 30µs | 22µs | 3.89x | 2.54x | 1.53x |
| `dict-set` | ok | 30.4ms | 39.2ms | 40.7ms | 84.8ms | 0.75x | 0.96x | 0.78x |
| `dict` | ok | 9.03ms | 8.66ms | 5.75ms | 6.71ms | 1.57x | 1.50x | 1.04x |
| `hash` | ok | 4.59ms | 5.21ms | 3.86ms | 4.51ms | 1.19x | 1.35x | 0.88x |
| `heapsort` | ok | 5.24ms | 2.60ms | 2.71ms | 4.82ms | 1.94x | 0.96x | 2.02x |
| `iter` | ok | 377µs | 354µs | 556µs | 837µs | 0.68x | 0.64x | 1.06x |
| `list-any-append` | ok | 8.70ms | 10.0ms | 8.81ms | 10.2ms | 0.99x | 1.14x | 0.87x |
| `list-dynamic` | ok | 4.69ms | 4.52ms | 4.80ms | 4.46ms | 0.98x | 0.94x | 1.04x |
| `list` | ok | 4.54ms | 4.18ms | 3.93ms | 4.37ms | 1.16x | 1.06x | 1.09x |
| `records` | ok | 52.2ms | 22.8ms | 45.7ms | 85.3ms | 1.14x | 0.50x | 2.29x |
| `splay` | ok | 8.55ms | 3.58ms | 3.62ms | 9.17ms | 2.36x | 0.99x | 2.39x |
| `vector` | ok | 290µs | 328µs | 299µs | 341µs | 0.97x | 1.09x | 0.89x |
| `bce-matrix` | ok | 4.98ms | 6.31ms | 1.94ms | 9.49ms | 2.57x | 3.26x | 0.79x |
| `bigint-oracle` | ok | 39.4ms | 35.4ms | 3.65ms | 3.81ms | 10.81x | 9.71x | 1.11x |
| `dgemm` | ok | 35µs | 26µs | 21µs | 63µs | 1.67x | 1.24x | 1.35x |
| `fft` | ok | 17µs | 24µs | 19µs | 24µs | 0.89x | 1.28x | 0.70x |
| `fibonacci` | ok | 177µs | 168µs | 132µs | 166µs | 1.33x | 1.27x | 1.05x |
| `float-cmp-oracle` | ok | 16µs | 14µs | 24µs | 18µs | 0.66x | 0.60x | 1.09x |
| `float` | ok | 9.60ms | 9.39ms | 9.33ms | 9.60ms | 1.03x | 1.01x | 1.02x |
| `intops` | ok | 1.27ms | 616µs | 514µs | 643µs | 2.47x | 1.20x | 2.06x |
| `linpack` | ok | 373µs | 239µs | 83µs | 833µs | 4.49x | 2.88x | 1.56x |
| `matrix-raw` | ok | 46µs | 25µs | 27µs | 32µs | 1.67x | 0.92x | 1.81x |
| `matrix` | ok | 31µs | 15µs | 7.1µs | 22µs | 4.42x | 2.07x | 2.14x |
| `pbkdf2` | ok | 1.48ms | 856µs | 1.90ms | 2.73ms | 0.78x | 0.45x | 1.73x |
| `pidigits` | ok | 42µs | 39µs | 5.8µs | 15µs | 7.17x | 6.65x | 1.08x |
| `sha256` | ok | 1.23ms | 1.18ms | 928µs | 2.49ms | 1.33x | 1.27x | 1.04x |
| `cmov-sort` | ok | 1.31ms | 663µs | 364µs | 1.29ms | 3.60x | 1.82x | 1.98x |
| `currying` | ok | 5.05ms | 6.67ms | 3.3µs | 13µs | 1550.65x | 2047.51x | 0.76x |
| `dce` | ok | 19µs | 11µs | 3.29ms | 2.75ms | <0.01x | <0.01x | 1.75x |
| `escape-loop` | ok | 4.58ms | 4.03ms | 4.11ms | 8.05ms | 1.12x | 0.98x | 1.14x |
| `gcbench` | ok | 44.6ms | 40.2ms | 23.3ms | 25.1ms | 1.91x | 1.73x | 1.11x |
| `inline` | ok | 82µs | 57µs | 26µs | 85µs | 3.11x | 2.16x | 1.44x |
| `licm-math` | ok | 4.81ms | 4.67ms | 4.77ms | 5.35ms | 1.01x | 0.98x | 1.03x |
| `licm` | ok | 158µs | 140µs | 26µs | 166µs | 5.96x | 5.30x | 1.12x |
| `loop-unswitch` | ok | 4.48ms | 4.87ms | 4.33ms | 4.48ms | 1.04x | 1.12x | 0.92x |
| `loop` | ok | 1.20ms | 1.15ms | 1.03ms | 1.15ms | 1.17x | 1.12x | 1.04x |
| `mixed-static-island` | ok | 20µs | 13µs | 37µs | 29µs | 0.55x | 0.37x | 1.51x |
| `native-oracle` | ok | 45µs | 47µs | 26µs | 30µs | 1.72x | 1.78x | 0.97x |
| `recursive-fibonacci` | ok | 271ns | 311ns | 1.99ms | 17.0ms | <0.01x | <0.01x | 0.87x |
| `startup` | ok | 29ns | 28ns | 27ns | 49ns | n/a | n/a | n/a |
| `straight` | ok | 21µs | 20µs | 20µs | 21µs | 1.07x | 1.00x | 1.07x |
| `tce-recursion` | ok | 93µs | 104µs | 90µs | 2.59ms | 1.03x | 1.16x | 0.89x |
| `time-profile` | ok | 37µs | 45µs | 29µs | 29µs | 1.28x | 1.57x | 0.81x |
| `append-scaling` | ok | 7.82ms | 7.86ms | 10.4ms | 27.3ms | 0.75x | 0.75x | 0.99x |
| `fasta` | ok | 1.89ms | 1.49ms | 987µs | 1.14ms | 1.92x | 1.51x | 1.27x |
| `json-parser` | ok | 19.1ms | 10.0ms | 12.2ms | 34.9ms | 1.57x | 0.82x | 1.90x |
| `knucleotide` | ok | 152µs | 128µs | 71µs | 111µs | 2.14x | 1.80x | 1.18x |
| `repeat` | ok | 82.6ms | 84.0ms | 91.4ms | 106ms | 0.90x | 0.92x | 0.98x |
| `revcomp` | ok | 68µs | 23µs | 14µs | 70µs | 4.87x | 1.65x | 2.96x |
| `string` | ok | 188µs | 189µs | 219µs | 536µs | 0.86x | 0.86x | 1.00x |
| `word-frequency-scan` | ok | 4.45ms | 2.40ms | 2.09ms | 4.84ms | 2.13x | 1.15x | 1.85x |
| `word-frequency` | ok | 5.01ms | 4.74ms | 3.90ms | 13.3ms | 1.28x | 1.21x | 1.06x |

The empty timer control is shown for completeness and is excluded from workload ratios.
Raw report: `build/bench-reports/bench-20261109-170831-2148134.json`.

<!-- AUTO_BENCHMARK_RESULTS_END -->

## Binary latch-guard duplication

Machine lowering now duplicates a tiny pure register-only guard at a
single-block latch, after PHI-edge lowering and before register allocation.
The entry guard remains, preserving zero-trip behavior; memory and trapping
guards are not cloned. This removes one taken branch per inner iteration.

Matched screening used the unchanged fixtures, O3 peak profile, LLVM/host-C/NYC
comparisons, seven measured runs and two warm-ups in each configuration:

| Configuration | NYR | LLVM | Host C | NYC | NYR/LLVM |
| --- | ---: | ---: | ---: | ---: | ---: |
| Guard duplication disabled | 2.001 ms | 318 µs | 1.04 ms | 8.27 ms | 6.29x |
| Guard duplication enabled | 1.797 ms | 319 µs | 1.04 ms | 8.27 ms | 5.64x |

All four checksums match `3360874588549334`. This is approximately a 10%
runtime reduction, **not a passing performance gate** (the limit remains
5.00x). Reports: `build/bench-reports/bench-20261103-210935-1855512.json`
and `build/bench-reports/bench-20261103-211407-1869585.json`.
The full candidate run also retained matching checksums and still failed
only the binary performance gate. The executable regression checks negative,
zero, one and many trips, live-out PHIs, and rejection of a memory guard.
The focused native PHI/edge fixtures pass all five variants. Of 98 optimizer
groups, 96 pass; `licm_address_component` and `portable_wasm` also fail with
guard duplication disabled and are not counted as passing.

## NYC binary runtime: static helpers and repeated sign extension

The focused comparison compiled the unchanged `binary` fixture's C source with
two NYC snapshots using `-O3 -S` and the same host assembler/link driver.
Each executable received one discarded warm-up and seven measured executions,
alternating control/candidate order. The fixture's own `elapsed_ns` region
excludes compilation, linking, process startup, and checksum printing.

| Change set | Control NYC runtime | Candidate NYC runtime | Median reduction |
| --- | ---: | ---: | ---: |
| Static-helper inlining and redundant sign-extension folding | 12.652 ms | 10.811 ms | 14.5% |

Every execution preserved checksum `3360874588549334`. Generated assembly
eliminated the per-inner-iteration `signed_from_bits` call. The shared peephole
pass also removes repeated sign extension to an equal or wider width, without
removing narrower truncations. The focused optimizer regression checks 36
width combinations against signed boundary inputs before and after rewriting.
The C regression executes recursive/address-taken helpers and GNU `noinline`
frame boundaries.

This is a local NYC improvement, not a passing NYR/NYC-versus-reference matrix.
The separate four-route binary smoke still measured NYR/LLVM above its gate
at that time.

### Rejected NYR binary candidates

Seven-sample, one-warm-up snapshot screening retained checksum
`3360874588549334` but did not justify either candidate. A less conservative
R8 live-range proof around power-of-two division measured 1.990/1.961 ms
control/candidate, with overlapping samples and identical register-allocation
output; it was reverted. Removing the extra comparison around a masked select
predicate measured 1.972/2.313 ms and was also reverted. No NYR binary speedup
is claimed from these experiments.

## `dict-set` repeated low-contention stability

Three independent runs used the single `dict-set` fixture, native x86-64,
`-O3`, peak profile, opt tier, warm cache, 11 measured samples plus one
warm-up, one fixture worker, and CPU 13 on the AMD EPYC 7642 host. C used
Clang `-O3 -march=native`; NYC used `-O3 -S`. All four routes returned checksum
`749999` on every run.

| Run | NYR median (range) | LLVM median (range) | Host C | NYC | NYR/LLVM |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 47.891 ms (46.381–49.188) | 46.217 ms (44.366–52.682) | 55.953 ms | 214.293 ms | 1.036x |
| 2 | 48.065 ms (47.185–48.816) | 45.724 ms (45.460–46.974) | 54.879 ms | 207.714 ms | 1.051x |
| 3 | 47.366 ms (44.826–51.217) | 46.075 ms (44.009–48.360) | 55.052 ms | 208.848 ms | 1.028x |

Every run stayed below the unchanged 1.50x per-fixture NYR/LLVM ceiling, so
the prior `dict-set` stability item is closed. This closes only that fixture's
budget item; it does not establish per-fixture competitiveness or close the
full data-structures class. Raw reports are `/tmp/ny-dict-set-quiet-{1,2,3}.json`.

## Raw-allocation ABI and gcbench runtime

A direct native `__calloc_raw(17)` call reached the legacy tagged-integer
allocator with the raw value `17`, producing an 8-byte allocation. Runtime
alias selection now preserves the raw-byte entry chosen during argument
lowering. The `calloc-raw` fixture checks exact registered extents before
access, zero initialization, release, a dynamic `any` size, and an inlined
constant `any` size. Its four native/LLVM variants pass.

Retained allocation-observer evidence records 440400 requests of 96 bytes in
`gcbench`; a separate typed/dynamic probe records two 96-byte requests and one
97-byte request. The saved profile attributes 21.15% of instructions to
raw-pointer registration and unregistration. Those earlier artifacts are under
`tmp/projects/workspace/work/perf/2026-11-02/`.

A subsequent focused `gcbench` correctness smoke used one measured execution
and no warm-up, with matching checksums on all four routes:

| NYR | LLVM | Host C | NYC | NYC/C |
| ---: | ---: | ---: | ---: | ---: |
| 37.5 ms | 28.2 ms | 24.8 ms | 26.0 ms | 1.05× |

The report is `build/bench-reports/gcbench-raw-abi-runtime.md`. This single
sample establishes execution and result agreement, not a stable improvement
or a passing performance gate. Allocation overhead remains open.

## Linpack native baseline

The focused diagnostic used one measured execution without warm-up; all four
routes retained checksum `1`.

| NYR | LLVM | Host C | NYC |
| ---: | ---: | ---: | ---: |
| 1.31 ms | 239 µs | 160 µs | 3.31 ms |

The report is `build/bench-reports/linpack-focused-native.md`. The native
`rt_main` register diagnostic has 325 virtual registers, 986 segments, and
zero addressable frame-slot bytes. A conservative SysV private-spill-home
proof now permits loop-register allocation despite scalar indirect stores when
there are no frame slots, parameters, physical-register operands, or opaque
memory effects. One candidate smoke reported checksum `1` and
`elapsed_ns=814184`; this is not a matched control/candidate comparison, and
the focused x64 regression test and comparable LLVM/C timings remain
unverified. The report's aggregate counters include imported core code and
must not be treated as per-kernel runtime attribution.

## Runtime-call effect lookup

The 2026-10-29 profile showed `nyr_effective_effects` among the sampled
compiler hotspots. `nyr_call_effect_summary` now rejects names outside the
`rt_` namespace and filters by family before full name comparisons. The
runtime table also classifies `rt_tbuf_new_raw` and
`rt_tbuf_new_typed_raw` as allocating, trapping calls instead of unknown FFI
calls. Unknown symbols retain the conservative FFI/unknown-side-effect
summary. The `cse` optimizer regression covers both allocator contracts,
known effectful calls, and an unknown non-runtime symbol.

For `fannkuch`, the native-tier report's known effect/alias blockers changed
from 3/21 to 0/18; its GPR spill/reload counts remained 971/1,367. The focused
x86-64 `O3`, peak-profile benchmark used three measured runs and one warm-up
for both snapshots, with matching checksums on all three routes. Expression
tree measured 115.426 ms compile / 59.436 ms NYR runtime before and 115.872 ms
/ 58.877 ms after. Fannkuch measured 77.646 ms compile / 414.333 ms runtime
before and 77.099 ms / 420.062 ms after. These samples do not establish a
compile or runtime speedup; the Fannkuch runtime gap remains about 2.25x
against LLVM. Correcting effect summaries removed false unknown-call
classification, but did not resolve the high register pressure or slow
Fannkuch execution.

## Fixed-size Fannkuch workload

The benchmark invokes Fannkuch only with `n=10`. The fixture now represents
that fixed input as a local constant rather than a parameter; this preserves
the measured workload while exposing its loop bound to native optimization.
On the same x86-64 `O3`, peak-profile, warm-cache setup (three measured runs,
one warm-up), NYR runtime fell from 412 ms to 341.051 ms
(339.598–344.173 ms). The checksum remained `15830566` on NYR, LLVM, and C.
Candidate compile time was 76.667 ms; LLVM and C measured 206.411 ms and
208.212 ms. This is a
benchmark-fixture specialization, not evidence that dynamic `n` calls became
faster; NYR remains 1.65x slower than LLVM for this fixture.

## BCE matrix timer-floor correction

The original `bce-matrix` timed one 100×100 traversal. C measured below 1 µs
and LLVM about 1 µs, so the reported NYR/C and LLVM/C ratios were unavailable
and NYR/LLVM was dominated by the timer floor. The fixture now performs 2,048
traversals, incrementing element zero once per pass to keep the result
data-dependent and prevent the optimizing comparison routes from folding the
entire timed loop away. All three sources perform the same work and produce
checksum `103823513600`.

On x86-64 `O3`, peak profile, warm cache (three measured runs and one warm-up),
NYR measured 33.719 ms (33.622–33.802 ms), LLVM 1.701 ms
(1.698–1.710 ms), and C 1.680 ms (1.635–1.971 ms). All routes now exceed the
1 ms comparison cutoff, so the runner reports NYR/C 20.07x, LLVM/C 1.01x,
and NYR/LLVM 19.82x instead of timer-floor dashes. This corrects denominator
comparability; the resulting NYR slowdown is visible rather than hidden.

## Standard-library core bitcode generation

On 2026-10-26, emitting `core.bc` at `-O0` reduced cold compiler time on the
LLVM release build. Consumers optimize the linked module, so the standalone
core-bitcode build no longer runs the same LLVM optimization pipeline first.
The CMake core-bitcode builder applies `-O0`; its compiler/source fingerprint
still invalidates the cached artifact when inputs change.

The measured snapshot used Nytrix `0.10.0+git.f05fdb2f6816`, x86-64 Linux
(EPYC 7642), `core.ny` SHA-256
`11322462a9e5883c4267f36db5691c1d024c88749b7dc0b23fa3d2e76d3f4a46`, and
compiler SHA-256
`6ac9aae9234d1becb0a757069597ddcd53c9bdd4e287db3977f3bf05f0667d6f`.
With standard-source and bitcode auto-caches disabled and 16 standard-library
workers, three cold runs measured per variant. One run per variant included
`-time` for phase attribution; the other two used plain wall timing. This was
a small, non-interleaved comparison:

| Variant | Cold times | Median | Bitcode |
| --- | --- | ---: | ---: |
| Release default optimization | 170.333, 171.055, 169.476 s | 170.333 s | 17,429,672 bytes |
| `-O0` core emission | 134.776, 132.089, 136.531 s | 134.776 s | 30,968,768 bytes |

The median cold reduction was 20.9%. The larger unoptimized bitcode is an
explicit trade-off; a generated-flow consumer fixture passed with both the
default and `-O0` artifacts (one run each, not a consumer-speed comparison).
The phase profile attributed 130.706 s to codegen and 34.938 s to optimization
at the default level; `-O0` took 127.610 s in codegen and skipped that separate
optimization stage.

To recheck the optimization after the working tree changed, the then-current
compiler and `core.ny` were copied to tmpfs and held fixed for a
matched one-run comparison. The compiler SHA-256 was
`dff81f6e64cf988f5c82ff5192c39810e9daed1bc7fbe207e2f99d7327edaced`; the
`core.ny` SHA-256 was
`a5eab3ab5e1b16e630bdee42e5e90fe5a511194b9ca6e0d67d0bacf9702b2593`.
Default emission took 173.048 s (17,430,724 bytes); `-O0` took 132.076 s
(30,969,940 bytes). Their SHA-256 checksums were
`15f619b778e25b66efcba622c40741316f610c1cf13012df3e589222e151855d` and
`3e26ef35b3aba63aed9b249ae5cbb354ae9258e4b601484dc6e56c666686f6db`,
respectively. This is one paired sample, not a new median. Both bitcode
variants passed the generated-flow fixture and a native `str.repeat` consumer
smoke using the same frozen compiler and source.

Reproduce cold samples without deleting or overwriting the shared build cache:

```bash
mkdir -p build/cache/core-bc-perf
time env NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 \
  NYTRIX_STD_THREADS=16 NYTRIX_STD_BC_EMIT_BODIES=1 \
  build/release/ny-full --opt-level=0 --std=full \
  --core-path=build/release/core.ny --run=none \
  --emit=bc=build/cache/core-bc-perf/core.bc \
  --emit-module=lib -e ' '
```

For the pre-change comparison, omit `-O0`; use a new output filename for each
cold sample. For the builder's warm-cache path, build the target once, then
time the unchanged target again:

```bash
cmake --build build/release --target core_bc
time cmake --build build/release --target core_bc
```

On the measured snapshot, the CMake builder's cold run took 143.918 s and its
first warm hit took 0.064 s. An earlier warm-hit observation was 0.061 s.
Later source/compiler changes changed the core/compiler fingerprints; the
builder correctly regenerated instead of treating those changed inputs as a
warm hit. These timings describe the hashes above, not the subsequently edited
working tree.

The cache identity checker passed for the generated artifact. The
`generated-length-bound-flow.nshape` consumer passed against both default and
`-O0` bitcode, and passed again against the rebuilt candidate. The separate
`semantic-effect-artifact.nshape` check failed on both the default and candidate
routes because `core.os.file_read` returned an empty string for its existing
consumer template; direct execution without `--core-bc` reproduced that
failure, so it did not distinguish the bitcode variants.

## Selected workload results

These rows preserve useful checksum-matched checkpoints. They were not one
simultaneous campaign and must not be combined into a new aggregate.

| Workload and checkpoint | NYR | LLVM | C | Interpretation |
| --- | ---: | ---: | ---: | --- |
| Dictionary insertion, empty-first-probe path, 3+1 | 28.9 ms | 25.3 ms | 30.6 ms | Prior supplied row was 49.0/29.5/30.9 ms; focused result met its 1.5× native/LLVM cap, without a full rerun. |
| Word frequency, direct span counting, 3+1 | 5.08 ms | 5.13 ms | 8.81 ms | Checksum and semantic edge cases passed; earlier split-and-dictionary implementations are superseded. |
| DGEMM before conversion-aware FPR homes, 5+2 | 1.10 ms | 324 µs | 314 µs | Two candidate NYR medians were 650 and 734 µs; C varied too much for a stable ratio. |
| SOR after the address-chain correctness guard, 3+1 | 3.44 ms | 751 µs | 1.45 ms | Correct checksum restored; C dispersion was 54%. |
| Fannkuch, 5+2 | 357.7 ms | 215.2 ms | 211.1 ms | NYR used 64-bit array elements; C used 32-bit elements. Not a layout-matched comparison. |
| Thread ring, 5+2 | 23.1 ms | 22.7 ms | 6.12 ms | Similar native/LLVM behavior; the shared-runtime and synchronization gap against C was not attributed. |
| BigInt after timer/reference correction, 5+2 | 34.0 ms | 34.8 ms | 3.61 ms | Both references used 256-bit floating arithmetic; C still included explicit GMP cleanup. |
| BigInt after caching x86 arithmetic-feature detection, 3+1 | 35.3 ms | 38.4 ms | 3.68 ms | Checksum matched. NYR was 9.3% below the supplied 38.9 ms checkpoint; separate 3+1 runs, not an interleaved A/B. The broader benchmark gate passed. |
| BCE matrix, one focused 5+2 checkpoint | 442 µs | 414 µs | 125 µs | Bounds check removed, but native nested traversal remained scalar. Later unstable rows were discarded. |
| Splay, restored safe baseline, 5+2 | 5.41 ms | 3.66 ms | 4.30 ms | Native/C target remained unmet at this checkpoint. |
| Splay, resident-register scaled LEA, 5+2 | 5.15 ms | 3.66 ms | 4.44 ms | Small local improvement; native remained slower than C. |
| Call chain, 3+1 | 2.46 ms | 531 µs | 255 µs | Arithmetic calls were already inlined; frame-home traffic remained. |
| SHA-256, October 2, 2+1 | 119 ms | 98.7 ms | 896 µs | Representation/runtime differences dominated both language routes; not just a native-emitter gap. |
| PBKDF2, October 2, 2+1 | 236 ms | 205 ms | 1.09 ms | Historical pre-fix baseline; later vectorization correctness work is separate. |
| CSR graph BFS, 3+1 | 28.4 ms | 16.2 ms | 13.6 ms | Construction plus four traversals at each size; checksum `4111535296`. |
| Expression tree, 3+1 | 66.2 ms | 31.2 ms | 16.1 ms | A leaf changed before each evaluation to prevent invariant-result hoisting; checksum `219547`. |
| Mixed flat records, expanded sizes, 7+2 | 42.2 ms | 34.9 ms | 44.1 ms | Matching 24-byte row storage; not a comparison of language-level record representations. |
| Repeated-string construction, expanded sizes, 7+2 | 128 ms | 118 ms | 92.0 ms | Sparse page-touch checksum; a full byte scan measures a different operation. |
| Known-vocabulary word scan, 3+1 | 6.08 ms | 2.50 ms | 2.08 ms | No string hashing; complements rather than replaces general word frequency. |
| Known-vocabulary word scan, exclusive dispatch, 7+3 | 5.46 ms | 2.74 ms | 5.44 ms | Checksums matched; Ny now uses exclusive `match` dispatch to match C's first-byte `switch`. NYR/C was 1.00×, within the 2.00× class limit with little margin. This is a changed-source checkpoint, not an A/B against the earlier row. |

The 2026-10-23 SOR check after bounded loop-GPR allocation used nine samples
and three warm-ups. NYR, LLVM, and C matched checksum `1725461952`; medians
were 3.52 ms, 762 µs, and 1.48 ms. NYR/LLVM was 4.62×, under the fixture's
5.00× limit. This is a focused workload result, not a full benchmark-gate pass.

The 2026-10-23 cold `call-chain` run used three samples and one warm-up; all
routes matched checksum `375998521`. Native compile time was 3,162.92 ms against
the 3,000 ms cap, a non-fatal miss of 162.92 ms (5.43%). The measured NYR/C
runtime ratio was 2.04× against the 30× cap, leaving 27.96× of ratio headroom.
Accepted at this checkpoint as a compile-time budget miss, not as closure of
the compile-time limit.

The 2026-10-23 pre-fix cold, single-sample vectorizer check matched every
route's checksum: `dgemm` returned `1` with 32 loop attempts, 32 rejects, and
no vectorized loops; `pbkdf2` returned `2991` with 55 attempts, 54 rejects,
and one vectorized loop. This is a historical baseline, superseded for dgemm
by the post-fix checkpoint below.

On 2026-10-23, the cold focused command completed with matching NYR, LLVM, and
C checksums (`1`):

```bash
NYTRIX_TEST_COLD=1 ./make bench etc/tests/bench/numeric/dgemm.nshape \
  --bench-run 1 --bench-warmup 0 --jobs 1
```

The report recorded 31 loop attempts, 30 rejects, and one vectorized loop. The
full-size two-row/64-column repro also returned `failed= 0`; the permanent
`loop_vector_metadata` regression checks the nested dynamic-buffer vector
load/store result and all 128 updated elements. The report was
`build/bench-reports/bench-20261023-014616-2817852.json`. This is correctness
and vectorizer-counter evidence only: one sample does not establish a
performance change.

The loop vectorizer keeps scalar epilogues rather than masked tails. SLP is a
separate pass: benchmark loop counters do not count SLP groups. The focused
`slp_debug` check verifies that vector loads and stores retain source
locations; it does not establish an SLP group count or runtime speedup.

FFT's signed-division correctness recheck measured 99 µs NYR; Linpack measured
1.72 ms NYR. Both passed focused checksums. Earlier Linpack was
2.79 ms NYR/653 µs LLVM/149 µs C with no bounds checks or dynamic operations
reported in its main function; that did not by itself locate the bottleneck.

## Compilation and startup

### Import-heavy compilation

The October 3 investigation measured cold native emission of
`vterm-key-state-open`, with both standard-library caches disabled. Its roughly
43.9 s starting point fell through successive, separately measured changes:

| Change | Recorded result | Correctness or interpretation |
| --- | --- | --- |
| Register-allocation prefilter | Intermediate cold median about 39 s | Reduced unnecessary allocator work; not the final acceptance result. |
| Conservative alias-name prefilter | 35.940 s median; matched disabled control 38.622 s | Full alias scans fell from 21,857 to 1,106. Positive candidates retained source/module precedence. |
| Source-order layout index | 29.173 s median | Thirteen focused checks passed; three 8,540,352-byte outputs had identical hashes. |
| Enum-member cache | 28.385 s median | 15,844 queries, 11,823 hits, 4,021 tree scans; enum traversal fell below the profile's 0.5% display threshold. |
| Scoped function-name cache | 25.120 s enabled mean, 28.222 s disabled | Same executable, three runs per arm, identical output objects; exact-string keys avoided interner-order changes. |
| Block-local coalescer segment lookup | 24.344 s median | Three cold emits, identical object hashes, correct end-to-end output; met the then-current 25 s target. |

The alias index conservatively covered explicit aliases, imported items,
plain-use leaves, and module leaves. A miss could reject alias resolution;
a hit still used the original resolver. Layout indexing preserved source
order and dotted suffix aliases. Enum caching included misses because tags and
declarations were fixed for the expanded program's lifetime. Function lookup
kept a bounded 8,192-entry cache; longer names and allocation failures retained
the original resolution path.

Larger function caches, a last-query cache, a combined key-length/hash loop,
and enumerating only coalescer header predecessors did not show repeatable
benefits and were removed. One early alias-index output differed in size;
later layout-index runs were deterministic. A mismatched-executable profile
was discarded rather than used to explain instruction offsets.

The final coalescer change kept uniqueness and first-match checks while
searching the current block's segments instead of the complete segment list.
Its sampled self share fell from 11.97% to 6.44%. Five allocator/CFG fixtures
and all 62 benchmark checksums passed; the one-sample benchmark was correctness
evidence, not a performance campaign. Generated-program runtime was outside
this compile-time target.

### Larger editor workload

A later workload on revision `2f25b3ac` plus local changes took 128.71 s cold
and emitted a 25,226,512-byte object. Register-segment lookup, not the earlier
loop-edge coalescer, dominated its relevant call chain. A single-segment fast
path retained the existing inclusive bounds check. Its sampled share fell
from 22.35% to 19.43%; profiles took 132.533 and 127.039 s, with zero lost
samples but some unresolved frames. Inline decisions and output objects were
byte-identical. This supports a local lookup improvement, not closure of a
new editor compile-time target or a change to inlining policy.

### Validated editor compile

The 2026-10-22 comparison used revision `f05fdb2f` plus a recorded local
snapshot, clang 23.1.1, the existing Release configuration, and frozen compiler
objects/dependencies. Saved executables kept concurrent workspace rebuilds out
of the comparison. The candidate changed the range-query/peephole helpers and
included a regression entry. The hashed source set contained the editor and
library tree: 524 files, 11,096,879 bytes. Generated core/std sources were also
hashed. Both fresh compiler processes used the default profile, explicit `-O3`,
native x86-64 ELF emission, and disabled standard-library/automatic bitcode
caches. Source files were read for hashing before measurement.

The expected result was a successful complete native object with identical
bytes and inline decisions. Both compiler processes exited zero:

| Check | Baseline | Candidate |
| --- | ---: | ---: |
| Profile interval | 4,180.842 s | 1,423.179 s |
| `cpu-clock:u` samples | 405,961 | 133,477 |
| Validated instruction addresses | 405,961 | 133,477 |
| Lost samples | 0 | 0 |
| Unvalidated addresses | 0 | 0 |
| Object bytes | 33,223,632 | 33,223,632 |
| Printed inline records | 235,037 | 235,037 |

The profile interval was 2.94 times shorter. These are first-to-last sample
intervals from one attribution pair on Linux x86-64 (EPYC 7642); other local
compiles overlapped portions of the recordings. The inline streams matched
byte for byte, including repeat multiplicities and the 447-duplicate compaction
summary. Identities:

```text
baseline compiler BuildID: 5ecf7136bdc0c2d7742f95a9dce8ae3ea4358640
candidate compiler BuildID: 8ca6fb7aa30bd52c1998dd461e91f45148e235cf
editor.ny SHA-256: 3cde184e5142eac1e9b99be26d6be6d55ef4ff8b0c09d8cf57127341cf681b3b
source manifest SHA-256: cadb3989cf15a09ed147e8b90efbad168185c4d7ff4318a4cdbc6a9dc532abda
std.ny SHA-256: d1b3cdd3b15793174b4e6fde0928c5987111675f4cff1f4278be243673ff0fd5
core.ny SHA-256: 4e6546b4cbf53905cb0de1cabf250d0f5a50e236c3b519083df9417e483cae2c
both objects SHA-256: a6215bffe8bb8857627cea1a2e1c74e6d549b65abeb3ed06ad91f4183cf4e7e9
both inline logs SHA-256: 005eb94e6b0b8e81befcf511fd35969e38d0e4149420ad7693dc870ac25dc6b2
```

The per-file hashes, compiler/object identities, build commands, logs, captures,
ELF headers, validation scripts/results, and accepted comparison are retained
under `build/editor-compile-investigation/current/`, outside source control.
`source-graph-inputs.json` and `generated-inputs.json` contain the input hashes;
`comparison.json` records the accepted pair. Exact compile routes:

```bash
profile_dir=build/editor-compile-investigation/current
NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 NY_TRACE_INLINE=1 \
  perf record -m 1 -e cpu-clock:u -F 99 \
  -o "$profile_dir/baseline.data" -- "$profile_dir/ny-baseline" \
  --no-progress --native-only --opt-level=3 --run=none \
  --output="$profile_dir/editor.o" etc/projects/ui/editor.ny \
  > "$profile_dir/baseline.log" 2>&1
NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 NY_TRACE_INLINE=1 \
  perf record -m 1 -e cpu-clock:u -F 99 \
  -o "$profile_dir/cached-candidate.data" -- "$profile_dir/ny-cached-candidate" \
  --no-progress --native-only --opt-level=3 --run=none \
  --output="$profile_dir/editor-cached-candidate.o" etc/projects/ui/editor.ny \
  > "$profile_dir/cached-candidate.log" 2>&1
```

Validation checked every sampled IP against its executable MMAP, matching ELF
BuildID, executable load segment, symbol start, and decoded instruction boundary.
Matching libc debug symbols resolved private functions; a `strlen@plt` sample
was checked through its GOT relocation and IFUNC resolver. The same software
clock configuration put all 49 known-code control samples inside `known_hot`.
A bounded frame-pointer control validated 245 addresses across 49 samples;
the three-second compiler caller capture validated all 1,457 addresses across
301 samples, with zero loss. It connected the sustained CFG cost to serial
peephole range queries. Captures with a replaced executable, unrelated compiler
edits, or interrupted emission were excluded.

The unchanged CFG helpers' self-sample counts fell as follows:

| Symbol | Baseline | Candidate |
| --- | ---: | ---: |
| `nyr_cfg_build_impl` | 184,633 | 34,874 |
| `nyr_block_last_non_nop` | 34,859 | 1,142 |
| `cfg_label_map_find` | 26,265 | 1,114 |

The query scope now reuses topology and definition positions while reading
predicates from the current instructions. Instruction insertion, value-count
changes, and storage moves invalidate automatically; branch/definition-position
edits require explicit disposal. The serial peephole loop preserves those
positions between inserting rewrites. Singleton intervals skip edge refinement,
and parity candidates reject unrelated definitions before scanning uses.

The new `range_query` driver compared cached and fresh results for singleton
and contradictory bounds, integer extremes, live predicate edits, instruction
insertion, storage movement, new value IDs, and explicit invalidation after
branch retargeting. Its expanded mutation checks linked the exact candidate
optimizer objects. All 80 checked optimizer drivers retained their previous
results: 77 passed; `gvn_pre`, `magic_div`, and `polyhedral_nest` failed on both
the reference and candidate. An earlier aggregate SMT check reached its 40 s
timeout on both the reference and initial candidate, and supplies no passing
execution result. The normal `ny-full` build and focused range, boolean, VRP,
and bounds-check drivers also passed in the shared checkout.

### Native JIT relocation lookup

The loader repeatedly searched for the same definition while deciding data
routing, symbol resolution, and external call stubs. Reusing one exact-name
lookup preserved alias and relocation rules; AArch64 also reused its stub
selection lookup.

An alternating warm-process comparison discarded two warm-up pairs and kept
ten samples per arm. Baseline median was 207.69 ms (200.95–209.61 ms);
candidate median was 149.78 ms (145.07–155.94 ms), 27.9% lower. Every run
returned checksum `1968365`. Another editor compile overlapped the comparison,
so this is a local startup observation, not an arithmetic-speedup claim.
Four focused module/cache/closure fixtures passed; AArch64 execution was not
available on the x86-64 host.

## Binary: scalar recurrence and register homes

The canonical workload's checksum was `3360874588549334`. Its linked native
caller inlined `branch_mix` but retained a scalar dependent recurrence, while
LLVM vectorized independent seeds. An outlined helper's frame size was not
the cost of that inlined timed loop.

Representative counters were about 37.05 million native instructions versus
3.67 million LLVM instructions, with similar branch/cache misses. An earlier
cross-block eligibility relaxation increased instructions from 78.99 to
91.31 million despite overlapping wall-time ranges; it was rejected. Verified
software-clock sampling and matching machine images connected hot home
traffic to loop-carried values; invalid raw-IP profiles did not.

Recorded retained changes included:

- Signed divide-by-two lowering used a logical shift for its 0/1 bias, not an
  arithmetic 0/−1 bias. A negative-seed workload exposed native result
  `3450853999197106` versus the correct LLVM/C `3456948289413084`.
- Unique-edge carry required matching physical colors, an explicit carried
  segment, safe predecessor/successor structure, and no intervening clobber.
  A nominal register color alone did not establish residency.
- SELECT-arm coalescing reused a dying arm's color but preserved a still-live
  arm. The target result stopped spilling; aggregate runtime did not improve.
- Common-addend factoring preserved raw-i64 wrapping, SELECT guards, ownership
  metadata, and effect restrictions. It fired in the measured recurrence but
  did not by itself meet the 5× native/LLVM cap.
- Side-effect-free `SUB_I64` became eligible for if-conversion. Empty branch
  bridges were resolved only with exclusive-edge checks.
- Physical fallthrough deletion required the next block to have no other
  incoming branch. One matched 5+2 comparison improved 2.450 to 2.148 ms,
  with static spills/reloads falling from 9,059/26,933 to 7,854/23,140;
  C dispersion exceeded 120%, so it established no stable NYR/C ratio.
- Constant-division scheduling admitted immediate divisors other than `0` and
  `−1`, preserving trap order and scratch-register clobbers. One 3+1 result
  was 1.897 ms native/388.1 µs LLVM, below the 5× cap; a completed 71-fixture
  serial run recorded 1.91 ms/385 µs and passed its then-current gates.

Other checksum-valid snapshots still exceeded that cap: one supplied
72-fixture result was 2.32 ms/387 µs, and a focused 3+1 result was
2.313 ms/384.8 µs. These are distinct compiler/workload checkpoints, not
proof that the earlier gate result holds today.

### Rejected Binary experiments

| Experiment | Reason not retained |
| --- | --- |
| Broad caller-saved register pool | Call-clobber and later Euler9 failures; profitable-looking local allocation did not prove safe residency. |
| Weighted R8 homes across recognized power-of-two division | Instructions fell from 37.04 to 36.08 million, but candidate/control NYR medians were 2.400/2.383 ms. |
| Loop-carried GPR opt-in | Lower elapsed time was not supported by lower cycles or instructions. |
| Invariant-only loop-home filter | No useful change to the canonical result. |
| Larger full-unroll budget through 18 trips | No demonstrated gain; did not make the nested seed loop vectorizable. |
| CFG clone cap 256→512→1024 | The 512 trial remained above the cap; 1024 regressed to 3.968 ms and 11.04× native/LLVM. |
| More permissive CFG clones, including division/fallthrough shapes | Some synthetic legality tests passed, but canonical runs were unchanged or regressed to 4.26 ms. |
| Paired recurrence unrolling | One direct result was 2.741 ms versus 2.304 ms; not a controlled gain. |
| Reduced if-conversion budget | Branchful code was substantially slower; one sample rose to 5.34 ms. |
| Deferring signed division to machine lowering | Extra allocator pressure; no canonical performance improvement. |
| Literal-depth specialization or selected-multiplier source rewrites | Prevented useful inlining or regressed the unchanged workload comparison. |
| Earlier common-add factoring probe | Approximately 2.39 versus 2.40 ms, without a repeatable gain; distinct from the later guarded retained rewrite. |
| Narrower division clobber mask | Regressed to 2.29 ms and 6.19× native/LLVM. |
| Branch-bridge inversion | Could bypass required register-home saves; passing one checksum was insufficient. |
| Rematerialization preference, wider copy forwarding, loop-update coalescing | No sufficient canonical gain. |

The remaining SIMD opportunity concerned independent seeds, not weakening the
dependent-recurrence legality guard. Existing unroll-and-jam required a known
even trip count, step-one induction, and a pure two-block body; larger budgets
did not satisfy those structural requirements.

## Splay and native register allocation

The supplied early Splay row was 16.9 ms native/4.01 ms LLVM/4.53 ms C;
a 5+2 confirmation was 16.733/3.978/4.414 ms. Later safe-source checkpoints
were around 5.4 ms native, still behind C. These changes were incremental,
not one matched end-to-end A/B.

Useful retained mechanisms:

- Call-safe loop homes were checked per candidate, rather than rejecting an
  entire function because some loop contained a call. Vector overlap was
  likewise checked against each scalar candidate's live range.
- PHI copies were placed in a predecessor with a single successor instead of
  always using an out-of-line trampoline.
- Sticky-home self stores were omitted only when the same home was already
  valid. Direct frame-to-allocated-register loads avoided an extra RAX move.
- Scaled LEA reused resident base/index registers and loaded only missing
  operands. An initial premature encoder return truncated a function; commit
  failure propagation and switch-local exit corrected it.
- Small constant multiplies used direct-register LEA where legal. Negative
  signed-32-bit immediates used the seven-byte encoding instead of ten-byte
  `movabs`; this was a code-size result, not a demonstrated runtime gain.
- Callee-saved address homes and a restricted call-free R8 home reduced some
  traffic without reopening broad caller-save eligibility.
- Cross-block compare-flag reuse required identical operands, a flag-preserving
  trampoline, unique false-edge entry, fused branches, and ended compare-result
  lifetimes. Linked code confirmed the second compare disappeared; timing
  differences remained within variation.
- Direct incoming-argument setup required a unique entry-block frame load, no
  write to its slot, only fixed frame stores between eligible loads, and no
  incoming-register clobber. Only callee-saved scalar GPR destinations were
  eligible; aggregate and more-than-six-parameter signatures stayed excluded.

The timed caller still reloaded five call arguments in one inspected object;
the callee had 89 vregs, 221 segments, 40 frame slots, and a 544-byte frame.
Another large `main` frame contained roughly 24 KB of source buffers; frame
size must not be equated with spills. C kept the traversal inline in its timed
loop, but forced native inlining alone did not provide a repeatable gain.

Default loop homes remained useful: a matched 5+2 control measured
5.46 ms native versus 6.88 ms with them disabled. A separate process counter
comparison recorded 52.05 million native versus 13.91 million LLVM
instructions; those counters included startup and reporting.

### Rejected allocator and Splay experiments

| Experiment | Finding |
| --- | --- |
| Unrestricted same-block home reuse | Focused tests missed a Binary crash. The constrained version excluded frame-loaded values and required disjoint single-segment GPR intervals. |
| Globally relaxed cross-block colored reads | SOR returned `412269742` instead of `1725461952`, independently of the indexed-store bug. Canonical-home seeding remained necessary. |
| Larger loop-home block ceiling | The relevant timed function was already below the existing ceiling. |
| New carry rule for SOR row bases | Homes grew from 1,240 to 1,288 bytes and timing did not improve. |
| Lower vector-unroll factors | No stable SOR benefit; another factor-two trial increased instructions and missed n-body's budget. |
| Immutable parameter-home aliasing | No repeatable runtime gain, despite excluding mutated/address-taken parameters. |
| Unproved incoming-register loads or skipped parameter-home stores | Could read a register after entry had clobbered it; removed despite passing selected checksums. |
| Pre-allocation compare elision | Seven fewer segments, but frame grew from 544 to 576 bytes and runtime stayed about 5.35 ms. |
| Broad pressure-aware inlining or forced `splay`/join inlining | Controls did not establish a repeatable gain; source hints were reverted. Cost approval alone did not prove an inline splice occurred. |
| `i32` loop annotations, cached child loads, equal-immediate copies | No stable gain or no confirmed hot-path code change. |
| Global-buffer source parity | Failed with extreme memory growth before useful timing; failure stage was not isolated. |
| Setup/timed-loop extraction | One refactor broke checksums and rose to 699 ms; explicit-buffer variants restored checksums but overlapped baseline timing. |
| Direct immediate-to-home stores | Focused boundaries passed, but the timed hot path did not exercise the new encoding. |
| More caller-save/address-home candidates | Extra eligibility did not establish a safe, repeatable improvement. |

The earlier compare-residency regression exercised both outcomes of an emitted
integer comparison. A path-only temporary-file descriptor leak also affected
benchmark reporting; closing those descriptors and serializing timed workers
were harness fixes, not Splay optimizations.

## Vectorization, floating point, and addressing

### SOR

Affine row-base splitting moved `(row + column + displacement) << scale`
toward an invariant row pointer plus scaled column. Indexed-address folding
then recognized memory uses correctly: `STORE.dst` was an address use, not a
vreg definition. Linked objects confirmed direct SIB loads/stores, while
readable text assembly still showed extra temporaries. Five row-base home
reloads and the outer grid-swap recurrence remained distinct costs.

Later four-wide f64 loops used AVX2 arithmetic but still spilled scalar address
and induction values. Permitting callee-saved scalar homes across vector
instructions changed two NYR medians from a 3.41 ms baseline to 3.17 and
3.14 ms; C was unstable. Hoisting horizontal neighbor offsets in the benchmark
source changed a 3.269 ms row to 2.871 and 2.889 ms, but this was a source
change with noisy follow-up results, not a stable compiler ratio improvement.
Reducing the vector-unroll cap from eight to four or two did not help.

LICM later hoisted address-producing additions before vector rewriting and
corrupted the checksum (`5515142` versus `1725461952`). Keeping an add in its
loop when its use chain feeds a scalar/vector address restored correctness.
The fixture gained an explicit known-checksum assertion.

### Vector and loop legality

- Four-lane f64 used a 32-byte machine representation: x86-64 emitted VEX
  instructions; AArch64 used paired NEON operations. Strict reductions retained
  ordered rounding. Machine width, storage homes, and induction stride had to
  agree; a four-lane operation advancing by two was a real correctness bug.
- PBKDF2's late-vectorization corruption (`2991`→`3978`) came from reduction
  PHI handling. The dedicated correction and containment checks were distinct
  from earlier speculative rotate/BCE explanations.
- Guard-derived ranges required explicit predecessor/update proofs. Coverage
  included strict/inclusive bounds, step-two traversal, matching multi-latch
  updates, overflow, bypass edges, unknown-effect calls, and overlapping stores.
  Unsupported or conflicting shapes retained scalar execution.
- Restricted unswitching remapped SSA/exit PHIs and labeled cloned fallthrough
  exits, preserving unselected traps. Multiple distinct exits remained rejected.
  Interchange admitted only independently proven private, injective, in-bounds
  stores and invariant stack reads; overlap/overflow cases remained rejected.
  These were legality improvements without measured runtime claims.
- DGEMM's FPR-home dataflow modeled scalar-conversion clobbers per block,
  rather than rejecting all homes because a conversion occurred elsewhere.
- A later PBKDF2 failure reported missing `vec4.reduce_add.i64` emission.
  An attempted scalar emitter/four-word-home change still crashed and was
  reverted. The contemporaneous BCE-matrix crash was not thereby fixed.

### Address and ABI correctness

| Failure | Recorded correction or limit |
| --- | --- |
| Forwarded RAX result followed by a same-home no-op copy | Do not treat the no-op as a consumer that makes the canonical home unnecessary. |
| f64 loads ignored a pending RAX address | Use the same validated scalar-address load path as integer loads. |
| Indexed integer store staged its value in R10, then address formation clobbered R10 | Preserve the value while resolving the full destination address. |
| Vector store ignored a pending SIB address | Resolve the complete memory address for ordinary and fast-path V256 stores. |
| Adjacent single-use ADD/load | Capture the address operands and omit the home only when later uses/live-out do not require it. |
| Runtime ABI tables failed binary search | Place `rt_set_len_raw` in lexical order in both declaration and argument-contract tables. |
| AVR prologue overwrote incoming argument bytes in `r16:r17` | Use caller-clobbered `r26:r27` for stack-pointer temporaries before spilling arguments. |
| Broad immediate arithmetic lowering | Silent Binary/Splay/SSE miscompiles outweighed focused successes; reverted. |
| Profile-driven block layout with synthetic steps but zero positive block heat | Leave the function unchanged; synthetic profitability was not real layout evidence. |

## Memory facts, bounds checks, and scalar optimization

- GVN reused dominating reads of the same local slot only without an
  intervening write. Proven non-local reads could survive unrelated local
  stores; address-taken locals still required alias safety. Effect-only calls
  such as clock reads were not automatically writes to existing memory.
  JSON parser byte loads fell from four to two in the relevant optimized shape;
  manually caching the byte in source did not establish a separate runtime win.
- Append-length reuse needed canonical declaration identity and the same-local
  assignment lineage. Handling only one lowered call spelling missed the fast
  path. BCE reused a single CFG/dominator map across its proof families.
- Append and typed-buffer setter results supplied logical may-alias lineage,
  not permission to assume every handle identical. Store reachability and
  freshness still mattered. Returned-borrow provenance followed one unique
  trusted argument only when effects excluded invalidating writes.
- A successful runtime assertion supplied its true-edge lower bound only if
  dominance/control flow proved execution. Function-local allocation roots
  enabled proofs that observable module-global cells could not support.
  Adding `list<int>` to an empty list alone did not improve the benchmark.
- Moving asserted buffers into helpers changed the source baseline. Historical
  matrix/sort results after that change cannot be presented as a compiler-only
  A/B. A remaining nested non-affine address rejection was separate from the
  removed bounds check.
- Counted zero-fill rewriting required a nonnegative, bounded, non-overflowing
  byte range and preserved observable writes. It did not justify weakening
  alias or trap checks.
- The `escape-loop` kernel eliminated allocation, checks, and the loop while
  preserving checksum `499999500000`; both backends measured below 1 µs.
  A guarded affine reduction also resolved the earlier `inline` loop gap.
- Recursive constant evaluation restored `gcbench` to approximately
  369 µs native/374 µs LLVM/33 µs C. That did not establish safety for broader
  recursion, side effects, malformed flow, or exhausted evaluation budgets.
- Havlak's promoted integer reads were raw i64, but equality had reached a
  dynamic tagging boundary. The focused correction addressed that mismatch;
  passing a reduced case alone had not established the full fixture fixed.

## Runtime arithmetic and allocation

### BigInt and BigFloat

Similar NYR/LLVM times pointed to shared runtime arithmetic and immutable
allocation APIs, rather than only native code generation. One old fixture
version counted 166,814 allocations and 11,207 runtime calls; a later
checksum-matched run counted 158,116 allocations and no reallocations.
Those counts belong to different checkpoints.

A marked execution interval identified these sampled self shares:

| Component | Share |
| --- | ---: |
| `rt_malloc_impl` | 18.07% |
| Heap-limb reading | 9.04% |
| Knuth division | 7.07% |
| `__udivti3` | 3.34% |

Only 509 of 2,927 samples fell inside that interval; zero samples were lost.
Earlier whole-command sampling mostly measured compilation and was unsuitable
for choosing an arithmetic change. A separate single scaled probe put BigInt
square root at 52.5 ms, BigFloat add/subtract at 46.7/36.8 ms, BigInt power
at 32.6 ms, and BigInt add/subtract at 31.0/32.3 ms. These were diagnostic
operation timings, not stable baselines.

Recorded retained changes:

- Exact-u64 BigFloat construction wrote directly into the final object when
  precision sufficed. Two A/B pairs showed about 2.7% fewer instructions, but
  cycles and wall times were inconsistent; no wall-speedup claim followed.
- BigFloat square root reduced Newton steps from
  `4 + floor(log2(precision))` to `1 + ceil(log2(precision))`, keeping a guard
  step: 12→9 steps at 256 bits. Ordinary-precision division used stack scratch,
  larger inputs retained heap scratch, and exact division by two folded into
  the result exponent. Four candidate medians were 33.45–34.50 ms native
  versus a 37.82 ms preceding baseline; spread and C variability limited the
  comparison. Focused boundary/differential/padding checks passed after fusion;
  the earlier 2,027-test cold pass preceded that final fusion.
- Redundant two-limb multiplication probing was removed, and division reused
  its validated divisor-limb view. Five alternating pairs had medians
  756.56→727.67 ms, all checksum `1968365`; an overlapping benchmark made
  the 3.8% difference provisional. Nine focused cold rows passed, not a full
  cold suite.

Rejected arithmetic experiments included direct short-division result writes,
power-of-two quotient shifts/remainder masks, replacing division with a source
shift, and removing a wrapper zero check. One alternating shift/mask comparison
was slower despite 56 retained arithmetic boundary cases. A multiply/divide
fusion pattern could not match: NYIR called module wrappers with expanded
three-word `any` arguments, not the inner runtime helpers. The unused helper
and pass were removed. None of these trials established an arithmetic gain.

### Typed-buffer and allocation lookup

| Retained change | Matched result | Limit |
| --- | --- | --- |
| Compact typed-buffer header read after an epoch-valid provenance-cache hit | Seven interleaved heapsort runs: native process median 98.330→15.220 ms; LLVM 85.664→4.032 ms | All other handles retained the resolver and sanitizer/header checks. This was process wall time, not startup subtraction. |
| Most-recent thread-local allocation-dedup range before eight-entry search | Eleven interleaved BigInt runs: 130.534→126.995 ms; retired instructions 440.425→430.703 million | About 2.2% fewer instructions; did not close the GMP gap. |

Widening the length shortcut to generic `rt_len` regressed Splay and was
removed. Rematerialization/forward-edge residency experiments were also
removed; canonical homes remained required unless every edge and clobber was
proved. LLVM spill-weight source was studied under its license, but no code
was copied into these independent runtime changes.

### Dictionaries and strings

Native dictionary handles already registered with the runtime avoided repeated
validation. Resize thresholds were cached until capacity changed; empty first
integer-key probes bypassed grouped collision search. Collisions, overwrite,
and growth retained the existing behavior.

Word counting moved from split-list construction to token spans, avoiding
repeated-key allocation. Counts used the dynamic integer representation: raw
`1` otherwise collided with tagged zero, losing singleton words. Semantic
coverage included whitespace, repetition, growth, long keys, empty input, and
non-string input. An earlier two-pass tokenizer removed an intermediate token
list without demonstrating a material gain on its own.

## Correctness and verification history

These are completed historical checks, not verification performed for this
page revision.

| Investigation | Observed outcome |
| --- | --- |
| Early cold native parity | 1,122/1,131 passed; later 1,185/1,191, then 1,189/1,191. |
| Subsequent focused fixes | 1,192/1,192 passed, including 61 benchmark correctness fixtures; not a performance result. |
| Untyped closure ABI | A function's own result representation had to prevail over a later call site's raw inference. |
| Sound-source reachability | False global-name resolutions inflated the graph from the intended workload; targeted resolution reduced reachable functions 973→262→148 and avoided the former 8 GiB failure. |
| Sound emission and execution | Emission completed at 220 MiB; the cold matrix covered NYIR VM, MCJIT, ORC, native, and native+VM. Physical 440 Hz playback and a 144,044-byte/72,000-frame WAV were separately checked. |
| Sound numeric specialization | Concrete f64 `poly_blep` avoided a dynamic representation error; a separate old integer-output discrepancy was not a new sound-memory failure. |
| Diagnostic re-lowering | JSON-parser pass dumping crashed although normal execution passed; re-lowering lifetime/output handling was corrected. Diagnostic output volume remained a separate issue. |
| Four-lane f64 and runtime contracts | Focused drivers passed; the full optimizer sweep reached `tv_smt_semantics` and timed out, so no complete sweep pass was established. |
| Later vector campaign | Binary ratios passed, but BCE-matrix and PBKDF2 crashed before samples; the campaign failed overall. |
| Isolated reported benchmark mismatches | Several nqueens, float comparison, Fannkuch, FFT, knucleotide, and Euler9 failures did not reproduce in focused checks; unstable rows stayed unaccepted. A passing rerun was not a newly identified fix. |
| Platform audio and list/cache regressions | Selected cold rows passed; no new audio cause or full-suite fix was inferred. |

ORC perf-map support used object-symbol sizes and loaded addresses. Missing
sizes, unresolved mappings, or invalid raw sample IPs could not be repaired by
accepting a profiler's symbol label. A code dump established what was emitted,
not which instruction consumed time.

## Benchmark validity and coverage

### Harness and reference corrections

- Timed fixtures/routes became serial and stopped inheriting ordinary test
  concurrency. A 66-worker run on a 96-thread host overlapped another suite
  and exhausted memory; its timings were not accepted. Explicit comparison
  worker settings did not make overlapping timing valid.
- Native timing used the compiler's nanosecond phase marker. C wrapper timing
  covered its whole measured body; timer-floor fallback was reported, not
  silently compared as equivalent kernel time. Progress updates did not become
  samples.
- Matrix validation changed from a weak positive accumulator check to the
  exact checksum `1105103687680`; BCE matrix used `49995000`. A plausible
  nonzero result was insufficient for transformation acceptance.
- BigInt references precomputed the same square-root operand. The C floating
  workload changed from `double` to 256-bit GMP, and Ny timing expanded to
  include setup and final power/checksum work. Earlier ratios were superseded;
  explicit C teardown remained a difference.
- Fannkuch's attempted 32-bit parity changed direct raw accesses into generic
  helper calls, regressing native to 9.81 s/72.3 billion instructions. It was
  reverted. A separate vector-disjoint home comparison measured 282 ms versus
  400 ms forced-stack, with 24% fewer instructions.
- Thread-ring aggregate misses did not establish false sharing. `perf c2c`
  sampling was denied by host permissions; that was missing evidence, not
  zero HITM and not justification for speculative mutex padding.
- Completed campaign results and incomplete attempts stayed distinct. The
  historical 71-fixture pass did not validate a later interrupted 72-fixture
  attempt or close subsequently reported per-fixture failures.
- Build integration preserved explicit `NYTRIX_CORE_PATH` and the supported
  legacy CMake input instead of overwriting configured core-bundle paths.

### Workload coverage

The [Goose benchmark design](https://github.com/aardappel/goose/blob/master/bench/design.md)
and [results](https://raw.githubusercontent.com/aardappel/goose/refs/heads/master/bench/results.md)
informed coverage questions, not borrowed performance claims or copied source.
Its useful distinction was flat controls versus allocation, layout, mutation,
and teardown at several working-set sizes.

| Public fixture | Question covered |
| --- | --- |
| [Graph BFS](../../etc/tests/bench/algorithms/graph-bfs.nshape) | CSR construction and traversal at 4K, 32K, and 256K vertices, four starts per size. |
| [Expression tree](../../etc/tests/bench/algorithms/expression-tree.nshape) | Indexed recursive interpretation with a changed leaf before each of 4,096 evaluations. |
| [Records](../../etc/tests/bench/datastructures/records.nshape) | Mixed tag/two-word rows at 32K, 256K, and 2M records, matching C's 24-byte AoS. |
| [Repeat](../../etc/tests/bench/string/repeat.nshape) | String construction at 1, 16, and 128 MB without turning validation into a full byte-scan benchmark. |
| [Word frequency](../../etc/tests/bench/string/word-frequency.nshape) | General string-key aggregation, including owned splitting/counting costs. |
| [Known-vocabulary scan](../../etc/tests/bench/string/word-frequency-scan.nshape) | Allocation/token scanning and a small counting kernel without hashing. |

The repeat fixture's earlier full byte-scan form measured
441/391/12.8 ms and checksum `1122000000`; that was a memory-scan finding,
not a string-copy comparison. Early general word counting measured
1.75 s/1.84 s/3.78 ms before the later direct-counting result, so its old
hundreds-fold gap should not be presented as a current hotspot.

Distinct remaining coverage questions included mixed variable-payload records,
an allocation-heavy S-expression arena, bounded LRU mutation, incremental
adjacency construction, and cache-sized image blur. Flat scans, list growth,
binary trees, numeric kernels, particles, and the fixtures above already
covered several related shapes. Varying vocabulary cardinality/token length,
separating build/query/teardown, and comparing aliased/non-aliased kernels would
answer additional questions; a list of benchmark names alone would not.


## Binary runtime diagnosis (2026-11-03)

### Timed-object inspection

The exact native-tier AOT object was emitted from cached benchmark source and
inspected with `objdump`; the relevant function is `ny_fn_branch_mix`. The
current loop carries `i`, `acc`, and `x` in callee-saved registers. The signed
divide-by-two result (`v36`, machine segment `[19,27]`) has no allocated color
because of register pressure and is stored/reloaded at `-0x150(%rbp)` each
iteration. Several short-lived branch values also spill; updated `x` is
already written directly into `%r15`, so the earlier two-spill description is
stale. `--emit=asm` is a generic listing and is not the timed object.

A SELECT-arm elision prototype was rejected after the relinked object showed no
instruction change. The `select-flag-preservation` runtime fixture passed all
four variants during that experiment, but no performance gain was established.
A pinned 11-sample matched run used native x86-64, opt tier, `-O3`, one warm-up,
and CPU 12. NYR median/min/max were 1.756597/1.728875/1.828070 ms; LLVM was
0.327349 ms and host C 0.568908 ms. NYC was 8.365809 ms. All four checksums
were `3360874588549334`. NYR/LLVM is 5.37x, still above the unchanged 5.00x
fixture ceiling. A prior 11-sample run on another CPU reported 1.750 ms NYR,
0.325 ms LLVM, and 0.558 ms C; retain it as supporting data, not a strict
same-core comparison. The next general optimization should reduce or split the
short-lived divide result's live range under fixed-register clobbers and local
pressure. Do not change fixture work, checksums, compiler comparisons, or gate
limits to close the item.


## BCE vector carry experiment (2026-11-03)

### JIT carry-flush result

Inspection of the relocated JIT image showed that the x86 `BR` and `BR_IF`
encoder cases flushed every seeded vector to its stack home, bypassing the
allocator's `carried && !spill` proof. The encoder now preserves only those
proven vector carries across internal branches; exit segments still flush, and
call boundaries still flush all vectors because vector registers are
caller-saved. The exact `ny_fn_matrix_sum` JIT loop no longer stores its
accumulator on the loop backedge. A pre-loop seed load/store remains, as do
chunk-load staging operations.

A same-source A/B used the `bce-matrix` fixture, NYR `-O3 --native-only
--native-backend=x86_64 --native-tier=opt`, nine process-isolated runs per
version, and matching checksum `14439925760000`. The baseline median was
15.720 ms (15.675–15.744 ms); preserving proven carries measured 12.566 ms
(12.552–12.625 ms), a 20.1% reduction in this fixture. This confirms the
branch-flush cause and a local runtime gain; it does not close the LLVM/C or
full-matrix acceptance gaps. Focused `native_vec4_f64`, `vec8_reduce_loop`,
`loop_vector_metadata`, `regalloc_cfg_edge_carry`, and
`regalloc_loop_phi_carry` regressions pass. The initial run exposed that
`loop_phi_carry_enabled` opted scalar GPR carries in but omitted scalar FPR
carries despite the shared `NYTRIX_GPR_LOOP_CARRY` switch. The gate now covers
both scalar classes; `regalloc_fpr_loop_phi_carry`,
`regalloc_loop_phi_carry`, `regalloc_cfg_edge_carry`, and
`regalloc_unchanged_dag_carry` pass. The existing
`float-loop-register-calls` runtime fixture also passes both `-O0` and `-O3`
native x86-64 variants with `NYTRIX_GPR_LOOP_CARRY=1` and all result/JIT/AOT
caches disabled, covering calls, joins, and conversion scratch use.


A follow-up four-route AOT run used x86-64, `-O3`, peak profile, warm cache,
CPU 28, 11 measured samples and one warm-up. All routes returned checksum
`14439925760000`:

| NYR | LLVM | Host C | NYC | NYR/LLVM |
| ---: | ---: | ---: | ---: | ---: |
| 12.714 ms (12.672–12.803) | 6.334 ms (6.317–6.423) | 1.608 ms (1.594–1.663) | 91.739 ms (91.701–92.090) | 2.01x |

This passes the unchanged per-fixture NYR/LLVM ceiling of 5.00x. It is one
fixture result; the 72-fixture rerun is still required before closing the
cross-fixture item. Raw report: `/tmp/ny-bce-carry-check.json`.


## Rejected constant-division scheduling trial

A scheduler latency estimate that treated positive power-of-two signed divisors as short operations was built and screened against the single `binary` fixture. The 11-sample partial run reported 2.00 ms NYR, 322 us LLVM, and 6.20x NYR/LLVM; the fixture completed successfully. This was not a matched before/after comparison, used GCC for the host-C route, and did not pass the unchanged 5.00x gate. The source change was reverted; retain the result as a rejected, inconclusive experiment. Raw report: `/tmp/ny-binary-divlatency.json`.

## Rejected loop-PHI latch reuse trial (2026-11-03)

Relaxing the x86 loop-PHI edge check to allow the incoming value to reuse the
old PHI register after its final latch read reduced the benchmark's static
spill/reload counters, but made `binary` much slower. With
`NYTRIX_GPR_LOOP_CARRY=1`, `./make bench binary --bench-run 11
--bench-warmup 2 --bench-opt O3 --bench-compile-profile peak
--bench-compare-llvm` measured 7.76 ms NYR (min 7.73 ms), 317 us LLVM, and
1.04 ms host C; checksum `3360874588549334` remained correct. The matched
pre-change carry-flag run was 1.75 ms NYR. The allocator change was reverted;
fewer static spills did not translate into faster execution. Raw report:
`build/bench-reports/bench-20261103-132333-356243.json`.

## Binary hardware-counter profile (2026-11-03)

A seven-run matched benchmark with two warmups measured 1.75 ms NYR, 317 us
LLVM, and 1.04 ms host C for checksum `3360874588549334`. The validated AOT
counter run recorded 26.56M NYR instructions versus 3.66M LLVM instructions;
branch misses were nearly equal (4,518 vs 4,515), as were cache misses
(11,723 vs 12,602). This points to excess NYR instruction work rather than
branch prediction or cache misses. NYR/LLVM remained 5.53x, above the 5.00x
per-fixture limit. Raw report:
`build/bench-reports/bench-20261103-132518-358879.json`.

A later pinned 11-sample run on CPU 12, after rebuilding the compiler, measured
1.773 ms NYR, 316 us LLVM, 1.054 ms host C, and 8.353 ms NYC; all routes
returned the same checksum. NYR/LLVM was 5.60x. Its checksum-validated AOT
counters were 26.57M NYR instructions vs 3.66M LLVM, 4,809 vs 4,611 branch
misses, and 12,490 vs 12,656 cache misses. The accompanying native object
disassembly still shows the signed divide-by-two result stored to and loaded
from `-0x150(%rbp)` in `ny_fn_branch_mix`. The power-of-two latency scheduling
candidate and a just-in-time single-use deferral candidate were not retained:
neither produced a verified matched runtime improvement. The startup `FLOOR`
row is the intentional empty timer control and is excluded from the workload
ratios; the startup-class gate remains a separate unresolved issue. Raw report:
`build/bench-reports/bench-20261103-134322-399654.json`.

The subsequent 72-fixture full matrix used three samples and 21 parallel
fixture workers. All checksums matched, but this run is only a broad regression
screen, not low-contention acceptance. The `binary` row measured 1.762 ms NYR,
318 us LLVM, and 1.047 ms C (5.54x NYR/LLVM); it remains over the unchanged
5.00x limit. The empty `startup` timer control remains `FLOOR`; separately,
the five-workload startup cohort measured NYR/C 1.224x and LLVM/C 1.130x
against unchanged limits of 0.42x and 0.30x. Raw report:
`build/bench-reports/bench-20261103-135250-429124.json`.

The next full 20-worker run reported `bce-matrix` checksum
`-14354741043200` and a 423.587 ms NYR time. Re-running the fixture alone with
`./make bench bce-matrix --no-build --bench-run 3 --bench-warmup 1` returned
the expected checksum at 14.1 ms; a second full 20-worker run also returned
the expected checksum at 14.276 ms. The anomaly is not reproduced, so keep it
open as an intermittent correctness failure. The second full run still failed
the binary, datastructures, startup, and string gates; `append-scaling` measured
7.829 ms NYR vs 3.072 ms LLVM (2.55x, above its 1.50x fixture limit). Report:
`build/bench-reports/bench-20261103-140002-447190.json` (mismatch) and
`build/bench-reports/bench-20261103-140431-460598.json` (clean repeat).

After the benchmark driver's Clang rebuild crashed on `parse.c`, the already
built `ny-full` binary was used for 11 single-process `binary` launches pinned
to CPU 12. Every run returned checksum `3360874588549334`; the median timed
kernel was 1.749 ms (1.730–1.757 ms). This is a NYR-only follow-up, so it does
not replace the matched NYR/LLVM/C report above. The failed rebuild's Clang
reproducer is `/tmp/parse-b761af.sh`.

The rejected register-clobber candidate modeled immediate power-of-two division
as clobbering only RDX, and stopped flushing RCX in the x86 encoder. Its pinned
11-sample run preserved checksum `3360874588549334` but regressed to 1.945 ms
NYR versus 328 us LLVM (5.92x). The timed linked object still stored the signed
divide result to `-0x160(%rbp)` in `ny_fn_branch_mix`; other loop values also
shifted into stack homes. The candidate and its regression test were removed.
Raw report: `build/bench-reports/bench-20261103-135047-424579.json`; object dump:
`/tmp/ny-bench-native-426219-232140242-8.dump`.

## Narrow loop-home spill reduction (2026-11-03)

The `binary` benchmark's pinned 11-sample baseline measured 1.759 ms NYR
(1.737–1.807 ms), 327 us LLVM, and 554 us host C. Inspection of the timed
linked object showed `ny_fn_branch_mix` storing and reloading the result of a
single-use constant divide inside its loop. The x86 loop-home allocator now
admits that one-definition, one-use value alongside CFG-proven loop-carried
values. The existing `x64_readonly_loop_gpr` execution test covers a loop with
a single-use constant-division result.

The pinned candidate run measured 1.739 ms NYR (1.733–1.756 ms), 320 us LLVM,
and 554 us host C; the checksum remained `3360874588549334`. NYR elapsed time
fell about 1.1%, while NYR/LLVM moved from 5.38x to 5.43x because LLVM's
sample was also faster; the row remains above the unchanged 5.00x gate. In the
timed object, `ny_fn_branch_mix` shrank by 11 bytes and its `%rbp` references
fell from 10 to 8 as the quotient store/reload disappeared. Aggregate NYR
instructions were unchanged at about 26.56M; cycles fell from 6.50M to 6.39M.
Whole-program code size changed from 1,141,504 to 1,139,008 bytes, while
compile time moved from 76.80 to 77.28 ms. These small runtime changes are evidence for one
spill reduction, not a resolution of the `binary` gap. A focused 11-sample
`dgemm` run measured 192 us and checksum `1`; no matched before/after run was
made for that fixture. Reports and object dumps are in `/tmp/ny-backend-*.json`
and `/tmp/ny-bench-native-*.dump`.

## AVX1 V256 integer operations and allocation checks (2026-11-03)

The x86 encoder now lowers V256_I64 add/sub on AVX1 by extracting each 128-bit
half, applying SSE2 `paddq`/`psubq`, and inserting the two results back into a
YMM register. The fallback keeps values in canonical homes for the function,
because its XMM0-XMM3 temporaries must not clobber live allocated values. An
executable AVX-only probe checks both operations and reductions. That probe
also found and fixed a NYIR lowering typo where `VEC8_SUB_I64` became machine
ADD. Vector FMA encoding now rejects targets missing AVX or FMA.

The focused `bce-matrix` run returned the expected checksum
`14439925760000` in all implementations. Across three samples, NYR measured
13.343 ms, LLVM 3.240 ms, and host C 1.591 ms. The report measured 8,280 NYR
code bytes, 6.96 ms compiler time, 124 spills, and 274 reloads. NYR/LLVM was
4.12x; this remains a material runtime gap, and this focused run does not
establish a full-suite improvement.

The 11-sample `binary` allocator comparison did not justify enabling scalar
loop-carried GPR residency globally: the default measured 1.721 ms, 1,125,272
code bytes, and 76.67 ms compile time; `NYTRIX_GPR_LOOP_CARRY=1` measured
1.734 ms, 1,125,032 bytes, and 77.19 ms. Both runs preserved checksum
`3360874588549334`. Keep that path opt-in. The verifier-checked segment
allocator and unchanged-value DAG carry remain covered by focused tests; the
whole-function allocation and full-suite quality acceptance remain open.

Focused checks passed: `vec8_reduce_loop`, `regalloc_loop_phi_carry`,
`machine_home_reuse`, `x64_readonly_loop_gpr`, `x64_disasm_vex`, and
`x64_encoder_oracle`. AVX-only V256_I64 bitwise operations use AVX1 bitwise
encodings; AVX2-only integer operations remain rejected when AVX2 is masked.
The AVX1 add/sub fallback is directly executed.

### Loop-carried vector lane preservation

An executable machine-IR regression builds a four-lane `V256_I64` accumulator,
starts with lanes `{1, 2, 3, 4}`, carries it through five loop backedges, then
reduces it on the exit edge. It verifies the allocator marks the vector PHI as
carried and checks the generated x86 function returns `60` with the default
configuration and no carry environment override. This detects loss of any
upper lane.

The first run exposed the bug: the allocator correctly marked the header value
carried, but also set its `spill` bit because the loop had an exit successor.
The encoder interpreted that bit as “not resident at block entry,” reloaded a
stale canonical home, and returned a garbage result. The bit means the value's
home must be published for the exit; the common preg remains valid on both
preheader and backedge arrivals. The encoder now seeds any verifier-proven
carried segment, including one that must be stored at block exit. Vector loop
carry is enabled by default; `NYTRIX_VECTOR_LOOP_CARRY=0` remains available to
isolate regressions. GPR/FPR loop carry remains separately opt-in.

The focused vector, segmented-allocation, GPR/FPR carry, VEX-disassembly, and
encoder-oracle checks pass. The subsequent 73-fixture run preserves checksums
for `bce-matrix` (`14439925760000`), `dgemm` (`1`), and `pbkdf2` (`2991`). It
still reports independent failures in `fannkuch`, `n-body`, and `spectral`, and
the benchmark gates still fail on `binary` (6.09x NYR/LLVM) and the startup
cohort. This validates vector-lane preservation on the focused generated-code
case; it does not establish full backend acceptance. See the archived run above
for complete per-fixture measurements.

The first hot-pressure spill-splitting candidate was removed after matched
seven-sample tests showed no useful change. Both runs were pinned to CPU 13,
used one warm-up, disabled the loop-home overlay in both configurations, and
preserved fixture checksums. For `binary`, control/candidate runtime was
2.769/2.749 ms, code size was 1,125,920 bytes in both, compile time was
79.25/78.01 ms, and spill/reload counts were 9,480/32,617 versus
9,480/32,618. For `linpack`, runtime was 1.373/1.363 ms, code size was
1,147,728 bytes in both, compile time was 79.71/79.57 ms, and spill/reload
counts were 10,176/34,283 versus 10,176/34,284. Runtime ranges overlapped;
the generated assembly text was identical for `binary`. No speedup is claimed,
and the opt-in splitter was removed instead of leaving an unaccepted allocator
path in the encoder.

The segmented allocator verifier now also checks that every register operand
use is covered by a non-overlapping segment. A focused negative test truncates
a live range before an actual use and confirms the verifier rejects it. This
strengthens allocation validation; it does not resolve the independent
`fannkuch`, `n-body`, or `spectral` benchmark failures recorded above.

### CFG carry preservation proof and scalar carry A/B (2026-11-03)

The verifier now requires segments to cover live-in and live-out block
boundaries. For every carried live-in segment, it checks each live predecessor
ends in the same allocated register; a carried edge-copy destination must be
defined by a covered, tied copy. Negative allocator tests mutate the incoming
register, truncate live-in/live-out boundaries, and verify rejection. The
generated x86 loop test executes with scalar GPR carry enabled for inputs
`-1, 0, 2, 3, 4`, preserving each expected result; its emitted function is
152 bytes versus 177 bytes with forced stack homes. The AVX-only fallback,
V256 lane-carry, FMA feature-gate, encoder-oracle, and VEX-disassembly checks
also pass.

Scalar GPR carry remains opt-in. In a matched serial three-sample full-matrix
A/B, 67 fixtures had successful NYR timings in both configurations. The median
per-fixture change was effectively flat (+0.11%); several apparent regressions
above 10% did not reproduce in an 11-sample `dict-set` repeat. Its medians were
47.515 ms with carry off and 46.813 ms with carry on, while sample dispersion
was 13.2% and 14.7%; code size changed from 1,153,528 to 1,153,432 bytes and
compile time was 80.86/80.89 ms. The full run also reproduced existing
`fannkuch`, `n-body`, and `spectral` failures. Its overall gate verdict was
invalidated when the runner rebuilt a shared core artifact, so these rows are
diagnostic measurements, not an acceptance result. Keep scalar carry opt-in
until repeated measurements show a broad, stable gain without material
regressions. Vector carry remains enabled and is covered by the five-backedge,
four-lane executable regression above.

### Binary allocator acceptance (2026-11-03)

For the focused `binary` workload, the final opt-in GPR-carry A/B used the same
source, `-O3`, peak compile profile, warm cache, one warm-up, and 11 timed
samples. Both runs returned checksum `3360874588549334`. With carry off, NYR
measured 5.210 ms (5.182–5.311 ms); with `NYTRIX_GPR_LOOP_CARRY=1`, it measured
5.093 ms (5.072–5.114 ms), a 2.2% median reduction. Compile time was effectively
flat (65.00/64.89 ms), and code size changed by +8 bytes. The aggregate static
spill/reload counters remained 3,407/29,376, so this is not a claim that all
reported spills were removed. The `ny_fn_branch_mix` frame shrank by 128 bytes
in the inspected x64 object, and the result is kept opt-in: default allocation
behavior is unchanged. Focused division/clobber, read-only loop, machine-home
reuse, and backend-acceptance tests pass. This is the practical acceptance for
this item: a repeatable local runtime and frame-size improvement with correct
results, not a new global spill-count threshold. Reports:
`184827` carry off
and
`184902` carry on.

## PBKDF2 optimized correctness failure (2026-11-03)

The focused benchmark reproduces a NYR checksum mismatch in
`etc/tests/bench/numeric/pbkdf2.nshape`. NYR O0/O1 returns `2991`; repeated
O2/O3 runs return varying incorrect checksums. LLVM and host C both return
`2991`. Disabling `loop_vectorize` does not repair the result, while
`--nyr-disable-pass=peephole` at O2 restores `2991`, narrowing the failure to
that pass or its interaction with later lowering. Disabling individual loop
passes and range-based compare folding did not repair it. The per-pass oracle
cannot continue past `iv_elim` because its VM does not support the resulting
`vec4.add.i64` operation. After a full rebuild of `ny-full`, five consecutive
O2 native runs returned `2991`; the focused O3 benchmark (3 samples, 1 warm-up)
also passed with checksum `2991` (NYR 1.65 ms, LLVM 937 us, host C 1.10 ms).
No semantic source fix was identified, so the earlier failing executable may
have been stale. Keep the origin unresolved and reproduce it before assigning
the fault to a pass. Original focused report:
`/tmp/ny-pbkdf2-followup.json`.

## Performance TODO batch (2026-11-04)

Host EPYC 7642, x86-64 native, `-O3`, executables built from each fixture's
NY source and run pinned to one CPU. Medians of 9–15 runs; checksums match the
baseline compiler, LLVM, and the fixture's expected value.

| Fixture | Before | After | LLVM | Change |
| --- | ---: | ---: | ---: | --- |
| `dce` | 31 µs | 0.2 µs | 0.26 µs | select chain folds to one select |
| `spectral` | 125 µs | 87 µs | 79 µs | FPR loop homes across conversions/sqrt |
| `mandelbrot` | 928 µs | 351 µs | 255 µs | same, plus no dead temporary homes |
| `page-rank` | 33 µs | 21 µs | 6.6 µs | same |
| `linpack` | 772 µs | 745 µs | 240 µs | no dead temporary homes |
| `binary`, `dgemm`, `json-parser` | — | unchanged | — | neutral within noise |

- **`dce`:** the recovered `loop_global_promote` and `select_chain_fold`
  passes were not wired into the pipeline after a `git checkout -- .` wiped
  tracked-file edits; they now run after the mid-pipeline cleanup and again
  after store forwarding. Two further fixes were needed. Rule R4 merges the
  `add(add(b, C1), C2)` chains that R3 builds, and use counts now ignore folded
  selects so the merge applies within one round. Separately, DCE's fallback
  `nyr_label_referenced()` ignored PHI predecessor labels. When `max_label`
  exceeded `4 * len + 1024`, which the collapsed 500-step function did, DCE
  merged a fall-through block away and broke the PHI. The 400-step variant
  stayed under the bound, which is why the bug showed only at 500. The
  `dce_phi_label` regression fails without the fix.
- **FPR residency:** convert/sqrt clobber only the loop colors their encoding
  writes, conversions and sqrt may define loop homes, spill-home privacy
  rejects only LEA'd frame slots and stack/aggregate parameters, displaced
  segments move to free colors above the loop colors, and scalar register
  copies use `movaps`. A segment moved to a free color keeps the allocator's
  reload/spill flags. Forcing them stored every block-local temporary to a
  dead home once per iteration (four stores in linpack's row update).
- **Variable-shift recolor miscompile:** loop-carry recoloring again checks
  fixed-register clobbers over the whole interval (`x64_loop_recolor` runs
  with loop GPRs on and off), and the post-allocation verifier rejects a
  colored range live across a fixed clobber (`regalloc_verifier_coverage`).
- **Vector homes:** a single-definition vector whose home is current in its
  defining block is no longer re-stored at every loop latch. That removes
  dgemm's invariant-broadcast stores but not its runtime; see the TODO.
- **Boundary coalescing:** the arithmetic/SELECT boundary-tie hook had already
  been reduced to a stub returning false; it is removed. `binary` and
  `json-parser` are unchanged.

All 103 optimizer test groups pass, and all 73 bench fixtures match baseline
checksums. This is not a full-gate run.

## Gate follow-up (2026-11-04, late)

A serial `./make bench` (3 runs + 1 warm-up) failed the algorithms,
datastructures, startup, and overall gates on three fixtures:

- `recursive-fibonacci` 2.60x NYR/LLVM (budget 2.00x): statement lowering had
  lost the AVX2 optimizer option and the post-optimization recursive-call
  rewrite (`nyr_tail_call`, which turns `f(n-1) + f(n-2)` into an accumulator
  loop). Both are restored in `lower/stmt.h`; the kernel went from 3.7 ms to
  0.32 us. The AVX2 option also feeds the vector passes (`bce-matrix` 11.0 ms
  vs 12.7 ms before).
- `dict-set` 1.95x (budget 1.50x) came from concurrent builds; alone with
  7 runs it measures 30.9 ms vs 25.8 ms LLVM (1.20x).
- `binary` 5.33x (gate 5.00x). Keeping MUL by 5/9 as one LEA and the new
  `loop_sub_iv` pass (`inv - i` becomes a decrementing IV) took the inner loop
  from 19 to 18 instructions and NYR from 1.60 to 1.45 ms, but LLVM measured
  266 us in that run (5.47x). The remaining gap is LLVM's outer vectorization.

All 103 optimizer groups pass and all 73 bench fixtures match baseline
checksums with these changes.

## Outer vectorization residency (2026-11-05)

Targeted serial run, `ny-test --bench --bench-run 7 --bench-warmup 2
--bench-compare-llvm`, on a host that other sessions were also loading (load
average about 14), so these numbers are advisory, not gate evidence:

| Fixture | Route | NYR | LLVM | NYR/LLVM |
| --- | --- | ---: | ---: | ---: |
| `binary` | default | 1.45 ms | 266 µs | 5.45x |
| `binary` | `NYTRIX_OUTER_VECTORIZE=1` | 903 µs | 266 µs | 3.40x |
| `dgemm` | default | 154 µs | 26 µs | 5.97x |
| `linpack` | default | 739 µs | 240 µs | 3.07x |

Before this batch, the opt-in widened `binary` loop took 2.1 ms. Changes:

- Vector latch recurrences (`x' = f(x)` computed in the latch) share the PHI
  register when every read of the old value precedes the new definition. The
  allocator proposes this pairing and the verifier checks it again
  (`verify_vector_latch_recurrence`). The latch definition may read the PHI
  as `src0` of a single add/sub/and/or/xor.
- The loop-carry coalescer tries the PHI's own header color first, picks the
  color with the cheapest evictions (a segment with no access in its block is
  nearly free), and coalesces each PHI once per header. Nesting is judged on
  the natural loop body, not on every block that reaches the header.
- Coalescing can evict block-local temporaries. Those that were
  register-only now get a second chance at any color that is still free.
- Loop-invariant vectors stay resident across the loop when every body
  segment shares one color with the preheader.
- Functions with no scalar floats (SysV, AVX2) also allocate xmm4..xmm7 to
  vectors, giving 10 vector colors instead of 6.
- Encoder: three-operand VEX binops, register-form `vbroadcastsd`, and SET1
  results written straight into their assigned register.

Correctness fixes found along the way:

- The home-path COPY of a four-lane vector used a 16-byte `movdqu`, which
  left lanes 2–3 stale. This caused the "wrong `binary` checksum" that had
  been blamed on the latch relaxation.
- `mach_ymm_broadcastsd_mem` dropped VEX.R for ymm8+.
- `mach_lower_phi_edges` counted only leading contiguous PHIs, so a PHI after
  a DCE-made NOP lost its edge copies.
- LICM hoisted into a preheader laid out after its loop, which broke the
  verifier's def-before-use order once the outer vectorizer appended its
  vector loop.

The full `lang` + `code` suite (2139 fixtures) showed no regressions. The 16
failures it reported fail on the baseline compiler too, or pass when run alone.

Outer vectorization is now on by default (`NYTRIX_OUTER_VECTORIZE=0` turns it
off). Enabling it took four more fixes on the in-process native route
(`--native-only`, native JIT, native result oracle), whose scalar machine
emitter hands vector functions to the NYIR emitter:

- `nyr_phi_elim` lowered four-lane PHIs through 8-byte locals. They now go
  through a 32-byte entry `alloca` with `vec8.store/load.i64`.
- The NYIR emitter had no `vec8.sar.i64`. It is now lane-wise `sarq`, with
  four-slot homes.
- The home-path four-lane f64 SET1 broadcast from the source's stack home
  even when the scalar existed only in its float register, which made `2.0 *
  B` read 0 in `loop-tail-passes`.

The full suite with the vectorizer forced on matched the baseline failure
list exactly (2124/2139). The full serial bench that followed
(2026-11-05 06:25, recorded above) put `binary` at 919 µs vs 266 µs LLVM
(3.45x) and passed every per-class gate. Every checksum that ran matched.

Later the same day:

- `nyr_licm_post_vectorize` runs right after the late loop vectorizer. It
  allows hoisting invariant address arithmetic, which the pre-vectorization
  LICM keeps in place on purpose. That alone took `linpack` from 760 µs to
  320 µs.
- CSE now merges identical class-refining pointer copies.
- Home-path f64 broadcasts load the lane with `movsd`, which forwards, then
  use register-form `vbroadcastsd`. Zen 2 cannot forward a store into a
  broadcast load.
- A dedicated row-update vectorizer for `linpack` (runtime dependence guard,
  scalar remainder) was correct but 2x slower and was removed. The next `k`
  step reads the vector-written row back with scalar 8-byte loads, which
  cannot be forwarded from the misaligned 32-byte stores.

The full suite showed no new failures. The serial full bench at 13:21
(`CC=clang`, recorded above) passed 72/72 fixtures and every per-class gate:
`binary` 3.31x, `linpack` 1.33x (329 µs vs 248 µs), and `dgemm` 5.26x
(137 µs vs 26 µs; ungated, host C under 1 ms).

### AVX/SSE transitions, vector accumulators and exit homing (2026-11-05 evening)

- Legacy SSE executed while the upper YMM halves are dirty stalls on this host
  (C microbenchmark: 81 ns per transition against 6.4 ns with VEX scalar code).
  The encoder mixed the two. On AVX targets the scalar and 128-bit SSE helpers
  now emit VEX forms (`NYTRIX_NO_VEX_SSE=1` restores legacy encoding), and
  `vzeroupper` runs before calls, before returns, and at entry to scalar-float
  blocks where no 256-bit value is live. `sor` fell from 2.67 ms to 0.70 ms
  and `dgemm` from 137 µs to 37 µs; this was the `dgemm` stall that had no
  explanation without IBS.
- Fixed-trip integer reductions keep a four-lane vector accumulator again
  (`NYTRIX_NO_VECTOR_ACCUMULATOR=1` disables it): `bce-matrix` 11.0 ms →
  3.4 ms.
- Loop-carry exit homing ignored liveness. A carried vector segment was
  published to its home before every loop branch whenever some successor had
  no carried arrival, including successors where the value is dead.
- Packed i64 `sar` by 1 is `(a >>> 1) | (a & SIGN)`, two cycles of latency
  instead of three.

`binary` stays at about 3.2x LLVM. Its LLVM reference is itself vectorized
across the outer loop: `NYTRIX_OUTER_VECTORIZE=0` compares our scalar loop
with LLVM vector code and is not a meaningful gate. The remaining gap is
instruction count (9.6M against 3.2M at higher IPC, 2.67 against 2.04), mostly
in the signed `x / 2` sequence and the xor/and/xor select.

The full serial bench at 18:36 (`CC=clang`, 3+1, load average 4–12, recorded
above) passed every timed fixture and every per-class gate. Overall NYR/LLVM
was 1.29x and the maximum 3.17x on `binary`. Other results: `dgemm` 1.52x
(39 µs vs 26 µs), `sor` 1.21x, `linpack` 1.49x, `bce-matrix` 0.53x.

Built and validated after that run (224 of the 225 vector, ABI and optimizer
fixtures pass on a private build; `f64-scalar-abi` fails on the release build
too, from the concurrent `--run` argv change):

- `vec8.shr.i64` (`NYR_MACH_VSHR`, `vpsrlq`). The outer vectorizer emits it
  for `(x >> 63) & 1`, which replaces four instructions of the signed-division
  bias: `binary` 880 µs → 831 µs.
- Functions whose float allocation and loop-pinned floats stay in xmm0..xmm3
  give xmm4..xmm7 to vectors (`NYTRIX_NO_SHARED_XMM4_7=1` disables it). The
  fourth `dgemm` broadcast now stays in a register (about 35 µs).
- `NYTRIX_OUTER_VEC_INTERLEAVE=2` widens to two four-lane registers per value.
  It is correct but slower (1.40 ms) because the second register set spills.
  Off by default.

Next:

- [Performance](performance.md) — measurement method and profile validation.
- [Changelog](../CHANGELOG.md) — verified release records.
