<!-- nytrix-doc: {"audience":"user","featured":false,"group":"spec","order":60,"summary":"Opt-in borrow, resource-move, and usage-count checks with their supported limits."} -->
# Ownership

Nytrix ownership checks are opt-in and cover documented source shapes. They are
not a general lifetime or resource theorem prover. Runtime heap policy and
reclamation are separate from static borrow and move analysis; see
[Runtime](runtime.md#heap-policy).

## Borrows

`&value` and `&mut value` create shared and exclusive borrows. For supported
local values, the checker enforces these rules:

- A borrow cannot escape into a field, collection, or captured closure.
- Returning a borrow requires `@returns_borrow(parameter)` to identify its
  source parameter.
- A live exclusive borrow prevents conflicting reads, writes, or additional
  borrows of an overlapping place.
- A borrow remains live through its last use on each analyzed control-flow
  path. Unknown callback behavior is handled conservatively.

Alias and place analysis covers the documented local, field, and literal-index
forms. Dynamic indices, global provenance, and general interprocedural return
summaries are not inferred. An unsupported or inconclusive shape does not gain
a borrow-safety certificate.

## Linear and affine resources

`linear` resource types require one consumption on each supported path;
`affine` resources permit zero or one. Reading a field is not consumption.
Moving a value, passing it to a consuming function, or explicitly releasing it
can discharge its obligation. Reusing a moved value is rejected.

```text
linear layout File { int fd }
affine layout Socket { int fd }

@borrows(value)
@returns_borrow(value)
fn inspect(value)

@consumes(resource)
@releases(resource)
fn close(resource)
```

Resource-bearing aggregates and field extraction have partial tracking. Do not
assume every dynamic aggregate or alias shape receives field-sensitive move
analysis; consult [Implementation status](../learn/status.md) and run the
focused fixture for the exact form.

## Usage intervals

`@allowance(parameter, [lower, upper])` constrains uses on supported
control-flow paths. The upper bound may be `omega` or `inf`; an integer is an
exact count. Sequential uses add, exclusive branches join, and a loop may
raise the upper bound to unbounded. Reassignment, closures, `defer`, and
complex control flow have bounded support. If the checker cannot validate an
allowance within its analysis limit, compilation fails closed.

## Enforcement

| Option | Effect |
| --- | --- |
| `--borrow-check` | Enables advisory borrow and ownership diagnostics without selecting RAII cleanup. |
| `--ownership-strict` | Treats supported ownership diagnostics as errors. |
| `--ownership` | Selects ownership-aware cleanup and heap behavior. |
| `--safe-mode` | Enables the stricter safety profile described in [Runtime](runtime.md#safety-profile). |

The checker runs in shared semantic analysis so supported diagnostics do not
depend on optimization level or code-generation backend. This does not imply
that all ownership shapes or runtime services have identical backend support.

## Verification

Focused positive and negative fixtures live under `etc/tests/lang/ownership/`.
For a safety-sensitive API, run the fixture with the same ownership options and
execution route used by the application. See [Ownership guide](../learn/ownership.md)
for practical API design.
