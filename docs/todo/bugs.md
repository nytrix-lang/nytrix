<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Open compiler correctness bugs with minimal reproductions."} -->
# Compiler bugs

Open correctness bugs found while working on other items. Each entry has a
reproduction; an item closes when its reproduction passes on every route and a
regression fixture covers it.

- [ ] **Native -O3 drops an editor backspace inside a longer function.**
  In `etc/tests/code/probes/ui/editor-utf8.nshape`, `case_backspace` ends
  with `bs("abc")`; on `--native-only -O3` the buffer stays `abc` (cursor 3)
  while `-O0` and the LLVM route give `ab` (cursor 2). The same calls in a
  small function are correct at -O3, so it needs the preceding statements.
  Reproduces with the 2026-11-18 05:16 compiler too. The fixture runs -O0 and
  LLVM until this is fixed.
- [ ] **cf-mem2reg -O3 miscompile.**
  `etc/tests/code/targets/x86_64/cf-mem2reg-multi-phi-x86-64.nshape` and
  `oracle-phi-elim-cond-merge-x86-64.nshape`: at `-O3 --native-enable-cf-mem2reg`
  the swap/increment loop gives 82 (VM and native agree) instead of 45; -O2 is
  correct. Present since at least the 2026-11-12 compiler. The final NYIR folds the
  whole loop to the constant 82. Disabling any one of `if_convert`,
  `select_chain_fold`, `loop_unroll_ssa` or `copy_prop` fixes it; stopping
  after the first run of each is still correct, so a later round produces it.
  `lu_try_one`'s per-iteration phi maps and exit values read the previous
  iteration's map (parallel, swap-safe) on inspection, so the next suspect is
  the select/copy rewriting of the if-converted swap. A per-pass IR dump would
  make this quick; none exists yet.
- [ ] **"value vN has no allocated spill home" in the NYIR x86-64 text
  emitter.** `o3-local-loop-unroll-x86-64`, `loop-optimizer-levels-x86-64`,
  `cfg-regalloc-split-x86-64` fail under `./make test` (the loop is
  vectorized there and `vec8.store.i64` reads an operand with no home in
  `machine64.c:ny_x64_nir_load`). Passes with the 05:16 compiler; the
  uncommitted `machine64.c` changes from 13:27 are the first suspect. Direct
  `ny -O3 --native-result-oracle=6` does not vectorize and passes, so the
  runner's target features matter for reproduction.
- [ ] **Remaining suite failures (unclassified, 2026-11-18 run):**
  `probes/cache/native-image-cache` (expected cache hit), `probes/cli/brainfuck-cli`,
  `intermediate/ny/bare/asm-artifacts`, `intermediate/ny/vm-disasm-artifact-diff`,
  `abi/calls/variadic-machine-object`, `bench/string/revcomp`,
  `probes/ui/ui-smoke:ui_replay_engine`.
- [ ] **Verify the Apple arm64 JIT data mapping on macOS.** Data sections in
  `gen/llvm/jit.c` (`ny_apple_jit_alloc_section`/`finalize`) are now plain RW
  pages (not `MAP_JIT`, never executable); only code keeps `MAP_JIT`. This
  path compiles only on macOS and has not been built or run there yet.
