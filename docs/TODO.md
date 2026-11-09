<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":3,"summary":"Open compiler, performance, UI, test, and cleanup work with acceptance criteria."} -->
# Open work

An item closes only when its acceptance check has passed on the current tree.
Figures are from 2026-11-09 and are a starting point, not a current baseline.
NYC kernel work is tracked in [NYC Linux kernel acceptance](todo/nyc.md).

Working rules:

- Compare generated code by instructions executed, then read the assembly.
- A change counts only if checksums still match and the suite stays green.
- Reproduce one benchmark or fixture at a time before changing anything.

## Correctness

- [ ] **`fmod` in GCC builds:** a compiler built with GCC returns the divisor from
  `__flt_fmod` (`fmod(5.5, 2.0)` gives 2), which also fails `ui_replay_animation`
  there. Clang builds are correct. Accept: both compilers pass the fixture.
- [ ] **C system headers:** `signal.h`, `sys/time.h` timers, sockets, and `sys/uio.h` now work. Still failing: `netinet/tcp.h` (duplicate promoted member), `stdatomic.h` (not found), `complex.h` (`cacos` declaration).
- [ ] **Machine encoder: by-value aggregates and inline asm operands.** `nyr_x64_c_func_needs_direct_encoder` sends these C functions to the stack encoder because machine lowering has no SysV aggregate parameter/return support and drops inline asm operands. Implement both, then remove the fallback.
- [ ] **Intermittent crashes under load:** `ui_replay_animation` segfaulted
  (exit 139) and `generated-mutable-flow-differential` aborted with "double
  free or corruption" in full runs. Both pass alone, and 84 concurrent cold runs
  of the second did not reproduce it. Accept: reproduced, root-caused, fixed.
- [ ] **Same address flaw in Nytrix code:** run the suite and benchmarks against
  the fix and look for a language-level reproducer. With the fix reverted, all
  71 NYR benchmarks still match gcc. Accept: reduced regression fixture added,
  or the path shown unreachable.
- [ ] **Assembly output accuracy:** `--emit=asm` and `nyc -S` must describe the
  same code as the object path. `nyc -S` lacks 16/32-bit and unsigned machine
  forms and falls back to the stack encoder. Accept: both agree on the
  benchmark set.
- [ ] **Rendering path audit:** review the render APIs and modules for logic and
  compiler bugs with `-vvvvvv`, `--nyr-check=verify,oracle`, and
  `--native-only --inspect`. Accept: each defect recorded here with a
  reproducer, then fixed.

## Performance: targets

- [ ] **At most 1.5x of C on every backend:** no benchmark above 1.5x on NYR,
  NYC, or LLVM. Accept: the `./make bench` table, with the assembly of each
  outlier reviewed.
- [ ] **Beat C where the machine allows it:** after the 1.5x target, reach parity
  or better on each benchmark class. Accept: per-class geomeans at or below 1.0x.

## Performance: NYR

Last table, ratio to C. Over 1.5x: 18 entries.

- [ ] **Outliers to read in mixed assembly:** `licm` 10.6x, `knight` 6.9x,
  `cmov-sort` 6.5x, `collatz` 5.6x, `revcomp` 5.6x, `intops` 4.0x,
  `expression-tree` 3.8x, `native-oracle` 3.2x, `inline` 3.1x, `linpack` 3.0x,
  `queen` 2.8x, `matrix` 2.5x, `gcbench` 2.5x, `call-chain` 2.5x, `page-rank`
  2.4x, `pbkdf2` 2.4x, `knucleotide` 2.1x.
- [ ] **Cases where C folds the work or uses GMP:** `currying`, `pidigits`,
  `bigint-oracle`. Decide per benchmark whether to implement the folding and
  bigint speedups or to compare against an equivalent workload.
- [ ] **Share C-only wins with NYR:** copy coalescing, direct destination-register
  shifts, and known-bits simplification run only for C. Enable them for raw
  values in language code. Accept: suite green, instruction counts improve.

## Performance: NYC

Mean 2.06x of `gcc -O3 -march=native` in instructions over 73 benchmarks, down
from 3.32x. NYR is at 1.88x on the same measure. Timing table worst: `tce-recursion` 42x, `bce-matrix` 17x,
`revcomp` 14x, `dgemm` 12x, `cmov-sort` 11x.

- [ ] **Vectorization:** `linpack` and `bce-matrix`. `linpack` needs row dependence
  analysis for `A[r*N+c]` against `A[k*N+c]` with `r > k`.
- [ ] **Dead-code and loop elimination:** `dce` 33x, `tce-recursion`.
- [ ] **Pointer induction variables** for array walks.
- [ ] **Compiler option fallbacks:** PIC, retpolines, named sections, and
  `-fomit-frame-pointer` still use the stack encoder. These matter for kernel
  builds. Accept: same machine path, verified against the kernel configuration.

## Performance: backend

- [ ] **Whole-function register allocation:** C has live-range splitting around
  call-free loops. Port it to NYR and find why loop registers conflict with
  ordinary values in `rsi`/`rdi` on C.
- [ ] **`rax` staging:** loads now go straight to their register. Arithmetic still
  moves through `rax`; use three-register forms.
- [ ] **Stack-slot traffic:** about 12% loads and 8% stores remain.
- [ ] **Encoder waste:** `vmovaps` before three-operand AVX and homing at block
  boundaries. C parameter stores, leaf pushes, and `jcc` over `jmp` are done;
  port those to NYR.
- [ ] **Backend API:** modularize and compact the x86-64 encoder and backend
  interfaces after the performance work settles. Accept: suite and benchmark
  gate unchanged.

## Compile time

- [ ] **AOT builds:** pong takes about 90 seconds (was 126) and engine about 3
  minutes. Proposed target: under 30 seconds each.
- [ ] **Where the AOT time goes:** a plain native object build of pong takes 43
  seconds of that 90. CPU profiles over-count idle OpenMP workers, so measure
  the main thread or wall time per compiler phase before optimizing further.
- [ ] **Known hot spots:** `nir_value_range_at_cached`, imported module member
  lookup, `tp_collect_call_observation`, `hm_complete_leaf_fact`,
  `nyr_cfg_build_impl`, `nyr_type_map_init`.
- [ ] **`--run=aot` executable path:** make it fast and reliable.

## Startup

- [ ] **First frame:** AOT pong takes about 6.3 s to its first frame. Bake fonts
  and other assets at init or build time and remove frame spikes.

## UI and engine

- [ ] **Audit:** read `etc/projects/ui/engine.ny`, `editor.ny`, and `lib/os/ui`
  end to end and record defects here.
- [ ] **Engine text garbling** on real Wayland.
- [ ] **Left panel clipping** and the **tab strip**.
- [ ] **Polish:** gizmos, menus, and overall editor quality.
- [ ] **Resize smoothness:** no flicker or stalls while dragging. Accept:
  headless resize stress plus frame-time logs.
- [ ] **Frame profiling:** per-phase cost visible in the app.
- [ ] **Modernize the UI system** after the audit identifies what to replace.
- [ ] **TTY/TUI backend:** translate renderer programs to a terminal target.

## Tests and tooling

- [ ] **Faster `./make check`:** a full run takes about 16 minutes before the
  benchmark phase. Find the largest costs and cut them without losing coverage.
- [ ] **Test cleanup:** remove remaining duplicates and merge overlapping
  fixtures. Accept: coverage unchanged.

## Code quality

- [ ] **Modularize the C sources:** `src/code/lang/parse/c/parse.c` (48,000
  lines) and `src/code/backend/arch/x86_64/object.c` (15,000 lines) carry long
  near-duplicate runs. Coordinate with any session working on NYC first.
- [ ] **Repeated source:** the audit reports 61,000 repeated lines. Start with
  `lib/os/dis/decomp/core.ny`, `lower.c`, and the fuzz drivers.
