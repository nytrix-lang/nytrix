<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"spec","order":390,"summary":"NYIR SSA and CFG invariants, verification, optimizer diagnostics, and native lowering."} -->
# Nytrix IR (NYIR)

> **Note:** Usually needed only for compiler work or when diagnosing a
> backend-level difference; use source-level specifications for program rules.

NYIR is the compiler's SSA representation for target-independent analysis and
optimization. The public C API is declared in [`nyr.md`](nyr.md) and
`src/include/nyr/`. The verifier in `src/code/backend/core/verify.c` is the
executable authority for IR validity.

## Contents

- [IR invariants](#invariants)
- [Optimization and lowering](#optimization-and-target-lowering)
- [Inspect and verify](#inspect-and-verify)

## Invariants

- Each value is defined once and used only where its definition dominates the
  use; control-flow joins use PHI inputs.
- Block labels are unique, branch targets exist, and terminators agree with CFG
  successors.
- PHIs appear at block headers and have one input for each predecessor.
- Instruction-owned payloads are copied and released with the public
  instruction lifecycle helpers.
- Typed scalar integers use raw 64-bit values. Dynamic tagging occurs only at
  explicit representation boundaries.
- CFGs, use-def chains, type maps, and range facts are derived state. Rebuild or
  invalidate them after changing instructions or edges.

The verifier checks structural and operation contracts. It does not prove that
a source-level ownership or effect claim was justified; those checks belong to
shared semantic analysis.

## Optimization and target lowering

The NYIR pipeline has optimization levels selected by the compiler. The exact
pass list and ordering are implementation details owned by
`src/code/backend/passes/`; inspect that source or the compiler's pass
statistics when changing the pipeline. A transform must preserve verifier
validity and observable program behavior.

Machine lowering, register allocation, and object emission happen after
NYIR optimization. Target-independent transforms do not select target registers
or calling conventions. `-flto` on the native path selects the
whole-program NYIR pipeline; it is not LLVM bitcode LTO.

## Inspect and verify

Use `ny --help` for the exact option surface. Useful diagnostics include:

```bash
ny --native-only --nyr-check=verify file.ny
ny --native-only --nyr-dump-to=build/trace.nyr file.ny
ny --native-only --inspect file.ny
ny --native-only --nyr-track-value=12 file.ny
ny --native-only --regalloc-heat file.ny
```

`--nyr-check=verify` checks optimized IR after passes. `--inspect` reports
per-pass changes and timing; `--nyr-track-value` follows an SSA value; and
`--regalloc-heat` reports machine register pressure and spills. Use
`--nyr-run` only when investigating the diagnostic VM route, not as proof of
native or LLVM behavior.

## Related

- [Pipeline](pipeline.md) for stage ordering, caches, and execution routes.
- [Nyr](nyr.md) for the public construction and object API.
- [Memory](memory.md) for value representation boundaries.
