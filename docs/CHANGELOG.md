# Changelog

Nytrix uses dated milestones. `ny --version` for snapshots.

## Unreleased

### Changed
- Full cross-platform real file watchers for language-level hot reloading via `.so`/`.dylib`/`.dll` and file watchers (Linux inotify, macOS kqueue EVFILT_VNODE, Windows FindFirstChangeNotification). New `std.os.fs.watch` module with `create`/`close`/`poll`/`has_event`/`wait_any`. Extended `std.os.fs`. New runtime `__kqueue` etc. CLI `--hot-reload`/`--watch` uses real kernel events (`select`/`kevent`/`WaitForSingleObject`) + mtime fallback for lower idle CPU and faster change detection vs polling. Specialized per-platform in compiler and stdlib. Foundation for reloadable native modules via watch + dlopen. All self-test prints start with ✓ .

## [0.8.0] — LLVM-free Native + C Interop + Polish

### Added
- Internal C frontend tolerant support for more complex headers; `_Complex`, `_Bool`, unknown types, and recoverable declarations continue on errors.
- NYIR asm: polished headers/comments for clean readable output.
- Docs: compact 1:1 TLDRs, direct explanations in README/start/perf/syntax/CHANGELOG.

### Changed
- Strict relative paths only (build/cache, NYNTH_ROOT).
- FFI tests: etc/tests/rt/c (internal C frontend).
- Public TODO list trimmed to remaining hard roadmap items.
- Module decls: short `module foo` / `module foo(internal)` auto-export (no bloat lists).
- Render/UI: removed deprecated, relative-safe resources.
- Native default x86_64, internal C for headers.

### Fixed
- C frontend: bumped parser capacities, fewer hard aborts, tolerant on unsupported declarations.
- No -O default (opt_level=0); explicit for perf.
- Compile time: prealloc, hash, no realloc churn in hot paths.
- Codegen: smarter lowering, clean output when -O.

## [0.7.0] - 2026-06-30 — Native backend & C frontend foundation

### Added
- NYIR: Nytrix-owned IR with verifier, optimizer, and debug VM (`--nyir-run`, `--nyir-dump-bin`, `--nyir-run-bin`).
- Native emitters for x86-64 (primary), i386, ARM, AArch64, RISC-V, plus debug-scoped WASM/BPF/PowerPC/MIPS/AVR.
- In-process ELF64/COFF/Mach-O object writers with relocations and multi-function aggregation.
- Direct ELF object link/run regression gates for raw `rt_main` i64 and f64 results, including proven single and multiple stack-passed i64, f64, and mixed i64/f64 call arguments, plus f32 arithmetic and f32 register/stack call results observed through an f64 return.
- Narrow internal ELF64 executable linker for object link/run fixtures, avoiding `cc` for all current i64/f64/f32/pointer/deref/memory-stub gates; pointer/string memory tests use tiny Linux `malloc`/`free`/`memset`/`memcpy`/`memmove`/`memcmp`/`memchr`/`strlen`/`strcmp`/`strchr` stubs plus `realloc(NULL, n)` and checked-product `calloc(count, size)` allocation semantics, not full libc linking.
- Compiler-owned ELF link/run gates treat Nytrix's in-process ELF64/ELF32 linkers as the native path; external linkers such as `mold` or platform `cc`/linker flows remain fallback/integration paths for unsupported external symbols and general host linking.
- Direct ELF object link/run regression gates for pointer memory helpers, source-level `*p` pointer deref reads/writes, bare deref compound assignment, and local `&local` stack address materialization.
- Direct ELF object link/run regression gates for ABI-visible narrow integer returns (`bool/u8/i16/u32`) on ELF64 and the supported ELF32 slice.
- Direct ELF object link/run and VM/native oracle regression gates for narrow native `addr_of(local)` stack-local address materialization.
- Internal C frontend (`src/code/c/`) for header import without libclang: macros, conditionals, typedefs, structs/unions, bitfields, alignment attributes, `sizeof`, object-like integer define lowering, and strict scalar, typedef-struct pointer, function-pointer-parameter, plus simple by-value aggregate-return import lowering for local C headers.
- Raw-int and f64 SysV call ABI coverage for register arguments plus focused stack-passed argument cases.
- X86/i386 NYIR assembly coverage for cdecl call3/call7/call9 plus logical, ternary, case, loop-break, and for-range lowering.
- Narrow compiler-owned i386 ELF32 relocatable writer and internal link/run gate for raw-int `rt_main`, local cdecl calls, x87 f64/f32 arithmetic/return/params/comparisons, high-bit u32 immediates, `R_386_PC32` relocations, pointer-memory helpers, tiny `malloc`/`free`/`memset`/`memcpy`/`memmove`/`memcmp`/`memchr`/`strlen`/`strcmp`/`strchr` stubs plus `realloc(NULL, n)` and checked-product `calloc(count, size)`, source-level deref reads/writes, arithmetic, div/mod, locals, branches, and loops.
- F32 NYIR/VM/x86-64 assembly and direct ELF object coverage for annotated f32 constants, arithmetic, comparison, direct f32 returns, register/stack calls, and f32-to-f64 observation.
- Signed i64 native-result oracle coverage for negative div/mod, signed comparisons, and arithmetic right shift.
- `--native-result-oracle` gate comparing VM and native execution results.
- Native/C-frontend regression suite under `etc/tests/rt/native/`.
- `@backend(...)` attribute and `backend_intrinsic(...)` builtin, replacing backend-specific spellings.

### Changed
- Native paths run before falling back to LLVM/libclang, which remain available as legacy fallback.
- NYIR lowering covers logical ops, ternaries, loops, break/continue, recursion, and match arms.
- Optimizer refreshes metadata and compacts SSA values after each pass.
- Constant/range fact propagation extended to arithmetic, bitwise ops, and comparisons.
- C frontend reports aggregate size/alignment and function-pointer counts through a public layout API.
- `packed, aligned(N)` now follows GCC's "aligned wins" semantics, order-independent.
- Native test files reorganized into subdirectories by kind (`nyir/`, `diff/`, `oracle/`, `elf64/`, etc.).
- FFI include examples drop redundant `as ""`.

### Fixed
- NYIR verifier/loader reject malformed metadata (bad effect masks, duplicate labels, invalid arity) before consumers see it.
- VM profile counters now aggregate correctly across nested calls.
- Native x86-64 calls save argument registers to locals before executing lowered bodies.
- i386/ARM signed division and modulo lower to real instructions instead of rejecting.
- 64-bit shifts up to 63 verify and evaluate correctly.
- Native object emission no longer collides with runtime `rt_main`.
- x86-64 ELF object emission now spills SysV incoming register and proven stack-passed args to NYIR locals, emits single and multiple stack-passed i64/f64 object-call args with alignment padding, stores f64 call returns from `xmm0`, emits the proven f32 arithmetic/conversion/call slice, and uses raw returns for externally linked object checks.
- x86-64 ELF object integer comparisons preserve flags through `setcc` and are covered by direct link/run bool comparison regression.
- Prefix `*p` now parses as `NY_E_DEREF`, type-checks as the pointed-to type, lowers to native `NYIR_LOAD_I64` for pointer deref reads, and supports `*p = value` plus bare `*p += value` writes through the typed raw store path.
- Native `addr_of(local)` lowers to `NYIR_ADDR_LOCAL`, executes in the debug VM oracle, and emits frame-relative `lea` on x86-64/i386 assembly and ELF object paths; address-taken local facts are invalidated after raw pointer stores so ordinary local reads observe those writes. This does not yet provide general `&expr` syntax.
- Internal C aggregate import now declines unsupported nested/by-value layouts without poisoning fallback, and proves a strict no-libclang `div(int, int)` aggregate-return slice through `load_layout`.
- Parser no longer suggests C-style `for(;;)` headers; points to Nytrix iterator syntax instead.
- C frontend rejects non-positive array extents and reports unsupported field shapes as diagnostics instead of dropping them silently.
- Binary NYIR format extended through v4 for wider call operands, preserving v1–v3 load compatibility.

## [0.6.0] - 2026-06-30 — Fuzzing, crypto/math expansion, renderer polish

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

## [0.5.0] - 2026-06-05 — Editor/viewer framework

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

## [0.4.0] - 2026-05-30 — Ownership, typed pipeline, CLI unification

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

## [0.3.0] - 2026-04-13 — Graphics stack and platform expansion

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

## [0.2.0] - 2026-03-09 — Compiler, runtime, and stdlib foundation

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

## [0.1.0] - 2025-12-24 — Prototype bootstrap

### Added
- Launcher skeleton, build script, and CMake scaffold (`make`, `CMakeLists.txt`, `src/cmd/ny/main.c`).
- Runtime placeholders and smoke fixtures for a first compilable, testable tree.
