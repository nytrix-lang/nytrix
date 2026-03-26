# UI and rendering

Use `std.os.ui.render` for drawing and `std.os.ui.window` for window/input
state. The render module owns the active graphics context after `init_window`;
most draw calls do not take a window handle.

A minimal UI program creates one window, enters one frame loop, draws visible
state inside that loop, and releases renderer resources after the loop exits.

## First window

This opens a window, clears every frame, draws primitives and text, adapts the
layout to resize events, and closes when the window requests shutdown.

```ny
use std.core
use std.os.ui.render as gfx

def win = gfx.init_window(960, 540, "Nytrix UI")
if(!win){ panic("window init failed") }

def font = gfx.font_load_first(["etc/assets/fonts/jetbrains.ttf"], 22)

while(!gfx.window_should_close()){
   def ui = gfx.begin_frame_layout(gfx.color_rgb(0.07, 0.08, 0.10), 960.0, 540.0)
   if(!ui){
      continue
   }

   gfx.draw_rect(48.0, 48.0, 260.0, 120.0, gfx.color_rgb(0.10, 0.32, 0.72))
   gfx.draw_rectangle_lines(48.0, 48.0, 260.0, 120.0, gfx.WHITE, 2.0)
   gfx.draw_circle(420.0, 108.0, 54.0, gfx.ORANGE)
   gfx.draw_line_2d(520.0, 72.0, 720.0, 164.0, gfx.GREEN, 4.0)
   gfx.draw_text(font, "draw text, shapes, images, and meshes", 64.0, 92.0, gfx.WHITE)

   gfx.end_frame()
}

gfx.close_window()
```

## Frame loop

| Step | API | Notes |
| --- | --- | --- |
| Create context | `gfx.init_window(width, height, title, flags=0, vsync=false, filter=false, msaa=1)` | Returns a window dict or `false`. |
| Start frame | `gfx.begin_frame_clear(color)`, `gfx.begin_frame_layout(color, base_w, base_h)` | Starts drawing and clears the framebuffer; `begin_frame_layout` also syncs a resize-aware 2D layout. |
| Draw | `gfx.draw_*` | Coordinates are pixel-like by default; after `begin_frame_layout` they are in the chosen design size. Colors are `[r, g, b, a]` floats or packed colors where supported. |
| Present | `gfx.end_frame()` | Submits the frame. |
| Close check | `gfx.window_should_close()` | Polls events and returns true after close/escape/OS quit. |
| Shutdown | `gfx.close_window()` | Releases renderer state and closes the active window. |

## Draw API map

| Need | APIs |
| --- | --- |
| Colors | `WHITE`, `BLACK`, `RED`, `GREEN`, `BLUE`, `ORANGE`, `color_rgb`, `color_rgba`, `color_hex`, `color_alpha`, `color_pack` |
| Rectangles | `draw_rect`, `draw_rectangle_lines`, `draw_rect_rounded`, `draw_rect_sharp`, `draw_rect_fast`, `draw_rect_outline_fast` |
| Lines and shapes | `draw_line`, `draw_line_2d`, `draw_triangle`, `draw_quad`, `draw_circle`, `draw_circle_lines`, `draw_ring`, `draw_polygon`, `draw_ellipse`, `draw_arc`, `draw_sector`, `draw_star` |
| Text | `font_load`, `font_load_first`, `draw_text`, `measure_text`, `font_line_height`, `font_ascent`, `font_destroy` |
| Textures | `texture_load`, `texture_load_ex`, `draw_texture`, `draw_rect_tex`, `draw_rect_tex_uv`, `texture_destroy` |
| 3D | `camera_init`, `camera_update`, `set_camera`, `begin_mode_3d`, `end_mode_3d`, `draw_cube`, `mesh_load`, `draw_mesh`, `draw_mesh_group` |
| Projection | `set_ortho_2d`, `set_ortho`, `set_perspective`, `set_model_matrix`, `set_view`, `set_projection` |
| Responsive layout | `begin_frame_layout`, `layout_fit`, `layout_x`, `layout_y`, `layout_size`, `layout_rect`, `framebuffer_size_f64` |
| Timing and capture | `get_frame_time`, `get_time`, `renderer_frame_stats`, `snapshot`, `request_frame_capture`, `get_pixel` |

## Input

Keep the window returned by `init_window` when you need keyboard, mouse, or event
state.

```ny
use std.core
use std.os.ui.render as gfx
use std.os.ui.window as window
use std.os.ui.consts as key

def win = gfx.init_window(800, 480, "Input")
if(!win){ panic("window init failed") }

mut accent = gfx.BLUE

while(!gfx.window_should_close(win)){
   if(window.key_pressed(win, key.KEY_SPACE)){
      accent = gfx.ORANGE
   }

   def mouse = window.mouse_pos(win)
   def down = window.mouse_down(win, 0) ; left mouse button

   gfx.begin_frame_clear(gfx.BLACK)
   gfx.draw_circle(float(mouse.get(0, 0)), float(mouse.get(1, 0)), down ? 30.0 : 18.0, accent)
   gfx.end_frame()
}

gfx.close_window()
```

For event-driven code, drain queued events inside the frame loop before drawing.

```text
mut e = window.check_event(win)
while(e){
   def typ = window.event_type(e)
   def data = window.event_data(e)
   if(typ == key.EVENT_KEY_PRESSED && window.event_key_is(data, key.KEY_ESCAPE)){
      window.set_should_close(win, true)
   }
   e = window.check_event(win)
}
```

## Text

`draw_text` takes a font id. Passing `0` asks the renderer to use its default
font; loading a known font gives predictable metrics.

```text
def font = gfx.font_load_first([
   "etc/assets/fonts/jetbrains.ttf",
   "etc/assets/fonts/maplemono.ttf"
], 18)

def size = gfx.measure_text(font, "Status: ready")
gfx.draw_text(font, "Status: ready", 20.0, 24.0, gfx.WHITE)
```

Call `measure_text` before clipping, alignment, or right-aligned labels.

## Textures

Load textures once, draw them every frame, and destroy them when the renderer no
longer needs them.

```text
def logo = gfx.texture_load("logo.png")

if(logo){
   gfx.draw_texture(logo, 32.0, 32.0, 0.5, gfx.WHITE)
}

gfx.texture_destroy(logo)
```

## 3D start

Switch into 3D mode for world-space draws, then switch back before 2D overlays.

```text
def cam = gfx.camera_init([0.0, 1.5, 5.0], 0.0, -12.0, 16.0 / 9.0)

gfx.begin_frame_clear(gfx.BLACK)
gfx.begin_mode_3d(cam)
gfx.draw_grid(10, 1.0, gfx.GRAY)
gfx.draw_cube([0.0, 0.5, 0.0], 1.0, gfx.BLUE)
gfx.end_mode_3d()
gfx.draw_text(0, "3D scene", 20.0, 20.0, gfx.WHITE)
gfx.end_frame()
```

## Find more

```bash
ny doc get std.os.ui.render
ny doc get std.os.ui.window
ny doc search --symbols draw_rect
ny doc search --symbols key_pressed
```

If a UI example fails before opening a window, check the platform backend,
display server, graphics driver, and asset paths before changing rendering
code.

## Related

- [library.md](library.md) for the UI module map.
- [examples.md](examples.md) for runnable programs.
- [troubleshooting.md](troubleshooting.md) for runtime and environment checks.
