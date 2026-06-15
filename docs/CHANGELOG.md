# Changelog

Nytrix uses dated release milestones. Exact build snapshots are identified by the generated `NYTRIX_VERSION` Git metadata reported by `ny --version`.

## Roadmap

* [ ] Native x86/x64 backend for faster comptime and reduced LLVM dependency.

## [0.5.0] - 2026-06-05

### Added

* Editor and engine viewer framework in `std.os.ui.render.viewer`, including asset browser, hierarchy, inspector, panels, color picker, view controls, transform tools, gizmos, runtime bootstrap, and input utilities.
* Native editor icons, prototype textures, shared environment assets, and renderer shaders under `etc/assets`.
* RSS feed, Discord, and Mastodon integration.
* OpenGL and WebGL rendering backends.
* WebAssembly (Wasm) compiler backend.

### Changed

* Simplified editor layouts, asset browsing, hierarchy and inspector panels, and F1 editor toggling.
* Simplified typed function syntax to `fn foo(type arg) ret`.
* Improved window resizing, input handling, MSYS2 support, and runtime deployment.
* Reworked renderer and viewer architecture around the `render/viewer` split.
* Optimized animated glTF mesh updates by retaining GPU index buffers.
* Moved lightweight module checks into `#main` self-tests.
* Removed dead code and tightened module boundaries.
* Shortened documentation and release notes.

### Fixed

* Fixed asset loading, model reloading, texture reuse, editor focus, and panel state regressions.
* Fixed Vulkan UI mesh caching and text-fitting crashes during startup.

## [0.4.0] - 2026-05-30

### Added

* Expanded cross-platform windowing and input support across Win32, Cocoa, X11, Wayland, and Vulkan.
* Added `lib/core/regex.ny`.
* Added the typed compiler pipeline, including typed AST metadata, Hindley–Milner inference, lambda inference, nested collection inference, nullable branch merging, and monomorphic specialization.
* Added `&expr` shorthand for `borrow(expr)`.
* Added ownership and borrow checking through `--borrow-check`, `--ownership-strict`, ownership contracts, and ownership primitives.
* Added `handle`, `fnptr`, and `seq` types.
* Added layout records, layout shapes, layout guards, ABI integration, and compile-time layout reflection.
* Added `--safe-mode`.
* Added unified CLI tooling: `ny fmt`, `ny test`, `ny doc`, `ny perf`, `ny make`, `ny pkg`, and `ny new`.
* Added documentation for ownership, FFI, packages, language features, and CLI tooling.

### Changed

* Kept `0.4.x` as the active development series.
* Added Git-derived build metadata reporting.
* Reworked bootstrap and dependency discovery.
* Improved REPL editing, Unicode handling, paste support, and Windows console behavior.
* Reworked compiler, runtime, and Vulkan internals around raw integer representations.
* Reworked parsing, typing, lowering, diagnostics, and tooling around the typed compiler pipeline.
* Unified optimization profile handling.
* Made `-O2` the default native optimization level.
* Simplified package layout and import paths.
* Ported most project tooling to native commands.
* Reduced repository clutter and standardized shared assets under `etc/assets`.

### Fixed

* Fixed emit-only compiler hangs caused by recursive raw-integer fast paths.
* Fixed macOS arm64 comptime evaluation for immutable collections.
* Fixed interpreted comptime block result propagation.
* Fixed lazy stdlib closure emission.
* Fixed FFI header import collisions.
* Fixed ownership diagnostics for returned owned values.
* Fixed single-value `if` expressions.
* Fixed integer range-proof propagation.
* Fixed macOS arm64 ABI coverage, SDK discovery, and sleep behavior.
* Fixed mutable closure captures across repeated calls.
* Fixed detached thread launch result tagging.
* Fixed unnecessary GC arena allocation during startup.
* Fixed compiler cache validation and cross-platform timestamp handling.

## [0.3.0] - 2026-04-13

### Added

* First major graphics stack, including glTF, Meshopt, image and font parsing, Vulkan rendering, scene integration, frame dumps, sky and SDF shader pipelines, and renderer math support.
* Added public fonts, dictionaries, website assets, renderer shaders, and expanded native windowing and asset-loading support.

### Changed

* Improved cache management, bigint support, FFI, lowering, code generation, shader generation, and profiling for larger graphics workloads.

## [0.2.0] - 2026-03-09

### Added

* Enums, suffixes, structs, packed layouts, `sizeof`, pointer dereference, `try`, reflection operators, effects, JIT metadata, and standard library attributes.
* Expanded the standard library with IO, JSON, fuzzing, crypto, math, process, audio, windowing, input, GPU, OpenCL, Vulkan, networking, LLVM, and vcpkg integration.

### Changed

* Moved the standard library to `lib/`.
* Removed prelude coupling.
* Adopted `#main { ... }` execution blocks.
* Reorganized crypto and math modules.

## [0.1.0] - 2025-12-24

### Added

* Core language foundations, including the lexer, parser, AST, runtime model, compiler intrinsics, JIT mapping, REPL, and `ny-lsp`.
* Added `mut`, `defer`, destructuring, runtime primitives, arena-backed compiler memory, sanitizer support, documentation, web documentation, CLI tooling, diagnostics, fuzzy symbol suggestions, and immutability checks.

### Changed

* Established the LLVM/C architecture, runtime ABI, module loading, diagnostics, lowering pipeline, cache infrastructure, and initial test suite.
