# Start

Run one file and import one module before package setup, native linking, docs
search, or project layout. These checks need only a working `ny`.

## Verify the tool

```bash
ny --version
ny -c 'print(1 + 1)'
```

The inline check prints `2`.

From a source checkout, build and install when `ny` is missing or stale:

```bash
chmod +x make
./make doctor
./make all
./make install
ny --version
```

`./make doctor` reads the machine and changes nothing. It reports missing build
tools, unwritable cache directories, optional qemu/wine runners, and UI display
state before a build or runtime command hides the cause.

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
source location. Keep labels short; you will search for them later.

## Add one import

```ny
use std.core
use std.parse.data.json as json

def cfg = json.json_decode("{\"name\":\"ny\",\"ports\":[8080,8081]}")
assert_eq(cfg.get("name", ""), "ny", "name")
assert_eq(cfg.get("ports", [])[0], 8080, "first port")
```

## Search before guessing

After the build, `ny doc` gives you the local API index:

```bash
ny doc search json
ny doc search --symbols recvuntil
ny doc get std.parse.data.json
```

Use `search` for names and topics. Use `get` after you know the module or
symbol.

## First List

`list(n)` reserves capacity and creates zero elements.

```ny
mut xs = list(4)
xs = xs.append("a")
assert_eq(xs[0], "a", "first item")
```

Use a literal for initialized elements:

```ny
def ys = [0, 0, 0, 0]
```

## Check the file

```bash
ny fmt --check hello.ny
ny --strict hello.ny
ny --strict-types hello.ny
ny --borrow-check --ownership-strict hello.ny
```

`fmt --check` verifies source layout. The compiler checks typed code, generics,
layouts, and native boundaries by default. `--strict-types` rejects dynamic
fallbacks in files that should stay statically explainable. `--strict` adds
ownership diagnostics. Borrow checking helps once a file owns resources,
returns references, or wraps native handles.

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
