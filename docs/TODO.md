# TODO

Open work only. Keep regression fixtures after fixes. Do not weaken assertions
or classify a timeout as solved by extending its limit. Fixed work is recorded
in `docs/CHANGELOG.md`, not here.

Local test runs: use `NYTRIX_TEST_JOBS=32 ./make test` (96-core host); the
CI image stays at `NYTRIX_TEST_JOBS=1` for scheduler determinism.

Focused correctness-suite command (results change as concurrent fixes land):

```bash
NYTRIX_TEST_JOBS=32 NYTRIX_TEST_NO_BENCH=1 NYTRIX_TRACE=1 NYTRIX_TRACE_CALLS=1 NYTRIX_TRACE_VALUES=1 NYTRIX_TRACE_VERBOSE=1 ./make test etc/tests/{runtime/{language/{extensible,type,proof-logic}.ny,execution/{errors,concurrency}.ny,values/{collections,iter}.ny,modules/import.ny},shapes/probes/sys/gltf-index-modes.nshape}
```

---

## Bug Group B — `any`-typed dict/nil handle corrupted at `len`/`get` boundary

**Remaining fixtures:** `proof-logic.ny`, `type.ny`.

### B4 — Premature constant folding of `mut` global list (blocks `proof-logic.ny`)

**Symptom:**
`PanicError: index_read out of range` in `_linear_predicate()` when calling `linear(constraints, bounds)`.

**Root cause isolated:**
In `src/code/native/lower.c:6818-6825` (`ny_native_nir_resolve_list_literal`):
```c
ny_native_nir_local_t *local = ny_native_nir_find_local(b, e->as.ident.name);
if (local && !local->semantic_mutable && local->list_literal)
  return local->list_literal;
const expr_t *v = ny_native_nir_find_top_level_value(b, e->as.ident.name);
if (v && v != e)
  return ny_native_nir_resolve_list_literal(b, v, depth + 1);
```
- For locals, `!local->semantic_mutable` is checked.
- For top-level values, `ny_native_nir_find_top_level_value` does NOT check if the declaration was mutable (`s->as.var.is_mut`).
- Global `mut _linear_constraints = []` is initialized as empty.
- `ny_native_nir_resolve_list_literal` resolves `_linear_constraints` to `[]` (len 0) and in `src/code/native/lower/lower_expr.h:1718` emits a hardcoded static bounds check against len 0: `rt_bounds_check(offset, 0)`.
- When `linear()` mutates `_linear_constraints = constraints` at runtime with 10 elements, indexing in `_linear_predicate()` triggers `PanicError: index_read out of range` on the hardcoded 0-element bounds check.

**Action Blueprint:**
In `src/code/native/lower.c:6822-6825`:
Verify that top-level variable declarations are immutable before folding:
```c
const stmt_t *top_stmt = ny_native_nir_find_top_level_stmt(b, e->as.ident.name);
if (top_stmt && top_stmt->kind == NY_S_VAR && top_stmt->as.var.is_mut)
  return NULL;
```

### B5 — Variadic argument packing missing in native NYIR `lower_call.h` (blocks `type.ny`)

**Root cause isolated:**
In native NYIR, a function declared with variadic parameters `fn foo(...args)` has its variadic parameter compiled as a list in `src/code/native/lower/lower_stmt.h:3039`:
```c
bool is_list = ... (fn->as.fn.is_variadic && i == fn->as.fn.params.len - 1);
```
Inside the callee, `args` expects the standard list 3-tuple ABI `(value_ptr, len, tag)` in `(rdi, rsi, rdx)`.

However, call-site lowering in `src/code/native/lower/lower_call.h` completely ignores `callee_fn->as.fn.is_variadic`:
1. It does NOT pack variadic arguments into a list. Instead, it emits each passed argument as a separate register/stack argument.
2. In `vec.Vector3(1.0, 2.0, 3.0)`:
   - Caller places `1.0` in `rdi`, `rt_len(1.0) = 0` in `rsi`, `rt_value_tag(1.0) = 110` in `rdx`, and `2.0`, `3.0` in subsequent registers.
   - Inside `Vector3`, `args` interprets `(rdi, rsi, rdx)` as its list handle. Since `rsi = 0`, `args.len` is 0.
   - `args.get(0, 0.0)`, `args.get(1, nil)`, `args.get(2, nil)` all fail the length check and evaluate to default `0.0`.
   - As a result, `Vector3(1.0, 2.0, 3.0)` and `Vector3(4.0, 5.0, 6.0)` both silently construct vectors with coordinates `(0.0, 0.0, 0.0)`.
3. In `type.ny:699`:
   `def ratio = b / a` divides `0.0 / 0.0`, triggering `ZeroDivisionError: division by zero` in `rt_flt_div`.
4. In calls with integer arguments like `foo(1, 2, 3)`:
   Caller attempts to compute `rt_len(1)` for `rsi`, which triggers `PanicError: len expects a sequence, got int` (directly linking to Bug Group B4).

**Minimal repro:**
```ny
fn foo(...args) {
   print("args len:", args.len)
   print("args get 0:", args.get(0, -1))
}
foo(10, 20, 30)
```
```bash
build/release/ny-full --no-progress -e '...'
# → args len: 0 (or PanicError on integer sequences)
```

**Comparison with legacy LLVM compiler:**
In `src/code/native/llvm/legacy/gencall/init.c:2147-2175`, legacy compiler explicitly packs variadic arguments:
```c
} else if (has_sig && is_variadic && !native_variadic &&
           i == (size_t)sig_arity - 1) {
  // Allocates list with __list_new(var_count)
  // Loops over trailing arguments storing each into the list with __store64_idx
  // Passes the constructed list as the final parameter
}
```

**Current status:** Trailing arguments are packed into a descriptor list and
the variadic callee receives the correct pointer and length. The remaining
failure is a float representation/ABI mismatch after `args.get`: emitted IR
loads f64 bits from an i64 local, then calls `rt_f64_bits(i64 %value)` even
though that C function accepts `double`. A reduced `Vector3(1.0, 2.0, 3.0)`
currently prints `(1, 1, 1)`. Fix float register representation through
locals and call lowering; do not add a vector-specific compiler exception.

---

---

## Bug Group E — Syntax registry macro integer representation mismatch

**Fixture:** `extensible.ny` (rc=134)
**Symptom:** `Nytrix assertion failed: merge_registry_in with overwrite should replace existing handlers`.

**Root cause isolated:**
In `__macro_double_plus1` with `x = 5`, the expression evaluated is `x + x + 1`:
1. `x + x` (dynamic + dynamic): `src/code/native/lower/lower_arith.h:1032` matches `(left_any || right_any)` and emits `rt_any_add(a, rhs)`. `rt_any_add` returns a tagged dynamic integer: `rt_tag_v(10) = 21`.
2. `(x + x) + 1` (dynamic + raw int literal 1): `lower_arith.h:1014-1023` matches `e->as.binary.right->semantic.rep == NY_SEM_REP_RAW_INT`. It unboxes `(x + x)` via `rt_any_to_i64(21) = 10` and emits `nyir_emit(NYIR_ADD_I64, 10, 1) = 11`. It returns a raw machine `i64`.
3. The macro function `__macro_double_plus1` has an untyped signature and returns raw `11`.
4. The macro expander / caller expects an untyped return to be a dynamic tagged value. It untags the return value: `11 >> 1 = 5`!
5. Assert expects `11`, but receives `5`, failing the overwrite test.

**Action Blueprint:**
In `src/code/native/lower/lower_arith.h:1014-1024`:
When the enclosing function return or expression context is dynamic/untyped, box/tag the result of `NYIR_ADD_I64` via `rt_tag_v` / `rt_tag`, or ensure all dynamic arithmetic paths return uniformly tagged representations.

---

## Bug Group F — Remaining Correctness Test Suite Issues

### F1 — Zero-capture lambda dynamic argument unboxing (blocks `collections.ny` & `iter.ny`)

**Symptom:**
`assert([1, 2, 3].map(fn(v) { v + 1 }) == [2, 3, 4], "list map method")` returns `[4, 6, 8]`.

**Root cause isolated:**
ABI mismatch between caller indirect dispatch and callee zero-capture lambda entry:
1. **Caller:** In `iter.ny:map(xs, fn1)`, `fn1(xs[i])` is an indirect callable. In `src/code/native/lower/lower_call.h:6302`:
   `bool raw_scalar_parameter = callee_fn && !indirect_callable && ...;`
   Because `indirect_callable` is true, `raw_scalar_parameter` evaluates to `false`. The caller does NOT unbox `xs[i]` and passes tagged dynamic integer `3` (representing 1).
2. **Callee:** In `src/code/native/lower/lower_stmt.h:3137-3143`:
   ```c
   if (lambda_entry && lambda_entry->capture_count > 0 && fn->as.fn.name &&
       strncmp(fn->as.fn.name, "__ny_lambda_", 12) == 0 &&
       !param->is_any && !param->is_list && !param->is_cstr &&
       !param->is_f64 && !param->is_f32) {
     arg_val = ny_native_nir_emit_runtime_call(
         &b, "rt_any_to_i64", arg_val, -1, -1, 1, 0);
   }
   ```
   Because `lambda_entry->capture_count == 0` for `fn(v) { v + 1 }`, this check evaluates to `false`!
   The callee does NOT unbox `v` either.
3. `v` remains tagged as `3`. The lambda executes `3 + 1 = 4`, `5 + 1 = 6`, `7 + 1 = 8`, producing `[4, 6, 8]`.

**Action Blueprint:**
In `src/code/native/lower/lower_stmt.h:3137`:
Unbox untyped/scalar parameters with `rt_any_to_i64` for all anonymous lambdas (`__ny_lambda_`) regardless of `capture_count`, or when called through dynamic function pointers.

---

### F2 — Double-tagging of fallback in `rt_value_get_tagged` (blocks `errors.ny`)

**Symptom:**
`assert(get(any_id(raw_probe), 0, 77) == 77)` fails with assertion error.

**Root cause isolated:**
In `src/code/runtime/core.c:2510-2511` and `2517-2518`:
```c
int64_t rt_value_get_tagged(int64_t value, int64_t key, int64_t fallback) {
  ...
  if (rt_raw_ptr_registered(value))
    return rt_tag_v(fallback);
  ...
  if (!value)
    return rt_tag_v(fallback);
```
In `rt_value_get_tagged`, `fallback` is already passed in tagged dynamic representation (e.g. `77` is passed as `(77 << 1) | 1 = 155`).
Lines 2511 and 2518 erroneously call `rt_tag_v(fallback)` again, tagging `155` into `311` (`77` becomes corrupted).
All other return points in this function (lines 2503, 2515, 2530) return `fallback` directly without double-tagging.

**Action Blueprint:**
In `src/code/runtime/core.c`:
Change line 2511 from `return rt_tag_v(fallback);` to `return fallback;`.
Change line 2518 from `return rt_tag_v(fallback);` to `return fallback;`.

---

### F3 — Unresolved JIT Runtime Symbols Emitting `call 0x0` / SIGSEGV (blocks `import.ny`)

**Symptom:**
`import.ny` crashes with `SegmentationFault: signal 11` at address `0x0000000000000000` (`movabs rax, 0x0; call rax`).

**Root cause isolated:**
In `import.ny:48`: `OS.len > 0` lowers via `ny_native_nir_emit_runtime_call(b, "rt_len_strict", sequence)`.
However, `rt_len_strict` is absent from `src/code/runtime/defs.h` and has no `LLVMAddSymbol` entry in `src/code/native/llvm/jit.c`.
Because it is not registered in the JIT symbol map, LLVM MCJIT leaves its function address as `0x0`, emitting:
```asm
movabs rax, 0x0
call   rax
```
Passing string `"linux"` in `rdi` to address `0x0` triggers immediate SIGSEGV.

**Full Audit of Missing JIT Symbols:**
A comprehensive scan of all runtime symbols emitted by `ny_native_nir_emit_runtime_call` across `src/code/native/` identified **21 runtime symbols** missing from `defs.h` and `jit.c`:
1. `rt_len_strict` (crashes `OS.len` in `import.ny:48`)
2. `rt_adt_alloc`
3. `rt_adt_tag`
4. `rt_alloc_string` (only legacy `__alloc_string` was registered)
5. `rt_any_to_cstr`
6. `rt_any_to_f64`
7. `rt_assert_cstr`
8. `rt_contains_raw`
9. `rt_cstr_cmp`
10. `rt_cstr_concat`
11. `rt_cstr_eq`
12. `rt_f64_to_cstr_raw`
13. `rt_fmod_f64`
14. `rt_getlogin`
15. `rt_gettimeofday`
16. `rt_i64_to_cstr_raw`
17. `rt_raw_word_tag`
18. `rt_tbuf_dyn_elem`
19. `rt_tbuf_extend`
20. `rt_tbuf_repeat`
21. `_setjmp`

Additionally, `import.ny:46` defines `fn flag_from(vals)` which shadows the module-local helper in `std.os.args`. Top-level symbol export must preserve module qualification for imported stdlib helpers.

**Action Blueprint:**
1. In `src/code/native/llvm/jit.c`: Add `LLVMAddSymbol` entries for all 21 missing runtime symbols in `ny_jit_add_runtime_symbols()`.
2. In `src/code/runtime/defs.h`: Add matching `RT_DEF` entries so interpreter and JIT share identical symbol resolution.

---

## Cross-cutting: compiler invariant / assertion hardening

- [ ] **Compiler invariant/assertion hardening before fuzz/deploy runs.** Add
  always-on, actionable assertions at parser/AST ownership boundaries,
  semantic-resolution output, HM representation changes, NYIR instruction
  construction/CFG joins, native ABI argument and return shapes, runtime
  handle validation, and backend emission. Every assertion should report the
  source span, function/module, invariant name, and the relevant type/rep/ABI
  state; convert recoverable compiler inconsistencies into structured
  diagnostics instead of crashes or silent zero/nil values. Add a dedicated
  compiler-assertion test matrix covering malformed ASTs, impossible semantic
  reps, invalid NYIR operands/labels, mismatched call arity, stale handles,
  and divergent interpreter/LLVM/JIT/native results.

---

## Benchmark Performance Blueprint — Crushing C & LLVM Across All Benchmarks

### 1. Benchmark Reality & Progress Log

Recent optimizations completed:
- **PERF-2 (Arraytab Interning):** Constant list literals pooled in `.data`, eliminating runtime construction.
- **PERF-3 (Inlined Bounds Checks in LLVM Emitter):** Replaced opaque external `rt_bounds_check` calls with inline `icmp ult` + cold branch to fail block. Unblocked LLVM LoopVectorizer/LICM. `sor` dropped from 52x to **4.5x**, `sha256` dropped from 139x to **2.2x - 3.8x**!
- **`i64buf_new` alloc_fact propagation:** Fixed missing fold of `const_count` in `lower_call.h`. `linpack` dropped from 48x to **2.5x - 5.7x**, `matrix` dropped from 799x to **2.7x - 6.0x**!

#### Current Benchmark Ratios (O3 peak profile, 62 fixtures):
| Benchmark | Baseline Ratio | Current Ny native | Current Ny LLVM | C (host) | Current Ratio vs C | Status / Key Mechanism |
|-----------|----------------|-------------------|-----------------|----------|--------------------|------------------------|
| `loop-unswitch` | 0.07x | 139µs | 115µs | 2.09ms | **0.06x** | **16x FASTER than C** |
| `mixed-static-island` | 0.66x | 19µs | 17µs | 31µs | **0.56x** | **Beats C** |
| `sieve` | 0.91x | 89µs | 79µs | 69µs | **1.14x** | Near parity with C |
| `vector` | 534.73x | 471µs | 470µs | 305µs | **1.54x** | Massive win (was 534x!) |
| `dgemm` | ~2.5x | 413µs | 414µs | 259µs | **1.59x** | Near C |
| `intops` | ~2.0x | 923µs | 903µs | 534µs | **1.69x** | Near C |
| `n-body` | ~2.2x | 3.93ms | 3.92ms | 2.29ms | **1.72x** | Near C |
| `binary` | ~2.5x | 3.11ms | 3.08ms | 1.63ms | **1.89x** | Near C |
| `nqueens` | ~2.5x | 73.2ms | 71.8ms | 37.9ms | **1.90x** | Near C |
| `spectral` | 1.28x | 101µs | 101µs | 50µs | **2.01x** | Near C |
| `mandelbrot` | ~2.5x | 491µs | 493µs | 241µs | **2.04x** | Near C |
| `calls` | ~2.5x | 229µs | 227µs | 103µs | **2.20x** | Near C |
| `splay` | ~3.0x | 8.34ms | 8.49ms | 3.66ms | **2.28x** | Near C |
| `fft` | ~3.0x | 60µs | 51µs | 22µs | **2.38x** | Near C |
| `json-parser` | ~3.5x | 42.4ms | 41.7ms | 16.9ms | **2.46x** | Fast JSON tokenizer |
| `sha256` | 139.77x | 3.50ms | 3.51ms | 915µs | **2.26x - 3.83x** | Huge win (was 139x!) |
| `linpack` | 48.09x | 460µs | 829µs | 98µs - 182µs | **2.53x - 5.72x** | Huge win (was 48x!) |
| `matrix` | 799.14x | 130µs | 130µs | 22µs - 49µs | **2.68x - 6.00x** | Huge win (was 799x!) |
| `havlak` | 32.77x | 6.77ms | 6.79ms | 2.40ms | **2.82x** | Huge win (was 32x!) |
| `fannkuch` | 104.06x | 726ms | 725ms | 209ms - 216ms | **3.35x - 3.65x** | Huge win (was 104x!) |
| `cmov-sort` | 978.90x | 3.32ms | 3.14ms | 949µs - 1.02ms | **3.31x - 4.26x** | Huge win (was 978x!) |
| `heapsort` | 420.87x | 19.1ms | 19.1ms | 4.84ms | **3.79x - 3.99x** | Huge win (was 420x!) |
| `sor` | 52.59x | 3.76ms | 3.76ms | 663µs - 822µs | **4.58x - 5.68x** | Huge win (was 52x!) |
| `revcomp` | 589.16x | 183µs | 183µs | 12µs | **14.7x** | Down from 589x |
| `iter` | 188.39x | 10.9ms | 13.3ms | 325µs - 355µs | **31.9x - 37.5x** | Down from 188x |
| `list` | 272.81x | 3.14ms | 6.44ms | 78µs - 163µs | **19.3x - 58.5x** | Down from 272x |
| `pbkdf2` | 538.49x | 368ms | 371ms | 1.16ms | **318x - 342x** | **PRIMARY BOTTLENECK** |

---

### 2. Comprehensive Findings & Actionable Blueprints for Open Bottlenecks

#### Bottleneck 1: `pbkdf2` (318x vs C) — Inner-Loop Buffer Churn & Dynamic Probing

- **Benchmark:** `etc/tests/bench/pbkdf2.nshape` (RFC 2898 PBKDF2-HMAC-SHA256, 1000 iterations).
- **Runtime:** Ny 368ms vs C 1.16ms.
- **Root Causes Discovered:**
  1. **Allocation in inner loop (`etc/tests/bench/pbkdf2.nshape:58`):**
     ```ny
     while off < padded {
        def w = zeros(64)   ;; <--- ALLOCATES A NEW 64-ELEMENT BUFFER EVERY BLOCK!
        ...
     ```
     `zeros(64)` calls `i64buf_new(64)` -> `rt_tbuf_new_raw(64, 8)`.
     Each block invocation performs `calloc(1, 32 + 64*8)`, `rt_map_oracle_add`, and `rt_tbuf_register_handle`.
     Over 1000 PBKDF2 iterations with 2 SHA256 passes per HMAC round, this executes **thousands of heap allocations** in the hot loop. In C (`pbkdf2.c`), `uint32_t w[64]` is an unallocated stack array reused across all blocks.
  2. **`rt_any_to_i64` -> `rt_is_str` header readability probe overhead:**
     `perf record` shows ~4% of total runtime spent inside `rt_addr_mapped` and `rt_header_readable_cached` because dynamic dispatch checks string-ness by probing page tables.
  3. **Leaf inlining of `zeros()` and `rotr()`:**
     `rotr(int x, int n)` is a user-defined function. `nyir_func_is_inline_candidate` (in `src/code/ir/advanced.c:967`) restricts inlining. If `rotr` is not inlined, every round executes 64 function calls.

- **Action Blueprint for Next Agent:**
  - **Plan A (Loop Allocation Hoisting / SROA):** In `lower_stmt.h` / NYIR optimizer: if `def x = i64buf_new(const_N)` occurs inside a `while` loop, and `x` does not escape the loop iteration (all stores/loads are internal to the loop), hoist `x = i64buf_new(const_N)` to the loop pre-header and insert `memset(buf, 0, N * elem_size)` at the loop head.
  - **Plan B (Stack Alloca Promotion):** If an `i64buf_new(const_N)` has a compile-time constant size `<= 256` and never escapes the function, allocate it on the stack (`LLVMBuildAlloca` / native stack frame) with a stack-backed tbuf header (`RT_NATIVE_TBUF_HEADER = 32`), skipping `malloc`/`calloc` and the global `rt_tbuf_register_handle` hash map.
  - **Plan C (Inline `rotr`):** Ensure `rotr` is fully inlined into the caller so the existing peephole rule (`peephole.c:430`) recognizes `((x >> n) | (x << (32 - n))) & 0xFFFFFFFF` and lowers it directly to `NYIR_ROR32_I64` (`llvm.fshr.i64` -> x86 `ror` instruction).

---

#### Bottleneck 2: `list` (19x-58x vs C) & `iter` (31x-37x vs C) — Append Reallocation & Runtime Boundary

- **Benchmark:** `etc/tests/bench/list.nshape` (append 20,000 ints, sum), `iter.nshape` (pipeline map/reverse/chain).
- **Runtime:**
  - `list`: Ny 3.14ms - 5.28ms vs C 80µs - 163µs (19x - 58x gap).
  - `iter`: Ny 10.9ms - 13.3ms vs C 325µs - 355µs (33x - 37x gap).
- **Root Causes Discovered:**
  1. **20,000 external C calls:** `lst = append(lst, i)` in a loop calls `rt_tbuf_append_i64_raw` on every single iteration. Even when capacity exists, it crosses the JIT-to-C boundary, reads tbuf magic, reads count, reads elem_size, reads capacity, branches, writes, and returns.
  2. **Geometric realloc churn:** `mut lst = []` starts at capacity 4, then reallocs to 8, 16, 32, ... 32768 (~15 reallocations + `rt_tbuf_replace_handle` hash lookups). C allocates `malloc(20000 * 8)` once.
  3. **The LLVM Inlining Hazard (CRITICAL FINDING):**
     When we inlined `rt_tbuf_append_i64_raw` directly into LLVM IR via `inttoptr` GEPs without alias domain metadata, `sor` regressed from **3.76ms to 7.70ms** because LLVM's basic alias analysis assumed integer-derived pointers alias all loop memory, destroying loop vectorization across unrelated benchmarks.
- **Action Blueprint for Next Agent:**
  - **Plan A (Preallocation Optimization):** In AST lowering (`lower_stmt.h` / `lower_expr.h`), pattern-match:
    ```ny
    mut lst = []
    mut i = 0
    while i < N {
       lst = append(lst, ...)
       i += 1
    }
    ```
    When `N` is known or bounded, rewrite `mut lst = []` to `mut lst = list(N)` (using `rt_list_new_raw(N)`) so the buffer is pre-allocated with capacity `N`. This eliminates all 15 reallocations and memory moves.
  - **Plan B (Native x86_64 Emitter Inline Append):** Instead of LLVM (which has fragile TBAA requirements), inline the fast path in the **native x86_64 backend** (`machine/x86_64.c`):
    ```asm
    mov rcx, [r_buf - 24]       ; load count
    cmp rcx, [r_buf - 8]        ; cmp count, capacity
    jae .slow_realloc
    mov [r_buf + rcx*8], r_val  ; direct store element
    inc qword ptr [r_buf - 24]  ; bump count
    jmp .cont
    .slow_realloc:
    call rt_tbuf_append_i64_raw
    .cont:
    ```
    This avoids LLVM pointer aliasing problems entirely while giving C-level append speed in the native JIT tier.

---

#### Bottleneck 3: `fasta` Checksum Mismatch — Proved & Solved Root Cause

- **Fixture:** `etc/tests/bench/fasta.nshape`
- **Symptom:** `MISMATCH: 11500000250000|11500000250000|17937500250000` (Ny gives 11500000250000, C gives 17937500250000).
- **Proved Arithmetic Proof:**
  - `tab = [65, 67, 71, 84]`.
  - In Ny: `sum += tab.get(seed % 4)`
  - `tab` is an 8-byte element list literal. `rt_tbuf_index_read_raw` returns raw `int64_t` values: 65, 67, 71, 84.
  - The call site incorrectly routes the return through `rt_any_to_i64(val)`.
  - In `src/code/runtime/core.c:864`:
    ```c
    if (is_int(value)) return rt_untag_v(value);
    ```
  - Since 65, 67, 71 are odd numbers (`val & 1 == 1`), `is_int()` classifies them as tagged VM ints!
  - `rt_untag_v(val)` computes `val >> 1`:
    - `65 >> 1 = 32`
    - `67 >> 1 = 33`
    - `71 >> 1 = 35`
    - `84 >> 1` NOT shifted (84 is even, `84 & 1 == 0`, left untouched as 84).
  - Average element read: `(32 + 33 + 35 + 84) / 4 = 184 / 4 = 46`!
  - Total sum: `46 * 250,000 = 11,500,000`!
  - Checksum formula: `sum * 1000000 + N = 11500000 * 1000000 + 250000 = 11500000250000`!
  - **EXACT MATCH TO THE CORRUPTED NY OUTPUT!**
- **Action Blueprint for Next Agent:**
  In `src/code/native/lower/lower_call.h` (around `NY_E_MEMCALL` for `"get"`):
  When target is proven to be an unboxed list/tbuf (`elem_size == 8` or `is_list && !dynamic_elements`), emit `rt_tbuf_index_read_raw` and **DO NOT** emit `rt_any_to_i64` on the returned value. The value is already an unboxed machine integer.

---

#### Bottleneck 4: `dict` Checksum Mismatch — Proved & Solved Root Cause

- **Fixture:** `etc/tests/bench/dict.nshape`
- **Symptom:** Checksum mismatch (`-100003` vs C `1249975000`).
- **Root Cause:**
  In `dict.nshape`, `acc += d.get(to_str(i), 0)`.
  The loop addition is lowered through `rt_any_add(%acc, %v)`.
  `rt_any_add` tags its result. On each loop iteration, the tag bit is doubled: `(acc << 1) | 1`. After 50,000 iterations, the tag bit shifts into the sign bit and overflows to `-100003`.
- **Action Blueprint for Next Agent:**
  When `acc` is known integer, lower `+=` directly to `NYIR_ADD_I64` / LLVM `add i64`, completely bypassing `rt_any_add`. Ensure `dict.get(..., default_int)` unboxes the return value at the call site if the dictionary returns dynamic values.

---

#### Bottleneck 5: `gltf-index-modes.nshape` Compiler Budget Timeout (Bug Group A)

- **Fixture:** `etc/tests/shapes/probes/sys/gltf-index-modes.nshape` (exceeds 20s compile timeout).
- **Root Cause:**
  During AST lowering in `src/code/native/lower.c`, functions like `ny_native_nir_find_top_level_value_in_stmt` linearly scan `prog->body.data[i]` for every single identifier lookup.
  In glTF's large generated AST with hundreds of top-level definitions, every identifier lookup performs an $O(N)$ scan, making lowering $O(N^2)$ in statement count.
- **Action Blueprint for Next Agent:**
  At the entry of `ny_native_nir_lower_prog` in `src/code/native/lower.c`, build a flat hash table or symbol map of top-level names:
  `ht_set(&b->top_level_syms, name, stmt_expr)`.
  Replace the $O(N)$ loop with an $O(1)$ hash table lookup. This will speed up compilation of large programs by 10x-50x and comfortably bring `gltf-index-modes` within the 20s budget.

---

#### Bottleneck 6: Small String Optimization (SSO) & Swiss-Table Dicts (`dict`: 7x-9x vs C, `json-parser`: 2.5x vs C)

- **Problem:** `dict` runs 50,000 insertions of `to_str(i)`. It allocates 50,000 small heap strings (e.g. `"42"`, `"12345"`).
- **Action Blueprint for Next Agent:**
  1. **SSO (Small String Optimization):** In `src/code/runtime/core.c`: Strings `<= 14` bytes can be stored directly inside the 16-byte object word (1 byte length, 1 byte tag, 14 bytes payload). This completely eliminates `malloc()` for `to_str(0..50000)`.
  2. **Swiss-Table SIMD Hash Map:** Replace the bucket-linked dictionary in `core.c` with flat 16-byte metadata control bytes and SIMD SSE2 `_mm_cmpeq_epi8` probing.

---

#### Bottleneck 7: Repeated Call Sequences & Inline Caching (11,906 Instances in `lib/`)

- **Audit Findings from `ny-fmt --audit --audit-mode=smart lib/`:**
  - `3810 get > get > get` in `lib/core/query.ny`
  - `2409 get > int > get` in `lib/math/crypto/analysis/worldfreq.ny`
  - `1554 int > get > int` in `lib/math/crypto/block/stream/core.ny`
  - `1244 get > float > get` in `lib/math/crypto/factorization/classical/qs.ny`
  - `1165 float > get > float` in `lib/math/crypto/lattice/bkz.ny`
  - `444 store32 > store32 > store32` in `lib/math/crypto/factorization/classical/qs.ny`
  - `253 load32_f32 > load32_f32 > load32_f32` in `lib/math/parse/3d/gltf/animation.ny`
- **Root Problem:**
  Chained `.get()` or repeated `load32`/`store32` operations go through full dynamic dispatch and boundary type checking on every step.
- **Action Blueprint:**
  1. **Monomorphic Inline Caching (MIC):** In `src/code/native/lower/lower_call.h`: At call sites with shape-stable receivers, cache the resolved struct offset or dictionary index after the first lookup.
  2. **Memory Coalescing:** Coalesce consecutive `store32` / `load32` into single 64-bit or 128-bit vector moves (`movq` / `movaps`).

---

#### Bottleneck 8: Preallocated Lists vs Dynamic Growth Churn (191 Instances in `lib/`)

- **Audit Findings from `ny-fmt --audit --audit-mode=smart lib/`:**
  191 instances across `lib/` where a preallocated or empty list is iteratively filled using `out = append(out, value)` inside a known-bound loop:
  - `lib/core/syntax/syntax.ny:429`
  - `lib/core/syntax/type.ny:61, 179, 413`
  - `lib/core/counter.ny:31` (`append > append > append`)
- **Root Problem:**
  `append()` performs capacity checks, potential buffer reallocations, and creates unnecessary temporary slice handles instead of writing directly into preallocated storage.
- **Action Blueprint:**
  1. Compiler optimization: When `mut lst = list(N)` or `mut lst = []` is filled in a loop `while i < N`, transform `append(lst, v)` to direct indexed write `lst[i] = v` with `__list_set_len(lst, N)`.
  2. Fast path: Emit native inline assembly for `append` in `x64.c` that checks `cap > len` in 3 instructions without C runtime call overhead.

---

#### Bottleneck 9: Repeated Dynamic Dictionaries to Typed Struct Layouts (80 Instances)

- **Audit Findings:**
  80 locations in `lib/` construct record-like dynamic dictionaries with constant keys, e.g. `{"x": ..., "y": ...}`, `{"ok": true, "tag": ...}`.
- **Root Problem:**
  Allocating a hash table, hashing string keys, and probing buckets for 2-4 static fields is 20x slower than accessing a 16-byte or 32-byte C struct.
- **Action Blueprint:**
  Introduce implicit anonymous struct shapes in `src/code/typing/`: When dictionary literals have compile-time known string literal keys, lower them to fixed-stride typed buffer records with direct 8-byte field offsets.

---

#### Bottleneck 10: Type Signature Propagation (312 Untyped Params, 116 Missing Returns)

- **Audit Findings:**
  312 untyped parameters and 116 missing return annotations in `lib/` force lowering to assume `any` representation, injecting `rt_any_to_i64` unboxing and `rt_tag_v` dynamic tagging on every call.
- **Action Blueprint:**
  1. Run Hindley-Milner intra-procedural type inference in `src/code/typing/pipeline/hm.c` to infer concrete scalar types (`int`, `float`, `bool`) from usage within function bodies.
  2. Propagate inferred signatures into `ny_native_nir_find_user_function` to eliminate dynamic boxing trampolines.

---

## Open items (prioritised)

### A. Correctness & Test Suite Unblocking
- [ ] **[B5] `type.ny` variadic float ABI** — packing is implemented; fix f64 bit-pattern locals being passed to `rt_f64_bits` with an i64 argument, then verify vector components and the full fixture.
- [ ] **[E] `extensible.ny` macro integer return representation** — in `lower_arith.h:1014-1024`, box raw machine integer when returning from untyped macro context so `11` is not untagged to `5`.
- [ ] **[F1] `collections.ny` & `iter.ny` zero-capture lambda unboxing** — in `lower_stmt.h:3137`, unbox tagged dynamic integer arguments for zero-capture anonymous lambdas (`__ny_lambda_`).
- [ ] **[F3] `import.ny` missing JIT runtime symbols** — add `LLVMAddSymbol` entries in `jit.c` and `RT_DEF` entries in `defs.h` for all 21 missing runtime functions (`rt_len_strict`, `rt_adt_alloc`, `rt_getlogin`, etc.), eliminating `call 0x0` / SIGSEGV.

### B. Benchmark Integrity & Correctness Fixes
- [ ] **[BENCH-CORRECT-1] `fasta` Checksum Mismatch Fix** — in `lower_call.h` / `lower_expr.h`, prevent `rt_any_to_i64` from running on unboxed element-8 list `.get()` reads (eliminates corrupting `65 >> 1 = 32`).
- [ ] **[BENCH-CORRECT-2] `dict` Checksum Mismatch Fix** — ensure `acc += d.get(...)` uses native integer addition (`NYIR_ADD_I64`), not `rt_any_add`.

### C. Performance Epics to 1000x the Language
- [ ] **[PERF-1] `pbkdf2` Buffer Allocation Hoisting / Stack Promotion (`alloca`)** — hoist `zeros(64)` out of the block loop or promote fixed-size non-escaping `i64buf_new` to stack `alloca`. Targets 100x speedup (<3ms).
- [ ] **[PERF-2] Monomorphic Inline Caching & Method Devirtualization** — cache receiver offsets for hot `get > get > get` chains (3,810 in `query.ny`) and monomorphic dictionary lookups.
- [ ] **[PERF-3] List Preallocation & Inline Append** — automatically transform `append` in bounded loops to direct index stores `out[i] = v` with `__list_set_len`, eliminating 191 growth churn patterns.
- [ ] **[PERF-4] Small String Optimization (SSO) & Swiss Table SIMD Probing** — store `<= 14` byte strings inline in 16-byte object handles; replace linked buckets with AVX2/SSE2 `_mm_cmpeq_epi8` control-byte probing.
- [ ] **[PERF-5] Top-Level Symbol Hash Table ($O(1)$ Lowering)** — replace $O(N)$ AST linear scans in `lower.c` with symbol map, speeding up compilation of glTF and large modules by 10x.
- [ ] **[PERF-6] Hardware Bitwise Idioms (`rotr`, `bswap`)** — lower `rotr` to `llvm.fshr.i32` (`ror`) and `bswap` to `llvm.bswap.i32`.
- [ ] **[PERF-7] Type Signature Propagation** — propagate HM inferred concrete types to eliminate boxing/unboxing overhead across 312 untyped params and 116 untyped returns.
