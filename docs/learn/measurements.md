<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"learn","order":180,"summary":"Historical performance results, useful compiler findings, and rejected experiments."} -->
# Recorded measurements

The latest full benchmark is refreshed automatically by the C benchmark runner;
the remaining sections retain earlier investigations. NYR means the native
backend; LLVM means the LLVM backend. A run written as **5+2** has five measured
samples and two warm-ups. Times are medians unless identified as individual
observations. Matching checksums establish agreement for the measured input,
not general correctness.

The latest run is summarized as a runtime-ratio distribution against Host C,
not a wall of per-fixture timing columns. Ratios below `1×` are faster than C;
ratios above `1×` are slower. The linked raw JSON retains every fixture's
measurements, checksums, and route metadata for exact comparisons.

Prerequisites: none; these are dated measurements, not current performance
claims.

## Quick navigation

- [Latest full benchmark graph](#latest-full-benchmark-run)
- [Measurement and profiling method](performance.md)
- [Compilation and startup results](#compilation-and-startup)
- [Compiler build-time fixes (2026-11-12)](#compiler-build-time-fixes-2026-11-12)
- [Runtime and code-generation investigations](#vectorization-floating-point-and-addressing)
- [Contribution and verification procedures](../CONTRIBUTE.md)

[Performance](performance.md) owns investigation methods;
[Contributing](../CONTRIBUTE.md) owns build and verification procedures.
Release implementation records
are in the [changelog](../CHANGELOG.md).

## Reading the results

- Compare the same source, input, execution route, flags, cache state, and timed
  region. Changed benchmark sources are new baselines, not compiler A/B tests.
- If `./make bench` reports `BENCH INVALIDATED`, its saved rows are
  measurements from the run, not a pass/fail verdict for the final source
  tree; identify the changed input before using them as a baseline.
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

## Resolved builtin operator reachability

A fresh cache-disabled native Pong compilation was sampled at 99 Hz with
`cpu-clock:u` and 8 KiB DWARF call stacks. The capture contained 5,338 samples
and zero losses. `ny_native_find_impl_operator_in_stmt` accounted for 1.55%
of self samples, reached through `ny_native_scan_expr_for_calls` and
`ny_native_collector_operator`.

Lowering already treats a resolved binary expression without a selected
operator declaration as a builtin. Reachability previously ignored that fact,
recomputed operand types, and traversed module/impl declarations again.
`ny_native_collector_operator` now uses the same authoritative fact before
those probes. For resolved builtins, work proportional to the module graph
at each expression becomes one fact check. Explicitly selected operator
declarations still take target resolution and its fallback, and unresolved
expressions retain the conservative search.

Seven focused fixtures passed: raw integer operators in the presence of BigInt
overloads, nominal impl operators (including call-result operands), chained
operator re-exports, scalar multiplication, portable impl laws, and the
ambiguous/unresolved operator error contracts. The runtime fixtures exercised
their declared native/LLVM variant matrices. Both `--bin` and
`NYTRIX_TEST_NY_FULL_BIN` pinned the candidate, with execution, stencil, and
stdlib source caches disabled.

Both compilers also built native x86-64 Pong executables from the frozen
library inputs. Each executable completed two frames and exited successfully
under a private Vulkan-rendered headless Weston compositor. Their first-frame
application traces agreed on ball position/velocity, paddles, score, font
sizes, and the 1280x688 framebuffer. A separate candidate `-vvvvvv` smoke
confirmed `[gfx] Using Native Vulkan Backend` and a shown Wayland surface.
These checks establish exercised runtime behavior, not pixel equivalence or
a startup/steady-state speedup.

The isolated control and candidate compile the same copied lowering source,
with only the collector header changed. Every other linked compiler object
is frozen and shared between the pair. A separate 541-file library/Pong/core
input snapshot prevents concurrent workspace edits from changing the measured
source graph; its manifest is checked before and after each timed run.
Artifacts are retained in `build/complexity-overhaul/`.

Three alternating control/candidate pairs used `--native-only -O2 --threads=4
--run=none`, identical absolute input/core paths, and disabled native, AOT,
JIT, stencil, stdlib source/bitcode, and incremental-inventory caches.
These are object-compilation measurements, not link, startup, or first-frame
times; other coding sessions were active on the host.

| Metric | Control | Candidate | Change |
| --- | ---: | ---: | ---: |
| Median wall time | 44.676 s | 42.471 s | 4.94% less |
| Median user CPU | 54.027 s | 51.814 s | 4.10% less |
| Median system CPU | 2.646 s | 2.287 s | 13.58% less |
| Emitted object bytes | 14,040,848 | 13,526,960 | 513,888 fewer |
| Defined text symbols, including entry | 2,728 | 2,694 | 34 fewer |

Object hashes were stable across all three runs of each compiler, but differ
between compilers. The harness's initial cross-compiler byte-equality check
therefore failed. Symbol inspection found 34 removed definitions, no added
definitions, and size changes in retained functions; this is not a
byte-preserving optimization. Removed definitions include BigInt comparisons,
finite-field equality/shift helpers, and polynomial equality helpers that
the previous collector rooted through the redundant overload search.

A separate candidate capture contained 4,983 samples and zero losses.
`ny_native_find_impl_operator_in_stmt` fell from 1.55% of self samples to
0.02% (one sample). The largest named remaining self bucket was
`nyr_mach_regalloc_segment_at` at 9.35%; CFG construction was 2.73%,
imported-module member recursion 2.13%, type-map initialization 1.85%, and
loop-edge coalescing 1.79%. The two large stripped OpenMP address buckets
remain unattributed; nearest exported symbols are not sufficient evidence
to name those private routines.

## Incremental inventory and HM tracing

`NYTRIX_INCREMENTAL=1` runs a dependency inventory after parsing. It does not
restore inferred schemes, substitute an HM cache for validation, or certify
that compilation of the reported files succeeded. The dependency snapshot
must not be mistaken for checked semantic state.

The inventory now schedules all inputs on cold and forced builds, detects
missing sources, propagates changes through both the previous and current
graphs, and conservatively retains ambiguous basename dependencies. Bare
module names match `.ny` filenames. Project roots select separate snapshots;
reconfiguration honors the current cache directory. Graph reconstruction
previously freed a pathname before using it for outgoing edges; moving the
free after edge reconstruction fixes that use-after-free. Allocation failures,
failed invalidation, and failed snapshot publication no longer become success.

`incremental_graph.c` covers cold/unchanged/forced transitions, provider edits,
provider deletion, unrelated additions, cache reconfiguration, ambiguous
providers, and sources disappearing after graph construction. The standalone
driver passed. Valgrind Memcheck reported zero errors and zero definitely,
indirectly, or possibly lost bytes; shared-library process-global allocations
remained reachable at exit.

HM now captures type-detail and deep tracing bits at the start of each normal
or reachable-fact pass. `hm_unify` reads those fields instead of calling
`ny_dbg_is_enabled` for each constraint. Quiet and `-vvvvvv` smoke checks
passed; verbose output retained `HM_CONSTRAINT` and `HM_SUBST`. A deliberately
invalid, unused function still failed HM validation. No wall-time speedup is
claimed for this small tracing-policy change.

After rebuilding `build/release/ny-full`, its integrated
`--ir-opt-test=incremental_graph` group passed. The cold module-use selection,
with explicit compiler and `core.ny` paths, passed 45/45, including positive
and fail-closed portable refinement cases.

The architecture audit distinguishes existing work from open designs:

| Area | Current implementation |
| --- | --- |
| Persistent per-module HM reuse | Not implemented; explicit successful ABI/opt artifacts are reusable only under their identity checks, and their proof path still runs HM. |
| Reachability | Native reachable-fact completion exists; full validation is not replaced by a graph that ignores unused-function errors. The main program is not redundantly re-inferred by `persist_native_facts`. |
| Parallel inference | Serial within one per-pass HM state. Schemes, type-variable instances, arena allocations, diagnostics, and AST writes are shared within that pass, not a process-global unification table. |
| HM name lookup | Existing cache keys include environment identity/generation, exact name, and exact syntax context. Replacing hygiene context with a guessed lexical-scope ID is unsound. |
| Import lookup | Existing exact module/member result caches and snapshot-owned import-declaration index preserve source order and re-export traversal; not every transitive query is one lookup. |
| Refinement lookup | Already indexed in the codegen compilation context; no second root cache was added. |

Preserving full-program diagnostics was explicitly selected for this pass.
Persistent per-module schemes/facts with dependency invalidation and independent
HM partitions with deterministic merging remain necessary before claiming the
first and third architectural blockers solved. The dirty-file inventory alone
does not satisfy either requirement.

## Compile-time cleanup under revised acceptance

The compile-time TODO pass now uses measured improvement and preserved behavior,
not the former 3–5 s compile/link/window cutoff. The user explicitly retired
that cutoff. Earlier target-miss statements below are historical; removing the
TODO section does not mean compilation costs disappeared or that incremental
or parallel HM was implemented.

The retained changes are confined to two compiler translation units:

- `src/code/backend/core/verify.c` builds a last-definition index during its
  existing verification setup. Scalar-class and ownership checks use constant
  time definition lookup instead of repeatedly scanning the whole function.
  Same-block reaching-store checks, recursion bounds, and all ABI/ownership
  checks remain. PHI, select, and local-store helpers avoid building analyses
  when there are no corresponding instructions.
- `src/code/lang/ny/lower/lower.c` indexes immediate valid `use` declarations per module
  or block for each compilation snapshot. Recursive re-export lookup visits
  those declarations in the same source order, without rescanning unrelated
  function and variable declarations. Alias, wildcard, cycle, and depth rules
  are unchanged. The index resets with the function/module snapshot and is
  freed with its owner; allocation failure or table saturation falls back to
  the original body walk.

An isolated control was relinked with only those two pre-change translation
units, using the candidate's remaining compiler objects and dependencies.
Frozen executables prevented concurrent workspace rebuilds from changing the
comparison. Both compiled the same `etc/projects/ui/pong.ny` through native
x86-64 at `-O2 --threads=4 --run=none`; standard-source, automatic bitcode,
native, AOT, and JIT caches were disabled, with `CCACHE_DISABLE=1`. This is
source-cache-disabled compilation, not an OS-page-cache drop.

Three runs per arm alternated control/candidate order:

| Measurement | Control | Candidate |
| --- | ---: | ---: |
| Median object-compilation wall time | 40.840 s | 39.788 s |
| Wall-time range | 40.214–41.025 s | 39.075–40.601 s |
| Median user CPU | 51.094 s | 49.102 s |

The local wall median decreased 2.6%, and user CPU decreased 3.9%. Each paired
candidate finished sooner, but the wall-time ranges overlap; this is a modest
local improvement, not a universal speedup or an end-to-end startup result.
Other coding sessions were active on the host. All six objects were identical:
SHA-256 `8af9128875b2432eb2a168c5ca95f1fc19111e1cdfe8ce7ef6689fbf775240d5`.

Compiler SHA-256 identities:

```text
control:   0414b30f69e975401a51d5aa7cf9db155e017510ca2157275faf8bfa74b7e913
candidate: 0c2dcd34a25b70a26870580c14b68219bbb8155024466b9dc5c11305a6fea866
```

The initial 199 Hz `cpu-clock:u` DWARF profile contained 10,609 samples and
zero lost samples. It identified imported-member recursion, register-segment
lookup, CFG construction, type-map construction, and scalar verification.
A candidate capture overlapped the AOT smoke and lost 253 samples in ten
chunks; it is not used to establish per-symbol improvements. Raw captures,
frozen binaries, timing logs, objects, and `comparison.json` are retained
under `build/compile-overhaul/`, outside source control.

Verification used explicit frozen compiler binaries and
`--std=/home/e/nytrix/build/release/core.ny`: the full module-use selection
passed 45/45 on both control and candidate. The existing named-reexport fixture
now also checks chained constant aliases, negative constants, unrelated
declarations surrounding imports, and identical member spellings in separate
providers across its four backend variants. The first default-path runner
invocation reported 32/45, including an installed-stdlib path error and
timeouts; it is not the explicit-binary comparison or a passing run.

The cache-disabled candidate also completed native AOT compilation, link,
launch, and two Pong frames under a private Weston Vulkan `kiosk-shell`
compositor. Both the Wayland surface and Vulkan framebuffer were 1280×720;
the client printed `[gfx] Using Native Vulkan Backend`. That smoke exited zero
in 45.750 s and logged frame 1 at 5.026 s after startup. It overlapped the
candidate profile and enabled `NY_DEBUG`, so it verifies the full path only:
neither its total nor first-frame time is a startup-performance comparison.

## HM function-environment buffer reuse

`hm_infer_function_scheme` now reuses one scratch environment vector across the
sequential scheme-inference passes in `hm_run` and `hm_run_reachable`. Each use
gets a fresh environment generation and copies the global entries before
binding function parameters, so cached lookups cannot reuse facts from the
previous function. Refinement, inference, and module-use selections passed
112/112. A cache-disabled Pong `--native-only -O2 --run=none` compile produced
the same object SHA-256 as the previous compiler. The single control took
36.589 s; two candidate runs took 36.219 s and 35.910 s (median 36.065 s).
Those runs do not establish a repeatable speedup and exclude linking, launch,
and first frame; the 5 s end-to-end target remains unmet.

## Pong imported-module lookup profile

A cache-disabled native-only Pong AOT run was sampled on 2026-11-11 with
`perf record -F 499 -e cpu-clock:u --call-graph dwarf,16384`. It captured
38,076 samples with no losses (about 76.3 billion cpu-clock events). Named
costs included `mach_encode_function` at 11.77% cumulative,
`mach_home_dse_analyze` at 6.33%, `nyr_find_imported_value` at 6.83%
(`nyr_find_imported_module_member_rec` at 6.45%), and HM inference at 3.7%.
`nyr_mach_regalloc_segment_at` accounted for 2.01%. The executable was
emitted, but launching Pong terminated with signal 11; the compositor reported
`message length 27503 exceeds 4096`, so this run did not verify a first window.

The module-member cache was increased from 16,384 to 65,536 four-way entries.
Matched `NYTRIX_TRACE_LOOKUPS=1` runs with `CCACHE_DISABLE=1` and AOT, native,
and standard caches disabled recorded 12,534,021 module-member queries and
991,142 tree scans at 16,384 entries, versus 11,769,712 queries and 917,513
scans at 65,536 entries (7.4% fewer scans). Both saved Pong executables had
SHA-256
`53c42af524e6e078cdea2de362733adfb880336b2893f7d369100914e9daa70e`.
The single traced wall times were 37.42 s and 37.84 s respectively; another
untraced candidate run took 37.63 s. The counter reduction confirms less
recursive lookup work, but these timings do not establish a total compile-time
speedup. Both AOT launches ended with the same Wayland protocol error. The
larger lazily allocated tables trade additional compiler memory for fewer
measured lookup scans; the 5 s compile/link/first-window target remains unmet.

## x86-64 home-DSE probe fast path

`mach_home_dse_analyze` can remove only 8-byte virtual-register homes. The
probe now avoids allocating its reference/dead maps and scanning encoded bytes
when a function has no 8-byte vreg homes or the probe emits no vreg-home
stores. Otherwise the existing reference scan and elimination test are
unchanged. A no-dead-home probe is still reused as the final encoding.

The cache-disabled native fixture selection passed 68/68 after this change.
Two cold-cache 64-thread Pong AOT runs on headless Weston with a Vulkan
renderer took 37.263 s and 38.365 s wall (median 37.814 s); first submitted
frames arrived 1.283 s and 1.134 s after app startup. Compiler, native, AOT,
standard-library, and standard-bytecode caches were disabled. Both emitted
the same ELF SHA-256 as the pre-change Pong build:
`53c42af524e6e078cdea2de362733adfb880336b2893f7d369100914e9daa70e`.
A debug-enabled run of that identical ELF printed
`[gfx] Using Native Vulkan Backend`. Weston output was 1280×720, but its
desktop shell configured the app surface at 1280×688.

A cache-disabled 199 Hz DWARF profile of the same 64-thread, `--run=none`
route captured 12,000 samples with no losses. `perf buildid-list` matched the
sampled `ny-full` Build ID `a79e6dbcd64bd354755bea5a1e59ccc500331255`.
`mach_home_dse_analyze` was below the 1% report cutoff. Remaining named costs
included imported-module recursion at 2.31%, CFG construction at 2.14%,
register-segment lookup at 1.99%, NYIR scalar verification at 1.73%,
loop-edge coalescing at 1.72%, type-map initialization at 1.72%, and
implementation-operator scanning at 1.45%. Two unresolved `libomp.so`
instruction-address buckets accounted for 13.13% and 9.79%; the profile does
not identify their symbols, so this is not yet evidence of a specific OpenMP
wait or scheduling defect.

The same output was rebuilt cache-disabled through Weston `kiosk-shell` with
an exact 1280×720 app surface. Each run requested one frame; timings are one
observation per thread setting, not repeatable medians.

| Compiler / std-library threads | AOT-to-frame wall | Frame 1 |
| ---: | ---: | ---: |
| 8 / 8 | 41.386 s | 1.438 s |
| 16 / 16 | 39.819 s | 1.338 s |
| 32 / 32 | 37.189 s | 1.185 s |
| 64 / 64 | 37.223 s | 1.269 s |

Every run emitted the same ELF SHA-256 as the earlier Pong build:
`53c42af524e6e078cdea2de362733adfb880336b2893f7d369100914e9daa70e`. A debug
launch of that identical ELF reported a 1280×720 Wayland surface and
`[gfx] Using Native Vulkan Backend`. The single 32/32 and 64/64 observations
are indistinguishable as a tuning result; neither approaches the 5 s target.

A preceding 64/64 cold run before the no-store gate took 43.107 s and produced
the same first-frame behavior; it is only one control against two candidates,
so the difference does not establish a repeatable total-time speedup. The
measured end-to-end time remains far above 5 s, and compile-hotspot work stays
open.

## Native Pong compiler call graph

A cache-disabled native-only Pong compile was sampled on 2026-11-10 with
`perf record -F 199 -e cpu-clock:u --call-graph dwarf,16384`. The baseline
record contained 3,698 samples, no lost samples, and about 18.58 billion
cpu-clock events. `find_refine_in_stmts` accounted for 5.76% of samples:
each unseen refinement name recursively scanned the program and nested module
statements. The old name memo helped repeated names but did not avoid a full
scan for each distinct refinement.

`ny_lookup_refine` now builds an arena-backed open-addressed index of refinement
statements in `prog` and `extra_progs`, preserving the original first-match
order (main program, nested modules in traversal order, then generated
programs). It rebuilds if the main program pointer or generated-program count
changes; definition objects are still materialized lazily. The candidate record
contained 3,378 samples, no lost samples, and about 16.97 billion cpu-clock
events. The scan fell below the report's 0.8% cutoff. These are single-run
profiles, not a controlled timing comparison.

The candidate call graph still puts `hm_run` at 55.89% cumulative children;
`hm_infer_function_scheme` accounts for 49.73%, with recursive statement
inference dominant. Flat samples include HM environment lookup (6.01%),
module statement lookup (5.39%), expression inference (5.54%), and
`collect_module_exports` with linear unique-name checks (3.29% cumulative).
Imported-module alias resolution and string/hash work remain visible below
those paths. These overlapping call-tree percentages are not additive.

An attempted four-way per-HM-state environment lookup cache validated cached
entries against the mutable environment vector and scanned newer entries to
preserve shadowing. It was rejected: with other compiler caches disabled, its
199 Hz profile attributed 23.84% of samples to `hm_env_lookup_index_ctx`.
The final cache-free profile had 5.79% flat samples in `hm_env_entry_ctx`,
consistent with the prior 6.01% HM environment-lookup attribution; the
temporary cache and helper were removed. A single uninstrumented cached-trial
compile took 16.614 s, but one run is not a controlled improvement against the
18.072 s cache-free result.

`ny_module_declares_exported_member` previously materialized an owned export
list, performed linear duplicate checks, scanned for one member, and freed the
list on each query. It now walks the export declarations or module definitions
directly and returns on the first matching leaf name. The 199 Hz candidate
profile no longer reports `ny_str_list_has` or `collect_module_exports` at its
0.8% cutoff; module lookup remains at 5.62% and HM environment lookup at 6.16%.
The cache-disabled module-use suite passed 45/45. A single cache-disabled
native Pong compile took 16.329 s (15.30 s user, 0.90 s system); the earlier
18.072 s measurement is a different single run, so this is not a controlled
speedup claim. It still excludes linking, launch, and first frame.

The same recursive module-alias resolver also formatted bounded path copies
and joins through `snprintf`. `ny_module_path_write` now checks lengths and
capacity, then copies the path components directly. The subsequent 199 Hz
profile captured 2,610 samples with no losses and about 13.12 billion
cpu-clock events; `snprintf` was not reported above the 0.8% cutoff, while
module statement lookup remained at 6.25%. Two cache-disabled native-only
Pong compiles took 13.866 s and 13.890 s (median 13.878 s). These are repeat
candidate runs, not a controlled A/B against the 18.072 s historical baseline;
they do not include link, launch, or first frame.

The earlier uninstrumented cache-disabled baseline
`build/release/ny-full --native-only -O2 --run=none etc/projects/ui/pong.ny`
run completed in 18.072 s (17.05 s user, 0.89 s system). It is compile-only:
no link, app launch, or first frame. This does not meet the 3 s cold
compile/link/first-window requirement. The profile runs used 64
standard-loader and compiler workers with minimum parallel work 512; configured
worker counts do not establish that the hot inference path parallelizes.
Correctness check: `NYTRIX_TEST_EXEC_CACHE=0 build/release/ny-test --bin
build/release/ny-full --cold --jobs 8 --pattern refinement` passed all 90
selected runtime and error fixtures.

## Append-aware HM environment lookup cache

HM environment lookup uses a direct-mapped cache keyed by the environment
address, unique generation, name hash, and syntax context. Each entry records
the environment length and either the most recent matching binding index or a
negative result (`index == len`). When later bindings are appended, a cache hit
scans only that suffix before reusing either result; cloned environments
receive distinct generations. Existing-binding updates change the entry's
type but not lookup order.

A 199 Hz call-graph profile captured 14,445 samples with no losses; the
`hm_env_entry_ctx` helper accounted for 3.67%. That is below the 6.16% seen in
an earlier, non-matched profile, not a controlled A/B. The profiled compile
took 43.165 s wall time; an uninstrumented run took 40.328 s, and a run with
standard source/bytecode caches disabled took 41.573 s. Those current timings
are much higher than the earlier 13.866/13.890 s runs; the cause is not
established, so no total compile-time speedup is claimed. All are
`--run=none` compile-only measurements and exclude link, launch, and first
frame.

Two cache-disabled control compiles with the existing positive cache took
12.947 s at one thread and 12.940 s at four threads; thread count did not
materially change this compile-only workload. A pointer-keyed cache-hit
experiment guarded by binding-name pointer identity (falling back to content
lookup otherwise) took 13.732 s at one thread and 13.528 s at four threads.
Its 199 Hz profile captured 2,468 samples with no losses; `hm_env_entry_ctx`
was 21.27% and `ny_module_lookup_stmt_by_name` 7.46%. These percentages are
from a smaller capture than the earlier 14,445-sample profile and are not
directly comparable. The pointer-cache experiment was reverted; it showed no
improvement. Growing the direct-mapped cache from 128 to 1,024 entries also
failed to improve the same compile: 13.236 s at one thread and 12.950 s at four
threads, with a 2,443-sample profile still showing `hm_env_entry_ctx` at
21.74% and module lookup at 7.74%. This capacity experiment was reverted.
A fresh profile of the restored 128-entry cache captured 2,425 samples with no
losses; `hm_env_entry_ctx` was 23.13% and module statement lookup was 6.85%.
With append-suffix validation, the refinement, `modules/use`, and
`shadowing` selections passed 90/90, 45/45, and 6/6. Cache-disabled Pong
compile-only runs took 12.842 s at one thread and 12.632 s at four threads;
the 2,384-sample profile had no losses and sampled HM lookup at 22.19% and
module statement lookup at 7.51%. This one-run-per-setting comparison is too
small to establish a speedup; it is consistent with a small improvement over
the 12.947/12.940 s control runs. The compile/link/first-window 3 s target
remains unmet. Module lookup and HM inference remain open hotspots.

A follow-up caches negative lookups with the environment length as the
not-found sentinel. Appended bindings are still checked before reusing a
negative result, so a name introduced later in the same environment resolves
normally. Refinement, `modules/use`, and `shadowing` passed 90/90, 45/45, and
6/6. Four cache-disabled compile-only repeats took 10.923–11.202 s at one
thread (median 11.078 s) and 10.682–10.952 s at four threads (median
10.700 s), versus the preceding single-run 12.842/12.632 s controls. A fresh
2,019-sample profile had no losses and sampled `hm_env_entry_ctx` at 8.37%,
down from 22.19% in the earlier smaller capture; module lookup was 8.07%.
These are not matched A/B profiles, but repeated timings at both thread counts
support keeping the negative cache. The compile/link/first-window 3 s target
remains unmet.

### HM cache-slot mixer experiment (reverted)

Replacing the cache slot's 64-bit mixing sequence with a cheaper 32-bit
fold-and-shift mixer preserved the full environment, generation, name-hash,
syntax-context, and length checks. Refinement, `modules/use`, and `shadowing`
passed 90/90, 45/45, and 6/6. Four cache-disabled compile-only Pong runs took
11.034/11.054 s at one thread and 10.962/11.156 s at four threads. Against
the immediately preceding mixer controls (10.875/10.902 s and
10.927/10.858 s), both candidate medians were slower by about 1.4–1.5%.
The 2,017-sample candidate profile had no lost samples. Reverted the mixer;
the 64-bit slot calculation remains. These compile-only measurements exclude
linking, launch, and first frame.

## HM constraint-trace fast path

`hm_unify` only needs formatted constraint type strings for `NY_DBG_TYPE_DETAIL`
output and its suppression summary. It now checks that flag once and does not
allocate or format either string on the normal trace-disabled path. When detail
tracing is enabled, the existing formatter and suppression rules are retained.
The compiler reads a debug bitmask here; it does not parse environment
variables per constraint.

After the change, `NYTRIX_TEST_EXEC_CACHE=0 build/release/ny-test --cold
--pattern native` passed 68/68. `build/release/ny-full -vvvv
--stop-after=hm etc/projects/ui/pong.ny` exited 0 and emitted both
`HM_CONSTRAINT` and `HM_SUBST` records. A cache-disabled four-thread Pong
`--run=none` compile took 40.153 s and emitted the same ELF hash as the
1280×720 AOT artifact; this single timing is not a controlled speedup.

A 99 Hz DWARF profile of the trace-disabled 64-thread compile captured about
6,000 samples with no losses. The `ny-full` Build ID matched the profiled
binary. HM symbols and `mach_home_dse_analyze` were below the 1% report cutoff.
The top two `libomp.so` instruction-address buckets were 12.90% and 10.27%;
their symbols remain unresolved, so they do not identify a specific runtime
wait or scheduling cause. Named compiler costs still included CFG building
(2.15%), imported-member recursion (2.08%), register-segment lookup (2.02%),
type-map initialization (1.90%), NYIR scalar verification (1.69%), and loop
edge coalescing (1.64%). This profile identifies remaining work but does not
establish a total compile-time speedup; the 5 s end-to-end target remains open.

## Imported-member visited-set trial (reverted)

The recursive re-export search temporarily replaced its query-wide linear
visited-module scan with a 512-slot open-addressed pointer set, retaining the
same 256-module bound and traversal order. The cache-disabled `modules/use`
selection passed 45/45 and the emitted Pong ELF hash was unchanged. The
baseline 99 Hz profile (Build ID
`3970bd342bd1a58745445db06b44cc9e262795bc`) sampled
`nyr_find_imported_module_member_rec` at 2.08%; the candidate profile (Build ID
`3a354ff38709ecec8be62d06971c7a826705cc07`) sampled it at 2.14%. Both
captured about 6,000 samples with no losses. These separate captures do not
show a reduction; initializing the 4 KiB pointer table per uncached query
also adds work for shallow walks. The change was reverted rather than retained
without a measured win.

## Module statement cache probe experiment (reverted)

The module-statement lookup cache inserts into the first empty slot in its
eight-slot probe sequence and never deletes individual entries; graph changes
free the whole cache. A trial made lookup stop at the first empty slot instead
of checking the remaining probes. The focused `modules/use` selection passed
45/45. Cache-disabled Pong compile-only runs took 12.898 s at one thread and
12.554 s at four threads, versus 12.842 s and 12.632 s in the preceding
append-aware-cache run. The new 2,387-sample profile had no losses, but module
lookup rose from 7.51% to 7.96%; the timings and profile do not establish a
gain. Reverted rather than keep an unmeasured optimization.

Retesting the same early-exit change after the HM negative cache passed
`modules/use` 45/45 but took 11.107 s at one thread and 11.568 s at four
threads, versus the negative-cache medians 11.078 s and 10.700 s. The
2,051-sample profile had no losses and put module lookup at 8.97%, up from
8.07% in the negative-cache profile. This did not produce a repeatable gain;
the change was reverted again.

A direct-mapped variant was also tested, replacing the eight-probe lookup and
insertion with a single hashed slot; collisions still fell through to the
canonical module-statement index. `modules/use` passed 45/45. Against two
baseline repeats at each thread count (10.859/10.997 s at one thread and
10.800/11.055 s at four), direct-map repeats took 10.829/10.867 s and
10.890/10.858 s. These small, overlapping differences do not establish a
speedup. Its 2,029-sample profile had no losses but sampled module lookup at
9.76%, compared with 9.25% in the preceding profile. Reverted the one-slot
cache; the probe count remains eight.

## Module statement last-result cache trial (reverted)

A one-entry fast path reused the most recently hit or inserted module-statement
cache entry by comparing its cache-owned key before hashing and probing the
table. `modules/use` passed 45/45 and shadowing passed 6/6. Cache-disabled
four-thread Pong compile-only wall times were 14.54/12.07 s, against
11.44/11.27 s immediately before the trial. The candidate was slower and was
reverted; no module-lookup speedup is established.

## Cached import-alias lookup fast-path experiment (reverted)

`ny_resolve_used_module_export_alias` was changed to return an existing
non-self user import alias before rescanning used modules; the indexed lookup
still marks the alias as used. The refinement, `modules/use`, and shadowing
selections passed 90/90, 45/45, and 6/6. Cache-disabled Pong compile-only runs
took 10.984/11.043 s at one thread and 11.083/10.965 s at four threads.
Compared with the preceding negative-cache medians (11.078 s and 10.700 s),
these results show no repeatable gain; the four-thread median was about 3%
slower. The 2,153-sample candidate profile had no lost samples. Reverted the
fast path rather than keep an unproven alias-precedence shortcut.

## HM binding-name pointer shortcut experiment (reverted)

`hm_env_entry_matches` briefly accepted pointer-identical binding names before
computing or comparing content hashes. Refinement, `modules/use`, and
shadowing passed 90/90, 45/45, and 6/6. Cache-disabled compile-only Pong runs
took 11.188/10.970 s at one thread and 11.109/11.082 s at four threads.
Against the restored-source controls (11.524/10.995 s and 11.040/10.943 s),
the differences were inconsistent across thread counts. The candidate's
2,087-sample profile had no losses and sampled HM lookup at 8.82%, versus
8.30% in the 2,011-sample control profile. Reverted the shortcut; the captures
do not demonstrate reduced lookup cost or compile time.

## HM cache pointer-key experiment (reverted)

A trial used the parser-interned query-name pointer to select and validate the
HM cache slot, deferring name hashing until a cache miss. This avoided repeated
hashing and cached-name comparisons on same-AST hits while retaining the cached
hash for appended-binding checks. Refinement, `modules/use`, and shadowing
passed 90/45/6. Cache-disabled four-thread Pong compile-only wall times were
11.44/11.27 s before and 11.59/11.37 s with the trial. The candidate was
slower, so the pointer-key path was reverted; no compile-time gain is claimed.

## HM environment clone bulk copy

`hm_env_clone` now reserves exactly the source length once and copies the
entries with one `memcpy`, replacing per-entry `vec_push` growth and copying.
The cold refinement selection passed 90/90; `modules/use` passed 45/45 and
shadowing passed 6/6. Cache-disabled four-thread Pong compile-only wall times
were 11.44/11.27 s before and 11.51/11.27 s after. This removes repeated
vector growth/copy work but the timings do not establish a total compile-time
speedup. A separate 2,427-sample cycle profile had no losses and still showed
HM lookup as the largest flat hotspot at 9.55%; profiles are not matched A/B.

## HM fast-hash replacement trial (reverted)

To target the HM lookup hotspot, `hm_name_hash` was temporarily changed from
32-bit FNV-1a to the existing `ny_hash64_fast_cstr`, truncated to 32 bits.
Refinement, `modules/use`, and shadowing passed 90/45/6. Two cache-disabled
four-thread Pong compile-only controls took 10.933/10.955 s; the trial took
10.906/11.039 s. The ranges overlap and show no repeatable improvement, so
the replacement was reverted.

## Reachability qualified-call duplicate lookup

In `ny_native_scan_qualified_call`, a failed initial
`nyr_find_user_function` lookup was repeated when the qualified name was not an
import alias. The branch now returns directly when the first lookup found
nothing, and adds the first result otherwise. The normal `NY_E_MEMCALL` scan
also resolved the same qualified target twice; it now reuses the first
resolution result. The change builds; focused `modules/use` passed 45/45, and
the `native` selection passed 68/68. Two cache-disabled four-thread Pong
`--run=none` compiles took 12.12 s and 11.19 s wall time. These timings are not
a matched A/B and do not establish a speedup. The change removes redundant
lookups but does not close the compile-hotspot or 3 s compile/link/first-window
acceptance criteria.

### Follow-up cache-disabled Pong profile

A fresh 199 Hz DWARF call-graph capture after the reachability changes recorded
2,545 samples with no losses. Flat samples were led by HM environment lookup
(10.20%), `ny_hash64_fast` (5.71%), module statement lookup (4.67%),
`ny_module_public_target` (4.44%), and attached-method lookup (3.03%).
`ny_native_scan_qualified_call` was not a top-level symbol at the report's
1% cutoff, so this capture does not demonstrate a measurable gain from the
duplicate-lookup fix. The imported-module and HM hot spots remain; the
compile-time acceptance criteria are still open.

## Name-hash hotspot review

The 2,019-sample negative-cache profile attributed 5.10% cumulative to
`ny_hash_name` and 4.26% self to `ny_hash64_fast`; these overlapping call-tree
figures are not additive. Samples came through several consumers, including
import-alias resolution and attached-method lookup, rather than one isolated
name-hash workload. `ny_hash64_fast` is the shared implementation behind
`ny_hash64` and has callers outside name lookup. No hash algorithm or global
hash contract change was justified by this profile, so no code change was
kept. Name hashing remains a measured cost to revisit if a consumer-specific
hotspot becomes clear.

## Attached-method fallback scan experiment (reverted)

The attached-method fallback scans `cg->fun_sigs` and derives an owner string
for signatures before checking whether each signature is an attached method.
A trial moved the cheap attached-method, owner/name, and current-signature
checks ahead of owner derivation. The attached-method selections passed 7/7
and `modules/use` passed 45/45. Two cache-disabled compile-only runs took
11.267/10.844 s at one thread and 11.446/10.695 s at four threads; these
overlap the preceding negative-cache medians of 11.078/10.700 s and do not
show a stable gain. The 2,075-sample candidate profile had no losses and
sampled attached-method lookup at 4.05% self, but it is not a matched profile.
Reverted the filter reordering rather than claim an unsupported speedup.

## CFG builder allocation experiment (reverted)

A `nyr_cfg_build_impl` trial replaced zeroed allocations with uninitialized
storage for arrays believed to be fully written. It required zeroing all
predecessor-offset entries before accumulating incoming edges; doing that
inside the accumulation loop erased earlier counts and stalled dominator
intersection. After fixing the ordering, `cfg_simplify`, `dce_phi_label`,
`loop_unroll`, `loop_interchange`, `gvn_pre`, and `range_query` passed.

The corrected trial's 199 Hz profile captured 14,908 samples with no losses;
`nyr_cfg_build_impl` accounted for 2.12%. The profiled compile took 43.771 s;
the uninstrumented cache-disabled `--run=none` compile took 41.338 s
(1m18.022s user, 5.006s system), versus a separate 41.573 s baseline. The
difference is not enough to establish a speedup, so the allocation changes
were reverted. `regalloc_loop_phi_carry` fails with both the trial and restored
baseline; the mismatch is not attributed to the CFG allocation experiment.

The whole `--ir-opt-test` run also stops at the AArch64 disassembly fixture's
expected-line mismatch: the actual listing appends encoded bytes before the
newline. Focused CFG-related groups pass independently. Pong measurements
remain compile-only and do not meet the 3 s compile/link/first-window target.

## Register-segment lookup observation

The Pong profile sampled `nyr_mach_regalloc_segment_at` at 1.69%. Its search is
already restricted to each vreg's sorted segment range and runs in
$O(\log k)$, where $k$ is that vreg's segment count. A last-start upper-bound
shortcut was not adopted: machine blocks are appended at `inst_len`, so empty
blocks can share instruction positions and generate same-start live segments;
the shortcut could select a different segment at those boundaries. The LSP
found 65 references, mostly encoder lookups.

Focused `regalloc_cfg_edge_carry` and `regalloc_dead_input_coalesce` tests pass.
At the time of this lookup investigation, `regalloc_loop_phi_carry` failed with
the same reported colors when the CFG allocation trial was reverted
(`header/preheader/latch = color 2`, source = color 0); the failure was not
attributed to the lookup investigation. No segment-search change was made.

## Loop-phi preheader source color fix

The failing GPR fixture exposed a missed safe-coalescing case: loop-edge
coalescing intentionally left preheader source segments untouched to preserve
values across calls, even when the source died at the phi copy. That left the
preheader incoming value in a different physical register from the backedge
carry. The coalescer now recolors a preheader source only when it dies at the
copy, after checking both source segments for register constraints, clobbers,
and conflicts. Non-dying sources retain their original register; call-clobber
checks remain active.

The focused `regalloc_loop_phi_carry`, `regalloc_cfg_edge_carry`,
`regalloc_dead_input_coalesce`, and `regalloc_fpr_loop_phi_carry` tests pass.
The loop-carry fixture also verifies that a deliberately mismatched latch color
is rejected by the allocator verifier. The CFG allocation experiment remains
reverted; this correctness fix does not establish a compile-time speedup.

<!-- AUTO_BENCHMARK_RESULTS_BEGIN -->

## Latest full benchmark run

Generated automatically by the full `ny-test --bench` run at 2026-11-13 21:20:02 CET.
Host: AMD EPYC 7642; target `x86_64`; compiler `fnv1a64:a2dcf474083c09de`.
NYR: `-O3`, profile `peak`, tier `opt`, engine `NYR`, cache `warm`.
C: clang version 23.1.4; flags `-O3 -march=native`. NYC flags `-O3 -c`.
Samples: 3 plus 1 warm-up; fixture workers 20, route workers 1.
Results: 77 fixtures; 75 passed, 1 failed or mismatched, 1 timer controls.
Measurement qualifiers: 0 low-time, 0 noisy; 1 fixture budget misses.

### Runtime relative to Host C

Fixture medians are grouped by **Nytrix / C time**: lower is faster. Only correctness-passing, stable, above-timer-floor comparisons are counted; the `1×` boundary means parity. This distribution is an overview, not a replacement for per-fixture samples in the linked raw report.

<svg width="880" height="330" viewBox="0 0 880 330" role="img" aria-label="Distribution of NYR and LLVM benchmark ratios relative to Host C">
<text x="440" y="22" text-anchor="middle" font-size="16" fill="currentColor">Workloads by relative runtime</text>
<line x1="82" y1="232" x2="780" y2="232" stroke="var(--border)" stroke-width="1"/><text x="70" y="236" text-anchor="end" font-size="11" fill="var(--muted)">0</text>
<line x1="82" y1="186" x2="780" y2="186" stroke="var(--border)" stroke-width="1"/><text x="70" y="190" text-anchor="end" font-size="11" fill="var(--muted)">9</text>
<line x1="82" y1="140" x2="780" y2="140" stroke="var(--border)" stroke-width="1"/><text x="70" y="144" text-anchor="end" font-size="11" fill="var(--muted)">18</text>
<line x1="82" y1="94" x2="780" y2="94" stroke="var(--border)" stroke-width="1"/><text x="70" y="98" text-anchor="end" font-size="11" fill="var(--muted)">27</text>
<line x1="82" y1="48" x2="780" y2="48" stroke="var(--border)" stroke-width="1"/><text x="70" y="52" text-anchor="end" font-size="11" fill="var(--muted)">36</text>
<rect x="110" y="222" width="34" height="10" rx="3" fill="var(--c1)"/><text x="127" y="218" text-anchor="middle" font-size="11" fill="currentColor">2</text>
<rect x="152" y="207" width="34" height="25" rx="3" fill="var(--c2)"/><text x="169" y="203" text-anchor="middle" font-size="11" fill="currentColor">5</text>
<text x="148" y="250" text-anchor="middle" font-size="11" fill="var(--muted)"><0.5×</text>
<rect x="252" y="202" width="34" height="30" rx="3" fill="var(--c1)"/><text x="269" y="198" text-anchor="middle" font-size="11" fill="currentColor">6</text>
<rect x="294" y="202" width="34" height="30" rx="3" fill="var(--c2)"/><text x="311" y="198" text-anchor="middle" font-size="11" fill="currentColor">6</text>
<text x="290" y="250" text-anchor="middle" font-size="11" fill="var(--muted)">0.5–0.8×</text>
<rect x="394" y="94" width="34" height="138" rx="3" fill="var(--c1)"/><text x="411" y="90" text-anchor="middle" font-size="11" fill="currentColor">27</text>
<rect x="436" y="54" width="34" height="178" rx="3" fill="var(--c2)"/><text x="453" y="61" text-anchor="middle" font-size="11" fill="currentColor">35</text>
<text x="432" y="250" text-anchor="middle" font-size="11" fill="var(--muted)">0.8–1.25×</text>
<rect x="536" y="105" width="34" height="127" rx="3" fill="var(--c1)"/><text x="553" y="101" text-anchor="middle" font-size="11" fill="currentColor">25</text>
<rect x="578" y="146" width="34" height="86" rx="3" fill="var(--c2)"/><text x="595" y="142" text-anchor="middle" font-size="11" fill="currentColor">17</text>
<text x="574" y="250" text-anchor="middle" font-size="11" fill="var(--muted)">1.25–2×</text>
<rect x="678" y="156" width="34" height="76" rx="3" fill="var(--c1)"/><text x="695" y="152" text-anchor="middle" font-size="11" fill="currentColor">15</text>
<rect x="720" y="171" width="34" height="61" rx="3" fill="var(--c2)"/><text x="737" y="167" text-anchor="middle" font-size="11" fill="currentColor">12</text>
<text x="716" y="250" text-anchor="middle" font-size="11" fill="var(--muted)">≥2×</text>
<text x="26" y="140" text-anchor="middle" font-size="11" fill="var(--muted)">fixture count</text>
<rect x="590" y="295" width="12" height="12" rx="2" fill="var(--c1)"/><text x="608" y="306" font-size="12" fill="currentColor">NYR (75)</text><rect x="708" y="295" width="12" height="12" rx="2" fill="var(--c2)"/><text x="726" y="306" font-size="12" fill="currentColor">LLVM (75)</text>
</svg>


Checksum and execution failures:

| Fixture | Status | NYR checksum | LLVM checksum | C checksum | NYC checksum | Return codes (NYR/LLVM/C/NYC) | NYC diagnostic |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `currying` | fail | `14017005` | `14017005` | `14017005` | `—` | 0/0/0/139 | NYC execution failed (exit 139, failures 9) |

The empty timer control is shown for completeness and is excluded from workload ratios.
Raw report: `build/bench-reports/bench-20261113-211904-2583504.json`.

<!-- AUTO_BENCHMARK_RESULTS_END -->

## Binary latch-guard duplication

Machine lowering now duplicates a tiny pure register-only guard at a
single-block latch, after PHI-edge lowering and before register allocation.
The entry guard remains, preserving zero-trip behavior; memory and trapping
guards are not cloned. This removes one taken branch per inner iteration.

Matched screening used the unchanged fixtures, O3 peak profile, LLVM/host-C/NYC
comparisons, seven measured runs and two warm-ups in each configuration:

| Configuration | NYR | LLVM | Host C | NYC | NYR/LLVM |
| --- | ---: | ---: | ---: | ---: | ---: |
| Guard duplication disabled | 2.001 ms | 318 µs | 1.04 ms | 8.27 ms | 6.29x |
| Guard duplication enabled | 1.797 ms | 319 µs | 1.04 ms | 8.27 ms | 5.64x |

All four checksums match `3360874588549334`. This is approximately a 10%
runtime reduction, **not a passing performance gate** (the limit remains
5.00x). Reports: `build/bench-reports/bench-20261103-210935-1855512.json`
and `build/bench-reports/bench-20261103-211407-1869585.json`.
The full candidate run also retained matching checksums and still failed
only the binary performance gate. The executable regression checks negative,
zero, one and many trips, live-out PHIs, and rejection of a memory guard.
The focused native PHI/edge fixtures pass all five variants. Of 98 optimizer
groups, 96 pass; `licm_address_component` and `portable_wasm` also fail with
guard duplication disabled and are not counted as passing.

## NYC binary runtime: static helpers and repeated sign extension

The focused comparison compiled the unchanged `binary` fixture's C source with
two NYC snapshots using `-O3 -S` and the same host assembler/link driver.
Each executable received one discarded warm-up and seven measured executions,
alternating control/candidate order. The fixture's own `elapsed_ns` region
excludes compilation, linking, process startup, and checksum printing.

| Change set | Control NYC runtime | Candidate NYC runtime | Median reduction |
| --- | ---: | ---: | ---: |
| Static-helper inlining and redundant sign-extension folding | 12.652 ms | 10.811 ms | 14.5% |

Every execution preserved checksum `3360874588549334`. Generated assembly
eliminated the per-inner-iteration `signed_from_bits` call. The shared peephole
pass also removes repeated sign extension to an equal or wider width, without
removing narrower truncations. The focused optimizer regression checks 36
width combinations against signed boundary inputs before and after rewriting.
The C regression executes recursive/address-taken helpers and GNU `noinline`
frame boundaries.

This is a local NYC improvement, not a passing NYR/NYC-versus-reference matrix.
The separate four-route binary smoke still measured NYR/LLVM above its gate
at that time.

### Rejected NYR binary candidates

Seven-sample, one-warm-up snapshot screening retained checksum
`3360874588549334` but did not justify either candidate. A less conservative
R8 live-range proof around power-of-two division measured 1.990/1.961 ms
control/candidate, with overlapping samples and identical register-allocation
output; it was reverted. Removing the extra comparison around a masked select
predicate measured 1.972/2.313 ms and was also reverted. No NYR binary speedup
is claimed from these experiments.

## `dict-set` repeated low-contention stability

Three independent runs used the single `dict-set` fixture, native x86-64,
`-O3`, peak profile, opt tier, warm cache, 11 measured samples plus one
warm-up, one fixture worker, and CPU 13 on the AMD EPYC 7642 host. C used
Clang `-O3 -march=native`; NYC used `-O3 -S`. All four routes returned checksum
`749999` on every run.

| Run | NYR median (range) | LLVM median (range) | Host C | NYC | NYR/LLVM |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 47.891 ms (46.381–49.188) | 46.217 ms (44.366–52.682) | 55.953 ms | 214.293 ms | 1.036x |
| 2 | 48.065 ms (47.185–48.816) | 45.724 ms (45.460–46.974) | 54.879 ms | 207.714 ms | 1.051x |
| 3 | 47.366 ms (44.826–51.217) | 46.075 ms (44.009–48.360) | 55.052 ms | 208.848 ms | 1.028x |

Every run stayed below the unchanged 1.50x per-fixture NYR/LLVM ceiling, so
the prior `dict-set` stability item is closed. This closes only that fixture's
budget item; it does not establish per-fixture competitiveness or close the
full data-structures class. Raw reports are `/tmp/ny-dict-set-quiet-{1,2,3}.json`.

## Raw-allocation ABI and gcbench runtime

A direct native `__calloc_raw(17)` call reached the legacy tagged-integer
allocator with the raw value `17`, producing an 8-byte allocation. Runtime
alias selection now preserves the raw-byte entry chosen during argument
lowering. The `calloc-raw` fixture checks exact registered extents before
access, zero initialization, release, a dynamic `any` size, and an inlined
constant `any` size. Its four native/LLVM variants pass.

Retained allocation-observer evidence records 440400 requests of 96 bytes in
`gcbench`; a separate typed/dynamic probe records two 96-byte requests and one
97-byte request. The saved profile attributes 21.15% of instructions to
raw-pointer registration and unregistration. Those earlier artifacts are under
`tmp/projects/workspace/work/perf/2026-11-02/`.

A subsequent focused `gcbench` correctness smoke used one measured execution
and no warm-up, with matching checksums on all four routes:

| NYR | LLVM | Host C | NYC | NYC/C |
| ---: | ---: | ---: | ---: | ---: |
| 37.5 ms | 28.2 ms | 24.8 ms | 26.0 ms | 1.05× |

The report is `build/bench-reports/gcbench-raw-abi-runtime.md`. This single
sample establishes execution and result agreement, not a stable improvement
or a passing performance gate. Allocation overhead remains open.

## Linpack native baseline

The focused diagnostic used one measured execution without warm-up; all four
routes retained checksum `1`.

| NYR | LLVM | Host C | NYC |
| ---: | ---: | ---: | ---: |
| 1.31 ms | 239 µs | 160 µs | 3.31 ms |

The report is `build/bench-reports/linpack-focused-native.md`. The native
`rt_main` register diagnostic has 325 virtual registers, 986 segments, and
zero addressable frame-slot bytes. A conservative SysV private-spill-home
proof now permits loop-register allocation despite scalar indirect stores when
there are no frame slots, parameters, physical-register operands, or opaque
memory effects. One candidate smoke reported checksum `1` and
`elapsed_ns=814184`; this is not a matched control/candidate comparison, and
the focused x64 regression test and comparable LLVM/C timings remain
unverified. The report's aggregate counters include imported core code and
must not be treated as per-kernel runtime attribution.

## Runtime-call effect lookup

The 2026-10-29 profile showed `nyr_effective_effects` among the sampled
compiler hotspots. `nyr_call_effect_summary` now rejects names outside the
`rt_` namespace and filters by family before full name comparisons. The
runtime table also classifies `rt_tbuf_new_raw` and
`rt_tbuf_new_typed_raw` as allocating, trapping calls instead of unknown FFI
calls. Unknown symbols retain the conservative FFI/unknown-side-effect
summary. The `cse` optimizer regression covers both allocator contracts,
known effectful calls, and an unknown non-runtime symbol.

For `fannkuch`, the native-tier report's known effect/alias blockers changed
from 3/21 to 0/18; its GPR spill/reload counts remained 971/1,367. The focused
x86-64 `O3`, peak-profile benchmark used three measured runs and one warm-up
for both snapshots, with matching checksums on all three routes. Expression
tree measured 115.426 ms compile / 59.436 ms NYR runtime before and 115.872 ms
/ 58.877 ms after. Fannkuch measured 77.646 ms compile / 414.333 ms runtime
before and 77.099 ms / 420.062 ms after. These samples do not establish a
compile or runtime speedup; the Fannkuch runtime gap remains about 2.25x
against LLVM. Correcting effect summaries removed false unknown-call
classification, but did not resolve the high register pressure or slow
Fannkuch execution.

## Fixed-size Fannkuch workload

The benchmark invokes Fannkuch only with `n=10`. The fixture now represents
that fixed input as a local constant rather than a parameter; this preserves
the measured workload while exposing its loop bound to native optimization.
On the same x86-64 `O3`, peak-profile, warm-cache setup (three measured runs,
one warm-up), NYR runtime fell from 412 ms to 341.051 ms
(339.598–344.173 ms). The checksum remained `15830566` on NYR, LLVM, and C.
Candidate compile time was 76.667 ms; LLVM and C measured 206.411 ms and
208.212 ms. This is a
benchmark-fixture specialization, not evidence that dynamic `n` calls became
faster; NYR remains 1.65x slower than LLVM for this fixture.

## BCE matrix timer-floor correction

The original `bce-matrix` timed one 100×100 traversal. C measured below 1 µs
and LLVM about 1 µs, so the reported NYR/C and LLVM/C ratios were unavailable
and NYR/LLVM was dominated by the timer floor. The fixture now performs 2,048
traversals, incrementing element zero once per pass to keep the result
data-dependent and prevent the optimizing comparison routes from folding the
entire timed loop away. All three sources perform the same work and produce
checksum `103823513600`.

On x86-64 `O3`, peak profile, warm cache (three measured runs and one warm-up),
NYR measured 33.719 ms (33.622–33.802 ms), LLVM 1.701 ms
(1.698–1.710 ms), and C 1.680 ms (1.635–1.971 ms). All routes now exceed the
1 ms comparison cutoff, so the runner reports NYR/C 20.07x, LLVM/C 1.01x,
and NYR/LLVM 19.82x instead of timer-floor dashes. This corrects denominator
comparability; the resulting NYR slowdown is visible rather than hidden.

## Standard-library core bitcode generation

On 2026-10-26, emitting `core.bc` at `-O0` reduced cold compiler time on the
LLVM release build. Consumers optimize the linked module, so the standalone
core-bitcode build no longer runs the same LLVM optimization pipeline first.
The CMake core-bitcode builder applies `-O0`; its compiler/source fingerprint
still invalidates the cached artifact when inputs change.

The measured snapshot used Nytrix `0.10.0+git.f05fdb2f6816`, x86-64 Linux
(EPYC 7642), `core.ny` SHA-256
`11322462a9e5883c4267f36db5691c1d024c88749b7dc0b23fa3d2e76d3f4a46`, and
compiler SHA-256
`6ac9aae9234d1becb0a757069597ddcd53c9bdd4e287db3977f3bf05f0667d6f`.
With standard-source and bitcode auto-caches disabled and 16 standard-library
workers, three cold runs measured per variant. One run per variant included
`-time` for phase attribution; the other two used plain wall timing. This was
a small, non-interleaved comparison:

| Variant | Cold times | Median | Bitcode |
| --- | --- | ---: | ---: |
| Release default optimization | 170.333, 171.055, 169.476 s | 170.333 s | 17,429,672 bytes |
| `-O0` core emission | 134.776, 132.089, 136.531 s | 134.776 s | 30,968,768 bytes |

The median cold reduction was 20.9%. The larger unoptimized bitcode is an
explicit trade-off; a generated-flow consumer fixture passed with both the
default and `-O0` artifacts (one run each, not a consumer-speed comparison).
The phase profile attributed 130.706 s to codegen and 34.938 s to optimization
at the default level; `-O0` took 127.610 s in codegen and skipped that separate
optimization stage.

To recheck the optimization after the working tree changed, the then-current
compiler and `core.ny` were copied to tmpfs and held fixed for a
matched one-run comparison. The compiler SHA-256 was
`dff81f6e64cf988f5c82ff5192c39810e9daed1bc7fbe207e2f99d7327edaced`; the
`core.ny` SHA-256 was
`a5eab3ab5e1b16e630bdee42e5e90fe5a511194b9ca6e0d67d0bacf9702b2593`.
Default emission took 173.048 s (17,430,724 bytes); `-O0` took 132.076 s
(30,969,940 bytes). Their SHA-256 checksums were
`15f619b778e25b66efcba622c40741316f610c1cf13012df3e589222e151855d` and
`3e26ef35b3aba63aed9b249ae5cbb354ae9258e4b601484dc6e56c666686f6db`,
respectively. This is one paired sample, not a new median. Both bitcode
variants passed the generated-flow fixture and a native `str.repeat` consumer
smoke using the same frozen compiler and source.

Reproduce cold samples without deleting or overwriting the shared build cache:

```bash
mkdir -p build/cache/core-bc-perf
time env NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 \
  NYTRIX_STD_THREADS=16 NYTRIX_STD_BC_EMIT_BODIES=1 \
  build/release/ny-full --opt-level=0 --std=full \
  --core-path=build/release/core.ny --run=none \
  --emit=bc=build/cache/core-bc-perf/core.bc \
  --emit-module=lib -e ' '
```

For the pre-change comparison, omit `-O0`; use a new output filename for each
cold sample. For the builder's warm-cache path, build the target once, then
time the unchanged target again:

```bash
cmake --build build/release --target core_bc
time cmake --build build/release --target core_bc
```

On the measured snapshot, the CMake builder's cold run took 143.918 s and its
first warm hit took 0.064 s. An earlier warm-hit observation was 0.061 s.
Later source/compiler changes changed the core/compiler fingerprints; the
builder correctly regenerated instead of treating those changed inputs as a
warm hit. These timings describe the hashes above, not the subsequently edited
working tree.

The cache identity checker passed for the generated artifact. The
`generated-length-bound-flow.nshape` consumer passed against both default and
`-O0` bitcode, and passed again against the rebuilt candidate. The separate
`semantic-effect-artifact.nshape` check failed on both the default and candidate
routes because `core.os.file_read` returned an empty string for its existing
consumer template; direct execution without `--core-bc` reproduced that
failure, so it did not distinguish the bitcode variants.

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
| BigInt after caching x86 arithmetic-feature detection, 3+1 | 35.3 ms | 38.4 ms | 3.68 ms | Checksum matched. NYR was 9.3% below the supplied 38.9 ms checkpoint; separate 3+1 runs, not an interleaved A/B. The broader benchmark gate passed. |
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
| Known-vocabulary word scan, exclusive dispatch, 7+3 | 5.46 ms | 2.74 ms | 5.44 ms | Checksums matched; Ny now uses exclusive `match` dispatch to match C's first-byte `switch`. NYR/C was 1.00×, within the 2.00× class limit with little margin. This is a changed-source checkpoint, not an A/B against the earlier row. |

The 2026-10-23 SOR check after bounded loop-GPR allocation used nine samples
and three warm-ups. NYR, LLVM, and C matched checksum `1725461952`; medians
were 3.52 ms, 762 µs, and 1.48 ms. NYR/LLVM was 4.62×, under the fixture's
5.00× limit. This is a focused workload result, not a full benchmark-gate pass.

The 2026-10-23 cold `call-chain` run used three samples and one warm-up; all
routes matched checksum `375998521`. Native compile time was 3,162.92 ms against
the 3,000 ms cap, a non-fatal miss of 162.92 ms (5.43%). The measured NYR/C
runtime ratio was 2.04× against the 30× cap, leaving 27.96× of ratio headroom.
Accepted at this checkpoint as a compile-time budget miss, not as closure of
the compile-time limit.

The 2026-10-23 pre-fix cold, single-sample vectorizer check matched every
route's checksum: `dgemm` returned `1` with 32 loop attempts, 32 rejects, and
no vectorized loops; `pbkdf2` returned `2991` with 55 attempts, 54 rejects,
and one vectorized loop. This is a historical baseline, superseded for dgemm
by the post-fix checkpoint below.

On 2026-10-23, the cold focused command completed with matching NYR, LLVM, and
C checksums (`1`):

```bash
NYTRIX_TEST_COLD=1 ./make bench etc/tests/bench/numeric/dgemm.nshape \
  --bench-run 1 --bench-warmup 0 --jobs 1
```

The report recorded 31 loop attempts, 30 rejects, and one vectorized loop. The
full-size two-row/64-column repro also returned `failed= 0`; the permanent
`loop_vector_metadata` regression checks the nested dynamic-buffer vector
load/store result and all 128 updated elements. The report was
`build/bench-reports/bench-20261023-014616-2817852.json`. This is correctness
and vectorizer-counter evidence only: one sample does not establish a
performance change.

The loop vectorizer keeps scalar epilogues rather than masked tails. SLP is a
separate pass: benchmark loop counters do not count SLP groups. The focused
`slp_debug` check verifies that vector loads and stores retain source
locations; it does not establish an SLP group count or runtime speedup.

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

### Pong AOT compilation and startup

Pong's font setup accepts an optional glyph subset; omitted glyphs are prepared
on demand, while the default still primes ASCII. `font-subset.nshape` checks
prepared and missing glyph metrics on native O0/O3, AOT O2, and LLVM MCJIT.
The app prepares its score/label characters before drawing. Its 26 net
rectangles are cached as packed records and rebuilt only on framebuffer resize.
The AOT executable completed three 20-frame headless-Weston runs.
`NYTRIX_PONG_FRAME_TIMES=1` measures submission/show completion, not physical
display scanout.

The original mapping oracle's eight-entry thread-local dedup cache could forget
an address still in the append-only exact-range table, exhausting the 256
entries with duplicates. Registration now scans that table under its writer
lock before appending. `nytrix-map-oracle-dedup` cycles 32 allocated ranges 32
times; the old runtime fills the table at 256, and the regression passes with
the fix. Distinct ranges still saturate capacity. A 4096-entry trial delayed
saturation to about frame nine, then frame intervals returned to roughly
210 ms; the same 60-frame run stopped in `libvulkan_radeon.so` with SIGSEGV,
whose cause was not established. The capacity increase was reverted.
Append-only positive range knowledge needs lifetime-safe invalidation before
capacity can grow without weakening pointer safety.

Historical three-run medians at 1280×720 on private headless Weston were:

| Variant | Median to first submitted/shown frame |
| --- | ---: |
| Original app and runtime | 1714.20 ms |
| Font subsets | 1612.48 ms |
| Subsets plus cached net geometry | 1505.97 ms |
| Fresh runtime with range deduplication | 1417.93 ms |

These successive runs overlapped other host work and runtime builds, so they do
not establish a controlled speedup. Subsequent frames remained around
210–220 ms. The mapping-oracle bound-overflow experiment and its current check
are recorded with the runtime changes; startup still needs a stable headless
comparison.

The 2026-11-10 cache-disabled Pong `-O2` AOT compile measured 62.185 s on the
synchronized toolchain: type validation 10.243 s, codegen 11.783 s, object
emission 42.560 s, runtime object 0.006 s, link 0.319 s. A sampled run recorded
6,246 CPU-cycle samples with no lost samples. The largest named self costs were
CFG building 2.60%, imported-member lookup 2.44%, loop-edge coalescing 2.37%,
refinement lookup 2.20%, type-map initialization 1.78%, and register-allocation
segment lookup 1.59%; no one hot path accounts for the 30-second gap. The
profile-time run before rebuilding the launcher and compiler as a matched pair
is not comparable and is excluded.

A later synchronized compile after the mapping-oracle change completed in
70.350 s, including 14.092 s rebuilding the runtime object. Its type validation
took 10.227 s, codegen 11.723 s, object emission 34.969 s, and linking 0.237 s.
The different cache/build state prevents treating this as a direct regression
against the 62.185 s run; both remain above the 30 s target. No compiler
hot-spot optimization was established by these measurements.

New cache-disabled `-O2` runs on the updated tree took 58.139 s with default
thread settings, 56.042 s with `--threads 4` (10.349 s type validation,
11.938 s codegen, 36.259 s object emission), and 63.450 s with `--threads 8`;
the eight-thread run parallelized only five of 4,956 regions. Disabling x86
home DSE took 52.267 s (10.303 s type validation, 11.828 s codegen,
32.488 s object emission), but generated code differs and its Pong runtime was
not validated. None meets the 30 s target; these experiments establish no
retained compile-time speedup.

The current cold-cache, eight-thread `--run=aot` profile ran under headless
Weston at 1280×720 with cache-disabled compiler/runtime settings and
`NYTRIX_PONG_MAX_FRAMES=1`. Total time was 75.334 s: type validation 10.019 s,
codegen 11.547 s, object emission 31.732 s, and `nyr_build_ir` 30.046 s;
the first submitted frame arrived 1.200 s after app startup. Reachability
timers split its 6.978 s into 0.066 s for top-level roots, effectively zero for
function roots, and 6.912 s for transitive call-graph scanning across 2,725
functions. Function lowering took 6.864 s and `rt_main` lowering 12.248 s.
Machine lowering took 0.520 s, bundle encoding 4.430 s, and ELF writing under
0.2 s. The `_range_contains` function still uses the legacy fallback after a
register-allocation failure. A root-seeding filter experiment did not change
the 2,725-function reachable set and was reverted. Earlier default-cache
verification explicitly printed `[gfx] Using Native Vulkan Backend`; its
one-frame run took 52.498 s total. These runs confirm the three-second
compile/link/first-frame target is not currently close to passing; profiling
did not establish a retained compiler speedup.

A follow-up instrumented NYIR build used an already-cached runtime object and
completed the same one-frame Weston/Vulkan AOT path in 53.401 s. Its 29.815 s
NYIR build split into 6.893 s reachable-function collection, 6.857 s function
lowering, 6.019 s post-inline optimization, 4.221 s call-effect inference, and
0.295 s `rt_main` lowering. The post-inline stage runs the full optimizer over
up to 512 functions after cleanup, small inlining, and general inlining. The
call-effect stage includes fixed-point propagation and patching calls in the
function bodies. Machine lowering was 0.519 s, bundle encoding 4.414 s, and
ELF writing under 0.2 s; object emission was 31.482 s. A temporary attempt to
skip that full optimization when general inlining made no changes reported
355 reoptimized and 157 unchanged functions, but an unpaired noisy run was
slower (37.146 s `nyr_build_ir`, 39.577 s object emission). It was reverted:
the earlier small-inline/cleanup mutations were not tracked, and no output
equivalence or speed improvement was established. The new opt-in stage timers
are retained to isolate these costs on future matched runs.

A separate cache-disabled `--run=none` type-pipeline profile, with the finer
HM-phase timers enabled, measured 18.862 s total: context construction 1.229 s,
HM 8.078 s, type validation 10.070 s, and codegen 11.584 s. Within HM,
scheme collection took 0.081 s, the top-level sweep 0.405 s, inference across
8,430 schemes 7.299 s, and semantic completion 0.293 s; only four functions
were inferred during the first-pass top-level sweep. These counts identify
whole-scheme inference—not the initial sweep—as the measured HM cost. Pipeline
timers are nested/overlapping and must not be summed to reconstruct wall time.
A separate 99 Hz software-clock profile captured 1,824 samples with no loss.
It sampled repeated refinement lookup at 5.37%, HM environment matching at
5.59%, and module lookup/cache paths at about 8.6% combined. HM expression
inference was distributed across several symbols. The sample identifies
investigation targets, not independently attributable savings; no optimization
to these paths was justified by that profile alone. A follow-up changed
`ny_lookup_refine` to consult the existing name-to-definition/miss memo before
scanning registered definitions, while invalidating the memo when the extra
program, parsed-refinement, or registry-refinement counts change. The build and
generated-refinement fixture pass. One cache-disabled `--run=none` Pong profile
after this change measured 18.948 s total (type validation 10.113 s, codegen
11.586 s); its instrumented repeat measured 18.728 s total, with 8.042 s HM
time on the validation pass and 6.7 s on the post-codegen HM pass. These are
single unpaired observations against an earlier 18.862 s profile; they do not
establish a compile-time speedup. The full language type suite completed with
343 passed, 0 failed across 273 runtime and 70 error fixtures, including the
generated-refinement and mutable-flow differentials.

A separate NYIR cleanup removed five redundant `rt_main` small-inliner scans:
the helper walks every current instruction, only clones straight-line bodies
without calls, and does not expose new calls for a later sweep. It also removed
a duplicate three-pass sweep. The cache-disabled one-frame AOT run completed
under the existing headless Weston/Vulkan compositor in 75.232 s: type
validation 10.236 s, codegen 11.713 s, object emission 31.501 s, runtime object
generation 22.926 s, and link 0.272 s. Frame 1 was reported at 1.267 s after
app startup. The client output did not include `[gfx] Using Native Vulkan
Backend`; backend selection is therefore unconfirmed for this run. This
single-sample result does not establish a compile-time improvement, and the
3-second acceptance remains unmet.

The `./make bench call-chain` smoke completed with exit 0. It reported NYR
699µs, LLVM 541µs, and C 245µs, but also reported an internal-versus-decoder
instruction-count divergence (180,701 versus 186,499) at an equivalent `lea`
whose operand ordering differed textually. Do not treat that output as proof
of instruction-count parity or as a speedup measurement for the inliner edit.

For output verification, a cache-disabled native `-O2` Pong AOT run with
`NY_DEBUG=1` completed two frames and exited 0 after 52.8 s process uptime.
It selected `[gfx] Using Native Vulkan Backend` on the AMD Radeon RX 9060 XT;
the frame-2 capture logged nine draw calls and saved
`/tmp/nytrix-pong-verify.png`. The image showed the court, center net, paddles,
score labels, and controls. Auto-dump/debug overhead means this is an output
check, not a clean timing sample; it still confirms the current compile-plus-
launch is far above 3 s.

Matched cold-cache AOT screening runs used `--native-only -O2 --run=aot`,
`CCACHE_DISABLE=1`, `NYTRIX_AOT_CACHE=0`, `NYTRIX_NATIVE_CACHE=0`,
`NYTRIX_STD_CACHE=0`, `NYTRIX_STD_BC_CACHE_AUTO=0`, runtime `-O3`, and one
Pong frame. All four selected native Vulkan and completed:

| Compiler threads | `NYTRIX_STD_THREADS` | Wall | User CPU |
| ---: | ---: | ---: | ---: |
| 8 | default (8) | 53.214 s | 63.539 s |
| 64 | default (8) | 49.881 s | 76.867 s |
| 96 | default (8) | 52.240 s | 87.735 s |
| 64 | 64 | 48.996 s | 76.959 s |

These are single-run screening results, not a repeated speedup estimate.
64 compiler threads with 64 standard-loader threads were fastest by 7.9%
versus 8/8, while consuming 21% more user CPU; 96/8 threads were slower than
8/8 despite consuming 38% more user CPU. Raising only standard-loader
parallelism from its 8-thread default saved 0.885 s at 64 compiler threads.
The object profile's `clock()` values are process CPU time, while its
`ny_ticks` subphases are elapsed time: build-IR CPU time was 30.347 s at 8
threads and 26.298/27.179 s at 64/96; parallel machine-lowering elapsed time
was 0.518/0.277/0.328 s, and bundle-encoding elapsed time was
4.505/1.054/1.071 s. The 3 s acceptance remains unmet.
A final cold `64/64` run with `NYTRIX_PONG_FRAME_TIMES=1` took 50.927 s
wall / 79.027 s user and reported frame 1 at 1.131 s after app startup. It
again selected native Vulkan; this confirms the first-frame timing path but
does not pass the 3 s compile/link/first-frame limit.

The latest cache-disabled, four-thread `--native-only -O2 --run=aot` run
completed in 53.684 s wall (78.506 s user, 7.964 s system) on 2026-11-10.
It selected native Vulkan and reported frame 1 at 1.460 s after startup.
NYIR construction took 37.616 s for 2,725 reachable functions; its measured
sub-stages were 7.590 s reachability, 7.304 s function lowering, 6.430 s
post-inline optimization, and 4.584 s call-effect inference. Machine-object
emission took 10.418 s, including 5.685 s bundle encoding; ELF writing itself
was under 0.13 s across the reported writer stages. These phase measurements
overlap with pipeline timings and are not additive. This one run establishes
no speedup, and cold compile/link/first-frame remains far above 5 s.

A cache-disabled `-vvvvvv --callgraph=3 --run=none` trace completed in
66.277 s and peaked at 1,060 MiB RSS. It loaded 146 modules (4,389,365
standard-library source bytes); import scanning took 0.00018 s, core loading
0.066 s, and parsing 0.348 s. Type validation took 9.403 s (HM inference
7.831 s); reachable-function collection took 7.566 s for 2,725 functions;
function lowering took 8.493 s, post-inline optimization 23.840 s, and
call-effect inference 4.345 s. The deep pass tracing materially increased
optimization time, so these are diagnostic figures, not performance
baselines. The bounded graph reported 305 displayed nodes and 4,873 edges at
depth 3, with remaining edges omitted from text output. Before the trace, the
default callgraph path used a 256-entry function buffer and failed on Pong;
it now allocates the native 16,384-function capacity, and this run completed.

The profile also exposed malformed x86-64 object layout: parallel register-
allocation fallbacks were appended after the data tables, placing function
symbols outside `.text` and allowing code/data overlap. The parallel builder
now appends the tables after all fallback bodies, and the ELF writer rejects
unsectioned function symbols outside `.text`. On the corrected output,
`_vk_check_descriptor_pool` and `rt_load32_idx` both lie within `.text`, and
the saved AOT executable selected Vulkan and completed a one-frame run.
One earlier first-launch attempt still exited with signal 11 during surface
startup; a GDB run, direct relaunch, and the later cold AOT run succeeded.
That intermittent runtime crash is not root-caused by these measurements.

The runtime archive builder's 25 C translation units now have independent
parallel compile tasks with distinct source/object paths; the archive is
assembled in the original fixed unit order after they finish. A first apparent
2.295 s runtime-object result was only a runtime-cache restore, not a parallel
compile. A controlled uncached comparison then used `NYTRIX_RUNTIME_OPT=3`,
`NYTRIX_RUNTIME_NATIVE=0`, `CCACHE_DISABLE=1`, and the same harmless host
warning flags (order reversed to create distinct runtime-cache keys):

| Runtime archive compile | Threads | Runtime object | Total AOT | Object emission |
| --- | ---: | ---: | ---: | ---: |
| Parallel | 8 | 7.473 s | 60.135 s | 32.207 s |
| Serial | 1 | 24.465 s | 81.501 s | 35.535 s |

Both launches reported one frame. The runtime-object stage improved 3.28× in
this paired profile; total AOT time is not a clean scaling comparison because
the compiler and object stages also used different thread counts and varied.
The parallel run reported 8 OpenMP regions across 2.86 million tasks; the
single-thread control reported no OpenMP regions. This verifies the runtime
archive scaling path, not the 3-second target. A separate cache-enabled
`NY_DEBUG=1` AOT launch printed `[gfx] Using Native Vulkan Backend` and
reported frame 1 at 1.459 s after app startup. This confirms the intended
renderer path but is not a cold-cache timing result.

The final cache-disabled `-O2` build after the AOT font-registration fix took
55.971 s: type validation 10.599 s, codegen 12.111 s, object emission
35.917 s, runtime object 0.0046 s, and link 0.242 s. A fresh current-source
run took 56.241 s: type validation 10.516 s, codegen 12.122 s, object emission
36.379 s, runtime object 0.0067 s, and link 0.249 s. The font change was a
source representation correction, not a compiler-speed optimization; the
small run-to-run difference does not establish a regression. Object emission
remains the largest stage and the 30 s target remains unmet.

Fresh `NYTRIX_AOT_CACHE=0` scheduler trials measured 56.622 s with four
threads and `NYTRIX_PARALLEL_MIN_WORK=512`, 55.653 s with eight threads and
the same threshold, and 57.144 s with eight threads and a 128-item threshold.
The parallel report counted 65 OpenMP regions for the four-thread case and
only five for the eight-thread cases, out of 4,967 regions overall. These are
single-run screening results, not a matched speedup; thread/grain tuning does
not close the 30 s gap.

A fresh cache-disabled `-O2 --threads=4 --native-only --run=none` object-emission
profile took 55.128 s: type validation 10.359 s, codegen 11.864 s, and object
emission 35.477 s. `perf record -e cpu-clock:u` captured 6,155 samples with no
losses; the recorded `ny-full` BuildID matches the executable. The largest
named self costs were CFG construction 2.73%, imported-module re-export lookup
2.26%, register-segment lookup 1.97%, refinement lookup 1.92%, module lookup
1.66%, loop-edge coalescing 1.54%, HM inference 1.51%, and type-map
initialization 1.41%. Two unresolved `libomp` addresses account for 10.20%;
without a known-code control, their attribution remains unverified. Object
emission is still the dominant measured stage, and no identified compiler
function accounts for the 30-second gap. A member-cache key experiment using
intern IDs measured 55.946 s without sampling and was reverted: the runs are
not a matched comparison and establish no speedup.

A CFG label-map audit found that `cfg_label_map_init` always chooses a
power-of-two capacity. Hash reduction and linear-probe wraparound now use
`cap - 1` masks instead of remainder. The seven `cfg`-matched fixtures passed.
Two cache-disabled AOT screenings took 50.718 s and 41.795 s wall versus the
earlier 53.684 s run; the spread is too large to establish a repeatable gain.
The latest run selected native Vulkan and showed the window through the X11
fallback because the isolated Weston socket was no longer running; it did not
record a first-frame time. A recursive exact-function subtree-cache trial
passed `modules/use` 45/45 but one AOT screening took 57.284 s, so that
unproven memoization was discarded.

The x86-64 mixed-bundle parallel-encoding candidate passed focused ELF64
debug-line, multifunc, and call-object fixtures. Three cache-disabled Pong
`-O2 --threads=4 --native-only --run=none` measurements with that candidate
took 57.054 s, 57.460 s (`NYTRIX_PARALLEL_MIN_WORK=1`), and 56.530 s
(`NYTRIX_PARALLEL_MIN_WORK=1`, parallel report enabled); the last reported
4,958 parallel regions and 2,856,770 tasks across the full compile, not
specifically the object emitter. These runs do not establish a compile-time
improvement; object emission remained about 37.5 s and the 30 s target remains
unmet.

Parallelizing NYIR-to-machine lowering in the same x86-64 bundle path also
passed the focused ELF64 fixtures. A cache-disabled run at four threads took
56.316 s (10.207 s type validation, 11.723 s codegen, 37.185 s object emission);
the parallel report counted 67 OpenMP regions. At 32 threads it took 57.611 s
with 38.624 s object emission and only two OpenMP regions. Neither run met the
target or showed a speedup over the current 55.128 s reference.

Changing bundle encoding to retain successful functions and use the legacy
machine/NIR fallback only for failed functions passed the focused ELF64
debug-line, multifunc, and call-object fixtures (5/5). A fresh cache-disabled
Pong `-O2 --threads=4 --native-only --run=none` compile took 51.948 s: type
validation 10.140 s, codegen 11.655 s, and object emission 32.809 s. A second
cache-disabled run at eight threads took 51.165 s: 10.193 s type validation,
11.707 s codegen, and 31.882 s object emission. Instrumented bundle encoding
handled 2,725 functions in 5.193 s at four threads and 4.117 s at eight; only
`ny_fn_core.reflect._range_contains` required the fallback. The eight-thread
output passed `readelf --lint` and `ld -r`. These are single-run measurements,
not matched evidence of a speedup; both remain above the historical 30 s
compile-only target and the user's 3 s compile/link/first-window target.

Repeating the eight-thread compile with the default parallel grain (rather than
forcing `NYTRIX_PARALLEL_MIN_WORK=1`) took 50.530 s: type validation 10.162 s,
codegen 11.621 s, and object emission 31.529 s. Bundle encoding took 4.148 s.
The output again passed `readelf --lint` and `ld -r`. This single run is only
0.635 s faster than the preceding forced-grain run; neither run measures
launch or first frame.

Further instrumentation on a fresh cache-disabled eight-thread run measured
51.599 s total: type validation 10.413 s, codegen 11.947 s, and object
emission 32.079 s. `nyr_build_ir` consumed 30.656 s, split into 7.091 s
reachable-function collection, 6.942 s function lowering, and 12.376 s
`rt_main` lowering. The 2,725-function machine bundle then encoded in 4.417 s;
ELF section serialization, including 154,901 relocations, took under 0.2 s.
This rules out ELF serialization as the object-time bottleneck. The run emitted
an object only; a compile/link/launch/first-frame measurement is still needed
against the new 3 s target.

A cache-disabled `--run=aot` run under the dedicated headless Weston Vulkan
compositor compiled and launched Pong, then completed 60 frames in 66.377 s
total. Its first frame was reported at 1.360 s after app startup; runtime object
generation took 13.480 s and link took 0.294 s. Weston loaded its Vulkan
renderer, but the client did not print `[gfx] Using Native Vulkan Backend`, so
the selected client renderer is unconfirmed. This run misses the 3 s
compile/link/first-window target by a wide margin and is not yet valid backend
acceptance.

A subsequent `--run=aot` invocation with default caches and `NY_DEBUG=1`
completed one headless-Weston frame in 52.498 s total. It reported a cached
runtime object (0.004 s), but object emission still took 31.674 s; frame 1
arrived 1.412 s after app startup. The client printed
`[gfx] Using Native Vulkan Backend`. Default caches therefore did not bring
this source compile near the 3 s end-to-end target.

One subsequent 20-frame headless run showed 1.629 s to the first submitted/
shown frame and roughly 200–210 ms between later frames. It had no paired
baseline, so it establishes neither a startup improvement nor a regression.
A separate `strace` run counted 928,250 `mincore` calls (8.796 s reported in
that syscall) but slowed the app enough to invalidate its frame timings. A
non-traced CPU profile sampled `_font_builtin_create` under
`_resolve_text_font`; this identifies a path for further investigation, not a
proven cause of the syscall count. The bound-overflow change preserved
conservative distant-address rejection but did not establish lower startup
cost.

#### Same-session Vulkan comparison after runtime fixes

Using the same 1280×720 headless Weston Vulkan renderer and explicit native
Vulkan backend, the pre-cache AOT executable took 1.189 s to its first shown
frame and 14.249 s for 60 frames, with later intervals around 204–210 ms. The
intermediate build with only the exact-pointer SSO-probe cache took 1.155 s
and 7.745 s; it still had a 0.51 s interval spike at frame 49 and failed to
load the Pong font. After fixing the AOT UI module's inferred global type, the
corrected build took 1.194 s to its first shown frame and 2.141 s for 60 frames;
later intervals were roughly 16–18 ms, without a spike or font-load warning.
The app reported `[gfx] Using Native Vulkan Backend`. A 60-frame `perf stat`
run also completed (2.190 s wall time, 1.074 s task-clock), without the
previously observed `ZeroDivisionError`.

Five-frame `strace -c -e mincore` runs counted 837,099 calls before runtime
fixes, 389,911 after the SSO-probe cache alone, and 77,524 after both the cache
and font fix. Traced timings are invalid; these counts show reduced calls in
this setup, not elimination of `mincore` or proof that distinct-range
exhaustion is impossible. The conservative range-capacity limit was not
increased.

The Pixman-rendered Weston variant rejected the Vulkan surface query and the
application fell back to its mock renderer; it was discarded. Weston with
`--renderer=vulkan` and the application log `[gfx] Using Native Vulkan
Backend` were required for valid runs. The font failure was in
`lib/os/ui/render/mod.ny`: the module-level `_next_font_id` was inferred as a
dynamic value, and AOT lowering mishandled its increment/return path. Declaring
it `mut int` preserves the integer representation. The AOT `-O2` variant of
`font-subset.nshape` now passes alongside native O0/O3 and LLVM MCJIT; the
corrected Pong app completed 60 frames with no font-load warning. The earlier
`ZeroDivisionError` did not recur after the fix; its connection to the font
bug is unproven.

### Larger editor workload

A later workload on revision `2f25b3ac` plus local changes took 128.71 s cold
and emitted a 25,226,512-byte object. Register-segment lookup, not the earlier
loop-edge coalescer, dominated its relevant call chain. A single-segment fast
path retained the existing inclusive bounds check. Its sampled share fell
from 22.35% to 19.43%; profiles took 132.533 and 127.039 s, with zero lost
samples but some unresolved frames. Inline decisions and output objects were
byte-identical. This supports a local lookup improvement, not closure of a
new editor compile-time target or a change to inlining policy.

### Validated editor compile

The 2026-10-22 comparison used revision `f05fdb2f` plus a recorded local
snapshot, clang 23.1.1, the existing Release configuration, and frozen compiler
objects/dependencies. Saved executables kept concurrent workspace rebuilds out
of the comparison. The candidate changed the range-query/peephole helpers and
included a regression entry. The hashed source set contained the editor and
library tree: 524 files, 11,096,879 bytes. Generated core/std sources were also
hashed. Both fresh compiler processes used the default profile, explicit `-O3`,
native x86-64 ELF emission, and disabled standard-library/automatic bitcode
caches. Source files were read for hashing before measurement.

The expected result was a successful complete native object with identical
bytes and inline decisions. Both compiler processes exited zero:

| Check | Baseline | Candidate |
| --- | ---: | ---: |
| Profile interval | 4,180.842 s | 1,423.179 s |
| `cpu-clock:u` samples | 405,961 | 133,477 |
| Validated instruction addresses | 405,961 | 133,477 |
| Lost samples | 0 | 0 |
| Unvalidated addresses | 0 | 0 |
| Object bytes | 33,223,632 | 33,223,632 |
| Printed inline records | 235,037 | 235,037 |

The profile interval was 2.94 times shorter. These are first-to-last sample
intervals from one attribution pair on Linux x86-64 (EPYC 7642); other local
compiles overlapped portions of the recordings. The inline streams matched
byte for byte, including repeat multiplicities and the 447-duplicate compaction
summary. Identities:

```text
baseline compiler BuildID: 5ecf7136bdc0c2d7742f95a9dce8ae3ea4358640
candidate compiler BuildID: 8ca6fb7aa30bd52c1998dd461e91f45148e235cf
editor.ny SHA-256: 3cde184e5142eac1e9b99be26d6be6d55ef4ff8b0c09d8cf57127341cf681b3b
source manifest SHA-256: cadb3989cf15a09ed147e8b90efbad168185c4d7ff4318a4cdbc6a9dc532abda
std.ny SHA-256: d1b3cdd3b15793174b4e6fde0928c5987111675f4cff1f4278be243673ff0fd5
core.ny SHA-256: 4e6546b4cbf53905cb0de1cabf250d0f5a50e236c3b519083df9417e483cae2c
both objects SHA-256: a6215bffe8bb8857627cea1a2e1c74e6d549b65abeb3ed06ad91f4183cf4e7e9
both inline logs SHA-256: 005eb94e6b0b8e81befcf511fd35969e38d0e4149420ad7693dc870ac25dc6b2
```

The per-file hashes, compiler/object identities, build commands, logs, captures,
ELF headers, validation scripts/results, and accepted comparison are retained
under `build/editor-compile-investigation/current/`, outside source control.
`source-graph-inputs.json` and `generated-inputs.json` contain the input hashes;
`comparison.json` records the accepted pair. Exact compile routes:

```bash
profile_dir=build/editor-compile-investigation/current
NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 NY_TRACE_INLINE=1 \
  perf record -m 1 -e cpu-clock:u -F 99 \
  -o "$profile_dir/baseline.data" -- "$profile_dir/ny-baseline" \
  --no-progress --native-only --opt-level=3 --run=none \
  --output="$profile_dir/editor.o" etc/projects/ui/editor.ny \
  > "$profile_dir/baseline.log" 2>&1
NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 NY_TRACE_INLINE=1 \
  perf record -m 1 -e cpu-clock:u -F 99 \
  -o "$profile_dir/cached-candidate.data" -- "$profile_dir/ny-cached-candidate" \
  --no-progress --native-only --opt-level=3 --run=none \
  --output="$profile_dir/editor-cached-candidate.o" etc/projects/ui/editor.ny \
  > "$profile_dir/cached-candidate.log" 2>&1
```

Validation checked every sampled IP against its executable MMAP, matching ELF
BuildID, executable load segment, symbol start, and decoded instruction boundary.
Matching libc debug symbols resolved private functions; a `strlen@plt` sample
was checked through its GOT relocation and IFUNC resolver. The same software
clock configuration put all 49 known-code control samples inside `known_hot`.
A bounded frame-pointer control validated 245 addresses across 49 samples;
the three-second compiler caller capture validated all 1,457 addresses across
301 samples, with zero loss. It connected the sustained CFG cost to serial
peephole range queries. Captures with a replaced executable, unrelated compiler
edits, or interrupted emission were excluded.

The unchanged CFG helpers' self-sample counts fell as follows:

| Symbol | Baseline | Candidate |
| --- | ---: | ---: |
| `nyr_cfg_build_impl` | 184,633 | 34,874 |
| `nyr_block_last_non_nop` | 34,859 | 1,142 |
| `cfg_label_map_find` | 26,265 | 1,114 |

The query scope now reuses topology and definition positions while reading
predicates from the current instructions. Instruction insertion, value-count
changes, and storage moves invalidate automatically; branch/definition-position
edits require explicit disposal. The serial peephole loop preserves those
positions between inserting rewrites. Singleton intervals skip edge refinement,
and parity candidates reject unrelated definitions before scanning uses.

The new `range_query` driver compared cached and fresh results for singleton
and contradictory bounds, integer extremes, live predicate edits, instruction
insertion, storage movement, new value IDs, and explicit invalidation after
branch retargeting. Its expanded mutation checks linked the exact candidate
optimizer objects. All 80 checked optimizer drivers retained their previous
results: 77 passed; `gvn_pre`, `magic_div`, and `polyhedral_nest` failed on both
the reference and candidate. An earlier aggregate SMT check reached its 40 s
timeout on both the reference and initial candidate, and supplies no passing
execution result. The normal `ny-full` build and focused range, boolean, VRP,
and bounds-check drivers also passed in the shared checkout.

### Runtime gate investigation (2026-11-13)

`./make bench --jobs 4 --bench-run 5 --bench-warmup 2` completed all 77
fixtures with matching checksums and no execution failures. Its JSON classified
62 rows as `ok`, 14 as unstable, and `gcbench` as a fixture-budget regression.
Sixty fixtures had at least one comparable NYR/C, LLVM/C, or NYC/C ratio above
1.2x. The EPYC 7642 host was shared with compiler and kernel builds (load roughly
39–50, 66 GB available in `/tmp`). This is investigation evidence, not an idle
host gate pass. The report precedes the scalar and wrapper changes below.

The `iter` post-inline failure exposed two separate vectorizer assumptions:
unrolling copied only instructions before the induction update, potentially
omitting a later reduction update; vector accumulation emitted the final
reduction at a named successor after an unlabeled exit had already observed it.
Unrolling now requires the reduction update within its cloned prefix. The
accumulator result is emitted at the actual exit block, preserving vectorization
and dominance of exit observations. The vector metadata driver checks both
orders and exit observations with vector accumulation enabled and disabled.
Native and LLVM return checksum `400020000`.

The C `iter` reference now uses preallocated, zero-initialized buffers, matching
the Ny fixture's allocation scope instead of timing dynamic append growth.
This changes the reference baseline. A focused 5+2 run reported NYR 34 µs and
LLVM 27 µs; a later cold run under contention reported noisy 72/52 µs results.
Neither focused result replaces the full gate. `time-profile` also remains
noise-sensitive; its status has not been hidden or its samples discarded.

The `iter` reduction also exposed a loop-phi carry gap in register allocation.
`coalesce_loop_edges` rejected any phi edge whose edge block had more than one
predecessor, even when the copy source was defined in that edge block. The
accumulator phi (header block 13, `v279`) therefore spilled and reloaded every
iteration. The carry now accepts a source defined on the edge and uses that
edge's live segment; only a source that must arrive across the branch still
requires a unique predecessor and a terminator-ending segment. After the fix,
the hot latch is `vpaddq ymm10, ymm10, ymm9` with the accumulator resident in
`ymm10`; the only remaining stack traffic is the loop-exit store/reload at
`[rbp-2352]`. Focused 5+2 warm runs pinned to an idle CPU reported checksum
`400020000`, NYR 357–369 µs, LLVM 356–364 µs, C 267–275 µs, NYC 304–307 µs
(NYR/C 1.30–1.38x, NYR/LLVM 1.00–1.02x).

That harness ratio is not a code comparison. The same fixture built as a
standalone AOT executable measured 288–307 µs against C at 266–276 µs
(1.08–1.12x). Both kernels fault the same pages inside the timed region (about
80 in map, 78 in reverse, none in sum), but `ny -run` starts with about 6,300
user page faults against C's 260. Pre-touching 24.6 MB in the standalone AOT
process raised it to about 342 µs and C to about 351 µs, reproducing the
harness range. The extra cost is first-touch allocation amplified by the
compiler's resident set, not generated loop code. The result is also
cache-state sensitive: forcing a fresh stencil or JIT compile
(`NYTRIX_STENCIL_CACHE=0` or `NYTRIX_JIT_CACHE=0`) pre-faults the heap during
compilation and the same timed region reports about 34 µs, while the default
warm-cache harness reports about 350 µs. Keep the cache state fixed when
comparing and do not read the cold-compile figure as a code result.

One real code gap remains in this fixture: the reverse loop is scalar because
the affine vectorizer rejects negative strides (`access.stride < 0` in
`loop_vectorize.c`), while C emits a 4-lane `vpermq` reverse load. It accounts
for roughly 10 µs of the standalone gap and is left as a vectorizer item.

The new `phi_elim_deep_vec8` regression also showed that vector phi
elimination materialized `vec8.load.i64`/`vec8.store.i64` without the
`MAY_TRAP` effect bit required by the verifier, and that its hand-built loop
used a backedge label that was not a CFG predecessor. Both are fixed and the
driver passes.

Scalar assembly review found that `3 * n` retained a constant left operand and
missed the x86 immediate/LEA selection used by `n * 3`. Machine lowering now
normalizes integer commutative operands without changing floating operations.
Native execution tests cover both operand orders, factors 3/5/9, large and
negative constants, live inputs, and wrapping limits. The boolean peephole also
removes positive-polarity comparisons of an exact `x & 1` result. Its regression
checks negative inputs, integer extremes, both mask operand orders, and
comparisons against -1/0/1/2. Native `collatz` assembly uses LEA and omits the
previous materialized equality comparison; its checksum remains `178`.

## Collatz mask-test fusion (2026-11-13)

The collatz inner loop lowers `if (n & 1) == 1 { n = 3*n+1 } else { n >>= 1 }`
to a SELECT whose condition is the mask `n & 1` itself (not a `==`/`!=` compare
against it). The x86-64 NIR emitter in
`src/code/backend/mach/arch/x86/machine.c` now recognizes a SELECT whose
condition is directly `AND(x, 1)` and emits `testq $1, %rax` + `cmovne`, dropping
the `and` instruction and the redundant register copies entirely. The AND's
stack home is elided, and the source value stays live through the SELECT for
register coloring.

Before: `mov n,%rax; and $1,%rax; mov n,%r9; sar $1,%r9; ...; test %rax,%rax; cmovne`.
After: `sar $1,%r9; lea (%r9,%r9,2),%r9; add $1,%r9; test $1,%rax; cmovne`.

Checksum `178` matches C/LLVM/NYC. Focused timing shows the Ny kernel at
~12µs vs C ~8.5µs (~1.4x; the reported 2.9x was a `ny--time` JIT-run overhead
artifact, not a kernel gap). Whole-program Ny time improved from ~15ms to
~12ms, still dominated by runtime and page-fault cost rather than the kernel.

A matching object-encoder pass was prototyped in `src/code/backend/mach/arch/x86/object.c`
but produced a checksum mismatch (`9` vs `178`) and was reverted; the NIR
emitter path is sufficient and correct.

A cold GCC rerun exposed an independent reference-wrapper defect: macro-renaming
C `main` removes its implicit successful return. The timer wrapper now inserts
an explicit final zero return into that body, preserving explicit early failure
returns. Its body scan skips comments and quoted braces. The wrapper regression
executes fallthrough, explicit failure, nested-block, quoted-brace, and argc/argv
cases through the selected host compiler.

Before runtime work was prioritized, a current-tree editor object compile with
six caches disabled took 99.24 s wall and 2,772,484 KiB peak RSS. Reported phases
were 10.36 s type validation, 12.37 s code generation, and 85.69 s object emission;
phase timings may be nested and must not be summed. A 10-second CPU sample put
19.28% of samples in `nyr_find_module_in_root_cached`. That helper now rejects
statement kinds that cannot contain modules before hashing/caching guaranteed
misses. Four existing alias/reexport/module fixtures passed cold. No isolated
compile-time speedup or 15-second acceptance result is claimed.

### Compiler build-time fixes (2026-11-12)

Machine: AMD EPYC 7642, 96 threads, 62 GB, shared with other agents. Cold means
the native, AOT, JIT, stencil, and stdlib caches disabled with a private build
of the current tree. These are single-session measurements taken at load 8–20,
not gate evidence. Twelve of the thirteen root projects meet 15 s; no UI
project does, so the all-project target stays open.

| Project (`-o`, cold) | wall | dominant phase |
| --- | --- | --- |
| herodoc | 0.36 s | parse/codegen |
| ffi | 1.15 s | codegen |
| args, 110, hexdump, matrix, conway | 2.6–3.5 s | codegen, emit |
| ant, brainfuck, donut, server | 3.8–4.3 s | emit |
| sound | 8.7 s | emit |
| animation | 26.6 s | emit 19.5 s |
| pong | 36.5 s | emit 28.0 s |
| term | 51.8 s | emit 36.1 s |
| monitor | 54.7 s | emit 43.1 s |
| synth | 60.6 s | emit 49.1 s |
| input | 95.1 s | emit 78.2 s |
| engine | 87.0 s | emit 72.9 s |
| editor | 73.2 s | emit 60.6 s |
| xtool | 6.8 s | emit 5.1 s |

Emit object is the dominant phase in every UI project. Per-project fixes in
this batch removed the HM blockers in the UI library (Wayland clipboard
wrappers, asset font/catalog defaults, engineering batch helpers, camera
`scale`). The remaining inference fixes made heterogeneous list literals
`list<any>` and bare
`list`/`set`/`tuple`/`dict` annotations use `any` elements; the `gf` scalar
overloads were rewritten from a comptime template into three explicit
functions, which removed `editor`'s nested-dict error and `xtool`'s
`expected gf, got int` error. A per-vreg first/last live-range pre-check in
`nyr_mach_regalloc_segment_at` measured no gain (animation emit 25.4 s vs
24.6 s, input 75.5 s vs 73.2 s) and was reverted.

The inliner was the other dominant cost. `nyr_inline_general` rebuilt the
caller CFG topology, loop depths, and type map after every committed splice, so
a 63k-instruction driver with thousands of tiny calls became quadratic. It now
stops after 128 splices per function (`NYTRIX_INLINE_MAX_SPLICES`) and skips
bodies above 100k instructions (`NYTRIX_INLINE_BODY_CAP`). Functions with fewer
than 128 inlinable calls are unchanged. This took `editor` from 270.7 s (at a
512-splice cap) to 205.8 s and `engine` from 122.9 s to 93.4 s. Indexing float
and vector segments by the instruction where they start or end (the scalar
allocator already did this) then took `engine` to 87.0 s. Finally, the
loop-edge coalescer's per-candidate `segment_in_block_vreg` scan was replaced
with a per-vreg index, and then with a one-time `(vreg, block) -> unique
segment` hash table; a block can hold thousands of segments and a long-lived
value can have thousands of its own, so both scans were quadratic. That took
`editor` from 205.8 s to 73.2 s (emit 189.5 s to 60.6 s). The `engine` and
`animation` objects are byte-identical before and after the index changes
(SHA-256 prefixes `f1a8e01a67f1f0a5` and `14a899e7b7b03d59`); engine re-runs
ranged 85.8–93.3 s (emit 72.2–79.2 s) at load ~20. The focused benchmark
fixtures still pass, but a full runtime benchmark comparison at this cap is
still owed.

NYIR type-map initialization now allocates flag regions after type propagation:
one region per class observed in the function, plus one zeroed region shared
by absent classes. Typed functions no longer allocate and clear fourteen
full-size value/local arrays when fewer than seven classes are present. The
earlier profile attributed 1.85% self time to type-map initialization; this
allocation change has no matched before/after compile-time measurement, so no
speedup is claimed and the build-time acceptance items remain open.

Run route `--run=native` with the mock replay environment: pong 36–41 s, input
109 s, engine 142 s (349 s before this batch's fixes). The default LLVM run
route takes pong 56 s and input 102 s, and engine segfaults in LLVM
SimplifyCFG after about 115 s on a malformed PHI (the unmodified release
compiler does the same, so it is separate from the native route).

Remaining engine native time (87.0 s): encode 72.9 s. In the profile taken
before the segment index, `nyr_mach_regalloc_segment_at` was 12.3 % self,
`coalesce_loop_edges` 8.2 %, `mach_float_begin_segments` 6.6 %, and
`mach_float_end_segments` 2.4 %. The float and vector begin/end scans now use
per-instruction start/end indexes, like the scalar scans;
`mach_flush_float_to_home_mode` still scans every vreg once per block. A
persistent per-function object cache is the larger lever for iteration time.
`editor` still spends 60.6 s in emit on its giant function, so it remains far
from the 15 s target even though it now compiles.

### Rejected NYIR CFG-boundary scan experiment (2026-11-12)

On the same AMD EPYC 7642 host, a focused `sound.ny` native compile sampled
`nyr_cfg_build_impl` at 4.04% of 520 CPU-clock samples. The experiment removed
its temporary boundary bitmap and one full scan, calculating block starts
directly in two passes. It passed the CFG regression and x86-64 target suites;
the emitted object stayed byte-identical
(`7df680223713507fe982ffb1cec8cb25bbb8412b96990520812393dc5dc2f917`).

The A/B used `build/ny-full-before-cfg` (BuildID
`994641023b6455a692f0dd04ff5036a9e0809a2f`) and the candidate `ny-full`
(BuildID `3f061e09c999dd80dc81a1a54bd67ecd807bbaab`) with:

```bash
NYTRIX_STD_CACHE=0 NYTRIX_STD_BC_CACHE_AUTO=0 \
  perf stat -r 5 -e task-clock,cycles,instructions -- \
  build/release/ny-full --no-progress --native-only --opt-level=3 \
  --run=none --output=build/perf-sound-time.o etc/projects/sound.ny
```

The same command was run against each compiler twice, in baseline/candidate
then candidate/baseline order. `nice -n 19` was applied to all runs. The host
was shared with the NYC Linux build. The user later clarified that it was run
with `-j32`, not `-j16` as initially recorded; the observed load averages were
16–20. The OS page cache was not cleared. These timings were taken under
substantial contention; retain them only as a rejected exploratory run, not
evidence of a real speedup or regression:

| Order | Baseline elapsed | Candidate elapsed |
| --- | ---: | ---: |
| First pair | 7.990 s ± 0.22% | 8.168 s ± 0.43% |
| Reverse pair | 8.067 s ± 0.18% | 8.104 s ± 0.15% |
| Mean of pair means | 8.028 s | 8.136 s |

Candidate task-clock and instruction counts were also slightly higher. Its
single CPU-clock profile sampled `nyr_cfg_build_impl` at 2.91% of 516 samples,
but that small sample-count shift did not correspond to an end-to-end gain.
The candidate was reverted; no speedup is claimed. The baseline compiler
passed `cfg_simplify`, the candidate passed `cfg_simplify` and all 166 x86-64
target tests, and both compilers emitted the identical object above. This
experiment did not change the open compile-time acceptance items.

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

### Entry-address rematerialization reachability

The reported C failure needs an entry-block LEA and another definition to
coalesce to the same machine vreg. Domination alone does not make a vreg-numbered
address fact immutable, so the x86-64 object encoder retains its
unique-definition guard.

That PHI-coalescing route is unreachable for current Nytrix functions:
`mach_coalesce_copies` starts at pass 1 for non-C functions and skips synthetic
PHI edge copies (`source_pc == UINT32_MAX`). Ordinary Nytrix coalescing also
requires both operands to pass `mach_cc_raw_vregs`; C does not have these
restrictions. This does not rule out other encoder bugs; revisit if synthetic
edge-copy coalescing becomes available.

`etc/tests/code/targets/x86_64/addr-local-loop-phi-x86-64.nshape` covers a
pointer changed across a loop, including zero and one iterations; its O3 native
oracle is 42 and `@noinline` preserves the function boundary. It passes with
and without only the unique-definition guard in an isolated compiler build,
so it is coverage, not a reproducer of the C failure. Five cold fixtures
passed: this case, `addr-local-x86-64`, `addr-local-multi-x86-64`,
`addr-borrow-local-x86-64`, and ELF64 `addr-deref`. No compiler behavior
changed.

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


## Binary runtime diagnosis (2026-11-03)

### Timed-object inspection

The exact native-tier AOT object was emitted from cached benchmark source and
inspected with `objdump`; the relevant function is `ny_fn_branch_mix`. The
current loop carries `i`, `acc`, and `x` in callee-saved registers. The signed
divide-by-two result (`v36`, machine segment `[19,27]`) has no allocated color
because of register pressure and is stored/reloaded at `-0x150(%rbp)` each
iteration. Several short-lived branch values also spill; updated `x` is
already written directly into `%r15`, so the earlier two-spill description is
stale. `--emit=asm` is a generic listing and is not the timed object.

A SELECT-arm elision prototype was rejected after the relinked object showed no
instruction change. The `select-flag-preservation` runtime fixture passed all
four variants during that experiment, but no performance gain was established.
A pinned 11-sample matched run used native x86-64, opt tier, `-O3`, one warm-up,
and CPU 12. NYR median/min/max were 1.756597/1.728875/1.828070 ms; LLVM was
0.327349 ms and host C 0.568908 ms. NYC was 8.365809 ms. All four checksums
were `3360874588549334`. NYR/LLVM is 5.37x, still above the unchanged 5.00x
fixture ceiling. A prior 11-sample run on another CPU reported 1.750 ms NYR,
0.325 ms LLVM, and 0.558 ms C; retain it as supporting data, not a strict
same-core comparison. The next general optimization should reduce or split the
short-lived divide result's live range under fixed-register clobbers and local
pressure. Do not change fixture work, checksums, compiler comparisons, or gate
limits to close the item.


## BCE vector carry experiment (2026-11-03)

### JIT carry-flush result

Inspection of the relocated JIT image showed that the x86 `BR` and `BR_IF`
encoder cases flushed every seeded vector to its stack home, bypassing the
allocator's `carried && !spill` proof. The encoder now preserves only those
proven vector carries across internal branches; exit segments still flush, and
call boundaries still flush all vectors because vector registers are
caller-saved. The exact `ny_fn_matrix_sum` JIT loop no longer stores its
accumulator on the loop backedge. A pre-loop seed load/store remains, as do
chunk-load staging operations.

A same-source A/B used the `bce-matrix` fixture, NYR `-O3 --native-only
--native-backend=x86_64 --native-tier=opt`, nine process-isolated runs per
version, and matching checksum `14439925760000`. The baseline median was
15.720 ms (15.675–15.744 ms); preserving proven carries measured 12.566 ms
(12.552–12.625 ms), a 20.1% reduction in this fixture. This confirms the
branch-flush cause and a local runtime gain; it does not close the LLVM/C or
full-matrix acceptance gaps. Focused `native_vec4_f64`, `vec8_reduce_loop`,
`loop_vector_metadata`, `regalloc_cfg_edge_carry`, and
`regalloc_loop_phi_carry` regressions pass. The initial run exposed that
`loop_phi_carry_enabled` opted scalar GPR carries in but omitted scalar FPR
carries despite the shared `NYTRIX_GPR_LOOP_CARRY` switch. The gate now covers
both scalar classes; `regalloc_fpr_loop_phi_carry`,
`regalloc_loop_phi_carry`, `regalloc_cfg_edge_carry`, and
`regalloc_unchanged_dag_carry` pass. The existing
`float-loop-register-calls` runtime fixture also passes both `-O0` and `-O3`
native x86-64 variants with `NYTRIX_GPR_LOOP_CARRY=1` and all result/JIT/AOT
caches disabled, covering calls, joins, and conversion scratch use.


A follow-up four-route AOT run used x86-64, `-O3`, peak profile, warm cache,
CPU 28, 11 measured samples and one warm-up. All routes returned checksum
`14439925760000`:

| NYR | LLVM | Host C | NYC | NYR/LLVM |
| ---: | ---: | ---: | ---: | ---: |
| 12.714 ms (12.672–12.803) | 6.334 ms (6.317–6.423) | 1.608 ms (1.594–1.663) | 91.739 ms (91.701–92.090) | 2.01x |

This passes the unchanged per-fixture NYR/LLVM ceiling of 5.00x. It is one
fixture result; the 72-fixture rerun is still required before closing the
cross-fixture item. Raw report: `/tmp/ny-bce-carry-check.json`.


## Rejected constant-division scheduling trial

A scheduler latency estimate that treated positive power-of-two signed divisors as short operations was built and screened against the single `binary` fixture. The 11-sample partial run reported 2.00 ms NYR, 322 us LLVM, and 6.20x NYR/LLVM; the fixture completed successfully. This was not a matched before/after comparison, used GCC for the host-C route, and did not pass the unchanged 5.00x gate. The source change was reverted; retain the result as a rejected, inconclusive experiment. Raw report: `/tmp/ny-binary-divlatency.json`.

## Rejected loop-PHI latch reuse trial (2026-11-03)

Relaxing the x86 loop-PHI edge check to allow the incoming value to reuse the
old PHI register after its final latch read reduced the benchmark's static
spill/reload counters, but made `binary` much slower. With
`NYTRIX_GPR_LOOP_CARRY=1`, `./make bench binary --bench-run 11
--bench-warmup 2 --bench-opt O3 --bench-compile-profile peak
--bench-compare-llvm` measured 7.76 ms NYR (min 7.73 ms), 317 us LLVM, and
1.04 ms host C; checksum `3360874588549334` remained correct. The matched
pre-change carry-flag run was 1.75 ms NYR. The allocator change was reverted;
fewer static spills did not translate into faster execution. Raw report:
`build/bench-reports/bench-20261103-132333-356243.json`.

## Binary hardware-counter profile (2026-11-03)

A seven-run matched benchmark with two warmups measured 1.75 ms NYR, 317 us
LLVM, and 1.04 ms host C for checksum `3360874588549334`. The validated AOT
counter run recorded 26.56M NYR instructions versus 3.66M LLVM instructions;
branch misses were nearly equal (4,518 vs 4,515), as were cache misses
(11,723 vs 12,602). This points to excess NYR instruction work rather than
branch prediction or cache misses. NYR/LLVM remained 5.53x, above the 5.00x
per-fixture limit. Raw report:
`build/bench-reports/bench-20261103-132518-358879.json`.

A later pinned 11-sample run on CPU 12, after rebuilding the compiler, measured
1.773 ms NYR, 316 us LLVM, 1.054 ms host C, and 8.353 ms NYC; all routes
returned the same checksum. NYR/LLVM was 5.60x. Its checksum-validated AOT
counters were 26.57M NYR instructions vs 3.66M LLVM, 4,809 vs 4,611 branch
misses, and 12,490 vs 12,656 cache misses. The accompanying native object
disassembly still shows the signed divide-by-two result stored to and loaded
from `-0x150(%rbp)` in `ny_fn_branch_mix`. The power-of-two latency scheduling
candidate and a just-in-time single-use deferral candidate were not retained:
neither produced a verified matched runtime improvement. The startup `FLOOR`
row is the intentional empty timer control and is excluded from the workload
ratios; the startup-class gate remains a separate unresolved issue. Raw report:
`build/bench-reports/bench-20261103-134322-399654.json`.

The subsequent 72-fixture full matrix used three samples and 21 parallel
fixture workers. All checksums matched, but this run is only a broad regression
screen, not low-contention acceptance. The `binary` row measured 1.762 ms NYR,
318 us LLVM, and 1.047 ms C (5.54x NYR/LLVM); it remains over the unchanged
5.00x limit. The empty `startup` timer control remains `FLOOR`; separately,
the five-workload startup cohort measured NYR/C 1.224x and LLVM/C 1.130x
against unchanged limits of 0.42x and 0.30x. Raw report:
`build/bench-reports/bench-20261103-135250-429124.json`.

The next full 20-worker run reported `bce-matrix` checksum
`-14354741043200` and a 423.587 ms NYR time. Re-running the fixture alone with
`./make bench bce-matrix --no-build --bench-run 3 --bench-warmup 1` returned
the expected checksum at 14.1 ms; a second full 20-worker run also returned
the expected checksum at 14.276 ms. The anomaly is not reproduced, so keep it
open as an intermittent correctness failure. The second full run still failed
the binary, datastructures, startup, and string gates; `append-scaling` measured
7.829 ms NYR vs 3.072 ms LLVM (2.55x, above its 1.50x fixture limit). Report:
`build/bench-reports/bench-20261103-140002-447190.json` (mismatch) and
`build/bench-reports/bench-20261103-140431-460598.json` (clean repeat).

After the benchmark driver's Clang rebuild crashed on `parse.c`, the already
built `ny-full` binary was used for 11 single-process `binary` launches pinned
to CPU 12. Every run returned checksum `3360874588549334`; the median timed
kernel was 1.749 ms (1.730–1.757 ms). This is a NYR-only follow-up, so it does
not replace the matched NYR/LLVM/C report above. The failed rebuild's Clang
reproducer is `/tmp/parse-b761af.sh`.

The rejected register-clobber candidate modeled immediate power-of-two division
as clobbering only RDX, and stopped flushing RCX in the x86 encoder. Its pinned
11-sample run preserved checksum `3360874588549334` but regressed to 1.945 ms
NYR versus 328 us LLVM (5.92x). The timed linked object still stored the signed
divide result to `-0x160(%rbp)` in `ny_fn_branch_mix`; other loop values also
shifted into stack homes. The candidate and its regression test were removed.
Raw report: `build/bench-reports/bench-20261103-135047-424579.json`; object dump:
`/tmp/ny-bench-native-426219-232140242-8.dump`.

## Narrow loop-home spill reduction (2026-11-03)

The `binary` benchmark's pinned 11-sample baseline measured 1.759 ms NYR
(1.737–1.807 ms), 327 us LLVM, and 554 us host C. Inspection of the timed
linked object showed `ny_fn_branch_mix` storing and reloading the result of a
single-use constant divide inside its loop. The x86 loop-home allocator now
admits that one-definition, one-use value alongside CFG-proven loop-carried
values. The existing `x64_readonly_loop_gpr` execution test covers a loop with
a single-use constant-division result.

The pinned candidate run measured 1.739 ms NYR (1.733–1.756 ms), 320 us LLVM,
and 554 us host C; the checksum remained `3360874588549334`. NYR elapsed time
fell about 1.1%, while NYR/LLVM moved from 5.38x to 5.43x because LLVM's
sample was also faster; the row remains above the unchanged 5.00x gate. In the
timed object, `ny_fn_branch_mix` shrank by 11 bytes and its `%rbp` references
fell from 10 to 8 as the quotient store/reload disappeared. Aggregate NYR
instructions were unchanged at about 26.56M; cycles fell from 6.50M to 6.39M.
Whole-program code size changed from 1,141,504 to 1,139,008 bytes, while
compile time moved from 76.80 to 77.28 ms. These small runtime changes are evidence for one
spill reduction, not a resolution of the `binary` gap. A focused 11-sample
`dgemm` run measured 192 us and checksum `1`; no matched before/after run was
made for that fixture. Reports and object dumps are in `/tmp/ny-backend-*.json`
and `/tmp/ny-bench-native-*.dump`.

## AVX1 V256 integer operations and allocation checks (2026-11-03)

The x86 encoder now lowers V256_I64 add/sub on AVX1 by extracting each 128-bit
half, applying SSE2 `paddq`/`psubq`, and inserting the two results back into a
YMM register. The fallback keeps values in canonical homes for the function,
because its XMM0-XMM3 temporaries must not clobber live allocated values. An
executable AVX-only probe checks both operations and reductions. That probe
also found and fixed a NYIR lowering typo where `VEC8_SUB_I64` became machine
ADD. Vector FMA encoding now rejects targets missing AVX or FMA.

The focused `bce-matrix` run returned the expected checksum
`14439925760000` in all implementations. Across three samples, NYR measured
13.343 ms, LLVM 3.240 ms, and host C 1.591 ms. The report measured 8,280 NYR
code bytes, 6.96 ms compiler time, 124 spills, and 274 reloads. NYR/LLVM was
4.12x; this remains a material runtime gap, and this focused run does not
establish a full-suite improvement.

The 11-sample `binary` allocator comparison did not justify enabling scalar
loop-carried GPR residency globally: the default measured 1.721 ms, 1,125,272
code bytes, and 76.67 ms compile time; `NYTRIX_GPR_LOOP_CARRY=1` measured
1.734 ms, 1,125,032 bytes, and 77.19 ms. Both runs preserved checksum
`3360874588549334`. Keep that path opt-in. The verifier-checked segment
allocator and unchanged-value DAG carry remain covered by focused tests; the
whole-function allocation and full-suite quality acceptance remain open.

Focused checks passed: `vec8_reduce_loop`, `regalloc_loop_phi_carry`,
`machine_home_reuse`, `x64_readonly_loop_gpr`, `x64_disasm_vex`, and
`x64_encoder_oracle`. AVX-only V256_I64 bitwise operations use AVX1 bitwise
encodings; AVX2-only integer operations remain rejected when AVX2 is masked.
The AVX1 add/sub fallback is directly executed.

### Loop-carried vector lane preservation

An executable machine-IR regression builds a four-lane `V256_I64` accumulator,
starts with lanes `{1, 2, 3, 4}`, carries it through five loop backedges, then
reduces it on the exit edge. It verifies the allocator marks the vector PHI as
carried and checks the generated x86 function returns `60` with the default
configuration and no carry environment override. This detects loss of any
upper lane.

The first run exposed the bug: the allocator correctly marked the header value
carried, but also set its `spill` bit because the loop had an exit successor.
The encoder interpreted that bit as “not resident at block entry,” reloaded a
stale canonical home, and returned a garbage result. The bit means the value's
home must be published for the exit; the common preg remains valid on both
preheader and backedge arrivals. The encoder now seeds any verifier-proven
carried segment, including one that must be stored at block exit. Vector loop
carry is enabled by default; `NYTRIX_VECTOR_LOOP_CARRY=0` remains available to
isolate regressions. GPR/FPR loop carry remains separately opt-in.

The focused vector, segmented-allocation, GPR/FPR carry, VEX-disassembly, and
encoder-oracle checks pass. The subsequent 73-fixture run preserves checksums
for `bce-matrix` (`14439925760000`), `dgemm` (`1`), and `pbkdf2` (`2991`). It
still reports independent failures in `fannkuch`, `n-body`, and `spectral`, and
the benchmark gates still fail on `binary` (6.09x NYR/LLVM) and the startup
cohort. This validates vector-lane preservation on the focused generated-code
case; it does not establish full backend acceptance. See the archived run above
for complete per-fixture measurements.

The first hot-pressure spill-splitting candidate was removed after matched
seven-sample tests showed no useful change. Both runs were pinned to CPU 13,
used one warm-up, disabled the loop-home overlay in both configurations, and
preserved fixture checksums. For `binary`, control/candidate runtime was
2.769/2.749 ms, code size was 1,125,920 bytes in both, compile time was
79.25/78.01 ms, and spill/reload counts were 9,480/32,617 versus
9,480/32,618. For `linpack`, runtime was 1.373/1.363 ms, code size was
1,147,728 bytes in both, compile time was 79.71/79.57 ms, and spill/reload
counts were 10,176/34,283 versus 10,176/34,284. Runtime ranges overlapped;
the generated assembly text was identical for `binary`. No speedup is claimed,
and the opt-in splitter was removed instead of leaving an unaccepted allocator
path in the encoder.

The segmented allocator verifier now also checks that every register operand
use is covered by a non-overlapping segment. A focused negative test truncates
a live range before an actual use and confirms the verifier rejects it. This
strengthens allocation validation; it does not resolve the independent
`fannkuch`, `n-body`, or `spectral` benchmark failures recorded above.

### CFG carry preservation proof and scalar carry A/B (2026-11-03)

The verifier now requires segments to cover live-in and live-out block
boundaries. For every carried live-in segment, it checks each live predecessor
ends in the same allocated register; a carried edge-copy destination must be
defined by a covered, tied copy. Negative allocator tests mutate the incoming
register, truncate live-in/live-out boundaries, and verify rejection. The
generated x86 loop test executes with scalar GPR carry enabled for inputs
`-1, 0, 2, 3, 4`, preserving each expected result; its emitted function is
152 bytes versus 177 bytes with forced stack homes. The AVX-only fallback,
V256 lane-carry, FMA feature-gate, encoder-oracle, and VEX-disassembly checks
also pass.

Scalar GPR carry remains opt-in. In a matched serial three-sample full-matrix
A/B, 67 fixtures had successful NYR timings in both configurations. The median
per-fixture change was effectively flat (+0.11%); several apparent regressions
above 10% did not reproduce in an 11-sample `dict-set` repeat. Its medians were
47.515 ms with carry off and 46.813 ms with carry on, while sample dispersion
was 13.2% and 14.7%; code size changed from 1,153,528 to 1,153,432 bytes and
compile time was 80.86/80.89 ms. The full run also reproduced existing
`fannkuch`, `n-body`, and `spectral` failures. Its overall gate verdict was
invalidated when the runner rebuilt a shared core artifact, so these rows are
diagnostic measurements, not an acceptance result. Keep scalar carry opt-in
until repeated measurements show a broad, stable gain without material
regressions. Vector carry remains enabled and is covered by the five-backedge,
four-lane executable regression above.

### Binary allocator acceptance (2026-11-03)

For the focused `binary` workload, the final opt-in GPR-carry A/B used the same
source, `-O3`, peak compile profile, warm cache, one warm-up, and 11 timed
samples. Both runs returned checksum `3360874588549334`. With carry off, NYR
measured 5.210 ms (5.182–5.311 ms); with `NYTRIX_GPR_LOOP_CARRY=1`, it measured
5.093 ms (5.072–5.114 ms), a 2.2% median reduction. Compile time was effectively
flat (65.00/64.89 ms), and code size changed by +8 bytes. The aggregate static
spill/reload counters remained 3,407/29,376, so this is not a claim that all
reported spills were removed. The `ny_fn_branch_mix` frame shrank by 128 bytes
in the inspected x64 object, and the result is kept opt-in: default allocation
behavior is unchanged. Focused division/clobber, read-only loop, machine-home
reuse, and backend-acceptance tests pass. This is the practical acceptance for
this item: a repeatable local runtime and frame-size improvement with correct
results, not a new global spill-count threshold. Reports:
`184827` carry off
and
`184902` carry on.

## PBKDF2 optimized correctness failure (2026-11-03)

The focused benchmark reproduces a NYR checksum mismatch in
`etc/tests/bench/numeric/pbkdf2.nshape`. NYR O0/O1 returns `2991`; repeated
O2/O3 runs return varying incorrect checksums. LLVM and host C both return
`2991`. Disabling `loop_vectorize` does not repair the result, while
`--nyr-disable-pass=peephole` at O2 restores `2991`, narrowing the failure to
that pass or its interaction with later lowering. Disabling individual loop
passes and range-based compare folding did not repair it. The per-pass oracle
cannot continue past `iv_elim` because its VM does not support the resulting
`vec4.add.i64` operation. After a full rebuild of `ny-full`, five consecutive
O2 native runs returned `2991`; the focused O3 benchmark (3 samples, 1 warm-up)
also passed with checksum `2991` (NYR 1.65 ms, LLVM 937 us, host C 1.10 ms).
No semantic source fix was identified, so the earlier failing executable may
have been stale. Keep the origin unresolved and reproduce it before assigning
the fault to a pass. Original focused report:
`/tmp/ny-pbkdf2-followup.json`.

## Performance TODO batch (2026-11-04)

Host EPYC 7642, x86-64 native, `-O3`, executables built from each fixture's
NY source and run pinned to one CPU. Medians of 9–15 runs; checksums match the
baseline compiler, LLVM, and the fixture's expected value.

| Fixture | Before | After | LLVM | Change |
| --- | ---: | ---: | ---: | --- |
| `dce` | 31 µs | 0.2 µs | 0.26 µs | select chain folds to one select |
| `spectral` | 125 µs | 87 µs | 79 µs | FPR loop homes across conversions/sqrt |
| `mandelbrot` | 928 µs | 351 µs | 255 µs | same, plus no dead temporary homes |
| `page-rank` | 33 µs | 21 µs | 6.6 µs | same |
| `linpack` | 772 µs | 745 µs | 240 µs | no dead temporary homes |
| `binary`, `dgemm`, `json-parser` | — | unchanged | — | neutral within noise |

- **`dce`:** the recovered `loop_global_promote` and `select_chain_fold`
  passes were not wired into the pipeline after a `git checkout -- .` wiped
  tracked-file edits; they now run after the mid-pipeline cleanup and again
  after store forwarding. Two further fixes were needed. Rule R4 merges the
  `add(add(b, C1), C2)` chains that R3 builds, and use counts now ignore folded
  selects so the merge applies within one round. Separately, DCE's fallback
  `nyr_label_referenced()` ignored PHI predecessor labels. When `max_label`
  exceeded `4 * len + 1024`, which the collapsed 500-step function did, DCE
  merged a fall-through block away and broke the PHI. The 400-step variant
  stayed under the bound, which is why the bug showed only at 500. The
  `dce_phi_label` regression fails without the fix.
- **FPR residency:** convert/sqrt clobber only the loop colors their encoding
  writes, conversions and sqrt may define loop homes, spill-home privacy
  rejects only LEA'd frame slots and stack/aggregate parameters, displaced
  segments move to free colors above the loop colors, and scalar register
  copies use `movaps`. A segment moved to a free color keeps the allocator's
  reload/spill flags. Forcing them stored every block-local temporary to a
  dead home once per iteration (four stores in linpack's row update).
- **Variable-shift recolor miscompile:** loop-carry recoloring again checks
  fixed-register clobbers over the whole interval (`x64_loop_recolor` runs
  with loop GPRs on and off), and the post-allocation verifier rejects a
  colored range live across a fixed clobber (`regalloc_verifier_coverage`).
- **Vector homes:** a single-definition vector whose home is current in its
  defining block is no longer re-stored at every loop latch. That removes
  dgemm's invariant-broadcast stores but not its runtime; see the TODO.
- **Boundary coalescing:** the arithmetic/SELECT boundary-tie hook had already
  been reduced to a stub returning false; it is removed. `binary` and
  `json-parser` are unchanged.

All 103 optimizer test groups pass, and all 73 bench fixtures match baseline
checksums. This is not a full-gate run.

## Gate follow-up (2026-11-04, late)

A serial `./make bench` (3 runs + 1 warm-up) failed the algorithms,
datastructures, startup, and overall gates on three fixtures:

- `recursive-fibonacci` 2.60x NYR/LLVM (budget 2.00x): statement lowering had
  lost the AVX2 optimizer option and the post-optimization recursive-call
  rewrite (`nyr_tail_call`, which turns `f(n-1) + f(n-2)` into an accumulator
  loop). Both are restored in `lower/stmt.h`; the kernel went from 3.7 ms to
  0.32 us. The AVX2 option also feeds the vector passes (`bce-matrix` 11.0 ms
  vs 12.7 ms before).
- `dict-set` 1.95x (budget 1.50x) came from concurrent builds; alone with
  7 runs it measures 30.9 ms vs 25.8 ms LLVM (1.20x).
- `binary` 5.33x (gate 5.00x). Keeping MUL by 5/9 as one LEA and the new
  `loop_sub_iv` pass (`inv - i` becomes a decrementing IV) took the inner loop
  from 19 to 18 instructions and NYR from 1.60 to 1.45 ms, but LLVM measured
  266 us in that run (5.47x). The remaining gap is LLVM's outer vectorization.

All 103 optimizer groups pass and all 73 bench fixtures match baseline
checksums with these changes.

## Outer vectorization residency (2026-11-05)

Targeted serial run, `ny-test --bench --bench-run 7 --bench-warmup 2
--bench-compare-llvm`, on a host that other sessions were also loading (load
average about 14), so these numbers are advisory, not gate evidence:

| Fixture | Route | NYR | LLVM | NYR/LLVM |
| --- | --- | ---: | ---: | ---: |
| `binary` | default | 1.45 ms | 266 µs | 5.45x |
| `binary` | `NYTRIX_OUTER_VECTORIZE=1` | 903 µs | 266 µs | 3.40x |
| `dgemm` | default | 154 µs | 26 µs | 5.97x |
| `linpack` | default | 739 µs | 240 µs | 3.07x |

Before this batch, the opt-in widened `binary` loop took 2.1 ms. Changes:

- Vector latch recurrences (`x' = f(x)` computed in the latch) share the PHI
  register when every read of the old value precedes the new definition. The
  allocator proposes this pairing and the verifier checks it again
  (`verify_vector_latch_recurrence`). The latch definition may read the PHI
  as `src0` of a single add/sub/and/or/xor.
- The loop-carry coalescer tries the PHI's own header color first, picks the
  color with the cheapest evictions (a segment with no access in its block is
  nearly free), and coalesces each PHI once per header. Nesting is judged on
  the natural loop body, not on every block that reaches the header.
- Coalescing can evict block-local temporaries. Those that were
  register-only now get a second chance at any color that is still free.
- Loop-invariant vectors stay resident across the loop when every body
  segment shares one color with the preheader.
- Functions with no scalar floats (SysV, AVX2) also allocate xmm4..xmm7 to
  vectors, giving 10 vector colors instead of 6.
- Encoder: three-operand VEX binops, register-form `vbroadcastsd`, and SET1
  results written straight into their assigned register.

Correctness fixes found along the way:

- The home-path COPY of a four-lane vector used a 16-byte `movdqu`, which
  left lanes 2–3 stale. This caused the "wrong `binary` checksum" that had
  been blamed on the latch relaxation.
- `mach_ymm_broadcastsd_mem` dropped VEX.R for ymm8+.
- `mach_lower_phi_edges` counted only leading contiguous PHIs, so a PHI after
  a DCE-made NOP lost its edge copies.
- LICM hoisted into a preheader laid out after its loop, which broke the
  verifier's def-before-use order once the outer vectorizer appended its
  vector loop.

The full `lang` + `code` suite (2139 fixtures) showed no regressions. The 16
failures it reported fail on the baseline compiler too, or pass when run alone.

Outer vectorization is now on by default (`NYTRIX_OUTER_VECTORIZE=0` turns it
off). Enabling it took four more fixes on the in-process native route
(`--native-only`, native JIT, native result oracle), whose scalar machine
emitter hands vector functions to the NYIR emitter:

- `nyr_phi_elim` lowered four-lane PHIs through 8-byte locals. They now go
  through a 32-byte entry `alloca` with `vec8.store/load.i64`.
- The NYIR emitter had no `vec8.sar.i64`. It is now lane-wise `sarq`, with
  four-slot homes.
- The home-path four-lane f64 SET1 broadcast from the source's stack home
  even when the scalar existed only in its float register, which made `2.0 *
  B` read 0 in `loop-tail-passes`.

The full suite with the vectorizer forced on matched the baseline failure
list exactly (2124/2139). The full serial bench that followed
(2026-11-05 06:25, recorded above) put `binary` at 919 µs vs 266 µs LLVM
(3.45x) and passed every per-class gate. Every checksum that ran matched.

Later the same day:

- `nyr_licm_post_vectorize` runs right after the late loop vectorizer. It
  allows hoisting invariant address arithmetic, which the pre-vectorization
  LICM keeps in place on purpose. That alone took `linpack` from 760 µs to
  320 µs.
- CSE now merges identical class-refining pointer copies.
- Home-path f64 broadcasts load the lane with `movsd`, which forwards, then
  use register-form `vbroadcastsd`. Zen 2 cannot forward a store into a
  broadcast load.
- A dedicated row-update vectorizer for `linpack` (runtime dependence guard,
  scalar remainder) was correct but 2x slower and was removed. The next `k`
  step reads the vector-written row back with scalar 8-byte loads, which
  cannot be forwarded from the misaligned 32-byte stores.

The full suite showed no new failures. The serial full bench at 13:21
(`CC=clang`, recorded above) passed 72/72 fixtures and every per-class gate:
`binary` 3.31x, `linpack` 1.33x (329 µs vs 248 µs), and `dgemm` 5.26x
(137 µs vs 26 µs; ungated, host C under 1 ms).

### AVX/SSE transitions, vector accumulators and exit homing (2026-11-05 evening)

- Legacy SSE executed while the upper YMM halves are dirty stalls on this host
  (C microbenchmark: 81 ns per transition against 6.4 ns with VEX scalar code).
  The encoder mixed the two. On AVX targets the scalar and 128-bit SSE helpers
  now emit VEX forms (`NYTRIX_NO_VEX_SSE=1` restores legacy encoding), and
  `vzeroupper` runs before calls, before returns, and at entry to scalar-float
  blocks where no 256-bit value is live. `sor` fell from 2.67 ms to 0.70 ms
  and `dgemm` from 137 µs to 37 µs; this was the `dgemm` stall that had no
  explanation without IBS.
- Fixed-trip integer reductions keep a four-lane vector accumulator again
  (`NYTRIX_NO_VECTOR_ACCUMULATOR=1` disables it): `bce-matrix` 11.0 ms →
  3.4 ms.
- Loop-carry exit homing ignored liveness. A carried vector segment was
  published to its home before every loop branch whenever some successor had
  no carried arrival, including successors where the value is dead.
- Packed i64 `sar` by 1 is `(a >>> 1) | (a & SIGN)`, two cycles of latency
  instead of three.

`binary` stays at about 3.2x LLVM. Its LLVM reference is itself vectorized
across the outer loop: `NYTRIX_OUTER_VECTORIZE=0` compares our scalar loop
with LLVM vector code and is not a meaningful gate. The remaining gap is
instruction count (9.6M against 3.2M at higher IPC, 2.67 against 2.04), mostly
in the signed `x / 2` sequence and the xor/and/xor select.

The full serial bench at 18:36 (`CC=clang`, 3+1, load average 4–12, recorded
above) passed every timed fixture and every per-class gate. Overall NYR/LLVM
was 1.29x and the maximum 3.17x on `binary`. Other results: `dgemm` 1.52x
(39 µs vs 26 µs), `sor` 1.21x, `linpack` 1.49x, `bce-matrix` 0.53x.

Built and validated after that run (224 of the 225 vector, ABI and optimizer
fixtures pass on a private build; `f64-scalar-abi` fails on the release build
too, from the concurrent `--run` argv change):

- `vec8.shr.i64` (`NYR_MACH_VSHR`, `vpsrlq`). The outer vectorizer emits it
  for `(x >> 63) & 1`, which replaces four instructions of the signed-division
  bias: `binary` 880 µs → 831 µs.
- Functions whose float allocation and loop-pinned floats stay in xmm0..xmm3
  give xmm4..xmm7 to vectors (`NYTRIX_NO_SHARED_XMM4_7=1` disables it). The
  fourth `dgemm` broadcast now stays in a register (about 35 µs).
- `NYTRIX_OUTER_VEC_INTERLEAVE=2` widens to two four-lane registers per value.
  It is correct but slower (1.40 ms) because the second register set spills.
  Off by default.

## NYC copy intrinsic register residency (2026-11-11)

The x86-64 loop allocator now treats known copy intrinsics as clobbers that
callee-saved registers can survive. Copies remain indirect-store hazards when
stack-home privacy is unproven. Both indexed and fallback proofs agree.

A frozen before/after compiler comparison on the string benchmark's C source,
with `memcpy` replaced by `__builtin_memcpy` to exercise this path, retained
checksum `300003294` in all 11 runs. Best kernel times fell from 643127 ns to
273198 ns; medians fell from 714369 ns to 324634 ns. Object assembly shows
fewer loop-counter reloads. This is an intrinsic-path measurement, not closure
of the existing string benchmark: its declared `memcpy` remains a call and
measured essentially unchanged. The `small-copy-loop-residency.c` regression
passes NYC O3 and Clang O3, including writes through an escaped local address.

### Declared memcpy follow-up

Small constant copies now also lower through compatible external `memcpy`
declarations, including the standard header prototype. Visible definitions,
assembler aliases, incompatible signatures, and `-fno-builtin` retain calls.
The seven-run focused string report
`build/bench-reports/bench-20261111-083623-3891112.json` reports `ok`, matching
checksums, and 184 us NYR, 202 us LLVM, 190 us C, and 280 us NYC (1.47x NYC/C).
The prior full report measured 3.04x NYC/C. Object inspection confirms no
`memcpy` relocation in the optimized fixture and a retained relocation with
`-fno-builtin`. The copy-residency and visible-definition regressions pass;
both are registered with the C frontend fixture runner.

Next:

- [Performance](performance.md) — measurement method and profile validation.
- [Changelog](../CHANGELOG.md) — verified release records.
