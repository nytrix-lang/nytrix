
# Nytrix

Nytrix is a **minimalist systems language** with a tiny 64-bit core and everything else written in `.ny`. It targets explicit control, predictable performance, and zero-cost abstractions, backed by LLVM.

## Core Model

* **Everything in std in Ny**: stdlib, collections, helpers
* **Opt-in stdlib**: zero overhead unless used
* **LLVM backend**: JIT and AOT via MCJIT

## Build

### Requirements

* LLVM 21+
* readline
* python3
* clang
* LibC

For now, Linux only.
Tested on Arch Linux x86_64.

### Build compiler

```bash
make -j$(nproc)
```

## Usage

### Repl

```bash
./build/ny -i
```

Just in time (JIT) Compiled.

### Run

```bash
./build/ny file.ny
```

Ahead of time (AOT) Compiled.

### Binary

```bash
ny -c "print('Hello ELF!')" -o && chmod +x a.out && ./a.out
Success: Saved ELF: a.out
Hello ELF!
```

### Install

```bash
sudo make install
```

## Project Status

> Note: Everything is work in progress. No stable state yet. The standard library is unfinished. This is a mockup.

Expect breaking changes.

## Philosophy

* Explicit control over convenience
* Minimal core, maximal clarity
* Opt-in features only

Nytrix aims to stay small, fast, and extensible/moddable. Not there yet.

## License

See `LICENSE`.
