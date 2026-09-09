<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"spec","order":45,"summary":"The Nytrix IR (NYIR) specification: SSA/CFG invariants, verification authority, pass ordering, and optimization presets."} -->
# Nytrix IR (NYIR)

Nytrix IR (NYIR) is the intermediate representation for SSA-based optimization, analysis, and native code generation in Nytrix.

The public interface is defined in `src/code/ir/ir.h`. The executable verifier in `src/code/ir/verify.c` is the authoritative correctness boundary when prose and implementation differ.

## Invariants

1. **Instruction Sequence & SSA Values**: A function owns a contiguous instruction array where `len <= cap`. Value identifiers are non-negative integers bounded by `next_value`.
2. **Single SSA Definition**: Every value is defined exactly once. Every operand use must refer to a dominating definition, except across control-flow joins handled by PHIs.
3. **CFG & Block Structure**: Block labels are unique, branch targets exist, and block terminators match successor edges in the CFG.
4. **PHI Ownership**: PHI nodes appear only at block headers, containing exactly one incoming `(predecessor_label, value)` pair per CFG predecessor.
5. **Memory Ownership**: Instruction-owned payload arrays (`extra_args`, `arg_sizes`, `phi_incoming`) are deep-copied and destroyed using `nyir_inst_discard` or `nyir_erase_instruction`.
6. **Raw Integer Model**: Typed scalar integers are raw 64-bit integers (`i64`). Dynamic `NyValue` tagging occurs only at explicit lowering boundaries.
7. **Derived View Invalidation**: CFG, use-def chains, type maps, and range facts are derived views that must be rebuilt or refreshed after mutating instructions or edges.

## Optimization Presets

Presets are defined in `src/code/ir/opt/pipeline.c`:

| Level | Goal | Description & Passes |
| :--- | :--- | :--- |
| **O0** | Normalize | Compacts instruction streams and lowers PHIs if required by the target encoder. |
| **O1** | Local Cleanup | Constant folding, peephole rewrites, copy propagation, CFG simplification, and dead code elimination (DCE). |
| **O2** | Balanced Optimization | Adds Common Subexpression Elimination (CSE), Dead Store Elimination (DSE), `mem2reg`, Sparse Conditional Constant Propagation (SCCP), function inlining, loop canonicalization, SCEV-lite, Inductive Range Check Elimination (IRCE), and Loop-Invariant Code Motion (LICM). |
| **O3** | Aggressive Native | Preserves SSA through loop analysis, loop vectorization, SLP vectorization, loop unrolling, and scalar/memory cleanup. |

## Pass Ordering Constraints

- **CFG Simplification & DCE** run early so downstream analyses operate on canonical, reachable blocks.
- **`mem2reg`** precedes scalar/loop analysis so SSA PHIs expose value flow to SCCP, CSE, and LICM.
- **Inlining** runs before scalar and loop optimization so newly exposed call boundaries and constants are simplified.
- **Loop Rotation** precedes SCEV and IRCE to guarantee canonical loop headers and induction structures.
- **Vectorization** runs before Induction Variable (IV) elimination because vector patterns require canonical source induction variables and affine memory addresses.
- **Loop Unrolling** precedes SLP vectorization so unrolled independent operations can be packed together.
- **PHI Elimination** is the final stage, executed only for backends that require explicit local-memory form.

## Verification & Tooling

```bash
# Run executable oracle verification
./make ny --native-only --native-result-oracle fixture.ny

# Run per-pass oracle isolation
./make ny --native-only --native-oracle-per-pass fixture.ny

# Dump text NYIR
./make ny --nyir-dump=/tmp/out.nyir fixture.ny

# Verify NYIR verifier during pass development
./make ny --nyir-verify fixture.ny
```
