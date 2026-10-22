<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":50,"summary":"Write executable checks and run the fixture that covers the behavior and target you need."} -->
# Testing

A Nytrix test is a program that checks an observable result and exits nonzero
when the result is wrong. Keep the smallest useful assertion close to the
behavior under test.

## A focused program check

```ny
use core

fn sum(int a, int b) int { a + b }

assert_eq(sum(19, 23), 42, "sum")
```

Run it directly while developing:

```bash
ny path/to/check.ny
```

## Run repository fixtures

Use the fixture runner for compiler, runtime, and standard-library regressions:

```bash
./make test --pattern path-or-name
./make test --with-stdlib --pattern module-or-path
```

`./make help test` lists suite options. A focused pattern avoids an unrelated
full-suite run; the fixture itself declares expected compile/run behavior and
any required flags. Use the fixture path linked from the relevant specification
when checking a language boundary.

For a route-specific behavior, run the fixture with the same backend and
options your program will use. `ny` on the default route does not prove
`--native-only`, Wasm, or browser behavior. Browser changes use
`./make web-test`; C frontend and target-link checks have their own registered
tests.

## Add a regression

- Put language and runtime fixtures under `etc/tests/` using the existing
  `.nshape` conventions.
- Make the fixture assert the user-visible result or expected diagnostic, not
  an implementation detail that does not affect the contract.
- Add a negative case when the change defines a rejection boundary.
- Keep generated inputs deterministic and bounded.
- Run the exact new fixture first, then the appropriate surrounding suite.

A fixture pass proves only its declared source, flags, executor, and assertions.
See [Implementation status](status.md) for current coverage limits and
[Contributing](../CONTRIBUTE.md) for compiler change workflow.
