<div align="center">
  <img src="etc/assets/website/logo.svg" alt="Nytrix" width="150">

  # Nytrix

  <strong>Programming language.</strong>

  [![Version](https://img.shields.io/badge/version-0.6.0-2f6fed)](docs/CHANGELOG.md)
  [![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
  [![Platforms](https://img.shields.io/badge/Linux%20%7C%20macOS%20%7C%20Windows-x86__64%20%7C%20arm64-lightgrey)](#install)

  [Website](https://nytrix.x3ric.com/) · [Changelog](https://nytrix.x3ric.com/#CHANGELOG)
</div>

```ny
use std.core
fn greet(str name) str {
    "Hello, " + name + "!"
}
print(greet("Nytrix"))
assert(greet("Nytrix") == "Hello, Nytrix!", "greet")
```

```bash
./make ny -run hello.ny
./make ny -o hello hello.ny
./hello
./make ny https://raw.githubusercontent.com/x3ric/xtool/refs/heads/main/xtool
```

## Install

```bash
chmod +x make
./make
./make install
ny --version
```

Use `python3 ./make` if your shell does not execute `./make` directly.

## Language

### Data

```ny
use std.core
enum Shape {
    Circle(int radius),
    Empty
}
fn area(shape) int {
    match shape {
        Shape.Circle(r) -> r * r
        Shape.Empty -> 0
    }
}
assert(area(Shape.Circle(4)) == 16, "area")
```

ADT payloads use `Type name` in declarations and positional values in constructors and match patterns.

### Comptime

```ny
use std.core
def base = comptime{ 2^5 }
def shifted = comptime{ range(4).map(fn(i){ i + base }) }
assert(base == 32, "comptime value")
assert(to_str(shifted) == "[32, 33, 34, 35]", "comptime list")
```

### Native ABI

```ny
use std.core
layout Vec2 pack(4) {
    f32 x,
    f32 y
}
#include <math.h> as "cos"
assert(cos(0.0) == 1.0, "cos")
```

Layouts use `Type name` fields. A colon may still parse as an implicit separator only where the grammar would otherwise be unambiguous.

## Project

| Path            | Purpose                       |
| --------------- | ----------------------------- |
| `src/`          | Compiler, runtime, and tools. |
| `lib/`          | Standard library.             |
| `etc/tests/`    | Tests.                        |
| `etc/projects/` | Examples and demos.           |

## Community

Use [Discord](https://discord.gg/XQDR6DZWb), GitHub issues, or
[nytrixlang@gmail.com](mailto:nytrixlang@gmail.com) for questions, bugs, docs,
and feedback.

Send security reports privately by email with the affected commit, platform,
command, proof of concept. Reports are voluntary; there is no paid
bounty program right now `;(`.

Discuss feature direction on Discord or by email.

## Status

Pin a commit for reproducible builds and check
[docs/CHANGELOG.md](docs/CHANGELOG.md) before upgrading.

## License

MIT. See [LICENSE](LICENSE).
