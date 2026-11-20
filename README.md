<!-- nytrix-doc: {"audience":"user","featured":false,"group":"overview","order":0,"summary":"A statically typed native language with explicit machine boundaries."} -->
<div align="center">

<img src="etc/assets/website/logo.svg" alt="Nytrix" width="150">

# Nytrix

[![Version](https://img.shields.io/badge/version-0.11.0-2f6fed)](etc/docs/CHANGELOG.md)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Platforms](https://img.shields.io/badge/Linux%20%7C%20macOS%20%7C%20Windows-x86__64%20%7C%20arm64-lightgrey)](#install)

[Website](https://nytrix.x3ric.com/) · [Changelog](etc/docs/CHANGELOG.md)

</div>

Statically typed for native performance, comptime generation, and low-level control.

<!-- overview:quick-start -->

<!--
# Nytrix

<div class="quick-start-list">
<div><b><img class="ico ico-xs ico-wrench" src="assets/website/svg/wrench.svg" alt="">Build</b><code>./make</code></div>
<div><b><img class="ico ico-xs ico-play" src="assets/website/svg/play.svg" alt="">Run</b><code>ny hello.ny</code></div>
<div><b><img class="ico ico-xs ico-terminal" src="assets/website/svg/terminal.svg" alt="">REPL</b><code>ny</code></div>
<div><b><img class="ico ico-xs ico-check" src="assets/website/svg/check.svg" alt="">Format</b><code>ny fmt file.ny</code></div>
<div><b><img class="ico ico-xs ico-search" src="assets/website/svg/search.svg" alt="">Symbols</b><code>ny doc search --symbols print</code></div>
<div><b><img class="ico ico-xs ico-new" src="assets/website/svg/new.svg" alt="">New</b><code>ny new app</code></div>
</div>
-->

<!-- overview:body -->

<!-- overview:cards {"kind":"learn","title":"Learn","summary":"Build and run a program, then choose a focused workflow."} -->

<!-- overview:cards {"kind":"spec","title":"Specification","summary":"Exact language behavior, grammar, and diagnostics."} -->

<!-- overview:cards {"kind":"project","title":"Project","summary":"Contributing, open work, and release notes."} -->

<!-- overview:cards {"kind":"reference","title":"Reference","summary":"Source-linked standard-library entry points."} -->

## First program

```ny
use core

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
./make ny https://raw.githubusercontent.com/nytrix-lang/nytrix/refs/heads/main/etc/labs/xtool.ny
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
use core

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

ADT payloads use `Type name` in declarations and positional values in
constructors and match patterns.

### Compile-time

```ny
use core

def base = comptime{ 2 ** 5 }
def shifted = comptime{ range(4).map(fn(i){ i + base }) }

assert(base == 32, "comptime value")
assert(to_str(shifted) == "[32, 33, 34, 35]", "comptime list")
```

### Native ABI

```ny
use core

layout Vec2 pack(4) {
    f32 x,
    f32 y
}

#include <math.h>

assert(cos(0.0) == 1.0, "cos")
```

Layouts use `Type name` fields. A colon may still parse as an implicit
separator where the grammar is otherwise unambiguous.

Compiler contributors can start with the [contribution guide](etc/docs/CONTRIBUTE.md).

## Project layout

| Path            | Purpose                                              |
| --------------- | ---------------------------------------------------- |
| `src/`          | Compiler, runtime, native backend, and tools         |
| `core/`         | Standard library                                     |
| `etc/tests/`    | Tests, fixtures, and benchmarks                      |
| `etc/labs/` | Examples and larger projects                         |
| `etc/docs/`         | Manual, guides, specification, and API documentation |

## Status

Nytrix is actively evolving. Check the [status and execution routes](etc/docs/learn/status.md) and [language quick reference](etc/docs/learn/quirks.md) for current backend support and capabilities.

Pin a commit for reproducible builds and check the [Changelog](etc/docs/CHANGELOG.md) before upgrading.

## Community

Ask questions on [Discord](https://discord.gg/XQDR6DZWb), report bugs through
GitHub issues, or write to [nytrixlang@gmail.com](mailto:nytrixlang@gmail.com).

Send security reports privately by email with the affected commit, platform, command, and a proof of concept.
