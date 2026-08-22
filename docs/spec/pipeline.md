<!-- nytrix-doc: {"audience":"user","featured":true,"group":"spec","order":15,"summary":"How source becomes a checked program, native artifact, JIT execution, or WebAssembly."} -->
# Pipeline

This page describes the compiler and runtime path for a Nytrix program. It is
an implementation map, not a promise that every source shape is accepted by
every backend.

## Stage map

For `ny file.ny`, the normal path is:

```text
source units + selected stdlib
  -> parse and source expansion
  -> symbol resolution and type/effect checks
  -> ownership/provenance analysis
  -> LLVM IR -> JIT execution or object -> linked executable
                 \
                  -> NYIR -> machine form -> native object
                  -> Wasm for the browser target
```

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
(`src/code/types.h` / `src/code/typeinfer.c`). ABI-facing lowering resolves
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
sources change. In particular, a change under `src/rt/` such as
`src/rt/core.c` invalidates the runtime build input; this is build-time
dependency tracking, not a runtime recompilation of a shared library.

## Worked trace

Take a minimal program:

```ny
use std.core

fn square(int x) int { x * x }

assert_eq(square(21), 441, "square")
```

Compile it through each stage:

```bash
ny -dump-ast hello.ny            # parsed structure
ny --dump-on-error hello.ny      # source/IR artifacts written on failure
ny -dump-llvm hello.ny           # LLVM IR to stderr (flag before the file)
ny --emit-ir=build/trace/hello.ll -emit-only hello.ny   # IR to a file
ny --nyir-dump-cfg --native-only hello.ny               # NYIR blocks
ny -trace hello.ny               # runtime trace
```

Flags come before the file argument. `-dump-llvm` prints LLVM IR; `--emit-ir`
writes it to a path and `-emit-only` stops before execution. On the native
path, `--nyir-dump`, `--nyir-dump-raw`, `--nyir-dump-cfg`, and
`--nyir-dump-stats` add focused diagnostics; they change observability and can
disable cache reuse, so do not use a trace run as performance evidence.

## Related

- [Units](units.md) - imports, modules, and entry points.
- [Compile-time execution](comptime.md) - evaluation and generated source.
- [Native](native.md) - native ABI and machine-form boundaries.
- [Memory](memory.md) - runtime representation boundaries.