# Changelog

Nytrix uses dated development milestones. The compiler reports a
Git-derived build version through the generated `NYTRIX_VERSION` define in
`nytrix_version.h`, surfaced by `ny --version`.

## Open maintenance checks

- [ ] Fix windows gamepad support.

## [0.4.3] - 2026-06-02

### Added
- Added much broader cross-platform window/input coverage: Win32 raw input,
  Cocoa AppKit windows/cursors/monitors, and Linux X11/Wayland/Vulkan plumbing.
- Added the rounded-rectangle UI shader, expanded gamepad/demo probes, a parser
  error fixture, and the new `lib/core/regex.ny` module.

### Changed
- Reworked `./make` bootstrap/dependency discovery.
- Reworked REPL input, paste, Unicode, delete, especially for Windows `cmd`.
- Reworked compiler/runtime/Vulkan internals around raw ints..

## [0.4.2] - 2026-05-31

### Changed
- Kept `0.4.x` as the active development line while Git-derived build metadata
  continues to identify exact build snapshots.
- Made development builds report the release version with Git build metadata,
  preferring the tracked remote ref from `.git` before falling back to `HEAD`.

### Fixed
- Fixed macOS arm64 comptime evaluation for small immutable list, tuple, and
  range values so compile-time sequence helpers do not fall back to transient
  JIT execution.
- Fixed interpreted comptime blocks so expression-only blocks and single-expression
  lambdas preserve their result value.

## [0.4.1] - 2026-05-31

### Changed
- Kept the repository focused on source, docs, assets, tests, and release
  metadata while leaving generated and local-only material out of version
  control.
- Kept shared assets under `etc/assets` so public runtime examples can resolve
  fonts, dictionaries, website files, and renderer shaders through stable paths.

### Fixed
- Fixed lazy stdlib closure emission so nested closure bodies are emitted when
  reached through deferred standard-library loading.
- Kept the public branch layout focused on `main` after consolidating the
  private development snapshots.

## [0.4.0] - 2026-05-30

### Added
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
- Added `--safe-mode` as a real safety profile covering strict types,
  ownership checks, RC/RAII cleanup, strict effect/alias policy, and
  compiler-tracked raw-memory range diagnostics.
- Added unified `ny` subcommands for tooling: `ny fmt`, `ny test`, `ny doc`,
  `ny perf`, `ny make`, `ny pkg`, and `ny new`.
- Added docs for ADTs, generic types, async/await, ownership contracts,
  `#include` FFI, compile-time proofs, package flows, and the unified CLI.

### Changed
- Reworked parsing, type metadata, lowering, call emission, diagnostics,
  module loading, and tool integration around the typed compiler pipeline.
- Made CLI optimization profiles drive the same LLVM/runtime/codegen fast
  paths as `NYTRIX_OPT_PROFILE`.
- Made default native emission match the documented `-O2` baseline while
  keeping `-O3` explicit.
- Reorganized generated/source modules around stable compiler-visible package
  categories and shorter import paths.
- Ported most project tooling into native C commands while keeping `./make` as
  a small compatibility entrypoint.

### Fixed
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

### Added
- Added glTF, Meshopt, image, and font parsing.
- Added lit, sky, ring, circle, and SDF shader paths.
- Added Vulkan buffer, pipeline, texture, renderer-state, and frame-dump paths.
- Added render math, environment textures, model scene integration, and broader
  native window/graphics/asset-loading boundaries.
- Added public fonts, dictionaries, website assets, and renderer shader assets
  under `etc/assets`.

### Changed
- Tightened cache, bigint, FFI, lowering, and call emission for large native
  graphics workloads.
- Routed generated shader and profile artifacts through cache-aware output
  paths.
- Refined render-facing math and codegen interactions without changing the
  language syntax.

## [0.2.0] - 2026-03-09 to 2026-03-27

### Added
- Added enums, numeric suffixes, structs, packed layouts, `sizeof`, pointer
  dereference, `try`, generic reflection operators, and effect attributes.
- Added standard-library JIT metadata and compiler attributes for hot stdlib
  functions, matching the March backup work around `@jit` and purity analysis.
- Added `std.os.io`, JSON support, fuzzing hooks, and broader FFI call
  capacity.
- Added number theory, matrices, finite fields, NTT, binary helpers, RSA, ECC,
  DLP, HNP, lattice, PRNG, hash, and protocol modules.
- Added process, audio, window, input, GPU, OpenCL, Vulkan, hardware-facing ABI,
  and network-facing FFI probes.
- Added multiline REPL editing, UI demos, fonts, render examples, and broader
  runtime coverage.
- Added cross-platform LLVM/vcpkg, Linux ARM, macOS, and Windows build paths.

### Changed
- Moved stdlib code under `lib/`, dropped prelude coupling, and enforced
  compact `if(comptime{__main()}){...}` gates.
- Reorganized crypto into encoding, protocol, number, lattice, RSA, ECC, DLP,
  PRNG, hash, and analysis groups.
- Expanded stdlib coverage while tightening compiler checks around inference,
  FFI boundaries, and runtime behavior.
- Iterated on Vulkan/UI modules, matrix/vector math, and stdlib optimization
  hooks through the March backup series.

## [0.1.0] - 2025-12-24 to 2026-01-31

### Added
- Added lexer, parser, AST, runtime model, core intrinsics, JIT mapping, REPL,
  and `ny-lsp` foundations.
- Added top-level script execution and `main` process exit behavior.
- Added `mut`, `defer`, destructuring, consistent runtime primitives,
  arena-backed compiler memory, and stdlib core organization.
- Added sanitizer flows, Texinfo docs, web docs, CLI tooling, diagnostics,
  fuzzy symbol suggestions, and immutability checks.

### Changed
- Established the C/LLVM build layout, runtime ABI boundary, source loading,
  module lookup, and initial test fixtures.
- Split the early compiler work into parser, module loading, diagnostics,
  lowering, runtime, REPL, wire/cache, and core-library milestones.
