<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Open UI, engine, editor, and compositor work with acceptance criteria."} -->
# UI, Engine, and Editor Roadmap

Only open work is listed; finished items are removed once their acceptance
check has passed. State on 2026-11-17 (second pass): `etc/tests/lang` +
`etc/tests/code/probes` run 1742/1742 under headless Weston (isolated tree:
HEAD plus this pass's runtime and UI files), and the C test
`nytrix-addr-probe-device` passes. Two input paths exist and both matter:
`NYTRIX_FIXTURE_EVENTS` pushes events into the window facade queue (drives
widgets, but bypasses the Wayland listeners); `window.inject_platform_input`
calls the real wl_pointer/wl_keyboard listeners.

Finished in this pass and removed from the list:

- **P0, bounded per-frame memory growth.** Live heap is flat over 20,000
  frames on both routes (idle engine, mock backend, NYTRIX_HEAP_COLLECT_TRACE=1).
  Native `-O3` executable: RSS 85.0 MB at frame 3,681, 87.4 MB at 18,305;
  pauses mean 5.5 ms, worst 7.9 ms (same-day A/B with the previous collector:
  83 MB plateau, worst 14.4 ms). JIT route, cold compile caches: RSS
  2,126 MB at frame 607, 2,141 MB at 19,299 (0.77 KB/frame, was 1.7);
  survivors after each full collection stay at about 6,000 objects; 289
  collections, pauses mean 63 ms, p95 70 ms, worst 109 ms (were 521 ms and
  5.3 s). Causes fixed: the first collection and every full collection read
  every page of the process, including holes of the compiler's freed thread
  arenas (2.15 M pages, 8.4 GB), which also filled the page tables with zero
  pages; now everything is write-protected when recording starts, holes are
  never reported, and full collections read only pages written since the
  start ([Memory: Frame collection](../spec/memory.md#frame-collection)).
  Managed blocks (strings, bytes) come from the collector's pool while it
  records, so a frame's garbage dirties ~1.7 k pages instead of ~30 k spread
  over the compiler's heap.
- **The three UI probe failures of the last full run.**
  `ui-smoke:ui_replay_editor` passes on all routes (repeated, also with a
  4 KB collection threshold). `postfx-effects` took over 15 minutes because
  the typed-buffer registry hash put neighbouring lists on neighbouring slots,
  so densely packed lists became one linear-probe chain walked by every
  insert; slots are now grouped per page and pages scattered (same for the
  managed-pointer table); the probe runs in 24 s. `gui-layout-route-parity`
  passes on all routes under Wayland; under an X display without DRI3 it
  falls back to GL on Mesa llvmpipe in the compiler's process and crashes in
  the LLVM symbol clash below (standalone executables fall back cleanly).
- **Integer output scales (HiDPI).** The Wayland backend tracks the outputs a
  surface is on, reports the largest output scale as the window content
  scale (framebuffer = window size x scale), and sets `wl_surface.set_buffer_scale`
  when a buffer of the new size is presented (Vulkan: `surface_buffer_ready`;
  GL: the EGL back buffer size queried at swap). Configure acks compare in
  surface units. Fractional scales take the `wp_fractional_scale_v1` value and
  a `wp_viewport` destination (implemented, see P1). Verified on headless
  Weston `--scale=2`: widgets demo 700x450 window draws a 1400x900 buffer at
  the right size with sharp text on Vulkan and GL (scale 1 unchanged, Vulkan
  and GL pixel-identical); `code/probes/ui/wayland-seat-input` now converts
  between surface units and framebuffer pixels and passes at scale 1 and 2
  (clicks, fast clicks, XKB typing). Apps scale their UI by
  `window.get_window_content_scale` (done in `etc/projects/ui/widgets.ny`).
  Also fixed: all locally defined Wayland interfaces declared `destroy` with
  a new_id argument.

UI performance work stays in this checklist. Compiler and NYR performance
work is tracked in [Performance work](perf.md).

### P1 — Complete real-compositor behavior

- [ ] **Exercise window and text-input environments.** Done: window creation
  failure reporting (`code/probes/ui/window-error-report`), TTY backend in a
  pty, seat input on headless Weston and the kiosk shell, integer scale 2 on
  Vulkan and GL (above). Still untested, because no available compositor
  offers them: fractional scales (`wp_fractional_scale_v1` + `wp_viewport`
  code exists but has never run against a compositor), IME
  composition/commit/cancel (`zwp_text_input_v3`; listeners exist, handlers
  are empty), clipboard (`wl_data_device` needs a seat), and events from a
  physical seat. Headless Weston 15 has no seat, only text-input v1 and no
  fractional scale; Hyprland nested in headless Weston refuses to start
  (needs `wl_compositor` v6, Weston offers v5). A headless sway
  (`WLR_BACKENDS=headless`) has a seat, text-input-v3/input-method-v2 and
  fractional scale; it is not installed. A JIT-run app on Mesa llvmpipe
  (software GL) crashes in shader compilation: the system `libLLVM.so.23`
  that Mesa loads binds its internal calls to the compiler's unversioned
  vendored `libLLVM.so.21` symbols. Standalone executables are not affected.
  Acceptance: captures/probes at each supported scale and size, no lost input
  or text, and an explicit support result for each backend.

### P2 — Toolkit and engine

- [ ] **Cut the runtime's value-classification cost in UI frames.**
  Acceptance: no single classifier above 3% of samples in an idle engine
  frame, with the `lang/memory` fixtures passing. Status 2026-11-17 (native
  `-O3` engine, Vulkan, headless Weston, 1,500 frames, flat
  `perf record -F 2000`): frame CPU fell from 63.4 G to 22.8 G cycles. The
  readability probe used `process_vm_readv`, which refuses device memory
  (VM_IO/VM_PFNMAP, e.g. a Vulkan buffer mapped from /dev/dri) with EFAULT,
  so raw stores into mapped vertex buffers were silently dropped: the engine
  drew no editor/inspector panels on Vulkan. Now EFAULT is settled by a
  pipe-copy probe (`rt_addr_probe_copy`, C test `nytrix-addr-probe-device`
  on `[vvar]`), and `rt_store32_idx` skips its checks for pages proven
  readable that hold no runtime object (the old fast path keyed on the
  exact base pointer and missed 94% of vertex stores; the free epoch was not
  the cause). `rt_gc_owns_pointer` no longer takes the GC lock when the GC
  heap has made no object. Still above 3%: `rt_heap_ptr_registry_has` 5.4%,
  `rt_tbuf_registered_handle` 4.2%, `is_v_str` 3.3%, `rt_map_oracle_check`
  3.0%; `rt_store32_idx` 14.8% is mostly the stores into GPU memory. The
  registry lookups are cheap per call (cache hits and a page bitmap); the
  cost is volume, about 60,000 calls per frame from `is_v_str`,
  `rt_cstr_to_str`, `rt_load32_idx`, `rt_is_str`, `rt_tbuf_check`. A page
  filter in front of the managed table and a last-handle check in the typed
  buffer lookup measured no change and were dropped. Next: cut the calls
  (typed paths in the renderer and toolkit that avoid re-classifying known
  lists and strings).

## Handoff notes (2026-11-17, second pass)

- **Where the code is.** Everything is uncommitted in the working tree:
  `src/code/backend/runtime/{core.c,memory.c,shared.h,string.c,gc.c}`,
  `src/code/test/backend/runtime/addr_probe_device.c`, `CMakeLists.txt`,
  `docs/spec/memory.md`, `etc/tests/lang/memory/heap-collect.nshape`,
  `lib/os/ui/window/platform/{mod.ny,linux/wayland/mod.ny,opengl/mod.ny,opengl/egl.ny}`,
  `etc/projects/ui/widgets.ny`, `etc/tests/code/probes/ui/wayland-seat-input.nshape`.
  Copies: `~/nytrix-recovery/ui-2026-11-17-session2/`. `.agents` (sync) is
  still unmounted. Other uncommitted files (`parser/*`, `semantics/*`,
  `etc/tests/lang/errors/type/*`, `types/*`, `runner.c`, `positivity.c`)
  belong to another session.
- **Testing.** Another session rebuilds `build/claude-dev` while running; use
  an isolated tree (`git archive HEAD` + own files, `cmake -B build/p`,
  `build/vendor` symlinked). Run the suite under headless Weston: with the
  VS Code X display (`DISPLAY=Arch:11.0`, no DRI3) GPU probes fall to
  llvmpipe and hit the LLVM clash. `weston --scale=2` with
  `--width/--height` doubled gives a HiDPI output.
- **User-visible state:** on Vulkan the engine's and editor's GUI panels
  draw again (they were blank since the `process_vm_readv` probe); HiDPI
  outputs get full-resolution windows.

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
