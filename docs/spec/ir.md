<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"spec","order":45,"summary":"NYIR SSA and CFG invariants, verification, optimizer diagnostics, and native lowering."} -->
# Nytrix IR (NYIR)

NYIR is the compiler's SSA representation for target-independent analysis and
optimization. The public C API is declared in [`nyr.md`](nyr.md) and
`src/include/nyr/`. The verifier in `src/code/backend/ir/verify.c` is the
executable authority for IR validity.

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
`src/code/backend/ir/opt/`; inspect that source or the compiler's pass
statistics when changing the pipeline. A transform must preserve verifier
validity and observable program behavior.

Machine lowering, register allocation, and object emission happen after
NYIR optimization. Keep target registers and calling conventions out of
Target-independent transforms. `-flto` on the native path selects the
whole-program NYIR pipeline; it is not LLVM bitcode LTO.

## Inspect and verify

Use `ny --help` for the exact option surface. Useful diagnostics include:

```bash
ny --native-only --nyir-verify file.ny
ny --native-only --nyir-dump=build/trace.nyir file.ny
ny --native-only --nyir-pass-stats file.ny
ny --native-only --nyir-track-value=12 file.ny
ny --native-only --regalloc-heat file.ny
```

`--nyir-verify` checks optimized IR after passes. `--nyir-pass-stats` reports
per-pass changes and timing; `--nyir-track-value` follows an SSA value; and
`--regalloc-heat` reports machine register pressure and spills. Use
`--nyir-run` only when investigating the diagnostic VM route, not as proof of
native or LLVM behavior.

For stage ordering, caches, and execution routes, see
[Pipeline](pipeline.md). For the public construction and object API, see
[Nyr](nyr.md).
