<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":12,"summary":"Execution routes, opt-in checks, and backend-specific limits in the current compiler."} -->
# Implementation status

Nytrix shares parsing and semantic checks across execution routes, but backend
support is not uniform. A passing program on one route is evidence for that
program and route only. Read the [specification](../spec/index.md) for the
language contract; use the focused fixture linked from a spec when a limit
matters.

## Execution routes

| Command | Route | Boundary |
| --- | --- | --- |
| `ny file.ny` | Default development route; LLVM JIT on supported builds | Use this for ordinary run-and-check work. |
| `ny -run file.ny` | Builds and runs a native executable | Target and ABI support depend on the selected native backend. |
| `ny -o app file.ny` | Emits a native executable without running it | The output still requires a compatible host or target runner. |
| `ny --native-only file.ny` | NYIR optimization and native lowering, without LLVM code generation | Unsupported shapes fail on this route; there is no LLVM fallback. |
| `ny --wasm file.ny` | WebAssembly target | Only the documented Wasm imports and runtime surface are available. |
| `./make web-test` | Browser build and execution checks | This exercises the browser host contract, not general desktop APIs. |

Check `ny --help` and `./make --help` for the installed command surface. The
[Native specification](../spec/native.md) describes target and ABI boundaries;
[Browser](browser.md) covers browser capabilities.

## Feature boundaries

| Area | Current boundary | Start here |
| --- | --- | --- |
| Type checking, traits, and overloads | Shared semantic checks run before backend selection; native lowering still has target- and shape-specific limits. | [Types](../spec/types.md), [Traits](../spec/traits.md) |
| Refinements and proofs | The solver accepts a bounded calculus. Unknown or unsupported obligations are not proof. | [Types](../spec/types.md), [Proofs](proofs.md) |
| Effects and ownership | Contracts and checks are opt-in or partial in documented areas; do not treat an annotation as runtime enforcement unless its spec says so. | [Functions](../spec/functions.md), [Ownership](ownership.md), [Effects](effect.md) |
| Compile-time generation | Evaluation and syntax are bounded; generated declarations still pass through normal checking. | [Comptime](../spec/comptime.md), [Comptime guide](comptime.md) |
| C interop and aggregate ABI | Supported declarations and calling conventions depend on the frontend, target, and signature shape. | [Native](../spec/native.md) |
| UI, audio, and networking | Host capabilities differ across native and browser routes. Check the API contract and its fixture for the target you ship. | [UI](ui.md), [Networking](networking.md), [Browser](browser.md) |

## How to verify a claim

1. Find the owning rule in the [specification index](../spec/index.md).
2. Identify the exact backend, target, and flags used by your program.
3. Run the focused fixture linked from that rule, or add a minimal fixture for
   the unsupported shape.
4. Treat historical changelog results as dated evidence, not a statement about
   the current checkout.

A fixture proves its tested inputs and routes. It does not establish complete
support for a feature family.
