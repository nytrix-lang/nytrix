# Tooling

Use `ny --help` and subcommand help for the full flag list. This page keeps the
common build, run, docs, format, test, and diagnostic loops.

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

Formatting changes layout. Audit modes report findings. Use `--apply` only
after reviewing the change class.

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
ny --borrow-check --ownership-strict file.ny
ny --heap=gc file.ny
ny --max-errors=20 file.ny
ny --warn=useful file.ny
ny --clean-cache
```

Compile-time type checks are on by default for typed code, generics, layouts,
and native boundaries. Suspicious dynamic fallbacks are warnings by default;
`--strict-types` rejects them for files that should stay fully statically
explainable. `--no-strict-types` is the legacy escape hatch when that stricter
layer was enabled by a wrapper or environment. `--safe-mode` adds
ownership/borrow checks, RC/RAII cleanup, strict effect/alias policy, and
raw-memory diagnostics. `--strict` adds
ownership/borrow diagnostics without the full safe-mode profile.

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

## Related

- [start.md](start.md)
- [diagnostics.md](diagnostics.md)
- [troubleshooting.md](troubleshooting.md)
- [testing.md](testing.md)
