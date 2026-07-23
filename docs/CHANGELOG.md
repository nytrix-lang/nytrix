# Changelog

Nytrix uses dated milestones. Use `ny --version` for snapshots.

## [0.9.0] - 2026-07-23 — Language security, in-tree C frontend, and clean build

Deep analysis of the C frontend, FFI layer, and language security, informed by
the Fil-C/zig discussion (codeberg.org/ziglang/zig/issues/36237). The goal is
for Nytrix programs, including C dependencies, to remain memory safe without
an unrestricted escape hatch and with acceptable overhead.

### Added

- In-tree Nytrix C frontend (`src/code/c/`) replaces libclang for FFI header
  parsing: arrays, unions, packed structs, bitfields, `__int128`, `_Complex`,
  `long double`, function pointers, typedefs, string/object-like macros, and
  variadics. Validated by `etc/tests/native/c/internal-lowering-abi.nshape`
  (L-1…L-12 ABI lowering probes).
- Parse-time hardening for untrusted C headers: a monotonic-clock deadline,
  recursion-depth limit (`NY_C_MAX_RECURSION_DEPTH`), and bounded tables defeat
  pathological/expansion bombs; `ny_parse_cleanup` frees parser intern buffers.
- Syscall denylist in `rt_syscall` (linux/x86_64): `mprotect`, `clone`, `fork`,
  and `vfork` return `-EPERM`, so Nytrix programs and FFI C code reaching
  `__syscall` cannot spawn processes or remap executable memory. Regression:
  `etc/tests/runtime/execution/syscall-filter.ny`.
- `#assert(cond[, msg])` comptime assertion and an `assert(cond[, msg])`
  lowering that embeds the condition source text in the panic message with a
  `file:line:col` location.
- `std.core.report` diagnostic-formatting module (error/warning/note/location
  with source-line underline), imported explicitly like `std.core.term`.
- `--zero-init` / `--no-zero-init` flag (default: zero) controls whether
  `rt_malloc` zero-fills managed heap allocations. Env var `NYTRIX_ZERO_INIT`
  also accepted. `rt_malloc_uninit` remains uninitialized. S-7 from security
  roadmap.
- Panic messages now report `at file:line:col` from the active trace.

### Changed

- `./make` and `./make ny` recompile the C binaries and std bundle
  incrementally on change; documented the runtime amalgamation model
  (`src/rt/init.c` includes `os.c`, `core.c`, …) in AGENTS.md.
- The FFI layer no longer stages includes into `codegen_ffi_t`; headers are
  processed directly through the in-tree frontend under a bounded deadline.

### Fixed

- **Security: S-3/S-5/S-6 verified.** No hidden allocations (S-3) enforced by
  reference-typed dicts and value-typed lists; no unsafe escape hatch (S-5)
  confirmed — no `unsafe` keyword exists; language subset enforcement (S-6)
  confirmed — C frontend rejects unsupported constructs with clear diagnostics.
  Security roadmap updated with S-7 implementation and S-1/S-2/S-8 research.
- **Security: syscall denylist was dead code.** The inline-syscall path in
  `src/rt/os.c` (denylist) and the hardware `rdrand` path in `src/rt/math.c`
  were guarded by `rt_x86_64__` / `rt_asm__` / `rt_volatile__` — three macros
  never defined anywhere. Every build silently `#if`'d the block out and fell
  through to an unfiltered libc `syscall()`, so `syscall(56)` actually cloned.
  Switched the guards to the standard `__x86_64__` / `__asm__` / `__volatile__`
  the rest of `src/rt/` already uses.
- **Name collision: `std.core` leaked a `report` identifier.** `lib/core/mod.ny`
  re-exported the new `std.core.report` submodule, so every `use std.core`
  consumer saw `report` as a module name and shadowed ubiquitous local
  `def report` variables (broke `shapes/probes/sys/gltf-index-modes.nshape`).
  Removed the re-export; the diagnostic module is now imported explicitly.
- C frontend: `parse_integer_size`/define lookup bridged `int64_t` parser
  output into `size_t` extents instead of passing mismatched pointer types
  (`-Wpointer-sign`); negative extents are now rejected.
- `fficlang.c` label-then-declaration made C11-portable; `c/parse.c` integer
  slicing no longer passes `size_t*` as `int64_t*`.

### Build hygiene

- Eliminated all 464 C compiler warnings (411 runtime `-Wmissing-prototypes`
  via a scoped pragma documenting the RT_DEF dispatch contract; 53 compiler
  warnings fixed at the owning layer: missing `static`, missing prototypes in
  `priv.h`/`llvm.h`/`internal.h`, two dead web helpers removed, one reserved
  parameter marked). The runtime and compiler now build clean under
  `-Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes`.

## [0.9.0] - 2026-07-21 → 2026-07-22 — Native pipeline polish, runtime boundaries, and tooling

### Added

- Optional GMP backend behind `NYTRIX_USE_GMP` CMake flag (default ON); builds
  cleanly without GMP using native bigint fallbacks for all operations.
- Full native bigint fallback: divmod, pow, modinv, iroot, clz/ctz, xor,
  to_str, to_bytes, from_bytes, powmod, sqrt_mod, quadratic_roots_mod.
- `-Oz` size-optimization profile alongside existing `-O0`/`-O1`/`-O2`/`-O3`.
- Public `std.os.rev` reverse-engineering stack, with decompilation, symbolic
  execution, constraint solving, and string-analysis modules plus executable
  facade self-tests.
- Reusable all-black Nytrix startup ident and deterministic project fixture
  capture.
- `ci skip` nshape metadata for probes that require optional desktop services
  or host-installed libraries; such probes remain part of local coverage.
- Optional development glTF asset discovery after the existing build-cache
  root; configured environment roots retain resolution priority.

### Changed

- Typed `extern` declarations now retain a distinct LLVM symbol identity from
  their native linker symbol, allowing one C ABI symbol to be exposed through
  multiple valid typed declarations.
- macOS JIT library loading searches standard Homebrew package locations for
  ordinary `#link` libraries without adding library-specific runtime paths.
- Windows native-only execution bypasses the ELF-only stencil-cache path while
  retaining the in-memory native JIT path.
- `-DNDEBUG` on runtime archive for release builds, eliminating debug-only
  destructors and their libc pulls.
- `RT_PRINT_BUF_SIZE` reduced from 64 KB to 8 KB with auto-flush, cutting
  per-process BSS by 57 KB.
- Added `rt_print_cstr` for pure-native C string output.
- Unified native IR naming to `NYIR_` prefix throughout codebase.
- Per-unit runtime archive compilation with per-function sections and
  `gc-sections` for aggressive dead code elimination at link time.
- Extended linker flags: `gc-sections`, `strip-all`, `dead_strip`, `OPT:REF`,
  `OPT:ICF` for platform-specific dead code elimination.
- Removed `rt_dict_write_fast` / `__dict_write_fast` in favor of `dict_set`.
- Removed unconditional `-lgmp` from linker flags.
- CMake GMP `find_library` now searches `/usr/lib` and `/usr/lib64`.
- Moved printable-string scanning out of the decompiler core into
  `std.os.rev.strings`, keeping binary analysis focused on ELF and lifting
  while making string extraction independently testable.

### Fixed

- Heap corruption from uninitialized `_bn_t` locals passed to `realloc` in
  bigint arithmetic, iroot, and `rt_long` list path.
- `_bn_iroot` Newton convergence: replaced broken inline long division with
  proven `_bn_divmod_unsigned`.
- `rt_bigint_from_int` now always allocates heap bigint for consistency with
  GMP path semantics.
- AArch64 `cset` encoding for inverted condition codes in mach IR path.
- Dominant-tree iteration to skip unreachable predecessors.
- JIT exit-code propagation from entry function.
- Stencil early-cache check for cold/warm path distinction.
- Large-ELF relocation analysis now indexes symbols once per scan instead of
  repeatedly searching the full symbol table, removing quadratic load-time
  behavior in `std.os.rev.decomp`.
- Removed an orphaned bigint source fragment that broke clean Linux and macOS
  runtime builds.
- Restored portable clean-build behavior for the native pipeline on Windows.
- Cross-platform test selection now uses explicit fixture metadata rather than
  implicit path exceptions for optional system probes.

### Added

- Reusable all-black Nytrix startup ident and deterministic project fixture
  capture.
- Public reverse-engineering and symbolic-analysis modules with executable
  decompiler fixtures.
- `ci skip` nshape metadata for probes that require optional desktop services
  or host-installed libraries; such probes remain part of local coverage.

### Changed

- Typed `extern` declarations now retain a distinct LLVM symbol identity from
  their native linker symbol, allowing one C ABI symbol to be exposed through
  multiple valid typed declarations.
- macOS JIT library loading searches standard Homebrew package locations for
  ordinary `#link` libraries without adding library-specific runtime paths.
- Windows native-only execution bypasses the ELF-only stencil-cache path while
  retaining the in-memory native JIT path.

### Fixed

- Removed an orphaned bigint source fragment that broke clean Linux and macOS
  runtime builds.
- Restored portable clean-build behavior for the native pipeline on Windows.
- Cross-platform test selection now uses explicit fixture metadata rather than
  implicit path exceptions for optional system probes.

## [0.9.0] - 2026-07-21 — Native pipeline polish, optional GMP, and binary compaction

### Added

- Optional GMP backend behind `NYTRIX_USE_GMP` CMake flag (default ON); builds
  cleanly without GMP using native bigint fallbacks for all operations.
- Full native bigint fallback: divmod, pow, modinv, iroot, clz/ctz, xor,
  to_str, to_bytes, from_bytes, powmod, sqrt_mod, quadratic_roots_mod.
- `-Oz` size-optimization profile alongside existing `-O0`/`-O1`/`-O2`/`-O3`.
- Public `std.os.rev` reverse-engineering stack, with decompilation, symbolic
  execution, constraint solving, and string-analysis modules plus executable
  facade self-tests.
- Optional development glTF asset discovery after the existing build-cache
  root; configured environment roots retain resolution priority.

### Fixed

- Heap corruption from uninitialized `_bn_t` locals passed to `realloc` in
  bigint arithmetic, iroot, and `rt_long` list path.
- `_bn_iroot` Newton convergence: replaced broken inline long division with
  proven `_bn_divmod_unsigned`.
- `rt_bigint_from_int` now always allocates heap bigint for consistency with
  GMP path semantics.
- AArch64 `cset` encoding for inverted condition codes in mach IR path.
- Dominant-tree iteration to skip unreachable predecessors.
- JIT exit-code propagation from entry function.
- Stencil early-cache check for cold/warm path distinction.
- Large-ELF relocation analysis now indexes symbols once per scan instead of
  repeatedly searching the full symbol table, removing quadratic load-time
  behavior in `std.os.rev.decomp`.

### Changed

- `-DNDEBUG` on runtime archive for release builds, eliminating debug-only
  destructors and their libc pulls.
- `RT_PRINT_BUF_SIZE` reduced from 64 KB to 8 KB with auto-flush, cutting
  per-process BSS by 57 KB.
- Added `rt_print_cstr` for pure-native C string output.
- Unified native IR naming to `NYIR_` prefix throughout codebase.
- Per-unit runtime archive compilation with per-function sections and
  `gc-sections` for aggressive dead code elimination at link time.
- Extended linker flags: `gc-sections`, `strip-all`, `dead_strip`, `OPT:REF`,
  `OPT:ICF` for platform-specific dead code elimination.
- Removed `rt_dict_write_fast` / `__dict_write_fast` in favor of `dict_set`.
- Removed unconditional `-lgmp` from linker flags.
- CMake GMP `find_library` now searches `/usr/lib` and `/usr/lib64`.
- Cleaned up Python benchmark dependencies; converted to `.nshape` format.
- Test suite at 637 passing (342 native, 73 runtime, 90 error, 5 interop,
  2 pre-existing system skips).
- Moved printable-string scanning out of the decompiler core into
  `std.os.rev.strings`, keeping binary analysis focused on ELF and lifting
  while making string extraction independently testable.

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
