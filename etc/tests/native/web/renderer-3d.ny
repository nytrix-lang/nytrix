;; Keywords: browser webgl renderer 3d portability fixture
;; Shared 3D renderer baseline: a depth-tested cube must not silently fall back.
use std.os.ui.render as gfx

def win = gfx.init_window(320, 180, "Nytrix browser 3d", 0, true, false, 1)
def cam = gfx.camera_init([0.0, 0.0, 4.0], 0.0, 0.0, 16.0 / 9.0)
gfx.begin_frame_clear(gfx.BLACK)
gfx.begin_mode_3d(cam)
gfx.draw_cube([0.0, 0.0, 0.0], 1.0, gfx.WHITE)
gfx.draw_cube([0.35, 0.0, 0.5], 0.5, [0.2, 0.6, 1.0, 0.5])
gfx.end_mode_3d()
gfx.draw_rect(2, 2, 8, 8, gfx.WHITE)
gfx.end_frame()
