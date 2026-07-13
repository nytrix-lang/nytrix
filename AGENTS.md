# Nytrix Agent Guide

Private, model-neutral instructions for working in this repository.

## Source of truth

Use `README.md`, `docs/CHANGELOG.md`, current source, tests, and focused probes.
Do not revive historical tasks that current documentation or regression tests
already mark complete. Preserve unrelated working-tree changes.

## Before editing

1. Search documentation for the concept.
2. Search exact symbols, helpers, methods, aliases, diagnostics, and keywords.
3. Inspect the closest implementation and its callers.
4. Read related tests, examples, and probes.
5. Run the smallest probe that confirms current behavior.
6. Modify only after the existing API and failure mode are understood.

Search focused paths first; avoid generated, build, and cache trees. Check both
receiver and module-level APIs before inventing a helper. Extend an existing
near-match instead of creating a parallel abstraction.

## Implementation rules

- Correctness precedes cleanup and performance.
- Prefer existing stdlib helpers, facades, and shared C utilities.
- Consolidate duplicates only after comparing semantics and all call sites.
- Keep native IR, ABI, object, backend, compiler-option, and pipeline edits
  narrow and paired with focused regression coverage.
- Treat `ny-fmt` findings as leads to inspect, never as automatic rewrites.
- Add keywords only when they improve user-facing discovery; do not mirror
  every export or bulk-generate metadata.
- Unsupported native shapes must fail explicitly.
- Native success must not rely on AST, LLVM, libclang, compiler, or linker
  fallback unless that adapter/fallback is the feature under test.

## Performance workflow

1. Establish a reproducible baseline with exact input, environment, and flags.
2. Capture a profile or other direct evidence.
3. Fix the dominant owned hotspot with the smallest clear change.
4. Rerun identical correctness and performance commands.
5. Keep added complexity only for a stable, meaningful improvement.

Use repeated measurements where noise matters. Never optimize from one timing.
For UI work, prefer bounded headless probes; use real renderer profiles when
the display and driver are controlled. Distinguish compiler startup, JIT,
renderer, driver, and workload costs.

Use `hyperfine` for short compiler/startup comparisons when available. Keep
the exact command stable, warm caches before sampling, collect at least ten
runs, and save machine-readable results:

```bash
hyperfine --warmup 2 --runs 10 --export-json /tmp/ny-bench.json \
  './build/release/ny --native-only --native-backend=x86_64 --no-progress --color=never -c "print(42)"'
```

Report mean, deviation, and range. Use `--prepare` or explicit cache controls
when comparing cold behavior, and rerun the same command after the change.

### Repeatable generated stress probes

Generate large deterministic Nytrix inputs with `awk` and export the result for
`hyperfine`. This avoids maintaining throwaway source files and ensures every
sample receives identical bytes. Keep generated input below the host's
environment/argument-size limit and state the generator parameters with the
result.

Useful `awk` rules for probes:

- Put generation in `BEGIN` so it does not wait for stdin; pass tunables with
  `awk -v branches=2000 'BEGIN { ... }'` instead of editing the program.
- Prefer `printf` when whitespace or numeric formatting is significant. Plain
  `print` appends `ORS` (normally a newline), while comma-separated values use
  `OFS`; set `OFS`/`ORS` explicitly when their defaults are part of the probe.
- Keep probes deterministic. Avoid `rand()`, or call `srand(<fixed integer>)`
  and record the seed. Use integer loop bounds and stable literal formatting.
- Shell substitutions remove trailing newlines. That is harmless for `-c`, but
  use `awk ... > /tmp/probe.ny` when exact file bytes or source locations matter.
  Quote `"$SRC"`; unquoted expansion changes whitespace and wildcard bytes.
- For inputs larger than the platform argument/environment limit, write a
  `/tmp` file and benchmark the file path. Generate it once outside `hyperfine`
  unless file generation is intentionally part of the measurement.
- Use `NR` for a total input record number and `FNR` when processing several
  files independently. Small post-processing checks such as
  `awk 'END { print NR }' /tmp/probe.ny` are useful for reporting exact size.
- Exit nonzero from invariant checks, for example
  `awk 'END { exit NR == expected ? 0 : 1 }' expected=2002 file`, so a malformed
  generated workload cannot silently enter a benchmark.

Warm caches only after generating the final workload. Confirm it once with the
same compiler flags, then let `hyperfine` vary only the command under study:

```bash
awk -v terms=5000 'BEGIN {
  print "mut x = 0"
  for (i = 1; i <= terms; i++) printf "x = x + %d\n", i
  print "x"
}' > /tmp/ny-straight.ny
wc -lc /tmp/ny-straight.ny
./build/release/ny --native-only --native-backend=x86_64 \
  --no-progress --color=never /tmp/ny-straight.ny
hyperfine --warmup 2 --runs 10 --export-json /tmp/ny-straight.json \
  './build/release/ny --native-only --native-backend=x86_64 \
   --no-progress --color=never /tmp/ny-straight.ny'
```

For branch-heavy NYIR and DCE scaling, precompile to `/dev/null` so the timing
includes parsing, lowering, optimization, and binary NYIR serialization but
does not become an object-writer label-capacity test:

```bash
SRC="$(awk 'BEGIN {
  print "mut x = 0"
  for (i = 0; i < 2000; i++)
    print "if x == " i " { x = x + 1 } else { x = x + 2 }"
  print "x"
}')"
export SRC
hyperfine --warmup 2 --runs 10 --export-json /tmp/ny-dce.json \
  './build/release/ny --native-only --native-precompile=/dev/null \
   --no-progress --color=never -c "$SRC"'
```

For long straight-line native object/register-allocation probes, use assignments
without branches and execute the in-memory native path:

```bash
SRC="$(awk 'BEGIN {
  print "mut x = 0"
  for (i = 1; i <= 5000; i++) print "x = x + " i
  print "x"
}')"
export SRC
hyperfine --warmup 2 --runs 10 --export-json /tmp/ny-native-large.json \
  './build/release/ny --native-only --native-backend=x86_64 \
   --no-progress --color=never -c "$SRC"'
```

Use `--nyir-dump-stats` when instruction removal and pass behavior matter. Do
not combine an intentionally huge label probe with in-memory object emission
unless object-label capacity is itself under test.

### Profiling commands

Profile a representative real project before changing compiler hot paths:

```bash
perf record -q -F 199 -g -o /tmp/ny-editor-perf.data -- \
  ./build/release/ny --no-progress --color=never -emit-only \
  etc/projects/ui/editor.ny
perf report --stdio --no-children -i /tmp/ny-editor-perf.data
perf report --stdio --children --sort symbol -i /tmp/ny-editor-perf.data
```

Use `strace -c -f` to separate compiler work from filesystem/process startup
cost, and pair it with Nytrix phase timing:

```bash
./build/release/ny --native-only --native-backend=x86_64 \
  --no-progress --color=never -time -vv -c 'print(42)'
strace -c -f ./build/release/ny --native-only \
  --native-backend=x86_64 --no-progress --color=never -c 'print(42)'
```

Cache hits and constant-command fast paths can make a backend comparison
meaningless. Report cache state, warm both commands identically, and include a
real function/call probe as well as trivial startup. When testing an optimizer
patch, measure the patched build, temporarily disable only that patch, rebuild,
measure the exact same command, then restore it with `apply_patch`. Never leave
the baseline variant in the worktree. Keep the change only when correctness
passes and repeated distributions show a useful improvement.

Useful `ny-fmt` modes include `--bugs`, `--trim`, `--dead`, `--specialize`,
`--metaprog`, `--overhaul`, and `--cloc`.

## Validation

Run the narrowest relevant check first, then broaden in proportion to risk.
Agent-driven commands should use `--no-progress --color=never` whenever
supported so logs remain plain, stable, and searchable.

```bash
./make # to compile nytrix
./make ny --no-progress --color=never <file>
build/release/ny-fmt --bugs --limit 80 <changed paths>
git diff --check
```

Additional expectations:

- Generated `.ny`: execute it, diagnose the first real error, fix minimally,
  and rerun the exact command.
- Comptime/macros/generated code: inspect expansion.
- Library/probe changes: run formatting and trim checks.
- Standard library: rebuild the standard bundle.
- Compiler/runtime/native: run focused fixtures and the broader relevant set.
- UI/rendering: run a bounded artifact-backed or framebuffer probe.
- Fuzzing: run the narrow validation or smoke command.
- Sanitizers: use ASan/UBSan/TSan only when the build actually enables them;
  do not infer race coverage from ASan or UBSan.

Never claim success unless the command ran and its expected marker appeared.
Report failed commands and environmental limits plainly.

## Native checks

```bash
LD_LIBRARY_PATH=build/vendor/lib/host \
  ./build/release/ny-test --jobs 10 etc/tests/rt/native/c/*.nshape

LD_LIBRARY_PATH=build/vendor/lib/host \
  ./build/release/ny-test --jobs 8 etc/tests/rt/native
```

For focused x86-64 scalar checks use `--native-result-oracle[=N]`. Expected:

```text
native oracle function=rt_main vm=<value> native=<value> ok=yes
```

Do not count external-linker fallback as internal link/run success. A backend
is complete only when ABI, object, link, and runtime gates prove it.

## Active incomplete areas

No evidence-backed release blocker is currently tracked here. New native work
must be scoped to an advertised capability and paired with its ABI, object,
link, and runtime gate; assembly-only cross-targets are not executable-backend
claims.

Do not re-add completed hot reload, proof types, renderer parity, file watching,
existing scalar ABI slices, AArch64 host/JIT/ELF gates, covered pointer/memory
fixtures, or established C imports to this list.

## High-risk files

- `src/code/native/ir.{h,c}`
- `src/code/native/native.c`
- `src/code/native/backend/x86_64.c`
- `src/code/native/object.c`
- `src/base/options.c`
- `src/wire/pipe/`

Keep changes focused and report the exact command, success marker, and behavior
each regression proves.

## Testing

```bash
./make test --with-stdlib --failures-only
```

## Repository workflow

Preserve unrelated work. Inspect `git status` and stage explicit paths instead
of using `git add -A` blindly. Do not touch `tmp/` or `.github/FUNDING.yml`
unless requested. Make checkpoint commits before risky restructuring when
requested. Never reset, rewrite, force-push, or discard user work. Push only
when requested, using `git push origin HEAD:main`.

The multi-platform workflow is manual-only. Do not add push or pull-request
triggers. Dispatch it with:

```bash
gh workflow run multi-platform.yml --ref main -f target=<linux|macos|windows|all>
```

Inspect runs with `gh run list --workflow multi-platform.yml`,
`gh run view <id> --json jobs`, and
`gh run view <id> --job <job-id> --log`. Read the final replay, debugger,
backtrace, and exit status rather than relying only on the red summary or a
`--log-failed` excerpt.

Keep timeout scopes distinct. `NYTRIX_TEST_TIMEOUT` limits each fixture,
`NYTRIX_TEST_SUITE_TIMEOUT` limits the complete local suite, and workflow
`timeout-minutes` limits the hosted job. Hosted tests that require a real
display, driver, service, or unavailable system dependency should use a narrow,
documented capability gate instead of heavyweight package installation or a
fake success. Never claim a fix from a skipped test or fallback path.
