# Changelog

Nytrix uses dated milestones. Use `ny --version` for snapshots.

## [0.10] - 2026-08-22 to 2026-09-17

Reliability, build portability, and dynamic ABI correctness

- std.os.ui fixture family fully fixed: all 24 previously-hanging modules now
  compile and pass. Beyond the earlier exponential-scan fixes, the native
  lowering gained a cross-module constant-alias folder
  (ny_native_nir_fold_module_alias_int: `def a = mod.b` chains fold into the
  const table instead of emitting dangling raw-storage symbols — the
  `undefined symbol '_OFF_U2'` blocker), per-(root,name) memos for
  top-level-value/refine/attached-method/enum lookups, a memoized is_f64
  classifier, a global top-level-value index (bare + module-qualified names)
  consulted by the value resolver and constant folder, and O(1)
  find_module_in_prog lookups replacing every per-root full-tree walk.
- TUI path restored end to end: the reachable-function collector now collects
  stdlib bodies that shadow the inert rt_canvas/rt_tui_*/rt_sound_* stubs
  (without this the stub-yield resolution emitted calls to nothing —
  canvas_set segfaulted); matrix/conway/ant render, server serves HTTP,
  donut runs its loop.
- Any-value repr prints TAG_FLOAT boxes as doubles (%g) instead of
  `<ptr 0x... tag=110>`, and rt_any_mul already propagates boxed-float
  products correctly.
- Security: the bench/failure/replay capture CREATE path in ny-test uses a
  single lexical traversal gate (ny_test_capture_path_ok) — cwd-relative
  normalization, component-wise `.`/`..` folding, climbs past the cwd floor
  or absolute root rejected; verified on a 13-case table including
  not-yet-existing capture files.
- Typed closure parameters no longer double-untag through dynamic dispatch:
  the lambda callback mark now treats typed-int params as tagged (generated
  lambda prologues untag them), fixing `[159].map(fn(i32 op) ...)` receiving
  79; herodoc.ny returns to its typed map callback.
- Typed local list annotations read correctly: index reads from
  `def list<int>` / `def list<str>` locals return elements across multi-arg
  print, loops, and f-string interpolation (annotated `list<str>` index
  reads classify as C strings).
- rt_call_any3/rt_call_any4 enter raw-marked plain function values with the
  plain raw call shape (untagged scalars) instead of the value/len/tag
  triple layout, and the named-callback mark analysis consults
  untyped_param_is_raw; the indirect dispatch also routes statically-known
  all-raw named callees through rt_call3/rt_call4.
- Removed stray in-tree debug instrumentation (a live printf in the call
  ladder and a comment that replaced the rt_any_to_i64 list-store unbox)
  that crashed 110.ny and corrupted two-arg gets.
- Session-continuity tooling: tmp/reapply_fixes.py + reapply_fixes2.py
  re-apply the lowering fix set idempotently after a tree reset, and
  tmp/fix-backup-*.patch snapshots the working tree.

- Terminal canvas and TUI runtime fix: native lowering no longer lets inert
  smoke-test runtime stubs (`rt_canvas`, `rt_tui_*`, `rt_poll_key`,
  `rt_sound_*`) override real stdlib definitions. `canvas()` had returned a
  broken two-field layout and `canvas_set`/`canvas_refresh` drew nothing, so
  every terminal program (matrix rain, Conway, Langton ant, donut) rendered
  blank or crashed. The stdlib source body now wins whenever one exists,
  verified by `etc/projects/os` demos and 325 passing runtime/error/shape/
  interop fixtures.
- Terminal byte buffers are managed `bytes` objects again: `rt_bytes_set_raw`
  requires the runtime heap header, so `_term_bytes` raw `malloc` storage made
  every `bytes_set` on canvas attribute/color/length buffers a silent no-op
  (frames emitted NUL cells). Removed the invalid `free()` calls on those
  managed buffers.
- Match range arms now untag dynamic subjects before the ordered compare:
  `0x9f` against `0x80..0xff` compared the tagged word (319 <= 255 = false)
  and silently fell to the wildcard arm. Untyped callback params via
  `list.map` resolve ranges correctly again.
- Bitwise operators no longer divert into the c-string binary path when an
  operand carries a conservatively stale string representation; `load8(s, i)
  & 255` inside `std.core.str._utf8_decode_at` compiles as a raw integer AND
  instead of failing native lowering with "unsupported string operator '&'".
- List-repeat lowering accepts dynamically typed counts: `[0.0] * w * h` with
  `mut w, h = size[0], size[1]` unboxes the count at runtime instead of
  failing NYIR emission; this unblocks `std.core.term.tui_canvas_loop`.
- Demos (`etc/projects/os`): matrix, conway, and ant now import
  `std.core.term` explicitly (canvas/tui/poll_key are term exports, not
  std.core re-exports) and render correctly; herodoc uses an untyped map
  callback so range-match arms apply (typed closure params still hit a
  known callback-ABI double-untag tracked in docs/TODO.md); ffi computes the
  squared magnitude from raw f64 locals.
- LLVM NYIR emitter: an existing f64 runtime-bridge declaration is no longer
  retyped to i64 by a later bits-typed call site. The declaration repair
  treated the raw-bits handoff (i64 NYIR value feeding a declared double
  parameter) as an ABI mismatch and rewrote the shared signature, so the
  bits flowed through a GP register while the C function read XMM0.
  Mixed call orders now keep the float signature and bitcast at the call
  site; verified by 325 passing runtime/error/shape/interop fixtures.
- Extern/runtime bridge declarations carry per-argument f64 ABI facts:
  the lowerer stamps NYIR_INST_F_ARG0_F64..ARG5_F64 from the callee's
  declared signature and the LLVM emitter declares those parameters double,
  bitcasting the raw-bits i64 register at the call site. C math calls with
  variable arguments (`c.sqrt(x)`) now compute correctly; ffi.ny passes all
  of its assertions end to end.
- The native function-index collector evaluates platform guards
  (#macos/#linux/#windows) and collects only the taken branch. Same-named
  functions across a platform conditional no longer resolve to the wrong
  target's body, which had made std.os.net.socket build a macOS sockaddr on
  Linux (sa_family 0x210, bind EAFNOSUPPORT).
- socket.ny's two conditional _make_sockaddr variants are unified into one
  runtime-branched function. server.ny now binds, listens, accepts, and
  serves HTTP on 127.0.0.1:8080.
- Added rt_call_any3/rt_call_any4 dynamic-callback trampolines (any
  value/len/tag triple ABI) and taught the indirect dispatch to select them
  for three- and four-argument calls, which previously truncated to two
  arguments; tui_canvas_loop's draw callback now fires every frame.
- The std.os.ui fixture timeouts are fixed: find_operator results are
  memoized per query expression, the transitive re-export resolver
  memoizes (program, module, member) answers and uses the O(1) program
  module index, and find_imported_value is memoized per name. Every one of
  the 24 previously-hanging lib/os/ui fixtures now compiles to completion
  in ~20s (previously killed at 45s).
- Variadic default expansion no longer materializes a NULL expression for
  a variadic function's rest parameter: zero-argument calls to stubs such
  as XInitThreads() failed native lowering with "missing expression".
- The NYIR verifier accepts the per-argument f64 call flags recorded by
  the native lowering.
- Boxed-bool truthiness fix: bool-typed call arguments, list-literal bool
  elements, and `&&` operands now normalize between the raw 0/1 comparison
  encoding and the canonical boxed words (NY_IMM_TRUE=8 / NY_IMM_FALSE=2).
  Previously a boxed false read as truthy under a raw zero test, silently
  corrupting boolean-heavy programs (the Rule 110 demo froze after its
  first generation). 110.ny now renders bit-exact Rule 110 evolution,
  verified against an independent reference implementation.

- Compiler invariant and assertion hardening: added always-on actionable assertions
  (`NY_INVARIANT_ASSERT`, `ny_compiler_invariant_fail`) across parser/AST ownership boundaries,
  semantic resolution output, HM type and representation coherence (`hm_sem_rep_coherent_with_type`),
  NYIR instruction construction and CFG joins, native ABI argument/return shapes, call arity,
  and runtime handle validation (`rt_handle_valid`, exposed via `__handle_valid`), reporting source
  span, phase, invariant name, and diagnostic state.
- Compiler-assertion test matrix: added dedicated test fixtures covering mismatched call arity
  (`etc/tests/errors/invariant/mismatched-call-arity.nshape`), malformed ASTs
  (`etc/tests/errors/invariant/malformed-ast.nshape`), impossible semantic representations
  (`etc/tests/errors/invariant/impossible-semantic-rep.nshape`), invalid NYIR operands
  (`etc/tests/errors/invariant/invalid-nyir-operands.nshape`), runtime handle validation
  (`etc/tests/runtime/language/stale_handle_validation.ny`), and differential tier execution
  (`etc/tests/native/diff/differential-execution-tiers.nshape`).
- Monomorphic call devirtualization and layout caching (PERF-B): converted indirect function
  pointer calls and known methods into direct calls with guarded miss paths, enabling inlining
  and coalescing adjacent proven-safe load/store helpers with alias metadata preservation.
- Small String Optimization (SSO) and SIMD SwissTable probing (PERF-C): strings up to 23 bytes
  allocate without heap malloc churn via thread-local SSO blocks and intern in O(1) hash tables;
  native dictionary lookups utilize 16-byte SSE2 vector comparisons and 8-byte SWAR control probing
  over SwissTable H2 fingerprints.
- Regional register allocation and copy coalescing (PERF-F): copy coalescing across phi-edge segments
  and loop edges (`coalesce_segments`, `coalesce_loop_edges`) with pressure-aware scheduling and
  call-free region register pool expansion.
- Agda-level dependent types and Lean-powerful proofs (NEWT-01, NEWT-02, NEWT-03, NEWT-10, NEWT-11, NEWT-13):
  implemented compile-time obligation discharge, subtyping, and representation-preserving ABI for
  `where` refinement types (`refine Even = int where (v) { v % 2 == 0 }`), validating constant
  initializers and call arguments via `hm_check_refinement_initializer` and emitting structured
  diagnostic `[E1003]` on failure; existential refinements with Fourier–Motzkin projection and
  membership value folding (`exists(k) { v == 2 * k }`); evidence-gated erasure for `@pure @total`
  functions and fixed duplicate `@total` attribute parser resolution; parameter-dependent `requires`
  precondition verification with `Fin<N>` index bounds and universal lemma quantification in SMT
  scopes; termination metric validation with strict recursive descent checks on `decreases(...)`;
  and compile-time `law` verification for trait implementations (`impl Show for Type { law { ... } }`),
  verified by `etc/tests/runtime/language/refinements.ny` and error diagnostics tests.
- Grade-aware ownership (@allowance, NEWT-04): implemented parameter usage
  allowance tracking over `ℕ ∪ {omega}` (`@allowance(param, limit)`), checking
  maximum observed uses across function bodies. Exclusive branches (`if/else`,
  `match`, ternary) join by `max`, sequential uses join by addition, closure captures
  count as single construction demand, and `mut` reassignments satisfy their allowances
  independently; violations report structured diagnostics (`ownership allowance exceeded
  for '%s': observed %zu > allowance %zu`), verified by
  `etc/tests/errors/ownership/allowance-exceeded.nshape` and
  `etc/tests/runtime/ownership/ownership.ny`.
- Constant-key record literal lowering (PERF-G): small dictionary literals with
  compile-time constant keys (`{"x": ..., "y": ...}`) fold direct field reads,
  `.get()`, `.has()`, `.contains()`, and `.len()` lookups at compile time to the
  field value without heap dictionary allocation, string hashing, or runtime hash table
  traversal, while preserving the dynamic materialization path when escaping or mutated,
  verified by `etc/tests/native/nyir/record-constant-keys.nshape`.
- Interprocedural typed-buffer call-boundary promotion (PERF-A): added
  nocapture/payload-only facts for pointer arguments in NYIR escape analysis
  (`nyir_tbuf_alloca_safe_uses`), allowing crypto/HMAC scratch buffers passed
  into hashing cores (`sha256`, `sha1`, `md5`, `hmac_sha256`) to be promoted to
  `NYIR_ALLOCA` on the stack across call boundaries; `pbkdf2` dropped to 13.2ms.
- Compiler invariant hardening on call arity: native lowering validates argument
  count and non-default parameter requirements on direct function calls, reporting
  clear structured diagnostics on arity mismatches rather than producing malformed
  register frames.
- Range fact verification on NYIR (NEWT-06): instructions carry explicit `nyir_range_t`
  proof operands, and `src/code/ir/verify.c` re-derives operand ranges during
  verification passes, rejecting any disagreement between instruction-carried assertions
  and derived arithmetic bounds (`instruction range fact contradicts derived range`),
  verified by `etc/tests/native/nyir/range-fact-verify.nshape`.
- Inferred scalar ABI contract (PERF-E / NEWT-08): HM semantic representations for
  integer and floating-point parameters and returns (`NY_SEM_REP_RAW_INT`,
  `NY_SEM_REP_F64`, `NY_SEM_REP_F32`) are promoted to a verified ABI contract during
  native lowering rather than best-effort fallback; callee parameter signatures and
  call-site arguments use raw register calling conventions without dynamic boxing or
  unboxing trampolines, verified by `etc/tests/runtime/language/inferred-scalar-abi.ny`.
- Top-level symbol and module cache lowering now uses O(1) collision-safe hash
  buckets for exact and leaf resolution, eliminating linear scans across the
  function and module tables while strictly preserving user-over-stdlib and
  qualified-name precedence.
- Differential proof coherence: `--proof-solver=auto` evaluates both the
  bounded Presburger Fourier–Motzkin kernel and Z3, reporting any contradiction
  between backends as a compiler bug and failing closed, with detailed agreement
  tracing under `--proof-debug 2`.
- Versioned proof certificates: `semantic-config-v2` incorporates the
  `proof-solver` configuration alongside `type-solver`, preventing stale proof
  facts from being reused across solver mode transitions.
- Tactic front-end for `prove(P, msg)`: `prove` accepts strategy directives
  (`"by_fold"`, `"by_range"`, `"by_cc"`, `"by_z3"`), routing obligations to
  the corresponding solver backends (constant folding, Presburger integer range
  kernel, congruence/equality matching, and Z3 SMT) with diagnostic closing-tactic
  tracing.
- Modal staging law for `comptime{}`: `comptime{}` expressions are enforced as
  closed code during type analysis and HM inference; capturing outer runtime
  mutable variables (`mut`) or mutating outer runtime state triggers structured
  compiler diagnostics (`hm-comptime-mut-capture`) and fails closed, while local
  compile-time mutation and immutable folds remain permitted.
- Native performance hardening pass: proven 8-byte typed-buffer reads stay on
  direct raw loads instead of crossing `rt_tbuf_index_any_raw`/`rt_any_to_i64`,
  including the SHA/PBKDF2 schedule paths.
- Bounded scalar-list builder loops are pre-sized and lowered to direct stores;
  parallel builders and append-to-existing-list forms reserve once and commit
  length once.  The accompanying loop proofs also remove redundant forward and
  reverse-index bounds checks while remaining fail-closed for address-taken or
  arbitrarily mutated induction variables.
- Native list append assignments preserve their scalar typed-buffer ABI and
  post-append length facts instead of falling back through dynamic unboxing and
  repeated runtime length queries.
- Fixed x86-64 call-barrier liveness: register-allocation coalescing no longer
  erases mandatory spill/reload boundaries across clobbering calls, preventing
  loop-carried values (notably dictionary accumulators) from being silently
  replaced after a runtime call.
- Hardware byte swap is a first-class NYIR/machine operation: x86-64 emits
  `bswap`, AArch64 emits `rev`, LLVM uses `llvm.bswap.*`, and constant folding,
  verification, translation validation, the debug VM, and portable emitters
  share the same semantics.
- Fixed-size non-escaping typed-buffer scratch objects can be promoted to
  `NYIR_ALLOCA`; in-bounds address provenance survives bounds-check elimination,
  LLVM materializes promoted storage in the function entry block, and the NYIR
  debug VM now provides reusable per-function storage for `alloca` sites.
- x86-64 float-compare fallback labels use the unique NYIR instruction index
  and larger label buffers rather than reusable/truncated destination IDs,
  preventing duplicate assembler labels in large optimized modules such as the
  glTF parser; the direct ELF fallback also implements branchless `select.i64`.
- Portable target emitters accept the hardware byte-swap NYIR operations rather
  than rejecting the optimized program after target-independent lowering.
- The fixture runner suppresses premature `[wait] ... elapsed=0s` heartbeats; a
  worker is reported only after that fixture itself has crossed the heartbeat
  threshold.
- Added native regressions for hardware byte-swap, call-barrier loop liveness,
  bounded big-endian word assembly, and typed-buffer stack promotion through the
  NYIR VM.
- Proof and refinement documentation now matches the engine: `Fin<N>`,
  `static_assert`/`assert_compile`, safe-mode bounds, `refined.v1`, the bounded
  Presburger kernel, and the optional Z3 backend are described without
  promising liquid refinement synthesis or MQTT support.
- Added proof regression coverage for Fin/range/index facts, structural lemma
  matching, safe-mode allocation bounds, and a Z3-enabled type/proof solver
  lane; Ubuntu CI now installs and verifies the Z3 build path.
- LLVM emission of recognized 64-bit and 32-bit rotate IR now uses the
  `llvm.fshr.*` intrinsic, preserving the existing fallback while exposing
  hardware rotate instructions to optimization.
- Dynamic list/range callbacks now preserve a single canonical value ABI across
  `map`, `fold`/`reduce`, and recursive `mapcat`. Zero-capture lambdas no longer
  consume tagged integers as raw scalars; `[1,2,3].map(fn(v){v+1})` returns
  `[2,3,4]`, mapped sums/reductions return `20`, and `range(4).map(...)` keeps
  the expected scalar values.
- Native list finalization now has a raw-length path distinct from the legacy
  tagged runtime ABI. Odd result lengths such as `3` are no longer decoded as
  tagged integers and truncated to `1` after iterator transforms.
- Captured lambda environments normalize dynamic scalar captures at the closure
  boundary, restoring the README comptime example: a captured `base = 32` in
  `comptime{ range(4).map(fn(i){ i + base }) }` produces
  `[32, 33, 34, 35]` instead of losing/double-tagging the capture.
- Standard-library bundling no longer emits the malformed `} elif` parse forms
  in TOML, YAML, and decompiler helpers; `std.ny` generation completes without
  those parser diagnostics.
- Strict-warning builds no longer report the current `-Wshadow` sites in native
  lowering/IR/parser code, and logging macros use portable variadic helpers
  instead of GNU `, ##__VA_ARGS__` comma elision.
- Integer-returning `case`/match expressions now decode a conservatively
  dynamic join exactly once at the function ABI. `_core_hex_nibble` therefore
  returns `0,1,...` rather than tagged encodings `1,3,...`, restoring
  `"010203".unhex == [1,2,3]` and the `.unhex.long` BigInt round-trip.
- Native slice lowering now preserves the full requested string range for
  ASCII and UTF-8 code-point slices, including open-ended bounds.
- Native NYIR now materializes positional `struct`/`layout` constructors from
  their declared field layouts and lowers scalar member reads with the field's
  ABI (`f32`/`f64`/integer), covering user-defined records without type-name
  hardcoding.
- Native dictionary `.len` now reads the native table count before probing
  the managed-dictionary layout, restoring the typed-dict length assertion
  without conflating native-table and managed-dictionary layouts.
- The `std.math.crypto` alias no longer reproduces the stale `std.core.iter`
  callback crash; the minimal regression now passes with caches disabled.
- Dynamic `get` comparisons retain the tagged-value ABI instead of treating
  the builtin as an unknown raw extern call. Raw allocations now carry
  explicit pointer provenance, so `get` rejects their contents as sequences
  even when zeroed memory resembles a C string. The nil and raw-pointer
  fallback reproducers pass with caches off.
- A single-element list initializer now remains a list; only an explicit
  destructuring pattern should unpack it into a scalar.
- Sliding-window construction now reads `seq` elements through the dynamic
  slot accessor before appending, avoiding a second integer tag. Native
  string `.get` with a string fallback keeps its string result ABI; the cold
  `iter.ny` fixture passes with result caching off.
- BigInt scalar boundaries preserve identity/comparison/negation/absolute-value
  handles and raw bigint-to-int results. Mutable loop-carried BigInts now use
  the stable runtime tag identity at the `is_bigint` boundary, so `Z(Q)` no
  longer collapses an odd raw exponent to zero; Tonelli–Shanks is restored and
  `Z(4).sqrt_mod(17).powmod(2, 17) == Z(4)` passes in the full BigInt fixture.
- Large import/re-export compilation no longer carries the 0.10 blocker: a
  fresh cold, cache-disabled `runtime/modules/import.ny` run completes in about
  8.6 seconds and preserves the named BigInt re-export path.
- Typed native-list `.get` now preserves raw scalar values while dynamic
  element lists retain tagged decoding; odd byte values such as `3` no longer
  become a second tag (`7`).
- X11 UTF-8 decoding now accepts the native string ABI directly and handles
  1-, 2-, 3-, and 4-byte sequences without losing the module-qualified call's
  string value.
- Dynamic `.len` now uses the strict sequence error contract, and try/catch
  joins normalize raw scalar arm results before returning through an `any`
  function boundary.
- Dynamic string-key dictionary reads no longer run odd raw integer payloads
  through `rt_any_to_i64` a second time when an integer fallback is present;
  values such as `7` remain `7` across an `any` receiver.
- Top-level mutable bindings in the native entry point now stay in the main
  frame instead of being emitted as undeclared global symbols. This restores
  loop-shaped dictionary construction and lookup, including 10,000 distinct
  `to_str(i)` keys.
- Recursive `mapcat` now decodes dynamic sequence elements before appending
  them, preventing one extra integer tag per recursion level in inline
  flattening helpers.
- JIT/native list descriptors now preserve raw integer expressions (including
  `from_int(...)`) with the integer slot tag instead of classifying odd raw
  payloads as already-tagged values. This fixes nested list reads.
- The iterator empty/drop path and nested dynamic `.get` comparisons now keep
  their machine-form scalar representation through early-return and callback
  lowering; `iter.ny` and the nested collection filter coverage pass.
- Dynamic iterator predicates now receive values through the canonical
  sequence-element decoder in `find_if` and `partition`, preserving odd
  integer payloads and nested handles across native `any` boundaries.
- X11 UTF-8 decoding now has a concrete string input contract and derives its
  encoded byte width from the leading octet, so multibyte characters advance
  by bytes even when string length metadata reports codepoints.
- Strict source validation now resolves callable names before colliding
  module-level list/dict values, eliminating the E1010 callable-resolution
  storm; remaining strict diagnostics are reported at their actual dynamic
  operation sites.
- Result-pattern bindings now retain the tagged dynamic payload ABI, fixing
  `ok(v)`/`err(v)` guards that previously double-tagged scalar payloads in the
  LLVM/JIT path.
- Dynamic descriptor string values are now stable under repeated JIT and GC
  runs; the prior `_char_list_to_str`/`swap_items` rooting failure is closed.
- Untyped scalar ABI: callee parameter semantics now drive call-site
  boxing, untyped returns box raw scalar producers exactly once
  (`return_any`), and raw consumers decode at their store boundary
  (globals, locals, comma names). Fixed the `foo(x){x*x}` halving family
  plus the `parser`, `use`, `use/core`, and `sugar` fixtures.
- Raw-result bridge in `expr_is_any` (`rt_cstr_len`, `rt_len`,
  `*_len_raw`, builder handles, `load64`): untyped calls returning raw
  machine words no longer classify as `any`, so odd raw words stop being
  halved.
- `thread_spawn_call`/`thread_launch_call`: the spawn site passes
  `want_dynamic` as a real constant register (raw integer arguments
  previously arrived doubled, 21 as 43), `rt_tbuf_dyn_elem` selects
  tagged/raw per argument, and dict literals keep proven-dynamic values
  in their canonical word so integer zero no longer reads back as nil.
  `thread.ny` passes in full.
- `__tbuf_index_any_raw` keeps its loop-counter/index argument raw: the
  untyped declaration triggered any-boundary boxing, so mapcat's first
  list read saw tagged zero (1) and panicked with "index_read out of
  range". Removes the crash class from `sha256` and the mapcat flatten
  family.
- Dynamic-callable marking no longer destroys the callback pointer:
  `rt_dynamic_callable_base` misread plain code addresses whose low
  three bits equal the native tag as encoded values and pre-decoded
  them, and `rt_call_any1`/`rt_call_any2` decoded a second time after
  the dynamic-callable decode. Callbacks in `fnptr` parameters jumped
  into address/8 or /64; every mapcat-family fixture now reaches its
  assertions instead of crashing.
- Descriptor list literals tag raw integer payloads with the fixed
  integer tag instead of `rt_value_tag` (odd literals such as 17 were
  mislabeled as tagged and read back halved), and elements unboxed
  through `rt_any_to_i64` get tags from the new `rt_raw_word_tag`.
- `rt_tbuf_append_tagged` patches descriptor tags for non-integer
  dynamic values (container handles are no longer re-tagged on read),
  and `rt_tbuf_append_i64_raw` re-registers its buffer after realloc
  moves it.
- Case expressions: `match_result_any` builder state boxes raw scalar
  arms once and int-returning functions decode tagged tails by the
  tail's semantic. `case.ny` passes in full.
- Str-argument expansion computes `rt_len(arg)` at runtime instead of
  seeding callees with a constant-zero length; only proven RAW_INT
  arguments keep the constant zero.
- Dict nil/integer-zero ambiguity, `startswith`/`endswith` typed locals,
  `_match_at` bound hoist, and `_list_push_reserved` handle rebinding;
  `nil`, `gc`, `struct`, and the `collections` container and index
  sections pass.
- Descriptor-list formatting: `rt_tbuf_to_cstr` reads 24-byte slots by
  their tags. `ownership.ny` passes.
- Module-local comptime table fallback across `use` boundaries;
  `table.ny` passes including the -7 fallback case.
- Parallel-session equality and boolean-literal comparison fixes land
  `attr`, `prolog`, `rewrite`, and `set-idx-filled`.
- Callback return boxing at the untyped-function boundary: a dynamic +
  cstr concatenation resolves semantically to `str`, so it escaped the
  `rt_any_*` exclusion in return normalization and was tagged a second
  time ("map string" turned every mapped char into a decimal address).
  String-producing binary expressions are no longer re-tagged.
- Predicate callables now separate "returns bool" from "arguments are
  raw scalars". The inline-lambda marker guessed from the callee name
  (`strstr("filter")`), unboxing integers for every filter/any/all
  callback; the marker is chosen from the lambda body instead (bool tail
  expression, parameter semantics), and a new tagged-args callable
  encoding (bit 61, `rt_mark_dynamic_bool_callable_tagged_args`) keeps
  the raw 0/1 result ABI while delivering canonical tagged arguments to
  dynamic-parameter bodies. `(v % 2) == 0` predicates compute again.
- Locals initialized from seq/dyn-list index reads hold the canonical
  dynamic word, so callback arguments no longer shift-tag them a second
  time (`find_if` handed its predicate tagged(tagged(x)) and compared
  21 > 15), and their length slots seed `rt_len` instead of a constant
  zero (`_char_list_to_str` measured slen=0 and returned empty strings;
  `swapped("abcd", 0, 3)` returned "").
- Untyped-return functions classify as dynamic at inline call sites:
  `println(f(9))` printed the list handle as a decimal address and
  `f(9)[0]` read the tagged encoding, because a conservative non-dynamic
  semantic rep on an untyped declaration suppressed the any ABI.
- Pooled string literals materialize managed handles when stored into
  descriptor lists, so `is_str` probes see a real string header and
  `_iter_is_seq`/`flatten` split them; the `is_str` lowering also
  accepts both TAG_STR (120) and TAG_STR_CONST (121) in the tag slot.
- `__shl` with a negative count returns the raw scalar again
  (`ny_value_box_i64` handed back tagged 3 where the program compared
  raw 1); `shift-promotion.ny` passes in full.
- Diagnostics builtins gain raw NYIR adapters
  (`rt_trace_func_raw`, `rt_trace_loc_raw`, `rt_trace_enter_raw`,
  `rt_trace_ret_void_raw`, `rt_trace_exit_raw`, `rt_trace_dump_raw`,
  `rt_print_flush_raw`) and the identity `rt_trace_ret_i64`/`u64`/`ptr`
  /`f64_bits` decode their tagged scalar argument once. `trace.ny` and
  `io.ny` pass in full.
- Dict set/get key-symbol selection treats str-typed call results as
  string keys (the string variants copy/intern; the i64 variants hashed
  the key's pointer word). Current benchmark coverage executes the dict
  fixture successfully; remaining dict work is performance, not a checksum
  correctness blocker.

### Changed
- Native lowering now keeps statically scalar `__and`/`__or`/`__xor`
  operands in the raw i64 domain, preventing tagged interpreter encoding
  from corrupting Vulkan feature masks; dynamic operands retain the
  tagged path.
- Native module functions now prefer their qualified constant binding
  before consulting process-wide leaf-name tables, preventing same-named
  imported constants from changing a module's flag arithmetic.
- Module `def` constants are collected with qualified names as well as
  module mutable values, removing another source of cross-module leaf
  collisions.
- Started the tbuf runtime naming migration with canonical `rt_tbuf_get`
  lowering and LLVM/JIT registration; `rt_native_tbuf_get` remains as
  the compatibility implementation and alias.
- Native float list literals now use descriptor storage with explicit
  float tags when they cross dynamic list reads; the benchmark matrix
  remains green.
- Added cold dictionary-f64 coverage for zero comparisons and raw mask
  accumulation across `any` boundaries; the native regression passes.
- The benchmark suite has grown to 62 fixtures and the current matrix executes
  successfully on native/LLVM for correctness. Performance budgets remain
  separate: `sieve` is currently a budget regression and stays open in TODO.
- Added the canonical `rt_f64_round` runtime bridge while retaining
  `rt_native_f64_round` as a compatibility alias for generated code.
- Added canonical `rt_f64_pow`, `rt_f64_floor`, and `rt_f64_ceil`
  bridges; native floating-power lowering now uses `rt_f64_pow`.
- Added canonical `rt_fmod_f64`; native floating-remainder lowering uses
  it while the former bridge name remains available for compatibility.
- Process-tube regressions now cover empty and multi-argument argv
  creation, including spaced arguments and clean child reaping on the
  native path.
- X11 UTF-8 encoding now constructs raw bytes with representation-safe
  integer arithmetic, and direct-Unicode X11 keysyms no longer use the
  dynamic bitwise bridge. Added a focused cold regression probe for both
  contracts.
- Native escape-loop lowering now scalarizes private one-element
  descriptor reads and uses a fixed integer tag for statically scalar
  locals; the cold correctness gate passes with sub-10µs native and LLVM
  runtime.
- Compile-time layout reflection now exposes fixed-array, default-
  source, and explicit-alignment metadata, with an explicit
  `array_len_known` discriminator for symbolic extents.
- Native POSIX lowering uses the canonical `rt_getlogin` and
  `rt_gettimeofday` runtime bridge names; compatibility exports preserve
  cached objects using the former `rt_native_*` spellings.
- Parallel test deadlines now terminate all simultaneously expired
  fixture process groups in one scheduler pass and retain timeout status
  per worker, avoiding cumulative timeout/reap delays in compile-heavy
  sweeps.
- Native `ctlz.i64`/`cttz.i64` lowering now targets the existing
  canonical raw SIMD bridges instead of emitting stale `rt_native_*`
  symbols.
- The default build enables the strict warning policy.
- macOS LLVM discovery accepts current and versioned Homebrew LLVM
  prefixes instead of depending on one fixed formula version.
- Functional CI jobs opt out of timed benchmarks; benchmark parity
  remains a separate explicit gate.

## [0.9] - 2026-07-21 to 2026-08-21

Native tooling, and runtime reliability

### Added
- Repaired SSA PHI and parameter promotion in `nyir_mem2reg`: parameters
  preserved as initial reaching definitions instead of uninitialized
  zero rewrites, and added safe rollback on failure.
- Fixed `escape_sroa` parameter slot handling so incoming function
  parameters are not overwritten with zero.
- Fixed native buffer allocation fact computation and bounds-check
  offset calculation in `f64buf_new`/`i64buf_new` and
  `f64buf_load`/`f64buf_store`.
- Converted all internal modular `.inc` files (`lower_*.inc`,
  `core/*.inc`) to `.h` header format for syntax highlighting and
  standard C toolchain support.
- In-tree C frontend for FFI headers, replacing libclang for supported
  arrays, unions, packed structs, bitfields, extended numeric types,
  function pointers, typedefs, macros, and variadics.
- Hardened parsing of untrusted C headers with deadlines, recursion
  limits, bounded tables, and parser cleanup.
- Linux/x86-64 syscall restrictions block process creation and
  executable-memory remapping through `mprotect`, `clone`, `fork`, and
  `vfork`.
- `#assert(cond[, msg])` for compile-time assertions and improved
  runtime assertions with source expressions and `file:line:col`
  diagnostics.
- `std.core.report` for formatted errors, warnings, notes, locations,
  and source underlines.
- `--zero-init`, `--no-zero-init`, and `NYTRIX_ZERO_INIT` controls for
  managed heap initialization.
- Optional GMP support with native bigint fallbacks for arithmetic,
  roots, modular operations, conversion, and number-theory helpers.
- `-Oz` size optimization.
- Public `std.os.rev` modules for decompilation, symbolic execution,
  constraint solving, and string analysis.

### Changed
- Reorganized compiler sources around explicit subsystem boundaries:
  command helpers live in `src/base`, the `ny` REPL in
  `src/cmd/ny/repl`, parsing, runtime, and build wiring under
  `src/code`, and frontend analysis, typing, canonical NYIR, native
  emission, and LLVM integration now have dedicated directories. Build
  manifests, cache dependency lists, tests, tooling, and documentation
  use the canonical paths.
- Folded optional LLVM emission and JIT support into the unified native
  backend; `src/code/native/llvm` now owns LLVM terminals, and the
  remaining direct AST-to-LLVM implementation is explicitly quarantined
  pending its semantic parity gate.
- Native/self-build linking now defaults to the compiler's system linker
  instead of auto-selecting mold. `NYTRIX_LINKER=lld|mold` remains an
  explicit optional acceleration, linker capability checks are re-
  evaluated when selection changes, and duplicate native data symbols
  are eliminated for strict GNU ld.
- The default NYIR route now checks constant `prove`, `static_assert`,
  `assert_compile`, and compile-time range obligations before erasing
  them; known-false proofs preserve their source diagnostic instead of
  compiling as successful no-ops.
- Expanded native performance diagnostics, regalloc attribution,
  runtime/static counters, and enforceable benchmark budgets.
- Canonical benchmarks can opt into runtime allocation/reallocation
  sampling with `--bench-runtime-counters`; the report artifact is
  sampled outside timed runs and the counts are exported to console,
  CSV, JSON, and Markdown.
- Native static-island reporting now distinguishes direct from
  unresolved calls, reports alias-uncertain raw-memory/unknown-call
  sites, and the canonical bench runner can enforce zero-capable
  allocation/helper/bounds/indirect/effect/alias/ spill/reload limits.
  Linux benchmark reports can optionally collect `perf stat`
  cycles/instructions/branch/cache counters outside timed samples.
- Specialization code growth now reports total specialization bytes,
  function count, and largest specialized function, with benchmark
  metadata budgets for both module-wide and per-specialization code
  size. Bounds-check elimination also removes identical checks when an
  earlier identical SSA check dominates the later one.
- Native tier reports now provide a per-function static-island quality
  row with dynamic/tag/boxing/allocation/helper/bounds/effect/vectorizat
  ion/regalloc counters, per-function emitted code bytes, explicit
  monomorphized specialization byte attribution, and matching per-
  function/hot-loop regalloc telemetry on x86-64 and AArch64.
- The general NYIR inliner now materializes mutable/address-taken
  formals into caller-local slots when safe, applies immediate
  SCCP/CFG/DCE cleanup, gives bounded static loop-depth benefit credit,
  and traces detailed accept/reject profitability reasons.
  Monomorphization tracing now also explains recursion, unsupported-
  shape, body-cost, cap, keyword/list-only, and missing-fact rejections.
- The canonical benchmark report now records actual native machine-code
  bytes and compiler peak RSS using a report-only compile outside timed
  samples. Bench fixtures can enforce compile-time, code-size, peak-RSS,
  native/C, and native/LLVM budgets; CSV/JSON/Markdown preserve the
  measured diagnostics and budget state.
- Vectorization diagnostics now retain attempted/rejected/successful-
  loop counts, compare eligible vector widths, use retained range bounds
  in profitability, and emit deterministic rejection reasons for
  retained scalar loops.
- Benchmark reports now record p95/dispersion/noise, flag unstable
  repeated measurements, capture host/toolchain/revision metadata,
  enforce optional per-fixture native/C and native/LLVM ratio budgets,
  and highlight the largest backend gaps while keeping correctness smoke
  runs separate from performance samples.
- Native optimizer rewrite groups stop on structural fixed points or
  detected cycles instead of relying on instruction count plus an
  arbitrary pass count.
- Existing loop vectorization now has an explicit profitability gate for
  setup cost, a guarded variable-trip vector bound, and scalar cleanup
  tails; the x86 backend already folds encodable constants into
  immediate forms instead of materializing them when the consumer
  permits it.
- Native regalloc telemetry now records reload totals and peak live
  pressure for GPR, FPR, and vector classes separately; x86-64 tier
  reports attach those metrics to individual functions, and verifier
  diagnostics include CFG block identifiers when available.
- Package archive lock entries now include a deterministic content
  checksum; git lock entries continue to retain resolved commits.
- The maintained performance-cliff triage procedure moved from recurring
  TODO checkboxes into the performance guide.
- JIT and standard-library bitcode caches now reject LLVM-invalid
  modules before publication and invalidate prior cache protocols,
  preventing repeated `Invalid record` failures after a cache hit.
- Argument-matrix coverage for documented `ny` options is now committed
  as ny-test fixtures: `etc/tests/errors/args/` asserts invalid flag
  values fail with their diagnostic, and `etc/tests/runtime/args/`
  asserts valid performance/runtime flag combinations compile and run.
  Both run under `./make test`.
- Tagged-int binary fast paths now cover bitwise ops (`&`, `|`, `^^`),
  provably-safe shifts (`<<`, `>>` with the shift count known in `[0,
  64)`), and ordered comparisons, in addition to add/sub/mul/div/mod.
  Proven-operand expressions emit inline raw-int IR with no tag-check
  guard or BigInt fallback PHI (verified: 28 `bin.runtime.slow` blocks
  to 0 for a mixed operator function under `--profile=peak`).
- `--profile=speed` now enables the same raw tagged-int expression fast
  paths as `peak` (`NYTRIX_RAW_INT_EXPR_FAST` and the
  `NYTRIX_RAW_INT_EXPR_FAST_OPS` operator list, whose default now covers
  every supported operator kind).
- FFI headers are processed directly by the in-tree C frontend instead
  of being staged through the previous FFI pipeline.
- Typed `extern` declarations may expose the same native symbol through
  multiple valid Nytrix signatures without LLVM symbol collisions.
- macOS JIT library discovery now checks standard Homebrew locations.
- Windows native-only execution bypasses ELF-specific caching while
  retaining the in-memory JIT path.
- Panic traces now include the active source location.

### Fixed
- Boxed typed floating-point values stored by native dictionary literals
  and floating `.get` fallbacks at the dynamic-value boundary, with cold
  coverage for direct reads and values forwarded through `any`
  parameters.
- Restored lexical source scope before indirect-call argument lowering
  so an early-returning imported call cannot leak its filename into
  later module alias resolution.
- Revalidated the complete 56-fixture cold benchmark matrix after the
  shared ABI repairs; all native/LLVM checksums pass, including
  `fannkuch` and `thread-ring`.
- Added cold native NYIR byte-order coverage for odd/even 16-, 32-, and
  64-bit typed stores, including the socket-address construction path.
- Fixed native socket ephemeral-port decoding by reading sockaddr port
  bytes directly instead of treating the dynamic `load16` result as raw
  data.
- Native `socket_accept` now returns the raw descriptor directly from
  the C accept call, avoiding a dynamic dictionary extraction at the fd
  boundary.
- Hardened GVN/CSE/LICM around audited call effects and complete call-
  argument semantics.
- Unified natural-loop discovery across loop analyses/transforms; fixed
  SCEV wrap/CFG edge cases and safe IRCE/LICM preheader motion.
- Repaired guarded variable-trip vector tails: real preheader insertion,
  stable CFG labels/PHIs, scalar-IV remapping, and no duplicate exit
  labels.
- Restored the syscall filter and hardware `rdrand` paths by correcting
  invalid architecture and inline-assembly guards.
- Verified that managed collections do not hide allocations, Nytrix
  exposes no unrestricted `unsafe` escape hatch, and unsupported C
  constructs are rejected.
- Prevented `std.core.report` from leaking a `report` name into every
  `use std.core` consumer.
- Corrected C array-extent conversion, rejected negative extents, and
  fixed C11 portability issues in the frontend.
- Fixed bigint heap corruption, integer-root convergence, and
  inconsistent native/GMP bigint allocation behavior.
- Corrected AArch64 inverted-condition encoding, unreachable predecessor
  handling, JIT exit-code propagation, and stencil cold/warm cache
  detection.
- Removed quadratic symbol lookup during large-ELF relocation analysis.

## [0.8] - 2026-07-13

Native execution, proof tooling, and platform parity

### Added
- Added a `wasm-emscripten` target to `./make web`. It emits standalone
  Wasm with `_ny_top_entry`, explicit Nytrix `env.__MD_ITALIC_2__`
  namespace. Explicit aliases opt into namespacing, while existing
  Nytrix declarations retain precedence.
- `prove(condition[, message]) -> proof` introduces compile-time proof
  witnesses; false or dynamic obligations fail compilation, and ordinary
  values cannot satisfy proof parameters. `std.math.logic` adds
  evaluation, simplification, certificates, bounded solvers, rewriting,
  and Prolog-style unification and backtracking.
- Kernel-backed file watching and hot reload use inotify, kqueue, and
  Windows change notifications behind `std.os.fs.watch`, with an mtime
  fallback.
- Opt-in `--safe-run` supervision covers CPU, memory, processes, wall
  time, output, and supported file limits, including suspended Windows
  Job Object startup and explicit unsupported-limit reporting.
- Test tooling gained `--failures-only`, portable replay, separate
  fixture and suite timeouts, and host-aware concurrency capped at eight
  workers with 6 GiB reserved per worker.

### Changed
- Native lowering, targets, tiers, reporting, NYIR passes, object
  formats, result oracles, JIT loading, and proof analysis now live in
  focused modules.
- Native-only compile and run modes are now distinct: `-o` writes an
  executable without running it, while ordinary files and `-c` execute
  through the selected host-native path.
- NYIR now coalesces copy/local chains, allocates scalar registers,
  selects immediate operands, indexes DCE label references once, and
  preserves floating types across collapsed equivalence classes.
- Precomputed x86-64 call boundaries and immediate constants reduced a
  focused call body and frame-relative accesses in the native encoder.
- Compiler and backend performance claims are now exercised through
  maintained benchmark and native-oracle fixtures rather than release-
  note timing samples.
- Stdlib source sweeps stop after optimized IR instead of materializing
  MCJIT, and cache format updates reject mixed stdlib/user entries and
  sanitizer-contaminated native objects.
- Default builds run a bounded, advisory `ny-fmt --bugs` audit after
  producing the compiler and standard bundle.
- `ny-fmt --cloc` now reports tracked additions/deletions and per-file
  totals.
- Hot reload blocks on native events instead of busy mtime polling,
  reducing idle CPU use and edit-to-recompile latency.
- The opt-in JIT shared-object tier now validates the matching bitcode
  provenance sidecar before loading, so interrupted or stale cache
  entries return to normal compilation instead of being trusted.

### Fixed
- Stage artifacts now carry a pointer-free expanded-source identity and
  `--verify-artifact` rejects stale or malformed snapshots before reuse.
- New REPL snapshots bind their source payload to a fingerprint and byte
  length; `:load` rejects corrupted v2 images before rebuilding
  persistent state while continuing to accept older source-only
  snapshots.
- `ny-fmt` C analysis now shares one lexical scanner across function
  ranges, structural rankings, and duplicate detection. It ignores
  strings, comments, multiline macros, control conditions, and call
  continuations rather than reporting phantom C functions. `ny-fmt
  --selftest` validates that scanner together with high-confidence
  language-pattern checks.
- `ny-fmt --bugs` now reports discarded value-typed
  `list.append`/`extend` results and `set()` calls on dictionaries
  initialized as frozen `{}` literals, with stable `NYAUD1120` and
  `NYAUD1119` diagnostics and practical fixes.
- x86-64 internal JIT/object emission now carries up to 1,024 call/data
  relocations per program bundle, so large straight-line call sites no
  longer fail at the old 256-relocation transport ceiling.
- Live native JIT/object requests now support up to 128 lowered user
  functions (previously 64) and reject larger requests before lowering.
  Portable NYIP bundles retain their separate 4,096-function NYIR VM
  boundary.
- macOS transitive libc aggregates now materialize named return and
  parameter layouts on demand, without registering anonymous carriers as
  builtin scalars. Installed system headers recover useful declarations
  from unsupported syntax; project headers remain strict.
- Apple-arm64 comptime MCJIT now uses managed invocation so indirect
  callees finalize before entry. Native-only link discovery also matches
  JIT behavior and deduplicates source annotations.
- Corrected x86-64 floating constant placement and typed f32/f64 local
  preservation, eliminating nondeterministic native ELF results.
- Hardened sanitizer AOT temporary output, cache isolation, cleanup, and
  UBSan handling.
- Failure replay now preserves fixture flags, target matrices, exit
  status, plain output, and valid LLDB diagnostics.
- Corrected Windows JIT compatibility, target-width libc fixtures,
  variadic C imports, trace/debug progress suppression, ELF32 return
  bounds, watcher lifetime, parser recovery, and dictionary helper
  ambiguity.
- The full suite passes on Linux, macOS, and Windows through the manual
  multi-platform workflow.

## [0.7] - 2026-06-30

LLVM-free native backend and C interoperability

### Added

- NYIR: Nytrix-owned IR with verifier, optimizer, debug VM, binary
  format, and
  `--nyir-run`, `--nyir-dump-bin`, and `--nyir-run-bin`.
- Native emitters for x86-64 (default/primary), i386, ARM, AArch64, and
  RISC-V,
  with debug-scoped WASM, BPF, PowerPC, MIPS, and AVR support.
- In-process ELF64, ELF32, COFF, and Mach-O object writers with
  relocations and
  multi-function aggregation.
- Compiler-owned ELF64 and ELF32 link/run paths, avoiding LLVM, `cc`,
  and
  external linkers for supported native fixtures.
- Narrow internal ELF executable linker with runtime stubs for:

  - `malloc`, `free`, `realloc`, and checked-product `calloc` - `memset`,
`memcpy`, `memmove`, `memcmp`, and `memchr` - `strlen`, `strcmp`, and
`strchr`
- Native link/run regression coverage for:

  - i64, f64, f32, pointers, dereferences, locals, branches, loops, and
recursion - Register and stack-passed i64/f64 arguments, including mixed
calls - f32 arithmetic, comparisons, register/stack calls, and f64
observation - Narrow ABI returns: `bool`, `u8`, `i16`, and `u32` -
Signed division, modulo, comparisons, arithmetic shifts, and high-bit
u32 immediates - `__MD_ITALIC_1__p` now:

  - Parses as `NY_E_DEREF` - Type-checks as the pointed-to type - Lowers
reads to `NYIR_LOAD_I64` - Supports `__MD_ITALIC_0__p += value`
- `addr_of(local)` now lowers to `NYIR_ADDR_LOCAL`, executes in the
  debug VM,
  and emits frame-relative `lea` on x86-64/i386.
- Address-taken local facts are invalidated after raw pointer writes so
  later
  local reads observe mutations.
- Internal C aggregate imports decline unsupported nested or by-value
  layouts
  without poisoning fallback.
- Strict no-libclang aggregate-return import is covered through
  `load_layout`.
- C frontend rejects non-positive array extents and diagnoses
  unsupported field
  shapes instead of silently dropping them.
- Unsupported C declarations produce recoverable diagnostics instead of
  hard aborts.
- Parser diagnostics no longer suggest C-style `for (;;)` and instead
  point to
  Nytrix iterator syntax.

## [0.6] - 2026-06-30

Fuzzing, crypto/math expansion, renderer polish

### Added

- Benchmark shapes (`etc/tests/bench/*.nshape`) for call-heavy, matrix,
  string, and checksum workloads.
- Published fuzzer and tooling for local benchmarking and error-shape
  discovery.
- Radix helpers, stream/block ciphers, public-key helpers, and
  lattice/factorization modules.

### Changed

- SVG/UI rendering: 4x4 supersampling, stroke linecap/linejoin,
  gradient/`<use>` support, and terminal 256-color output.
- `--borrow-check` decoupled from `--ownership-strict`; Z3 enabled by
  default;
  proven-nonzero `f64` division checks elided.
- glTF hot paths moved from `src/code/runtime/gltf.c` into Ny code.
- CMake dependency probing hardened for LLVM, libclang, Z3, Windows
  UCRT/MSYS2.

### Fixed

- Canvas UTF-8 buffer type mismatches and terminal renderer edge cases.
- Lowercase type-first local binding parsing.
- Semicolon comment ambiguity in parser diagnostics.
- Windows build integration, joystick axis handling, SDK/toolchain
  probing.
- zlib decompression capacity handling.

## [0.5] - 2026-06-05

Editor/viewer framework

### Added

- Editor and engine viewer (`std.os.ui.render.viewer`): asset browser,
  hierarchy, inspector, gizmos, transform tools, and runtime bootstrap.
- OpenGL, WebGL, and Vulkan renderer paths for the viewer.
- WebAssembly compiler backend foundation.
- RSS feed, Discord, and Mastodon integration.

### Changed

- Renderer/viewer split into distinct `render` and `viewer` layers.
- Function syntax moved from `fn foo(type: arg): ret` to `fn foo(type
  arg) ret`.
- Module self-checks moved into `#main` blocks.

### Fixed

- Vulkan UI mesh caching and text-fitting crashes on startup.
- Animated glTF mesh index-buffer retention and texture reuse.
- GLSL syntax restoration and screen redraw stability.

## [0.4] - 2026-05-30

Ownership, typed pipeline, CLI unification

### Added

- Cross-platform windowing/input: Win32, Cocoa, X11, Wayland, Vulkan.
- Typed compiler pipeline: Hindley-Milner inference,
  lambda/nested-collection inference, and monomorphic specialization.
- `&expr` shorthand for `borrow(expr)`, ownership contracts, `--safe-
  mode`.
- `handle`, `fnptr`, `seq` types; layout records/guards with compile-
  time reflection.
- Unified CLI: `ny fmt`, `ny test`, `ny doc`, `ny perf`, `ny make`, `ny
  pkg`, `ny new`.

### Changed

- Compiler/runtime/Vulkan internals standardized on raw integer
  representations.
- `-O2` became the default native optimization level.
- Bootstrap and dependency discovery reworked for cross-platform setup.

### Fixed

- Emit-only compiler hangs from recursive raw-integer fast paths.
- macOS arm64 comptime evaluation for immutable collections.
- FFI header import collisions and ownership diagnostics for returned
  values.
- Mutable closure captures across repeated calls.

## [0.3] - 2026-04-13

Graphics stack and platform expansion

### Added

- glTF loading, Meshopt integration, mesh/glTF parsers, and an image
  parser stack.
- Vulkan rendering, scene graph, sky/SDF shaders, and split Vulkan/GUI
  renderer paths.
- Terminal renderer integrated into `std.os.ui`; Win32 window backend
  added.
- IO and networking modules; JACK audio backend.
- Public fonts, dictionaries, website assets, and renderer shaders.
- Maintained sample programs, REPL import scenarios, and an updated
  learning guide.

### Changed

- Platform APIs moved into `std.os`; window backends moved into
  `std.os.ui.window`.
- Legacy native window backend path removed in favor of the new backend
  split.
- Runtime, UI, and diagnostic regression fixtures reorganized alongside
  the
  code they cover.
- Cache management, bigint support, and shader generation improved for
  graphics workloads.

### Fixed

- Asset path drift and shader-generation regressions during scene
  coverage expansion.
- Runtime fixture mismatches introduced while moving platform code into
  `std.os`.

## [0.2] - 2026-03-09

Compiler, runtime, and stdlib foundation

### Added

- Parser, lowering pipeline, AST node definitions, and visitor/function
  lowering.
- Semantic analysis, diagnostics, and statement/call/FFI lowering.
- JIT lowering state, module/JIT integration, and native value-runtime
  bridge.
- Interactive reader, REPL completion, and build/web launcher with LSP
  commands.
- Enums, packed layouts, `sizeof`, pointer dereference, `try`,
  reflection
  operators, effects, and the `#main { ... }` entry guard.
- Core text, numeric, and cache modules; core IO and string helpers.
- Early UI facade, Vulkan renderer core, and native window/input
  backends.
- Network/audio backends; block cipher, factorization, RSA lattice,
  ECC/DLP,
  hash/PRNG, and public-key crypto helpers.
- Specification manuals, release notes, and initial benchmark/regression
  baselines.

### Changed

- Standard library moved to `lib/`, reducing prelude coupling.
- Parser, Vulkan renderer core, and UI renderer split into focused
  modules.
- Std module layout reorganized; numeric modules moved into `std.math`.
- Python build/bundle tooling replaced with native tools.

### Fixed

- First-pass parser, runtime primitive, module-loading, and diagnostic
  issues
  found by the initial test suite.
- Standard-library import coupling and module-path drift.

## [0.1] - 2025-12-24

Prototype bootstrap

### Added

- Launcher skeleton, build script, and CMake scaffold (`make`,
  `CMakeLists.txt`, `src/cmd/ny/main.c`).
- Runtime placeholders and smoke fixtures for a first compilable,
  testable tree.
