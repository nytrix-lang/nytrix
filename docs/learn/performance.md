<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":140,"summary":"Measure compile time, startup, and steady-state runtime separately before optimizing."} -->
# Performance

Measure the workload you need to improve. Keep the input, result check, target,
optimization level, and cache state fixed while comparing runs.

## Separate the costs

- **Compile time:** time the compiler separately from execution. `-time` prints
  compiler phase timings; `--profile-time` can repeat a compile and report the
  samples.
- **Startup:** compare the default run path with `-run` when process startup
  dominates. `-run` builds a native executable before executing it.
- **Steady-state work:** measure the same repeated workload and check the same
  result on every run. Do not compare different algorithms or input sizes.

For a runtime hotspot, `--perf=compact` gives a short profile;
`--perf=verbose` includes more detail. Use the current `ny --help` for available
profile options.

## Keep the comparison valid

1. Make the program verify a checksum or exact result outside the timed region.
2. Build and run the same source with the same target and optimization settings.
3. Separate cold compilation from warm-cache execution; state which one you
   measured.
4. Repeat enough samples to see variance. Avoid profiling while other heavy
   work competes for CPU or memory bandwidth.
5. Record compiler revision, target, flags, and input with the result.

`./make bench --jobs 1` runs the repository's paired C/Ny benchmark fixtures
with reduced timing interference. Those fixtures are compiler benchmarks, not
a substitute for measuring an application workload.

## Compiler compile-time work

For a compiler hotspot, capture a representative source graph and phase or
sampling profile before changing code. Optimize the owning general path; retain
observable output and inline decisions, then compare generated code and compile
time on the same workload.

A focused editor-graph profile reported `nyir_inline_general` at about 32% of
cycles and machine register allocation at about 14%. A follow-up attributed
34.86% to repeated static loop-depth scans. A per-function depth map and cached
callee summaries reduced that compile from a 180-second timeout to 133 seconds.
The largest known remaining hotspot is machine register allocation. These are
workload-specific profile observations, not general compiler timings; reprofile
the same editor graph before drawing a new conclusion.

See [Testing](testing.md) for repeatable fixtures and
[Execution routes](routes.md) for backend selection.
