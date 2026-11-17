<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Open UI, engine, editor, and compositor work with acceptance criteria."} -->
# UI, Engine, and Editor Roadmap

Only open work is listed; finished items are removed once their acceptance
check has passed. State on 2026-11-17 (third pass): `etc/tests/lang` +
`etc/tests/code/probes` run 1743/1743 under headless Weston (isolated tree:
HEAD plus the uncommitted runtime and UI files), and the C test
`nytrix-addr-probe-device` passes. Two input paths exist and both matter:
`NYTRIX_FIXTURE_EVENTS` pushes events into the window facade queue (drives
widgets, but bypasses the Wayland listeners); `window.inject_platform_input`
calls the real wl_pointer/wl_keyboard listeners.

Earlier passes finished P0 (bounded per-frame memory), the three failing UI
probes and integer HiDPI scales; this pass finished P2 below. The only open
item is P1, which needs a compositor this machine does not run.

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
  fractional scale; it is not installed system-wide. The signed Arch packages
  (`sway 1:1.12-4`, `wlroots0.20 0.20.2-1`, `wtype 0.4-2`) unpack and resolve
  against the installed libraries without root, but starting a private
  instance from them was refused by the agent's permission policy in the
  2026-11-17 third pass, so this needs the user to install sway or allow
  running it. A JIT-run app on Mesa llvmpipe
  (software GL) crashes in shader compilation: the system `libLLVM.so.23`
  that Mesa loads binds its internal calls to the compiler's unversioned
  vendored `libLLVM.so.21` symbols. Standalone executables are not affected.
  Acceptance: captures/probes at each supported scale and size, no lost input
  or text, and an explicit support result for each backend.

## Finished in the third pass (2026-11-17)

- **P2, value-classification cost in UI frames.** Acceptance met: in an idle
  engine frame (native `-O3`, Vulkan, headless Weston, 7,000 frames, flat
  `perf record -F 2000` started 12 s in, after startup) no classifier is
  above 3%: `rt_heap_ptr_registry_has` 2.99%, `rt_tbuf_registered_handle`
  2.77%, `rt_map_oracle_check` 2.36%, `is_v_str_unmanaged` 2.05% (were 5.4,
  4.2, 3.0 and 3.3); `lang/memory` passes. The registry lookups were cheap;
  the cost was volume and repeated questions:
  - The renderer read its trace/diagnostic environment flags through
    `env_truthy_cached` (a dictionary lookup, every value classified) on every
    flush and pipeline bind; hot sites now keep the flag in a variable and
    re-read it only when `ui_profile.env_generation()` changes (any reset or
    override bumps it). The colour-font test lower-cased the font path and
    ran five `str.find` per glyph; it is memoized on the font.
  - GUI text was queued as five mixed values per run and re-packed per font at
    flush; it is now queued in draw layout, segmented by font.
  - `rt_native_is_str(x) || is_v_str(x)` (about 60 sites) is just
    `is_v_str(x)`; callers that already asked `rt_heap_object_ptr` reuse the
    answer (`is_v_str_managed`/`is_v_str_unmanaged`, `rt_len`,
    `rt_any_to_i64`, `rt_value_tag`, dictionary key normalization); element
    reads, `rt_has_tag` and `rt_tbuf_get_any` validate a typed buffer once
    instead of up to six times.
  - The typed-buffer cache and the managed-pointer cache (now 2,048 entries,
    holding hits and misses) are invalidated per slot instead of wholesale on
    every free. String literals of a native executable (`ny_str_N` globals)
    remember their interned copy per thread.
- **Crash: small-string probe read an unmapped page.** Found by the new
  fixture `lang/memory/classifier-caches` (3 of 8 runs segfaulted, also on the
  previous runtime). `rt_is_sso_string` reads `p - 32`; for a block at the
  start of a fresh mapping that is the page before, and three sources still
  called it readable after it was unmapped (a compiler worker thread's
  stack): the per-thread page cache, the `/proc/self/maps` snapshot (taken
  once) and the oracle's page bitmap (never cleared). Spans that cross a page
  now ask the kernel unless the page is in a stable mapping (file, main stack,
  vDSO), and freed large blocks leave the oracle (`rt_map_oracle_forget`).
  Engine wall time is unchanged (20.4 s vs 20.3 s for 4,000 frames).

## Handoff notes (2026-11-17, third pass)

- **Where the code is.** Everything is uncommitted in the working tree:
  `src/code/backend/runtime/{core.c,memory.c,shared.h,string.c,gc.c,os.c,ffi.c,bigint.c}`,
  `src/code/test/backend/runtime/addr_probe_device.c`, `CMakeLists.txt`,
  `docs/spec/memory.md`, `etc/tests/lang/memory/{heap-collect,classifier-caches}.nshape`,
  `lib/os/ui/render/{dump.ny,mod.ny,vk/renderer.ny,vk/utils.ny,vk/texture.ny,viewer/gui.ny}`,
  `lib/os/ui/window/platform/{mod.ny,linux/wayland/mod.ny,opengl/mod.ny,opengl/egl.ny}`,
  `etc/projects/ui/widgets.ny`, `etc/tests/code/probes/ui/wayland-seat-input.nshape`.
  Other uncommitted files (`parser/*`, `semantics/*`, `etc/tests/lang/errors/type/*`,
  `types/*`, `runner.c`, `positivity.c`) belong to another session.
- **Testing.** Another session rebuilds `build/claude-dev` while running; use
  an isolated tree (`git archive HEAD` + own files, `cmake -B build/p`,
  `build/vendor` symlinked). Run the suite under headless Weston: with the
  VS Code X display (no DRI3) GPU probes fall to llvmpipe and hit the LLVM
  clash. The UI replay probes take minutes each; when other sessions build at
  the same time (load average near 100) they hit the runner's timeout, so
  re-run them alone before reading a failure as a regression.
- **Profiling.** `perf record --user-callchains --call-graph dwarf` works
  (kernel samples are refused); `-D 12000` skips engine startup (the sky
  EXR decode) so the profile is the idle frame. Under zsh, `$a` holding
  several flags is passed as one argument; use `${=a}` or a `case`.

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
