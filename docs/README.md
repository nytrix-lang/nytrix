
# Nytrix

Nytrix **Language**.

> Note: WIP expect breaking changes.

## Philosophy

Nytrix aims to stay small and extensible. Not there yet.

* **Compiled**
* Control > Convenience
* Simple Semantics
* Minimal Core
* Scripting

## Build

### Requirements

* LLVM 21+
* readline
* python3
* clang
* LibC

For now, Linux only.
Tested on Arch Linux x86_64.

### Install

```bash
sudo make install
```

## Usage

### Repl

```bash
ny -i
```

### Run

```bash
ny etc/examples/matrix.ny
```

### Binary

```bash
ny -c "print('Hello NyELF!')" -o && ./a.out
```

### Docs

Generate a static local documentation site.

```bash
make docs
xdg-open build/docs/index.html >/dev/null 2>&1
```

## License

See `LICENSE`.
