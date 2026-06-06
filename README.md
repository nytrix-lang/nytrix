# Nytrix

Explicit imports. Native binaries. Compile-time type checks. Cross-platform stdlib. Direct C ABI.

> *Status*: Work in progress expect breaking changes.

## Focus

- Productivity.
- Performance.
- Simplicity.
- Iteration.

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
