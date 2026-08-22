# Changelog

## Native optimizer hardening (2026-08-21)

- Hardened GVN/CSE/LICM around audited call effects and complete call-argument semantics.
- Unified natural-loop discovery across loop analyses/transforms; fixed SCEV wrap/CFG edge cases and safe IRCE/LICM preheader motion.
- Repaired guarded variable-trip vector tails: real preheader insertion, stable CFG labels/PHIs, scalar-IV remapping, and no duplicate exit labels.
- Expanded native performance diagnostics, regalloc attribution, runtime/static counters, and enforceable benchmark budgets.


Nytrix uses dated milestones. Use `ny --version` for snapshots.

## [0.9.0] - 2026-07-21 → 2026-08-21 - Language security, native tooling, and runtime reliability

### Added
- Repaired SSA PHI and parameter promotion in `nyir_mem2reg`: parameters preserved as initial reaching definitions instead of uninitialized zero rewrites, and added safe rollback on failure.
- Fixed `escape_sroa` parameter slot handling so incoming function parameters are not overwritten with zero.
- Fixed native buffer allocation fact computation and bounds-check offset calculation in `f64buf_new`/`i64buf_new` and `f64buf_load`/`f64buf_store`.
- Converted all internal modular `.inc` files (`lower_*.inc`, `core/*.inc`) to `.h` header format for syntax highlighting and standard C toolchain support.
- In-tree C frontend for FFI headers, replacing libclang for supported arrays,
  unions, packed structs, bitfields, extended numeric types, function pointers,
  typedefs, macros, and variadics.
- Hardened parsing of untrusted C headers with deadlines, recursion limits,
  bounded tables, and parser cleanup.
- Linux/x86-64 syscall restrictions block process creation and executable-memory
  remapping through `mprotect`, `clone`, `fork`, and `vfork`.
- `#assert(cond[, msg])` for compile-time assertions and improved runtime
  assertions with source expressions and `file:line:col` diagnostics.
- `std.core.report` for formatted errors, warnings, notes, locations, and source
  underlines.
- `--zero-init`, `--no-zero-init`, and `NYTRIX_ZERO_INIT` controls for managed
  heap initialization.
- Optional GMP support with native bigint fallbacks for arithmetic, roots,
  modular operations, conversion, and number-theory helpers.
- `-Oz` size optimization.
- Public `std.os.rev` modules for decompilation, symbolic execution, constraint
  solving, and string analysis.

### Changed
- Canonical benchmarks can opt into runtime allocation/reallocation sampling with `--bench-runtime-counters`; the report artifact is sampled outside timed runs and the counts are exported to console, CSV, JSON, and Markdown.
- Native static-island reporting now distinguishes direct from unresolved calls,
  reports alias-uncertain raw-memory/unknown-call sites, and the canonical bench
  runner can enforce zero-capable allocation/helper/bounds/indirect/effect/alias/
  spill/reload limits. Linux benchmark reports can optionally collect `perf stat`
  cycles/instructions/branch/cache counters outside timed samples.
- Specialization code growth now reports total specialization bytes, function
  count, and largest specialized function, with benchmark metadata budgets for
  both module-wide and per-specialization code size. Bounds-check elimination
  also removes identical checks when an earlier identical SSA check dominates
  the later one.
- Native tier reports now provide a per-function static-island quality row with
  dynamic/tag/boxing/allocation/helper/bounds/effect/vectorization/regalloc
  counters, per-function emitted code bytes, explicit monomorphized
  specialization byte attribution, and matching per-function/hot-loop regalloc
  telemetry on x86-64 and AArch64.
- The general NYIR inliner now materializes mutable/address-taken formals into
  caller-local slots when safe, applies immediate SCCP/CFG/DCE cleanup, gives
  bounded static loop-depth benefit credit, and traces detailed accept/reject
  profitability reasons. Monomorphization tracing now also explains recursion,
  unsupported-shape, body-cost, cap, keyword/list-only, and missing-fact
  rejections.
- The canonical benchmark report now records actual native machine-code bytes
  and compiler peak RSS using a report-only compile outside timed samples. Bench
  fixtures can enforce compile-time, code-size, peak-RSS, native/C, and
  native/LLVM budgets; CSV/JSON/Markdown preserve the measured diagnostics and
  budget state.
- Vectorization diagnostics now retain attempted/rejected/successful-loop counts,
  compare eligible vector widths, use retained range bounds in profitability,
  and emit deterministic rejection reasons for retained scalar loops.
- Benchmark reports now record p95/dispersion/noise, flag unstable repeated
  measurements, capture host/toolchain/revision metadata, enforce optional
  per-fixture native/C and native/LLVM ratio budgets, and highlight the largest
  backend gaps while keeping correctness smoke runs separate from performance
  samples.
- Native optimizer rewrite groups stop on structural fixed points or detected
  cycles instead of relying on instruction count plus an arbitrary pass count.
- Existing loop vectorization now has an explicit profitability gate for setup
  cost, a guarded variable-trip vector bound, and scalar cleanup tails; the x86
  backend already folds encodable constants into immediate forms instead of
  materializing them when the consumer permits it.
- Native regalloc telemetry now records reload totals and peak live pressure for
  GPR, FPR, and vector classes separately; x86-64 tier reports attach those
  metrics to individual functions, and verifier diagnostics include CFG block
  identifiers when available.
- Package archive lock entries now include a deterministic content checksum; git
  lock entries continue to retain resolved commits.
- The maintained performance-cliff triage procedure moved from recurring TODO
  checkboxes into the performance guide.
- JIT and standard-library bitcode caches now reject LLVM-invalid modules before
 publication and invalidate prior cache protocols, preventing repeated
 `Invalid record` failures after a cache hit.
- Argument-matrix coverage for documented `ny` options is now committed as
 ny-test fixtures: `etc/tests/errors/args/` asserts invalid flag values fail
 with their diagnostic, and `etc/tests/runtime/args/` asserts valid
 performance/runtime flag combinations compile and run. Both run under
 `./make test`.
- Tagged-int binary fast paths now cover bitwise ops (`&`, `|`, `^^`),
 provably-safe shifts (`<<`, `>>` with the shift count known in `[0, 64)`),
 and ordered comparisons, in addition to add/sub/mul/div/mod. Proven-operand
 expressions emit inline raw-int IR with no tag-check guard or BigInt
 fallback PHI (verified: 28 `bin.runtime.slow` blocks to 0 for a mixed
 operator function under `--profile=peak`).
- `--profile=speed` now enables the same raw tagged-int expression fast paths
 as `peak` (`NYTRIX_RAW_INT_EXPR_FAST` and the `NYTRIX_RAW_INT_EXPR_FAST_OPS`
 operator list, whose default now covers every supported operator kind).
- Added a `wasm-emscripten` target to `./make web`. It emits standalone Wasm
 with `_ny_top_entry`, explicit Nytrix `env.__MD_ITALIC_2__` namespace. Explicit aliases opt into namespacing, while existing Nytrix
 declarations retain precedence.
- `prove(condition[, message]) -> proof` introduces compile-time proof
 witnesses; false or dynamic obligations fail compilation, and ordinary values
 cannot satisfy proof parameters. `std.math.logic` adds evaluation,
 simplification, certificates, bounded solvers, rewriting, and Prolog-style
 unification and backtracking.
- Kernel-backed file watching and hot reload use inotify, kqueue, and Windows
 change notifications behind `std.os.fs.watch`, with an mtime fallback.
- Opt-in `--safe-run` supervision covers CPU, memory, processes, wall time,
 output, and supported file limits, including suspended Windows Job Object
 startup and explicit unsupported-limit reporting.
- Test tooling gained `--failures-only`, portable replay, separate fixture and
 suite timeouts, and host-aware concurrency capped at eight workers with
 6 GiB reserved per worker.

### Changed

- Native lowering, targets, tiers, reporting, NYIR passes, object formats,
 result oracles, JIT loading, and proof analysis now live in focused modules.
- Native-only compile and run modes are now distinct: `-o` writes an executable
 without running it, while ordinary files and `-c` execute through the selected
 host-native path.
- NYIR now coalesces copy/local chains, allocates scalar registers, selects
 immediate operands, indexes DCE label references once, and preserves floating
 types across collapsed equivalence classes.
- Precomputed x86-64 call boundaries and immediate constants reduced a focused
 call body and frame-relative accesses in the native encoder.
- Compiler and backend performance claims are now exercised through maintained
 benchmark and native-oracle fixtures rather than release-note timing samples.
- Stdlib source sweeps stop after optimized IR instead of materializing MCJIT,
 and cache format updates reject mixed stdlib/user entries and
 sanitizer-contaminated native objects.
- Default builds run a bounded, advisory `ny-fmt --bugs` audit after producing
 the compiler and standard bundle.
- `ny-fmt --cloc` now reports tracked additions/deletions and per-file totals.
- Hot reload blocks on native events instead of busy mtime polling, reducing
 idle CPU use and edit-to-recompile latency.
- The opt-in JIT shared-object tier now validates the matching bitcode
 provenance sidecar before loading, so interrupted or stale cache entries
 return to normal compilation instead of being trusted.

### Fixed

- Stage artifacts now carry a pointer-free expanded-source identity and
 `--verify-artifact` rejects stale or malformed snapshots before reuse.
- New REPL snapshots bind their source payload to a fingerprint and byte length;
 `:load` rejects corrupted v2 images before rebuilding persistent state while
 continuing to accept older source-only snapshots.
- `ny-fmt` C analysis now shares one lexical scanner across function ranges,
 structural rankings, and duplicate detection. It ignores strings, comments,
 multiline macros, control conditions, and call continuations rather than
 reporting phantom C functions. `ny-fmt --selftest` validates that scanner
 together with high-confidence language-pattern checks.
- `ny-fmt --bugs` now reports discarded value-typed `list.append`/`extend`
 results and `set()` calls on dictionaries initialized as frozen `{}` literals,
 with stable `NYAUD1120` and `NYAUD1119` diagnostics and practical fixes.
- x86-64 internal JIT/object emission now carries up to 1,024 call/data
 relocations per program bundle, so large straight-line call sites no longer
 fail at the old 256-relocation transport ceiling.
- Live native JIT/object requests now support up to 128 lowered user functions
 (previously 64) and reject larger requests before lowering. Portable NYIP
 bundles retain their separate 4,096-function NYIR VM boundary.
- macOS transitive libc aggregates now materialize named return and parameter
 layouts on demand, without registering anonymous carriers as builtin scalars.
 Installed system headers recover useful declarations from unsupported syntax;
 project headers remain strict.
- Apple-arm64 comptime MCJIT now uses managed invocation so indirect callees
 finalize before entry. Native-only link discovery also matches JIT behavior
 and deduplicates source annotations.
- Corrected x86-64 floating constant placement and typed f32/f64 local
 preservation, eliminating nondeterministic native ELF results.
- Hardened sanitizer AOT temporary output, cache isolation, cleanup, and UBSan
 handling.
- Failure replay now preserves fixture flags, target matrices, exit status,
 plain output, and valid LLDB diagnostics.
- Corrected Windows JIT compatibility, target-width libc fixtures, variadic C
 imports, trace/debug progress suppression, ELF32 return bounds, watcher
 lifetime, parser recovery, and dictionary helper ambiguity.
- The full suite passes on Linux, macOS, and Windows through the manual
 multi-platform workflow.

## [0.7.0] - 2026-06-30 - LLVM-free native backend and C interoperability

### Added

- NYIR: Nytrix-owned IR with verifier, optimizer, debug VM, binary format, and
 `--nyir-run`, `--nyir-dump-bin`, and `--nyir-run-bin`.
- Native emitters for x86-64 (default/primary), i386, ARM, AArch64, and RISC-V,
 with debug-scoped WASM, BPF, PowerPC, MIPS, and AVR support.
- In-process ELF64, ELF32, COFF, and Mach-O object writers with relocations and
 multi-function aggregation.
- Compiler-owned ELF64 and ELF32 link/run paths, avoiding LLVM, `cc`, and
 external linkers for supported native fixtures.
- Narrow internal ELF executable linker with runtime stubs for:

 - `malloc`, `free`, `realloc`, and checked-product `calloc`
 - `memset`, `memcpy`, `memmove`, `memcmp`, and `memchr`
 - `strlen`, `strcmp`, and `strchr`
- Native link/run regression coverage for:

 - i64, f64, f32, pointers, dereferences, locals, branches, loops, and recursion
 - Register and stack-passed i64/f64 arguments, including mixed calls
 - f32 arithmetic, comparisons, register/stack calls, and f64 observation
 - Narrow ABI returns: `bool`, `u8`, `i16`, and `u32`
 - Signed division, modulo, comparisons, arithmetic shifts, and high-bit u32 immediates
 - `__MD_ITALIC_1__p` now:

 - Parses as `NY_E_DEREF`
 - Type-checks as the pointed-to type
 - Lowers reads to `NYIR_LOAD_I64`
 - Supports `__MD_ITALIC_0__p += value`
- `addr_of(local)` now lowers to `NYIR_ADDR_LOCAL`, executes in the debug VM,
 and emits frame-relative `lea` on x86-64/i386.
- Address-taken local facts are invalidated after raw pointer writes so later
 local reads observe mutations.
- Internal C aggregate imports decline unsupported nested or by-value layouts
 without poisoning fallback.
- Strict no-libclang aggregate-return import is covered through `load_layout`.
- C frontend rejects non-positive array extents and diagnoses unsupported field
 shapes instead of silently dropping them.
- Unsupported C declarations produce recoverable diagnostics instead of hard aborts.
- Parser diagnostics no longer suggest C-style `for (;;)` and instead point to
 Nytrix iterator syntax.

## [0.6] - 2026-06-30 - Fuzzing, crypto/math expansion, renderer polish

### Added

- Benchmark shapes (`etc/tests/bench/*.nshape`) for call-heavy, matrix,
 string, and checksum workloads.
- Published fuzzer and tooling for local benchmarking and error-shape discovery.
- Radix helpers, stream/block ciphers, public-key helpers, and
 lattice/factorization modules.

### Changed

- SVG/UI rendering: 4x4 supersampling, stroke linecap/linejoin,
 gradient/`<use>` support, and terminal 256-color output.
- `--borrow-check` decoupled from `--ownership-strict`; Z3 enabled by default;
 proven-nonzero `f64` division checks elided.
- glTF hot paths moved from `src/rt/gltf.c` into Ny code.
- CMake dependency probing hardened for LLVM, libclang, Z3, Windows UCRT/MSYS2.

### Fixed

- Canvas UTF-8 buffer type mismatches and terminal renderer edge cases.
- Lowercase type-first local binding parsing.
- Semicolon comment ambiguity in parser diagnostics.
- Windows build integration, joystick axis handling, SDK/toolchain probing.
- zlib decompression capacity handling.

## [0.5] - 2026-06-05 - Editor/viewer framework

### Added

- Editor and engine viewer (`std.os.ui.render.viewer`): asset browser,
 hierarchy, inspector, gizmos, transform tools, and runtime bootstrap.
- OpenGL, WebGL, and Vulkan renderer paths for the viewer.
- WebAssembly compiler backend foundation.
- RSS feed, Discord, and Mastodon integration.

### Changed

- Renderer/viewer split into distinct `render` and `viewer` layers.
- Function syntax moved from `fn foo(type: arg): ret` to `fn foo(type arg) ret`.
- Module self-checks moved into `#main` blocks.

### Fixed

- Vulkan UI mesh caching and text-fitting crashes on startup.
- Animated glTF mesh index-buffer retention and texture reuse.
- GLSL syntax restoration and screen redraw stability.

## [0.4] - 2026-05-30 - Ownership, typed pipeline, CLI unification

### Added

- Cross-platform windowing/input: Win32, Cocoa, X11, Wayland, Vulkan.
- Typed compiler pipeline: Hindley-Milner inference,
 lambda/nested-collection inference, and monomorphic specialization.
- `&expr` shorthand for `borrow(expr)`, ownership contracts, `--safe-mode`.
- `handle`, `fnptr`, `seq` types; layout records/guards with compile-time reflection.
- Unified CLI: `ny fmt`, `ny test`, `ny doc`, `ny perf`, `ny make`, `ny pkg`, `ny new`.

### Changed

- Compiler/runtime/Vulkan internals standardized on raw integer representations.
- `-O2` became the default native optimization level.
- Bootstrap and dependency discovery reworked for cross-platform setup.

### Fixed

- Emit-only compiler hangs from recursive raw-integer fast paths.
- macOS arm64 comptime evaluation for immutable collections.
- FFI header import collisions and ownership diagnostics for returned values.
- Mutable closure captures across repeated calls.

## [0.3] - 2026-04-13 - Graphics stack and platform expansion

### Added

- glTF loading, Meshopt integration, mesh/glTF parsers, and an image parser stack.
- Vulkan rendering, scene graph, sky/SDF shaders, and split Vulkan/GUI renderer paths.
- Terminal renderer integrated into `std.os.ui`; Win32 window backend added.
- IO and networking modules; JACK audio backend.
- Public fonts, dictionaries, website assets, and renderer shaders.
- Maintained sample programs, REPL import scenarios, and an updated learning guide.

### Changed

- Platform APIs moved into `std.os`; window backends moved into `std.os.ui.window`.
- Legacy native window backend path removed in favor of the new backend split.
- Runtime, UI, and diagnostic regression fixtures reorganized alongside the
 code they cover.
- Cache management, bigint support, and shader generation improved for
 graphics workloads.

### Fixed

- Asset path drift and shader-generation regressions during scene coverage expansion.
- Runtime fixture mismatches introduced while moving platform code into `std.os`.

## [0.2] - 2026-03-09 - Compiler, runtime, and stdlib foundation

### Added

- Parser, lowering pipeline, AST node definitions, and visitor/function lowering.
- Semantic analysis, diagnostics, and statement/call/FFI lowering.
- JIT lowering state, module/JIT integration, and native value-runtime bridge.
- Interactive reader, REPL completion, and build/web launcher with LSP commands.
- Enums, packed layouts, `sizeof`, pointer dereference, `try`, reflection
 operators, effects, and the `#main { ... }` entry guard.
- Core text, numeric, and cache modules; core IO and string helpers.
- Early UI facade, Vulkan renderer core, and native window/input backends.
- Network/audio backends; block cipher, factorization, RSA lattice, ECC/DLP,
 hash/PRNG, and public-key crypto helpers.
- Specification manuals, release notes, and initial benchmark/regression baselines.

### Changed

- Standard library moved to `lib/`, reducing prelude coupling.
- Parser, Vulkan renderer core, and UI renderer split into focused modules.
- Std module layout reorganized; numeric modules moved into `std.math`.
- Python build/bundle tooling replaced with native tools.

### Fixed

- First-pass parser, runtime primitive, module-loading, and diagnostic issues
 found by the initial test suite.
- Standard-library import coupling and module-path drift.

## [0.1] - 2025-12-24 - Prototype bootstrap

### Added

- Launcher skeleton, build script, and CMake scaffold (`make`,
 `CMakeLists.txt`, `src/cmd/ny/main.c`).
- Runtime placeholders and smoke fixtures for a first compilable, testable tree.