# Nytrix Development Guide

A compact reference for working in the Nytrix tree: repository layout, compiler boundaries, native quirks, debugging tools, validation, benchmarking, browser behavior, and memory tooling.

The useful default is simple: start from current source and observed behavior, reduce the problem, change the layer that owns it, and keep the final diff narrow. Historical notes are useful only while they still match the code.

## Repository Map

* `src/` — compiler, runtime, native pipeline, and tools
* `lib/` — standard library
* `etc/tests/` — executable fixtures, shape corpora, and benchmarks
* `etc/projects/` — examples and applications
* `docs/spec/` — exact language behavior and contracts
* `docs/learn/` — workflows and user guidance

Existing utilities, facades, and shared implementations are usually better places to extend behavior than new parallel helpers.

## Source and Documentation Style

Nytrix modules keep `;; Keywords: ...` first:

```ny
;; Keywords: compiler lowering native
;; Optional concise purpose line.

module example
```

Use hyphens for `.nshape` tests and benchmarks:

```text
loop-unswitch.nshape
time-profile.nshape
call-chain.nshape
```

Use underscores for `.c` and `.h` files; C identifiers remain `snake_case`:

```text
loop_unswitch.c
gvn_pre.c
loop_vectorize.c
alias_store_sink.c
slp_vectorize.c
```

### Documentation metadata

Pages under `docs/learn/` and `docs/spec/` may begin with:

```html
<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":10,"summary":"One concise sentence for cards and search."} -->
```

* `audience` — intended reader
* `group` / `order` — grouping and sequence
* `summary` — stable card/search description
* `featured` — eligibility for curated landing placement

Summaries should describe the page, not read like release notes.

Public library facades keep their high-level documentation in source:

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

The purpose line becomes the module summary; the `Documentation:` body renders as Markdown. Facade docs are a good place for scope, namespaces, portability boundaries, and selection guidance. Function docs stay with the declaration that owns them.

Fence Nytrix as `ny`, shell as `bash`, and other formats with their matching language. Examples should state required services, files, libraries, fixtures, endpoints, or platform capabilities before the code. Prefer explicit imports and small assertions; silent success should mean every assertion passed. Specification pages are clearer when they use exact forms and behavior tables rather than tutorial prose.

## Compiler Model

Current source and executable behavior are the primary evidence. For a bug, the useful starting set is the owning implementation, nearest callers, relevant tests, and a minimal reproducer. Search focused source paths before generated, build, cache, vendor, or temporary trees.

Public behavior stays stable unless the change intentionally changes the contract. Unsupported behavior should remain visible and diagnosable instead of silently degrading to another implementation path.

### Important boundaries

* Source semantics and target/ABI policy belong in their owning layers.
* Target-independent NYIR must not depend on physical registers or calling-convention accidents.
* Machine encoders should encode machine form, not reconstruct source-language meaning.
* Target layout and ABI facts should come from shared queries rather than duplicated constants across front end, NYIR, and emitters.
* Dynamic fallback belongs at genuinely uncertain boundaries and should remain explicit.
* A static fast path must preserve the same observable semantics and failure behavior as its general path.
* Source-derived facts become trusted optimizer proofs only through explicit derivation and invalidation rules. A pass-level guess is not a source-level proof witness.
* IO, FFI, synchronization/thread state, traps, volatile state, and floating-environment effects are optimization barriers unless a more precise audited effect summary proves otherwise.
* Diagnostics should be source-span-rich and deterministic. When an optimization is blocked, a concrete reason is preferable to a silent conservative fallback.
* Formatter, parser/compiler, documentation, and LSP syntax rules should stay aligned.
* Unsupported C forms and package metadata should remain diagnosable rather than disappearing during conversion or parsing.
* Benchmark names must never affect compiler behavior.
* General compiler fixes are preferred over permanent duplicate “fast” library variants. Handwritten kernels make sense only where the generic path cannot reasonably express equivalent code.
* Performance guidance should describe measured compiler behavior and, when practical, have a reproducible regression threshold or fixture behind it.

### Language design and syntax discipline

New capabilities should extend existing Nytrix syntax rather than introduce isolated mini-languages.

* Reuse `<...>` for type and compile-time value parameters.
* Use `where` for refinements and constraints.
* Keep `proof<P>` for explicit evidence.
* Reuse `comptime` for static evaluation, specialization, and type normalization.
* Extend existing `@...` attributes for contracts and effects.
* Reuse `shape` for structural typing.
* Use `quote { ... }`, `${...}`, and `emit` for typed metaprogramming.
* Prefer contextual keywords over globally reserved words.
* Introduce no new statement separator; `;` remains a comment token.
* Keep compiler-only concepts such as SSA versions, fact graphs, proof invalidation, and optimization evidence out of source syntax.
* Do not add syntax when inference can express the feature cleanly.

The goal is a larger semantic model with a small, recognizable surface language.

## Correctness and Evidence

A useful result shows more than a zero exit code. For non-trivial changes, the strongest evidence usually covers the changed contract, nearby rejected/boundary/failure cases, the intended implementation path, and any new static-analysis signal in touched code.

An exit status alone is weak evidence whenever another path can produce the same result.

### Native path

Think about native failures in pipeline order:

```text
source -> parsing -> semantic analysis -> lowering -> NYIR
       -> machine form -> allocation -> object/link -> runtime
```

IR, assembly, and object output are intermediate evidence. They can support a hypothesis without proving encoding, linking, or execution.

For native behavior, the strongest check is the intended native path plus an executable oracle. These do **not** prove native support by themselves:

* skipped fixtures
* cache hits
* IR or assembly output
* object output
* interpreter fallback
* LLVM fallback
* host-compiler execution

If a fallback can produce the same observable result, record which implementation path actually ran.

Native representation has an important split: typed `i64` slots contain raw values, while dynamic `NyValue` containers retain tagged values. Many “cast” bugs are really boundary bugs. Boxing, unboxing, or retagging should appear only at a concrete dynamic boundary with a clear owner and reliable oracle.

Keep `NYTRIX_USE_GMP` optional and preserve the documented BigInt layout.

Compiler-enforced language subsets belong in the compiler, not in a linter.

### Diagnostics

Unsupported language, ABI, renderer, and browser shapes should fail with a practical diagnostic rather than quietly changing behavior or execution path.

Good compiler diagnostics keep:

* source location
* stable category
* practical message
* no secondary-error flood

Use structured results for recoverable library operations and compiler diagnostics for language failures.

## Process State and Ownership

The compiler, loader, intern table, and some diagnostic/profile state use process-global state. A compiler invocation is single-threaded with respect to its compilation context.

Parallel tests and builds are fine as separate processes. Sharing one compiler context concurrently, or embedding several independent compiler contexts in one process, is not the supported model.

Ownership and mutability contracts should remain exact. A workaround that moves ownership across a layer often fixes the first symptom while creating a later lifetime or ABI bug.

## Standard Library

After a standard-library change:

1. rebuild the standard bundle;
2. test direct calls;
3. test common composition patterns.

Compact module `#main` self-tests work well for local behavior. Cross-module, ABI, and lifecycle cases belong in external fixtures. Keep one canonical standard-library behavior rather than parallel “special” versions.

A direct call passing is not always enough: imports, callbacks/function values, module composition, and ABI boundaries can expose different failures.

## Source Converters

Extend the existing converters instead of adding a parallel translator. Both entry points live in `src/cmd/fmt/init.c`:

* `c2ny_line` — C conversion
* `py2ny` — Python conversion

Each new source shape should have a converter fixture. Unsupported input stays visible:

```text
// c2ny: unsupported
```

or:

```text
# py2ny: unsupported
```

Pair the marker with a clear diagnostic. Never silently drop source. Where the converter contract allows it, unsupported output should remain compilable so the unsupported shape is visible to both humans and tooling.

## Renderer and Runtime

Renderer fixes belong in shared renderer code. The usual things to check are resource lifetime, failure propagation, headless behavior, and bounded framebuffer/artifact probes.

The runtime amalgamation starts at `src/code/runtime/init.c`; CMake tracks its included sources.

Generated `build/release/std.ny`, generated shared libraries, and generated `.so`/`.dll` files do not belong in source control. Vendored dependencies should change only for a demonstrated defect.

Project-local libraries referenced by `link "...so"` with a sibling C source are built under:

```text
<tmpdir>/nytrix-shlib/
```

That is a temporary build location, not a source directory to copy binaries out of just to satisfy a test.

### Runtime and interop playbook

Runtime failures are usually representation-boundary failures. Identify the
payload on both sides before changing a helper:

| Boundary | Contract | Owning layer |
| --- | --- | --- |
| Typed NYIR | raw `i64`, pointer, or float bits | native lowering |
| Dynamic value | tagged scalar/object plus metadata | runtime adapter |
| Native buffer | header, count, width, raw slots | `runtime/core.c` |
| FFI/process | pointer, fd, errno, or return ABI | OS/FFI runtime |

Do not infer a kind from odd/even bits after a dynamic crossing. Preserve an
explicit tag/length or keep the value canonical until the dynamic consumer
reads it. Typed reads decode once; dynamic reads must not be boxed or unboxed a
second time.

For a runtime or FFI change:

1. Reduce the failure to the first wrong value with a fixture or short
   `ny-full` reproducer.
2. Inspect the emitted NYIR and the call/return ABI at that boundary.
3. Fix the owning layer and add a regression covering payload and metadata:
   value, tag, length, ownership, and failure status where relevant.
4. Run cold, then cover scalar, nil/bool, object, nested, empty, malformed,
   and odd-address pointer cases as applicable.
5. For native work, use the executable result oracle; IR alone is not proof.

```bash
export NYTRIX_NO_PROGRESS=1
NYTRIX_TEST_CACHE=0 build/release/ny-test --bin build/release/ny \
  --color=never --timeout 300 etc/tests/runtime/<fixture>.ny

NYTRIX_STD_CACHE=0 build/release/ny-full --no-progress --color=never \
  --native-only --native-result-oracle etc/tests/runtime/<fixture>.ny

gdb -q -batch -ex run -ex 'bt 20' --args \
  build/release/ny-full --no-progress --color=never <reproducer>
```

For process, socket, window, Vulkan, and terminal fixtures, record capability
limits explicitly. Headless environments may skip device access, but CPU-only
argument construction, EOF handling, fd ownership, and deterministic errors
still need coverage. Never turn a timeout into a pass by raising its limit.
Keep temporary shared libraries under `<tmpdir>/nytrix-shlib/`, and include a
valid plus malformed C-ABI case whenever an interop boundary is changed.

## Build and Validation

Start with the original reproducer or the narrowest relevant check, then widen according to risk.

```bash
NYTRIX_NO_PROGRESS=1 ./make ny --no-progress --color=never
NYTRIX_NO_PROGRESS=1 ./make test --no-progress --color=never
NYTRIX_NO_PROGRESS=1 ./make test --failures-only --no-progress --color=never
NYTRIX_NO_PROGRESS=1 ./make all --no-progress --color=never
NYTRIX_NO_PROGRESS=1 ./make web-test --no-progress --color=never
NYTRIX_NO_PROGRESS=1 ./make bench --no-progress --color=never
NYTRIX_NO_PROGRESS=1 ./make perf --no-progress --color=never
git diff --check
```

### Baseline verification

```bash
./make ny

./make test
# green as of 2026-08-26

./make bench --bench-show-passes
# 56/56 ok

NY_DUMP_OBJ_NYIR=1 \
  ny --native-backend x86_64 -emit-only -o /tmp/x.o f.ny
```

Focused and cold checks:

```bash
./make test --failures-only --color=never \
  --pattern=<focused-fixture>

NYTRIX_TEST_COLD=1 \
  ./make test --failures-only --color=never \
  --pattern=<focused-fixture>
```

Cached iteration is useful while working; the cold run matters because stale artifacts or skipped work can hide behind a cache hit.

For broader changes, use the canonical quality gate:

```bash
./make check
```

`./make check` is fail-closed: it rejects NUL bytes and temporary print-debugging
probes, then runs tidy, formatting/parser verification, the source audit, the
required compiler tools build, and the test suite. It stops at the first
failure. For a focused investigation, run `./make test`
with a pattern before the complete gate; do not substitute that focused run
for it.

## Exact Working Commands

This is the canonical command inventory for native Nytrix development. Always
append `--no-progress --color=never` to `ny`/`./make` invocations and export
`NYTRIX_NO_PROGRESS=1` for child tools. The current standalone `ny-test`
binary does not accept `--no-progress`; use `./make test` to filter that flag,
or set the environment variable and pass only `--color=never` directly.
For a single fixture without the additional full benchmark run:

```bash
NYTRIX_NO_PROGRESS=1 NYTRIX_TEST_COLD=1 NYTRIX_TEST_NO_BENCH=1 \
  ./make test --pattern runtime/modules/import.ny --no-progress --color=never
```

```bash
# Build native compiler
cmake --build build/release --target ny-full -j 8
# or, through the repository driver:
./make build

# Clean rebuild (use after moving/renaming headers)
cmake --build build/release --target ny-full --clean-first -j 8

# Compile-time stress target
NYTRIX_STD_CACHE=1 build/release/ny-full --no-progress --color=never -O0 --native-only \
  --native-backend=x86_64 --native-tier=baseline -emit-only \
  -o /tmp/nytrix-ui-engine.o etc/projects/ui/engine.ny

# Focused semantic regressions
build/release/ny-test --bin build/release/ny --color=never --timeout 300 \
  etc/tests/native/optcheck/loop-predication.nshape \
  etc/tests/native/optcheck/scev-loop-exit.nshape \
  etc/tests/native/nyir/semantic-dynamic-containers.nshape \
  etc/tests/native/nyir/semantic-temp-regressions.nshape \
  etc/tests/native/nyir/semantic-builtin-shadowing.nshape \
  etc/tests/native/nyir/extern-multi.nshape

# Remaining language gates (run individually until empty)
NYTRIX_STD_CACHE=0 build/release/ny-full --no-progress --color=never etc/tests/runtime/language/<file>.ny

# Staged CI validation: run Ubuntu first, then expand to other platforms only
after the Ubuntu run is green. Automatic push runs can be superseded by the
newer commit; inspect/cancel them before dispatching the manual gate.
gh run list --workflow multi-platform.yml --limit 10
gh run cancel <run-id>                 # cancel stale automatic runs
gh workflow run multi-platform.yml --ref main \
  -f target=ubuntu -f suite=standard -f checks=tests
# Only after that run is green:
gh workflow run multi-platform.yml --ref main \
  -f target=macos -f suite=standard -f checks=tests
gh workflow run multi-platform.yml --ref main \
  -f target=windows -f suite=standard -f checks=tests

# One-knob introspection: every trace on, compact header first
NY_TRACE_ALL=1 build/release/ny-full --no-progress --color=never -O2 file.ny

# Deep native diagnostics, one-line records with optional category filtering
NY_TRACE_DEEP=1 NYTRIX_TRACE_FILTER=LOWER,RESOLVE,MACHINE,EMIT \
  build/release/ny-full --no-progress --color=never --native-only file.ny 2>trace.log

# Release compile/object sweep for every OS example (interactive demos need a
# positive step/count argument or a terminal input stream to execute)
for f in etc/projects/os/*.ny; do
  NYTRIX_STD_CACHE=0 build/release/ny-full --no-progress --color=never --native-only \
    --native-backend=x86_64 --native-tier=baseline -O0 -g -emit-only \
    -o /tmp/nytrix-os-check.o "$f" || exit $?
done

# Interactive/project smoke checks should be run individually, for example:
# NYTRIX_STD_CACHE=0 build/release/ny-full --no-progress --color=never --native-only etc/projects/os/ant.ny 100
# NYTRIX_STD_CACHE=0 build/release/ny-full --no-progress --color=never --native-only etc/projects/os/110.ny
#
# `herodoc.ny` is a deterministic native smoke test; generic higher-order
# sequence adapters remain tracked separately until their ABI is complete.
```

When extending the focused regression list, keep the checked-in fixture paths
intact; the files above are the current set under `etc/tests/native/`. When a
focused run goes green, widen to the full gate:

```bash
./make check --no-progress --color=never
```

## Git history and publication

Preserve concurrent working changes. Before an authorized history rewrite,
record the source HEAD and remote tip, create a backup branch and archive, and
verify that both can recover the original state. Work on a separate branch or
isolated checkout. Group commits by coherent behavior; use calendar-day groups
and normalized author/committer timestamps only when the task requests them.

Use Conventional Commit subjects with a mandatory scope and strictly under 68
characters: `<type>(<scope>): concise imperative summary` (for example
`fix(native): preserve typed BigFloat call ABIs`). Plain ASCII and no
generated/co-author trailers. Before publishing a rewritten branch,
compare its tree with the backup and account for every intentional addition:

```bash
git diff --exit-code <rewritten-branch> <backup-branch>
```

An empty diff proves tree equality, not behavioral correctness; the normal
validation gates still apply. Documentation or a handoff is not authorization
to publish. When the user authorizes a rewritten-history push, use
`--force-with-lease` against the verified remote tip; inspect any rejected lease
rather than retrying with unconditional force.

## Git quality hooks

Install the repository hook once after cloning:

```bash
git config core.hooksPath .githooks
```

The hooks are intentionally small and layered:

* `pre-commit` checks staged whitespace, source hygiene, temporary print
  debugging, and formatting without changing files.
* `commit-msg` requires the mandatory-scope Conventional Commit style, such as
  `fix(native): preserve raw string tags`, with subjects strictly under 68 characters (the hook may allow a wider limit).
* `pre-applypatch` and `pre-merge-commit` reuse the same staged protections.
* `pre-push` runs `./make check`, the complete fail-closed repository gate.

Use the compiler trace and diagnostic flags instead of temporary print
debugging. Run `./make check` yourself before pushing to get the same result
without waiting for Git.

Audit findings are actionable: fix them, show that they predate the change and
are unrelated, or narrow the change until ownership is clear. Do not silence a
finding merely to make the gate quiet.

## Debugging

Use the repository driver rather than stale binaries or ad-hoc build trees:

```bash
./make --help
./make env
./make doctor
./make targets
./make ny --help
./make fmt --help
```

### Debug environment cheatsheet

```text
NY_DUMP_OBJ_NYIR   post-build NYIR per function
NY_DUMP_MACH       machine-lowering / machine-form traces
NY_KEEP_ASM        keep temporary .s files

NY_TRACE_LOWER     lowering trace
NY_TRACE_OCE       OCE trace
NY_TRACE_TCO       TCO trace
NY_TRACE_PF        PF trace
```

`NY_DUMP_OBJ_NYIR`, `NY_DUMP_MACH`, and `NY_KEEP_ASM` are usually enough to localize native object failures. Trace switches are more useful one at a time unless the failure is already known to cross several layers. Turning everything on at once tends to produce enormous logs without improving the signal.

A quick triage map:

| Problem               | First evidence                         | Next step                                                        |
| --------------------- | -------------------------------------- | ---------------------------------------------------------------- |
| Parse/type/diagnostic | direct `./make ny` reproducer          | reduce input; preserve location/category                         |
| Native correctness    | `--native-only --native-result-oracle` | `--native-oracle-per-pass`, then inspect NYIR/machine path       |
| Native optimization   | `--nyir-dump=/tmp/name.nyir`           | check the fast-path marker and absence of the replaced slow path |
| Machine/object        | `NY_DUMP_OBJ_NYIR=1`                   | `NY_DUMP_MACH=1`, then preserved asm if needed                   |
| Runtime/JIT           | direct run with `-trace`               | ASan Debug tree; separate JIT limits from source faults          |
| Browser               | `./make web-test`                      | record engine, fixture, marker, and virtual-time budget          |
| Setup/CI              | `./make doctor`                        | inspect each OS/job and the exact failure marker                 |

For huge traces, reduce the source or search for the first failing function/opcode instead of repeatedly dumping the whole program.

For a non-trivial native fix, the most useful record is: exact command and input, expected marker, observed marker, and implementation path exercised.

### Diagnostic inventory

Use these flags at the owning phase. Prefer a focused trace over enabling
every category; do not insert temporary print debugging in source or fixtures.

- **Frontend & HM Type Inference**:
  - `--dump-ast`: dump full parsed AST.
  - `--dump-ast-typed`: dump AST annotated with inferred principal HM types.
  - `--explain=types`: human-readable explanation of inferred types.
  - `--explain=signatures`: explain function signatures and constraint resolution.
  - `--explain=fallbacks`: inspect why expressions degraded or required fallback paths.
  - `--explain=any`: find where and why `any` types were introduced.
  - `--dump-proofs`: dump proven value ranges, array bounds, discharged invariants.
  - `--dump-escapes`: inspect heap vs stack allocation decisions.

- **Lowering & NYIR Pipeline**:
  - `NY_TRACE_LOWER=1`: trace AST-to-NYIR lowering and inlining sweeps.
  - `NY_TRACE_DEVIRT=1`: trace indirect call devirtualization and symbol propagation.
  - `NY_TRACE_ALIAS=1`: trace flow-insensitive alias analysis decisions.
  - `NY_TRACE_NCE=1`: trace null-check elimination.
  - `NY_TRACE_BCE=1`: trace bounds-check elimination.
  - `--nyir-dump-pipeline`: dump full NYIR function representations after every optimization pass.
  - `--nyir-dump=<file>`: dump final NYIR function representations to a file.
  - `--nyir-verify`: strict SSA, dominance, type, and CFG invariant verification.
  - `--pass-stats`: count transforms, bounds checks, allocations, and calls.

- **Native Backend & Execution Oracles**:
  - `--native-only --native-result-oracle`: dual-run native machine code against the reference VM interpreter to immediately pinpoint any divergence.
  - `--native-only --native-oracle-per-pass`: run result oracle after every individual pass to isolate the exact optimization pass causing regression.
  - `--trace-regalloc`: machine virtual register coloring, live intervals, and spills/reloads.
  - `--debug-everything`: enable all debug, parse, scope, HM, escape, and LLVM traces.

- **Runtime & Failure Diagnostics**:
  - `NYTRIX_TRACE=1`: trace runtime dispatch and memory allocation.
  - `NYTRIX_TRACE_CALLS=1`: trace every runtime call and arguments.
  - `NYTRIX_TRACE_VALUES=1`: trace runtime value tagging and conversions.
  - `NYTRIX_TRACE_VERBOSE=1`: verbose runtime execution logs.
  - `NYTRIX_TRACE_FILTER="<regex>"`: filter runtime traces to specific functions or symbols.
  - `gdb -batch -ex run -ex "bt 20" --args build/release/ny-full --no-progress --color=never <args>`: capture stack trace on crash or assertion failure directly.


### Test process hygiene

A terminated test runner may leave fixture descendants consuming CPU or holding
sockets. Inspect process ownership before treating later failures as timeouts:

```bash
ps -eo pid,ppid,pgid,etime,pcpu,args | rg 'build/release/ny|ny-test'
```

Terminate only the PID or process group belonging to the abandoned run, first
with TERM and with KILL only if necessary. Check descendants afterward. Avoid
broad `pkill -f` commands in a shared workspace: they can kill someone else's run.
Fixture timeouts must terminate their whole process group. Preserve replay
output and explicit pass/fail/timeout/skip status in the suite profile.

## Benchmark Discipline

Measure before optimizing. Keep input, compiler flags, target, cache mode, warmups, measured runs, and execution path fixed.

```bash
./build/release/ny-test --bench --bench-engine native \
  --bench-target x86_64 --bench-run <N> --bench-warmup <N>
```

Keep enough context to reproduce a row later:

* executable path
* JIT/native engine
* optimization level
* cache state
* warmups and measured runs
* checksum
* exit status

One run is a smoke test, not performance evidence. Keep compiler/startup wall time separate from self-reported kernel time.

LLVM, MCJIT, ORC, native AOT, native JIT, the legacy AST-to-LLVM path, and C are separate execution paths. A result from one does not represent the others. Compare only semantically equivalent fixtures; when there is a meaningful C analogue, keep matching `source ny` and `source c` blocks. Workloads without one should remain explicitly non-comparative.

For legacy LLVM differential work, use `--legacy-llvm` to execute through the
compatibility AST backend, or `--legacy-llvm-dump=/tmp/program.ll` to emit its
LLVM IR without executing. Legacy mode bypasses JIT caches so dumps always
reflect the selected backend. Pair it with `--emit-ir` or `-dump-llvm` when a
second IR view is useful.

Native AOT/JIT comparison is most useful after recording an LLVM/JIT/C baseline and then enabling the native path explicitly.

Cache hits, skipped engines, failed compiler runs, and zero-duration rows are not performance evidence.

For C/Nytrix/optional-GMP comparisons, the implementations need to perform equivalent work and expose correctness markers before timing differences mean anything.

## Browser Testing

`./make web-test` is the maintained browser path and reads:

```text
etc/tests/native/web/tests.json
```

Each fixture needs an observable browser marker and a bounded virtual-time budget. Fixtures that require a live browser lifecycle should say so and only skip in an explicitly supported headless mode.

`wasm-bare` is the maintained browser contract.

Emscripten is a separate adapter: verify imports, linking, browser execution, and the same observable fixture separately. Keep WebGL2, Asyncify, assets, and persisted VFS behavior behind capability checks.

A browser host boundary is not native filesystem, process, or Vulkan support.

Useful browser reports include the browser/engine, exact command, expected marker, observed result, implementation path, and environmental limits such as headless mode or virtual-time budget.

## CI

Push and pull-request workflows run the normal platform jobs. Memory-safety and coverage jobs are opt-in from **Actions → test → Run workflow**.

* `target` — one platform or `all`
* `suite` — `standard` or `full-stdlib`
* `checks` — `tests`, `memory-safety`, `coverage`, or `all`

Equivalent local commands:

```bash
./make test
./make test --with-stdlib
./make asan --with-stdlib
./make ubsan --with-stdlib
```

## Sanitizers and Memory Tools

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

Direct reproducer:

```bash
build/asan/ny_debug path/to/reproducer.ny
```

Exercise JIT and native execution separately. JIT non-local exits can produce unsupported ASan shadow-stack warnings; a warning or signal exit alone is not sanitizer evidence. Preserve the actual sanitizer report and reproduce the fixture directly before changing allocation or bounds code.

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

### Valgrind

```bash
./make bin

valgrind --leak-check=full --show-leak-kinds=definite \
  --errors-for-leak-kinds=definite --error-exitcode=97 \
  build/release/ny-full path/to/reproducer.ny
```

Use `ny-full`, not the `ny` launcher, so Valgrind follows the compiler process. Invalid-memory reports and definite leaks are actionable; LLVM may retain process-lifetime allocations.

### Coverage

```bash
cmake -B build/cov -DCMAKE_C_FLAGS="--coverage -fno-omit-frame-pointer -g"
cmake --build build/cov -j"$(nproc)"
NYTRIX_BUILD_DIR=build/cov ./make test

lcov --capture --directory build/cov --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage-report
```

## High-Risk Paths

These paths normally deserve focused regression coverage:

```text
src/code/native/
src/base/options.c
src/code/wire/pipe/
```

Shared compiler, runtime, ABI, parser, and standard-library changes usually need wider coverage than a local fix.

For native work, preserve the small reproducer and the implementation path it exercised. For performance work, preserve inputs, compiler flags, cache mode, sample count, execution path, and correctness markers.

## Finishing a Change

A useful final review asks:

* Is the change in the layer that owns the behavior?
* Is there a minimal reproducer or focused fixture for the bug?
* Are relevant rejected, boundary, and failure cases still correct?
* Did the intended implementation path actually run?
* Was cached iteration followed by a cold focused run where relevant?
* Did shared compiler/runtime/ABI/parser/library changes get wider coverage?
* Is `git diff --check` clean?
* Is the diff free of generated output and unrelated edits?
* Were new assertions, diagnostics, allocations, fallbacks, and compatibility shims reviewed?
* Are new static-analysis findings understood?
* For tricky changes, are the exact command/input, expected marker, observed marker, and environmental limits recorded?

Skipped work, cache hits, fallback execution, and untested platforms should be described exactly as that rather than summarized as passing.

## Open Work

Keep unfinished work near the thing it describes:

* source comments for a specific missing behavior at a named function or data structure;
* executable fixtures for regressions and compatibility gaps;
* benchmarks for measurable performance contracts;
* documentation for supported behavior and its limits.

For an unfamiliar bug, reducing the case first usually pays for itself. Once the failing layer is clear, keep the smallest useful regression and fix that layer rather than spreading workarounds across the tree.
