<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":0,"summary":"A topic map for Nytrix language contracts and implementation boundaries."} -->
# Specification

Language contracts define syntax and behavior; backend support varies by source
shape. Check [implementation status](../learn/status.md) for route limits and the
linked fixtures for tested cases.

## Language

| Topic | Contract |
| --- | --- |
| [Units](units.md) | Files, imports, modules, and entry points |
| [Syntax](syntax.md) | Source forms and grammar |
| [Values](values.md) | Literals, containers, equality, and value operations |
| [Types](types.md) | Type forms, inference, refinements, and proofs |
| [Functions](functions.md) | Calls, parameters, closures, and function contracts |
| [Control flow](control-flow.md) | Branches, loops, exits, cleanup, and error handling |
| [Operators](operators.md) | Precedence, built-ins, and overloads |
| [Patterns](patterns.md) | `case`, `match`, bindings, and exhaustiveness |
| [Errors](errors.md) | Assertions, panics, results, and propagation |
| [Traits and impl](traits.md) | Methods, operators, capabilities, and laws |

## Runtime and machine boundaries

| Topic | Contract |
| --- | --- |
| [Ownership](ownership.md) | Borrow, move, and allowance analysis |
| [Memory](memory.md) | Runtime representation and storage boundaries |
| [Runtime](runtime.md) | Execution, cleanup, concurrency, and effects |
| [Native](native.md) | FFI, ABI, targets, and unsupported interfaces |
| [Nyr](nyr.md) | Public low-level IR and object API |
| [Linking](linker.md) | Object and symbol resolution support |
| [Embedding](nytrix.md) | C embedding and VM APIs |

## Compiler reference

Compiler architecture and compile-time facilities:

- [Compile-time execution](comptime.md)
- [Effect rows](functions.md#effect-rows)
- [Compiler pipeline](pipeline.md)
- [NYIR](ir.md)

## Reading a boundary

For backend-, target-, or source-shape-specific behavior, follow the owning
page's implementation and fixture links. The
[status guide](../learn/status.md) covers adoption and execution-route limits.
