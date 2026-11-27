# Bounded polyhedral scheduling

This is an independent NYIR optimizer; it does not link Polly, ISL, Z3,
LLVM, or another scheduling library. The existing `nyr_affine_nest` remains
available for local fusion and address splitting. The former metadata-only
`nyr_polyhedral_tiling` is replaced by an actual schedule transformation.

## Model and contract

- `model.c`: extract a single-entry, single-exit two- or three-dimensional region with
  constant nonwrapping starts and strides (including descending loops),
  normalized to zero-based ordinals; rectangular or upper/lower unit-stride triangular
  iteration domains (triangular only in 2D), and affine byte addresses rooted in distinct
  dominating stack allocations. Prove that each modeled access fits inside
  its allocation. Reject unsupported control, effects, scalar live-outs,
  unknown addresses, volatile accesses, or noncanonical shapes. Bounded
  symbolic dimensions use dominating SSA bounds with finite range facts,
  constant starts and nonzero constant strides, including descending loops.
  Dynamic triangular domains are supported, including inner starts `j=i`
  and `j=i+1` with a shared zero-based bound. The maximum domain is used
  for legality. `domain.h` computes affine extrema at the actual triangle
  vertices rather than the enclosing rectangle, for extraction, byte-range
  dependence screening and guards. Float constants are supported; f32/f64
  addition, subtraction and multiplication are reordered only under
  NYR's existing explicit fast-math policy. Strict FP is not relaxed.
  Fixed and bounded symbolic 2D/3D raw C pointer-parameter regions can
  optionally use guarded alias versioning; unknown provenance and mixed stack/pointer regions are rejected.
- `schedule.c`: compare iteration orders using a byte-granularity conflict
  engine (RAW, WAR, WAW). On larger domains, use a conservative injectivity,
  disjoint-range, same-slice, congruence, or bounded affine-distance proof.
  Different affine layouts can fall back to a bounded integer relation
  proof or the earlier finite/box search. The cache model
  is a heuristic, not part of the legality proof. Search both loop orders,
  square tiles and three-loop strip schedules, then refine rectangular
  tile sizes. Skip already contiguous inner streams; simulate at most
  64 x 64 iteration points per candidate. Obvious independent, rectangular
  column-major walks use a constant-time interchange fast path. Test symbolic legality before
  materializing the finite event relation; materialize that relation at most
  once, and only for an otherwise profitable unresolved candidate.
  The 3D path supports six axis permutations and explicit cube schedules;
  its large-domain proof recognizes disjoint slabs and a restricted
  same-layout, single-zero-stride memory reduction. Cache scoring reuses precomputed packed access tags in both 2D and 3D
  (at most 4096 sampled points), with 32-bit recency counters and a verified
  last-way hit fast path. The dense object ID and cache set together
  distinguish roots and line addresses without hash collisions. Exact
  score cutoffs and candidate deduplication avoid redundant simulation.
  Independent 3D streams with a clearly contiguous alternative inner axis
  have a narrow default-enabled permutation fast path.
- `relation.c`: model schedule inversions as unions of integer inequalities,
  with explicit quotient dimensions for floor-based tiling. Use GCD
  tightening, interval propagation, capped Fourier–Motzkin projection and
  bounded subdivision. Projection is a relaxation, not exact integer
  elimination; only proven emptiness establishes legality. Exhaustion
  means unknown. The same directed relation supports cross-statement
  fusion and weighted wavefront schedules. A dependence query checks whether
  a schedule band carries any conflict; bounded positive skew search selects
  legal wavefronts with a dependence-free inner band. A reusable permutable
  band certificate proves componentwise monotonicity of original-order
  conflicts; a single proof covers all positive tile sizes and axis orders.
  Failed or exhausted proofs never authorize a schedule.
- `regenerate.c`: generate real point and tile loops, clip partial and
  triangular boundaries, reconstruct nonzero/strided original IVs,
  omit the redundant fourth loop for strip schedules, preserve the original statement order, and verify
  a cloned candidate before committing. Full tiles omit unnecessary min
  guards and the replaced inner IV update is not copied into the body.
  Generate weighted diagonal wavefront loops with clipped bounds; for two adjacent
  equal-domain nests, emit the two bodies in their original statement order
  at each point. Check bridge control flow and both bodies before replacing
  the region. Experimental pointer versioning checks unsigned address-envelope
  overflow and disjointness for all root pairs with a write, and emits the
  original region as a fallback. This is not a noalias assumption. Small
  pure inner-preheader setup is accepted for affine starts; any literal
  integer constant shared with the body is recreated at the new entry.
  Same-point affine reference groups forward full-width raw integer SSA
  loads only when their exact last value is known. Any possibly overlapping
  write kills the forwarding entry (including partial-width writes); stores
  remain observable. Versioned external pointers do not use same-point forwarding.
  The private-recurrence path promotes one invariant full-width stack
  location to a loop-carried SSA accumulator and stores it on exit.
  Fixed-domain external pointer reductions may use the same path under the
  opt-in overflow/disjointness guards. Symbolic rectangular guards use actual
  runtime extents; triangular guards retain conservative maximum envelopes.
  Register blocking can carry up to eight independent accumulators and
  share read-only input loads invariant across the blocked axis. It
  requires a fixed positive domain and proves disjoint lane outputs. Full
  blocks have a separate scalar remainder. Triangular tile bands are
  projected onto their nonempty outer ranges. Experimental packed F64
  SIMD blocking (`NYTRIX_POLY_EXPERIMENTAL_SIMD=1`) accepts only contiguous
  or invariant inputs and contiguous independent output lanes, respects the
  vectorization-disable policy, and requires existing fast-math permission.
  Explicit block widths 1 through 8 can be requested with
  `NYTRIX_POLY_EXPERIMENTAL_PROMOTION=1` and
  `NYTRIX_POLY_EXPERIMENTAL_REGISTER_BLOCK=N`; widths 4 and 8 support
  one or two packed AVX accumulators when SIMD is requested. Wider scalar
  and vector blocks are not selected automatically because they may spill.
  Medium strided integer recurrences can select two scalar accumulators;
  floating-point scalar defaults retain the previous thresholds. On targets
  with the AVX2 vectorization flag, dense fixed-size F64 dot-product nests
  may instead vectorize the reduction axis under explicit fast-math. One
  or two independent vector accumulators are combined once per output;
  complete vector remainders precede at most three scalar iterations.
  `NYTRIX_POLY_HORIZONTAL=0` disables this path; `1` and `2` select the
  respective number of packed accumulators (subject to legality and target
  checks). The four-vector candidate is not supported: native testing
  exposed a machine-code failure for a larger guarded pointer reduction.
  Explicit scalar promotion tuning overrides automatic horizontal selection.
  Both wavefront orientations are supported.
  Halide-inspired compute placement eliminates an entire intermediate stack
  allocation for two or three adjacent, same-point producer/consumer stages.
  This requires full-width matching references, a whole-function SSA escape
  proof, and every pairwise dependence and bridge proof. Deleted stores
  become same-point SSA values; a missing forwarded read aborts the rewrite.
  This is a narrow default for sufficiently large contiguous raw C nests;
  `NYTRIX_POLY_INLINE_STAGE=0` disables it. FP forwarding requires existing
  fast-math permission. The optional `NYTRIX_POLY_EXPERIMENTAL_RECOMPUTE=1`
  path evaluates a cheap integer producer with zero or up to two read-only
  input accesses directly at up
  to three translated consumer coordinates. Only fixed dense rectangular
  producer domains and proven in-domain translated reads are supported.
  Consumer writes to any producer input object prevent recomputation.
  A two-point unit-step sliding window carries one reusable SSA value across
  adjacent inner iterations; `NYTRIX_POLY_SLIDING=0` disables the window.
  This path removes private storage but may increase execution cost. It is
  not general circular-buffer folding, arbitrary compute-at, or parallelism.
- `profile.c`: optional native JSON region diagnostics (`NYTRIX_POLY_DUMP=1`;
  use serial compilation for readable logs). Records parameter SSA IDs,
  normalized domains, ordered statements, byte-address relations, schedule
  and a region fingerprint. Symbolic extents are conservative maxima. This
  is not the OpenScop file format. `NYTRIX_POLY_REPLAY=key:order:tile:inner_tile:wavefront:skew`
  accepts an exact region fingerprint and a bounded schedule override,
  revalidates legality before regeneration, and falls back on invalid or
  stale selections. Identity can suppress optimization for a matching region.
  Placement diagnostics (`NYR_POLY_PLACEMENT`) identify original region keys,
  removed allocation SSA IDs, and point/recompute/sliding placement. These
  are separate from traversal diagnostics; recomputation is not a permutation.
  Fingerprints are not portable across revisions or targets. Offline tuning
  is optional; no empirical search runs during normal compilation.
- `pass.c`: O3 integration before the existing affine-nest and LICM passes.
  A cheap header-shape prefilter in `model.c` avoids building CFG/dominance
  and use-def tables for functions without a potentially eligible nest.
  Reuse analysis across rejected regions and avoid redundant legality checks
  when committing the chosen schedule. General automatic 3D permutation
  selection remains experimental (`NYTRIX_POLY_EXPERIMENTAL_3D=1`); only
  the restricted independent-stream path is enabled by default. Automatic
  general adjacent-nest fusion is also opt-in (`NYTRIX_POLY_EXPERIMENTAL_FUSION=1`):
  measured runtime wins are not yet consistent. Cube tiling and wavefront
  scheduling are explicit-API only. Runtime symbolic nonzero starts or
  nonunit strides are opt-in (`NYTRIX_POLY_EXPERIMENTAL_SYMBOLIC_STRIDE=1`)
  because native profitability is mixed. Pointer alias versioning is opt-in
  (`NYTRIX_POLY_EXPERIMENTAL_ALIAS_VERSIONING=1`), currently tested on x86-64
  raw C fixed and bounded symbolic 2D/3D regions and guarded fixed-domain
  reductions. `NYTRIX_POLY_EXPERIMENTAL_ISOLATE=1` isolates full and partial
  strips for small single-statement 2D rectangular local regions. This is
  opt-in because the duplicated body can increase code size or runtime.

This is not a general Presburger solver or a complete replacement for Polly.
It does not yet extract arbitrary-depth or general multi-statement SCoPs,
arbitrary symbolic bounds, general pointer provenance, general reductions,
general affine schedule synthesis, or parallel execution. Guarded pointer
versioning is deliberately restricted and experimental. The cache parameters
are fixed heuristics, not detected hardware properties. Compile-time budgets
are deliberately finite. The current pass searches and may transform at most four eligible regions
per invocation, skipping each regenerated region before rebuilding analysis.
The box solver has a 65,536-node per-candidate budget, and the affine
distance engine has a separate 32,768-distance budget. The relation solver
limits its rows, projection coefficients and branch-search budget. The
FPL-inspired narrow arithmetic path certifies headroom for all constraint
rows over the initial box; shrinking boxes retain that certificate. It
falls back to wide arithmetic otherwise. `NYR_POLY_FORCE_WIDE` is a
compile-time testing option.

`python3 etc/labs/polyhedral/plot.py dump.log -o region.svg --limit 12`
renders bounded iteration points, tile groups and sampled byte conflicts
from native JSON diagnostics, with no Python package dependency. The plot
is not a proof: symbolic bounds are maxima, only a displayed prefix/slice
is analyzed, and distinct external roots assume the runtime guard.

## References

Original C implementation, informed by public compiler algorithms; no
upstream source code or new runtime dependency incorporated.

- LLVM Polly: https://github.com/llvm/llvm-project/tree/main/polly
- MLIR affine dependence analysis: https://github.com/llvm/llvm-project/blob/main/mlir/lib/Dialect/Affine/Analysis/AffineAnalysis.cpp
- MLIR Presburger relations: https://github.com/llvm/llvm-project/blob/main/mlir/lib/Analysis/Presburger/IntegerRelation.cpp
- ISL: https://libisl.sourceforge.io/ (independent test oracle only).
- PLUTO: https://github.com/bondhugula/pluto (affine scheduling and locality).
- Halide, commit 94258a7: https://github.com/halide/Halide/tree/94258a748ec1cb7ee9fec222b094b1ca19212f1b (`ScheduleFunctions.cpp`: producer realization placement; `Inline.cpp`: pure producer substitution; `SlidingWindow.cpp`: monotone reuse and warmup; `StorageFolding.cpp`: live storage footprints). Independent bounded NYIR implementation, no source imported.
- PPCG, commit 5e07c52: https://github.com/Meinersbur/ppcg/tree/5e07c52e13fc8ea9fac7c1daf247d81d60822a93 (MIT; schedule bands, coordinate normalization, reference grouping, CPU/GPU locality).
- PET, commit c8adf54: https://github.com/Meinersbur/pet/tree/c8adf54b904c7b293d0400d80cb6f21249a5a1cd (domain extraction and bound refinement).
- PRL, commit bf721dc: https://github.com/Meinersbur/prl/tree/bf721dcbe0f7230e86943615638d5896f81a74ab (memory ownership, warmup and timing methodology; no accelerator runtime port).
- PolyBench/C 4.1, commit 9981a18: https://github.com/Meinersbur/polybench/tree/9981a18f648723eb8a7ecfa216f9f49d48f759b3 (numerical kernels and live-out validation; original local fixtures inspired by these algorithms, not the official suite).

- tmLQCD, commit 8bc7dcf: https://github.com/Meinersbur/tmLQCD/tree/8bc7dcf8a486b68077feb87c7cae43d5a284dfde (numerical locality and SIMD stencil patterns; GPL code not imported).
- mctree, commit d23f039: https://github.com/Meinersbur/mctree/tree/d23f0396b0654392c9d3cce5bf90a07f97ec8cd4 (offline transformation search and measurement; no source imported).
- ISL, commit dc16f8e: https://github.com/Meinersbur/isl/tree/dc16f8e3d62c9e808ef86ffe82c2b93ac1446da3 (schedule clustering and dependence constraints; not linked).
- PLUTO, commit 2b6fd99: https://github.com/bondhugula/pluto/tree/2b6fd99c976e4fec340cb227c8048bcd40864755 (affine hyperplanes and dependence satisfaction; no source imported).

- CLooG, commit 059654a: https://github.com/periscop/cloog/tree/059654afb0a7b2716073167423afd06002886cad (domain projection, regular and boundary loop generation; no source imported).
- OpenScop, commit 2f86173: https://github.com/periscop/openscop/tree/2f8617355f095c05ef78cd3762c2fd1cdb9a629f (separate domain, scattering and access relations; native JSON diagnostic subset, not format compatibility).

- Polly development notes: https://polly.llvm.org/docs/ReleaseNotes.html (complete tile isolation and code-size tradeoffs).
- Grosser research: https://grosser.science/ (FPL transprecision arithmetic; independent guarded 64-bit propagation, not a full FPL implementation).
- islplot, commit 496dbf1: https://github.com/tobiasgrosser/islplot/tree/496dbf1175b1bce43dc1fedbdd722ca545f1df2f (domain/map/group visualization; independent SVG diagnostic).

## Validation

`src/code/test/backend/passes/loops/polyhedral_schedule.c` compares the
finite and symbolic dependence engines against a separate all-pairs
oracle, checks large-domain symbolic cases with differing layouts,
regenerates rectangular, triangular, strided, bounded symbolic and 3D loops with partial tiles
and three-loop strip schedules, weighted wavefronts, dependence-free band
queries, runtime triangular/strided loops, and fused adjacent
statements, and tests exact-tile guard elimination and unit-stride profitability,
upper/lower triangular address extrema, strict versus fast-math FP,
permutable bands, same-point forwarding with narrow clobbers, both wavefront orientations,
private recurrence promotion, register blocking with scalar tails, packed
SIMD blocking, horizontal reduction legality and scalar tails, projected triangular bands, schedule replay validation, two/three-stage private storage elimination, shifted producer recomputation and sliding reuse, including observable-storage and unsupported producer rejection. It rejects an in-place transpose with opposing
memory dependences. It is picked up by the existing backend test glob.

For a pass-ablation experiment in NYC, compile the same source at `-O3`
with and without `NYC_DISABLE_PASS=polyhedral_tiling`, then link both with
the same external benchmark driver. Check the results before timing.

The IV-merge pass also bounds its definition-table lookups by the original
SSA snapshot size, preventing out-of-bounds queries on newly staged IDs.

Native AVX2 horizontal-reduction regression: run
`bash etc/labs/polyhedral/test-horizontal-native.sh /absolute/path/to/nyc`
on a host supporting AVX2 with Clang installed. The test compares every
array element against a scalar reference under disjoint and overlapping
arguments; it exercises automatic selection and explicit widths 1 and 2.
