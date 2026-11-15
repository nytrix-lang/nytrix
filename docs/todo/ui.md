<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Open UI, engine, editor, and compositor work with acceptance criteria."} -->
# UI, Engine, and Editor Roadmap (Fable 5.2)

Only open work is listed; finished items are removed once their acceptance
check has passed. State on 2026-11-14 (evening, private build of the
current tree): the UI probe directory passes on native -O0, native -O3, and
LLVM JIT (85 shapes, no exceptions; `editor-keymap-profiles` now passes on
MCJIT). Loaded glTF scenes draw textured on Vulkan and GL over the default
HDR sky. Compositor checks run on headless Weston with Vulkan and GL;
scripted pointer input goes through the window event queue
(`NYTRIX_FIXTURE_EVENTS`) and drives widgets on the real backends.

UI performance work stays in this checklist. Compiler and NYR performance
work is tracked in [Performance work](perf.md).

### P0 — Restore correctness and long-run stability

- [ ] **Bound per-frame memory growth.** The idle engine on the mock backend
  allocates about 124 KB of live heap per frame on native `-O3` (massif on a
  `-g` executable, 400 frames, 2026-11-15) and its RSS grows 143 KB/frame on
  MCJIT when the compile comes from the JIT cache. RSS sampled after an
  in-process compile under-reports this (it showed 23 KB/frame native and
  0.6 KB/frame MCJIT `-O3`): the frames first reuse the memory the compiler
  freed, so measure with massif or a cache-warm run. The default heap never
  reclaims a dropped temporary, and no heap mode does for lists
  (`--heap=gc`, `--heap=rc`, and `--mode=safe` leak the same minimal loop),
  so every rectangle list, queue, key string, and option dict the toolkit
  builds per widget stays allocated. Done so far: float reads no longer box
  (`rt_value_get_f64`, `rt_tbuf_index_any_f64`), the float intern table
  probes, the text run queue and batch are reused, text measures are keyed
  per font, `set_model_matrix` no longer clones per element, and the print
  history is a ring. What remains is spread over hundreds of sites of about
  0.3 MB per 400 frames each (`_layout_rect`, `_clip_rect_inset`,
  `_window_body_clip`, `scissor_push`, `begin_scroll_area`, `_wid`,
  `_main_loop_count_frame`, `_idle_opts`, `_inspector_state`), so site-by-site
  edits will not reach zero: this needs either a reclaiming heap for lists,
  dicts, strings, and float boxes, or a frame-scoped region released at
  `gui.end_frame` with escape tracking. Acceptance: flat live heap over a
  20,000-frame run on native and JIT routes.

### P1 — Complete real-compositor behavior

- [ ] **Exercise window and text-input environments.** Cover fractional and
  HiDPI scaling, resize, IME composition/commit/cancel, clipboard, and the
  TTY backend in a real terminal. For GL on Wayland, first make window
  creation failure observable; do not count "no window appeared" as a pass.
  Pointer and keyboard events delivered by a compositor seat are still
  untested (headless Weston has none). Acceptance: captures/probes at each
  supported scale and size, no lost input or text, and an explicit support
  result for each backend.

### P2 — Toolkit and engine

- [ ] **Remove the last post-load frame spike.** The first frame with the
  default EXR sky used to stall about 13 s on Vulkan (three procedural
  environments built eagerly, ~3 s each, plus a 2 s EXR conversion). On
  2026-11-14 the environments are built only when a scene prefers one, the
  conversion takes ~0.6 s, and a startup worker decodes the sky
  (`NY_UI_SKY_PREFETCH_OFF=1` disables it). Clicking BarramundiFish 2 s
  after startup: 144 ms to scene, load frame 126 ms, no sky stall (835 ms
  without the worker). One ~55 ms frame about 100 frames after the load
  remains and also appears with the worker off. Acceptance: first-load
  click-to-scene under 300 ms with no frame over 50 ms after it.

### P3 — Editor and panels

- [ ] **Move the editor's popups onto the shared toolkit.** The engine and the
  gallery use `viewer/gui.ny` for menus, popups, tables, and docking; the
  editor still draws its own palette, context menu, find bar, and colour
  picker. Port them to `gui` popups and delete the duplicates. Acceptance:
  each subsystem has focused probes and no widget behaviour is implemented
  twice.

### P4 — Native media and UI libraries

- [ ] **Shader effects and render handling.** Needs the effect list the user
  supplied earlier; it is not in the repository. Acceptance: each effect
  renders on Vulkan and GL on the compositor with a capture, toggles at run
  time, and has a probe.
- [ ] **Vector graphics: composite modes and transformed text.**
  `core.os.ui.render.vg` covers paths, fills with holes, strokes, joins and
  caps, antialiasing, gradients, image patterns, transforms, scissor, and
  text, with `vg-golden` passing on mock, Vulkan, and GL (11 ms and 18 ms
  for the thirty-shape card against a 20 ms budget). Remaining: blend
  operators other than source-over (the renderer has one alpha blend
  pipeline), and glyphs that follow rotation and scale.
- [ ] **Port/fix `smem` process memory scanner UI (`etc/projects/ui/smem.ny`).**
  Single-file Nytrix port of `x3ric/smem` (https://github.com/x3ric/smem);
  original reference cloned at `/tmp/kilo/smem` (`README.md`, `src/main.rs`,
  `src/types.rs`, `src/scan.rs`, `src/ui.rs`, `test`). Current state (verified
  from `/home/e/nytrix`): `./build/release/ny etc/projects/ui/smem.ny` and
  `./make ny etc/projects/ui/smem.ny` compile and run (warnings only);
  `timeout 150 env NY_UI_HEADLESS=1 ./build/release/ny etc/projects/ui/smem.ny`
  and `timeout 180 ./make ny etc/projects/ui/smem.ny` exit 0. Implements typed
  parsing/encoding/compare (`bool/byte/hex/int*/float*/size/ptr`, auto sizing,
  scan helpers), `/proc/<pid>/maps` grouping and `/proc/<pid>/comm` PID
  listing/filtering, attach/detach via `/proc/<pid>/mem` (`__open_raw`,
  `__read_off_raw`/`__write_off_raw` with `ptr_add`), scan history with
  first/next/prev/reset and modes (Exact/Changed/Unchanged/Increased/Increased
  or Greater/Increased by/Decreased/Decreased or Less/Decreased by), set-all
  and lock writer (mutex-guarded background thread), GUI panels for
  attach/results/maps with hex preview and address copy, hotkeys F2/F3/F4/F5/
  F11/F12, and optional PID argv. Remaining work to completely fix/finish and
  reach parity with upstream behavior/UI:
  - **Feature parity:** implement missing controls/hotkeys from README (`S`/`R`
    scan/region view toggles, `F1` attach/detach, `F5` focus specific address,
    `F7` lock toggle/management with visible state, `F8-F10` quick scans for
    changed/increased/decreased), per-region selection/filter behavior matching
    upstream `selected_region` semantics, and full results/maps interaction.
  - **Visualization/UI fidelity:** replace capped text/hex preview with a
    responsive byte-grid/value visualization closer to `smem`'s egui view
    (zoom-scaled cells, row addressing, hover tooltip/value decode at pointer,
    right-click copy behavior), correct scroll/viewport handling for large
    maps/results, and polish layout/state (view mode `auto/maps/results`,
    PID/process picker UX, error/status reporting).
  - **Correctness/perf:** audit scan correctness on real processes (type scan
    ordering, baseline/next semantics, partial reads, large regions chunking,
    address/value refresh), ensure lock thread lifecycle/start-stop safety and
    no stale writes after detach/reset, and avoid UI stalls on large `/proc`
    scans or previews.
  - **Compiler/runtime warnings:** eliminate `W2101/W2102` dynamic-type-fallback
    warnings in `etc/projects/ui/smem.ny` (currently includes e.g. comparison
    operands inferred as `any == bool` at `107:30`, `any < f64` at `231:48`,
    `232:48`, `233:22`, with suppressed follow-ups) by adding precise types/
    guards so strict verification is clean.
  - **Verification:** exercise interactively on a real display/compositor and
    against a known target process (`./build/release/ny etc/projects/ui/smem.ny
    <pid>` as root/capable user) to confirm attach, scan/filter/set/lock, map
    browsing, copy/tooltip, and hotkeys; keep headless/mock paths working.
  - **Acceptance:** behavior and controls match the upstream README/source for
    supported Linux `/proc` workflows, no compile warnings, stable on native/JIT
    routes used for UI tools, and no regressions in existing UI/tool checks.

### Compiler defects found during this work (open)

- A module global whose name matches a variable anywhere else in the loaded
  libraries can be lowered as that other value. With `mut alpha = 0` in one
  module, an importing module's `alpha * 1000 + vc` calls the list-repeat
  runtime and returns 12 after `alpha = 2`, because the bare-name lookup
  finds `def alpha = [0, 1]` inside a function of `lib/math/gf.ny`. Renaming
  the global to a unique name gives 2012. The resolved cell symbol, not the
  bare name, should drive the operand type.
- A module global initialised with a negative literal (`mut word = -1`) and
  assigned from an importing module reads back `nil` through `owner.word`
  from the root program.
- With two inline modules that both own a global `x`, the root program's
  `second.x` reads the first module's cell. Importing modules now resolve
  the bare name correctly (`lang/modules/module-global-same-leaf-two-owners`).

## Execution brief

> Work from the current checkout and `docs/todo/ui.md` and
> `docs/todo/perf.md`; inspect active agent ownership before touching shared
> files. Treat every dated number above as historical until reproduced. Start
> with P0 correctness and stability, then proceed in priority order; do not
> skip open acceptance criteria or report a workaround as a fix. For each task:
> reproduce on the current tree, capture a minimal failing test/probe and
> baseline, identify the responsible invariant or measured bottleneck,
> implement the smallest maintainable fix, add/update durable behavioral
> coverage, run the narrowest relevant check, and record exact commands/results.
> For UI claims, verify the real Wayland compositor and capture evidence;
> mock tests alone do not close compositor items. Update this checklist only
> when its stated acceptance evidence passes. Do not leave debug scaffolding,
> temporary compiler copies, background processes, or unexplained failing
> probes.
