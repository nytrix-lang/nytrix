<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":0,"summary":"A topic map for Nytrix language contracts and implementation boundaries."} -->
# Specification

Use these pages for exact syntax and behavior. The specification describes
current language contracts; it does not imply that every backend accepts every
program shape. Check [implementation status](../learn/status.md) for known route
limits, and follow the linked fixtures when a boundary matters.

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

These pages describe implementation architecture rather than additional source
language guarantees:

- [Compile-time execution](comptime.md)
- [Effect-row implementation boundary](effect-rows.md)
- [Compiler pipeline](pipeline.md)
- [NYIR](ir.md)

## Reading a boundary

A specification claim should point to the owning implementation or a focused
fixture when behavior is backend-, target-, or source-shape-specific. A fixture
proves only the input and route it exercises. For adoption guidance, start with
[Where Nytrix fits](../learn/fits.md); for current support, use
[Implementation status](../learn/status.md).
