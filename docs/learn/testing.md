# Testing

A Nytrix check is an executable source file that asserts behavior and exits
non-zero on failure.

Tests are ordinary programs. That keeps examples, regressions, and integration
checks close to the language users run outside the test harness.

## Minimal check

```ny
use std.core
use std.core.iter as it

def xs = [1, 2, 3, 4]
assert_eq(it.filter(xs, fn(v){ (v % 2) == 0 }), [2, 4], "even values")
assert_eq(it.reduce(xs, 0, fn(acc, v){ acc + v }), 10, "sum")
```

## Commands

```bash
ny --color=never path/to/check.ny
ny fmt --trim --check path/to/file.ny
ny test --pattern name
ny test --with-stdlib module-or-path
```

`.nshape` checks may use `flags_matrix` when the same source must be compiled
through several native backends. Rows are separated by `;` or escaped newlines,
and each row is appended to the normal `flags` for one focused harness run.

## Native optimizer stress

Native optimizer regressions use a small, deterministic source program and an
optimization-level matrix. Each row runs the NYIR VM and the selected native
backend through `--native-result-oracle`; a result mismatch is a failure. Keep
the fixture target-specific and focused on one control-flow or lowering shape,
then cover broader input variation through the existing `shapes/` fuzz corpus.
This gives every fixed optimizer bug a reproducible regression while keeping
generated stress separate from correctness baselines.

```bash
LD_LIBRARY_PATH=build/vendor/lib/host \
  ./build/release/ny-test --jobs 8 --failures-only \
  etc/tests/native/oracle/*optimizer*levels*x86-64.nshape
```

## Check classes

| Class | Scope |
| --- | --- |
| Value | Pure function, parser, encoder, or transform returns an exact value. |
| Boundary | File, process, socket, FFI call, or native handle crosses runtime boundaries. |
| Regression | A fixed bug has a small reproducer. |
| Integration | Multiple modules cooperate. |
| Visual | UI, image, font, renderer, or scene behavior changed. |
| Performance | Timing, allocation, throughput, or generated-code behavior changed. |

## Repository suites

The repository keeps test inputs with the subsystem that owns their behavior:

```text
etc/tests/
  runtime/   ordinary Nytrix behavior, grouped by language/runtime concern
  errors/    expected parser, type, ownership, and safety diagnostics
  native/    NYIR, backend, object, linker, oracle, and sanitizer checks
  interop/   C headers, FFI fixtures, and Nytrix interop entry programs
  bench/     checksum-producing compile and throughput workloads
  shapes/    fuzz generators, probes, stress inputs, and compiler regressions
```

Run a `.ny` file directly. Run a `.nshape` specification through `ny-test`;
it is harness metadata, not source code for `ny` itself.
`ny-test` executes the repository tree as one deterministic run while reporting
Runtime, Native, and Interop ownership separately.

```bash
./make test --with-stdlib --failures-only
LD_LIBRARY_PATH=build/vendor/lib/host \
  ./build/release/ny-test --jobs 8 etc/tests/native --failures-only
./build/release/ny-fuzz compiler known-bugs --json /tmp/ny-known-bugs.json
```

## Assertions

```ny
use std.core

def condition = true
def actual = [1, 2]
def expected = [1, 2]

assert(condition, "behavior name")
assert_eq(actual, expected, "behavior name")
```

Assertion messages name behavior, not implementation details. Exact-value
assertions define deterministic behavior. Timing assertions are performance
checks.

## IO boundaries

| Boundary | Deterministic setup |
| --- | --- |
| Files | Temporary path, exact readback, cleanup. |
| Processes | Bounded command, explicit stdin/stdout, timeout. |
| Sockets | Local server, readiness check, timeout. |
| HTTP | Local fixture server or documented local service requirement. |
| TLS | Response transport/error metadata included in assertion. |
| Terminal | Color disabled unless color is the behavior under test. |

## Code fences

Every code fence declares what it contains:

| Fence | Rule |
| --- | --- |
| `ny` | Complete Nytrix source that must compile as an extracted doc fence. |
| `bash` | Shell command. |
| `json` | Manifest or data payload. |
| `text` | Grammar, output, directory tree, non-source transcript, or contextual source fragment. |

Silent success is valid for runnable examples. Assertion failure, panic, nonzero
exit status, or diagnostic output carries the failure signal.

## Completion

A change is complete when the focused check passes, relevant formatting/audit
commands pass, and the required build or test target passes.

For compiler, runtime, stdlib, or docs-generator changes, run
`./make test --with-stdlib`. For documentation-only changes, run `./make docs`
and any focused checks for changed examples.
