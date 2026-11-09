<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":3,"summary":"Open compiler, performance, UI, test, and cleanup work with acceptance criteria."} -->
# Open work

An item closes only when its acceptance check has passed on the current tree.
Figures are from 2026-11-09 and are a starting point, not a current baseline.
NYC work (all NYC notes/todos) is tracked in [NYC work](todo/nyc.md).

Working rules:

- Compare generated code by instructions executed, then read the assembly.
- A change counts only if checksums still match and the suite stays green.
- Reproduce one benchmark or fixture at a time before changing anything.

## Correctness

- [ ] **Intermittent crashes under load:** `ui_replay_animation` segfaulted
  (exit 139) and `generated-mutable-flow-differential` aborted with "double
  free or corruption" in full runs. Both pass alone, and 84 concurrent cold runs
  of the second did not reproduce it. Lead: `gc.c` drops the GC lock to run
  finalizers during a collection. Accept: reproduced, root-caused, fixed.
- [ ] **Same address flaw in Nytrix code:** run the suite and benchmarks against
  the fix and look for a language-level reproducer. With the fix reverted, all
  71 NYR benchmarks still match gcc. Lead: `object.c` seeds rematerialization by
  vreg from entry-block LEAs without a unique reaching definition. Accept:
  reduced regression fixture added,
  or the path shown unreachable.
- [ ] **Boxed float builtin ABI:** builtins declared `f64` (`__flt_pow`, `__flt_sin`,
  and others) return a boxed float in `rax` while callers read `xmm0`. It works
  only because `xmm0` still holds the result. Changing them to return `any` broke
  `render-camera-contract`. Accept: one contract on every route, suite green.
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
- [ ] **Bigint workloads:** `pidigits` runs at 3.3x, and `bigint-oracle` has no
  reference here without GMP. `currying` is settled: its budget records that C folds
  the call. Decide whether to speed up bigint or compare an equivalent workload.
- [ ] **Share C-only wins with NYR:** copy coalescing and known-bits simplification
  now run for proven-raw values. Extend them to more of language code, since only
  `cmov-sort` gained (8.2%). Accept: suite green, instruction counts improve.
- [ ] **Whole-function register allocation:** C has live-range splitting around
  call-free loops. Port it to NYR and find why loop registers conflict with
  ordinary values in `rsi`/`rdi` on C.
- [ ] **`rax` staging:** spilled right-hand operands are done. Other arithmetic
  still moves through `rax`; use three-register forms.
- [ ] **Stack-slot traffic:** about 12% loads and 8% stores remain.
- [ ] **Encoder waste:** homing at block boundaries, and C parameter stores and
  leaf pushes for NYR. Scalar AVX copies and `jcc` over `jmp` are done.
- [ ] **Backend API:** modularize and compact the x86-64 encoder and backend
  interfaces after the performance work settles. Accept: suite and benchmark
  gate unchanged.

## Compile time

- [ ] **AOT builds:** pong takes 55 s since the duplicate LLVM pass was removed
  (was 90). Target: under 30 s. Reachable-function collection (10 s) and `rt_main`
  lowering (13 s) are the largest stages.
- [ ] **Known hot spots:** imported module member
  lookup, `tp_collect_call_observation`, `hm_complete_leaf_fact`,
  `nyr_cfg_build_impl`, `nyr_type_map_init`.

## Startup

- [ ] **First frame:** AOT pong takes about 6.3 s to its first frame. Bake fonts
  and other assets at init or build time and remove frame spikes.

## Audio formats

Reference: [dr_libs](https://github.com/mackron/dr_libs) (`dr_wav`, `dr_flac`,
`dr_mp3`). Native decoders live in `lib/os/sound/res.ny`; each needs fixtures
checked against reference output.

- [ ] **AIFF `ima4`:** PCM, float, `sowt`, A-law, and mu-law match `ffmpeg`.
- [ ] **AU encodings:** 1-7 and 27 are done; the rest (ADPCM variants) remain.
- [ ] **Native FLAC decoder:** fixed and LPC subframes, stereo decorrelation,
  metadata blocks, CRC checks, seek tables, and Ogg-encapsulated FLAC.
- [ ] **Native MP3 decoder:** MPEG-1/2/2.5 Layer I-III, ID3v1/ID3v2 skipping,
  Xing/VBRI/LAME headers for gapless length, and seeking.
- [ ] **Streaming API:** decode by PCM frame range without loading the whole
  file, with seek and exact length. Accept: frame-accurate against `dr_libs`.

## UI and engine

- [ ] **Audit:** read `etc/projects/ui/engine.ny`, `editor.ny`, and `lib/os/ui`
  end to end and record defects here.
- [ ] **Engine text garbling** on real Wayland.
- [ ] **Left panel clipping.**
- [ ] **Polish:** gizmos, menus, and overall editor quality.
- [ ] **Resize smoothness:** no flicker or stalls while dragging. Accept:
  headless resize stress plus frame-time logs.
- [ ] **Modernize the UI system** after the audit identifies what to replace.
- [ ] **TTY/TUI backend:** translate renderer programs to a terminal target.

## Tests and tooling

- [ ] **Faster `./make check`:** a full run takes about 16 minutes before the
  benchmark phase. Find the largest costs and cut them without losing coverage.

## Code quality

- [ ] **Modularize the C sources:** `src/code/lang/parse/c/parse.c` (48,000
  lines) and `src/code/backend/arch/x86_64/object.c` (15,000 lines) carry long
  near-duplicate runs. Coordinate with active NYC work (see [NYC work](todo/nyc.md)).
- [ ] **Repeated source:** the audit reports 61,853 repeated lines. Start with
  `lib/os/dis/decomp/core.ny`, `lower.c`, and the fuzz drivers.
