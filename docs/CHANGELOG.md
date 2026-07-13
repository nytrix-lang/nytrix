# Changelog

Nytrix uses dated milestones. Use `ny --version` for snapshots.

## [0.8.0] - 2026-07-13 — Cross-platform hot reload, proof types, renderer parity + polish

### Added

- The internal C frontend now accepts forward-declared aggregate tags before
  their definitions and preserves unnamed zero-width bitfield padding in
  aggregate layouts, with forced-internal-frontend regressions.
- The test runner now has `--failures-only`, an integrated cross-platform
  failure replay filter that preserves test exit status while suppressing
  successful fixture noise. Suite and per-fixture timeout controls are also
  independent, so slower hosted platforms are not killed by a fixture limit.
- `ny-fmt --cloc` now reports tracked Git additions and deletions, including
  per-file change totals, rather than displaying an empty diff summary.
- The x86-64 object register allocator now materializes floating constants in
  their assigned XMM registers and preserves typed f32/f64 values across local
  loads and stores, fixing nondeterministic native ELF float results.
- `std.math.logic` now provides a compact self-hosted proposition API with
  evaluation, simplification, bounded decisions, and counterexamples.
  `std.math.logic.prolog` adds bounded Prolog-style facts, rules, unification,
  occurs checks, recursive backtracking, and projected query answers entirely
  in Nytrix code.
- Added the first explicit proof witness constructor:
  `prove(condition[, message]) -> proof`. Only compile-time true obligations
  construct a witness; false and dynamic obligations are compiler errors, and
  non-proof values cannot satisfy proof parameters. The specification now
  states the current non-dependent boundary instead of implying a hidden
  theorem kernel.
- Native-only execution now treats compilation and execution as separate
  operations: `--native-only -o app` produces the requested executable without
  running it, ordinary file execution and `-c` both run the temporary native
  executable, and `--native-precompile` remains NYIR-only. Runtime cache format
  v7 also invalidates legacy sanitizer-contaminated objects before native links.
- Stdlib bitcode caches now reject user script entry points and use a new cache
  format version. This prevents a cache populated after mixed stdlib/user
  codegen from replaying an earlier program for unrelated sources; the complete
  uncached 928-test suite validates the corrected boundary.
- `--emit-bc` is now backend-sensitive: LLVM emits LLVM IR bitcode, while a
  native backend emits Nytrix-owned `NYIR` binary bytecode. The native artifact
  is reloadable through `--nyir-run-bin`; `--native-precompile` is its explicit
  LLVM-free alias.
- Stdlib source sweeps now validate through optimized IR instead of MCJIT
  materialization. This removes a measured 18-22 GiB single-process peak and
  converts the prior JIT crash cluster into bounded compile checks. Automatic
  test concurrency reserves 6 GiB per worker and is capped at eight.
- Sanitizer execution now reliably creates temporary AOT binaries when no
  output path is supplied. Sanitized runtime objects bypass the ordinary cache,
  preventing ASan/UBSan object reuse; successful runs remove their temporary
  executable. LLVM-unsupported generic UBSan instrumentation is no longer
  falsely requested, while the C runtime remains UBSan-compiled and linked.

- Added `--native-only` on x86-64: supported programs branch after parsing
  directly through NYIR, the native object writer, runtime link, and execution
  without constructing an LLVM module or MCJIT engine. Unsupported NYIR shapes
  fail explicitly. A function/call/`print(int)` probe measured 77.0-85.1ms
  total versus 132.6-142.8ms through MCJIT across three repeated runs.
- Windows safe-run AOT spawning now assigns suspended children to Job Objects
  before execution, with CPU, memory, active-process, wall, output, and
  kill-on-close containment. Unsupported open-file and in-process JIT callback
  limits are diagnosed instead of silently claimed.
- Native target/ABI selection and capability registration moved out of the
  lowering monolith into a dedicated ownership module, with ELF32/ELF64,
  COFF64, Mach-O, and all-architecture metadata regression coverage.
- Proof certificates now have a compact checker, persistent versioned index,
  bounded congruence/arithmetic/linear/finite/induction solvers, and explicit
  resource budgets across proposition, Prolog, and rewrite engines.
- Module-defined reasoning commands work through the syntax registry,
  comptime, generated modules, metadata, and ordinary LSP-visible exports.
  Generic law certificates operate on existing domain values and retain
  counterexamples without duplicating math models.
- Native archives rebuild atomically from dash-named sources; Apple-arm64
  comptime evaluation now runs through MCJIT's managed execution API so LLVM
  finalizes all generated callees before invocation; `addr_of` and `borrow`
  accept dereferenced pointer lvalues in native lowering, and
  `NYIR_ADDR_SYMBOL` extends address lowering to non-local (global/extern)
  symbols: the x86-64 backend emits `leaq sym(%rip), reg` with a PC32
  relocation, the ELF64 object writer encodes `R_X86_64_PC32` for data
  addresses (distinct from `R_X86_64_PLT32` for calls), and the in-memory JIT
  patches data addresses directly without a call trampoline.
- The internal C importer now loads external scalar globals with their raw C
  ABI representation, resolves ABI-width `size_t`/`ssize_t`/`ptrdiff_t`/
  `intptr_t`/`uintptr_t` spellings, uses target-aware layouts, and covers
  local-header nested layouts, pointer callbacks, variadics, and libc calls
  in one combined regression alongside the isolated probes.
- Supported x86-64 native-only runs now encode, relocate, W^X-finalize, and
  execute directly from memory. Runtime calls use local trampolines, removing
  temporary objects, runtime recompilation, external linking, and process spawn
  from the one-shot fast lane (39.1ms mean over ten warm runs for `print(42)`).
- `--native-only` interactive REPL sessions use the same LLVM-free image path;
  accumulated function and typed-binding source remains available across
  evaluations while unsupported forms fail explicitly.
- The ELF32 return harness no longer copies one byte past its instruction
  literal; RelWithDebInfo builds now cover that warning-clean boundary.
- Added bounded self-hosted proposition, rewriting, certificate, and Prolog
  modules, plus compile-time-only `proof` witnesses through `prove(...)`.
- Added opt-in `--safe-run` CPU, memory, process, file, wall-time, and output
  containment with platform-specific supervision and diagnostics.
- Full cross-platform real file watchers enabling fast language-level hot reloading via dynamically linked libraries (`.so` / `.dylib` / `.dll`):
  - Linux: full inotify support with event masks (`IN_*`), `watch_init`/`watch_add`/`watch_rm`, `watch_read_events`, and `watch_has_change`.
  - macOS: kqueue + `EVFILT_VNODE` (NOTE_WRITE, NOTE_DELETE, NOTE_RENAME, NOTE_ATTRIB, etc.) via new runtime primitives.
  - Windows: `FindFirstChangeNotification` / `FindNextChangeNotification` with `FILE_NOTIFY_CHANGE_*` filters.
- New first-class module `std.os.fs.watch` with clean portable API: `create(path)`, `close(handle)`, `poll(handle)`, `has_event(handle)`, `wait_any(handle)`, plus `WATCH_*` constants. Makes implementing hot-reloading of native modules trivial from .ny code.
- Extended `std.os.fs` with cross-platform watch facade + platform-specific low-level helpers.
- New runtime intrinsics for efficient watching: `__kqueue`, `__kevent`, `__watch_open_vnode`, `__win32_find_first_change` / `__win32_find_next_change` / `__win32_find_close_change`.
- CLI `--hot-reload` (`--hot`, `-H`), `--watch`, and `--watch-poll` now use real kernel event mechanisms on Linux/macOS/Windows (with mtime fallback), including proper `select`/`kevent`/`WaitForSingleObject` waiting.

### Changed / Performance & Optimization
- Native NYIR now coalesces copy/local chains, allocates scalar registers, and
  selects immediate operands; native-only execution and NYIR bytecode avoid
  LLVM for supported programs.
- Native lowering, targets, tiers, reports, NYIR passes, object packaging,
  result oracles, and compile-time proof analysis now have separate ownership
  modules instead of growing the former monoliths.
- File watching and hot reload use OS-native event notification (inotify, kqueue, Win32 directory change APIs) with blocking waits (`select`, `kevent`, `WaitForSingleObject`) instead of busy mtime polling. This reduces CPU usage when idle and improves change detection latency.
- The `--hot` / `--watch` loop performs edit-save-recompile-rerun with lower overhead.
- Support for watching combined with dlopen/dlsym of compiled dynamic libraries enables faster iteration without requiring full restarts in user code (provides foundation for reloadable modules).
- The watcher code is specialized per platform in the compiler and standard library.

### Fixed
- Multiplatform full-test from CI actions on linux/windows/macos.
- Apple-arm64 comptime MCJIT now uses LLVM's managed `LLVMRunFunction`
  invocation instead of manually calling a pointer returned before MCJIT's
  final execution pass. This lets MCJIT materialize and finalize indirect
  callees before control enters generated code.
- Trace and `--debug` compilation no longer render progress bars, and
  `--no-progress` now consistently overrides environment-driven progress on
  every platform. Failure replay also forces `--no-progress --color=never` so
  debugger output stays stable and readable.
- Failure replay preserves `.nshape` compiler flags, explicit native targets,
  and every `flags_matrix` row instead of debugging a different configuration.
  LLDB uses valid `frame variable -T -L` switches, allowing disassembly and
  memory-region diagnostics to continue.
- Windows JIT symbol resolution provides a portable variadic `snprintf`
  bridge and `optind` compatibility storage, local libc fixtures use
  target-aware `size_t`, and the direct internal-C variadic import regression
  now runs on Windows instead of being capability-skipped.
- `std.core.syntax.syntax` now uses one full `std.core.dict_mod` import, keeping
  `dict_write` and the other dictionary helpers unambiguous.
- Various platform-conditional and watcher handle lifetime issues during cross-platform implementation.
- Parser and dict construction robustness for the new watch handle types.
## [0.7] - 2026-06-30 — LLVM-Free Native Backend, C Interop & Polish

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
- Native/C frontend regression suites under `etc/tests/rt/native/` and `etc/tests/rt/c/`.
- `@backend(...)` and `backend_intrinsic(...)`, replacing backend-specific spellings.
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
- The public TODO list now contains only remaining hard roadmap items.
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
- Fuzz benchmark shapes (`etc/tests/fuzz/bench/*.nshape`) for call-heavy, matrix, string, and checksum workloads.
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
