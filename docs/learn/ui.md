<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":60,"summary":"Build a windowed program and check rendering or input support on its target."} -->
# UI

Use `core.os.ui.window` for window and input state and
`core.os.ui.render` for drawing. Native and browser targets have different host
capabilities; a matching function name is not proof of matching behavior.

## Start from a working project

The repository has focused programs under [`etc/projects/ui/`](../../etc/projects/ui/):

- `engine.ny` exercises the renderer and asset viewer.
- `input.ny` displays keyboard and gamepad input.
- `monitor.ny` inspects monitor and framebuffer state.
- `term.ny` demonstrates terminal rendering.

Run a project only on a host with its required display, graphics backend, and
assets. For browser builds, use [Browser and WebAssembly](browser.md).

## Frame responsibilities

| Stage | Work |
| --- | --- |
| Initialize | Create the window and graphics context; check the returned result. |
| Poll | Process close requests and input once per frame. |
| Update | Change application state from input and elapsed time. |
| Draw | Begin a frame, draw the current state, and present it. |
| Release | Destroy loaded textures, fonts, meshes, and the window/context. |

Keep resources outside the per-frame loop when they do not change. Release
owned resources when the application exits.

## Coordinates and input

Choose one coordinate model for the whole frame. Pixel coordinates track the
live framebuffer; an aspect-fit design space preserves a chosen logical size
when the window changes. Query the renderer docs for the exact setup and
resize behavior.

Poll input from the same live window that owns the event queue. For held state,
use the documented down-state API; use edge-triggered state for one-shot actions
such as confirming a menu choice.

## Check a target capability

The renderer exposes capability information for portable and backend-specific
features. Check it before requesting a Vulkan-only, compute, or other
backend-specific operation. Do not silently substitute a different rendering
path when a requested capability is absent.

Find current signatures in the checked-out standard library:

```bash
ny doc get core.os.ui.window
ny doc get core.os.ui.render
ny doc search --symbols draw_rect
ny doc search --symbols key_pressed
```

Use `./make web-test` for the browser runner. For route-specific support, see
[Implementation status](status.md) and [execution routes](status.md#execution-routes).

## Verification

A successful compile or window creation does not prove drawing. Verify a real
frame, input response, resize, and clean exit on the selected backend before
profiling it. Browser execution has a separate [host contract](browser.md).

### Select a backend and host

The `input.ny` project reads `NY_UI_RENDER_BACKEND` through
`render.apply_backend_env()`:

```bash
NY_UI_RENDER_BACKEND=gl build/release/ny-full --no-progress etc/projects/ui/input.ny
NY_UI_RENDER_BACKEND=vk build/release/ny-full --no-progress etc/projects/ui/input.ny
```

These commands open interactive windows and require a usable display plus the
selected backend. Run them only when desktop interaction is intended.
For automated Linux checks, use an isolated X11 display, not the user's desktop.
Xvfb with `GALLIUM_DRIVER=softpipe` can exercise OpenGL software rendering; it
is not a GPU frame-rate measurement. Xvfb may lack the DRI3 presentation surface
needed for Vulkan. A no-surface or simulated mode cannot prove swapchain
presentation.

Bound automated runs and stop their process groups on failure. A timeout or
forced termination is not a clean application exit. Create test inputs and
state explicitly rather than depending on a saved desktop session.

### Observable checks

| Check | Required observation |
| --- | --- |
| Startup | The intended window becomes viewable; an unmapped window is a startup failure, not yet a blank-frame diagnosis. |
| Drawing | Expected text, primitives, and colors appear, not only the clear color or a few unrepresentative pixels. |
| Input | The event reaches the application's event pump and produces the intended visible state change. |
| Held input | Capture while the key/button is held; a screenshot after release cannot establish held-state behavior. |
| Resize | Drawable dimensions change and layout/content remain valid on a later completed frame. |
| Close | The application handles its close request, releases resources, and exits successfully without forced cleanup. |

Capture screenshots and matching logs at known frame points. A synthetic input
tool's keymap reports X-server state, not delivery to the application. A key
such as `w` is not an input oracle unless the application assigns it a visible
action. An image taken before first draw or after a probe's draw lifetime
cannot establish a rendering regression.

Slow first frames can include font discovery, shader setup, atlas uploads, and
software rasterization. Choose startup/settle limits from measured frame cost;
do not turn a timeout into a pass by waiting indefinitely.

### Backend parity

Use the same source, assets, input sequence, window dimensions, and observed
state for OpenGL and Vulkan. Compare primitives, batches, glyph runs, colors,
resize, and startup/cleanup. Exact pixels can differ with antialiasing,
rasterization, and driver coverage.

Reduce discrepancies to one drawing API; retain the backend, image, log, frame
count, and exit status. Shared contracts live in
[`lib/os/ui/render/mod.ny`](../../lib/os/ui/render/mod.ny); backend implementations
live under [`gl/`](../../lib/os/ui/render/gl/) and
[`vk/`](../../lib/os/ui/render/vk/).

### Stalls and performance

A debugger stack locates a stall; repeated frame times or a profiler establish
cost. Attach CPU profiling to the actual runtime process, not a compiler wrapper
that may have spawned it:

```bash
perf record --call-graph dwarf -p <runtime-pid> \
  -o build/ui-runtime.data -- sleep 10
perf report --stdio --input build/ui-runtime.data
```

Keep renderer/device, driver, window size, vsync, optimization, binary, cache
state, input sequence, and frame interval fixed. Separate compilation, startup,
first-frame setup, vsync waits, and steady-state work; report sample count and
spread. Software OpenGL and hardware Vulkan timings are not a before/after
comparison.

`NY_UI_FPS_LOG=1` enables frame-rate logging where supported. Other trace
variables belong to the demo or backend; use the narrowest relevant control.
Diagnostic output perturbs timing, so repeat the quiet comparison after
attribution. See [Performance](performance.md) for address validation, counters,
and interpretation limits.
