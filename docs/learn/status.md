<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":12,"summary":"Choose an execution route and assess Nytrix's current feature and target limits."} -->
# Status and execution routes

Nytrix is experimental. Its parser and semantic checks are shared across
execution routes, but code generation and host APIs have route-specific limits.
For a project, test the features, ABI signatures, and host services it needs on
the target it will use.

## Where Nytrix fits

Small tools, native experiments, and compiler research can use C interop and
compiler inspection while adapting to compiler changes. APIs, package support,
and target coverage are still changing; pin a commit for reproducible builds.

Do not treat the bounded proof, ownership, or effect analyses as general formal
verification. Do not assume a program that runs through LLVM also runs through
the owned native backend or in a browser.

## Execution routes

| Goal | Command | Boundary |
| --- | --- | --- |
| Run a file during development | `ny file.ny` | Default route; LLVM JIT on supported builds. |
| Build and run an executable | `ny -run file.ny` | AOT compilation followed by execution. |
| Save an executable | `ny -o app file.ny` | AOT compilation without execution; needs a compatible target to run. |
| Require the owned native path | `ny --native-only file.ny` | NYIR and native JIT; unsupported shapes fail without LLVM fallback. |
| Emit WebAssembly | `ny --wasm file.ny` | Wasm target with explicit imports and its supported runtime surface. |
| Test browser behavior | `./make web-test` | Browser host contract and browser-specific fixtures. |
| Inspect interpreted NYIR | `ny --nyir-run file.ny` | Diagnostic VM, not native or LLVM execution. |

`-o` implies compile-only. Combine `--native-only` with `-run` or `-o` when
checking an AOT artifact rather than native JIT execution. Command defaults
and available backends depend on the build; `ny --help` reports its options.
See [Native](../spec/native.md) for target and ABI rules, [REPL](repl.md) for
interactive execution, and [Browser](browser.md) for browser capabilities.

## Feature boundaries

| Area | Current boundary | Reference |
| --- | --- | --- |
| Types, traits, and overloads | Shared semantic checks precede backend selection; lowering still has target- and shape-specific limits. | [Types](../spec/types.md), [Traits](../spec/traits.md) |
| Refinements and proofs | A bounded calculus; false and unknown proof obligations are not evidence. | [Types](../spec/types.md#proof-and-refinement-types), [Proofs](proofs.md) |
| Preconditions and termination | `requires`, `decreases`, and `@total` have distinct checks; a measure alone is not a termination proof. | [Function contracts](../spec/functions.md#preconditions-and-termination-measures) |
| Trait laws | Checked in shared trait validation with the proof solver and compile-time Boolean evaluation. | [Law blocks](../spec/traits.md#law-blocks) |
| Effects | Declared masks and bounded callback-row propagation; not arbitrary callback provenance or resource authorization. | [Effects](effect.md), [Effect rows](../spec/functions.md#effect-rows) |
| Ownership | Opt-in borrow, move, and allowance checks; aggregate and alias tracking is partial. Cleanup policy is separate. | [Ownership](../spec/ownership.md), [Heap policy](../spec/runtime.md#heap-policy) |
| Compile-time generation | Bounded evaluation and syntax insertion; generated declarations pass through normal checks. | [Comptime](../spec/comptime.md) |
| C interop | Declaration import and aggregate ABI support depend on frontend, target, and signature. | [Native](../spec/native.md) |
| UI, audio, and networking | Host capabilities differ between native and browser routes. | [UI](ui.md), [Networking](networking.md), [Browser](browser.md) |

## Evaluate a project

1. List its required targets, ABI signatures, host services, and language features.
2. Read their contracts in the [specification](../spec/index.md), including
   unsupported cases.
3. Compile and run a small representative program through the route you will ship.
4. Keep checks for the boundaries the application relies on, including expected
   failures and cleanup.

[Testing](testing.md) explains focused fixtures. A fixture covers its declared
inputs and execution routes, not every member of a feature family. Changelog
results are historical evidence, not a test of the current checkout.

## Documentation smoke limitations

The 2026-10-22 manual smoke used `build/release/ny-full`
(`0.10.0+git.60f30d2f0220`) on the default LLVM route, with
`NYTRIX_STD_CACHE=0` and `NYTRIX_STD_BC_CACHE_AUTO=0`. It exposed these
source-shape failures. They are implementation limits, not changes to the
language contracts:

| Source shape | Observed failure |
| --- | --- |
| Reassigning `counts = counts.set(...)` on a mutable `dict()` accumulator | The cookbook's two-increment check read `0` instead of `2`. In-place mutation is shown in the cookbook; the setter-return path is not established by that example. |
| A top-level `def any boxed = nil` followed by `is_nil(boxed)` and `type(boxed)` | The value-specification example failed its dynamic nil identity assertion. |
| Defers appending to a local string captured from a function | The control-flow example did not produce the specified `"ba"` at scope exit. |
| `atomic_add` followed by `atomic_load` on an `atomic_i64` cell | The runtime example did not read the updated value `1`, including with qualified calls to `core.os.atomic`. |

The affected examples describe the intended contract and identify their known
failures. These observations cover only the tested route and compiler revision.
