<div align="center">

<img src="etc/assets/website/logo.svg" alt="Nytrix" width="150">

# Nytrix

**Think freely. Control precisely.**

Native · Explicit · Ownership · Interop · Paradigms

[![Version](https://img.shields.io/badge/version-0.10.0-2f6fed)](docs/CHANGELOG.md)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Platforms](https://img.shields.io/badge/Linux%20%7C%20macOS%20%7C%20Windows-x86__64%20%7C%20arm64-lightgrey)](#install)

[Website](https://nytrix.x3ric.com/) · [Manual](docs/README.md) · [Changelog](docs/CHANGELOG.md)

</div>

Nytrix is a statically typed native language that keeps high-level abstractions transparent, guarantees checked, costs visible, and the machine within reach.

See the [Nytrix Manual](docs/README.md) for guides, specification, and API reference.

## First program

```ny
use std.core

fn greet(str name) str {
    "Hello, " + name + "!"
}

print(greet("Nytrix"))
assert(greet("Nytrix") == "Hello, Nytrix!", "greet")
```

Run it:

```bash
./make ny -run hello.ny
```

Or build a native executable:

```bash
./make ny -o hello hello.ny
./hello
```

Source can also be run directly from a URL:

```bash
./make ny https://raw.githubusercontent.com/x3ric/xtool/refs/heads/main/xtool
```

## Install

```bash
chmod +x make
./make
./make install
ny --version
```

If `./make` cannot be executed directly:

```bash
python3 ./make
```

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

### Compile-time

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

#include <math.h>

assert(cos(0.0) == 1.0, "cos")
```

Layouts use `Type name` fields. A colon may still parse as an implicit separator where the grammar is otherwise unambiguous.

## Project layout

| Path            | Purpose                                              |
| --------------- | ---------------------------------------------------- |
| `src/`          | Compiler, runtime, native backend, and tools         |
| `lib/`          | Standard library                                     |
| `etc/tests/`    | Tests, fixtures, and benchmarks                      |
| `etc/projects/` | Examples and larger projects                         |
| `docs/`         | Manual, guides, specification, and API documentation |

## Status

> Work in progress. The compiler is still experimental. The native backend is being developed toward a full JIT/AOT path without LLVM.

Pin a commit for reproducible builds and check the [Changelog](docs/CHANGELOG.md) before upgrading.

## Community

Use [Discord](https://discord.gg/XQDR6DZWb), GitHub issues, or [nytrixlang@gmail.com](mailto:nytrixlang@gmail.com) for questions, bug reports, ideas, and documentation feedback.

Send security reports privately by email with the affected commit, platform, command, and a minimal proof of concept.

Feature direction can be discussed on Discord or by email.

## License

MIT. See [LICENSE](LICENSE).
