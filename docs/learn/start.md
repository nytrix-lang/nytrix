# Start

Run one file and import one module before moving to package setup, native
linking, docs search, or project layout. The checks below cover the compiler,
standard library, and assertions with no extra setup beyond a working `ny`.

## Verify the tool

```bash
ny --version
ny -c 'print(1 + 1)'
```

The inline check prints `2`.

From a source checkout, build and install only when `ny` is missing or stale:

```bash
chmod +x make
./make doctor
./make all
./make install
ny --version
```

`./make doctor` is read-only. It reports missing build tools, unwritable cache
directories, optional qemu/wine runners, and UI display state before a build or
runtime command hides the real cause.

## First file

Put this in `hello.ny`:

```ny
use std.core

fn greet(str name) str {
   "hello, " + name
}

assert_eq(greet("ny"), "hello, ny", "greet")
```

Run the file:

```bash
ny --color=never hello.ny
```

No output means the assertion passed. A failure prints the assertion label and
the source location. Keep assertion labels short and behavior-focused; they are
what you search for when a check fails later.

## Add one import

```ny
use std.core
use std.parse.data.json as json

def cfg = json.json_decode("{\"name\":\"ny\",\"ports\":[8080,8081]}")
assert_eq(cfg.get("name", ""), "ny", "name")
assert_eq(cfg.get("ports", [])[0], 8080, "first port")
```

## Search before guessing

After the tool is built, `ny doc` is the local API index:

```bash
ny doc search json
ny doc search --symbols recvuntil
ny doc get std.parse.data.json
```

Use `search` for names and topics. Use `get` once you know the module or
symbol you want.

## Check the file

```bash
ny fmt --check hello.ny
ny --strict hello.ny
ny --strict-types hello.ny
ny --borrow-check --ownership-strict hello.ny
```

`fmt --check` verifies source layout. Compile-time type checks are on by
default for typed code, generics, layouts, and native boundaries.
`--strict-types` turns suspicious dynamic fallbacks into errors for files that
should stay fully statically explainable. `--strict` keeps type checks on and
adds ownership diagnostics. Borrow checking is most useful once a file owns
resources, returns references, or wraps native handles.

## Native Output And Cross Targets

Build a native executable with `-o`:

```bash
ny -o hello hello.ny
./hello
```

From a checkout, `./make cross` gives the same compiler a target triple and the
matching C/linker flags:

```bash
./make targets
./make cross linux-arm64 hello.ny
./make cross-run linux-arm64 hello.ny
```

`cross-run` uses qemu or wine when present and otherwise leaves the compiled
artifact in `build/cache/cross/`. See [tooling.md](tooling.md) for sysroot,
qemu, and custom target flags.

## Next

| If you need | Go to |
| --- | --- |
| A script, module, or import shape | [programs.md](programs.md) |
| Copyable examples | [programs.md](programs.md#complete-project-examples) |
| Standard-library APIs and parsers | [library.md](library.md) |
| Command reference | [tooling.md](tooling.md) |
| Windows or drawing | [ui.md](ui.md) |
| Failure diagnosis | [troubleshooting.md](troubleshooting.md) |
