# Tooling

Use `ny --help` and subcommand help for the full flag list. This page keeps the
common build, run, docs, format, test, and diagnostic loops.

## Core commands

```bash
./make
./make ny --no-progress --color=never <file>
./make test --with-stdlib --failures-only
ny-fmt --bugs --limit 80 <changed-paths>
git diff --check
```

Use `--no-progress --color=never` for stable, searchable output.

**Recompiling C source:** `./make` and `./make ny` recompile C binaries and the
standard-library bundle incrementally on change. The runtime is an
amalgamation — `src/rt/init.c` `#include`s `os.c`, `core.c`, `memory.c`, etc.
as one translation unit — so an edit to any amalgamated `.c` is picked up by
the next `./make`. If a run looks like it ignored a source edit, force a clean
rebuild:

```bash
./make ny --no-progress --color=never <file>   # rebuild + run (normal loop)
./make clean && ./make                          # full recompile when in doubt
./make test --color never
```

When validating runtime/native behavior, confirm a unique probe string appears
in `strings build/release/ny-full` before trusting the run.

## Validation depth

- Generated `.ny`: execute it, fix the first real error, rerun.
- Parser or diagnostics: run the focused parser/diagnostic fixture set.
- Comptime or macros: inspect expansion and run the generated source.
- Standard library: run focused probes and rebuild the standard bundle.
- Compiler/runtime/native: run focused fixtures, then the broader set.
- Renderer/UI: run a bounded visual, framebuffer, or artifact probe.
- Fuzzing: run the smallest reproducible seed first.
- Sanitizers: only claim ASan/UBSan/TSan coverage when that sanitizer is
  enabled.

## Common Loop

```bash
ny file.ny
ny fmt --check file.ny
ny --diag-rich file.ny
ny test --pattern name
```

Use `-run` or `-o` for native checks. Use `ny doc search` before guessing API
names.

## Tooling Shape

The command surface stays small on purpose. Use diagnostics to catch mistakes
before runtime, and use environment inspection instead of remembering hidden
state:

```bash
./make doctor
./make doctor --install
./make env
./make targets
```

`doctor` checks required build tools, writable caches, built artifacts, optional
runners, optional std/native deps, and UI display state. Plain `doctor` is
read-only; `doctor --install` installs known required deps, optional std/native
deps, and qemu/wine runners where the host package manager is supported. `env`
prints the effective paths and overrides. `targets` lists the supported cross
presets and runner status.

## Configuration

Nytrix tools load default configuration from simple env-style files. Real
environment variables still win.

Search order:

```text
$NYTRIX_CONFIG or $NY_CONFIG
./.nytrix/config
./nytrix.config
$XDG_CONFIG_HOME/nytrix/config
$XDG_CONFIG_HOME/ny/config
~/.config/nytrix/config
~/.config/ny/config
```

Use `KEY=value` or `export KEY=value` lines. `#` and `;` start comments.

```text
BUILD_DIR=build
NYTRIX_BUILD_JOBS=12
NYTRIX_PKG_HOME=~/.local/share/nytrix/pkg
NYTRIX_PKG_PATH=./ny_modules:./vendor/ny_modules
NYTRIX_STD_OVERLAY=./std_overrides
NYTRIX_MINGW_CC=x86_64-w64-mingw32-gcc
NYTRIX_MINGW_SYSROOT=/usr/x86_64-w64-mingw32
repo core = git+https://github.com/owner/ny-packages.git
```

`./make env` prints the loaded config files. `ny pkg repo list` also reads
`repo name = source` lines from the same config files.

`NYTRIX_STD_OVERLAY` is a `:` or `;` separated list of roots scanned before the
bundled standard library. Use it to replace one std/lib module in a project
without copying all of std:

```text
std_overrides/core/str.ny       # declares: module std.core.str(...)
std_overrides/os/ui/theme.ny    # declares: module std.os.ui.theme(...)
```

Project-local `.nytrix/std`, `.nytrix/lib`, `std_overrides`, and
`lib_overrides` directories are scanned automatically when they exist.

## Build

```bash
chmod +x make
./make all
./make install
ny --version
```

Windows:

```bat
py -3 -B .\make all
```

The Windows wrapper finds or installs MSYS2, installs UCRT64 packages, and
points CMake at that toolchain.

## Cross Compile

Nytrix native output can target another platform by giving the compiler a target
triple and matching host compiler/linker flags. The `./make cross` wrapper keeps
that setup in one place:

```bash
./make targets
./make cross linux-arm64 hello.ny
./make cross --target aarch64-linux-gnu --sysroot /opt/aarch64-sysroot hello.ny
./make cross-run windows-x64 hello.ny
```

The wrapper emits binaries under `build/cache/cross/<target>/` unless `-o` or
`--output` is passed. Presets include `linux-x64`, `linux-arm64`,
`linux-armhf`, `linux-riscv64`, and `windows-x64`. The Windows preset emits an
`.exe`, prefers `NYTRIX_MINGW_CC` when configured, auto-detects
`x86_64-w64-mingw32-gcc`, and uses Wine for `cross-run`.

`./make cross-run` compiles first, then runs through qemu or wine when the
runner is installed:

```bash
./make cross-run linux-arm64 hello.ny
./make cross-run linux-arm64 --sysroot /opt/aarch64-sysroot hello.ny -- arg1 arg2
```

qemu and wine are soft dependencies. If the runner is missing, `cross-run`
prints the tool name and keeps the compiled artifact.

The direct compiler flags remain available for custom toolchains:

```bash
ny --host-triple aarch64-linux-gnu \
   --host-cflags "--target=aarch64-linux-gnu --sysroot=/opt/aarch64-sysroot" \
   --host-ldflags "--target=aarch64-linux-gnu --sysroot=/opt/aarch64-sysroot" \
   -o build/hello-aarch64 hello.ny
```

## Wasm Runner

Compile one Ny source file to WebAssembly:

```bash
./make wasm etc/projects/os/args.ny --out build/web/wasm/args.wasm
```

Build the browser runner:

```bash
./make web-demos
```

Build one browser-ready Ny source into a deployable directory:

```bash
./make web etc/projects/ui/pong.ny --out build/web/pong
```

`web` currently selects the explicit `wasm-bare` + WebGL2 target. It writes
the runner, `app.wasm`, `web-report.json`, and `build-manifest.json`; it rejects
missing browser host APIs before publishing output. It is not
an Emscripten compatibility spelling: requesting `wasm-emscripten` fails
clearly until that adapter is implemented.

The manifest declares the implemented baseline: WebGL2, keyboard, mouse, an
Asyncify frame loop, `audioLifecycle`, and a small depth-tested WebGL2 3D
baseline (`camera_init`, `begin_mode_3d`, `draw_cube`, `end_mode_3d`). It is a
portable proof path, not yet a claim that every desktop renderer primitive or
camera option has browser parity. A 3D pass clears depth once, accepts multiple
draws, and is composited with later 2D stage drawing at `end_frame`; it is not a
special fullscreen cube replacement. Translucent baseline draws use standard
source-alpha blending without writing depth, while opaque draws remain
depth-writing. `std.os.sound.init()` opens Web
Audio in its browser-required suspended state; the runner resumes it only after
a key, pointer, or touch gesture and visibly reports `Audio suspended`,
`Audio`, or `Audio unavailable`. The browser suite covers lifecycle and packed
asset decode/playback; this is not a claim that every desktop sound backend or
synchronous file decoder is portable.
Touch and gamepad facades are covered by browser fixtures and are advertised
as supported capabilities. Gamepad connection events refresh the browser host
state immediately, and canvas scaling follows its containing stage through
`ResizeObserver` when available, with a window-resize fallback. General host
host filesystem access, network, threads, native windows, and Vulkan remain
explicitly unsupported; they are not host fallbacks. A browser module may read
an explicitly packaged file through `std.os.file_read`. Browser
`std.os.file_write`, `file_remove`, and `file_rename` use a scoped VFS persisted
in `localStorage`; they never write the host filesystem. Directory handles
expose entries from packaged assets and the persisted VFS. These paths are
covered by `filesystem-asset-read.ny` and
`filesystem-persistence.ny`.
The normal window facade does expose browser fullscreen and input-exclusive
requests. Those requests return the current browser state, never an optimistic
success value: query again after the browser's `fullscreenchange` or
`pointerlockchange` event. The manifest records them as `fullscreenRequest`
and `pointerLockRequest`, while `fullscreen` and `pointerLock` remain false
until the facade has an event/result API and interaction coverage.

`web` keeps Asyncify enabled for ordinary `main`-style games so they cannot
block the browser event loop. `--no-asyncify` is accepted only when the module
exports `ny_web_frame` or `ny_web_render`, which the runner calls from its
browser frame callback.

Use `--assets` to package and preload the files referenced by string literals in
the source under that directory. The build writes a deterministic
`assets.data` blob and `assets.data.json` index (logical path, aligned offset,
size, and SHA-256) rather than scattering selected files through output.
Pass `--preload-all` with one or more `--assets` roots when a game loads assets
indirectly or needs a complete bundle: every regular file below those roots is
included in the same deterministic data pack. It fetches the complete pack but
does not blindly activate every file as a font or other browser resource;
source-referenced font assets keep their normal eager font registration.
Repository-relative paths are retained, so a program that refers to
`etc/assets/fonts/name.ttf` can use `--assets etc/assets`; preloaded TTF, OTF,
WOFF, and WOFF2 fonts are available to browser text drawing. This is an
Emscripten-style data pack for the implemented browser runner. It supports
packaged reads and the scoped browser VFS; it is not a claim of general host
POSIX filesystem access inside Wasm.

The browser stage uses nearest-neighbour WebGL sampling and disables Canvas2D
image smoothing. This preserves pixel-art and bitmap-font pixels at the final
present step. Text positions and requested font sizes are snapped to whole
stage pixels; a future direct WebGL glyph-atlas path is still needed for a
fully renderer-native text pipeline.

Every deployable browser output also packages `assets/monocraft.ttf` for the
runner UI and its Canvas fallback text. It uses the same Monocraft face with a
readable monospace fallback, so local browser font installation never changes
the tool's layout.

Check that a Ny source uses only browser-hosted APIs, then run the maintained
Pong WebGL2 smoke test in a headless browser. The smoke test also proves that a
native process call is rejected with the exact machine-readable unsupported
import record:

```bash
./make web-check etc/projects/ui/pong.ny
./make web-check etc/projects/ui/pong.ny --target wasm-bare
./make web-test
```

With a working browser installation, `web-test` is an executable browser
check, not a packaging-only check. It covers the Pong WebGL2 frame path, audio
gesture and decode paths, the 3D baseline, pointer/touch/gamepad facades, and
framebuffer/texture round trips. The command prints the browser version before
launching; an unusable browser fails with a repair-oriented diagnostic.

Both `web` and `web-check` resolve the same browser target descriptor. Asking
either command for `wasm-emscripten` reports that the dedicated adapter is not
implemented; it is never silently treated as `wasm-bare`.

All web build output lands under `build/web/` (`build/web/demos` for the demo
runner, `build/web/check` for web-check reports, `build/web/<app>` for `web`
apps, `build/web/wasm` for wasm output, and `build/web/ir` for LLVM IR). The
runner is a small static browser shell:
`index.html`, `web.css`, `wasm.js`, and `demos-data.js`. It can load a local
`.wasm` file from the page, or load optional manifest entries from
`etc/assets/website/wasm/demos.json`.

Browser-facing Ny modules should export one of `ny_web_frame`, `ny_web_render`,
`ny_web_main`, or `main`. The runner provides a compact host ABI:
`ny_web_clear`, `ny_web_rect`, `ny_web_line`, `ny_web_text`, `ny_web_present`,
keyboard, mouse-pointer, and minimal OS/runtime stubs. Keyboard state tracks each
held key and one press edge per key; `std.os.ui.window.input.mouse_pos`,
`mouse_button_down`, and `mouse_button_pressed` use logical-stage coordinates
and per-button held/edge state. `std.os.ui.window.scroll_pos` accumulates
browser wheel deltas. General native sound, network, and filesystem APIs are
not faked in the browser. `web-check` reports unsupported host imports and
writes a machine-readable report beside its output.

Use `--out DIR` to choose a different output directory, `--no-ny-wasm` to copy
only the static browser files, or `--require-ny-wasm` to fail unless every
manifest Ny source emits wasm.

## Run Modes

| Form | Behavior |
| --- | --- |
| `ny` | Start REPL, or read piped stdin as REPL batch input. |
| `ny file.ny` | Run through JIT path. |
| `ny -c 'code'` | Run inline source. |
| `ny -ic 'code'`, `ny -ci 'code'` | Run inline source, then enter REPL. |
| `ny --repl < file.ny` | Run stdin source once through REPL batch path. |
| `ny -run file.ny` | Build and run a temporary native executable. |
| `ny -o app file.ny` | Emit a native executable. |
| `ny -i`, `ny --interactive`, `ny --plain-repl` | Start explicit REPL. |

Native `-o` defaults to optimized native output. JIT and REPL favor edit
latency.

## Format And Audit

```bash
ny fmt --fix file.ny
ny fmt --check file.ny
ny fmt --smart --checks file.ny
ny fmt --bugs --limit 80 file.ny
ny fmt --trim --check file.ny
ny fmt --cloc path
ny fmt --dead path --limit 80
ny fmt --specialize file.ny
ny fmt --metaprog file.ny
ny fmt --modules path
```

Formatting changes layout. Audit modes report advisory structural rankings and
findings; they are not proof that code is dead or an instruction to rewrite it.
Use `--apply` only after reviewing the change class.

## Compiler stage artifacts

`ny --stop-after=STAGE --emit-artifact=PATH file.ny` writes a pointer-free
semantic snapshot for `parse`, `hm`, `trait`, `flow`, `abi`, or `opt`. Every
artifact carries `artifact.schema = "ny.semantic-artifact.v1"`, a hexadecimal
source fingerprint and byte length for the exact expanded source it describes.
Reusable ABI/opt artifacts also carry compiler-source and semantic-configuration
fingerprints.
Consumers must validate those fields before reusing type, resolution, range,
or lowering facts; they are snapshots, never serialized parser-arena pointers.
Use `--verify-artifact=PATH` with the same input to perform that identity check
without compiling it. It succeeds only when both the expanded byte length and
fingerprint match; a changed source, stale standard library, or malformed
artifact fails explicitly.

```bash
ny --stop-after=flow --emit-artifact=/tmp/flow.json program.ny
ny --verify-artifact=/tmp/flow.json program.ny
```

`ny --use-artifact=PATH program.ny` is the compiler-facing form. It accepts
only a successful `abi` or `opt` artifact for the exact expanded input and
matching compiler-source and semantic-configuration fingerprints, then skips the duplicate
semantic-validation pass. Parsing and code generation still run, so this is
not a serialized arena or an unchecked compilation shortcut. Older snapshots
without those reuse identities remain source-verifiable but cannot be reused
for semantic facts. The configuration identity includes strict-type mode,
solver selection, validation scope, native ABI, and C frontend selection.

The audit JSON contract is `ny-fmt.audit.v1`: it includes aggregate counters,
ranked files and functions, and diagnostic records. `--selftest` is a fast,
in-process regression check for C function-range detection and the
high-confidence semantic patterns used by `--bugs`; it does not format files
or require a separate test tool:

```bash
cmake --build build/release --target ny-fmt
build/release/ny-fmt --selftest
```

Some high-confidence `--bugs` diagnostics capture Nytrix runtime semantics:

| Diagnostic | Pattern | Correction |
| --- | --- | --- |
| `NYAUD1119` | `mut out = {}` followed by `out.set(...)` | Start mutable accumulators with `dict()`. |
| `NYAUD1120` | A standalone `xs.append(...)` or `xs.extend(...)` | Rebind the returned value: `xs = xs.append(...)`. |

These findings are still review leads: the formatter intentionally avoids
rewriting source, and accepted exceptions can be recorded with an audit
acceptance marker.

| Mode | Use |
| --- | --- |
| `--check`, `--fix` | Verify or rewrite formatting. |
| `--analyze`, `--audit`, `--smart`, `--checks` | General source review and stricter checks. |
| `--trim`, `--bloat`, `--overhaul`, `--dupes` | Size, repetition, and refactor pressure. |
| `--bugs` | Suspicious source patterns. |
| `--syntax`, `--types`, `--contracts` | Syntax, type, and contract audits. |
| `--dead`, `--modules`, `--profiles` | Dead code, module shape, and profile structure. |
| `--layouts`, `--ffi` | Native layout and FFI boundary checks. |
| `--constants`, `--constfold` | Constant and foldable expression checks. |
| `--specialize`, `--metaprog` | Typed fast-path and compile-time-generation candidates. |
| `--cloc`, `--conv` | Line counts and Texinfo conversion. |

## Docs

```bash
ny doc search [--docs|--symbols] query
ny doc get query
ny doc -o docs
```

Use `--symbols` for API names and `--docs` for concepts.

## Diagnostics

```bash
ny --diag-compact --collect-errors file.ny
ny --diag-rich file.ny
ny --safe-mode file.ny
ny --strict file.ny
ny --strict-types file.ny
ny --no-strict-types legacy_probe.ny
ny --borrow-check file.ny
ny --borrow-check --ownership-strict file.ny
ny --ownership file.ny
ny --heap=gc file.ny
ny --max-errors=20 file.ny
ny --warn=useful file.ny
ny --clean-cache
```

Compile-time type checks are on by default for typed code, generics, layouts,
and native boundaries. Suspicious dynamic fallbacks are warnings by default;
`--strict-types` rejects them for files that should stay fully statically
explainable. `--no-strict-types` is the compatibility escape hatch when that stricter
layer was enabled by a wrapper or environment. `--safe-mode` adds
ownership/borrow checks, RC/RAII cleanup, strict effect/alias policy, and
raw-memory diagnostics. `--strict` adds
ownership/borrow diagnostics (moves, releases, borrow escapes) without forcing
RAII runtime cleanup. `--borrow-check` enables the same diagnostics as `--strict`
without the extra type restrictions. Use `--ownership` (`--heap=raii`) to add
automatic runtime cleanup of owned values.

## Packages

```bash
ny new myapp
ny pkg init myapp
ny pkg info
ny pkg search [--interactive] query
ny pkg repo list
ny pkg add foo ./deps/foo
ny get bar
```

Package layout and resolver behavior are in [packages.md](packages.md).

## Tests

```bash
ny test
ny test --pattern name
ny test --with-stdlib module-or-path
```

Use a focused pattern for one area. Run the wider matrix for compiler, runtime,
stdlib, docs-generator, or public API changes.

## Compile-Time Audits

```bash
ny fmt --metaprog file.ny
ny fmt --specialize file.ny
ny fmt --trim --check file.ny
```

For compile-time guarantees, use `assert_compile`, `assert_compile_range`, and
`assert_compile_index` in source and run the file normally.

## Performance

```bash
ny perf
ny -o build/cache/bench/app bench.ny
ny -O3 --profile=peak -o build/cache/bench/app.peak bench.ny
ny fmt --cloc path
```

Performance notes should include command, binary, input, cache state, and
validation. Use [performance.md](performance.md) for timing discipline.

## Native checks

```bash
LD_LIBRARY_PATH=build/vendor/lib/host \
  ./build/release/ny-test --jobs 10 etc/tests/exec/native/c/*.nshape

LD_LIBRARY_PATH=build/vendor/lib/host \
  ./build/release/ny-test --jobs 8 etc/tests/exec/native
```

For focused x86-64 scalar checks use `--native-result-oracle[=N]`. The success
marker is:

```text
native oracle function=rt_main vm=<value> native=<value> ok=yes
```

External-linker fallback is not internal link/run success. Assembly emission is
not an executable-backend claim.

## Debugging with gdb and DWARF

When a native/AOT binary misbehaves, use gdb with DWARF:

```bash
ny -g --native-only --native-backend=x86_64 -o /tmp/ny-case \
  --no-progress --color=never <file-or--c>
gdb -q /tmp/ny-case
# (gdb) break rt_main
# (gdb) run
# (gdb) info sharedlibrary
# (gdb) disassemble /m
# (gdb) bt full
```

Prefer `-g` / `opt->debug_symbols` so line tables and locals are present.
For JIT crashes, attach after load or use a small AOT repro first.

## IR and assembly-driven optimization

When optimizing or diagnosing codegen quality:

1. Dump high-level NYIR and machine form before guessing:
   `--nyir-dump-raw`, `--nyir-dump-stats`, and any emit-asm / object dump the
   backend exposes for the path under test.
2. Reason about SSA values, machine instructions, and ABI slots from the dump
   — not from source-level intuition alone.
3. Profile when the cost is unclear (`perf record -g`, callgraph report).
4. Keep the fix at the owning pass (lower, machine, regalloc, object, runtime).

## Related

- [start.md](start.md)
- [troubleshooting.md](troubleshooting.md)
- [testing.md](testing.md)
