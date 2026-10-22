<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"spec","order":15,"summary":"How checked source reaches NYIR, machine code, LLVM, or WebAssembly."} -->
# Pipeline

The compiler checks source before selecting a code-generation route. Each
backend has its own source-shape and host limits.

## Stage map

Source units and the selected standard library pass through parsing, expansion,
symbol resolution, and semantic checking before NYIR lowering. Ownership and
provenance analysis run when enabled. The selected backend then consumes NYIR:

| Route | Code generation |
| --- | --- |
| LLVM | NYIR-to-LLVM emission, then LLVM JIT execution or object emission and linking. |
| Owned native | NYIR optimization, typed machine lowering, register allocation, and native object or JIT emission. |
| Browser/Wasm | Supported Wasm lowering and explicit browser host imports. |

The REPL's compatibility emitter is a separate incremental path, described
under [Lowering and execution](#lowering-and-execution).

The exact terminal path is selected by the command and backend. `--native-only`
requires the NYIR/native path. On supported hosts, `-run` and `-o` select the
owned native object encoder unless `--native-backend` explicitly chooses
another backend; the browser tools use the Wasm path. Unsupported shapes
diagnose at their owning boundary instead of selecting a different backend
silently.

## Before code generation

Parsing builds the source program after imports and source-unit rules are
known. Compile-time expressions and generated declarations are resolved before
their dependent runtime code is emitted. Type inference, trait/effect facts,
and ABI/layout checks run before code generation. Ownership and provenance
checks are enabled by ownership, borrow, or safe-mode options; they are
separate from ordinary type inference.

### Trait validation

After HM typing, the type pipeline runs trait validation before ABI analysis
(`NY_TYPE_PIPELINE_STAGE_TRAIT` in `src/code/lang/types/pipeline/emit.c`).
Failures report `trait validation failed`.

It checks:

- An operator's target function must exist, take two arguments, and match the
  declared parameter and return types. Duplicate `(operator, left, right)`
  declarations in one scope are rejected.
- User code cannot define duplicate attached methods with the same owner,
  method tail name, and arity. Standard-library delegating duplicates are allowed.
- Binary operators on nominal custom types must resolve to exactly one impl;
  unresolved or ambiguous operators produce diagnostics.

Impl law propositions are checked by the shared trait-validation stage with
the proof policy and compile-time Boolean evaluator, before codegen prepare.
Trait definitions enter the codegen trait list; `impl Trait for Type` method
bodies lower like ordinary functions with trait metadata.

User-facing detail: [Traits and impl](traits.md).

Attributes such as `@inline` are requests evaluated by the relevant optimizer.
They do not bypass type, effect, ownership, ABI, or backend-support checks.

Inlining is bounded deterministically. The small NYIR inliner accepts only
straight-line callees with no nested calls. The general inliner rejects direct
self-recursion and runs at most four fixed-point sweeps; after inlining one edge
of a mutually recursive pair, an inserted call back to the current caller is a
self-call and is not expanded. This prevents recursive expansion from depending
on incidental pass order or unbounded code growth.

## Semantic facts and invalidation

Nytrix deliberately keeps facts in three layers instead of treating every
annotation as interchangeable metadata. The source/type layer owns language
meaning; NYIR owns optimizer facts; machine form owns target/ABI facts. A pass
may derive a stronger fact for its own layer, but it must not write that result
back as a source-level guarantee.

After HM/type checking, the structured source type graph is `ny_type_t`
(`src/code/lang/types/types.h` / `src/code/lang/types/typeinfer.c`). ABI-facing lowering resolves
source type names through `resolve_type_name` / `resolve_abi_type_name` and
caches the resulting carrier types in semantic function/variable records. NYIR
then uses its own typed opcodes and `nyir_type_map_t`; machine lowering consumes
that typed NYIR plus the selected target descriptor. String type names remain a
source/diagnostic boundary, not an optimizer fact format.

| Fact | Primary producer | Main consumers | Invalidated or recomputed when |
| --- | --- | --- | --- |
| semantic type / trait | type inference and semantic checking | call lowering, ABI resolution, specialization | source binding/call shape changes |
| ownership / escape / alias | ownership and function-summary analysis | call optimization, SROA, LICM/vector legality | capture, mutation, unknown/FFI/thread escape changes |
| effects | function-effect analysis and NYIR opcode effects | CSE, DCE, LICM, scheduling | call target or memory/control operation changes |
| range / proof evidence | proof/range analysis, SCEV/IRCE, NYIR analysis | folding, BCE, narrowing, strength reduction | defining value or controlling CFG changes |
| layout / ABI | layout resolver plus target descriptor | call lowering, object emission | target, ABI, or declaration layout changes |
| machine facts | NYIR→machine lowering and regalloc | encoder/object writer | NYIR or target selection changes |

The tooling-only `--emit-artifact` output keeps these source facts in
versioned, pointer-free records. Typed expression facts use
`nytrix.semantic.expression-effects` v1 for the inferred built-in and custom
effect rows of an expression subtree. `inferred_custom_row` contains the
closed custom capability row composed from the call and child rows; it is
null when an open tail or unresolved callback prevents a sound result. The
built-in row remains null when the trusted analysis cannot resolve a child.
Call expressions separately use
`nytrix.semantic.call-effects` v1, which includes resolved signature rows and
callback-contributed effects. A `known: false` record is not a pure row.
Function records also include `nytrix.semantic.statements` v1, containing
source-ordered `nytrix.semantic.statement-effects` v1 entries for supported
statement trees. Unsupported statement kinds are marked unknown; declarations
and control-only statements are explicitly classified as effect-free.
Statement and expression custom rows are composed independently of built-in
mask rows and preserve capability identities from direct calls and known
callback substitutions. These compiler artifacts are
read-only tooling data, not a runtime reflection API or an authority for
optimization.

The NYIR pass manager refreshes instruction metadata after every successful pass
and verification checkpoints reject malformed effects/ranges and broken CFG/SSA
invariants. CFG-changing transforms therefore cannot rely on a range/effect fact
that was merely left attached to an old instruction shape; a pass that needs a
dominator, loop, alias, or range view rebuilds that analysis from the current
function.

Pass statistics retain before/after instruction, SSA-value, and basic-block
counts for each recorded pass, so optimizer growth can be attributed before a
later expensive pass rather than inferred only from the final function size.
Translation validation and verify-each-pass checks are diagnostic modes: they
are disabled unless their command-line controls request them, keeping their
cloning/solver cost off ordinary release compilation.

## Lowering and execution

LLVM builds can optimize one linked module, execute it through the JIT, or emit
an object for the host linker. `-flto` preserves LLVM optimization after
parallel module bitcode is linked.

The native path lowers supported code into NYIR, runs its verified optimization
pipeline, then lowers to typed machine form and emits an object. Native
`-flto` selects the aggressive whole-program NYIR pipeline; it is not LLVM
bitcode LTO. The browser path lowers the supported browser surface to Wasm and
uses explicit host imports.

The CLI uses the NYIR-to-LLVM emitter for LLVM execution and AOT emission.
`--legacy-llvm-dump=PATH` emits the quarantined AST-to-LLVM compatibility
backend's IR for diagnostics without executing it. A NYIR lowering or emission
failure is reported by that pipeline and does not silently retry through
legacy codegen. The REPL still calls the compatibility emitter for standard
initialization and snippet execution; migrating that incremental path is part
of removing the legacy backend from CMake and the compiler.

## Imports, standard library, and caches

Imports are resolved as source units before ordinary declarations need their
names. The selected standard-library source or bitcode is incorporated into the
program and may use a validated cache. Cache keys include source, compiler and
option identity, selected standard-library inputs, and relevant timestamps.

`--parallel=modules` additionally keeps persistent module bitcode under the
Nytrix cache directory. A module key includes the compiler identity, emission
options, the entry source identity, and the content fingerprints of that
module's transitive source-unit dependencies. An unchanged unit is linked from
that cache; changing a dependency gives its dependents new keys and rebuilds
only their module artifacts. Publication is atomic, so an interrupted worker
cannot be reused. Set `NYTRIX_TRACE_CACHE=1` to see `module cache hit` and
`module cache saved` records. The normal single-module JIT path continues to
use its whole-program cache.

Runtime artifacts are rebuilt by the build driver when their owned runtime
sources change. In particular, a change under `src/code/lang/runtime/` such as
`src/code/lang/runtime/core.c` invalidates the runtime build input; this is build-time
dependency tracking, not a runtime recompilation of a shared library.

## Worked trace

Take a minimal program:

```ny
use core

fn square(int x) int { x * x }

assert_eq(square(21), 441, "square")
```

Compile it through each stage:

```bash
ny --dump-ast hello.ny           # parsed source tree; no execution
ny --dump-ast-typed --dump-ast-filter=square hello.ny  # checked declaration
ny -vvvv hello.ny                # full compiler trace; save failure artifacts
ny --dump-on-error --dump-dir=build/debug/case hello.ny
ny --dump-diagnose --dump-dir=build/debug/case hello.ny
ny -dump-llvm hello.ny           # LLVM IR to stderr (flag before the file)
ny --emit-ir=build/trace/hello.ll -emit-only hello.ny   # IR to a file
ny --nyir-dump-cfg --native-only hello.ny               # NYIR blocks
ny --nyir-track-value=12 --native-only hello.ny         # follow SSA value 12
ny -trace hello.ny               # runtime trace
```

Flags come before the file argument. AST dumps stop before execution; the typed
form first validates types and traits. See [AST inspection](../learn/debugging.md#ast-inspection)
for node metadata and filtering. `-dump-llvm` prints LLVM IR; `--emit-ir`
writes it to a path and `-emit-only` stops before execution. `-vvvv` and
`-vvvvv` enable the deepest compiler trace and save artifacts on failure under
`build/debug/`; `--dump-dir` chooses another location. The failure manifest
records the failing stage and which artifacts were actually written. At
`-vvvvv`, optimizer candidate-decision hooks print without per-pass
`NY_TRACE_*` setup; passes without candidate hooks still report their pass
summary and whether the IR changed. A parse
failure saves the input source and removes stale IR from an earlier run.
`--dump-diagnose` writes the input source, pre/post LLVM IR, assembly, bitcode,
statistics, and a summary when compilation succeeds. A failed run refreshes
the source and summary with its exit status; artifacts from an earlier run are
cleared first. On the native path,
`--nyir-dump`, `--nyir-dump-raw`, `--nyir-dump-cfg`, and `--nyir-dump-stats`
add focused diagnostics. `--nyir-track-value=N` follows zero-based NYIR SSA
value `N` across optimization passes and reports its defining operation and
use count, or that the pass removed it; the option enables those logs itself.
Dump and trace runs bypass compilation caches, so do not use them as
performance evidence.

## Related

- [Units](units.md) - imports, modules, and entry points.
- [Compile-time execution](comptime.md) - evaluation and generated source.
- [Native](native.md) - native ABI and machine-form boundaries.
- [Memory](memory.md) - runtime representation boundaries.
