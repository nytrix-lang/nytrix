<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":10,"summary":"Compile and run a first Nytrix file, then find the next guide for your task."} -->
# Start

Here's your first program. Under a minute.

Prerequisites: a Nytrix compiler on `PATH`, or a source checkout with its build
tools installed.

Create `hello.ny`:

```ny
use core

print("hello, world")
```

Run it:

```bash
ny hello.ny
```

The program prints `hello, world`. If you are working from a source checkout
instead of an installed compiler, build it first:

```bash
./make
./make install
```

Next:

- [Tour](tour.md) — read a small program with functions, cases, and loops.
- [Programs](programs.md) — organize scripts, modules, and imports.
- [Build](build.md) — build from source or diagnose a toolchain.
