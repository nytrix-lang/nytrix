<div align="center">
  <img src="etc/assets/website/logo.svg" alt="Nytrix" width="150">

  # Nytrix

  <strong>A systems programming language.</strong>

  [![Version](https://img.shields.io/badge/version-0.5.0-2f6fed)](docs/CHANGELOG.md)
  [![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
  [![LLVM](https://img.shields.io/badge/LLVM-%3E%3D16-orange)](#install)
  [![Platforms](https://img.shields.io/badge/Linux%20%7C%20macOS%20%7C%20Windows-x86__64%20%7C%20arm64-lightgrey)](#install)

  [Install](#install) |
  [Language](#language) |
  [Docs](docs/README.md) |
  [Learn](docs/learn/start.md) |
  [Spec](docs/spec) |
  [Examples](docs/learn/examples.md) |
  [Changelog](docs/CHANGELOG.md) |
  [Website](https://nytrix.x3ric.com/)
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
    Circle(int: radius),
    Empty
}

fn area(shape) int {
    match shape {
        Shape.Circle(radius: r) -> r * r
        Shape.Empty -> 0
    }
}

assert(area(Shape.Circle(radius: 4)) == 16, "area")
```

### Comptime

```ny
use std.core

def base = comptime{ 2^5 }
def shifted = comptime{ range(4).map(fn(i) { i + base }) }

assert(base == 32, "comptime value")
assert(to_str(shifted) == "[32, 33, 34, 35]", "comptime list")
```

### Native ABI

```ny
use std.core

#include <math.h> as "cos"

assert(cos(0.0) == 1.0, "cos")
```

## Project

`src/` holds the compiler, runtime, and tools. `lib/` holds the standard
library. `etc/tests/` holds tests. `etc/projects/` holds examples and demos.

The stdlib covers core data, OS APIs, networking, parsers, math, crypto, UI,
audio, rendering, and editor support.

## Community

Use [Discord](https://discord.gg/XQDR6DZWb), GitHub issues, or
[nytrixlang@gmail.com](mailto:nytrixlang@gmail.com) for questions, bugs, docs,
and feedback.

Send security reports privately by email with the affected commit, platform,
command, proof of concept, and impact. Reports are voluntary; there is no paid
bounty program right now.

Discuss feature direction on Discord or by email before opening a PR for syntax,
compiler, runtime, stdlib, CLI, or docs changes. Keep fixes focused, update
tests or docs, include commands run, and use prefixes such as `fix:`, `docs:`,
`feat:`, `test:`, `perf:`, `refactor:`, `assets:`, `build:`.

## Status

Expect breaking changes. Pin a commit and check
[docs/CHANGELOG.md](docs/CHANGELOG.md) before upgrading.

## License

MIT. See [LICENSE](LICENSE).
