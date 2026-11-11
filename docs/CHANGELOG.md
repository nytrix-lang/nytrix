<!-- nytrix-doc: {"audience":"user","featured":true,"group":"project","order":10,"summary":"Release highlights and notable changes for each version."} -->
# Changelog

Each release starts with a short summary, followed by details when useful.
Entries describe notable changes in that release; measurements and local
artifacts, where mentioned, are not a fresh test of the current checkout.

## [0.10] - 2026-10-13

- **Native backend**: improved x86-64 code generation, register allocation,
  ABI handling, and runtime lowering across compiled programs and benchmarks.
- **Language and compiler**: expanded semantic checks, diagnostics, effects,
  templates, reflection, and compile-time capabilities with targeted correctness
  fixes.
- **Runtime and standard library**: reliability fixes across strings, lists,
  buffers, I/O, crypto, networking, audio, terminal UI, and platform bridges.
- **Cross-target support**: continued work on LLVM, NYR, NYC, and
  architecture-specific backends, with fail-closed behavior where a target does
  not support an operation.
- **Tooling and docs**: benchmark/reporting improvements, clearer contributor
  guides, and consolidated documentation.

<details>
<summary>Details</summary>

### Compiler and backends

- Native reachability now trusts the frontend's resolved builtin-operator
  facts, avoiding repeated module/impl scans for ordinary arithmetic.
  Explicitly selected overloads and unresolved expressions retain target
  discovery; operator regressions include overloaded call-result operands.
- Fixed incremental dependency inventories: cold/forced builds now schedule
  inputs, removed providers invalidate surviving consumers, bare module names
  match `.ny` files, and ambiguous basenames retain all candidate dependencies.
  Graph reload no longer reads a freed pathname; cache roots follow the current
  configuration, and allocation/publication failures are reported.
- HM unification snapshots type-detail/deep tracing policy once per pass,
  avoiding repeated debug-policy calls for each constraint while preserving
  verbose constraint and substitution output.
- Native compile-time cleanup indexes verifier definitions once per verification,
  avoids PHI/select/local-store analyses when their instructions are absent, and
  indexes immediate module imports without changing re-export precedence.
  An isolated three-pair Pong object-compilation comparison reduced median wall
  time from 40.840 s to 39.788 s (2.6%) with byte-identical objects.
- Native x86-64 lowering improved register allocation, call-boundary handling,
  signed arithmetic, address/base forwarding, immediate multiply encoding, and
  several miscompilation/crash cases found in focused regressions.
- Refinement lookup now indexes the main and generated program trees while
  preserving traversal precedence, avoiding repeated full-AST scans for
  distinct refinement names.
- Exported-member checks scan module declarations directly instead of
  allocating and deduplicating a temporary export-name list.
- Module alias resolution writes bounded path copies and joins directly,
  avoiding formatted-I/O calls in its recursive lookup path.
- Native module-member lookup caches compare the stored key hash before
  comparing member strings, avoiding string scans on set collisions while
  retaining exact module/member verification.
- Expanded the four-way module-member lookup cache from 16,384 to 65,536
  entries after cold Pong profiling showed repeated imported-member tree scans;
  the cache-disabled trace recorded 7.4% fewer such scans, but no total
  compile-time speedup is established.
- x86-64 home DSE now skips reference-buffer allocation and byte scanning when
  a function has no scalar homes or the probe emits no home stores; the probe
  encoder is still reused, preserving emitted bytes.
- Native list append dispatch now gives f32/f64 classification precedence over
  stale raw-integer ABI facts, preventing floating values from reaching the
  raw-i64 append entry. Terminal/editor native fixtures and integer-append
  regressions pass.
- HM environment lookup caches positive and negative results, checking
  appended bindings before reusing either result to preserve shadowing and
  syntax-context behavior across cloned environments.
- HM unification now skips constraint type-string formatting and allocation
  when type-detail tracing is disabled; `-vvvv --stop-after=hm` still emits
  constraint and substitution details.
- HM function-scheme inference reuses its temporary environment buffer across
  sequential schemes instead of allocating and freeing a buffer per function.
- HM environment clones reserve their final capacity once and copy entries in
  bulk instead of growing the vector one binding at a time.
- Loop-phi register coalescing now recolors a preheader source only when it
  dies at the copy and candidate-color constraints pass; call-crossing and
  non-dying values retain their existing register/home behavior.
- NYR and LLVM execution paths received targeted correctness and performance
  work, including typed-buffer/store behavior, inlining/splice budget handling,
  block layout guards, range/memory facts, and runtime fast paths where safe.
- NYC/C frontend work expanded accepted C constructs and fixed targeted parser,
  preprocessor, declarator, initializer, typeof, array, and assembly-related
  cases. Target-dependent optimization and instruction selection remain limited.
- Cross-target backends (AVR, ARM/AArch64, i386, RISC-V64, MIPS64, PowerPC64,
  Wasm/BPF-related paths, and object/ABI handling) received focused ABI,
  lowering, and fail-closed fixes covered by targeted tests.

### Language, diagnostics, and analysis

- Added and refined compile-time checks, effect handling, proof/refinement
  behavior, template/macro diagnostics, and type/ownership-related validation.
- Improved diagnostics for unsupported operations, invalid compile-time claims,
  and backend/target limitations so failures are explicit rather than silently
  miscompiled.
- Fixed targeted semantic/lowering issues involving partial application and
  currying paths, callback/effect inference, dynamic indexing cases, and
  ABI-sensitive call lowering.
- Fixed NYR lowering for integer conversion of dynamic-list elements, tagged
  floating-point builtin returns, raw-integer `append` results, and list
  copy-on-write aliases mutated inside loops; the affected regression fixtures
  pass their configured execution variants.
- Fixed DCE's unreachable-instruction removal to erase in reverse index order,
  preserving original CFG-to-instruction mappings while compacting SSA.
- Vectorized loop reductions now initialize unused NYIR operands to the
  sentinel value, so later SSA compaction does not treat zero as a live value.

### Runtime and standard library

- Fixed correctness issues in string/list/buffer operations, hashing/crypto
  helpers, subprocess/process handling, terminal and UI behavior, audio
  resource loading, HTTP handling, and platform-specific bridges.
- Mock-renderer CPU surfaces are now freed when switching to a hardware
  renderer, on failed hardware initialization, and on close/reinitialization.
- Mock fast-fill skips are disabled during auto-dump, frame-hash, and UI replay
  capture so those captures retain the clear color and rendered pixels.
- Terminal renderer readback buffers are released through a deferred,
  type-checked cleanup helper, including when ANSI conversion exits early.
- The editor's headless mock exit now uses the render facade's exported
  `close_window` API.
- Editor line limits now derive from the current measured body-font advance
  instead of a fixed 7.5-pixel cell width.
- Fixed AOT UI font registration by keeping the module-level font counter
  explicitly typed; added O2 coverage and verified the Pong font load on the
  native Vulkan path. Cached exact-pointer SSO readability probes with
  lifecycle invalidation, reducing repeated runtime address checks.
- Added pure-Ny AU ADPCM (G.721/G.722/G.723), native FLAC/Ogg FLAC, and MPEG
  Layers I–III decoding. Compressed streams support frame-range reads and
  seeking without requiring libsndfile for those formats.
- Preserved runtime contracts in focused fixes, including typed-buffer store
  return values and native lowering behavior for affected paths.
- Reduced avoidable overhead in selected hot paths (for example, hashing/hex
  formatting, string building, and targeted runtime lookups/caches) while
  keeping behavior and checksums stable.

### Projects, benchmarks, and tooling

- Improved example projects and interactive demos (terminal graphics, audio,
  HTTP, and small games/utilities) with focused correctness and rendering fixes.
- Benchmark harness output and reporting were refined for clearer per-fixture
  results, comparable ratios, and optional metric collection; benchmark gates
  and fixture checksums remain the source of truth for comparisons.
- Test/CI and build tooling received reliability fixes for worker isolation,
  cache reuse, platform differences, and focused regression coverage.

### Documentation

- Consolidated contributor guides, measurements, and implementation notes;
  removed obsolete scratch/TODO material from active docs.
- Editor compile-time investigation is closed. The validated 2026-10-22 editor
  compile comparison remains in `docs/learn/measurements.md`. Future editor
  compile-time regressions should be reported with a fresh reduced reproducer
  and validated baseline/candidate evidence (same source graph, build, target,
  flags, cache state, hashes/BuildID, inline-decision evidence, and validated
  CPU samples).

</details>

## [0.9] - 2026-08-21

- C header imports, subsystem reorganization, and native/runtime reliability.

<details>
<summary>Details</summary>

- Development period: 2026-07-21–2026-08-21.

### Added

- In-tree C header frontend replaces libclang for supported arrays, unions,
  packed structs, bitfields, extended numbers, function pointers, typedefs,
  macros, and variadics. Untrusted headers have parse deadlines, recursion
  limits, and bounded tables.
- Optional GMP with native BigInt fallbacks; `-Oz`, compile-time `#assert`,
  formatted `core.report` diagnostics, managed-heap `--zero-init`, and
  `core.os.rev` decompilation/symbolic-execution modules.
- Linux/x86-64 syscall restrictions on process creation and executable-memory
  remapping through `mprotect`/`clone`/`fork`/`vfork`.

### Changed

- Explicit subsystem boundaries under `src/base`, `src/cmd/ny/repl`, and
  `src/code/*`. Optional LLVM emission/JIT joins the native backend; direct
  AST-to-LLVM remains behind a semantic-parity gate.
- Native/self-build links default to the system linker; fast alternatives
  require explicit selection.
- Expanded diagnostics and measurements: register allocation, allocation
  sampling, native/C and native/LLVM budgets, emitted bytes, peak RSS,
  vectorization profitability, static-island quality, and inlining decisions
  including monomorphization rejection.
- Tagged-int bitwise operations, proven-safe shifts, and ordered comparisons
  use fast paths under `--profile=speed`.

### Fixed

- SSA: mem2reg parameter promotion, escape-SROA slots, and native allocation
  and bounds facts for `f64buf`/`i64buf`.
- Sockets: decode ephemeral ports from sockaddr bytes; return the raw
  descriptor from `socket_accept`.
- Unified loop analysis; repaired vector-tail preheaders, PHIs, and scalar
  induction remapping, syscall/`rdrand` assembly guards, AArch64 conditions,
  and BigInt heap corruption.

</details>

## [0.8] - 2026-07-13

- Proof witnesses, filesystem watching, cache integrity, and platform fixes.

<details>
<summary>Details</summary>

### Added

- Standalone `wasm-emscripten` output with `_ny_top_entry` and explicit
  `env.*` namespacing through aliases; existing declarations retain precedence.
- `prove(condition[, message]) -> proof` witnesses reject false/dynamic
  obligations and ordinary values in proof parameters. `core.math.logic`
  adds evaluation, simplification, certificates, bounded solvers, rewriting,
  and Prolog-style unification/backtracking.
- `core.os.fs.watch_*` and hot reload use inotify, kqueue, or Windows change
  notifications, with an mtime fallback.

### Changed

- NYIR coalesces copy/local chains, allocates scalar registers, selects
  immediates, indexes DCE labels once, and preserves floating types across
  collapsed equivalence classes.
- Precomputed x86-64 call boundaries and immediates reduce the measured call
  body and frame-relative accesses.
- Library sweeps stop after optimized IR rather than materializing MCJIT.
  Cache formats reject mixed library/user entries and sanitizer-tainted objects.
- `ny-fmt --cloc` reports tracked additions/deletions and per-file totals.
- Hot reload blocks on native events rather than busy polling.
- The opt-in JIT shared-object tier checks its bitcode-provenance sidecar;
  stale/interrupted entries return to normal compilation.

### Fixed

- REPL v2 snapshots bind source to a fingerprint and byte length; `:load`
  rejects corrupt payloads before rebuilding state. Older source-only snapshots
  remain accepted.
- One C lexical scanner serves formatter function ranges, rankings, and
  duplicate detection, excluding strings, comments, multiline macros,
  conditions, and call continuations.
- `ny-fmt --bugs` diagnoses discarded value-typed `list.append`/`extend`
  results (`NYAUD1120`) and `set()` on frozen `{}` dictionaries (`NYAUD1119`).
- Internal x86-64 JIT/object bundles support 1,024 relocations, up from 256.
  Live requests support 128 lowered user functions, up from 64, and reject
  larger requests before lowering; portable NYIP retains its separate
  4,096-function VM boundary.
- macOS libc imports materialize named aggregate parameter/return layouts
  without registering anonymous carriers as scalars. System headers recover
  supported declarations; project headers remain strict.
- Apple-arm64 compile-time MCJIT finalizes indirect callees through managed
  invocation. Native-only link discovery matches JIT and deduplicates annotations.
- Preserved x86-64 floating constants and typed f32/f64 locals, removing
  nondeterministic ELF results.
- Hardened sanitizer AOT output, cache isolation, cleanup, and UBSan handling.
- Repaired Windows JIT, target-width libc fixtures, variadic C imports,
  trace/debug progress suppression, ELF32 returns, watcher lifetime, parser
  recovery, and dictionary helper ambiguity.

</details>

## [0.7] - 2026-06-30

- LLVM-free native backends, object emission, and C interoperability.

<details>
<summary>Details</summary>

### Added

- Native emitters for x86-64 (default/primary), i386, ARM, AArch64, and
  RISC-V, with debug-scoped WASM, BPF, PowerPC, MIPS, and AVR support.
- In-process ELF64, ELF32, COFF, and Mach-O object writers with relocations
  and multi-function aggregation.
- Narrow internal ELF executable linker with runtime stubs for
  `malloc`/`free`/`realloc`/`calloc`, `memset`/`memcpy`/`memmove`/
  `memcmp`/`memchr`, and `strlen`/`strcmp`/`strchr`.
- `*p` dereference expressions parse as `NY_E_DEREF`, type-check as the
  pointed-to type, lower to `NYIR_LOAD_I64`, and support `*p += value`.
- `addr_of(local)` lowers to `NYIR_ADDR_LOCAL`, executes in the debug VM, and
  emits frame-relative `lea` on x86-64/i386; address-taken local facts are
  invalidated after raw pointer writes so later reads observe mutations.
- The wasm-emscripten web target emits standalone Wasm with `_ny_top_entry`;
  explicit aliases opt into namespacing while existing Nytrix declarations
  keep precedence.
- C frontend hardening: internal aggregate imports decline unsupported nested
  or by-value layouts without poisoning the fallback; strict no-libclang
  aggregate-return imports are covered through `load_layout`; non-positive
  array extents are rejected; unsupported C declarations produce recoverable
  diagnostics instead of hard aborts.
- Parser diagnostics no longer suggest C-style `for (;;)` and point to Nytrix
  iterator syntax instead.

</details>

## [0.6] - 2026-06-30

- Fuzzing, cryptography/math modules, rendering, and platform tooling.

<details>
<summary>Details</summary>

### Added

- Published fuzzer and tooling for local benchmarking and error-shape
  discovery.
- Radix helpers, stream/block ciphers, public-key helpers, and
  lattice/factorization modules.

### Changed

- SVG/UI rendering: 4x4 supersampling, stroke linecap/linejoin,
  gradient/`<use>` support, and terminal 256-color output.
- `--borrow-check` decoupled from `--ownership-strict`; Z3 enabled by default;
  proven-nonzero `f64` division checks elided.
- glTF hot paths moved from `src/code/lang/runtime/gltf.c` into Ny code.
- CMake dependency probing hardened for LLVM, libclang, Z3, Windows
  UCRT/MSYS2.

### Fixed

- Canvas UTF-8 buffer types, terminal rendering, lowercase type-first local
  bindings, and semicolon-comment diagnostics.
- Windows builds, joystick axes, SDK/toolchain probing, and zlib decompression
  capacity handling.

</details>

## [0.5] - 2026-06-05

- Editor/viewer framework, renderer layers, and initial WebAssembly support.

<details>
<summary>Details</summary>

### Added

- Editor and engine viewer (`core.os.ui.render.viewer`): asset browser,
  hierarchy, inspector, gizmos, transform tools, and runtime bootstrap.
- OpenGL, WebGL, and Vulkan renderer paths for the viewer.
- WebAssembly compiler backend foundation.
- RSS feed, Discord, and Mastodon integration.

### Changed

- Renderer/viewer split into distinct `render` and `viewer` layers.
- Function syntax moved from `fn foo(type: arg): ret` to
  `fn foo(type arg) ret`.
- Module self-checks moved into `#main` blocks.

### Fixed

- Vulkan UI mesh caching and startup text-fitting crashes; animated glTF
  index-buffer retention and texture reuse; GLSL syntax and redraw stability.

</details>

## [0.4] - 2026-05-30

- Ownership checks, typed compilation, and a unified CLI.

<details>
<summary>Details</summary>

### Added

- Cross-platform windowing/input: Win32, Cocoa, X11, Wayland, Vulkan.
- Typed compiler pipeline: Hindley-Milner inference, lambda/nested-collection
  inference, and monomorphic specialization.
- `&expr` shorthand for `borrow(expr)`, ownership contracts, `--safe-mode`.
- `handle`, `fnptr`, `seq` types; layout records/guards with compile-time
  reflection.
- Unified CLI: `ny fmt`, `ny test`, `ny doc`, `ny perf`, `ny make`, `ny pkg`,
  `ny new`.

### Changed

- Compiler/runtime/Vulkan internals standardized on raw integer
  representations.
- `-O2` became the default native optimization level.
- Bootstrap and dependency discovery reworked for cross-platform setup.

### Fixed

- Emit-only hangs in recursive raw-integer fast paths and macOS arm64
  compile-time evaluation of immutable collections.
- FFI header collisions, ownership diagnostics for returned values, and mutable
  closure captures across repeated calls.

</details>

## [0.3] - 2026-04-13

- Graphics, media assets, networking/audio, and platform expansion.

<details>
<summary>Details</summary>

### Added

- glTF loading, Meshopt integration, mesh/glTF parsers, and an image parser
  stack.
- Vulkan rendering, scene graph, sky/SDF shaders, and split Vulkan/GUI
  renderer paths.
- Terminal renderer integrated into `core.os.ui`; Win32 window backend added.
- IO and networking modules; JACK audio backend.
- Public fonts, dictionaries, website assets, and renderer shaders.
- Maintained sample programs, REPL import scenarios, and an updated learning
  guide.

### Changed

- Keep canonical stack homes current for dedicated loop GPRs, including
  non-resident CFG-exit consumers.
- Loop GPR homes are enabled by default; `NYTRIX_DISABLE_LOOP_GPR=1` disables
  them for backend diagnosis. Calls within loops remain conservative.
- Platform APIs moved into `core.os`; window backends moved into
  `core.os.ui.window`.
- Legacy native window backend path removed in favor of the new backend split.
- Cache management, bigint support, and shader generation improved for
  graphics workloads.

### Fixed

- Asset paths and shader generation during scene-coverage expansion; runtime
  fixture mismatches from moving platform code into `core.os`.

</details>

## [0.2] - 2026-03-09

- Compiler/runtime bootstrap, semantic analysis, REPL, library, and manuals.

<details>
<summary>Details</summary>

### Added

- Parser, lowering pipeline, AST node definitions, and visitor/function
  lowering.
- Semantic analysis, diagnostics, and statement/call/FFI lowering.
- JIT lowering state, module/JIT integration, and native value-runtime bridge.
- Interactive reader, REPL completion, and build/web launcher with LSP
  commands.
- Enums, packed layouts, `sizeof`, pointer dereference, `try`, reflection
  operators, effects, and the `#main {... }` entry guard.
- Core text, numeric, and cache modules; core IO and string helpers.
- Early UI facade, Vulkan renderer core, and native window/input backends.
- Network/audio backends; block cipher, factorization, RSA lattice, ECC/DLP,
  hash/PRNG, and public-key crypto helpers.
- Specification manuals, release notes, and initial benchmark/regression
  baselines.

### Changed

- Standard library moved to `lib/`, reducing prelude coupling.
- Parser, Vulkan renderer core, and UI renderer split into focused modules.
- Std module layout reorganized; numeric modules moved into `core.math`.
- Python build/bundle tooling replaced with native tools.

### Fixed

- Standard-library import coupling and module-path drift.

</details>

## [0.1] - 2025-12-24

- Initial buildable launcher/runtime tree and smoke fixtures.

<details>
<summary>Details</summary>

### Added

- Launcher skeleton, build script, and CMake scaffold (`make`,
  `CMakeLists.txt`, `src/cmd/ny/main.c`).
- Runtime placeholders and smoke fixtures for a first compilable, testable
  tree.

</details>
