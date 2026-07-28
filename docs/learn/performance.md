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

Native `-o`, JIT, and REPL default to `-O0` for edit latency. Use `-O1` for a
quick compact native build, `-O2` for ordinary releases, and
`-O3 --profile=peak` only when compile time can be traded for native speed.

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
| `NYTRIX_JIT_CACHE_RUN=0` | Force a cold ordinary-file run; validated JIT bitcode reuse is enabled by default. |
| `NYTRIX_JIT_NATIVE_CACHE=1` | Promote validated JIT bitcode into a native shared-object tier; later validated hits are reused automatically. |
| `NYTRIX_LAZY_STDLIB_CODEGEN=1` | Demand-emit imported stdlib bodies. |
| `NYTRIX_RUNTIME_OPT=3` or `speed` | Speed settings for runtime support. |
| `NYTRIX_RUNTIME_NATIVE=1` | Native CPU tuning for speed-profile runtime objects. |

### Warm JIT tier

The native JIT cache is an edit-loop optimization, not a runtime-performance
claim. On the first eligible file run, Nytrix writes the normal bitcode cache
and, when `NYTRIX_JIT_NATIVE_CACHE=1` is set, promotes it to a shared object.
Later ordinary file runs probe that validated artifact automatically; setting
the variable is required only to create or refresh the native tier.
Later processes validate the bitcode sidecar's exact expanded-source
fingerprint and byte count before loading that object. The cache key also binds
the semantic configuration, so a change such as `--strict-types`, solver,
backend, ABI, or C frontend produces a miss rather than bypassing validation.
A missing, stale, or partial marker falls back to normal compilation.
With the native tier enabled, the compiler reports an early hit, a miss with
its reason, a successful promotion, or a promotion failure; a fallback is not
silent.

Measure the two warm paths with the same source and cache state:

```bash
NYTRIX_JIT_NATIVE_CACHE=0 ny --no-progress bench.ny
NYTRIX_JIT_NATIVE_CACHE=1 ny --no-progress bench.ny

hyperfine --warmup 2 --runs 10 \
  'NYTRIX_JIT_NATIVE_CACHE=0 ny --no-progress bench.ny' \
  'NYTRIX_JIT_NATIVE_CACHE=1 ny --no-progress bench.ny'
```

Do not compare a cold native-cache promotion against a warm bitcode hit: the
promotion includes one-time shared-object linking work.

### AOT cache identity

The AOT executable cache is only reused when the expanded source and complete
code-generation configuration match. That identity includes semantic strictness
and solver choices, runtime/heap policy, backend and ABI selection, sanitizer,
debug/link settings, and relevant host environment. Changing any of those
inputs deliberately recompiles rather than returning an executable produced
under different rules.

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

Performance work requires evidence. Never optimize from a single timing.

1. Establish a reproducible baseline with exact input, environment, flags, and
   cache state.
2. Capture a profile or direct counter identifying the dominant owned cost.
3. Apply the smallest clear fix.
4. Rerun identical correctness and performance commands.
5. Keep complexity only when repeated measurements show a stable improvement.

Separate startup, parsing, semantic analysis, lowering, optimization, code
generation, JIT, object writing, linking, runtime, renderer, driver, and
workload costs. If only `total` moved, the regression is not isolated.

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

## Short benchmark template

```bash
hyperfine --warmup 2 --runs 10 --export-json /tmp/ny-bench.json \
  'ny --native-only --native-backend=x86_64 --no-progress --color=never -c "print(42)"'
```

Report mean, deviation, and range. Warm compared commands identically. Use
`--prepare` or explicit cache control for cold comparisons.

## Deterministic generated probes

Generate repeatable workloads with `awk`. Put generation in `BEGIN`, pass
parameters with `-v`, avoid unseeded randomness, and keep exact bytes stable.
Use a temporary file once command-line or environment size could matter.

```bash
awk -v terms=5000 'BEGIN {
  print "mut x = 0"
  for (i = 1; i <= terms; i++) printf "x = x + %d\n", i
  print "x"
}' > /tmp/ny-straight.ny

wc -lc /tmp/ny-straight.ny
ny --native-only --native-backend=x86_64 \
  --no-progress --color=never /tmp/ny-straight.ny

hyperfine --warmup 2 --runs 10 --export-json /tmp/ny-straight.json \
  'ny --native-only --native-backend=x86_64 --no-progress --color=never /tmp/ny-straight.ny'
```

For branch-heavy NYIR or DCE scaling, precompile to `/dev/null` so object label
capacity does not become the accidental benchmark.

```bash
SRC="$(awk 'BEGIN {
  print "mut x = 0"
  for (i = 0; i < 2000; i++)
    print "if x == " i " { x = x + 1 } else { x = x + 2 }"
  print "x"
}')"
export SRC

hyperfine --warmup 2 --runs 10 --export-json /tmp/ny-dce.json \
  'ny --native-only --native-precompile=/dev/null --no-progress --color=never -c "$SRC"'
```

Use `--nyir-dump-stats` when pass behavior and instruction removal matter.
Use `--nyir-dump-raw` (and asm/object dumps) when deciding whether a pass
should change instruction shape, not only counts. See
[tooling.md](tooling.md#ir-and-assembly-driven-optimization).

## Profiling

```bash
perf record -q -F 199 -g -o /tmp/ny-perf.data -- \
  ny --no-progress --color=never -emit-only <representative-file>
perf report --stdio --no-children -i /tmp/ny-perf.data
perf report --stdio --children --sort symbol -i /tmp/ny-perf.data
```

Use `strace -c -f` to separate process/filesystem startup from compiler work,
and pair it with Nytrix phase timing. For hotspots that look like codegen
shape rather than algorithm cost, dump NYIR/machine form first, then profile
to confirm the dominant owned cost.

```bash
ny --native-only --native-backend=x86_64 \
  --no-progress --color=never -time -vv -c 'print(42)'
strace -c -f ny --native-only \
  --native-backend=x86_64 --no-progress --color=never -c 'print(42)'
```

Cache hits and constant-command fast paths can invalidate backend comparisons.
Include a real function/call workload in addition to trivial startup.

## Related

- [tooling.md](tooling.md)
- [testing.md](testing.md)
- [runtime.md](../spec/runtime.md)
