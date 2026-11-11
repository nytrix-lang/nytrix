<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":3,"summary":"Open compiler, performance, UI, test, and cleanup work with acceptance criteria."} -->
# Open work

An item closes only when its acceptance check has passed on the current tree.
Figures below are dated investigation snapshots, not a substitute for rerunning
the relevant gate on the current tree.
NYC work (all NYC notes/todos) is tracked in [NYC work](todo/nyc.md).

Working rules:

- Compare generated code by instructions executed, then read the assembly.
- A change counts only if checksums still match and the suite stays green.
- Reproduce one benchmark or fixture at a time before changing anything.

## Performance: NYR

- [ ] **At most 1.2x of C on every backend:** no valid, comparable benchmark
  above 1.2x on NYR, NYC, or LLVM. Accept: a fresh complete `./make bench`
  report with valid checksums and stable measurements, plus mixed-assembly
  review for every row above 1.2x. Keep optimization work focused on those
  severe outliers; rows at or below 1.2x are outside this performance pass.

### Gate status (2026-11-11)

Baseline: full `./make bench` report
`build/bench-reports/bench-20261111-012737-2689407.json` (77 fixtures, 3 runs,
1 warm-up). Figures below marked local were measured against
`clang -O3 -march=native`, not by `./make bench`; every fix closes for good
only when the next full report confirms it.

Rows still above 2.0x:

| Fixture | NYR/C | LLVM/C | NYC/C | Evidence and cause |
| --- | ---: | ---: | ---: | --- |
| `page-rank` | 3.83x | 1.58x | 3.85x | `bench-20261111-032403-2923171`. Six-instruction inner loop with `newrank[0]` stored and reloaded every iteration (4-5 cycle store-forward plus add chain); C keeps it in a register. `rank`/`newrank` are swapped top-level globals, so disjointness is unproven. Needs scalar promotion of the read-modify-write guarded by a runtime no-overlap check (loop versioning). Type-based disambiguation via scalar classes was rejected: the C frontend shares NYIR and can take global addresses. |
| `matrix` | 3.4x | 2.08x | 2.9x | Vectorizer rejects the `mul.i64` reduction. There is no `NYIR_VEC8_MUL_I64`: adding one touches about 17 files (IR op, verifier, interpreter `eval.c`, SMT semantics `tv.c`, `portable.c`, LLVM emitter, both machine lowerings, the x86-64 encoder). AVX2 has no `vpmullq`; the lowering is `vpmuludq` on the low halves plus two shifted cross products. |
| `revcomp` | 3.4x | 1.4-2.0x | 6.0x | Not diagnosed. Replacing packed reductions with eight scalar lane extracts regressed NYR 19 to 31 µs (reverted). |
| `dict-oracle` | 3.05x | 3.90x | 1.05x | The fixture has no kernel timer, so its ratio is whole-process time (mostly startup). A timed steady-state loop (one `set` plus three `get` on a persistent dict, 200,000 iterations) measures 560 ns per iteration in Ny against 22 ns for an 8-slot C table: about 140 ns per operation. The profile is flat: `rt_map_oracle_check` 13%, `is_v_str` 9%, `ny_native_dict_get_impl` 9%, `rt_cstr_to_str` 8% (each literal key is converted to a Ny string on every call), `ny_native_dict_find` 7%, `rt_is_sso_string` 7%, heap-registry lookups 5%. Fixed already: a `mincore` syscall per store (157 ms to 23 ms for 20,000 create-and-use iterations). Hoisting the literal conversion needs proof that the interned string stays rooted across the loop. The fixture still needs a timed loop and an unfoldable C reference. |
| `bigint-oracle` | 9.9x | 9.7x | 1.2x | Compares Ny's library bignum against GMP; profile per function before assuming a codegen cause. |
| `bce-matrix` | 1.35x | 2.5x | 3.4x | LLVM and NYC keep a serial reduction dependency. A uniform-state term and four independent accumulators regressed NYC to 4.00x and 4.32x; a pointer loop stopped vectorizing; all reverted. |
| `fft` | 1.08x | 2.04x | 1.09x | Only the LLVM route is above the gate; not diagnosed. |

NYC rows still above 2.0x (local `nyc -c -O3`, best of 11, checksums match):

| Fixture | NYC/C | Cause |
| --- | ---: | --- |
| `pbkdf2` | 2.09x | Scalar SHA-256 rounds; see `sha256`. |
| `json-parser` | 2.03x | Not diagnosed. |
| `string` | 1.47x | Small-copy lowering and callee-saved residency are implemented. Seven-run focused report `bench-20261111-083623-3891112.json` reports matching checksums and `ok`, down from 3.04x in the last full run. The current 1.2x target remains open; confirm in the next full run. |
| `append-scaling` | 2.34x | Not diagnosed. |
| `sha256` | 2.56x | 8 rotating state words spill in the round loop; clang keeps them in registers. |
| `pidigits` | 2.64x | Not diagnosed (14.8 µs against 5.6 µs). |
| `bce-matrix`, `matrix`, `linpack`, `licm`, `revcomp`, `page-rank` | 3.0x, 3.0x, 2.6x, 2.8x, 4.2x, 3.8x | The NYC vectorizer does not widen these; same causes as the NYR rows above. |

`currying` is excluded: its C timer measures 3.3 µs against 5.27 ms for NYR.

Open follow-ups from the 2026-11-11 performance work:

- The full report `build/bench-reports/bench-20261111-082901-3859446.json`
  confirms `splay`, `spectral`, and `licm` below 2.0x on every route.
  `linpack` clears NYR (1.20x) and LLVM (0.84x), but NYC remains 2.28x.
  The NYC vectorizer still rejects the row
  update (needs row-dependence analysis for `A[r*N+c]` against `A[k*N+c]`,
  `r > k`).
- The carry/coalescing defect behind the allocator verifier rejection
  ("carried v29 / v4 color mismatch") is worked around by a retry that drops
  carried registers, not fixed.
- Flags reuse (`mach_cmp_reuse_alloc_view`) is off behind `NY_X64_CMP_REUSE`:
  its stripped view fails the machine verifier, and enabling it without
  verification miscompiled `splay`. Needs a correct view and a fixture.

### Open performance work

- [ ] **Diagnose `revcomp`, `dict-oracle`, and the undiagnosed NYC rows (`json-parser`, `append-scaling`, `pidigits`):** record timed-kernel
  instruction counts, vector lanes, spills, and the vectorizer rejection reason
  (`NY_TRACE_VECTORIZE=1`), then choose a change.
- [ ] **Pointer induction variables for NYC array walks:** verify generated
  code and affected workloads against the 2.0x gate.
- [ ] **Allocator and encoder traffic:** re-measure the older 12% load / 8%
  store stack-slot traffic and `rax` staging moves on the current tree before
  choosing three-register forms, boundary home publishing, or shrink-wrapped
  callee saves. The opt-in MIR address-reassociation probe (`NYTRIX_ADDR_REASSOC=1`)
  rewrites `cmov-sort`'s `add index, 1; lea base, index, 8` to reuse the existing
  `base + 8`, but currently makes x86-64 register allocation fail and falls back
  to a slower path (about 3.76 ms vs 0.50 ms without the probe). Keeping the ADD
  intact still fails, so deletion of that instruction is not the cause. Keep
  this transformation disabled by default until the cross-block live-range /
  carried-register issue is fixed and the focused benchmark clears its gate.

---

### Measurement Discipline for Every Fix

1. Run `./make bench` (77 fixtures, 3 runs + 1 warm-up) — all checksums must match
2. For each changed fixture: capture mixed assembly (`--bench-show-asm --bench-compare-llvm`)
3. Compare dynamic instruction counts, vector lanes, spills/reloads, branches in timed kernel
4. Recheck all >2.0x rows after shared optimizer/encoder changes (ranking moves)
5. Only review assembly for rows above 2.0x; exclude documented timer-floor rows using benchmark rules

---

### UI and engine

- [ ] **Visual confirmation of the editor and engine on a real compositor.**
refiine all hud edt engiine style features and how works and all more thiinked wiht a lot more detaels and polsiehd
  Gizmo picking, the context menu, text editing, undo/redo, panel layout, resize
  and the TTY backend are covered by headless probes (`gizmo-pick`,
  `context-menu`, `editor-history`, `editor-edit`, panel and resize sweeps).
  Pixel-level polish of gizmo drawing, menu rendering and engine text needs a
  person to look at a live window; it cannot be judged from headless logs.
