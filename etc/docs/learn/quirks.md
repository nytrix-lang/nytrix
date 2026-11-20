<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":13,"summary":"A linked field guide to Nytrix syntax and behavior that often surprises newcomers."} -->
# Nytrix quick reference

These are the rules most likely to conflict with habits from C, Rust, or
Python. Each item links to the page with the complete contract.

Prerequisites: none; this is a lookup page.

## Coming from another language

| Coming from X | You might write Y | Nytrix does Z |
| --- | --- | --- |
| C, Rust, or Python | `let value = 1` | `let` is rejected; use `def value = 1` for an immutable binding or `mut value = 1` for a reassignable binding. |
| C or Rust | `value;` to end a statement | `;` begins a comment; statement boundaries come from the grammar and source layout. |
| Python | `for value in 1..4` and expect `4` to be excluded | Nytrix ranges include the end: this visits `1`, `2`, `3`, and `4`. |
| C or Python | `list(3)` to create three initialized elements | `list(3)` creates an empty list with capacity reserved; append before indexing. |
| C | `nil` as integer zero | `nil` is a distinct value, though both are falsy. |

## Declarations and syntax

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| Types go first | `fn greet(str name) str { ... }`; a return type follows `)`. | [Syntax](../spec/syntax.md#bindings), [Functions](../spec/functions.md#bindings-and-declarations) |
| Semicolons comment | `;` starts a line comment. It never ends a statement. | [Syntax](../spec/syntax.md#lexical-forms) |
| Use `fn` for functions and lambdas | Function values use the same keyword as named functions. | [Functions](../spec/functions.md#parameters-and-calls) |
| `if` is not pattern matching | `if` branches on truthiness; `case` dispatches values and inclusive ranges; `match` dispatches on patterns and ADTs. | [Control flow](../spec/control-flow.md), [Patterns](../spec/patterns.md) |
| Ranges include the end | `for x in 1..4` visits 1, 2, 3, and 4. `case` range arms are inclusive too. | [Control flow](../spec/control-flow.md#loops), [Patterns](../spec/patterns.md#case-patterns) |
| Counted loops are available | `for(mut i = 0 i < n ++i) { ... }` keeps the counter binding in scope. | [Control flow](../spec/control-flow.md#loops) |
| `goto` has cleanup rules | Leaving a scope runs its registered `defer` blocks. | [Control flow](../spec/control-flow.md#defer) |
| `del` is a binding operation | `del name` needs a mutable binding; it sets that binding to `nil` and clears its flow facts. It does not free an object. | [Functions](../spec/functions.md#bindings-and-declarations), [Memory](../spec/memory.md) |
| Near-miss spellings never parse | `let`, `func`, `import`, `macro`, `var`, `void`, and `printf` are all rejected with a message naming the supported spelling (`def`/`mut`, `fn`, `use`, `quote` with splicing, `none`, `print`). | [Syntax](../spec/syntax.md#lexical-forms) |

## Values and collections

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| `nil` is not zero | They are distinct values and dictionary keys, although both are falsy. | [Values](../spec/values.md#nil), [Control flow](../spec/control-flow.md#truthiness) |
| `list(n)` is empty | `n` reserves capacity; the initial length is zero. Append before indexing. | [Values](../spec/values.md#lists), [Troubleshooting](troubleshooting.md) |
| Bare `{}` is frozen | It is an immutable empty dictionary. Use `dict()` for a mutable empty accumulator. | [Values](../spec/values.md#frozen-and-mutable-literals) |
| Retain collection updates | List growth may return a replacement value; keep the return value. Use `clone` when an API needs a detached mutable container. | [Values](../spec/values.md#list-updates), [Functions](../spec/functions.md#return-and-container-behavior) |
| Indexing and `get` differ | Dictionary indexing returns the runtime default when a key is absent; `get(key, fallback)` makes the fallback explicit. | [Values](../spec/values.md#lookup-insertion-and-identity) |
| Truthiness is broad | Zero, `nil`, empty strings, and empty containers are false. Pointers, handles, and callables are true. | [Control flow](../spec/control-flow.md#truthiness) |

## Open representation defects in library code

These are compiler defects, not language rules: inside standard-library
modules a value's raw/boxed representation can be lost at the boundaries
below. Library code works around them as shown; remove a workaround only with
a fixture that fails without it.

| Pattern | Symptom | Workaround in the tree |
| --- | --- | --- |
| A record from a ternary, `def r = h ? dict_get(m, h, 0) : 0`, then `r = r.set(...)` | The record is typed `int` and stored back as a bare handle (the Wayland window table lost every field at fractional scales) | Assign in an `if` |
| `return d.set(k, v)` from a function returning a record | The caller receives a raw handle | Bind `out = out.set(...)`, then return `out` |
| A local named `s` holding a dict | It reads as `str` | Use another name |
| An empty list stored in a record, read in another module | It reads as an integer | Check `is_list` and fall back to `[]` |
| A typed `int` parameter or C callback argument stored in a dict | It reads back as `nil` or halved | `__box_i64(v)`, or type callback parameters (`i32 fd`) instead of `any` |
| An `int` match payload appended to a list in a stdlib module | It is stored untagged | Append `h + 0` |
| Reassigning an `int` parameter from `min`/`max` | The value is halved | Clamp into a fresh local |
| `mut any x = dict(8)` as a module global | The global never holds the dict | Initialise with the concrete type |
| `.len` read straight off an imported list global on `--native-only` | Wrong length | Bind to a typed local first |
| A stdlib function named like a `core` function (`split`) | Calls inside the module reach it and crash | Use a distinct name |

## Types, modules, and native code

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| `module` and `#main` serve two jobs | A file can expose an importable module and have a direct-entry body guarded by `#main`. | [Units](../spec/units.md) |
| `struct` and `layout` differ | `struct` is a language value; `layout` fixes field order and representation for ABI use. | [Types](../spec/types.md), [Native](../spec/native.md#layouts) |
| Choose the execution path | Default execution, `--run=aot`/`--output`, `--native-only`, and browser Wasm select different compiler paths. Unsupported forms can differ by path. | [Pipeline](../spec/pipeline.md), [Implementation status](status.md) |
| Safety has levels | Ownership analysis is off by default. `--borrow-check` enables advisory diagnostics; `--ownership-check=strict` promotes supported rules, and `--mode=safe` also enables strict typing and tracked raw-memory checks. | [Ownership](ownership.md), [Runtime](../spec/runtime.md#safety-profile) |
| Attributes are an open namespace | Any `@name` parses and stays visible to reflection; behavior attaches only to the recognized set in [Functions](../spec/functions.md#attributes). | [Functions](../spec/functions.md#attributes), [Comptime](../spec/comptime.md) |

## Threads and contracts

| Remember | Nytrix rule | Full details |
| --- | --- | --- |
| `@thread` depends on call position | A statement-position call detaches; using the call as a value joins and returns its result. | [Functions](../spec/functions.md#attributes), [Concurrency](concurrency.md) |
| Proof clauses are not a full theorem prover | `requires`, `decreases`, `law`, and `@total` have different checks and backend gaps. Check the current matrix before relying on them as a safety boundary. | [Proofs](proofs.md), [Implementation status](status.md) |

For maturity and intended use, see [Where Nytrix fits](status.md#where-nytrix-fits).

Next:

- [Start](start.md#tour) — read the common source forms.
- [Syntax](../spec/syntax.md) — source grammar and spellings.
- [Implementation status](status.md) — check feature boundaries.
