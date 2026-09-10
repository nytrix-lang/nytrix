# TODO

Open work only. Keep regression fixtures after fixes. Do not weaken assertions
or classify a timeout as solved by extending its limit.

Local test runs: use `NYTRIX_TEST_JOBS=32 ./make test` (96-core host); the
CI image stays at `NYTRIX_TEST_JOBS=1` for scheduler determinism.

## Landed 2026-09-10 (kept here until the next full-suite green run)

- [x] **Dictionary slot `0` ambiguity (nil vs int 0) fixed.**
  `rt_native_dict_set_str_compact` and `ny_native_dict_get_impl`
  (`src/code/runtime/core.c`) boxed every raw-looking word with
  `rt_native_is_int`, which returns true for `v <= 4096` — so a stored nil
  (raw 0) re-emerged as tagged int 0 (`type()` "unknown", `is_nil` false) and
  stored int-0 flipped the other way. Both boundaries now leave raw 0
  untouched: the dynamic ABI reserves 0 for nil, and a raw-int producer that
  needs an unambiguous scalar 0 must box before the call. Fixed fixtures:
  `values/nil.ny`, `values/collections.ny`, `execution/gc.ny` (plus the
  dict-dependent asserts across `use`, `sugar`, `prolog`).
- [x] **Untyped extern vs int/nil literal equality fixed.**
  `__str_builder_append(0, "x") == 0` routed through `rt_any_eq`, which reads
  the raw word as a dynamic value (0 = nil) and tags the literal. The
  comparison lowering in `lower_arith.h` now keeps both sides raw when one
  side is a call to a non-user (runtime bridge) function and the other is an
  int or nil literal. Fixed fixture: `values/strings.ny` builder asserts.
- [x] **`startswith`/`endswith` fixed** by keeping raw `__str_len` results in
  typed-int locals (`lib/core/str.ny`). Bare `__str_len(s)` in an untyped
  context is a raw i64 that dynamic consumers decode (len 11 reads as 5,
  len 5 as 2); annotate `def int n = __str_len(s)` at call sites.
- [x] `rt_type_name` maps tagged scalar immediates (tag 1) to "int" so
  `type()` of a dynamic integer no longer reports "unknown".

## Compiler and runtime

- [ ] Complete the remaining semantic/ABI audit of shared NYIR lowering. The
  unresolved groups are intrinsic families not yet driven by canonical semantic
  facts; validate specialization, bounds/tag elimination, alias analysis, and
  allocation on native and LLVM.
  **Confirmed remaining sites** (every direct call to
  `ny_native_runtime_symbol(...)` that bypasses the semantic-first wrapper
  `ny_native_runtime_symbol_for_expr`, `lower.c:655-680`): `lower.c:2868`,
  `lower.c:4265`, `lower_call.h:3332`, `lower_expr.h:460-461`,
  `lower_expr.h:491-492`, `lower_stmt.h:4042`, `lower_stmt.h:4084`. That's
  the full list — grep for `ny_native_runtime_symbol(` minus the wrapper's
  own definition to reconfirm if this drifts.
  **Important nuance, checked, don't skip it:** these are not uniform.
  `lower_stmt.h:4042` and `:4084` query a bare `name` string with no
  `expr_t*` call site available (a function-declaration collection gate
  asking "does this name have a raw bridge," not "resolve this call") — they
  can't just swap in `_for_expr`. Triage into (a) real call sites with an
  `expr_t*` in scope (`lower.c:2868`, `lower_call.h:3332`,
  `lower_expr.h:460/461/491/492` — straightforward swap) vs (b) name-only
  queries (`lower_stmt.h:4042`, `4084`, check `lower.c:4265`) that need
  either a `fn->semantic`-based sibling helper or a documented reason they
  stay spelling-based. Migrate one site at a time with the same cold-probe
  verification the last migration used (dict, dynamic-dict-scalar-box,
  typed-store-byte-order, comptime, matrix), not all seven in one pass.

- [ ] **HIGHEST VALUE, blocks the most fixtures: the raw/tagged decode chaos
  on untyped scalar returns.** A raw i64 with bit0=1 is indistinguishable from
  a tagged int, and every dynamic consumer (`rt_any_to_i64`, `rt_any_eq`,
  `print`, arg adapters) halves odd words exactly once. So `fn square(x) { x *
  x }` returns raw 49 and `square(7) == 49` compares 24 against 49; `foo(6..9)`
  prints `0 19 0 33` (repro: an untyped `fn foo(x) { x * x }`, direct calls —
  results differ per calling spelling, which proves multiple box/decode layers
  disagree). This one gap is the failing assert behind `parser.ny` ("fn expr"),
  `sugar.ny` ("comma assignment from pair"), `use/use.ny` + `use/core.ny`
  ("file module add"), `bytes-chain.ny` (bytes get through an untyped local
  routes to the dynamic tbuf bridge and returns tagged bytes: 65 reads as 32),
  and large parts of `type.ny`/`iter.ny`/`sha256` index panics.
  **Why the obvious fix was reverted twice on 2026-09-10:** boxing at the call
  site (`lower_call.h`, dropping the `callee_name_alias` gate on
  `module_raw_scalar_leaf`) and boxing binary tails in
  `ny_native_nir_normalize_return` (`lower_stmt.h`, `b->return_any`) each moved
  the corruption instead of fixing it — the callee-side literal box, the
  alias-only call-site box, `rt_any_*` helpers (which return tagged), and the
  consumer decoders all act on the same register, so a box added at one layer
  surfaces as a *different* wrong value one layer later. The fix must be a
  single authoritative fact, not another local box: thread a per-function
  return representation (RAW vs TAGGED) through the NYIR builder — set it in
  the callee's return lowering for every shape (literal, binary scalar-op over
  scalar operands, resolved raw ident, rt_any_* call), emit the box exactly
  once there, and give the call site a query so no adapter re-decodes. Verify
  with the square/foo repro plus `git stash`-style A/B on: `values/nil.ny`
  (`identity(nil)` must keep nil), `values/strings.ny`, `modules/use/*.ny`.
  Do not ship any box that fixes `square(7) == 49` while breaking
  `identity(nil) == nil` — that is the exact failure mode of both reverted
  attempts.

- [ ] Define one explicit dynamic-value/container ABI. Replace ambiguous raw
  eight-byte dynamic slots with canonical metadata/storage and decode values
  exactly once across calls, returns, captures, globals, dictionaries,
  callbacks, and list writes. Remaining failures include cross-call reads,
  callbacks, non-scalar dictionary values, vector/object returns, and the LLVM
  proof-logic raw-index/value path.
  The legacy LLVM matrix/list ABI still has mismatched raw-slot behavior.
  **State as of 2026-09-09, freshly verified:** every fix that landed this
  session was on the *read* side — `rt_native_tbuf_get_any` and the
  recursive case in `rt_native_tbuf_eq` (`runtime/core.c`). Confirmed
  working: `items(range(...)).get(...)` round-trips correctly now.
  **The write side is the same disease, still unfixed, in two places:**
  1. List writes: `_list_set` (`lib/core/iter.ny:440`) and
     `_store_item_raw` (`lib/core/reflect.ny:124`) both write via bare
     `store64(xs, value, idx * 8)` with no identity/tag handling at all.
     Freshly repro'd: `it.chunk([1,2,3,4,5],2).get(0,nil)` returns `[3, 3]`
     instead of `[1,2]`; `[[1,2],[3,4],[5,6],[7,8]].filter(fn(v){v.get(0,nil)>2})`
     drops an element (len 2 not 3) and returns `[5,6]` as element 0 instead
     of `[3,4]`.
  2. Dict writes have the identical shape of bug on the C side:
     `ny_native_dict_set_impl` (`runtime/core.c:1945-1975`) ends with a bare
     `slot->value = item;` — no tag/identity handling, same pattern as the
     list `store64` calls, just in the native dict implementation instead of
     `.ny` stdlib code. This is almost certainly what "non-scalar dictionary
     values" in this bullet already refers to; not yet repro'd this session
     but worth treating as the same fix, not a separate investigation.
     **Update 2026-09-10:** the nil/int-0 half of this is fixed (see "Landed"
     above — both dict boundaries now preserve raw 0). What remains here is
     the identity/tag handling for non-scalar values on the write path.
  **Prework, in order:** (a) give `store64`'s codegen
  (`ny_gencall_store_idx_intrinsic`/`fast_store64`) the same
  detect-a-magic-tbuf-handle-and-preserve-it treatment `tbuf_get_any` has on
  read, or more simply, stop routing `_list_set`/`_store_item_raw` through
  raw `store64` and point them at `rt_native_tbuf_set` (`runtime/core.c:888`)
  instead if that bridge already normalizes — check that before writing new
  C; (b) apply the same fix to `slot->value = item` in
  `ny_native_dict_set_impl`; (c) retest, in this order: the `it.chunk` and
  nested-list `filter` repros above, `etc/tests/runtime/values/collections.ny`,
  `etc/tests/runtime/language/prolog.ny` (currently failing — "proof-logic
  raw-index/value path" above is most likely this fixture, not a separate
  LLVM-only mechanism), and the benchmark checksum mismatches already noted
  elsewhere for `fasta`/`iter` (hypothesized same root cause: dynamic
  list/value ABI).

- [ ] Finish import-scope and probe families: vector/object return ABI, image
  decoder paths, and compile-heavy UI/Vulkan fixtures. Keep device-dependent
  probes gated from the CPU-only suite and bound cold compilation separately
  from warm execution.
  **Vector/object return ABI:** the `dim(vec2(...))` case was retested with
  the corrected import (`use std.math.vector as vecm`) and returns `2` cleanly
  on native — the earlier report used the wrong import, so this sub-item is
  closed rather than an open ABI defect (fresh repro: `dim(vec2(1,2))` →
  `2`).
  Still open in the same family: `etc/tests/shapes/probes/math/vec3-normalize.nshape`
  segfaults (signal 11) on the typed `vec3` layout path
  (`load64_f64`/`store64_f64` with `__layout_offset`), pre-existing and
  reproduced at baseline.
  **Image decoder paths:** genuinely not shortcuttable from here — the JPEG
  heap corruption is specifically flagged elsewhere as bogus Huffman
  counts/list reads causing OOB stores, which needs real codec-level
  debugging, not a file:line pointer.
  **Cold-vs-warm compile timing:** there's already a flag for this —
  `-prof`/`--prof` ("Enable compiler/runtime profiling outputs (timings +
  stats)", confirmed present in `--help`). Use it to get a real cold/warm
  split on the compile-heavy Vulkan/window probes instead of estimating from
  wall-clock timeouts; if `-prof`'s output doesn't already separate
  compile-time from run-time, that gap in the flag itself is worth fixing
  before adding new timing instrumentation elsewhere.

## Semantic model roadmap

- [ ] Generalize value-indexed types beyond `Fin<N>` to user-defined bounded
  constructors, dependent result normalization, and first-class `where`
  obligations.
  **Root gap, confirmed in code:** `ny_type_t`'s `apply` case
  (`src/code/typing/types.h:211-229`) only carries *type* arguments
  (`arg0`/`arg1` as `ny_type_t*`). There is no value-argument slot. `Fin<N>`'s
  bound is smuggled through the type's plain name *string* ("Fin<42>")
  instead — which is why every consumer re-parses that string:
  `types.c:1510-1520` (`strncmp(want_base, "Fin<", 4)`), `types.c:2114-2256`
  (bound extraction/range-check), and `lower.c:1538-1780`
  (`ny_native_parse_fin_bound`, `ny_native_resolve_fin_bound`,
  `ny_native_nir_fin_bound_for_name`). None of that generalizes to a second
  constructor name; it's all keyed on the literal substring `"Fin<"`.
  **Prework, in order:**
  1. Give `ny_type_t.as.apply` a real value-argument (e.g.
     `int64_t value_arg; bool value_resolved; const char *value_symbol;`,
     the symbolic case already exists ad hoc in `lower.c:1657-1780` and
     should move here) instead of encoding it in the name string.
  2. Add `ny_type_apply_bounded(...)` next to `ny_type_apply` in `types.c`.
  3. Find and extend the generic-type-argument parser to accept an
     int-literal or def-identifier token as an argument (not yet located —
     do this before step 4, don't guess at its shape).
  4. Replace the `types.c:1510-1520` string comparisons with a check on the
     new struct fields, for any constructor that declares a value parameter
     — not just the name "Fin".
  5. Replace `lower.c`'s `ny_native_parse_fin_bound`/`ny_native_resolve_fin_bound`
     with reads off the new field; delete the string re-parsing.
  6. Extend `ny_try_monomorphize_call`'s cache key (referenced at
     `types.c:13`) to `(ctor_name, value_arg)` pairs generically.
  Only once 1-6 land does `where`/dependent-result-normalization become a
  small addition instead of a second special case — don't start on those
  until the value-carrying type exists.

- [ ] Add hygienic AST `quote { ... }` and typed `${...}` splicing for comptime
  declarations, types, expressions, tests, and serializers.
  **What exists already:** a non-hygienic `macro name(args) { body }` form —
  parsed at `src/code/parse/stmt/core.c:2629` (`parse_macro_stmt`) and
  `stmt/dispatch.c:383,418` — expanded by cloning the body AST and
  substituting parameter names, in `src/code/parse/stmt/comptime.c` (the
  clone/substitute pass around its `NY_E_MEMCALL` handling, ~line 1067).
  That clone-and-substitute function is the "existing textual templates"
  the roadmap text above refers to as a compatibility layer.
  **What's missing, confirmed by grep:** zero hits anywhere in `src/code/`
  for `gensym` or `hygien*`. There is no fresh-name generator and no
  syntax-context/provenance tag on identifiers. This is greenfield, not an
  extension of the macro system.
  **Prework, in order:**
  1. Add a `quote { ... }` expression that captures its block as a first-
     class AST *value* instead of compiling it immediately — reuse the
     existing `fields(...)`/`exports(...)` comptime-reflection channel that
     already hands real AST/type metadata to comptime code, rather than
     inventing a second value representation for quoted trees.
  2. Add `${ expr }` splice parsing, valid only nested inside `quote{}`
     (context-sensitive, not a general grammar rule).
  3. Add a gensym counter and a syntax-context tag distinguishing
     "identifier introduced by the template" from "free identifier resolved
     at the quote{} call site" — this doesn't exist and is the actual
     hygiene mechanism, not a detail.
  4. Extend `NY_E_IDENT` resolution in `src/code/native/lower/lower_expr.h`
     (the fallback chain at lines 137-465, the exact code already touched
     this session for the leaked-name segfault) to check that tag: template-
     introduced names get gensym'd before insertion, free names resolve
     against the *defining* scope, not the use scope. This is the one place
     hygiene actually gets enforced, and it's already fragile code — go
     carefully, add regression fixtures alongside the existing leaked-name
     ones rather than after.
  5. Extend to types/tests/serializers only after plain expression/decl
     splicing is solid — they're variations on the same mechanism.
  6. Once quote/splice works, desugar `macro` (item above) into it and
     retire the separate clone/substitute path rather than maintaining both.

- [ ] Add effect parameters/capability values and row-polymorphic `shape`
  composition for typestate APIs while keeping ABI-sensitive layouts nominal.
  **Current state, confirmed in code:** effects are a flat `uint32_t`
  bitmask with exactly four bits — `NY_FX_NONE/IO/ALLOC/FFI/THREAD/ALL`
  (`src/code/typing/types.h:188-193`) — fully resolved once per function
  signature (`sig->effects` in `funcpurity.c`, enforced against
  `forbid_effects` around `funcpurity.c:2300-2400`). There is no effect
  *variable*: a higher-order function like `map`/`filter`/`each` can't
  currently express "my effect is whatever my callback's effect is," so
  those stdlib functions are stuck either over-approximating to `NY_FX_ALL`
  or under-checking.
  **Prework, in order, cheapest/highest-impact first:**
  1. Add an effect-variable concept parallel to the type-variable system —
     reuse the existing union-find substitution machinery
     (`ny_subst_t`/`ny_subst_fresh`/`ny_subst_bind`/`ny_subst_union` in
     `types.h`) rather than building a second unification engine; either tag
     effect vars into the same structure or clone the pattern into a small
     parallel `ny_effect_subst_t`.
  2. Change `sig->effects` to "resolved mask OR effect-var id" and unify it
     at call sites the way HM already unifies types, so a call to
     `map(fn, xs)` gets `fn`'s effect propagated instead of a fixed mask.
     This alone fixes the real, present imprecision in every higher-order
     stdlib function debugged this session.
  3. Only after propagation works, add capability *values*: a stdlib opaque
     handle type (e.g. `IoCap`) taken as an ordinary parameter; extend the
     existing enforcement pass (`funcpurity.c` ~2300-2400) to also require a
     matching capability value in scope. This is mostly a stdlib + checker
     addition once (1)-(2) exist, not a new core mechanism.
  4. Row-polymorphic `shape` composition for typestate: no existing
     structural/row machinery was found for this — don't start it blind;
     first confirm whether `shape`'s current reflection support
     (`fields(...)`/`exports(...)`) has anything extensible, as a separate
     scoping pass before committing to a design.
  Do effect-variables (1-2) before capability values (3) before typestate
  rows (4) — each step is load-bearing for the next, and (1-2) is the one
  with an existing pattern to copy rather than invent.

## Benchmark correctness and performance

From `./make bench` 2026-09-09 (62 fixtures). Everything below was
independently re-verified, not just read off the table — repro commands are
exact and copy-pasteable. Ordered by what's actually most valuable to fix
first, not by table order.

- [ ] **JIT/mcjit and shared NYIR correctness need separate diagnosis from the
  dynamic-value ABI work above.** The latest benchmark reports `matrix` as a
  three-way checksum mismatch (`false|false|true`): both Ny native and Ny
  LLVM agree with each other but disagree with C, so it is not currently
  justified to call this JIT-only. Keep the JIT-specific reproducer below as
  a separate check because engine selection can still expose an additional
  failure:
  1. `matrix` (checksum `false|false|true`, i.e. native and LLVM both wrong,
     C right): isolated to a 25-line reproducer — a `mut acc = 0.0` float
     accumulator updated by `acc += d` once per outer-loop iteration of a
     nested `for i { for j {...}; d = dot(...); acc += d }`. Confirmed:
     `-O3 -run` → `acc=1.1051e+12` (correct); `-O3 --jit` → `acc=0` (wrong),
     reproduces identically on both `--native-backend=x86_64` and
     `--native-backend=llvm`. Repro at `/tmp/matrix_iso2.ny` this session,
     rebuild from `etc/tests/bench/matrix.nshape`'s `source ny` block if that
     path is gone.
  2. `sha256` (table says `FAIL`, not `MISMATCH` — the harness never gets a
     comparable number): `-run` succeeds but computes the wrong checksum
     (253567 vs C's 248162 — a real, separate bug, same shape as the
     fasta/list-ABI issues below); `--jit` doesn't even get that far, it
     panics outright with `PanicError: index_read out of range`. Repro:
     extract `source ny` from `etc/tests/bench/sha256.nshape`, run with
     `-run` vs `--jit -O3`.
  **Working hypothesis for the JIT-specific variant, not yet confirmed — test this first:** the emitted
  native asm header reports `tier budget=150000 hot=16 cold=1` (visible via
  `--emit-asm`), i.e. a loop promotes from cold to hot after 16 iterations.
  `matrix`'s outer loop runs 128 times, `sha256` processes many message
  blocks — both comfortably cross a hot-tier promotion mid-run. An
  accumulator or index losing its value exactly at a tier promotion (OSR)
  boundary is a classic bug shape. To test: find whatever sets that `hot=16`
  threshold and temporarily raise it well above the loop's iteration count;
  if `matrix` stops returning `acc=0` at the higher threshold, that confirms
  OSR/tiering as the cause rather than something in the mcjit codegen path
  generally. Don't start fixing before this test — it decides which
  subsystem the bug is even in.

- [ ] **`escape-loop` (marked REGRESS, 14752x native/C — the worst gap on
  the whole board): SROA/escape-analysis for a per-iteration transient
  literal is provably not firing, despite `-O3` and despite dedicated
  `src/code/ir/opt/escape_sroa.c` and `sroa_scalar.c` passes existing.**
  Confirmed by dumping `--emit-asm` for the fixture's exact body
  (`def p = [i]; acc += p[0]` inside a 1,000,000-iteration loop): the loop
  body contains a live call to `rt_tbuf_new_raw` (24-byte heap alloc),
  `rt_tbuf_index_any_raw`, and `rt_any_to_i64` — a full allocate/write/
  read/decode cycle every single iteration, exactly what SROA/escape
  analysis exists to eliminate since `p` provably never escapes the loop
  body. Since this is flagged REGRESS (not just "still open"), check
  `git log --follow -- src/code/ir/opt/escape_sroa.c src/code/ir/opt/sroa_scalar.c`
  for a recent change before writing a new fix — this may be an actual
  regression in existing logic, not a gap that was never covered, and the
  fix might be reverting or adjusting a recent change rather than adding
  new logic.

- [ ] **`cmov-sort` (11614x native/C) and the broad family of catastrophically
  slow-but-*correct* benchmarks (`iter` 4773x, `list` 7008x, `vector` 3641x,
  `heapsort` 3191x, `dict` 76x, `string`/no ratio but 618ms) likely share one
  root cause: list index read/write has no fast path for a provably
  homogeneous element type — every access is a full runtime call.**
  Confirmed for `cmov-sort` specifically by dumping `--emit-asm` for its
  O(N²) insertion-sort inner loop (`a[j] > key`, `a[j+1] = a[j]` on a plain
  `list` of ints built via `append`): every single comparison and shift goes
  through `rt_tbuf_index_read_raw`/`rt_tbuf_set_i64_raw` as real function
  calls — no inlining, no raw pointer-offset load/store even though nothing
  about this list is polymorphic in practice. At ~2M inner-loop steps this
  alone plausibly explains the multi-second runtime against C's 605µs.
  **This is the same "bounds/tag elimination" pass category already listed
  as existing coverage** in the compiler optimization reference ledger
  elsewhere in this doc — worth checking whether it's implemented but gated
  behind a specialization/monomorphization precondition that a plain
  `list`-typed local never satisfies, versus never implemented for list
  index ops specifically. **Prework:** before touching codegen, add a
  minimal reproducer (a tight loop doing `a[i]` read and `a[i]=x` write on a
  `list` built via `append`, no other complexity) as a dedicated fixture, and
  check with `--emit-asm` whether *any* existing specialization path already
  produces a direct load/store for it under different type annotations
  (e.g. a function parameter typed as a fixed-size array/`Fin`-bounded list)
  — if so, the fix is widening that path's trigger condition, not building a
  new one. This one fix, if it lands, is likely to move `iter`/`list`/
  `vector`/`heapsort`'s numbers simultaneously since they all hammer list
  index ops in a loop — highest performance ROI on this whole board.

- [ ] **`fasta` (MISMATCH, checksum 36125000250000 vs C's 17937500250000,
  ratio not a clean multiple) is confirmed *not* the JIT-only bug above** —
  retested `-run` vs `--jit`, both give the identical wrong 36125000250000,
  matching the table's own "both Ny backends agree with each other but
  disagree with C" note. That points at the shared dynamic-value/list-ABI
  work already tracked in this doc's Compiler-and-runtime section, not a
  JIT-specific issue. Next step: bisect `fasta.nshape`'s source (it
  generates/checksums a byte sequence) to find which specific operation
  diverges, the same way the `it.chunk`/`filter` repros were isolated
  earlier — don't assume it's identical to those without a bisected repro,
  just don't treat it as a fresh mystery either.

- [ ] **Two results are suspiciously *better* than C and should be verified
  before being trusted, not celebrated:** `loop-unswitch` at 0.07x/0.08x
  native+LLVM vs C, and `mixed-static-island` at 0.60x native but 1.14x LLVM
  (inconsistent between Nytrix's own two backends, which is itself a flag).
  Genuine dead-code elimination already shows up elsewhere in this same
  table as `<10µs` (e.g. `bce-matrix`, `binary-trees`, `dce`, `gcbench`) —
  `loop-unswitch` printing a nonzero 114µs while still beating C by 14x is a
  different shape of result and more likely means part of the intended work
  is being silently skipped than that it's been legitimately optimized away.
  Confirm by checking the printed checksum against the C reference's
  checksum by value, not just trusting the `ok` status — the harness's `ok`
  here may only mean "didn't crash," not "checksum matched," the same gap
  that let `sha256`'s AOT path silently compute the wrong number above.

## Full-suite failure triage

The 2026-09-10 full-suite replay reported 31 failures (CI run 34476295757,
commit `c2ea3b8`; local 32-job replay: 38). Status after the 2026-09-10
session: `values/nil.ny`, `values/collections.ny` (container/index asserts),
`execution/gc.ny`, `language/struct.ny`, and the `values/strings.ny` builder +
startswith/endswith asserts are FIXED (dict 0-ambiguity, extern literal
compare, typed `__str_len` locals). Keep the rest as separate work items:
several are genuine semantic/runtime defects, while the
`ny: failed to exec ny-full` rows are test-harness path failures and must not
be “fixed” in the language.

- [ ] **`runtime/modules/import.ny`: timeout; `shapes/probes/lang/ext.nshape`:
  timeout; `gltf-index-modes`, `render-camera-contract`, and
  `proof-logic`: timeout.** These need phase profiling before code changes:
  run each with `--prof --no-progress` and separately bound cold compilation
  (`NYTRIX_STD_CACHE=0`) from warm execution. If compile time dominates,
  fix import expansion/cache invalidation; if execution dominates, capture
  `--nyir-run-profile` and isolate the first hot loop. Acceptance is a
  bounded run under the existing 20-second limit, not a larger timeout.

- [ ] **`extensible.ny`: runtime/comptime expansion mismatch.** Reduce to the
  smallest declaration whose runtime and comptime reflection differ. Compare
  AST serialization after parse, comptime expansion, and NYIR lowering; unify
  the first divergent representation. Add a paired runtime/comptime fixture
  and require byte-for-byte normalized expansion equality.

- [ ] **`remote-socket.nshape`: `sendlineafter prompt`.** Reproduce with the
  fixture's deterministic socket/tube backend and `NYTRIX_TRACE_FILTER` on
  receive buffering. Check prompt matching across chunk boundaries, especially
  `recv_until` plus unread-buffer handling. Add a one-byte-at-a-time fixture;
  acceptance requires matching prompts for chunk sizes 1, 2, 8, and backend
  default without sleeping longer.

- [ ] **`concurrency.ny`: segmentation fault.** Run under ASan with thread
  tracing and isolate the smallest spawned callback/future sequence. Audit
  callback capture lifetime, thread argument packing, and shutdown ordering;
  add a join-before-free regression. Acceptance is clean under ASan and the
  normal runtime for repeated runs.

- [ ] **`type.ny`: `get expects ... got int`.** Reduce the failing `get` call
  and inspect whether a dynamic receiver is being lowered as a scalar after
  `__tagof`/unboxing. Fix dispatch at the semantic receiver boundary, not by
  accepting integers in `get`; preserve the existing type error for genuine
  integer receivers.

- [ ] **`matrix-assign.nshape`: the first assert now passes; two `mat4`
  perspective asserts remain behind the dynamic float/container ABI.** The
  "parameter index assignment does not mutate" hypothesis is disconfirmed:
  parameter-index writes and reads do mutate correctly. The real bug was the
  *method* spelling `list.get(i, d)` used inline as an operand — it lowers to
  the tagged dynamic ABI (`rt_tbuf_get_any`, `runtime/core.c:1443`) but the
  comparison/arithmetic path only unboxed the *free-call* spelling
  (`get(l, i, d)`, commit `e8f67d1`), so `a.get(1,0) == 99` compared the
  boxed int against a raw literal. Fixed by extending that scalar-get unbox
  to the memcall form (`lower_arith.h`, gated to skip the f64-converting
  `.get` path). Regression fixture:
  `etc/tests/runtime/values/memcall-get-inline.ny` — passes under JIT,
  `--native-only`, and the NYIR-backed LLVM backend; the legacy compat
  `--legacy-llvm` backend still differs on the parameter-mutation assert.
  Fresh repro (still failing): `q[7] = 1.7505` on a float list reads back
  `1` — the `set_idx` unproven-view fallback (`lower_call.h:1996-2008`)
  calls `rt_f64_to_i64`, truncating the float to an integer store, and the
  read side independently degrades (`p.get(2, 0.0)` → `inf`, `p[17]` → a
  heap address). Those two asserts (`p.get(2, 0.0) != 1.0`,
  `p.get(17, 1.0) == 0.0`) depend on the dynamic container ABI bullet, not
  on parameter lowering.

- [ ] **`runtime/modules/use/use.ny`: file-module `add` mismatch.** Reproduce
  with std cache disabled and dump imported module aliases plus the selected
  function symbol. Check relative-module caching and alias collision before
  changing arithmetic or call lowering. Acceptance requires the file-module
  and inline equivalent to return the same value in all engines.
  **Update 2026-09-10:** `helper_val() == 123` passes; the residual failure is
  `helper_add`-style arithmetic returns — same untyped raw-scalar return gap
  as the HIGHEST VALUE bullet above (the alias-gated box at `lower_call.h`
  covers `use`-module aliases but direct same-module calls stay raw).

- [ ] **`runtime/language/prolog.ny`: recursive query returns the wrong number
  of answers.** Reduce to one recursive relation and inspect list/dictionary
  writes crossing callbacks. Compare raw values at callback entry, proof
  unification, and result collection; this is likely related to the dynamic
  container ABI but must be proven with a value trace. Add a two-answer and a
  zero-answer regression.

- [ ] **`runtime/language/attr.ny`: `@jit` function-pointer call failure.**
  Reduce to one attributed function passed as a pointer. Verify whether the
  pointer names the JIT trampoline, the source function, or a stale cache
  address; invalidate/rebind the pointer at the compilation boundary. Add a
  repeated-call test after cache reset.

- [ ] **`runtime/values/iter.ny`: `filter list`; `collections.ny` mutation
  failures.** Run the fixtures independently under JIT, `-run`, and
  `--native-only`. Keep the nested-list regressions already present; fix the
  engine-specific representation mismatch in the shared list write/read
  bridge and require all engines to pass before closing the ABI TODO.
  **Update 2026-09-10:** both fixtures now fail with `PanicError: index_read
  out of range` *after* their earlier asserts (collections passes its
  container/comptime/index blocks first) — the panic moved past the dict
  fixes into the list write/read bridge (`store64`-style writes, bullet above).

- [ ] **`values/strings.ny` — `swapped` segfault (blocks the tail of the
  fixture).** `swapped("abcd", 0, 3)` walks
  `_sorted_list_copy` → `swap_items` → `_char_list_to_str`; the last one
  returns a corrupted handle (prints as a raw heap address), and the
  following `== "dbca"` dereferences it (signal 11). `_char_list_to_str`
  (`lib/core/mod.ny`) builds via `Builder`/`builder_append`/`builder_to_str`
  then `builder_free(out)` before returning `s` — the string comes from
  `rt_malloc` (GC-managed) and appears clobbered/freed by the time the caller
  compares it, so suspect GC rooting of the intermediate `def s` across
  `builder_free`, or a missing `@returns_owned` propagation through the
  `use std.core.str` inside the function body. Repro:
  `def chars = _sorted_list_copy("abcd"); swap_items(chars, 0, 3); print(
  _char_list_to_str(chars))` → garbage address.

- [ ] **`values/bytes-chain.ny` — bytes values through untyped locals.**
  `b.set(0, 65).set(1, 66)` stores correctly, but `b.get(0)` on an untyped
  `def b` routes through the dynamic tbuf bridge and the tagged byte then
  loses a bit-shift per consumer: `print` shows 65 as 32, and
  `b.get(0) == 65` compares a decoded 32/49 against the tagged literal.
  `bytes.get` on a *typed* bytes local lowers to `rt_bytes_get_raw` and is
  correct — the fixture needs the untyped-receiver path to stay raw (part of
  the return/representation-fact work at the top of this file). Repro:
  `def b = bytes(3); b.set(0, 65); print(b.get(0))` → 32.

- [ ] **Performance correctness gate.** Do not optimize the benchmark ratios
  until `matrix`, `fasta`, `sha256`, `escape-loop`, and the list-heavy
  `cmov-sort`/`iter`/`list`/`heapsort` cases have independent checksum and
  crash reproducers. For each optimization, record `--prof` compile/runtime
  phases, `--emit-asm` call counts, and before/after checksum. A speedup is
  accepted only when native, LLVM, JIT, and C agree where the fixture supports
  all four.
