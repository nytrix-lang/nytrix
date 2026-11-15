<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Open UI, engine, editor, and compositor work with acceptance criteria."} -->
# UI, Engine, and Editor Roadmap (Fable 5.2)

Only open work is listed; finished items are removed once their acceptance
check has passed. State on 2026-11-15 (private build of the
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
  per font, `set_model_matrix` no longer clones per element, the print
  history is a ring, and float boxes are tracked per pool chunk instead of
  in a per-box hash set (its doubling rehash was a 50-140 ms frame). What remains is spread over hundreds of sites of about
  0.3 MB per 400 frames each (`_layout_rect`, `_clip_rect_inset`,
  `_window_body_clip`, `scissor_push`, `begin_scroll_area`, `_wid`,
  `_main_loop_count_frame`, `_idle_opts`, `_inspector_state`), so site-by-site
  edits will not reach zero: this needs either a reclaiming heap for lists,
  dicts, strings, and float boxes, or a frame-scoped region released at
  `gui.end_frame` with escape tracking. Acceptance: flat live heap over a
  20,000-frame run on native and JIT routes.

### P1 — Complete real-compositor behavior

- [ ] **Exercise window and text-input environments.** Results so far
  (2026-11-15, headless Weston, Vulkan and GL):
  - Window creation failure is observable: `gfx.window_error()` names the
    requested backend and the display variables when `init_window` returns
    false or falls back to the off-screen mock surface, and is empty when
    the window opened (`code/probes/ui/window-error-report`).
  - Integer output scale 2: both backends draw the same frame, but into a
    1x buffer that the compositor upscales, so text is soft. The Wayland
    layer records `wp_fractional_scale_v1` in `content_scale` and never
    applies it. Not HiDPI-aware yet: needs the buffer sized by the scale
    (`set_buffer_scale` or `wp_viewport`), the swapchain/EGL surface resized
    to match, the toolkit scale multiplied in, and pointer coordinates kept
    logical.
  - TTY backend in a pseudo-terminal (100x30, truecolour): draws, and now
    restores the terminal on `exit()` as well as on `close_window()` (the
    runtime exit skipped its terminal restore, leaving raw mode, the
    alternate screen, and a hidden cursor).
  Still untested: fractional scales, resize, IME composition/commit/cancel,
  clipboard, and pointer/keyboard events from a compositor seat (headless
  Weston has none). Acceptance: captures/probes at each supported scale and
  size, no lost input or text, and an explicit support result for each
  backend.

### P2 — Toolkit and engine

- [ ] **Cut the runtime's value-classification cost in UI frames.** With a
  scene loaded on Vulkan the render thread spends about 84% of its samples
  in runtime C helpers that decide what a word is (2026-11-15):
  `ny_native_dict_ptr` 13%, `rt_tbuf_registered_handle` 9%,
  `rt_addr_readable` 9% (two variants), `rt_tbuf_check` 6%,
  `rt_heap_ptr_registry_has` 4%. `rt_addr_readable` was 22% plus 9% kernel
  time in `mincore`; a page bitmap of runtime-owned ranges and a two-entry
  cache in front of the mapping snapshot's binary search brought it down.
  `len()` now checks the typed-buffer handle cache and the native
  dictionary registry before the layout probes (182 -> 42 ns on a dict,
  67 -> 13 ns on a list); `ny_native_dict_ptr` (12.5%) and `rt_len` (7.7%)
  are still the top two, called mostly from JIT code through `rt_len`.
  Acceptance: no single classifier above 3% of samples in an idle engine
  frame, with the `lang/memory` fixtures passing.

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
- [ ] **Vector graphics: composite modes.** `core.os.ui.render.vg` covers
  paths, fills with holes, strokes, joins and caps, antialiasing, gradients,
  image patterns, transforms, scissor, and text that follows rotation, scale
  and skew on Vulkan and GL (`vg-golden` passes on mock, Vulkan, and GL).
  Remaining: blend operators other than source-over. They need
  premultiplied output from `vg` (vertex colours, baked gradients, image
  patterns), a blend-factor setter in the renderer with one Vulkan pipeline
  per factor set, `glBlendFuncSeparate` on GL, and the same formulas in the
  software renderer. Acceptance: the eleven Porter-Duff operators match a
  reference card on all three backends, with a probe.
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

- The engine built as a standalone executable (`--run=none --output`, -O1
  and -O3) crashes on Vulkan in its first frame: `_select_static_draw_pipeline`
  passes the line pipeline as a tagged word (`0x34091021` for handle
  `0x1a048810`) to `vkCmdBindPipeline`. The same engine runs on the default
  LLVM route and `ny --native-only`; a compiler from 2026-11-13 also crashed
  on this route (earlier, in OpenEXR `findSlice`). A minimal module-global
  `?handle` written in one module and read in another does not reproduce it.

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
