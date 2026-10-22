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
[Implementation status](status.md) and [Execution routes](routes.md).
