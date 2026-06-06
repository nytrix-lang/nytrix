# Changelog

Nytrix uses dated development milestones. Exact build snapshots still come from
the generated `NYTRIX_VERSION` Git metadata surfaced by `ny --version`.

## [0.5.0] - 2026-06-05

### Added
- Added the editor/engine viewer surface under `std.os.ui.render.viewer`,
  including asset browsing, hierarchy, inspector, panel chrome, color picker,
  profile/view controls, transform tools, gizmos, runtime bootstrap, and input
  helpers.
- Added native editor icons, prototype textures, shared environment assets, and
  renderer shader resources under `etc/assets`.

### Changed
- Reworked the UI renderer and viewer modules around the `render/viewer` split,
  compact editor panels, file-like asset browsing, tiled hierarchy/inspector
  layouts, and F1 editor toggling.
- Optimized dynamic indexed glTF meshes by keeping GPU index buffers for
  CPU-updated meshes, reducing per-frame vertex expansion on animated/skinned
  models.
- Changed syntax from `fn foo(typ: arg): type` to `fn foo(typ arg) type`.
- Tightened cross-platform polish around window resizing, cursor/input behavior,
  Windows/MSYS2 build setup, and runtime dependency copy.
- Moved cheap module checks behind `#main` self-tests where they protect public
  APIs without bloating the external test tree.
- Shortened release notes and documentation wording so changes are easier to
  scan.
- Removed dead helpers and tightened module boundaries.

### Fixed
- Hardened cached Vulkan UI meshes and icon-button text handling to avoid
  static-buffer/text-fit crashes during first-frame UI startup.
- Fixed renderer/editor asset loading, model reload, texture reuse, editor focus,
  and panel state handling regressions found while exercising the glTF viewer.

## [0.4.0] - 2026-05-30

### Added
- Added much broader cross-platform window/input coverage: Win32 raw input,
  Cocoa AppKit windows/cursors/monitors, and Linux X11/Wayland/Vulkan plumbing.
- Added the new module `lib/core/regex.ny`.
- Added the typed compiler pipeline: typed AST metadata, HM inference for
  function values and lambdas, nested list/dict inference, nullable branch
  merging, and monomorphic call specialization.
- Added `&expr` as source spelling for `borrow(expr)`.
- Added strict ownership and borrow checking through `--borrow-check`,
  `--ownership-strict`, `borrow`, `own`, `release`, `forget`, and ownership
  contract attributes.
- Added first-class compiler surface types for native and callable boundaries:
  `handle`, `fnptr`, and `seq`.
- Added typed layout records, layout shapes, layout guards, native layout ABI
  arguments/returns, and compile-time layout reflection.
- Added `--safe-mode` as a real safety profile covering strict types, ownership
  checks, RC/RAII cleanup, strict effect/alias policy, and compiler-tracked
  raw-memory range diagnostics.
- Added unified `ny` subcommands for tooling: `ny fmt`, `ny test`, `ny doc`,
  `ny perf`, `ny make`, `ny pkg`, and `ny new`.
- Added docs for ADTs, generic types, async/await, ownership contracts,
  `#include` FFI, compile-time proofs, package flows, and the unified CLI.

### Changed
- Kept `0.4.x` as the active development line while Git-derived build metadata
  continues to identify exact build snapshots.
- Made development builds report the release version with Git build metadata,
  preferring the tracked remote ref from `.git` before falling back to `HEAD`.
- Reworked `./make` bootstrap/dependency discovery.
- Reworked REPL input, paste, Unicode, and delete behavior, especially for
  Windows `cmd`.
- Reworked compiler/runtime/Vulkan internals around raw ints.
- Reworked parsing, type metadata, lowering, call emission, diagnostics, module
  loading, and tool integration around the typed compiler pipeline.
- Made CLI optimization profiles drive the same LLVM/runtime/codegen fast paths
  as `NYTRIX_OPT_PROFILE`.
- Made default native emission match the documented `-O2` baseline while
  keeping `-O3` explicit.
- Reorganized generated/source modules around stable compiler-visible package
  categories and shorter import paths.
- Ported most project tooling into native C commands while keeping `./make` as
  a small compatibility entrypoint.
- Kept the repository focused on source, docs, assets, tests, and release
  metadata while leaving generated and local-only material out of version
  control.
- Kept shared assets under `etc/assets` so public runtime examples can resolve
  fonts, dictionaries, website files, and renderer shaders through stable
  paths.

### Fixed
- Fixed emit-only compiler hangs caused by self-recursive raw-int fast-path
  calls.
- Fixed macOS arm64 comptime evaluation for small immutable list, tuple, and
  range values so compile-time sequence helpers do not fall back to transient
  JIT execution.
- Fixed interpreted comptime blocks so expression-only blocks and
  single-expression lambdas preserve their result value.
- Fixed lazy stdlib closure emission so nested closure bodies are emitted when
  reached through deferred standard-library loading.
- Kept the public branch layout focused on `main` after consolidating private
  development snapshots.
- Fixed unprefixed FFI header imports so colliding C functions no longer block
  wrappers such as `atoi` and `atof`.
- Fixed strict ownership diagnostics for returning owned tracked values without
  `@returns_owned`.
- Fixed single-value `if` expressions in binding and expression position.
- Fixed integer range-proof preservation through positive integer division and
  subtraction accumulators.
- Fixed macOS arm64 JIT/native ABI coverage, SDK/header discovery for FFI, and
  safe millisecond sleep behavior.
- Fixed mutable closure captures across repeated calls.
- Fixed detached thread launch return tagging so launch APIs report status
  instead of an untagged runtime value.
- Fixed runtime GC startup so default executions do not reserve nursery and
  tenured spaces unless GC is explicitly enabled.
- Fixed compiler cache keys and validation paths around std bitcode, AOT/JIT
  cache separation, and cross-platform file timestamp handling.

## [0.3.0] - 2026-04-13

- Added the first large graphics stack: glTF, Meshopt, image/font parsing,
  Vulkan buffers/pipelines/textures, scene integration, frame dumps, sky/SDF
  shader paths, and renderer-facing math.
- Added public fonts, dictionaries, website files, renderer shaders, and broader
  native window/asset-loading boundaries.
- Tightened cache, bigint, FFI, lowering, call emission, shader generation, and
  profile artifacts for larger native graphics workloads.

## [0.2.0] - 2026-03-09

- Added major language/runtime surface: enums, suffixes, structs, packed
  layouts, `sizeof`, pointer dereference, `try`, reflection operators, effects,
  JIT metadata, and hot stdlib attributes.
- Expanded the stdlib across IO, JSON, fuzzing hooks, math/crypto/protocol
  modules, process/audio/window/input/GPU/OpenCL/Vulkan/network probes, and
  cross-platform LLVM/vcpkg build paths.
- Moved stdlib code under `lib/`, removed prelude coupling, adopted compact
  `#main { ... }` execution gates, and reorganized crypto/math modules.

## [0.1.0] - 2025-12-24

- Built the foundations: lexer, parser, AST, runtime model, core intrinsics,
  JIT mapping, REPL, `ny-lsp`, top-level scripts, and process exit behavior.
- Added `mut`, `defer`, destructuring, runtime primitives, arena-backed
  compiler memory, sanitizer flows, docs, web docs, CLI tooling, diagnostics,
  fuzzy symbol suggestions, and immutability checks.
- Established the C/LLVM layout, runtime ABI boundary, source/module loading,
  diagnostics, lowering, wire/cache, and initial tests.
