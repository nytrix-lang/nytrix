<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":140,"summary":"Measure compile time, startup, runtime, and contention with reproducible workloads and validated profiles."} -->
# Performance

Hold the workload and its result fixed when comparing performance. Separate
compilation, startup/linking, execution, and memory before choosing a profiler.
Timing identifies a slow workload; profiling and code inspection locate its cost.

Prerequisites: a workload with a correctness check and a stable execution route.

## Separate the costs

- Use `-time` for compiler phase timings or `--profile-time` for repeated
  compilation samples.
- Compare the default run path with `--run=aot` when startup dominates.
  `--run=aot` builds a native executable before executing it.
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
./make bench binary --bench-run 7 --bench-warmup 2 \
  --bench-opt O3 --bench-compile-profile peak --bench-compare-llvm
```

Use the fixture stem reported by `./make bench --list-bench`. Run the same
source and result check on each route. The benchmark runner executes comparison
routes sequentially within a fixture and runs fixtures serially by default;
explicit fixture workers prioritize throughput and can contaminate latency
comparisons. Keep unrelated builds, tests, and benchmarks out of the
measurement interval.

The default `NYC` column measures execution of the fixture's C workload
compiled by NYC. NYC emits assembly; the selected host C driver assembles and
links that assembly without compiling the workload's C source. Source
extraction, compilation, linking, process startup, and discarded warm-ups are
outside the reported kernel time. `C` and `NYC` use the same source, timing
wrapper, arguments, requested sample counts, fixture flags, and libraries.
NYC uses its supported optimization flags; the host reference additionally
uses `-march=native`. Both compiler identities and base flags are recorded.
Compiler throughput is not a benchmark result.

NYC checksums participate in correctness checks. Build failures, execution
errors, missing checksums, and mismatches remain visible and fail the run;
there is no fallback to host-compiled C for NYC. JSON and CSV expose
`nyc_ms`, sample count, checksum, exit status, timing source, spread, and
diagnostics. No compile-time fields masquerade as runtime.
`NYC/C` is NYC execution time divided by host-C execution time: below `1.00×`
means NYC is faster. Its colors use that lower-is-better direction. As with
the other runtime ratios, timer controls, mismatched checksums, failed routes,
low-signal samples, and unstable measurements are not comparable.

For a fast correctness/CLI smoke, use one measured run and no warm-up; partial
reports are advisory, not performance-gate evidence:

```bash
./make bench string --bench-run 1 --bench-warmup 0
```

For a C reference, match the measured work and representation: allocation size,
zero-initialization, retained-object lifetime, and input layout must agree with
the Ny workload. Otherwise a ratio may measure different allocation or memory
traffic rather than backend performance.

For suite throughput rather than isolated timings, opt into fixture workers:

```bash
./make bench --jobs 4
```

Parallel fixture workers can add contention and produce `NOISY` rows. Use the
serial default for comparisons and gate verdicts.

A fixture's `bench_elapsed_ns=`, `elapsed_ns=`, and `elapsed_ms=` output markers
are explicit self-timed samples. A mismatch, timeout, unavailable sample,
fallback marker, or `LOW` timer-floor row is not a speed result. The empty
`startup` fixture is the timer-floor control and is labeled `FLOOR`; it must
remain empty and is not a runtime workload. A partial-set geomean does not
establish a full-suite gate. Keep short C-reference rows separate from gated
ratios when the runner's cutoff excludes them; do not change thresholds or
checksums to hide a regression.
The table retains sub-1µs measurements in nanoseconds and marks workload rows
`LOW`; the empty `startup` floor control is labeled `FLOOR`. Its workload
ratios are `n/a`, even if scheduling delays push a timer sample above 1µs.
Other ratios remain unavailable until every route in that comparison clears
the 1µs signal floor. JSON and CSV retain nanosecond precision in millisecond
fields; Markdown uses the same unit-aware timing text as the terminal.
`NOISY` remains a separate status for unstable samples.
Report median, spread, and sample count, not only the fastest run. The bench
noise verdict uses scaled median absolute deviation, so an isolated scheduler
pause remains visible in the sample range without discarding a stable median.
Persistent spread still marks the row `NOISY` and excludes its ratios.

## Bound compiler cache storage

`NYTRIX_CACHE_DIR` selects the cache root. Its managed `std-src`, `jit`, and
`modules` directories each default to a 256 MiB budget and 4096 published-file
limit. `NYTRIX_CACHE_MAX_MB` accepts a positive integer in MiB and changes the
byte budget, not the entry limit. Eviction uses publication-file modification
times, not access times; it is not an LRU policy.

Completed artifacts publish atomically under a directory lock. Core/module
bitcode selected for an in-flight compilation holds a read lease until linking
consumes it. A busy publication lock declines optional persistence instead of
stalling another compilation. Missing or evicted optional artifacts are rebuilt;
an artifact larger than the budget is used without being cached. Writers also
reclaim recognized temporaries whose owning process has exited. Files outside the
managed cache formats, symlinks, and subdirectories are not eviction targets.
Active temporary output, package downloads, profiles, and diagnostic reports
are outside these published-artifact limits.

Bitcode stays binary by default. A failed bitcode write does not print and
reparse the entire module to retry the same disk operation. Frozen compiler
snapshots with preserved build timestamps reuse the same standard-source key
instead of partitioning that cache by PID-specific executable names.

Suite wrappers retain profiles and diagnostics but release frozen `core.ny`,
`core.bc`, and metadata snapshots after use, including interpreter error exits.
On POSIX, a later wrapper invocation also releases these inputs for dead suite
owners. The functional runner's second bitcode name hard-links its already
frozen snapshot where supported; it never hard-links the mutable build output.

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

## Optimizer profiles

One profile name drives the whole optimization pipeline: `speed`, `peak`,
`balanced`, `compile`, `none`, and `size`. The CLI form is
`--opt-profile=MODE`; `peak` favors native benchmark speed over compile
latency, `compile` and `none` minimize compiler work, and `size` targets
smaller output. The CLI validates the name; tools and child processes
inherit the choice through `NYTRIX_OPT_PROFILE`, which accepts the same
names and treats unrecognized values as the custom profile. The in-tree
executable's `--help` is the authority for the modes in the current build.

## Compile-only profiling

An explicit output path prevents an early emit-only exit from being mistaken
for completed native code generation. This example requires an existing
`case.ny` and a task-owned output directory:

```bash
NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 \
  perf record -e cpu-clock:u -F 99 --call-graph dwarf,4096 \
  -o build/compile-profile.data -- \
  build/release/ny-full --no-progress --native-only --opt-level=3 \
  --run=none --output=build/profile-output.o case.ny
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

### Native kernel loop

This loop attributes a native fixture kernel to instructions. It uses only
user-space counters, which `kernel.perf_event_paranoid=2` permits.
`--native-only --output` writes a relocatable object, not an executable, so
link through the default driver while still selecting the native backend:

```bash
awk "/source ny <<'NY'/{f=1;next} /^NY$/{f=0} f" \
  etc/tests/bench/algorithms/binary.nshape > tmp/probes/binary.ny
build/release/ny -O3 --native-backend x86_64 --run=none \
  --output=tmp/probes/binary tmp/probes/binary.ny
tmp/probes/binary                          # checksum and elapsed_ns
perf stat -r 5 -e cycles:u,instructions:u,branch-misses:u tmp/probes/binary
objdump -d -M intel --no-show-raw-insn --disassemble=main tmp/probes/binary
```

Startup is a fixed cost in whole-process counters. To get per-iteration
cycles and instructions, scale only the probe copy's trip count, for example
10x `rounds`, and divide by the iteration count; never edit the fixture.
Compare instructions per iteration and IPC before and after a change. At IPC
near the core's sustained width, removing instructions matters more than
shortening latency chains. Check that the disassembled loop is the one the
counters measured, and confirm the checksum on every build.

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

The [2026-10-22 validated comparison](measurements.md#validated-editor-compile)
identified repeated CFG construction in peephole range queries. Reusing topology
and definition positions within that pass preserved the complete object and
inline logs. The profiled interval fell from 4,180.842 to 1,423.179 seconds;
the archive records input hashes, BuildIDs, commands, validation, and limits.

A performance-only candidate must preserve input/object hashes and inline
decisions while measuring the same build configuration, route, target, and caches.
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
allocator/inliner experiments. Those earlier unresolved instruction addresses
keep their sampled shares provisional.

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

Next:

- [Testing](testing.md) — run the workload's correctness check.
- [Recorded measurements](measurements.md) — dated results and limits.
- [Status](status.md) — select the route being measured.
