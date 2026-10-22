<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":140,"summary":"Measure compile time, startup, runtime, and contention with reproducible workloads and validated profiles."} -->
# Performance

Hold the workload and its result fixed when comparing performance. Separate
compilation, startup/linking, execution, and memory before choosing a profiler.
Timing identifies a slow workload; profiling and code inspection locate its cost.

## Separate the costs

- Use `-time` for compiler phase timings or `--profile-time` for repeated
  compilation samples.
- Compare the default run path with `-run` when startup dominates. `-run` builds
  a native executable before executing it.
- For steady-state execution, repeat the same workload and verify its result.
  Different algorithms or input sizes do not isolate compiler performance.

`--perf=compact` gives a short runtime profile; `--perf=verbose` includes more
detail. Check the in-tree compiler's `--help` for available profile controls.
Repository benchmarks measure compiler-generated workloads, not necessarily
the application cost you need to improve.

## Establish a comparison

Record source and compiler identities, target, backend, optimization/profile,
cache state, input, checksum, sample count, and timed region. Build baseline
and candidate from the same source except for the intended change. Measure
quiet runs on an otherwise idle host; tracing and sampling are attribution
runs, not timing baselines.

```bash
NYTRIX_BENCH_JOBS=1 ./make bench binary --bench-run 7 --bench-warmup 2 \
  --bench-opt O3 --bench-compile-profile peak --bench-compare-llvm --jobs 1
```

Use the fixture stem reported by `./make bench --list-bench`. Run the same
source and result check on each route. The benchmark runner executes comparison
routes sequentially within a fixture; additional fixture workers measure
throughput and can contaminate latency comparisons. Keep unrelated builds,
tests, and benchmarks out of the measurement interval.

A mismatch, timeout, unavailable sample, fallback marker, or `LOW` timer-floor
row is not a speed result. A partial-set geomean does not establish a full-suite
gate. Keep short C-reference rows separate from gated ratios when the runner's
cutoff excludes them; do not change thresholds or checksums to hide a regression.
Report median, spread, and sample count, not only the fastest run.

## Choose the measurement

| Symptom | Evidence | Interpretation |
| --- | --- | --- |
| Slow cold compile | Phase/pass times and a compile-only CPU profile | Match the source graph and disabled caches. |
| Slow warm compile | Cache-hit/miss records and repeated phase times | Distinguish lookup/validation from code generation. |
| Native runtime gap | Matching checksums, timed linked object, and LLVM/native code | Locate calls, checks, conversions, homes, and loop/vector differences. |
| High RSS | Per-process/per-fixture peak and allocation or retained-IR evidence | A child-process high-water mark is not a sum or per-function attribution. |
| Poor thread scaling | Fixed worker/affinity matrix and cache-line contention evidence | Distinguish true sharing, false sharing, serial work, and bandwidth. |
| Wrong benchmark result | Exact output, checksum, route, and first divergence | Resolve correctness before interpreting time. |

`--bench-show-ir`, `--bench-show-asm`, `--bench-show-passes`, and
`--bench-profile` are diagnostic controls. The last reports compiler phases,
not a generated-loop runtime profile. Use [Compiler debugging](debugging.md)
for pass, value, ABI, and allocator filters.

## Compile-only profiling

An explicit output path prevents an early emit-only exit from being mistaken
for completed native code generation. This example requires an existing
`case.ny` and a task-owned output directory:

```bash
NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 \
  perf record -e cpu-clock:u -F 99 --call-graph dwarf,4096 \
  -o build/compile-profile.data -- \
  build/release/ny-full --no-progress --native-only -O3 -emit-only \
  -o build/profile-output.o case.ny
perf report --stdio --input build/compile-profile.data --sort=symbol --no-children
```

Check the exit status and emitted artifact. Keep cold/warm measurements distinct;
record the standard-library inputs and source byte count. Deep verbosity can
emit megabytes of logs and materially change compile time.

Pass statistics help attribute growth before a later expensive phase. Compare
function instruction/value/block counts as well as time. Repeated name lookup,
CFG construction, type-map building, or inline-summary scans are hypotheses
only after the matching profile identifies them.

## Runtime attribution

Compile the intended executable first and profile that executable directly.
Profiling a compile-and-run wrapper can attribute compiler work instead of the
kernel. Whole-process counters include startup and reporting unless the capture
is explicitly restricted to a marked interval.

```bash
perf stat -e cycles,instructions,branches,branch-misses -- ./program
perf record -e cpu-clock:u -F 99 -g -o build/runtime.data -- ./program
perf report --stdio --input build/runtime.data
```

For JIT captures, enable `--jit-perf-map` and confirm that the process's
`/tmp/perf-<pid>.map` contains the hot function. `NYTRIX_JIT_CODE_DUMP=1`
retains relocated code and its range sidecar. Match sampled addresses to that
image or the exact executable's MMAP records and BuildID.

Validate the same event/profiler combination on a known-code control. A report
assigning all samples to one symbol is not trustworthy if raw IPs lie outside
that symbol's mapping. Record lost samples and sample count; zero loss alone
does not prove correct attribution. Unknown or unresolvable addresses make
sample-derived percentages inconclusive. Aggregate counters, disassembly, and
quiet timings remain separate evidence with their own limits.

A readable `.s` file is not necessarily the linked encoder's output. Match
allocator vregs, frame slots, and code bytes to the artifact actually timed.
Frame size alone does not identify a runtime bottleneck; a frame can contain
large source buffers rather than spills.

## Cache-line contention

Use `perf c2c` for a thread-scaling or shared-line question, not ordinary
single-thread instruction cost. PMU events and HITM categories depend on CPU,
kernel, and perf version. Record `perf --version`, `uname -r`, `lscpu`, CPU
placement, and the available events before capturing.

```bash
perf c2c record -e list
perf c2c record -v -e mem-ldst -o build/contention.data -- ./threaded-program
perf c2c report --stdio --input build/contention.data
perf c2c report --stdio --input build/contention.data \
  --show-all --full-symbols --double-cl -d tot,lcl,rmt,peer -N
perf c2c report --stdio --input build/contention.data \
  --coalesce pid,tid,iaddr,dso --full-symbols
```

Select an event the host exposes. Failure to open an event is an access
or support limitation, not zero contention. Do not change system-wide profiling
permissions as part of a benchmark run without authorization.

Read the report in this order:

1. Rank contended cache lines by local/remote HITM and total activity.
2. Record each hot line's address, offsets, participating threads/CPUs, reads,
   writes, and instruction addresses.
3. Resolve instructions against the exact binary and derive the owning field
   from object layout. A source line alone may touch several fields.
4. Distinguish writers to the same logical state from independent fields sharing
   a line. Padding cannot remove true-sharing handoffs.
5. Compare single-thread, normal-worker, and topology-separated placements with
   fixed input/checksum. High HITM without a corresponding throughput effect is
   not sufficient reason to change layout.

The recorded EPYC 7642 environment exposed `mem-ldst` as `ibs_op//u`, but capture
was denied under its profiling permissions. That was an unavailable measurement,
not evidence of zero HITM. Rerun quiet benchmarks after any attribution capture.

## Editor compile investigation

The current [open item](../TODO.md#editor-compile-time) concerns large editor
compilation. A performance-only candidate must preserve input/object hashes and
inline decisions while measuring the same build, route, target, and caches.
Prefer bounded `cpu-clock:u` captures validated against the matching binary;
the prior oversized DWARF recording lost samples.

A recorded 2026-10-21 comparison on revision `2f25b3ac` plus local changes
emitted the same 25,226,512-byte object before and after a single-segment lookup
fast path in `nyr_mach_regalloc_segment_at`. The inclusive bounds check matched
the previous binary search. The 62,710-line inline logs and object SHA-256
matched across builds:

```text
8fbe6319f7ce15508f8eddb16c210fa789bf0b48242d1dd99efd2517d2824b9f
```

The before/after sampled shares for segment lookup were 22.35% and 19.43%, with
zero lost samples; some other raw IP frames remained unresolved. Those shares
were directional, not exact time accounting or a current speed claim. The
recorded compiler BuildIDs were
`b21ef392c90bcea1bad92991a88447db0186492c` and
`de14b7480c4feacbd9dc6969786bc482d369baed`.
The [measurement archive](measurements.md) retains the full record and rejected
allocator/inliner experiments. Its old task labels do not close the current
acceptance check.

## Recording a result

Keep the exact command, identities, checksum, time/memory samples, spread,
attribution validity, changed invariant, and relevant correctness checks.
Separate an accepted change from a rejected or provisional experiment. A new
hypothesis should name an observable code/profile change and a discriminating
check, not repeat an old unsuccessful budget increase.

Keep only unresolved acceptance in [Open work](../TODO.md). Put concise verified
release-facing changes under the latest existing changelog version; changing
that version is a separate user decision. Preserve large capture files outside
source control and document their limitations when results are summarized.

See [Testing](testing.md) for repeatable fixtures and
[Status and execution routes](status.md#execution-routes) for backend selection.
