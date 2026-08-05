# Contributing to Nytrix

This guide contains the repository conventions and engineering evidence that
are useful to every contributor. Private automation policy stays in the local
agent setup.

## Source and structure

Keep code in the layer that owns its behavior:

- `src/` contains the compiler, runtime, native pipeline, and tools.
- `lib/` contains the standard library.
- `etc/tests/` contains executable fixtures, shape corpora, and benchmarks.
- `etc/projects/` contains examples and applications.
- `docs/spec/` defines language behavior; `docs/learn/` explains workflows.

Use the established module header: one `;; Keywords: ...` line first,
optional concise `;;` purpose lines next, then `module`. Prefer an existing
utility or facade over a parallel helper. Keep names descriptive, paths
role-based, and patches narrow.

## Evidence and correctness

Native support is proven only by the intended path and an executable oracle.
A skipped fixture, cache, emitted assembly file, interpreter fallback, LLVM
fallback, or host compiler is not proof of native execution.

Keep compile-time safety separate from runtime safety. Unsupported language,
ABI, renderer, and browser shapes must fail with an actionable diagnostic;
they must not silently fall back. Do not add an `unsafe` escape hatch to cover
a missing compiler feature.

For native changes, follow parsing, semantic analysis, lowering, NYIR, machine
form, allocation, object generation, linking, and runtime behavior. Typed
native `i64` slots contain raw values; dynamic `NyValue` containers retain
their tagged representation. Keep `NYTRIX_USE_GMP` optional and preserve the
documented BigInt layout. Preserve diagnostic locations, categories, and
actionable messages.

## Libraries and tools

Keep one canonical standard-library behavior. Preserve ownership and
mutability, rebuild the standard bundle after library edits, and test direct
calls plus normal composition.

Extend `c2ny` or `py2ny` for source conversion. Unsupported constructs need an
explicit compilable marker and clear diagnostic, never a silent drop.

Renderer fixes belong in shared renderer code. Check resource lifetime, failure
propagation, headless behavior, and bounded framebuffer or artifact probes.

The runtime amalgamation starts at `src/rt/init.c`; CMake tracks its included
sources. Do not edit generated `build/release/std.ny`, commit generated
shared libraries, or change vendored dependencies without a demonstrated
defect.

## Validation

Start with the smallest relevant check, rerun the original reproducer, then
widen validation according to risk:

```bash
./make bin
./make test --with-stdlib --failures-only --color=never
./make all
./make web-test
./make bench
./make perf
git diff --check
```

`web-test` is the real browser path. It can use Chromium-family browsers by
default or a bundled Playwright Firefox with `NYTRIX_BROWSER=firefox`.
Report the browser, exact command, expected marker, observed result, and
implementation path. State environmental limits separately.

## Performance and high-risk changes

Measure before optimizing. Keep benchmark inputs, compiler flags, cache mode,
sample count, and execution path fixed. Compare C, Nytrix, and optional GMP
variants only when they perform the same work and report correctness markers.

Changes under `src/code/native/`, `src/base/options.c`, or `src/wire/pipe/`
need focused regression coverage. For native work, report the exact command,
marker, observed result, and implementation path it exercised.
