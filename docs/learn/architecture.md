<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"learn","order":190,"summary":"Locate the compiler layers that own parsing, semantics, lowering, execution, and tools."} -->
# Compiler architecture

Use the layer that owns the invariant. Source syntax belongs in the frontend;
source-level meaning belongs in shared semantic analysis; target details belong
in lowering or the backend. A backend must not silently reinterpret a failed
contract from an earlier layer.

Prerequisites: a source checkout and a concrete behavior or subsystem to locate.

## Source map

| Concern | Primary location | Responsibility |
| --- | --- | --- |
| Configuration and diagnostics | `src/base/` | Options, source diagnostics, tracing, and process configuration |
| Parsing | `src/code/lang/parse/` | Tokens, AST construction, and source diagnostics |
| Types and contracts | `src/code/lang/types/` | Type inference, refinements, proofs, effects, and ownership checks |
| Language lowering | `src/code/lang/lower.c`, `src/code/lang/lower/` | Convert checked source constructs into compiler IR |
| NYIR | `src/code/backend/ir/` | NYIR verification, optimization, interpretation, and reports |
| Machine code | `src/code/backend/machine/`, `src/code/backend/object/` | Target lowering, register allocation, object encoding, and linking support |
| LLVM integration | `src/code/backend/llvm/` | LLVM emission and compatibility code |
| LLVM preparation and compatibility code | `src/code/lang/llvm/` | Module/signature preparation and LLVM support helpers; legacy AST-body emitters are not a live CLI or REPL route |
| Build pipeline | `src/code/lang/pipeline/`, `src/code/backend/wire/` | Stage orchestration, caches, and compilation artifacts |
| Builtin semantic contracts | `src/code/lang/runtime/contracts.c`, `src/code/lang/types/funcpurity.c` | Language-level purity and effect metadata consumed by semantic analysis |
| Runtime | `src/code/backend/runtime/` | Allocation, managed values, platform services, and runtime helpers |
| C frontend | `src/code/lang/parse/c/` | Clang integration and the in-tree C frontend |
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

The runtime entry point `src/code/backend/runtime/init.c` includes `ast.c`,
`bigfloat.c`, `bigint.c`, `core.c`, `ffi.c`, `ffigates.c`, `gc.c`, `math.c`,
`memory.c`, `os.c`, `proof.c`, `simmd.c`, and `string.c` into one translation
unit. Keep those files compatible when built together, including static names
and declarations.

Language-level builtin effect contracts stay in
`src/code/lang/runtime/contracts.c`; they are compiler semantics, not runtime
dispatch data.

The compiler pipeline is orchestrated in
`src/code/lang/pipeline/init.c`. The C frontend is split under
`src/code/lang/parse/c/`: `lex.c` supplies tokens to both header parsing and the
standalone `nyc` frontend, `declarations.c` parses included declarations,
and `parse.c` parses and lowers supported C translation units. The shared
declarations live in `c.h`; keep lexer, parser, and frontend interfaces
aligned when changing that boundary.

## Substrate guides

These contributors' substrates sit under the layers above. Each entry names
the owner file, its contract, and its wiring, grounded in the file headers.

The equality-saturation engine in `src/code/backend/ir/opt/egraph.c`
canonicalizes pure scalar integer arithmetic per basic block: e-classes over
block values, a declarative rule pool applied to saturation (48 iterations,
2048 nodes of fuel), and cost-directed extraction. Only pure ALU/COPY nodes
participate; memory, calls, traps, control flow, PHIs, floats, and locals are
leaves. Rewrites rebuild the function on a scratch buffer and commit only if
`nyr_ir_verify` accepts them, and a value is rewritten only when the extracted
form is strictly cheaper. It enters the pass pipeline as `egraph_local` and
`egraph_saturate` (`backend/ir/opt/pipeline.c:897-898`); `nyr_egraph_stats`
feeds IR reports and the `advanced.c` local driver.

ISLE-style selection in `src/code/backend/ir/isle.c` is a declarative rule
table (clean-room, ISLE-inspired): rules match a NYIR op plus immediate
operand shapes and emit cheaper forms, one shared applicator serving NYIR
text rewrites (`nyr_apply_rules`) and machine-form identity folds
(`ny_isle_apply_nir`). Extend the table rather than adding ad-hoc peepholes;
the pipeline passes `apply_rules` (`pipeline.c:899`) at the same site.

The tier planner in `src/code/backend/ir/tier.c` resolves
baseline/stencil/fast/opt from the requested tier plus target
frequency/size/cache-pressure facts (`nyr_tier_plan_init`). Explicit tier
requests are honored as labels and budget selectors only; the executable path
stays the shared NYIR→Machine IR→encode pipeline, and `llvm` remains a
top-level backend choice, never a native tier.

Effect and purity analysis in `src/code/lang/types/funcpurity.c` classifies
functions for effect checking and optimization gating, including the builtin
effect table (`NY_BUILTIN_EFFECT_*` entries for direct calls, callable
factories, and query from/step shapes). Diagnostics surface as
`[effects.*]` hint logs; do not bypass the table with per-call-site guesses.

Null-narrowing in `src/code/lang/types/nullnarrow.c` tracks
nullable-to-nonnull flow facts per binding through branches and assignments
(`true_nonnull`/`false_nonnull` plus optional true/false arm types), feeding
type refinement. Like all shared semantic analysis, it fails closed: absent
or empty facts refine nothing.

The proof substrate: `src/code/lang/types/proof.c` hosts the solver backends
(a dependency-free Presburger subset over linear integer arithmetic via
Fourier–Motzkin, capped at 16 variables with a bounded constraint budget,
plus the
optional Z3 path when built with `NYTRIX_ENABLE_Z3`). Both return true only
when the negated proposition is proven unsatisfiable, so they cannot admit a
false proof; unknown falls through to constant evaluation.
`src/code/lang/types/policy.c` is the versioned obligation/certificate
funnel (`ny-proof-obligation-v1` / `ny-proof-certificate-v1`, percent-encoded
fields), mirroring the `lib/proof.ny` tactic order fold → range → solver
with the compiler-only `cc`/interval steps; `policy/check.c`'s bounded
checker is the sole verdict decider. `lib/proof.ny` owns the language-side
`core.proof` policy, where `unknown` is first-class and never stands in for
false.

The fuzz harness under `src/cmd/test/fuzz/` synthesizes random programs from
shape templates (`generate.c`, covering parser, type-checker, and codegen
edges), drives workers (`proc.c`, `worker.c`), and compares results through
`compare/` (its JSON reports carry `native_compare`/`native_replay` fields;
its audit tools cross-check definition/reference sets, including the SIMMD
wrapper audit). `usage.c` prints the recommended command lines with
tuned profiles, thread counts, and duration parameters; the driver is
`./build/ny-test fuzz`.

The portable SIMD substrate `src/code/backend/runtime/simmd.c` provides
vectorized f32/f64/i64 wrappers with x86 (SSE/AVX via `immintrin.h` and
cpuid dispatch) and Arm NEON fast paths over a portable fallback. It is
included into the single runtime translation unit by `init.c`; guard every
new wrapper with the platform checks already in the file and keep fallbacks
semantically identical.

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

Next:

- [Pipeline](../spec/pipeline.md) — compiler stages and routes.
- [Compiler debugging](debugging.md) — inspect a failing stage.
- [Contributing](../CONTRIBUTE.md) — build and verification procedures.
