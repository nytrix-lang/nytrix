# Contributing to Nytrix

Thanks for contributing to Nytrix.

Good contributions are **small, owned by the right layer, reproducible, and easy to verify**. Start from current source and observed behavior, make the narrowest correct change, and prove it with focused evidence.

## Contribution Flow

For most changes:

1. Find the implementation that owns the behavior and inspect its nearest callers.
2. Reproduce the current behavior with the smallest useful fixture or command.
3. Read the relevant tests and current documentation.
4. Identify the public contract, failure mode, and compatibility surface.
5. Fix the owning layer; avoid parallel helpers or workarounds elsewhere.
6. Add or update a focused regression when practical.
7. Run the smallest relevant check first, then widen validation according to risk.
8. Review the final diff before requesting review.

A skipped test, cache hit, fallback path, emitted artifact, or unexecuted platform is not proof that a change works.

## Repository Layout

- `src/` - compiler, runtime, native pipeline, and tools
- `lib/` - standard library
- `etc/tests/` - executable fixtures, shape corpora, and benchmarks
- `etc/projects/` - examples and applications
- `docs/spec/` - exact language behavior and contracts
- `docs/learn/` - workflows and user guidance

Prefer existing utilities, facades, and shared implementations. Keep paths role-based, names descriptive, and patches narrow.

## Module Headers

Keep `;; Keywords: ...` first:

```ny
;; Keywords: compiler lowering native
;; Optional concise purpose line.
module example
```

## Naming

Use **hyphens** for `.nshape` tests and benchmarks:

```text
loop-unswitch.nshape
time-profile.nshape
call-chain.nshape
```

Use **underscores** for `.c` and `.h` source files:

```text
loop_unswitch.c
gvn_pre.c
loop_vectorize.c
alias_store_sink.c
slp_vectorize.c
```

Do not use hyphens in C source/header filenames. Existing C variables and functions continue to use `snake_case`.

## Documentation

Keep generated documentation close to the code or page it describes so source, website cards, search, and API pages do not drift apart.

### Markdown Metadata

Pages in `docs/learn/` and `docs/spec/` may begin with:

```html
<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":10,"summary":"One concise sentence for cards and search."} -->
```

- `audience` - intended reader
- `group` / `order` - manual grouping and sequence
- `summary` - short factual card/search description
- `featured` - eligible for curated landing placement

Avoid release-note language in summaries.

### Public Library Facades

Public facades keep documentation in source:

```ny
;; Keywords: text collections iteration
;; One concise summary for the module card and API header.
;; References:
;; - std
;; Documentation:
;; ## Scope
;; What this facade owns and where its boundary ends.
;;
;; ## Namespaces
;; - **Text:** `std.example.text`
module std.example
```

The purpose line becomes the module summary; the `Documentation:` body renders as Markdown.

Use facade docs for scope, important namespaces, portability boundaries, and selection guidance. Keep function documentation with its owning declaration.

## Working Rules

Use current source and executable behavior as primary evidence. Historical notes matter only when they still match the code.

Before editing, inspect:

- the owning implementation;
- nearest callers;
- relevant tests;
- a minimal reproducer.

Search focused source paths before generated, build, cache, vendor, or temporary trees.

Preserve public behavior unless the change intentionally modifies it. Reuse established diagnostics and utilities. Unsupported behavior should fail clearly rather than silently degrading to another implementation path.

### Compiler invariants

- Keep source semantics and target/ABI policy in their owning layers. Target-independent NYIR transforms must not depend on physical registers or calling-convention accidents, and machine encoders must not reconstruct source-language meaning.
- Centralize target layout/ABI queries instead of duplicating constants across front-end, NYIR, and emitters.
- Keep dynamic fallback explicit at genuinely uncertain boundaries. A static fast path must preserve the same observable semantics and failure behavior as its general path.
- Treat source proofs as trusted evidence only through explicit derivation/invalidation rules. Optimizer guesses are never source-level proof witnesses.
- Treat IO, FFI, synchronization/thread state, traps, volatile state, and floating-environment effects as optimization barriers unless a more precise audited summary proves otherwise.
- Keep diagnostics source-span-rich and deterministic. When an optimization is blocked, prefer a concrete reason over a silent conservative fallback.
- Keep formatter, parser/compiler, documentation, and LSP syntax rules aligned. Unsupported C or package metadata must remain diagnosable rather than being silently dropped.
- Do not hard-code benchmark names into compiler behavior. Prefer general compiler fixes over duplicate permanent “fast” library variants; handwritten kernels are for cases where the generic path cannot reasonably express equivalent code.
- Performance guidance must describe measured compiler behavior and should be backed by reproducible regression thresholds when practical.

### Examples

Fence Nytrix source as `ny`, shell commands as `bash`, and other formats with their matching language.

State required services, libraries, files, fixtures, or network endpoints immediately before an example. Prefer explicit imports and small assertions; silent success should mean every assertion passed.

Specification pages should favor exact forms and behavior tables over tutorial prose.

## Correctness and Evidence

A change is ready for review when the observable claim is reproducible and the closest plausible regressions are covered.

Good evidence shows:

- the changed contract works;
- relevant rejected, boundary, or failure cases still behave correctly;
- the intended implementation path was exercised;
- static analysis has no unexplained new signal.

An exit status alone is insufficient when a fallback can produce the same result.

### Native Changes

Native support is proven only by the intended native path plus an executable oracle.

These are not proof by themselves:

- skipped fixtures;
- cache hits;
- IR or assembly output;
- interpreter fallback;
- LLVM fallback;
- host-compiler execution.

For native changes, follow the relevant path:

```
source -> parsing -> semantic analysis -> lowering -> NYIR
-> machine form -> allocation -> object/link -> runtime
```

An IR dump supports a hypothesis; it does not prove encoding, linking, or execution.

Typed native `i64` slots contain raw values. Dynamic `NyValue` containers retain tagged values. Keep `NYTRIX_USE_GMP` optional and preserve the documented BigInt layout.

Do not add box/unbox or retagging paths without a concrete dynamic boundary, a clear owner, and a reliable oracle.

Compiler-enforced subsets belong in the compiler, not in a linter.

### Diagnostics and Unsupported Behavior

Unsupported language, ABI, renderer, and browser shapes must fail with an practical diagnostic rather than silently changing execution paths or behavior.

Compiler diagnostics should preserve:

- source location;
- stable category;
- practical message;
- no secondary-error flood.

Use structured results for recoverable library operations and compiler diagnostics for language failures.

## Process Model

The compiler, loader, intern table, and some diagnostic/profile state use process-global state.

A compiler invocation is single-threaded with respect to its compilation context.

Do not:

- embed multiple independent compiler contexts in one process;
- call one compiler context concurrently.

Parallel test/build workers should use separate processes.

## Libraries and Ownership

Keep ownership and mutability contracts exact.

After standard-library changes:

1. rebuild the standard bundle;
2. test direct calls;
3. test common composition patterns.

Prefer compact module `#main` self-tests for local behavior. Keep cross-module, ABI, and lifecycle cases in external fixtures.

Maintain one canonical standard-library behavior.

## Source Converters

Extend the existing converters instead of creating a parallel translator.

Both entry points live in `src/cmd/fmt/init.c`:

- `c2ny_line` - C conversion
- `py2ny` - Python conversion

Cover each new source shape with a converter fixture.

Unsupported constructs must remain visible and compilable:

```text
// c2ny: unsupported
```

or:

```text
# py2ny: unsupported
```

Pair the marker with a clear diagnostic. Never silently drop source.

## Renderer and Runtime Changes

Renderer fixes belong in shared renderer code. Check resource lifetime, failure propagation, headless behavior, and bounded framebuffer/artifact probes.

The runtime amalgamation starts at `src/rt/init.c`; CMake tracks its included sources.

Do not:

- edit generated `build/release/std.ny`;
- commit generated shared libraries;
- change vendored dependencies without a demonstrated defect.

Project-local libraries referenced by `link "..."so"` and a sibling C source are built under:

```
<tmpdir>/nytrix-shlib/
```

Never commit generated `.so` or `.dll` files.

## Validation

Start small, rerun the original reproducer, then widen according to risk.

```bash
./make
./make test
./make test --failures-only --color=never
./make all
./make web-test
./make bench
./make perf
git diff --check
```

### Quality Gate

Before review, run the applicable checks:

```bash
git diff --check
./make check
./make audit
./make test --failures-only --color=never --pattern=<focused-fixture>
NYTRIX_TEST_COLD=1 ./make test --failures-only --color=never \
  --pattern=<focused-fixture>
```

### Performance Measurement

Use fixed inputs with warmups and repeated runs:

```bash
./build/release/ny-test --bench --bench-engine native \
  --bench-target x86_64 --bench-run <N> --bench-warmup <N>
```

Preserve the structured output and target/toolchain environment. One run is a
smoke test, not performance evidence.

Before pushing:

```bash
./make tidy
git diff --check
```

`./make audit` is an investigation queue, not a behavioral test. For new findings in touched code, fix them, show they predate the patch and are unrelated, or narrow the change until ownership is clear. Do not silence findings just to quiet the report.

### Review Checklist

- [ ] Owning layer and observable contract are clear.
- [ ] Fixed bugs have a minimal reproducer or focused fixture.
- [ ] Intended, rejected, boundary, or failure behavior is exercised where relevant.
- [ ] A cold focused test follows cached iteration.
- [ ] Shared compiler/runtime/ABI/parser/library changes receive wider test coverage.
- [ ] Final diff contains no accidental generated output or unrelated edits.
- [ ] Assertions, diagnostics, allocations, fallbacks, and compatibility shims were reviewed.
- [ ] Review notes include exact commands, inputs, expected markers, observed markers, and environmental limits.

Do not report skipped work, cache hits, or unexecuted platforms as passing.

## Tool Map and Debugging

Use the repository driver instead of stale binaries or ad-hoc build trees.

```bash
./make --help
./make env
./make doctor
./make targets
./make ny --help
./make fmt --help
```

Triage from the smallest observable failure:

| Problem | First evidence | Escalation |
| --- | --- | --- |
| Parse/type/diagnostic | direct `./make ny` reproducer | reduce input; preserve location/category |
| Native correctness | `--native-only --native-result-oracle` | `--native-oracle-per-pass`, then inspect NYIR/machine path |
| Native optimization | `--nyir-dump=/tmp/name.nyir` | prove fast-path marker and absence of replaced slow path |
| Runtime/JIT | direct run with `-trace` | ASan Debug tree; separate JIT limits from source faults |
| Browser | `./make web-test` | record engine, fixture, marker, virtual-time budget |
| Setup/CI | `./make doctor`, then job log | inspect each OS and exact failure marker |

Use caches while iterating, then disable them for evidence:

```bash
NYTRIX_TEST_COLD=1 ./make test --failures-only --color=never \
  --pattern=<focused-fixture>
```

Do not turn on every debug facility at once.

## Benchmark Discipline

Measure before optimizing.

Keep benchmark input, compiler flags, cache mode, warmups, measured runs, and execution path fixed.

Reports should identify:

- executable path;
- JIT engine;
- optimization level;
- cache state;
- warmups and measured runs;
- checksum;
- exit status.

Keep compiler/startup wall time separate from self-reported kernel time.

Compare only semantically equivalent fixtures with matching C source. Run LLVM, MCJIT, and ORC separately; one JIT result does not represent the others.

Native AOT/JIT comparison should follow a recorded LLVM/JIT/C baseline and an explicitly enabled native path.

Cache hits, skipped engines, failed compiler runs, and zero-duration rows are not performance evidence.

Every `perf-real` benchmark should provide equivalent `source ny` and `source c` blocks when a meaningful C analogue exists. Workloads without one must remain explicitly non-comparative.

## Browser Testing

`./make web-test` is the real browser path and reads:

```text
etc/tests/native/web/tests.json
```

Each fixture needs an observable browser marker and bounded virtual-time budget. Fixtures requiring a live browser lifecycle must declare it and skip only in explicit headless mode.

`wasm-bare` is the maintained browser contract.

Treat Emscripten as a separate adapter. Verify imports, linking, browser execution, and the same observable fixture. Keep WebGL2, Asyncify, assets, and persisted VFS behavior behind capability checks.

Do not present a browser host boundary as native filesystem, process, or Vulkan support.

Browser reports should include the browser, exact command, expected marker, observed result, implementation path, and environmental limits.

## CI Options

Push and pull-request workflows run normal platform jobs.

Memory-safety and coverage jobs are opt-in from:

**Actions -> test -> Run workflow**

Options:

- `target` - one platform or `all`
- `suite` - `standard` or `full-stdlib`
- `checks` - `tests`, `memory-safety`, `coverage`, or `all`

Equivalent local commands:

```bash
./make test
./make test --with-stdlib
./make asan --with-stdlib
./make ubsan --with-stdlib
```

## Sanitizers

Use separate Debug build trees so instrumented binaries are explicit.

### ASan + UBSan

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build/asan -j"$(nproc)"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  build/asan/ny-test --bin build/asan/ny_debug \
  --color=never --failures-only --pattern=<focused-fixture>
```

Run the reproducer directly with:

```bash
build/asan/ny_debug path/to/reproducer.ny
```

Exercise JIT and native execution separately. JIT non-local exits can produce unsupported ASan shadow-stack warnings; a warning or signal exit alone is not sanitizer evidence.

Preserve the sanitizer report, reproduce the fixture directly, and fix the owning allocation or bounds error.

### ThreadSanitizer

```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
cmake --build build/tsan -j"$(nproc)"
```

### MemorySanitizer

Clang only:

```bash
CC=clang cmake -S . -B build/msan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=memory -fno-omit-frame-pointer -g"
cmake --build build/msan -j"$(nproc)"
```

## Valgrind

```bash
./make bin
valgrind --leak-check=full --show-leak-kinds=definite \
  --errors-for-leak-kinds=definite --error-exitcode=97 \
  build/release/ny-full path/to/reproducer.ny
```

Use `ny-full`, not the `ny` launcher, so Valgrind follows the compiler process. Treat invalid-memory reports and definite leaks as failures; LLVM may retain process-lifetime allocations.

## Coverage

```bash
cmake -B build/cov -DCMAKE_C_FLAGS="--coverage -fno-omit-frame-pointer -g"
cmake --build build/cov -j"$(nproc)"
NYTRIX_BUILD_DIR=build/cov ./make test

lcov --capture --directory build/cov --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage-report
```

## High-Risk Paths

Changes under these paths need focused regression coverage:

```
src/code/native/
src/base/options.c
src/wire/pipe/
```

For native work, report the exact command, expected marker, observed result, and implementation path exercised.

For performance work, keep inputs, compiler flags, cache mode, sample count, and execution path fixed. Compare C, Nytrix, and optional GMP variants only when they perform equivalent work and expose correctness markers.

## Open Work

Keep work in its owning layer:

- Source comments only for a specific missing behavior at a named function or data structure.
- Executable fixtures for regressions, compatibility gaps, and benchmark contracts.
- Documentation for supported behavior and its limits.

If you find an issue, reproduce it, identify its owner and observable contract, then add the smallest focused regression before changing implementation.
If you want to contribute but do not know where to start, choose a narrowly scoped item, find its owner and tests, reproduce the current behavior, and build the smallest verifiable patch.
