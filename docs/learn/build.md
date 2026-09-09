<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":195,"summary":"Comprehensive build system guide: CMake options, LLVM/GMP/Z3 integration, sanitizer builds, and cross-compilation."} -->
# Build System & Toolchain Setup

Nytrix uses CMake for its build configuration, wrapped with an ergonomic Python `./make` driver at the repository root.

## Build Driver (`./make`)

The `./make` driver provides canonical commands for configuring, building, formatting, testing, and auditing the codebase:

```bash
./make --help          # Show all driver commands
./make env             # Display resolved environment and configuration
./make doctor          # Diagnose toolchain, dependencies, and system state
./make targets         # List all available build targets
./make bin             # Build Release binaries
./make tidy            # Format source, run linter, and check header integrity
./make audit           # Run static bug audit analysis
./make check           # Run full validation suite (build + audit + tests)
```

## CMake Options Matrix

| Flag | Values | Default | Description |
| :--- | :--- | :--- | :--- |
| `NYTRIX_USE_LLVM` | `ON`, `OFF` | `ON` (Unix), `ON` (Win) | LLVM backend support (required on Windows; optional on Unix for pure-native builds). |
| `NYTRIX_USE_GMP` | `ON`, `OFF` | `ON` (when available) | Use GNU MP for big-integer oracle validation; CMake disables it automatically when headers or the library are unavailable. |
| `NYTRIX_ENABLE_Z3` | `auto`, `on`, `off` | `auto` | Finite constraint solving in the compile-time proof engine. |
| `NYTRIX_FAST_BUILD` | `ON`, `OFF` | `OFF` | Enables host `-march=native` optimizations for faster local builds. |
| `NYTRIX_LINKER` | `system`, `lld`, `mold` | `system` | Selects the build linker explicitly; no optional fast linker is required. |
| `NYTRIX_RUNTIME_O3` | `ON`, `OFF` | `ON` | Compiles runtime hot paths with `-O3`. |
| `NYTRIX_LTO_MODE` | `off`, `thin`, `full` | `off` | Link-Time Optimization mode. |
| `NYTRIX_PGO_MODE` | `off`, `gen`, `use` | `off` | Profile-Guided Optimization mode. |
| `NYTRIX_PGO_PROFILE` | `<path>` | `""` | Path to profile data file for PGO. |
| `NYTRIX_ICF_MODE` | `off`, `safe`, `all` | `off` | Identical Code Folding linker mode. |

## LLVM Discovery

CMake searches for `llvm-config` binaries from version 22 down to 16 automatically. To use a specific version or custom installation:

```bash
export LLVM_CONFIG=/usr/bin/llvm-config-21
export NYTRIX_LLVM_INCLUDE=/usr/lib/llvm21/include
```

On Unix systems, you can build without LLVM for pure-native execution:
```bash
cmake -S . -B build/release -DNYTRIX_USE_LLVM=OFF
cmake --build build/release -j"$(nproc)"
```

## Dedicated Sanitizer Build Trees

Always build sanitizers in dedicated debug build directories to keep instrumented objects isolated:

### AddressSanitizer + UndefinedBehaviorSanitizer (ASan + UBSan)
```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build/asan -j"$(nproc)"

# Run focused test fixture under ASan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  build/asan/ny-test --bin build/asan/ny_debug --failures-only --pattern=<fixture>

# Run reproducer directly
build/asan/ny_debug path/to/reproducer.ny
```

### ThreadSanitizer (TSan)
```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
cmake --build build/tsan -j"$(nproc)"
```

### MemorySanitizer (MSan - Clang Only)
```bash
CC=clang cmake -S . -B build/msan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=memory -fno-omit-frame-pointer -g"
cmake --build build/msan -j"$(nproc)"
```

### Valgrind Memory Validation
```bash
./make bin
valgrind --leak-check=full --show-leak-kinds=definite \
  --errors-for-leak-kinds=definite --error-exitcode=97 \
  build/release/ny-full path/to/reproducer.ny
```

## Release and deployment

Use a separate build directory for every toolchain or sanitizer configuration. The
install prefix is embedded into the generated runtime lookup paths, so choose the
final prefix before configuring the release build:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DNYTRIX_USE_LLVM=ON -DNYTRIX_LINKER=system
cmake --build build/release --target ny-full ny-test -j"$(nproc)"
cmake --install build/release

# Verify the installed compiler and standard library
"$HOME/.local/bin/ny" --version
NYTRIX_STD_CACHE=0 "$HOME/.local/bin/ny" etc/projects/os/110.ny
```

For a pure-native deployment on Unix systems, disable LLVM at configure time:

```bash
cmake -S . -B build/native-release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DNYTRIX_USE_LLVM=OFF
cmake --build build/native-release -j"$(nproc)"
cmake --install build/native-release
```

Before packaging a release, run the project smoke suite from the repository root:

```bash
for f in etc/projects/os/*.ny; do
  echo "== $f"
  env NYTRIX_STD_CACHE=0 timeout 60s build/release/ny-full --native-only "$f" || exit $?
done
```

The internal native linker is still experimental. Production deployments should
use the default system linker; `NYTRIX_LINKER=lld` or `NYTRIX_LINKER=mold` are
optional build-time accelerators, not runtime dependencies.

## Common Build Issues & Troubleshooting

| Issue / Symptom | Root Cause | Solution |
| :--- | :--- | :--- |
| `LLVM headers not found` | Missing development headers | Install `llvm-dev` / `libclang-dev`, or set `NYTRIX_LLVM_INCLUDE`. |
| `libclang not found` | Missing Clang library | Install `libclang-dev`, or set `NYTRIX_CLANG_LIBRARY`. |
| `GMP not found` | Optional big-integer oracle missing | Install `libgmp-dev`, or configure with `-DNYTRIX_USE_GMP=OFF`. |
| `Z3 not found` | Optional constraint solver missing | Install `libz3-dev`, or configure with `-DNYTRIX_ENABLE_Z3=off`. |
| Cross-compiling Windows | Missing Windows SDK headers/libs | Export `NYTRIX_WINSDK_CFLAGS` and `NYTRIX_WINSDK_LDFLAGS`. |
| Slow build turnaround | Default linker or baseline flags | Enable `-DNYTRIX_FAST_BUILD=ON`; optionally configure `-DNYTRIX_LINKER=lld` or `mold`. |
