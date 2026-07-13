# Performance

Performance work answers two different questions:

1. how expensive the compiler/toolchain path is;
2. how fast the produced program runs.

Do not mix those numbers unless compile time is part of the workload.

## Command Matrix

| Command | Measures | Use |
| --- | --- | --- |
| `ny file.ny` | Compile plus JIT/runtime path. | Edit loop and behavior checks. |
| `ny --native-only file.ny` | Host-selected LLVM-free NYIR, native object/JIT, and run path on x86-64 or AArch64. | Native compiler/startup comparisons. |
| `ny -run file.ny` | Temporary native executable plus run. | Quick AOT smoke test. |
| `ny -o app file.ny` | Native compilation only. | Stable runtime artifact. |
| `ny -O3 --profile=peak -o app file.ny` | Peak native compilation only. | Upper-bound runtime check. |
| `./app` | Runtime only. | Program comparisons without compile noise. |
| `ny -time file.ny` | Compiler phases plus run time. | Find parse/import/codegen/cache regressions. |
| `ny -prof file.ny` | Timing and compiler/runtime stats. | Broader toolchain profile. |
| `ny perf` | Maintained perf checks. | Regression pass. |

Native `-o` defaults to `-O2`. JIT and REPL default to `-O0` for edit latency.
Use `--profile=peak` only when compile time can be traded for native speed.

The internally executable host backends are x86-64 and AArch64. Other target
names are explicit assembly/NYIR inspection backends; selecting one does not
silently claim object, link, JIT, or runtime support. AArch64 internal-link
regressions use QEMU only to execute already-linked machine code.

## Compile Once, Run Many

```bash
ny -O3 --profile=peak -g -o build/cache/bench/app bench.ny
build/cache/bench/app
build/cache/bench/app
build/cache/bench/app
```

`-g` keeps profiler symbols. Drop `-g` and add `-strip` only for distribution
size checks.

## Read `-time`

| Area | Meaning |
| --- | --- |
| read/import/stdlib | Source size, import graph, stdlib cache. |
| parse/type/codegen | Compiler work from syntax, types, generated IR. |
| native/JIT compile | Selected backend cost and cache behavior. |
| run | Program runtime after execution starts. |
| total | Whole edit-loop command cost. |

If only `total` moved, the regression is not isolated. If `run` moves in a
reused binary, the program behavior changed.

## Native Profiling

Linux `perf` flow:

```bash
ny -O3 --profile=peak -g -o build/cache/bench/app bench.ny
perf record -F 997 -g -o build/cache/bench/perf.data -- build/cache/bench/app
perf report -i build/cache/bench/perf.data
```

Compiler artifacts:

```bash
ny -O3 -time -dump-stats bench.ny
ny -O3 --emit-ir=build/cache/bench/app.ll -emit-only bench.ny
ny -O3 --emit-asm=build/cache/bench/app.s -emit-only bench.ny
```

Use IR and assembly to confirm a hypothesis, not to start one.

## Cache Discipline

```bash
ny --clean-cache
ny -time bench.ny
ny -time bench.ny
```

Record cold/warm cache state. Public notes should prefer CLI flags; mention
environment variables only when they are part of the experiment.

Common knobs:

| Setting | Use |
| --- | --- |
| `NYTRIX_JIT_CACHE_FORMAT=ir|bc` | Select JIT cache artifact format. |
| `NYTRIX_LAZY_STDLIB_CODEGEN=1` | Demand-emit imported stdlib bodies. |
| `NYTRIX_RUNTIME_OPT=3` or `speed` | Speed settings for runtime support. |
| `NYTRIX_RUNTIME_NATIVE=1` | Native CPU tuning for speed-profile runtime objects. |

## Benchmark Shape

```ny
use std.core

def data = [1, 2, 3, 4]

fn work(list<int> xs) int {
   mut int total = 0
   for x in xs { total += int(x) }
   total
}

assert(work(data) == 10, "bench result")
```

Benchmarks separate setup from timed work and assert the result.

## Optimization Order

1. Pin the command and input.
2. Compile a native binary and confirm the slow path still exists.
3. Profile or use `-time` to separate compiler cost from runtime cost.
4. Change the narrow measured hot path.
5. Run focused tests and rerun the same benchmark.
6. Keep dynamic fallback behavior when the public API accepts dynamic values.

Common useful changes: typed internal helpers, direct indexed access after a
type contract, fewer repeated `get` calls in loops, and precomputed
`comptime` tables.

## Report

```text
command: ny -O3 --profile=peak -g -o build/cache/bench/app bench.ny
run: build/cache/bench/app
input: rows=1024 cols=2048
cache: warm std cache, native binary reused
before: 185ms pipeline, 111ms solver
after: 25ms pipeline, 25ms solver
validation: ny test --pattern factorization
```

## Related

- [tooling.md](tooling.md)
- [testing.md](testing.md)
- [runtime.md](../spec/runtime.md)
