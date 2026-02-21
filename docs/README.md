
# Nytrix

Nytrix is a compact systems language with explicit imports and a small runtime core.

> Status: work in progress. Expect breaking changes.

* **Compiled**
* Control > Convenience
* Simple Semantics
* Minimal Core

**Dependencies**
- LLVM 21+
- Python 3

**Arch**
- x86_x64
- arm64

**Os**
- Linux
- MacOS
- Windows

### Build

```bash
chmod +x make
./make
```

or `python3 ./make`

### Install

```bash
./make install
```

### Usage

```bash
ny -i
ny etc/tests/matrix.ny
ny -c "print('Hello Ny!')" -o && ./a.out
```

If `ny` is not installed system-wide, use `./build/release/ny`.

### Docs

```bash
./make docs
```

## License

See `LICENSE`.
