<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":140,"summary":"Measure a real workload, preserve result checks, and compare identical build paths."} -->
# Performance

Measure the program you intend to improve. Separate compile time, startup, and
runtime. Keep the input, flags, cache state, and result check fixed.

## Build the right artifact

| Command | Use |
| --- | --- |
| `ny file.ny` | Ordinary edit-loop execution. |
| `ny -run file.ny` | Build and run a temporary native executable. |
| `ny -O2 -o app file.ny` | Produce an ordinary native release artifact. |
| `ny -O3 --profile=peak -o app file.ny` | Trade compiler time for a peak native build. |
| `ny -O3 -flto -o app file.ny` | Enable aggressive whole-program NYIR optimization before native lowering. |
| `ny -time file.ny` | Separate compiler phases from execution time. |
| `ny perf` | Run maintained performance checks. |
| `./make optcheck` | Compare maintained deterministic C, GMP, and Nytrix workloads. |

For reproducible cross-tool measurements, use `./make optcheck` for deterministic
C/GMP/Nytrix kernel comparisons or `ny perf compare` to compare executable
targets and capture JSON/CSV/Markdown reports.

Run a compiled artifact repeatedly when runtime is the question:

```bash
ny -O3 --profile=peak -o build/cache/bench/app bench.ny
build/cache/bench/app
```

## JIT vs AOT startup

`ny` provides an explicit execution mode for short programs:

- `--run=auto` chooses the best default execution path.
- `--run=aot` builds a temporary native executable and runs it, which avoids LLVM JIT startup overhead for short-lived workloads.
- `--run=jit` uses the LLVM JIT path explicitly and is useful when compile-and-run latency is dominated by runtime rather than frontend/startup costs.

For timing short programs, prefer `--run=aot` or `-O2 -o app` to avoid the fixed LLVM JIT startup cost unless you need the JIT path.

## Write a checkable workload

```ny
use std.core

def data = [1, 2, 3, 4]

fn work(list<int> xs) int {
   mut int total = 0
   for x in xs { total += int(x) }
   total
}

assert(work(data) == 10, "benchmark result")
```

Keep setup outside the timed operation. Keep one assertion that proves the
workload result. A faster wrong result is not an optimization.

## Compare commands fairly

```bash
hyperfine --warmup 2 --runs 10 \
  'build/cache/bench/app' \
  'build/cache/bench/app'
```

Compare the same source, input, flags, machine state, and cache state. Report
the mean, variation, and exact command. Measure cold and warm runs separately.

## Inspect before changing code

```bash
ny -O3 -time -dump-stats bench.ny
ny -O3 --emit-ir=build/cache/bench/app.ll -emit-only bench.ny
ny -O3 --emit-asm=build/cache/bench/app.s -emit-only bench.ny
```

Use a profile or direct measurement to identify the owned cost. Inspect an
artifact to test a hypothesis. Do not infer execution from emitted assembly.

## Language cost model

The common hidden costs are representation changes and ownership/container
work, not arithmetic syntax itself. Treat these as the default performance
contract unless a narrower typed path is proven by the compiler:

- Typed `int`/`f32`/`f64` arithmetic stays in raw scalar form. Dynamic `any`
  arithmetic may require tag checks, boxing/unboxing, runtime dispatch, and for
  integer results outside the small tagged range, BigInt allocation/promotion.
- Explicit `bigint` arithmetic may allocate result storage. Converting back to
  fixed-width integers is an explicit narrowing operation; do not assume a
  dynamic integer expression is allocation-free merely because benchmark inputs
  happen to be small.
- String construction, concatenation, normalization, and list-producing helpers
  may allocate. A borrowed/raw byte view avoids a copy only when the owning API
  explicitly returns a view and its lifetime remains valid.
- Lists/dicts/sets are managed objects. Growth can allocate or reallocate backing
  storage. `clone` creates detached mutable storage. Value-typed tuples/structs
  cross ordinary function boundaries by value; `layout` is instead an ABI-shaped
  physical record and may require ABI-mandated copies when passed/returned by
  value.
- A closure value keeps captured bindings alive. Creating or returning a closure
  can therefore extend captured lifetimes and may allocate closure/environment
  storage; a non-capturing direct function call is the cheaper baseline.
- Iterator/map/filter/reduce helpers are not a promise of allocation. Optimized,
  monomorphic loops may eliminate adapter layers, but code that depends on that
  should verify the optimized NYIR/machine path and keep a benchmark guard.
- FFI wrappers can add boxing, ownership bookkeeping, string conversion, and
  aggregate copies. A typed raw C call is the zero-wrapper baseline, but only
  when its ownership/effect/ABI contract is actually correct.

Use `-dump-stats`, NYIR dumps, native tier reports, and allocation/runtime-helper
counters to verify which of these costs remain in a hot function. Performance
guidance describes current compiler behavior; it is not permission to weaken
source semantics to obtain a fast path.

`--native-tier-report[=PATH]` is the strict static-island inspection mode. In
addition to aggregate backend facts, it emits one `static_island` row for every
emitted native function. Each row counts dynamic operations and tag checks,
box/unbox conversions, heap-allocation effects, runtime-helper calls, retained
bounds checks, direct and unresolved/indirect calls, unknown effects, unresolved
alias-sensitive memory sites, vectorization attempts/rejections/successes, and
spill/reload pressure (including the profile-hot loop subset when available).
A call is classified as direct only when NYIR carries a concrete symbol. Raw
pointer memory operations and unknown calls remain visible as alias-unresolved
sites instead of being silently presented as proven-independent memory.

## Performance-cliff triage

Treat a large slowdown as a classification problem before changing code. Keep the
workload, observable result, input, optimization level, target, and cache state
equivalent, and use repeated medians rather than a single timing. Compare Nytrix
native against Nytrix LLVM before comparing either against C: when both Nytrix
paths are slow, start with source lowering, representation, and runtime semantics;
when LLVM is fast but native is slow, start with NYIR optimization, machine
lowering, register allocation, instruction selection, and object emission.

For the suspected hot function or loop, attribute the relevant costs explicitly:
allocation count/bytes, runtime-helper traffic, retained bounds/tag checks, direct
versus indirect calls and inlining, spills/reloads and peak live-register
pressure, and vectorization legality/profitability failures. Record the smallest
missing semantic fact or lowering capability that would remove the cost, then fix
the general compiler path rather than special-casing the benchmark.

The canonical `ny-test --bench` runner separates correctness smoke runs from
performance measurements. `--bench-correctness` uses one non-warmup run; repeated
performance runs report median, p95, min/max dispersion, and a p95-vs-median noise
percentage. Five-sample runs are marked unstable, and fail, when noise exceeds
`NYTRIX_BENCH_MAX_NOISE_PCT` (20% by default). JSON/Markdown output records CPU,
OS/arch, target features, C compiler/version/flags, and compiler revision. A
fixture can set `max_native_c_ratio` and/or `max_native_llvm_ratio` metadata to
define its own runtime regression budget. It can also set `max_ny_compile_ms`,
`max_ny_code_bytes`, `max_ny_specialization_code_bytes`,
`max_ny_specialization_function_bytes`, and `max_ny_peak_compiler_rss_kb`;
exceeding any available budget marks the row as a regression. Static-island
quality limits deliberately accept zero: `max_ny_heap_allocations`,
`max_ny_runtime_calls`, `max_ny_bounds_checks`, `max_ny_indirect_calls`,
`max_ny_unknown_effects`, `max_ny_alias_unresolved`, `max_ny_spills`, and
`max_ny_reloads` can therefore lock a known static path to an allocation-,
helper-, check-, indirect-call-, or spill-free contract. For native/AOT
measurements the runner performs one report-only compile after the timing samples
and records actual emitted machine-code bytes, specialization bytes/count/max
function size, static-island counters, and compiler peak RSS (KiB on supported
POSIX hosts). Those compile diagnostics are intentionally outside the timed
sample set and are exported to console, CSV, JSON, and Markdown reports.
Repeated timing noise is still evaluated separately, so unstable samples are not
silently accepted as a clean performance baseline.

On Linux, `ny-test --bench --bench-hw-counters` additionally runs the already
`--bench-runtime-counters` samples the compiled report artifact once outside timed runs and exports its existing allocation/reallocation counters.
compiled report artifact under `perf stat` outside the timed sample set and
exports cycles, instructions, branches, branch misses, and cache misses. Missing
`perf` support or permissions leave the hardware-counter fields unavailable
rather than corrupting the timing run.

Benchmark equivalence is an executable contract: paired Nytrix/LLVM/C variants
must emit the same `checksum=` marker. The runner records every backend checksum
and fails a mismatch, so the exact semantic-equivalence check is visible in each
result rather than being an informal timing assumption.

## Optimization loop

1. Record a reproducible baseline.
2. Identify one dominant owned cost.
3. Apply the smallest clear change.
4. Rerun the result check and the same benchmark.
5. Keep complexity only after repeated improvement.

Typed helpers, direct indexed access after a proven bound, smaller allocations,
and compile-time tables are often more useful than a target-specific rewrite.

## Related

- [Tooling](tooling.md)
- [Testing](testing.md)
- [SIMD and acceleration](simd.md)
- [Native compilation](native.md)