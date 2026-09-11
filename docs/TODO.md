# TODO

Open work only. Keep regression fixtures after fixes. Do not weaken assertions
or classify a timeout as solved by extending its limit. Fixed work is recorded
in `docs/CHANGELOG.md`, not here.

Local test runs: use `NYTRIX_TEST_JOBS=32 ./make test` (96-core host); the
CI image stays at `NYTRIX_TEST_JOBS=1` for scheduler determinism.

Tracked set: 34 fixtures, 16 passing as of 2026-09-11 (evening). Suite:
862/887. Freshly passing this pass: `shift-promotion.ny`, `trace.ny`,
`io.ny` (dual-ABI diagnostics builtins), `strings.ny` (flaky, see GC
rooting below).

## Compiler and runtime

- [ ] **JIT-only miscompile: early return + conditional + tail local.**
  `fn f(any xs) any { if is_str(xs) { return "s" } mut out = list(0)
  if is_tuple(xs) { ... } out }` returns NY_IMM_FALSE (2) through the
  `-run` JIT while the AOT object path returns the list handle. Same
  NYIR input, so the defect is in the machine-form/JIT side of the
  pipeline; an opt-tier pass likely repairs the sequence under AOT.
  Reducers: `/tmp` shapes no longer exist — re-derive from
  `std.core.iter._iter_empty_like` (early return + mut local + untaken
  if + tail ident). Blocks `iter.ny` ("drop empty list").
- [ ] **Nested `.get(0, 0) > 2` compares the tagged payload.** Inside a
  callback body, `v.get(0, 0)` on an `any` receiver routes to
  `rt_value_get_tagged` and returns canonical tagged(1)=3, but the
  comparison lowers as a raw `cmp $0x2/setg`, so
  `[[1,2],[3,4]].filter(fn(v) { v.get(0, 0) > 2 })` keeps every element.
  The int-literal default types the expression raw; the decode must land
  at whichever of the several `get` emission sites this shape actually
  takes (a semantic-gated decode in the free-`get` handler did not fire).
  Blocks `collections.ny` ("filter preserves nested list values").
- [ ] **`flatten_inline` zeroes scalar leaves.**
  `fn flatten_inline(l) { is_list(l) ? mapcat(flatten_inline, l) : [l] }`
  returns a list of the right length with raw-pointer/zero elements; the
  ternary arm's list literal `[l]` stores its element pre-tagged for
  some shapes (tail-position `[l]` and def-bound reads are correct, so
  the corruption follows the recursion/ternary combination, not the
  literal construction itself). Blocks `errors.ny` ("inline
  function-body flatten should preserve scalar leaves").
- [ ] **GC rooting for dynamic descriptor slots.** Strings stored into
  24-byte descriptor tbufs read back empty or as raw addresses unless a
  print perturbs the heap (`swapped`/`_char_list_to_str` crash in
  `strings.ny`). Repro: `_char_list_to_str` after `_sorted_list_copy` +
  `swap_items`. Flaky: `strings.ny` passed the 2026-09-11 evening suite
  run and segfaulted solo minutes later.
- [ ] **Dict string keys alias through `to_str` buffers in loops.**
  Reproducer (the independent checksum the perf gate asked for):
  `mut d = dict(); while i < n { d.set(to_str(i), i); i += 1 }` then a
  second loop of `d.get(to_str(j), -1) != j` misses exactly n/2+1 keys
  (51 at n=100, 5001 at n=10000, `d.len` reports n). Def-bound keys
  (`def k = to_str(5); d.get(k, -1)`) and inline literal keys always
  hit; only the loop-shaped inline call key misses. The set/get
  symbol-selection helpers now classify str-typed call results as
  string keys, which was necessary but not sufficient — the loop shape
  still reaches an i64-key path somewhere. Explains the `dict` bench
  MISMATCH (90674|90674|1249975000): native/LLVM agree on a wrong
  checksum, C computes the true sum.
- [ ] **E1010 typer-resolution storm on the strict source path.** 83
  "local call: expected callable, got list<int>/dict<...>" errors when
  the stdlib compiles from source (`NYTRIX_STD_CACHE=0`): a call whose
  callee name resolves to a container *value* (`fn list(int cap=8) list`
  in `lib/core/mod.ny:861`; `lib/math/vector.ny:252/446/560` blocks
  `type.ny`). Fix the callee-first resolution for `name(` when the name
  is also a value, or align the cache-miss path with the bundle
  validation mode.
- [ ] **bigint tonelli_shanks returns a nil-ish handle on the p ≡ 1
  (mod 4) path.** `tonelli_shanks(Z(4), 17).str` prints empty (free
  function and `Z(4).sqrt_mod(17)` method alike), so the earlier
  method-boundary suspicion is wrong — `Z(2).pow_int(8)` and other
  methods with `any` params pass. 17 ≡ 1 (mod 4), so the failing path
  is the full Tonelli loop (the `p_mod4 == 1` branch: the
  `while true` / `M - i - 1` section ending in `return R`) or the
  return-boundary boxing of `R`; the strict-source HM check flags
  return-type cliffs in this module. Probe the p ≡ 3 quick path
  (`sqrt_mod(4, 7)`) to separate the halves, then trace
  `legendre`/`power_mod` handles inside the loop. Covers "modular
  sqrt method" and, with powmod downstream, "large Barrett powmod" in
  `bigint.ny` / `bigint-large-reduction.nshape`.
- [ ] **`x11-utf8` — "utf8 euro decode".** `decodeUTF8(euro, 0)` returns
  `[0, 0]`, the signature of an entry guard firing: `!is_str(s)` or
  `start >= n` with a zero-seeded slot. Everything upstream is correct
  (`euro.len == 3`, the three bytes, and `euro == chr(0x20ac)` all
  pass), so the module-qualified call `xc.decodeUTF8(euro, 0)` seeds
  one of the `any` param's (value, len, tag) slots with 0. The
  `--native-only` dumper cannot resolve the x11 module today; dump the
  harness-path lowering another way.
- [ ] **`extensible.ny` — runtime/comptime expansion contract.** Compare
  AST serialization after parse, comptime expansion, and NYIR lowering
  for the smallest declaration whose reflections differ.
- [ ] **`match.ny` — result-object render/eq crash.** First `match`
  guard now passes; `def x = ok(42); println(x)` renders `<ptr ...>`
  and the later `assert_eq` panic path dereferences a garbage C string
  inside `rt_alloc_string`. The result object's descriptor is not
  recognized by `rt_val_to_str_info`/`eq` on the native path.
- [ ] **`proof-logic.ny` — "solver exhaustion is explicit".**
- [ ] **Probe segfaults/timeouts: `remote-socket`, `interact-process`
  ("recv_until prompt"), `remote-process-recvuntil` ("sendline writes
  all bytes"), `render-camera-contract`, `ext`, `gltf-index-modes`,
  `import.ny` (flaky 20s timeout, passes in some full runs),
  `concurrency` ("10k stackless tasks"), `vec3-normalize`, `z3-bv`,
  `mt19937-smt` (all rc=139).** Phase-profile with
  `--prof --no-progress`; bound cold compile (`NYTRIX_STD_CACHE=0`)
  separately from warm execution.
- [ ] **Performance correctness gate.** Bench deltas from 2026-09-11:
  `dict` MISMATCH (see the string-key reproducer above),
  `escape-loop` REGRESS (10877x, confirmed allocation-bound: the loop
  allocates one list per iteration and the native engine keeps all 1M
  allocations — an SROA/escape-analysis gap, not a checksum bug),
  `fannkuch` 105x (marked ok, still slow). No optimization work until
  each has an independent checksum/crash reproducer and
  native/LLVM/JIT/C agree.
