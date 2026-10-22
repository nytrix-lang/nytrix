<!-- nytrix-doc: {"audience":"user","featured":true,"group":"project","order":10,"summary":"Release highlights with expandable development records for each version."} -->
# Changelog

Release highlights are followed by expandable development records. Dates,
measurements, fixture paths, and local artifacts in those records describe the
reported work, not a fresh test of the current checkout.

## [0.10] - 2026-10-13

- Native code: register allocation, signed arithmetic, target ABIs, and object
  linking.
- Language tools: bounded refinements, callback effects, hygienic templates,
  and typed reflection.
- Applications: UI rendering, audio playback, terminal behavior, and runtime
  representation fixes.
- Documentation: public contributor guides, source-highlighted examples, and
  explicit implementation limits.

<details>
<summary>Details</summary>

### Documentation and contributor workflow

- Moved fixture diagnostics, fuzz scratch, optimizer-test files, and example
  output under `build/`. Repository discovery no longer requires a local
  scratch directory; default model lookup uses the managed asset cache.
  Public measurements retain findings and comparison limits, not private
  filenames or session instructions.
- Condensed the measurement archive into workload results, retained mechanisms,
  rejected experiments, and verification limits; contributor procedures remain
  in the public guides.
- Replaced numeric AST-kind output with `--dump-ast` source trees, declaration
  substring filtering, and checked `--dump-ast-typed` trees. Dumps include source
  positions, child roles, defaults, fields, and available semantic facts without
  executing the program; the former single-dash spelling was removed.
- Removed landing-page slogans, repetitive preambles, and decorative emphasis.
  Tightened reference prose and separated historical investigation notes from
  current acceptance work without changing source examples or measurements.
- Replaced the linker roadmap with the implemented request fields, tool
  selection, entry conventions, and ELF limits.
- Consolidated execution/adoption guidance in the status guide, syntax
  extensions in the compile-time guide, vectors in the numbers guide, and
  effect-row coverage in the function reference.
- Published debugging, profiling, graphics verification, and open acceptance
  work through `CONTRIBUTE.md`. Guides and measurements live under `learn/`;
  detailed development records stay with their release here.
- Clarified that package discovery has no default repository or implicit public
  fallback. Users add the source repositories they trust.
- Corrected numeric literals, atomic return documentation, function syntax,
  and Texinfo resource binding. Documented observed dictionary-return,
  dynamic-nil, deferred-capture, and atomic-update failures without claiming
  compiler fixes.
- Fixed static paragraphs, wrapped inline code, escaped table pipes, ordered
  list continuations, indented closing fences, emphasis, disclosure blocks,
  heading anchors, metadata comments, explicit index links, and repository-source
  navigation.
  The renderer regression passed native AOT/JIT and LLVM MCJIT/ORC with library
  caches disabled.
- Assigned topic-specific icons to learning and specification navigation,
  cards, and page headers. Added history, checklist, debugging, performance,
  contribution, workflow, and linking symbols; aligned the package/add-package
  geometry and replaced the mathematical glyph with a continuous summation mark.
- Removed the redundant document-title bar and generic manual-page caption.
  Long contents panels scroll within a bounded height; outline and direct
  heading navigation reveal containing disclosures.
- Removed Proofs from the overview's curated cards while retaining its guide
  in the manual tree, search, and ordinary documentation links.
- Separated sidebar folders, pages, and API symbols with tree guides and
  consistent indentation. Native disclosure buttons expose their expanded
  state and support Enter/Space; namespace and module links navigate separately
  from their arrows. Keep the selected page visible and prevent type-to-search
  from stealing keys from focused controls.

### C frontend reliability

- The lexer and integer parser accept C23 numeric digit separators and standard
  `uUlLzZ` suffixes, including `1'000`, `0x1'0000`, `0b1'0101`, `42UL`, and
  `1'000ULL`. Punctuators use longest-match selection, following
  `stb_c_lexer.h`; its public-domain/Unlicense or MIT reuse rights were checked.
- Four large `ny_parser_t` lookahead copies in `skip_unary_expr`,
  `skip_cast_expr_check`, aggregate lookahead, and `parse_array_extent_primary`
  use heap storage instead of roughly 11.7 MB of stack each. Recorded parser
  checks through `ny_parse_decl` cover `int x[1]`, `int arr[1000]`, and
  `int arr[1'000]`.

### Runtime, library, and build integration

- `str.word_frequency` counts directly without an intermediate token list;
  a follow-up corrects the count for words appearing once.
- Native `??` lowering evaluates the left operand once, preserves non-nil
  false/zero/empty values, and handles optional-chain fallback. Separate
  initialization fixes avoid double-tagging nil and confusing dynamic `false`
  with nil.
- Static LLVM strings are distinguished from tagged runtime strings at
  C-string boundaries. The sorted builtin alias table again resolves
  `core.set_mod.len`.
- Native UI startup discards the trace logger's floating-point result rather
  than returning it from a side-effect-only function. Large UI bundles use a
  larger bounded relocation capacity.
- Windows formatter/audit workers use portable processor discovery; Darwin
  test tools include their signal declarations; BigInt CPU-feature detection
  works across the supported Clang versions.
- Test path filters accept either directory separator across platforms.
  POSIX peak-memory reports use host-specific units, while audit
  workers share sorted function metadata instead of duplicating analysis.
- Windows native execution uses the host calling convention, reserves
  sufficient compiler stack, and exports internal runtime helpers for JIT
  lookup, preserving strict public FFI pointer boundaries.
- Windows failure replay retains compiler/debugger logs. Benchmark names and
  JSON reports handle native paths and escaped text across platforms.
- `./make check` streams cold standard-library, failure-replay, and benchmark
  checks. CI also has an explicit build/source-check mode and manual controls
  for native platforms, filtered fixtures, replay, and serial benchmarks, with
  retained logs and summaries.

### Native x86-64 backend

- Signed remainder by a known power-of-two divisor now uses shift-one
  bit/remainder lowering instead of `idiv`; negative odd operands still
  produce `-1`, matching truncating signed remainder. The `collatz` benchmark
  checksum matched NYR, LLVM, and C (`178`). With 25 runs and 5 warm-ups,
  NYR measured 250 µs versus 259–262 µs before the change; one paired
  p95 comparison moved from 277 µs to 261 µs. Disassembly confirms the
  hot `% 2` path no longer contains `idiv`. `binary` stayed at 3.23 ms
  (5 runs, 2 warm-ups).

### Project demos

- `etc/projects/donut.ny` is now the runnable donut-shaped source itself, not a
  comments-only preview followed by a separate readable program. The private
  Gist-derived shaper removes common left padding and picks the smallest
  configured mask that fits the executable source. The renderer uses a 16-bit
  depth buffer, a 0.02-radian ring step, and 12 surface-normal light levels
  mapped across ANSI blue, magenta, cyan, and white with restrained bright
  highlights. A shared viewport scale uses the drawable width and height to
  keep the torus proportional and avoid clipping across sampled rotations and
  terminal sizes. Terminal mode helpers coerce TTY status to integers before
  testing success, so a successful restore does not fall through to the
  sane-mode fallback and alter saved termios flags. The source was redirected
  directly into `donut.ny` and is 3,276 bytes, 451 bytes smaller than the
  pre-dedent output. `./make ny etc/projects/donut.ny` rendered without
  type-fallback warnings. A PTY smoke
  passed 80x24 → 40x16 → 100x32 resize, four ANSI colors, `q` exit,
  alternate-screen restoration, and exact terminal-attribute restoration.
- Native dictionary-key lowering now tags proven raw integer carriers even
  when an imported expression lacks source-level representation facts.
  This keeps variable and literal key lookups in the same domain: X11 W
  remains key 87/scancode 25 instead of colliding with scancode 51.
  Native reachability retains the source bodies of `window.key_down` and
  string replacement helpers instead of assuming every call is inlined.
  The cold `x11-logical-key-map.nshape` regression passed; six focused
  dictionary, nil, and keyboard fixtures also passed with execution caches
  disabled.
- `etc/projects/ui/pong.ny` now keeps its physics helpers and mutable game
  state explicitly `f64`. It converts `core.math.abs` and container-returned
  values at the numeric boundary, preventing `any` from leaking into steering,
  swept collision, wall reflection, and ball motion. A strict-types mock run
  completed 120 frames and emitted `NYTRIX_FIXTURE_READY pong`.
- UI demo verification now uses a private Xvfb smoke that captures the generic
  `w` key while it is held, rather than inferring app input from the X server's
  keymap or a screenshot taken after release. GUI window collapse state is
  normalized to `bool` and only applied when both titlebar and collapsibility
  are enabled; the UI engine passes the current tiled editor/inspector bounds
  instead of stale fixed rectangles. Headless OpenGL smokes showed visible
  synth and editor output after their slow software-rendered first frames,
  retained output after resize, and a visible Pong response while `w` was
  held. A renderer-probe resize capture taken after its own five-second draw
  lifetime was correctly identified as invalid; a run resized before that
  deadline retained its colored primitives. The synth no longer forces
  Vulkan, so the configured backend can be selected. Vulkan presentation
  remains unverified in Xvfb: Mesa reports that DRI3 is unavailable and
  logical-device initialization fails before the window maps.
- Terminal grid resize now keeps the old column stride as an `int` before
  computing source-row addresses. The dynamic dictionary value had corrupted
  copied cells when widening the grid. Raw row copies now use
  `__copy_mem_raw`; the focused `vterm-cell-output.nshape` regression verifies
  preserved rows and blank newly allocated rows after resize.
- Linux joystick initialization and the fallback rescan timestamp now use
  dedicated module state rather than dictionary fields. This keeps initialization
  one-shot and preserves the five-second rescan throttle when inotify is
  unavailable. The private Xvfb input smoke rendered the keyboard, registered
  the synthetic `W` key, and retained visible keyboard content after resizing.

### UI renderer

- Dynamic string classification now validates the complete small-string header
  once, and address readability checks no longer query the map oracle twice
  for known mappings. Two matched six-second headless input-renderer profiles
  reduced cycles by about 8.5% and instructions by about 11% versus the
  pre-change sample; keyboard input and resize captures remained visible.
- The OpenGL draw module now imports `VERTEX_STRIDE` as a named constant.
  Rounded rectangles, circles, and rings therefore allocate and address the
  expected vertex bytes instead of treating the module-qualified constant as
  zero. Packed integer circle/ring colors use the shared color-component
  decoder, and ring validation uses a non-mutating normalized inner radius.
  Scratch-buffer growth preserves the existing allocation if `realloc` fails
  and returns no buffer if its fallback allocation also fails. A private
  640×480 Xvfb capture showed colored rounded-rectangle, circle, SDF-rectangle,
  and ring output; the focused `render-color.nshape` passed all three variants.
- GL text quads now use the typed vertex writer, retaining NaN/infinity
  fallback and f32 range checks without routing every text vertex through
  dynamic `any` conversion. The synth demo applies FX controls only when their
  values change and scales its coordinate space to preserve the layout and
  pointer mapping after resize. The focused renderer fixture passed all three
  variants, and private Xvfb captures showed colored text/primitives and a
  fully drawn synth at 720×480.

### Test runner

- Parallel-runner wait heartbeats now use a compact active/oldest/elapsed-time
  line and emit every 45 seconds instead of every 30 seconds.
- The Ny desugar worker uses a compact unique temporary basename so its
  generated path literals retain their string lengths when passed through the
  VM. The cold `ny-desugar-parity.nshape` fixture passed all eight variants.
- The `.nshape` runner now supplies the CMake executable and sibling
  `ny-fmt`/`ny-doc` paths when the caller has not provided explicit values.
  This makes the migrated CTest fixtures run through `./make test` as well as
  CTest; the 12 previously failing environment-dependent fixtures pass in a
  focused run. The full 14-fixture failure set from the reported cold run also
  passed together through `ny-test` with execution caches disabled and shared
  core bytecode supplied.
- CTest compiler/documentation checks now use the same `.nshape` runner as
  focused tests. The legacy derive expansion fixture retains its AST digest
  assertion; documentation discovery checks standalone/embedded output parity,
  current ranking order, and the no-compiler fallback. Module-cache,
  standard-source-cache, and AOT-cache identity checks now express their file,
  process, environment, and cache-hit assertions in executable `.nshape`
  fixtures. Formatter case-dispatch parity also runs as a shape fixture with
  the standalone formatter supplied by CTest; CLI alias/option parity runs
  directly through the same compiler fixture runner. The operator-surface
  integration now exercises NYIR, native, LLVM, emitted executable, and Wasm
  paths from one executable fixture. Formatter operator migration now checks
  legacy, conservative, mixed, and embedded-shape source through the fixture;
  formatter CLI rejection and repeated-literal audit parity now run both tool
  executables from `.nshape`. Removed duplicate helper scripts and the legacy
  derive helper source; CMake still registers target-dependent checks and owns
  build setup. The cold-start compile budget now runs from `.nshape`, where it
  controls the cache-bypass environment, creates an isolated source, validates
  execution output, and enforces the configurable wall-time limit. AOT-cache
  identity now uses a per-run source pathname so shared system-temp cache
  entries cannot turn its expected initial miss into a stale hit. Formatter
  and native-image cache scripts that require separate stdout/stderr capture,
  PTY-backed REPL tests, and target/toolchain-owned C tests remain CMake-driven.

### Native backend performance

- Fast glyph texture ids now decode the packed 32-bit sentinel as signed before
  validity checks. GL text no longer treats `0xffffffff` (the unavailable
  texture marker) as a usable texture; zero-ink fast glyphs are reported as
  present only for the ordinary and non-breaking space characters. If the GL
  flat-color text batch cannot draw any glyphs, it now falls back to the normal
  text path instead of silently dropping the run. A live GL renderer-probe
  smoke showed both the white label and yellow `FAST TEXT RUN`; the unavailable
  glyph now reports `A=false, texture=-1` rather than the invalid unsigned id.
  The GL built-in bitmap fallback also batches horizontal lit-pixel spans into
  one vertex submission per string instead of issuing a rectangle draw for each
  lit pixel; the renderer-probe screenshot retained both labels after this
  change.
- Native branches now decode mutable typed-bool globals from their canonical
  tagged storage before testing them. Previously, stored `false` was the
  nonzero false-tag and incorrectly selected the true branch, which caused
  native UI startup flags to enable unrelated paths. The new
  `global-bool-storage.nshape` regression passes native O0/O3, native-only,
  and LLVM JIT variants.
- Native lowering now preserves the pointer representation when reloading a
  mutable top-level typed-buffer handle. The inferred-buffer loop no longer
  emits `rt_any_to_i64` conversions for those loads; its `rt_main` static-island
  counts changed from 2 dynamic operations, 2 box/unbox conversions, and 10
  runtime calls to 0, 0, and 8. A native-result-oracle smoke passed, and the
  1,000,000-element typed-byte sieve returned the expected 78,498 primes.
- The inferred-pointer reload classifier now requires a pointer type name
  instead of treating every pointer-shaped semantic representation as a raw
  pointer. This keeps typed-buffer globals on the raw path without corrupting
  native execution of the legacy derive-expansion fixture.
- OpenSSL EVP/HMAC imports and cached digest/context values now use `ptr`,
  matching the C pointer ABI instead of tagging them as language handles.
  This fixes optimized NYIR verification of `EVP_MD_CTX_free`; the focused
  legacy derive-expansion fixture passes both configured routes.
- Private-list scalar replacement now forwards a slot load only when one
  matching i64 store dominates it in the control-flow graph, including stores
  before a branch. Bounds elimination uses the actual eight-byte access width
  against the buffer's logical capacity. The observable recursive-list
  reproducer's `count` static-island facts changed from one heap allocation,
  two runtime calls, and two bounds checks to zero for all three; its
  `--native-oracle-per-pass` checksum is 4,057. The focused legacy
  derive-expansion fixture also passes after the optimizer change.
- The synthesized `rt_main` now keeps private F64 globals in entry-local slots
  when no user function references them. The n-body workload improved from
  5.22 ms to 4.67 ms NYR time; its matched 5-run/2-warm-up LLVM comparison
  measured 1.98x LLVM/NYR, down from 2.23x.
- Native GPR loop-phi coalescing is opt-in with `NYTRIX_GPR_LOOP_CARRY=1`.
  The guarded-division regression showed that default carried-register
  residency could lose a loop accumulator update; normal builds now retain
  canonical homes until the encoder's edge proof covers that case. The
  opt-in previously improved the focused binary benchmark from 3.93 ms to
  3.67 ms over matched 5-run/2-warm-up samples, with matching checksums.
- The generic indexed setter now tags only values whose emitted NYIR
  representation is raw. An already-tagged integer key was being tagged a
  second time, so dictionary lookup used a different key than assignment.
  The focused X11 logical-key-map probe now passes.
- Loop-phi edge coalescing now rejects caller-saved colors clobbered anywhere
  in the live source segment, not only at its terminator. A call in the
  preheader previously allowed the timer call to clobber a carried loop value,
  causing checksum divergence in `call-chain`, `hash`, `inline`, and `licm`.
  A register-allocation regression covers the preheader-call case. The focused
  benchmark checksum gates now pass for all four fixtures.

### Audio playback

- Sound streams now retain the backend-opened handle and backend ID, and the
  sound facade normalizes its dynamic looping argument before dispatch. Pulse
  playback with `init(false)` therefore stays synchronous instead of starting
  a mixer thread that cannot access the open stream. Synchronous playback now
  reports write failures instead of returning a successful instance, and
  backend diagnostics report the selected backend. The backend initializes its
  master gain before first playback, and the S16/F32 fast mixer preserves its
  floating-point cursor across output blocks; this prevents silence from an
  unset gain and broken samples after the first period. The remaining severe
  distortion came from deriving frame bytes through dynamic integer division:
  S16 stereo was calculated as 10 bytes instead of 4, so each mixer write read
  beyond its 1,024-byte block. Frame sizing now maps the declared S16/F32
  format directly to 2/4 bytes per channel; the backend I/O self-test covers
  mono and stereo for both formats. The sound source fixture verifies
  consecutive mixer blocks. The sound project writes a 48 kHz, 440 Hz sine
  WAV with a smooth 20 ms edge fade, loads and plays that WAV synchronously
  through the selected backend at unity gain, then cleans up. The source stays
  at 4% full scale; the mixer does not reduce the WAV's amplitude.

### Terminal projects

- Matrix rain now follows cmatrix's alternating-column layout, with independently
  paced streams, varied lengths and gaps, and mostly alphanumeric glyphs with
  occasional symbols. White heads fade through cyan into green; incremental
  cell updates avoid repainting blank canvas cells. The resize-aware
  `fixture.canvas_loop` still owns quit handling and terminal restoration.

### NYR public API

- Consolidated the single- and multi-object ELF linker into
  `src/code/backend/object/linker.c`. Added `nyr_link` requests for executables,
  shared libraries, and static archives: native Linux x86-64 executable links
  keep the in-process ELF path, while other target formats dispatch directly
  to clang/LLD, wasm-ld, or llvm-ar. The public API minor version is now 8.
  A local linker smoke linked sample COFF/PE, Mach-O, and Wasm objects and
  verified each output format; the existing NYR ELF acceptance executable
  passed. Cross-target use still depends on the selected linker and sysroot.
- `nyr_optimize` now runs the optimization pipeline on a deep copy and only
  replaces the caller's function after success. A failed pipeline leaves the
  original NYIR intact, at the cost of temporary memory proportional to the
  function size. The public API reference documents this failure guarantee.

### Compiler and test reliability

- The test runner's `Peak fixture RSS` summary is a `RUSAGE_CHILDREN.ru_maxrss`
  high-water mark across reaped child processes; it is not summed RSS or a
  per-fixture attribution. The investigated `loop-call-current-argument`
  reports used different compiler executables before and after rebuild, so
  their 237.0/243.0/247.7 MiB values are not a matched-binary memory
  comparison. No memory regression was established.
- The 2,185.3 MiB peak reported by `run-960027` is reproducible in the focused
  `kernel-list-alias-cow.nshape` fixture: its cold, serial 2-check run passed
  and reported 2,194.9 MiB. The fixture sweeps list sizes 1–513 while
  interleaving indexed writes through an alias and appends. A matching native
  `-O0` program run returned checksum `26812284435` and reported 2,140.0 MiB
  with `--report-peak-rss`; a no-op native control reported 238.4 MiB. Massif
  attributed the growth to runtime `rt_tbuf_append_i64_raw` and
  `rt_tbuf_clone_raw` allocations. The long differential fixtures listed
  first in `run-960027` each peaked around 346–356 MiB when run alone, and
  `legacy-derive-expansion.nshape` passed cold at 348.8 MiB. Thus the large
  peak is a fixture's runtime allocation workload, not summed worker RSS or
  compiler startup variance. No suite rerun or change to fixture coverage was
  needed.
- Native `int(BigInt)` lowering now passes already-raw i64 values through
  unchanged instead of treating their bits as a tagged BigInt handle. This
  prevents `INT64_MIN` from being dereferenced as a pointer in the native
  result-oracle path. The runtime-bound-wrap oracle fixture passes, and the
  current cold full suite passed 2,352/2,352 fixtures. A cache-enabled
  full-suite run also reported 2,352/2,352; the separate cold run and focused
  cache-disabled executions provide the execution evidence.
- NYIR inlining now stops the outer round loop when splice growth exhausts its
  budget; previously it stopped only the current round and retried until
  excessive IR growth or a timeout. Cloned return copies also inherit the
  call's scalar ABI class, preserving aggregate results across inlining.
  `inline_general` optimizer checks and the focused `inlined-guarded-division`,
  crypto-import, and aggregate-argument ABI fixtures pass.
- Wasm internal calls now push every argument onto the operand stack instead
  of limiting argument loading to the native four-register count. AVR function
  setup reserves the return-address space below locals before calls. Focused
  Wasm executable-runtime and AVR bounds-check fixtures pass.
- Typed-buffer store helpers now return their declared buffer pointer instead
  of the stored scalar. Native `i64buf_store` and `f64buf_store` lowering now
  preserves that same contract. This fixes pointer loss when a store result is
  assigned and prevents subsequent stores from dereferencing the scalar as an
  address. The focused all-width store smoke and 1M-element u8 sieve completed
  with the expected checksum; the single `knucleotide` benchmark also passed.
- x86-64 object emission now keeps the canonical home for an address base
  forwarded into a nested ADD→load SIB sequence. The SIB emitter loads the
  index first, so it must not clobber an unspilled forwarded base; the
  ownership regression fixture now passes in native execution.
- x86-64 immediate multiplies by 3, 5, or 9 now encode `%rbp`/`%r13` as a
  SIB base with a zero displacement. The previous base encoding selected the
  no-base form and corrupted following instructions; the proven-integer branch
  probe now executes successfully.
- POSIX subprocess capture now uses `posix_spawnp` file actions and
  close-on-exec pipes kept above the standard descriptors. Concurrent
  compiler subprocesses no longer fork from a multithreaded runtime, inherit
  unrelated workers' pipe ends, or close a redirected child stream when a
  parent standard descriptor is unavailable.
- The Ny-side desugar worker now isolates inherited standard-library bundle
  overrides while honoring its explicit minimal-stdlib mode. Test-runner
  prebuilt paths no longer make the nested compiler exhaust its memory limit.
- When shared core bytecode is unavailable, `./make test` now resolves standard
  modules from the checkout instead of forcing the monolithic generated source
  bundle. The n-body native-JIT fixture now passes its previously unresolved
  standard-library string symbol.
- The unfiltered cold suite (`./make test --cold --with-stdlib --no-bench
  --jobs=5`) passed 2,352/2,352 runnable fixtures with zero failures or
  timeouts. All 17 fixture paths in the separate cache-enabled failure profile
  `run-4079221` also passed in this cold run. The runner skipped 199 fixtures
  by policy or host capability: 137 optional system stdlib modules, 36
  CI-skip fixtures, 22 browser-only fixtures, 2 architecture-specific native
  fixtures, and 2 CMake-only sources.


### Example projects and focused regressions

- Rule 110 now renders a mirrored half-state symmetrically and applies the
  correct asymmetric transition table. Inclusive range endpoints are bounded
  to the actual cell counts, preventing an extra terminal column and wrapped
  rows. It stops exactly at the horizontal edge when width limits generations,
  reserves space for the heading and prompt when they fit, and emits no
  trailing blank-line padding. It uses double-width blocks whenever the
  terminal can fit them; on odd column counts the incomplete final column stays
  unused rather than distorting the cell grid. It never clears existing
  contents.
  The 45-line simulator reuses two state buffers and one pre-sized output
  builder.
- Fixed the native terminal-size bridge to store descriptor-list integer
  payloads untagged. `get_terminal_size()` now reports physical rows and
  columns instead of the tagged `2*n+1` representation.
- Fixed SHA-1 fallback hex output to append digest words through a
  managed string builder. Chained temporary-string concatenation could return
  an invalid value and crash callers that read the digest length.
- Fixed global mutable-list method effects so project functions such as
  `draw` keep known effect summaries. Function reachability now resolves
  `utf8_slice` by semantic declaration identity, avoiding a zero-address
  indirect call in the donut renderer.
- The HTTP server now parses decimal `Content-Length`, preserves the success
  flag in response dictionaries, and exits successfully after a bounded
  successful run. Loopback smoke testing returned HTTP 200 with the expected
  `/health` JSON body. Its example project now builds heterogeneous JSON and
  server-option dictionaries with explicit `dict_set` calls and handles
  `/favicon.ico` with an empty 204 response.
- Matrix rain now stores its single-width ASCII glyph table as a string rather
  than rebuilding a 93-entry string list at each indexed glyph lookup. In
  matched 80×24 native `-O3` PTY runs, 40 no-delay frames fell from a 0.700 s
  median to 0.071 s. A timed 40-frame resize smoke completed in 0.898 s and
  restored both alternate-screen and cursor state without adding a newline.
  A current 40-frame PTY profile completed in 0.049 s with no `mincore` calls;
  20 ms pacing completed in 0.875 s. Resizing from 80×24 to 100×32 cleared
  and redrew the canvas, then restored alternate-screen and cursor state.
  `rt_canvas_put_cell` retains its x/y bounds checks.
- Sound resource loading keeps decoded sources dynamically typed instead of
  narrowing them to the initial integer sentinel. Native libsndfile decoding
  opens the normalized file path directly and reads its 64-bit frame count as
  a tagged Nytrix integer. A generated-WAV round-trip regression verifies the
  loaded source type, frame count, rate, and channel count.
- Added explicit scalar types to UI engine loop parameters that were
  incorrectly treated as callback candidates during effect inference.
- Fixed x86-64 call-boundary register tracking: ABI argument placement could
  re-seed a caller-clobbered colored value after its home was flushed, and
  later uses then read the clobbered physical register. Calls now invalidate
  caller-clobbered color residency after argument setup; the existing typed
  list-index ABI regression passes all four execution/optimization variants,
  including the previously failing O2 native route. An unseeded argument now
  loads directly from its canonical home into its ABI destination, avoiding a
  reload into (and clobber of) an earlier argument's register; the native SoA
  codegen fixture passes all four variants.

- All 20 `etc/projects` programs compile. Bounded matrix and donut runs rendered
  frames. The focused project regressions and the two reported suite failures
  (`legacy-derive-expansion` and `realloc-null-stub`) pass cold (6/6).
- Conway's Life now converts dynamic row-offset values to integers before
  indexing, so its first source compilation no longer emits W2101 fallbacks.
  Its non-interactive frame remains bounded to one draw; interactive runs
  retain the 600-frame cap. Random seeds now start at 20% density instead of
  half-filled noise, and the doubled-width cells leave the terminal's final
  column unused to avoid edge wrapping. Canvas frames use the active TUI's
  configured delay, preventing tight-loop redraws.
- `core.os.process.run` now supplies `argv[0]` when callers pass only command
  options, so option-leading commands such as `stty -F /dev/tty ...` receive
  their arguments correctly. Raw terminal mode disables `ISIG`, so Ctrl-C is
  read by the TUI and runs its deferred screen/cursor cleanup instead of
  terminating the process mid-frame. Compiler diagnostics coordinate with the
  live progress renderer, keeping warning and hint lines separate from the bar.
- Matrix rain now uses integer lists for its drop state and builds replacement
  lists locally before publishing them to global state. This avoids dynamic
  arithmetic fallbacks and the NYR invalid-free failure during one-frame runs.
  Its glyph set is single-column, its fading colors avoid per-cell randomized
  palettes, and the terminal-frame scan uses nested row/column counters rather
  than division and modulo for every cell. Frame-stamped trails avoid rewriting
  each live cell every tick. Interactive canvas refreshes repaint changed runs
  instead of transmitting the full screen each frame. Fixture canvas cleanup
  now releases the current resized canvas exactly once, and its incremental
  renderer reuses one output buffer instead of allocating and freeing it on
  every frame. The runtime address
  oracle now returns authoritative positive mappings directly instead of
  issuing redundant `mincore` probes: a matched 40-frame headless Matrix run
  fell from 13.242 s to 2.726 s, and a 10-frame trace fell from 1,614,476
  `mincore` calls to 16.
- Conway now evolves rows with the allocated grid width as its stride. The
  previous extra halving updated only part of each row and indexed later rows
  incorrectly; initial seeding is also sparser to keep the display legible.
  The project compiled and completed 40 PTY-driven frames across a resize,
  restoring the alternate screen and cursor.
- TUI teardown now restores the saved screen and cursor without writing a
  trailing newline, avoiding a blank line before the shell prompt. Rebuilt the
  standard library and verified bounded exit after resize and Ctrl-C through a
  PTY; both exited cleanly, restored the alternate screen/cursor, and emitted
  no trailing newline.
- Donut's palette index and the heredoc example's numeric interpolation now
  keep integer types through their arithmetic, removing their first-compile
  W2101 fallback warnings.
- The audio example's unavailable-backend branch now tests optional sound and
  instance handles as booleans; its compile smoke exits cleanly without W2101.
  The HTTP server explicitly converts request method/path and error payloads
  before using them as strings. Its heterogeneous JSON and server-option
  dictionaries use incremental `dict_set` calls to avoid W2101 fallback
  warnings.

### Benchmark output

- The Ny heapsort benchmark now stores its array in a typed i64 buffer rather
  than a generic list. Its isolated checksum-checked benchmark measured 7.80 ms
  NYR versus 19.3 ms before the change; the C reference and algorithm are
  unchanged.
- The n-queens benchmark now uses first-row mirror symmetry in both Ny and C.
  The checksum-checked N=13 run fell from 95.8 ms to 52.1 ms in NYR and from
  30.4 ms to 16.8 ms in C; both retain the 73,712-solution result.
- Benchmark fixture execution now defaults to one worker so concurrent timed
  fixtures cannot distort per-row runtime ratios. `--jobs N` and
  `NYTRIX_BENCH_JOBS` still opt into bounded parallel runs. A full warm
  65-fixture run (three samples, one warm-up) passed all fixture budgets and
  geomean gates; `binary` measured LLVM/NYR at 4.64x and `dict-set` at 1.04x.
- Benchmark summaries now render NYR consistently in console and Markdown
  output, and use one ratio formatter across fixture rows, hotspot callouts,
  geomeans, and gates. Ratios below 0.01x display as `<0.01x` rather than the
  misleading `0.00x`; the one-fixture string smoke showed aligned columns and
  preserved advisory status for a partial run.

- Native string concatenation now recognizes an immutable local initialized
  directly from a string literal, allowing looped `result = result + suffix`
  to lower to `rt_cstr_builder_repeat_known` instead of repeated append calls.
  Local shadowing is resolved before top-level literals, and the shadowing
  fixture now checks the exact repeated string as well as its length. The
  focused fixture passes cold. A full 66-fixture benchmark completed with all
  checksum and geomean gates passing; its append-scaling row measured 9.06 ms
  native, 9.00 ms LLVM, and 12.8 ms C.
- Benchmark fixture rows now stream when each fixture finishes, showing
  per-route timings and comparable ratios without worker-ID chatter. Two
  complete 66-fixture runs exited successfully; observed wall times were
  25.841 s and 12.431 s with warm execution caches. These runs verify output
  and warm throughput only; they do not establish cold `core.bc` build
  throughput, which remains open in `docs/TODO.md`.
- The focused `native-image-cache`, `std-source-cache-reuse`, and
  `core-bc-cache-selector` CTest regressions pass together (3/3). The
  native-image case checks warm reuse of
  relocation-bearing code with mutable data, source and optimization-key
  changes, truncated-cache recovery, explicit-assertion bypass, inline LLVM
  reuse, and the `fannkuch` checksum on cold and cached runs. Reuse remains
  specific to matching source/configuration keys; this does not establish
  cross-program native core reuse.
- Shared `core.bc` identity now uses the assembled core and compiler
  source/configuration fingerprints rather than the full compiler executable
  hash, so ordinary relinks can reuse the artifact. A Ny AOT checker validates
  the signature and exact source manifest before test runners select it.
  `NYTRIX_TEST_USE_STD_BC=1` opts focused tests and benchmarks into reuse;
  they skip the shared bitcode build by default.
- The test memory guard now budgets 3.4 GiB per worker, including 0.2 GiB of
  headroom over the observed ~3.2 GiB native-codegen peak, and retains an 8 GiB
  host reserve. A cold full-suite run exercised six workers without OOM
  (2,338 tests; 2,335 passed; 2,239.3 MiB peak fixture RSS). This confirms the
  guarded configuration is viable on the measured host; it is not a matched
  five-worker speedup measurement. The three target failures from that
  historical run were later cleared by focused cold target/replay runs.

### Test and standard-library build throughput

- Cold source-build profiling led to two compiler-path reductions: user
  function resolution now uses the existing exact-name hash buckets before
  falling back to the ordered scan, and NYIR type-map setup skips its
  union-find workspace when a function contains no typed float/vector values.
  With identical cold stdlib-emission flags, elapsed time fell from 195.99 s
  to 167.45 s (14.6%); emitted bitcode SHA-256 was identical
  (`84d0ebd0d30109df166d03809355c433c1ce9df1612f135ca8ae3da4d8d74175`).
  A current low-frequency CPU profile collected 8,138 samples with no lost
  samples; the remaining largest costs are call lowering and function
  resolution.
- A fresh cold-build profile collected 16,748 CPU samples with no lost samples;
  function resolution remained the dominant lowering cost. Expanded the
  target-aware tree-fallback cache from 4,096 entries/four probes to 16,384
  entries/16 probes, and indexed dynamic-list fact lookups. With identical
  cold stdlib-emission flags, per-function lowering fell from 122.742 s to
  114.012 s (7.1%), NYIR build time from 154.786 s to 143.074 s, and wall time
  from 168.376 s to 156.297 s. Both emitted bitcode files were 30,051,776
  bytes with SHA-256
  `84d0ebd0d30109df166d03809355c433c1ce9df1612f135ca8ae3da4d8d74175`.
  The current cold suite with the stdlib sweep passed 2,481/2,489 cases. Its
  eight failures were replayed against the saved pre-change compiler and
  reproduced there as well.
- Full benchmark validation passed all 65 fixtures with nine samples and
  three warm-ups, pinned to CPU 24 for a serial, affinity-controlled run.
  All repeated samples passed the existing noise check and checksum
  comparisons; `binary` measured LLVM/NYR at 4.86x, below its unchanged 5.00x
  fixture limit. No benchmark threshold or workload was changed.
- Fixed POSIX subprocess capture to merge child stderr into stdout, matching
  `core.os.subprocess.run_capture`'s contract and restoring generated
  negative-diagnostic assertions. The callback-alias differential passed all
  130 compiler invocations cold; changing its two workers to four reduced
  fixture time from 46.134 s to 26.424 s and 26.315 s in repeated runs.
- Generated ownership/refinement fixtures now invoke the compiler directly
  through argv instead of starting a shell for every generated case. The
  focused cold batch of eight generated differential fixtures plus the AVR
  float fixture passed 9/9 in 39.563 s with five workers. The legacy derive
  probe also passed separately cold (1/1, 10.527 s). These are focused
  correctness results, not a full-suite throughput comparison.
- Consolidated the AVR float fixture from 15 compiler/QEMU invocations to
  eight precision-specific checks, retaining its comparison, ABI, arithmetic,
  signed-conversion, widening, and narrowing assertions. Two focused cold runs
  passed in 24.338 s and 24.459 s; splitting arithmetic checks preserves AVR
  branch range and the two-second emulator window.
- Fixed arithmetic operator tails being lowered as heterogeneous positional
  lists, whose second slot was read incorrectly by the LLVM JIT route. Tails
  now use named `operator` and `operand` fields. A cold
  `arithmetic-parser-kit` run passed all four backend/runner variants, including
  shared `core.bc` and LLVM `mcjit`/`orc` routes.
- Added opt-in NYIR construction stage timings to the existing
  `NYTRIX_TRACE_NYIR_EMIT_TIMING` trace. A measured cold stdlib emission spent
  180.264 s in per-function lowering, 28.828 s on reachable-function analysis,
  and 2.310 s in dynamic-list prewarming; NYIR build totaled 211.853 s and LLVM
  emission 3.658 s. This instrumentation is disabled unless requested. It
  localizes the cost but does not yet demonstrate a compile-time improvement.
- The latest cold focused rerun of the six slow generated differential
  fixtures passed all six with 332.095 s aggregate fixture time. The callback
  alias fixture invokes the compiler 130 times serially; reducing that repeated
  work remains open because any batching must preserve each independent
  positive and negative assertion.

### Cross-target backends

- Wasm32 `extern "c"` imports now classify multi-field aggregates as memory
  class even when they occupy only eight bytes; Wasmtime fixtures check that
  small integer-pair and two-float-pair arguments cross the boundary as
  pointers. One-piece four-byte INTEGER and SSE aggregate arguments and returns
  map to `i32` and `f32`. Eight-byte INTEGER and SSE aggregate arguments map to
  `i64` and `f64`, with direct `i64`/`f64` returns. The portable-Wasm NYIR
  test passes; Wasmtime SSE/INTEGER argument/return and memory-class
  argument/return fixtures pass (3/3). Eight-byte multi-field returns also use the Wasm32 sret
  pointer and pass Wasmtime. Nonstandard multivalue return metadata is outside
  the C ABI support scope.
- The AVR floating-constant fixture now checks `f32`/`f64` stack arguments,
  exact tagged return bytes, all six comparison predicates, signed zero,
  unordered NaN comparisons, and infinity comparisons in two QEMU programs.
  Predicate checks use direct branch assertions rather than integer-weighted
  boolean sums, avoiding a generated 64-bit multiply loop in the test itself.
  The legacy derive expansion probe now checks record fields and generated
  declaration names instead of pinning the entire serialized AST hash. Both
  affected fixtures pass together in a cold focused run.
- AVR external C calls now marshal `f32` arguments to their register and stack
  locations, including after two raw `i64` register arguments, and capture
  `f32` return values. A QEMU fixture checks IEEE payloads and exact caller
  stack cleanup; unsupported `f64` arguments remain fail-closed. Existing
  3/4/9-argument stack calls still pass.
- ARM32 stack homes now keep i64 high words separate from adjacent 32-bit
  values. Its NYIR-verified QEMU smoke executes carry/borrow add/subtract,
  low-64 multiplication, bitwise operations, signed i64 comparisons, and
  variable left/arithmetic-right shifts at word-boundary and high counts,
  alongside full-width signed division/remainder, byte-swap, and AAPCS raw
  returns. ARM32 also materializes exact `f32`/`f64` constants into both
  words. Soft-float `f64` arguments follow AAPCS register-pair alignment and
  stack spilling; `f64` results preserve `r1:r0`. Soft-float `f32` arguments
  use their core-register or stack-word locations. NYIR-verified QEMU
  roundtrips check trailing `f64` and `f32` arguments after zero through eight
  integer arguments, including register-pair alignment and stack spill offsets.
  Mixed-order `f32`/integer/`f64` calls also pass exact-result QEMU checks. The
  expanded ARM runtime fixture passes cold under QEMU.
  Division and remainder use a restoring 64-bit path. QEMU checks signed
  results crossing the 32-bit boundary and the existing zero-divisor trap.
  ARM panic-spin labels
  now include their function identity, preventing duplicate symbols when one
  function contains multiple checked divisions. The focused ARM runtime fixture
  and the six D1 regressions from the reported replay pass cold (7/7).
  Removed the stale COFF64 aggregate-return rejection fixture: Win64 aggregate
  return and forwarding are already exercised by the passing
  `aggregate-composed-call.nshape` runtime test.
  The new AVR fail-closed matrix passes cold and verifies that representative
  unsupported square-root and transcendental operations fail without leaving
  partial assembly.
  AVR aggregate-parameter rejection now covers one-word, two-word,
  two-`f32`, and three-word layouts; its focused cold fixture passes all four
  cases.
  The new portable-AVR NYIR selftest enumerates all current vector and floating
  operations and verifies explicit rejection before partial assembly.
  The Wasm C ABI classifier emits no multi-register aggregate class; all
  direct scalar-class and memory-class aggregate import/return fixtures pass.
- AVR now lowers `f32` and `f64` addition, subtraction, multiplication, and
  division through the AVR libgcc soft-float helpers. Integer/float conversion
  and width-conversion helpers are included; the QEMU fixture links with
  `-mdouble=64` and exercises signed truncation, precision rounding, and both
  float widths. Unsupported square-root, transcendental, and vector operations
  remain explicitly fail-closed. The full focused AVR target fixture set
  passes cold (11/11); the three cross-target replay crashes reproduce neither
  serially (3/3).
- The x86-64 object encoder now homes colored values before every ABI call,
  including ordinary internal calls. Loop-phi residency is disabled only for
  loop regions containing calls, where the current home/residency contract
  cannot safely carry a value through the call boundary. A permanent allocator
  regression checks both loop-phi carry without calls and home-backed values
  with calls. The previous focused cold replay of the 31 reported D1 failures
  passed 34/34 variants. A later broad replay reported crashes in
  `gltf-index-modes`, `legacy-derive-expansion`, and `ffi-gen`; their serial
  cold rerun, together with the reported `f64-scalar-abi` and COFF aggregate
  fixtures, passed 5/5. A focused cold batch including those fixtures, the AVR
  scalar-call runtime, and both Wasm memory-class aggregate import fixtures
  passes 8/8. The crash signals did not reproduce serially.
- Current focused cold verification of the named AVR target fixtures, Wasm
  register-class and memory-class aggregate import/return fixtures, ARM/i386
  portable runtimes, reported probe/FFI regressions, and scalar ABI fixtures
  passes 20/20; the AVR NYIR unsupported-operation matrix passes independently.
- Focused cross-target regressions pass for the AVR, BPF, ARM, AArch64, i386,
  RISC-V64, MIPS64, and PowerPC64 paths (47/47 fixture variants), portable
  Wasm runtime and fail-closed import checks (6/6), and ELF32/ELF64 scalar ABI
  calls (50/50). Cross-target stack-bounds runtime passes, and the generic
  architecture fixture passes all seven target variants. The privileged BPF
  kernel load/test-run smoke returns 33.
- Portable `ror32.i64` now lowers on ARM with register-count `ror`, on i386
  with explicit zero-extension of the 32-bit result, and on RISC-V64 with
  operand and count masking. `bswap32.i64` now uses `rev` on ARM, `bswap` and
  explicit result zero-extension on i386, a portable shift/mask sequence on
  RISC-V64, and word rotate/mask instructions on PowerPC64. NYIR-verified
  QEMU smokes compare both operation results while checking the struct-copy
  payload on AVR, ARM, AArch64, i386, RISC-V64, MIPS64, and PowerPC64. The six
  D1 runtime/ABI/probe fixtures, including the AVR scalar-call
  runtime, pass cold (6/6).
  `bswap.i64` now has NYIR-verified QEMU checks on ARM32, i386, RISC-V64,
  MIPS64, PowerPC64, and AArch64. ARM32 constants initialize both 32-bit
  words; its raw i64 return path uses the AAPCS `r1:r0` register pair, checked
  against the full swapped value in QEMU.
  i386 `NYIR_CONST_I64`, `NYIR_LOAD_I64`, and `NYIR_STORE_I64` operate on both
  halves. All six `NYIR_CMP_I64` predicates compare signed high words and
  unsigned low words. `ADD`, `SUB`, `MUL`, `AND`, `OR`, `XOR`, variable
  `SHL`, and arithmetic `SAR` now operate on both halves; shift counts are
  masked to 0–63. i386 parameter entry sign-extends signed 32-bit `i64` inputs
  and local loads/stores preserve both words, so noinline callees receive the
  correct signed values. QEMU checks cover counts on either side of the 32-bit
  crossover, at 32, and at 63, plus full-width constants, memory operations,
  arithmetic carry/borrow and multiplication, comparisons, bitwise operations,
  signed boundary ordering, and raw i64 returns through the SysV `EDX:EAX` pair.
  `--ir-opt-test=portable_wasm` passes with the cross-target runtime checks.
- The portable WebAssembly backend now emits `env` imports for external
  scalar `i64`, Wasm32 pointer (`i32`), `f32`, and `f64` functions. Pointer
  arguments and results convert between the NYIR `i64` carrier and Wasm32
  `i32`; float arguments and results use their raw NYIR carriers. A Wasmtime
  preload fixture exercises these signatures, including a 64-argument import
  at the NYIR limit and a high-bit pointer round trip; an eight-argument
  internal call also executes. Wasm calls no longer inherit the host-register
  argument limit. Unsupported runtime helpers and unsupported aggregate
  signatures remain fail-closed; import validation rejects aggregate-return
  metadata instead of treating it as a void result. Wasm32 memory-class
  aggregate arguments now cross the import boundary as pointers, checked by a
  compiled, assembled, and
  executed provider fixture; register-class aggregate marshalling remains
  unsupported. WAT-emission negatives assert the unsupported runtime-helper
  diagnostic, and the focused portable-Wasm NYIR test rejects vector and
  unsupported aggregate argument/return import signatures. Its explicit bounds
  check is emitted and exercised for negative offsets and out-of-range limits.
  Wasm `select.i64` now uses the typed `select` instruction, and `ror32.i64`
  wraps to the 32-bit domain before `i32.rotr`; the native-only Wasmtime
  runtime fixture checks both select arms and a low-32-bit rotate. The verified
  NYIR contains both `select` and `ror32` in the exercised functions.
- The portable Wasm emitter now lowers `NYIR_COPY_STRUCT` through
  `memory.copy`. Both source and destination use the checked Wasm32 address
  conversion, and sizes above `INT32_MAX` fail emission instead of truncating.
  The focused Wasmtime NYIR smoke copies and reloads an eight-byte payload,
  confirms a high-bit pointer traps, and exercises oversized-size rejection.
- The ARM, i386, AArch64, RISC-V64, MIPS64, and PowerPC64 portable emitters
  lower `NYIR_COPY_STRUCT` to byte-copy loops using caller-saved temporaries.
  The focused backend NYIR smoke emits the load after this side-effecting
  operation and executes the copied-value check under each QEMU target. ARM and
  i386 reject sizes outside their 32-bit address spaces; all six native
  byte-loop emitters reject negative sizes. The existing MIPS64 and PowerPC64
  portable runtime fixtures still pass cold (2/2).
- The AVR portable emitter lowers `NYIR_COPY_STRUCT` to a bounded byte loop,
  rejects sizes outside its 16-bit address space, and traps if either pointer
  has nonzero upper bits. The backend NYIR smoke assembles and runs an
  eight-byte copy under QEMU and verifies oversized-size rejection.
- WebAssembly now lowers the classified `rt_malloc_i64` allocator helper to
  the module's linear-memory allocator, with checked i64-to-Wasm32 size bounds.
  Repeated zero-size allocations return distinct pointers. A cold NYIR-verified
  native-only Wasmtime fixture allocates memory, stores and reloads an i64,
  checks the tagged result, and confirms a negative size and a size above
  `UINT32_MAX` trap rather than wrapping or truncating; unsupported
  `rt_realloc` remains a diagnostic failure.
- The AArch64 portable-text backend now emits scalar `f32`/`f64` arithmetic,
  signed truncating `f32`/`f64`-to-`i64` conversions, `i64`-to-float and
  `f32`/`f64` width conversions, comparisons, and `f64` square root. Its QEMU
  runtime fixtures exercise float-to-integer conversion with noinline functions
  as well as the other scalar operations through runtime-dependent branches,
  including all ordered predicates and IEEE unordered/NaN comparisons. The
  portable text emitter also lowers variable `ror.i64`, exercised by a
  noinline QEMU runtime check at counts 1, 8, and 63, plus `ror32.i64` using
  the AArch64 W-register form and a constant-count QEMU runtime check. Float
  calls now marshal `f32`/`f64` arguments and results through the AAPCS64
  `s`/`d` registers, including mixed integer
  and floating argument banks. Scalar parameters and calls beyond the first
  eight registers per bank use the aligned AAPCS64 stack area; QEMU coverage
  includes mixed overflow banks, ninth-argument integer and floating calls,
  and 64-argument integer and floating calls. Floating power, sine, cosine,
  and remainder link to the production math runtime and execute; the
  no-runtime negative still rejects unresolved helpers. A fresh cold replay of
  `portable-runtime.nshape` and `f32-to-i64.nshape` passed 2/2.
- AVR portable text now has QEMU runtime checks for `bswap.i32` and `bswap.i64`,
  verifying byte order, zero-extension, and tagged return encoding. The
  scalar-call fixture checks a variable shift whose result spans the high
  32 bits, in addition to smaller shift boundaries, and executes a mutable
  branch merge carried through NYIR PHI values and all signed i64 comparison
  predicates at negative, equal, and positive operands. It also confirms
  unsupported dynamic panic formatting fails closed. Internal AVR scalar calls
  now pass arguments beyond the two register slots on the caller stack; a
  NYIR-verified QEMU fixture checks three-, four-, and nine-argument calls,
  including full-width and negative stack values, parameter order, and caller
  cleanup. External C calls now use the AVR stack convention for classified
  raw-i64 arguments; QEMU links and executes three- and four-argument C
  callees. AVR `f32` and `f64` constants now materialize their IEEE
  representations through the portable emitter; a NYIR-verified QEMU fixture
  checks the tagged result bytes for both widths. Internal `f32` and `f64`
  parameters pass through AVR's eight-byte scalar slots, with QEMU checks for
  fifth-argument stack loads. AVR `f32`/`f64` comparisons now implement all six
  predicates; noinline QEMU cases cover true/false results, signed-zero
  equality/inequality, and all unordered `f64` NaN outcomes. AVR lowers the
  canonical `rt_flt_nan` and `rt_flt_inf` helpers to quiet-NaN and
  positive-infinity constants, with QEMU comparison coverage for infinity.
  The cold comparison fixture passes. All six named D1 target regressions
  (`f64-scalar-abi`, COFF aggregate-return rejection, `gltf-index-modes`,
  `legacy-derive-expansion`, `ffi-gen`, and AVR `scalar-call-runtime`) pass in
  a six-job cold focused replay (6/6).
  All eight AVR target fixtures pass in the focused cold run (10/10 variants).
  External float stack arguments and aggregate
  parameters remain rejected. Focused AVR stack-argument fixtures pass 3/3
  cold, and the float-constant/fifth-argument fixture passes 1/1 cold. The
  scalar-call QEMU fixture passes 1/1 cold. The focused AVR,
  WebAssembly, and AArch64 target regressions pass cold (12/12). The privileged
  BPF kernel load/test-run smoke also passes in its container route (result 33);
  the focused BPF rotate and signed-operation fixtures pass cold (2/2).
  The AVR scalar-call fixture passes cold with the PHI and comparison cases;
  the focused WebAssembly runtime/rejection fixtures pass 5/5, and
  `--ir-opt-test=portable_wasm` passes.
- WebAssembly now supports C-ABI memory-class aggregate returns from `env`
  imports by passing the caller-allocated result buffer as a Wasm32 `i32`
  pointer and treating the imported call as `void`. The new Wasmtime regression
  compiles and assembles a 24-byte aggregate return, checks that the host
  receives a non-null pointer, and executes `rt_main`. Wasm runtime/helper and
  sret import fixtures pass cold (5/5); `--ir-opt-test=portable_wasm` passes.
- The metadata-aware x86-64 COFF bundle writer now places CodeView data before
  the symbol table, matching the symbol-table offset in the COFF header. A
  MinGW/Wine smoke links and executes a `ny-full`-generated Win64 object whose
  Ny function passes a C aggregate factory result by value to an imported C
  function and receives a by-value aggregate from the C host; the previous
  object was rejected by `ld` because its symbol table pointed into the
  CodeView filename string. Aggregate-returning Win64 functions remain
  fail-closed.
- Portable `ror32.i64` lowering now covers BPF, MIPS64, and PowerPC64 in
  addition to the AArch64 W-register form. MIPS64 and PowerPC64 QEMU runtime
  fixtures check the low-32-bit rotate result; the privileged BPF kernel
  load/test-run smoke checks the same result and returns 33. The corresponding
  NYIR-verification paths pass, including target assembly checks for BPF,
  MIPS64, and PowerPC64.

### Benchmark harness

- Benchmark fixtures now print one aligned, colored metric row as each
  completes, with layouts for narrow terminals. Warm execution caching is
  enabled by default; the runner still executes every timed sample and checks
  its checksum. `--bench-cache cold` retains repeated compilation timing.
- Report-only compiles for code-size and static metrics are optional during
  the ordinary console run. `--bench-code-metrics`, structured exports,
  counters, and static fixture budgets request the necessary report compiles.
- Geomean gates include all comparable fixture ratios. Sample instability is
  evaluated within each fixture; large gaps between different workloads are
  retained instead of being treated as scheduling outliers.
- Per-class geomean gate output sizes every column from its header and longest
  row value, keeping the startup and long-fixture rows aligned without
  truncating cells to a fixed width.
- Default compiler assertions no longer disable every JIT cache lookup.
  Explicit assertion requests still bypass compiled artifacts. Native x86-64
  images now retain unrelocated code and named relocations, which are resolved
  in the current process on warm execution. Cache writes are atomic and
  malformed images fall back to compilation.
- Inline LLVM execution promotes validated bitcode to a native image by
  default. Core content fingerprints are reused while source identity, size,
  permissions, and nanosecond modification/change times match; strict content
  hashing remains available with `NYTRIX_CACHE_STRICT_CONTENT=1`.
- AVX YMM SIB memory operands now encode the inverted base/index extension
  bits correctly. The previous swap could address the stack frame in place
  of a high index register and crash the vectorized `fannkuch` kernel.
- The `startup` LLVM/C geomean ceiling was recalibrated from an unreachable
  0.05x to 0.30x. The old value predated the current C>=1ms startup subset
  (`escape-loop`, whose C reference performs 1M malloc/free, plus `inline`);
  those fixtures stably geomean to ~0.17x LLVM/C, so the gate failed
  deterministically whenever `escape-loop`'s C time crossed the 1ms inclusion
  threshold. 0.30x restores the documented 1.6x-baseline relationship.

### Performance

- Typed-buffer length queries now read validated headers directly when the
  epoch-checked handle cache proves allocator provenance. Other representations
  retain the existing resolver. Seven interleaved runs of identically compiled
  heapsort executables reduced median NYR time from 98.330 ms to 15.220 ms and
  LLVM time from 85.664 ms to 4.032 ms, with unchanged checksums. Focused tests
  cover cached lengths, growth, aliases, scalar widths, and pointer ABI paths.
- Mapping-range registration checks its most recent deduplication entry before
  scanning the remaining entries. An allocation-heavy bigint executable
  retained its checksum while reducing retired instructions from 440.42 M to
  430.70 M; eleven interleaved runs measured medians of 130.53 ms before and
  127.00 ms after. The larger bigint arithmetic gap remains unresolved.
- Cold full-stdlib bytecode emission now caches the package-surface module list
  using the same module/block traversal and declaration order as the prior
  lookup. Two cache-disabled, opposite-order comparisons emitted byte-identical
  bitcode and reduced wall time from 254.894 s to 236.276 s and from 261.473 s
  to 228.422 s (7.3% and 12.6%). NYIR function lowering fell by 8.0% and
  15.5%; LLVM emission remained 3.6–3.7 s.
- The direct-mapped enum-member lookup cache grew from 4,096 to 16,384 slots
  after a cold full-stdlib compile measured 81,680 recursive scans. The larger
  cache reduced scans to 72,446 and raised its hit rate from 77.2% to 79.8%.
  An opposite-order cold bytecode pair produced identical output and reduced
  external wall time from 233.08 s to 226.45 s (2.8%); NYIR build time fell
  from 218.984 s to 212.457 s. A no-loss profile after the change measured enum
  tree scanning at 7.13% self samples; top-level variable and extern-declaration
  scans are the next measured lookup costs. Cold core/full-suite throughput
  remains open in TODO.
- The shared program-query cache grew from 8,192 to 32,768 slots after a
  cache-traced full-stdlib compile recorded 98,147 misses and 89,955 evictions.
  The larger cache recorded 66,211 misses and 33,550 evictions; NYIR build time
  fell from 222.358 s to 199.495 s and external wall time from 236.27 s to
  213.26 s. Both cold builds emitted byte-identical bitcode. The cold import
  fixture passes 10/10 with the larger cache.
- Native compilation now builds a per-emission symbol-to-section index for LLVM
  origin metadata and a sorted source-path index for module import resolution.
  Reachability discovery also reuses source-aware function resolutions and
  avoids repeating a qualified function lookup when import-alias resolution
  fails. These indexes are scoped to the compilation and retain the existing
  lookup order and fallback behavior.
- Re-exported constant lookup now memoizes positive and negative module/member
  resolutions for the current compilation snapshot, avoiding repeated walks
  through the same import graph. The AArch64 encoder initializes instruction
  offsets with one allocation and fill instead of zeroing them before
  overwriting every entry with the invalid-offset sentinel. Its floating-point
  allocator also skips the floating-point preflight for integer-only functions.
  Peak-live calculation now keeps small-function difference counters on the
  stack and reserves heap allocation for larger machine functions.
- Typed-list indexed stores now use the thread-local handle cache when its
  registry generation is current and the canonical tbuf header is valid. Cache
  misses, forwarded handles, malformed headers, and sanitizer-poisoned headers
  continue through the checked runtime path.
- Native string-builder lowering folds loops with a statically known inclusive
  range and immutable literal suffix into one reserve and a bulk repeated-copy
  operation. Dynamic bounds and suffixes keep the general loop lowering.
- Native list assignment now recognizes when a returned handle came from the
  canonical stdlib `append` intrinsic, even though semantic resolution records
  the stdlib function as a user function. It reuses that local's companion
  length, which the append lowering updates on both fast and growth paths,
  instead of emitting `rt_len` after the join PHI.

- The x64 object encoder now omits a literal's canonical home only when every
  machine use is an explicitly supported immediate right operand. Data reads,
  call arguments, returns, and cross-block uses keep their homes.

- Extended exact SMT translation validation to the two-lane integer broadcast
  and reduction operations. Both CFG path exploration and straightline
  evaluation now model packed lanes as bit-vectors and reduction as wrapping
  i64 addition. `tv_smt_semantics` proves equivalence to scalar addition and
  rejects a changed lane in both encodings; the bounded interpreter fallback
  also agrees on the CFG case.

- Added LLVM ORC JIT perf-map entries for materialized defined functions. The
  ORC object-transform layer now captures object symbol sizes, which are
  paired with loaded LLJIT addresses; symbols without size metadata are
  omitted rather than assigned guessed ranges.

- Bounds-check elimination now reuses one CFG analysis for dominating-guard
  and fresh-buffer proofs. Because BCE rewrites only non-terminators, its block
  and dominator maps remain valid for the full scan, avoiding repeated analysis
  construction for checked accesses.

- E-graph dominated-reuse lookup now indexes earlier pure scalar expressions by
  opcode and operands before checking CFG dominance. This avoids comparing each
  definition with unrelated prior instructions while retaining instruction
  order and the same first-dominating-definition rule.

- Indexed register-allocation copy hints by destination virtual register and
  source live segments by virtual register/start position. Color selection now
  binary-searches candidate copies and source segments instead of rescanning
  every instruction in each segment and linearly searching all segments. Copy
  affinity remains advisory; the existing conflict/spill rules are unchanged.

- Register allocation now indexes fixed-register clobber sites once per
  register color and advances monotone cursors across the index as live
  segments are visited. This removes instruction-by-instruction scans across
  long live ranges and avoids a binary search for each segment. Common
  register sets use a 64-bit occupancy mask instead of allocating and clearing
  per-color arrays. The existing monolithic and multi-block backend scaling
  cases exercise both paths.

- Phi pruning now builds CFG topology only. Its predecessor and block-membership
  queries do not need dominators or dominance frontiers, so generated join
  graphs avoid computing those unused analyses during dead-code elimination.

- NYIR phi-edge validation, CFG structure checks, and ownership-flow checks now
  request topology-only CFG data. They inspect block ranges, reachability, and
  predecessor edges; dominance analysis remains in the verifier paths that
  check value ordering and dominance.

- Dead-code elimination skips CFG construction for straight-line functions
  without PHIs, branches, labels, or instructions after a return. Its
  reachability check scans each instruction once, including functions with
  repeated returns.

- Parallel work dispatch now applies its minimum grain size per active worker,
  avoiding machine-wide thread teams for workloads that are small per task.

- If-conversion builds a sorted CFG label index once per pass, replacing
  repeated full-block scans for branch and join labels while retaining the
  existing dominance and natural-loop checks.

- Native `ctpop.i64` lowering now uses the typed popcount runtime bridge, so
  host builds can emit the target's population-count instruction instead of
  expanding the operation into a SWAR sequence.

- Hardened the rectangular loop-interchange legality gate against induction
  values escaping through non-induction exit PHIs or expanded call arguments.
  The transform now swaps only the loop guards' constant operands, preserving
  shared constant definitions and their range metadata.

- Generic `@effects(e)` callback aliases now retain outer-local updates across
  block scopes and join `if` branches, `match` arms, `while` bodies, and
  C-style `for` bodies by union. A match without `else` and loops include the
  incoming row as a possible path. Exception and deferred callback
  reassignment remain fail-closed.

- Qualified module function values passed through local callback aliases now
  preserve their pure or IO row when invoked through a generic `@effects(e)`
  helper.

- A named module re-export of an IO callback retains its IO row through a
  local alias and generic callback invocation.

- Returned lambdas that capture scalar factory parameters now get a known row
  from their body summary.

- A direct returned-lambda factory now defers its callback argument row to the
  returned closure and substitutes known captured `fnptr` arguments into the
  lambda summary.

- Callback rows now survive capture through immutable dictionary literals and
  `get("key")` lookup, and through immutable list literals with constant-index
  reads. Facts are copied through immutable local container aliases.

- Wired the 10k+ instruction monolithic and multi-block native backend scaling
  driver into the regular POSIX `./make test` path. It now checks machine
  lowering, register allocation, and object emission separately instead of
  leaving the stress guard as an uninvoked source file. Its CFG case now
  covers 2,500 blocks and at least 10,000 machine instructions. These
  measurements validate the existing budgets; they are not a runtime speedup
  claim.

- Fixed list length queries for registered native typed buffers. `rt_len`
  previously tested the C-string heuristic before the typed-buffer header, so
  a list beginning with zero appeared empty despite valid indexed elements.

- The `float-cmp-oracle` O3 native hang was fixed: after a fixed-register
  operation loads an operand, native lowering now spills any live values that
  operand loading re-seeded in the operation's scratch registers before
  clobbering them.

- Fixed a live-range corruption in x64 fixed-register arithmetic. Loading a
  dividend can re-seed its assigned physical register after the initial
  clobber flush; the quotient/remainder lowering now commits those newly
  resident values to their canonical homes before clobbering scratch
  registers. Reciprocal remainder remains enabled.

- Reduced register-allocation loop-edge discovery from a full machine-block
  scan per loop header to scanning that header's already-built predecessor
  list. The predecessor list preserves layout order; duplicate branch edges
  are deduplicated and the existing 256-edge limit and eligibility checks are
  unchanged.

- The overflow result is pinned to `INT64_MIN`; bounded recursion specializes,
  while unbounded recursive, effectful, and malformed calls remain out of
  line. This adds evaluator-boundary coverage, not general recursion
  specialization or effect-summary support.

- Closed the straightline SMT translation-validation gap for float operations:
  `nyir_smt_eval_straightline` now models f64/f32 arithmetic, sqrt, sin/cos
  (shared uninterpreted bit-level functions), f64/f32 comparisons, and all
  int↔float conversions with exactly the encodings the CFG path already uses,
  so straightline i64-returning rewrites with float intermediates
  (scaling-then-truncate idioms) reach the solver instead of the bounded
  interpreter. The straightline path also gains the same returned-float
  NaN-payload guard the CFG path uses.

- Removed a duplicated nested `ok && !nyir_pass_stopped` guard around the O3
  SSA loop-unroll pass. It did not change runtime behavior; it obscured the
  pass pipeline and made the guard structure misleading.

- CFG translation validation now models `NYIR_SIN_F64` and `NYIR_COS_F64` as
  shared uninterpreted functions over complete input/output bit patterns. This
  soundly admits congruence proofs without inventing transcendental identities
  or constraining NaN payloads.

- Native inliner diagnostics now distinguish a cost-model-approved candidate
  from a clone that was actually applied, including inserted instruction count
  and result value. This confirms the remaining gap is downstream loop
  specialization/vectorization and machine code quality, not a missing inline.

- Centralized invalidation of NYIR range and alias facts after rewrites that
  change an instruction's operation or operands. Pipelined-loop clones, rotate
  idiom recognition, parity comparison rewriting, fast-math reciprocal
  factoring, and scalar-to-SIMD rewrites now share this helper. Reciprocal,
  rotate, parity, and vector-reduction contracts seed stale range and alias
  metadata and confirm both are cleared; strict IEEE mode remains unchanged.

- Expanded optimizer pass-order validation from 120 orderings over three
  arithmetic-only shapes to 120 orderings over six shapes, adding alias-aware
  memory forwarding, mixed f64/i64 conversion, and loop-carried CFG cases.

- Enabled `NYIR_SQRT_F64` in the pure-IR TV eligibility gate, matching its
  existing exact SMT encoding instead of silently treating sqrt-containing
  CFGs as out of scope.

- CFG SMT translation validation now models f32/f64-to-i64 with truncation
  toward zero and tracks the valid finite signed-i64 input range separately
  from the solver's total bit-vector result.

- Native scalar-list append now carries its proven post-append length through
  the fast/slow result merge into the mutated local's companion slot.
  Reassigning `a = append(a, value)` reuses that count instead of calling
  `rt_len` on every loop iteration. Indexed-setter length validation remains.

### Language

- Added a strict-types negative case for an `any` binding named `std` that
  shadows the imported module; unresolved dynamic member access remains an
  error under lexical shadowing.

### Performance and sound

- Safe-mode raw-memory validation no longer treats `assert_compile_range` as
  an unchecked assumption. It verifies exact ordered bounds and proves the
  current value range is contained before using the assertion as a raw-index
  fact; unknown or false claims fail closed.

- Added a shared pre-backend check for `assert_compile_range` inside `while`
  loops. It derives an interval only from an exact start, exact `<`/`<=`
  bound, positive constant step, a single direct index write, and a
  side-effect-safe body; list-literal `.len` bounds are recognized. False
  claims and unprovable loop starts now fail before backend selection. A
  direct positive `xs.len` loop prints `60` on both routes.

- Fixed an unsound x86-64 scalar-emitter RAX-forwarding case where a
  single-use producer was followed by a coalesced same-home COPY that emits no
  machine instruction. The forwarder had treated the COPY as consuming the
  transient `%rax` value, skipped the producer's canonical home store, then a
  later consumer reloaded an uninitialized slot. It now retains the store for
  that exact no-op-copy shape.

- Fixed an O2/O3 dynamic-index miscompile where synthetic ICP profile steps
  triggered block layout despite zero per-block heat, causing a meaningless
  CFG reorder. `nyir_block_layout` now leaves zero-heat functions unchanged.

- Fixed three x86-64 f64 oracle crashes: the F64 indirect-load emitter ignored
  a forwarded address held in `%rax` and reloaded an uninitialized stack home.
  It now uses the shared cached-address helper.

- Re-enabled profile-guided NYIR block layout behind its existing CFG ordering
  checks and candidate verifier.

- The x64 encoder no longer rescans scalar register homes after a block
  terminator, and its pre-branch materialization skips flags-fused compare
  results that have no stack consumer.

- Refined GVN's memory barriers so observable clock/IO effects and fresh
  allocation do not automatically invalidate proven stable reads. Added exact
  runtime summaries for `rt_monotonic_ns_raw` and `rt_len_strict`, including
  the latter's read, allocation-on-error, and trap effects, plus optimizer
  checks that retain calls while forwarding a stable symbol read across them.
  Allocation still blocks forwarding through unknown pointer roots, whose
  heap-object lifetime is not established.

- Bounds-check elimination now removes the canonical negative-index
  normalization select when every store to a non-address-taken source local is
  a nonnegative constant, SCEV proves the value's interval is nonnegative, or
  a dominating signed-comparison edge proves nonnegativity at the select. The
  edge proof requires the guard target block to have that branch as its sole
  predecessor, preventing an alternate path from bypassing the comparison. In
  `cmov-sort`, four normalization selects disappear from the hot sort path.

- GVN-PRE now checks memory stability across loop re-entry before reusing a
  dominating load.

- Software pipelining now clears inherited range and alias facts from each
  cloned stage-A instruction after its operands are remapped. In particular,
  the look-ahead clone may evaluate at `iv + 1` after the original loop's last
  iteration, outside the source expression's proven range.

- SLP now checks the immediately following store when searching for adjacent
  store groups; the prior index increment skipped that candidate. Packed
  vector memory and arithmetic operations retain a representative source
  location.

- Reduced x86-64 carried-register lookup work by searching each virtual
  register's contiguous allocation segment range rather than scanning the
  whole function's segments.

- Expanded the compile-only backend stress guard from a single 10k-instruction
  block to also cover 10.3k machine instructions across 128 control-flow
  blocks.

- Extended verifier-backed SSA full unrolling from unit strides to exact
  positive strides for loops with known trip counts up to 16.

- Fixed native `remove`/`delete` lowering for dynamic collection receivers:
  native lowering now specializes only when the receiver is proven to be a set
  or dictionary, and otherwise uses a checked runtime layout dispatch. The
  helper preserves receiver aliasing, handles native and managed dictionaries,
  and records its memory effects and borrow contract.

- Fixed the sound-source compiler RSS blowup in `_poly_blep` by declaring its
  private float-only inputs as `f64`, avoiding dynamic-value arithmetic.
  Native lowering also trusts a concrete NYIR float producer over a stale
  BigInt AST classification when selecting a comparison ABI. Added opt-in
  statement-level resource checkpoints, restricted by
  `NY_TRACE_NATIVE_BUILD_FILTER`, to localize future pathological function
  lowering without flooding normal builds.

### Diagnostics

- The runtime ownership ABI now explicitly marks OS/resource release arguments
  as consuming for close, socket, websocket, directory, dynamic library,
  kqueue, canvas, mutex, and Windows change-notification helpers;
  `rt_zfree_raw` also consumes its allocation pointer. Raw memory copy,
  compare, fill, and file read/write bridges now mark pointer operands as
  borrowed while leaving scalar fd/length operands unknown; raw file open and
  file-exists borrow their path. C-string concatenation, equality, length,
  replacement, conversion, and indexed-access helpers borrow their input
  pointers. Other explicit borrows cover runtime string formatting/printing,
  bytes/range/typed-buffer reads, dictionary lookups, canvas text, and
  in-place string/list helpers. Result-aliasing getters declare
  `returns_borrow` for both the receiver and a fallback that may be returned;
  byte/canvas mutations do so for their receiver. Canvas text borrow metadata
  is recorded at formal argument 3, with intervening scalar arguments left
  unknown. `rt_any_to_cstr`, `rt_to_str`, and `rt_cstr_to_str` also mark their
  input as `returns_borrow` because already- string inputs pass through
  unchanged. Raw pointer arithmetic, memory loads/stores, and copy/fill
  helpers now record borrowed addresses and returned aliases; store helpers
  that return their value preserve that borrow relation. List item reads now
  declare a conditional result alias to the list; item stores borrow the
  receiver and declare the returned stored value as an alias of argument 2.
  Direct BigInt ABI inputs now declare borrows for arithmetic, comparison,
  formatting, conversion, bit count, and number-theory helpers;
  `rt_bigint_submul` records the conditional returned alias of its first
  argument.

- Extended the runtime argument ownership table to complex arithmetic and
  accessors, membership and sequence length, generic indexed lookup, value
  inspection, vector reads, and vector fallback results. Clear/clone/set
  conversion now declare conditional receiver aliases.

- Dynamic `rt_any_*` conversions, comparison, length, and arithmetic now
  declare borrowed inputs. Result constructors borrow payloads, while unwrap
  helpers distinguish a possible payload/fallback alias from raw scalar
  fallback arguments.

- Set insertion/removal helpers now declare their mutable receiver as a
  possible returned alias and their keys as borrowed; generic dynamic addition
  records its in-place set receiver alias.

- Public FFI adapters now reject captured Ny closures as well as marked Ny
  function pointers before C dispatch.

- Added opt-in `NY_TRACE_NATIVE_BUILD` checkpoints for function parameter
  setup, body lowering, and finalization. The hook is disabled by default and
  was used to replace an incomplete per-function memory diagnosis with
  whole-harness evidence.

- NYIR calls now keep actual argument ownership separate from formal
  `borrows`, `consumes`, and `returns_borrow` dispositions. Native lowering
  records the resolved source-function disposition on each logical argument's
  first ABI slot using two bits per argument; a returned borrow includes the
  borrow relation. Binary v18 serializes it, and verification, semantic
  fingerprints, and CSE keys preserve/check it. Runtime value ABI declarations
  now populate formal borrow metadata for the flat native dictionary bridges:
  borrowed inputs are marked `borrows`, and an aliased result marks its
  borrowed receiver as `returns_borrow`. Other synthetic/runtime call
  constructors remain unknown pending explicit contracts. The canonical
  runtime argument ABI now declares borrowed formals for audited dictionary
  and typed-buffer helpers, and source calls plus compiler-generated 3-, 5-,
  and argv-call constructors propagate them. A verbose `list.len` compile
  shows `rt_tbuf_len_raw`'s borrow contract in NYIR before and after
  optimization; a dictionary lookup trace shows borrowed receiver/key slots on
  `rt_dict_get_raw` after optimization. String-builder calls now declare
  append as a borrowed receiver result with a borrowed input, conversion as a
  receiver borrow, and free as consuming its builder. The native C-string
  self-concatenation builder now records borrowed initial text, borrowed
  append suffix, returned-borrow receiver, and consuming finalize contracts.
  Managed and raw memory free helpers now consume their pointer argument;
  `rt_init_str` returns a borrow of its pointer. RC retain/count helpers now
  borrow their pointer, release/drop consume it, and drop-owned-slot borrows
  the slot address while consuming its stored value. A full source search
  audited all eight NYIR CALL constructors: ordinary source calls and shared
  runtime call emitters propagate declarations; zero-argument generated
  entry/print calls have no argument lanes; the optimizer-generated memset and
  prefetch calls retain unknown ownership on raw-address operands because no
  source ownership fact is available.

- RC runtime calls now have explicit optimizer effect summaries: retain reads
  and writes the synchronized RC registry and may allocate; count reads the
  registry; release/drop read and write ownership state and may trap while
  freeing; drop-owned-slot also writes its addressed slot.

- CFG translation validation now lowers f32/f64 arithmetic, comparisons,
  square root, integer-to-float conversions, and f32/f64 width conversions
  through Z3's IEEE floating-point theory. Branch-sensitive f64 comparison and
  f32/f64 arithmetic counterexamples verify the path-sensitive model. Returned
  float bit patterns and float-to-integer conversions remain on the
  conservative bounded evaluator path because NaN payloads and invalid integer
  conversions need explicit semantics.

- GVN now refreshes a same-key candidate when its prior definition fails the
  dominance or memory-stability proof. Later loads can be checked against the
  newer definition instead of repeatedly missing against a stale one. GVN now
  consumes summarized read-only user-call effects for commoning, blocks reuse
  across a writing call, and includes call result/effect contracts in the
  value key.

- GVN now tracks explicit pointer provenance through COPY and pointer
  arithmetic when proving whether a direct local-slot store can invalidate a
  memory read. A store to a local slot no longer blocks reuse of a read
  through a proven symbol/allocation address; local-address reads retain
  exact-slot barriers, and unknown/load-derived pointers remain conservative.
  `gvn_pre` covers a proven non-local read with unrelated local writes and
  address-taken local reads.

- GVN's opt-in `NY_TRACE_GVN_MISSES=1` diagnostics now include canonical
  byte-load addresses and value-number lookup results, alongside per-load
  misses. This makes it possible to distinguish a mismatched expression key
  from a later dominance or memory-stability rejection. The compact `GVN_PRE`
  summary remains available in verbose traces; filtered trace-mode checks
  print identical program output with or without the diagnostic override.

  The larger JSON parser trace has been refreshed after correcting a lowerer
  validation regression; see the following entry.

- Fixed missing braces in native lowerer argument guards. Valid `__env_get`,
  `argv`, environment-count, float-memory, tty, callback, panic, and defaulted
  call paths had returned failure unconditionally after the validation `if`.
  The fresh O3 trace again shows 41 GVN replacements, 6 dominance misses, and
  4 memory-proof misses; repeated byte loads remain an open optimization task.

- DSE's allocation escape analysis now includes expanded CALL stack arguments,
  so an allocation passed after the six inline argument slots keeps its
  stores. A malformed nonempty argument vector conservatively keeps all
  allocation stores.

- Redundant-load elimination now uses the `COPY_STRUCT` destination's alias
  class to invalidate only potentially overwritten cached addresses. Its
  source operand remains read-only; unknown destinations flush conservatively.
  It also keeps a load's address class separate from the loaded value's
  provenance: a pointer loaded from memory remains unknown for subsequent
  alias decisions unless an explicit value fact is available. The other direct
  aggregate-copy consumers were audited: DSE records source reads, ADCE
  preserves the operation, loop reordering rejects it, and CFG simplification
  treats it as observable.

- Alias escape analysis now follows function-local pointer provenance through
  `COPY`, `ADD_I64`, and `SUB_I64` before checking call arguments. A local
  address passed directly or through those derivations to any inline or
  expanded call argument is conservatively escaped. Provenance is propagated
  in one reverse SSA scan. Returned local pointers and pointer values stored
  into memory are escape roots as well. `alias_contracts.c` covers direct,
  copied, and offset-derived pointers at inline/expanded call arguments,
  returns, and memory stores; alias, redundant-load, DSE, and effects drivers
  pass.

- Central NYIR rewrite helpers now clear derived alias classes when replacing
  an instruction with `COPY` or an integer/float constant, while preserving
  semantic class, ownership, mutability, source location, and applicable range
  facts. Previously a folded pointer-derived scalar could retain the pointer's
  nonzero alias class and mislead later memory passes.

- Private-entry-global promotion now invalidates address alias classes when
  rewriting global loads/stores to local-slot operations. Its removed
  `ADDR_SYMBOL` becomes a fully reset `NOP` while retaining only the source
  location.

- SROA's pointer-to-local promotion clears address-derived alias metadata when
  rewriting memory loads/stores to local-slot operations. Removed instructions
  use the full NYIR discard helper so side metadata is reset.

- Redundant-load forwarding now rebuilds `LOAD_LOCAL` as a `COPY` through the
  shared rewrite helper and assigns only the source value's proven provenance.
  Previously it retained the old slot-access alias class, which could make a
  scalarized pointer value look like the local slot.

- GVN-PRE duplicate-load forwarding now uses the shared COPY rewrite contract
  and carries the dominating value's alias provenance.

- `alias_contracts` seeds those facts and verifies the replacement constant;
  `alias_contracts`, `gvn_pre`, and `pass_order` pass.

- Counted-loop idiom reduction now clears alias metadata when replacing
  `MUL_I64` with `SHL_I64`, matching the alias contract that only COPY and
  pointer ADD/SUB propagate address provenance.

- E-graph extraction now rebuilds constants through the shared rewrite helper
  and clears alias metadata on other rewritten expressions pending fresh alias
  analysis.

- Peephole rotate recognition now clears alias provenance when replacing
  scalar shift/OR expressions with `ROR32_I64` or `ROR_I64`.

- Loop vectorization now clears scalar alias provenance when it widens an
  arithmetic result or COPY into vector values, including reduction updates
  rewritten as vector reductions.

- Common-divisor floating-point factoring is now gated by explicit fast-math:
  reciprocal multiplication can change strict IEEE rounding and exceptional
  behavior. Rewritten divisions also clear stale alias metadata and refresh
  opcode effects. `peephole_reciprocal_contract` checks strict-mode retention,
  fast-math factoring, verifier validity, and alias invalidation.

- The shared f32 constant rewrite now retains source debug location as well as
  scalar representation and ownership metadata, and explicitly clears every
  unused operand.

- PHI elimination now resets alias metadata when replacing a value-producing
  PHI with a local-slot load, and preserves its range, semantic ownership,
  mutability, and debug location.

- Strength reduction now preserves scalar class, semantic ownership,
  mutability, range, and source location on the replacement result while
  clearing the original operation's alias provenance. This applies to
  multiply/add-to-shift rewrites and signed/nonnegative divide or modulo
  rewrites.

- Constant folding and correlated value propagation now route constant
  rewrites through the shared constructors, so alias classes are invalidated
  and semantic/range facts are preserved consistently. The CVP regression also
  exposed a lost-fact bug: unioning a COPY destination with its source changed
  the representative before reading the source's affine constant. CVP now
  snapshots that fact before union and propagates it to the merged root.
  `alias_contracts` verifies the constant result, cleared alias class, and
  retained metadata; `pass_order` still passes.

- Corrected the packed call-contract setter to use thirty-two two-bit entries
  per word. The old `/16` word index misplaced arguments 16–31 and wrote
  beyond the contract array for arguments 32–63. `effects.c` round-trips all
  64 slots. The native `rt_tag` helper is now classified as a pure scalar
  call, and ADCE drops unused effect-free calls while retaining unknown and
  trapping calls. This removes a dead `rt_tag` call from each Fibonacci
  recurrence iteration; the focused result is noisy and no stable speedup is
  claimed yet.

- The NYIR runtime-call effect table now marks `rt_native_is_int` as a memory
  reader because its representation checks inspect heap registries and
  candidate object headers.

- Raw memory builtins and their NYIR runtime helpers now report memory reads,
  writes, and possible traps accurately. GVN-PRE reuses equal raw loads before
  a write and invalidates them after it; the effects and GVN-PRE C drivers
  cover those contracts.

- Its value numbering follows COPY chains, including copies of PHIs, when
  matching equivalent operands. The focused `json-parser` measurement did not
  improve, so this is a correctness and missed-CSE fix without a claimed
  runtime gain.

- Every pass boundary is verified and evaluated, then repeated until the
  semantic fingerprint reaches a fixed point.

- Native register-allocation loop-edge coalescing now resolves candidate
  segments through the existing block index instead of rescanning each vreg's
  full segment list.

- `NY_DUMP_REGALLOC=1` now reports segment live-in/live-out state and named
  spill reasons; `NY_DUMP_REGALLOC_CONFLICTS=1` shows loop-edge color
  evictions and the retained segment.

- The x86-64 scalar machine emitter now handles immediate integer divisors
  directly instead of reading a nonexistent virtual-register home. This
  removed a wrong-result/SIGFPE case in optimized control flow. Signed
  power-of-two division uses sign bias plus `sar`; remainder reconstructs `x -
  trunc(x / d) * d`, preserving the dividend's sign.

- NYIR translation validation skips solver work when a pass leaves the
  semantic IR fingerprint unchanged. The CFG SMT evaluator also simplifies
  expressions and prunes branches whose conditions become constant.

- The induction-variable eliminator now folds an even constant-trip reduction
  that selects between two constants using `(iv + invariant) & 1`, when the IV
  step is odd. Each adjacent pair contributes both select arms, independent of
  starting parity, so the rewrite uses wrapping i64 arithmetic and keeps
  odd-trip, even-step, and dynamic-bound loops unchanged.

- The rotate peephole now matches shift operands through intervening NYIR
  copies using same-value analysis, so SHA `rotr` expressions become `ROR32`
  instead of remaining two shifts and an OR.

- The range narrowing pass now accepts proven `uint32` intermediates and
  recognizes exact `& 0xFFFFFFFF` boundaries. It propagates low-word codegen
  through add/sub/mul/bitwise expression chains only when every use stays in
  the low-word region, leaving values with a wide-use escape unchanged. The
  `optimizer/narrow.c` driver guards unsigned boundaries, overflow rejection,
  masked unknown values, and escaping uses. This is a small result, not a
  parity claim; both remain far slower than C.

- Peephole now removes canonical zero/one normalization around boolean
  comparisons. It reduces `cmp != 0`, boolean `& 1`, and comparisons against
  zero/one back to the original or inverse i64 predicate, while leaving float
  complements that need unordered predicates untouched. This removes redundant
  compare/and instructions in the `json-parser` digit loop.

- Kept redundant-load forwarding block-local after an O3 `heapsort` regression
  showed that ignoring CFG backedges can reuse a load across loop iterations
  where a PHI changes the address.

- The x86-64 scalar machine-form gate now accepts immediate operands for
  `SHL`, `SAR`, `ROR`, and `ROR32`; the encoder materializes the masked count
  in `RCX` before the variable-count instruction. Immediate shifts had been
  incorrectly rejected, sending integer kernels to the older NYIR emitter.
  `NY_DUMP_MACH=1` now reports the first ineligible machine instruction when
  that fallback occurs.

- The x86-64 scalar machine-form gate accepts `i64 = ptr + i64` for typed
  buffer byte-address calculation; the emitted operation is ordinary 64-bit
  addition. Without this case, runtime-guard and affine typed-buffer vector
  loops were rejected by the machine backend and then failed in the NYIR
  fallback.

- The native i64 if-converter now follows up to four empty forward branch
  trampolines on diamond edges and accepts arms with up to eight safe scalar
  operations, including a fallthrough arm. A proven raw-i64 truthy condition
  may become an explicit `!= 0` comparison before SELECT, but only when the
  two arms together contain at most two instructions; larger speculation is
  rejected after the `loop-unswitch` kernel measured slower with both arms
  evaluated. Other conditions still require an existing boolean proof. The
  existing last-write loop reduction continues to reduce the benchmark's 100
  iterations to one, while retaining its predictable branch under this cost
  gate.

- Native JIT compilation now appends generated function address/size/name
  records to the Linux perf map when `--jit-perf-map` or profiler mode is
  enabled.

- `NYTRIX_JIT_CODE_DUMP=1` now saves the exact relocated JIT image and a
  sidecar mapping function addresses, sizes, image offsets, and names. A
  scaled `binary` perf capture used it to map the hottest sample to the
  signed-division sign-mask temporary. Image I/O stays opt-in and best-effort.

- `NY_DUMP_FN` also prints selected functions' final NYIR after cross-function
  inlining, PHI elimination, and scalar-class reconciliation, immediately
  before machine lowering. This distinguished a correct solver key operand
  from the backend's stale register/home write; normal output is unchanged.

- Mem2reg now rejects nested-join shapes with unproven downstream parity
  before cloning or inserting labels/zeros, preserving the input fingerprint
  exactly and tracing the downstream parity reason.

- Requested allocation counters are emitted once on explicit cleanup or
  process exit.

- Native-only compilation now honors `--emit-asm` before returning from its
  early route, including `-emit-only`, execution, and cross-target diagnostic
  requests. Assembly write failures return a nonzero status.

- `NY_DUMP_FN=rt_main` now prints actual synthesized-entry NYIR snapshots
  around inlining and after the final post-inline optimizer; the leaf-wrapper
  snapshot is named explicitly. `--nyir-dump` remains the AST-derived
  diagnostic view.

- The `loop-unswitch` benchmark's C reference no longer uses `volatile` for a
  local value whose intermediate writes are unobservable in Nytrix. Added the
  O2/O3 `loop_last_overwrite` NYIR pass: for a pure, constant-trip loop with
  one canonical induction variable and carried outputs overwritten solely from
  that IV and invariants, it seeds the IV at the proven final iteration and
  executes the observable body once. It rejects unknown/zero trips,
  reductions, trapping/effectful bodies, unsupported exits, and
  unrepresentable final IVs.

- The list benchmark's C reference now performs geometric dynamic growth with
  `realloc`, matching Nytrix's repeated `append` rather than preallocating the
  final array.

### Correctness

- The bigint classifier no longer treats every `any`-returning call as a
  BigInt handle.

- Bigint comparison is register-class coherent. The compare bridge now detects
  a float producer on either side and resolves the contradiction as a
  machine-f64 comparison — decoding tagged dynamics, widening f32, or
  promoting raw ints as needed — mirroring the arithmetic bridge's existing
  rule.

- The NYIR runtime-call effect table now describes `rt_cstr_to_str` as a
  checked read of caller-provided string bytes plus allocation. It no longer
  falls through to `CALL|FFI|UNKNOWN_SIDE_EFFECT`; allocation, memory-read,
  and trap effects remain explicit so optimizers keep their existing safety
  barriers.

- Dynamic-arithmetic producer classes are now coherent with float slot and
  argument contracts across the native backend. When operator lowering kept a
  dynamic operand in the canonical tagged domain (the `rt_any_mul` bridge and
  siblings), four boundaries previously consumed the tagged handle word as a
  number: the binary f64 promotion SIToFP-converted it, the dynamic-bridge
  boxing and the untyped-return box took IEEE bits of the handle, and the call
  funnel's f64-argument promotion treated it as a raw int. Each boundary now
  decodes via `rt_any_to_f64` when the register class says TAGGED, so `mut s =
  list.get(0, 0.0) * sin(1.0)` and `s +=...` produce identical, correct
  results on the tree-walking VM, the NYIR VM, and native x86-64 (previously
  the three backends disagreed by up to 1%).

- A violated `requires` contract now fails closed on the native path with the
  real `E2001` diagnostic instead of corrupt NYIR. The obligation failure
  returned `ny_native_nir_fail`'s bool result, and `false` doubles as NYIR
  value id 0, so the caller lowered `to_str(<violation>)` into a
  self-referential call (`call rt_i64_to_cstr_raw v0`) that the verifier
  rejected with an unrelated message. The site now propagates the `-1` failure
  sentinel.

- The NYIR VM no longer double-invokes unmarked callables at callback arities
  5-15. The VM's callback switch executed the resolved target for arities 5-15
  and then fell through to the generic dlsym path, which invoked
  `rt_call_anyN` a second time and overwrote the result — doubling side
  effects and keeping the second return.

- Intrinsic policy is declaration-driven on the active native path.

- The numeric and callback bridges are reconciled across backends. The LLVM
  NYIR emitter implements `NYIR_F64_TO_I64` via `FPToSI` (previously any NIR
  containing the opcode failed emission while the VM and machine paths
  executed it), and the JIT binds `rt_call_any3..15` alongside any1/any2. The
  complex family now has complete ABI rows (arithmetic, predicates, and
  `rt_complex_new`'s boxed-flt tagged arguments), `complex` classifies as the
  object-handle representation it is in the shared semantic layer, and the
  `__complex_real`/`__complex_imag` aliases document why the raw-f64 mapping
  matches the stdlib's declared `f64` returns. The `.long` receiver
  classification is one shared helper (the memcall copy was missing the bigint
  exclusion the member copy had), the stale "tagged small int" contract
  comment is corrected, and the two undeclared simmd helpers carry ABI rows
  matching their C signatures.

- The native backend now derives its wide-int, extended-float, alias,
  nullable, list, and call-boundary ABI decisions from the shared semantic
  fact layer instead of native type-name fallbacks. `ny_type_is_bigint` is the
  shared wide-int predicate (both `bigint` spellings, nullable-stripped,
  qualified-leaf aware) consumed by the bigint classifier, param/return slot
  decisions, and call-boundary boxing. The native float predicates consult
  `ny_semantic_rep_for_type` before the spelling check, so `number` and
  `numeric` annotations own the machine-f64 ABI everywhere instead of falling
  to the raw-int path. Function results, callback raw-int markers, lambda
  callback ABI, and the direct-call result class prefer a resolved and
  coherent `return_semantic` over the declared name, with coherence guarding
  stale facts; i128/u128 keep their non-raw-i64 exclusion.
  `ny_native_nir_primitive_layout` requires a container boundary character
  after `list`/`dict`/`set` prefixes so user types whose names merely start
  with those letters (for example `listx`) no longer get pointer-sized
  primitive layouts. Lambda and unresolved-call semantic stamps derive from
  `ny_semantic_rep_for_type`, so aggregate, pointer, and typed-buffer results
  stop being stamped raw-int.

- Machine-form home assignment no longer treats a PHI edge destination as a
  pure SSA alias. After `mach_lower_phi_edges` a phi result carries one COPY
  per incoming edge, so it is multi-defined and each edge aliases a different
  producer. The cross-block copy-chain walk now stops at multi-definition
  vregs, which keep private homes, and constant rematerialization refuses
  multi-def values. The plain-NIR x86-64 emitter applies the same rule to its
  COPY chain homes and const fusion.

- Native x86-64 `SELECT` emission now preserves its predicate while
  materializing the false arm. The old order tested the condition before
  loading that arm; zero rematerialization can use `xor`, which overwrites
  flags and selected the wrong value. The encoder now keeps the predicate in
  reserved R10 and tests it immediately before `cmovne`.

- Verbose heuristic output reports candidate trip count, rejection reason, or
  the applied final IV.

- `loop_last_overwrite` now validates every header-PHI preheader/backedge pair
  before its first edit. A rejected candidate therefore leaves its IR and
  fingerprint untouched; the former late rejection branch could only occur
  after partially rewriting the CFG and also referenced freed analysis
  buffers. DCE removes the loop test and induction PHI after the
  single-iteration rewrite; the live TODO tracks the remaining native/LLVM gap
  and general invariant-branch cloning.

- Runtime-bound induction-variable reduction rewrites now mark generated
  integer division/remainder operations with their required trap effects. This
  lets the verifier accept the closed-form formula after inlining instead of
  retaining the hot loop. Historical focused measurements remain as earlier
  snapshots, not the current acceptance result.

- Integer division and remainder now report `MAY_TRAP`; ADCE keeps these
  operations even when their results are dead.

- The effects, ADCE, CSE, GVN-PRE, and redundant-load drivers pass.

- Alias analysis now assigns address classes to all scalar and vector
  single-address load/store opcodes, including vec4/vec8 i64 accesses.
  `optimizer/alias_contracts.c` guards same/different global roots, unknown
  addresses, each memory opcode, all tested call positions, and reanalysis
  after rewriting a pointer producer to a scalar. Reanalysis clears the prior
  generation before assigning fresh facts.

- Unknown facts remain conservative. `pass_order` now includes ADCE in its
  reordered cleanup and alias/memory pipelines.

- NYIR CALL now carries packed per-argument ownership facts. Ordinary native
  calls record resolved source ownership; binary format v17 preserves it, the
  semantic fingerprint and CSE key include it, and verification rejects
  conflicts with known value producers while accepting unknown producers.

- Formal borrow contracts from canonical runtime callback and FFI ABI
  declarations now survive all three native call-construction paths, including
  calls whose expanded argument list uses NYIR's extra-argument storage.

- Runtime calls that return the same logical container now mark the receiver
  formal as `returns_borrow`: typed-buffer append, clear, set, and swap, and
  dictionary clear and delete.

- Verbose NYIR dumps now print known per-argument ABI classes and formal
  ownership dispositions beside each call, making expanded call contracts
  inspectable in the same compact instruction row.

- A missing-chain guard in attached-method suffix lookup prevents native
  `--nyir-dump-stats --nyir-pass-stats` diagnostics from following a
  zero-initialized function index as if it were a built snapshot.

- Debug function filters now preserve nested context push/pop even when a
  context line is filtered, and native raw O3/pass-stat dumps honor
  function-only selection. An impossible function filter emitted no compiler
  trace records.

- This establishes a repeated-read opportunity while leaving the bounds and
  memory-stability proof requirements explicit; no runtime gain is claimed.

- Native local inference now lets a concrete f32/f64 producer ABI clear stale
  list-shape metadata and its companion length slot before storing the result.
  This handles stale inferred list types even when the local's current
  `is_list` classification is already false, preventing a float call result
  from receiving a typed-buffer length query.

- Native call-effect lookup now uses a sorted symbol index instead of scanning
  every reachable function for each call. The use-alias result cache grows
  instead of silently missing recursive-resolution results after its fixed
  probe window fills.

- Native reachability now collects the concrete receiver-owner method as well
  as a general `any` candidate, so typed `set.add` calls include the wrapper
  body emitted by native lowering.

- General NYIR inlining now recomputes caller-local alias classes after a
  successful splice. Callee alias IDs are scoped to their original function
  and could otherwise collide with caller allocations, misleading later memory
  optimizations.

- Common-subexpression elimination now reuses calls only when the IR marks
  their effect summary trusted and the summary contains only local reads,
  control, traps, and the call itself. Memory reads, mutation, allocation,
  I/O, FFI, threading, and unknown effects remain excluded; writes clear the
  block-local table. CSE rewrites preserve result scalar ABI, ownership,
  mutability, alias, source location, and range facts.
- Within a basic block, CSE also resolves representation-preserving raw-i64
  copies when their scalar class, ownership, mutability, and alias metadata
  match. Tagged, pointer, aggregate, and contract-changing copies remain
  distinct.
- On the O3 binary-trees fixture, this merged the two identical recursive
  calls into one while preserving the 4057 checksum. A serial 3+1 run measured
  NYR 218 us versus its 237 us baseline; the sample ranges did not overlap.

- COPY/PHI producer consistency is now covered separately; stores, loads, and
  calls still need explicit ownership contracts.

- NYIR verification now rejects a known ownership annotation on a COPY or PHI
  when it contradicts the known producer/incoming ownership. Unknown ownership
  remains a valid conservative result.

- x86-64 immediate rematerialization now respects fixed-register clobber
  boundaries in both arithmetic and shift-immediate selection. Also bounded
  two standalone encoder-test relocation arrays: those fixtures have no
  external relocations, and the production maximum had overflowed their
  stacks.

- Explicit native f64/f32 local declarations now reassert their storage
  carrier after expression-semantic joins and immediately before the store.
  This preserves the widened f64 ABI for f32-returning calls and the declared
  f32 ABI symmetrically.

- The nested-join mem2reg gate remains enabled pending dedicated downstream
  SSA coverage.

- Native indexed stores now honor an explicitly declared top-level `list<int>`
  element ABI even when its initializer is the generic `list(size)` allocator.
  This keeps writes at 8-byte integer stride and prevents later
  descriptor-slot promotion from corrupting direct typed-list accesses in
  `sift_down`.

- Native vector arithmetic dispatch now checks the lowered operand class
  before calling vector helpers. This prevents scalar integer multiplication
  in loop bounds (`d * d <= n`) from becoming a vector dot product and hanging
  the prime benchmark.

- Member `.get` lowering preserves the tagged receiver ABI for dynamic
  producers, including nested sequence indices, and selects the checked
  accessor rather than assuming a dictionary pointer. This fixes the
  `_has_header` receiver-class verification failure and wrong nested-list
  lookups.

- The previously recorded typed-int marker failure was an invalid ABI
  combination: `tagged_args` deliberately supplies tagged values, while typed
  `int` parameters consume raw integers. Captured native closure
  identity/environment, typed closure scalar returns, parallel-module JIT
  startup, and resolved Result payload representation now have cross-route
  parity regressions (2026-10-01).

- Explicit raw-integer callback results are boxed by contract, without
  interpreting their bits as pointer, nil, or boolean encodings. Typed
  closures previously returned `21` instead of `42` after this ambiguity
  skipped boxing and the caller decoded again. Canonical `any` list callbacks
  in the marker fixture now use the tagged-result marker, retaining their
  structural list assertions rather than claiming an integer result ABI.

- Global dynamic loads preserve their tagged storage class instead of
  advertising raw integers to comparison lowering. Dynamic equality also tags
  proven raw integer peers, including unary negatives and computed values,
  before calling the runtime comparator.

- Native block-entry register residency now follows the same eligibility rule
  as operand reads. Values read from canonical stack homes are no longer
  falsely marked resident by a carried segment; a subsequent call flush
  previously replaced a solver string key with the unrelated tag `121`.

- The x86-64 machine encoder reserves its r8-r11 scratch registers from
  allocation and uses only callee-saved GPR colors in functions with calls.
  Win64 leaf functions also exclude rsi/rdi until their nonvolatile saves are
  represented. Register metrics use the same ABI-specific pool as emission.

- Mem2reg now captures incoming parameters before branch-local stores and PHI
  placement. An untouched predecessor previously supplied zero when the first
  parameter load occurred at a join.

- Replaced mem2reg's reverse-postorder reaching-definition table with an
  iterative dominator-tree traversal that restores value stacks on subtree
  exit and fills predecessor-specific PHI operands.

- Removed its stale loop-carried register-allocation TODO.

- Native IV elimination follows the CFG's real loop-exit edge through
  branch-only glue and preserves invariant local bounds when replacing a loop.
  Runtime affine reductions now keep the induction step separate from the
  accumulator's scale when computing trip counts.

- Runtime affine reductions with an even accumulator step absorb the division
  by two into the constant coefficient, preserving modulo-2^64 arithmetic.

- Typed closure parameters no longer double-untag through dynamic dispatch;
  typed local list annotations, single-element list initializers, and
  recursive `mapcat` preserve element values across print, loops, and
  f-strings.

- Match range arms untag dynamic subjects before the ordered compare; bitwise
  operators no longer divert into the c-string path on stale string
  representations; list-repeat counts unbox dynamic values.

- Boxed-bool truthiness normalizes between raw 0/1 and canonical boxed words
  in call arguments, list literals, and `&&` operands (Rule 110 now renders
  bit-exact).

- Integer-returning `case`/match expressions decode a conservative dynamic
  join once at the function ABI; Result-pattern bindings keep the tagged
  payload ABI; try/catch joins normalize scalar arm results.

- String slices preserve the full requested range (ASCII and UTF-8, including
  open bounds); X11 UTF-8 decoding accepts the native string ABI and advances
  by encoded bytes.

- BigInt boundary fixes preserve identity, comparison, negation, and
  absolute-value handles: `Z(4).sqrt_mod(17).powmod(2, 17) == Z(4)`, large
  import/re-export compilation completes, and `.unhex`/`.long` round-trips are
  restored.

- Native x64 constant folding now follows constants through uniquely-defined,
  type-preserving machine copies in any block. The earlier int32 C version
  vectorized a narrower computation. The sparse O(vregs) block-use summary
  preserves the old cross-block home-safety query without repeated CFG scans.
  OpenMP worker teams are capped at each region's task count, avoiding idle
  workers on short regions.

- The standard-library workaround inventory is closed. `core.common`'s
  functions are genuine cross-module utilities (not compiler workarounds):
  every one is used by dozens of files, and none exists to paper over a
  compiler deficiency. `core`'s polymorphic `len`/`get`/`set`/
  `contains`/`append` functions are the language's dynamic-dispatch surface,
  not re-exports. The remaining categories (dynamic-callback markers in async,
  simmd pointer helpers, camera contract, env scans) were all resolved or
  shown to be load-bearing in prior rounds.

- Register allocation derives block intervals from values touched or live in
  that block, avoiding full virtual-register scans for every block, and the
  machine encoder uses the active block for instruction lookup. The inline
  callee registry sizes to the actual function set. Constant signed modulo
  uses the magic-divisor path, with live values in its fixed R8-R10 scratch
  registers synchronized before the sequence.

- The general inliner leaves directly recursive callees out of line. A
  separate bounded scalar evaluator folds verified direct self-recursion with
  a depth limit of 32 and shared 100k-instruction budget, declining effectful
  bodies, non-self calls, and work beyond budget.

- Redundant-load elimination now clears local forwarding facts at call
  boundaries.

- Unified SROA now ends local store/load forwarding at every call boundary.
  Escape analysis proves whether an address is retained, not whether a call
  temporarily writes through it; retaining a cached local definition across
  that boundary crashed optimized native `fannkuch`.

- CSE no longer pools constants by erasing instructions while iterating stale
  CFG ranges, and it no longer merges calls without argument-location mod/ref
  summaries. Constant pooling remains disabled until it can rewrite in place
  while retaining semantic metadata.

- Native NYIR preserves the tagged ABI when an untyped lambda returns a
  dynamic ternary: raw branches are boxed once and the joined value keeps its
  class through the implicit return. Concrete local declarations override
  stale dynamic expression facts when selecting arithmetic.

- Index lowering now carries a post-proof representation refinement into the
  NYIR semantic stamp only when the emitted SSA scalar class matches it. This
  keeps append-built typed integer reads from retaining their earlier tagged
  source stamp without propagating unrelated call-context refinements.

- A typed `Result<f64, int>` regression and `unwrap(os.file_read(path))` both
  keep their resolved payload carriers: string and handle words are stamped
  with their semantic class instead of being boxed as integers.

- Explicit `int` locals for `old_cap` and `new_cap` in
  `core.dict_mod._dict_resize` keep capacity arithmetic raw, and the
  dict-clone list-result classification no longer overrides the resolved
  dictionary type.

### Native ABI and callbacks

- The untyped scalar ABI is driven by callee semantics end to end: raw
  producers box exactly once at `any` boundaries, raw consumers decode at
  their store boundary, raw-result bridges (`rt_len`, `rt_cstr_len`, builder
  handles) stop classifying as `any`, and spawn/index/descriptor paths keep
  loop counters, integer literals, and callback pointers in their canonical
  words.

- `rt_call_any3`/`rt_call_any4` trampolines carry three- and four-argument
  dynamic callbacks; indirect dispatch selects all-raw shapes for
  statically-known callees.

- Extern/runtime bridge declarations carry per-argument f64 ABI facts; f64
  runtime-bridge declarations are no longer retyped to i64 by later bit-typed
  call sites.

- Dictionary fixes: native `.len` reads the native table count first,
  string-key reads stop double-decoding integer fallbacks, and top-level
  mutable bindings stay in the entry frame (10,000-key construction works).

- Dynamic callable IDs are treated as closure-like values in descriptor
  buffers: returned callables work through direct `fnptr` calls, `any`
  formals, and list store/read; callable arguments keep the callable's
  recorded ABI at `fnptr` calls and skip scalar unboxing for closure formals;
  integer return boundaries follow the emitted tagged scalar class when the
  callee surface is qualified.

- High-arity indirect calls share the dynamic argument preparation used by
  lower arities, so raw integer words no longer enter tagged callback formals.
  The NYIR VM resolves unmarked captured closures before they reach the C
  callback trampoline, and reads bool, tagged-argument, and raw-result ABI
  flags from the closure metadata registry, matching native routes.

- The x64 object emitter stages integer call operands before placing them in
  ABI registers. Previously an early argument move could overwrite a later
  argument's source register, corrupting four- and six-argument callbacks,
  ordinary typed calls, and chained self-return calls.

- The raw SIMD scalar runtime bridges now declare their raw integer result
  class as well as their no-boxing argument contract. Without the result
  class, leading-zero counts returned raw 63 but NYIR treated that word as a
  tagged integer and decoded it as 31.

- File open retry loops keep their descriptor and counters in explicit raw
  integer locals across backedges. The cold file roundtrip preserves odd byte
  counts and binary payloads on NYIR, MCJIT, and ORC.

- Native-only lowering now preserves proven f64 results from sine/cosine and
  f64 arithmetic, plus the raw integer result from float-to-int conversion.

- Legacy LLVM intrinsic shortcuts (sqrt, sin, cos, abs, `get`, f64
  conversions) now require the registered `core.math`/`core` module identity
  or resolved return types instead of recognizing leaf names, so user modules
  owning those spellings keep their own implementations.

### Compiler performance

- IRCE now uses its affine induction-variable interval analysis to remove
  fixed-bound checks when every natural-loop byte offset is nonnegative and
  strictly below the exclusive limit.

- Root lowering now keeps indexed reads from a mutable homogeneous
  integer-list literal in the raw i64 ABI when the consumer does not request
  `any`. The previous path read an odd raw payload such as `5`, then called
  `rt_any_to_i64` and changed it to `2`, which made native insertion-style
  loops skip comparisons. This narrow fix does not close the broader dynamic
  list specialization gap.

- The AVR floating-point runtime fixture combines its constant/stack-argument
  checks and groups comparison predicates into 11 QEMU executions instead of
  38, while retaining the checked payloads and comparison cases. Its focused
  cold run passed in 14.121 s with shared bytecode; the earlier focused run
  took 59.9 s.

- The legacy derive-expansion probe now passes the test runner's shared
  `core.bc` to its nested compiler invocation, keeping expansion on the same
  standard-library bytecode as the parent test process.

- Focused `make test` runs now reuse an existing `core.bc` automatically.
  They still avoid building one solely for a focused run when the artifact is
  absent; `NYTRIX_TEST_USE_STD_BC=1` continues to force that build.
  A cold allowance-flow fixture timed out at 180 s on the source fallback and
  passed in 78.964 s with shared bytecode (peak fixture RSS 842.7 MiB).
  A cold seven-fixture slow-flow selection passed all cases in 127.9 s wall
  time with 873.3 MiB peak fixture RSS.

- The `core.math.big` multiplication, division, and modulo wrappers now keep a
  tagged integer operand immediate instead of allocating a temporary BigInt;
  the runtime's existing small-limb arithmetic accepts that representation.
  Addition and subtraction now preserve tagged integer operands too, and
  `pidigits` passes its scalar factors directly. The spread is large and the
  native/C gap remains substantial.
- Focused, checksum-passing `bigint-oracle` and `pidigits` runs confirm the
  scalar-operand specialization remains effective: NYR measured 41.5 ms versus
  LLVM 39.8 ms on `bigint-oracle`, and 339 µs versus 470 µs on `pidigits`
  (3 runs plus one warm-up each). The remaining NYR/C gap is shared with LLVM
  on `bigint-oracle`; the measured native path is not its current bottleneck.
- Signed power-of-two division by two now uses the shared strength-reduction
  path. A checksum-identical five-run native A/B improved the `binary` fixture
  from 8.48 ms to 6.70 ms; keep this general reduction and focus remaining
  typed-loop work on the separate right-shift case.

- Explicit `./make bench --jobs N` values remain exact.

- GVN load reuse no longer treats direct local-slot assignments as arbitrary
  memory writes when the function has no `ADDR_LOCAL` instruction. Functions
  that materialize a local address retain the conservative barrier because it
  may alias a subsequent memory read.

- GVN now records the canonical source as soon as it rewrites an instruction
  into a COPY, instead of waiting for a later scan to encounter that COPY.

- GVN-PRE value keys now preserve scalar-class metadata for every opcode.
  Previously, equal immediate bits with raw and tagged representations could
  share a value number; a replacement COPY then violated a downstream call's
  argument ABI. Previously, later expressions in the same pass could not see
  the new alias; identical address arithmetic therefore prevented a safe
  repeated-load match.

- GVN can now number `LOAD_LOCAL` results and reuse a dominating read of the
  same slot across CFG edges. A write to that slot, a call, or a possibly
  aliasing write to escaped local storage blocks reuse; writes to other local
  slots do not.

- `set_idx` on a top-level integer list proven to remain an elem-8 tbuf now
  emits a guarded direct store when the normalized index is within the live
  length. Negative indexing shares the runtime's wrap rule; null, capacity
  growth, and out-of-range writes still call the runtime setter.

- Native statement lowering now recognizes a bounded overwrite-loop pattern: a
  positive constant-trip `<` loop with unit induction and a pure two-arm
  assignment to one independent scalar can evaluate its final iteration once.
  The guard requires a straight-line dominating constant initializer, raw-i64
  operands, and a trip count no larger than 1024; zero-trip and other shapes
  keep their normal CFG.

- Native lowering now uses the existing exact-size multi-fill path for
  inferred empty lists when whole-program append analysis proves an integer
  element representation. Its zero-length proof recognizes the generated
  `rt_tbuf_new_raw(0, 8)`/`rt_tbuf_len_raw` pair; the `iter` optimized NYIR
  replaces its repeated append helpers with sized buffers and direct stores.

- SysV register allocation now uses caller-saved `rsi`/`rdi` for GPR segments
  only when the segment is local, has no spill/reload boundary, and belongs to
  a block without calls. Linear and loop-edge coalescing enforce the same
  color limit, so later coalescing cannot carry those registers across a call
  or block boundary. This suggests a modest native gain, but the LLVM and C
  variation means it is not a parity claim.

- Machine lowering now shares canonical homes for same-type cross-block SSA
  COPY chains as well as coloring block-local intervals.

- The `binary` benchmark's C reference now uses explicit unsigned wrapping for
  the recurrence and bit-preserving signed division, matching Nytrix's i64
  semantics without C signed-overflow undefined behavior.

- Native machine lowering now colors stack homes for virtual registers whose
  definitions and uses are confined to one basic block, and coalesces dying
  same-type copies only when source and destination share a home safely.
  Cross-block and loop-carried values keep private homes. This reduces frame
  footprint.

- The scalar x86 text emitter now lowers signed division and remainder by a
  compile-time non-power-of-two constant with the same exact magic-reciprocal
  sequence as the NYIR and object emitters. It resolves literal divisors
  through COPY chains and keeps runtime `idiv` for unknown or exceptional
  divisors.

- Added a compile-scoped cache for native user-function name lookup. Exact
  string keys preserve the recursive resolver as a fallback for long names or
  allocation failure.

- Native enum-member lookup now memoizes positive and negative results by
  interned query name for the compile-scoped expanded program. The original
  source-order tree walk remains the fallback if cache allocation or name
  interning fails. `NYTRIX_TRACE_LOOKUPS=1` reports cache effectiveness.


- x86-64 machine lowering now recognizes the verified signed power-of-two
  division sequence and fuses it to the existing immediate-divisor machine
  form. The encoder spills only its RAX/RCX scratch colors for this sequence,
  preserving unrelated loop-carried registers. Linked counters fell from
  31.04M to 29.61M cycles and 79.00M to 76.12M instructions; generated code
  shrank 48 bytes, with 28 fewer static spill segments and 9 fewer reload
  segments.

- A compile-scoped negative import-alias prefilter indexes explicit aliases,
  plain-use leaves, and module leaves as a conservative superset. Definite
  non-alias names skip local-shadow lookup and full use-tree traversal; actual
  alias resolution retains the existing source and module precedence. The
  opt-in `NYTRIX_TRACE_IMPORTS=1` summary measured 407,709 early rejects and
  reduced full scans from 21,857 to 1,106 on `vterm-key-state-open`. This
  generated- code variation remains under investigation.

- LICM moves signed division and remainder only when value/SCEV ranges prove
  zero-divisor and `INT64_MIN / -1` traps impossible. The fallback for
  incomplete call-alias summaries is now scoped to each loop that combines
  calls with raw memory, so unrelated calls after a loop do not suppress its
  LICM opportunities. Such mixed loops remain conservative. The `120000 / d`
  condition itself remains in the inner loop because the compiler must
  preserve its explicit zero-divisor panic path; the earlier claim that this
  division was hoisted was inaccurate.

- Native COFF, Mach-O, and single-function ELF object writers now allocate
  relocation symbol/name tables in proportion to actual relocations. This
  removes multi-megabyte fixed stack frames (the ELF path reserved roughly 68
  MiB) and avoids zeroing the global relocation limit for small objects.

- Fixed 24 hanging `core.os.ui` modules with operator-resolution and re-export
  memos, a global top-level-value index, O(1) module lookups, and cross-module
  constant-alias folding that replaces dangling raw-storage symbols.

- Small String Optimization plus SIMD SwissTable probing for native
  dictionaries; constant-key record literals fold reads, `.get`, `.has`, and
  `.len` to fields without hash-table traversal.

- Proven 8-byte typed-buffer reads stay on raw loads through the SHA/ PBKDF2
  schedule paths; raw-length list finalization stops decoding odd lengths as
  tagged.

- Counted-loop unrolling now locates a candidate header across its guard's
  loads and comparison instead of requiring the label immediately before the
  branch. PHI legality is checked in the candidate loop, so unrelated
  post-loop PHIs no longer disable this transform for the whole function.
  `loop_unroll.c` verifies that case unrolls with unchanged evaluation and
  that a loop-header PHI remains untouched pending edge-aware cloning.

### Language, proofs, and effects

- Audited all `static int` functions in native NYIR lowering sources for
  direct `return ny_native_nir_fail(...)` paths. These returned boolean
  `false` as value ID 0, which is valid NYIR; all 218 such paths now preserve
  the diagnostic and return the `-1` failure sentinel. Boolean lowerers retain
  their boolean failure contract.

- Native call lowering now fails closed when a `requires` obligation is false
  or unknown. It can fold a direct immutable module literal into the
  obligation; mutable values, computed globals, and unconstrained parameters
  stay unknown until shared solver discharge is available.

- `@total` now triggers call-graph recursion analysis and is validated after
  that graph is known. Recursive claims fail closed, including claims with
  `decreases(...)`, because that syntax annotation is not a well-foundedness
  proof.

- `@allowance` grade-aware ownership tracks parameter use over `ℕ ∪ {omega}`
  with structured diagnostics.

- `comptime{}` is enforced as closed code: capturing outer runtime `mut` state
  is a structured diagnostic.

- Intentionally unknown CPS/query and captured-return factory callbacks remain
  rejected rather than guessed.

### REPL

- `core.str`'s `_env_present` no longer walks the process environment
  manually: it resolves the entry with the native `__env_get` builtin in one
  call, fixing prefix-colliding name lookups (e.g. `NY_H` matching `NY_HOME`)
  in the string-layer debug gate.

- Interactive input no longer fails with `undefined symbol 'repl_show'`: the
  echo helper is emitted into the snippet itself and uses only
  runtime-provided conversion builtins, assignments are not wrapped as display
  expressions (a wrapped assignment parsed the left side as a named argument),
  and the wrapper is skipped for non-value inputs.

### Compiler infrastructure

- `rt_fail(kind, msg)` now decodes both native C strings and short tagged
  Nytrix strings before composing its panic message; it also terminates the
  bounded output buffer explicitly. The previous heap-only conversion dropped
  short strings and emitted `PanicError::`. Indexed-store bounds errors and
  invalid integer `get`/`contains` receivers now route through `err.index` and
  `err.type`; no runtime source calls `rt_panic` directly outside `rt_fail`.

- The nested-join SSA promotion gate is now runnable per invocation via
  `NYTRIX_NESTED_MEM2REG=1` (previously a rebuild-only switch). The
  `set-effect-contracts` native-only row is now fixed by collecting its
  concrete attached-method wrapper; see the compiler infrastructure closure
  above.

### Diagnostics and tooling

- The `-vvvvv`/`-vvvvvv` introspection flood is compacted at its biggest
  sources.

- C5 normalized-failure infrastructure: `rt_fail(kind, msg)` joins the runtime
  API, building the panic message as `"<kind>: <message>"` using the
  lib/error.ny ERR_* kind vocabulary. Division-by-zero, modulo-by-zero, all
  index-out-of-range sites, and typed-buffer-handle failures now route through
  it. The machine-form bounds-check trap block calls `rt_bounds_trap` instead
  of raising bare SIGILL.

- Native register allocation no longer splits every long live range at
  arbitrary 16-instruction intervals or rescans each interval for fixed
  register clobbers. A 10,000-instruction, single-block stress driver keeps
  5,000 values live across the reduction and enforces a 1.5-second allocator
  budget.

- Large straight-line machine blocks no longer enter the post-RA scheduler's
  dense O(n²) dependency builder. The scheduler skips only safe regions over
  512 instructions, leaving their original valid order intact; normal sized
  regions retain latency scheduling.

- CFG translation validation now models `SELECT_I64` directly in both the
  straight-line and path-sensitive Z3 evaluators. Previously this valid pure
  operation forced CFG proofs into 2,048 interpreter trials; a 516-sample CPU
  profile attributed 96.7% of runtime to that fallback.

- Z3 translation evaluation now models `ROR_I64` and `ROR32_I64` with the same
  6-bit/5-bit count masks as the NYIR VM and x86 hardware. `SHL_I64` and
  `SAR_I64` likewise mask counts to six bits instead of using Z3's
  out-of-range shift result. `tv_smt_semantics.c` proves each unmasked form
  equivalent to its explicit masked-count form over all bit-vector inputs.

- Z3 translation validation now tracks whether signed `DIV_I64` and `MOD_I64`
  are defined, including zero divisors and `INT64_MIN / -1`. SMT bit-vector
  arithmetic defines results for those cases while NYIR traps; equivalence now
  requires both sides to agree on definedness as well as returned values.
  `tv_smt_semantics.c` confirms that rewrites to wrapping negation or zero
  cannot erase those traps. CFG validation also rejects a proof when its
  bounded path exploration is incomplete. The bounded differential fallback
  now enumerates directed signed boundary values, covering those same traps
  without Z3.

- Native lowering now forward-declares its f64-to-f32 emitter before the
  return-ABI reconciliation call sites, keeping the strict C build valid.

- Crash forensics: fatal signals write the last compiler decision records to
  `$NYTRIX_CRASH_DIR/nytrix-crash-<pid>.log` with consecutive duplicates
  folded, working with and without debug verbosity.

- `docs/learn/api-matrix.md` was renamed to `docs/learn/routes.md`.

- The native function-index collector evaluates platform guards
  (#macos/#linux/#windows), socket sockaddr variants are unified, and variadic
  default expansion no longer materializes a NULL rest argument.

- Hardware byte-swap and rotate are first-class operations across NYIR, x86-64
  (`bswap`), AArch64 (`rev`), LLVM, and the debug VM.

- Strict-warning builds are clean in native lowering/IR/parser; stdlib
  bundling no longer emits malformed parse forms in TOML/YAML helpers.

- Local development tooling re-applied the lowering fixes and saved
  working-tree snapshots.

### Standard library and legacy lowering

- `_split_impl` takes typed string parameters, `cstr_to_str` takes a typed
  integer offset, and `_list_push_reserved` remains the documented native
  append contract.

- Loop element lowering uses the inferred element type instead of recognizing
  function names: a user `range` returning a string list preserves string
  elements. Legacy `core.math` argument checks require the registered module
  identity before applying name-based numeric rules, and float-like inference
  trusts reserved intrinsics or resolved return types rather than arbitrary
  `sqrt`/`sin` spellings.

### Platform

- Native JIT allocation cleanup releases memory through the matching platform
  allocator, removing a Windows-only failure-path `munmap` reference.

<details>
<summary>Earlier implementation records</summary>

### Undated updates

- Signed division by a positive power-of-two now uses a compact bias-and-shift
  sequence: negative values receive `(divisor - 1)` before arithmetic shift,
  which preserves truncation toward zero, including at `INT64_MIN`, without a
  remainder test or multiply.

- Constant folding preserves the scalar-class/ownership/metadata trio when
  rewriting arithmetic into constants, matching copy propagation and keeping
  CSE from treating the folded value as a different entity.

- The NYIR verifier now rejects control-flow and label instructions that carry
  a semantic ownership tag (a stale stamp from a buggy rewrite), and the
  mem2reg/SROA rewrites preserve ownership, mutability, alias, and range facts
  across their PHI-to-load and load/store rewrites instead of dropping them.

- A user function owning the `range` spelling decides its ABI from its
  declared return type before the stdlib range spelling check, and the memcall
  form consults the user method first — user `range` functions and `.range()`
  methods no longer feed ~20 downstream ABI sites a wrong raw range
  classification. The `c.sqrt` hardware-sqrt fold no longer hijacks a Nytrix
  local or module named `c`.

- Documented that insertion is validated at its actual statement position,
  avoiding a redundant parser-only emit-context restriction.

- Recorded an explicit effect-row design in `docs/learn/effects.md`: inferred
  rows, upper-bound annotations, qualified effect identity, latent
  higher-order effects, and alpha-stable interface fingerprints. This is a
  design milestone; the existing effect mask remains the implemented contract.

### Earlier updates (date unverified)

- AVR portable-call validation now admits runtime entries only when their ABI
  metadata marks an untagged, non-f64 raw pointer result with no dynamic
  boxing; the check no longer special-cases the allocator symbol.

- Callback effect analysis now preserves custom rows when a factory returns
  matching immutable callback tuples from simple if-return branches and the
  caller extracts a dynamic index. Tuple allocation is included in the
  declared rows.

- The verifier still accepts the separately bounded nonlinear cases only when
  interval evidence proves descent and a finite lower bound.

- The independent proof checker now splits finite variable intervals when
  direct nonlinear bounds overlap, capped at depth 8 and 255 partitions. The
  general proof search remains bounded and fail-closed.

- Structural measures now support a same-enum component after an unchanged
  integer/boolean tuple prefix.

- Increased the bounded proof constraint budget to 128 and aligned the
  independent checker's DNF walk depth with its declared depth budget. Solver
  DNF branch lists now grow in bounded heap storage instead of placing a full
  branch matrix on every recursive C stack frame. Division and remainder now
  admit symbolic divisors only when the active path proves nonzero and
  excludes the `INT64_MIN / -1` trap. Remainder intervals use C-signed bounds
  for positive and negative divisors; Z3 remainder claims and floor-division
  claims pass only when the independent checker validates C semantics.

- Effect-row deserialization now has an explicit regression for import
  isolation: decoding the same alpha-normalized open row twice creates
  distinct fresh tail variables.

- The x64 integer arithmetic fast path now seeds an assigned cross-block
  source from its canonical home before checking register residency. This lets
  the remaining consumers in that block reuse the seeded register instead of
  loading the same home repeatedly. `machine_home_reuse` executes a dynamic
  cross-block value, verifies the result, and asserts no stack-home offset is
  loaded more than once; `regalloc_cfg_edge_carry` remains passing.

- The main object's SHA call remains an external `ny_fn_sha256` relocation, so
  the textual helper frame/load counts are not verified for the final linked
  helper/runtime body.

- Inspected the actual SOR AOT `main` disassembly beside the matched LLVM
  assembly. The native inner loop reloads row bases and the column index from
  stack homes and forms each neighbor address separately; LLVM keeps pointer
  and byte-offset recurrences in registers and uses constant load offsets.
  Loop-carried register residency remains the next native SOR target.

### 2026-10-13

- AVR native-only now lowers signed i64 division and remainder with a
  restoring binary long-division routine. Panic-trap labels include the
  function name, preventing collisions when multiple functions in one module
  contain checked operations.

- AVR native-only now lowers bounds checks by rejecting negative signed
  offsets and bounds, then comparing the nonnegative values.

- The AArch64 portable text backend now materializes arbitrary 64-bit integer
  constants with `movz`/`movk` rather than emitting invalid `mov` immediates.
  It lowers `bswap.i32` with `rev w0,w0` and `bswap.i64` with `rev x0,x0`.

- AVR now accepts the statically recognized `rt_cstr_to_str` →
  `rt_alloc_string` → `rt_panic` path for literal messages and lowers the
  panic to `break` followed by a non-returning loop.

- Exclusive-place analysis now recognizes bounded `@returns_borrow(param)`
  functions whose exhaustive `match` arms return `borrow_mut(param)`. The
  selector, patterns, and guards must be side-effect-free, there must be an
  unguarded wildcard or explicit default, and every arm must return the same
  parameter borrow. Other match shapes remain conservative.

- The portable WebAssembly panic path now exports its linear memory, emits
  NUL-terminated static string data, and writes literal panic and checked
  division-by-zero messages through WASI `fd_write` before trapping. General
  dynamic-string construction and unrelated runtime imports remain
  unsupported.

- RISC-V64 checked division now links without the unavailable runtime
  allocator and panic helpers: only a static `rt_alloc_string` immediately
  consumed by `rt_panic` is elided, and the panic call lowers to `ebreak` plus
  a non-returning loop. This trap path does not preserve a panic message;
  other runtime panic calls remain unsupported.

- Exclusive-place analysis now follows a `@returns_borrow(param)` call to its
  actual source argument. It classifies the returned pointer as exclusive only
  when the callee directly returns `borrow_mut(param)`, returns a bounded
  straight-line chain of local aliases rooted at that call, every arm of a
  side-effect-free `if` directly returns `borrow_mut(param)`, or a bounded
  chain of up to 15 side-effect-free early-return `if` statements falls
  through to the same borrow. Other return shapes remain conservative.

- Unverified `@returns_borrow` bodies now fail closed in strict ownership
  analysis: while the returned borrow is live, possible accesses are treated
  as overlapping an unknown source. Direct returns, local aliases, and
  side-effect-free `if` branches returning `borrow_mut` retain precise
  disjoint-place checking.

- Effect callback invalidation now tracks list and dictionary callback facts
  by bounded local alias groups. Mutating one alias invalidates facts for that
  group without discarding independent same-kind containers. The
  returned-callback differential also includes a generated dictionary
  alias-mutation positive/negative pair.

- Match selectors, patterns, and guards remain required to be side-effect-free
  and exhaustive.

- Dynamic dictionary-key places are represented as wildcard indices for
  exclusive-borrow overlap checks, so neither an identical dynamic key nor a
  different variable key is assumed disjoint.

- Fixed-index list refinements using integer `<`, `<=`, `>`, `>=`, `==`, or
  `!=` now survive a write only when the index is disjoint or the stored
  signed-i64 constant proves the predicate against an immutable top-level
  signed-i64 constant bound. Checked arithmetic is supported in those constant
  expressions; overflow, unknown values, and local shadows fail closed. String
  and boolean `==`/`!=` use resolved literal value equality, not identifier
  spelling. Appending preserves nonnegative fixed-index predicates; a `-1`
  predicate survives only when the appended value proves it. Every conjunct
  must remain proven; overloaded comparisons remain fail-closed. When a
  fixed-index comparison's other operand also reads the list, a store is
  accepted as disjoint only when those reads cannot alias the destination;
  mixed-sign fixed indices remain potentially overlapping.

- A fixed-index equality refinement also survives storing the exact expression
  from its opposite operand, provided the expression is side-effect-free and
  does not read the written index. This proves the equality by construction;
  different values, unsupported expressions, overloaded equality, and reads
  that may alias the destination remain invalidated.

- Content-dependent store preservation now composes across bounded `&&`/`||`
  predicates: each atom must remain unchanged or be proven true by the
  predicate, so every conjunct stays valid without relying on short-circuit
  branch assumptions. For append, `items[-2] == items[-1]` survives when the
  appended expression is the exact old `items[-1]`; equality and inequality
  both survive when appending the exact old `items[-2]`, which swaps the pair.
  Appending the old last element to an inequality is rejected because it makes
  the new last two elements equal. Unsupported negative-index expressions fail
  closed. Copied-tail proofs compose with bounded conjunctions and disjunctions
  only when every other atom is independently append-stable. Generated
  positive/negative cases cover same-tail invalidation, pair-swap preservation,
  conjunction transport, and a nonnegative indexed value whose negative-index
  bound changes after append.

- A fixed `items[-1]` comparison against an immutable scalar bound now
  survives append when the appended value is that exact bound and the relation
  is reflexive (`==`, `>=`, or `<=`). Appending another value expires the
  fact.

- Append now also preserves monotone comparisons between a fixed-index value
  and `items.len` (`items[i] < items.len`, `items[i] <= items.len`,
  `items.len > items[i]`, or `items.len >= items[i]`) when every indexed
  operand is stable across append. Nonmonotone comparisons and shifted negative
  indices still expire the refinement. Generated positive and negative modes
  cover both operand orders and a relation invalidated by length growth.

- Content append transport checks stability of the comparison's bound, not
  only its indexed left operand. A negative-index bound that shifts on append
  invalidates the proof; copying the exact old last element preserves reads
  of `items[-1]`. The generated differential catches the previously accepted
  invalid `items[0] < items[-1]` append and its preserving counterpart.

- Exact opposite-expression stores now prove reflexive `<=` and `>=` content
  predicates as well as equality, provided the expression is side-effect-free
  and cannot read the written index. The generated differential accepts an
  exact bound reconstruction and rejects a value beyond that bound.

- The generated mutable-flow differential now also covers exact immutable
  arithmetic-expression stores under symbolic equality, paired with a
  violating store.

- Transparent callback-container forwarding now includes argument evaluation
  in its safety check. A nested forwarding call with a computed argument is
  not treated as an unchanged-container return: argument evaluation may mutate
  or escape that container. A generated negative case mutates a dictionary
  callback while evaluating another argument and rejects an underdeclared
  caller; the returned-callback differential passes cold.

- Callback effect provenance now follows a bounded chain of immutable local
  aliases across interprocedural forwarding functions. Every body in the chain
  must contain only immutable identifier aliases and a direct return or a
  nested forwarding call; computed arguments, mutation, and unknown bodies
  remain conservative. Generated positive and underdeclared negative cases
  cover an aliased inner return followed by a second wrapper.

- Returned callback-container summaries now recurse through bounded
  interprocedural calls when a forwarding function receives another function's
  returned list or dictionary. The nested result must itself have a supported
  returned-container flow; unknown results retain conservative callback
  effects. Generated positive/negative modes cover a locally mutated callback
  list returned through an identity wrapper.

- Generated effect coverage now follows a returned closure that captures a
  mutable callback alias. Replacing the callback after closure creation is
  reflected when the escaped closure is invoked; the paired underdeclared
  effect contract is rejected. A second pair verifies callback effects survive
  forwarding the closure through an identity function.

- Returned callback effects now propagate through bounded chains of direct
  callback-returning calls. Each forwarding function must return its nested
  callback call directly; computed or side-effecting prefixes remain
  conservative. Generated positive and underdeclared cases cover two
  forwarding layers around closures that capture a callback replaced after
  closure creation.

- The five reported generated D2 regressions—noalias index overlap,
  returned-callback aliases, length-bound flow, allowance flow, and mutable
  aggregate flow—pass together cold and serially after these changes.

- Closed the D2 tracking item after the focused generated flow suite passed
  5/5. The supported refinements and alias/effect transfers are listed above;
  unknown aliases, dynamic dictionary-key overlap, unverified borrow-return
  bodies, mutable symbolic bounds, and paths beyond analysis limits remain
  conservative and fail closed.

- The noalias differential now also verifies that an unrecognized
  `@returns_borrow` body shape fails closed: a borrow returned after an
  unrelated local declaration is treated as unverified, so even a disjoint
  write is rejected. The focused differential passes cold.

- Effect callback facts now pass through a callee that returns one of its
  typed list/dictionary parameters unchanged. The callee must consist solely
  of `return parameter`; facts are preserved only for the returned container
  alias. Generated modes 91–94 cover dictionary/list forwarding and reject an
  underdeclared caller.

- Returned callback-container provenance also survives bounded exhaustive
  `if`/`match` bodies when every arm returns the same container parameter,
  including up to 16 consecutive immutable local aliases of that parameter.
  Pure single-return forwarding chains of up to 16 calls also preserve the
  parameter identity.

- Effect callback facts also pass through a callee that binds one typed
  list/dictionary parameter to a single local and immediately returns that
  local. No intervening mutation or escape is accepted. Generated modes 95–98
  cover both container kinds and reject underdeclared callers.

- Fixed-index list refinements with an immutable scalar comparison bound now
  survive disjoint same-sign fixed-index stores and append at nonnegative
  guarded indices. A store to the guarded index preserves the fact only when
  it stores the exact bound identifier and the comparison is `==`, `>=`, or
  `<=`. Other mutable bounds and unsupported relations still fail closed.

- The AVR portable emitter now lowers optimized 64-bit rotate-right with a
  masked variable count, byte swaps, and all six signed i64 comparisons.

- AVR now emits `ror32.i64` as a zero-extending 32-bit rotate.

- BPF `rt_main` now receives its `socket` section before the function symbol
  and label are emitted, and its ELF size is closed while still in that
  section. This fixes the zero-sized `.text` program that libbpf rejected. A
  minimal object with a noinline internal call loaded on the host kernel with
  `CAP_BPF` and `CAP_PERFMON`, passed `BPF_PROG_TEST_RUN` on a 64-byte packet,
  and returned the expected tagged value, 85.

- Portable function prologues now spill only declared parameters, not every
  local slot up to the function's maximum slot index. The old behavior read
  nonexistent BPF argument registers in one-argument callees, so the kernel
  verifier rejected a function using local memory. A noinline BPF program with
  local byte/i64 accesses and an internal call now loads and executes through
  `BPF_PROG_TEST_RUN`, returning 171.

- Bounded disjunctions of fixed-index integer comparisons now retain list
  refinements across stores and append only when every comparison is proved
  preserved.

- Bounded returned-callback effect inference accepts side-effect-free boolean
  conditions composed from identifiers, literals, `!`, `&&`, and `||`.

- Dynamic-key replacements in local callback lists and dictionaries now retain
  a complete effect summary by joining the pre-mutation container summary with
  the stored callback. Restoration requires a known summary and stable key and
  callback expressions; unknown callback rows still invalidate the summary,
  and same-kind aliases remain invalidated. Generated accepted and
  underdeclared cases cover builtin and custom capability rows for list
  replacement, dictionary `set`, and indexed dictionary assignment.

- Generated builtin/custom-row positives and underdeclared negatives cover
  callback escape through returned containers, mutation joins, and returned
  aliases; unsupported factory shapes remain conservative.

- The BPF, MIPS64, and PowerPC64 portable emitters now materialize local-slot
  addresses and lower byte- and word-width memory operations.

- MIPS64 and PowerPC64 now lower NYIR bounds checks to signed-negative and
  upper-bound trap sequences.

- PowerPC64 NYIR constants now materialize all 64 bits instead of relying on
  the signed-16-bit `li` instruction for arbitrary values.

- The direct WebAssembly text emitter now lowers byte and i64 memory loads and
  stores using linear-memory instructions. Separate byte load and word store
  probes trap at the end of a one-page memory.

- The direct WebAssembly emitter now lowers scalar f32/f64 constants,
  arithmetic, comparisons, square root, and integer/float conversions.

- AVR now lowers a bounded scalar NYIR slice: i64 constants, local spills,
  addition/subtraction/multiplication, bitwise operations, variable-count
  left/arithmetic-right shifts, direct internal calls with at most two i64
  arguments, conditional branches, and `select.i64`. AVR-GCC argument/result
  and callee-save conventions are respected. Spill frames use Y-relative
  accesses, with Z-based addressing beyond displacement.

- BPF portable output now places `rt_main` in a `socket` program section and
  emits a GPL `license` section.

- The shared ARM/AArch64/i386/RISC-V stack emitter now enforces
  `NYIR_BOUNDS_CHECK` instead of skipping it. Target-width comparisons reject
  negative offsets and offsets at or beyond the bound, using each ISA's trap
  instruction.

- The BPF text emitter now lowers 64-bit rotate-right to a masked right shift
  and complementary masked left shift. Runtime loading remains unavailable in
  this environment: unprivileged BPF is disabled and no effective `CAP_BPF` or
  `bpftool` is present.

- The portable WebAssembly emitter now enforces `NYIR_BOUNDS_CHECK` with
  signed-negative and upper-bound traps.

- Mutable list refinement transport now recognizes concatenation with an
  in-scope list-typed local suffix, not only a literal list suffix. Facts
  about the unchanged nonnegative prefix survive; negative-index facts still
  expire.

- Dereferencing a direct `borrow`/`borrow_mut` expression now resolves the
  exclusive borrow to its syntactic source place. Other unresolved pointer
  provenance conservatively overlaps every access while live.

- Returned callback summaries now have generated coverage for a branch-joined
  callback alias captured by two nested local lambda wrappers.

- Bounded early-return callback factories now join up to fifteen
  identifier/literal-guarded `if` returns plus one direct fallthrough return.
  Direct-return `match` arms with an `else` return or a direct fallthrough
  return are joined too. Leading local callback bindings are preserved through
  the bounded return analysis, so captured lambda aliases in a branch join
  retain their inferred rows; generated positive and underdeclared cases cover
  this path. Fully returning nested `if` trees are joined within the same
  branch-count and depth limits, with generated positive and underdeclared
  cases.

- Reassigning a mutable callback binding now refreshes the builtin-effect
  summary of local lambdas that capture it, including wrappers that capture
  another dependent lambda. Exact dictionary callback facts for captured
  lambdas are refreshed too; the affected dynamic-container summary is
  invalidated. Custom effect rows affected by such a capture change remain
  unknown and fail closed. Generated modes 31–46 cover both reassignment
  directions, nested wrappers, underdeclared builtin rows, exact dictionary
  and list lookup through aliases, dynamic summary invalidation, and a
  custom-row underdeclaration.

- Exact literal-key `dict.set` calls now update local callback provenance
  after invalidating the dictionary's previous callback facts. Generated
  positive and underdeclared cases verify the replacement callback's builtin
  IO row.

- The direct portable WebAssembly text emitter now has an executable scalar
  parity fixture: native-only NYIR output assembles with `wat2wasm` and
  Wasmtime invokes an internal i64 function call with the expected tagged
  result. Checked division now uses a local identity/trap helper pair only for
  the compiler-generated message-allocation followed immediately by panic:
  nonzero division returns the expected tagged result, zero divisor traps, and
  unrelated external imports still fail closed. The trap preserves failure but
  not the runtime panic message.

- Both emitters lower all six signed i64 comparison predicates instead of
  producing a comment/no-op; PowerPC register syntax and per-function local
  labels are valid for the assembler. Optimized `ror.i64` and `select.i64` now
  have executable MIPS64/PowerPC64 lowering. WAT `select.i64` converts its i64
  condition to the required i32 selector; both Wasmtime paths verify optimized
  compare/select control flow. Panic-message output and unrelated runtime
  helper imports remain unsupported.

- The shared stack emitter now lowers `select.i64` on RISC-V and gives NYIR
  branch labels function-specific names, preventing duplicate symbols when
  multiple functions contain control flow. Checked division and explicit
  literal panic now link and execute: static-message allocation immediately
  consumed by `rt_panic` is elided, and panic lowers to `ebreak` plus a
  non-returning loop.

- The shared stack text emitter lowers `select.i64` on ARM, AArch64, i386, and
  RISC-V. ARM leaf functions now return through `lr` instead of popping an
  unsaved return address. The generic seven-backend matrix now compiles
  optimized select flow.

- The BPF portable emitter now rejects optimized 64-bit rotate-right instead
  of silently emitting an arithmetic shift. Arithmetic right shift, divmod,
  and ordering comparisons now emit their signed eBPF operations. Comparison
  labels are unique across functions. Symbol addresses used by dynamic
  checked-divmod guards now use eBPF `lddw` syntax. The emitted BPF frame
  comment also uses assembler-valid syntax. This verifies object encoding
  only; loadable program sections and panic runtime integration remain
  unimplemented.

- Dynamic panic-message output and operations outside the exercised AVR subset
  remain unsupported.

- AVR now has QEMU runtime coverage for scalar calls, branches/select, integer
  arithmetic and shifts, local spills, and byte/word memory operations.

- Canonical list `append` results now retain known fixed-index callback facts
  from their source list. When the source-list summary and appended callback
  row are both complete, dynamic-index calls use their union; a callback
  parameter's known open owner row is preserved as a latent row, while unknown
  summaries remain fail-closed. Discarding the result preserves source facts,
  since append returns a new list. Other list mutations remain conservative.

- Returned-callback factories now strip builtin effect labels from each branch
  row before joining custom capability rows. This avoids losing a complete
  summary when three returned callbacks repeat a builtin effect label.

- Exclusive-borrow place analysis now distinguishes exact literal string keys
  in dictionary indexing and canonical indexed writes. Different keys are
  disjoint; same-key access remains overlapping.

- Callback-summary invalidation now uses a known container's kind: mutating a
  dictionary no longer erases list callback summaries, and vice versa.
  Same-kind aliases remain conservatively invalidated; unknown container kinds
  still invalidate both.

- Allowance analysis now handles a reassignment in a trailing `defer` at the
  lexical block's scope exit when its body is within the nonthrowing
  assignment fragment. This preserves uses after defer registration in the
  pre-exit epoch and validates the new epoch separately.

- Allowance analysis now recursively unrolls nested fixed counted loops and
  literal ranges when every loop is bounded and the nested body remains in the
  nonthrowing supported fragment. The existing path cap still fails closed.

- A fact survives only when pattern and guard evaluation is read-only and each
  arm's mutation is independently proved preserving.

- Mutable-list length lower-bound transport now also recognizes comparisons in
  reversed operand order: `N < items.len` and `N <= items.len`.

- Mutable-list refinement flow now recognizes assignment from concatenation
  with a literal list suffix. Direct length lower bounds and nonnegative
  fixed-index/content facts survive because the original prefix is unchanged;
  negative-index facts expire. Overloaded addition and computed suffixes
  remain conservative.

- Literal-list prefix concatenation preserves direct list length lower bounds,
  but invalidates fixed-index/content facts because prepended values shift the
  original elements. Overloaded addition remains conservative.

- Callable effect factories join rows from up to three leading `if` returns
  followed by a direct fallthrough return. For example:

  ```ny
  if choose { return callback_a }
  return callback_b
  ```

  This is a function-body fragment. Both returned callback rows contribute,
  including their custom capability rows.

- Bounded allowance loops now model parameter reassignment independently in
  each match arm, using the existing per-branch path state.

- Mutable literal callback lists and dictionaries now retain per-slot effect
  summaries. Exact nonnegative-index or string-key writes through canonical
  `core.set_idx` refresh the written slot. For local lists, only that
  binding's callback facts are invalidated because lowering detaches a shared
  list before the write; the source COW alias retains its original callback
  facts. Dictionaries keep global copied-alias invalidation. A branch-joined
  local callback alias stored into an exact slot retains the joined row.

- Mutable-list refinement transport recognizes direct `.len > N` and
  `.len >= N` predicates for folded nonnegative integer bounds. Those facts survive
  fixed-element stores and builtin append; a `pop` still invalidates them.

- Mutable-list refinements now also preserve bounded `&&` facts combining
  fixed-index integer comparisons with direct length lower bounds. Fixed-index
  stores and append retain the conjunction only when every content atom is
  stable and every length atom is an append-preserved lower bound;
  exact-length conjunctions still expire after append.

- Mutable-list refinement transport now preserves a bounded disjunction of a
  direct length lower bound and a fixed nonnegative content predicate through
  append only when each alternative remains true.

- Mutable callback-effect tracking now invalidates only the list snapshot
  changed by a direct indexed assignment. Copy-on-write aliases retain their
  independent callback rows; the replaced slot receives the assigned
  callback's row.

- Assigning the result of builtin list append now carries forward exact source
  slot facts and combines a complete known callback summary with the appended
  callback's effect mask and custom row. If either summary is unknown, the
  destination's dynamic-index summary remains unknown.

- Callable factory effect inference now follows returned local callback
  aliases, including aliases of callback parameters and aliases selected by
  ternary, `if`, and `match` joins. It preserves latent builtin and custom
  effect rows. Factories with an earlier return remain fail-closed rather than
  using an incomplete summary.

- The `[0,2]` case is accepted and `[1,2]` is rejected for the missing minimum
  use under strict native-only NYIR verification.

- An unguarded wildcard no longer creates a false zero-use path; guarded
  wildcard fallthrough remains represented.

- Effect analysis now carries the union of builtin and custom effect rows when
  a local callback is selected by a ternary. Lowering also preserves the
  callable ABI marker when a ternary selects named functions, including nested
  literal ternaries.

- Fixed-index integer refinement transport now folds bounded signed constant
  expressions in indices, comparison bounds, and stored values. Addition,
  subtraction, multiplication, division, remainder, and unary negation are
  accepted only when every intermediate is representable and division is
  defined; overflow and division by zero remain unknown.

- Mutable list refinement transport now preserves arbitrary content predicates
  across an exact indexed self-store. Refinement names also retain their base
  list indexing interface inside the guarded region. The generated refinement
  differential includes both flow cases.

- Ownership allowance analysis now splits layout-guard success and fallback
  paths, closing the pre-reassignment epoch only on the fallback path. Loop,
  potentially throwing `try`, and `defer` reassignment still fail closed.

- Allowance epochs now split inside try bodies proven non-throwing by the
  bounded literal/identifier assignment checker.

- Ownership allowance analysis now unrolls literal `for` ranges of up to 64
  iterations when the body uses supported nonthrowing control flow.

- Ownership allowance analysis also unrolls canonical counted `for` and
  `while` loops with literal initialization/bounds and unit increment or
  decrement when they execute at most 64 times. Strict and inclusive ascending
  or descending conditions split each bounded body epoch; empty loops preserve
  the current epoch, while dynamic or unsupported headers and induction-
  variable writes fail closed.

- Parser-generated indexed assignments now carry an explicit AST marker.
  Borrow-path analysis and mutable-refinement transport apply `set_idx` rules
  only to those synthetic stores; a source function with that name follows
  ordinary call handling.

- Exclusive-borrow place analysis now carries dynamic list indices as
  conservative wildcards while preserving member suffixes. Different fields
  remain disjoint even when indices may alias; same-field accesses remain
  overlapping.

- Exclusive-borrow analysis now folds checked literal signed-integer
  arithmetic in list indices. Computed disjoint indices retain a noalias
  proof, while two expressions resolving to the same index remain rejected.
  Indexed path comparison now interprets bracket components before matching
  their contents, so mixed-sign pairs such as `[-1]` and `[2]` conservatively
  overlap.

- Mutable list content refinements now survive fixed-length stores at distinct
  fixed indices when all predicate and store indices have the same sign.
  Append also preserves fixed `-1` integer comparison atoms when a constant
  appended expression proves them; other negative-index predicates still
  expire when their referents shift.

- Content predicates whose reads use fixed nonnegative indices now also
  survive appending to the list. A fixed `-1` integer comparison also survives
  when the appended constant satisfies the comparison, including conjunctions
  with unchanged nonnegative-index atoms. Other negative-index predicates
  still expire.

### 2026-10-12

- Exclusive-borrow place analysis now proves distinct constant indices
  disjoint when both are nonnegative or both are negative, while retaining
  overlap for mixed-sign indices whose aliasing depends on list length.

- Ownership allowance analysis now keeps separate bounded path states through
  `if` and `match`, closing an epoch on each path at the point it reassigns
  the parameter. One-sided joins and early exits no longer merge pre- and
  post-reassignment use counts. Reassignment in loops, `try`, `guard`, and
  `defer` fails closed as inconclusive.

- Mutable list refinements now transport bounded conjunctions of fixed-index
  integer comparisons across element stores only when every atom remains true.
  A write to an unrelated nonnegative index is proven disjoint; a write to a
  guarded index must prove the stored literal satisfies its comparison. Append
  preserves these facts only when all indices are nonnegative. Negative-index
  aliasing and append cases fail closed.

- Exact indexed stores now preserve `==`, `<=`, and `>=` refinements when the
  stored expression reconstructs the guarded side-effect-free arithmetic bound
  from immutable identifiers. Generated positive, violating-store, and
  mutable-bound invalidation cases pass in the mutable-flow differential.

- Append now transports a `items[-1] <= bound`-style refinement when the
  appended value reconstructs the same side-effect-free scalar expression
  from immutable identifiers. The generated differential verifies the exact
  preserving append and a one-step violating append.

- Ownership now verifies bounded `@returns_borrow` forwarding chains by
  tracing the callee's annotated source parameter through each body to
  `borrow_mut`. A two-wrapper positive disjoint-store case and overlapping
  negative case pass the generated noalias differential; unsupported bodies
  remain unverified.

- Returned callback-container effects now compose across two bounded identity
  wrappers around a locally mutated returned list. Generated positive and
  underdeclared negative cases pass.

- Backend acceptance covers all five portable emitters' rejection paths and
  WebAssembly `ROR32_I64` count boundaries (0, 1, 8, 31, 32, and -1).

- Machine lowering now accepts `F64_TO_I64` and `F32_TO_I64`. The AArch64
  object encoder also handles the single-precision conversion.

- Ownership allowance analysis now marks depth-limit results inconclusive and
  reports a compile-time error instead of counting an unanalyzed subtree as
  zero uses. The source borrow checker also reports its bounded traversal
  instead of silently skipping deeply nested statements and expressions.

- ELF64 `object_link_run` dispatch now reads `e_machine` instead of routing
  every ELF64 object through the AArch64 runner.

- Expanded ownership closure-capture traversal through nested statement
  bodies, match expressions, f-string interpolations, assembly operands, and
  additional expression wrappers.

- Added AArch64 ELF emission to the public Nyr object API for supported NYIR
  operations. The standalone C frontend now carries AAPCS64 aggregate
  parameter and return metadata through the AArch64 machine encoder; aggregate
  parameter homes preserve ascending memory order despite descending frame
  slots, and aggregate-signature entry functions receive the platform ABI
  spills. Unsupported AArch64/Windows aggregate targets still fail closed.
  Closed D5.

- Scoped the SOR grid buffers to the local `run_sor` helper, allowing the O3
  vectorizer to widen the inner column loop to four lanes. This is a measured
  SOR improvement; it does not establish native/LLVM parity.

- Refinement flow now preserves a checked `list.len > 0` fact across indexed
  element stores and append-to-self assignments inside the guarded region.
  Whole-list rebinding, mutating calls that may empty the list, and
  content-dependent predicates remain fail-closed. Alias-return calls before
  the guard and closure captures also remain fail-closed.

- `@allowance` now treats direct parameter reassignment as an epoch boundary.

- The standalone C frontend's completed AAPCS64 and Win64 cross-language
  matrix is recorded in the D5 entry above.

- Win64 COFF object emission now rejects aggregate ABI metadata before either
  the machine or NYIR encoder can emit a mismatched calling convention.

- Mach-O x86-64 object emission now has a verifier-enabled C aggregate matrix
  for integer, mixed, SSE, packed memory-class, aligned, and three-word
  layouts.

- The restored scalar, aggregate, and function bindings remain usable and
  mutable.

- Implemented `@async_effects` lowering: eligible direct calls produce
  stackless handles with positional/default argument expansion. Fixed async
  argument packing to use byte offsets, preserving multi-argument calls.

- Closed D4: typed `Code<T>` values, helper transport into template splices,
  source-text parsing into generated declarations, category diagnostics, and
  parser depth/size limits have focused coverage.

- A type-rejected assignment leaves earlier mutable state usable by later
  submissions, and `:trace off` reconstruction replays accepted updates while
  retaining earlier functions. Closed D6.

- Documented the proof/refinement acceptance matrix against paired positive,
  negative, and inconclusive fixtures. The documented calculus now names the
  exact tests for closed propositions, bounded nonlinear arithmetic, solver
  limits, refinement transport, mutation, and backend parity.

- Clarified that handles preserve aggregate runtime values without converting
  them and do not own host memory referenced by raw pointers.

- NYIR match lowering now keeps tagged dynamic subjects out of the raw-integer
  dense-dispatch optimization, so integer literal patterns compare against
  correctly tagged values. Consumer-specific LLVM cache-link and post-link
  verification failures no longer unlink the validated shared core bytecode.

- Expression lowering now preserves the proven borrowed ownership on exclusive
  borrow-call results instead of overwriting it with the generic unknown
  ownership stamp.

- BigInt type probing now calls a value from the dynamic globals registry only
  when its runtime tag identifies a function pointer.

- The ownership runtime-shadow and generated allowance differentials pass with
  the snapshot and current shared core bytecode.

- Effect-summary propagation now passes the appended-list item expression at
  both local declaration and reassignment sites, matching the helper's
  four-argument contract.

- Byte-load producers carry the raw integer ABI before local-store and call
  reconciliation, preserving odd byte values in shared-core string copies.
  Documentation-tree collection remains a separate failing acceptance check.

- Native dictionary getters keep numeric values tagged until the actual
  consumer requests a scalar or string; a string-returning function no longer
  turns unrelated numeric fields into text.

- Runtime locals shadow module constants during qualified integer folding.
  Dynamic multiplication boxes raw integer operands before entering the tagged
  arithmetic ABI, preserving dictionary-derived dimensions and row offsets.

- Byte, word, dword, qword, and typed-buffer memory indices use the existing
  integer-value normalizer before address arithmetic.

### 2026-10-11

- Boolean-formal normalization now stamps its emitted predicate as raw i64
  before integer ABI reconciliation, preventing `true` from being decoded as
  tagged integer zero. The source-loaded NYIR CLOC smoke counts CRLF source,
  multiline comments, and inline comments as `3`, `1`, and `1`.

- Inferred handle locals discard a fallback `any` type spelling when their
  emitted producer establishes a concrete carrier. Initializer scalar decoding
  also requires an actually tagged producer. The source-loaded AST smoke
  preserves a typed declaration sequence and its `declarations` category.

- Conditional locals retain their uniform tagged carrier through later
  raw-scalar classification and comparisons.

- Documentation metadata scans leading header lines instead of splitting
  entire file bodies before discarding them. Header parsing preserves CRLF
  handling, early termination, and a final line without a newline. Search
  scoring uses allocation-free ASCII-folded substring scans and cached byte
  lengths.

- Inferred dictionary locals now refine dynamic mutation results to the object
  carrier at the store boundary, preserving pointer bits and keeping later
  call arguments consistent with store-class resolution.

- NYIR call resolution uses a bounded per-evaluation symbol cache, including
  unresolved source-function lookups before runtime dispatch. Exact names
  still take precedence over suffix aliases, and ambiguous aliases remain
  unresolved.

- Native layout field loads carry their declared scalar class, and inferred
  locals retain resolved field/callee carriers instead of being overwritten by
  a stale dynamic expression stamp. A rebuilt shared-core smoke preserves the
  nested resource field `53` through a local and integer return.

- Getter comparison normalization records decoded scalar operands before
  dictionary-read handling, preventing overlapping source classifications from
  decoding an odd success flag twice. The rebuilt shared-core parser smoke
  returns integer `8080` and sequence `["port=", 8080, ";"]` at offset `10`;
  CPS returns `42`.

- Private append-fill induction counters and empty integer lists can now be
  localized before AST loop construction, using collected callee references,
  increment-only counter updates, and whole-program list escape checks. Other
  scalar globals retain post-lowering promotion. This exposes reserve-once
  append filling to real compiled entry points, rather than only source-only
  IR dumps. Function-observed globals, address-taken storage, closure
  captures, and standard-library module storage keep their existing cells.

- Managed-object type and length queries now use the live allocation registry
  before foreign-pointer heuristics. GMP result export writes directly into
  the final BigInt payload instead of allocating and copying an intermediate
  limb buffer.

- Parallel benchmark completion notices are also emitted in captured logs
  instead of remaining TTY-only.

- Integer-shape assignment-site queries now detect active identifier cycles
  without allocating a second dependency graph. Local scalar recurrences no
  longer restart whole-program scans indefinitely.

- Native scalar shift lowering explicitly masks variable and out-of-range
  counts to six bits before inlining and verification. BigInt shifts retain
  their separate runtime path.

- Embedded `ny fmt --audit` keeps Ny batching for `.ny` inputs and uses the
  existing C collector for `.c`/`.h` inputs.

- The LLVM and NYIR runtime resolvers register the raw-i64 allocation helpers
  emitted by compiled core code.

- Documentation metadata collection now normalizes native path/string values
  before dynamic container storage and uses byte-safe substring extraction for
  routes, titles, and keywords.

- Native result-oracle assembly now emits each reachable function symbol once,
  preventing duplicate assembler definitions when the function pool contains
  repeated declarations.

- Safe-mode alias policies now recognize declarations parsed from the
  synthetic `<stdlib>` source as standard-library code, even when a token
  lacks the standard-library feature bit.

- Native vector arithmetic now uses the vector marker's dict dimension before
  testing the dynamic list predicate.

- Runtime allocation registries skip repeated safe pointer `mincore` probes
  for known handles while retaining validation for unknown pointers and ASan
  redzones. Native lowering uses amortized-growth string builders for
  loop-carried append and a raw integer tbuf helper.

- HM now recognizes `load_layout` as a known metadata-resolved intrinsic
  inside generic codegen templates.

- Standard-library lemma declarations are checked with the stable `auto` proof
  policy, independent of the consuming program's `--proof-solver` mode. This
  keeps `none` and `presburger` modes from rejecting unrelated imported
  standard lemmas.

- Certificate-index persistence now uses a raw-path `__rename_raw` runtime
  entry point, matching `__open_raw`'s native string and status ABI. The
  general dynamic-value rename wrapper had formatted native path pointers as
  `"<ptr...>"`, so `save` could report success while leaving only its
  temporary file. A direct rename smoke test now moves the source and leaves
  only the destination.

- Proof constant evaluation now resolves unqualified lemma calls through the
  current module's imports before scanning the flattened program. This avoids
  rejecting valid witnesses when unrelated standard-library modules contain
  same-named lemma declarations.

- Fixed native `file_read("/dev/null")` failures across NYIR, LLVM MCJIT, LLVM
  ORC, and native-only routes. OS raw-open callers now pass typed Ny strings
  directly to `__open_raw`; routing them through `__any_to_cstr` converted
  normalized native strings to pointer descriptions instead of C paths.
  `path.normalize` also returns already-normalized components without the
  miscompiled dynamic-list iteration path.

- `make test` supplies the freshly built standard-library bundle to every
  fixture child and prefers it over rebuilding the library from sources for
  each temporary fixture tree.

- Native BigInt classification no longer lets a same-named module global
  override an `any` parameter in a user callback. Global-initializer recovery
  is limited to the `rt_main` builder, where root globals are represented in
  its local table.

- Strict ownership return checking now follows local borrow aliases: returning
  `ref` after `def ref = &param` requires a matching `@returns_borrow(param)`.

- Native dictionaries now register their runtime-created payload handles.

- The 2M explicit dynamic-list append case now uses the raw integer tbuf
  helper when lowering proves the appended value is an integer.

- Consolidated strict static borrow checks into `lang/types/ownership.c` and
  its existing public header; removed the parallel versioned module, CLI mode,
  and codegen/cache plumbing. The unified checker currently rejects borrow
  values stored in collections and enforces `@returns_borrow(param)` for
  borrow returns. Allowances now accept `[lo,hi]` intervals with branch joins
  and unbounded loop upper bounds, while integer limits retain exact-count
  meaning.

- Standard-library source cache keys no longer depend on the requesting source
  path. Automatic fixture workers scale from available RAM with a 2 GiB
  per-worker budget; `ny-test` applies a matching memory guard.

- Generalized native loop-carried string-builder promotion from literal
  suffixes to a distinct string-local suffix.

- Integer-key lookups in the managed dictionary path now compare immediate
  values directly and avoid the generic string/pointer key comparator. This is
  a regression baseline; the absolute native set cost remains several seconds.

### 2026-10-10

- Fixed native lowering of the modular/finite-field polynomial coefficient
  helpers: `gfe` and `zmod` values are dictionaries, so `_poly_coeff_value`
  now reads their `"value"` field by key instead of ambiguous `.value`
  property dispatch. The property path had pulled an unrelated imported
  `#main` initializer into the helper and left `json_decode` unresolved in
  native-only JIT builds.

- Fixed `ny-test --bench --jobs N` so an explicit worker count controls the
  benchmark fixture pool; previously the benchmark pool ignored `--jobs` and
  used its CPU-based default unless `NYTRIX_BENCH_JOBS` was set. This restores
  isolated serial measurements with `--jobs 1`.

- Native proof folding now evaluates effect-free user functions over known
  scalar and zero-field enum arguments when their bodies reduce through
  returns, matches, and conditionals.

- The 2026-10-11 entry records the replacement resource-based policy and
  uncached failure-batch result.

- Unknown-type append calls now use the checked runtime append boundary
  instead of bypassing validation with raw typed-buffer writes.

- Reorganized the library tree into `lib/`, `lib/math/`, `lib/os/`, and
  `lib/compiler/{syntax,tools}/`; removed the empty `lib/core/` level and
  updated Ny module declarations, imports, aliases, compiler/loader mappings,
  examples, and fixtures to use `core.*` namespaces without legacy import
  shims. Regenerated `std.ny`.

- Added summary and keyword metadata to every one of the 530 library modules.
  `./make docs` parses 530 modules and 21,441 symbols and generates
  `build/docs/index.html`.

- Fixed repeated literal-key scanning for both single- and double-quoted
  dictionary keys by removing the crashing `chr()` call from the Ny scanner.

- REPL module loading no longer treats an active-use list as proof that a
  module's source is loaded; this preserves explicit imports after a `core`
  root import. Crypto's public `hex_encode` wrapper calls its exported binary
  helper rather than relying on an attached extension that is unavailable in
  that module context.

- `dict_clone` now reads the legacy dictionary's stored length directly on its
  non-native fallback, avoiding an unresolved inline-only `dict_len` symbol in
  JIT routes. Call-result container indexing now preserves the accessor's
  tagged physical representation when static typing knows a scalar element,
  preventing a second unbox before comparison.

- Dynamic operands now override an inferred BigInt arithmetic result in native
  lowering.

- Native proof lowering now evaluates a lemma invocation by substituting its
  compile-time positional arguments into the lemma proposition and folding the
  supported integer/Boolean fragment. Unknown arguments and ambiguous lemma
  names still fail closed.

- Async task constructors now have audited fixed `io|alloc` effect summaries
  and compose the callback's open effect row at the call site.

- Native ABI type lookup now recognizes top-level unit enums directly from the
  program AST when enum signature metadata has not yet been materialized.

- Made `^` bitwise XOR and `**` exponentiation throughout Ny parsing,
  lowering, and runtime paths; `^^`/`^^=` now alias power. BigInt,
  finite-field, and ring operators are declared in Ny. C-to-Ny and
  Python-to-Ny conversions, pure-expression evaluation, disassembly, and
  decompiler output use the new spellings; decompiler rendering canonicalizes
  `^^` to `**`.

- Replaced operator quirks documentation with the normal XOR/power contract.

- Counted sieve survivors eight at a time from the existing byte flags using a
  64-bit byte-lane reduction.

- Checked odd/even Euclid parity before Euler 9's perimeter division in both
  reference implementations.

- Factored the common `depth` addition out of both arms of the Ny and C
  `binary` benchmark recurrence.

- Reworked both Ny and C heapsort `sift_down` implementations to retain the
  root value and move children into the hole rather than swapping at every
  level.

- Fixed raw-buffer deallocation lowering: `free_raw` and `__free_raw` now
  resolve to `rt_free_raw`, preserving unboxed pointer ABI in compiler/runtime
  probes.

- Fixed copy propagation to refresh each rewritten call argument's ownership
  metadata from its resulting producer while preserving the independent formal
  borrow/consume contract.

- Restricted Ny extern-group analysis to `.ny` inputs. A C block comment with
  lines resembling `extern fn` declarations no longer produces a rejected Ny
  record or aborts embedded C audit collection; C full-JSON parity covers it.

- Kept formatter-analysis coverage on native-only, LLVM MCJIT, and LLVM ORC
  routes; the NYIR interpreter exceeded the fixture timeout on this scanner.
  Tightened fast-path guards in the analyzer, corrected the function-metadata
  column expectation, and removed invalid `--color=never`/incompatible sound
  fixture routes.

- Fixed two native benchmark correctness defects. Added the qualified
  `core.dict_mod.list` runtime alias so native heapsort resolves the list
  constructor.

- C still validates the bounded wire protocol and applies safe file edits.

- Moved unified `ny` command-token selection and alias classification into
  `core.compiler.tools.cli`.

- Migrated trivial `#main` wrapper detection (`NYAUD4902`) into the Ny audit
  batch and suppressed its embedded C duplicate.

- Removed the embedded C bug collector invocation after its Ny and C-family
  rules moved to the batched Ny scanner. Fixed dispatch-score decoding so
  boxed batch values do not become pointer-sized counts; independent branches
  no longer trigger false `NYAUD3002` findings, and `collect.ny` trim JSON
  matches the standalone output.

- Moved adjacent extern-group (`NYAUD5001`) collection into the same Ny batch
  and excluded the embedded C duplicate.

- Moved repeated dictionary-access layout findings (`NYAUD6001`) for C-family
  files into the Ny audit batch, then disabled the embedded C duplicate.

- Moved battery findings (`NYAUD2001`–`NYAUD2006`) for C-family sources into
  the Ny batch and disabled the embedded C collector.

- Moved C-family zero-argument property-method findings (`NYAUD8101`) into the
  Ny batch and excluded the embedded C duplicate.

- Enabled colored physical-register arithmetic in the x86-64 object encoder
  for destination vregs whose source has no usable register: the source moves
  through its trusted load channel (immediate rematerialization, sticky rax,
  seeded preg, or canonical home) straight into the destination register and
  the ALU applies in place, skipping the def-side register publish. Copy
  propagation now also folds scalar-class-preserving `COPY` contracts (the
  aliased source's definition already carries the same class), which removes
  the per-copy register publish + home store pairs that dominated hot loops;
  this benefits the Ny LLVM backend equally.

- Replaced Fannkuch's three dynamically indexed integer lists with typed `i64`
  buffers.

- Finished moving Fannkuch's permutation, flip, and rotation operations onto
  typed-buffer loads/stores, removing the remaining dynamic-list indexing.

- Marked `i64buf_load` and `i64buf_store` inline so hot typed-buffer clients
  can avoid wrapper calls.

- The optimized NYIR had cloned the static list at each round-constant access.

- Prepared PBKDF2's normalized HMAC key and inner/outer pads once per
  derivation instead of rebuilding them for each U value.

- Replaced Knight's per-recursion indexed move-list literals with arithmetic
  that derives the same eight `(dx, dy)` pairs from the move index.

- Replaced Euler 9's remaining `(a,b)` search with Euclid's primitive
  Pythagorean-triple enumeration, scaling only coprime opposite-parity `(m,n)`
  pairs whose perimeter divides 1000.

- Samples concentrate in `rt_main`; the disassembly shows repeated stack-home
  traffic in its hot loop. The current evidence does not justify a safe
  allocator/codegen change: the prior matched if-conversion comparison found
  only a noisy ~1.3% median difference and did not isolate this diamond. No
  unmeasured performance optimization was retained. Large native-versus-C gaps
  remain in several benchmark rows.

- CLI parity passes with the expanded parser protocol.

- Repeated literal-key dictionary reads (`NYAUD3005`) now cross the embedded
  boundary as complete Ny issue records.

- Repeated if/elif selector dispatch (`NYAUD3002`) now crosses the embedded
  boundary as a complete Ny issue record.

- File-pressure findings now cross the embedded boundary as complete Ny issue
  records: trim targets (`NYAUD3001`), hot files (`NYAUD1001`), and hot
  functions (`NYAUD1002`). Ny mirrors the C score inputs and repeated-line
  threshold; bounded full-JSON trim and bloat parity covers both trim
  triggers, score metrics, and hot-file/function counts.

- C and header sources now use Ny-computed pressure scores for hot-file
  (`NYAUD1001`), hot-function (`NYAUD1002`), trim-pressure (`NYAUD3001`), and
  append-loop builder (`NYAUD3004`) records.

- Fixed embedded analysis parity for adjacent declaration grouping by
  normalizing indentation on the following source line.

- Layout-boundary records (`NYAUD4101`–`NYAUD4105`), resource cleanup
  suggestions (`NYAUD4201`–`NYAUD4203`), table/key-map findings
  (`NYAUD4301`–`NYAUD4302`), repeated extern advice (`NYAUD4401`), and C
  diagnostic-rule suggestions (`NYAUD4501`–`NYAUD4503`), platform-module
  advice (`NYAUD4601`), Ny and C sibling-function families (`NYAUD4701`),
  derive-block candidates (`NYAUD4801`), and verbose direct-main guards
  (`NYAUD4901`) now come from the Ny audit batch. Embedded mode skips the old
  C metaprogram collector for Ny, C, and header files; standalone `ny-fmt`
  keeps it as the oracle.

- Adjacent variadic-`free` findings (`NYAUD4204`) now cross the embedded
  boundary as complete Ny issue records, including their original source
  column, count-bearing message, and cleanup suggestion.

- REPL `:doc` ambiguity lists and missing-name messages now render through
  `core.compiler.tools.doc.meta`, with caller-provided terminal styles. Source
  snippets now remove leading docstrings and tokenize/style their Ny syntax
  through `core.compiler.tools.doc.source`; C reports renderer failures
  without recreating these presentations, while interactive line editing keeps
  its C highlighter.

- Ny's audit batch now returns complete issue records for branch hazards,
  typed-nil returns, unreachable statements, stalled loop counters, literal
  arithmetic hazards, duplicate dictionary keys and case arms, overwritten
  assignments, frozen-dictionary mutation, discarded list updates, and all six
  battery heuristics.

- Ny now returns complete audit issue records for backend contract hints
  (`NYAUD6101`), dense literal tables (`NYAUD3006`), append-loop builders
  (`NYAUD3004`), and preallocated-list builders (`NYAUD3009`).

- The Ny audit batch now emits complete records for unused private helpers
  (`NYAUD7001`), adjacent extern groups (`NYAUD5001`), and internal profile
  helpers (`NYAUD7201`). Removed the duplicate C-side parser and message
  construction for these records and the migrated Ny bug rules.

- Loop guard findings (`NYAUD3007` and `NYAUD3008`) now travel as complete Ny
  issue records for both Ny and C/H inputs.

- Repeated dictionary-access layout findings (`NYAUD6001`) now include their
  final severity and text in the Ny audit record.

- Unused top-level constants (`NYAUD7301`) and dead key constants
  (`NYAUD7302`) now arrive as complete Ny issue records.

- Corrected `core.str.builder_append_byte` to append the requested raw byte
  instead of UTF-8 encoding it as a Unicode code point. This keeps bytewise Ny
  source formatting from corrupting non-ASCII literals.

- Moved embedded `ny fmt --check` structure-diagnostic rendering and semicolon
  fix-offset selection into the Ny analyzer batch.

- Ny now returns complete issue records for C-family constant branches
  (`NYAUD1102`), empty bodies (`NYAUD1103`), unsafe string helpers
  (`NYAUD1104`), duplicate conditions and operands (`NYAUD1108`–`NYAUD1112`),
  literal zero divisors (`NYAUD1115`), and negative allocation sizes
  (`NYAUD1118`).

- Moved the embedded C/H `NYAUD1103` empty-control-body finding into Ny's
  batched audit scanner. The standalone audit now carries C comment state
  across lines for this scan.

- Extended the Ny C/H scan to constant branches (`NYAUD1102`), repeated branch
  conditions (`NYAUD1108`), duplicate logical operands (`NYAUD1109`),
  self-comparisons (`NYAUD1110`), contradictory literal conditions
  (`NYAUD1111`/`NYAUD1112`), literal zero divisors (`NYAUD1115`), negative
  allocation sizes (`NYAUD1118`), and unsafe string helpers (`NYAUD1104`).

- Moved adjacent simple `free(...)` grouping (`NYAUD4204`) into Ny's batched
  audit scanner. It retains argument/column reporting, nested-call and list
  arguments, and ignores comma-separated, qualified, and quoted lookalikes.

- Moved duplicate dictionary-key (`NYAUD1113`), duplicate case-arm
  (`NYAUD1114`), adjacent overwritten-assignment (`NYAUD1117`), frozen empty
  dictionary mutation (`NYAUD1119`), and discarded list update (`NYAUD1120`)
  findings for Ny sources into the same scanner.

- Moved Ny literal zero-divisor (`NYAUD1115`), negative-index (`NYAUD1116`),
  and negative allocation-size (`NYAUD1118`) audit findings into the batched
  scanner. The C implementation remains the oracle for standalone `ny-fmt` and
  for C/H sources.

- Moved Ny branch assignment (`NYAUD1101`), constant branch (`NYAUD1102`), and
  repeated `elif` condition (`NYAUD1108`), duplicate logical operand
  (`NYAUD1109`), self-comparison (`NYAUD1110`), and contradictory literal
  conditions (`NYAUD1111`/`NYAUD1112`), typed-nil returns (`NYAUD1105`), and
  unreachable statements (`NYAUD1106`) and stalled while counters
  (`NYAUD1107`) findings into the batched scanner for Ny sources.

- Moved embedded C/H `NYAUD1104` unsafe string-helper findings into the Ny
  audit batch. Its lexical scan ignores quoted text, comments, and member-call
  lookalikes; the standalone C oracle now ignores inline comments for this
  rule.

- Moved embedded `ny fmt --analyze` C-family function range collection into
  `core.compiler.tools.fmt.analyze`. The Ny scanner skips prototypes and
  ignores declarations inside comments and quoted literals; C still backs
  standalone `ny-fmt` as the differential oracle. Embedded output now retains
  C duplicate name findings as well as long-function findings.

- Extended Ny-owned loop-shape audit findings (`NYAUD3007` and `NYAUD3008`) to
  `.c` and `.h` inputs. The embedded C collector now leaves these findings to
  the Ny batch for every scanned source type; standalone `ny-fmt` retains the
  C implementation as oracle.

- Moved both method-syntax audit findings (`NYAUD8101` and `NYAUD8102`) for Ny
  files into the batched `core.compiler.tools.fmt.analyze` scanner. It skips
  quoted literals and semicolon comments, recognizes qualified `bin.*`
  helpers, and keeps method-only and legacy-only modes distinct. The same Ny
  batch now collects `NYAUD3003` helper-indexed-write and `NYAUD3009`
  preallocated-list append findings for trim audits, plus
  `NYAUD3007`/`NYAUD3008` loop guard findings. The specialization scanner now
  owns all five `NYAUD3101`–`NYAUD3105` rules for Ny files, including
  compile-time branch/case analysis and multi-line literal-case thresholds. Ny
  also emits the per-file `NYAUD9001` type summary alongside its existing
  per-function type findings, and supplies the source-level legacy helper and
  receiver-call totals used by methods and legacy audits.

- Moved Ny `NYAUD6001` repeated-dictionary-access layout findings into the
  same batch scanner.

- Moved Ny `NYAUD7301` and `NYAUD7302` dead top-level constant findings into
  the batch scanner, including platform API exclusions and key-constant
  thresholds.

- Moved the `NYAUD6101` backend contract hint into the Ny batch for backend
  source paths.

- Moved Ny `NYAUD3006` dense-literal-table scoring into the batch scanner,
  preserving its bounded code-line scan, score threshold, first declaration,
  severity, and JSON summary counter.

- Moved Ny `NYAUD3004` append-heavy function checks into the batched scanner.

- Moved Ny `NYAUD7001` unused private-helper findings into the batch scanner.

- Moved Ny `NYAUD5001` adjacent extern declaration findings into the batch
  scanner, preserving blank/include/link continuation, bounded symbol samples,
  thresholds, and summary counters.

- Moved Ny `NYAUD7201` unused `internal(...)` helper findings into the batch
  scanner.

- Moved the six `NYAUD2001`–`NYAUD2006` battery findings for Ny sources into
  the batched scanner. The checks preserve neighboring-line windows and C
  string/comment recognition, while C retains the rules for non-Ny files.

- Completed full LLVM stdlib bitcode generation. The target now passes a
  whitespace-only source argument and keeps `--emit-module=std` so the bundle
  contains stdlib definitions without a synthetic `_ny_top_entry`. A
  module-pipeline compilation using `--std-bc=build/release/std.bc` succeeds
  and emits valid consumer bitcode (27,654,896 bytes).

- Moved embedded `ny fmt` argument parsing into
  `core.compiler.tools.fmt.cli.parse_args_batch`; the C parser remains the
  standalone `ny-fmt` oracle.

- Ported the standalone `.c` source formatter into
  `core.compiler.tools.fmt.format.format_c` and routed embedded `ny fmt --fix`
  batches for Ny and C files through one Ny session. `ny-fmt-format-parity`
  compares both CLIs on decorated and ordinary block comments, blank spacing,
  tabs, and CRLF.

- Ported the C-family header comment tidier into
  `core.compiler.tools.fmt.format.format_c_tidy`.

- Ported embedded `ny fmt --check` brace-balance and semicolon-swallow
  analysis into `scan_structure_batch`. Ny returns bounded diagnostics and
  exact semicolon offsets; the C shell applies those edits only after the full
  batch response validates. Structure parity also covers balanced braces
  inside string/comment text and an unmatched closing brace.

- Moved `ny fmt --analyze`'s unbounded C string-helper finding into the
  batched Ny analyzer. It checks `.c` and `.h` lines while ignoring strings
  and `//` comments, returns the `unsafe_c` count with Ny's other analysis
  stats, and retains byte-for-byte output parity with standalone `ny-fmt`.

- Bounded Ny formatter long-function analysis to the 121 lines needed to
  decide the existing 120-line threshold.

- Moved Ny function, public-function, missing-doc, duplicate-name, and
  long-function summary counts into the batched Ny analyzer. Embedded `ny fmt
  --analyze` no longer reparses Ny functions in C; C continues to scan
  C-family functions and remains the standalone comparison implementation.

- Moved audit's Ny function signature scan into
  `scan_function_metadata_batch`. A bounded single-threaded Ny prepass emits
  function ranges, visibility, docs, parameter/return metadata, and inference
  directives before the audit workers start. Ny now also emits typed parameter
  and return findings from those rows; the standalone C implementation remains
  the parity oracle.

- Native lowering now derives a top-level scalar global's storage ABI from its
  declaration, not the current assignment's RHS.

- Inferred mutable globals initialized with signed integer literals now keep
  the raw integer ABI on reads as well as writes.

- Native NYIR call resolution now strips the generated `ny_fn_` prefix before
  consulting runtime aliases, and the qualified dictionary-length wrapper has
  the same raw runtime mapping as its unqualified form.

- Native call lowering now keeps a same-source user function ahead of an
  imported same-name stdlib function.

- Mutable top-level lists initialized from literals no longer qualify for the
  immutable raw-index equality shortcut.

- Mutable list append calls now copy aliased local buffers before updating the
  receiver slot.

- Native scalar comparisons now preserve raw integer payloads from immutable
  homogeneous tuple indexes and byte indexes.

- Socket async operations now use the runtime's `__errno()` builtin.

- The shape test runner now ignores braces in quoted metadata, comments, and
  embedded source heredocs while finding fixture boundaries.

- Shape source-tree materialization now skips sibling fixtures without an
  embedded Ny block instead of failing the entire tree.

- Native lowering preserves the BigInt representation when a wide literal is
  aliased through another binding.

- Dynamic-list member `get` lowering now uses the raw-index tbuf accessor
  instead of decoding raw loop indices as tagged integers. A reduced async
  accumulation now reads `1, 2, 3, 4` and sums to `10`.

- Mutable globals initialized from dynamic-returning calls now retain their
  tagged ABI across reassignment, including negative integer results.

- Guarded native caller-module resolution so a name-only fallback cannot
  replace a semantically resolved function with a declaration that rejects the
  call arity.

- Documented Nytrix embedding and the standalone Nyr frontend API directly in
  `docs/spec/nytrix.md` and `docs/spec/nyr.md` with raw C blocks, including a
  small Lisp-to-NYIR frontend. Removed standalone demo sources and targets;
  retained API and GC-handle regression tests. Test fixtures are grouped under
  `etc/tests/code`, `etc/tests/lang`, and `etc/tests/bench`; backend code
  fixtures sit directly in `code/`; NYIR and source-lowering fixtures use
  `code/intermediate/`; REPL, stress, cache, and tools probes use
  `code/probes/`. Derive fixtures are grouped by behavior. Removed redundant
  cache subdirectories and the standalone lowering folder, updating fixture
  routing, CMake references, direct paths, and documentation.

- Moved `ny fmt --cloc` collection, source-line counting, Git diff
  aggregation, ranking, and JSON/text rendering into
  `core.compiler.tools.fmt.cloc`; `ny-full` calls the Ny entry point through
  `nytrix_embed_run_source`, without a compiler subprocess. The standalone
  `ny-fmt` C implementation remains the differential oracle and is excluded
  from `ny-full`. Fixed `nytrix_embed_run_source` to enable JIT execution; the
  embedded example now exercises a Ny tool function rather than merely
  compiling it.

- Routed REPL `:doc` exact-name, dotted-suffix, basename, and ambiguity
  resolution through `core.compiler.tools.doc.meta.resolve`. Ny
  `doc.meta.render_entry` now composes the entry heading, signature,
  documentation body, colors, and missing-doc fallback; C retains
  ambiguity/miss messages and optional source syntax highlighting.

- Routed unified `ny doc search` through the Ny ranking library in-process. A
  live `ny-doc` versus `ny doc` search for `parser` produced byte-identical
  output during the cutover check.

- Routed Markdown section extraction and production documentation search-index
  row construction through `core.compiler.tools.doc.index`. C keeps the
  filesystem walk and a cheap heading-candidate scan; Ny validates headings,
  computes section spans, and emits the rows consumed by the C index adapter.

- The embedded portal collects Markdown bodies and renders them in one Ny
  batch, keeping compilation out of the per-page loop; standalone `ny-doc`
  retains the C renderer as the comparison path. Replaced per-line UTF-8-wide
  slicing with byte-range slices, and replaced per-byte HTML hex output with a
  length-prefixed raw batch result. The docs bundle parser now advances a
  cached lexical brace-depth cursor instead of rescanning each source prefix
  for every declaration. A UTF-8 and many-line regression joins the existing
  HTML/route fixture.

- Routed embedded `ny fmt --fix` and `--tidy` rewriting of Ny files through
  `core.compiler.tools.fmt.format.format_ny` in one in-process batch. The
  bridge hex-encodes source text through bounded temporary payloads, validates
  every result before applying edits, and leaves C-family formatting on the
  existing path.

- Routed human issue-row rendering in `ny fmt --analyze` and `--audit`, plus
  `ny fmt --analyze --json`, through `core.compiler.tools.fmt.report`. C
  continues to collect, filter, and enrich issue records, while Ny composes
  locations, severity colors, audit metadata, notes, JSON schema, and limit
  summaries. `ny fmt --analyze` and `--audit` summary rows also use Ny's
  aligned, wrapped key/value renderer, and audit JSON uses the Ny JSON
  serializer. C continues to collect and enrich audit records.

- Moved the embedded analyzer's shared trailing-whitespace, tab, and
  TODO/FIXME scan into `core.compiler.tools.fmt.analyze`, then moved
  visual-width counting, compact-line exemptions, import-style findings, and
  compact `return(...)`, control-block spacing, repeated-append chains, and
  repeated `mut` markers; reversed fields in `layout` blocks now use the same
  bounded batch, as do adjacent declaration and assignment grouping
  suggestions. Ny also detects oversized function bodies, duplicate public
  names within a file, and missing public stdlib docstrings; it recognizes
  preceding documentation comments, body docstrings, and simple forwarding
  wrappers. Ny now supplies function metadata to embedded audits and aggregate
  counts.

- Routed Ny-source `ny fmt --optimize --diff/--apply` through the embedded Ny
  formatter batch used by `--fix` and `--tidy`.

- Removed the lexer’s whole-prefix scan for semicolon comments at ordinary
  parenthesis depths. Tracked delimiter state preserves `for`/`if`/`while`
  initializer separators; nesting beyond the inline tracking depth retains the
  previous source scan. A capped 25-second stdlib probe now reaches lowering;
  the pre-fix profile spent 59% of its samples in `lexer_next`.

- Closed the P2 aggregate-ABI gap in the self-contained C frontend. `nyc` now
  lowers struct/union values end to end: by-value parameters and arguments
  (register-class, MEMORY-class, and mixed INTEGER/SSE with the shared SysV
  register counters), register-class returns through the two-eightbyte RET
  pieces, MEMORY-class returns through the hidden caller buffer, struct locals
  and arrays with zero-filled storage and member stores, and aggregate
  assignment through `NYIR_COPY_STRUCT`. Function-entry ABI travels as
  `NYIR_PARAM_AGGREGATE` plus packed `param_abi` words and
  `return_aggregate_size`/`return_aggregate_classes`, serialized in
  `NYIR_BINARY_VERSION` 19 and validated by the NYIR verifier.

- Fixed two optimizer/codegen defects uncovered by that work, both reachable
  from ordinary O2 compiles. `nyir_func_replace` zeroed the replacement's
  `param_count` whenever `param_abi` was NULL, so later passes treated
  parameter slots as uninitialized locals and folded parameter loads to zero
  (loops collapsed under `iv_elim`); and the x86-64 object writer lowered
  `NYIR_COPY` for float values through the integer spill path even when the
  value was register-resident, computing a zero frame displacement and
  returning garbage.

- Its temporary input path is unique per invocation, and the parent only
  accepts the rewritten source after successful completion.

- Extended comptime caching per the roadmap: closed immutable-global name
  references (keyed by binding identity), `len`/`range`/`range2`/`Z` calls,
  and member `.get` forms are memoized with shadowing checks; target queries,
  environment reads, and unresolved calls stay uncached.

- Finished read-only compiler reflection. Dynamic dictionary index callbacks
  now carry their joined builtin and custom effect rows through local aliases,
  matching existing list/tuple index and `.get` flows; mutation still
  invalidates stale container facts. The live-field Ny derive templates and Ny
  compound-assignment rewrite both have C-reference differential fixtures.

- Cut production search ranking over to the Ny `core.compiler.doc_rank`
  library and verified ordered-result parity across a ten-query matrix over
  docs/lib corpus, including alternatives, private-name penalties, and
  zero-match queries.

- Completed the ranking cutover: removed the legacy C scorer and
  `NYTRIX_DOC_C_RANK` switch.

- Reworked `ffi.gen` scanning to classify bytes and slice whole token,
  declaration, and parameter spans instead of allocating a string per
  character.

- Replaced the standalone Python derive AST parity checker and its six
  script-only `.ny` inputs with `derive-templates-parity.nshape`. Removed the
  Python CTest hook. Removed the unused `doc-rank-parity.cmake` harness;
  `doc-rank.nshape` remains the direct test for the Ny ranking library.

- Completed the bounded comptime expression memo's semantic keying. Immutable
  globals resolve to binding identities; closed receiver expressions and
  positional arguments participate structurally; pure user functions include
  their resolved declaration identity and evaluate through the bounded
  interpreter. Target queries, `__main`, and registered refinements stay
  uncached because they depend on compiler or typing context; random,
  effectful, and unresolved calls are uncached as well. Each parallel module
  worker owns an independent memo.

- Fixed intrinsic dispatch when standard-library `range2` is present and
  preserved user shadowing for `range2`. The comptime-caching TODO item is
  closed, and the cache's supported scope and exclusions are documented in the
  comptime spec.

- Comptime string `.get(index[, default])` now resolves closed string literals
  in the LLVM fast evaluator, counting UTF-8 code points and supporting
  positive and negative indexes.

- Trimmed the open comptime-caching TODO to its remaining acceptance gaps;
  landed closed-form evaluation and its regression coverage remain documented
  here.

### 2026-10-09

- Specified the accepted `requires`/`decreases` proof calculus and closed its
  bounded acceptance item. Unsupported or inconclusive formulas, calls,
  predicates, and measures remain `unknown` and fail closed; the contract does
  not claim completeness for nonlinear arithmetic or arbitrary termination.

- Plain `parse_ast` records now include declaration attributes, so compiler
  reflection sees the same attribute names as versioned snapshots. GPU
  platform checks use the exported `platform.is_windows()` and
  `platform.is_macos()` APIs. NYIR local stores now materialize an explicit
  object-class copy when an aggregate-class producer enters an object-handle
  slot; this keeps imported nominal values consistent through method and
  operator calls.

- `sound.get_backend_name()` now reports `"none"` whenever initialization has
  no active context, including an empty stored name.

- `swapped` now uses the existing append-based copy path for empty and
  single-element lists.

- Explicit `int` initialization from `.long` now avoids decoding values
  already emitted as raw integers, including comptime constants.

- Dynamic ternary joins now box raw scalar arms whenever either arm's
  arithmetic depends on `any` inputs.

- Arithmetic consuming `list.get` now follows the lowered result's scalar ABI:
  a tagged dynamic result is decoded even when its receiver is statically
  declared as a list.

- Native dictionary `merge` calls now preserve the receiver's scalar ABI
  because the runtime returns that same receiver. The focused Pollard module
  emits valid LLVM bitcode after this correction.

- Added versioned effect-row module interfaces with exact imported-signature
  matching and per-call substitution of known closed callback rows for
  imported open rows. Shared signatures remain unchanged; unsupported callback
  provenance fails closed. Relocated the derive parity Python test out of
  `etc/tests/language/compiler/` and removed that directory's bytecode cache.

- Consolidated NYIR backend tests under `src/code/backend/test/`: optimizer
  drivers are in `opt/`, and the public IR acceptance test is in `ir/`.
  Updated CMake, the generated registry include, command includes, and testing
  documentation. Merged the public evaluator API wrappers into `eval.c` and
  removed `eval_api.c`. The full optimizer registry currently fails in
  `runtime_call_contracts` with `runtime lookup mismatch for
  rt_f64_from_bits`.

- A completed full-bundle attempt then exposed a NYIR call-class mismatch in
  `core.math.gf.solve_gf2`: `rt_len` received a tagged indexed-list value
  while the call retained an aggregate source-formal class. The runtime-call
  emitter now records the physical carrier class for polymorphic `rt_len` and
  `rt_len_strict` calls.

- Refinement guard typing now asks the bounded overflow-aware contract solver
  to prove supported pure relational implications, including stronger lower
  bounds and conjunctions. Candidate specificity now instantiates both
  predicates on the guarded subject before solver comparison, selecting
  `Positive` over its implied weaker `Nat` predicate instead of rejecting the
  guard as ambiguous. Formal matching and solver substitution preserve each
  predicate's syntax context. Versioned refinement interfaces now serialize
  qualified identities, base/formal names, and supported canonical predicates;
  imports reconstruct predicate ASTs into the compiler arena and reject
  malformed, incompatible, duplicate, or unsupported records. Canonical AST
  roundtrips cover relational/logical, float, and escaped-string predicates.

- Pure predicate inlining for `requires` now composes multiple sequential
  no-`else` early-return guards into one exact formula.

- Pure predicate expansion now follows immutable local callable aliases and
  specializes a callback formal only when the actual target resolves to a pure
  function. Finite `match` predicates also lower closed integer-literal range
  patterns with inclusive endpoints.

- Pure predicate expansion now supports a single immutable `if` initializer,
  substituting its value into the guard and both branch results.

- Pure predicate expansion now lowers closed match expressions nested in
  supported boolean expressions.

- Closed integer-range patterns also lower in statement-form pure matches.

- Sound backend startup now passes the connected backend context directly into
  stream selection instead of relying on its module-global copy, and device
  list reads reject non-list values before taking their length. On the hosted
  PulseAudio workstation, `sound.init(false)`, generated sine-source
  `sound.play()`, and shutdown completed with real PCM output from an imported
  synthesized WAV resource. The facade reports the active backend and resets
  that name on shutdown. ALSA PCM handles are now explicitly unboxed at the C
  boundary, and ALSA's silent `null` candidate was removed so a missing device
  fails instead of claiming playback success. With an invalid PulseAudio
  server, initialization now returns false without crashing in
  `snd_pcm_set_params`.

- Attached-method re-export coverage now includes a nested consumer module
  with a same-named free function, local method-name value, imported-module
  alias, and nominal type name; receiver dispatch remains owner-qualified on
  all four routes.

- Completed backend API isolation: LLVM emission/JIT adapters, source-program
  entry points, runtime-aware NYIR evaluation, and the AST x86-64 fallback are
  language-owned; runtime source manifests and runtime build orchestration
  moved to the language pipeline, with process spawning in the base layer.
  Backend JIT symbol lookup, mapping registration, VM evaluation, and profile
  collection use injected runtime services. The neutral x86-64 NYIR encoder is
  included in the backend-only `nyr` library; CMake rejects direct frontend
  includes in that source set. Shared native buffer layout constants live in a
  neutral public header.

- Affine loop interchange now supports write-only accesses to external pointer
  roots when every stored address retains the frontend's in-bounds proof and
  affine analysis proves an injective iteration map. Different external roots
  remain potentially aliased and are rejected; memory reads and unproved
  addresses remain barriers. `loop_interchange.c` now checks loop behavior,
  including external buffer contents, aliasing, bounds traps, and overflow
  outcomes, without IR-length or bound-layout assertions.

- Native x86-64 code generation now keeps canonical stack homes for functions
  with carried scalar machine values, avoiding O3 loop-state corruption.

- Centralized built-in vector alias recognition in the language type layer and
  routed LLVM and native lowering type checks through its semantic dimension
  query. The backend no longer carries its own `vecN`/`VectorN` name tables.

- The chosen operator target remains stable on NYIR, MCJIT, ORC, and
  native-only (4/4).

- The LLVM NYIR emitter now receives a neutral backend module and an
  `ny_llvm_context_t` with injected source-origin, debug-scope, and global
  symbol resolvers. Program lowering and the codegen-to-backend context
  adapter moved to `src/code/lang/pipeline/native.c`; the compiler pipeline
  and REPL call the language adapter. The neutral NYIR lowering declaration is
  exposed from `src/code/lang/lower.h`. The LLVM emitter, language adapter,
  pipeline, and REPL translation units compile; a release `ny` build
  subsequently linked successfully after the source-program API move.

- Refinement guard matching now canonicalizes operand-reversed integer
  comparisons, including their complements in the opposite branch.

- Mutable refinement subjects may now carry a guard fact when both the branch
  and the enclosing function are proven read-only by the AST checker. The
  function result is computed once when entering HM inference. It rejects
  assignments, indirect memory operations, closures, calls, deferred bodies,
  and unknown syntax; mutable transport is limited to scalar representations.
  Writes, captured mutation, and aggregate/object aliases remain fail-closed.

- Extended comptime expression memoization to structurally closed `.get` calls
  whose receiver is a dictionary literal and whose one or two positional
  arguments are closed expressions. The call key includes the dictionary,
  method, and argument structure; named arguments and nonliteral receivers
  remain excluded.

- Added opaque `void *` function parameter and return support to the
  standalone C frontend.

- Added interleaved seven-integer/nine-double definitions and direct calls to
  exercise independent integer-register and XMM-register exhaustion. The
  generated objects matched host-C references for both entry arguments and
  forwarded calls.

- The closed-expression comptime memo now evaluates and owns list/tuple slices
  with omitted or explicit starts/stops, negative bounds, and positive or
  negative steps; step zero remains unsupported.

- Native reachability now follows the canonical operator declaration selected
  by shared typing. This retains module-scoped operator targets that are not
  nested in `impl` blocks. Native lowering resolves the selected module target
  from the function index and carries its canonical declaration into the call.

- The nested chained re-export fixture additionally combines method-name,
  free-function, module-alias, and nominal-type shadows.

- Added public opaque `nytrix_value_handle_t` roots for raw Ny words. Root
  slots survive moving collections; reads synchronize with the collector and
  writes use its barrier. `nytrix-embed-gc-handles` forces a major collection
  and verifies the rooted object remains live.

- Moved source/file embedding orchestration and its process-wide compile guard
  from the NYIR interpreter implementation to
  `src/code/lang/pipeline/embed.c`; `src/code/backend/ir/embedding.c` now owns
  the backend interpreter entry point.

- Renamed the internal backend target header to `src/code/backend/target.h` and
  updated the embedding example include sites.

- Refinement guard transport now recognizes a declared predicate that exactly
  matches either operand of a positive `&&` guard, and an exact complemented
  predicate in either operand of a false `||` arm. Logical `!` carries the
  selected guard polarity through the same exact matcher. It does not infer
  facts from disjunction true arms or conjunction false arms.

- Exact relational refinement transport now searches registered refinement
  declarations from loaded modules as well as the importing source tree.

- Loop unswitching now handles an invariant one-sided branch through an
  unlabelled branch-only exit trampoline when the loop has a proven positive
  trip count and its header and condition-block prefix are pure control. The
  optimizer dispatches to the existing loop header or the resolved exit and
  preserves the selected internal body edge. `loop_unswitch` verifies both
  branch outcomes and the zero-trip boundary.

- Refinement flow transports a source refinement from an exact alpha-matched,
  side-effect-free literal/formal relational predicate on an immutable local,
  in `if`, `while`, and C-style `for` true bodies. Positive conjunctions may
  contribute either exact operand; false disjunction arms may contribute
  either exact complemented operand. Facts are restored at branch/body
  boundaries. The matcher rejects calls, unrelated identifiers, unsupported
  logical implication, and ambiguous refinement matches. Mutable bindings
  remain unsupported until alias and write invalidation are tracked.

- Attached-method coherence now rejects repeated method names on one owner
  even when declarations have different arities. The compiler does not yet
  implement arity-based attached overload dispatch, and previously such
  declarations could survive validation only to resolve by signature-table
  order.

- Moved the embedding examples and their CMake registration to `src/examples`,
  keeping repository demos outside compiler implementation. Updated all build,
  status, spec, and changelog paths.

- Split callgraph input into a borrowed NYIR module view and neutral callgraph
  options, and moved target selection/layout records into `nyr/target.h`.
  Backend target discovery now consumes `nyr_target_request_t`; source options
  translate into that record at the boundary. Program-to-IR callgraph
  orchestration now lives in the language pipeline, leaving the backend report
  entry IR-only.

- Completed the broader CFG translation-validation acceptance. Also repaired a
  work-tree regression in `mach_build_imm_fold`, which returned false on
  success and pushed every constant-bearing function to the legacy NYIR object
  writer.

- Landed safe f64 row/column vectorization for the nested tbuf address shape
  (`payload + row_bytes + column * 8`). The vectorizer's address decomposition
  now peels nested `ADD_I64` row bases outside the candidate loop, keeps the
  dynamic bounds proof (guarded min/max over the full scalar induction range,
  which bounds every widened lane because the vector body stops at `vec_end -
  VF` and a scalar tail covers the remainder), and keeps the dependence rules
  intact: a dynamic-offset write still rejects every other access on its root,
  and cross-root independence still requires the swap-pair proof. The x64
  encoder gained the missing halves this needed: vector live ranges now
  allocate (V256 types were missing from the vector class filter, so every
  vector value took the scratch path), vector colors moved to xmm8-xmm13 with
  the scalar float allocator clamped below xmm8 so both XMM populations
  coexist, fully-resident SIB addressing serves widened loads and stores
  (`vmovdqu ymm, [base+idx*8]`), and V256 copies broadcast immediates and
  scalar SET1 sources instead of copying adjacent stack bytes into the upper
  lanes. A partial-residency SIB variant (colored base, RAX/R10 scratch index)
  miscompiled stencil loops and was backed out; it is documented as a TODO
  constraint in `docs/TODO.md`.

- A chained module re-export regression exposed imported nominal types
  retaining their short alias while method signatures carried the declaration
  identity. Type-name resolution now follows the import/re-export alias chain
  for known layouts. An earlier module-owned enum-constructor attempt exposed
  an unrelated native NYIR lowering limitation and was not retained.

- The LLVM comptime fast evaluator's bounded structural memo now owns and
  clones cached values, including allocated strings and nested dictionary
  values, and frees them with the codegen context. Closed dictionary literals
  support comptime `.get(key[, default])` lookup, including last-write
  behavior for duplicate keys and explicit or default-zero missing-key
  fallbacks; the receiver literal is structurally memoized. Closed UTF-8
  string indexing counts code points; list/tuple indexing supports negative
  sequence indexes, and dictionary indexing evaluates keys structurally.
  Slices and invalid indexes use the existing fallback. Closed equivalent
  expressions in distinct AST nodes reuse results at the same evaluator depth.

- Fixed runtime C compilation after the parser AST began including the public
  Nyr IR header: runtime compile commands now include `src/include`, and the
  runtime object cache tracks `src/include/nyr/ir.h` with a bumped cache
  revision.

- Closed the Ny-owned reflection/codegen cutover item for the existing
  compile-stage generators. Live compiler reflection and rewrites over live
  compiler state remain under the read-only reflection item.

- Native attached-method lowering now trusts the canonical declaration
  selected by shared type validation. The direct-call AST fallback and the
  second receiver/member lookup are removed, so an unvalidated member no
  longer silently lowers as an attached method.

- The C lexer, parser, and Clang bridge now live under `src/code/lang/c`, and
  the obsolete `src/code/lang/ffi` directory is gone. Backend example
  verification is blocked by an unrelated compile error in
  `src/code/backend/object/x64.c:4422` (`mach_vector_types_present`
  undeclared).

- Extended the public Nyr API to emit x86-64 COFF and Mach-O objects for an
  explicit target triple; `nyc --target=...` exposes the same target selection
  without requiring the `ny` CLI or LLVM.

- Kept two QS accumulators in the raw-integer lane: the target-prime-bound
  growth step and byte-sieve maximum score now use typed integer intermediates
  and explicit comparisons instead of generic `min`/`max` results.

- The source compilation pipeline driver and its cache, stage, and tiny-path
  fragments now live in `src/code/lang/pipeline`; its entry header is
  `src/code/lang/pipeline.h`. The pipeline coordinates parsing, semantic/type
  stages, and backend invocation, so it belongs with the language compiler.
  The NYIR verifier and two NYIR optimizer passes also dropped unused parser
  AST includes.

- Pure user-function call sites now retain the required `CALL` effect bit when
  the inferred transitive effect summary is empty. The missing tag prevented
  bounded recursive constant evaluation from recognizing `gcbench`'s
  allocation-free optimized `tree_count` body.

- VRP now models `rt_argc`'s tagged result as `(argc << 1) | 1` for C's
  nonnegative `int argc` range. This proves `argc()`-derived positive divisors
  nontrapping, so LICM hoists `licm-math`'s invariant signed division from its
  million-iteration loop.

- `core.compiler.tools.doc.rank.rank` now computes each matching entry's fuzzy
  score once and carries it through stable insertion sorting. Sorting no
  longer reruns fuzzy matching for every comparison; score, kind, full-name,
  and source-order behavior are unchanged.

- `bigint-oracle` now has a GMP C reference for its add/subtract/multiply/
  divide/sqrt/power workload. Parallel benchmark workers no longer print
  out-of-order completion rows by default; `NYTRIX_BENCH_PROGRESS=1` opts back
  in.

- Fixed the `@thread` argument-count transport at the spawn boundary: the
  attribute-call lowering emitted the ABI argument count tagged (2n+1) while
  `rt_thread_prepare_call_args` validates it as a raw scalar, so every call
  with eight or more arguments failed in `pthread_create` preparation and the
  value-position join crashed on the -1 handle; counts of seven or fewer were
  dispatched with doubled-plus-one arguments and read past the caller's
  argument buffer. The attribute path now emits the raw count, matching the
  list-form `thread_spawn_call` lowering and the declared runtime ABI, and
  `rt_thread_join` rejects non-positive handles as the declared invalid-handle
  error instead of invoking `pthread_join` on them.

- Fixed the executable entry contract in `nyr_build_rt_main`: the call to a
  user `main` was emitted only when no top-level statement had left a tail
  value, so any trailing module-level statement (including a plain `def`)
  suppressed `main` entirely and — for a trailing `def` — returned the
  initializer's value as the process status. The entry now always calls a
  user-defined `main` after top-level statements and returns its value; a
  module-only unit without `main` keeps its no-op entry.

- Direct immutable refinement guards (`if Refinement(value)`) now establish
  the nominal refinement only in the true arm. The fact follows same-typed
  local aliases and is discarded after the arm; mutable bindings are not
  narrowed. Refinement predicates are classified as pure by effect analysis,
  so a guard does not make an otherwise effect-known function unknown.

- Added `core.math.checked` with checked signed-64-bit add, subtract,
  multiply, divide, and negate operations plus saturating
  add/subtract/multiply. Checked operations return `Result` values with
  `err.overflow`, `err.div_zero`, or `err.type`; exact BigInt intermediates
  avoid relying on wrapping machine arithmetic.

- Refinement returns now fail closed in shared HM typing when the returned
  value is neither already of the declared refinement type nor proved by the
  compile-time predicate evaluator.

- Shared operator analysis now attaches the uniquely selected operator
  declaration to the expression, and native lowering consumes that identity
  before its compatibility AST walk.

- Clarified heap-policy tradeoffs using Baker's *Lively Linear Lisp*: tracing
  GC remains optional because the paper's no-tracing result depends on
  single-use values, no visible sharing/cycles, and explicit copying; its
  optimized shared heap still uses reference counts and hash-consing.
  Documented Nytrix's manual, RAII, RC, and GC policies and corrected the
  status summary.

- Grouped compiler-facing libraries under `lib/compiler/`: typed syntax and
  quote APIs moved from `core.ast` to `core.compiler.ast`, and Ny code
  generators moved from `core.codegen` to `core.compiler.codegen`. Updated
  imports, fixtures, tooling references, and guides. Fixed the formatter
  collector self-check's stale source paths.

- Added `core.compiler.tools.doc.rank`, a pure Ny implementation of the
  current documentation fuzzy scorer, field weights, query alternatives, and
  stable score/kind/full-name ordering. C still owns production ranking;
  differential scoring over the real search index remains required before
  switching the CLI.

- Matched C's private-function scoring when an entry has an unqualified
  `full_name`: the private-name penalty uses the function's `name`, as the C
  search does when no qualified leaf exists.

- Made C documentation search ordering deterministic for entries tied on
  score, kind, and full name by using index insertion order as the last
  tie-break.

- Closed the `@thread` default-argument ABI hole: NYIR now rejects calls whose
  expanded ABI exceeds 15 arguments with the callee-specific diagnostic and
  source location, before spawn lowering; the LLVM path rejects oversized
  signatures before substituting defaults.

- Preserved integer-index ABI for dynamic `.get` calls when the surrounding
  numeric context widens an integer-only index expression to f64.

- Kept dynamically extracted decompiler jump-table entries on the boxed `any`
  ABI when passing them through state-case helpers; those values are lists at
  runtime but are not statically typed NY lists.

- Canonicalized f32 values to the descriptor-backed list's f64 payload ABI
  instead of sending f32 bits through the integer tag bridge.

#### Standalone Nyr C compiler path

- Split the Nyr VM-facing wrappers and static-array lookup into separate
  translation units. The Linux x86-64 `nyc` executable can now link the static
  Nyr library without pulling the optional runtime handle-registration symbol
  or LLVM/libclang libraries; VM evaluation returns no static-array address
  when that runtime hook is unavailable.

- Normal compiler builds retain that profiling.

- Added function/data sections and section garbage collection to the
  standalone static compiler link so unused optional API paths do not add
  runtime dependencies.

- `ldd` on that `nyc` showed only libc, libm, and libgomp (no LLVM, libclang,
  or Ny runtime).

- The C frontend still lacks aggregate support, broader grammar/ABI coverage,
  and CFG-aware native backedge liveness; this is not a full-C or LLVM
  replacement claim.

### 2026-10-08

- Non-LLVM CMake builds now omit the LLVM language and backend implementation
  sources from `nytrix_compiler`. The standalone Nyr library / C compiler
  target is still not available in this configuration.

- The Nyr-backed C frontend now accepts C empty statements (`;`) in compound
  blocks and as control-flow bodies.

- C block declarations may now contain multiple declarators with independent
  initializers, including declaration-before-assignment locals. Executable
  host-C parity covers initialized and uninitialized declarators in one
  declaration; `for` initializer declarations support multiple loop-scoped
  declarators as well.

- Organized the compiler-interfacing Ny libraries under `lib/compiler/`:
  `fmt_collect`, `test_collect`, `doc_meta`, and `doc_rank` moved there with
  module paths `core.compiler.*` (matching the existing
  `core.reflect.semantic` subdirectory convention), and the five fixtures plus
  the fmt/web/test CLI Ny-program strings were updated to the new paths.

- Completed the frontend rename `ny_native_nir_*` → `nyr_*` (388 symbols,
  including the `NY_NATIVE_NIR_*` fact macros → `NYR_*`), unifying with the
  public `nyr_` builder API the concurrent migration had already introduced
  (`nyr_builder_t`, `nyr_fail`); no collisions — both targets were the same
  typedef/helper bodies.

- Merged `src/code/backend/llvm/string_init.c` into `strings.c` (its only
  external symbol `codegen_emit_string_init` is declared in `priv.h`; the rest
  were file-local), completing the backend/llvm consolidation.

- Fixed documentation header parsing and collection: title extraction now
  survives keyword normalization, and keyword scanning continues across
  leading `;;` title comments.

- Preserved the enclosing effect contract while summarizing closure bodies.

- Pure predicate proof expansion now follows immutable, unshadowed global
  function aliases. Four-route fixtures prove the positive caller and continue
  rejecting a caller without evidence for the predicate's precondition.

- Native runtime ABI metadata now preserves typed-buffer result classes for
  append, extend, reserve, clear, set, swap, and concat operations.

- Dictionary lookup ABI metadata now boxes dynamic fallback arguments while
  preserving typed-buffer fallback values. Nested float defaults and integer
  keys pass across NYIR, native, and both LLVM JIT engines.

- GLTF component and animation readers now widen binary32 loads to f64
  explicitly.

- CVP beam states now keep costs and coefficient vectors in parallel lists,
  avoiding an aggregate-to-typed-buffer call mismatch while retaining aligned
  beam candidates.

- Disassembler target parsing accepts an explicit row base for operand
  strings, preserving RISC-V relative target calculation without passing
  heterogeneous row aggregates through dynamic call boundaries.

- Effect-row v1 decoding now reconstructs closed nested generic applications,
  arrows, rows, numeric or symbolic `Fin` payloads, pointer/nullable and
  `struct`/`enum`/`layout` spellings, and non-atom concrete names through the
  `concrete#` hex form. The design note keeps effect rows separate from
  authority; they describe effects and grant no permissions.

- Fixed a native-lowering compile error by keeping the resolved callable value
  in scope for both local and global lambda lookup paths.

- Renamed selected native lowerer operation, constant-folding, failure,
  comparison-emitter, fact, and loop-frame symbols to the compact `nyr_`
  namespace. This is an internal symbol cleanup; emitted NYIR and runtime
  behavior are unchanged.

- Extended the public Nyr C API to version 1.1 with `nyr_eval_with_calls`,
  exposing synchronous host-call resolution over raw NYIR words while keeping
  `nyr_eval` as the no-resolver entry point.

- Expanded `nyc` to compile `void`-returning functions, explicit `return;`,
  implicit fallthrough returns, and direct function calls used as statements.

- Closed an affine loop-interchange dependence hole: individually injective
  stores into the same allocation can still overlap across iterations with
  different affine maps. A transposed 2×3 case changed the observed result
  from 10 to 1 before the fix; interchange now rejects that candidate and
  permits distinct same-allocation stores only when their complete byte ranges
  are disjoint.

- Widened loop vectorization for four-lane f64 elementwise memory/arithmetic
  bodies. SOR still does not vectorize: its nested row-base-plus-column
  `ADD_I64` address chain hides the typed-buffer root from the current direct
  root matcher. The vector trace now prints the address and addend opcodes;
  SOR reports `add.i64` / `shl.i64` with `root=-1`.

- Pure predicate inlining now lowers exhaustive boolean matches without a
  default arm, but only for a statically typed boolean subject with unguarded
  `true` and `false` patterns, in both expression and statement forms. Partial
  or guarded pattern sets remain unproved.

- Preserved closure-owned callback ABI metadata when generic call-site markers
  revisit an already marked closure. A native call through a returned untyped
  closure had overwritten its tagged result contract with the caller's
  inferred raw-integer contract; `rt_dynamic_callable_with_flags` now leaves
  an existing closure ABI intact.

- Fixed local shadowing in native container-literal callable classification. A
  parameter named `type_name` could be mistaken for the imported
  `core.type_name` function and stored as a callback marker instead of a
  string.

- Applied the same lexical-precedence rule to native callable-result ABI
  inference and callback lookup: a non-callable local no longer falls through
  to a same-named top-level function, while recorded callable aliases still
  resolve through their local metadata.

- Fixed compile-time existential witness memoization: each quantified integer
  candidate now has an immutable AST identity, so the pointer-keyed comptime
  memo cannot reuse the first candidate's value for later witnesses.

- Retired the comptime LLVM-module snapshot fallback. Unsupported comptime
  forms fail at their source with a diagnostic. The snapshot escape hatch and
  its bitcode/JIT implementation have now been deleted; the interpreter is the
  sole comptime evaluation boundary.

- Advanced `std.bc` emission from broken to two named defects, with the first
  class of fixes landed: untyped list locals in
  `lib/math/crypto/factorization/classical/gf2.ny` (vectors, vectors_packed,
  images, image_la, deps, solution) and `qs.ny` (`relations`, `poly_reports`)
  now carry explicit `list`/`dict`/`int`/`f64` declarations, which stabilizes
  their storage carriers under the call ABI verifier. The remaining blocker is
  a newly identified frontend defect, recorded in the TODO with an exact
  reproducer: in `_mpqs_best_adiv_mask` the implicit function-value handle for
  `_dict_with` is allocated to the same local slot as the f64 local
  `best_score`, so the mark-dynamic-callable funnel receives an f64-classified
  handle and the post-optimization ABI verifier rejects `std.bc` emission.

- Documented `bigint-oracle` as deliberately non-comparative: the harness
  C-reference path links libc/libm only and a fair bigint reference would need
  GMP, a non-hermetic dependency. Merged `prepare_links.h` into `prepare.c` as
  part of the llvm/ organization pass.

- Restricted the per-codegen comptime scalar memo to closed literal/operator
  expression trees. Identifiers, calls, member access, and nested comptime
  blocks are excluded so AST-address reuse cannot cross evaluation
  environments.

- The fast comptime evaluator now memoizes up to 128 successful scalar AST
  results per `codegen_t`, keyed by node identity within that immutable
  program context. Cache hits consume the shared comptime fuel budget;
  sequence and heap-backed results are not cached, and entries never cross
  compile sessions. Fuel exhaustion is diagnosed before the unsupported-form
  diagnostic.

- Dynamic dictionary callback storage now preserves boolean result ABI
  metadata across returned factory dictionaries, including both `true` and
  `false` results.

- Dynamic tuple and list storage now preserve named NYIR callback handles and
  their raw-versus-tagged argument conventions and integer/boolean result ABIs
  across factory returns. Dictionary storage uses the same typed marker
  selection. Indirect-call lowering recognizes direct sequence-index
  expressions as well as sequence `.get` calls.

- Comptime LLVM snapshot entry names now derive from the source file and token
  location and resolve collisions within the snapshot module. They no longer
  use a process-global incrementing counter, so concurrent evaluations do not
  race on generated names.

- Removed the formatter CLI's duplicate C recursive file collectors. The Ny
  collector also fails closed when a root or discovered path disappears or
  cannot be listed.

- Reorganized `src/code/lang/llvm/` into category folders with one-word names
  and retired the `legacy/` quarantine: `comptime_fast/if/interp.c` became
  `comptime/{fast,if,interp}.c`; `legacy/gencall/` became `calls/`;
  `legacy/expr/` became `expr/`;
  `legacy/{binary,core,debug,func,module,stmt,syms}.c` and `genintrinsics.c`
  moved to the `llvm/` top level under their own names; `gencalltypes.c` and
  `expr_profile.c` became `calltypes.c` and `profile.c`; the `core/` fragment
  headers stay beside `core.c`. The CMake globs and amalgamation
  `OBJECT_DEPENDS` tracking moved with the files (the `calls/init.c` and
  `expr/init.c` units still textually include their sibling fragments, and
  only those two units compile standalone), and all comment/doc path
  references were updated — the active LLVM emitter is no longer named
  "legacy". The remaining true legacy boundary (`gen_comptime_eval` module
  snapshots, interpreter parity, `std.bc` completeness) stays open in the
  renamed TODO item.

- Fixed compiler source-list initialization for the LLVM call and expression
  amalgamations.

- Callback effect inference now joins both outcomes of a direct ternary-return
  factory when each branch returns a complete immutable dictionary, list, or
  tuple with a matching shape. Dictionary keys must match in order; mismatched
  shapes remain unknown. It also joins a single top-level if-return and its
  fallback return, plus up to 32 explicit `match` arms when an `else` or
  unguarded wildcard arm makes the match exhaustive and all containers match.
  The simple if-return form accepts up to 32 immutable local prefix bindings.

- The same returned-container recognizer now includes guarded `match` arms
  when each guarded arm and the required default directly return matching
  containers. Guard expressions continue through the ordinary effect visitor.

- Dictionary callback values now retain callable identity through NYIR dynamic
  storage. Dynamic dictionary `.get` calls recover a matching callback result
  ABI from immutable literals or immutable factory returns, including dynamic
  keys when every candidate has the same result ABI. The existing
  dynamic-index callback matrix also remains green.

- A new negative compile shape pins the documented `@thread` f64-return
  diagnostic. Probing the ABI boundary found one real defect, recorded as the
  item's remaining work: an `@thread` call with eight or more source arguments
  under default-argument sugar bypasses the lowered-argument gate and crashes
  at runtime in the spawn path (non-default calls receive a clean diagnostic;
  the runtime dispatch itself handles 15 ABI arguments). `@async_effects`
  keeps its documented effect-analysis hint.

- Trimmed `docs/TODO.md` from 626 to 205 lines: every item now states its
  remaining acceptance criteria first, with landed sub-case narratives
  delegated to this changelog.

- Added `core.os.fs.list_dir_checked`, which returns `nil` when a directory
  cannot be opened while preserving an empty list for a valid empty directory.
  Documentation metadata collection uses the checked API and now fails closed
  on failed subtree opens instead of silently omitting them.

- The x64 object encoder now elides an adjacent, single-use scalar `ADD` when
  its address result feeds a scalar load. At the ADD, it captures the
  still-live base and either the raw or already-scaled index in reserved
  RAX/R10; the load consumes that pending SIB address. Shared, non-adjacent,
  vector, and store consumers retain the ordinary ADD.

- Callback effect analysis now joins rows from every callable entry in a
  complete immutable dictionary for dynamic `dict.get(key)` and `dict[key]`
  calls, and composes a dynamic `dict.get(key, fallback)` with the fallback's
  custom capability row. Direct-return factories preserve callback rows for
  returned dictionary/list/tuple literals and bounded chains of immutable
  local aliases, including rows supplied through callback parameters;
  factories with control flow or mutation stay unknown. Captured aliases
  preserve the summary; mutable dictionaries stay unknown, including after
  mutation.

- Website head links, RSS, static-page discovery, and `ny-doc search` doc
  enumeration now use the Ny documentation library exclusively; duplicate C
  tree walks are removed. Collection and RSS write errors propagate as
  site-build or search failures. The full `ny-doc` output builds, and a
  pre/post cutover comparison found identical file sets. Only the
  changelog-derived page, feed, index, and manual text differed because the
  docs changed between builds. An unavailable Ny collector now makes search
  fail instead of silently falling back to C.

- `core.doc_meta.collect` now fails on unreadable supported documentation
  files instead of silently omitting them.

- `core.doc_meta.resolve` now implements the REPL documentation lookup order
  in Ny: exact name, unique dotted suffix, then basename. REPL rendering
  remains in the C driver.

- `core.reflect.semantic_diagnostics` now exposes immutable HM diagnostic
  records, and `semantic_find_diagnostic` looks them up by stable code and
  optional source location.

- Two defects: `mach_vextract_i128` set the VEX R/B extension bits inverted,
  so every extract from ymm8-15 silently read an unextended register (the V256
  i64 reduce summed garbage lanes), and `mach_sib_lea_mem` folded single-use
  indexed LEAs under vector load/store consumers although only the scalar
  memory paths emit the folded SIB addressing — eliding the address
  computation and leaving the consumer's canonical home unwritten. f64 loop
  vectorization stays rejected inside the vectorizer: its lane math still
  models two lanes per VEC4_F64 op and needs dedicated widening plus SOR f64
  parity verification.

- Comptime templates can splice a reflected layout's ordered field sequence
  into a generated function signature with `comptime fields(Layout)`;
  `comptime fields(Layout, required)` omits defaults for fixed-arity APIs.
  Function cloning expands these parameters through the shared parameter
  expander, preserving field names, types, defaults, origins, and the template
  splice's syntax context.

- `core.codegen.new_fields_for` and `store_for` now generate positional
  constructors and fixed-arity typed layout stores from reflected fields.
  Reflected `${field.name}` values splice as identifiers in expression slots,
  allowing generated bodies to refer to the matching parameter hygienically.

- C `derive(store)` now emits typed per-field writes and returns safely when
  its destination is null.

- `core.codegen.default_for` now emits a no-argument constructor using
  reflected explicit and implicit field defaults. C-derived default
  constructors now return safely on allocation failure; the pinned legacy
  derive AST digest was refreshed.

- `core.codegen.default_ctor_for` now matches C `derive(default)`'s typed,
  defaulted parameter list. Field parameter splices preserve explicit defaults
  and synthesize implicit scalar defaults; `comptime fields(Layout, required)`
  keeps store values mandatory.

- C `derive(zero)` now returns null on allocation failure before calling
  `memset`, matching Ny `zero_for` safety.

- `core.codegen.load_fields_for(Layout)` now emits typed per-field
  `Layout_load_field` functions as well as the ordered dictionary reader.

- The C debug-string derive now uses the same ordered accumulation as Ny, with
  identical output and null handling.

- `core.doc_meta.collect` now rejects an existing file as its root; the
  collector requires a directory before bounded traversal starts.

- Unknown fallback rows remain unknown.

- The NYIR VM now preserves opaque user-function handles returned by dynamic
  `.get(index, fallback)` reads, including fallback misses, and marks
  statically known fallback functions with their raw scalar result ABI.

- Native module-member resolution now checks module-qualified top-level values
  before treating an identifier as a namespace.

- NYIR now lowers direct callback invocations from a one-index list/tuple
  `.get` expression as indirect calls.

- NYIR dynamic callback lists now retain the callable identity through indexed
  reads. The VM tags recognized opaque function handles as closures when
  building dynamic list descriptors and resolves zero-argument indirect calls
  through its function table instead of jumping to the handle as a C address.

- Four-lane f64 is now native end to end. The type map classifies the
  `VEC4_F64` family into a new 32-byte `V256_F64` machine representation,
  `ny_mach_lower_nir` accepts all ten opcodes, and the x86-64
  machine/object/JIT encoders select VEX.256 float forms: vaddpd/vsubpd/
  vmulpd/vdivpd with an explicit VEX.vvvv accumulator, vbroadcastsd SET1,
  vfmadd213pd fused multiply-add (admitted only on targets with a detected
  fused-FMA capability — a two-rounding mul+add substitute is refused),
  vextractf128 half extraction, general four-lane shuffles assembled from lane
  broadcasts plus cross shufpd on the home path, and reductions in the
  evaluator's sequential lane order. Several pre-existing latent encoder
  defects surfaced and were fixed along the way: the ymm register helpers
  silently dropped the VEX R/B extension bits for registers 8-15, the slow
  V256 store treated its value operand as an address, vector pointer operands
  read possibly-elided homes instead of the preg-aware path, and `UNPCKHPD`'s
  lane order had been misread. The AArch64 object fallback now emits all ten
  opcodes as paired two-lane NEON sequences over 32-byte homes (fused `fmla`,
  TBL-based shuffles, pairwise reduce), while its machine form fails closed by
  design so the from-NYIR caller selects the fallback.

- Native lowering now distinguishes zero-field enum variants from module
  namespace paths before resolving attached methods. Static variant receivers
  are preserved when lowering calls such as `Shape.Empty.method_area()`.

- Native indexed reads now prioritize the whole-program proof for append-built
  integer lists over the conservative dynamic-sequence accessor.

- Dynamic `any.get` now converts its already-tagged fallback to the raw
  fallback expected by the typed-buffer bridge, preventing a missing nested
  sequence lookup from tagging an integer default twice.

- Syntax registry membership checks now treat an absent raw-map entry as
  false, so builtin and user attribute names are retained in registration
  order.

- Native compile-time proof handling now validates tactic tokens before
  accepting a witness, so unknown `by_` strategies fail closed.

- Prolog substitution now stops on an absent raw dictionary binding instead of
  treating it as a present variable link.

- Comptime-generated recursive functions now retain direct-call identity
  through effect analysis, contract/decreases proof checking, and native call
  lowering. Generated `@total` countdown contracts pass through NYIR,
  native-only, LLVM MCJIT, and LLVM ORC.

- Native lowering now treats syntax-spliced identifiers as module namespace
  roots.

- Native `len` now checks registered typed-buffer handles before tagged-string
  recognition.

- The example builds/verifies NYIR, registers functions and a host callback,
  exercises nested execution plus callback re-entry into the same VM, and
  validates the returned result. It also verifies that duplicate registration
  fails without replacing the original function, registry changes are rejected
  inside callbacks, and a depth-limit failure unwinds cleanly for the next
  call. Function unregister, missing-call failure, and re-registration are
  verified. A host callback failure is also propagated through the VM API and
  verifies the VM accepts a subsequent call. Exact arity and per-call step
  budget failures also have recovery checks. The public header now states
  callback borrowing, reentrancy, registry-mutation, VM lifetime, and
  thread-safety constraints. Source loading, managed value handles, and
  runtime startup/shutdown remain unimplemented.

- `dict_pop` now routes membership, lookup, and deletion through the
  dictionary abstraction, which handles both managed and native storage.

- The self-contained C frontend now lowers `&&` and `||` with C precedence and
  short-circuit evaluation, returning normalized `0`/`1` values.

- The self-contained C frontend now supports prefix/postfix increment and
  decrement for mutable integer and double locals, including for-loop steps.

- Updated the Nyr API specification to describe the current C frontend subset,
  including short-circuit logic, conditional expressions, increment/decrement,
  compound assignments, dynamic declaration storage, and the 64-argument
  call-site limit.

- The C frontend now lowers `+=`, `-=`, `*=`, `/=` for integer/double locals
  and parameters, plus integer `%=` and bitwise compound assignments. Integer
  and floating local examples plus scalar/indexed pointer updates match host-C
  reference values and memory in `nyr-c-frontend`.

- The C frontend now lowers `do`/`while` loops.

- The C frontend now lowers signed integer shifts and `<<=`/`>>=`. C-reference
  cases exercise shift counts, signed negative right shifts, additive-versus-
  shift precedence, and compound shifts on defined inputs; `nyr-c-frontend`
  passes.

- The C frontend now lowers `pointer + integer`, `integer + pointer`, and
  `pointer - integer` for its 64-bit integer and double pointers, scaling the
  element offset before address arithmetic. Pointer differences and other
  pointer compound updates remain rejected.

- Const-qualified pointer parameters now distinguish pointee immutability from
  pointer-variable mutability: pointer offsets/increments and reassignment are
  allowed, while stores through const data pointers remain rejected. Host-C
  parity covers postfix increment, reassignment, and compound offset through a
  pointer-to-const parameter.

- Caller-side proof bindings now size to the formula instead of stopping at
  eight names; the solver and versioned certificate budget are aligned at 16
  variables and 64 constraints.

- Added `core.codegen.zero_for(Layout)`, a Ny compile-time generator for a
  layout-sized zero-initialized allocation.

- Added `core.codegen.load_fields_for(Layout)` to generate an ordinary Ny
  function that reads all layout fields through `load_layout`.

- Added `core.codegen.eq_for(Layout)` for null-safe, fieldwise equality.

- Added `core.codegen.hash_for(Layout)` with the legacy ordered field hash
  fold and null behavior.

- Added `core.codegen.debug_str_for(Layout)` with the legacy field order,
  formatting, and null marker.

- Added `core.codegen.defaults_for(Layout)` and reflected `field.default_expr`
  / `field.default_value`.

- Added `store_layout_field(dst, "Layout", "field", value)` with layout-schema
  type/offset validation and matching NYIR/LLVM stores.
  `store_fields_for(Layout)` emits a name-checked dynamic writer plus typed
  per-field writers, `store_dict_for(Layout)` emits a whole-layout writer that
  supplies defaults for omitted dictionary keys, and `new_for(Layout)`
  allocates an instance from named overrides and defaults. `new_for` also
  rejects invalid keys and releases its allocation.

- NYIR now boxes f64 values by their actual producer class when storing them
  in generic dictionaries.

- The shared NYIR list-append boundary now boxes f64/f32 values before generic
  tagged appends.

- `load_layout` now decodes stored f32 bits and boxes them for dynamic
  consumers, while preserving declared f32 and f64 return ABIs.

- Declaration-name splices now accept reflected identifier facts such as
  `${field.name}` and `${field.type}`. The cold four-route
  `comptime-reflection-name-splice` fixture generates and calls a helper for
  each layout field.

- `table_gen` also now has exact f64 cell formatting coverage (`1.5`, `2.25`).

- `core.reflect.semantic_find_function_at` now resolves duplicate function
  names by authored declaration location and fails closed when origins are
  ambiguous.

- Semantic source-location lookups now require correctly typed file, line, and
  column fields; malformed origins are rejected instead of coerced.

- The independent proof-policy interval checker now proves bounded signed
  division when the divisor's interval is strictly positive, using
  truncation-toward-zero bounds.

- `core.reflect.semantic` now exposes direct immutable accessors for an
  expression's resolved type, runtime representation, ownership, and versioned
  expression-effect record, custom expression and statement effect rows,
  mutability, and alias-class facts.

- `core.fmt_collect.collect` now enforces hard maxima of depth 256 and 100,000
  visited entries even when callers request larger limits, rejects empty or
  non-string roots before traversal, returns unique sorted paths, and fails
  closed when a root or discovered directory disappears or cannot be listed
  instead of silently formatting a partial tree.

- Semantic expression-effect aggregation now marks unhandled expression kinds
  unknown instead of treating them as effect-free.

- Custom callback capability rows now survive a returned closure that reads
  the callback from an immutable dictionary alias. The generated-origin
  consumer now identifies library-origin binary expressions instead of pinning
  a source line that moved as `codegen.ny` grew; emission location is still
  checked.

- The seven C layout derives were audited against Ny runtime generators. The
  audit initially found that template signatures could not expand typed field
  sequences. That gap was closed with `comptime fields(Layout)` parameter
  splicing, `new_fields_for`, `store_for`, and `default_ctor_for`; exact AST
  parity now covers all seven C-backed layout derives.

- Callback effect analysis now preserves callable rows through immutable tuple
  literals and bracketed constant-index reads, matching its existing list
  handling. List `get` reads preserve callback facts during analysis, while
  mutating and unknown list methods still invalidate them.

- Dynamic indexing into an immutable callback list or tuple now uses a summary
  only when every literal slot has a known callback and composable effect row.
  It unions builtin masks and custom rows across all slots and carries the
  summary through immutable aliases; mutation still invalidates it. Incomplete
  and mutable containers stay unknown.

- Match expressions now inherit the conservative union of their scrutinee,
  guards, and every arm's effects.

- Logical, ternary, index, member, dereference, and `try` expression rows now
  aggregate evaluated children. `sizeof` is resolved to an empty runtime row
  without visiting its unevaluated operand. HM now resolves the target type of
  `sizeof(expression)` without evaluating it, and NYIR plus LLVM lowering use
  that resolved type to emit the size constant.

- Runtime effect analysis now skips quoted code and proof-query syntax instead
  of attributing calls inside the stored syntax to the containing function.
  The comptime IO scan retains its conservative syntax traversal.

- Audited effect aggregation against the AST enums: all 29 current expression
  kinds and 32 statement kinds have explicit classifications, with new kinds
  remaining unknown through the fallback.

- Closed custom capability rows now survive aggregation through expressions
  and statements.

- Compile-time expression nodes now carry a resolved empty runtime-effect row
  even though the traversal skips their compile-time bodies.

- Fixed `core.report.format_error` and `format_warning` dropping the location
  prefix after constructing it.

- Environment cache hashing now binds each value to its variable name,
  distinguishes unset from empty values, and includes value lengths. The old
  value-only concatenation let different single-variable settings collide.
  JIT, std bitcode, AOT, std, and module cache identities were bumped. It also
  verifies unset and empty interpreter-only values use distinct entries.

- Whole-program AOT, standard-source, and standard-bitcode cache keys now bind
  `NYTRIX_COMPTIME_FUEL`, `NYTRIX_COMPTIME_INTERP_ONLY`, and
  `NYTRIX_TEST_MODE`. Module-bitcode keys bind the same inputs. The
  corresponding cache schema versions were advanced.

- Fixed `core.test.collect.collect` counting the root and every queued child
  against its visited-entry bound.

- Unqualified custom capability names declared inside a named module now
  receive that module's identity; explicitly dotted names stay unchanged. The
  capability contract matrix adds a matching qualified-owner call and rejects
  a same-named top-level capability that would previously collide. The
  row-algebra test round-trips the resulting qualified capability label. The
  cold/warm cache fixture now also derives `project.math.read` from
  unqualified `cap(read)` in its provider.

- Nested function effect facts now use the lexical leaf name used by local
  calls, rather than the module-qualified AST name. Nested function contracts
  are now installed in the nested-body effect context before summarizing calls
  to sibling module functions; this prevents the enclosing function's broader
  row from masking an underdeclared nested row.

- Module bitcode cache identities now include semantic/compiler options,
  relevant solver and comptime environment values, including
  `NYTRIX_COMPTIME_INTERP_ONLY`, standard-library paths, and link inputs.
  Module workers inherit strict-type, effect, alias, ownership, borrow,
  solver, safe-mode, standard-library mode, and heap settings.

- `core.reflect.semantic_obligations` now enumerates versioned proof
  obligation records attached to calls in a function. The ambiguity-safe
  `semantic_find_obligation` locates `requires` and `decreases` records by
  call origin and reuses the trusted call lookup rules.

- `core.reflect.semantic_statement_effects` now enumerates immutable statement
  effect records, and `semantic_find_statement_effect` provides ambiguity-safe
  lookup by kind and authored location.

- `core.reflect.semantic_signature`, `semantic_parameters`, `semantic_return`,
  and `semantic_signature_effects` expose checked function signature facts
  without requiring callers to traverse the generic snapshot.

- The independent bounded proof checker now tightens interval facts from
  overflow-safe multi-variable linear constraints, not only one-variable
  bounds.

- Added bounded `core.fmt_collect.collect` with deterministic `ny`, `fmt`,
  `code`, `c`, and `cloc` modes, extension filtering, and traversal limits.
  Formatter check, rewrite, optimize, C alignment, duplicate scan, and cloc
  paths now use its Ny-generated file list; analyze and audit now use its
  `code` mode as well. Existing C walks remain fail-safe paths. Formatter
  discovery resolves the sibling `ny-full` executable from `ny-fmt`; the
  replacement path is now correctly NUL-terminated.

- `core.reflect` now exposes `semantic_calls`, ambiguity-safe
  `semantic_find_call`, and `semantic_call_effects` over immutable semantic
  function records.

- Bounded-allocation preflight admits statically known loop trip counts
  through the eight-backedge walk limit; regressions prove three- and
  eight-trip cases, detect changed stores, and keep nine-trip loops
  inapplicable.

- `loop_interchange` now checks two individually injective affine stores whose
  shifted footprints overlap across iterations in the same private allocation.
  The pass verifies the function while preserving the original loop order and
  result.

- The semantic statement-effect visitor now includes the initializer subtree
  of `if` statements before aggregating the test and branches. The shared AST
  visitor and callback-alias effect walk now visit that initializer before the
  condition, making its bindings visible to branch analysis. The lexer now
  preserves semicolon separators in `if` and `while` headers as it already did
  for `for` headers.

- Template type substitution now recurses through nullable/pointer prefixes
  and nested generic arguments while preserving the surrounding type syntax
  and numeric layout arguments.

- The unified semantic reflection facade now forwards optional owners for
  function lookup and supports owner-qualified layout lookup.

- Template cloning now substitutes indexed-family enum index and payload
  types, plus layout/struct default-source expressions used by generated
  derive helpers. `#link` library names now use string interpolation rather
  than identifier validation, so generated library names may contain
  punctuation; explicit external symbol names use the same string
  interpolation.

- Compound assignment desugaring now retains the operator token on its
  synthesized binary expression, preserving source origins through template
  cloning and semantic reflection.

- Prefix increment/decrement and dereference compound-assignment desugaring
  now retain operator origins too; synthetic zero initializers use the binding
  declaration token.

- Typed `Code<T>` JSON restoration now rejects payloads above one MiB or
  deeper than 64 containers before exposing syntax.

- The deep-input matrix verifies the dedicated nesting-limit diagnostic
  instead of accepting a later envelope error.

- Added `json_try_decode_bounded` to the shared JSON parser so callers can
  reject excessive container nesting during parsing without a second source
  scan.

- Added immutable semantic function/layout list and name lookup helpers to the
  reflection facade.

- Added semantic expression-list access and ambiguity-safe lookup by
  expression kind and source location, with an optional file filter.

- Template AST cloning now explicitly covers every expression and statement
  kind. Interpolated f-string expressions and inline-assembly operands recurse
  through origin-aware cloning; embedded paths and include path/prefix/library
  fields receive template substitution.

- Native compile-time proof lowering now rejects source integer literals
  outside signed-i64 before evaluating their saturated parser payload.

- NYIR match arms that return through an `any`-valued function now box raw
  integer and f32/f64 arm results at the match join. Float arms use the
  runtime's canonical heap-float representation rather than integer tagging.

- Quoted compile-time emission now stores its expansion-origin token in the
  parser arena instead of retaining the address of `parse_emit_call_body`'s
  stack parameter. This prevents later semantic traversal from dereferencing a
  dangling origin when inspecting hygienic `#insert` declarations.

- Lemma regressions now bound positive operands before asserting a positive
  signed-i64 sum. The former unbounded claim was false under wrapping
  arithmetic at `INT64_MAX`; the independent checker correctly rejected it.

- The Ny typed-config DSL now supports nested table descriptors. Nested input
  is recursively validated for required, unknown, and mistyped fields, and
  errors carry qualified paths such as `service.port`. Nested schemas reject
  recursive validation deeper than 32 levels. Fixed TOML boolean decoding so
  the emitted `true`/`false` values round-trip with boolean type.

- Canonical TOML encoding now accepts and correctly quotes non-bare field keys
  inside nested dictionaries and nested table names, matching the existing
  `_key` escaping path. The cold four-route typed-config fixture round-trips a
  table name with spaces and a nested key containing a dot.

- TOML parsing now decodes basic-string escapes, validates Unicode scalar
  escapes, and splits arrays without treating commas inside strings or nested
  arrays as separators. Config encoding and parsing round-trip escaped quotes,
  backslashes, commas, and supported control bytes; NUL, raw DEL, malformed
  UTF-8, and surrogate code points remain rejected.

- Forged schemas fail with `E_CONFIG_SCHEMA`; required fields with defaults
  are rejected by schema construction. `schema(...)` also reports malformed
  raw descriptors instead of coercing names or flags.

- The arithmetic parser kit now consumes whitespace around operators and
  parentheses using byte-range predicates.

- Call expressions also carry version-1 `nytrix.semantic.obligations` records
  preserving instantiated `requires` formulas, caller premises, source
  origins, and proof outcomes, as well as recursive `decreases` strict-descent
  and `INT64_MIN` lower-bound obligations. Semantic signature slots, typed
  expression facts, obligations, and layouts preserve available template
  emission origins.

- Records use the `nytrix.semantic.call-effects` version-1 schema and keep
  unresolved effect rows explicitly unknown. It also verifies that a call
  through a statically known local callback alias preserves the IO row.

- Typed expression facts now include version-1
  `nytrix.semantic.expression-effects` records with inferred built-in rows
  aggregated from child expressions.

- Unsupported statement kinds now serialize as unknown instead of inheriting
  an empty, apparently complete row; declaration-only and control-only nodes
  are explicitly classified as effect-free.

- The trusted effect visitor composes rows through nested expressions and
  enclosing statements, including known higher-order callback substitutions;
  open tails and unresolved callbacks remain null.

- Runtime `parse_ast`, `parse_ast_snapshot`, and the `parse_code` declaration
  APIs reject inputs larger than one MiB before exposing parsed records.

- The generated-function case also covers loops, match, try/catch, defer, and
  nested expressions. Every serialized syntax node in those expansions must
  retain both its source definition and template emission origins.

- The self-contained C frontend now retains parameter names from a function
  definition when the same function was first declared with an unnamed
  prototype.

- C frontend local storage is sized from the tokenized translation unit rather
  than an arbitrary 32-local limit. A 33-local generated function is compared
  against the C reference executable.

- C frontend function storage now scales with the translation unit as well.

- C function signatures no longer stop at six parameters.

- The C frontend also lowers arguments after the first six of a direct call
  through NYIR's stack-argument payload. C block scopes now permit local
  shadowing but reject same-scope redeclarations; for-initializer names also
  leave outer locals visible after the loop. The generated objects and
  rejection diagnostic have focused host comparisons.

- Semantic-layout JSON emission now reads fields from the current
  `stmt_struct_t` AST union member, keeping the whole compiler build aligned
  with the parser.

- Effect analysis now resolves fully qualified calls into inline modules
  before checking custom rows.

- Template argument evaluation now follows typed `code_*_syntax` accessors
  through bounded pure helper functions.

- Bounded template helper evaluation now covers functions with the declared
  `Result<Code<CodeExpr>, str>` return type and functions that unwrap it to
  `Code<CodeExpr>`, including an immutable local holding the typed constructor
  result, forwarding the quoted AST through the typed constructor.

- Cold `comptime.nshape` coverage now also splices a `Code<CodeExpr>` created
  inside an ordinary zero-argument Ny function, then forwards that returned
  value through a second ordinary helper.

- Module workers now use the CLI's accepted `-no-std` spelling. Linking a
  cached worker drops only function bodies already defined in the entry
  module, preserving the entry definition while resolving declarations from
  the worker.

- The standalone `ny-doc` target now links `src/base/process.c`, which its
  metadata command requires for `ny_process_capture`.

- The docs renderer now consumes those Ny records for SEO links and schema
  entries, and RSS ordering uses their source paths and file modification
  times. The C collector remains a fallback if the Ny subprocess or record
  decoding fails. Recursive file discovery for the embedded manual's Markdown
  list and docs search also uses the Ny path collector; C still parses bodies
  and builds search entries. Static page generation now uses the same Ny path
  list while the C page renderer produces the output; the standalone render
  produced 53 documentation pages and completed successfully.

- Compile-time `code_category` now mirrors the Ny library query for scalar AST
  values and code sequences, while `code_kind` continues to report concrete
  singleton node kinds.

- Template arguments can resolve pure Ny helper functions with positional
  syntax parameters, immutable locals, and a final return expression. Typed
  syntax constructors and nested forwarding helpers preserve the returned
  quote AST; nested helper evaluation is capped at 64 and unsupported bodies
  fail closed.

- Parser-time shortcuts for `parse_code`, `codeof`, typed `code_*` helpers,
  compile-time code queries, `ident`, and typed-code `unwrap` now yield to
  same-named declarations in the current file or module. This keeps ordinary
  local name shadowing intact while preserving the standard-library syntax
  bridge.

- Template arguments now accept the typed `code_expr`, `code_stmt`,
  `code_decl`, and `code_ident` constructors over quoted syntax. The parser
  preserves the underlying AST and validates constructor and template
  parameter categories before expansion, including homogeneous sequence
  splicing.

- Parameters are marked borrowed according to the HM environment; returns
  remain unknown unless an owned or borrowed return contract establishes the
  state.

- Typed HM inference now retains final canonical type names separately from
  backend-directed `resolved_type_name`. Type-variable substitutions are
  finalized after inference before serialization, so the records do not freeze
  early variable spellings.

- The built-in row has its own known bit and uses canonical built-in effect
  identities; it does not claim that custom row labels were inferred.

- Effect analysis now resolves nested functions' closed `@effects` rows before
  their signatures enter the global function table. Capability rows and
  built-in effect names retain their identity at local calls; unsupported
  annotation forms still fail closed. Local callable aliases preserve the
  nested row as well.

- Imported source-module member calls now resolve the module-qualified
  signature before checking its custom effect row.

- Added `core.reflect.fields_with_origins_for(Layout)`, which emits an
  immutable version-1 field snapshot including source file, line, and column.
  The C comptime field descriptor reads the original field token; the existing
  `fields_for` row shape stays unchanged. Added the matching
  `variants_with_origins_for(Enum)` API using each variant and payload field's
  source token.

- Native lowering now treats an imported module-qualified function value as a
  function reference, not a method call with the module namespace passed as an
  argument. The local callable alias retains its canonical function identity,
  so callback invocation uses the correct target and arity.

- Zero-argument imported module calls now retain call semantics during native
  lowering instead of becoming function-value references.

- Inferred `any` locals now box an integer initializer only when the emitted
  NYIR value is actually raw. A dynamic-return user call already produces the
  tagged representation; applying `rt_tag` based on the call expression's
  stale raw-integer annotation double-tagged it and changed values such as
  `add(1, 2)` from `3` to `7`.

- `core.ast` now provides reusable `Code<T>` values for expression, statement,
  declaration, and identifier syntax. Constructors return typed results after
  category validation; identifier values require a marked AST identifier
  rather than a string. Canonical values use a versioned `nytrix.code.value`
  envelope. Typed accessors also reject forged or malformed envelopes whose
  canonical encoding, field types, schema, category, 0/1 sequence flag, or
  syntax category disagrees. The sequence flag is derived from the syntax
  shape when constructing or restoring `Code<T>`, avoiding duplicate mutable
  state.

- Added `core.ast.parse_code_typed`, which combines isolated declaration-only
  parsing with validated `Code<CodeDecl>` construction.

- Added `core.reflect.layout_for(Layout)`, a versioned immutable layout
  snapshot with the compiler-resolved size and alignment plus ordered field
  facts.

- `core.ast.code_signature` now returns a versioned immutable tuple for
  function, extern, and anonymous `fn` syntax, including ordered parameter
  names and types, canonical default expressions, parameter origins, variadic
  state, and declaration attributes. `code_contract` exposes canonical
  `requires` and `decreases` syntax.

- Added `comptime attributes(Declaration) as attribute` and
  `core.reflect.attributes_for(Declaration)`. Ordered declaration attributes
  expose their names; the library packages them in an immutable version-1
  tuple.

- `core.ast.code_attributes` now returns source-ordered declaration attributes
  as deeply immutable `nytrix.code.attributes` version-1 tuples. Each row
  contains the attribute name, canonical argument JSON, and canonical source
  and expansion origins.

- Moved `ny doc meta` header parsing and keyword normalization into
  `core.doc_meta`; the C entry point is now a thin Ny subprocess driver.

- Empty code sequences remain valid for expression, statement, and declaration
  sequence contracts, but no longer satisfy scalar `code<ident>`.

- Spliced call argument lists now have explicit cold arity controls for too
  few and too many expanded arguments.

- Canonical serialization now has a cold quote snapshot round-trip covering
  schema version, source origin, and the distinct syntax marks on quote-local
  and spliced identifiers.

- Versioned quote AST records now carry a `nytrix.code` version-1 category
  record.

- Versioned AST snapshots now preserve source and template-emission origins
  for each selected `use` import name and alias, and for individual module
  export names.

- Directly emitted `quote{...}` syntax now passes through the origin-aware
  clone path, preserving source locations and attaching the `comptime emit`
  site as the expansion origin for declaration, statement, and expression
  quotes.

- Template reflection now attaches emission origins to synthesized field-load
  callees and arguments, nested enum payload tuple rows and leaves, and layout
  offset call trees. Literal and identifier substitutions retain their
  original source tokens; identifier splices also retain their syntax mark
  while carrying the template emission origin on both the splice node and
  inserted identifier.

- Top-level assignments are no longer registered as `codeof` declarations.
  Compile-time declaration queries reject assignment-shaped `NY_S_VAR` nodes,
  so mutating a declared variable does not make `codeof(variable)` ambiguous.

- Fixed declaration-only `parse_code` validation for `NY_S_VAR`, which also
  represents assignment statements. Template-time parsing checks the
  declaration flag before accepting a variable node; runtime `parse_code`,
  `code_kind`, `code_name`, and sequence classification use the versioned
  snapshot's `is_decl` fact to distinguish declarations from assignments.

- Comptime template parameters can now declare `code<expr>`, `code<stmt>`,
  `code<decl>`, and `code<ident>` splice contracts. Category checking handles
  scalar quotes, homogeneous quoted sequences, empty sequences, and assignment
  statements before AST cloning; declaration-as-expression is rejected with a
  diagnostic that reports the expected and received categories.

- Cold REPL imports now resolve stdlib functions in batch and interactive
  sessions, including later prompts after imports. Fixed imported-signature
  ownership across engine rebuilds by deep-copying LLVM names, by-value layout
  strings, and effect allowances; guarded runtime string recognition by
  checking heap ownership before probing arbitrary C-string addresses.

- Payload binders are scoped per arm. Structural `decreases` now accepts a
  recursive call whose measure is a directly bound payload of the same enum
  type in a matching constructor arm, and composes descent through nested
  matches on that payload; rebinding that payload name anywhere in the arm
  invalidates the proof, and a non-descending call on the original value
  remains rejected. Native call lowering now expands chained defaults by
  substituting earlier normalized arguments before lowering later defaults.
  This prevents a formal such as `upper` from being rebound to a same-named
  imported function when compiling at the call site; builder-owned scratch
  storage bounds the cloned expression lifetime.

- Native NYIR lowering now supports range-valued `for` iterables without
  materializing a list: it reads the range length and indexed values through
  the runtime range ABI. Literal list and tuple destructuring lowers each
  initializer element once through the regular local-binding path. The runtime
  ABI declaration table now keeps the lexical ordering required by its
  binary-search lookup; the misplaced `rt_f64_to_i64` row had caused
  floating-point arguments to be classified as unknown.

- Native lowering now preserves dynamic results for list `get` calls,
  including named `key`/`default` arguments, while keeping typed scalar
  results on the raw path. Float comparisons against dynamically indexed
  sequence elements now retain the dynamic float representation.

- Native local initialization now trusts the lowered scalar class, not a stale
  source-level raw-integer annotation, when storing indexed values. Tagged
  values read from an untyped literal list are decoded before entering
  inferred integer locals; equality uses the raw comparison only for proven
  scalar-list reads.

- Added Ny-owned `core.reflect.fields_for(Layout)` and `exports_for(Module)`
  compile-time generators. They copy layout field facts and ordered export
  names into deeply immutable, version-1 tuple snapshots with schema
  identifiers; no compiler-owned descriptor escapes. Added
  `variants_for(Enum)` snapshots with ordered discriminants and typed payload
  field pairs. The enum variant compiler descriptor now exposes immutable
  `payload_fields` tuples to template code. Native call lowering now preserves
  the exact declaration recorded by type inference for unqualified module
  calls, preventing duplicate helper names in sibling modules from binding to
  the wrong function.

- Native lowering now treats a splice's recorded syntax mark as exact during
  identifier lookup.

- Compile-time `ident(...)` now preserves the syntax mark of its source symbol
  through template binding and reconstruction. Identifier code is kept inside
  the exact-context splice wrapper, including identifier elements expanded
  from expression lists, so quote-local same-named bindings cannot capture the
  caller reference.

- `many(parser, 0)` now succeeds with an empty result and no input
  consumption; negative repetition bounds fail with a structured error.
  `choice` now tries alternatives iteratively, so the number of alternatives
  does not consume grammar nesting depth; a 141-branch choice passes with a
  depth limit of 8 and a bounded step budget.

- Compile-time `parse_code` now commits its syntax-mark and deterministic
  gensym counters only after the isolated input parses completely as
  declarations. Malformed, empty, or executable input leaves caller parser
  state unchanged.

- Runtime `core.ast.parse_code` now returns declaration nodes from the
  versioned syntax snapshot, preserving decorator attributes and source
  origins that the legacy AST shape omits.

- `core.ast.code_kind` now classifies homogeneous declaration, statement, and
  expression sequences, and reports empty, mixed, invalid, and unknown
  sequences explicitly. Single-node queries now return `invalid` for malformed
  records and `unknown` for unrecognized node kinds; `code_name` returns names
  only for declarations and rejects malformed declaration-name fields rather
  than coercing arbitrary values to strings. The category/name helpers accept
  both list and tuple code sequences. The AST self-test now explicitly
  distinguishes unknown future node kinds from malformed records for both
  single nodes and sequences. `code_str` now accepts tuple code sequences,
  matching `code_kind` and `code_name`; tuple serialization round-trips
  through canonical JSON.

- Added `core.ast.code_has_attribute` for JSON AST declarations and as a
  compile-time query over `codeof`, `parse_code`, and quoted declarations.
  Template-time `code_kind`, `code_name`, and `code_has_attribute` now match
  library neutral results for scalar values, identifier code,
  non-declarations, and multi-declaration sequences; malformed query calls and
  nonliteral attribute names remain diagnostics.

- Compile-time `code_kind([...])` now derives the sequence category from every
  quoted item instead of labeling every list as declarations. Homogeneous
  expression and statement sequences, mixed sequences, and empty sequences
  preserve the same categories as the `core.ast` query; parsed declaration
  lists report `declarations`. Identifier code values classify as expressions
  and can occupy expression-list splices with their exact syntax marks. Scalar
  quoted expressions now report their concrete AST kind (`literal`, `ident`,
  and so on), matching `core.ast.code_kind` instead of returning the generic
  string `expr`.

- Every serialized record carrying a syntax `type` category in those fixtures
  has `source_origin` and survives canonical serialization.

- `ny test` now gets default and explicitly selected directory path lists,
  `--list-meta`, `--list-bench`, and benchmark fixture enumeration through
  `core.test.collect`; the duplicated C directory walker is removed. Its
  driver retains shape parsing/expansion, selection, scheduling, and
  reporting.

- Fixed the runtime ABI metadata for raw filesystem results: existence and
  directory predicates are raw integers, and directory-open returns a raw
  pointer.

- Renamed the semantic-reflection consumer source to `.ny.in`.

- Completed template AST cloning for generated links, labels, gotos, defines,
  nested modules, and macro bodies/arguments. Identifier-bearing fields are
  substituted in the caller's expansion context, and nested module bodies are
  recursively cloned.

- `core.ast.code_str` now rejects sets, non-finite floats, unsupported scalar
  objects, and non-string object keys instead of producing unstable or invalid
  JSON.

- JSON float encoding now emits 17-digit round-trip values, preserves negative
  zero and integral-float identity, and emits `null` for non-finite values.

- Added `<nytrix/nytrix.h>`, an embeddable NYIR VM API with registered
  functions, raw-word host callbacks, and bounded execution/call depth.
  Lisp/Lua hosts can lower their own forms to NYIR; source-text parsing and
  host-object marshalling remain the embedding application's responsibility.

- Moved their shared support header into `test/support.h` and the type/effect
  row test into `test/typing.c`; no standalone test Makefile is needed.

- Separated typed AST-to-NYIR language lowering into `src/code/lang/lower.c`
  from backend execution, optimization, LLVM integration, wire/build stages,
  machine encoding, and object writing under `src/code/backend/`. Moved
  optimizer implementation tests and their runner under
  `src/code/backend/ir/opt/test/`. Host JIT memory/runtime management lives
  beside NYIR emission in `src/code/backend/ir/jit.c`.

- Restored the language-lowering fragments beside `src/code/lang/lower.c`,
  made the builder typedef available before those fragments, and aligned dump
  exports and the shared function-pool limit with the Nyr backend names. CMake
  now compiles the legacy LLVM expression initializer, not its included
  `core.c` and `f64.c` fragments as independent translation units.

- Moved the embedding API CLI test to `src/cmd/ny/test.c`; its declaration now
  lives alongside the command in `src/cmd/ny/test.h`.

- Made `src/include/nyr/` the source of truth for the installed Nyr API: moved
  the public NYIR declarations to `nyr/ir.h`, installed the API header
  directory directly, and removed the generated-header copy and internal
  forwarding header. Private IR implementation headers remain in
  `src/code/backend/ir/`; compiler and optimizer consumers now include the
  public contract explicitly.

- Replaced libclang parsing in `nyc` with an in-tree lexer/parser that lowers
  the currently supported C subset through the public Nyr API.

- Extended the supported C subset with initialized and mutable signed-integer
  and double locals, parameter updates, direct signed 64-bit pointer stores,
  and direct integer/double function calls across separately emitted objects;
  generated native objects match C references across 135 deterministic
  argument and mutation cases.

- Added direct and indexed `double *` loads and stores, plus C's equivalent
  unsized one-dimensional array parameter spelling for integer and double
  pointers and direct pointer-argument calls, to the Nyr-backed C frontend.
  Const-pointee writes are rejected; generated x86-64 ELF objects match the
  host C reference.

- Consolidated the Nyr-backed C frontend's indexed pointer address calculation
  for reads and writes, centralized scalar-type lookahead, removed unused
  parser state, and clarified that the FFI header declaration parser and
  standalone function-body parser share one lexer but serve separate
  contracts.

- Removed `nyc`'s redundant post-optimization verification call; Nyr object
  emission verifies the optimized function before code generation.

- Restored the native array-table interner's shared declaration, definition,
  and lowering callsite after the internal symbol rename; this fixes runtime
  loading of `nytrixrt` for the standalone `nyc` compiler.

- Separated private native-target and program-object names from the public Nyr
  target descriptor and single-function object emitter. Updated recovered
  lowerer fragments to the current target initialization and IR-build APIs.

- Tightened the public Nyr surface: `nyr_func_t`/`nyr_inst_t` aliases, named
  O0–O3 levels, fallible emission with separate value IDs and diagnostics, and
  Nyr-specific capability bits for evaluation and x86-64 ELF output. Installed
  IR declarations now use `<nyr/ir.h>` rather than an internal-looking detail
  path. Object capability is omitted on hosts this API cannot emit for.

- Track `const` on scalar locals and pointer parameters, reject writes through
  const-qualified data, reject volatile objects and unsupported pointer
  arithmetic/comparisons, and diagnose out-of-range integer literals instead
  of silently wrapping them. Statement-lowering now propagates semantic errors
  raised after consuming a complete statement, preventing invalid pointer
  stores from being silently omitted and accepted.

- Both `while` and restricted `for` bodies can now return directly; the loop
  exit remains available when the condition initially fails.

- A `for` declaration is scoped to the loop; use after the loop is rejected.

- Add scaled signed-`long` pointer subscripting for loads and stores, with
  reference-compiler checks across five array positions and const-store
  rejection.

- Restored the statement-parser amalgamation's static helper declarations
  after its core/comptime split; the compiler and `nyc` targets now build from
  the split sources without implicit declarations.

- Reorganized typing pipeline implementation into
  `src/code/lang/types/pipeline/{core,hm,emit,init}.c`, the independent
  certificate checker into `src/code/lang/types/policy/check.c`, and compiler
  pipe amalgamation units into
  `src/code/backend/wire/pipe/{cache,stage,tiny,init}.c`. The `*_init.c`
  textual inclusion model and incremental object dependencies remain explicit
  in CMake.

- Caller-side `requires` inlining now handles pure predicate functions that
  explicitly return or finish with a supported finite literal `match` with a
  default arm.

- Moved statement-parser components into
  `src/code/lang/parse/stmt/{core,comptime,dispatch,init}.c` and retained
  their textual amalgamation through `parse/shared.c`.

- Legacy nested `@effects(cap(...))` annotations now reject capability names
  longer than the 63-byte identity limit instead of silently omitting that
  row.

- Callback-row analysis now snapshots and joins local aliases across
  `try`/`catch` normal and handler paths when the try body does not call or
  mutate tracked callback aliases. A body call or alias write makes the
  summary unknown because a catch may observe an intermediate exceptional
  state.

- Dynamic native integer stores now resolve the indexed destination before
  selecting a distinct source register. The encoder now avoids address
  registers, spills any resident value occupying its chosen scratch register,
  and loads the store source only after address resolution. These performance
  gaps are not fixed by the correctness repair.

- The x86-64 object encoder folds a same-block, single-use scaled `LEA`
  directly into an indexed load/store SIB operand. It caches the index in R10
  across eligible floating-point instructions; the definition map no longer
  mistakes `NY_MACH_STORE.dst` (an address use) for a value definition.
  `machine_home_reuse` checks the emitted indexed accesses, index-register
  reuse, and generated-code results. The object dump confirms direct indexed
  loads and store.

- The f64 loop-vectorizer now fails closed until all backends agree on
  `VEC4_F64` width: NYIR operations have four-lane semantics, but native
  machine lowering still represents them as two lanes. `loop_vector_metadata`
  now checks f64 memory reduction semantics remain unchanged and the unsafe
  vector load is not formed.

- Named closed effect rows now participate in effect-contract checking.
  Positive custom-row joins remain unsupported. Nested functions with
  unresolved custom effect annotations fail closed when called rather than
  silently dropping the annotation. Capability authority remains a separate,
  unimplemented interface.

- Recursive `@total` checking now has four-route coverage for bounded affine
  measures with a nonzero constant offset (`2 * n + 3`), in addition to the
  existing zero-offset affine countdowns.

- The independent proof checker now validates bounded nonlinear measures used
  by `@total` when it can prove both strict descent and a finite lower bound
  without signed overflow.

- The bounded interval proof path now handles products of distinct variables
  as well as squares.

- Caller-side `requires` checking now transports bounds from inclusive `for
  item in start..end` loops when both endpoints are simple integer identifiers
  or literals, the upper endpoint is proved below `INT64_MAX` (including by a
  `Fin<N>` bound), and the body does not mutate either endpoint.

- Block-scoped effect analysis now preserves callback-row facts when
  committing local updates to an enclosing scope. Unresolved and incompatible
  open-row joins remain fail-closed.

- Parser `gensym` now preserves full prefixes instead of silently truncating
  names at 127 bytes. Counter exhaustion rejects generation before wraparound;
  parser-local numbering remains unchanged. Named modules now have independent
  counters and reversible hexadecimal namespaces, preserving names when module
  declaration order changes. F-string and isolated declaration parsing share
  the enclosing name-generation state.

- Template cloning now expands deferred cleanup bodies, try bodies/handlers,
  and while-loop invariant expressions. Versioned loop snapshots retain
  invariant syntax and its source/emission origins; substitution and canonical
  round trips are covered without changing legacy loop record shape.

- Template expansion now clones external signatures and lemma declarations,
  including parameter provenance, substituted declaration names, and lemma
  propositions. Statement decorators and their argument ASTs are cloned per
  emission instead of shared with the template.

- Template string substitution and string-valued AST splices now preserve
  counted bytes, including NUL in the original literal, after the literal
  splice site, and inside the bound string argument.

- Template cloning now substitutes refinement names/base types/predicates,
  trait declarations and their methods, and implementation trait names/laws.
  Existential predicates also clone their bodies rather than sharing an
  unexpanded formula.

- Template cloning now expands function `requires`, `decreases`, and `@proves`
  expressions instead of sharing their unexpanded template AST.

- Parameter parsing now retains declaration tokens and binding scope marks.
  Versioned parameter records expose those marks and source origins; template
  cloning preserves definition metadata and adds emission origins for
  function, lambda, and layout parameters.

- AST string literals now use counted JSON escaping for every control byte,
  including embedded NUL. Formatted-string parts use the same counted writer;
  assembly strings and link/include paths use JSON escaping. The Ny builder
  byte API preserves NUL using an explicitly counted one-byte string.

- Versioned statements now retain binding scope marks, and variable records
  retain mutability, declaration/destructuring flags, and type annotations.
  Oversized AST integer literals now expose normalized exact source text with
  an explicit out-of-range flag and null numeric payload, rather than
  presenting the saturated internal int64 payload as their value.

- Versioned layout snapshots now retain deftype parameters, layout flavor,
  methods, field defaults with their source spelling, and array-extent syntax
  with expression origins. Struct snapshots retain the same field metadata and
  methods.

- AST serialization now represents refinement and trait declarations and
  existential predicates instead of emitting unknown-node placeholders. This
  lets `core.ast.parse_code` accept those valid declaration kinds. Versioned
  enum records retain generic parameters, indexed-family metadata, and typed
  payload fields; implementation records retain their trait and law syntax.

- Versioned AST snapshots now expose ordered statement attributes with parsed
  argument nodes and per-attribute source origins. Generated decorators carry
  emission origins. Functions also retain `requires`, `decreases`, and their
  documentation string.

- AST literal JSON now emits 17 significant digits for binary64 values and
  uses portable signed 64-bit integer formatting. Precise fractions, small
  exponent values, and `INT64_MAX` have a cold serialization regression. JSON
  number decoding now keeps its floating result in an explicit `f64` local and
  extracts number text through the string slicing API. JSON null decodes as
  nil, independently of integer zero.

- Added `core.ast.code_str` for canonical compact JSON serialization of AST
  records and versioned snapshots. Key ordering now uses a Ny merge sort with
  lexical string comparisons. JSON encoding distinguishes integer zero from
  nil instead of testing equality with zero.

- Added `core.ast.code_kind` and `code_name` for AST record inspection.

- Added `comptime variants(Enum)` reflection for declaration-order variant
  names, indices, payload presence, and payload arity.
  `core.codegen.match_gen` emits a typed variant-name matcher for unit and
  payload enums using the pattern-only `variant.pattern` descriptor. Template
  emissions now retain definition origins and expose their emission site as
  `expansion_origin` in syntax snapshots; generated declarations are validated
  in the calling module.

- Expanded cold acceptance to native-only for root `parse_code` insertion,
  generated-module insertion, and `table_gen(Layout)`.

- Returned callback factories now analyze their local prefix statements with
  the existing effect-flow visitor instead of rejecting every non-declaration
  prefix.

- Nested closures that capture and forward a callback alias now preserve its
  latent effect row.

- Lexicographic `@total` measures now lower boolean components as boolean
  formulas instead of embedding integer literals as proposition nodes.

- The x86-64 object encoder can omit a dead indexed address home for a
  load-address consumer, float store, or narrow-immediate store when
  immediate-use and liveness checks prove the RAX value can be forwarded.
  Dynamic integer stores retain the home. Fresh native-tier-opt assembly still
  contains load-address home saves and an output-store address store/reload.

- Added deferred layout reflection inside compile-time templates and three
  Ny-owned generators, `core.codegen.json_for(Layout)`, `soa_for(Layout)`, and
  `table_gen(Layout)`. They emit a field dictionary, a structure-of-arrays
  projection; generated declarations pass through ordinary typing and code
  generation. A dedicated AST marker distinguishes deferred queries from
  user-written calls. Template cloning now also recursively expands lambdas,
  including reflected member splices inside callback bodies.

- Consolidated the documented metaprogramming syntax around `comptime
  template`, `comptime emit`, `quote {... }`, and `${value}`. The `$`
  interpolation spelling now has one documented role across template
  arguments, quoted AST insertion, and template-string interpolation;
  hash-prefixed spellings remain compatibility aliases.

- Added `codeof(name)` as a template argument intrinsic for previously parsed
  named file- and module-level declarations. It preserves the declaration AST
  for category-checked insertion, resolves qualified names, and rejects
  missing, ambiguous, or expression-slot uses.

- A literal `parse_code("...")` template argument now parses declaration-only
  source in an isolated parser and supplies its AST nodes as a `${...}` splice
  list. Parser registries are copied before parsing so new source declarations
  cannot mutate the caller's `codeof` lookup state. Generated output is
  checked by the ordinary pipeline. The four `parse_code` negative controls
  pass.

- `table_gen(Layout)` now emits ordered headers and string rows as a pure
  library adapter.

- A polymorphic helper's open effect variable now receives the union of rows
  from all direct named callback arguments instead of being unified repeatedly
  with each callback independently. Row composition preserves a single open
  tail and composes distinct open tails by binding each to the other's
  label-residual plus a fresh shared tail.

- Lexicographic recursive measures accept finite boolean components before or
  after bounded integer components.

- Passing bounded proof and callback-row subsets do not close the general
  proof-obligation or effect-row TODOs.

- Caller-side relational proofs now cover bounded numeric disjunctions in both
  `while` and C-style `for` guards when every arm implies the called
  function's `requires` condition. Unsupported disjuncts and facts invalidated
  by mutation remain fail-closed.

- Caller-side contract substitution now maps mixed positional/named
  arguments—including positional arguments after named ones—for direct calls
  and attached methods, matching call normalization. Cold four-route
  regressions prove both forms under symbolic caller preconditions.

- Pure-predicate expansion uses the same next-unassigned-formal mapping for
  mixed positional/named arguments in both orders.

- Caller-side predicate expansion now resolves direct qualified module
  functions as well as local identifiers.

- Symbolic `requires` expansion now handles a uniquely identified attached
  method when its signature is pure and effect-free. Receiver actuals map to
  the implicit first parameter; ambiguous and unresolved methods remain
  fail-closed.

- Recursive `@total` proofs now accept `Fin<N>` measures using the resolved
  type-level `[0, N)` bound and still require strict descent at every
  recursive edge, including lexicographic tuples that mix `Fin<N>` and signed
  integers.

- Caller-side `requires` proofs now consume a resolved `Fin<N>` argument's
  `[0, N)` range.

- Added `core.parser`, a versioned Ny parser-combinator toolkit with
  sequence/choice, mapping, literals, byte predicates, integers,
  optional/repeated parsers, full-input and prefix runners, bounded parser
  steps/depth/input, recursive forward references, and structured byte-span
  diagnostics. Repetition rejects zero-width success. Its callback-bearing
  execution APIs conservatively declare `all` effects. The typed configuration
  API and recursive arithmetic parser/emitter complete the acceptance for
  roadmap item 10; these conservative callback effects keep parser execution
  sound until the type system can express stored callback rows.

- Added `core.math.parse.arithmetic`, a recursive integer-expression parser
  and canonical fully-parenthesized emitter built from `core.parser`.

- Added `core.config` for versioned TOML schemas, typed validation,
  required/optional/defaulted fields, unknown-key rejection, structured parse
  errors, and canonical TOML output. Scalar schema fields retain schema order;
  nested table keys sort deterministically, with scalar keys before child
  tables. Fixed nested TOML section updates to rebuild each dictionary path
  with `dict_set`, and added a nested encode/parse regression. Together with
  the recursive arithmetic parser/emitter matrix, this completes the
  parser-kit, configuration, validation, and emitter acceptance in roadmap
  item 10.

- Template expansion now has a cold positive proving an attached `@inline`
  attribute survives generated declaration expansion.

- Template name interpolation now requires an identifier-valued argument. Bare
  symbols remain identifiers; strings and scalars need explicit
  `ident(value)`, whose fragment alphabet is checked and whose completed name
  must be a valid identifier. Non-identifier values in type and symbol slots
  fail closed.

- `${value}` inside template string literals now interpolates string,
  identifier, integer, and boolean arguments. This fixes layout-derived helper
  names and layout-name strings when the layout itself is emitted by a
  template. The standard-library bundle passes after the fix.

- Quote syntax now has one braced opener, `quote {... }`. The former `quote $
  {... }` and `quote ${... }` openers fail with a targeted syntax diagnostic,
  leaving `${...}` available for splicing inside the quoted body. `quote` is
  now recognized as syntax only before a braced body, so `quote(...)` remains
  an ordinary identifier call.

- `NYTRIX_BENCH_JOBS` controls fixture workers (routes remain sequential
  inside each fixture); set it to `1` for isolated performance samples. It no
  longer inherits `NYTRIX_TEST_JOBS`.

### 2026-10-07

- Audited user-facing limitation statements in the REPL, status, errors,
  cookbook, networking, and project-fit pages against the open TODO list. Cold
  REPL stdlib imports now have one explicit acceptance item linked from
  affected feature pages. A bare `find("a", "a")` in `--repl` reports an
  undefined symbol; `use std` followed by the same call segfaults, while
  `ny-full -c` returns `0`. Removed the stale `TODO.md#current-verification`
  links and suite-count snapshot from the status and project-fit pages. Other
  documented feature gaps without an existing backlog item are recorded in the
  P2 section; none is marked complete.

- The runtime `parse_ast` inspection bridge now parses quietly and returns nil
  for malformed source instead of serializing a recovered partial AST. Its
  diagnostic limit cannot terminate the host program, and diagnostic
  de-duplication no longer hides parser failure state.

- `core.ast.parse_code` now returns a complete list of parsed declaration
  records, or nil for empty input, executable top-level statements, or any
  parser error. It builds on `parse_ast`, which discards recovered trees when
  parsing fails, so callers never receive partial declarations.

- `core.ast.parse_ast_snapshot` adds a versioned syntax view with the
  `nytrix.ast` schema identifier, version `1`, and a per-node `source_origin`
  filename/line/column. The legacy `parse_ast` list shape is unchanged. The
  snapshot is still JSON data and does not yet contain compiler type, effect,
  ownership, layout, or proof records.

- `core.ast.code_origin` returns source and expansion locations in a versioned
  `nytrix.origin` record, keeping consumers independent of raw AST field
  names.

- Versioned AST snapshots now include complete `use` import descriptors
  (locality, wildcard state, and ordered imported names and aliases).

- Versioned layout and struct field records now retain the field-name source
  token and comptime emission origin.

- Generated scalar expressions inserted into statement sequences now mark the
  synthesized statement wrapper with its definition and emission locations.

- Versioned enum variant and payload-field records now expose definition and
  template-emission origins.

- Versioned enum snapshots now include generic-parameter records with source
  and template-emission origins while retaining the existing `type_params`
  strings for compatibility.

- Template interpolation and quote splicing now share `${...}`. A code list
  expands in expression lists, positional call arguments, and statement
  sequences; scalar quoted code inserts one expression. Quote-body splices
  remain AST nodes inside template parsing, category errors retain the splice
  source location, and identifier suffix interpolation is unchanged.

- Quote syntax now has one braced opener, `quote {... }`. The former `quote $
  {... }` and `quote ${... }` openers fail with a targeted syntax diagnostic,
  leaving `${...}` consistently available for splicing inside the quoted body.

- Quote parsing now reports an error when nesting exceeds its 64-entry scope
  mark stack, and restores the exact saved caller mark during unwinding.

- LLVM NYIR lowering now uses `<4 x double>` for every `NYIR_VEC4_*_F64`
  operation, including memory, shuffle, and reduction. Vector FMA calls LLVM's
  fused `llvm.fma` intrinsic. The x86-64 NIR assembly emitter stores four
  scalar lanes, implements four-lane arithmetic/shuffle/reduction, and calls
  `fma` per lane for fused results. Its machine-scalar shortcut is bypassed
  for these functions because that representation still has two f64 lanes.

- Caller-side `requires` transport now accepts typed boolean parameters and
  boolean formulas in addition to signed integer formulas. Fixed-size
  lexicographic `decreases` tuples retain signed-integer components (with the
  `INT64_MIN` lower bound), and a standalone boolean measure uses the finite
  order `false < true`.

- Caller-side `requires` transport now inlines a direct call to a bounded,
  pure/effect-free predicate before checking the symbolic implication. A
  predicate body may be one value expression/return, sequential immutable
  single-name locals followed by a value, or nested `if` statements whose arms
  provide values and may introduce immutable single-name locals. A top-level
  boolean early-return guard with a fallthrough value is also supported.
  Recursive, indirect, mixed-argument-style, and other multi-statement
  predicate bodies remain unsupported and fail closed. Positional and complete
  named predicate calls are supported. Parameter substitution uses
  collision-free placeholders, so swapped actuals cannot capture one another.
  Multi-local, nested-branch, and early-return predicates preserve positive
  proofs and weaker-premise rejection on all four routes. The independent
  certificate checker now validates nonlinear integer comparisons when bounded
  interval evaluation decides them without overflow. It handles addition,
  subtraction, multiplication, and negation over explicit obligation ranges or
  bounds derived from overflow-safe linear premises; missing ranges,
  unsupported operators, and overlapping intervals remain undecided. The
  policy path can use this checker as a bounded range fallback. Signed
  division reasoning now accepts strictly negative as well as positive divisor
  intervals under truncation toward zero.

- The compile-time fallback can interpret direct identifier calls to pure,
  effect-free functions when argument substitution makes a caller-side
  `requires` predicate exact. The bounded interpreter's fuel limit applies;
  qualified, indirect, and impure calls are not interpreted, and false/unknown
  results remain unproved. Positive, false-predicate, and impure-predicate
  controls pass through NYIR, LLVM MCJIT, LLVM ORC, and native-only.

- `@effects(read|read_memory)` and `@effects(write|write_memory)` now map to
  their canonical memory-effect rows instead of being treated as capability
  names. Core and runtime `memcmp`, `memcpy`, and `memset` aliases have
  matching summaries, so an underdeclared callback that carries a memory row
  is rejected.

- HM effect types no longer use `NY_TYPE_EFFECT`: built-in effect masks
  translate to canonical `std.effects.*` row labels, while capabilities have
  their own type kind. Effect-row serialization is versioned,
  order-independent, duplicate-normalizing, and alpha-stable for open tails.

- Multiple names inside `@effects(cap(...))` now become distinct
  `ny.capability.<name>` row labels instead of duplicate `cap` labels.

- Polymorphic `@effects(e)` contracts now check known concrete body effects
  against the annotation's fixed builtin portion instead of skipping the
  contract check. Direct IO outside `e` is rejected on all four backends, both
  with and without a callback invocation; `@effects(io, e)` remains valid for
  a wrapper that performs IO and invokes an effect-polymorphic callback.

- Effect variables allocated for function signatures now receive unique IDs
  outside each HM substitution table's local index range. Previously, two
  independent signature variables both used ID zero, allowing `ny_subst_apply`
  to copy one callback's row into another signature.

- Closed the comptime mutable-capture check across module globals and locals
  from an enclosing function, with the same stable diagnostic. A positive
  control confirms mutation of a binding created inside comptime is allowed.

- The comptime IO call-graph scan now rejects incomplete analyses instead of
  treating an unvisited helper tail or unknown callback as IO-free. Its depth
  and step bounds produce the same dedicated diagnostic as unresolved callback
  effects. A 130-helper chain ending in IO and an isolated unknown callback
  both fail on NYIR, LLVM MCJIT, LLVM ORC, and native-only when unknown
  summaries are allowed.

- AST reflection serializes the origin, and template cloning preserves it for
  nested compile-time expressions.

- AST reflection now recursively serializes quoted bodies and expressions, and
  identifier records include their syntax context.

- Recursive `@total` edges now need both a strict measure decrease and proof
  that the caller's measure is greater than `INT64_MIN`. This rules out an
  indefinitely decreasing signed measure while allowing bounded negative
  domains. This remains a restricted integer proof, not general termination
  verification.

- Fixed the recursive lower-bound builder to return its constructed predicate
  rather than a boolean null value.

- Recursive `decreases` now accepts fixed tuples of signed integer terms and
  proves lexicographic descent plus a finite lower bound on each recursive
  edge. Tuple/list expression substitution now preserves formal-to-actual
  substitution inside measures.

- Caller-side `requires` checking now walks predicates through calls and
  member expressions, substitutes named and positional actuals, and asks the
  fast evaluator to prove predicates outside the bounded integer fragment when
  they become compile-time exact.

- Effect type unification now treats `NY_FX_ALL` as the exact set of built-in
  effects, not a wildcard that can unify with `pure` or another narrower set.
  When an effect carries a capability identity, unification also checks its
  mask and exact capability name, so a named capability cannot unify with a
  builtin mask or a different named capability.

- Loop unswitching now conservatively declines loops with multiple distinct
  exit blocks until cloning remaps each exit and its PHIs independently.

- Native equality now follows the emitted producer class when source inference
  misclassifies an immediately invoked untyped lambda result. Its tagged
  return is compared through the tagged-value path rather than against a raw
  integer literal.

- Integer proof evidence now distinguishes an exact `int64` literal from a
  saturated parser payload. Out-of-range source spelling remains distinct in
  canonical propositions; contract, range, and compile-time evaluators do not
  treat the saturated value as exact evidence. A violated callee precondition
  behind an `INT64_MAX + 1` literal guard previously compiled and aborted at
  runtime. It now rejects before execution. A false constant equality between
  `INT64_MAX` and that wider literal likewise rejects on NYIR, LLVM MCJIT,
  LLVM ORC, and native-only.

- Row unification now keeps a closed tail closed, uses the same variable
  storage for ordinary and effect variables, and allocates fresh residual
  variables in the input row's arena. Distinct open tails retain their
  respective missing labels, and matching a label no longer removes it from a
  shared input row. These checks do not close the remaining P1
  language/interface work.

- Equality on a promoted `list<int>` element was reaching `rt_any_eq` after
  sequence classification, even though the loaded element and scalar peer use
  raw i64 ABI; the tagged comparator can mistake odd raw payloads for tagged
  immediates.

- Type validation rejects unsupported `comptime` bodies that have proven IO,
  preventing the LLVM snapshot or native NYIR fallback from leaving that IO
  executable at runtime. A four case negative matrix passes NYIR, LLVM MCJIT,
  LLVM ORC, and native-only.

- Loop interchange now accepts invariant stack reads when several direct
  pre-loop stores target the same nonescaped allocation and the final store
  dominates every reordered read, including when an earlier store is
  conditional and the dominating overwrite is unconditional.

- Loop interchange now admits multiple distinct, private, in-bounds, injective
  affine stack stores in one reordered nest. This expands legality only; no
  performance gain was measured.

- Affine-nest optimization now splits a scaled inner-loop address into an
  invariant row base and a scaled induction value before LICM.

- Effect-row inference now follows straight-line local alias chains captured
  by a factory's returned lambda. Open callback rows inside an `@effects(e)`
  helper also survive a two-link local alias chain and forwarding the alias to
  another row-polymorphic helper. Returned aliased wrappers are checked for
  their effect rows but not invoked at runtime.

- The x86-64 NYIR emitter now uses memory-source operands for dynamic i64
  add/subtract/multiply/and/or/xor and signed comparisons when the right-hand
  value has a stack home. Dynamic shifts load the count byte directly into
  `%cl` from its home instead of staging it through `%r10`. These remove
  temporary RHS register moves while preserving immediate, fused-constant, and
  fallback paths. Packed f64 add/subtract/multiply/divide likewise use a
  memory-source operand instead of loading the right vector into `%xmm1`. A
  neighboring sample had nearly identical counters but different wall time, so
  - The x86-64 NYIR emitter now stores packed f64 values in a contiguous
  two-word home and uses one unaligned 128-bit move to reload/store both
  lanes. Previously those vector operations stored only the low lane, then
  reloaded 128 bits for packed arithmetic. Assembly shows packed stack
  `movupd` transfers with `addpd` and `mulpd`.

- The proof-parser bridge now includes `base/util.h` directly for its
  `ny_strdup` dependency; this restored release builds after the parser
  amalgamation began compiling that source.

- NYIR now classifies `rt_any_to_i64` as a read-only runtime call. Captured
  AOT assembly no longer calls either helper.

- Its observable global-writing loop callback also weights each input by its
  call sequence, pinning both call count and order.

- LLVM constant-folded the fixed ARX kernel to its pinned return value while
  NYIR retained the loop; both kept the guarded signed divide, list mutation
  and alias runtime operations, and the effectful loop's state updates (LLVM
  inlined its callback).

- Expanded `effects-row-polymorphism.nshape` from native NYIR to all four
  effect-policy routes (NYIR, LLVM MCJIT, LLVM ORC, native-only), using
  `--effect-require-known` on each.

- A matching `||`-guard negative ensures the proof does not extract one side
  of a disjunction.

- User-defined `@effects(e)` helpers now substitute effects from directly
  invoked `fnptr` parameters at positional and named call sites, inline
  lambdas, local aliases of direct function values, and zero-argument returned
  callbacks. The callback row contributes to the caller's inferred effects,
  and an `@effects(none)` caller is rejected when the callback performs IO.
  Straight-line reassignments update the current callback row; direct local
  callback reassignment in `if`, loop, and `match` control flow also joins
  rows. Four-route underdeclared-caller cases for each flow shape diagnose the
  inferred IO row.

- Caller-side `requires` transport now drops its premise when the function
  body reassigns a premise parameter.

- Caller-side `requires` transport now substitutes omitted integer defaults
  when the default is a plain signed integer literal, an immutable integer
  global resolved in the callee's module, or a supported arithmetic expression
  whose terms resolve to signed integers. The evaluator reads resolved global
  bindings by canonical name, including module-qualified bindings. Resolution
  declines if the caller declares a same-name local; unknown, mutable, or
  noninteger defaults remain fail-closed. HM now resolves bare global names
  against an enclosing module owner when that module exists, so the compound
  module default passes strict type checking. Chained omitted defaults now
  substitute each earlier bound parameter during proof transport and during
  LLVM and native NYIR call lowering, so a later default such as `limit =
  upper + 1` uses the actual `upper` value instead of resolving it as a module
  global. Negatives reject forwarding without a caller-side range proof and
  ensure a caller local with the same name cannot be mistaken for the callee
  global; another negative confirms a mutable module global is never treated
  as a proof constant; another negative confirms the `value + 1` default is
  not proved from a positive-only premise that allows signed overflow.

- Shared `requires` transport now retains supported parameter-only facts from
  `if` and `while` guards when other conjunction terms refer to local values.
  It still rejects an unsupported disjunction branch, since that branch may
  satisfy the guard without establishing the retained fact.

- Extended shared contract transport to guarded symbolic integer arithmetic.
  Each intermediate needs a checker-validated i64 overflow proof before its
  mathematical value can discharge a precondition; short-circuit premises are
  validated before reuse. Both independent linear kernels retain checked
  128-bit constants for endpoint differences, while coefficients stay bounded
  i64. The certificate parser now accepts `INT64_MIN` without accepting an
  out-of-range positive magnitude. Existing overflow, division-by-zero,
  custom-operator, rebinding, shadowing, and argument-substitution negatives
  remain rejected.

- Inlining now uses canonical function identity rather than ambiguous leaf
  names, and persisted call targets retain interned names after typing arenas
  are freed. Dynamic call results and container loads preserve their actual
  scalar carrier instead of decoding or boxing twice. List setters detach
  shared receivers before crossing the dynamic runtime bridge; a mutable
  binding's initial list literal no longer proves its current length. Comptime
  functions retain an implicit final expression, and mixed-list formatting
  quotes string elements.

- Added a symbolic XOR translation-validation case: `NYIR_XOR_I64` is proved
  equivalent to `(a | b) - (a & b)`, and an OR-only result is rejected.

- Restored strict symbol ordering in both runtime ABI declaration tables by
  placing `rt_proof_policy_digest_hex` before `rt_proof_policy_solve`. The
  full optimizer suite remains unrun.

- Closed the interprocedural optimization TODO at its tested bounded scope.
  Unsupported memory/effect shapes retain conservative call barriers.

- Moved resolved binary representation and custom-operator return facts ahead
  of recursive native string classification. The `is_cstr` classifier now
  relies on those frontend facts and the structural string-`+` rule instead of
  starting overload lookup itself. Release rebuild passes; a fresh full
  `std.bc` profile no longer shows operator/type recursion among its leading
  costs.

- Replaced repeated module-body scans for imported functions with an ordered
  lookup through the compilation function index, filtered by module path and
  source file.

- Corrected native function lookup so namespace-only `use module as alias`
  bindings do not leak functions into bare-name resolution. This does not
  verify full-bundle completeness or reuse.

- Fixed strict `.long` lowering in two paths: raw comptime i64 constants now
  bypass a redundant BigInt conversion, and `Result<T, E>` match bindings keep
  the instantiated payload type across `rt_result_unwrap_raw`. This prevents
  aggregate payloads such as `list<int>` from being mistaken for raw integers.

- Constant global folding now evaluates a simple `comptime { return __main()
  }` initializer using the expression token's source-file identity.

- Evaluator label-PC cache signatures now include each label's instruction
  offset, so in-place layout changes cannot reuse targets computed from an
  earlier layout with the same label IDs and function size.

- Loop interchange now accepts only a narrow invariant stack-memory read: the
  load must directly use an 8-byte-or-larger `ALLOCA` address, one dominating
  `STORE_I64` must initialize it, and every address use must be that store or
  a read in the candidate loops.

- Added comptime interpreter support for list/tuple `get(key, default)` calls,
  including named arguments, negative indices, default zero, and explicit
  out-of-range defaults. Dynamic negative-index results and string-keyed
  dictionary reads now cross into the raw integer ABI through an explicit
  decode when their scalar type is proven. This fixes nested-list accumulation
  and typed dictionary arithmetic. Added primitive f-string formatting to the
  AST interpreter, with owned string results so evaluation does not depend on
  an LLVM arena. Nil formatting follows runtime stringification. The comptime
  list-rendering assertion now matches the runtime's quoted string
  representation. Fixed NYIR result classification so comptime static-list
  `get` values remain boxed at dynamic consumers.

- Extended comptime interpretation to resolve user functions without an LLVM
  codegen context and to evaluate dictionary literals, nested lookup,
  defaults, and duplicate-key last-write behavior. NYIR now honors
  `NYTRIX_COMPTIME_INTERP_ONLY=1` and reports unsupported forms at their
  source location instead of lowering them as runtime expressions.

- Expanded `tv_smt_semantics --scalar-opcodes` with independent constant
  references for wrapping i64 addition, subtraction, and multiplication, plus
  defined signed division/remainder boundaries. The long full TV/optimizer
  gates remain unrun as requested.

- Added a process-start measurement recipe that separates compilation from
  direct executable launch and distinguishes launch-to-exit wall time from an
  internal steady-state timer. `file` and `readelf` confirm ELF64 x86-64
  executable output plus native and LLVM relocatable objects. These are
  launch-to-exit samples, not pure startup or speedup claims. This is a
  separation sample, not a throughput claim.

- Kept the growing matrix and insertion-sort buffers local to their benchmark
  functions so BCE can prove the allocation lineage instead of treating a
  module-global symbol as an unknown alias. `cmov-sort` also uses a typed
  `list<int>` local to preserve element representation. Focused O3 traces
  prove all four sort indices and the flattened matrix index range `[0,9999]`
  safe under the successful length assertion.

- The pass keeps the write-capable call and pre-call load, forwards the
  immediately following load only for a matching actual address, and leaves an
  unrelated-address load and a load after an unknown mutator in place.
  Distinct SSA arguments with equal pointer values alias in the bounded store
  map. A read-only callee and a mismatched effect summary remain conservative.

- `nyir_ipa_constprop` now refuses to erase a call unless its verified
  call-site effect summary is exactly `NYIR_EFFECT_CALL`. Other memory-effect
  shapes remain conservative.

- IPA now specializes the scalar return of a bounded callee that stores
  directly through an unchanged formal pointer. It requires an exact verified
  observable-effect summary and retains the original write/trap call; DCE also
  preserves the call. The bounded exact-address load forwarding is described
  above and does not remove or relax that call barrier.

- `inline_general` now covers a known write-through-pointer callee. This
  verifies effectful clone preservation, not memory-model-based call
  elimination.

- Both NYIR inliners now retain a call when a verified call-site summary has
  effects not represented by the cloned body. A focused case previously lost
  `WRITE_MEMORY` while inlining an apparently pure scalar callee; it now keeps
  the call through both `nyir_inline_small` and `nyir_inline_general`.

- A successful `rt_assert_cstr` is now treated as a true-edge lower-bound
  guard for a typed-buffer length only when the checked value traces to that
  buffer and the length remains stable through the bounds check.

- Retried `std_bc` with a process memory limit. This now gets past the earlier
  OOM point and exposed additional GF(2) NYIR ABI mismatches. The full stdlib
  compile currently fails optimized verification because a tagged `vectors`
  local is passed to a typed-list formal.

- Expanded the comptime AST interpreter to evaluate ordinary non-variadic
  function calls with positional/named arguments and defaults, preserve
  lexical block scopes, and execute bounded iterator-style and C-style `for`
  loops under the shared fuel budget. Hardened comptime integer evaluation
  against signed division/modulo overflow and C signed-shift undefined
  behavior.

- Fixed the no-LLVM `ny-dap` link by adding the shared debug logger and
  tool-only NYIR stubs used by the other small tools.

- Expanded the proposed effect-row contract with canonical library-owned
  identities, callback-row substitution, versioned serialization, provenance
  diagnostics, and backend acceptance requirements. This is a design target;
  implementation still uses built-in masks and incomplete callback summaries.

- Quote splices now retain the caller syntax context; native lowering and HM
  binding, identifier, call, and member lookups use exact-context identity,
  preventing an introduced local from capturing a spliced caller identifier or
  its type. The comptime specification now distinguishes quote-based
  generation from the missing first-class runtime code value; `type(quote { 1
  })` currently reports `int`.

- `value.(field)` now resolves under a compile-time `#fields(Layout)`
  descriptor and expands to `load_layout(value, "Layout", "field-name")`;
  unscoped dynamic member access emits a source-located error instead of
  reaching codegen.

- `core.proof` documents the v1 obligation/certificate API and its bounded
  checker. The docs landing page links to the proof guide.

- Integrated `nyir_devirtualize` at the start of the O1–O3 pipelines, before
  verifier checkpoints and inlining. The pass now reads the canonical call
  argument vector and preserves per-argument scalar, ownership, contract, and
  aggregate-size metadata when moving a resolved function target into the
  closure ABI's first argument slot.

- Entry-global localization now accepts pointer-class loads only when the
  global is independently proven to contain an initialized typed-buffer
  handle. This lets the native O3 SOR loop reach the vectorizer and emit
  packed `movupd`/`addpd`/`mulpd` instructions.

- BCE tbuf-root provenance is memoized per function, with scratch storage
  allocated only when a dynamic-length check reaches the guarded-proof path.
  Length forwarding scans for mutations only up to the header load; later
  mutations cannot alter the already-loaded SSA value used by the guard. No
  indexed-read proof or runtime speedup is claimed for `bce-matrix` or
  `cmov-sort`.

- BCE local-root resolution now ignores stores in CFG blocks that cannot reach
  the queried load, while retaining an over-approximation for reachable
  blocks. Append and returned `rt_tbuf_set_i64_raw` values preserve only
  logical may-alias lineage; their mutation still blocks length forwarding.
  Trusted calls whose effects exclude writes, unknown effects, FFI, threads,
  and volatility are treated as read-only observers even when they may trap.
  The indexed-read checks remain, and no runtime speedup is claimed.

- Runtime tbuf bounds checks can also use a path-isolated comparison against
  `rt_tbuf_len_raw` when the queried index range fits the resulting minimum
  length and intervening mutations cannot shorten the buffer. A false `len ==
  minimum` edge is explicitly rejected: inequality alone does not establish a
  lower bound.

- Emitted production assembly uses sign-bias plus arithmetic-shift lowering
  for signed power-of-two division and remainder reconstruction; LLVM uses its
  reciprocal-multiply sequence, while dynamic division retains `idiv` on both
  paths (LLVM also emits a narrow-divisor fast path).

- Moved incremental compilation sources under
  `src/code/backend/wire/incremental/` and updated the CMake source glob and
  both callers. The relocated wire amalgamation now includes `<elf.h>` for its
  Linux x86-64 tiny-ELF emitter.

#### Affine loop interchange and multi-exit unswitch

- Extended interchange to permit one store per inner-body iteration into a
  nonescaping stack allocation when its byte address is affine in both
  induction variables, its full-width access stays in bounds without signed
  intermediate overflow, and the two-dimensional address map is proven
  injective. Unsupported effects, aliases, recurrences, and unproved address
  ranges remain rejected.

- Broadened natural-loop unswitching from one shared exit to multiple resolved
  exits. Cloned exits retain their own targets, fallthrough bridges select the
  matching target, and each exit PHI receives cloned predecessor/value inputs.
  Added a two-exit loop whose selected exit PHI consumes a loop-carried IV;
  both invariant outcomes preserve their results after cloning.

- Extended unswitching to natural loops with multiple direct latches when all
  induction-PHI backedges prove the same constant step, even when each latch
  computes a distinct SSA update. The pass unions the latch-specific natural-
  loop members, clones/remaps all latch labels and PHI inputs, and rejects
  mismatched steps or noncanonical latch branches.

- The unselected call stays unexecuted; when selected, its trap and one-call
  count are preserved before and after cloning.

- Added a fallthrough exit without an explicit label; unswitching fails closed
  and preserves its evaluated result and original IR length.

- General indirect-exit cloning is still unsupported.

- Rebuilt with `./make ny`.

- Wrapped the late O3 loop-vectorize call in the default-off
  `NYIR_ENABLE_LATE_LOOP_VECTORIZE` build gate. O2 vectorization remains
  enabled.

- The post-gate native assembly uses a 608-byte frame and scalar
  `movsd`/`addsd`/`mulsd` operations with stack-home traffic; the `cc` loop is
  no longer vectorized in this O3 path. The performance loss is accepted
  containment, not a parity fix.

- The timer spread reinforces that these samples are correctness smokes only;
  neither the historical PBKDF2 regression nor the SOR native gap is fixed.

- Restored and rebuilt the default-off gate.

- Closed-form IV elimination now fails closed when a counted loop has an
  additional exit besides its induction guard.

- The direct ELF writer now keeps functions containing any backedge in their
  canonical NYIR value homes instead of assigning registers from textual
  intervals. Loop bounds and values live across one iteration could otherwise
  be reused for comparison results; this also broke `continue` in a `for`
  loop. Reference-checked nested-loop sums and `for`/`while` loop-control
  cases pass with the conservative fallback.

- The self-contained C frontend lowers `continue` to the `while` header or
  `for` step block and `break` to the current loop exit. Both forms are
  reference-tested across empty, short, and larger bounds. A nested-loop
  reference fixture for each loop kind proves `break` and `continue` affect
  only the inner loop. Using either statement outside a loop remains a located
  diagnostic.

- Linux automatic window backend selection now reports unavailable when both
  `DISPLAY` and `WAYLAND_DISPLAY` are absent, instead of claiming successful
  availability or initialization for the `none` backend. Window creation
  returns false promptly; gamepad enumeration returns an empty list. An
  explicit `PLATFORM_NULL` hint or `NY_UI_BACKEND=none` remains an intentional
  separate backend selection.

- The Linux joystick module now uses `core.os.fs.watch_init`, `watch_add`, and
  `watch_rm` for inotify.

- Effect-row composition now structurally unifies payload types when the same
  label appears in both rows.

- Mutable list refinement transport no longer treats every function spelled
  `append` as the standard list operation. The generated mutable-flow
  differential now includes the shadowed-call boundary.

- Structural `decreases` proofs now follow immutable single-name local aliases
  of a same-enum constructor payload, including alias chains.

- Structural `decreases` now tracks every same-enum payload binder in a
  constructor arm, not just the first one.

- Structural descent can lead a tuple `decreases` measure.

- Pure contract expansion now handles finite literal statement-form matches
  with a default arm and branch returns.

### 2026-10-06

- Corrected pointer access to the caller-owned SMT environment in the
  Presburger and Z3 proof paths.

- Fixed pointer status reads in the independent proof-certificate checker's
  Boolean branch expansion. The C policy API still has no production
  proof-site callsites.

- Explicit polymorphic effect contracts such as `@effects(e)` now count as
  known symbolic effects under the require-known policy; truly unannotated
  unknown callback effects remain rejected. The native-nyir fixture remains
  blocked on safe callback-row substitution; see the current status in
  [effect-row reference](spec/functions.md#effect-rows).

- `ipa_constprop` now tests its aggregate interpreter work budget across two
  call sites: each callee requires 50,003 instructions, the first folds, and
  the second stays out of line after the shared 100,000-instruction budget is
  exhausted. It also folds a bounded four-trip CFG loop with two header PHIs
  to 6. The bounded scalar interpreter handles `NYIR_SELECT_I64` in a callee,
  and caller constant collection propagates a known `SELECT_I64` argument.

- Loop interchange now admits only a plain `LOAD_LOCAL` effect when the nested
  bodies contain no writes, traps, calls, or memory accesses. Another case
  gives a body call the `CALL|FFI|UNKNOWN_SIDE_EFFECT` summary and likewise
  checks that its loop schedule does not change. A third case adds an ordinary
  carried PHI to the inner loop and checks the result, bounds, and instruction
  count stay unchanged. A nonvolatile, potentially trapping load from a local
  allocation also stays ordered. Interchange also requires both SCEV trip
  counts to be known finite and non-overflowing.

- Each of 256 cases uses a reproducible seed and 24 generated steps; original
  and rewritten expressions are compared at eight inputs, including signed
  boundaries.

- Scalar calls with exactly known `NYIR_EFFECT_CALL`, `CALL | READ_MEMORY`,
  `CALL | WRITE_MEMORY`, or `CALL | READ_MEMORY | WRITE_MEMORY` effects use
  shared uninterpreted results. Read results key on the input byte array;
  writing calls also produce a shared next-memory array from arguments and
  prior bytes. Captured secondary returns remain congruent for supported
  non-writing calls. Exact `CALL | MAY_TRAP` calls now carry shared
  definedness keyed by callee and scalar arguments; repeat calls are congruent
  and erasing a potentially trapping call is rejected on straightline and CFG
  paths. Unknown, volatile, conflicting, and other unsupported summaries
  remain inapplicable rather than being assumed pure. CFG allocation sites are
  admitted only when SCEV proves every enclosing counted loop executes at most
  twice, keeping path exploration bounded. Unknown-trip and larger cycles
  remain inapplicable. Fixed-size `NYIR_COPY_STRUCT` is checked against scalar
  loads/stores in straightline and CFG paths, including changed-word
  rejection. All eight exact `CALL` summaries formed by `READ_MEMORY`,
  `WRITE_MEMORY`, and `MAY_TRAP` are now accepted. Write-capable trapping
  calls share deterministic results and definedness and produce memory
  transitions keyed by arguments and incoming bytes; targeted read/write
  memory proofs pass. Unknown, volatile, conflicting, and other unsupported
  effects remain inapplicable. The interpreter and SMT model now process
  four-lane f64 loads, stores, arithmetic, FMA, shuffle, and reduction in
  straightline and CFG functions. The `--scalar-opcodes` group proves
  `SELECT_I64` equivalent to conditional control flow and both byte-swap
  opcodes against independent bytewise bit-vector constructions in
  straightline and CFG forms. Allocation shape matching permits differing
  function lengths when each positive-size allocation remains at the same
  instruction index; the bounded write-capable trap selector covers this case.

- Native C imports now box signed 64-bit scalar returns at dynamic boundaries,
  and `store64_i` avoids unboxing an out-of-range integer literal twice when
  `rt_any_to_i64` already produced raw i64 bits.

- The native runtime ABI table now classifies `rt_flt_box_val` results as
  tagged values. An inferred f64 local initialized through the boxed
  `float(any)` wrapper therefore unboxes at the local's f64 boundary instead
  of bitcasting a boxed pointer to `double`.

- HM call-target persistence now prefers a non-stdlib declaration owned by the
  current module for an unqualified same-module call.

- Declared user-defined value-indexed types now erase through their declared
  base ABI in type and native ABI resolution. HM call checking preserves index
  identity while accepting the erased base value, and compile-time integer
  arguments are checked against literal or immutable symbolic bounds.

- Explicit `i64` variable declarations now accept constant integer
  initializers throughout the signed 64-bit range, even when unhinted literal
  inference classifies their magnitude as BigInt. The change is limited to
  type unification for checked constant initializers; BigInt materialization
  and arithmetic overflow promotion remain unchanged.

- An inclusive `row <= 6` guard proves the same final strided row and
  preserves result 232; its wrapping counterpart remains scalar and preserves
  result 52. An unknown-effect call in the inner loop remains scalar and
  executes 16 times before and after the pass. A two-latch outer loop now
  vectorizes when both edges carry the same `+1` update, preserving result
  136; it also vectors under an inclusive `row <= 3` guard, and uniform `+2`
  latch edges vectorize under `row <= 6`. Differing `+1`/`+2` edge updates
  remain scalar and preserve result 94. A shifted same-buffer store
  overlapping later loads also remains scalar and preserves result 84.

- Loop interchange now rejects loop bodies with trap, volatile, I/O, thread,
  FFI, local-state, allocation, or unknown-side-effect effects. Its optimizer
  regression keeps a volatile/trapping read loop's iteration order unchanged.

- `nyir_loop_unswitch` now clones a restricted single-latch natural loop with
  an identified induction PHI, additional header recurrences, an invariant
  in-loop branch, and one shared exit. Cloned value IDs, labels, and internal
  and exit-PHI predecessor labels are remapped; external fallthrough is
  redirected to the shared exit. A false-arm divide-by-zero stays unexecuted
  when the invariant condition selects the true arm, and returning an
  induction value after the loop keeps the transform disabled.

- Dynamic append-length propagation remains unresolved.

- SCEV now derives in-body bounds for a unit-step nested induction variable
  whose initializer is affine in a dominating outer induction variable. A
  check on the guard's false edge also remains. Stack-backed accesses preserve
  results 10 and 1 before and after elimination for the ordinary and
  negative-initializer cases. No speedup or production bounds-check removal is
  claimed.

- Translation validation now models acyclic CFG allocation sites and rejects
  allocation sites inside backward-edge cycles.

- The SMT TV model now evaluates positive-size `NYIR_COPY_STRUCT` as a byte
  copy in straightline and path-sensitive CFG functions. It requires valid
  source/destination ranges that do not overlap, matching the copy's `memcpy`
  contract. `tv_smt_semantics` proves a 16-byte copy equivalent to scalar
  loads/stores over disjoint stack allocations and rejects a changed copied
  word. The same SMT paths now map `NYIR_ADDR_SYMBOL` to a consistent symbolic
  address; same-symbol loads prove equivalent, while changing the symbol is
  rejected. Scalar calls with exact known-pure or read-only effects use shared
  uninterpreted results; read-only calls are keyed by arguments and the
  current memory array. `tv_smt_semantics` proves duplicate-call congruence
  and detects different results when writes move across a read-only call.
  `NYIR_CAPTURE_RET` uses a shared selector-indexed result in both evaluators
  for supported calls; repeat captures prove equivalent, while changed
  selectors or arguments produce counterexamples. Calls with the four exact
  pure/read/write effect summaries carry their modeled memory transition
  through straightline and CFG evaluation; writing calls cannot be captured as
  secondary returns yet. Exact `CALL | MAY_TRAP` calls share argument-keyed
  definedness in both evaluators. Additional memory effects, unknown,
  volatile, conflicting, aggregate, and sret calls remain inapplicable.

- `NYIR_ADDR_LOCAL` now maps address-taken local slots into the shared
  symbolic byte memory in straightline and CFG evaluation. Direct local loads
  and stores agree with indirect memory operations; an eight-byte out-of-range
  load is rejected.

- The restricted `@total` countdown proof recognizes equivalent spellings for
  the `n <= 0` base (`0 >= n`, `n < 1`, `1 > n`) and the additional valid `n <
  0`/`0 > n` base guards; recursive calls may use positive integer-literal
  decrement steps. Overloaded comparisons/decrements, zero-step recursion,
  increasing recursion, and unmeasured recursion remain rejected.

- Failures included the `compiler/optimizer/vector-f64.nshape` result-oracle
  mismatch. The native list-shape proof did not recognize method `.append` and
  treated `.pop()` as an unsupported mutation, leaving a raw slot and tagged
  pop result mixed at `rt_any_add`. The standalone benchmark phase was
  skipped.

- Its direct native smoke prints `20 22` and `42`, confirming the indexed raw
  integer is correctly combined with the value returned by `pop()`.

- The native float-cast lowering now checks the raw-int ABI proof first and
  emits numeric i64-to-f64 conversion; dynamic tagged values still use
  `rt_any_to_f64`.

- Fixed the native-only qualified `core.len([1, 2, 3])` path: semantic
  completion had canonicalized it to `core.any.len` and included the `core`
  module expression as an implicit receiver. Native lowering now removes only
  a verified stdlib module-prefix receiver before checking the method's source
  arity. The build and clangd diagnostics for `lower.c` pass.

### 2026-10-05

- All execute to the known result 136 before and after the pass. A step-two
  induction wraps from `INT64_MAX - 1` to `INT64_MIN`, safely processes the
  next row, returns 52, and remains scalar. No `sor` performance or broader
  loop-transformation completion is claimed.

- Audited NYIR mutations that change alias/range dependencies. ISLE strength
  rewrites, memory-SSA forwarding, mem2reg, PHI pruning, double-negation, and
  private-entry localization now use shared metadata-preserving constructors
  or clear derived facts.

- Defined NYIR call memory effects as a conservative top set: `READ_MEMORY`
  and `WRITE_MEMORY` include all existing memory possibly reachable through
  arguments or globals, and carry no per-object alias set or bounded extent.
  Audited GVN/PRE, CSE, redundant-load elimination, LICM, and DSE against that
  contract; calls with unknown provenance or extent remain barriers. No memory
  barrier was relaxed by that audit; the later bounded GVN exception is
  recorded below.

- Straightline SMT now gives fixed-size `ALLOCA` instructions
  pairwise-disjoint symbolic regions; constant derived-pointer offsets are
  checked against each accessed width, while unsupported allocation
  shapes/provenance remain inapplicable. External pointers may still alias
  those regions and retain unconstrained validity.

- Configured OMP to use the project-built `ny-lsp` for `.ny` files. Fixed
  positional JSON integer parsing and NY function-signature hover formatting;
  rename now emits valid workspace edits directly instead of reconstructing
  nested ranges.

- Corrected the existing HM row unifier so an empty closed row no longer
  unifies with a non-empty row, and repeated labels normalize only when their
  field types unify.

- Removed the unused NYIR raw-call write-argument mask: at that stage no
  optimization consumer had a bounded write-extent proof, so the mask could
  not justify weakening memory barriers. The bounded GVN exception is recorded
  below.

- GVN now uses constant `rt_memcpy_raw`/`rt_memset_raw` destination ranges
  against provenance-rooted eight-byte loads. It forwards reads across
  disjoint ranges and zero-length calls, while overlapping, dynamic-length,
  and unknown-provenance calls remain barriers. Added negative-length (`-1`,
  `INT64_MIN`) fill/copy cases to pin the runtime no-op boundary.

- Fixed BigInt-to-`int` conversion at the dynamic ABI boundary, where
  heap-backed BigInts had been mistaken for pointer values. Thread callback
  helpers now match their LLVM/native call signatures, preserve tagged
  callback results, reconstruct tagged values from descriptor buffers, and
  carry mixed dynamic/raw parameter layouts for heterogeneous callbacks.

- The x64 object encoder now keeps values in the same physical register across
  a regalloc-proven unique CFG edge when the only external read is its edge
  copy. It omits the redundant canonical-home store but retains homes for
  returns, call arguments, and other cross-block reads; allocating safepoints
  flush resident GPR values to canonical homes. The proof is cached per live
  segment.

- `@total` proves one guarded direct integer countdown with `decreases(n)` and
  built-in subtraction, both with a following recursive expression and an
  explicit `else` return; increasing, overloaded-decrement, and unmeasured
  recursion fail closed. HM now preserves the canonical declaration for
  resolved named calls; shared typing proves that restricted implication
  before native lowering instead of relying on backend-local inference. The
  shared proof shortcut is disabled when the caller reassigns a formal or
  introduces a binding that shadows its name, including local declarations,
  iterator variables, and lambda parameters. The caller's entry precondition
  cannot justify a call using a changed or shadowed binding. The call-site
  contract solver now rejects variable-containing or overloaded integer
  arithmetic as unknown and still accepts constant arithmetic when its
  intermediate results fit `int64`. Dynamic method dispatch still fails
  closed. The proof fragment intentionally fails closed outside integer terms,
  arithmetic/logical formulas, positional or complete named calls, and
  built-in operators.

- Comptime AST interpretation now consumes the same compilation-wide fuel
  budget as the fast evaluator. Removed the independent 500,000-step limit and
  100,000-iteration while guard; fuel exhaustion now fails compilation with a
  source-located diagnostic instead of returning a runtime value.

- Continued the staged LLVM-legacy retirement without claiming completion.
  Preparation/link handling is in `prepare.c`/`prepare_links.h`; comptime-if
  dispatch, fast comptime expression evaluation, string initialization,
  layout/signature collection, attached call lookup, and ownership mutation
  analysis have been moved to `comptime_if.c`, `comptime_fast.c`,
  `string_init.c`, `layout.c`, `sigs.c`, `gencalltypes.c`, and
  `typing/ownership.c`. `comptime_fast.c` now also owns platform/main-mode
  queries, fuel accounting, unary/binary fast evaluation, binding constant
  lookup, bigint/long-property conversion, sequence/range construction,
  fast-value ownership, and tagged conversion. Expression-kind profiling
  state/reporting moved to `expr_profile.c`. The fallback comptime interpreter
  now lives in `comptime_interp.c`; LLVM expression lowering and
  `gen_comptime_eval` remain in `legacy/expr/core.c`, which is still called by
  `comptime_if.c`. The legacy expression unit is not yet deletable. The
  legacy-emission directory remains. The thread wrapper is now resolved as a
  stdlib function before the direct-call intrinsic path; this advances
  `std_bc` past its earlier `thread_spawn_call` lowering error. The latest
  automatic and serial `std_bc` precompile attempts were killed while
  compiling `std.bc`; neither produced bitcode. The last detailed compiler
  diagnostic before those attempts was NYIR verification in
  `core.math.crypto.lattice.lll._lll_rel_diff` (an f64 value stored with a
  non-float local ABI class). The no-LLVM Ninja compiler object target builds,
  but this configuration provides no full compiler executable.

- MCJIT REPL perf maps now reset when the engine is rebuilt and append symbols
  from each submitted module instead of retaining only the initial top-level
  entry. Supported object formats remain unvalidated.

- Native local stores now stamp known ownership from the stored value's
  producer. If a mutable local receives a different known ownership than its
  earlier value, the local fact becomes unknown so later loads do not claim
  one ownership across mixed paths.

- Fixed native verification and execution for unparameterized list elements
  passed to raw-int formals. An `elem_size == 8` guard proves slot width, not
  that a generic element is a raw integer; both arms now retain the tagged
  class at their temporary-slot join unless the source contract proves
  `list<int>`.

- Reviewed the related native lowering, x64 allocator/emitter, and runtime
  pointer bridge changes; removed the superseded queue probes and captured
  dumps from `tmp/`. The individual fixes remain documented with their owning
  changes below and in earlier dated entries.

- Removed `--legacy-llvm-dump` (the first step of the legacy-retirement Stage
  2): the option field, help entry, and parse branch are gone, the two
  JIT-cache conditions in `wire/pipe/init.c` no longer consult it, and the
  legacy `codegen_emit` branch in the LLVM pipe is deleted — leaving the
  unified NYIR-to-LLVM path as the normal program emitter. The comptime legacy
  LLVM expression path remains active. This orphans
  `codegen_emit`/`codegen_emit_script` (their last external caller), the
  designed precondition for deleting the legacy emission TUs.

- The removed P0 items stay removed; the repros are archived under `tmp/`
  (`sound-source-length-probe.ny`, `bigint-promoted-mul-repro.ny`).

- The COW route-divergence item now points at the archived reproducer
  (`tmp/cow-alias-append-repro.ny`), which still diverges — native 314384
  versus VM 182928 after the list rework moved both sides of the original
  split.

- Fast-math reciprocal factoring now marks its synthesized constant and
  reciprocal result as F64 and attaches the originating division location.
  Strict IEEE mode remains unchanged.

- Self-recursive tail-call conversion now carries the original call-site
  source location onto generated parameter stores and its loop-back branch.
  The pass now discards both unreachable originals, including the moved call's
  owned metadata.

- PHI elimination now attaches the PHI's source location to every generated
  predecessor-edge store, preserving attribution through the temporary-local
  lowering as well as on the replacement load.

- If-conversion now transfers the replaced PHI's scalar class, ownership,
  mutability, range, and source location to its `SELECT`; removed bridge
  branches retain their own location on the no-op. `if_convert` seeds all PHI
  facts and verifies the converted instruction preserves them with an
  unchanged result.

- CFG simplification now preserves the source location when removing a
  constant-false `BR_IF`.

- The unconditional branch retains its target, source location, `dst=-1`, and
  required control effect; the false branch retains its source location.
  `null_check_elim` verifies both rewritten shapes and results.

- Induction-variable elimination now rewrites an exit PHI to a copy through
  `nir_make_copy`, preserving its debug location, scalar class, ownership, and
  mutability while dropping facts tied to the removed incoming edges. Rebuilt
  `build/release/ny`; the `pass_order` and `loop_idiom_metadata` drivers pass.

- Redundant-load forwarding now uses the shared NYIR copy constructor,
  clearing stale load range/effect metadata while preserving value provenance.

- NYIR debug fingerprints now include source file, line, and column.

- Generic NYIR `nir_make_copy` rewrites now clear both range and alias facts;
  the helper cannot assume facts from the old producer apply to a different
  source value. GVN explicitly restores the replaced range only after its
  expression-equality and memory-stability proof. Extended `alias_contracts`
  with an incompatible seeded-range case. This closes that helper contract,
  not the audit of all direct IR mutations.

- GVN's fixed-width scalar load/store alias proof now retains checked constant
  offsets from a shared symbol, local, or alloca root. It reuses a load across
  an eight-byte-adjacent store, retains the load for overlapping four-byte
  separation, and stays conservative when the modular distance can overlap at
  address wraparound. Offset-adjusted accesses rooted at different objects
  remain conservative because no object-bounds proof exists.

- Correct rewrites prove equivalent, an omitted-lane mutation is rejected, and
  removing an otherwise-unused load is rejected when it changes whether the
  program is defined.

- GVN now keeps raw `rt_memset_raw` and `rt_memcpy_raw` as memory barriers:
  their explicit arg0 write-base summary does not bound the runtime write
  extent, so distinct symbolic bases alone cannot prove non-aliasing. The
  address-root proof also stops stripping nonzero pointer offsets.

- Its inventory covers all 229 runtime argument declarations and reports the
  sole unknown, `rt_syscall_raw`, whose raw machine-word operands
  intentionally have no ownership promise. This protects declaration
  integrity.

- Added `machine_copy_scaling`, a compile-only 36,002-instruction workload
  with 12,000 COPY pairs. The allocator keeps its conservative conflict rule;
  machine-home semantics need an explicit model before this affinity can
  safely alter physical assignment.

- Capped automatic benchmark fixture concurrency at four workers and now
  applies the same 8 GiB system reserve / 12 GiB per worker budget as the main
  fixture runner. Explicit `NYTRIX_TEST_JOBS` still overrides the automatic
  choice. This prevents large logical CPU counts from launching 24+ competing
  compiler/runtime fixtures by default.

#### Rechecked reduced havlak native parity

- Rebuilt the in-tree release compiler; `./make --color=never ny
  --no-progress` completed with Ninja reporting no work.

- The preserved `N=10` stateful traversal reproducer now returns identical
  `cycles=2`, `pass_id=6`, `next`, and `vis` values on native O0, native O3,
  LLVM O3, and optimized NYIR VM routes. The old native code-generation
  mismatch no longer reproduces on this checkout.

#### Restore proof-pipeline build after context rename

- Synchronized the requires-proof visitor's save/restore state with the
  existing `current_fn_params_rebound` fields and `tp_function_rebinds_param`
  helper in `typing/pipeline/emit.c`; this resolves the compile errors from
  stale `_mutated` names without changing the proof invalidation condition.

#### Follow returned-borrow contracts in BCE provenance

- BCE root tracing now follows a unique formal `returns-borrow` argument.

- Length forwarding accepts returned-borrow calls only when their trusted
  effect summary excludes writes and unknown side effects. Mutating append
  calls are not treated as identity-preserving because they can replace the
  allocation.

- Removed the unimplemented `nyir_loop_memset` pass hook and declaration after
  the release link failed on the missing definition.

#### Add conservative NYIR zero-fill loop recognition

- `nyir_loop_memset` replaces counted zero-store loops over sufficiently large
  fixed `NYIR_ALLOCA` objects with `rt_memset_raw` at O2/O3. It requires a
  zero-initialized unit-step induction variable, a store before the increment,
  no bounds check or escaping loop value, and an induction slot used only for
  initialization, loads, and increment.
- Unknown pointer provenance, small allocations, nonzero fills, and observable
  final induction values remain unchanged. The cross-backend fixture's
  bounds-checked loops are deliberate non-candidates. No speedup was claimed;
  broader high-level loop work remained open at this checkpoint.

#### Carry allocated values across non-adjacent CFG layout

- Same-value live segments can coalesce across a unique CFG edge despite
  non-adjacent instruction layout. Carry requires exact block-boundary
  segments, compatible value classes, and one runtime incoming edge.
- Conflicting target segments can be evicted only with a valid canonical
  home; carried values and division/remainder results stay protected. In the
  scaled `binary` kernel, v66 carries B11→B21 without a spill/reload while
  conflicting, non-carried v39 uses its home.

#### Optimizer call-contract value numbering

- GVN/PRE call keys now hash and compare the packed per-argument actual
  ownership and formal borrow/consume contracts. This prevents two pure calls
  with identical operands but different ownership behavior from being merged.

#### Exact packed integer-vector validation

- Added exact per-lane bit-vector translation for two-lane packed i64 add,
  subtract, and/or/xor, and masked left/arithmetic-right shifts. The model
  matches NYIR's wrapping arithmetic and shift-count semantics.

#### Four-lane integer memory translation validation

- Extended the Z3 model to interpret NYIR `VEC8_I64` symbolic loads as four
  consecutive 64-bit reads, including per-lane address validity, then reduce
  all four lanes with wrapping i64 addition.

- Added straightline and branch-CFG solver comparisons against four scalar
  loads, plus negative cases omitting the final lane.

- Added four-lane packed add, subtract, bitwise-and, bitwise-or, and
  bitwise-xor proofs against four scalar load/op pairs; these exercise
  lane-wise SMT arithmetic in addition to load/reduction semantics.

- Modeled the currently implemented two-lane `VEC4_F64` load, set, arithmetic,
  and reduction operations with IEEE-754 round-to-nearest semantics. A
  memory-backed reduction matches two scalar f64 loads and rejects a missing
  lane after conversion to a defined i64 result. Broadcast/add/reduce also
  matches scalar f64 arithmetic and rejects a missing lane.

- At this stage of the work, memory was still read-only; the following update
  records the scalar-write extension.

#### Path-local scalar memory in translation validation

- Replaced the translation validator's immutable read function with one shared
  symbolic byte array.

- Modeled little-endian 64-bit stores and byte stores, including overlapping
  loads and separate byte/full-width validity predicates.

- Vector stores and concrete allocated-object validity remain unsupported; the
  abstraction does not claim that an address is allocated or writable.

#### Integer vector-store translation validation

- Added path-local stores for the two-lane `VEC4_STORE_I64` and four-lane
  `VEC8_STORE_I64` operations. Every lane uses the shared byte-array memory
  state, and each store carries validity for its complete byte range.

- Added SMT comparisons against scalar load/store sequences for both widths;
  removing the final scalar lane store is rejected. Repeated this comparison
  across branch CFGs, exercising forked path-local memory state through the
  join; omitted-lane cases are rejected there too.

#### Floating vector-store translation validation

- Modeled the existing two-lane `VEC4_STORE_F64` operation against the shared
  symbolic byte array, including validity for both 8-byte lanes and path-local
  state across CFG branches.

- Added straightline and branch-CFG equivalence checks against scalar
  load/store sequences. Both reject a rewrite that omits the second-lane
  store, including when source and destination addresses may alias.

- `VEC4_F64` remains two lanes in the current NYIR/LLVM contract, so this does
  not close the separate true-four-lane-f64 or concrete
  allocated-object-validity work.

#### Fused floating-vector operation in translation validation

- Added exact round-to-nearest `VEC4_FMA_F64` semantics to the SMT evaluator.
  The solver proves its two-lane result against scalar multiply/add for exact
  operands, rejects an altered addend, and distinguishes fused rounding from
  separate multiply/add rounding at a halfway product.

- Added fused two-lane `VEC4_FMA_F64` evaluation to `nyir_eval`. The bounded
  F32 vector subset is documented in the following section.

#### Four-lane float32 translation validation

- Modeled the existing four-lane `VEC8_F32` load/store, splat, and packed
  add/subtract/multiply/divide operations with shared byte memory and
  round-to-nearest IEEE-754 semantics.

- `nyir_eval` now supports every existing four-lane f32 vector opcode:
  load/store, splat, add/subtract/multiply/divide, fused FMA via `fmaf`, and
  shuffle. The solver support gate explicitly admits the modeled opcodes on
  its SMT path. Arithmetic cases have straightline and CFG
  scalar-reference/rejection checks.

- A distinct-lane shuffle is checked against a scalar byte-store reference,
  and an identity shuffle is rejected. An adversarial product/addend pair
  distinguishes fused from separately rounded multiply-plus-add.

- The bounded shuffle path reverses distinct input lanes and rejects
  identity-shuffle substitution.

#### Removed unreachable constant-branch rewrites

- Deleted two constant-branch mutation blocks in `const_fold.c` that were
  guarded by `false`; active constant-edge simplification remains in the
  SCCP/CFG cleanup path.

- `alias_contracts` and `cfg_simplify` passed after the cleanup.

#### Raw memory-call effect contracts

- Classified `rt_memcmp_raw` as read-only and potentially trapping in NYIR;
  GVN reuses a stable load across it, while CSE retains identical raw
  comparison calls. The shared builtin-effect registry now reports `read` for
  raw memcmp, `read|write` for raw memcpy, and `write` for raw memset.

- Corrected the tagged `__memcmp` and `__memcpy` builtin rows: comparison
  reads argument memory, and copying reads its source as well as writing its
  destination. The public `core.memcmp` wrapper retains the inferred read-only
  effect.

- Added negative checks for omitted tagged `__memcmp` read, `__memcpy`
  read/write, and `__memset` write effects.

#### CFG path-local memory validation

- The two-arm rewrite proves equivalent to full-width stores on both arms; a
  one-byte partial write is rejected.

### 2026-10-04

- Tightened guard-derived induction ranges in the native loop vectorizer: a
  two-input PHI is now refined only when its incoming CFG edges are exactly
  one explicit backedge and one non-backedge predecessor, the header has no
  additional predecessors, and the update is defined in the latch block. This
  closes an ambiguity in dominance-based edge classification; it does not
  broaden the proof to multi-latch or irreducible loops.

- Recorded guarded-rotate output contains ten immediate `rorl` instructions.

- Shared compile-range validation now proves descending canonical `while`
  induction intervals for `i > bound` and `i >= bound` with direct constant
  decrements. The validator requires one induction-variable write, an
  effect-safe body, a known start, and a decrement that cannot overflow.

- Fixed inferred integer locals initialized by `__flt_to_int`: the intrinsic
  emits raw i64 in native code, but expression classification had inherited
  the public runtime entry's tagged return class. This mismatch boxed the
  value at the local and tagged it again after the branch join (`480` became
  `961`).

- Extended SMT translation validation to model `NYIR_BOUNDS_CHECK` as a trap
  unless its signed byte offset is nonnegative and below the static or dynamic
  byte length.

- Closed the guarded-division-after-inlining regression with a cold
  eight-route optimizer fixture. The stale P0 entry was removed from
  `docs/TODO.md`.

- Made the SSA unroller's multi-block rejection actionable at high verbosity:
  it reports the proven trip count, interior-block count, first block, and
  that CFG cloning is unavailable.

- Fixed native typed-list indexing when an element is passed to an `int`
  formal. A runtime descriptor read now keeps the proven raw `list<int>`
  element class instead of labeling an elem-8 payload as tagged and decoding
  it again at the call boundary. The corresponding P0 entry was removed from
  `docs/TODO.md`.

#### Preserve dynamic BigInt arithmetic at raw-integer boundaries

- Native mixed-add uses raw `i64` only with explicit raw-read provenance.
  Other tagged operands use `rt_any_add`, preserving overflow-promoted BigInts
  through a following multiply rather than truncating them into heap pointers.
- Native function tails reject implicit `int` returns of proven BigInts before
  conversion, matching HM's diagnostic. Local-store normalization respects a
  proven BigInt carrier even when HM names it `int`; wide decimal literals
  bypass `rt_any_to_i64`.

</details>

</details>

## [0.9] - 2026-08-21

- C header imports, subsystem reorganization, and native/runtime reliability.

<details>
<summary>Details</summary>

- Development period: 2026-07-21–2026-08-21.

### Added

- In-tree C header frontend replaces libclang for supported arrays, unions,
  packed structs, bitfields, extended numbers, function pointers, typedefs,
  macros, and variadics. Untrusted headers have parse deadlines, recursion
  limits, and bounded tables.
- Optional GMP with native BigInt fallbacks; `-Oz`, compile-time `#assert`,
  formatted `core.report` diagnostics, managed-heap `--zero-init`, and
  `core.os.rev` decompilation/symbolic-execution modules.
- Linux/x86-64 syscall restrictions on process creation and executable-memory
  remapping through `mprotect`/`clone`/`fork`/`vfork`.

### Changed

- Explicit subsystem boundaries under `src/base`, `src/cmd/ny/repl`, and
  `src/code/*`. Optional LLVM emission/JIT joins the native backend; direct
  AST-to-LLVM remains behind a semantic-parity gate.
- Native/self-build links default to the system linker; fast alternatives
  require explicit selection.
- Expanded diagnostics and measurements: register allocation, allocation
  sampling, native/C and native/LLVM budgets, emitted bytes, peak RSS,
  vectorization profitability, static-island quality, and inlining decisions
  including monomorphization rejection.
- Tagged-int bitwise operations, proven-safe shifts, and ordered comparisons
  use fast paths under `--profile=speed`.

### Fixed

- SSA: mem2reg parameter promotion, escape-SROA slots, and native allocation
  and bounds facts for `f64buf`/`i64buf`.
- Sockets: decode ephemeral ports from sockaddr bytes; return the raw
  descriptor from `socket_accept`.
- Unified loop analysis; repaired vector-tail preheaders, PHIs, and scalar
  induction remapping, syscall/`rdrand` assembly guards, AArch64 conditions,
  and BigInt heap corruption.

</details>

## [0.8] - 2026-07-13

- Proof witnesses, filesystem watching, cache integrity, and platform fixes.

<details>
<summary>Details</summary>

### Added

- Standalone `wasm-emscripten` output with `_ny_top_entry` and explicit
  `env.*` namespacing through aliases; existing declarations retain precedence.
- `prove(condition[, message]) -> proof` witnesses reject false/dynamic
  obligations and ordinary values in proof parameters. `core.math.logic`
  adds evaluation, simplification, certificates, bounded solvers, rewriting,
  and Prolog-style unification/backtracking.
- `core.os.fs.watch_*` and hot reload use inotify, kqueue, or Windows change
  notifications, with an mtime fallback.

### Changed

- NYIR coalesces copy/local chains, allocates scalar registers, selects
  immediates, indexes DCE labels once, and preserves floating types across
  collapsed equivalence classes.
- Precomputed x86-64 call boundaries and immediates reduce the measured call
  body and frame-relative accesses.
- Library sweeps stop after optimized IR rather than materializing MCJIT.
  Cache formats reject mixed library/user entries and sanitizer-tainted objects.
- `ny-fmt --cloc` reports tracked additions/deletions and per-file totals.
- Hot reload blocks on native events rather than busy polling.
- The opt-in JIT shared-object tier checks its bitcode-provenance sidecar;
  stale/interrupted entries return to normal compilation.

### Fixed

- REPL v2 snapshots bind source to a fingerprint and byte length; `:load`
  rejects corrupt payloads before rebuilding state. Older source-only snapshots
  remain accepted.
- One C lexical scanner serves formatter function ranges, rankings, and
  duplicate detection, excluding strings, comments, multiline macros,
  conditions, and call continuations.
- `ny-fmt --bugs` diagnoses discarded value-typed `list.append`/`extend`
  results (`NYAUD1120`) and `set()` on frozen `{}` dictionaries (`NYAUD1119`).
- Internal x86-64 JIT/object bundles support 1,024 relocations, up from 256.
  Live requests support 128 lowered user functions, up from 64, and reject
  larger requests before lowering; portable NYIP retains its separate
  4,096-function VM boundary.
- macOS libc imports materialize named aggregate parameter/return layouts
  without registering anonymous carriers as scalars. System headers recover
  supported declarations; project headers remain strict.
- Apple-arm64 compile-time MCJIT finalizes indirect callees through managed
  invocation. Native-only link discovery matches JIT and deduplicates annotations.
- Preserved x86-64 floating constants and typed f32/f64 locals, removing
  nondeterministic ELF results.
- Hardened sanitizer AOT output, cache isolation, cleanup, and UBSan handling.
- Repaired Windows JIT, target-width libc fixtures, variadic C imports,
  trace/debug progress suppression, ELF32 returns, watcher lifetime, parser
  recovery, and dictionary helper ambiguity.

</details>

## [0.7] - 2026-06-30

- LLVM-free native backends, object emission, and C interoperability.

<details>
<summary>Details</summary>

### Added

- Native emitters for x86-64 (default/primary), i386, ARM, AArch64, and
  RISC-V, with debug-scoped WASM, BPF, PowerPC, MIPS, and AVR support.
- In-process ELF64, ELF32, COFF, and Mach-O object writers with relocations
  and multi-function aggregation.
- Narrow internal ELF executable linker with runtime stubs for
  `malloc`/`free`/`realloc`/`calloc`, `memset`/`memcpy`/`memmove`/
  `memcmp`/`memchr`, and `strlen`/`strcmp`/`strchr`.
- `*p` dereference expressions parse as `NY_E_DEREF`, type-check as the
  pointed-to type, lower to `NYIR_LOAD_I64`, and support `*p += value`.
- `addr_of(local)` lowers to `NYIR_ADDR_LOCAL`, executes in the debug VM, and
  emits frame-relative `lea` on x86-64/i386; address-taken local facts are
  invalidated after raw pointer writes so later reads observe mutations.
- The wasm-emscripten web target emits standalone Wasm with `_ny_top_entry`;
  explicit aliases opt into namespacing while existing Nytrix declarations
  keep precedence.
- C frontend hardening: internal aggregate imports decline unsupported nested
  or by-value layouts without poisoning the fallback; strict no-libclang
  aggregate-return imports are covered through `load_layout`; non-positive
  array extents are rejected; unsupported C declarations produce recoverable
  diagnostics instead of hard aborts.
- Parser diagnostics no longer suggest C-style `for (;;)` and point to Nytrix
  iterator syntax instead.

</details>

## [0.6] - 2026-06-30

- Fuzzing, cryptography/math modules, rendering, and platform tooling.

<details>
<summary>Details</summary>

### Added

- Published fuzzer and tooling for local benchmarking and error-shape
  discovery.
- Radix helpers, stream/block ciphers, public-key helpers, and
  lattice/factorization modules.

### Changed

- SVG/UI rendering: 4x4 supersampling, stroke linecap/linejoin,
  gradient/`<use>` support, and terminal 256-color output.
- `--borrow-check` decoupled from `--ownership-strict`; Z3 enabled by default;
  proven-nonzero `f64` division checks elided.
- glTF hot paths moved from `src/code/lang/runtime/gltf.c` into Ny code.
- CMake dependency probing hardened for LLVM, libclang, Z3, Windows
  UCRT/MSYS2.

### Fixed

- Canvas UTF-8 buffer types, terminal rendering, lowercase type-first local
  bindings, and semicolon-comment diagnostics.
- Windows builds, joystick axes, SDK/toolchain probing, and zlib decompression
  capacity handling.

</details>

## [0.5] - 2026-06-05

- Editor/viewer framework, renderer layers, and initial WebAssembly support.

<details>
<summary>Details</summary>

### Added

- Editor and engine viewer (`core.os.ui.render.viewer`): asset browser,
  hierarchy, inspector, gizmos, transform tools, and runtime bootstrap.
- OpenGL, WebGL, and Vulkan renderer paths for the viewer.
- WebAssembly compiler backend foundation.
- RSS feed, Discord, and Mastodon integration.

### Changed

- Renderer/viewer split into distinct `render` and `viewer` layers.
- Function syntax moved from `fn foo(type: arg): ret` to
  `fn foo(type arg) ret`.
- Module self-checks moved into `#main` blocks.

### Fixed

- Vulkan UI mesh caching and startup text-fitting crashes; animated glTF
  index-buffer retention and texture reuse; GLSL syntax and redraw stability.

</details>

## [0.4] - 2026-05-30

- Ownership checks, typed compilation, and a unified CLI.

<details>
<summary>Details</summary>

### Added

- Cross-platform windowing/input: Win32, Cocoa, X11, Wayland, Vulkan.
- Typed compiler pipeline: Hindley-Milner inference, lambda/nested-collection
  inference, and monomorphic specialization.
- `&expr` shorthand for `borrow(expr)`, ownership contracts, `--safe-mode`.
- `handle`, `fnptr`, `seq` types; layout records/guards with compile-time
  reflection.
- Unified CLI: `ny fmt`, `ny test`, `ny doc`, `ny perf`, `ny make`, `ny pkg`,
  `ny new`.

### Changed

- Compiler/runtime/Vulkan internals standardized on raw integer
  representations.
- `-O2` became the default native optimization level.
- Bootstrap and dependency discovery reworked for cross-platform setup.

### Fixed

- Emit-only hangs in recursive raw-integer fast paths and macOS arm64
  compile-time evaluation of immutable collections.
- FFI header collisions, ownership diagnostics for returned values, and mutable
  closure captures across repeated calls.

</details>

## [0.3] - 2026-04-13

- Graphics, media assets, networking/audio, and platform expansion.

<details>
<summary>Details</summary>

### Added

- glTF loading, Meshopt integration, mesh/glTF parsers, and an image parser
  stack.
- Vulkan rendering, scene graph, sky/SDF shaders, and split Vulkan/GUI
  renderer paths.
- Terminal renderer integrated into `core.os.ui`; Win32 window backend added.
- IO and networking modules; JACK audio backend.
- Public fonts, dictionaries, website assets, and renderer shaders.
- Maintained sample programs, REPL import scenarios, and an updated learning
  guide.

### Changed

- Keep canonical stack homes current for dedicated loop GPRs, including
  non-resident CFG-exit consumers.
- Loop GPR homes are enabled by default; `NYTRIX_DISABLE_LOOP_GPR=1` disables
  them for backend diagnosis. Calls within loops remain conservative.
- Platform APIs moved into `core.os`; window backends moved into
  `core.os.ui.window`.
- Legacy native window backend path removed in favor of the new backend split.
- Cache management, bigint support, and shader generation improved for
  graphics workloads.

### Fixed

- Asset paths and shader generation during scene-coverage expansion; runtime
  fixture mismatches from moving platform code into `core.os`.

</details>

## [0.2] - 2026-03-09

- Compiler/runtime bootstrap, semantic analysis, REPL, library, and manuals.

<details>
<summary>Details</summary>

### Added

- Parser, lowering pipeline, AST node definitions, and visitor/function
  lowering.
- Semantic analysis, diagnostics, and statement/call/FFI lowering.
- JIT lowering state, module/JIT integration, and native value-runtime bridge.
- Interactive reader, REPL completion, and build/web launcher with LSP
  commands.
- Enums, packed layouts, `sizeof`, pointer dereference, `try`, reflection
  operators, effects, and the `#main {... }` entry guard.
- Core text, numeric, and cache modules; core IO and string helpers.
- Early UI facade, Vulkan renderer core, and native window/input backends.
- Network/audio backends; block cipher, factorization, RSA lattice, ECC/DLP,
  hash/PRNG, and public-key crypto helpers.
- Specification manuals, release notes, and initial benchmark/regression
  baselines.

### Changed

- Standard library moved to `lib/`, reducing prelude coupling.
- Parser, Vulkan renderer core, and UI renderer split into focused modules.
- Std module layout reorganized; numeric modules moved into `core.math`.
- Python build/bundle tooling replaced with native tools.

### Fixed

- Standard-library import coupling and module-path drift.

</details>

## [0.1] - 2025-12-24

- Initial buildable launcher/runtime tree and smoke fixtures.

<details>
<summary>Details</summary>

### Added

- Launcher skeleton, build script, and CMake scaffold (`make`,
  `CMakeLists.txt`, `src/cmd/ny/main.c`).
- Runtime placeholders and smoke fixtures for a first compilable, testable
  tree.

</details>
