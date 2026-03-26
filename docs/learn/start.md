# Start

Run one file and import one module before moving to package setup, native
linking, docs search, or project layout. The checks below cover the compiler,
standard library, and assertions with no extra setup beyond a working `ny`.

## Verify the tool

```bash
ny --version
ny -c '1 + 1'
```

From a source checkout, build and install only when `ny` is missing or stale:

```bash
chmod +x make
./make all
./make install
ny --version
```

## First file

Put this in `hello.ny`:

```ny
use std.core

fn greet(str: name): str {
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

`fmt --check` verifies source layout. `--strict-types` catches dynamic type
cliffs before they become runtime surprises. `--strict` enables strict types
and ownership diagnostics together. Borrow checking is most useful once a file
owns resources, returns references, or wraps native handles.

## Next

| If you need | Go to |
| --- | --- |
| A script, module, or import shape | [programs.md](programs.md) |
| Copyable examples | [examples.md](examples.md) |
| Standard-library APIs and parsers | [library.md](library.md) |
| Command reference | [tooling.md](tooling.md) |
| Windows or drawing | [ui.md](ui.md) |
| Failure diagnosis | [troubleshooting.md](troubleshooting.md) |
