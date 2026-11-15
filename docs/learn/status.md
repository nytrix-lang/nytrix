<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":12,"summary":"Choose an execution route and assess Nytrix's current feature and target limits."} -->
# Status and execution routes

Nytrix is experimental. Its parser and semantic checks are shared across
execution routes, but code generation and host APIs have route-specific limits.
For a project, test the features, ABI signatures, and host services it needs on
the target it will use.

Prerequisites: none; check the compiler revision and target before using this
status information.

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
| Build and run an executable | `ny --run=aot file.ny` | AOT compilation followed by execution. |
| Save an executable | `ny --output=app file.ny` | AOT compilation without execution; needs a compatible target to run. |
| Require the owned native path | `ny --native-only file.ny` | NYIR and native JIT; unsupported shapes fail without LLVM fallback. |
| Emit WebAssembly | `ny --native-backend=wasm --emit=wasm=app.wasm file.ny` | Wasm target with explicit imports and its supported runtime surface. |
| Test browser behavior | `./make web-test` | Browser host contract and browser-specific fixtures. |
| Inspect interpreted NYR | `ny --nyr-run file.ny` | Diagnostic VM, not native or LLVM execution. |

`--output` implies compile-only. Combine `--native-only` with `--run=aot` or
`--output` when
checking an AOT artifact rather than native JIT execution. Command defaults
and available backends depend on the build; `ny --help` reports its options.
See [Native](../spec/native.md) for target and ABI rules, [REPL](repl.md) for
interactive execution, and [Browser](browser.md) for browser capabilities.

## Feature boundaries

| Area | Current boundary | Reference |
| --- | --- | --- |
| Memory safety checks | `--heap=safe` and `--mode=safe` enable guard pages and quarantine for user allocations. LLVM AddressSanitizer, MemorySanitizer, and ThreadSanitizer instrument emitted code and its runtime. UBSan checks emitted signed 64-bit arithmetic and null accesses; ownership analysis stays opt-in and bounded. | [Safety](safety.md), [Ownership](ownership.md) |
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

## Previously observed smoke findings

A manual smoke on 2026-10-22 (`0.10.0+git.60f30d2f0220`, default route,
library caches disabled) found four source-shape failures. Later fixes cover
the dictionary setter reassignment, top-level `is_nil` folding, atomic
tagged-word updates, and deferred mutation of captured locals. The defer and
loop-capture fixtures now check LIFO mutation, integer and floating-point
slots, panic unwinding, null callbacks, and registration-time snapshots across
native JIT, native-only, LLVM MCJIT/ORC, and the diagnostic VM.

These are historical observations, not current route limitations. See the
[changelog](../CHANGELOG.md#compiler-gates-and-optimization-evidence) for the
defer fix and its focused test command.

Next:

- [Testing](testing.md) — run a target-specific fixture.
- [Troubleshooting](troubleshooting.md) — investigate an unsupported path.
- [Execution pipeline](../spec/pipeline.md) — identify backend boundaries.
