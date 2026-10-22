# Changelog

## [0.10] - 2026-10-13

Compiler, native platform, graphics, audio, and tooling improvements.

<details>
<summary>Added</summary>

- Expanded compile-time templates, quoted code, AST reflection, and code
  generation. Generated code retains source context and uses the normal
  compiler pipeline.
- Added reusable parser combinators and typed configuration support in the
  standard library.
- Added `str.word_frequency` for counting words without allocating an
  intermediate token list.
- Added public NYR APIs for linking objects and optimizing NYIR safely.
- Expanded C frontend support for scalar variables, pointers, arrays, calls,
  and loop control, with diagnostics for unsupported constructs.
- Expanded target support for aggregate and scalar ABIs, floating-point
  operations, memory copies, rotations, and stack arguments across Wasm, ARM,
  AVR, i386, AArch64, BPF, MIPS64, PowerPC64, and RISC-V64. Unsupported
  target cases retain explicit diagnostics.

</details>

<details>
<summary>Changed</summary>

- Reused native JIT symbol lookups across relocation patching and call-stub
  selection, reducing repeated scans when loading fresh or cached code.
- Native x86-64 bit-test selections now use their allocated result register
  directly, avoiding redundant copies through scratch registers.
- Native x86-64 integer additions use LEA when it removes a register copy.
- Removed redundant native jumps when a block can safely fall through to its
  sole successor.
- Strengthened proof and refinement checking for function contracts, loop
  ranges, recursive measures, mutable collections, and callback effects.
  Unsupported or uncertain cases continue to fail closed.
- Improved callback effect tracking across aliases, returned closures,
  containers, and control-flow joins.
- Improved compiler optimizations and translation validation while preserving
  conservative behavior around unknown effects, memory aliases, overflow,
  and unsupported control flow.
- Scheduled non-trapping constant integer divisions with independent machine
  work while preserving trapping-division order.
- Avoided speculative if-conversion when both integer-multiply arms would
  execute unnecessarily; short, cheaper diamonds remain eligible.
- Improved fixed-trip integer reductions by avoiding spill-heavy
  loop-carried vector accumulators while retaining vectorized memory loads.
- Improved specialization of short counted loops with nested control flow and
  ordered calls.
- Safely unrolled small counted loops whose dynamic division divisor is proven
  positive throughout the loop.
- Hoisted invariant integer division only when its explicit zero-divisor exit
  and signed-overflow behavior remain preserved.
- Corrected string-length handling at C-string boundaries so static LLVM
  literals are not mistaken for tagged runtime strings based on adjacent data.
- Made `??` distinguish nil from other falsy values and prevented top-level
  nil initializers from being tagged a second time.
- Corrected operator precedence, standard-library module names, source-tree
  layout, and fixture references in the contributor and language guides.
- Reduced repeated inliner work after a function has no remaining direct
  symbol calls; large UI module compilation profiling showed the inliner was a
  major compile-time hotspot.
- Fast-pathed machine-register segment lookup for values with one segment,
  reducing repeated binary searches during native code generation.
- Kept fixture frame admission and key polling out of line, reducing the
  callback-boundary reproducer from 415 to 66 NYIR instructions in its wrapper.
- Avoided cloning high-pressure control-flow helpers into repeated loops when
  their expanded body is likely to overwhelm native register allocation.
- Improved x86-64 code generation, register allocation, scalar and
  typed-buffer handling, native call conventions, C imports, pointer
  handling, and BigInt conversions.
- Avoided redundant parameter-home spills when native register allocation
  proves a safe direct argument setup.
- Reduced avoidable register spills by reusing a `select` arm's register when
  that arm has no later use.
- Used compact sign-extended immediates for negative 32-bit constants in
  x86-64 code generation.
- Corrected signed division by two for negative values held in native x86-64
  registers, preserving truncation toward zero in the optimized shift path.
- Improved x64 loop code generation with call-safe homes, direct PHI copies,
  vector-aware liveness, and register residency for high-use values without
  call or division barriers.
- Improved x86-64 floating-point loop allocation across scalar conversions,
  avoiding repeated hot-loop stack traffic.
- Avoided a temporary limb buffer when constructing exactly representable
  BigFloat values from unsigned integers.
- Reduced stack traffic in x86-64 vectorized loops by keeping safe address
  values in registers.
- Reduced redundant x86-64 address setup for indexed memory accesses by
  reusing registers already holding the base or index.
- Improved OpenGL primitives, colors, text, glyph fallback, and rendering
  after resize. UI examples handle input, layout, and window/terminal cleanup
  more consistently. The animation example caches its pixel-art rows and
  palette, avoiding repeated per-frame decoding. It defaults to OpenGL unless
  a renderer is explicitly selected.
- Refined the donut and Matrix rain terminal projects, and improved Conway's
  Life, Rule 110, Pong, and HTTP-server examples. Pong includes a predictive,
  speed-limited CPU opponent.
- Improved string building, collection operations, filesystem/process
  behavior, and runtime error reporting.
- Reduced overhead for common native integer-key dictionary insertions while
  preserving collision handling and resizing.
- Made test fixtures usable through both the Nytrix runner and CTest, with
  consistent tool discovery and subprocess output capture.
- Improved compiler and standard-library lookup, cache identity checks, and
  recovery from invalid cache entries.
- C frontend lexer: accept C23 digit separators (`'`) inside numeric literals
  (`1'000`, `0x1'0000`, `0b1'0101`) and reworked punctuator matching to a
  longest-match table (stb_c_lexer.h design reference; reuse rights verified:
  public domain/Unlicense or MIT). Integer parsing accepts the separators and
  standard `uUlLzZ` suffixes (`42UL`, `1'000ULL`).

</details>

<details>
<summary>Fixed</summary>

- Fixed native UI startup return typing where the startup trace logger's
  floating-point result was used as a side-effect-only function result. Expanded
  the bounded native relocation capacity for large UI bundles.
- Fixed a C frontend parser stack overflow caused by four `ny_parser_t`
  (~11.7 MB each) stack copies in `skip_unary_expr`, `skip_cast_expr_check`,
  aggregate lookahead, and `parse_array_extent_primary`. Copies are now
  heap-allocated, so `int x[1]`, `int arr[1000]`, and `int arr[1'000]` parse
  through `ny_parse_decl`.
- Preserved a reloadable x86-64 value home when an LEA index feeds a folded
  memory access, preventing native BCE benchmark misexecution.
- Corrected the SOR benchmark's alternating-buffer update so native and
  reference backends produce the same checksum.
- Reduced native compile time for large UI modules by precomputing inline
  callee facts and loop-depth hints instead of rescanning those structures at
  every call site.
- Kept cold multi-block callees out of nested-loop inline sites and skipped
  counted-loop unrolling when the loop body contains division or modulo until
  the optimizer can prove cloned divisors safe. This fixes nested-loop checksum
  drift and a guarded division verifier failure.
- Restored native runtime lookup for `core.set_mod.len` by keeping the sorted
  builtin alias table in lexical order.
- Lowered native `??` by evaluating the left operand once and reusing its value
  for the non-nil result, including optional-chain nil fallback. The focused
  fixtures cover a side-effecting left operand and typed false, zero, and
  empty-string behavior.
- Preserved `false` as a boolean when initializing a dynamic `any` value,
  rather than interpreting its raw representation as nil.
- Fixed `str.word_frequency` to return a count of one for words seen once.
- Fixed a native startup crash in the editor by making completion, preview, and
  drag state dictionaries own their storage.
- Fixed audio stream sizing, sample continuity, gain initialization, and
  playback error handling. The sound example generates and plays a clean
  sine-wave WAV through the selected backend.
- Fixed the automatic Vulkan-to-OpenGL fallback showing a blank window before
  its first rendered frame.
- Fixed Vulkan swapchain image selection and presentation semaphore reuse that
  could leave X11 animation windows black.
- Fixed the animation example's blank startup and unresponsive Escape/resize
  handling.
- Fixed native inlining to preserve aggregate return types and stop when its
  growth budget is exhausted. Fixed Wasm calls with more than four arguments
  and AVR frame setup clobbering argument registers before they were saved.
- Fixed native variable shifts and rotations reading stale shift counts,
  including in loops expanded by control-flow inlining.
- Fixed x86-64 register allocation across constant division by marking all
  lowering scratch registers as clobbered.
- Fixed x86-64 vector stores to use computed memory addresses, and restored
  binary-search lookup for the set-length runtime ABI declaration.
- Fixed x86-64 loop-resident values using stale spill homes on alternate CFG
  paths; register-home commits now invalidate stale RAX home tags before a
  later load can reuse them. Dedicated loop registers are skipped when a
  function contains calls inside loops or vector values until those cases are
  covered by the allocation proof.
- Aligned the BigInt benchmark's C reference with Ny's 256-bit BigFloat
  operations.
- Preserved signed integer division by powers of two unless the dividend is
  proven non-negative, preventing incorrect optimized FFT and Linpack results.

</details>

## [0.9] - 2026-08-21

Native tooling, compiler reorganization, and runtime reliability.

<details>
<summary>Added</summary>

- In-tree C frontend for FFI headers — arrays, unions, packed structs,
  bitfields, extended numeric types, function pointers, typedefs, macros, and
  variadics — replacing libclang for supported shapes, with parse deadlines,
  recursion limits, and bounded tables for untrusted headers.

- Optional GMP support with native bigint fallbacks; `-Oz` size optimization;
  `#assert` compile-time assertions; `core.report` formatted diagnostics;
  `--zero-init` managed-heap controls; and public `core.os.rev` modules for
  decompilation and symbolic execution.

- Linux/x86-64 syscall restrictions blocking process creation and
  executable-memory remapping through `mprotect`/`clone`/`fork`/`vfork`.

</details>

<details>
<summary>Changed</summary>

- Compiler sources reorganized around explicit subsystem boundaries
  (`src/base`, `src/cmd/ny/repl`, `src/code/*`); optional LLVM emission and
  JIT folded into the unified native backend with the direct AST-to-LLVM
  implementation quarantined behind a semantic parity gate.

- Native/self-build linking defaults to the system linker instead of
  auto-selecting an optional fast linker; alternate linker selection stays
  explicit.

- Expanded native compiler diagnostics and optimization analysis.

- Tagged-int fast paths extended to bitwise ops, proven-safe shifts, and
  ordered comparisons, and enabled under `--profile=speed`.

</details>

<details>
<summary>Fixed</summary>

- SSA correctness: mem2reg parameter promotion, escape-SROA parameter slots,
  and native buffer allocation/bounds facts for `f64buf`/`i64buf` operations.

- Native socket fixes: ephemeral-port decoding reads sockaddr bytes directly,
  and `socket_accept` returns the raw descriptor.

- Loop analysis unification, guarded vector-tail repairs (preheader insertion,
  PHI stability, scalar-IV remapping), syscall filter and `rdrand` assembly
  guards, AArch64 condition encoding, and bigint heap corruption.

</details>

## [0.8] - 2026-07-13

Native execution, proof tooling, and platform parity

<details>
<summary>Added</summary>

- `wasm-emscripten` web target emits standalone Wasm with `_ny_top_entry` and
  an explicit `env.*` namespace; explicit aliases opt into namespacing while
  existing Nytrix declarations keep precedence.

- `prove(condition[, message]) -> proof` introduces compile-time proof
  witnesses; false or dynamic obligations fail compilation, and ordinary
  values cannot satisfy proof parameters. `core.math.logic` adds evaluation,
  simplification, certificates, bounded solvers, rewriting, and Prolog-style
  unification and backtracking.

- Kernel-backed file watching and hot reload use inotify, kqueue, and Windows
  change notifications behind the `core.os.fs.watch_*` API, with an mtime
  fallback.

</details>

<details>
<summary>Changed</summary>

- NYIR now coalesces copy/local chains, allocates scalar registers, selects
  immediate operands, indexes DCE label references once, and preserves
  floating types across collapsed equivalence classes.

- Precomputed x86-64 call boundaries and immediate constants reduced a focused
  call body and frame-relative accesses in the native encoder.

- Stdlib source sweeps stop after optimized IR instead of materializing MCJIT,
  and cache format updates reject mixed stdlib/user entries and
  sanitizer-contaminated native objects.

- `ny-fmt --cloc` now reports tracked additions/deletions and per-file totals.

- Hot reload blocks on native events instead of busy mtime polling, reducing
  idle CPU use and edit-to-recompile latency.

- The opt-in JIT shared-object tier now validates the matching bitcode
  provenance sidecar before loading, so interrupted or stale cache entries
  return to normal compilation instead of being trusted.

</details>

<details>
<summary>Fixed</summary>

- New REPL snapshots bind their source payload to a fingerprint and byte
  length; `:load` rejects corrupted v2 images before rebuilding persistent
  state while continuing to accept older source-only snapshots.

- `ny-fmt` C analysis now shares one lexical scanner across function ranges,
  structural rankings, and duplicate detection. It ignores strings, comments,
  multiline macros, control conditions, and call continuations rather than
  reporting phantom C functions.

- `ny-fmt --bugs` now reports discarded value-typed `list.append`/`extend`
  results and `set()` calls on dictionaries initialized as frozen `{}`
  literals, with stable `NYAUD1120` and `NYAUD1119` diagnostics and practical
  fixes.

- x86-64 internal JIT/object emission now carries up to 1,024 call/data
  relocations per program bundle, so large straight-line call sites no longer
  fail at the old 256-relocation transport ceiling.

- Live native JIT/object requests now support up to 128 lowered user functions
  (previously 64) and reject larger requests before lowering. Portable NYIP
  bundles retain their separate 4,096-function NYIR VM boundary.

- macOS transitive libc aggregates now materialize named return and parameter
  layouts on demand, without registering anonymous carriers as builtin
  scalars. Installed system headers recover useful declarations from
  unsupported syntax; project headers remain strict.

- Apple-arm64 comptime MCJIT now uses managed invocation so indirect callees
  finalize before entry. Native-only link discovery also matches JIT behavior
  and deduplicates source annotations.

- Corrected x86-64 floating constant placement and typed f32/f64 local
  preservation, eliminating nondeterministic native ELF results.

- Hardened sanitizer AOT temporary output, cache isolation, cleanup, and UBSan
  handling.

- Corrected Windows JIT compatibility, target-width libc fixtures, variadic C
  imports, trace/debug progress suppression, ELF32 return bounds, watcher
  lifetime, parser recovery, and dictionary helper ambiguity.

</details>

## [0.7] - 2026-06-30

LLVM-free native backend and C interoperability

<details>
<summary>Added</summary>

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

Fuzzing, crypto/math expansion, renderer polish

<details>
<summary>Added</summary>

- Published fuzzer and tooling for error-shape discovery.

- Radix helpers, stream/block ciphers, public-key helpers, and
  lattice/factorization modules.

</details>

<details>
<summary>Changed</summary>

- SVG/UI rendering: 4x4 supersampling, stroke linecap/linejoin,
  gradient/`<use>` support, and terminal 256-color output.

- `--borrow-check` decoupled from `--ownership-strict`; Z3 enabled by default;
  proven-nonzero `f64` division checks elided.

- glTF hot paths moved from `src/code/lang/runtime/gltf.c` into Ny code.

- CMake dependency probing hardened for LLVM, libclang, Z3, Windows
  UCRT/MSYS2.

</details>

<details>
<summary>Fixed</summary>

- Canvas UTF-8 buffer type mismatches and terminal renderer edge cases.

- Lowercase type-first local binding parsing.

- Semicolon comment ambiguity in parser diagnostics.

- Windows build integration, joystick axis handling, SDK/toolchain probing.

- zlib decompression capacity handling.

</details>

## [0.5] - 2026-06-05

Editor/viewer framework

<details>
<summary>Added</summary>

- Editor and engine viewer (`core.os.ui.render.viewer`): asset browser,
  hierarchy, inspector, gizmos, transform tools, and runtime bootstrap.

- OpenGL, WebGL, and Vulkan renderer paths for the viewer.

- WebAssembly compiler backend foundation.

- RSS feed, Discord, and Mastodon integration.

</details>

<details>
<summary>Changed</summary>

- Renderer/viewer split into distinct `render` and `viewer` layers.

- Function syntax moved from `fn foo(type: arg): ret` to `fn foo(type arg)
  ret`.

- Module self-checks moved into `#main` blocks.

</details>

<details>
<summary>Fixed</summary>

- Vulkan UI mesh caching and text-fitting crashes on startup.

- Animated glTF mesh index-buffer retention and texture reuse.

- GLSL syntax restoration and screen redraw stability.

</details>

## [0.4] - 2026-05-30

Ownership, typed pipeline, CLI unification

<details>
<summary>Added</summary>

- Cross-platform windowing/input: Win32, Cocoa, X11, Wayland, Vulkan.

- Typed compiler pipeline: Hindley-Milner inference, lambda/nested-collection
  inference, and monomorphic specialization.

- `&expr` shorthand for `borrow(expr)`, ownership contracts, `--safe- mode`.

- `handle`, `fnptr`, `seq` types; layout records/guards with compile- time
  reflection.

- Unified CLI: `ny fmt`, `ny test`, `ny doc`, `ny perf`, `ny make`, `ny pkg`,
  `ny new`.

</details>

<details>
<summary>Changed</summary>

- Compiler/runtime/Vulkan internals standardized on raw integer
  representations.

- `-O2` became the default native optimization level.

- Bootstrap and dependency discovery reworked for cross-platform setup.

</details>

<details>
<summary>Fixed</summary>

- Emit-only compiler hangs from recursive raw-integer fast paths.

- macOS arm64 comptime evaluation for immutable collections.

- FFI header import collisions and ownership diagnostics for returned values.

- Mutable closure captures across repeated calls.

</details>

## [0.3] - 2026-04-13

Graphics stack and platform expansion

<details>
<summary>Added</summary>

- glTF loading, Meshopt integration, mesh/glTF parsers, and an image parser
  stack.

- Vulkan rendering, scene graph, sky/SDF shaders, and split Vulkan/GUI
  renderer paths.

- Terminal renderer integrated into `core.os.ui`; Win32 window backend added.

- IO and networking modules; JACK audio backend.

- Public fonts, dictionaries, website assets, and renderer shaders.

- Maintained sample programs, REPL import scenarios, and an updated learning
  guide.

</details>

<details>
<summary>Changed</summary>

- Keep canonical stack homes current for dedicated loop GPRs, including
  non-resident CFG-exit consumers.

- Loop GPR homes are enabled by default; `NYTRIX_DISABLE_LOOP_GPR=1` disables
  them for backend diagnosis. Calls within loops remain conservative.

- Platform APIs moved into `core.os`; window backends moved into
  `core.os.ui.window`.

- Legacy native window backend path removed in favor of the new backend split.

- Cache management, bigint support, and shader generation improved for
  graphics workloads.

</details>

<details>
<summary>Fixed</summary>

- Asset path drift and shader-generation regressions during scene coverage
  expansion.

- Runtime fixture mismatches introduced while moving platform code into
  `core.os`.

</details>

## [0.2] - 2026-03-09

Compiler, runtime, and stdlib foundation

<details>
<summary>Added</summary>

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

- Specification manuals and release notes.

</details>

<details>
<summary>Changed</summary>

- Standard library moved to `lib/`, reducing prelude coupling.

- Parser, Vulkan renderer core, and UI renderer split into focused modules.

- Std module layout reorganized; numeric modules moved into `core.math`.

- Python build/bundle tooling replaced with native tools.

</details>

<details>
<summary>Fixed</summary>

- Standard-library import coupling and module-path drift.

</details>

## [0.1] - 2025-12-24

Prototype bootstrap

<details>
<summary>Added</summary>

- Launcher skeleton, build script, and CMake scaffold (`make`,
  `CMakeLists.txt`, `src/cmd/ny/main.c`).

- Runtime placeholders and smoke fixtures for a first compilable, testable
  tree.

</details>
