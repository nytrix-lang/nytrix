# Performance

Performance work in Nytrix has two separate questions:

1. how long the compiler/toolchain takes to parse, type, optimize, and emit;
2. how fast the produced program runs.

Keep those separate. A JIT run fits edit-time checks, but a compiled native
binary gives a more isolated runtime measurement.

## Execution modes

| Command | Measures | Use |
| --- | --- | --- |
| `ny file.ny` | Compile plus JIT/runtime path. | Fast edit loop and behavior checks. |
| `ny -run file.ny` | Build a temporary native executable, then run it. | Quick AOT smoke test. |
| `ny -o app file.ny` | Compile only, emit the default optimized native binary. | Stable runtime benchmark artifact. |
| `ny -O3 --profile=peak -o app file.ny` | Compile only, emit the peak native profile. | Upper-bound runtime check. |
| `app` | Runtime only. | Compare program changes without compile noise. |
| `ny -time file.ny` | Compiler phase timings plus run timing. | Find parse/import/codegen/cache regressions. |
| `ny -prof file.ny` | Timing and compiler/runtime stats. | Broader profiling report from the toolchain. |
| `ny perf` | Bundled toolchain performance checks. | Regression pass across maintained probes. |

Native `-o` builds default to `-O2`; JIT and REPL runs stay at `-O0` for edit
latency. `-O3` and `--profile=peak` are explicit peak-native-speed modes.
`--profile=speed` is the conservative speed profile; `--profile=peak`
also turns on heavier typed arithmetic and imperative-loop lowering paths
that trade compile time for peak native throughput. JIT timings and native
runtime timings answer different questions unless compile time is part of the
measurement.

The default native profile is allowed to use safe range proofs and LLVM loop
vectorization when the compiler can prove the integer operations stay inside
Nytrix fixnum bounds. Typed counted loops can vectorize without making `-O3`
the default.

## Separate compile and runtime timing

This is a toolchain timing:

```bash
ny -time bench.ny
```

This is closer to a runtime timing:

```bash
ny -O3 --profile=peak -g -o build/cache/bench/app bench.ny
build/cache/bench/app
```

The first command answers "how expensive is this edit loop?" The second
answers "how fast is the optimized program after compilation?" Mixing those
two numbers combines compiler cost with runtime cost.

## Compile then measure

Compile once, run many times:

```bash
ny -O3 --profile=peak -g -o build/cache/bench/app bench.ny
build/cache/bench/app
build/cache/bench/app
build/cache/bench/app
```

`-g` keeps debug symbols for profilers. Drop `-g` and add `-strip` only for a
distribution-sized binary check, not for normal profiling.

When comparing JIT and native behavior:

```bash
ny -time bench.ny
ny -O3 -time -run bench.ny
ny -O3 -o build/cache/bench/app bench.ny
build/cache/bench/app
```

Compiled `-o` binaries are normally faster to rerun because compilation is no
longer part of the measured command.

## Read `-time`

`-time` is for separating compiler cost from program cost. Treat the labels as
questions:

| Label area | What it tells you |
| --- | --- |
| read/import/stdlib | Source size, import graph, stdlib cache behavior. |
| parse/type/codegen | Compiler work caused by syntax, type shape, and generated IR. |
| native/JIT compile | LLVM/native backend cost and cache behavior. |
| run | Runtime cost after the program starts executing. |
| total | Whole command cost for the edit loop. |

If only `total` moved, the change is not isolated. If `run` moved in a reused
binary, the change affected runtime behavior.

## Profile a native binary

On Linux, use `perf` against the emitted binary:

```bash
ny -O3 --profile=peak -g -o build/cache/bench/app bench.ny
perf record -F 997 -g -o build/cache/bench/perf.data -- build/cache/bench/app
perf report -i build/cache/bench/perf.data
```

If samples collapse into runtime glue, rebuild with `-g`, keep symbols, and
profile a workload that spends enough time in the code being measured.

Use compiler artifacts when the hot path is unclear:

```bash
ny -O3 -time -dump-stats bench.ny
ny -O3 --emit-ir=build/cache/bench/app.ll -emit-only bench.ny
ny -O3 --emit-asm=build/cache/bench/app.s -emit-only bench.ny
```

IR and assembly are for confirming a hypothesis, not for starting one.

## Profile checklist

Before trusting a profile:

- compile with `-g`;
- profile a native `-o` binary;
- run enough work to collect representative samples;
- keep the same input and cache state between before/after runs;
- save the exact command beside the result.

If the profile points at dynamic value helpers, try proving the type contract
inside the hot helper. If it points at allocation or dictionary/list fallback
paths, check whether the public function is too broad or whether only an
internal helper needs a typed fast path.

## Cache discipline

Caches shorten developer runs and can invalidate benchmark comparisons. Record
whether a run is warm or cold.

```bash
ny --clean-cache
ny -time bench.ny
ny -time bench.ny
```

The first command after `--clean-cache` includes cold stdlib/JIT/AOT work. The
second run shows warm-cache behavior. A comparison records whether each run is
cold or warm.

Relevant environment knobs include:

| Setting | Use |
| --- | --- |
| `NYTRIX_JIT_CACHE_FORMAT=ir|bc` | Select JIT cache artifact format. |
| `NYTRIX_LAZY_STDLIB_CODEGEN=1` | Demand-emit imported stdlib bodies. |
| `NYTRIX_RUNTIME_OPT=3` or `speed` | Compile runtime support with speed settings. |
| `NYTRIX_RUNTIME_NATIVE=1` | Allow native CPU tuning for speed-profile runtime objects. |

Use CLI flags in public notes. Mention environment variables when they are
part of the experiment.

## Compare matrix

`ny perf compare` includes plain `ny-default-*` rows so the default native
build stays measured separately from explicit profiles. The `ny-o3-speed-*`
rows are peak-profile checks, not the default baseline.

## Benchmark structure

Benchmarks keep setup separate from the timed operation and assert the result:

```ny
use std.core

def data = [1, 2, 3, 4]

fn work(list<int>: xs): int {
   mut int: total = 0
   for x in xs { total += int(x) }
   total
}

assert(work(data) == 10, "bench result")
```

Typed hot-path inputs fit code paths where they match the public contract. Keep
the dynamic path when the public API intentionally accepts dynamic or mixed
values.

## Optimization order

1. Reproduce the workload and pin the command.
2. Compile to a native binary and confirm the slow path still exists.
3. Profile the binary or use `-time` to separate compiler cost from runtime
   cost.
4. Make the narrowest change that targets the measured hot path.
5. Run focused tests, then rerun the same benchmark command.
6. Keep the fallback path when the public API accepts dynamic or mixed values.

Measured Nytrix fixes often involve:

- typed internal helper signatures for tight loops;
- direct indexed access after the type contract is known;
- fewer repeated `get`/default calls in inner loops;
- precomputed tables in `comptime` data;
- one reusable compiled binary for repeated measurement.

Changes that need stronger measurement include:

- changing style without moving a measured hot path;
- optimizing a debug or cold-cache command and reporting it as runtime speed;
- removing broad API behavior instead of adding a narrower internal helper;
- comparing `ny file.ny` before with `app` after.

## Report template

```text
command: ny -O3 --profile=peak -g -o build/cache/bench/app bench.ny
run: build/cache/bench/app
input: rows=1024 cols=2048 weight=5
cache: warm std cache, native binary reused
before: 185ms pipeline, 111ms solver
after: 25ms pipeline, 25ms solver
validation: ny test --pattern crypto/factorization
```

Performance notes include the command, input, cache state, and validation
beside the numbers.

## Related

- [tooling.md](tooling.md) for `ny perf`, `ny fmt --profiles`, and audit commands.
- [testing.md](testing.md) for executable checks.
- [runtime.md](../spec/runtime.md) for execution modes.
