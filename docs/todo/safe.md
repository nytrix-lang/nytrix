<!-- nytrix-doc: {"audience":"user","featured":false,"group":"todo","order":90,"summary":"Open memory and resource safety work for Nytrix."} -->
# Safety roadmap

An item closes only when its acceptance check passes on the current tree.

## Done (2026-11-13)

Removed from this list because they are implemented and covered by fixtures
under `etc/tests/lang/memory/`; see the [safety guide](../learn/safety.md).

- Runtime use-after-free detector for user programs, heap quarantine, runtime
  out-of-bounds traps on the program's own heap blocks, double-free reports,
  and leak detection: `--heap=safe` (`safe-heap-traps`).
- A checked out-of-memory path to exercise: user allocators return `0` and
  `NYTRIX_FAIL_ALLOC_AFTER=N` injects the failure (`allocation-failure-injection`).
- The safety user guide (`docs/learn/safety.md`) and the status-table entry.

Bugs fixed on the way: `free(p)` released the variadic argument list instead of
`p` on the native route, so freed user blocks were never freed; `str.chr`
halved odd code points.

## Baseline

- Ownership and borrow checks are opt-in (`--borrow-check`,
  `--ownership-check=strict`, `--heap=raii`, `--mode=safe`).
- The checker covers documented local shapes; dynamic indices, global
  provenance, and interprocedural summaries are not inferred.
- There is no static use-after-free analysis.
- AddressSanitizer and UBSan sanitize the compiler (`./make asan`), not user
  programs.

## Static memory safety

- [ ] **Verify `--mode=safe` on every route.** It promotes ownership
  diagnostics to errors; confirm borrow escapes, use-after-move, double
  release, and conflicting borrows are rejected on LLVM, native NYIR, and Wasm.
  Accept: a negative fixture per shape under `etc/tests/lang/ownership/`.
- [ ] **Static use-after-free analysis.** Track allocation, release, and last
  use of raw pointers through local and function-boundary flow, reusing the
  alias/place analysis. Accept: negative fixtures for use after free, dangling
  borrow, and use after release.
- [ ] **Aggregate and alias tracking.** Extend field-sensitive move analysis to
  dynamic aggregates, dictionaries, and lists. Accept: a `linear` field moved
  out of a struct, then the struct used, is rejected.
- [ ] **Interprocedural ownership summaries.** Summarize `@consumes`,
  `@releases`, `@returns_owned` across calls. Accept: a callee that
  `@consumes(x)` prevents the caller from using `x` afterwards.
- [ ] **Optional lifetime annotations.** Explicit region syntax for borrows the
  checker cannot infer. Accept: a function returning a borrow tied to an input
  compiles under `--ownership-check=strict`.

## Runtime memory safety

- [ ] **Make `--mode=safe` use the safe heap.** Decide whether `--mode=safe`
  implies `--heap=safe`, and document the cost. Accept: an out-of-bounds
  `store8` past a `malloc` size traps under `--mode=safe` without extra flags.
- [ ] **Compile-time and runtime range checks together.** Extend
  `assert_compile_range`, `load8`, and `store8` checks to dynamic ranges on
  memory the safe heap does not own (stack buffers, FFI buffers).
- [ ] **Checked OOM in containers.** `set` and `dict` growth under
  `NYTRIX_FAIL_ALLOC_AFTER` must run cleanup (`defer`/`with`) and report out of
  memory, not crash. Accept: a fixture that grows a dict past the injected
  limit and observes a clean error.

## Allocation failure

- [ ] **Audit every allocation site.** Catalog each `malloc`/`__malloc`/
  `calloc_raw`/`realloc` call in `lib/` and `src/` as checks-and-panics,
  returns-null-to-caller, or unchecked, starting with `lib/set.ny`, `lib/mem.ny`,
  `lib/dict.ny`, `lib/alloc.ny`, `src/code/lang/pipeline/vm.c`, and
  `src/code/lang/types/typeinfer.c`.
- [ ] **One consistent convention.** The user-facing contract is "return `0`,
  caller checks" (documented in the safety guide); make the stdlib and compiler
  follow it, or document each exception.

## Sanitizers for user programs

- [ ] **AddressSanitizer lane.** `ny --sanitize=address file.ny` instruments
  the emitted program (stack and global checks that the safe heap cannot give).
- [ ] **UBSan, MSan, TSan lanes.** Accept: `ny --sanitize=ub` traps on signed
  overflow and null dereference in emitted code.

## Defaults and ergonomics

- [ ] **Decide the default mode.** Whether `--mode=safe` becomes the default for
  new projects, with `--mode=fast` opting out; publish the cost of each mode.
- [ ] **RAII as the default reclamation policy** for resource handles. Accept:
  `with ptr p = malloc(8)` releases `p` on every exit path, including panic.

## Verification

- [ ] **Memory-safety proof obligations.** Connect refinement and proof types to
  allocation size, index range, and release-once. Accept: a `requires`/`ensures`
  contract on a raw-memory function is checked by the solver.
- [ ] **Do not overclaim.** Nytrix is not a general formal verifier; unsafe FFI,
  raw casts, and `forget` stay explicit escape hatches.

## Testing and fuzzing

- [ ] **Finish the memory-safety fixture suite.** Add dangling-borrow and
  use-after-move fixtures, and run the suite on LLVM JIT, native NYIR, and
  `--mode=safe`.
- [ ] **Fuzz under sanitizers.** Extend `./make fuzz` to run user programs under
  the safe heap and the future sanitizer lanes.
- [ ] **Keep `./make asan` green** for the compiler.

## Non-goals

- Memory safety without annotations (needs a different language design).
- Replacing manual memory management in performance-critical code; `manual`
  and `forget` remain supported.
- Safety across unsafe FFI or raw pointer casts by default.

## Related

- [Safety guide](../learn/safety.md)
- [Ownership spec](../spec/ownership.md)
- [Runtime/heap policy](../spec/runtime.md#heap-policy)
- [Memory spec](../spec/memory.md)
- [Status and execution routes](../learn/status.md)
