<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"spec","order":55,"summary":"Current effect-row support, design boundaries, and parity requirements."} -->
# Effect rows: implementation boundary

This is an implementation note, not a broader language guarantee. The
user-facing `@effects(...)` contract is specified in
[Functions](functions.md#attributes); this page separates that contract from
open-row implementation work.

## Current support

The type layer has an open-row unifier and serialized effect rows. Direct calls
through a user helper's `fnptr` parameter can substitute a known callback row
for tested positional or named arguments, aliases, inline lambdas, and returned
callbacks. Some local callback aliases retain or join rows across tested
branches, loops, and matches. Imported rows use qualified function name and
arity.

This is not general callback provenance. Unknown or escaping callbacks,
unsupported control-flow joins, re-export identity, and arbitrary captured
callback flows remain conservative. Built-in effects still enter analysis
through fixed masks; an effect label does not grant authority to access a
resource. Check [Implementation status](../learn/status.md) before relying on a
specific flow or route.

## Intended row model

An effect row is a normalized set of labels with an optional row variable.
Label order is irrelevant, duplicate labels collapse, and closed rows unify
only when their normalized labels match. Function arrows carry latent effects:
calling a function contributes its row, while creating a closure does not run
its body. A higher-order call must preserve the callback's effects in the
caller. The initial design is rank-1 row polymorphism; effect handlers and
higher-rank polymorphism are out of scope.

Effects, capabilities, ownership, and event traces are separate facts. An
effect describes observable work; a capability authorizes access; ownership
controls use and destruction; a trace describes ordering. Do not treat an
`@effects` row as a permission grant.

## Interface and migration requirements

Portable interfaces must serialize canonical function identity and normalized
rows with a versioned schema. Alpha-equivalent row variables must serialize
identically. A malformed or unsupported interface must fail with a diagnostic,
not silently become a pure row. Cache keys must include the normalized
interface identity.

Keep the existing mask checker as the user-facing contract until row
inference, interface transport, and backend parity cover existing effect
fixtures. Validate normalization, open-tail unification, occurs-check failure,
higher-order calls, partial application, aliases, imported interfaces, and
fail-closed diagnostics. These checks establish row behavior only; they do not
establish that an effectful operation is authorized or safe.

## Related

- [Function attributes](functions.md#attributes) define the current source
  contract.
- [Implementation status](../learn/status.md) summarizes known support limits.
- [Runtime](runtime.md#attributes-and-effects) describes runtime effect
  metadata.
