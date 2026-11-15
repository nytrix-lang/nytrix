<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":4,"summary":"Open NYR and compiler build-performance work with acceptance criteria."} -->
# Performance work

An item closes only when its acceptance check has passed on the current tree.
Historical measurements are investigation snapshots, not gate evidence.

Working rules:

- Compare generated code by instructions executed, then read the assembly.
- A change counts only if checksums still match and the suite stays green.
- Reproduce one benchmark or fixture at a time before changing anything.

## NYR

- [ ] **At most 1.2x of C on every backend:** no valid, comparable benchmark
  above 1.2x on NYR, NYC, or LLVM. Accept: a fresh complete `./make bench`
  report with valid checksums, stable measurements, and mixed-assembly review
  for every row above 1.2x.

### Fixed 2026-11-13 (focused fixtures, private build)

Each of these was a hard failure in `./make bench` and is now covered by a
fixture or a bench row that passes.

- `binary`, `iter`, and `pbkdf2` no longer fail NYR emission or checksum. The
  type-map flag regions in `ir/init.c` indexed the used-class mask by array
  position instead of class bit, so a used 256-bit vector class shared the
  "unused" region and was overwritten with false; a vec8 value then looked
  128-bit and failed machine lowering.
- NYC `bce-matrix` crashed at `-O3`: a vector load whose address ADD was folded
  into a pending SIB read an unwritten spill slot
  (`code/abi/calls/nyc-vector-load-folded-address`).
- NYC `dict-set` crashed at every level: the struct-copy intrinsic read both
  pointers from stack homes while only RSI/RDI were flushed
  (`code/abi/calls/nyc-struct-copy-computed-address`). `fannkuch` passes on the
  same build (its earlier timeout did not reproduce).
- `inline` was 92x C on NYR and NYC: strength reduction turned `s += i * c` into
  a second induction variable that `iv_elim` did not recognize. It now folds to
  the closed form (2.2 ms to 0.09 ms;
  `code/intermediate/ny/loops/ivelim-secondary-iv-accumulator`).
- A vector reduction accumulator was not published at the exit of a zero-trip
  vector loop, so `result += invariant` over fewer than four iterations read
  stack garbage (817 x 3 gave 2491;
  `code/intermediate/ny/loops/vector-reduction-zero-trip-exit`).
- `knight` and `queen` were 20 to 100 microsecond workloads and measured
  NOISY. They now repeat the search with a result-dependent start so no compiler
  can hoist it, and print their own elapsed time.
- `iter`'s vector reduction accumulator spilled on every iteration because
  loop-phi edge carry rejected an edge block with multiple predecessors even
  when the copy source was defined in that block. `regalloc.c` now carries
  that source from the edge segment. The accumulator stays in `ymm10`
  (`vpaddq ymm10, ymm10, ymm9`) and checksum `400020000` is unchanged. Vector
  phi elimination also publishes the `MAY_TRAP` effect bits the verifier now
  requires (`phi_elim_deep_vec8`).

### Assembly and code-size audit (2026-11-15)

- All 77 bench sources now have a `--bench-show-asm` record in the
  [assembly audit](../learn/asm-audit.md): 91,714,429 bytes of emitted x86-64
  code, median 26,381 bytes per fixture. The largest programs are generated
  fixtures (`pbkdf2` 5.6 MB, `sha256` 5.6 MB, `inline` 5.4 MB).
- The cost is MACH-to-x64 expansion, not a missed opcode idiom. `ny_fn_moves`
  in `knight` is 188 MACH instructions and 498 x64 instructions; 199 of those
  reference `(%rbp)` homes. Zero tests already use `test %reg,%reg` and
  immediates already take the compact forms, so `cmp $0` / `mov $0` rewrites
  have no targets.
- Fixed 2026-11-15: the 37 reversed register pairs in `knight` came from the
  scalar COPY path staging an equal-color copy through `%rax`. The guard now
  accepts equal colors, so `ny_fn_moves` is 418 x64 instructions (was 498) and
  5,016 bytes of text (was 5,240) with checksum `340233` and neutral timing.
  `NYTRIX_NO_SAME_COLOR_COPY=1` restores the old path.
- Open: single-use copy chains that still stage through `%rax` on the
  arithmetic path, and a MACH-level copy-forwarding pass for the remaining
  `copy a,b; copy b,a` shapes outside the scalar COPY emitter.

### Fixed 2026-11-15: i386 object writer stops paying for disp32 everywhere

The i386 ELF object writer emitted every `(%ebp)` home access as mod=10 disp32
and every frame reservation/adjustment through the `0x81` imm32 group. It now
uses the disp8 form for offsets in `[-128,127]` (ModRM built from the register
field; offset 0 still needs mod=01 because base=EBP has no no-displacement
form) and the imm8 form for `sub $frame,%esp` (frame <= 127) and post-call
`add $stack,%esp` (stack <= 127). `NYTRIX_NO_I386_DISP8=1` restores the old
memops for A/B.

Measured with `--native-backend x86 --native-only -O3 --nyr-verify -emit-only
-o out.o` and `objcopy -O binary --only-section=.text | wc -c`:

| input | before | after | delta |
| --- | --- | --- | --- |
| `knight` | 8,379 | 7,909 | -470 (-5.6%) |
| recursive `fib(12)` via `-e` | 1,036 | 804 | -232 (-22.4%) |

`knight`'s 616-byte frame is out of imm8 reach, so its gain is purely the disp8
memops; the small recursive fixture shows the combined effect. `--pattern i386`
13/13 and `--pattern elf32` 45/45 pass, and the ELF32 link-run fixtures link the
emitted objects with clang and execute them under qemu, so the new encodings are
verified by running, not just by assembling.

A related generalization was tried and reverted: letting
`mach_vreg_home_colorable` share a home across the union of a value's resident
segments is safe but changed nothing on `knight` (5,016-byte `.text`, checksum
`340233`, stack-relative ops 199 -> 206, all still disp8), because the values
that stay resident are already register-allocated and the shareable remainder
are cold spill slots. See `learn/measurements.md`.

### Fixed 2026-11-15: spill-slot reuse in the x64 home layout

`mach_build_vreg_homes` reserved a unique frame slot for every value that was
not a single-block resident, which is the spilled population that dominates the
frame. `mach_vreg_home_spill_interval` now colors spilled values by the union of
all their live segments (GPR class, 8-byte home, must touch its home), so
values with disjoint live ranges reuse one slot while overlapping ones keep
separate slots. Incoming-argument homes and floating-predicate GPR results stay
unshared, as in the resident path. `NYTRIX_NO_SPILL_HOME_COLOR=1` restores one
slot per value.

| fixture | `.text` | frame bytes |
| --- | --- | --- |
| `knight` | 5,016 -> 4,688 (-6.5%) | 616 -> 488 |
| `compose-local` | 7,040 -> 6,800 (-3.4%) | 632 -> 488 |
| `combs-test` | 3,152 -> 3,152 | 88 -> 72 |
| `eq-test` | 3,928 -> 3,912 | 136 -> 136 |

`knight` checksum `340233` is unchanged. `--pattern x86` 223/223, `regalloc`
11/11, `codegen` 15/15, `copy` 8/8. The full `--bench-correctness` sweep is
2,357/2,366, and all 9 failures (`doc-rank`, `doc-meta`,
`sanitizer-thread-race`, `bare/asm-artifacts`, `terminal`,
`vm-disasm-artifact-diff`, `comptime-contracts`, both parser
`invalid-byte-stops` fixtures) fail identically with
`NYTRIX_NO_SPILL_HOME_COLOR=1 NYTRIX_NO_SAME_COLOR_COPY=1`, so they are
pre-existing.

### Kept 2026-11-15: every x64 frame ModRM goes through the range-checked helper

Eleven hand-rolled `0x85 disp32` frame accesses now use `mach_rbp_disp`, and the
struct-argument staging `lea disp(%rsp), %rdi` uses a reg-aware
`mach_rsp_disp`: aggregate-param home `lea`, SSE scalar op with a memory
operand, v256 `ymm` load/store, packed add/sub/and/or/xor, x87 `fld`/`fstp` for
sin/cos, `imul` and ALU with a memory operand, `cmov`/`cmp` with a memory
source, and `cmpq $0, mem; jne` (which now also keeps the `83 /7 ib` imm8:
`48 83 7d d8 00` instead of `48 83 bd <disp32> 00`).

`NYTRIX_NO_FRAME_DISP8=1` forces disp32 from both helpers, so the whole frame
encoding is A/B-able from one binary. On `knight` the hatch costs 424 bytes
(5,112 vs 4,688): that is the cumulative value of frame narrowing, 8.3% of
`.text`.

The converted sites are cold in the fixtures that build today, so `.text` is
byte-identical to the pre-change encoder on `knight`, `compose-local`,
`combs-test` and a small float kernel. The open opportunity is therefore not
more encoding work but slot ordering: 303 of `knight`'s frame accesses still pay
disp32 only because their offsets sit below -128 bytes, so ranking homes by
access frequency and packing the hottest into the low 128 bytes would convert
them.

### Kept 2026-11-15: hot-slot ordering puts the busiest x64 homes in disp8 reach

Frame narrowing only pays for homes inside the 128 bytes below `%rbp`, and the
coloring pass allocates in live-range order rather than access order. New
`mach_order_hot_homes` counts MACH operand references per vreg, but only for
vregs that actually reach a home, groups the vregs that share a slot, and
permutes the offsets of equal-width slots so the most-used homes sit closest to
`%rbp`. Slots keep their byte ranges, widths and proven disjointness, vector
homes are untouched, and the frame size does not change.

| fixture | allocation order | hot ordering |
| --- | --- | --- |
| `knight` | 4,688 | 4,560 (-2.7%) |
| `compose-local` | 6,800 | 6,712 (-1.3%) |

`NYTRIX_NO_HOT_HOMES=1` restores allocation order for A/B runs. Weighting every
operand reference instead of only home-using vregs cost `unicode-identifiers` 8
bytes, because resident values have many operands and no home traffic.

The remaining ceiling is the low 128 bytes themselves. Measured layout on the
two hot fixtures: `knight` has no frame slots at all and `compose-local` none
reachable, so every offset from 48 (resp. 24) down is a vreg home; the window
opens at `8 * cs_slots` because the callee-save slots sit directly under `%rbp`.
That leaves 80 (resp. 104) bytes, i.e. 10 (resp. 13) homes, and both are now
filled with the hottest values: `knight` has 178 of 209 frame accesses in disp8
reach (was 132), `compose-local` 173 of 197 (was 166). Ordering frame slots would
buy nothing here; the next lever has to come from non-frame encoding.

### Kept 2026-11-15: x64 missed-optimization audit (`NYTRIX_AUDIT=1`)

The encoder now emits one stable line per function naming what it did and what it
left on the table: `lea_copy`, `self_move`, `spill_pair`,
`fallthrough_elided`, and the decision counters `lea_color` / `lea_staged`
(symbol addresses materialized in the destination color versus staged through
`%rax`). The decision counters are the point: they attribute a size delta to a
transformation instead of leaving it to be inferred from assembly.

| fixture | in-color | `lea_staged` | `NYTRIX_NO_LEA_COLOR=1` | `.text` |
| --- | --- | --- | --- | --- |
| `knight` | 87 | 0 | 87 | 4,320 vs 4,584 (-264) |
| `compose-local` | 93 | 3 | 96 | 6,424 vs 6,680 (-256) |

`knight`'s checksum `340233` is identical in both encodings, so the correction is
verified by behaviour. `self_move=0` and `spill_pair=0` are the expected end
state of the same-color copy fold and the frame-slot work; `lea_copy=0` shows the
single-use-address-plus-COPY pattern does not occur, so the fold written for it
was measured at zero candidates and reverted (its counter stays as the evidence
for re-adding it).

The audit is opt-in and reads its environment once per function; with it unset
the pass does not run at all. Wiring note for future instrumentation: this
encoder has two encode entry points (`mach_encode_function` and the home-DSE
analysis), and an audit placed only in the fallback branch silently reports
nothing.

Open: correlate with Linux `perf`/hardware counters rather than code size alone,
fold the counters into the JSON report, add MIR-to-source-line attribution using
the existing `inst_offsets` and debug lines, and rank the findings so the largest
recoverable byte count is named first.

### Kept 2026-11-15: e-graph saturator gets hash-consing, batched rebuild, guarded arithmetic

The mid-pipeline saturator (`passes/scalar/egraph.c`) interned nodes with a
linear scan and closed congruence by comparing every pair of nodes each
iteration — quadratic in the block. Interning now probes an open-addressing
table keyed by `(op, imm, canonical children)`, closure is one egg-style
`eg_rebuild` per iteration, and union-find merges by rank.

On top of that, class non-negativity is a fixpoint (`and` inherits it from
either operand, COPY forwards it, constants carry it), which justifies rewrites
that are not equalities: `x / 2^k -> x >> k` and `x % 2^k -> x & (2^k-1)` for
`x >= 0`, with a nonzero constant divisor so no trap moves. DIV/MOD are not
e-nodes (they may fault), so that rewrite is planned per instruction against
the dividend's class rather than through extraction — the regression driver
caught the first attempt silently never firing. `x&x` and `x|x` became
unconditional rules.

`ny --backend-test=passes/scalar/egraph` passes, including the new cases that
assert the guard fires for `band(x,255)/8` and does *not* fire for an unknown
sign; the 256-seed randomized equivalence check still passes. `.text` is
byte-identical with the pass on, guarded rules off, and pass off across
`knight`, `compose-local`, `unicode-identifiers`, `combs-test` and `eq-test`,
and 120-op straight-line compile time is 543-550 ms either way. The pass runs
only on loop-free functions, and earlier passes already remove the division
there, so this round buys compile time and rule correctness rather than size.

Open: a real directional `<=` relation with per-argument variance and
refinement-aware extraction, richer class facts (known bits, ranges,
alignment, provenance), register-pressure-aware conversion cost, and SMT
discharge of the guarded rules instead of hand arguments.

### Kept 2026-11-15: symbol addresses go straight to the destination color

169 of `knight`'s 175 LEAs were `leaq sym(%rip),%rax`, one per address-of-global
use, and 87 of them were immediately followed by a three-byte move of that RAX
result into the register the value actually needed. That move was
`mach_commit_vreg` staging, not a machine COPY. The symbol path now emits into
the assigned color directly, as the indexed-LEA paths in the same case already
did:

| fixture | RAX staging | in-color |
| --- | --- | --- |
| `knight` | 4,584 | 4,320 (-5.8%) |
| `compose-local` | 6,680 | 6,424 (-3.8%) |
| `unicode-identifiers` | 2,640 | 2,384 (-9.7%) |
| `combs-test` | 3,160 | 2,888 (-8.6%) |
| `eq-test` | 3,928 | 3,656 (-6.9%) |
| `unicode-debug7` | 3,296 | 3,032 (-8.0%) |

`NYTRIX_NO_LEA_COLOR=1` restores RAX staging and reproduces the old sizes exactly,
so every row is A/B-able from one binary. Checksum `340233` unchanged; `--pattern
x86` 223/223, `regalloc` 11/11, `copy` 8/8, `codegen` 14/15; full sweep
2,290/2,370 with the same 80 load-induced failures as the baseline binary.

Audited and rejected on the way, so they are not worth re-trying blindly: loop
alignment padding is at most 7 bytes per run (a max-skip policy saves zero); only
1 of `knight`'s 67 branches fits rel8; all 169 symbol targets are distinct, so
there is no duplicate address to CSE; and the hot fixtures have no frame slots to
reorder. The next lever is the 22/35 surviving LEA+move pairs, where the LEA's
own color is RAX and the following move is a genuine COPY of a single-use
address.

### Current evidence - still open

- The complete 2026-11-13 run (`./make bench --jobs 4 --bench-run 5
  --bench-warmup 2`) matched all 77 checksums and had no execution failures.
  It still had 14 unstable rows, a `gcbench` budget regression, and 60 fixtures
  with at least one valid backend/C ratio above 1.2x. This was a loaded-host
  snapshot before the subsequent scalar peephole and C timer-wrapper fixes,
  not a pass for the current tree. See
  [the investigation record](../learn/measurements.md#runtime-gate-investigation-2026-11-13).
- `page-rank` returns checksum `84250`; the timed kernel is about 3.8x C
  (1.845 ms versus 0.484 ms with 100x the iterations).
  `NY_TRACE_VECTORIZE=1` rejects the f64 update loop because the accumulator is
  a memory read-modify-write (`newrank[0]`), not an f64 PHI reduction, and the
  swapped `rank`/`newrank` buffers are not proven non-aliasing.
- `knight`'s C reference now uses the same arithmetic move calculation as Ny;
  the previous lookup-table comparison was not like for like. The current
  `ny-test` runner passes the aligned checksum fixture, the documented rebuild
  failure no longer reproduces, and a fresh NYR/C row is recorded in the
  2026-11-15 progress note.
- `currying` now has matched per-iteration work on both sides: the Ny loops
  publish through a global slot as the C loops do, so the remaining ratio is
  closure-call cost rather than a fixture artifact. The 2026-11-15 progress
  note records the per-loop kernel times; inspecting closure allocation and
  indirect calls is still open work.
- `iter` is 1.30–1.38x C in the `ny -run` harness but 1.08–1.12x C when the
  same fixture is built as a standalone AOT executable. The timed loops fault
  the same number of pages in both; the harness penalty tracks the compiler's
  roughly 25 MB resident set, and pre-touching that much memory in the
  standalone process reproduces it. The row still blocks the gate until the
  harness measures a small process. Its reverse loop is scalar because the
  affine vectorizer rejects negative strides; C emits a `vpermq` reverse load.
- Single-call microbenchmarks (tens of microseconds) are dominated by process
  and page-fault cost; do not use them as gate evidence.
- No fresh full `./make bench` has been run on an idle host: the load average
  during this work was 50 to 70 from other sessions, so wall-clock ratios from
  this machine are not gate evidence.

### Progress 2026-11-15 (fresh bench rows, private build)

- `knight` builds and measures again. The documented rebuild failure
  (`src/code/web/core.c` undeclared variables) did not reproduce: `ny-doc`
  builds and `./make bench knight` runs the rebuilt toolchain. Fresh row: NYR
  1.68x to 2.29x C across repeated runs (noise 0.4% over 3 samples), NYC 1.62x
  to 1.64x, checksum `340233` matching on NYR/C/NYC. The row is still above the
  1.2x gate and needs the same treatment as the other >1.2x rows.
- `knight` select folding. Its body is fully unrolled, but every ternary in the
  expanded loop (`dx`, `sign`, `magnitude`, and the two arms of `dy`) has an
  induction-value predicate, and nothing folded a `SELECT_I64` whose condition
  was already a constant. `select_chain_fold` now folds that to a copy of the
  taken arm (R0) after releasing the dead arm, and `peephole` does the same for
  conditions it has proven constant by the end of the pipeline. `ny_fn_moves`
  fell from 565 to 498 instructions with no conditional moves left. The NYR row
  moved from 1.75x to 1.59-1.62x C (2.33-2.38 ms against C 1.47 ms, 5 runs,
  1.9-2.3% noise), spills/reloads from 485/903 to 382/800, and checksum
  `340233` still matches on every route. The residual gap is home traffic: 199 of the 498
  instructions touch `(%rbp)` homes because cross-block GPR residency is opt-in
  and loop-only (`NYTRIX_GPR_LOOP_CARRY`), so `x`, `y`, `nx`, `ny`, and `total`
  reload at each unrolled block boundary. Eight compares also cannot use the
  flags-fused branch because their condition value is live-out. Relaxing the
  acyclic unchanged-value carry to allow call-spanning regions in callee-saved
  colors was measured and reverted: it accepted 478 values but left the object
  unchanged (`x` and `y` were already resident, and the rest is join-boundary
  traffic). The next step is acyclic PHI/join coalescing, not another peephole;
  see the 2026-11-15 measurements.
- `collatz` verifies with the mask-test fusion: NYR 96µs (2.66x C), LLVM 53µs
  (1.46x C), NYC 45µs (1.24x C), checksum `178` on every route.
- `currying` fixture parity. Both sources now publish each iteration through a
  global slot (`keep`), so the C reference no longer measures a closed-form
  loop against a real one. Measured kernels: C 982/921/932ns for
  direct/residual/factory; Ny AOT 2224/541968/2413129ns. Direct-call cost is
  about 2.3x C; the residual and factory loops remain closure-call bound at
  roughly 270ns per call, which is the effect the row is meant to expose, not a
  fixture artifact.

### Fixed 2026-11-15: LLVM rows segfaulted on core-importing fixtures

`knight` and `currying` reported `llvm_rc=139` at `-O3` while standalone runs
of the same materialized source exited 0. The pinned stdlib bitcode was the
cause, not MCJIT: `./make bench` reused an existing `build/release/core.bc`
without refreshing it, and the JIT cache key hashed only the artifact _path_,
so modules linked against an older `core.bc` were reused after it was rebuilt.
Those modules stored to core globals the stale bitcode no longer defined,
which resolved to address 0 at the top of `_ny_top_entry`. The bench now
builds `core_bc` before pinning it, and `ny_jit_cache_path` folds the pinned
artifact's size and timestamp into the key (`NY_JIT_CACHE_VERSION` 33
invalidates older entries). After the fix `./make bench knight -O3` passes:
NYR 2.56ms (1.75x C, 2.00x LLVM), LLVM 1.28-1.31ms (0.88x C), NYR 1.64x NYC,
checksum `340233` on every route, three runs agreeing within 0.4%.
- `currying` measures its LLVM row (2.36ms) but still fails the 4.0x NYR/LLVM
  ratio: a 20000-iteration micro-benchmark puts its indirect closure call at
  about 588ns while the same loop's direct call costs about 2ns. The
  adapter rail is not the bulk of it. Closure ABI flags latch once at
  creation, so a call-site re-mark is a no-op; `ffi.c` now answers those
  re-marks and the per-call flag lookup from a memoized resolution instead of
  taking the callable lock, which moved the row from 688x to 668x NYR/C. What
  the remaining ~580ns is not: not a syscall (`strace -c` shows 2502 calls
  total, all during compile), and not the generated trampoline, which is three
  instructions. `perf` is denied here and the emitted artifact is a relocatable
  object, so the cost is still unattributed; attribute it before changing the
  adapter again.
- The bench decoder oracle reports a text divergence on SIB operands
  (`[rdi*1+r9]` internal versus `[r9+rdi]` objdump) and an internal decode count
  of 1102 against objdump's 1112 for `knight`; `collatz` shows the same operand
  ordering difference. The check is advisory today (a row still passes when the
  checksums agree), but the count gap means the in-tree decoder still misses
  about ten instructions in that object.

### Next work

- [ ] **Optimize measured >1.2x rows (first priority).** Review every route in
  the complete report, using matched executed instructions and mixed assembly.
  The large shared NYR/LLVM `currying` gap, and NYC vector/memory rows need
  investigation alongside `page-rank`: prove an f64 reduction accumulator or
  use a runtime non-overlap check to enable loop versioning, then re-measure.
  Re-measure every other row in a fresh full report before adding it here; do
  not reuse the stale `append-scaling` figures.

  `collatz` mask-test fusion landed in `src/code/backend/mach/arch/x86/machine.c`:
  a SELECT whose condition is directly `x & 1` now folds into `testq $1, %rax`
  plus `cmovne`, eliminating the materialized `and` and its redundant copies.
  Checksum `178` matches C/LLVM/NYC. Focused timing: Ny kernel ~12µs vs C ~8.5µs
  (~1.4x kernel gap; the reported 2.9x was a JIT-run timing-source artifact).
  Whole-program Ny time is ~12ms (from ~15ms baseline), still dominated by
  runtime/page-fault cost rather than the kernel.
- [ ] **Vectorize negative-stride reverse loads.** `loop_vectorize.c` rejects
  `access.stride < 0`, leaving `iter`'s reverse loop scalar while C emits a
  4-lane `vpermq` reverse load. Emit a forward vector load plus lane reversal
  (or a permuted load) and cover the guard and tail cases.
- [ ] **Close the 1.2x gate.** Run a complete `./make bench` on an idle host
  with enough temporary space, require matching checksums and stable samples,
  and inspect mixed assembly for every row above 1.2x.

Measurement rule: use focused fixtures while diagnosing; use the complete
benchmark and inspect timed-kernel assembly only after correctness is restored.

## Compiler build-time performance

- Scalar verifier checks now share one lazy type map within an immutable
  verification invocation. A matched pair linked from frozen objects, differing
  only in `verify.c`, emitted byte-identical editor objects with unchanged
  source/core input hashes. Cache-disabled O3 compilation executed
  694,090,384,774 instructions before and 692,206,818,136 after (0.27% fewer);
  single-run wall times were 85.13 s and 83.77 s. The scalar-class regression
  passed. `runtime_call_contracts` fails identically on both binaries on the
  existing `rt_dict_new_raw` runtime-index mismatch. This removes repeated work
  but does not close the 15-second target.

- 2026-11-13 current-tree cold editor object investigation: with native,
  AOT, JIT, stencil, stdlib source, and stdlib bitcode caches disabled, the
  native `-O3` compile took 83.79 s, including 10.35 s type validation and
  70.49 s object emission. A CPU-clock profile attributed 5.48% of samples to
  `nyr_type_map_init`, 3.55% to register-allocation recoloring, and 3.20% to
  segment lookup. Reordering a SIB eligibility check removed a 1.41% scan
  from the profile and produced the same object hash, but a second compile
  took 86.26 s under shared load, so the experiment was reverted; it does not
  establish a speedup. This is a single-project investigation, not the
  required complete build baseline.
- A subsequent call-graph profile of the same cache-disabled editor route
  took 86.60 s. SLP accounted for 10.35% of sampled CPU including callees;
  x86 home-store analysis accounted for 15.05%, and type-map construction
  for 5.47% self time. SLP now skips cloning when no basic block has two
  scalar stores. The focused SLP regression passed; a candidate compile
  produced an identical object and took 84.85 s, with SLP at 7.40% of
  sampled CPU. These single runs on a shared host suggest less SLP work but
  are not a controlled end-to-end speedup or the 15-second acceptance.
- An experimental home-store-analysis shortcut for functions without scalar
  homes retained a byte-identical editor object and passed the home-reuse and
  x86 encoder regressions, but the cold compile took 85.28 s versus 84.85 s
  in the preceding run; it was reverted. A cold `-O0` editor object compile
  took 56.99 s (10.98 s type validation, 43.04 s object emission). It was not
  linked or run. Even this lower optimization setting misses 15 seconds, so
  merely lowering the optimization level does not solve cold build latency.

- [ ] **Establish a reproducible compile-time baseline before optimizing.**
  Profile all configured project targets and the common editor/engine launch
  paths. Separate clean/cold build, warm build, no-op build, one-file
  incremental build, code-generation, link, and startup costs; record
  hardware, cache state, target set, wall time, peak memory, and compiler
  phase timings. Do not use a frozen compiler copy or a warm cache as evidence
  for a cold-build claim.
- [ ] **Bring every project compile under the requested 15-second target.**
  Apply phase-level profiles to identify dominant costs (including repeated
  parsing/type analysis, dependency invalidation, code generation, and
  linking), then optimize the measured bottlenecks. Report every configured
  project separately and the full all-project command. Acceptance: clean
  all-project build and the documented common incremental workflows each
  complete in under 15 seconds on the same recorded machine, with correct
  outputs and no stale-cache dependency. If a workflow cannot meet the target,
  keep it open and publish the measured blocker rather than silently changing
  the target or excluding projects.
- [ ] **Preserve compiler correctness and determinism while speeding builds.**
  Add before/after phase timings and cache invalidation tests; compare clean
  and incremental outputs and run affected project tests. Acceptance: no
  missing dependency rebuild, stale generated code, nondeterministic output,
  or correctness/performance regression in the runtime benchmarks.

Dated measurements and fixes are recorded in
[Recorded measurements](../learn/measurements.md#compiler-build-time-fixes-2026-11-12).

## Register allocation: measured gaps (2026-11-15)

Audited `src/code/backend/lower/regalloc.c` against the SSA-allocator spec that
used to live in `docs/todo/ssa.md` (now removed). Already implemented and kept:
loop-depth plus profile-heat, next-use distance and use-count eviction scoring;
tied-input affinity and dead-input coalescing; callee-save preference under calls;
fixed-clobber handling; deterministic end/vreg tie-breaks; per-class allocation
for GPR, FPR and vector with separate homes.

Closed on 2026-11-15 (numbers and A/B details in
`docs/learn/measurements.md`, "Register-allocation shape audit"):

1. Spill composition is measured, not assumed. `ra_spill_live`/`ra_spill_dying`
   split the spilled count; `knight` is 748 live to 56 dying, so the pressure is
   real store-and-reload work. A "spill without a home" count was dropped as
   unmeasurable: `vreg_home_offs` is only populated for home-DSE.
2. No treescan allocator, as a decision rather than an omission. The existing
   scan already uses loop depth, profile heat, next-use distance and use count,
   verifies its own interference (`regalloc_verify_mode`), and the encoder
   repairs local evictions afterwards (`nyr_mach_regalloc_recolor_locals`).
   Replacing that verified core with a new sweep on a host contended by other
   agents would ship unmeasurable risk, which the repository's keep-only-if-
   measured rule forbids. Revisit only with an idle host and a runtime A/B.
3. Call-crossing-weighted spill cost was implemented, measured as byte-
   identical on six fixtures (the use-count term is already the reload count),
   and reverted; `regalloc.c` is byte-identical with its previous state.
4. The duplicate audit line was home-DSE's probe encode, and it also double-
   counted the process-wide regalloc stats, so released `mach regalloc` reports
   overstated every function's segments and spills. Probe encodes now set
   `suppress_stats`; a reused probe clears it. Allocation time can therefore be
   compared against `NYTRIX_NO_HOME_DSE=1` (one encode) or the default two-
   encode path, but the two must not be mixed.

Use `NYTRIX_AUDIT=1` and compare `ra_segments`/`ra_spilled`/`ra_spill_live`/
`ra_colors` per function name when changing allocation.

## SCEV: remaining symbolic work

- Exact symbolic exit counts (`n - a` as a value rather than an interval) need a
  symbolic range type; `nyr_range_t` is a constant interval by design and is
  part of the serialized IR format. Range-derived intervals now cover the
  bounds case (see `docs/learn/measurements.md`, 2026-11-15).
- Known multiples (`{0,+,3}` is always a multiple of three) has no consumer yet.
  Add it together with the first div/mod strength-reduction query that wants it,
  rather than carrying dead metadata.
- Loop guards proven from range facts are used only as in-body intervals; the
  after-loop exhaustion singleton still requires an exact trip count.
