# Changelog

Nytrix uses dated milestones. Use `ny --version` for snapshots.

## [0.9.0] - 2026-07-21 → 2026-07-23 — Language security, native tooling, and runtime reliability

### Added

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

- FFI headers are processed directly by the in-tree C frontend instead of being
  staged through the previous FFI pipeline.
- Typed `extern` declarations may expose the same native symbol through multiple
  valid Nytrix signatures without LLVM symbol collisions.
- macOS JIT library discovery now checks standard Homebrew locations.
- Windows native-only execution bypasses ELF-specific caching while retaining
  the in-memory JIT path.
- Panic traces now include the active source location.

### Fixed

- Restored the syscall filter and hardware `rdrand` paths by correcting invalid
  architecture and inline-assembly guards.
- Verified that managed collections do not hide allocations, Nytrix exposes no
  unrestricted `unsafe` escape hatch, and unsupported C constructs are rejected.
- Prevented `std.core.report` from leaking a `report` name into every
  `use std.core` consumer.
- Corrected C array-extent conversion, rejected negative extents, and fixed C11
  portability issues in the frontend.
- Fixed bigint heap corruption, integer-root convergence, and inconsistent
  native/GMP bigint allocation behavior.
- Corrected AArch64 inverted-condition encoding, unreachable predecessor
  handling, JIT exit-code propagation, and stencil cold/warm cache detection.
- Removed quadratic symbol lookup during large-ELF relocation analysis.

## [0.8.0] - 2026-07-13 — Native execution, proof tooling, and platform parity

### Added

- `ny --nyir-run-bin=PATH` now executes a validated, versioned NYIR artifact
  directly without reparsing the source program that originally produced it.
  `--nyir-dump-bin` now writes complete `rt_main` plus user-function bundles,
  so reusable artifacts preserve internal calls; compatible pre-v8 members are
  normalized before verification.
- LLVM-free execution now covers supported x86-64 and AArch64 programs from
  NYIR through internal object, linker, and W^X JIT paths. This includes local
  calls, relocations, runtime symbols, persistent REPL bindings, and explicit
  rejection of unsupported shapes.
- The AArch64 backend gained AAPCS64 scalar and floating-point calls, control
  flow, signed division/modulo, local pointer memory, internal ELF64 linking,
  and assembler-, compiler-, LLVM-, and linker-free QEMU runtime validation.
- Native ABI coverage now includes x86-64 System V aggregate classification,
  register/stack by-value arguments, two-eightbyte returns, hidden `sret`, and
  validated non-x86 call decoding. AArch64, ARM, and RISC-V also support proven
  local address/load/store shapes.
- JIT and AOT now share source-link discovery, multi-archive ELF merging,
  global/extern relocation, pointer lvalues, and target-aware scalar imports.
  Reloadable native NYIR artifacts are available through `--emit-bc`,
  `--native-precompile`, and `--nyir-run-bin` without changing LLVM bitcode
  behavior on LLVM backends.
- The Nytrix-owned C frontend now handles supported installed and compiler
  headers, macros, typedefs, layouts, callbacks, variadics, libc declarations,
  and external scalar globals. Floating/pointer callbacks and complex aggregate
  layouts gained native ABI coverage; project headers remain strictly checked.
- Unaliased C includes expose declarations directly, never through an implicit
  `c.*` namespace. Explicit aliases opt into namespacing, while existing Nytrix
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
- All 427 previously undocumented public stdlib functions now have source
  documentation; analysis reports no missing public API docs, and the
  471-module portal builds successfully.
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
  with stable `NYAUD1120` and `NYAUD1119` diagnostics and actionable fixes.
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

## [0.7.0] - 2026-06-30 — LLVM-free native backend and C interoperability

### Added

- NYIR: Nytrix-owned IR with verifier, optimizer, debug VM, binary format, and `--nyir-run`, `--nyir-dump-bin`, and `--nyir-run-bin`.
- Native emitters for x86-64 (default/primary), i386, ARM, AArch64, and RISC-V, with debug-scoped WASM, BPF, PowerPC, MIPS, and AVR support.
- In-process ELF64, ELF32, COFF, and Mach-O object writers with relocations and multi-function aggregation.
- Compiler-owned ELF64 and ELF32 link/run paths, avoiding LLVM, `cc`, and external linkers for supported native fixtures.
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
  - `*p` reads, writes, compound assignments, and local stack addresses
  - `addr_of(local)` through VM, assembly, ELF64, and ELF32 paths
- `--native-result-oracle` for VM/native result comparison.
- Internal C frontend under `src/code/c/`, replacing libclang for supported header imports:

  - Macros and conditionals
  - Typedefs, structs, unions, bitfields, and alignment attributes
  - `_Bool`, `_Complex`, unknown types, and recoverable declarations
  - `sizeof` and object-like integer define lowering
  - Scalar, typedef-struct pointer, function-pointer parameter, and simple aggregate-return imports
  - Public aggregate layout API exposing size, alignment, and function-pointer counts
- Increased C parser capacities and tolerant recovery for complex or unsupported declarations.
- X86/i386 NYIR assembly coverage for cdecl call3/call7/call9, logical operations, ternaries, match cases, loops, break, and ranges.
- Native/C frontend regression suites under `etc/tests/native/` and `etc/tests/interop/c/`.
- `@backend(...)` and `intrinsic(...)` target-selection forms.
- Cleaner NYIR assembly headers and comments.
- Compact 1:1 TLDR documentation across README, start, performance, syntax, and CHANGELOG pages.

### Changed

- Native compilation and the internal C frontend now run before LLVM/libclang fallback.
- x86-64 is the default native target.
- LLVM and libclang remain legacy fallbacks for unsupported cases.
- All build, cache, resource, and `NYTRIX_ROOT` paths are strictly relative.
- Default optimization level is now `0`; optimization must be explicitly enabled for performance builds.
- NYIR lowering now covers logical operators, ternaries, loops, break/continue, recursion, and match arms.
- Optimizer passes refresh metadata and compact SSA values after every pass.
- Constant and range propagation now covers arithmetic, bitwise operations, and comparisons.
- `packed, aligned(N)` follows GCC's order-independent “aligned wins” semantics.
- Native tests are organized by kind under directories such as `nyir/`, `diff/`, `oracle/`, and `elf64/`.
- Module declarations support compact auto-export forms:

  - `module foo`
  - `module foo(internal)`
- FFI include examples no longer require redundant `as ""`.
- Render/UI resources are deprecated-free and relative-path safe.
- Compilation hot paths use preallocation and hashing to avoid repeated reallocations.
- Codegen performs smarter lowering and emits cleaner optimized output.

### Fixed

- NYIR verification and loading now reject malformed effect masks, duplicate labels, invalid arity, and invalid metadata before consumers process them.
- Binary NYIR format v4 supports wider call operands while preserving v1-v3 loading compatibility.
- VM profile counters aggregate correctly across nested calls.
- Native x86-64 functions save incoming argument registers into locals before executing lowered bodies.
- x86-64 ELF emission now correctly:

  - Spills SysV register and stack-passed arguments
  - Handles multiple stack-passed i64/f64 arguments with alignment padding
  - Stores f64 returns from `xmm0`
  - Emits the supported f32 arithmetic, conversion, and call slice
  - Uses raw returns for externally linked object checks
  - Preserves comparison flags through `setcc`
- i386 and ARM signed division/modulo lower to native instructions instead of being rejected.
- i386 ELF32 now supports cdecl calls, x87 f32/f64 operations, `R_386_PC32` relocations, pointer helpers, dereferences, locals, branches, and loops.
- 64-bit shifts from 0 through 63 now verify and evaluate correctly.
- Native object output no longer collides with runtime `rt_main`.
- Prefix `*p` now:

  - Parses as `NY_E_DEREF`
  - Type-checks as the pointed-to type
  - Lowers reads to `NYIR_LOAD_I64`
  - Supports `*p = value` and `*p += value`
- `addr_of(local)` now lowers to `NYIR_ADDR_LOCAL`, executes in the debug VM, and emits frame-relative `lea` on x86-64/i386.
- Address-taken local facts are invalidated after raw pointer writes so later local reads observe mutations.
- Internal C aggregate imports decline unsupported nested or by-value layouts without poisoning fallback.
- Strict no-libclang aggregate-return import is covered through `load_layout`.
- C frontend rejects non-positive array extents and diagnoses unsupported field shapes instead of silently dropping them.
- Unsupported C declarations produce recoverable diagnostics instead of hard aborts.
- Parser diagnostics no longer suggest C-style `for (;;)` and instead point to Nytrix iterator syntax.

## [0.6] - 2026-06-30 — Fuzzing, crypto/math expansion, renderer polish

### Added

- Benchmark shapes (`etc/tests/bench/*.nshape`) for call-heavy, matrix, string, and checksum workloads.
- Published fuzzer and tooling for local benchmarking and error-shape discovery.
- Radix helpers, stream/block ciphers, public-key helpers, lattice/factorization modules.

### Changed

- SVG/UI rendering: 4x4 supersampling, stroke linecap/linejoin, gradient/`<use>` support, terminal 256-color output.
- `--borrow-check` decoupled from `--ownership-strict`; Z3 enabled by default; proven-nonzero `f64` division checks elided.
- glTF hot paths moved from `src/rt/gltf.c` into Ny code.
- CMake dependency probing hardened for LLVM, libclang, Z3, Windows UCRT/MSYS2.

### Fixed

- Canvas UTF-8 buffer type mismatches and terminal renderer edge cases.
- Lowercase type-first local binding parsing.
- Semicolon comment ambiguity in parser diagnostics.
- Windows build integration, joystick axis handling, SDK/toolchain probing.
- zlib decompression capacity handling.

## [0.5] - 2026-06-05 — Editor/viewer framework

### Added

- Editor and engine viewer (`std.os.ui.render.viewer`): asset browser, hierarchy, inspector, gizmos, transform tools, runtime bootstrap.
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

## [0.4] - 2026-05-30 — Ownership, typed pipeline, CLI unification

### Added

- Cross-platform windowing/input: Win32, Cocoa, X11, Wayland, Vulkan.
- Typed compiler pipeline: Hindley-Milner inference, lambda/nested-collection inference, monomorphic specialization.
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

## [0.3] - 2026-04-13 — Graphics stack and platform expansion

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
- Runtime, UI, and diagnostic regression fixtures reorganized alongside the code they cover.
- Cache management, bigint support, and shader generation improved for graphics workloads.

### Fixed

- Asset path drift and shader-generation regressions during scene coverage expansion.
- Runtime fixture mismatches introduced while moving platform code into `std.os`.

## [0.2] - 2026-03-09 — Compiler, runtime, and stdlib foundation

### Added

- Parser, lowering pipeline, AST node definitions, and visitor/function lowering.
- Semantic analysis, diagnostics, and statement/call/FFI lowering.
- JIT lowering state, module/JIT integration, and native value-runtime bridge.
- Interactive reader, REPL completion, and build/web launcher with LSP commands.
- Enums, packed layouts, `sizeof`, pointer dereference, `try`, reflection operators, effects, `#main { ... }` entry guard.
- Core text, numeric, and cache modules; core IO and string helpers.
- Early UI facade, Vulkan renderer core, and native window/input backends.
- Network/audio backends; block cipher, factorization, RSA lattice, ECC/DLP, hash/PRNG, and public-key crypto helpers.
- Specification manuals, release notes, and initial benchmark/regression baselines.

### Changed

- Standard library moved to `lib/`, reducing prelude coupling.
- Parser, Vulkan renderer core, and UI renderer split into focused modules.
- Std module layout reorganized; numeric modules moved into `std.math`.
- Python build/bundle tooling replaced with native tools.

### Fixed

- First-pass parser, runtime primitive, module-loading, and diagnostic issues found by the initial test suite.
- Standard-library import coupling and module-path drift.

## [0.1] - 2025-12-24 — Prototype bootstrap

### Added

- Launcher skeleton, build script, and CMake scaffold (`make`, `CMakeLists.txt`, `src/cmd/ny/main.c`).
- Runtime placeholders and smoke fixtures for a first compilable, testable tree.
