<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":170,"summary":"Use the compiler, formatter, test runner, documentation search, and target checks."} -->
# Tooling

Use `./make` for repository builds and test suites; use `ny` for source-level
commands. Start with these help commands when you need the installed options.

Prerequisites: an installed Nytrix compiler; `./make` commands also require the
repository checkout.

Skip the sections for workflows you do not use. [Start](start.md) covers the
minimal compile-and-run path.

```bash
./make --help
ny --help
ny --help all
ny --help short
ny --help emit
ny doc --help
```

`ny --help` is the short overview. The full canonical flag list, accepted
values, and option groups come from the registry via `ny --help all`; focused
references use `ny --help TOPIC`. `ny --help short` documents global
shortcuts and option-family clusters.

## CLI configuration

Nytrix reads options in this order: built-in defaults, the user file at
`~/.config/nytrix/ny.toml`, the project file `./ny.toml`,
`NYTRIX_FLAGS` (or `NY_FLAGS` when the primary variable is unset or empty), and
then explicit CLI arguments. `-n` / `--print-flags=json` shows canonical
arguments, per-source provenance, and the program-argument boundary.

These files use a limited TOML-compatible syntax: `[options]` (or `[flags]`)
uses option names without dashes; `[alias]` is supported only in the user file.
For example:

```toml
[options]
run = "none"
opt-level = 2

[alias]
dbg = "-Dnv -Ea -vv"
```

Use `ny +dbg file.ny` to expand a user alias. Built-in presets cannot be
overridden by aliases. Project configuration cannot set registry-marked
code-execution or path-injection flags unless `--trust-config` appears on the
CLI before the program input. `NYTRIX_FLAGS` and `NY_FLAGS` are quoted
option-only argument lists; they cannot carry program arguments or `--`.

## Run and inspect

```bash
ny file.ny
ny -c 'print(1 + 1)'
ny --run=aot file.ny
ny --output=app file.ny
ny --native-only file.ny
```

These commands exercise different paths. See [Execution routes](status.md#execution-routes)
before treating a successful run as evidence for another backend.
Successful executable, object, bitcode, and assembly saves report the output
path and size; larger files include both KB/MB and the exact byte count.
Ordinary compilation defaults to `-O2` with size-oriented native code
generation. Use `-O0` to disable optimization explicitly; the interactive REPL
keeps its unoptimized startup behavior.

For a compact ELF/Linux executable with host LTO and stripping, use:

```bash
ny --release --opt=lto --output=app file.ny
```

On ELF/Linux, `--release` selects size-oriented native code generation,
compiles the Nytrix runtime with `-Os`, omits its frame pointers and unwind
tables, requests section garbage collection at link time, disables the build
ID, and strips the result. `-flto` also enables host LTO for runtime
compilation and the final link on non-Windows hosts. LTO can increase compile
time and does not remove the CRT or libc. Size-optimized ELF links also remove
the remaining `.eh_frame`, `.eh_frame_hdr`, `.sframe`, and `.comment` sections;
this disables native stack unwinding/debug frame inspection for those outputs.

For AOT output, `--mode=bare` supports inline source and source files on Linux
x86-64 and AArch64. String and signed-integer `print` calls encode `write` syscalls
directly into the bare ELF and link without a host assembler; simple constant
`print`/`eprint`/no-op programs use direct syscalls, and dependency-free native
programs use the in-process ELF linker and encoded startup. Native signed
integer output formats values directly into a stack buffer; typed-buffer
allocation, length, signed index reads, and typed `pop` also use direct
machine-code lowering in supported x86-64 bare programs. Raw `memcpy`,
`memset`, and `memcmp` also use direct byte loops. Invalid typed-list
indexes exit with status 1 rather than entering libc bounds handling. Other runtime-backed x86-64 programs can still use an
assembled syscall-only startup and runtime shim. On Linux AArch64,
constant-output programs use direct `write` syscalls, and dependency-free NyR
programs use the in-process ELF linker with an AArch64 entry stub.
Runtime-backed AArch64 code can also print signed
64-bit integers and constant booleans, using per-function stack scratch and
direct `write` syscalls, and `ticks()` uses the `clock_gettime` syscall. Bare
output has no CRT, libc, or interpreter.
Integer/boolean conversion, string/integer/boolean `print`/`eprint` output,
monotonic nanosecond ticks, typed buffers, and basic `malloc`/`free` use
syscall shims backed by anonymous `mmap` on x86-64; allocations are
zero-filled. AArch64 additionally supports signed integer and constant-boolean
formatting, `ticks()`, and basic `malloc`/`free` via `mmap`/`munmap`, alongside
constant output, dependency-free NyR programs, and typed `i64buf_new`
allocation with direct memory loads/stores. Bare AArch64 `print` borrows static
C-string literals directly, without constructing managed strings. Bare AArch64
`eprint` supports
string, integer, and boolean arguments; its C-string length and fd-write helpers
are emitted as Linux syscalls. Raw AArch64 byte copy, fill, and compare helpers
are emitted directly. Numeric list literals can be
cloned into bare mappings, with positive/negative scalar index reads without
the managed handle registry. Native-list length and raw `pop` lower to shared
NYIR memory/control-flow operations; pop updates the count and returns the
removed value. An invalid index exits with status 1. Dynamic-list operations
that still require managed runtime helpers remain unsupported in bare mode.
Among managed-string operations, bare mode supports C-string conversion,
concatenation, equality, comparison, `str.chr`, and `str.repeat`. Floating-point
formatting, other runtime helpers, external libraries, and target/OS
combinations without a bare adapter remain unsupported. Other execution routes
retain the bare runtime policy and are not freestanding.

## NYR instruction listing

Use `--vm-disasm` to inspect NYR instructions produced for a source program or
loaded from an NYR bundle. Function names are sorted; each instruction line
includes its operation and available ABI/source annotations.

```bash
ny --vm-disasm file.ny
ny --vm-disasm=helper file.ny
ny --mode=bare --vm-disasm file.ny
```

The optional filter matches part of a function name. This is an instruction
listing, not native machine-code disassembly or a trace of the branches taken
at runtime.

For source inspection without execution, use `ny --dump-ast file.ny` or
`ny --dump-ast=typed,filter=name file.ny`.
The [debugging guide](debugging.md#ast-inspection) explains the tree and checks.

## Format and test

```bash
ny fmt --check file.ny
ny fmt --fix file.ny
./make test --pattern path-or-name
./make web-test
```

`--check` is read-only; `--fix` writes formatting changes. Use the narrowest
fixture pattern that covers the behavior, then run broader tests as required by
the change. For C and header files, `--check` reports missing file-purpose
comments and continued-macro backslashes that are not column-aligned;
`--fix` aligns those backslashes. `ny fmt --align-macros src` applies only the
macro alignment pass to C and header files under `src`. See
[Testing](testing.md).

## Find API documentation

```bash
ny doc search --symbols function_name
ny doc search --docs topic
ny doc get module.name
```

The results depend on the checked-out source and its generated docs. See
[Library](library.md) for a search workflow.

In the generated website, **Contents** links open any containing disclosure
before scrolling to a heading. Long outlines scroll within their own panel.
Release highlights are visible in the [changelog](../CHANGELOG.md); each
version's **Details** contains its development record.

The sidebar separates folder groups from page links with disclosure arrows,
indentation, and tree guides. Click an API namespace or module name to open its
page; use its arrow to expand children without navigating. Folder and arrow
buttons support Enter and Space. The selected page stays visible in the tree.
Typing outside interactive controls starts a search; focused navigation
controls keep their normal keyboard behavior.

## Editor navigation

`ny-lsp` provides symbols, definitions, references, rename, and
`textDocument/prepareRename`. Reference/rename discovery excludes line comments
and quoted strings, avoiding edits to those lexical lookalikes. Clients can
preview the selected name and range before applying a workspace edit. This does
not yet provide interactive move-declaration or change-signature refactors.

## Diagnose a failure

First rerun the failing command without changing its route or flags. Then
separate parse/type errors from runtime, linker, and host-capability failures.
Use `ny --help` for diagnostics and NYIR options; use
[Troubleshooting](troubleshooting.md) to identify the next owner to inspect.

Next:

- [Build](build.md) — build and install the checkout.
- [Testing](testing.md) — run a focused fixture.
- [Troubleshooting](troubleshooting.md) — follow a failure to its owner.
