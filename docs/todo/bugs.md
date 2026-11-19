<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"project","order":5,"summary":"Open compiler correctness bugs with minimal reproductions."} -->
# Compiler bugs

Open correctness bugs found while working on other items. Each entry has a
reproduction; an item closes when its reproduction passes on every route and a
regression fixture covers it.

- [ ] **SVG raster probe fails at native -O3 (`peephole`).**
  `etc/tests/code/probes/ui/svg.nshape:svg-icon-pixels` fails only on
  `--native-only -O3` with `Nytrix assertion failed: red rectangle center`
  (the first `load8(pixels, center) == 255` check); `--native-only -O0` and
  `--native-backend=llvm --jit --jit-engine=mcjit` both print
  `SVG-ICON-PIXELS-OK`. `--nyr-disable-pass=peephole` restores the pass, and no
  other single pass does, so the peephole rewrite is folding something in the
  raster loop incorrectly. The fixture already runs all three routes, so it is
  the regression test once the fold is fixed.

- [ ] **UI replay engine JIT route segfaults through a null call.**
  `build/release/ny-test ui-replay --compiler build/release/ny-full --apps engine`
  exits 139 on `engine/jit`; `engine/native` and `engine/vm` pass, as do the
  other replay apps (animation, editor, input, monitor, pong, synth, term).
  Minimal reproduction: the JIT route with `NY_UI_SKYBOX_SOURCE=generated`
  (omit the variable and the same command exits 0):

  ```
  NY_UI_SKYBOX_SOURCE=generated NYTRIX_UI_REPLAY=1 NY_UI_RENDER_BACKEND=mock \
  NYTRIX_FIXTURE_TIMEOUT_MS=0 NYTRIX_FIXTURE_FRAME_MS=100 \
  NYTRIX_FIXTURE_FRAMES=5 NYTRIX_FIXTURE_WIDTH=32 NYTRIX_FIXTURE_HEIGHT=24 \
  NYTRIX_FIXTURE_EVENTS="5,close" build/release/ny --native-backend=llvm \
  --jit --jit-engine=mcjit -O0 --parallel=off --no-progress \
  etc/projects/ui/engine.ny
  ```

  Under gdb: `#0 0x0000000000000000` called from
  `ny_fn_core.os.ui.render.viewer.engine.env.load_skybox` ←
  `startup_init_timers_and_skybox` ← `startup`. So the generated-skybox path
  makes an indirect call whose target is null on the JIT route (the
  `generate_compare_visible_env_image` / `render_scene.upload_generated_texture`
  pair is the likely site). Covered by
  `etc/tests/code/probes/ui/ui-smoke.nshape:ui_replay_engine`.

- [ ] **Verify the Apple arm64 JIT data mapping on macOS.** Data sections in
  `gen/llvm/jit.c` (`ny_apple_jit_alloc_section`/`finalize`) are now plain RW
  pages (not `MAP_JIT`, never executable); only code keeps `MAP_JIT`. This
  path compiles only on macOS and has not been built or run there yet.
