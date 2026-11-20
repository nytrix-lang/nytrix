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
| Parsing | `src/code/lang/ny/parser/` | Ny tokens, AST construction, and proof syntax |
| Types and contracts | `src/code/lang/ny/semantics/`, `src/code/lang/c/semantics/` | Language-specific type inference, refinements, effects, ownership, and C model checks |
| Language lowering | `src/code/lang/ny/lower/`, `src/code/lang/c/lower/` | Convert checked source constructs into compiler IR |
| NYIR | `src/code/backend/core/` | NYIR verification, optimization, interpretation, and reports |
| Machine code | `src/code/backend/lower/`, `src/code/backend/mach/arch/` | Target lowering, register allocation, object encoding, and linking support |
| LLVM integration | `src/code/backend/mach/emit/llvm/`, `src/code/lang/ny/gen/llvm/` | LLVM bridge, emission, and compatibility code |
| Compilation pipeline | `src/code/lang/ny/pipeline/`, `src/code/lang/ny/semantics/pipeline/`, `src/code/backend/core/cache/` | Ny stages, semantic analysis stages, caches, and compilation artifacts |
| Builtin semantic contracts | `src/code/backend/runtime/contracts.h`, `src/code/lang/ny/semantics/effects.c` | Language-level purity and effect metadata consumed by semantic analysis |
| Runtime | `src/code/backend/runtime/` | Allocation, managed values, platform services, and runtime helpers |
| C frontend | `src/code/lang/c/` | Clang integration and the in-tree C frontend |
| CLI tools | `src/cmd/` | Compiler, formatter, test, documentation, and platform commands |
| Standard library | `core/` | User-facing modules and host APIs |

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

Language-level builtin effect contracts live in
`src/code/backend/runtime/contracts.h` and are included by `effects.c`; they are
compiler semantics, not runtime dispatch data.

The compiler pipeline is orchestrated in
`src/code/lang/ny/pipeline/init.c`. The C frontend is split under
`src/code/lang/c/`: `parser/lex.c` supplies tokens to both header parsing and the
standalone `nyc` frontend, `declarations.c` parses included declarations,
and `parse.c` parses and lowers supported C translation units. The shared
declarations live in `c.h`; keep lexer, parser, and frontend interfaces
aligned when changing that boundary.

## Substrate guides

These contributors' substrates sit under the layers above. Each entry names
the owner file, its contract, and its wiring, grounded in the file headers.

The equality-saturation engine in `src/code/backend/passes/scalar/egraph.c`
canonicalizes pure scalar integer arithmetic per basic block: e-classes over
block values, a declarative rule pool applied to saturation (48 iterations,
2048 nodes of fuel), and cost-directed extraction. Only pure ALU/COPY nodes
participate; memory, calls, traps, control flow, PHIs, floats, and locals are
leaves. Rewrites rebuild the function on a scratch buffer and commit only if
`nyr_ir_verify` accepts them, and a value is rewritten only when the extracted
form is strictly cheaper. It enters the pass pipeline as `egraph_local` and
`egraph_saturate` (`backend/core/opt/pipeline.c:897-898`); `nyr_egraph_stats`
feeds IR reports and the `advanced.c` local driver.

ISLE-style selection in `src/code/backend/lower/isle.c` is a declarative rule
table (clean-room, ISLE-inspired): rules match a NYIR op plus immediate
operand shapes and emit cheaper forms, one shared applicator serving NYIR
text rewrites (`nyr_apply_rules`) and machine-form identity folds
(`ny_isle_apply_nir`). Extend the table rather than adding ad-hoc peepholes;
the pipeline passes `apply_rules` (`pipeline.c:899`) at the same site.

The tier planner in `src/code/backend/lower/tier.c` resolves
baseline/stencil/fast/opt from the requested tier plus target
frequency/size/cache-pressure facts (`nyr_tier_plan_init`). Explicit tier
requests are honored as labels and budget selectors only; the executable path
stays the shared NYIR→Machine IR→encode pipeline, and `llvm` remains a
top-level backend choice, never a native tier.

Effect and purity analysis in `src/code/lang/ny/semantics/effects.c` classifies
functions for effect checking and optimization gating, including the builtin
effect table (`NY_BUILTIN_EFFECT_*` entries for direct calls, callable
factories, and query from/step shapes). Diagnostics surface as
`[effects.*]` hint logs; do not bypass the table with per-call-site guesses.

Null-narrowing in `src/code/lang/ny/semantics/null_narrow.c` tracks
nullable-to-nonnull flow facts per binding through branches and assignments
(`true_nonnull`/`false_nonnull` plus optional true/false arm types), feeding
type refinement. Like all shared semantic analysis, it fails closed: absent
or empty facts refine nothing.

The proof substrate: `src/code/lang/ny/semantics/proof.c` hosts the solver backends
(a dependency-free Presburger subset over linear integer arithmetic via
Fourier–Motzkin, capped at 16 variables with a bounded constraint budget,
plus the
optional Z3 path when built with `NYTRIX_ENABLE_Z3`). Both return true only
when the negated proposition is proven unsatisfiable, so they cannot admit a
false proof; unknown falls through to constant evaluation.
`src/code/lang/ny/semantics/policy.c` is the versioned obligation/certificate
funnel (`ny-proof-obligation-v1` / `ny-proof-certificate-v1`, percent-encoded
fields), mirroring the `core/proof.ny` tactic order fold → range → solver
with the compiler-only `cc`/interval steps; `policy/check.c`'s bounded
checker is the sole verdict decider. `core/proof.ny` owns the language-side
`core.proof` policy, where `unknown` is first-class and never stands in for
false.

The fuzz harness under `src/cmd/tools/test/fuzz/` synthesizes random programs from
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

### Bare runtime payloads

The bare emitter supports a small runtime subset without an assembler, C
compiler, libc, or external runtime. `src/code/backend/mach/bare/runtime.c`
implements selected raw-word operations; the checked-in payloads at
`src/code/backend/mach/arch/x86/payload.h` and `a64/payload.h` contain the
position-independent machine code and read-only constants. `bare/generate.c`
maps backend operation IDs to the private entry ABI: four argument words
followed by an operation number. Floating-point arguments are passed as their
binary64 bit patterns. A function that needs a supported operation embeds one
payload behind a jump and calls its entry; other functions embed none.
Other target/OS combinations remain unsupported.

The payload strings retain the existing 32-byte bare header (magic, mapping
size, tagged byte length, and string tag 120) before the NUL-terminated bytes.
Float boxes use tag 110 at payload minus eight. Builders own a wrapper mapping
and a separate raw buffer; growth preserves self-append offsets and unmaps the
old buffer. Finalization releases the wrapper and returns the raw string.
Allocation and size failures return zero, except repeated append keeps the
runtime's partial-append fallback. Formatting supports nil, booleans, tagged
integers, strings, float boxes, and opaque pointer/function text; it does not
add bigint, bigfloat, complex, or collection support.

Float formatting converts the exact binary64 value to base-10^9 limbs, then
rounds to six significant digits using ties-to-even and sticky digits. It
follows C-locale `%g` formatting, including signed zero, infinities, NaN,
exponent thresholds, and exponent padding, without relying on host floating
point state. The payload also clones checked static typed-buffer templates and
classifies the bare value subset used by dynamic formatting. Native array data
keeps its public symbol at element zero; a private header range retains the
32-byte typed-buffer header before that symbol.

Regenerate or check the headers with `./make bare-payload [--check]`. The
current generated files use Clang/LLD 23.1.1 and llvm-objcopy. Generation
rejects absolute or page-relative relocations, unresolved symbols, and mutable
data. AArch64 uses the tiny code model so embedding at any instruction offset
does not invalidate ADRP page calculations. The
`etc/tests/code/intermediate/ny/bare/runtime-ops.nshape` fixture compares both
architectures with VM output while setting `CC=/nonexistent`, and
`asm-artifacts.nshape` checks emitted artifacts and unsupported targets. The
`runtime-payload.c` regression executes embedded code at a non-page-aligned
address and checks headers, integer extremes, UTF-8 recovery, builder growth
and self-aliasing, and float output against `snprintf("%g")`. Build and run it
on the host with:

```bash
cc -O2 -Isrc etc/tests/code/intermediate/ny/bare/runtime-payload.c \
  -o /tmp/nyr-bare-payload
/tmp/nyr-bare-payload 1000000
```

For AArch64, cross-build and run under QEMU:

```bash
aarch64-linux-gnu-gcc -O2 -Isrc \
  etc/tests/code/intermediate/ny/bare/runtime-payload.c \
  -o /tmp/nyr-bare-payload-a64
qemu-aarch64 -L /usr/aarch64-linux-gnu \
  /tmp/nyr-bare-payload-a64 100000
```

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
