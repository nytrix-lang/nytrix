# Nytrix

> *Status*: Work in progress expect breaking changes.

## Focus

- Simplicity.
- Low-level.
- C include.
- Batteries.

## Requirements

- LLVM >= 16
- Python 3

Targets: Linux, macOS, and Windows on x86_64 or arm64.

## Build

```bash
chmod +x make
./make
```

or `python3 ./make`.

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
