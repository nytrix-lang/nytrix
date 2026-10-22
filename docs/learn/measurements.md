<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"learn","order":180,"summary":"Historical performance results, useful compiler findings, and rejected experiments."} -->
# Recorded measurements

Results below describe the October 2026 investigations, not a fresh evaluation
of the current compiler. NYR means the native backend; LLVM means the LLVM
backend. A run written as **5+2** has five measured samples and two warm-ups.
Times are medians unless identified as individual observations. Matching
checksums establish agreement for the measured input, not general correctness.

[Performance](performance.md) owns investigation methods;
[Contributing](../CONTRIBUTE.md) owns build and verification procedures;
[Open work](../TODO.md) owns acceptance criteria. Release implementation records
are in the [changelog](../CHANGELOG.md).

## Reading the results

- Compare the same source, input, execution route, flags, cache state, and timed
  region. Changed benchmark sources are new baselines, not compiler A/B tests.
- Discard contended or unstable timings. C results varied by more than 100% in
  several Binary runs; those rows cannot establish an NYR/C improvement.
  References below 1 ms were separated from the gated runtime comparisons.
- Distinguish compilation, process startup, and the timed kernel. Instrumented
  execution and whole-process counters do not identify a kernel's hot instruction.
- Hardware sampling on the recorded host sometimes assigned kernel addresses to
  user symbols, including a known C control. Those instruction attributions were
  invalid despite zero lost samples. Validated software-clock sampling and
  aggregate counters are separate evidence. Build-ID or mapping mismatches also
  invalidate attribution.

## Selected workload results

These rows preserve useful checksum-matched checkpoints. They were not one
simultaneous campaign and must not be combined into a new aggregate.

| Workload and checkpoint | NYR | LLVM | C | Interpretation |
| --- | ---: | ---: | ---: | --- |
| Dictionary insertion, empty-first-probe path, 3+1 | 28.9 ms | 25.3 ms | 30.6 ms | Prior supplied row was 49.0/29.5/30.9 ms; focused result met its 1.5× native/LLVM cap, without a full rerun. |
| Word frequency, direct span counting, 3+1 | 5.08 ms | 5.13 ms | 8.81 ms | Checksum and semantic edge cases passed; earlier split-and-dictionary implementations are superseded. |
| DGEMM before conversion-aware FPR homes, 5+2 | 1.10 ms | 324 µs | 314 µs | Two candidate NYR medians were 650 and 734 µs; C varied too much for a stable ratio. |
| SOR after the address-chain correctness guard, 3+1 | 3.44 ms | 751 µs | 1.45 ms | Correct checksum restored; C dispersion was 54%. |
| Fannkuch, 5+2 | 357.7 ms | 215.2 ms | 211.1 ms | NYR used 64-bit array elements; C used 32-bit elements. Not a layout-matched comparison. |
| Thread ring, 5+2 | 23.1 ms | 22.7 ms | 6.12 ms | Similar native/LLVM behavior; the shared-runtime and synchronization gap against C was not attributed. |
| BigInt after timer/reference correction, 5+2 | 34.0 ms | 34.8 ms | 3.61 ms | Both references used 256-bit floating arithmetic; C still included explicit GMP cleanup. |
| BCE matrix, one focused 5+2 checkpoint | 442 µs | 414 µs | 125 µs | Bounds check removed, but native nested traversal remained scalar. Later unstable rows were discarded. |
| Splay, restored safe baseline, 5+2 | 5.41 ms | 3.66 ms | 4.30 ms | Native/C target remained unmet at this checkpoint. |
| Splay, resident-register scaled LEA, 5+2 | 5.15 ms | 3.66 ms | 4.44 ms | Small local improvement; native remained slower than C. |
| Call chain, 3+1 | 2.46 ms | 531 µs | 255 µs | Arithmetic calls were already inlined; frame-home traffic remained. |
| SHA-256, October 2, 2+1 | 119 ms | 98.7 ms | 896 µs | Representation/runtime differences dominated both language routes; not just a native-emitter gap. |
| PBKDF2, October 2, 2+1 | 236 ms | 205 ms | 1.09 ms | Historical pre-fix baseline; later vectorization correctness work is separate. |
| CSR graph BFS, 3+1 | 28.4 ms | 16.2 ms | 13.6 ms | Construction plus four traversals at each size; checksum `4111535296`. |
| Expression tree, 3+1 | 66.2 ms | 31.2 ms | 16.1 ms | A leaf changed before each evaluation to prevent invariant-result hoisting; checksum `219547`. |
| Mixed flat records, expanded sizes, 7+2 | 42.2 ms | 34.9 ms | 44.1 ms | Matching 24-byte row storage; not a comparison of language-level record representations. |
| Repeated-string construction, expanded sizes, 7+2 | 128 ms | 118 ms | 92.0 ms | Sparse page-touch checksum; a full byte scan measures a different operation. |
| Known-vocabulary word scan, 3+1 | 6.08 ms | 2.50 ms | 2.08 ms | No string hashing; complements rather than replaces general word frequency. |

FFT's signed-division correctness recheck measured 99 µs NYR; Linpack measured
1.72 ms NYR. Both passed focused checksums. Earlier Linpack was
2.79 ms NYR/653 µs LLVM/149 µs C with no bounds checks or dynamic operations
reported in its main function; that did not by itself locate the bottleneck.

## Compilation and startup

### Import-heavy compilation

The October 3 investigation measured cold native emission of
`vterm-key-state-open`, with both standard-library caches disabled. Its roughly
43.9 s starting point fell through successive, separately measured changes:

| Change | Recorded result | Correctness or interpretation |
| --- | --- | --- |
| Register-allocation prefilter | Intermediate cold median about 39 s | Reduced unnecessary allocator work; not the final acceptance result. |
| Conservative alias-name prefilter | 35.940 s median; matched disabled control 38.622 s | Full alias scans fell from 21,857 to 1,106. Positive candidates retained source/module precedence. |
| Source-order layout index | 29.173 s median | Thirteen focused checks passed; three 8,540,352-byte outputs had identical hashes. |
| Enum-member cache | 28.385 s median | 15,844 queries, 11,823 hits, 4,021 tree scans; enum traversal fell below the profile's 0.5% display threshold. |
| Scoped function-name cache | 25.120 s enabled mean, 28.222 s disabled | Same executable, three runs per arm, identical output objects; exact-string keys avoided interner-order changes. |
| Block-local coalescer segment lookup | 24.344 s median | Three cold emits, identical object hashes, correct end-to-end output; met the then-current 25 s target. |

The alias index conservatively covered explicit aliases, imported items,
plain-use leaves, and module leaves. A miss could reject alias resolution;
a hit still used the original resolver. Layout indexing preserved source
order and dotted suffix aliases. Enum caching included misses because tags and
declarations were fixed for the expanded program's lifetime. Function lookup
kept a bounded 8,192-entry cache; longer names and allocation failures retained
the original resolution path.

Larger function caches, a last-query cache, a combined key-length/hash loop,
and enumerating only coalescer header predecessors did not show repeatable
benefits and were removed. One early alias-index output differed in size;
later layout-index runs were deterministic. A mismatched-executable profile
was discarded rather than used to explain instruction offsets.

The final coalescer change kept uniqueness and first-match checks while
searching the current block's segments instead of the complete segment list.
Its sampled self share fell from 11.97% to 6.44%. Five allocator/CFG fixtures
and all 62 benchmark checksums passed; the one-sample benchmark was correctness
evidence, not a performance campaign. Generated-program runtime was outside
this compile-time target.

### Larger editor workload

A later workload on revision `2f25b3ac` plus local changes took 128.71 s cold
and emitted a 25,226,512-byte object. Register-segment lookup, not the earlier
loop-edge coalescer, dominated its relevant call chain. A single-segment fast
path retained the existing inclusive bounds check. Its sampled share fell
from 22.35% to 19.43%; profiles took 132.533 and 127.039 s, with zero lost
samples but some unresolved frames. Inline decisions and output objects were
byte-identical. This supports a local lookup improvement, not closure of a
new editor compile-time target or a change to inlining policy.

### Native JIT relocation lookup

The loader repeatedly searched for the same definition while deciding data
routing, symbol resolution, and external call stubs. Reusing one exact-name
lookup preserved alias and relocation rules; AArch64 also reused its stub
selection lookup.

An alternating warm-process comparison discarded two warm-up pairs and kept
ten samples per arm. Baseline median was 207.69 ms (200.95–209.61 ms);
candidate median was 149.78 ms (145.07–155.94 ms), 27.9% lower. Every run
returned checksum `1968365`. Another editor compile overlapped the comparison,
so this is a local startup observation, not an arithmetic-speedup claim.
Four focused module/cache/closure fixtures passed; AArch64 execution was not
available on the x86-64 host.

## Binary: scalar recurrence and register homes

The canonical workload's checksum was `3360874588549334`. Its linked native
caller inlined `branch_mix` but retained a scalar dependent recurrence, while
LLVM vectorized independent seeds. An outlined helper's frame size was not
the cost of that inlined timed loop.

Representative counters were about 37.05 million native instructions versus
3.67 million LLVM instructions, with similar branch/cache misses. An earlier
cross-block eligibility relaxation increased instructions from 78.99 to
91.31 million despite overlapping wall-time ranges; it was rejected. Verified
software-clock sampling and matching machine images connected hot home
traffic to loop-carried values; invalid raw-IP profiles did not.

Recorded retained changes included:

- Signed divide-by-two lowering used a logical shift for its 0/1 bias, not an
  arithmetic 0/−1 bias. A negative-seed workload exposed native result
  `3450853999197106` versus the correct LLVM/C `3456948289413084`.
- Unique-edge carry required matching physical colors, an explicit carried
  segment, safe predecessor/successor structure, and no intervening clobber.
  A nominal register color alone did not establish residency.
- SELECT-arm coalescing reused a dying arm's color but preserved a still-live
  arm. The target result stopped spilling; aggregate runtime did not improve.
- Common-addend factoring preserved raw-i64 wrapping, SELECT guards, ownership
  metadata, and effect restrictions. It fired in the measured recurrence but
  did not by itself meet the 5× native/LLVM cap.
- Side-effect-free `SUB_I64` became eligible for if-conversion. Empty branch
  bridges were resolved only with exclusive-edge checks.
- Physical fallthrough deletion required the next block to have no other
  incoming branch. One matched 5+2 comparison improved 2.450 to 2.148 ms,
  with static spills/reloads falling from 9,059/26,933 to 7,854/23,140;
  C dispersion exceeded 120%, so it established no stable NYR/C ratio.
- Constant-division scheduling admitted immediate divisors other than `0` and
  `−1`, preserving trap order and scratch-register clobbers. One 3+1 result
  was 1.897 ms native/388.1 µs LLVM, below the 5× cap; a completed 71-fixture
  serial run recorded 1.91 ms/385 µs and passed its then-current gates.

Other checksum-valid snapshots still exceeded that cap: one supplied
72-fixture result was 2.32 ms/387 µs, and a focused 3+1 result was
2.313 ms/384.8 µs. These are distinct compiler/workload checkpoints, not
proof that the earlier gate result holds today.

### Rejected Binary experiments

| Experiment | Reason not retained |
| --- | --- |
| Broad caller-saved register pool | Call-clobber and later Euler9 failures; profitable-looking local allocation did not prove safe residency. |
| Weighted R8 homes across recognized power-of-two division | Instructions fell from 37.04 to 36.08 million, but candidate/control NYR medians were 2.400/2.383 ms. |
| Loop-carried GPR opt-in | Lower elapsed time was not supported by lower cycles or instructions. |
| Invariant-only loop-home filter | No useful change to the canonical result. |
| Larger full-unroll budget through 18 trips | No demonstrated gain; did not make the nested seed loop vectorizable. |
| CFG clone cap 256→512→1024 | The 512 trial remained above the cap; 1024 regressed to 3.968 ms and 11.04× native/LLVM. |
| More permissive CFG clones, including division/fallthrough shapes | Some synthetic legality tests passed, but canonical runs were unchanged or regressed to 4.26 ms. |
| Paired recurrence unrolling | One direct result was 2.741 ms versus 2.304 ms; not a controlled gain. |
| Reduced if-conversion budget | Branchful code was substantially slower; one sample rose to 5.34 ms. |
| Deferring signed division to machine lowering | Extra allocator pressure; no canonical performance improvement. |
| Literal-depth specialization or selected-multiplier source rewrites | Prevented useful inlining or regressed the unchanged workload comparison. |
| Earlier common-add factoring probe | Approximately 2.39 versus 2.40 ms, without a repeatable gain; distinct from the later guarded retained rewrite. |
| Narrower division clobber mask | Regressed to 2.29 ms and 6.19× native/LLVM. |
| Branch-bridge inversion | Could bypass required register-home saves; passing one checksum was insufficient. |
| Rematerialization preference, wider copy forwarding, loop-update coalescing | No sufficient canonical gain. |

The remaining SIMD opportunity concerned independent seeds, not weakening the
dependent-recurrence legality guard. Existing unroll-and-jam required a known
even trip count, step-one induction, and a pure two-block body; larger budgets
did not satisfy those structural requirements.

## Splay and native register allocation

The supplied early Splay row was 16.9 ms native/4.01 ms LLVM/4.53 ms C;
a 5+2 confirmation was 16.733/3.978/4.414 ms. Later safe-source checkpoints
were around 5.4 ms native, still behind C. These changes were incremental,
not one matched end-to-end A/B.

Useful retained mechanisms:

- Call-safe loop homes were checked per candidate, rather than rejecting an
  entire function because some loop contained a call. Vector overlap was
  likewise checked against each scalar candidate's live range.
- PHI copies were placed in a predecessor with a single successor instead of
  always using an out-of-line trampoline.
- Sticky-home self stores were omitted only when the same home was already
  valid. Direct frame-to-allocated-register loads avoided an extra RAX move.
- Scaled LEA reused resident base/index registers and loaded only missing
  operands. An initial premature encoder return truncated a function; commit
  failure propagation and switch-local exit corrected it.
- Small constant multiplies used direct-register LEA where legal. Negative
  signed-32-bit immediates used the seven-byte encoding instead of ten-byte
  `movabs`; this was a code-size result, not a demonstrated runtime gain.
- Callee-saved address homes and a restricted call-free R8 home reduced some
  traffic without reopening broad caller-save eligibility.
- Cross-block compare-flag reuse required identical operands, a flag-preserving
  trampoline, unique false-edge entry, fused branches, and ended compare-result
  lifetimes. Linked code confirmed the second compare disappeared; timing
  differences remained within variation.
- Direct incoming-argument setup required a unique entry-block frame load, no
  write to its slot, only fixed frame stores between eligible loads, and no
  incoming-register clobber. Only callee-saved scalar GPR destinations were
  eligible; aggregate and more-than-six-parameter signatures stayed excluded.

The timed caller still reloaded five call arguments in one inspected object;
the callee had 89 vregs, 221 segments, 40 frame slots, and a 544-byte frame.
Another large `main` frame contained roughly 24 KB of source buffers; frame
size must not be equated with spills. C kept the traversal inline in its timed
loop, but forced native inlining alone did not provide a repeatable gain.

Default loop homes remained useful: a matched 5+2 control measured
5.46 ms native versus 6.88 ms with them disabled. A separate process counter
comparison recorded 52.05 million native versus 13.91 million LLVM
instructions; those counters included startup and reporting.

### Rejected allocator and Splay experiments

| Experiment | Finding |
| --- | --- |
| Unrestricted same-block home reuse | Focused tests missed a Binary crash. The constrained version excluded frame-loaded values and required disjoint single-segment GPR intervals. |
| Globally relaxed cross-block colored reads | SOR returned `412269742` instead of `1725461952`, independently of the indexed-store bug. Canonical-home seeding remained necessary. |
| Larger loop-home block ceiling | The relevant timed function was already below the existing ceiling. |
| New carry rule for SOR row bases | Homes grew from 1,240 to 1,288 bytes and timing did not improve. |
| Lower vector-unroll factors | No stable SOR benefit; another factor-two trial increased instructions and missed n-body's budget. |
| Immutable parameter-home aliasing | No repeatable runtime gain, despite excluding mutated/address-taken parameters. |
| Unproved incoming-register loads or skipped parameter-home stores | Could read a register after entry had clobbered it; removed despite passing selected checksums. |
| Pre-allocation compare elision | Seven fewer segments, but frame grew from 544 to 576 bytes and runtime stayed about 5.35 ms. |
| Broad pressure-aware inlining or forced `splay`/join inlining | Controls did not establish a repeatable gain; source hints were reverted. Cost approval alone did not prove an inline splice occurred. |
| `i32` loop annotations, cached child loads, equal-immediate copies | No stable gain or no confirmed hot-path code change. |
| Global-buffer source parity | Failed with extreme memory growth before useful timing; failure stage was not isolated. |
| Setup/timed-loop extraction | One refactor broke checksums and rose to 699 ms; explicit-buffer variants restored checksums but overlapped baseline timing. |
| Direct immediate-to-home stores | Focused boundaries passed, but the timed hot path did not exercise the new encoding. |
| More caller-save/address-home candidates | Extra eligibility did not establish a safe, repeatable improvement. |

The earlier compare-residency regression exercised both outcomes of an emitted
integer comparison. A path-only temporary-file descriptor leak also affected
benchmark reporting; closing those descriptors and serializing timed workers
were harness fixes, not Splay optimizations.

## Vectorization, floating point, and addressing

### SOR

Affine row-base splitting moved `(row + column + displacement) << scale`
toward an invariant row pointer plus scaled column. Indexed-address folding
then recognized memory uses correctly: `STORE.dst` was an address use, not a
vreg definition. Linked objects confirmed direct SIB loads/stores, while
readable text assembly still showed extra temporaries. Five row-base home
reloads and the outer grid-swap recurrence remained distinct costs.

Later four-wide f64 loops used AVX2 arithmetic but still spilled scalar address
and induction values. Permitting callee-saved scalar homes across vector
instructions changed two NYR medians from a 3.41 ms baseline to 3.17 and
3.14 ms; C was unstable. Hoisting horizontal neighbor offsets in the benchmark
source changed a 3.269 ms row to 2.871 and 2.889 ms, but this was a source
change with noisy follow-up results, not a stable compiler ratio improvement.
Reducing the vector-unroll cap from eight to four or two did not help.

LICM later hoisted address-producing additions before vector rewriting and
corrupted the checksum (`5515142` versus `1725461952`). Keeping an add in its
loop when its use chain feeds a scalar/vector address restored correctness.
The fixture gained an explicit known-checksum assertion.

### Vector and loop legality

- Four-lane f64 used a 32-byte machine representation: x86-64 emitted VEX
  instructions; AArch64 used paired NEON operations. Strict reductions retained
  ordered rounding. Machine width, storage homes, and induction stride had to
  agree; a four-lane operation advancing by two was a real correctness bug.
- PBKDF2's late-vectorization corruption (`2991`→`3978`) came from reduction
  PHI handling. The dedicated correction and containment checks were distinct
  from earlier speculative rotate/BCE explanations.
- Guard-derived ranges required explicit predecessor/update proofs. Coverage
  included strict/inclusive bounds, step-two traversal, matching multi-latch
  updates, overflow, bypass edges, unknown-effect calls, and overlapping stores.
  Unsupported or conflicting shapes retained scalar execution.
- Restricted unswitching remapped SSA/exit PHIs and labeled cloned fallthrough
  exits, preserving unselected traps. Multiple distinct exits remained rejected.
  Interchange admitted only independently proven private, injective, in-bounds
  stores and invariant stack reads; overlap/overflow cases remained rejected.
  These were legality improvements without measured runtime claims.
- DGEMM's FPR-home dataflow modeled scalar-conversion clobbers per block,
  rather than rejecting all homes because a conversion occurred elsewhere.
- A later PBKDF2 failure reported missing `vec4.reduce_add.i64` emission.
  An attempted scalar emitter/four-word-home change still crashed and was
  reverted. The contemporaneous BCE-matrix crash was not thereby fixed.

### Address and ABI correctness

| Failure | Recorded correction or limit |
| --- | --- |
| Forwarded RAX result followed by a same-home no-op copy | Do not treat the no-op as a consumer that makes the canonical home unnecessary. |
| f64 loads ignored a pending RAX address | Use the same validated scalar-address load path as integer loads. |
| Indexed integer store staged its value in R10, then address formation clobbered R10 | Preserve the value while resolving the full destination address. |
| Vector store ignored a pending SIB address | Resolve the complete memory address for ordinary and fast-path V256 stores. |
| Adjacent single-use ADD/load | Capture the address operands and omit the home only when later uses/live-out do not require it. |
| Runtime ABI tables failed binary search | Place `rt_set_len_raw` in lexical order in both declaration and argument-contract tables. |
| AVR prologue overwrote incoming argument bytes in `r16:r17` | Use caller-clobbered `r26:r27` for stack-pointer temporaries before spilling arguments. |
| Broad immediate arithmetic lowering | Silent Binary/Splay/SSE miscompiles outweighed focused successes; reverted. |
| Profile-driven block layout with synthetic steps but zero positive block heat | Leave the function unchanged; synthetic profitability was not real layout evidence. |

## Memory facts, bounds checks, and scalar optimization

- GVN reused dominating reads of the same local slot only without an
  intervening write. Proven non-local reads could survive unrelated local
  stores; address-taken locals still required alias safety. Effect-only calls
  such as clock reads were not automatically writes to existing memory.
  JSON parser byte loads fell from four to two in the relevant optimized shape;
  manually caching the byte in source did not establish a separate runtime win.
- Append-length reuse needed canonical declaration identity and the same-local
  assignment lineage. Handling only one lowered call spelling missed the fast
  path. BCE reused a single CFG/dominator map across its proof families.
- Append and typed-buffer setter results supplied logical may-alias lineage,
  not permission to assume every handle identical. Store reachability and
  freshness still mattered. Returned-borrow provenance followed one unique
  trusted argument only when effects excluded invalidating writes.
- A successful runtime assertion supplied its true-edge lower bound only if
  dominance/control flow proved execution. Function-local allocation roots
  enabled proofs that observable module-global cells could not support.
  Adding `list<int>` to an empty list alone did not improve the benchmark.
- Moving asserted buffers into helpers changed the source baseline. Historical
  matrix/sort results after that change cannot be presented as a compiler-only
  A/B. A remaining nested non-affine address rejection was separate from the
  removed bounds check.
- Counted zero-fill rewriting required a nonnegative, bounded, non-overflowing
  byte range and preserved observable writes. It did not justify weakening
  alias or trap checks.
- The `escape-loop` kernel eliminated allocation, checks, and the loop while
  preserving checksum `499999500000`; both backends measured below 1 µs.
  A guarded affine reduction also resolved the earlier `inline` loop gap.
- Recursive constant evaluation restored `gcbench` to approximately
  369 µs native/374 µs LLVM/33 µs C. That did not establish safety for broader
  recursion, side effects, malformed flow, or exhausted evaluation budgets.
- Havlak's promoted integer reads were raw i64, but equality had reached a
  dynamic tagging boundary. The focused correction addressed that mismatch;
  passing a reduced case alone had not established the full fixture fixed.

## Runtime arithmetic and allocation

### BigInt and BigFloat

Similar NYR/LLVM times pointed to shared runtime arithmetic and immutable
allocation APIs, rather than only native code generation. One old fixture
version counted 166,814 allocations and 11,207 runtime calls; a later
checksum-matched run counted 158,116 allocations and no reallocations.
Those counts belong to different checkpoints.

A marked execution interval identified these sampled self shares:

| Component | Share |
| --- | ---: |
| `rt_malloc_impl` | 18.07% |
| Heap-limb reading | 9.04% |
| Knuth division | 7.07% |
| `__udivti3` | 3.34% |

Only 509 of 2,927 samples fell inside that interval; zero samples were lost.
Earlier whole-command sampling mostly measured compilation and was unsuitable
for choosing an arithmetic change. A separate single scaled probe put BigInt
square root at 52.5 ms, BigFloat add/subtract at 46.7/36.8 ms, BigInt power
at 32.6 ms, and BigInt add/subtract at 31.0/32.3 ms. These were diagnostic
operation timings, not stable baselines.

Recorded retained changes:

- Exact-u64 BigFloat construction wrote directly into the final object when
  precision sufficed. Two A/B pairs showed about 2.7% fewer instructions, but
  cycles and wall times were inconsistent; no wall-speedup claim followed.
- BigFloat square root reduced Newton steps from
  `4 + floor(log2(precision))` to `1 + ceil(log2(precision))`, keeping a guard
  step: 12→9 steps at 256 bits. Ordinary-precision division used stack scratch,
  larger inputs retained heap scratch, and exact division by two folded into
  the result exponent. Four candidate medians were 33.45–34.50 ms native
  versus a 37.82 ms preceding baseline; spread and C variability limited the
  comparison. Focused boundary/differential/padding checks passed after fusion;
  the earlier 2,027-test cold pass preceded that final fusion.
- Redundant two-limb multiplication probing was removed, and division reused
  its validated divisor-limb view. Five alternating pairs had medians
  756.56→727.67 ms, all checksum `1968365`; an overlapping benchmark made
  the 3.8% difference provisional. Nine focused cold rows passed, not a full
  cold suite.

Rejected arithmetic experiments included direct short-division result writes,
power-of-two quotient shifts/remainder masks, replacing division with a source
shift, and removing a wrapper zero check. One alternating shift/mask comparison
was slower despite 56 retained arithmetic boundary cases. A multiply/divide
fusion pattern could not match: NYIR called module wrappers with expanded
three-word `any` arguments, not the inner runtime helpers. The unused helper
and pass were removed. None of these trials established an arithmetic gain.

### Typed-buffer and allocation lookup

| Retained change | Matched result | Limit |
| --- | --- | --- |
| Compact typed-buffer header read after an epoch-valid provenance-cache hit | Seven interleaved heapsort runs: native process median 98.330→15.220 ms; LLVM 85.664→4.032 ms | All other handles retained the resolver and sanitizer/header checks. This was process wall time, not startup subtraction. |
| Most-recent thread-local allocation-dedup range before eight-entry search | Eleven interleaved BigInt runs: 130.534→126.995 ms; retired instructions 440.425→430.703 million | About 2.2% fewer instructions; did not close the GMP gap. |

Widening the length shortcut to generic `rt_len` regressed Splay and was
removed. Rematerialization/forward-edge residency experiments were also
removed; canonical homes remained required unless every edge and clobber was
proved. LLVM spill-weight source was studied under its license, but no code
was copied into these independent runtime changes.

### Dictionaries and strings

Native dictionary handles already registered with the runtime avoided repeated
validation. Resize thresholds were cached until capacity changed; empty first
integer-key probes bypassed grouped collision search. Collisions, overwrite,
and growth retained the existing behavior.

Word counting moved from split-list construction to token spans, avoiding
repeated-key allocation. Counts used the dynamic integer representation: raw
`1` otherwise collided with tagged zero, losing singleton words. Semantic
coverage included whitespace, repetition, growth, long keys, empty input, and
non-string input. An earlier two-pass tokenizer removed an intermediate token
list without demonstrating a material gain on its own.

## Correctness and verification history

These are completed historical checks, not verification performed for this
page revision.

| Investigation | Observed outcome |
| --- | --- |
| Early cold native parity | 1,122/1,131 passed; later 1,185/1,191, then 1,189/1,191. |
| Subsequent focused fixes | 1,192/1,192 passed, including 61 benchmark correctness fixtures; not a performance result. |
| Untyped closure ABI | A function's own result representation had to prevail over a later call site's raw inference. |
| Sound-source reachability | False global-name resolutions inflated the graph from the intended workload; targeted resolution reduced reachable functions 973→262→148 and avoided the former 8 GiB failure. |
| Sound emission and execution | Emission completed at 220 MiB; the cold matrix covered NYIR VM, MCJIT, ORC, native, and native+VM. Physical 440 Hz playback and a 144,044-byte/72,000-frame WAV were separately checked. |
| Sound numeric specialization | Concrete f64 `poly_blep` avoided a dynamic representation error; a separate old integer-output discrepancy was not a new sound-memory failure. |
| Diagnostic re-lowering | JSON-parser pass dumping crashed although normal execution passed; re-lowering lifetime/output handling was corrected. Diagnostic output volume remained a separate issue. |
| Four-lane f64 and runtime contracts | Focused drivers passed; the full optimizer sweep reached `tv_smt_semantics` and timed out, so no complete sweep pass was established. |
| Later vector campaign | Binary ratios passed, but BCE-matrix and PBKDF2 crashed before samples; the campaign failed overall. |
| Isolated reported benchmark mismatches | Several nqueens, float comparison, Fannkuch, FFT, knucleotide, and Euler9 failures did not reproduce in focused checks; unstable rows stayed unaccepted. A passing rerun was not a newly identified fix. |
| Platform audio and list/cache regressions | Selected cold rows passed; no new audio cause or full-suite fix was inferred. |

ORC perf-map support used object-symbol sizes and loaded addresses. Missing
sizes, unresolved mappings, or invalid raw sample IPs could not be repaired by
accepting a profiler's symbol label. A code dump established what was emitted,
not which instruction consumed time.

## Benchmark validity and coverage

### Harness and reference corrections

- Timed fixtures/routes became serial and stopped inheriting ordinary test
  concurrency. A 66-worker run on a 96-thread host overlapped another suite
  and exhausted memory; its timings were not accepted. Explicit comparison
  worker settings did not make overlapping timing valid.
- Native timing used the compiler's nanosecond phase marker. C wrapper timing
  covered its whole measured body; timer-floor fallback was reported, not
  silently compared as equivalent kernel time. Progress updates did not become
  samples.
- Matrix validation changed from a weak positive accumulator check to the
  exact checksum `1105103687680`; BCE matrix used `49995000`. A plausible
  nonzero result was insufficient for transformation acceptance.
- BigInt references precomputed the same square-root operand. The C floating
  workload changed from `double` to 256-bit GMP, and Ny timing expanded to
  include setup and final power/checksum work. Earlier ratios were superseded;
  explicit C teardown remained a difference.
- Fannkuch's attempted 32-bit parity changed direct raw accesses into generic
  helper calls, regressing native to 9.81 s/72.3 billion instructions. It was
  reverted. A separate vector-disjoint home comparison measured 282 ms versus
  400 ms forced-stack, with 24% fewer instructions.
- Thread-ring aggregate misses did not establish false sharing. `perf c2c`
  sampling was denied by host permissions; that was missing evidence, not
  zero HITM and not justification for speculative mutex padding.
- Completed campaign results and incomplete attempts stayed distinct. The
  historical 71-fixture pass did not validate a later interrupted 72-fixture
  attempt or close subsequently reported per-fixture failures.
- Build integration preserved explicit `NYTRIX_CORE_PATH` and the supported
  legacy CMake input instead of overwriting configured core-bundle paths.

### Workload coverage

The [Goose benchmark design](https://github.com/aardappel/goose/blob/master/bench/design.md)
and [results](https://raw.githubusercontent.com/aardappel/goose/refs/heads/master/bench/results.md)
informed coverage questions, not borrowed performance claims or copied source.
Its useful distinction was flat controls versus allocation, layout, mutation,
and teardown at several working-set sizes.

| Public fixture | Question covered |
| --- | --- |
| [Graph BFS](../../etc/tests/bench/algorithms/graph-bfs.nshape) | CSR construction and traversal at 4K, 32K, and 256K vertices, four starts per size. |
| [Expression tree](../../etc/tests/bench/algorithms/expression-tree.nshape) | Indexed recursive interpretation with a changed leaf before each of 4,096 evaluations. |
| [Records](../../etc/tests/bench/datastructures/records.nshape) | Mixed tag/two-word rows at 32K, 256K, and 2M records, matching C's 24-byte AoS. |
| [Repeat](../../etc/tests/bench/string/repeat.nshape) | String construction at 1, 16, and 128 MB without turning validation into a full byte-scan benchmark. |
| [Word frequency](../../etc/tests/bench/string/word-frequency.nshape) | General string-key aggregation, including owned splitting/counting costs. |
| [Known-vocabulary scan](../../etc/tests/bench/string/word-frequency-scan.nshape) | Allocation/token scanning and a small counting kernel without hashing. |

The repeat fixture's earlier full byte-scan form measured
441/391/12.8 ms and checksum `1122000000`; that was a memory-scan finding,
not a string-copy comparison. Early general word counting measured
1.75 s/1.84 s/3.78 ms before the later direct-counting result, so its old
hundreds-fold gap should not be presented as a current hotspot.

Distinct remaining coverage questions included mixed variable-payload records,
an allocation-heavy S-expression arena, bounded LRU mutation, incremental
adjacency construction, and cache-sized image blur. Flat scans, list growth,
binary trees, numeric kernels, particles, and the fixtures above already
covered several related shapes. Varying vocabulary cardinality/token length,
separating build/query/teardown, and comparing aliased/non-aliased kernels would
answer additional questions; a list of benchmark names alone would not.
