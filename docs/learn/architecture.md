<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"learn","order":190,"summary":"Locate the compiler layers that own parsing, semantics, lowering, execution, and tools."} -->
# Compiler architecture

Use the layer that owns the invariant. Source syntax belongs in the frontend;
source-level meaning belongs in shared semantic analysis; target details belong
in lowering or the backend. A backend must not silently reinterpret a failed
contract from an earlier layer.

## Source map

| Concern | Primary location | Responsibility |
| --- | --- | --- |
| Parsing | `src/code/lang/parse/` | Tokens, AST construction, and source diagnostics |
| Types and contracts | `src/code/lang/types/` | Type inference, refinements, proofs, effects, and ownership checks |
| Language lowering | `src/code/lang/lower.c`, `src/code/lang/lower/` | Convert checked source constructs into compiler IR |
| NYIR | `src/code/backend/ir/` | NYIR verification, optimization, interpretation, and reports |
| Machine code | `src/code/backend/machine/`, `src/code/backend/object/` | Target lowering, register allocation, object encoding, and linking support |
| LLVM integration | `src/code/backend/llvm/` | LLVM emission and compatibility code |
| Build pipeline | `src/code/lang/pipeline/`, `src/code/backend/wire/` | Stage orchestration, caches, and compilation artifacts |
| Runtime | `src/code/lang/runtime/` | Allocation, managed values, platform services, and runtime helpers |
| C frontend | `src/code/lang/c/` | Clang integration and the in-tree C frontend |
| CLI tools | `src/cmd/` | Compiler, formatter, test, documentation, and platform commands |
| Standard library | `lib/` | User-facing modules and host APIs |

These are source ownership guides, not a promise that every feature has a single
file or that each backend supports the same shapes. Follow the public headers
under `src/include/` when changing a supported C API.

## Choosing an implementation layer

- Change the parser when the source form itself is accepted or rejected
  incorrectly. Keep semantic validation out of token recognition.
- Change shared type/contract analysis when all backends must agree on a source
  fact. Preserve source-located diagnostics and fail closed when analysis is
  inconclusive.
- Change language lowering when a checked source construct needs an IR
  representation. Do not repeat source name or overload resolution in a
  backend.
- Change NYIR when the behavior is target-independent. Verify the IR invariant
  before and after each transform.
- Change machine lowering only for target-specific ABI or instruction
  behavior. Keep source semantics in earlier layers.
- Change runtime or library code when the observable behavior belongs to a
  runtime service or public API; update the matching contract and tests.

## Translation-unit boundaries

The runtime entry point `src/code/lang/runtime/init.c` includes `ast.c`,
`bigfloat.c`, `bigint.c`, `core.c`, `ffi.c`, `ffigates.c`, `gc.c`, `math.c`,
`memory.c`, `os.c`, `proof.c`, `simmd.c`, and `string.c` into one translation
unit. Keep those files compatible when built together, including static names
and declarations.

The compiler pipeline is orchestrated in
`src/code/lang/pipeline/init.c`. The C frontend is split under
`src/code/lang/c/`: `c_lex.c` supplies tokens to both header parsing and the
standalone `nyc` frontend, `c_parse.c` parses declarations, and
`nyc_frontend.c` lowers supported C function bodies. The shared declarations
live in `c.h`; keep lexer, parser, and frontend interfaces aligned when
changing that boundary.

## Change checklist

1. Name the observable behavior and the layer that owns it.
2. Check existing callers, public contracts, and focused tests before changing
   an interface.
3. Keep unsupported cases explicit; do not add a fallback that changes the
   selected route silently.
4. Verify the changed layer and run an end-to-end smoke through the affected
   user-facing route.

For public behavior and backend caveats, see [Implementation status](status.md)
and the [specification index](../spec/index.md). For contribution commands, see
[Contributing](../CONTRIBUTE.md).
