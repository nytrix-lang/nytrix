<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":13,"summary":"A linked field guide to Nytrix syntax and behavior that often surprises newcomers."} -->
# Nytrix quick reference

These are the rules most likely to conflict with habits from C, Rust, or
Python. Each item links to the page with the complete contract.

## Declarations and syntax

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| Types go first | `fn greet(str name) str { ... }`; a return type follows `)`. | [Syntax](../spec/syntax.md#bindings-and-functions), [Functions](../spec/functions.md#bindings-and-declarations) |
| Semicolons comment | `;` starts a line comment. It never ends a statement. | [Syntax](../spec/syntax.md#lexical-forms) |
| Use `fn` for functions and lambdas | Function values use the same keyword as named functions. | [Functions](../spec/functions.md#parameters-and-calls) |
| `if` is not pattern matching | `if` branches on truthiness; `case` dispatches values and inclusive ranges; `match` dispatches on patterns and ADTs. | [Control flow](../spec/control-flow.md), [Patterns](../spec/patterns.md) |
| Ranges include the end | `for x in 1..4` visits 1, 2, 3, and 4. `case` range arms are inclusive too. | [Control flow](../spec/control-flow.md#loops), [Patterns](../spec/patterns.md#case-patterns) |
| Counted loops are available | `for(mut i = 0 i < n ++i) { ... }` keeps the counter binding in scope. | [Control flow](../spec/control-flow.md#loops) |
| `goto` has cleanup rules | Leaving a scope runs its registered `defer` blocks. | [Control flow](../spec/control-flow.md#defer) |
| `del` is a binding operation | `del name` needs a mutable binding; it sets that binding to `nil` and clears its flow facts. It does not free an object. | [Functions](../spec/functions.md#bindings-and-declarations), [Memory](../spec/memory.md) |

## Values and collections

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| `nil` is not zero | They are distinct values and dictionary keys, although both are falsy. | [Values](../spec/values.md#nil), [Control flow](../spec/control-flow.md#truthiness) |
| `list(n)` is empty | `n` reserves capacity; the initial length is zero. Append before indexing. | [Values](../spec/values.md#lists), [Troubleshooting](troubleshooting.md) |
| Bare `{}` is frozen | It is an immutable empty dictionary. Use `dict()` for a mutable empty accumulator. | [Values](../spec/values.md#frozen-and-mutable-literals) |
| Retain collection updates | List growth may return a replacement value; keep the return value. Use `clone` when an API needs a detached mutable container. | [Values](../spec/values.md#list-updates), [Functions](../spec/functions.md#return-and-container-behavior) |
| Indexing and `get` differ | Dictionary indexing returns the runtime default when a key is absent; `get(key, fallback)` makes the fallback explicit. | [Values](../spec/values.md#lookup-insertion-and-identity) |
| Truthiness is broad | Zero, `nil`, empty strings, and empty containers are false. Pointers, handles, and callables are true. | [Control flow](../spec/control-flow.md#truthiness) |

## Types, modules, and native code

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| `module` and `#main` serve two jobs | A file can expose an importable module and have a direct-entry body guarded by `#main`. | [Units](../spec/units.md) |
| `struct` and `layout` differ | `struct` is a language value; `layout` fixes field order and representation for ABI use. | [Types](../spec/types.md), [Native](../spec/native.md#layouts) |
| Choose the execution path | Default execution, `-run`/`-o`, `--native-only`, and browser Wasm select different compiler paths. Unsupported forms can differ by path. | [Pipeline](../spec/pipeline.md), [Implementation status](status.md) |
| Safety has levels | Ownership analysis is off by default. `--borrow-check` enables advisory diagnostics; `--ownership-strict` promotes supported rules, and `--safe-mode` also enables strict typing and tracked raw-memory checks. | [Ownership](ownership.md), [Runtime](../spec/runtime.md#safety-profile) |

## Threads and contracts

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| `@thread` depends on call position | A statement-position call detaches; using the call as a value joins and returns its result. | [Functions](../spec/functions.md#attributes), [Concurrency](concurrency.md) |
| Proof clauses are not a full theorem prover | `requires`, `decreases`, `law`, and `@total` have different checks and backend gaps. Check the current matrix before relying on them as a safety boundary. | [Proofs](proofs.md), [Implementation status](status.md) |

For the overall maturity and intended use, see [Where Nytrix fits today](fits.md).
