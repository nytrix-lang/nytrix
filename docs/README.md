
# Nytrix

> Status: work in progress. Expect breaking changes.

Nytrix is a multiplatform systems language focused on static typing, native execution, and a small standard library surface.

* Statically typed, compiled language
* Performance
* Simplicity
* MacroFree
* LowLevel

**Dependencies**
- LLVM 21+
- Python 3

**Arch**
- x86_x64
- arm64

**Os**
- Windows
- MacOS
- Linux

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
