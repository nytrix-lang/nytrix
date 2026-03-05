# Nytrix

Language for native code, explicit imports,
and direct C/ABI interop.

> *Status*: Work in progress expect breaking changes.

## Focus

- Simplicity.
- Low-level.
- Batteries.
- C include.

## Requirements

- CMake and a C compiler
- Python 3 'bootstrap'
- LLVM 16 through 22

Targets: Linux, macOS, and Windows on x86_64 or arm64.

## Build

```bash
chmod +x make
./make
```

or `python3 ./make`

## Install

```bash
./make install
```

## Use

```bash
ny -c "print('Hello Ny!')" -o && ./a.out
```

If `ny` is not installed system-wide, use `./build/release/ny` after `./make`.

## Docs

```bash
./make docs
```

## License

See `LICENSE`.
