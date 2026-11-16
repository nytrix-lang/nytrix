<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Open UI, engine, editor, and compositor work with acceptance criteria."} -->
# UI, Engine, and Editor Roadmap (Fable 5.2)

Only open work is listed; finished items are removed once their acceptance
check has passed. State on 2026-11-16 (private snapshot `acdf17fa` plus the
handoff commit): `etc/tests/code/probes` + `etc/tests/lang` run 1686 fixtures
with only six pre-existing failures (`comptime-contracts`, `doc-meta`,
`doc-rank`, `terminal:test_term_quit_key`, two lexer-invalid-byte fixtures);
`lang/modules` + `code/probes/ui` (173) all pass. Loaded glTF scenes draw
textured on Vulkan and GL over the default HDR sky. Compositor checks run on
headless Weston with Vulkan and GL. Two input paths exist and both matter:
`NYTRIX_FIXTURE_EVENTS` pushes events into the window facade queue (drives
widgets, but bypasses the Wayland listeners); `window.inject_platform_input`
calls the real wl_pointer/wl_keyboard listeners (see P1).

Finished in this pass and removed from the list: the old P3 (editor palette,
context menu, find bar and colour picker are `viewer/gui.ny` windows; probe
`editor-gui-popups`), and P4 (smem rewritten on `core.os.memscan`, probe
`os/memscan`; screen effects `core.os.ui.render.postfx` with
`set_blend_factors`, probe `postfx-effects`, demo `etc/projects/ui/postfx.ny`).

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
  - Seat input (2026-11-16): on a real compositor (Hyprland) no click or
    key reached any app. The Wayland listeners keyed windows by
    `int(surface)`, which the compiler stored as a pointer key but looked
    up as an int, so every lookup missed (compiler fixed: `int(ptr)` is an
    integer); the keymap handler's bare `close(fd)` resolved to the window
    `close` and quit the app; a press and release inside one
    `wl_pointer.frame`, or one toolkit frame, lost the click. All fixed.
    `window.inject_platform_input` drives the backend's own listeners, so
    `code/probes/ui/wayland-seat-input` covers pointer, fast clicks, XKB
    typing with Shift, and Backspace on headless Weston (also under the
    kiosk shell, which forces the window size as tiling compositors do).
  - A JIT-run app on Mesa llvmpipe (software GL) crashes in shader
    compilation: Mesa's LLVM 21 and the compiler's LLVM 23 share symbols
    in one process. Standalone executables are not affected.
  Still untested: fractional scales, IME composition/commit/cancel,
  clipboard, and events from a physical seat. Acceptance: captures/probes at each supported scale and
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

### Compiler defects found during this work (open)

- The engine built as a standalone executable (`--run=none --output`, -O1
  -O1, -O3 and -O0) crashes on Vulkan in its first frame: `_select_static_draw_pipeline`
  passes the line pipeline as a tagged word (`0x34091021` for handle
  `0x1a048810`) to `vkCmdBindPipeline`. The same engine runs on the default
  LLVM route and `ny --native-only`; a compiler from 2026-11-13 also crashed
  on this route (earlier, in OpenEXR `findSlice`). A minimal module-global
  `?handle` written in one module and read in another does not reproduce it.

## Handoff notes (2026-11-16)

Where the last session stopped, so the next one can resume without
re-deriving it.

- **Next item in progress: standalone engine crash** (compiler defects
  above). Reproduce: `ny --native-only -O1 --run=none --output eng.exe
  etc/projects/ui/engine.ny`, run under headless Weston with
  `NY_UI_RENDER_BACKEND=vk NYTRIX_UI_REPLAY=1 NYTRIX_UI_REPLAY_BACKEND=vk
  NYTRIX_FIXTURE_FRAMES=30`; -O0 executables crash the same way, JIT routes
  do not. Found so far: the global `core.os.ui.render.vk.state._line_pipeline`
  holds the raw handle (gdb watchpoint), but in
  `vk/draw.ny:_select_static_draw_pipeline` the executable route lowers
  `target = _line_pipeline` (and the other pipeline globals) through
  `rt_tag`, i.e. the `mut ?handle target` local is a dynamic slot there and
  a raw one on the JIT route (diff the function with `NY_DUMP_FN=
  core.os.ui.render.vk.draw._select_static_draw_pipeline` on both routes).
  Treating declared `ptr`/`handle` locals as raw in the stmt.h slot join
  (`declared_raw_scalar`) did NOT fix it and was reverted; the dynamic slot
  decision comes from another rule (look at `prior_dynamic_slot` and
  `canonical_dynamic_init` for the declaration, and why `nyr_expr_is_any`
  differs for imported cells between `--run=none` and the JIT). A small
  two-module `?handle` repro does not reproduce it.
- **P2** has not been touched since its entry was written.
- **P1** remaining: fractional scale (needs `wp_viewport`/buffer scale,
  swapchain/EGL resize, toolkit scale), IME (text-input-v3), clipboard, and
  a check with a physical seat. The user's Hyprland monitor is scale 1.
- **P0** needs a design decision (reclaiming heap vs frame region); nothing
  started.
- **Test environment used:** a clean copy of the repo at
  `$N/tree` (N = the session scratchpad) with other agents' uncommitted
  backend edits reverted to HEAD, built in `build/p2`; live-tree edits were
  copied or patched into it before building. Headless Weston helper scripts
  (`shoot.sh`, `in_weston.sh`, kiosk variant) and the smem test target
  (`target/target.c`, a C program with known values; `smem_run.sh` starts it
  as smem's child so ptrace scope allows attaching) lived in that
  scratchpad and are not in the repo; the recipes are in this file and in
  the agent memory notes.
- **User-visible state:** input on real Wayland works (the user confirmed
  clicks and typing on Hyprland after the fixes); apps no longer launch
  twice on close (the runtime calls `main()`; apps must not call it
  themselves). smem has the one-row icon top bar the user asked for.

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
