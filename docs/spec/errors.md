# Errors and diagnostics

Errors use assertions, panics, recoverable result values, and compiler
diagnostics.

## Assertions

```ny
use std.core

def condition = true
def actual = [1, 2]
def expected = [1, 2]

assert(condition, "behavior")
assert_eq(actual, expected, "behavior")
```

Assertions fail the current execution with the supplied message. Assertion
messages name the behavior being checked.

## Panic

```ny
use std.core

try {
   panic("message")
} catch err {
   assert_eq(repr(err), "\"message\"", "panic message")
}
```

`panic` aborts the current execution path. It is for unrecoverable states.
The abort is catchable by `try`/`catch` on the language/runtime path.

## Structured errors

```ny
use std.core
use std.core.error

def e = exception(ERR_DIV_ZERO, "division by zero")
def w = warning(WARN_RUNTIME, "slow fallback")

assert_eq(error_kind(e), ERR_DIV_ZERO, "error kind")
assert_eq(error_message(e), "division by zero", "error message")
assert(is_error(e, ERR_DIV_ZERO), "error match")
assert_eq(error_kind(w), WARN_RUNTIME, "warning kind")
```

Structured errors and warnings are ordinary values with a kind and message.
Runtime panics produced by checked operations, such as division by zero or
invalid receiver/index access, can be captured by `try`/`catch`.

## Recoverable results

Standard-library APIs can return structured result values for recoverable
failure. Callers inspect the success/error shape before using the payload when
strict type checking requires refinement.

```ny
use std.core

fn operation(): dict {
   {"ok": true, "value": 42}
}

def r = operation()
if(!r.get("ok", false)){ return r }
def value = r.get("value", nil)
assert_eq(value, 42, "result value")
```

Exact field names belong to the API returning the result.

## Diagnostics

Diagnostics identify source location, severity, and message. Compact
diagnostics collect a dense error set. Rich diagnostics print wider source
context.

```bash
ny --diag-compact --collect-errors file.ny
ny --diag-rich file.ny
```

## Common compile-time failures

| Failure | Meaning |
| --- | --- |
| Undefined symbol | Missing import, wrong export name, or out-of-scope binding. |
| Unavailable receiver method | Receiver form is not available for that value/API. |
| Strict type failure | Dynamic shape was not refined enough. |
| Ownership failure | A move, borrow, release, return, or contract violates `--borrow-check`. |
| Compile-time proof failure | `assert_compile`, range proof, or index proof could not be proven. |
| Safe-mode raw memory failure | A raw pointer access lacks a proven in-bounds byte range. |
| Native boundary failure | Pointer, handle, layout, or ABI shape is wrong. |
| Parser failure | Source spelling is not a valid syntax form. |

## Related

- [types.md](types.md) for strict type rules.
- [syntax.md](syntax.md) for parser-level forms.
- [diagnostics.md](../learn/diagnostics.md) for debugging workflow.
- [troubleshooting.md](../learn/troubleshooting.md) for fixes by symptom.
